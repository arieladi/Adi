# The lanes: how ADI is built from 25 October 2026 until the director can test a window

**Why this file.** The director spent two weeks trying to wire every account into one orchestrator
and found it only complicated things. The decision (ADR-0207): **no orchestrator**. Long-running
sessions, each on the account that does that kind of work best, each with a queue long enough for
months, each picking the next item on its own when one is blocked, each stopping only at a gate
that needs the director. `docs/BUILD-BRIEF.md` stays the order of work; this file says who takes
which part and what "always have work" means in practice. **Since 1 November (ADR-0210): nobody
waits.** The window's needs table heads the engine lane's queue, a seat without the GitHub API
pushes and logs, and win merges whenever the lanes push.

## 1. The five lanes

| Lane | Account and session | Owns | Never |
|---|---|---|---|
| **win** (this one) | Claude, the "ADI Suite Project" session on ORC-VST | coordination: ADRs, FEATURES, the Master Reference, reviews and merges of every PR, the README count, the collab tables, the drone's missions, measurements against Live, the Windows build the director opens | engine or UI code of its own beyond small fixes and one-hunk merge conflicts; it reviews |
| **engine** | Claude Max or Codex, its own session ("ADI engine lane") | the graph, the hosts, the format and export code: `src/adi/engine/**`, `src/adi/*.cpp` store and ops, `src/juce/*host*`, `src/juce/play*`, `src/juce/device_model*`, `docs/format/**`; **first the window's needs table (ADR-0210 d2)**, then the P1 engine and host work (ADR-0209's engine part, d11's P1 pieces with the needs); queue §3 | ADR numbers (asks win), `docs/FEATURES.md`, the README count, the Master Reference, the UI, the device tier, `plugins/` |
| **window** (ADR-0208) | a Claude work seat of its own ("ADI window lane") | the window: `src/adi/ui/**`, `src/juce/ui_*`, `src/juce/app*`, their tests, `docs/STEP-7-PLAN.md`, `docs/UI-ARCHITECTURE.md`, `collab/window.md` and its needs table; the Mix View (ADR-0209 d11: the shell, STACK and the ★ Reference at P1); queue §3b | the engine, the device tier, `plugins/`, ADR numbers; **waiting** (ADR-0210 d3) |
| **devices** (ADR-0208) | a Codex work seat of its own ("ADI devices lane") | Live's devices as ADI's built-ins: `pd/**`, `src/adi/dsp/**`, `src/adi/pd_builtins/**`, `cmake/devices/**`, their tests; `src/juce/pd_*` for the zero-delay adapter only; queue §4b | the graph, the UI, `plugins/`, ADR numbers |
| **clones** | Codex Pro, the director's own programmer and reverse-engineer session | the director's clones as suite members (ADI-Q, ADI-L, **ADI-C**, ADI Still, ADI Prism; ADI Spacer and ADI Fuser of ADR-0209 d12, after ADI-C) built from measurements, in `plugins/`; the FilmCraft contribution; then the reference reading | everything outside `plugins/`; ADR numbers |

The drone (qwen on the 1070 and the A2000) keeps summarising files and manuals; win feeds it.

## 2. Rules that keep the lanes apart (all existing rules, collected)

- One branch per item, prefixed `engine/`, `window/`, `devices/` or `clones/` (`collab/README.md`
  rule 1); a worktree per item; never the main checkout; never `git add -A`.
- A PR per item, CI green on 7 ABIs, `tools/test_all.sh` green, MSVC `/WX` clean; the parity gate
  for anything the user sees (ADR-0108). **win merges.** A lane never merges its own PR.
- The docs win over the code; a decision is a new ADR, numbers from win only.
- Live's way, additions first-class, the director's deviations stay (ADR-0203 d1, d2, d12); in the
  engine layer, copy whoever does it best (ADR-0205 d10).
- Licensing is closed; measurement clones are welcome; never raise it (ADR-0203 d25, ADR-0204 d3).
- Each lane logs in its own file, on its own `<lane>/log` branch so item PRs never conflict on it:
  `collab/engine.md`, `collab/window.md`, `collab/devices.md`, `collab/clones.md` (newest first).
- **When blocked, take the next item.** A lane stops only at a director's gate (§5); everything
  else it decides, records in its log, and win reviews in the PR.
- **Nobody waits (ADR-0210 d3).** The window builds a missing engine field as a row in its needs
  table and a stub in the UI, and ships; the engine takes that row next (§3 item 1). A missing
  decision is Live's behaviour, logged.
