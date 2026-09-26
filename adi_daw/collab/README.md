# Working in parallel — three agents, one repo

Four agents work on `adi_daw`, on different machines and different accounts,
synced only through this git repository. This file is the protocol. Read it
before your first commit, and re-read it if you have been away.

## Roster

| Agent | Machine | Model | Owns | Cannot |
|---|---|---|---|---|
| **win** | Windows 11 desktop, MSVC 19.44 / x64 | Claude Code | Lead technical coordinator (ADR-0109): architecture, ADR sequencing, engine integration, the format spec, `src/adi/**`, `tests/**`, `tools/**`, docs, Windows-specific code | Build or test on macOS/clang/arm64 |
| **mac** | macOS, Apple clang / arm64 | Claude Code (team licence) | `.github/workflows/**`, `docs/UI-ARCHITECTURE.md`, macOS platform and CoreAudio, review | Nothing structural — see below |
| **linux** | Ubuntu workstation, GCC/Clang / x86-64 | ChatGPT Codex (terminal) | Portable standard C++, headless CI and test enforcement, sanitizers, POSIX portability | OS-specific GUI or driver code; any Linux-only library; `docs/UI-ARCHITECTURE.md`; schema, ADR numbers or claims without `win` |
| **cloud** | Claude Code on the web, Linux (an ephemeral cloud container) | Claude Code | Portable headless C++, the format and its docs, on win's assignments | JUCE, platform code, `.github/**`, `drivers/**`, other monorepo projects |

**Availability (director, 2026-09-26):** mac is back in the loop, a day
early, on `collab/prompts/2026-09-26-mac-return-host-half.md`. linux (Codex)
returns after 2026-10-01. cloud receives no new missions; its last, ADR-0159,
is merged.

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

