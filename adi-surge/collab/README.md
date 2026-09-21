# Working in parallel — two Claude agents, one project

Two Claude Code agents work on `adi-surge`, on different machines and different
accounts, synced only through git. This file is the protocol. Read it before
your first commit, and re-read it if you have been away.

This mirrors `adi-vst/collab/README.md`, which in turn mirrors
`adi_daw/collab/README.md` — same rules, same habits, so no agent has to hold
three protocols in their head. The differences are in the roster, the build
system and the repo topology, and each one is called out where it occurs.

## What this project is

`adi-surge` is the **second** AI synth. `adi-vst` is a fork of Vital and ships
a **VST3**; this is a fork of **Surge XT** and ships a **CLAP**. Same AI idea —
describe a sound in words, the synth changes — deliberately built twice, on two
codebases, in two plugin formats.

That is not duplicated effort for its own sake. The two synths differ in exactly
the ways that matter for the AI work:

| | `adi-vst` (Vital) | `adi-surge` (Surge XT) |
|---|---|---|
| Plugin format | VST3 (CLAP dropped, ADR-0017 there) | **CLAP**, first-class, plus VST3/AU/standalone |
| Build | Projucer + patched JUCE 6.0.5 | **CMake + JUCE 8.0.12** |
| Preset | nlohmann JSON, partial load legal | binary/XML chunk, streaming-versioned |
| Param addressing | `std::map<std::string, Value*>` | `Parameter::oscName` + a reverse map |
| Machine surface | none — we built a validator and an extractor | **OSC in/out, `surgepy`, a CLI, a test runner** |
| Undo | none upstream; ours to build | **`Surge::GUI::UndoManager` upstream** |

The last two rows are why this project exists as more than a second coat of
paint: Surge ships most of the scaffolding `adi-vst` had to hand-build.

**Intended integration with `adi_daw` is real but not yet designed.** `adi_daw`
has mandated CLAP *hosting* (its ADR-0052), specifically for CLAP's
non-destructive parameter modulation. Hosting a format and exporting one are
unrelated capabilities and neither decision constrains the other. Do not write
anything into this repo that assumes the two are coupled until there is an ADR
that says how.

## Roster

| Agent | Machine | Model | Owns | Cannot |
|---|---|---|---|---|
| **win** | Windows 11 desktop, MSVC 19.44 (VS 2022 Community) / x64 | Claude Opus 5 | Windows builds of every target, `surge/**` C++ on the Windows side, `tools/**` | Build or test on macOS / clang / arm64 |
| **mac** | macOS, Apple clang / arm64 | Claude Opus 5 | macOS builds, `.github/workflows/**`, the OSC + `surgepy` harness, review | Build or test the Windows CLAP |

Neither agent is senior to the other. Disagree in a PR, not by reverting.

**The split here is weaker than in `adi-vst`, and that is a real difference.**
Over there the split was forced: Projucer's exporters are per-platform and the
Windows VST3 genuinely could not be built on a Mac. Surge is one CMake tree that
both agents can configure and build. So the split is about **who verifies what
on which hardware**, not about who is allowed to touch which file. Claim paths
in the table below and the ownership column stops mattering.

## The three rules

**1. Never commit to `main`.** Branch, push, open a PR, merge it yourself when
CI is green. Branch names are prefixed with your agent name:

```
win/clap-build-baseline
mac/osc-schema-extractor
```

In `surge/`, `main` has a second job: it mirrors `upstream/main` so that
`git diff upstream/main` stays meaningful. Committing to it breaks the one
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
| `collab/**`, `docs/**`, `ARCHITECTURE.md`, `tools/fetch_surge.sh` | mac | `mac/adi-surge-bootstrap` | 2026-09-20 |

## Reserved ADR numbers

**Claim the number BEFORE writing the entry. Push immediately.** Mark the row
`used` once the entry exists, and leave it there.

