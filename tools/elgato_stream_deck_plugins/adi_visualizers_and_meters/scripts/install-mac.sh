#!/usr/bin/env bash
# =============================================================================
# Install the plugin into Stream Deck on macOS for local development.
#
#   ./scripts/install-mac.sh            # symlink (live-edit: changes apply on
#                                       #   the next Stream Deck restart)
#   ./scripts/install-mac.sh copy       # copy the folder instead of linking
#
# Re-run after editing the manifest. For end users, ship a packaged
# .streamDeckPlugin built with ./scripts/pack.sh instead.
# =============================================================================
set -euo pipefail

UUID="com.adi.visualizers-and-meters.sdPlugin"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/$UUID"
DEST_DIR="$HOME/Library/Application Support/com.elgato.StreamDeck/Plugins"
DEST="$DEST_DIR/$UUID"
MODE="${1:-symlink}"

[ -d "$SRC" ] || { echo "Plugin folder not found: $SRC" >&2; exit 1; }

echo "Validating…"
python3 "$ROOT/scripts/validate.py"

mkdir -p "$DEST_DIR"
rm -rf "$DEST"

if [ "$MODE" = "copy" ]; then
  echo "Copying -> $DEST"
  cp -R "$SRC" "$DEST"
else
  echo "Symlinking -> $DEST"
  ln -s "$SRC" "$DEST"
fi

echo "Restarting Stream Deck…"
osascript -e 'quit app "Elgato Stream Deck"' 2>/dev/null || osascript -e 'quit app "Stream Deck"' 2>/dev/null || true
sleep 1
open -a "Elgato Stream Deck" 2>/dev/null || open -a "Stream Deck" 2>/dev/null || \
  echo "Could not auto-launch Stream Deck — start it manually."

echo "Done."