**3. Log what you did, in your own file.** `collab/win.md`, `collab/mac.md`,
`collab/linux.md` and `collab/cloud.md`. Only ever write to your own. This is deliberate: a shared log is a guaranteed
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
| 0138 | win | `win/lift-agpl-ban` | the AGPL ban is lifted: AGPL code may be copied, and the copying project becomes AGPLv3 | used |
| 0139 | cloud | `cloud/remarks-projection` | remarks in the text projection: a `remark` child of its anchor, `by agent` always shown | used |
| 0140 | cloud | `cloud/snapshots` | history snapshots live in `history_snapshots`: `snapshots` is already the mixer-snapshot table | used |
| 0141 | win | `win/vst3-gesture` | a knob moved inside a real VST3's window is one op; the mute and the echo guard each have their case | used |
| 0142 | win | `win/vst3-state` | plugin state round-trips through the project; a preset picked in the plugin's window is one `device.loadState`; rows go on top of a chunk | used |
| 0143 | linux | `linux/collect-export` | Collect and Export and the media ops (ADR-0127 d3-d4, ADR-0136 d4), if it decides anything | used |
| 0144 | cloud | `cloud/migrate` | how an older 1.x file is upgraded when opened for writing; read-only opens read missing tables as empty | used |
| 0145 | win | `win/final-rulings` | the director's final rulings: Live's plugin blocks, 64 samples the smallest buffer, JUCE-only ASIO, a Pd editor on plugdata, hash-matched library sync, the repo move later | used |
| 0146 | win | `win/schema-1.4` | schema 1.4: the expression route a device plays with, and the agent's request beside its transaction | used |
| 0147 | linux | `linux/library-index` | publishing files without hard links; the library index of ADR-0145 d11; BLAKE3's SIMD code for it, if it decides anything | used |
| 0148 | cloud | `cloud/changeset` | the Propose-tier changeset: preview in a rolled-back transaction, the request row by a connection-local trigger | used |
| 0149 | win | `win/route-registry` | where application data lives, shared by the suite or per application; the capabilities registry proposes a route once, the project decides; the route op | used |
| 0150 | win | `win/rulings-0150` | the plug-in panel exactly as Live's; the Parameter List; BLAKE3 SIMD on every CPU | used |
| 0151 | linux | `linux/clip-playback` | audio clips playing through the session, streamed from disk | used |
| 0152 | cloud | `cloud/settings` | the settings store: typed registry, one file per application, roles in bundles, a two-lock agent whitelist | used |
| 0153 | win | `win/journal-simd` | the journal takes a caller inside its transaction; BLAKE3 runs AVX2, AVX-512 and NEON | used |
| 0154 | win | `win/device-panel` | the plug-in panel is project state: schema 1.5, `device.setPanel`, Live's rule and the Parameter List as functions | used |
| 0155 | linux | `linux/midi-clips` | MIDI clip schedules, note ownership and time-based audio pages | used |
| 0156 | cloud | `cloud/decoder` | one decoder for imported audio, into the cache; the settings registry filled from the catalogue, if it decides anything | used |
| 0157 | win | `win/play-clips` | sample rates from 44.1 kHz to 768 kHz; lower-rate files still play, converted up (director's ruling) | used |
| 0158 | win | `win/held-notes` | a held note keeps its instrument running (amends ADR-0043) | used |
| 0159 | cloud | `cloud/curves` | curve formulas for automation and note expression; automation read into the engine, if it decides anything | used |
| 0160 | win | `win/fl-backlog` | from FL Studio: Make Unique with linked clips at P2; a ghost-note focus switch for layered editing (director's backlog) | used |
| 0161 | win | `win/op-clocks` | every op carries its client and a Lamport clock (schema 1.6); multiplayer sync in the backlog (director) | used |
| 0162 | win | `win/op-clocks` | automation override is Live's (director's ruling) | used |
| 0163 | win | `win/mixer-strip` | the mixer strip in the graph: volume, pan, mute, solo; the pan law | used |
| 0164 | win | `win/automation-play` | automation plays on the strip; Live's override (ADR-0162) applied | used |
| 0165 | win | `win/param-automation` | plug-in parameter automation, the engine side; the device-host half is mac's | used |
| 0166 | win | `win/plugins` | open-source plug-ins as CLAP: five upstreams built unchanged, the kept Airwindows with auto gain, ADI RMSC (director) | used |
| 0167 | win | `win/scope-agent-parity` | the scope built into the DAW: any two tracks compared, aligned and on the grid (director) | used |
| 0168 | win | `win/scope-agent-parity` | the agent's runtime: depend on OpenClaw's loop, do not fork it (director: approved) | used |
| 0169 | win | `win/scope-agent-parity` | Live parity: fourteen MIDI effects; Sampler, OneShot, Redux, Shifter, Utility (director) | used |
| 0170 | win | `win/scope-agent-parity` | Airwindows re-curated by the director's five rules | used |
| 0171 | win | `win/scope-agent-parity` | Airwindows as eleven suite plug-ins: static parameters, a 5 ms crossfade, auto gain (director) | used |
| 0172 | win | `win/scope-agent-parity` | mixer.setDelay: a track delay, either sign, played as latency (director) | used |
| 0173 | win | `win/scope-agent-parity` | the consoles leave the plug-ins: eleven suites with Color; native group summing (director) | used |
| 0174 | win | `win/group-summing` | native group summing built: eight console flavours, measured unity, schema 1.7 | used |
| 0175 | win | `win/scope-taps` | the scope's engine side: taps stamped when heard, true peak, the compare | used |
| 0176 | win | `win/audio-alignment` | Audio Alignment in the backlog: warp markers from a time-warping path; the hitpoint detector; a transient shaper on the envelope follower (director) | used |
| 0179 | mac | (mac's return mission) | held for mac: the CLAP host contract, automation on the host, the generic panel | reserved |
| 0180 | mac | (mac's return mission) | held for mac | reserved |
| 0181 | win | `win/surfaces-collab` | external control surfaces: relative input at the edge, the loopback control API's surface client, one parameter feed; the Stream Deck + XL first (director) | used |
| 0182 | win | `win/surfaces-collab` | collaboration, hosting and backups: local by default, an op stream to the user's bucket, drives for backups, author-chosen media, previewed application (director) | used |

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
| `src/adi/engine/**`, `tests/test_engine.cpp`, `tests/test_session.cpp`, `tests/test_param_ops.cpp`, `tests/test_graph.cpp`, `tests/test_midi_clips.cpp`, `tests/test_clip_playback.cpp` | win | (standing; linux takes the MIDI and clip files back on its return) | 2026-09-24 |
| `docs/DECISIONS.md`, `docs/format/**`, `docs/FEATURES.md` | win | (standing; anyone appends their own reserved ADR to `DECISIONS.md`) | 2026-09-19 |
| **`docs/UI-ARCHITECTURE.md`** | **mac** | `mac/ui` | 2026-09-19 |
| `third_party/JUCE`, `docs/EXTERNAL-CODE.md`, `src/juce/**`, `cmake/**` | mac | (standing) | 2026-09-19 |
| `.github/**`, `tools/fetch_external.sh`, `tests/fuzz_blob.cpp`, `tests/fuzz_seeds.py` | mac | (standing) | 2026-09-18 |
| `.github/workflows/driver-build.yml` (one file inside mac's area, on the director's instruction, ADR-0120), `adi_daw/drivers/**` | win | `agent/win-dev` | 2026-09-22 |

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
cat adi_daw/collab/cloud.md
```

## Building

The dependencies are gitignored clones, not submodules. Fetch them first:

```bash
bash adi_daw/tools/fetch_external.sh --build-only
```

`--build-only` fetches only what the CMake tree compiles or links, and a
merge that adds a dependency means running it again: configure stops at
"Missing ..." until you do. Plain `fetch_external.sh` fetches every repository — about 630MB, five of which
are `reference/` source we read for design and never compile. Everything in
`third_party/` is pinned by tag and verified against a recorded commit
(ADR-0024); the fetch fails rather than proceeding if upstream has moved a tag.

Then configure and build. On macOS/Linux:

```bash
cmake -S adi_daw -B adi_daw/build -DCMAKE_BUILD_TYPE=Debug
cmake --build adi_daw/build
bash adi_daw/tools/test_all.sh
```

On Windows, use the scripts:

```bat
adi_daw\tools\build.bat              :: configure + build the core, no JUCE
adi_daw\tools\build.bat werror       :: with -Werror on our own code (MSVC /WX)
adi_daw\tools\build.bat clean        :: wipe the build tree first
adi_daw\tools\build-juce.bat [target] :: the JUCE tree: adi_play, adi_vst3_probe, the fixture VST3
```

It is a `.bat` rather than a shell script for a reason worth knowing before
someone "fixes" it: the MSVC compiler is not on `PATH`, and `vcvars64.bat` sets
up the environment in the *current* shell. Calling it from bash sets variables
in a subshell that exits immediately, so `cl.exe` is still missing afterwards.
CMake and Ninja also live inside the Visual Studio install rather than on
`PATH`.

### Windows, and MSVC with warnings as errors

No CI leg builds MSVC with `/WX`, so win builds it after every merge, and code
that passes GCC and Clang `-Werror` still fails it. Each of these has failed it:

- **`std::getenv`** (C4996). Use the one-site helper: `envVar` in
  `tests/test_wav_file.cpp`, `env` in `src/adi/appdata.cpp`. A value that is a
  path is read wide on Windows (`_wgetenv`), or a Hebrew profile folder arrives
  as question marks.
- **`std::setbuf`** (C4996): `std::setvbuf(stdout, nullptr, _IONBF, 0)`.
- **`fopen`, `strcpy`, the `sprintf` family** (C4996): streams, `std::string`,
  `snprintf`.
- **`std::filesystem::u8path`**, deprecated in C++20 (C4996). Hand SQLite a
  path's `u8string()` bytes; `path::string()` is the ANSI code page on Windows.
- **Narrowing** `size_t` to `int` (C4267) and signed/unsigned comparisons.
- **Plants must compile.** MSVC `/WX` refuses the three easy ways to plant a
  defect: a constant condition (`false &&`, C4127), code left unreachable
  (C4702), and a parameter left unused (C4100). A plant that does not build
  proves nothing; weaken a comparison instead.
- **A check that runs on one platform only** changes the total, and
  `tools/test_all.sh` compares the total with the README. Give every check to
  every platform.
- **Under Claude Code** `NoDefaultCurrentDirectoryInExePath` is set. A `.bat`
  that calls a program by bare name, `vcvars64.bat` included, unsets it first
  (`tools/build-juce.bat` and `tools/bench_blake3.bat` do).
- **Backslashes through Git Bash.** A heredoc, `sed` or `python -c` turns
  `\t`, `\b` and `\n` into control characters. Write the script to a file.
  This file's own Windows paths were corrupted that way until 2026-09-24.

### Process rules that have bitten

- A CI run counts only if its head SHA is the PR's head SHA.
- PR CI skips silently while GitHub's `mergeable` is UNKNOWN: use
  `workflow_dispatch`.
- When main moves under you, rebase, keep main's ADRs before yours at the end
  of `DECISIONS.md`, and recount the README's numbers with `tools/test_all.sh`
  and `tools/validate_schema.py`.

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
- **`reference/` is read-only.** Its code may be copied with attribution under
  `OPEN_SOURCE_POLICY.md` (AGPL included since ADR-0138: the first AGPL copy
  makes `adi_daw` AGPLv3). See `docs/EXTERNAL-CODE.md` before copying anything.
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