This project has its **own ADR namespace starting at ADR-0001**. It does not
continue `adi-vst`'s or `adi_daw`'s numbering. When you need to cite one of
theirs, write it in full — "adi-vst ADR-0017", "adi_daw ADR-0052" — because a
bare "ADR-0017" in this file will be read as ours.

The mechanism is carried over from `adi_daw` ADR-0051 and the argument transfers
unchanged: "pull main before writing an ADR" cannot work, because the collision
does not happen at the pull. It happens in the window between pulling and
merging, which is however long the work takes. Pulling earlier closes nothing.

**A row is never deleted, only marked.** An abandoned reservation is `burned` and
the number is never reused — a gap costs nothing.

**The Subject column is load-bearing.** A number reservation does not stop both
agents writing the same ADR; reading the other agent's subject does.

| Number(s) | Agent | Branch | Subject | Status |
|---|---|---|---|---|
| 0001 | mac | `mac/adi-surge-bootstrap` | why Surge XT, and why a second synth at all | used |
| 0002 | mac | `mac/adi-surge-bootstrap` | `surge/` is a fork tracked against upstream, not vendored | used |
| 0003 | mac | `mac/adi-surge-bootstrap` | CLAP is the primary target here | used |
| 0004 | mac | `mac/adi-surge-bootstrap` | submodules are gitlinks; the `protocol.file.allow` trap | used |
| 0005 | mac | `mac/adi-surge-adr0005` | the AI write path: surgepy first, C++ overlay second, OSC is scaffolding | used |
| 0006 | mac | `mac/adi-surge-bootstrap` | the fork has no `origin` yet, and why that does not block win | used |
| 0007 | mac | `mac/adi-surge-bootstrap` | CMake >= 3.22 is hard; a build must assert the artifact | used |

## Before you start work, every time

```bash
git -C <Adi repo> checkout main && git pull
cat adi-surge/collab/README.md      # claims may have changed
cat adi-surge/collab/<other>.md     # what they did since you last looked
cat adi-surge/docs/DECISIONS.md     # settled questions; do not re-litigate
```

`ARCHITECTURE.md` is the how-it-works reference. `docs/DECISIONS.md` is the
append-only record of what was decided and why. Read the ADR log before
proposing a change to something that looks odd — most of the odd things are
load-bearing and the reason is written down.

## Repo topology — read this before cloning

`adi-surge/` is a project directory in the `arieladi/Adi` monorepo, exactly like
`adi_daw/` and `adi-vst/`. One repo, one clone, public.

```
arieladi/Adi
├── adi_daw/            the DAW — a DIFFERENT project
├── adi-vst/            the Vital fork — a DIFFERENT project
└── adi-surge/          this project
    ├── ARCHITECTURE.md
    ├── docs/DECISIONS.md
    ├── collab/
    ├── tools/
    │   └── fetch_surge.sh
    └── surge/          ← SEPARATE REPO, gitignored by the monorepo
```

`surge/` is a clone of `surge-synthesizer/surge` whose remote is named
`upstream`, kept out of the monorepo because vendoring it would lose
`git diff upstream/main`, the only thing that tells us what we have actually
forked. Working tree is ~1.5 GB with all submodules checked out.

Do not "fix" this by `git add`-ing `surge/` into the monorepo. The ignore rule is
deliberate and stops it becoming a stray gitlink.

**Getting it:**

```bash
bash adi-surge/tools/fetch_surge.sh
```

That clones the fork if missing and initialises all 22 submodules. Read the
header of that script before running anything by hand — it absorbs two traps
that will otherwise cost you an afternoon (see `ARCHITECTURE.md` §2.1).

### Staying out of the other projects' way

`adi_daw` and `adi-vst` are under active development in this same repo and
working tree, by different sessions. Separation here is a **working practice**,
not a structure, so it has to be kept deliberately:

- **Touch only `adi-surge/**`.** Never edit, stage or revert anything under
  `adi_daw/` or `adi-vst/`, even to "fix" something obvious. Raise it instead.
