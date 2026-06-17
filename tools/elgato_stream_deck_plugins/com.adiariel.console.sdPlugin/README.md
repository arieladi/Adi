# Adi Ariel Console (`com.adiariel.console`)

A Stream Deck + XL plugin: a BPM-driven **delay/reverb calculator** (Nick Fever style), a **numpad** that sends real OS keystrokes or doubles as a **standalone calculator**, and an **A4 = 442 Hz note frequency / wavelength** tool on the touchscreen.

---

## Target hardware — Stream Deck + XL

Built for the **Stream Deck + XL** (Elgato, 2026, $349.99): **36 LCD keys, 6 push-encoder dials, and a 161 × 14 mm touch strip** — their largest Stream Deck. This plugin maps directly onto it: keys for the delay grid + numpad, the 6 dials for BPM / 3 delay ranges / 2 acoustic controls, and the touch strip for the per-dial readouts.

Profile device type is **13** (Stream Deck + XL) in `manifest.json`. Software floor is Stream Deck app **6.6+**, on Windows 11 / macOS 13 per Elgato's stated requirements.

> The touch strip is **not** a free HTML/Canvas surface. It's split into per-dial segments, each a **200 × 100** logical canvas driven by JSON **layouts** + `setFeedback`. Each dial owns its segment — that *is* the "split view." With 6 dials you get 6 segments; the `tapPos` midpoint split (x < 100) is correct per segment.

---

## Gesture map

**BPM dial (Dial 0)** — default **143**
- Rotate → ±1 · Push → reset to 143 · **Hold dial** → accelerating sweep toward the last-turned direction (stops at 1 / 300)
- Touch **−/+** (left/right half) → ±1 · **Hold touch** → accelerating sweep to that limit

**Delay range dials (Dials 1–3 = Straight / Triplet / Dotted)**
- Rotate or tap **◀ / ▶** → shift the visible 4-note window (limits **1/1 … 1/128**). Default window: **1/4, 1/8, 1/16, 1/32**.

**Numpad toggle (top-right key, col 8 / row 0)**
- Short press → types `9` · **Long press → switch State A ⇄ State B**

