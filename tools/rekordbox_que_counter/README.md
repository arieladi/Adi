# Rekordbox Cue Counter

DJ'ing Set Timer — enter each track's cue in & cue out to calculate the play time
and how long the overall set will be. Live at
**https://adiariel.com/tools/rekordbox_que_counter/** (GitHub Pages) — short link
**https://adiariel.com/q** (a redirect page at `/q/index.html` in the repo root).
(`avastha.info/q` is still to do — that site lives in the separate Avastha repo.)

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

- **Saving** is automatic, with zero setup — there is no Save button. List
  files (`lists/<user>/*.json`) are open, so every visitor can save. (A device
  GitHub token via ⋯ → GitHub sync is an optional fallback.)
- **Autosave** (per list): every change is kept on this device at once and
  committed 30 s after the last change — at most 3 min after the first one while
  edits keep coming (each save is two commits: the list and its `index.json`
  entry). Switching lists, logging in / out and leaving the page (closed; on a
  phone or tablet also when it goes to the background: keepalive requests, best
  effort) save at once; New list, Duplicate and Rename save right away. One save
  per list is in flight at a time, and a change made during it is never lost:
  the list stays unsaved (●) and goes in the next save. The list's
  `index.json` entry is updated after each save — that one
  entry only, read again and redone if another device wrote the index meanwhile
  — and other devices refresh by it: on load, on login and whenever the page
  comes back to the front. The status chip shows *Saving soon…* / *Saving…*,
  *All changes saved*, *Not saved yet — retrying* (failures retry after 5 s,
  15 s, 60 s, then every 5 min — a GitHub rate limit right away every 5 min —
  and as soon as the browser is back online), *Not saved — …* with the reason
  for a save that cannot work (a toast says it too; it is tried again after the
  next change) or *Offline — kept on this device*; the list name carries a ●
  while it has unsaved changes. A list left unsaved (closed offline) is saved on
  the next visit.
- **Never over another device's save:** a save only goes over the version this
  device's copy came from. If another device (or tab) saved the list since,
  that version takes the list's place, ↶ there brings this device's version
  back as one step, and it is kept as a list of its own too, named
  `<name> (conflict <date time>)` — a toast says so. A New list or Duplicate
  whose file name another device already uses is saved under the next free one
  (`friday-2.json`); the list keeps its name. File names are at most 60
  characters and never `index`.
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
- **Move a track** by dragging its **⠿** handle (mouse or touch); the other rows
  slide aside to preview the move and an OK / Cancel confirm commits it. ↶ / ↷
  (or Ctrl/⌘+Z, Ctrl/⌘+Shift+Z) undo and redo every change, moves included.
- **Undo history** is per list and remembers its last 2000 changes (the ↶ / ↷
  tooltips count the steps), from the list's first change to its last, kept on this
  device across reloads and closed tabs (IndexedDB `rqc` → `history`; each
  track's JSON is pooled once and a step stores only ids, written as appends
  and trims). Typing is grouped into one step per burst. Rename keeps the
  history, Duplicate and New list start empty, deleting a list deletes it. A
  list replaced by another device's version (a refresh, a save that found it)
  gets that as one step: the first ↶ brings back what this device had, and the
  redo steps go. With no IndexedDB (some private modes) it lives in memory for
  the session; should writing it fail even after reopening the database, it
  goes on in memory and its copy on disk is dropped on the next load rather than
  offering steps that skip the changes since.
