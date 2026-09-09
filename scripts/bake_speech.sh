#!/usr/bin/env bash
set -euo pipefail

# Re-bake the screen reader's spoken lexicon from the words the interface uses.
# Run this after adding or changing text that reaches the user; the generated
# files under main/a11y/assets/ are committed, so no ordinary build needs a
# speech engine.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v say >/dev/null 2>&1; then
  cat >&2 <<'MSG'
No 'say' on PATH.

The bake needs macOS's speech synthesiser. Its output is committed, so this is
only ever a problem for someone re-baking, never for building or for CI.
MSG
  exit 1
fi

python3 "$ROOT_DIR/tools/bake_speech.py" "$@"

echo
echo "Now run ./scripts/format.sh - the emitted tables are laid out by"
echo "clang-format, the same as the baked icon fonts."
