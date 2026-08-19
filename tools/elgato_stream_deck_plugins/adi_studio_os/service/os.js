// OS routing: keystrokes, hotkeys, volume, zoom, app switching, launching.
//
// The CEF frontend has no child_process, so everything here is why the service
// exists at all. Both platforms are implemented; macOS is verified on this
// machine, Windows is written but UNVERIFIED until Adi runs it there (see
// docs/DECISIONS.md — the Windows pass is explicitly a later session).
//
// Windows input goes through keybd_event via Add-Type rather than SendKeys,
// because SendKeys cannot press the Windows key at all and is unreliable for
// Ctrl+Shift+Esc. That one choice makes Win+R, Win+Plus and Alt-Tab possible.
//
// macOS requires the Stream Deck app to hold Accessibility permission, exactly
// as the legacy console plugin's numpad did.
import { execFile } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";

const PLATFORM = os.platform();
const isMac = PLATFORM === "darwin";
const isWin = PLATFORM === "win32";

let log = console;
export function setLogger(l) { log = l; }

function run(cmd, args) {
  return new Promise((resolve) => {
    execFile(cmd, args, { timeout: 5000 }, (err, stdout, stderr) => {
      if (err) log.error?.(`${cmd} failed: ${err.message} ${stderr || ""}`);
      resolve(!err);
    });
  });
}
const osa = (script) => run("osascript", ["-e", script]);
const ps = (script) => run("powershell", ["-NoProfile", "-NonInteractive", "-Command", script]);

// PowerShell prelude giving us real virtual-key control on Windows.
const WIN_KEY_SHIM =
  "Add-Type -MemberDefinition '[DllImport(\"user32.dll\")] public static extern void " +
  "keybd_event(byte bVk, byte bScan, uint dwFlags, System.UIntPtr dwExtraInfo);' " +
  "-Name U -Namespace W -PassThru | Out-Null; ";
const winDown = (vk) => `[W.U]::keybd_event(${vk},0,0,[UIntPtr]::Zero); `;
const winUp = (vk) => `[W.U]::keybd_event(${vk},0,2,[UIntPtr]::Zero); `;
const winTap = (vk) => winDown(vk) + winUp(vk);

// ---------------------------------------------------------------- numpad keys
// macOS numeric-keypad key codes; Windows virtual-key codes for the same keys.
const MAC_KEY = {
  "0": 82, "1": 83, "2": 84, "3": 85, "4": 86, "5": 87, "6": 88, "7": 89, "8": 91, "9": 92,
  decimal: 65, enter: 76, plus: 69, minus: 78, multiply: 67, divide: 75,
  backspace: 51, clear: 53, // 53 = escape; see the note in os.key() below
};
const WIN_VK = {
  "0": 0x60, "1": 0x61, "2": 0x62, "3": 0x63, "4": 0x64, "5": 0x65,
  "6": 0x66, "7": 0x67, "8": 0x68, "9": 0x69,
  decimal: 0x6e, enter: 0x0d, plus: 0x6b, minus: 0x6d, multiply: 0x6a, divide: 0x6f,
  backspace: 0x08, clear: 0x1b,
};

/* Send one numpad token to the focused application.
   `clear` deliberately sends ESCAPE on both platforms rather than a literal
   numpad-Clear: Windows has no Clear key that types anything, and "cancel the
   current entry" is what the key means to a person looking at it. Inside the
   State 1 calculator the token never reaches here — the overlay handles it
   internally. */
export function key(token) {
  const t = String(token);
  if (isMac) {
    const code = MAC_KEY[t];
    if (code == null) return Promise.resolve(false);
    return osa(`tell application "System Events" to key code ${code}`);
  }
  if (isWin) {
    const vk = WIN_VK[t];
    if (vk == null) return Promise.resolve(false);
    return ps(WIN_KEY_SHIM + winTap(vk));
  }
  const X11 = { decimal: "KP_Decimal", enter: "KP_Enter", plus: "KP_Add", minus: "KP_Subtract",
                multiply: "KP_Multiply", divide: "KP_Divide", backspace: "BackSpace", clear: "Escape" };
  return run("xdotool", ["key", X11[t] || `KP_${t}`]);
}

