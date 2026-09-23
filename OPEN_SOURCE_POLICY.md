# Open-Source Policy

**Permanent mandate from Adi, 2026-09-21. Applies to every project in this
repository and to every standalone repository Adi owns.**

Every project is open source. There is no intent to commercialise, sell or close
any of it. This document settles licensing and code-reuse questions in advance,
so that nobody — human or agent — has to stop and ask.

> **For agents:** when a licensing, copyright or third-party-reuse question
> comes up, read this file first. If the action is authorised here, do it
> without asking. Ask only about something this file does not cover.

---

## 1. Default licence: MIT

Original code is released under the **MIT License** wherever that is possible.

## 2. Escalation to GPLv3, and to AGPLv3

A project that copies code from a **GPL** or **LGPL** source is licensed
**GPLv3** instead, so that the copy is lawful. A project that copies code from
an **AGPL** source is licensed **AGPLv3** (§4). Escalation is per project and
automatic: no decision is needed, only a correct `LICENSE` file. The rule in
one line: **a project takes the strictest licence of the code it copies.**

Where each project stands today:

| Project | Licence | Why |
|---|---|---|
| `adi_daw/` | GPLv3 | ADR-0015; reads and copies from GPL references |
| `adi-surge/` | GPLv3 | a fork of Surge XT, which is GPLv3 |
| `adi-vital/` | GPLv3 | contains a fork of Vital, which is GPLv3 |
| AdiGuard (own repository) | to be applied — see §6 | was private/proprietary under its ADR-0018; this mandate supersedes that |

**Two legal limits the escalation rule has to respect:**

- **GPL-2.0-only code cannot be copied into a GPLv3 project.** The two licences
  are incompatible. GPL-2.0-*or-later* is fine, and is what Ardour uses; check
  for the words "or (at your option) any later version" before copying anything
  GPLv2.
- **LGPL-3.0 always fits.** LGPLv3 is GPLv3 plus extra permissions, so LGPL code
  can always be carried under GPLv3. Escalating is stricter than LGPL requires,
  but never wrong.

## 3. Pre-authorised reuse

Agents may copy, adapt and reuse code from reference repositories under these
licences **without asking**, provided the original copyright headers and licence
notices are kept and the source is named in the commit message:

| Licence | Authorised | Effect on our project |
|---|---|---|
| MIT, BSD-2/3-Clause | ✅ directly by Adi | none beyond keeping the notice |
| LGPL-2.1+/3.0 | ✅ directly by Adi | escalates to GPLv3 (§2) |
| GPL-2.0-or-later, GPL-3.0 | ✅ directly by Adi | escalates to GPLv3 (§2) |
| ISC, Zlib, Boost, Unlicense | ✅ same legal effect as MIT/BSD | none beyond keeping the notice |
| Apache-2.0 | ✅ same effect as MIT, **GPLv3-compatible only** | fine in GPLv3 or MIT projects; never into a GPLv2-only one |
| MPL-2.0 | ✅ file-level copyleft, GPLv3-compatible | copied files stay MPL; the project need not escalate |
| MS-PL (Microsoft Public License) | ✅ **only under `adi_daw/drivers/`** — ruled by Adi on 2026-09-22 (ADR-0121) for the Windows virtual audio device, derived from Microsoft's `sysvad` sample | **GPL-incompatible**: never into `src/`, never into anything a GPLv3 binary links. A driver is its own program: derived files stay MS-PL with the licence text beside them, our own files there are MIT, and the DAW's GPLv3 is untouched |
| **GPL-2.0-only** | ❌ | incompatible with GPLv3 (§2) |
| AGPL-3.0 | ✅ directly by Adi (2026-09-24) | escalates to AGPLv3 (§4) |
| No licence at all | ❌ | "all rights reserved" by default; read it, copy nothing |

The rows marked "same legal effect" were added when this file was written, so
that licences behaving like the ones Adi named are handled the same way. Strike
them if that was not the intent.

## 4. AGPL is allowed

**Ruled by Adi on 2026-09-24; this replaces the earlier ban.** AGPL-3.0 code
may be copied, adapted and reused like GPL code. Today that means
**ZLEqualizer** and **Zrythm** are usable sources, not only design references.

Why it is safe: AGPL is GPLv3 plus one extra rule — if someone runs the program
as a **network service**, its users must be able to get the source. GPLv3 §13
and AGPLv3 §13 explicitly allow the two to be combined. Our projects are open
source anyway, so the extra rule costs nothing, and every `adi_daw` build that
links JUCE (AGPL-3.0, ADR-0048) already carries it.

What it changes: a project that copies AGPL code becomes **AGPLv3** (§2) — its
`LICENSE` says so and its copied files keep their headers. Nothing escalates
before a copy actually happens. An MIT project that wants to stay MIT does not
copy AGPL (or GPL) code.

One thing AGPL code can bring that is not a licence term: **trademarks.**
Zrythm's licence adds a trademark notice under AGPL §7; copying its code is
fine, using its name is not (§5).

## 5. What "open source" does not cover

This policy governs **our code**. It gives no right to publish things we do not
own, and it changes nothing about keeping them out of every repository:

- **Commercial binaries and installers** — e.g. `Vanguard.dll`, the
  `adi-vital/Reason_*` installers.
- **Presets, banks, samples and wavetables from commercial products** — e.g.
  Vanguard's `.fxb` banks, and the wavetables extracted from NI Massive in
  AdiGuard's `Wavetable/`. These stay local and gitignored.
- **Other companies' names and trademarks.** "Pro-Q 3", "Pro-L 2", "EQ Eight",
  "K-Clip" and the like are working titles at most; a shipped plugin carries our
  own name. We clone behaviour, never branding.

## 6. Applying this to existing projects

Nothing here is applied retroactively by an agent that does not own the project.
In particular, **AdiGuard is a separate project with its own session and its own
private repository**: making it public and committing its `LICENSE` is
AdiGuard-session work, and changing a repository from private to public is an
outward-facing step for Adi to take or explicitly ask for. §5 applies to it in
full either way.
