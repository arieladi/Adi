# HEB ENG MIX FIX

A Manifest V3 Chrome extension that automatically fixes Hebrew text accidentally
typed with the **English keyboard layout**.

> Type `tbh nkl vguko` → get `אני מלך העולם`

When you forget to switch your keyboard to Hebrew, the keys you press still land
as Latin gibberish. This extension detects those tokens as you finish each word
(on <kbd>Space</kbd> or <kbd>Enter</kbd>) and rewrites just that word in place —
without interrupting your typing flow.

## How it works

- **`hebrew_map.js`** — the SI‑1452 US‑QWERTY → Hebrew key map and the pure
  detection logic (`toHebrew`, `evaluate`, `inDictionary`). Position‑agnostic
  transliteration of keystrokes, including final letter forms (ך=l, ם=o, ן=i,
  ף=;, ץ=.).
- **`hebrew_dict.js`** — a curated set of common Hebrew words used as a
  **precision gate**: in the default mode a token is only converted when its
  mapped form is a real Hebrew word (prefixes ה/ו/ב/ל/כ/מ/ש are stripped when
  matching), so ordinary English is never mangled.
- **`content.js`** — a single delegated `keydown` listener (capture phase) on
  `document`. Work happens only at the Space/Enter word boundary, so steady‑state
  typing stays cheap. Handles `<input>`, `<textarea>`, and `contenteditable`.
- **`background.js`** — MV3 service worker; only seeds default settings on install.
- **`popup.html` / `popup.css` / `popup.js`** — ON/OFF, *Manual / Automatic*,
  and *Aggressive mode* toggles, persisted in `chrome.storage.local`. Changes
  apply live to open tabs.

### Manual vs. Automatic

Toggle in the popup:

| Mode | Behavior |
|------|----------|
| **Automatic** (default) | Fixes each word as you finish it (Space / Enter). |
| **Manual** | Nothing happens as you type. Select any text and press <kbd>Ctrl</kbd>+<kbd>E</kbd> to convert exactly that selection. |

### Aggressive gate (Automatic mode only)

| Setting | Behavior |
|---------|----------|
| **Off** (default) | Converts only dictionary‑confirmed Hebrew words (highest precision). |
| **Aggressive** | Converts any mappable token that isn't a common English word (higher recall). |

*Manual mode ignores the dictionary entirely — the selection is an explicit
request, so it is transliterated verbatim.*

## Install (unpacked)

1. Open `chrome://extensions`.
2. Enable **Developer mode** (top‑right).
3. Click **Load unpacked** and select this `heb_eng_mix_fix` folder.
4. Click the extension icon to toggle it on/off.

## Extending the dictionary

Add words to `HEB_WORDS_LIST` in `hebrew_dict.js` (stored in a `Set` for O(1)
lookup), or enable *Aggressive mode* for full coverage without a dictionary.
