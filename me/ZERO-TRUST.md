# Locking down `adiariel.com/me` with Cloudflare Zero Trust

Target: only Adi reaches `/me`, proven by **two independent factors** — an mTLS client
certificate installed on his devices, plus a TOTP code from Google Authenticator.

Verified against the live account on **2026-08-02**.

---

## 0. Read this first — there is a prerequisite

**`adiariel.com` is currently NOT proxied through Cloudflare, so Access cannot protect
it yet.** Verified:

```
$ dig +short adiariel.com A
185.199.110.153     ← GitHub Pages
185.199.111.153     ← GitHub Pages
185.199.109.153     ← GitHub Pages
185.199.108.153     ← GitHub Pages

$ dig +short adiariel.com NS
rory.ns.cloudflare.com.
leia.ns.cloudflare.com.
```

The zone is on Cloudflare nameservers, but the A records are **DNS-only (grey cloud)** —
they hand visitors GitHub's IPs and browsers connect straight to GitHub. Cloudflare never
sees the request, so there is nothing for Access to intercept.

Access works by intercepting HTTP requests *at Cloudflare's edge*. No proxy, no Access.
**Step 1 fixes this and is not optional.**

> This does **not** move the domain off GitHub Pages. Pages still hosts and serves every
> byte; Cloudflare just sits in front. The 2026-07-12 outage came from removing content
> from Pages while DNS still pointed there — a different change entirely. This one is a
> per-record toggle you can revert in one click.

---

## 1. Proxy the domain (prerequisite)

1. Dashboard → **adiariel.com** → **DNS** → **Records**.
2. **Before touching anything**, check **SSL/TLS** → **Overview** and confirm the
   encryption mode is **Full (strict)**.
   - GitHub Pages serves a valid Let's Encrypt cert for `adiariel.com`, so Full (strict) works.
   - ⚠️ If it is set to **Flexible**, proxying causes an **infinite redirect loop** and the
     site goes down. Change it to Full (strict) *first*.
3. Back in **DNS**, click the grey cloud next to each `adiariel.com` A record → it turns
   **orange**. Do all four.
4. Verify — the IPs should now be Cloudflare's (`104.x` / `172.67.x`), not `185.199.x`:

   ```bash
   dig +short adiariel.com A
   ```

5. Load `https://adiariel.com` and confirm the site still works, including the intro video.

**Rollback:** click the orange cloud back to grey. Propagation is seconds.

---

## 2. Turn on Zero Trust

1. Dashboard → **Zero Trust**. If this is the first time, pick a **team domain**
   (e.g. `adiariel` → `adiariel.cloudflareaccess.com`) and the **Free plan**
   (up to 50 users — you need one).
2. Note that team domain; it is the login hostname and the enrollment URL below.

---

## 3. Create the Access application for `/me`

1. **Zero Trust** → **Access controls** → **Applications** → **Add an application**.
2. Choose **Self-hosted**.
3. Configure:
   - **Application name:** `Adi personal hub`
   - **Session duration:** `24 hours`
   - **Public hostname:** subdomain *(blank)*, domain `adiariel.com`, **path `me`**
4. Save and continue to policies.

> The path scope means only `adiariel.com/me*` is gated. The public site, `/he`, and
> `/tools` stay open to everyone.

---

## 4. Policy: allow only you

1. In the application → **Policies** → **Add a policy**.
2. **Name:** `Adi only` · **Action:** `Allow`
3. **Include** rule:

   | Rule type | Selector | Value |
   |---|---|---|
   | Include | Emails | `adidatabase@gmail.com` |

4. Save.

Anyone else who hits `/me` gets the Cloudflare login screen and is denied.

---

## 5. Second factor: Google Authenticator (TOTP)

Cloudflare shipped **Independent MFA** in April 2026 — Access prompts for a second factor
itself, so you do **not** need Google Workspace, Okta, or any external IdP.

**Enable it at the org level:**

1. **Zero Trust** → **Access controls** → **Access settings**.
2. Under **Allow multi-factor authentication (MFA)**, tick **Authenticator application**.
   (Also tick **Biometrics** if you want Touch ID as a backup — recommended, see §8.)
3. Set **Authentication duration** — `24 hours` is reasonable; use **Require every login**
   for maximum strictness.
4. Save.

**Enrol your phone:**

1. Go to `https://<your-team-domain>.cloudflareaccess.com/AddMfaDevice`.
2. Log in with a one-time PIN sent to `adidatabase@gmail.com`.
3. **Account** → **MFA devices** → **Add an MFA device** → **Authenticator application**.
4. Scan the QR with Google Authenticator, then enter the 6-digit code to confirm.
   - Manual entry: hash **SHA1**, time step **30 seconds**.
   - ⚠️ **Only one TOTP authenticator can be enrolled at a time.** If you want it on both
     your phone and iPad, scan the *same* QR code on both during this one enrolment.

**Require it on the app:**

1. Back in the `Adi personal hub` application → **Policies** → your `Adi only` policy.
2. Add a **Require** rule for the **Authenticator application** MFA method.
3. Save.

---

## 6. Second factor: mTLS client certificate

This proves *device* identity — a machine without the cert cannot even complete the TLS
handshake, which puts a wall in front of the login page itself.

### 6a. Create a CA and a client certificate

Run locally. Keep `ca.key` somewhere safe (1Password) — anyone holding it can mint certs.