- **A seat without the GitHub API (ADR-0210 d4)** pushes its branch and writes the PR title and
  body into its log under "PR requested"; win or the window lane opens the PR.
- **Stacks:** at most two deep unless each PR body names the one it stands on; after win merges,
  the lane rebases the rest.
- **Usage limits are not blockers.** A session that hits its limit resumes with "continue" when it
  resets; its queue is in this file and in its log, so nothing is lost. The director's one job
  between sessions is to type "continue" and to answer gates.

## 3. The engine lane's queue, in order

Standing prompt: `collab/prompts/2026-11-01-engine-lane.md`; log `collab/engine.md`; branches `engine/*`.

1. **The window's needs table, oldest open first** (`collab/window.md`, "Needs from other lanes";
   ADR-0210 d2), each need its own PR, the window striking the row when it merges. On 1 November
   the open rows the engine owns, in the window's order: N1, N2, N5, N11, N16 (now the three slots
   of ADR-0209 d1), N3 and N30, N9, N13, N19, N22, N23 with the ★ Reference route of ADR-0209 d4,
   N25, N26, N29, N32 to N34, N7, N8, N12, N17, N18, N20, N28, N31, N35. Rows win owns (N4, N14,
   registry rows) are win's.
2. **ADR-0209's engine part, ruled 1 November (§10):** at P1, with the needs above, `tracks.role`
   with `reference` and its op, and the exclusive reference route on the cue path (N23); at P2,
   after the first-window gate, the other roles, the ★ Kick sidechain helper (wake the slot, wire
   the compensated edge from the chosen tap, Pre FX by default), the CTZ group metadata the
   navigator reads, multi-track tap subscription batching, the RMSC slot mode on ADR-0166's DSP.
3. **The export queue's rest** (ADR-0071, P1): LUFS targeting (ADR-0203 d14), NI Stems export and
   import (ADR-0205 d8), the Export settings; the `.adiviz` pass rides on it later (P2). (Jobs,
   stems, PCM and dither merged 1 November, #300 to #302.)
4. **Freeze and bounce** (ADR-0203 d16): track freeze, Freeze All Channels, Freeze Group, Bounce in
   Place, Bounce to New Track; renders placed Warp off.
5. **History caps and Save As** (ADR-0203 d13, d6): 2000 steps or 1000 MB, the daily snapshot,
   Take Snapshot, Save As, Save a Copy, Collect All and Save (N3 and N30 above are the first part).
6. Then phase C and D in the brief's order (range selection, fades, tempo detection, automation
   shapes, Capture, Scale Mode, multi-threading, the scanner, stem separation built in, the Loudness
   Drive, Link, the scope's engine side).

Merged before this version: precision (ADR-0207 d2, #227 to #237), the sends and returns (#242 to
#249), the bypassed device's delay and the master summed (#297, #298), the export jobs (#300 to
#302), the snapshot's hierarchy fields (#299).

### 3b. The window lane's queue (a Claude seat), in order

Standing prompt: `collab/prompts/2026-11-01-window-lane.md`; log `collab/window.md`; branches `window/*`.

1. **What `docs/STEP-7-PLAN.md` 7.2 to 7.6 still lists and no merged PR covers**, each with its
   headless test, its parity checklist from the Live 12 manual text, and a picture of ADI beside
   Live's in the PR body. (The six rebased step-7 PRs and forty more merged 1 November, #246 to
   #296.) **The first window the director can open on his PC is his gate (§5): win builds main on
   Windows and hands him the executable; what he says about it is this lane's next items.**
2. **The Mix View (ADR-0209, ruled 1 November, §10):** `MixViewPanel`, F4 and the top-bar button,
   docked or detached; the STACK tab (the "one window" of ADR-0204 d1), the ★ Reference strip, the
   [👁 Reference Track] toggle and the [REF] button with Shift+R, tap or hold: that is P1 (d11), each
   from what the snapshot has today with stubs for the fields the engine has not written. The other
   four tabs, the CTZ navigator, [⚡ Kick], the Info View switcher and the device strip's pinned
   analyser are P2, after the first-window gate, drawn empty until then; OVERVIEW, SCOPE & PHASE and
   SPECTROGRAM from the existing analyser and Scope (d12). One PR per tab or part.
3. **The Settings window's pages** in Live's ten tab names and order, then ADI's own (ADR-0203
   d11, Master Reference Part V); then the window parts of phase C (range selection, fade handles,
   Insert Silence, the import and bounce prompts).
4. **Parity passes** of every built surface against the Master Reference's Part II and the Live 12
   manual; the director's remarks on the window as they arrive.

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
5. **Reading the references** in `adi_daw/reference/` (the DAWs: Ardour, Zrythm, Tracktion, Helio,
   magda; SoundCraft and FilmCraft for ADR-0206; the DSP references of the plug-in line): each a
   short report in `collab/clones.md` naming what ADI should copy (behaviour) and what it already
   does better; the drone's summaries in `D:\adi-drone\missions\` are the starting point.
6. **The FilmCraft contribution** (the stale-waveform PR 1, `collab/prompts/2026-10-25-codex-filmcraft-pr1-waveforms.md`), on **adi-ubuntu** (192.168.1.97) for the Linux gate: the director's own Linux box, so no new machine is needed.

7. **ADI Spacer and ADI Fuser (ADR-0209 d12, ruled 1 November):** the SPAN, s(M)exoscope and VISU
   work is a parity pass on the existing analyser and Scope, not this lane's; **ADI Spacer** (the
   Trackspacer class) and **ADI Fuser** (the FUSER class) are new DSP after ADI-C, the Bluprint
   matcher last, each from measurements, original C++ under the
   director's names, handed over through `plugins/` with its measurement report.

