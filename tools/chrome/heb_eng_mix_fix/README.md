# HEB ENG MIX FIX (v3)

A Manifest V3 Chrome extension that fixes wrong-keyboard-layout typing in
**both directions** — instantly and locally for words, and with a
Grammarly-style floating badge (backed by the Google Suggest API) for whole
sentences.

> `nts bjns` → the machine backtracks to `מאד נחמד` and keeps converting.
> `מקקג אם ןצפרםהק ןא` → it backtracks to `need to improve it` and keeps
> converting — the reverse direction (English typed on a Hebrew layout).

## Layer A — the local bidirectional state machine (instant, offline)

| State | What happens |
|-------|--------------|
| **1 Monitoring** | At each word boundary, the last two words are checked in **both directions**. Wrong-layout evidence: maps to a dictionary word, has no English vowel (EN→HE), reverses to an English word, or breaks Hebrew final-letter rules — ך ם ן ף ץ mid-word is impossible Hebrew (HE→EN). |
| **2 Override** | On a match, the machine walks **backwards over the whole wrong-layout run** (so `מקקג אם ןצפרםהק ןא` converts wholesale), replaces it, then live-maps every subsequent keystroke. |
| **3 Alert** | Yellow caret tooltip + field glow while overriding. |
| **4 Reset** | Exits on Alt+Shift, a keystroke that natively matches the target language, Escape, or blur. |

## Layer B — Grammarly-style word-level spell-check

**A few seconds after you pause typing**, the recent text is scanned with a
**sliding window**: it is split into small overlapping windows (~3 words) and
each is sent to the **Google Suggest API** (`client=chrome&hl=iw`). Short
windows are what make Suggest behave as a spell-checker and they sidestep its
length limit entirely (probed live: 3-word windows catch every typo in a
sentence; 5-8 word windows miss most). Windows are cached, newest-first, capped
per pause.

`background.js` runs a **word-level LCS diff** of each window against Google's
suggestion and returns granular **correction objects**
`{ original, corrected, index, start, end }` — one per misspelled word (a
merge like "recieve alot" → "receive a lot" stays a single object; dropped
words are never swallowed).

### The transparent overlay (the hard part)

Native `<input>`/`<textarea>` can't wrap their own text in tags, so each flagged
word is highlighted through a **transparent mirror overlay**:

- a `<div>` pinned over the field's **padding box**, `pointer-events:none` so
  typing and clicking pass straight through;
- **content-box width = `clientWidth − paddingLeft − paddingRight`** (clientWidth
  already excludes the scrollbar) with the field's exact paddings and copied
  typography, so text wraps into the **same pixels** — getting width from
  clientWidth (not offsetWidth) is what prevents scrollbar-induced drift;
- `scrollTop`/`scrollLeft` mirrored so it tracks the field's internal scroll;
- misspelled words wrapped in `<mark>` (invisible text, red wavy underline).

For `contenteditable`, real geometry exists, so squiggles are drawn off
`Range.getClientRects()` instead. Measured alignment: within ~2–6px.

### Fixing

Hovering a flagged word shows a badge (`original → corrected`):

- **Fix** — replace just that word, carefully adjusting the caret;
- **Fix all (Tab)** — replace every current correction at once;
- **✕** — ignore that suggestion.

Edits are re-validated on every keystroke (a highlight whose word changed is
dropped), and applied with the native setter + `input`/`change` events so
React/Vue register them.

**Privacy note:** Layer B sends the windows you pause on to Google. The popup's
*Spell check* toggle turns it off; Layer A is fully local.

## Files

- `hebrew_map.js` — bidirectional key map + heuristics (pure, Node-testable)
- `hebrew_dict.js` / `english_dict.js` — precision gates per direction
- `content.js` — the state machine, sliding-window scan, correction model, fix/apply
- `ui.js` — caret math, override tooltip, transparent overlay + squiggles + hover badge
- `content.css` — tooltip, glow, overlay/mark/squiggle, and hover-badge styles
- `background.js` — Suggest API fetcher + validation (service worker)
- `popup.*` — Enabled / Manual / Sentence-suggestions toggles

## Modes (popup)

| Toggle | Effect |
|--------|--------|
| **Enabled** | Master on/off. |
| **Manual mode** | Machine off; select text and press **Ctrl+E** to convert it (direction auto-detected). |
| **Spell check** | Layer B on/off (off = nothing leaves the browser). |

## Install (unpacked)

1. `chrome://extensions` → enable **Developer mode**.
2. **Load unpacked** → select this folder.
3. After code changes: reload the extension there, then refresh the page.
