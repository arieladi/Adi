# Working in parallel — two Claude agents, one project

Two Claude Code agents work on `adi-vst`, on different machines and different
accounts, synced only through git. This file is the protocol. Read it before
your first commit, and re-read it if you have been away.

This mirrors `adi_daw/collab/README.md` deliberately — same rules, same habits,
so neither agent has to hold two protocols in their head. The differences are in
the roster and in the repo topology, both of which are genuinely different here.

## Roster

| Agent | Machine | Owns | Cannot |
|---|---|---|---|
| **win** | Windows 11, MSVC 19.44 / x64 | `vital/**` C++, the Windows VST3 + standalone builds, `tools/extract_schema.py`, `tools/validator/**`, `ARCHITECTURE.md` | Build or test on macOS/clang/arm64; build for iOS |
| **mac** | macOS, Apple clang / arm64 | macOS builds of the fork and their portability fixes, `backend/**`, `mobile/**`, `.github/workflows/**`, review | Build or test the Windows VST3 |

Neither agent is senior to the other. Disagree in a PR, not by reverting.

The split is **by platform capability, not by taste**. iOS builds for Tool 1's
mobile client require macOS; the Windows VST3 requires MSVC. Everything else —
the FastAPI backend, the patch validator, the training pipeline — is portable
and goes to whoever has capacity, claimed in the table below.

## The three rules

**1. Never commit to `main`.** Branch, push, open a PR, merge it yourself when
CI is green. Branch names are prefixed with your agent name:

```
win/prompt-section-stub
mac/backend-patch-validator
```

In `vital/`, `main` has a second job: it mirrors `upstream/main` (ADR-0001) so
that `git diff upstream/main` stays meaningful. Committing to it breaks the one
command that tells us what we have actually forked.

**2. Claim before you write.** Add a row to the claims table below in your first
commit on a branch, and remove it when the branch merges. If a path you need is
claimed by the other agent, say so in your log file rather than editing it.

**3. Log what you did, in your own file.** `collab/win.md` and `collab/mac.md`.
Only ever write to your own. A shared log is a guaranteed merge conflict on
every push; splitting the files means neither agent ever resolves one.

## Current claims

One row per active branch. Delete your row when it merges.

| Path | Agent | Branch | Since |
|---|---|---|---|
| `adi-vst/` → `adi-vital/`, `.gitignore`, `OPEN_SOURCE_POLICY.md`; path-only edits across project docs/logs on Adi’s explicit rename instruction | linux | `linux/rename-adi-vital` | 2026-09-24 |
| `collab/**`, `docs/**` | win | `main` (bootstrap) | 2026-09-18 |

## Reserved ADR numbers

**Claim the number BEFORE writing the entry. Push immediately.** Mark the row
`used` once the entry exists, and leave it there.

Carried over from `adi_daw` ADR-0051, which this project's protocol mirrors (see
the top of this file). The argument transfers unchanged: "pull main before
writing an ADR" cannot work, because the collision does not happen at the pull.
It happens in the window between pulling and merging, which is however long the
work takes. Pulling earlier closes nothing.

**A row is never deleted, only marked.** An abandoned reservation is `burned` and
the number is never reused — a gap costs nothing.

**The Subject column is load-bearing.** A number reservation does not stop both
agents writing the same ADR; reading the other agent's subject does.

| Number(s) | Agent | Branch | Subject | Status |
|---|---|---|---|---|
| 0017 | mac | `mac/vst3-macos` | VST3 only; CLAP dropped from this repo | used |
| 0018 | mac | `mac/vst3-macos` | the macOS exporter defects (NO_AUTH, copy step, team id) | reserved |

## Before you start work, every time

```bash
git -C <Adi repo> checkout main && git pull
cat adi-vst/collab/README.md      # claims may have changed
cat adi-vst/collab/<other>.md     # what they did since you last looked
cat adi-vst/docs/DECISIONS.md     # settled questions; do not re-litigate
```

`ARCHITECTURE.md` is the how-it-works reference. `docs/DECISIONS.md` is the
append-only record of what was decided and why. Read the ADR log before
proposing a change to something that looks odd — most of the odd things are
load-bearing and the reason is written down.

## Repo topology — read this before cloning

`adi-vst/` is a project directory in the `arieladi/Adi` monorepo, exactly like
`adi_daw/` (ADR-0015). One repo, one clone, public.

```
arieladi/Adi
├── adi_daw/            the DAW — a DIFFERENT project
└── adi-vst/            this project
    ├── ARCHITECTURE.md
    ├── docs/DECISIONS.md
    ├── collab/
    ├── tools/
    └── vital/          ← SEPARATE REPO, gitignored by the monorepo
```

