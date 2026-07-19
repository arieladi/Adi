# HEB ENG MIX FIX (v7)

A Manifest V3 Chrome extension that fixes wrong-keyboard-layout typing in
**both directions** — instantly, locally, and fully offline. Nothing else.
(Spell-checking lives in its own extension: **adi spell chekcer**.)

> `nts bjns` → the machine backtracks to `מאד נחמד` and keeps converting.
> `מקקג אם ןצפרםהק ןא` → it backtracks to `need to improve it` — English
> typed on a Hebrew layout.

## The state machine

| State | What happens |
|-------|--------------|
| **1 Monitoring** | At each word boundary, the last two words are checked in **both directions**, then a sentence-level density gate must agree (see below). On a match, the machine walks **backwards over the whole wrong-layout run**. |
| **2 Override** | Replaces the run, then live-maps every subsequent keystroke into the intended language. |
| **3 Alert** | Yellow caret tooltip + field glow while overriding. |
| **4 Reset** | Exits on Alt+Shift, a keystroke that natively matches the target language, Escape, or blur. |

## False-positive protection (v7)

Technical English must never convert — `just go for 100mb llm ?` and
`cpu cores with 3ghz freqency` stay untouched. Three stacked gates:

1. **Digit gate** — a token containing any digit (`100mb`, `3ghz`, `x86`) is
   never a wrong-layout candidate; Hebrew words don't embed digits.
2. **Hebrew shape gate** — the mapped form must be orthographically plausible:
   final letters (ך ם ן ף ץ) only at the end (`llm`→ךךצ fails), and no word
   ends in a regular כ/מ/נ/צ (`mb`→צנ fails). The weak no-vowel path also
   requires ≥3 letters.
3. **Sentence density gate** — even a qualifying pair won't fire inside text
   that is predominantly English: the last ~8 tokens are classified
   (candidates vs. plain English; digits and Hebrew are neutral) and the fix
   only proceeds when candidates ≥ English tokens. `cpu runs at ghz xkz` is
   blocked (3 English vs 2); `send me tbh nkl` fires (2 vs 2).

Dictionary-confirmed words (`tbh`→אני, `nkl`→מלך, `vguko`→העולם) pass all
gates, so the flagship behavior is unchanged.

## Files

- `hebrew_map.js` — bidirectional key map + detection heuristics (pure, Node-testable)
- `hebrew_dict.js` / `english_dict.js` — precision gates per direction
- `content.js` — the state machine + manual Ctrl+E
- `ui.js` — caret math + override tooltip
- `content.css` — tooltip and field-glow styles
- `background.js` — seeds defaults on install (no network access of any kind)
- `popup.*` — Enabled / Manual toggles

## Modes (popup)

| Toggle | Effect |
|--------|--------|
| **Enabled** | Master on/off. |
| **Manual mode** | Machine off; select text and press **Ctrl+E** to convert it (direction auto-detected). |

## Install (unpacked)

1. `chrome://extensions` → enable **Developer mode**.
2. **Load unpacked** → select this folder.
3. After code changes: reload the extension there, then refresh the page.
