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

`index.html` plays `media/adi_welcome.mp4` full-screen on load (`muted` +
`autoplay` + `playsinline`). When it ends, the frozen last frame **crops to a
centered strip, then rises to the top** to become the site banner, and the rest
of the page reveals itself. There is no nav menu — visitors scroll.

- During playback the video is a **full-width 16:9 box centered vertically**, so
  the whole frame shows at a sane size on any device (portrait phones letterbox
  it instead of a fullscreen-cover zoom). The banner it settles into is full-bleed
  `aspect-ratio: 3.7/1` (max 58vh) so the whole "ADI" logo stays in frame and
  scales on any resize / resolution. The morph math mirrors those numbers
  (`ASPECT`, `MAXVH`, `MINH`).
- Plays on **every load** (no skip button, no once-per-session gate). Playback is
  driven by JS `.play()` only — the `autoplay` attribute was removed because it
  raced the JS call and made `.play()` reject on cached loads, which skipped the
  intro. If a browser still blocks muted autoplay, the first user interaction
  starts it; `prefers-reduced-motion` lands straight on the banner frame.
- Served by GitHub Pages from `/media/` (2.3 MB) — swap the file (keep the name)
  to change it.

## Theme

Palette is **emerald & walnut** on near-black (`--c1 #5eead4`, `--c2 #10b981`,
`--c3 #c08a57`, `--bg #0a0d0b`) — pulled from the banner (chrome/foliage/wood).
Change the `:root` variables in `index.html` (and `admin.html`) to reskin.

## Local dev

Node test suite for the worker (mock KV/R2) lives in the Claude scratchpad —
ask Claude to “run the adiariel-site worker tests” and it will recreate it;
nothing extra is committed here.