`vital/` is a fork of `mtytel/vital` with an `upstream` remote (ADR-0001), kept
out of the monorepo because vendoring it would lose `git diff upstream/main`,
currently the only thing that tells us what we have actually forked. (Size is
not the reason: the working tree is ~180 MB but the packed repo is 31.5 MiB.)

**The fork lives at `arieladi/adi-vst-synth` (public, GPLv3)** — ADR-0016. Clone
it into `adi-vst/vital`; it carries both `origin` (ours) and `upstream`
(mtytel), so `git diff upstream/main` shows the whole fork delta. `mac` is no
longer blocked on the C++.

Do not "fix" this by `git add`-ing `vital/` into the monorepo. The ignore rule is
deliberate and stops it becoming a stray gitlink.

### Staying out of adi_daw's way

`adi_daw` is under active daily development in this same repo and working tree,
by different sessions. Separation here is a **working practice**, not a
structure, so it has to be kept deliberately:

- **Touch only `adi-vst/**`.** Never edit, stage or revert anything under
  `adi_daw/`, even to "fix" something obvious. Raise it instead.
- **Stage explicit paths.** Never `git add -A` or `git add .` — the monorepo
  routinely holds other people's untracked work in progress.
- **Check before anything destructive.** Run `git status` and
  `git branch --show-current` as their **own step** and read the output before
  any `reset --hard`, `clean`, force-push or history rewrite. Reset branches
  **by name**; use `git branch -f <name> <ref>` for a branch that is not checked
  out, which does not touch the working tree at all.
- **Prefer branches.** Work on `win/...` or `mac/...` and merge via PR, so you
  are never committing straight onto the branch someone else is standing on.

This is not defensive boilerplate. A `reset --hard` aimed at the wrong branch
destroyed unstaged `adi_daw` work in `engine/graph.hpp` that had never been
staged and so could not be recovered — see ADR-0014 and ADR-0015.

## Building

### Windows (win)

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

# standalone — the primary development target
& $msbuild adi-vst\vital\standalone\builds\vs19\Vial.sln `
    /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m

# VST3 plugin
& $msbuild adi-vst\vital\plugin\builds\vs19\Vial.sln `
    /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m
```

The `PlatformToolset` override is required, not optional (ADR-0004).

### macOS (mac)

Xcode projects exist at `vital/standalone/builds/osx` and
`vital/plugin/builds/osx`, regenerated by Projucer from the `.jucer` files. They
have **never been built in this project** — assume nothing works until you have
run it. Expect the same class of latent breakage the Windows exporters had
(ADR-0005, ADR-0006, ADR-0007): dead defines, paths to SDKs that are not there.

When you fix an exporter, fix it in the **`.jucer`** and re-run
`Projucer --resave`, never in the generated Xcode project (ADR-0002).

### Editing `.jucer` files

Both agents edit the same two `.jucer` files, one per platform exporter block.
A resave rewrites *all* exporters, so a macOS fix will touch the Windows
`.vcxproj` files too and vice versa. This is the single most likely source of
conflict between us. Mitigation: keep `.jucer` edits in their own small,
short-lived PR, merge it promptly, and say so in your log.

## What "done" means here

A branch is ready to merge when:

- it builds on your platform,
- `VitalValidator` passes against the built VST3 (win) — 15/15 is the baseline,
- `python adi-vst/tools/extract_schema.py` still reports 794 parameters,
  0 unresolved, if you touched anything under `vital/src/common/`,
- any decision you made is a new ADR in `docs/DECISIONS.md`,
- you have appended an entry to your own log file,
- your claims row is removed.

## Ground rules on the work itself

- **The docs are the source of truth, not the code.** `ARCHITECTURE.md` and
  `docs/DECISIONS.md` describe what is true. If the code disagrees, one of them
  is a bug — decide which, fix that one, and say which in your log.
- **Decisions go in `docs/DECISIONS.md` as a new numbered ADR.** Append only.
  Superseding means a new entry that says so, never editing the old one.
- **`vital/` is GPLv3 and carries naming restrictions.** Upstream forbids using
  "Vital", "Vital Audio" or "Tytel" to name a distribution built from this
  source, and forbids connecting to vital.audio. The build is already named
  "Vial" upstream — that is Tytel's own trademark-stripped name, not a typo.
- **Keep the fork delta small and legible.** `git -C adi-vst/vital diff
  upstream/main --stat` should stay something a human can read. Prefer adding
  files over editing Vital's, and prefer one small edit at a seam over a
  refactor.
- **Check claims rather than asserting them.** Two numbers in `ARCHITECTURE.md`
  exist because they were measured, not estimated: 794 parameters (the extractor
  resolves them from the C++ source) and 232,438 bytes of preset state (the
  validator reports it). If you write a number into a doc, make something verify
  it.
