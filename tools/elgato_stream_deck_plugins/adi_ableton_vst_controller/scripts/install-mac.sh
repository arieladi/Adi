#!/usr/bin/env bash
# =============================================================================
# Install on macOS: the Stream Deck plugin AND the AdiVST Ableton Remote Script.
#
#   ./scripts/install-mac.sh           # symlink both (live-edit friendly)
#   ./scripts/install-mac.sh copy      # copy instead of symlink
#
# Then in Ableton: Settings > Link/Tempo/MIDI > Control Surface > "AdiVST".
# =============================================================================
set -euo pipefail

UUID="com.adiariel.ableton-vst.sdPlugin"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_PLUGIN="$ROOT/$UUID"
SRC_RS="$ROOT/ableton/remote_script/AdiVST"
MODE="${1:-symlink}"

SD_DIR="$HOME/Library/Application Support/com.elgato.StreamDeck/Plugins"
RS_DIR="$HOME/Music/Ableton/User Library/Remote Scripts"

echo "Validating…"; python3 "$ROOT/scripts/validate.py"

link_or_copy() {
  local src="$1" dest="$2"
  rm -rf "$dest"
  if [ "$MODE" = "copy" ]; then cp -R "$src" "$dest"; else ln -s "$src" "$dest"; fi
}

echo "Installing plugin -> $SD_DIR/$UUID"
mkdir -p "$SD_DIR"; link_or_copy "$SRC_PLUGIN" "$SD_DIR/$UUID"

echo "Installing Remote Script -> $RS_DIR/AdiVST"
mkdir -p "$RS_DIR"; link_or_copy "$SRC_RS" "$RS_DIR/AdiVST"

echo "Restarting Stream Deck…"
osascript -e 'quit app "Elgato Stream Deck"' 2>/dev/null || osascript -e 'quit app "Stream Deck"' 2>/dev/null || true
sleep 1
open -a "Elgato Stream Deck" 2>/dev/null || open -a "Stream Deck" 2>/dev/null || true

echo
echo "Done. Now in Ableton Live:"
echo "  Settings > Link/Tempo/MIDI > Control Surface -> select \"AdiVST\""
echo "  (restart Live if it was already running so it picks up the script)."
