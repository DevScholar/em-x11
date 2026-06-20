#!/usr/bin/env bash
# Check that clang-format is available.  Called from postinstall as an
# early heads-up — without it you can't auto-format C/H files locally.
set -euo pipefail

if [ "$(uname -s)" != "Linux" ]; then
  echo "ERROR: This project requires Linux. Run from WSL, not Git Bash or Windows."
  exit 1
fi

if command -v clang-format &>/dev/null; then
  echo "  clang-format: $(clang-format --version 2>&1 | head -1)"
else
  echo ""
  echo "WARNING: clang-format not found."
  echo "  Install it via:  sudo apt install clang-format"
  echo ""
fi
