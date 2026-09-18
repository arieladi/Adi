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
| `src/**`, `tests/**`, `CMakeLists.txt` | win | `win/step4-store-layer` | 2026-09-18 |
| `.github/**`, `tools/fetch_external.sh`, `tests/fuzz_blob.cpp` | mac | `mac/pin-deps-and-fuzz` | 2026-09-18 |

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
./adi_daw/build/adi_tests
```

On Windows the compiler is not on `PATH`; `vcvars64.bat` has to run first, and
CMake and Ninja come from inside the Visual Studio install. See `collab/win.md`
for the exact invocation that works.

## What "done" means here

A branch is ready to merge when:

- `adi_tests` passes on your platform,
- `python adi_daw/tools/validate_schema.py` passes,
- `python adi_daw/tools/validate_ops.py` passes,
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
