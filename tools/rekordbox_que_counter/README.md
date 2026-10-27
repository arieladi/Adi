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

- **Logged in once per device:** the profile is remembered in `localStorage`
  (`rqc.session.v5`) with no expiry, so reloads, new tabs and browser restarts
  stay logged in until **Log out** (👤 → Log out). Logging in or out in one tab
  does it in the other tabs too (each saves its unsaved lists first). Nothing
  secret is kept: no password, hash or token, only `{id,label,admin}` — list
  files are open on the relay anyway, so it is a convenience, and every check
  that matters is the Worker's. On each load the app quietly asks the relay's
  roster whether the account still exists: removed by the admin, it logs out
  with a toast; relay unreachable, it stays logged in. A login kept by an
  earlier version (per tab, `sessionStorage` `rqc.session.v4`) moves over. The
  admin password is never stored: after a reload, ⋯ → Users… asks for it again
  (memory only, for that page). Limit: Safari (iPhone / iPad / Mac) clears a
  site's storage when it has not been opened for 7 days of Safari use — that
  logs out (and drops the undo history) there; the app added to the Home Screen
  is exempt.
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
- **Start + End markers:** two markers bracket the part of the set you plan to
  play — the **Start marker** right before the start track and the **End
  marker** right after the end track: each an accent line across the list in a
  gap of its own (neither ever covers a row, nor does a move's preview put one
  over a row), with a pill on it. The End's pill shows the time from the start
  of the start track to the end of the end track (`0:43:17`) and
  `#3 → #10 · 8 tracks`; the Start's is compact and tinted, `Start · #3`. A set
  plays its start track from 0:00, not from its cue in, so that intro is added
  to the time and noted on the Start pill (`Start · #9 · +0:46`); a start track
  cued at 0 adds nothing. The Set time column is unchanged. Both
  are always there (a list has a track): until you move them they sit before
  the first track and after the last, so the End reads the whole set. **Drag a
  pill** (mouse or pen at once; touch: by its ⇕ at once, anywhere else on it
  after a short hold — a quick swipe scrolls the list, on the pill too): it
  lifts and follows the pointer, snaps to the nearest track boundary (a line
  shows where), the tracks from the Start to the End light up and the End pill
  and the S → E box count live; near the top or bottom the list scrolls. Let go
  to set it there — one undo step, autosaved; the rows the gap moved past slide
  into place. The Start never goes below the End, nor the End above the Start;
  Escape (or a cancelled touch) puts it back. Focused, a pill is a slider:
  ↑ / ↓ move it one track (a run of presses is one undo step), Home / End jump
  as far as it can go (the Start: the first track / the End; the End: the Start
  / the last track). The tint and the **S → E** box show only while the range
  is less than the whole list; click / tap the box to bring the End marker into
  view, centred; its ✕ puts both back round the whole list (one undo step).
  Saved with the list as `markStart` / `markEnd` on the track: no `markStart`
  is the first track (dropping the Start on it clears the mark), no `markEnd`
  the last one (the End keeps a mark wherever it is dropped). Both travel with
  their track when it moves — a move that puts the end above the start lets it
  go back to the last track (an end above the start, from elsewhere, counts as
  none). Deleting the end track moves the End up a track; deleting the start
  track moves the Start on to the next one (never past the End).

## History

Every list has a timeline of its versions — ⋯ → **🕘 History…**, or a click on
the save status (phones: the status in the summary line). It is built from the
list file's commits on GitHub, so it holds every save by every user and device,
oldest at the top and newest at the bottom ("start to end"), plus a last
**Now · this device** entry while the list here differs from the newest saved
version (not saved yet; opening the history first refreshes the list from the
relay, so another device's newer save is never shown as this copy saved over
it). Each entry says who saved it — the user from the app's
own commit message (`cue counter — <user>: save "<name>"`; `public` reads
*Public (anyone)*), or for any other commit its author and first line, marked
*repo edit* — when (*Today 14:02*, *Yesterday 22:40*, *Sat 18 Oct 09:15*), and
what changed since the version before it (`#5 Cue out 6.1 → 6:20 · +1 track`,
`renamed to “…”`, `created · 39 tracks`, `deleted the list`; at most three
items, then `+N more`).

- **Start / End, as in the list:** the timeline's **End marker** sits in a gap
  right after the version shown (the newest at first) — drag it (mouse at once;
  touch by its ⇕, or anywhere on it after a short hold), tap a version, or focus
  it and use ↑ / ↓ / Home / End; Escape cancels a drag. The **Start marker**
  sits in a gap right before the version it is compared with (the base): *auto*
  at first — the version just before the End, following it as it is dragged —
  and pinned where you drag it (or move it with ↑ / ↓ / Home / End); its
  **↺ auto** (or Delete when focused) puts it back to auto. The versions from
  the Start to the End are tinted; the Start never goes below the End nor onto
  it — with the End on the oldest version there is no Start at all — and
  tapping a version at or above a pinned Start puts it back to auto.
