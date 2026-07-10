# Rekordbox Cue Counter

Cue in / cue out set-time calculator for DJ sets — a web version of the Excel sheet,
with the exact same formulas. Live at **https://adiariel.com/tools/rekordbox_que_counter/**
(once pushed; served by GitHub Pages).

## Time convention (same as the Excel)

Values are **mm.ss**: `6.53` = 6 minutes 53 seconds.

| Column | Meaning | Excel |
|---|---|---|
| Play | `Cue out − Cue in`, plain decimal subtraction | D (`=SUM(C2)-B2`) |
| Total | running decimal sum of Play | E (`=SUM(D2)`, `=E2+D3`, …) |
| Set time | `int(Total)` minutes + `frac(Total)×100` seconds, carried into `h:mm:ss` | F |

The math runs on integer hundredths, so there is no floating-point drift and the
output matches the spreadsheet cell-for-cell (verified against the *Avastha 4h set*,
39 tracks → Total `240.91` → Set time `4:01:31`).

> Note (inherited from the Excel formula): the decimal subtraction borrows at 100,
> not at 60, so *Set time* can differ from real wall-clock elapsed time by ±40 s
> per row until a later carry absorbs it. Kept as-is on purpose — same numbers as
> the sheet.

## Saved lists

- Lists live in [`lists/`](lists/) as JSON; [`lists/index.json`](lists/index.json) is the manifest.
- The app autosaves your edits to the browser (`localStorage`) as you type.
- **Save** commits the list to this repo through the GitHub Contents API
  (one commit for the list, one for the manifest).
- Anonymous visitors can view lists (read via the Pages site / public API); saving
  requires login.

## Login

Static site — there is no server, so "login" works like this:

1. First save asks for a **fine-grained GitHub token**
   (github.com → Settings → Developer settings → Fine-grained tokens:
   Repository access = only `Adi`, Permissions → **Contents: Read and write**).
2. You pick a password. The token is encrypted with it (PBKDF2 310k → AES-GCM)
   and stored only in this browser. The password never leaves the device.
3. Next time, **Unlock** with the password. "Forget this device" wipes the stored token.

Anyone can *view* the page; only someone with a valid token can write to the repo.

## Files

- `index.html` / `style.css` / `app.js` — the app (no build step, no dependencies)
- `seed.js` — embedded copy of the Avastha list so the app works offline / before first push
- `lists/*.json` — saved lists + `index.json` manifest
- `icon.svg`, `manifest.webmanifest` — add-to-home-screen support

Local dev: `python3 -m http.server` in this folder, then open `http://localhost:8000`.
