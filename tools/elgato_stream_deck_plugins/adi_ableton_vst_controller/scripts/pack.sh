#!/usr/bin/env bash
# Build a distributable .streamDeckPlugin with Elgato's DistributionTool (macOS/Linux).
#   ./scripts/pack.sh
# Requires DistributionTool on PATH, or DISTRIBUTION_TOOL=/path/to/it.
# https://docs.elgato.com/streamdeck/sdk/ — output lands in ./release/
set -euo pipefail
UUID="com.adiariel.ableton-vst.sdPlugin"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/release"; TOOL="${DISTRIBUTION_TOOL:-DistributionTool}"
python3 "$ROOT/scripts/validate.py"
mkdir -p "$OUT"
if ! command -v "$TOOL" >/dev/null 2>&1 && [ ! -x "$TOOL" ]; then
  echo "DistributionTool not found. Get it at https://docs.elgato.com/streamdeck/sdk/ , add to PATH, or set DISTRIBUTION_TOOL=/path/to/DistributionTool." >&2
  exit 1
fi
"$TOOL" -b -i "$ROOT/$UUID" -o "$OUT"
echo "Built .streamDeckPlugin into: $OUT"