- **Start + end marker:** press **S** (the Start column; on phones **Start** in
  the track's sheet) on the track you plan to start with. An **end marker**
  then appears right after the end track — after the last track until you move
  it: an accent line across the list, in a gap of its own (it never covers a
  row, nor does a move's preview put it over one), with a pill showing the time
  from the start of the S track to the end of the end track (`0:43:17`) and
  `#3 → #10 · 8 tracks`. **Drag the pill** (mouse or pen at once; touch: by its
  ⇕ at once, anywhere else on it after a short hold — a quick swipe scrolls
  the list, on the pill too): it lifts and follows the pointer, the end snaps to
  the nearest track boundary (a line shows where), the tracks from S to there
  light up and the pill and the S → E box count live; near the top or bottom
  the list scrolls. Let go to set the end there — one undo step, autosaved; the
  rows the gap moved past slide into place. It never goes above the S; Escape
  (or a cancelled touch) puts it back. Focused, the pill is a slider: ↑ / ↓
  move the end one track (a run of presses is one undo step), Home / End jump to
  the S track / the last one. The **S → E** box shows the same time; click /
  tap it to bring the marker into view, centred; its ✕ clears the start and the
  end, and so does unmarking S. Marking S below the end lets the end go back to
  the last track. One start and one end per list, saved with it (`markStart` /
  `markEnd` on the track, the end set by the marker); both travel with their
  track when it moves — a move that puts the end above the S lets it go back
  to the last track (an end above the S, from elsewhere, counts as none).
  Deleting the end track moves the end up a track (to the S at most); deleting
  the S track clears the end too.

## Appearance

⋯ → Theme & size: Dark / Light presets, custom colors (background, cards,
lines, text, muted text, accent), and size sliders — overall size (zoom),
text size, row height. Saved per device, works on mobile and desktop.

### On phones

At 740px wide and below the table becomes a read-only list. Desktop is unchanged.

- **Pinned at the top** (the brand row and 👤 scroll away): one list bar line —
  the list (● while it has unsaved changes), ↶ ↷ and ⋯, which also holds New list,
  Duplicate, Rename and Delete list — then one summary line: set time · tracks ·
  status, plus S → E with its ✕ when a start is set (tap S → E to bring the end
  marker into view) — then the column header.
- **Each track** is one list line: its number (accent-coloured inside the S → E
  stretch) over the ⠿ grip, the title on up to two lines with `140 BPM · F# · 0:54 → 6:53`
  and a Start tag under it, Play Time over Set time on the right. An empty
  track reads "Tap to add BPM, key & cues". The end marker sits under the end
  track, its pill on two lines (the time over `#3 → #10 · 8 tracks`).
- **Tap a track** to open its sheet from the bottom: big inputs for name, BPM,
  key, cue in and cue out, the live Play Time / Set time, Start, ▶ Open /
  ✎ Link, and Prev / Next; 🗑 Delete sits alone at the sheet's top left (the
  toast offers Undo). The track stays in view above the sheet — the end track
  together with its end marker when there is room; when the keyboard (or a big
  size setting) leaves no room, the pinned bars scroll away meanwhile. Dragging
  the end marker puts the sheet away, as a row drag does; the room it left
  under the list stays until a scroll up no longer needs it, so the list never
  jumps when the drag ends.
- **Typing a set straight through:** Prev / Next keep the field you are in, with
  its text selected so typing replaces it. The keyboard's Next / Enter goes to
  the next field; from Cue out it goes on to the next track's Cue in (or its
  name if it has none), and on the last track it adds a new one.
- **Close** with Done, by swiping the sheet down, or with the phone's Back
  button (Back closes the sheet, not the page).
- **Move a track:** drag its ⠿ grip; the rest of the row scrolls the list as
  usual. The sheet steps aside once the drag starts.

## Files

- `index.html` / `style.css` / `app.js` — the app (no build step, no dependencies)
- `cuemath.js` — time parsing/formatting, shared with the Node test script
- `relay-worker.js` — Cloudflare Worker: holds the GitHub token + user registry
  (KV), does saving and all auth. Deploy per its header comment.
- `seed.js` — generated: embedded seed list only (no credentials) for offline use
- `lists/<user>/*.json` — saved lists + per-user `index.json` (no secrets)
- `icon.svg`, `manifest.webmanifest` — add-to-home-screen support

Local dev: `python3 -m http.server` in this folder, then open `http://localhost:8000`.