// ------------------------------------------------------------------- hotkeys
// "cmd+space", "ctrl+shift+escape", "alt+tab", "win+r" — modifiers are named
// per-platform on purpose so a caller can be explicit when it matters.
const MAC_MODS = { cmd: "command down", command: "command down", ctrl: "control down",
                   control: "control down", alt: "option down", option: "option down",
                   shift: "shift down", win: "command down" };
const MAC_SPECIAL = { space: 49, tab: 48, escape: 53, esc: 53, enter: 36, return: 36,
                      delete: 51, up: 126, down: 125, left: 123, right: 124,
                      "=": 24, "+": 24, "-": 27, minus: 27,
                      // V33 — the OS-navigation dials press these.
                      home: 115, end: 119, pageup: 116, pagedown: 121 };
const WIN_MODS = { ctrl: 0x11, control: 0x11, alt: 0x12, shift: 0x10, win: 0x5b, cmd: 0x5b };
const WIN_SPECIAL = { space: 0x20, tab: 0x09, escape: 0x1b, esc: 0x1b, enter: 0x0d, return: 0x0d,
                      delete: 0x2e, up: 0x26, down: 0x28, left: 0x25, right: 0x27,
                      "=": 0xbb, "+": 0xbb, "-": 0xbd, minus: 0xbd,
                      home: 0x24, end: 0x23, pageup: 0x21, pagedown: 0x22 };

export function hotkey(combo) {
  const parts = String(combo).toLowerCase().split("+").map((s) => s.trim()).filter(Boolean);
  if (!parts.length) return Promise.resolve(false);
  const target = parts[parts.length - 1];
  const mods = parts.slice(0, -1);

  if (isMac) {
    const using = mods.map((m) => MAC_MODS[m]).filter(Boolean);
    const suffix = using.length ? ` using {${using.join(", ")}}` : "";
    const code = MAC_SPECIAL[target];
    const action = code != null ? `key code ${code}` : `keystroke "${target.replace(/"/g, '\\"')}"`;
    return osa(`tell application "System Events" to ${action}${suffix}`);
  }
  if (isWin) {
    const modVks = mods.map((m) => WIN_MODS[m]).filter((v) => v != null);
    const vk = WIN_SPECIAL[target] ?? target.toUpperCase().charCodeAt(0);
    let s = WIN_KEY_SHIM;
    modVks.forEach((v) => { s += winDown(v); });
    s += winTap(vk);
    [...modVks].reverse().forEach((v) => { s += winUp(v); });
    return ps(s);
  }
  return run("xdotool", ["key", parts.join("+")]);
}

// -------------------------------------------------------------------- volume
export function volume(delta) {
  const d = Number(delta) || 0;
  if (isMac) {
    return osa(
      `set cur to output volume of (get volume settings)\n` +
      `set n to cur + (${d})\n` +
      `if n > 100 then set n to 100\n` +
      `if n < 0 then set n to 0\n` +
      `set volume output volume n`
    );
  }
  if (isWin) {
    // Media keys are the only route that respects the per-app mixer.
    const vk = d > 0 ? 0xaf : 0xae;
    const taps = Math.min(10, Math.max(1, Math.round(Math.abs(d) / 2)));
    return ps(WIN_KEY_SHIM + winTap(vk).repeat(taps));
  }
  return run("amixer", ["-q", "sset", "Master", `${Math.abs(d)}%${d > 0 ? "+" : "-"}`]);
}

export function mute() {
  if (isMac) return osa("set volume output muted not (output muted of (get volume settings))");
  if (isWin) return ps(WIN_KEY_SHIM + winTap(0xad));
  return run("amixer", ["-q", "sset", "Master", "toggle"]);
}

// ---------------------------------------------------------------------- zoom
// macOS Accessibility Zoom (System Settings > Accessibility > Zoom > keyboard
// shortcuts must be enabled); Windows Magnifier.
export function zoom(dir) {
  const inward = Number(dir) >= 0;
  if (isMac) return hotkey(`cmd+alt+${inward ? "=" : "-"}`);
  if (isWin) return hotkey(`win+${inward ? "=" : "-"}`);
  return Promise.resolve(false);
}

