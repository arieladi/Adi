# HEB ENG MIX FIX (v2)

A Manifest V3 Chrome extension that detects Hebrew typed with the **English
keyboard layout** and takes over with live conversion until you switch back.

> Type `nts bjns` → the extension backtracks to `מאד נחמד`, flashes a yellow
> alert, and converts everything you keep typing — until you hit Alt+Shift.

## The state machine

v2 is a bidirectional, 4-state machine rather than a one-shot word fixer:

| State | Name | What happens |
|-------|------|--------------|
| **1** | **Monitoring** (passive) | Watches word boundaries (Space/Enter). When **two consecutive words** are gibberish in English but valid Hebrew once mapped, it fires. (One word in *Aggressive mode*.) |
| **2** | **Active override** | Backtracks and converts the trigger words, then **intercepts every keystroke** and injects its Hebrew glyph before it lands. |
| **3** | **Visual alert** | A pulsing **yellow tooltip** pinned above the caret ("Auto-Fixing Hebrew — press Alt+Shift to switch") plus a **yellow glow** around the field. |
| **4** | **Reset** | Leaves override the instant you switch layout — **Alt+Shift**, or a **real Hebrew keystroke** arrives — or you press **Escape** / leave the field. |

### Why two words?

Mapping a single English word to Hebrew glyphs almost always produces
*something*, so a one-word trigger would wreck normal English typing. Requiring
**two consecutive** wrong-layout words (each either a known Hebrew word or a
token with no English vowel — an impossible shape for English) makes false
positives vanishingly rare. *Aggressive mode* trades that guard for a faster,
one-word takeover.

## Files

- **`hebrew_map.js`** — pure, testable core: the **bidirectional** SI-1452 key
  map (`toHebrew` / `fromHebrew`), single-key live translation (`keyToHeb`), the
  Hebrew-Unicode detector for STATE 4, and the wrong-layout heuristics.
- **`hebrew_dict.js`** — curated common-word set; strengthens detection and lets
  known words match even with attached prefixes (ה/ו/ב/ל/כ/מ/ש).
- **`ui.js`** — STATE 3 visuals. Caret coordinates via the "mirror div"
  technique for `<input>`/`<textarea>`, and the Selection rectangle for
  `contenteditable`.
- **`overlay.css`** — tooltip + field-glow styles, injected into every page.
- **`content.js`** — the state machine: one delegated capture-phase `keydown`
  listener driving passive detection, override interception, and reset.
- **`background.js`** — MV3 service worker; seeds defaults on install.
- **`popup.*`** — ON/OFF, Manual/Automatic, and Aggressive toggles.

## Modes (popup)

| Toggle | Effect |
|--------|--------|
| **Enabled** | Master on/off. |
| **Manual mode** | Turns the machine off. Nothing happens as you type; select text and press **Ctrl+E** to convert it. |
| **Aggressive mode** | (Automatic only) Enter override after **one** wrong word instead of two. |

## Install (unpacked)

1. Open `chrome://extensions` → enable **Developer mode**.
2. **Load unpacked** → select this `heb_eng_mix_fix` folder.
3. Click the icon to toggle modes.

## Extending the dictionary

Add words to `HEB_WORDS_LIST` in `hebrew_dict.js` (a `Set`, O(1) lookup).
The no-vowel heuristic already catches most wrong-layout tokens without it.
