#!/usr/bin/env bash
# Studio OS installer — macOS.
#
# Installs the plugin, installs the backend service, registers it as a LaunchAgent
# so it survives login and restarts if it dies, generates the 36-key + 6-dial
# profile, and points the device at it.
#
# Nothing is downloaded. The MIDI natives are committed prebuilds and the service
# runs on the Stream Deck app's own bundled Node, so this is a copy plus two
# config writes. Every step that touches the user's setup is listed up front and
# skippable, and the profile step takes a restorable backup.
#
#   ./scripts/install-mac.sh            interactive
#   ./scripts/install-mac.sh --yes      no prompts
#   ./scripts/install-mac.sh --uninstall
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

SUPPORT="$HOME/Library/Application Support/com.elgato.StreamDeck"
PLUGINS="$SUPPORT/Plugins"
PLUGIN_SRC="$ROOT/com.adiariel.studioos.sdPlugin"
PLUGIN_DST="$PLUGINS/com.adiariel.studioos.sdPlugin"
SERVICE_DST="$PLUGINS/com.adiariel.studioos.service"
AGENT_ID="com.adiariel.studioos.service"
AGENT_PLIST="$HOME/Library/LaunchAgents/$AGENT_ID.plist"
LOG_DIR="$HOME/Library/Logs/StudioOS"

ASSUME_YES=0
UNINSTALL=0
for arg in "$@"; do
  case "$arg" in
    --yes|-y) ASSUME_YES=1 ;;
    --uninstall) UNINSTALL=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

