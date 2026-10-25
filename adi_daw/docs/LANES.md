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
| **engine** | Claude Max, Opus 5.5, its own session ("ADI engine lane") on the director's PC, in its own worktree | phase A of the brief, then the P1 engine and host work, then merging and finishing step 7 (the window) from the open step-7 PRs | ADR numbers (asks win), `docs/FEATURES.md`, the README count, the Master Reference, anything in `plugins/` the clones lane owns |
| **clones** | Codex Pro, its own session ("ADI clones lane"), its own worktree per plug-in | the director's clones as suite members (ADI-Q, ADI-L, ADI Still, ADI Prism) built from measurements, then the parity passes of Live's devices against Live's renders, then the reference reading (the DAWs and plug-ins in `reference/`) | ADI's engine (`src/adi/engine/**`, `src/juce/**`) except the device contract it plugs into; the UI; ADR numbers |

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

1. **64-bit end to end** (ADR-0207 d2, the director's choice): the sample type behind
   `AudioIo`/`NodeIo`, the nodes, the hosts (VST3 `kSample64`, CLAP 64-bit ports), the Pd tier
   (`PD_FLOATSIZE=64`), the native devices in double; a 32/64 Processing Precision switch on the
   Audio page, default 64; double summing at every mix point first, with Live's Fact Sheet
   cancellation case as the test. Several PRs: the seam, the graph, each host, the Pd tier.
2. **Sends and returns play** (ADR-0203 d3): the planner, compensation across sends, the Live loop
   allowed and every other refused, Pre/Post per return, Enable Send, Solo in Place, the three tap
   points, ADI's sends into any track and a pan on a send; step 6 closes.
3. **A bypassed device keeps its delay** (ADR-0203 d4); the Pd adapter's fixed block removed for the
   built-ins; the master summed (ADR-0203 d14).
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
8. **Step 7, the window:** review-and-rebase the open step-7 PRs (#196, #198, #199, #203, #204,
   #205) against today's main with the rulings of ADR-0203 d5 to d7 (purely as Live; keys; zoom
   around the selection; the strip minimum), merge them through win, then finish 7.2 to 7.6 per
   `docs/STEP-7-PLAN.md`. **This is the gate the director waits for: a window he can open.**
9. Then phase C and D in the brief's order (range selection, fades, tempo detection, automation
   shapes, Capture, Scale Mode, multi-threading, the scanner, stem separation built in, the Loudness
   Drive, Link, the Settings window, the scope).

## 4. The clones lane's queue (Codex Pro), in order

1. **ADI-Q** (the Pro-Q 4 behaviour clone, the channel EQ): finish against the measurement harness
   (the UI-testing rounds already run against Pro-Q 4), as a CLAP and as the native slot's engine
   (ADR-0204 d1): zero-latency and linear-phase modes, dynamic bands, EQ Match, the masking
   highlight, the multi-track overlay carried over from the retired Dynamic EQ (ADR-0195 d4, d5).
2. **ADI-L** (the Pro-L 2 behaviour clone): the per-channel light mode and the master's full mode;
   the Loudness Drive's cascade stage (ADR-0204 d1).
3. **ADI Still** (soothe2 class) and **ADI Prism** (Serum 2 class), as CLAP suite members.
4. **The DJ Filter device** on ADI-Q's cores (ADR-0203 d8), a pure bypass at centre.
5. **Live's devices, the parity passes:** the fifteen Pd effects re-checked against Live 12 renders
   as the director's Live 12 arrives; the MIDI effects, OneShot and Sampler against Live's manual;
   the open device PRs (#206 OneShot, #207 MIDI effects, #208 Sampler, #209 Hybrid Reverb) rebased
   and brought to win for merge.
6. **Reading the references** in `adi_daw/reference/` (the DAWs: Ardour, Zrythm, Tracktion, Helio,
   magda; SoundCraft and FilmCraft for ADR-0206; the DSP references of the plug-in line): each a
   short report in `collab/clones.md` naming what ADI should copy (behaviour) and what it already
   does better; the drone's summaries in `D:\adi-drone\missions\` are the starting point.
7. **The FilmCraft contribution** (the stale-waveform PR 1, `collab/prompts/2026-10-25-codex-filmcraft-pr1-waveforms.md`), on **adi-ubuntu** (192.168.1.97) for the Linux gate: the director's own Linux box, so no new machine is needed.

## 5. The director's gates (the only things that wait on him)

| Gate | When | What he does |
|---|---|---|
| Rust toolchain on adi-ubuntu for the FilmCraft work | now | "yes" (one install: rustup; the box already has the compilers) |
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
