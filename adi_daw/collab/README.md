# Working in parallel — two Claude agents, one repo

Two Claude Code agents work on `adi_daw`, on different machines and different
accounts, synced only through this git repository. This file is the protocol.
Read it before your first commit, and re-read it if you have been away.

## Roster

| Agent | Machine | Model | Owns | Cannot |
|---|---|---|---|---|
| **win** | Windows 11 desktop, MSVC 19.44 / x64 | Claude Opus 5 | The format spec, `src/adi/**`, `tests/**`, `tools/**`, docs | Build or test on macOS/clang/arm64 |
| **mac** | macOS, Apple clang / arm64 | Claude (team licence) | `.github/workflows/**`, macOS portability, review | Nothing structural — see below |

Neither agent is senior to the other. Disagree in a PR, not by reverting.

## The three rules

**1. Never commit to `main`.** Branch, push, open a PR, merge it yourself when
CI is green. Branch names are prefixed with your agent name:

```
win/step4-store-layer
mac/ci-macos-build
```

**2. Claim before you write.** Add a row to the claims table below in your first
commit on a branch, and remove it when the branch merges. If a path you need is
claimed by the other agent, say so in your log file rather than editing it —
two agents editing one file through git is how an afternoon disappears.

**3. Log what you did, in your own file.** `collab/win.md` and `collab/mac.md`.
Only ever write to your own. This is deliberate: a shared log is a guaranteed
merge conflict on every single push, and the whole point of splitting the files
is that neither agent ever has to resolve one.

## Current claims

Keep this short. One row per active branch. Delete your row when it merges.

| Path | Agent | Branch | Since |
|---|---|---|---|
| `src/adi/store_rows.*`, `src/adi/textproj_store.*`, `tests/test_textproj_store.cpp` | win | `win/adapter` | 2026-09-19 |
| `third_party/JUCE`, `docs/EXTERNAL-CODE.md`, `cmake/**` | mac | `mac/juce` | 2026-09-19 |
| `.github/**`, `tools/fetch_external.sh`, `tests/fuzz_blob.cpp`, `tests/fuzz_seeds.py` | mac | (standing) | 2026-09-18 |

`src/adi/textproj.*` stays mac's even while win writes the adapter against it:
the adapter builds a `Tree` and never reaches into the pure layer. `src/adi/check.*`
is released — win held it for `adi_tool check` and that branch merged.

`src/adi/blob.*`, `tests/test_main.cpp` and `CMakeLists.txt` are **shared** —
either agent may touch them, in small changes, saying so in their log. They are
the files both sides keep needing, and locking them to one agent would block the
other for no gain. `CMakeLists.txt` in particular: adding your own target is
expected, and the merge is one line — as it was here, both sides adding a target
and a source, resolved by keeping both.

## Before you start work, every time

```bash
git -C <repo> checkout main && git pull
cat adi_daw/collab/README.md          # claims may have changed
cat adi_daw/collab/<other-agent>.md   # what they did since you last looked
```

## Building

The dependencies are gitignored clones, not submodules. Fetch them first:

```bash
bash adi_daw/tools/fetch_external.sh --build-only
```

`--build-only` fetches the two entries the CMake tree actually links. Plain
`fetch_external.sh` fetches all nine repositories — about 630MB, five of which
are `reference/` source we read for design and never compile. Everything in
`third_party/` is pinned by tag and verified against a recorded commit
(ADR-0024); the fetch fails rather than proceeding if upstream has moved a tag.

Then configure and build. On macOS/Linux:

```bash
cmake -S adi_daw -B adi_daw/build -DCMAKE_BUILD_TYPE=Debug
cmake --build adi_daw/build
bash adi_daw/tools/test_all.sh
```

On Windows, use the script:

```bat
adi_daw	oolsuild.bat            :: configure + build
adi_daw	oolsuild.bat werror     :: with -Werror on our own code
adi_daw	oolsuild.bat clean      :: wipe the build tree first
```

It is a `.bat` rather than a shell script for a reason worth knowing before
someone "fixes" it: the MSVC compiler is not on `PATH`, and `vcvars64.bat` sets
up the environment in the *current* shell. Calling it from bash sets variables
in a subshell that exits immediately, so `cl.exe` is still missing afterwards.
CMake and Ninja also live inside the Visual Studio install rather than on
`PATH`.

## What "done" means here

```bash
bash adi_daw/tools/test_all.sh
```

Runs every test binary, both schema validators, and the spec-vs-binary layout
check, and exits non-zero if any of them fails. The binaries are **discovered**,
not listed, so a new suite is covered the moment it builds. It replaced a
five-item checklist that had grown to nine binaries plus three scripts — a list
that long is a list someone runs four of.

A branch is ready to merge when:

- `tools/test_all.sh` passes on your platform,
- CI is green (17 jobs, seven ABIs),
- you have appended an entry to your own log file,
- your claims row is removed.

## Ground rules on the work itself

- **The docs are the source of truth, not the code.** `docs/format/SPEC.md`,
  `docs/OPS.md` and `docs/DECISIONS.md` describe what is true. If the code
  disagrees with them, one of the two is a bug — decide which, fix that one, and
  say which in your log. Do not let them drift silently.
- **Decisions go in `docs/DECISIONS.md` as a new numbered ADR.** Append only.
  Superseding an old decision means writing a new entry that says so, never
  editing the old one.
- **`reference/` is read-only, and `reference/zrythm` is AGPL** — read it for
  design, never copy code from it. See `docs/EXTERNAL-CODE.md` before copying
  anything from any of those repos.
- **Check claims rather than asserting them.** Both validators exist because a
  confident sentence in a spec turned out to be false — see the `152 ops` and
  `960 PPQ quintuplet` entries in the logs. If you write a number or a
  divisibility claim into a doc, add it to a validator.
- **Prove the check can fail.** Distinct from the rule above, and the one this
  project leans on hardest. A test or guard that has never been watched reject
  something is a comment: plant the defect it exists to catch, watch it fire,
  put it back. It has paid every time — `>` versus `>=` in the snapshot
  publisher **segfaults** when weakened; removing the op-log transaction reports
  "saw 2 tracks"; an op made to read ambient state fails the replay corpus while
  every unit suite stays green.
- **Low severity is a prediction about people, and it can be wrong.** A span
  lifetime hazard was reported, ranked low because "nobody would write that",
  and written by the other agent within the week — producing a checker that
  confidently reported corruption that did not exist. Where a hazard closes at
  the type level for one line, close it (ADR-0034).

## Working with the other agent

- **The repository is PUBLIC**, and it is a monorepo containing unrelated
  projects and whatever work-in-progress is sitting in the tree. **Stage
  explicit paths — `git add adi_daw/` — never `git add -A` from the root.** A
  `-A` once swept ten untracked files from another project into a pushed branch;
  on a public repo a force-push does not fully undo that.
- **Pull `main` before writing an ADR.** Numbers are assigned by looking at the
  file, so an ADR written on a stale branch collides. It has happened once
  (two ADR-0031s) and the fix is a renumber and a merge conflict.
- **Before reporting that the other agent has not fixed something, check the
  merge-base.** A finding can be accurate when written and stale by the time it
  is read. `git merge-base main <their-branch>` says which main they were
  looking at. One standing finding turned out to have been fixed two PRs before
  it was reported.
- **Taking something from the other agent's claimed paths is allowed when it is
  blocking them** — say so plainly in your log rather than quietly. It has
  happened twice, both times to unblock a merge, both times recorded.