```bash
mkdir -p ~/adi-mtls && cd ~/adi-mtls

# Root CA, 5 years
openssl genrsa -out ca.key 4096
openssl req -x509 -new -nodes -key ca.key -sha256 -days 1825 -out ca.pem \
  -subj "/CN=Adi Personal CA/O=adiariel.com" \
  -addext "basicConstraints=critical,CA:TRUE"

# Client key + CSR
openssl genrsa -out adi-macbook.key 2048
openssl req -new -key adi-macbook.key -out adi-macbook.csr \
  -subj "/CN=adi-macbook/O=adiariel.com"

# Sign the client cert, 2 years
openssl x509 -req -in adi-macbook.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
  -out adi-macbook.crt -days 730 -sha256

# Bundle into a .p12 for macOS/iOS import (set a strong passphrase when prompted)
openssl pkcs12 -export -out adi-macbook.p12 \
  -inkey adi-macbook.key -in adi-macbook.crt -certfile ca.pem
```

`basicConstraints=critical,CA:TRUE` on the root matters — Cloudflare **rejects** a CA
certificate without it.

Repeat the client block per device (`adi-iphone`, `adi-ipad`) so you can revoke one
device without re-issuing the others.

### 6b. Upload the CA to Cloudflare

1. **Zero Trust** → **Access controls** → **Service credentials** → **Mutual TLS**.
2. **Add mTLS Certificate**.
3. **Name:** `Adi Personal CA`
4. **Certificate content:** paste all of `ca.pem` (including the
   `-----BEGIN CERTIFICATE-----` lines).
5. **Associated hostnames:** `adiariel.com`
6. Save.

### 6c. Require the certificate

1. Application `Adi personal hub` → **Policies** → **Add a policy**.
2. **Name:** `Device certificate` · **Action:** `Allow`
3. **Include** rule:

   | Rule type | Selector | Value |
   |---|---|---|
   | Include | Common Name | `adi-macbook` |

   Use **Valid Certificate** instead if you want to accept any cert from your CA rather
   than naming each device.
4. Save.

> Use **Common Name** over **Valid Certificate** if you want per-device revocation — drop
> the CN from the rule and that laptop is locked out immediately, no CA rotation needed.

### 6d. Install the certificate

- **macOS:** double-click `adi-macbook.p12` → import into **login** keychain → enter the
  passphrase. Safari and Chrome will offer it automatically; you may need to set the cert
  to *Always Trust*.
- **iOS:** AirDrop or email the `.p12` to yourself → **Settings** → **Profile Downloaded**
  → install → then **Settings → General → About → Certificate Trust Settings** and enable
  full trust for `Adi Personal CA`.

---

## 7. Don't forget the API — `/me` is only half the surface

Gating `adiariel.com/me` protects the **page**. It does **not** protect
`finance.adidatabase.workers.dev`, which is where the actual financial data lives and is
reachable from anywhere on the internet.

Right now that API is defended by the `API_TOKEN` Bearer check (the worker fails closed —
503 — if the secret is missing, and returns 401 on a bad token). That is real protection,
but it is a single shared secret held in `sessionStorage`.

To bring the API up to the same bar, pick one:

**Option A — put the Worker on a proxied custom domain and gate it too** *(recommended)*

1. **Workers & Pages** → `finance` → **Settings** → **Domains & Routes** → **Add custom
   domain** → `finance.adiariel.com`. Worker custom domains are proxied automatically.
2. Create a second self-hosted Access application for `finance.adiariel.com`.
3. Add a **Service Auth** policy with the **Valid Certificate** selector so the browser's
   mTLS cert authorises the API calls too.
4. Update `API` in [`me/index.html`](index.html) and `ALLOWED_ORIGINS` in
   [`finance/wrangler.toml`](../finance/wrangler.toml) to the new hostname.
5. Disable the `workers.dev` route so the old URL stops answering:
   add `workers_dev = false` to `wrangler.toml` and redeploy.

**Option B — leave it on `workers.dev`** and rely on the Bearer token. Acceptable for now,
but rotate the token periodically:

```bash
printf '%s' "$(openssl rand -base64 33 | tr -d '/+=' | head -c 40)" | npx wrangler secret put API_TOKEN --name finance
```

---

## 8. Lock-out insurance

mTLS plus TOTP means **two things that can strand you**. Before you rely on this:

- Enrol **Biometrics (Touch ID)** as a second MFA method alongside TOTP, so a lost phone
  is not fatal.
- Keep `ca.key` and the `.p12` passphrase in 1Password, off the laptop that holds the cert.
- Save the TOTP secret (the manual-entry string, not just the QR) in 1Password.
- You always retain dashboard access with your Cloudflare account login — you can delete
  the Access application from there to unlock yourself. That is the true break-glass path.

---

## 9. Verify it works

```bash
# Public site unaffected — expect 200
curl -sS -o /dev/null -w "%{http_code}\n" https://adiariel.com/

# /me now gated — expect 302 to the Cloudflare login
curl -sS -o /dev/null -w "%{http_code} %{redirect_url}\n" https://adiariel.com/me

# Without a client cert the TLS handshake itself should fail once mTLS is required
curl -sS https://adiariel.com/me
```

Then in a browser: open `https://adiariel.com/me`, pick the client certificate when
prompted, complete the email/TOTP login, and confirm the dashboard loads.

Finally, check **Zero Trust** → **Logs** → **Access** to see the allow/deny decisions.
