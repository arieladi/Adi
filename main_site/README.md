# adiariel.com — GitHub Pages site + Cloudflare Worker backend

Same architecture as avastha.info/Materials:

- **GitHub Pages serves the site** — `/index.html`, `/admin.html`, `/404.html` at the
  repo root, custom domain `adiariel.com` (see `/CNAME`). The tools under `/tools/*`
  are plain same-origin pages, nothing special.
- **The Worker is only the backend** — [`adiariel-site-worker.js`](adiariel-site-worker.js)
  (worker name `adiariel-site-worker`, URL `https://adiariel-site-worker.adidatabase.workers.dev`):
  editable content in KV, admin auth (Bearer tokens), and images/audio from R2 at
  `/assets/*` with Range + ETag.

The public page has the default content **baked in** — it renders even if the worker
is down — then hydrates the live text from `GET /api/content` (public, CORS `*`).

## Deploy the worker (dash.cloudflare.com)

1. Workers & Pages → **adiariel-site-worker** → paste the whole file → **Deploy**.
2. Bindings (already created): KV → `ADIARIEL_SITE_KV`, R2 → `SITE_ASSETS`.
3. Secret **`DEFAULT_PASS`** (already set) — bootstrap admin password, used only to
   seed the auth record on the very first login.
4. **No domain/route changes** — adiariel.com stays on GitHub Pages.

## Admin — adiariel.com/admin

- **Content** — every text, link and list on the page. Lists are one-per-line
  (`label | url`, `Title :: description`, `Title :: description :: url`).
  Saving publishes instantly (KV `content:site`, merged over the defaults baked
  into the worker; the public page re-reads it on every load).
- **Assets** — upload / delete files in the R2 bucket. Served publicly at
  `<worker>/assets/<folder>/<name>` — copy the URL from the table and use it for
  banners, images and audio. Max ~95 MB per upload.
- **Security** — change the password. Signs out every other device.

Auth: salted PBKDF2 (100k iterations — the Workers cap), 32-byte Bearer session
tokens in KV (7 days, kept in the browser's sessionStorage), login rate-limited to
8 attempts / 10 min per IP. **Forgot the password?** Delete the `auth:admin` key in
the KV namespace — the next login re-seeds it from `DEFAULT_PASS`.

## Still to do

- Set the real **Patreon URL** — admin → Content → **Gigs & Support** → “Patreon — URL”
  (ships with a placeholder `https://www.patreon.com`). This one field feeds every
  “support” link on the site: the hero button, the bio note, and the support card.
- Photos / audio — upload via the Assets tab and use the worker URLs.

## Intro video → banner

`index.html` plays `media/adi_welcome.mp4` on load (`muted autoplay playsinline`).
The whole entrance is choreographed so nothing cuts abruptly:

1. During the **last ~3s of playback**, the still-playing frame (`.intro-frame`,
   which wraps the video) does everything at once: it crops + settles into the
   banner shape/place, the **drop shadow + edge fades cast in slowly** (`.lit`),
   and the **green accent line draws itself left→right** (`.lined`). So it visibly
   *turns into* the banner as the video finishes — no hard switch at the end.
2. At the video's end the frame's video is handed to `#heroBanner`, which already
   shows the same shadow/fades/line statically, so there's no visible change.
3. A welcome line **types itself** below the banner at a readable pace ("Welcome
   to Adi's site — the home for all Adi Ariel stuff"), holds a beat, then **fades
   away and collapses**.
4. The rest of the site **writes itself into its place** — the hero staggers in,
   and the sections fade in as you scroll.

Always animates (no reduced-motion short-circuit). There is no nav menu — visitors scroll.

- During playback the video is a **full-width 16:9 box centered vertically**, so
  the whole frame shows at a sane size on any device (portrait phones letterbox
  it instead of a fullscreen-cover zoom). The banner it settles into is full-bleed
  `aspect-ratio: 3.7/1` (max 58vh) so the whole "ADI" logo stays in frame and
  scales on any resize / resolution. The morph math mirrors those numbers
  (`ASPECT`, `MAXVH`, `MINH`).
- Plays on **every load** (no skip button, no once-per-session gate). Playback is
  driven by the declarative **`autoplay` attribute** — NOT a JS `.play()` at load,
  because the two raced and made `.play()` reject on cached loads, which skipped
  the intro on desktop. JS only nudges `.play()` after a beat / on first
  interaction if a browser blocks muted autoplay, and never skips the video.
  `prefers-reduced-motion` plays the video but skips the crop/rise + typewriter.
- Served by GitHub Pages from `/media/` (2.3 MB) — swap the file (keep the name)
  to change it. A `poster` (`media/adi_welcome_poster.jpg`, the video's first
  frame) shows instantly while the video buffers, so the first load opens on the
  scene instead of a blank dark screen. Regenerate it after swapping the video:
  `ffmpeg -ss 0 -i media/adi_welcome.mp4 -frames:v 1 -q:v 4 media/adi_welcome_poster.jpg -y`.

## Sections

Five sections, revealed top-to-bottom: **01 Electronic Music** (Avastha + solo),
**02 Collabs** (collaborations + gig bookings), **03 Tools** (free tools + a
Patreon note), **04 About** (bio + contact/socials), **05 Support** (the dedicated
Patreon call-to-action). Patreon touchpoints: hero button, Tools note, Support
section — all driven by one `support.url` field in `/admin`.

## Theme

Palette is **emerald & walnut** on near-black (`--c1 #5eead4`, `--c2 #10b981`,
`--c3 #c08a57`, `--bg #0a0d0b`) — pulled from the banner (chrome/foliage/wood).
Change the `:root` variables in `index.html` (and `admin.html`) to reskin.

## Local dev

Node test suite for the worker (mock KV/R2) lives in the Claude scratchpad —
ask Claude to “run the adiariel-site worker tests” and it will recreate it;
nothing extra is committed here.
