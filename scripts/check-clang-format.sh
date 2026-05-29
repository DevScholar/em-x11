#!/usr/bin/env bash
# Check that clang-format is available. Called from postinstall so the
# developer gets an early warning rather than a late pre-commit failure.
set -euo pipefail

if command -v clang-format &>/dev/null; then
  echo "  clang-format: $(clang-format --version 2>&1 | head -1)"
else
  echo ""
  echo "WARNING: clang-format not found."
  echo "  Install it via:  sudo apt install clang-format"
  echo "  Without it, 'git commit' on C/H files will be rejected."
  echo ""
fi
