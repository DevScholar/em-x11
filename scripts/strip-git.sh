#!/usr/bin/env bash
# When cloned into a folder whose name contains "reproducibility-tests",
# remove the .git directory so the checkout behaves as a fresh GitHub clone.
# This is wired into postinstall so it fires automatically after pnpm install.
set -euo pipefail

case "$(pwd)" in
  *reproducibility-tests*)
    if [ -d .git ]; then
      rm -rf .git
      echo "  reproducibility-tests: removed .git"
    fi
    ;;
esac
