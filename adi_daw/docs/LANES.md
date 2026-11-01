# The lanes: how ADI is built from 25 October 2026 until the director can test a window

**Why this file.** The director spent two weeks trying to wire every account into one orchestrator
and found it only complicated things. The decision (ADR-0207): **no orchestrator**. Three
long-running sessions, each on the account that does that kind of work best, each with a queue
long enough for months, each picking the next item on its own when one is blocked, each stopping
only at a gate that needs the director. `docs/BUILD-BRIEF.md` stays the order of work; this file
says who takes which part and what "always have work" means in practice.

## 1. The three lanes

| Lane | Account and session | Owns | Never |
|---|---|---|---|
| **win** (this one) | Claude, the "ADI Suite Project" session on ORC-VST | coordination: ADRs, FEATURES, the Master Reference, reviews and merges of every PR, the README count, the collab tables, the drone's missions, measurements against Live | engine or UI code of its own beyond small fixes; it reviews |
| **engine** | Claude Max, Opus 5.5, its own session ("ADI engine lane") | the graph, the hosts, the format and export code: `src/adi/engine/**`, `src/adi/*.cpp` store and ops, `src/juce/*host*`, `src/juce/play*`, `docs/format/**`; phase A of the brief, then the P1 engine and host work | ADR numbers (asks win), `docs/FEATURES.md`, the README count, the Master Reference, the UI, the device tier, `plugins/` |
| **window** (ADR-0208) | a Claude work seat of its own ("ADI window lane") | step 7, the first window: `src/adi/ui/**`, `src/juce/ui_*`, `src/juce/app*`, their tests, `docs/STEP-7-PLAN.md`, `docs/UI-ARCHITECTURE.md`; queue §3b | the engine, the device tier, `plugins/`, ADR numbers |
| **devices** (ADR-0208) | a Codex work seat of its own ("ADI devices lane") | Live's devices as ADI's built-ins: `pd/**`, `src/adi/dsp/**`, `src/adi/pd_builtins/**`, `cmake/devices/**`, their tests; `src/juce/pd_*` for the zero-delay adapter only; queue §4b | the graph, the UI, `plugins/`, ADR numbers |
| **clones** | Codex Pro, the director's own programmer and reverse-engineer session | the director's clones as suite members (ADI-Q, ADI-L, **ADI-C**, ADI Still, ADI Prism) built from measurements, in `plugins/`; the FilmCraft contribution; then the reference reading | everything outside `plugins/`; ADR numbers |

The drone (qwen on the 1070 and the A2000) keeps summarising files and manuals; win feeds it.

## 2. Rules that keep the lanes apart (all existing rules, collected)

- One branch per item, prefixed `engine/` or `clones/` (`collab/README.md` rule 1); a worktree per
  item; never the main checkout; never `git add -A`.
- A PR per item, CI green on 7 ABIs, `tools/test_all.sh` green, MSVC `/WX` clean; the parity gate
  for anything the user sees (ADR-0108). **win merges.** A lane never merges its own PR.
- The docs win over the code; a decision is a new ADR, numbers from win only.
- Live's way, additions first-class, the director's deviations stay (ADR-0203 d1, d2, d12); in the
  engine layer, copy whoever does it best (ADR-0205 d10).
- Licensing is closed; measurement clones are welcome; never raise it (ADR-0203 d25, ADR-0204 d3).
- Each lane logs in its own file: `collab/engine.md`, `collab/clones.md` (newest first).
- **When blocked, take the next item.** A lane stops only at a director's gate (§5); everything
  else it decides, records in its log, and win reviews in the PR.
- **Usage limits are not blockers.** A session that hits its limit resumes with "continue" when it
  resets; its queue is in this file and in its log, so nothing is lost. The director's one job
  between sessions is to type "continue" and to answer gates.

## 3. The engine lane's queue (Claude Max, Opus 5.5), in order

Phase A, then phase B/C/D of `BUILD-BRIEF.md`, with the rulings of 25 October applied:

