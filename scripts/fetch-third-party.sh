#!/usr/bin/env bash
#
# fetch-third-party.sh -- populate third-party/ from upstream tarballs.
#
# The checked-in tree keeps third-party/ gitignored; this script
# re-hydrates it so the build has the libraries libXt / libXaw /
# libXmu / libXpm available. Run it once after cloning, and again
# after bumping a library version in the LIBS table below.
#
# For each library:
#   1. Download the official X.Org tarball
#   2. Extract its src/, include/, COPYING into third-party/<name>/
#   3. Copy our em-x11 meta files (CMakeLists.txt, config.h,
#      ORIGIN.txt) from scripts/third-party/<name>/ on top
#   4. Apply any patches from scripts/third-party/<name>/patches/
#   5. Drop any pre-generated sources from
#      scripts/third-party/<name>/generated/ into src/
#   6. Delete the downloaded tarball
#
# The script is destructive: each run wipes third-party/<name>/ before
# re-populating, so edits made directly to third-party/ will be lost.
# Upstream modifications belong under scripts/third-party/<name>/patches/.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
THIRD_PARTY_DIR="$REPO_ROOT/third-party"
OVERLAY_DIR="$REPO_ROOT/scripts/third-party"
TARBALL_CACHE="$REPO_ROOT/references/_tarballs"

log()  { printf '    %s\n' "$*"; }
die()  { printf 'fetch-third-party: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"; }

have curl
have tar
have patch

# Each entry: logical-name  upstream-dir-prefix  version  url-base  layout
# layout=lib  -> upstream has src/ + include/; we copy those (libraries).
# layout=app  -> upstream is flat (sources at root); we mirror the whole tree.
LIBS=(
    "libXt   libXt   1.3.1   https://www.x.org/releases/individual/lib  lib"
    "libXaw  libXaw  1.0.16  https://www.x.org/releases/individual/lib  lib"
    "libXmu  libXmu  1.2.1   https://www.x.org/releases/individual/lib  lib"
    "libXpm  libXpm  3.5.17  https://www.x.org/releases/individual/lib  lib"
    "xeyes   xeyes   1.3.1   https://www.x.org/releases/individual/app  app"
    "xclock  xclock  1.1.1   https://www.x.org/releases/individual/app  app"
)

mkdir -p "$THIRD_PARTY_DIR"

fetch_one() {
    local name="$1" up="$2" ver="$3" url_base="$4" layout="${5:-lib}"
    local tarball="$up-$ver.tar.xz"
    local url="$url_base/$tarball"
    local dst="$THIRD_PARTY_DIR/$name"
    local overlay="$OVERLAY_DIR/$name"

    [ -d "$overlay" ] || die "missing overlay directory: $overlay"

    printf '==> %s %s\n' "$name" "$ver"

    local tmp
    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" RETURN

    # Prefer a cached tarball under references/_tarballs/ when present,
    # so re-running after a successful fetch is a no-network operation
    # and dev boxes without reliable X.Org access still work. Fall back
    # to curl otherwise.
    if [ -f "$TARBALL_CACHE/$tarball" ]; then
        log "using cached $tarball"
        cp "$TARBALL_CACHE/$tarball" "$tmp/$tarball"
    else
        log "downloading $url"
        curl -fsSL --retry 3 --retry-delay 2 -o "$tmp/$tarball" "$url"
    fi

    log "extracting"
    tar -xf "$tmp/$tarball" -C "$tmp"
    local extracted="$tmp/$up-$ver"
    [ -d "$extracted" ] || die "expected $extracted after extract"

    rm -rf "$dst"
    mkdir -p "$dst"

    # Upstream source files we keep verbatim. Libraries expose src/
    # and include/; apps ship a flat tree with the .c/.h files at
    # the root, so mirror the whole extracted directory in that case.
    case "$layout" in
        lib)
            [ -d "$extracted/src" ]     && cp -r "$extracted/src"     "$dst/src"
            [ -d "$extracted/include" ] && cp -r "$extracted/include" "$dst/include"
            [ -f "$extracted/COPYING" ] && cp    "$extracted/COPYING" "$dst/COPYING"
            ;;
        app)
            cp -r "$extracted/." "$dst/"
            ;;
        *)
            die "unknown layout '$layout' for $name"
            ;;
    esac

    # Overlay em-x11 meta files.
    for f in CMakeLists.txt config.h ORIGIN.txt; do
        if [ -f "$overlay/$f" ]; then
            cp "$overlay/$f" "$dst/$f"
        else
            log "warning: overlay/$name/$f not found, skipping"
        fi
    done

    # Apply patches in sorted order (0001-*, 0002-*, ...).
    if [ -d "$overlay/patches" ]; then
        local p
        for p in "$overlay/patches"/*.patch; do
            [ -e "$p" ] || continue
            log "applying patch $(basename "$p")"
            (cd "$dst" && patch -p1 --quiet < "$p")
        done
    fi

    # Drop any pre-generated source files (e.g. libXt's StringDefs.c /
    # Shell.h, which upstream produces via `makestrs` at build time).
    # The tree layout under generated/ mirrors the target tree under
    # third-party/<name>/, so a file at overlay/generated/include/X11/
    # Shell.h lands at third-party/<name>/include/X11/Shell.h.
    if [ -d "$overlay/generated" ]; then
        log "overlaying generated files"
        cp -r "$overlay/generated/." "$dst/"
    fi
}

for row in "${LIBS[@]}"; do
    # shellcheck disable=SC2086
    set -- $row
    fetch_one "$1" "$2" "$3" "$4" "$5"
done

printf '\ndone. third-party/ is ready.\n'