**Acoustic / Calculator dials (Dials 4 & 5)**
- *State A (numpad):* Dial 4 scrolls **note** (C…B, wraps), Dial 5 scrolls **octave** (0–8). Touchscreen shows `Note Oct | Hz` and `Oct | cm`. Default: **C0 | 16.43 Hz | 2100.34 cm**.
- *State B (calculator):* touchscreen becomes the LCD. Dial 4 rotate → cycle operator (`+ − × ÷`), press → commit; **hold → clear**. Dial 5 rotate → backspace, press → **equals**. Numpad **Enter** also = equals. (12 keys can't hold operators, so operators live on the dials.)

---

## Math (verified)

```
Straight (ms) = 60000 / BPM * (4 / denom)
Triplet  (ms) = Straight * 0.667
Dotted   (ms) = Straight * 1.5
Freq     (Hz) = 1000 / ms

note freq (Hz) = 442 * 2^((midi - 69) / 12),  midi = 12*(octave+1) + noteIndex
wavelength(cm) = 34500 / Hz            // 345 m/s -> C0 = 2100.34 cm
```
Sanity: 143 BPM, 1/4 straight = **419.58 ms**; C0 = **16.43 Hz / 2100.34 cm**.

---

## Cross-platform (Windows + macOS)

This is a **Node.js plugin**: `src/plugin.js` is bundled into a single self-contained `bin/plugin.js` that runs on the Node runtime the Stream Deck app ships on **both Windows and macOS**. You build once (on any OS) and the same artifact installs on both — no Mac required to build, no per-platform binaries.

What makes it portable, and how it's enforced here:

- **No native modules.** The only imports are Node built-ins (`node:child_process`, `node:os`, etc.) plus the bundled SDK. Nothing needs `node-gyp` / platform compilation. (`ws`'s optional `bufferutil` / `utf-8-validate` speedups are guarded by try/catch and fall back to pure JS.)
- **OS-specific keystrokes are branched at runtime**, not at build time — `os.platform()` picks PowerShell (Windows), `osascript` (macOS), or `xdotool` (Linux). One bundle, correct behavior everywhere.
- **Lowercase filenames + forward slashes.** macOS is case-sensitive and Windows is not, so every asset file is lowercase and every path in `manifest.json` / code uses `/`. Verified: all manifest asset references resolve case-exactly.
- **`.gitattributes`** forces LF line endings so a Windows checkout doesn't introduce CRLF churn.
- **`CodePath`** (not `CodePathWin` / `CodePathMac`) — those split keys are only for compiled C++/C# plugins; a Node bundle is identical on both OSes, so one `CodePath` is correct.

CI (`.github/workflows/console-plugin.yml`, at the **repo root** — GitHub only runs workflows from there) builds and validates the bundle on `ubuntu-latest`, `windows-latest`, and `macos-latest` on every push that touches this folder, so cross-platform breakage is caught automatically.

---

## Build from source / GitHub

Clone, install, build:

```bash
git clone https://github.com/<you>/com.adiariel.console.sdPlugin.git
cd com.adiariel.console.sdPlugin
npm install
npm run build        # bundles src/plugin.js -> bin/plugin.js
```

`bin/plugin.js` is committed so the folder is installable straight from a clone; `npm run build` regenerates it after any change to `src/`.

---

## Build & install (dev)

Requires Node 20+ and the Stream Deck app 6.6+.


```bash
npm install
npm run build        # bundles src/plugin.js -> bin/plugin.js
```

Copy/symlink this whole `com.adiariel.console.sdPlugin` folder into the plugins directory:

- **Windows:** `%APPDATA%\Elgato\StreamDeck\Plugins\com.adiariel.console.sdPlugin`
- **macOS:** `~/Library/Application Support/com.elgato.StreamDeck/Plugins/com.adiariel.console.sdPlugin`

Or use the Elgato CLI from inside the folder:

```bash
npx streamdeck link            # registers the plugin for development
npx streamdeck restart com.adiariel.console
# package a single distributable when done:
npx streamdeck pack            # -> com.adiariel.console.streamDeckPlugin
```

`streamdeck pack` is the modern, cross-platform replacement for the old Windows-only `DistributionTool.exe`. It zips this `.sdPlugin` folder into one `com.adiariel.console.streamDeckPlugin` file that installs by double-click on **both** Windows and macOS — build it on whichever machine you have.

> Don't have the CLI scaffold yet? Easiest path: `npx @elgato/cli@latest create`, accept the Node template, then drop these files in. That guarantees the manifest schema matches your installed app version.

---

## The launcher = profile switch (one-time setup)

Clicking **Launch Adi Ariel Console** calls `streamDeck.profiles.switchToProfile(deviceId, "AdiArielConsole")`. The profile is declared in `manifest.json`, but a `.streamDeckProfile` is a **binary** file and can't ship as text — **create it once**:

1. In the Stream Deck app, add a profile named exactly **`AdiArielConsole`** for your Stream Deck + XL.
2. Arrange the Adi Ariel Console actions on it (see layout below).
3. Right-click the profile → **Export** → save `AdiArielConsole.streamDeckProfile` into this plugin's `Profiles/` folder (create it).
4. Put a **Launch Adi Ariel Console** key on your *default* profile — pressing it now takes over.

Until you export it, the launcher logs a friendly "create the profile first" error instead of switching.

---

## Suggested grid placement (delay + numpad on a key device)

```
col:  0      1     2      3     4      5      6     7     8
row0 1/1ms  1/1Hz 1/1ms 1/1Hz 1/1ms 1/1Hz    7     8     9*  (* = toggle)
row1 1/2ms  1/2Hz  ...    ...   ...    ...    4     5     6
row2 1/4ms  1/4Hz  ...    ...   ...    ...    1     2     3
row3 1/8ms  1/8Hz  ...    ...   ...    ...    0     .   Enter
        Straight     Triplet      Dotted        Numpad
```
Each **Delay Cell** key is configured in its Property Inspector: **category** (straight/triplet/dotted), **field** (ms/Hz), **row** (0–3). Each **Numpad Key** sets its **token**; tick **Toggle key** only on the top-right `9`.

---

## Dial map (6 dials)

| Dial | Role |
|---|---|
| 0 | BPM |
| 1 | Delay range — **Straight** (axis category set in PI) |
| 2 | Delay range — **Triplet** |
| 3 | Delay range — **Dotted** |
| 4 | Acoustic **note** (C…B) / calc operator (State B) |
| 5 | Acoustic **octave** (0–8) / calc equals + backspace (State B) |

All three range dials are independent — no paging or category-switching needed on the + XL.

---

## Notes & limits

- **OS keystrokes** use `child_process` (no native modules): Windows → PowerShell `SendKeys`, macOS → `osascript` numpad key codes, Linux → `xdotool`. **macOS:** grant the Stream Deck app **Accessibility** permission (System Settings → Privacy & Security → Accessibility), or keystrokes silently no-op. `SendKeys` emits digit characters, not raw numpad VKs — fine for almost everything; some games that read scancodes need a native module instead.
- **Long-press** is timer-based (500 ms) — the SDK has no native long-press event.
- **Touch has no release event**, so a touch-*hold* on BPM starts an auto-sweep that runs to the limit (this is exactly the "accelerate 1→300" behavior). Dial-hold sweeps and stops on release.

---

## Files

```
com.adiariel.console.sdPlugin/
├─ manifest.json            # actions, encoders, profile, OS, CodePath, Node runtime
├─ package.json             # build scripts + @elgato/streamdeck + dev tooling
├─ rollup.config.mjs        # src/plugin.js -> bin/plugin.js
├─ .gitignore               # ignores node_modules, *.streamDeckPlugin, logs
├─ .gitattributes           # forces LF line endings (Win/Mac parity)
├─ LICENSE                  # MIT (Adi Ariel) — change if you prefer another
├─ src/plugin.js            # all state, math, long-press, keystrokes, calculator, handlers
├─ bin/plugin.js            # built bundle (committed; regenerate via npm run build)
├─ ui/inspector.html        # shared Property Inspector (vanilla, no deps)
├─ layouts/                 # touchscreen layouts: bpm / range / acoustic / calc
├─ imgs/                    # plugin + per-action icons (placeholders)
└─ Profiles/                # you export AdiArielConsole.streamDeckProfile here
```

