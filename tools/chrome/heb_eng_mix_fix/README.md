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

## Layer B — the idle spell-checker (Grammarly-style)

**3 seconds after you pause typing**, the recent text (last ~300 chars) is
split into **overlapping 3-word chunks** and each unseen chunk is checked via
the **Google Suggest API** (`suggestqueries.google.com`, `client=chrome&hl=iw`).
Chunking matters, probed live: single words only produce autocomplete noise
("helo"→"helos") and long conversational sentences return nothing at all —
but short query-like chunks get real corrections ("now chec the"→"now check").
Chunks are cached, so pausing again only re-checks text that changed
(≤6 API calls per pause, newest text first).

Each response is **word-diffed (LCS alignment)** against what you typed, so
ALL misspelled words come back as individual fixes while Google's habit of
appending words ("… spam emails") or dropping them ("the spel why"→"the spell")
never leaks in — within a changed block, the fix targets only the best-matching
sub-range. Works for English and Hebrew typos alike.

A floating panel below the caret lists every fix:

- **click a row** — fix just that word;
- **Tab or "Fix all"** — fix everything (with `input` + `change` dispatched
  for React/Vue);
- **✕** — dismiss; those exact suggestions won't be offered again;
- keep typing — the panel steps aside and re-evaluates at the next pause.

**Charset gotcha:** with `hl=iw` the API responds in `windows-1255`, not UTF-8.
The worker decodes the raw bytes with the declared charset — `res.json()`
would silently mangle every Hebrew suggestion.

**Privacy note:** Layer B sends the text you pause on to Google. The popup's
*Spell check* toggle turns it off; Layer A is fully local.

## Files

- `hebrew_map.js` — bidirectional key map + heuristics (pure, Node-testable)
- `hebrew_dict.js` / `english_dict.js` — precision gates per direction
- `content.js` — the state machine, idle debounce, fix-panel apply/dismiss
- `ui.js` — caret math, override tooltip, multi-fix panel
- `content.css` — tooltip, glow, and badge styles
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
