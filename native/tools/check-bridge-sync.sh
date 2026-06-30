#!/usr/bin/env bash
# Verify bridges.c EM_JS functions and library_em-x11.js stay in sync.
# Exit 0 if they match, 1 with diff output if they don't.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BRIDGES_C="$ROOT/em_x11/bridges.c"
BRIDGES_JS="$ROOT/src/lib/library_em-x11.js"

if [ ! -f "$BRIDGES_C" ]; then
  echo "check-bridge-sync: missing $BRIDGES_C" >&2
  exit 1
fi
if [ ! -f "$BRIDGES_JS" ]; then
  echo "check-bridge-sync: missing $BRIDGES_JS" >&2
  exit 1
fi

# Extract em_x11_js_* function names from bridges.c.
c_names=$(grep -oP '\bem_x11_js_\w+' "$BRIDGES_C" | sort -u)
# Extract em_x11_js_* function names from the JS library, excluding __sig/__deps.
js_names=$(grep -oP '\bem_x11_js_\w+' "$BRIDGES_JS" | grep -vP '__sig|__deps' | sort -u)

if diff <(echo "$c_names") <(echo "$js_names"); then
  echo "[check-bridge-sync] OK — bridges.c and library_em-x11.js match"
  exit 0
else
  echo "[check-bridge-sync] MISMATCH — update both bridges.c and library_em-x11.js" >&2
  exit 1
fi
