#!/usr/bin/env bash
# Deploy Studio OS to the live Elgato directories. Sequence matters — see
# docs/CONTINUE.md. Two things here exist because getting them wrong cost real
# debugging time:
#
#  1. The app's binary is "MacOS/Stream Deck", NOT "Elgato Stream Deck". Every
#     obvious pgrep pattern silently never matches, so an "app is dead" check
#     lies and the rsync lands under a live app that caches plugin files.
#
#  2. THE SERVICE MUST ALWAYS BE RESTARTED. It is a separate long-lived process;
#     rsyncing new code does nothing to the one already running. A deploy that
#     only started it "if not running" left four Root Hub dials calling verbs a
#     stale service had never heard of — and because those verbs are
#     fire-and-forget, nothing reported the failure.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PLUGINS="$HOME/Library/Application Support/com.elgato.StreamDeck/Plugins"
APP_PAT="Elgato Stream Deck.app/Contents/MacOS/Stream Deck"
NODE="$HOME/Library/Application Support/com.elgato.StreamDeck/NodeJS/20.20.0/node"
SVC="$PLUGINS/com.adiariel.studioos.service/index.js"

echo "1. quitting the Stream Deck app"
pkill -f "Elgato Stream Deck.app" 2>/dev/null || true
until ! pgrep -f "$APP_PAT" >/dev/null; do sleep 1; done
echo "   confirmed dead (matched on the real binary name)"

echo "2. syncing plugin + service + remote script"
rsync -a --delete "$REPO/com.adiariel.studioos.sdPlugin/" "$PLUGINS/com.adiariel.studioos.sdPlugin/"
rsync -a --exclude vendor/ "$REPO/service/" "$PLUGINS/com.adiariel.studioos.service/"
ADIVST="$REPO/../adi_ableton_vst_controller/ableton/remote_script/AdiVST"
[ -d "$ADIVST" ] && rsync -a --delete --exclude __pycache__ "$ADIVST/" \
  "$HOME/Music/Ableton/User Library/Remote Scripts/AdiVST/" || true

echo "3. verifying the copy"
diff -r "$REPO/com.adiariel.studioos.sdPlugin" "$PLUGINS/com.adiariel.studioos.sdPlugin" >/dev/null \
  && echo "   plugin identical" || { echo "   *** DIFFERS ***"; exit 1; }

echo "4. RESTARTING the service (never conditional)"
pkill -f "studioos.service/index.js" 2>/dev/null || true
sleep 1
mkdir -p "$HOME/Library/Logs/StudioOS"
nohup "$NODE" "$SVC" >> "$HOME/Library/Logs/StudioOS/service.log" 2>&1 &
disown
sleep 2
pgrep -f "studioos.service/index.js" >/dev/null && echo "   service restarted" || { echo "   *** service down ***"; exit 1; }

echo "5. starting the app"
open -a "Elgato Stream Deck"
echo "   watch: ls -t ~/Library/Logs/ElgatoStreamDeck/com.adiariel.studioos[0-9].log | head -1"
