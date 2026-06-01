#!/usr/bin/env bash
#
# fetch-third-party.sh -- populate ignored-area/third-party/ from upstream tarballs.
#
# The checked-in tree keeps ignored-area/ gitignored; this script re-hydrates
# it so the build has the libraries libXt / libXaw / libXmu / libXpm and the
# apps xeyes / xcalc / twm / glxgears available. Run it once after cloning,
# and again after bumping a version in the LIBS table below.
#
# For each package:
#   1. Download the official tarball (cached under ignored-area/tarballs/)
#   2. Extract into ignored-area/temp/<name>/ for processing
#   3. Run emconfigure ./configure to generate config.h
#   4. Copy the minimal build tree into ignored-area/third-party/<name>/
#   5. Clean up ignored-area/temp/<name>/
#
# The script is destructive: each run wipes ignored-area/third-party/<name>/
# and ignored-area/temp/<name>/ before re-populating. On re-runs, libraries
# that already have a .fetched sentinel are skipped — only missing or
# version-bumped ones are rebuilt.

set -euo pipefail

if [ "$(uname -s)" != "Linux" ]; then
  echo "ERROR: This project requires Linux. Run from WSL, not Git Bash or Windows."
  exit 1
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
THIRD_PARTY_DIR="$REPO_ROOT/ignored-area/third-party"
TEMP_DIR="$REPO_ROOT/ignored-area/temp"
OVERLAY_DIR="$REPO_ROOT/cmake/third-party"
CONFIG_CACHE="$REPO_ROOT/scripts/emx11-config.cache"
TARBALL_CACHE="$REPO_ROOT/ignored-area/tarballs"

# Regenerate the config cache on every run so hardcoded absolute paths
# from a previous machine or clone don't poison configure with "changes
# in the environment can compromise the build" errors.
rm -f "$CONFIG_CACHE"

# Strip CR from the dummy pkg-config script so the shebang works even
# when the file was checked out with CRLF on Windows (belt-and-suspenders
# for the .gitattributes fix).
if [ -f "$REPO_ROOT/scripts/emx11-pkg-config" ]; then
    sed -i 's/\r$//' "$REPO_ROOT/scripts/emx11-pkg-config"
fi