// -------------------------------------------------------------- app switcher
/* V38 — SPINNING NEVER SELECTS. Adi's ruling: the app is committed ONLY by a
   physical press of dial 5.

   The trouble with ANY idle timeout is that releasing the held modifier IS the
   selection — so a timeout does not "give up", it CHOOSES. That is exactly the
   random behaviour he reported, first at 900 ms and then at 2.5 s. Removing it
   outright is not safe either: a held Command that is never released leaves the
   machine unusable.

   THE WORKAROUND: the safety net CANCELS instead of committing. Escape while the
   switcher is open dismisses it WITHOUT switching, so the guard sends Escape and
   only then releases the modifier. Nothing is ever chosen by the passage of time.
   The guard is deliberately long — it is a deadlock breaker, not part of the
   interaction. */
const SWITCH_GUARD_MS = 25000;
let switchTimer = null;
let switchHeld = false;

function releaseModifier() {
  switchHeld = false;
  if (isMac) return osa('tell application "System Events" to key up command');
  if (isWin) return ps(WIN_KEY_SHIM + winUp(0x12));
  return Promise.resolve(false);
}

// The guard, and dial 5's hold: dismiss, then let go. Selects nothing.
async function switchAbandon() {
  switchTimer = null;
  if (!switchHeld) return true;
  if (isMac) await osa('tell application "System Events" to key code 53');    // Escape
  else if (isWin) await ps(WIN_KEY_SHIM + winTap(0x1b));
  await releaseModifier();
  return true;
}

function armGuard() {
  clearTimeout(switchTimer);
  switchTimer = setTimeout(switchAbandon, SWITCH_GUARD_MS);
}

/* TURN — hold the modifier and tap Tab. It never releases, so the switcher stays
   open and only the highlight moves. */
export async function appSwitch(dir) {
  const forward = Number(dir) >= 0;
  if (isMac) {
    if (!switchHeld) { switchHeld = true; await osa('tell application "System Events" to key down command'); }
    await osa(`tell application "System Events" to key code 48${forward ? "" : " using {shift down}"}`);
  } else if (isWin) {
    let s = WIN_KEY_SHIM;
    if (!switchHeld) { switchHeld = true; s += winDown(0x12); }
    if (!forward) s += winDown(0x10);
    s += winTap(0x09);
    if (!forward) s += winUp(0x10);
    await ps(s);
  } else {
    return false;
  }
  armGuard();
  return true;
}

/* PRESS — commit. Releasing the modifier is what chooses the highlighted app,
   exactly as letting go of Command does on the keyboard. A no-op when the
   switcher is not open, so the press is never destructive. */
export function appSwitchCommit() {
  clearTimeout(switchTimer);
  switchTimer = null;
  if (!switchHeld) return Promise.resolve(true);
  return releaseModifier();
}

export function appSwitchCancel() { clearTimeout(switchTimer); return switchAbandon(); }
export function appSwitchHeld() { return switchHeld; }


/* ---------------------------------------------------------------- type text
   V15 — the delay calculator's value key types its figure into whatever has
   focus. `key()` above sends ONE numpad token; this sends a short string.

   The payload is VALIDATED, not filtered. The first cut of this stripped
   everything outside [0-9.-] and typed whatever survived — which meant
   "no-digits-here" collapsed to "--" and got typed, because filtering can
   SYNTHESISE a valid-looking payload out of garbage. Caught by probing the
   running service, not by the unit test, whose sample happened to contain no
   hyphen.

   A plain decimal number is the only thing this verb exists to send, so it is
   also the only thing accepted. Nothing that passes can break out of the
   AppleScript literal below — there is no quote, backslash or newline in the
   grammar — so there is still nothing to escape. */
const TYPEABLE = /^-?\d{1,15}(\.\d{1,6})?$/;

export function type(text) {
  const s = String(text == null ? "" : text).trim();
  if (!TYPEABLE.test(s)) return Promise.resolve(false);
  if (isMac) return osa(`tell application "System Events" to keystroke "${s}"`);
  if (isWin) {
    // Reuse the numpad virtual keys already proven by key(): every character
    // that survives the filter has an entry, so a digit types as a real
    // keystroke rather than through SendKeys.
    const VK = { ".": WIN_VK.decimal, "-": WIN_VK.minus };
    let script = WIN_KEY_SHIM;
    for (const ch of s) {
      const vk = /[0-9]/.test(ch) ? WIN_VK[ch] : VK[ch];
      if (vk != null) script += winTap(vk);
    }
    return ps(script);
  }
  return run("xdotool", ["type", "--clearmodifiers", s]);
}

