# Working in parallel — three agents, one repo

Three agents work on `adi_daw`, on different machines and different accounts,
synced only through this git repository. This file is the protocol. Read it
before your first commit, and re-read it if you have been away.

## Roster

| Agent | Machine | Model | Owns | Cannot |
|---|---|---|---|---|
| **win** | Windows 11 desktop, MSVC 19.44 / x64 | Claude Code | Lead technical coordinator (ADR-0109): architecture, ADR sequencing, engine integration, the format spec, `src/adi/**`, `tests/**`, `tools/**`, docs, Windows-specific code | Build or test on macOS/clang/arm64 |
| **mac** | macOS, Apple clang / arm64 | Claude Code (team licence) | `.github/workflows/**`, `docs/UI-ARCHITECTURE.md`, macOS platform and CoreAudio, review | Nothing structural — see below |
| **linux** | Ubuntu workstation, GCC/Clang / x86-64 | ChatGPT Codex (terminal) | Portable standard C++, headless CI and test enforcement, sanitizers, POSIX portability | OS-specific GUI or driver code; any Linux-only library; `docs/UI-ARCHITECTURE.md`; schema, ADR numbers or claims without `win` |

## Governance (ADR-0109)

- **The director (Adi) is the authority.** A direct instruction from Adi to any
  agent overrides the roadmap, any ADR and any assignment, immediately. The log
  is kept true afterwards by a superseding ADR, never by editing history.
- **Without a direct instruction, `win` coordinates.** Schema changes, new ADR
  number allocations and cross-agent claims synchronise through `win`, so that
  two agents never write the same ADR or edit the same file through git.
- Disagree in a PR, not by reverting. Nobody reverts another agent's work.
- **A tool an agent runs is that agent.** Gemini CLI on the Ubuntu box, or any
  other model an agent delegates to, writes nothing to the repository, opens no
  PRs, holds no credentials and has no log. What it finds is a claim the agent
  makes, verified by that agent with a test or a plant before it is written
  down. Audit reports live under the agent's own folder (`collab/linux/audits/`).
- The `linux` agent's onboarding is `collab/linux/ONBOARDING.md`; its log is
  `collab/linux.md`. Linux desktop work is phase 3 and has not started
  (ADR-0109).

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

**3. Log what you did, in your own file.** `collab/win.md`, `collab/mac.md` and
`collab/linux.md`. Only ever write to your own. This is deliberate: a shared log is a guaranteed
merge conflict on every single push, and the whole point of splitting the files
is that neither agent ever has to resolve one.

## Reserved ADR numbers

**Claim the number BEFORE writing the entry. Push immediately.** Mark the row
`used` once the entry exists, and leave it there.

`used` rather than `merged` because the number is spent the moment the entry
is written, not when the PR lands — check 8 caught that distinction on its
first run, against this very row.

mac's proposal, and their argument for it is the part worth keeping: "pull main
before writing an ADR" is advice I gave and it cannot work, because the
collision does not happen at the pull. It happens in the window between pulling
and merging, which is however long the work takes. Pulling earlier closes
nothing.

This does **not** eliminate the conflict; it moves it to before the work. Two
agents reserving at the same moment still collide — on one line of this table,
minutes in, and the loser renumbers before writing a word. Today it lands on a
multi-paragraph append with cross-references in four files to fix.

**A row is never deleted, only marked.** An abandoned reservation is `burned`
and the number is never reused — a gap costs nothing, and "0051 was reserved,
dropped, then reused" is the ambiguity ADR-0028 exists to prevent (ADR-0051).

**The Subject column is load-bearing, not decoration.** The expensive collision
was not a numbering accident: a directive went to both agents and we each wrote
the same four ADRs. A number reservation does not prevent that; reading the
other agent's subject does. Check it before you start, and if it is your
subject, say so in your log instead of writing it twice.

