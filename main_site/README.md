# adiariel.com — Cloudflare Worker site

The whole site is **one file**: [`adiariel-site-worker.js`](adiariel-site-worker.js).
It serves the public page, a password-protected admin panel at `/admin`, images/audio
from R2 at `/assets/*`, and proxies `/tools/*` to GitHub Pages so the existing tools
keep their URLs.

## Deploy (dash.cloudflare.com)

1. Workers & Pages → **adiariel-site-worker** → paste the whole file → **Deploy**.
2. Bindings (already created):
   - KV namespace → variable name `ADIARIEL_SITE_KV`
   - R2 bucket → variable name `SITE_ASSETS`
3. Settings → Variables and Secrets → add **`DEFAULT_PASS`** (type *Secret*) —
   the bootstrap admin password. It is only used to seed the auth record on the
   very first login; after you change the password in `/admin` it is ignored.
4. Test on the `*.workers.dev` URL, then Settings → Domains & Routes →
   add `adiariel.com` and `www.adiariel.com` (www 301-redirects to the apex).

### Moving the domain off GitHub Pages

The repo root is currently served at adiariel.com by GitHub Pages (see `/CNAME`).
Once the domain is attached to the worker:

1. Remove the custom domain from the repo's GitHub Pages settings
   (Settings → Pages). Pages then serves the repo at `arieladi.github.io/Adi` again.
2. The worker proxies `/tools/*` → `https://arieladi.github.io/Adi/tools/*`,
   so `adiariel.com/tools/rekordbox_que_counter/` keeps working unchanged.
   (Optional plain-text var `TOOLS_ORIGIN` overrides that origin.)
   Until step 1 is done, `/tools` shows a note explaining the redirect loop.

## Admin panel — `/admin`

- **Content** — every text, link and list on the page. Lists are one-per-line
  (`label | url`, `Title :: description`, `Title :: description :: url`).
  Saving publishes instantly (stored in KV `content:site`, merged over the
  defaults baked into the worker).
- **Assets** — upload / delete files in the R2 bucket. Served publicly at
  `/assets/<folder>/<name>` with caching, ETag and HTTP Range support
  (audio seeking works). Max ~95 MB per upload.
- **Security** — change the password. Signs out every other device.

Auth details: salted PBKDF2 (100k iterations — the Workers cap), HttpOnly
`SameSite=Strict` session cookie (7 days), login rate-limited to 8 attempts /
10 min per IP. **Forgot the password?** Delete the `auth:admin` key in the KV
namespace — the next login re-seeds it from `DEFAULT_PASS`.

## Still to do

- Set the real **Patreon URL** (admin → Content → Music → “Patreon — URL”;
  ships with a placeholder `https://www.patreon.com`).
- Banner / photos / audio for v2 — upload via the Assets tab and reference the
  `/assets/...` URLs.

## Local dev

There is a Node harness (mock KV/R2, 55 headless tests) in the Claude scratchpad —
ask Claude to “run the adiariel-site tests / preview” and it will recreate it;
nothing extra is committed here.
