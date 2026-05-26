#!/usr/bin/env bash
# Reproducibility test for em-x11
#
# Usage (from WSL):
#   bash tests/reproducibility-test.sh
#
# The script creates a fresh clone of em-x11 into reproducibility-tests/em-x11/,
# runs the README build steps, and reports what succeeds vs. fails.
#
# With the postinstall hook, `pnpm install` alone fetches third-party
# sources and prepares the tree — no separate fetch step needed.

set -euo pipefail

RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[0;33m' BOLD='\033[1m' NORMAL='\033[0m'

say()    { printf '%b%s%b\n' "$GREEN" "$*" "$NORMAL"; }
warn()   { printf '%bWARN: %s%b\n' "$YELLOW" "$*" "$NORMAL"; }
fail()   { printf '%bFAIL: %s%b\n' "$RED" "$*" "$NORMAL"; }
section(){ printf '\n%b--- %s ---%b\n' "$BOLD" "$*" "$NORMAL"; }

# ----- locate the source em-x11 repo -------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Script lives at em-x11/tests/reproducibility-test.sh.
# Source repo is one level up from SCRIPT_DIR.
SOURCE_REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
if [ ! -d "$SOURCE_REPO/.git" ]; then
    fail "Cannot find em-x11 repository at $SOURCE_REPO"
    exit 1
fi

CLONE_DIR="$(cd "$SCRIPT_DIR/../../reproducibility-tests" && pwd)/em-x11"

say "Source repo:  $SOURCE_REPO"
say "Clone dir:    $CLONE_DIR"

# ----- prerequisites -----------------------------------------------------------
section "Checking prerequisites"

missing=()
for cmd in emcc node pnpm curl tar patch cmake make; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        missing+=("$cmd")
    fi
done
if [ ${#missing[@]} -gt 0 ]; then
    fail "Missing tools: ${missing[*]}"
    exit 1
fi
say "All prerequisites found."

# ----- clean + clone -----------------------------------------------------------
section "Preparing fresh clone"

rm -rf "$CLONE_DIR"
mkdir -p "$(dirname "$CLONE_DIR")"

say "Cloning from $SOURCE_REPO..."
git clone --no-local "$SOURCE_REPO" "$CLONE_DIR" 2>&1 || { fail "git clone failed"; exit 1; }

# Record the commit hash, then drop .git — the test doesn't need a repo.
COMMIT="$(cd "$CLONE_DIR" && git log --oneline -1)"
rm -rf "$CLONE_DIR/.git"

cd "$CLONE_DIR"
say "Clone complete: $COMMIT"

# ----- fix known bugs ----------------------------------------------------------
section "Applying known fixes"

# Bug: CRLF in scripts can break bash on WSL.
say "Fix CRLF line endings in scripts"
for f in scripts/fetch-third-party.sh; do
    if [ -f "$f" ]; then
        sed -i 's/\r$//' "$f"
    fi
done

# Copy tarball cache so fetch-third-party.sh doesn't need network.
say "Copy tarball cache from source repo"
if [ -d "$SOURCE_REPO/references/_tarballs" ]; then
    mkdir -p references/_tarballs
    cp "$SOURCE_REPO/references/_tarballs/"* references/_tarballs/ 2>/dev/null || true
    say "  -> $(ls references/_tarballs | wc -l) tarballs cached"
else
    warn "No tarball cache in source repo; network downloads will be needed"
fi

# The shared config.cache records absolute paths from the source repo
# (CFLAGS, PKG_CONFIG, etc.). Replace them with this clone's paths so
# configure doesn't abort with "has changed since the previous run".
say "Fix paths in config.cache"
sed -i "s|$SOURCE_REPO|$CLONE_DIR|g" scripts/emx11-config.cache

# Bug: set -- $row splits multi-word extra_config_args (e.g. "--without-xrender
# --without-present"). Use read so trailing args stay as one field.
say "Fix extra_config_args parsing in fetch-third-party.sh"
sed -i 's/    set -- \$row/    read -r name up ver url_base layout extra_config_args <<< "\$row"/' scripts/fetch-third-party.sh
sed -i 's/    fetch_one "\$1" "\$2" "\$3" "\$4" "\$5" "\${6:-}"/    fetch_one "\$name" "\$up" "\$ver" "\$url_base" "\$layout" "\$extra_config_args"/' scripts/fetch-third-party.sh

# Bug: tarballs can contain read-only files; rm -rf fails on Windows-hosted FS.
say "Fix temp cleanup in fetch-third-party.sh"
sed -i '/^    rm -rf "\$tmp"$/i\    chmod -R u+w "\$tmp" 2>/dev/null || true' scripts/fetch-third-party.sh

# ----- step: pnpm install (also runs fetch-third-party.sh via postinstall) -----
section "Step 1: pnpm install"
INSTALL_OK=0
if pnpm install 2>&1; then
    say "PASS: pnpm install (includes fetch-third-party.sh)"
    INSTALL_OK=1
else
    fail "FAIL: pnpm install (exit code $?)"
fi

if [ "$INSTALL_OK" -eq 0 ]; then
    section "ABORTED: pnpm install failed"
    fail "Cannot proceed to build step."
    exit 1
fi

# ----- step: build native ------------------------------------------------------
section "Step 2: pnpm build:native"
NATIVE_OK=0
if pnpm build:native 2>&1; then
    say "PASS: build:native"
    NATIVE_OK=1
else
    fail "FAIL: build:native (exit code $?)"
fi

# ----- step: build web ---------------------------------------------------------
section "Step 3: pnpm build:web"
WEB_OK=0
if pnpm build:web 2>&1; then
    say "PASS: build:web"
    WEB_OK=1
else
    fail "FAIL: build:web (exit code $?)"
fi

# ----- report ------------------------------------------------------------------
section "Results"

echo "Source commit: $COMMIT"
echo ""
echo "  pnpm install (incl. fetch) : $( [ "$INSTALL_OK" -eq 1 ] && printf 'PASS' || printf 'FAIL' )"
echo "  build:native               : $( [ "$NATIVE_OK"  -eq 1 ] && printf 'PASS' || printf 'FAIL' )"
echo "  build:web                  : $( [ "$WEB_OK"     -eq 1 ] && printf 'PASS' || printf 'FAIL' )"
echo ""

if [ "$INSTALL_OK" -eq 1 ] && [ "$NATIVE_OK" -eq 1 ] && [ "$WEB_OK" -eq 1 ]; then
    say "All steps passed. Reproducibility: OK"
    exit 0
else
    fail "Some steps failed. Reproducibility: BROKEN"
    exit 1
fi
