# adi spell chekcer

A standalone Manifest V3 Chrome extension: one **Fix All** button that
spell-checks the focused text field via the free **LanguageTool** public API.

## How it works

1. Click inside any `<input>`, `<textarea>`, or `contenteditable` on a page.
2. Open the popup and hit **✓ Fix All**.
3. `content.js` (in the frame that owns the focused field) reads the full
   text and sends it to the service worker.
4. `background.js` POSTs it to `https://api.languagetool.org/v2/check` with
   `language=auto`.
5. The `matches` array (`offset` / `length` / `replacements`) is applied —
   first replacement per match, **in reverse order** (end → start) so earlier
   offsets stay valid while the string changes length under later edits;
   overlapping matches are skipped.
6. The corrected text is written back with the native value setter and
   `input` + `change` events (React/Vue-safe); contenteditable uses
   select-all + `insertText` to keep the undo stack.

## Free-API limits (handled)

| Limit | Behavior |
|-------|----------|
| 20 requests/minute | HTTP 429 → popup shows **"Rate limit reached — wait a minute."** |
| 20k chars/request | Rejected client-side → **"Text too long (20k char limit)."** |
| Unsupported language (e.g. Hebrew — LanguageTool detects `NoopLanguage`) | **"Language not supported by LanguageTool."** |

Other statuses: no focused field, empty box, text changed mid-check, network
error — each gets its own popup message.

## Files

```
adi_spell_chekcer/
├── manifest.json     MV3; host permission for api.languagetool.org only
├── background.js     service worker — the API call + error mapping
├── content.js        focused-field reader + reverse-order replacement
├── popup.html        the Fix All button + status line
├── popup.js          popup wiring
└── icons/            16/48/128 green check
```

## Install (unpacked)

1. `chrome://extensions` → enable **Developer mode**.
2. **Load unpacked** → select this folder.