// -------------------------------------------------------------------- launch
export function launch(app) {
  const name = String(app || "").trim();
  if (!name) return Promise.resolve(false);
  if (isMac) return run("open", ["-a", name]);
  if (isWin) return ps(`Start-Process ${JSON.stringify(name)}`);
  return run("xdg-open", [name]);
}

/* ===========================================================================
   V33 — OS NAVIGATION for the Root Hub dials.

   Every verb here is ONE CONCEPT with two platform spellings, exactly like
   ACTIONS below: a module says "next tab" and never learns whether that is
   Cmd or Ctrl. That is the whole reason these live in the service rather than
   being hotkey() strings in root.js — the frontend cannot know the platform.

   ON SCROLLING, HONESTLY. macOS System Events cannot synthesise a scroll-wheel
   event; neither can keybd_event on Windows. So a scroll here is ARROW KEYS,
   repeated, which is what actually scrolls a focused view in nearly every app.
   It is not pixel-smooth and it will not scroll a view that ignores arrows.
   True wheel events would need a native helper (cliclick, or a node addon) and
   that is a decision worth taking on its own rather than smuggling in here.

   Repeats are batched into ONE osascript / ONE PowerShell call. Six ticks must
   not be six process spawns — that is the difference between a responsive dial
   and a dial that queues up half a second of shell.
   =========================================================================== */

const REPEAT_CAP = 12;                 // a fast flick must not spawn a storm

function repeatKey(target, times) {
  const n = Math.max(1, Math.min(REPEAT_CAP, Math.abs(times | 0) || 1));
  if (isMac) {
    const code = MAC_SPECIAL[target];
    if (code == null) return Promise.resolve(false);
    return run("osascript", ["-e",
      `tell application "System Events"\nrepeat ${n} times\nkey code ${code}\nend repeat\nend tell`]);
  }
  if (isWin) {
    const vk = WIN_SPECIAL[target];
    if (vk == null) return Promise.resolve(false);
    let script = WIN_KEY_SHIM;
    for (let i = 0; i < n; i++) script += winTap(vk);
    return ps(script);
  }
  return run("xdotool", ["key", "--repeat", String(n), target]);
}

// Dial 1 / Dial 2 — scroll the focused view. delta sign gives the direction.
export function scroll(axis, delta) {
  const d = Number(delta) || 0;
  if (!d) return Promise.resolve(false);
  const vertical = String(axis || "y").toLowerCase() !== "x";
  const target = vertical ? (d > 0 ? "down" : "up") : (d > 0 ? "right" : "left");
  return repeatKey(target, d);
}

// Dial 1 push / Dial 2 push — jump rather than crawl.
export function pageDown() { return hotkey("pagedown"); }
export function home() { return hotkey("home"); }

/* Dial 3 — APPLICATION zoom, which is Cmd/Ctrl +/-. Deliberately not the same
   thing as zoom() above: that one is cmd+alt+= , the macOS SYSTEM magnifier
   (D12 era). A dial labelled "Zoom" on a hub next to a browser means the page. */
export function appZoom(dir) {
  const inward = Number(dir) >= 0;
  const mod = isMac ? "cmd" : "ctrl";
  return hotkey(`${mod}+${inward ? "=" : "-"}`);
}
export function appZoomReset() { return hotkey(`${isMac ? "cmd" : "ctrl"}+0`); }

/* Dial 4 — tabs. Ctrl+Tab is the tab cycle on BOTH platforms (it is not Cmd on
   macOS), while new/close are Cmd on macOS and Ctrl on Windows. Getting that
   asymmetry right is exactly what this layer is for. */
export function tab(dir) {
  return Number(dir) >= 0 ? hotkey("ctrl+tab") : hotkey("ctrl+shift+tab");
}
export function tabNew() { return hotkey(`${isMac ? "cmd" : "ctrl"}+t`); }
export function tabClose() { return hotkey(`${isMac ? "cmd" : "ctrl"}+w`); }

