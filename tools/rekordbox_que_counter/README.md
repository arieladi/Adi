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

## Saving & auth — the sync Worker

Everything talks to a **Cloudflare Worker** ([`relay-worker.js`](relay-worker.js))
that holds the GitHub token and the user registry server-side, so **no secrets
live in this public repo**. Its URL is baked into `DEFAULT_CONFIG.syncUrl` in
`app.js`.

- **Saving** just works with zero setup — list files (`lists/<user>/*.json`)
  are open, so every visitor can save. (A device GitHub token via ⋯ → GitHub
  sync is an optional fallback.)
- **Auth** is server-side. The Worker keeps usernames + salted PBKDF2 hashes in
  **Cloudflare KV** (private), and verifies every login, the public delete
  password, and all admin changes. The browser never receives a hash.
  > Note: Cloudflare Workers cap PBKDF2 at **100,000 iterations** — the KV
  > registry must use ≤ that (it uses 100k). Higher counts throw at runtime.

Worker setup (one time): create a KV namespace, bind it as `USERS_KV`, set
`GH_TOKEN`/`OWNER`/`REPO`/`BRANCH`/`ALLOW_ORIGIN`, deploy, then load the
registry once with a `{op:"seed"}` call (only works while KV is empty). Full
steps are in the header of `relay-worker.js`.

## Users

Default page is **Public** — open to everyone. The 👤 button opens a
username + password login; the Worker verifies it and returns only the public
profile (`{id,label,admin}`). Users are never listed in the public UI or repo.

- Lists per user in `lists/<user>/` + `index.json` manifest.
- Limits: **100 lists per user**, **500 tracks per list**.
- Rename with the ✎ next to the list name; ⧉ duplicates; 🗑 deletes — a
  logged-in user just confirms; **Public** deletion needs the dedicated delete
  password (Worker-verified).
- **Admin (adi):** log in as `adi` → ⋯ → Users… to add accounts, reset
  passwords, or remove users — authorized by the admin login password, checked
  by the Worker against the admin user in KV. No token, nothing in the repo.
- Every track has a **▶ link** — YouTube search for the track name by default,
  or any custom link set via ✎.

## Appearance

⋯ → Theme & size: Dark / Light presets, custom colors (background, cards,
lines, text, muted text, accent), and size sliders — overall size (zoom),
text size, row height. Saved per device, works on mobile and desktop.

## Files

- `index.html` / `style.css` / `app.js` — the app (no build step, no dependencies)
- `cuemath.js` — time parsing/formatting, shared with the Node test script
- `relay-worker.js` — Cloudflare Worker: holds the GitHub token + user registry
  (KV), does saving and all auth. Deploy per its header comment.
- `seed.js` — generated: embedded seed list only (no credentials) for offline use
- `lists/<user>/*.json` — saved lists + per-user `index.json` (no secrets)
- `icon.svg`, `manifest.webmanifest` — add-to-home-screen support

Local dev: `python3 -m http.server` in this folder, then open `http://localhost:8000`.