1. **Precision** (ADR-0207 d2): first **64-bit summing at every mix point** with 32-bit processing,
   Live's scheme, with Live's Fact Sheet cancellation case as the test; then the **64-bit end to end
   mode** behind a Processing Precision switch on the Audio page (32 by default, 64 optional): the
   sample type behind `AudioIo`/`NodeIo`, the nodes, the hosts (VST3 `kSample64`, CLAP 64-bit ports),
   the Pd tier (`PD_FLOATSIZE=64`), the native devices in double. Several PRs: the summing, the seam,
   the graph, each host, the Pd tier.
2. **Sends and returns play** (ADR-0203 d3): the planner, compensation across sends, the Live loop
   allowed and every other refused, Pre/Post per return, Enable Send, Solo in Place, the three tap
   points, ADI's sends into any track and a pan on a send; step 6 closes.
3. **A bypassed device keeps its delay** (ADR-0203 d4); the master summed (ADR-0203 d14). (The Pd
   adapter's fixed block is the devices lane's, §4b item 1, since ADR-0208.)
4. **The export queue** (ADR-0071, P1): jobs, wildcards in the registry's brace form, stem jobs,
   NI Stems export and import (ADR-0205 d8), LUFS targeting (ADR-0203 d14), the Export settings; the
   `.adiviz` pass rides on it later (P2).
5. **Freeze and bounce** (ADR-0203 d16): track freeze, Freeze All Channels, Freeze Group, Bounce in
   Place, Bounce to New Track; renders placed Warp off.
6. **History caps and Save As** (ADR-0203 d13, d6): 2000 steps or 1000 MB, the daily snapshot,
   Take Snapshot, Save As, Save a Copy, Collect All and Save.
7. **The channel slots for ADI-Q and ADI-L** (ADR-0204 d1): present-but-off device slots on every
   strip and group, the window that lists them all; the clones lane fills them when the plug-ins
   are handed over.
8. (Step 7, the window, is the window lane's since ADR-0208: §3b.) The engine lane goes on to the
   hosts in 64-bit, the sends, the queue, and the P1 engine work of phase D.
9. Then phase C and D in the brief's order (range selection, fades, tempo detection, automation
   shapes, Capture, Scale Mode, multi-threading, the scanner, stem separation built in, the Loudness
   Drive, Link, the Settings window, the scope).

### 3b. The window lane's queue (a Claude seat), in order

Standing prompt: `collab/prompts/2026-10-27-window-lane.md`; log `collab/window.md`; branches `window/*`.