| Number(s) | Agent | Branch | Subject | Status |
|---|---|---|---|---|
| 0050 | mac | `mac/device` | the frame and repaint discipline | used |
| 0051 | win | `win/adr-numbering` | this table | used |
| 0052 | win | `win/clap` | CLAP hosting is mandated, and the route to it | used |
| 0053 | win | `win/clap` | native AudioGridder client: remote plugins as devices | used |
| 0054 | win | `win/mpe` | MPE and MPE+ end to end, and the floor bound they impose | used |
| 0055 | win | `win/graph` | the node contract, and what the scheduler decides per block vs per segment | used |
| 0056 | win | `win/routing` | buses, levelled scheduling for parallelism, and MPE+ event capacity | used |
| 0057 | mac | `agent/mac-dev` | VST3 hosting behind the format-agnostic device model | used |
| 0058-0064 | win | `win/blueprint` | the director's five-pillar blueprint: PDC, freezing, racks, stretch, native DSP, undocking, async AI | used |
| 0065 | win | `win/setparent` | absence of a main routing row means the default, and setParent is composite | used |
| 0066 | win | `win/setparent` | latency changes while running: recompute off-thread, publish, crossfade | used |
| 0067-0071 | win | `win/routing-mandates` | aux sends, multi-project tabs, item FX, region export, the export queue | used |
| 0072 | mac | `agent/mac-dev` | the director's ruling on aux sends: supersedes ADR-0067 | used |
| 0073 | mac | `agent/mac-dev` | the VST3 process call is indivisible: events and parameters ride together | used |
| 0074 | mac | `agent/mac-dev` | the native broadcast node, and sink nodes as a graph property (Phase 2) | used |
| 0075 | mac | `agent/mac-dev` | the CLAP host: built from scratch, and it needs no JUCE | used |
| 0076 | mac | `agent/mac-dev` | the two-tier UI: DAW-rendered panels vs floating third-party GUIs | used |
| 0077 | win | `agent/win-dev` | realising a plan into a live graph: the junction, the chain, and what a VCA is not | used |
| 0078 | win | `agent/win-dev` | `NodeIo` addresses the BLOCK; `frames`/`blockOffset` address the segment | used |
| 0079 | win | `agent/win-dev` | a latency change is a TAP MOVE, not a second render: amends ADR-0066 d1 and d4 | used |
| 0080 | win | `agent/win-dev` | swapping the docked side of a panel: width follows the pane, not the side | used |
| 0081 | mac | `agent/mac-dev` | event frames are block-relative; a device subtracts, and a mismatch is counted | used |
| 0082 | win | `agent/win-dev` | the latency coalescer: it polls, the clock is an argument, and a burst has a ceiling | used |
| 0083 | mac | `agent/mac-dev` | AudioGridder natively, and the server forked to host CLAP | used |
| 0084 | mac | `agent/mac-dev` | CLAP already says why it wants a restart; we were not listening | used |
| 0085 | win | `agent/win-dev` | escalation: a ring that is too small is GROWN per edge, primed against the old one | used |
| 0086 | mac | `agent/mac-dev` | dependency architecture: native C++ DSP core vs RPC AI services | used |
| 0087 | mac | `agent/mac-dev` | ADR-0084 closed: measured CLAP latency reporting on Pro-Q 3 | used |
| 0088 | win | `agent/win-dev` | the compensation headroom default is measured, not zero | used |
| 0089 | win | `agent/win-dev` | the rebuild path: a new graph is published, and the swap is faded not cut | used |
| 0090 | mac | `agent/mac-dev` | closing the rebuild loop: who decides to rebuild, and what a failed rebuild means | used |
| 0091 | win | `agent/win-dev` | events travel along edges: what a node emits reaches what it feeds | used |
| 0092 | win | `agent/win-dev` | a rebuild keeps the history of every edge that exists in both graphs | used |
| 0093 | win | `agent/win-dev` | the DSP plugin roadmap: references fetched not vendored, and what each goal needs first | used |
| 0094 | win | `agent/win-dev` | the open-source mandate, applied: ADR-0093's licence question is answered | used |
| 0095 | win | `agent/win-dev` | the libpd latency protocol: a patch reports its latency through `$0-report_latency` | used |
| 0096 | win | `agent/win-dev` | the DSP corrections: Pd biquad signs, peak detection, RMSC's clamp and sidebands | used |
| 0097 | win | `agent/win-dev` | MPE+ through VST3: per-note expression without breaking plain MIDI | used |
| 0098 | win | `agent/win-dev` | real plugins on Windows: Surge XT through both hosts, and what it found | used |
| 0099 | win | `agent/win-dev` | CLAP note dialects: the host sends what the plugin's note port declares | used |
| 0100 | win | `agent/win-dev` | the expression test rig: a fixture VST3 with a reachable controller, and every dimension measured | used |
| 0083-0084 | mac | `agent/mac-dev` | AudioGridder native integration and the CLAP fork; CLAP restart causes | used |
| 0101-0116 | win | `agent/win-dev` | the director's V0.2 directives: Session View returns last (0101), small blocks (0102), microtonal scales (0103), app-scoped browser (0104), the ADI Suite (0105), loopback and virtual device (0106), PTP (0107), parity gate (0108), portability and three agents (0109), parameter ops (0110), historical tabs (0111), views (0112), 64 buses (0113), macro curves (0114), editing behaviours (0115), Pd second view (0116) | used |
| 0117 | win | `agent/win-dev` | the director's answers: Session View docks Ableton-style, session clips mirror Live, tuning as child tables, the driver signing route | used |
| 0118 | win | `agent/win-dev` | the Windows virtual device: our own sysvad driver signed via SignPath Foundation; bundling VB-CABLE rejected | used |
| 0119 | win | `agent/win-dev` | the driver's endpoint names and home (`drivers/`, MIT), and the SignPath preconditions; the director sends the form | used |
| 0120 | win | `agent/win-dev` | the driver build workflow; sysvad is MS-PL not MIT, fetched never vendored; the licence ruling is the director's | used |
| 0121 | win | `agent/win-dev` | MS-PL ruled acceptable under drivers/; policy row added | used |
| 0122 | win | `win/step6-session` | step 6 opens: the session runtime -- rows place devices, placeholders never gaps, a format change rebuilds and never reloads; what "done" means for step 6 | used |
| 0123 | win | `win/adr-0123-clap-contract` | CLAP host contract corrections from linux's audit (C1, C2, C3, C5 fixed; C4 to mac) and the audio thread never asks a node its latency (C6) | used |
| 0124 | win | `win/adr-0124-param-ops` | the parameter-op glue: one ring per device, normalized on the wire, applied compares before it sets, the first edit writes where it started | used |
| 0125 | win | `win/settings-rulings` | the Settings Reference review: R-01 to R-27 ruled; the agent's settings pipeline, RTL, control room deferred, JUCE 9.0.2 already the pin | used |
| 0126 | win | `win/settings-rulings` | LAN audio is a native node built from SonoBus; Link Audio stays a wish | used |
| 0127 | win | `win/settings-rulings` | audio and video are never embedded in the .adi; Collect and Export writes a ZIP; supersedes SPEC 10.4 | used |
| 0128 | win | `win/settings-rulings` | history is a snapshot tree; revert never discards | used |
| 0129 | win | `win/settings-rulings` | navigation and zoom: Live 12's gestures as the first parity checklist; per-window scaling | used |
| 0130 | win | `win/settings-rulings` | the Master Focus Dial | used |
| 0131 | win | `win/settings-rulings` | Info View, tooltips, anchored remarks; the agent reads remarks as context, never as commands | used |
| 0132 | win | `win/settings-rulings` | recording, import and export formats; import defaults; warp on demand; Rec-Q; retrospective capture | used |
| 0133 | win | `win/settings-rulings` | the suite is ADI DAW, ADI Live and aDiJ; Rec-Q and Play-Q | used |
| 0134 | win | `win/settings-rulings` | architecture rulings from the Settings review: offline tabs, buses on demand, AudioGridder fallback, PTP, block sizes and ASIO, compact plugin blocks, capability registry, Pd parameters, Linux | used |
| 0135 | win | `win/settings-rulings` | the sibling projects' names, and the DSP56300 emulation project | used |
| 0136 | win | `win/schema-1.1` | schema 1.1: embedded media forbidden by triggers, removed only at 2.0 (corrects ADR-0127 d2) | used |
| 0137 | win | `win/asio` | ASIO on Windows from the headers JUCE bundles, under GPL-3.0; the probe guards it | used |

