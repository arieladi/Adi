#!/usr/bin/env bash
# =============================================================================
# Build a distributable .streamDeckPlugin with Elgato's DistributionTool.
#
#   ./scripts/pack.sh
#
# Requires the DistributionTool on PATH, or set DISTRIBUTION_TOOL=/path/to/it.
# Download it from https://docs.elgato.com/streamdeck/sdk/ (Distribute section).
# Output lands in ./release/.
# =============================================================================
set -euo pipefail

UUID="com.adi.visualizers-and-meters.sdPlugin"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/release"
TOOL="${DISTRIBUTION_TOOL:-DistributionTool}"

python3 "$ROOT/scripts/validate.py"

mkdir -p "$OUT"

if ! command -v "$TOOL" >/dev/null 2>&1 && [ ! -x "$TOOL" ]; then
  echo "DistributionTool not found." >&2
  echo "  - Download it: https://docs.elgato.com/streamdeck/sdk/" >&2
  echo "  - Then add it to PATH, or run: DISTRIBUTION_TOOL=/path/to/DistributionTool ./scripts/pack.sh" >&2
  exit 1
fi

"$TOOL" -b -i "$ROOT/$UUID" -o "$OUT"
echo "Built .streamDeckPlugin into: $OUT"