1. The six open step-7 PRs of 29 September (#196, #198, #199, #203, #204, #205), each rebased onto
   main and brought under the rulings of ADR-0203 d5 to d7, d11, d12 and ADR-0204 d1 (purely as Live;
   the keys; zoom around the selection; the strip minimum; the Status Bar; tooltips on; the AI chat
   tab; the 7.6 mixer with sends and returns, Split Stereo Pan, the master loudness meter, the
   ADI-Q/ADI-L slots), pushed one at a time in the plan's order.
2. Everything of `docs/STEP-7-PLAN.md` 7.2 to 7.6 no PR covers, each with its headless test and
   its parity checklist drafted from the Live 12 manual text. **The first window the director can
   open is his gate (§5) and this lane's first milestone.**
3. The window parts of phase C (range selection, fade handles, Insert Silence, the import and
   bounce prompts); the Settings window in Live's shape (ADR-0203 d11); the EQ/limiter window
   (ADR-0204 d1).

## 4. The clones lane's queue (Codex Pro), in order

1. **ADI-Q** (the Pro-Q 4 behaviour clone, the channel EQ): finish against the measurement harness
   (the UI-testing rounds already run against Pro-Q 4), as a CLAP and as the native slot's engine
   (ADR-0204 d1): zero-latency and linear-phase modes, dynamic bands, EQ Match, the masking
   highlight, the multi-track overlay carried over from the retired Dynamic EQ (ADR-0195 d4, d5).
2. **ADI-L** (the Pro-L 2 behaviour clone): the per-channel light mode and the master's full mode;
   the Loudness Drive's cascade stage (ADR-0204 d1).
3. **ADI-C** (the K-Clip-class clipper, ADR-0208 d2): the clipper stage of the Loudness Drive's
   cascade beside ADI-L. Then **ADI Still** (soothe2 class) and **ADI Prism** (Serum 2 class), as
   CLAP suite members.
4. **The DJ Filter device** on ADI-Q's cores (ADR-0203 d8), a pure bypass at centre.
5. (Live's devices and their parity passes are the devices lane's since ADR-0208: §4b.)
6. **Reading the references** in `adi_daw/reference/` (the DAWs: Ardour, Zrythm, Tracktion, Helio,
   magda; SoundCraft and FilmCraft for ADR-0206; the DSP references of the plug-in line): each a
   short report in `collab/clones.md` naming what ADI should copy (behaviour) and what it already
   does better; the drone's summaries in `D:\adi-drone\missions\` are the starting point.
7. **The FilmCraft contribution** (the stale-waveform PR 1, `collab/prompts/2026-10-25-codex-filmcraft-pr1-waveforms.md`), on **adi-ubuntu** (192.168.1.97) for the Linux gate: the director's own Linux box, so no new machine is needed.

### 4b. The devices lane's queue (a Codex seat), in order

Standing prompt: `collab/prompts/2026-10-27-devices-lane.md`; log `collab/devices.md`; branches `devices/*`.

1. The zero-delay Pd adapter (ADR-0203 d4): ADI's own devices run their native DSP directly; user
   patches in whole 64-sample multiples without the fixed block; remainders reported.
2. The four device PRs of 30 September (#206 OneShot, #207 the MIDI effects, #208 Sampler, #209
   Hybrid Reverb), rebased and brought to win.
3. Every Live device not yet built, in the manual's order, P1 before P2 (ADR-0169): the audio
   effects, then the instruments, then Live Suite's Max for Live devices as native devices; one PR
   per device with its measured-render test and its plants.
4. The parity passes of the fifteen built effects against Live 12 renders as they arrive.
5. The DJ Filter device (ADR-0203 d8) on ADI-Q's cores once handed over, on Auto Filter's until then.

## 5. The director's gates (the only things that wait on him)

| Gate | When | What he does |
|---|---|---|
| Rust toolchain on adi-ubuntu for the FilmCraft work | approved 2026-10-25 | the box has 8 GB of RAM and a 3rd-gen i5 with its iGPU; builds are slow but unattended (`CARGO_BUILD_JOBS=2`, the target dir on its disk); no discrete GPU needed (Mesa's software Vulkan, as FilmCraft's own CI) |
| Open the FilmCraft PR | when the clones lane reports PR 1 green on Linux | "go", then the PR is opened from his GitHub account |
| ADI-Q / ADI-L hand-over | when Codex calls each ready | run the measurement round himself against Pro-Q 4 / Pro-L 2 on his machine, say "accepted" |
| The first window | when the engine lane reports step 7.2 merged | open it, say what is wrong |
| Live measurements | any time | `docs/AWAITING.md` rows 6 to 17, on his Mac (Live 11) and the PC (Live 12) |
| Master Reference v1.2 | after the above rulings are in | read it; strike the placements marked "win's" |
| Strikes | any time | any "win's placement, to strike if wrong" he disagrees with |

## 6. How win keeps it honest

Weekly: read both lane logs, review and merge their PRs, measure main and update the README count,
re-queue the drone, fold rulings into the ADR log and the Master Reference, and bring the director
only the gates above. The two lanes never wait for each other: the engine lane's channel slots take
whatever ADI-Q/ADI-L build exists; the clones lane tests its plug-ins in any CLAP host until ADI's
window exists.
