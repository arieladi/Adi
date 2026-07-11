# Rekordbox Cue Counter

DJ'ing Set Timer — enter each track's cue in & cue out to calculate the play time
and how long the overall set will be. Live at
**https://adiariel.com/tools/rekordbox_que_counter/** (GitHub Pages).

## Cues & time formats

Take the times straight from the rekordbox cues: **Cue in** is where you start
the track (e.g. hot cue A at `00:27.4`), **Cue out** is where you start the
*next* track (e.g. hot cue C at `06:10.2`) — that way the mix time between
tracks is counted in the set. **Play Time** = Cue out − Cue in (here `5:42.8`),
**Set time** = true running total (h:mm:ss, with `.t` when the total has tenths).

All of these mean 6 minutes 10.2 seconds (where tenths apply):

| Input | Style |
|---|---|
| `6:10.2` | rekordbox (m:ss.t — tenths of a second) |
| `6:10` | m:ss |
| `6.10` | the old Excel convention (fraction digits = seconds) |
| `6.10.2` | Excel convention + tenths |
| `1:02:37` | h:mm:ss (for hour-long values) |

## Saving — no passwords, no questions

Lists autosave to the browser as you type. **Save** commits to this repo, and
after a one-time setup it never asks anything again. Two ways to set it up:

1. **Per device (2 min):** ⋯ → GitHub sync → paste a fine-grained token
   (Repository access: only this repo · Contents: Read and write). It's stored
   on that device only; "Remove token" forgets it.
2. **For everyone (5 min, recommended):** deploy [`relay-worker.js`](relay-worker.js)
   to Cloudflare Workers (free) — it keeps the token server-side and only
   accepts `lists/<user>/*.json` writes. Put the worker URL in
   ⋯ → GitHub sync → Sync relay URL (or bake it into `DEFAULT_CONFIG.syncUrl`
   in `app.js`). Then **every visitor can save with zero setup**.

## Users

Default page is **Public** — open to everyone. The 👤 button opens a
username + password login (users are not listed anywhere in the UI).
Accounts live in [`users.json`](users.json) as PBKDF2-SHA256 hashes — a
client-side gate, not server-grade auth; don't reuse valuable passwords.

- Lists per user in `lists/<user>/` + `index.json` manifest.
- Limits: **100 lists per user**, **500 tracks per list**.
- Rename with the ✎ next to the list name; ⧉ duplicates; 🗑 deletes — deleting
  always asks a password (the user's own; Public uses the dedicated delete
  password).
- **Admin (adi):** log in as `adi` → ⋯ → Users… to add accounts, reset
  passwords, or remove users. Admin changes commit `users.json` and need the
  GitHub token on the device (the relay refuses non-list files on purpose).
- Every track has a **▶ link** — YouTube search for the track name by default,
  or any custom link set via ✎.

## Appearance

⋯ → Theme & size: Dark / Light presets, custom colors (background, cards,
lines, text, muted text, accent), and size sliders — overall size (zoom),
text size, row height. Saved per device, works on mobile and desktop.

## Files

- `index.html` / `style.css` / `app.js` — the app (no build step, no dependencies)
- `cuemath.js` — time parsing/formatting, shared with the Node test script
- `relay-worker.js` — optional Cloudflare Worker for zero-setup saving
- `seed.js` — generated: embedded users + seed lists so the app works offline
- `users.json` — user registry with password hashes (adi has `admin: true`)
- `lists/<user>/*.json` — saved lists + per-user `index.json`
- `icon.svg`, `manifest.webmanifest` — add-to-home-screen support

Local dev: `python3 -m http.server` in this folder, then open `http://localhost:8000`.