say()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m !\033[0m %s\n' "$*"; }
die()  { printf '\033[31m X\033[0m %s\n' "$*" >&2; exit 1; }

ask() {
  [ "$ASSUME_YES" = "1" ] && return 0
  printf '\033[35m ?\033[0m %s [y/N] ' "$1"
  read -r reply </dev/tty || reply=""
  [[ "$reply" =~ ^[Yy] ]]
}

quit_streamdeck() {
  if pgrep -f "Elgato Stream Deck.app/Contents/MacOS/Stream Deck" >/dev/null; then
    say "quitting the Stream Deck app (it rewrites its profile store on exit)"
    osascript -e 'tell application "Elgato Stream Deck" to quit' >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
      pgrep -f "Elgato Stream Deck.app/Contents/MacOS/Stream Deck" >/dev/null || return 0
      sleep 0.5
    done
    die "the Stream Deck app would not quit — close it by hand and re-run"
  fi
}

# Prefer the Node the Stream Deck app already ships, so nothing has to be
# installed. Fall back to a system node only if the app's copy is missing.
find_node() {
  local newest
  newest="$(ls -1d "$SUPPORT/NodeJS"/*/ 2>/dev/null | sort -V | tail -1 || true)"
  if [ -n "$newest" ] && [ -x "${newest}node" ]; then
    printf '%s' "${newest}node"; return 0
  fi
  command -v node 2>/dev/null || return 1
}

# ---------------------------------------------------------------- uninstall
if [ "$UNINSTALL" = "1" ]; then
  say "uninstalling Studio OS"
  if [ -f "$AGENT_PLIST" ]; then
    launchctl bootout "gui/$(id -u)/$AGENT_ID" 2>/dev/null || true
    rm -f "$AGENT_PLIST"
    say "removed the LaunchAgent"
  fi
  quit_streamdeck
  rm -rf "$PLUGIN_DST" "$SERVICE_DST"
  say "removed the plugin and service"
  warn "the Studio OS profile was left in place. Remove it in the Stream Deck app,"
  warn "or restore the pre-install state with: python3 scripts/make_profile.py --restore"
  exit 0
fi

# ------------------------------------------------------------------ preflight
[ -d "/Applications/Elgato Stream Deck.app" ] || die "Elgato Stream Deck is not installed"
[ -d "$SUPPORT" ] || die "no Stream Deck support folder — launch the app once first"
[ -d "$PLUGIN_SRC" ] || die "plugin source missing at $PLUGIN_SRC"

NODE_BIN="$(find_node)" || die "no Node runtime found (expected one under $SUPPORT/NodeJS)"
say "using node: $NODE_BIN"

python3 - <<'PY' || die "manifest is not self-consistent"
import json, os, sys
root = os.path.join(os.environ.get("ROOT", "."), "com.adiariel.studioos.sdPlugin")
m = json.load(open(os.path.join(root, "manifest.json")))
missing = []
def need(p, exts=(".png",)):
    if not p: return
    if os.path.exists(os.path.join(root, p)) or any(
        os.path.exists(os.path.join(root, p + e)) for e in exts): return
    missing.append(p)
need(m["Icon"]); need(m["CategoryIcon"])
need(m["CodePath"], ("",)); need(m.get("PropertyInspectorPath"), ("",))
for a in m["Actions"]:
    need(a.get("Icon"))
    for s in a.get("States", []): need(s.get("Image"))
    if "Encoder" in a:
        need(a["Encoder"].get("Icon")); need(a["Encoder"].get("layout"), ("",))
if missing:
    print("missing:", missing, file=sys.stderr); sys.exit(1)
PY

cat <<EOF

Studio OS will:
  1. install the plugin   -> $PLUGIN_DST
  2. install the service  -> $SERVICE_DST
  3. register a LaunchAgent so the service starts at login  ($AGENT_ID)
  4. generate a "Studio OS" profile (36 keys + 6 dials) and make it the default
     for your Stream Deck + XL  — with a restorable backup of the profile store

Nothing is downloaded. The Stream Deck app will be quit and relaunched.

EOF
ask "proceed?" || die "cancelled"

# --------------------------------------------------------------------- install
quit_streamdeck

say "installing plugin"
rm -rf "$PLUGIN_DST"; mkdir -p "$PLUGINS"; cp -R "$PLUGIN_SRC" "$PLUGIN_DST"

say "installing service"
rm -rf "$SERVICE_DST"; cp -R "$ROOT/service" "$SERVICE_DST"

if ask "register the service to start at login?"; then
  mkdir -p "$LOG_DIR" "$(dirname "$AGENT_PLIST")"
  cat > "$AGENT_PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$AGENT_ID</string>
  <key>ProgramArguments</key>
  <array>
    <string>$NODE_BIN</string>
    <string>$SERVICE_DST/index.js</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$LOG_DIR/service.log</string>
  <key>StandardErrorPath</key><string>$LOG_DIR/service.log</string>
</dict>
</plist>
PLIST
  launchctl bootout "gui/$(id -u)/$AGENT_ID" 2>/dev/null || true
  launchctl bootstrap "gui/$(id -u)" "$AGENT_PLIST"
  say "service registered — logs at $LOG_DIR/service.log"
else
  warn "skipped. Start it by hand with:"
  warn "  \"$NODE_BIN\" \"$SERVICE_DST/index.js\""
fi

if ask "generate the Studio OS profile and make it the device default?"; then
  ( cd "$ROOT" && python3 scripts/make_profile.py --activate )
else
  warn "skipped — the surface will report INCOMPLETE until a profile places the actions."
fi

say "relaunching the Stream Deck app"
open -a "Elgato Stream Deck"

# The plugin logs its coverage on a settle timer; surface it so a failed install
# is obvious here rather than discovered as a blank device.
sleep 12
PLUGIN_LOG="$HOME/Library/Logs/ElgatoStreamDeck/com.adiariel.studioos0.log"
if [ -f "$PLUGIN_LOG" ]; then
  echo
  grep -E "surface (COMPLETE|INCOMPLETE)|service (online|offline)" "$PLUGIN_LOG" | tail -3 || true
fi

echo
say "done. macOS will ask for Accessibility permission the first time the numpad"
say "sends a keystroke — grant it to the Stream Deck app in System Settings."