- **Stage explicit paths.** Never `git add -A` or `git add .` — the monorepo
  routinely holds other people's untracked work in progress, and it is public,
  so a force-push does not fully undo a leak.
- **Check before anything destructive.** Run `git status` and
  `git branch --show-current` as their **own step** and read the output before
  any `reset --hard`, `clean`, force-push or history rewrite. Reset branches
  **by name**; use `git branch -f <name> <ref>` for a branch that is not checked
  out, which does not touch the working tree at all.
- **Prefer branches.** Work on `win/...` or `mac/...` and merge via PR, so you
  are never committing straight onto the branch someone else is standing on.

This is not defensive boilerplate. A `reset --hard` aimed at the wrong branch
destroyed unstaged `adi_daw` work in `engine/graph.hpp` that had never been
staged and so could not be recovered — see adi-vst ADR-0014 and ADR-0015.

## Building

Full detail, including the traps, is in `ARCHITECTURE.md` §2. The short form:

**CMake ≥ 3.22 is a hard requirement (ADR-0007).** Below 3.21 the build
*succeeds* and silently produces no CLAP. Check your version first, and check
that `surge-xt_CLAP` exists afterwards — the exit code will not tell you.

### Windows (win)

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

cmake --version   :: must be >= 3.22 -- see ADR-0007

cmake -S adi-surge\surge -B adi-surge\surge\build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build adi-surge\surge\build --config Release --target surge-xt_CLAP --parallel
```

### macOS (mac)

```bash
cmake -S adi-surge/surge -B adi-surge/surge/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64"
cmake --build adi-surge/surge/build --target surge-xt_CLAP --parallel
```

Both command shapes are upstream's own, taken from
`surge/.github/workflows/build-pr.yml`. CMake and Ninja ship **inside** the
Visual Studio install on Windows and are not on `PATH`; see `ARCHITECTURE.md`
§2.3 for the full paths.

Useful targets: `surge-xt_CLAP`, `surge-xt_Standalone`, `surge-xt-distribution`,
`surge-common`, `surge-testrunner`, `surgepy`, `surge-xt-cli`.

## What "done" means here

A branch is ready to merge when:

- `surge-xt_CLAP` builds on your platform,
- `ctest -j 4` passes in the build tree — this is upstream's own suite and it is
  the regression gate; record the count in your log the first time you run it,
- any decision you made is a new ADR in `docs/DECISIONS.md`,
- you have appended an entry to your own log file,
- your claims row is removed.

## Ground rules on the work itself

- **The docs are the source of truth, not the code.** `ARCHITECTURE.md` and
  `docs/DECISIONS.md` describe what is true. If the code disagrees, one of them
  is a bug — decide which, fix that one, and say which in your log.
- **Decisions go in `docs/DECISIONS.md` as a new numbered ADR.** Append only.
  Superseding means a new entry that says so, never editing the old one.
- **`surge/` is GPLv3.** A distributed fork must be GPLv3 too. Private use is
  unrestricted. Check the naming and trademark position before any distribution
  — Surge's own README carries redistribution restrictions on VST2 builds, and
  the "Surge" name is the Surge Synth Team's.
- **Keep the fork delta small and legible.** `git -C adi-surge/surge diff
  upstream/main --stat` should stay something a human can read. Prefer adding
  files over editing Surge's, and prefer one small edit at a seam over a
  refactor. This matters more here than in `adi-vst`: Surge is actively
  developed and we will want to pull upstream regularly.
- **Prefer the surface upstream already gives you.** Surge ships OSC in/out,
  Python bindings, a headless CLI and a test runner. `adi-vst` had to build a
  validator and a schema extractor from scratch because Vital ships none of
  that. Before writing C++ here, check whether the thing you want already
  exists — and if you do write it, say in your log what upstream surface you
  rejected and why.
- **Check claims rather than asserting them.** Every number in
  `ARCHITECTURE.md` was produced by a command, and the command is written next
  to it. If you write a number into a doc, make something verify it.