/* Dial 5 — cycle applications. appSwitch() already exists above and is BETTER
   than a pair of hotkey() calls would be: it holds the modifier down across
   ticks and releases it only after the dial goes quiet, so the switcher stays
   open while you spin instead of committing on every detent. Reused as-is. */

// Dial 5 push — the "show me everything" gesture each platform already has.
export function missionControl() {
  if (isMac) return hotkey("ctrl+up");      // Mission Control
  if (isWin) return hotkey("win+tab");      // Task View
  return Promise.resolve(false);
}

/* ===========================================================================
   V36 — WINDOW LAYOUTS: snap left half / fill / snap right half.

   HOW ELGATO DOES IT, since Adi asked me to look: their Window Mover plugin
   ships a compiled native Node addon (bin/addon/mac/System.node) and drives the
   macOS Accessibility API directly. It uses osascript for exactly one thing —
   resolving an app's display name — and nothing else.

   We have no addon, but AppleScript's System Events exposes the same AX
   attributes: `position of front window` and `size of front window` are readable
   AND writable, which is enough to place a window precisely.

   THE ONE THING IT CANNOT DO is read NSScreen.visibleFrame. So the usable area is
   derived: the menu bar height is readable exactly (30 pt here), and the Dock is
   estimated from `dock size`, which AppleScript reports as a NORMALISED 0-1 value
   rather than pixels. The estimate below is within roughly ten points. Being
   exact needs the native addon Elgato ships; this is stated rather than hidden.

   All of the arithmetic happens INSIDE one AppleScript so a key press costs a
   single process spawn, not three.
   =========================================================================== */

/* V38 — THE NINE NATIVE WINDOW STATES, driven through macOS's OWN menu.

   The geometry approach (writing AX position/size) works and is kept as a
   fallback, but it can only express SINGLE-window states. Three of the nine Adi
   asked for — Left & Right, Left & Quarters, Quarters — are macOS "Arrange"
   commands that place TWO OR MORE windows, which no single frame write can do.

   So these click the real menu items, enumerated from this machine rather than
   guessed:

     Window > Fill | Center
     Window > Move & Resize > Halves:   Left | Right | Top | Bottom
                            > Quarters: Top Left | Top Right | Bottom Left | Bottom Right
                            > Arrange:  Left & Right | Left & Quarters | Quarters | ...
                            > Return to Previous Size
     Window > Full Screen Tile > Left of Screen | Right of Screen

   TWO HONEST LIMITATIONS: these are menu item NAMES, so English-only, and an app
   without the system-provided Window menu has nothing to click. Both are why the
   geometry fallback is retained for the states it can express.

   Full Screen is NOT a menu item — it is Ctrl+Cmd+F, which is both more robust
   and exactly what the green traffic-light button does. */

const MENU_TILES = {
  left:         ['Move & Resize', 'Left'],
  right:        ['Move & Resize', 'Right'],
  top:          ['Move & Resize', 'Top'],
  bottom:       ['Move & Resize', 'Bottom'],
  topleft:      ['Move & Resize', 'Top Left'],
  topright:     ['Move & Resize', 'Top Right'],
  bottomleft:   ['Move & Resize', 'Bottom Left'],
  bottomright:  ['Move & Resize', 'Bottom Right'],
  leftright:    ['Move & Resize', 'Left & Right'],
  leftquarters: ['Move & Resize', 'Left & Quarters'],
  quarters:     ['Move & Resize', 'Quarters'],
  restore:      ['Move & Resize', 'Return to Previous Size'],
  fill:         [null, 'Fill'],
  center:       [null, 'Center'],
};

// The states a frame write can also express, for the fallback.
const GEOM_TILES = { left: [0, 0.5], right: [0.5, 0.5], fill: [0, 1] };

/* Walk to the frontmost process that actually HAS a window. The Stream Deck app
   itself is the obvious counter-example and raises -1719 ("Invalid index")
   rather than failing quietly — found by running this, not by compiling it. */
const FRONT_WITH_WINDOW = [
  '  set target to missing value',
  '  repeat with pr in (every application process whose frontmost is true)',
  '    if (count of windows of pr) > 0 then',
  '      set target to pr',
  '      exit repeat',
  '    end if',
  '  end repeat',
].join("\n");