log()  { printf '    %s\n' "$*"; }
warn() { printf '    WARNING: %s\n' "$*"; }
die()  { printf 'fetch-third-party: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"; }

have curl
have tar

check_host_tools() {
    # Check for host build tools that the user must install themselves.
    # This script only detects — it does NOT auto-install anything.
    local missing=""

    if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1; then
        missing="$missing  cc / gcc / clang (host C compiler — needed to compile makestrs for libXt)\n"
    fi
    if ! command -v bison >/dev/null 2>&1; then
        missing="$missing  bison (needed by some third-party configure scripts)\n"
    fi

    if [ -n "$missing" ]; then
        printf 'fetch-third-party: the following host tools are required but not found:\n'
        printf "$missing"
        printf 'Please install them and re-run.\n'
        printf '\n'
        printf '  Debian/Ubuntu:  sudo apt install gcc bison\n'
        printf '  Fedora/RHEL:    sudo dnf install gcc bison\n'
        printf '  macOS:          xcode-select --install && brew install bison\n'
        printf '  Arch:           sudo pacman -S gcc bison\n'
        exit 1
    fi
}

check_host_tools

# Each entry: name  upstream-prefix  version  url-base  layout
#
# layout=lib  -> upstream library with src/ + include/.
#                configure is run to generate config.h.
#                CMakeLists.txt comes from cmake/third-party/<name>/.
# layout=app  -> flat upstream application. Full tree mirrored.
#                configure is run to generate config.h.
# layout=data -> data files (xbm, cursors). No compilation, no configure.
# layout=mesa-xdemo -> mesa-demos tarball; only src/xdemos/<name>.c extracted.
#                      No configure run.
#
# Origin metadata (URL, license) is captured inline. All X.Org packages
# are MIT/X11 licensed; glxgears is MIT via mesa-demos.
LIBS=(
    # xtrans 1.6.0 -- X11 transport abstraction headers
    #   URL: https://www.x.org/releases/individual/lib/xtrans-1.6.0.tar.xz
    "xtrans    xtrans    1.6.0    https://www.x.org/releases/individual/lib   lib"

    # xorgproto 2025.1 -- X11 protocol headers
    #   URL: https://www.x.org/releases/individual/proto/xorgproto-2025.1.tar.xz
    "xorgproto xorgproto 2025.1   https://www.x.org/releases/individual/proto lib"

    # libICE 1.1.2 -- X Inter-Client Exchange library
    #   URL: https://www.x.org/releases/individual/lib/libICE-1.1.2.tar.xz
    "libICE    libICE    1.1.2    https://www.x.org/releases/individual/lib   lib"

    # libSM 1.2.6 -- X Session Management library
    #   URL: https://www.x.org/releases/individual/lib/libSM-1.2.6.tar.xz
    "libSM     libSM     1.2.6    https://www.x.org/releases/individual/lib   lib"

    # libXt 1.3.1 -- X Toolkit Intrinsics
    #   URL: https://www.x.org/releases/individual/lib/libXt-1.3.1.tar.xz
    "libXt     libXt     1.3.1    https://www.x.org/releases/individual/lib   lib"

    # libXaw 1.0.16 -- X Athena Widgets
    #   URL: https://www.x.org/releases/individual/lib/libXaw-1.0.16.tar.xz
    "libXaw    libXaw    1.0.16   https://www.x.org/releases/individual/lib   lib"

    # libXmu 1.2.1 -- X miscellaneous utilities
    #   URL: https://www.x.org/releases/individual/lib/libXmu-1.2.1.tar.xz
    "libXmu    libXmu    1.2.1    https://www.x.org/releases/individual/lib   lib"

    # libXpm 3.5.17 -- XPM pixmap library
    #   URL: https://www.x.org/releases/individual/lib/libXpm-3.5.17.tar.xz
    "libXpm    libXpm    3.5.17   https://www.x.org/releases/individual/lib   lib"

    # xbitmaps 1.1.4 -- shared X bitmap data
    #   URL: https://www.x.org/releases/individual/data/xbitmaps-1.1.4.tar.xz
    "xbitmaps  xbitmaps  1.1.4    https://www.x.org/releases/individual/data  data"

    # xeyes 1.3.1 -- classic X demo
    #   URL: https://www.x.org/releases/individual/app/xeyes-1.3.1.tar.xz
    #   Extra: --without-xrender --without-present (em-x11 is pure Xlib, not XCB/RENDER)
    "xeyes     xeyes     1.3.1    https://www.x.org/releases/individual/app   app       --without-xrender --without-present"

    # xclock 1.1.1 -- analog/digital clock
    #   URL: https://www.x.org/releases/individual/app/xclock-1.1.1.tar.xz
    "xclock    xclock    1.1.1    https://www.x.org/releases/individual/app   app"

    # xcalc 1.1.3 -- scientific calculator (Xaw)
    #   URL: https://www.x.org/releases/individual/app/xcalc-1.1.3.tar.xz
    "xcalc     xcalc     1.1.3    https://www.x.org/releases/individual/app   app"

    # twm 1.0.13.1 -- Tab Window Manager
    #   URL: https://www.x.org/releases/individual/app/twm-1.0.13.1.tar.xz
    "twm       twm       1.0.13.1 https://www.x.org/releases/individual/app   app"

    # glxgears -- OpenGL gear demo from mesa-demos
    #   URL: https://archive.mesa3d.org/demos/mesa-demos-9.0.0.tar.xz
    "glxgears  glxgears  9.0.0    https://archive.mesa3d.org/demos            mesa-xdemo"
)

mkdir -p "$THIRD_PARTY_DIR" "$TEMP_DIR"

run_configure() {
    # Run emconfigure ./configure to generate config.h. Uses a shared
    # config.cache so repeated fetches are fast. A dummy pkg-config
    # bypasses PKG_CHECK_MODULES.
    #
    # Returns 0 when configure itself succeeds, non-zero when it fails.
    # If configure exits non-zero but config.h was left behind (e.g. the
    # run wrote it before a late error), we still return non-zero so the
    # caller can decide whether to count this as "done."
    #
    # If configure fails, fall back to generating config.h from
    # config.h.in via the cache values — but the function still returns
    # non-zero so fetch_one knows configure needs a retry next time.
    local dir="$1" name="$2"
    local extra_args="${3:-}"
    if [ ! -x "$dir/configure" ]; then
        warn "$name has no configure script, skipping"
        return 0
    fi
    log "running configure for $name"
    local configure_ok=0
    (cd "$dir" && \
     CONFIG_SITE="$CONFIG_CACHE" \
     PKG_CONFIG="$REPO_ROOT/scripts/emx11-pkg-config" \
     CFLAGS="-I$REPO_ROOT/native/include" \
     CPPFLAGS="-I$REPO_ROOT/native/include" \
     emconfigure ./configure \
         --host=wasm32-unknown-emscripten \
         --cache-file="$CONFIG_CACHE" \
         --disable-shared \
         --enable-static \
         --without-xmlto \
         --without-fop \
         --without-xsltproc \
         --disable-nls \
         $extra_args \
         2>&1) && configure_ok=1

    if [ "$configure_ok" -eq 1 ]; then
        log "config.h generated for $name"
        return 0
    fi

    warn "configure for $name had non-zero exit"

    # Fallback: configure failed before writing config.h (e.g. missing
    # yacc/bison). Generate it from config.h.in using the cache values.
    # config.h might already exist from a previous run's fallback.
    if [ -f "$dir/config.h" ]; then
        log "keeping existing config.h for $name (from previous run)"
        return 1
    fi

    local hin="$dir/config.h.in"
    if [ ! -f "$hin" ]; then
        warn "no config.h.in for $name, cannot generate config.h"
        return 1
    fi
    log "generating config.h for $name from config.h.in (configure fallback)"

    # Build sed expressions from cache: #undef SYMBOL → #define SYMBOL val
    # Only convert values that are "yes" or numeric; "no" values stay #undef.
    local sedfile="$dir/.emx11-fallback.sed"
    rm -f "$sedfile"
    while IFS='=' read -r var val; do
        # Skip comments and env/metadata variables
        case "$var" in
            ''|\#*|ac_cv_env_*|ac_cv_build|ac_cv_host|ac_cv_path_*|ac_cv_prog_*|ac_cv_objext|ac_cv_c_compiler_gnu|ac_cv_c_undeclared_builtin_options|ac_cv_c_const|ac_cv_type_signal|ac_cv_have_x|have_x|ac_cv_have_decl_*|ac_cv_file_*|xorg_cv_*|ac_cv_func_mmap_fixed_mapped|ac_cv_func_malloc_0_nonnull|ac_cv_func_realloc_0_nonnull|ac_cv_header_stdc)
                continue ;;
        esac

        # Map ac_cv_header_foo_h → HAVE_FOO_H
        if [[ "$var" == ac_cv_header_* ]]; then
            local sym="HAVE_$(echo "${var#ac_cv_header_}" | tr 'a-z.' 'A-Z_')"
            [ "$val" = "yes" ] && printf 's|^# *undef *%s$|#define %s 1|\n' "$sym" "$sym" >> "$sedfile"
        # Map ac_cv_func_bar → HAVE_BAR
        elif [[ "$var" == ac_cv_func_* ]]; then
            local sym="HAVE_$(echo "${var#ac_cv_func_}" | tr 'a-z' 'A-Z')"
            [ "$val" = "yes" ] && printf 's|^# *undef *%s$|#define %s 1|\n' "$sym" "$sym" >> "$sedfile"
        # Map ac_cv_sizeof_type → SIZEOF_TYPE
        elif [[ "$var" == ac_cv_sizeof_* ]]; then
            local sym="SIZEOF_$(echo "${var#ac_cv_sizeof_}" | tr 'a-z' 'A-Z')"
            printf 's|^# *undef *%s$|#define %s %s|\n' "$sym" "$sym" "$val" >> "$sedfile"
        fi
    done < "$CONFIG_CACHE"

    # Additional known defines not in the cache.
    cat >> "$sedfile" <<'EOF'
s|^# *undef *STDC_HEADERS$|#define STDC_HEADERS 1|
s|^# *undef *_CONST_X_STRING$|#define _CONST_X_STRING 1|
s|^# *undef *YYTEXT_POINTER$|#define YYTEXT_POINTER 1|
EOF

    cp "$hin" "$dir/config.h"
    sed -i -f "$sedfile" "$dir/config.h"
    rm -f "$sedfile"
    log "config.h generated for $name (fallback)"
    return 1
}

fetch_one() {
    local name="$1" up="$2" ver="$3" url_base="$4" layout="${5:-lib}"
    local extra_config_args="${6:-}"
    local need_configure=0
    local configure_ok=0
    local tarball
    if [ "$layout" = "mesa-xdemo" ]; then
        tarball="mesa-demos-$ver.tar.xz"
    else
        tarball="$up-$ver.tar.xz"
    fi
    local url="$url_base/$tarball"
    local dst="$THIRD_PARTY_DIR/$name"
    local tmp="$TEMP_DIR/$name"

    printf '==> %s %s\n' "$name" "$ver"

    # Skip if already successfully fetched on a previous run.
    if [ -f "$dst/.fetched" ]; then
        log "already built, skipping"
        return 0
    fi

    rm -rf "$tmp"
    mkdir -p "$tmp"

    # Download or use cached tarball. Retry up to 3 times on download failure.
    if [ -f "$TARBALL_CACHE/$tarball" ]; then
        log "using cached $tarball"
        cp "$TARBALL_CACHE/$tarball" "$tmp/$tarball"
    else
        local attempt=1
        while [ $attempt -le 3 ]; do
            log "downloading $url (attempt $attempt/3)"
            if curl -fsSL --connect-timeout 30 --max-time 600 -o "$tmp/$tarball" "$url"; then
                mkdir -p "$TARBALL_CACHE"
                cp "$tmp/$tarball" "$TARBALL_CACHE/$tarball"
                break
            fi
            if [ $attempt -eq 3 ]; then
                die "download failed after 3 attempts: $url"
            fi
            warn "download failed, retrying in 3s..."
            sleep 3
            attempt=$((attempt + 1))
        done
    fi

    log "extracting"
    tar -xf "$tmp/$tarball" -C "$tmp"
    local extracted
    if [ "$layout" = "mesa-xdemo" ]; then
        extracted="$tmp/mesa-demos-$ver"
    else
        extracted="$tmp/$up-$ver"
    fi
    [ -d "$extracted" ] || die "expected $extracted after extract"

    rm -rf "$dst"
    mkdir -p "$dst"

    case "$layout" in
        lib)
            # Run configure in the full extracted tree to generate config.h,
            # then copy the buildable subset (src, include, config.h) to dst.
            need_configure=1
            run_configure "$extracted" "$name" "$extra_config_args" && configure_ok=1
            [ -d "$extracted/src" ]     && cp -r "$extracted/src"     "$dst/src"
            [ -d "$extracted/include" ] && cp -r "$extracted/include" "$dst/include"
            [ -f "$extracted/config.h" ] && cp    "$extracted/config.h" "$dst/config.h"
            [ -f "$extracted/COPYING" ]  && cp    "$extracted/COPYING"  "$dst/COPYING"

            # Some packages (xtrans) have source and headers directly in the
            # root directory instead of under src/ + include/.
            local f
            for f in "$extracted"/*.h "$extracted"/*.c "$extracted"/*.pc.in; do
                [ -f "$f" ] && cp "$f" "$dst/"
            done

            # Copy CMakeLists.txt for cmake-based compilation.
            if [ -f "$OVERLAY_DIR/$name/CMakeLists.txt" ]; then
                cp "$OVERLAY_DIR/$name/CMakeLists.txt" "$dst/CMakeLists.txt"
            else
                die "missing CMakeLists.txt for $name in cmake/third-party/$name/"
            fi

            # libXt: generate StringDefs.c/StringDefs.h/Shell.h via makestrs.
            # Upstream tarball ships util/makestrs.c + util/string.list but not
            # the generated output; we compile makestrs with the host cc and run
            # it to produce the files that the libXt CMakeLists.txt expects.
            if [ "$name" = "libXt" ]; then
                log "compiling makestrs for libXt"
                (cd "$extracted" && cc -o makestrs util/makestrs.c) \
                    || die "host C compiler (cc/gcc/clang) required for makestrs — please install one and re-run"
                log "running makestrs for libXt"
                (cd "$extracted" && ./makestrs < util/string.list > StringDefs.c) \
                    || die "makestrs failed"
                cp "$extracted/StringDefs.c" "$dst/src/StringDefs.c"
                cp "$extracted/StringDefs.h" "$dst/src/StringDefs.h"
                cp "$extracted/StringDefs.h" "$dst/include/X11/StringDefs.h"
                cp "$extracted/Shell.h"       "$dst/include/X11/Shell.h"
            fi
            ;;
        app)
            # Mirror the full tree, then run configure inside dst.
            cp -r "$extracted/." "$dst/"
            need_configure=1
            run_configure "$dst" "$name" "$extra_config_args" && configure_ok=1
            ;;
        data)
            cp -r "$extracted/." "$dst/"
            ;;
        mesa-xdemo)
            local xsrc="$extracted/src/xdemos/$name.c"
            [ -f "$xsrc" ] || die "expected $xsrc inside mesa-demos tarball"
            cp "$xsrc" "$dst/$name.c"
            [ -f "$extracted/COPYING" ] && cp "$extracted/COPYING" "$dst/COPYING"
            ;;
        *)
            die "unknown layout '$layout' for $name"
            ;;
    esac

    # Mark as successfully fetched so re-runs skip this library.
    # For layouts that run configure, only mark success when configure
    # itself succeeded — a fallback-generated config.h is best-effort
    # and should not prevent a retry next time.
    if [ "$need_configure" -eq 0 ] || [ "$configure_ok" -eq 1 ]; then
        touch "$dst/.fetched"
    fi

    # Clean up temp. tarballs can contain read-only files that break
    # rm -rf on Windows-hosted filesystems; chmod first to be safe.
    chmod -R u+w "$tmp" 2>/dev/null || true
    rm -rf "$tmp" || true
}

for row in "${LIBS[@]}"; do
    read -r name up ver url_base layout extra_config_args <<< "$row"
    fetch_one "$name" "$up" "$ver" "$url_base" "$layout" "$extra_config_args"
done

printf '\ndone. ignored-area/third-party/ is ready.\n'