- **The preview:** while the history is open the list is **read-only** — its
  rows are hidden (and left untouched), ↶ / ↷, editing and drags are off, and
  autosave of earlier edits goes on. In their place the End version is shown in
  the list's own layout (the table on desktop, the list lines on phones) with
  the changes since the base marked: a changed value is tinted with the old one
  under it, struck through (`was 6.1`); new tracks say **NEW**; removed ones stay
  where they were as struck-through ghosts; moved ones say `moved from #15`; a
  changed title shows the old one struck through; the version's own Start and
  End markers are drawn where they were, outlined when they moved onto another
  track (`was before #2`, `was after #30` — or the old track's name when the
  new one has its number: `was before “Kick”`, its track deleted); a mark on the
  first / last track is the default, so setting or clearing one there is no
  change; a rename shows in the banner. The banner sums it up —
  `7 changes · 3 cues, 1 added, 1 removed, 2 moved · Set time 3:58:02 → 4:02:11` —
  and **↓ Next change** steps from one changed row to the next.
- **Restore as new list** makes a NEW list of the End version, named
  `<its name> (restored 25 Oct 14:02)`, saved like any new list; the list itself
  is never written over (100 lists per user still applies).
- **Close** with ✕, Escape, the phone's Back button, or by switching list or
  logging in / out. On desktop the timeline is a panel on the right; on phones a
  sheet from the bottom, like the track sheet.
- **Matching tracks** (`histdiff.js`, pure; its Node test is `test_histdiff.js`,
  `node test_histdiff.js`): tracks have no ids, so the two versions are aligned
  by an LCS on the whole track (title, BPM, key, cues, link), then, between two
  of those, by one on the normalised title — of two copies of a title, the one
  that went is the one missing; a title found out of that order has moved
  (the most alike copy first); a leftover pair with another title is a renamed
  track when both its cues (or its link) are the same and more fields agree
  than differ, or, in the same place between two kept tracks, when two of BPM /
  key / cues / link agree and none differ (or one title is blank) — BPM, key and
  a 16-bar cue in alone are shared by too many tracks of a set; the most alike
  pair first, then the nearest. The rest are added / removed. Then field by field.
- **GitHub requests:** the versions come from
  `GET {ghApi}/repos/{owner}/{repo}/commits?path=…&sha={branch}` (newest 100 a
  page, **Load older versions** at the top for more) — unauthenticated, 60
  requests an hour per IP (a device token is sent if one is set). Each page is
  kept with its ETag and asked again with `If-None-Match`; the `304` saves the
  download, but unauthenticated it still counts against the 60 (only with a
  device token is it free), so every open, Load older and own save while the
  history is open costs one. Pages asked again only ever add versions: one that
  slid from page 1 to page 2 as new saves came in stays in the timeline.
  A version's content comes from `{ghRaw}/{owner}/{repo}/{commit}/{path}` and,
  never changing, is kept for good; only the versions shown, compared and
  scrolled into view are fetched, four at a time. Both caches live in IndexedDB
  `rqc-ver` (the undo history's `rqc` is untouched; memory only where IndexedDB
  is unavailable). GitHub's limit reached: *GitHub's history limit is reached
  until 15:20*, with the versions loaded before; its limit for the contents
  (`429`): the loading waits until it lifts and says so; no connection: a Retry
  button — a saved list whose versions could not load at all says so, it is not
  shown as a new one. `config.ghApi` / `config.ghRaw` default to
  `https://api.github.com` and `https://raw.githubusercontent.com` (the setup
  dialog stores only what differs from the defaults). A list never saved yet
  says *Not on GitHub yet — history starts with the first autosave.*

## Appearance

⋯ → Theme & size: Dark / Light presets, custom colors (background, cards,
lines, text, muted text, accent), and size sliders — overall size (zoom),
text size, row height. Saved per device, works on mobile and desktop.

### On phones

At 740px wide and below the table becomes a read-only list. Desktop is unchanged.

- **Pinned at the top** (the brand row and 👤 scroll away): one list bar line —
  the list (● while it has unsaved changes), ↶ ↷ and ⋯, which also holds New list,
  Duplicate, Rename and Delete list — then one summary line: set time · tracks ·
  status, plus S → E with its ✕ while the Start / End range is less than the
  whole list (tap S → E to bring the End marker into view) — then the column
  header.
- **Each track** is one list line: its number (accent-coloured inside the S → E
  stretch) over the ⠿ grip, the title on up to two lines with `140 BPM · F# · 0:54 → 6:53`
  under it, Play Time over Set time on the right. An empty track reads "Tap to
  add BPM, key & cues". The Start marker sits over the start track, its pill on
  one line (`Start · #3`, a finger's height); the End marker under the end
  track, its pill on two lines (the time over `#3 → #10 · 8 tracks`).
- **Tap a track** to open its sheet from the bottom: big inputs for name, BPM,
  key, cue in and cue out, the live Play Time / Set time, ▶ Open / ✎ Link, and
  Prev / Next; 🗑 Delete sits alone at the sheet's top left (the toast offers
  Undo). The track stays in view above the sheet — the start / end track
  together with its marker when there is room; when the keyboard (or a big
  size setting) leaves no room, the pinned bars scroll away meanwhile. Dragging
  a marker puts the sheet away, as a row drag does; the room it left
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
- `histdiff.js` — the history view's diff of two versions of a list (pure, also loads in Node);
  `test_histdiff.js` is its test: `node test_histdiff.js`
- `relay-worker.js` — Cloudflare Worker: holds the GitHub token + user registry
  (KV), does saving and all auth. Deploy per its header comment.
- `seed.js` — generated: embedded seed list only (no credentials) for offline use
- `lists/<user>/*.json` — saved lists + per-user `index.json` (no secrets)
- `icon.svg`, `manifest.webmanifest` — add-to-home-screen support

Local dev: `python3 -m http.server` in this folder, then open `http://localhost:8000`.