function clickWindowMenu(submenu, item) {
  const path = submenu
    ? `menu item "${item}" of menu 1 of menu item "${submenu}" of menu 1 of menu bar item "Window" of menu bar 1`
    : `menu item "${item}" of menu 1 of menu bar item "Window" of menu bar 1`;
  return run("osascript", ["-e", [
    'tell application "System Events"',
    FRONT_WITH_WINDOW,
    '  if target is missing value then error "no window"',
    '  tell target',
    `    if not (exists ${path}) then error "no menu item"`,
    `    if not (enabled of ${path}) then error "disabled"`,
    `    click ${path}`,
    '  end tell',
    'end tell',
  ].join("\n")]);
}

/* Geometry fallback — an exact frame write. Needs no menu and no English, and is
   the only path that works in an app whose Window menu lacks the tiling items.

   It cannot read NSScreen.visibleFrame (AppleScript has no access), so the usable
   area is derived: menu bar exact, Dock estimated from a NORMALISED `dock size`.
   Within about ten points. Elgato's Window Mover ships a native addon for exactly
   this reason. */
function geomFrame(xf, wf) {
  return run("osascript", ["-e", [
    'tell application "Finder" to set b to bounds of window of desktop',
    'set sw to item 3 of b',
    'set sh to item 4 of b',
    'tell application "System Events" to set mbh to item 2 of (get size of menu bar 1 of process "Finder")',
    'set dockH to 0',
    'tell application "System Events" to tell dock preferences',
    '  set dockEdge to (screen edge as string)',
    // NOT `hidden` — reserved in this context; System Events raises -10006. It
    // compiles cleanly and fails only when run.
    '  set dockAuto to autohide',
    '  set dockSz to dock size',
    'end tell',
    'if (dockEdge is "bottom") and (dockAuto is false) then set dockH to (16 + (dockSz * 112) + 16)',
    `set winX to round (sw * ${xf})`,
    `set winW to round (sw * ${wf})`,
    'set winY to mbh',
    'set winH to round (sh - mbh - dockH)',
    'tell application "System Events"',
    FRONT_WITH_WINDOW,
    '  if target is missing value then return "nowindow"',
    '  tell target',
    '    set position of window 1 to {winX, winY}',
    '    set size of window 1 to {winW, winH}',
    '  end tell',
    'end tell',
    'return "ok"',
  ].join("\n")]);
}

export async function windowLayout(which) {
  const key = String(which || "").toLowerCase().replace(/[^a-z]/g, "");

  // The green traffic light. A real shortcut, not a menu name.
  if (key === "fullscreen") {
    if (isMac) return hotkey("ctrl+cmd+f");
    if (isWin) return hotkey("win+up");
    return false;
  }

  if (isMac) {
    const menu = MENU_TILES[key];
    if (menu && await clickWindowMenu(menu[0], menu[1])) return true;
    const g = GEOM_TILES[key];               // menu missing/disabled/non-English
    if (g) return geomFrame(g[0], g[1]);
    return false;
  }

  if (isWin) {
    // Windows has halves natively; the multi-window arrange sets do not map.
    const WIN_TILES = {
      left: "win+left", right: "win+right", top: "win+up", bottom: "win+down",
      fill: "win+up", topleft: "win+left", topright: "win+right",
      bottomleft: "win+left", bottomright: "win+right",
    };
    const combo = WIN_TILES[key];
    return combo ? hotkey(combo) : false;
  }
  return false;
}

/* Named Root Hub actions (D11). Each is one concept with per-platform
   implementations, so modules never learn a platform spelling.

   D14 REVISED on hardware: Start / Run / Shell are Windows concepts, and mapping
   them to Launchpad / Spotlight / Terminal was a derived guess Adi rejected —
   they are now `mac: null`, so they simply do not exist on macOS. `app` names an
   application whose presence is probed, so a tile for something uninstalled
   (Cubase, Lynx Mixer) never appears rather than failing when pressed. Install
   the app and the tile shows up on its own. */
