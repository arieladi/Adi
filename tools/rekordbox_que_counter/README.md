# Rekordbox Cue Counter

DJ'ing Set Timer — enter each track's cue in & cue out to calculate the play time
and how long the overall set will be. Live at
**https://adiariel.com/tools/rekordbox_que_counter/** (GitHub Pages).

## Cues & time formats

Take the times straight from the rekordbox cues: **Cue in** is where you start
the track (e.g. hot cue A at `00:27.4`), **Cue out** is where you start the
*next* track (e.g. hot cue C at `06:10.2`) — that way the mix time between
tracks is counted in the set. **Play Time** = Cue out − Cue in (here `5:42.8`),
**Set time** = true running total, shown as h:mm:ss.

All of these mean 6 minutes 10 seconds:

| Input | Style |
|---|---|
| `6:10.2` | rekordbox (m:ss.t — tenths of a second) |
| `6:10` | m:ss |
| `6.10` | the old Excel convention (fraction digits = seconds) |
| `1:02:37` | h:mm:ss (for hour-long values) |

> v2 formula change: math is now real time arithmetic (in tenths of a second),
> not the Excel's decimal trick. The Excel borrowed at 100 instead of 60, which
> drifted ±40 s on some rows — e.g. the Avastha set is really **4:02:11**, the
> sheet said 4:01:31. Unit tests in the build script pin the parser and totals.

## Users

Workspaces are defined in [`users.json`](users.json) (PBKDF2-SHA256 password
hashes — a client-side gate, not server-grade auth; don't reuse valuable
passwords). Default page is **Public**: everyone can view and build lists there.
Named users unlock their own workspace with a password.

- Each user's lists live in `lists/<user>/` with an `index.json` manifest.
- Limits: **100 lists per user**, **500 tracks per list** (enforced in the UI).
- Rename a list with the ✎ button next to its name (works on mobile and desktop).
- Every track has a **▶ link button** — opens a YouTube search for the track
  name in a new tab, or any custom link (YouTube/SoundCloud/…) set via ✎.
- Deleting a list always asks for a password: the user's own password, or for
  Public the dedicated delete password (hash in users.json).
- To add a user: generate a salt + PBKDF2-SHA256 hash (150k iterations, 32
  bytes, base64) and append to `users.json` — the build script in the repo
  history shows the recipe.

## Saving to GitHub

Lists autosave to the browser as you type. **Save** commits the list to this
repo through the GitHub Contents API — that needs a one-time per-device setup
(⋯ → GitHub sync): paste a fine-grained token (Repository access: only this
repo; Contents: Read and write), lock it behind a device password. The token is
AES-GCM encrypted and never leaves the device.

## Themes

⋯ → Theme: Dark / Light presets plus custom colors (background, cards, lines,
text, muted text, accent). Stored per device; applies to mobile and desktop.

## Files

- `index.html` / `style.css` / `app.js` — the app (no build step, no dependencies)
- `cuemath.js` — time parsing/formatting, shared with the Node test script
- `seed.js` — generated: embedded users + seed lists so the app works offline
- `users.json` — user registry with password hashes
- `lists/<user>/*.json` — saved lists + per-user `index.json`
- `icon.svg`, `manifest.webmanifest` — add-to-home-screen support

Local dev: `python3 -m http.server` in this folder, then open `http://localhost:8000`.