### 4b. The devices lane's queue (a Codex seat), in order

Standing prompt: `collab/prompts/2026-10-27-devices-lane.md`; log `collab/devices.md`; branches `devices/*`.

1. The zero-delay Pd adapter (ADR-0203 d4): merged 1 November (#250). Remainders reported in the log.
2. The four device PRs of 30 September (#206 OneShot, #207 the MIDI effects, #208 Sampler, #209
   Hybrid Reverb), rebased and brought to win: Hybrid Reverb merged (#253), OneShot in draft (#278),
   the MIDI effects and Sampler still to come.
3. Every Live device not yet built, in the manual's order, P1 before P2 (ADR-0169): the audio
   effects, then the instruments, then Live Suite's Max for Live devices as native devices; one PR
   per device with its measured-render test and its plants. (Limiter merged, #259.)
4. The parity passes of the fifteen built effects against Live 12 renders as they arrive.
5. The DJ Filter device (ADR-0203 d8) on ADI-Q's cores once handed over, on Auto Filter's until then.

## 5. The director's gates (the only things that wait on him)

| Gate | When | What he does |
|---|---|---|
| Rust toolchain on adi-ubuntu for the FilmCraft work | approved 2026-10-25 | the box has 8 GB of RAM and a 3rd-gen i5 with its iGPU; builds are slow but unattended (`CARGO_BUILD_JOBS=2`, the target dir on its disk); no discrete GPU needed (Mesa's software Vulkan, as FilmCraft's own CI) |
| Open the FilmCraft PR | when the clones lane reports PR 1 green on Linux | "go", then the PR is opened from his GitHub account |
| ADI-Q / ADI-L hand-over | when Codex calls each ready | run the measurement round himself against Pro-Q 4 / Pro-L 2 on his machine, say "accepted" |
| The first window | when win hands him the Windows build of main (ADR-0210 d6) | open it, say what is wrong; each remark becomes a window-lane item |
| Live measurements | any time | `docs/AWAITING.md` rows 6 to 17, on his Mac (Live 11) and the PC (Live 12) |
| ADR-0209, the Mix View | ruled 1 November | strike the placements if wrong: Shift+R for the ★ Reference (d9), `tracks.role` and the slot rows (Q8) |
| Master Reference v1.3 | built 1 November | read it; strike the placements marked "win's" |
| Strikes | any time | any "win's placement, to strike if wrong" he disagrees with |

## 6. How win keeps it honest

**Whenever the lanes push, not weekly (ADR-0210 d5):** read the lane logs, merge every green PR the
same day in stack order (a one-hunk conflict between lanes is merged main-into-branch by win and
noted in `collab/win.md`), open the PRs a seat without the API requested, measure main and update
the README count, build main on Windows and tell the director where the executable is, re-queue
the drone, fold rulings into the ADR log and the Master Reference, and bring the director only the
gates above. The lanes never wait for each other: the engine's slots take whatever ADI-Q/ADI-L
build exists; the window builds on stubs and logs the need; the clones lane tests its plug-ins in
any CLAP host until ADI's window exists.