export const ACTIONS = {
  start:    { mac: null, win: () => hotkey("ctrl+escape") },
  run:      { mac: null, win: () => ps('(New-Object -ComObject "Shell.Application").FileRun()') },
  shell:    { mac: null, win: () => ps("Start-Process powershell") },
  taskmgr:  { macApp: "Activity Monitor", mac: () => launch("Activity Monitor"), win: () => ps("Start-Process taskmgr") },
  chrome:   { macApp: "Google Chrome",    mac: () => launch("Google Chrome"),    win: () => ps("Start-Process chrome") },
  lynx:     { macApp: "Lynx Mixer",       mac: () => launch("Lynx Mixer"),       win: () => ps('Start-Process "Lynx Mixer"') },
  cubase:   { macApp: "Cubase",           mac: () => launch("Cubase"),           win: () => ps("Start-Process Cubase") },

  /* V17 — the Ableton SMART LAUNCHER. The bundle name carries the version
     ("Ableton Live 11 Suite"), so this cannot be a fixed string the way Chrome
     can: it resolves the newest installed Live at press time. macOS `open -a` is
     idempotent — it focuses a running Live rather than starting a second one —
     so pressing the key is safe whether or not Live is already up.

     The Windows arm searches the two directories Ableton actually installs to
     and is UNVERIFIED, like every other Windows path in this file. */
  ableton:  {
    macApp: "Ableton Live",
    mac: () => { const app = findMacApp("Ableton Live"); return app ? launch(app) : Promise.resolve(false); },
    win: () => ps('$p = Get-ChildItem -Path "$env:ProgramData\\Ableton","$env:ProgramFiles\\Ableton" '
                + '-Recurse -Filter "Ableton Live*.exe" -ErrorAction SilentlyContinue | '
                + 'Sort-Object Name -Descending | Select-Object -First 1; '
                + 'if ($p) { Start-Process $p.FullName }'),
  },
};

const APP_DIRS = ["/Applications", "/System/Applications", "/System/Applications/Utilities",
                  path.join(os.homedir(), "Applications")];

/* The cache holds the bundle names AS WRITTEN. It used to lowercase on the way
   in, which was fine while the only question was "is it installed?" — but
   launching needs the real name to hand to `open -a`, and "ableton live 11
   suite" is not a thing on disk. Comparison lowercases at the call site now. */
let appCache = null;
function macApps() {
  if (appCache) return appCache;
  appCache = [];
  for (const dir of APP_DIRS) {
    try {
      for (const entry of fs.readdirSync(dir)) {
        if (entry.endsWith(".app")) appCache.push(entry.slice(0, -4));
      }
    } catch { /* directory may not exist */ }
  }
  return appCache;
}

// Cheap prefix match so "Cubase" finds "Cubase 13", which is how Steinberg
// versions its bundle names.
function macAppInstalled(name) {
  return !!findMacApp(name);
}

/* The best installed bundle whose name starts with `prefix`, NEWEST FIRST.
   "Ableton Live 11 Suite" and "Ableton Live 12 Suite" can both be installed and
   the launcher has to pick one; comparing the embedded version NUMERICALLY is
   the only ordering that survives Live 9 sitting next to Live 12. */
function findMacApp(prefix) {
  const want = String(prefix).toLowerCase();
  const hits = macApps().filter((a) => {
    const have = a.toLowerCase();
    return have === want || have.startsWith(want);
  });
  if (!hits.length) return null;
  const ver = (s) => { const m = /(\d+)/.exec(s.slice(prefix.length)); return m ? Number(m[1]) : -1; };
  hits.sort((a, b) => ver(b) - ver(a) || b.localeCompare(a));
  return hits[0];
}

/* Which named actions can actually run here. The Root Hub hides everything that
   reports unavailable, so the surface never shows a key that cannot work. */
export function actionAvailability() {
  const out = {};
  for (const [name, a] of Object.entries(ACTIONS)) {
    let available = false;
    if (isMac) available = !!a.mac && (!a.macApp || macAppInstalled(a.macApp));
    else if (isWin) available = !!a.win;
    out[name] = { available, platform: PLATFORM };
  }
  return out;
}

export function rescanApps() { appCache = null; return actionAvailability(); }

export function action(name) {
  const a = ACTIONS[String(name)];
  if (!a) return Promise.resolve(false);
  if (isMac) return a.mac ? a.mac() : Promise.resolve(false);
  if (isWin) return a.win ? a.win() : Promise.resolve(false);
  return Promise.resolve(false);
}

export const platform = PLATFORM;