`tools/validate_schema.py` check 8 enforces it: a number that exists in
`DECISIONS.md` while its row still says `reserved` is a row someone forgot, a
`burned` number that reappears is the reuse 0028 forbids, and a `used` row
with no entry behind it is a number claimed and never spent.

## Current claims

Keep this short. One row per active branch. Delete your row when it merges.

| Path | Agent | Branch | Since |
|---|---|---|---|
| `src/juce/**`, `tests/test_device.cpp`, `docs/DEVICE-CONTRACT-PANEL.md` | mac | `mac/vst3` | 2026-09-20 |
| `src/juce/**`, `tests/test_device.cpp` | mac | `mac/device` | 2026-09-20 |
| `src/adi/store_rows.*`, `src/adi/textproj_store.*`, `tests/test_textproj_store.cpp` | win | (standing) | 2026-09-19 |
| `src/adi/engine/**`, `tests/test_engine.cpp` | win | `win/graph` | 2026-09-20 |
| `docs/DECISIONS.md`, `docs/format/**`, `docs/FEATURES.md` | win | (standing) | 2026-09-19 |
| **`docs/UI-ARCHITECTURE.md`** | **mac** | `mac/ui` | 2026-09-19 |
| `third_party/JUCE`, `docs/EXTERNAL-CODE.md`, `src/juce/**`, `cmake/**` | mac | (standing) | 2026-09-19 |
| `.github/**`, `tools/fetch_external.sh`, `tests/fuzz_blob.cpp`, `tests/fuzz_seeds.py` | mac | (standing) | 2026-09-18 |
| `.github/workflows/driver-build.yml` (one file inside mac's area, on the director's instruction, ADR-0120), `adi_daw/drivers/**` | win | `agent/win-dev` | 2026-09-22 |
| `src/adi/engine/param_ops.*`, `tests/test_param_ops.cpp`; additive edits inside mac's area on the director's step-6 instruction (ADR-0124): `src/juce/device_model.*`, `src/juce/clap_host.*`, `src/juce/vst3_host.*` | win | `win/adr-0124-param-ops` | 2026-09-23 |

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
cat adi_daw/collab/win.md             # what the others did since you last looked
cat adi_daw/collab/mac.md
cat adi_daw/collab/linux.md
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

## Working with the other agents

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
