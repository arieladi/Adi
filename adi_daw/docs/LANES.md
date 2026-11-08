# The lanes: how ADI is built from 25 October 2026 until the director can test a window

**Why this file.** The director spent two weeks trying to wire every account into one orchestrator
and found it only complicated things. The decision (ADR-0207): **no orchestrator**. Long-running
sessions, each on the account that does that kind of work best, each with a queue long enough for
months, each picking the next item on its own when one is blocked, each stopping only at a gate
that needs the director. `docs/BUILD-BRIEF.md` stays the order of work; this file says who takes
which part and what "always have work" means in practice. **Since 1 November (ADR-0210): nobody
waits.** The window's needs table heads the engine lane's queue, a seat without the GitHub API
pushes and logs, and win merges whenever the lanes push. **Since 5 November** the order of work
after the needs table is the milestones of `docs/PLAN.md` §2 (M0 to M7, Track C alongside, then
P2 by priority); the brief's phases stay the scope map, and where their order differs the
milestones win (BUILD-BRIEF §6.0).

## 1. The lanes

| Lane | Account and session | Owns | Never |
|---|---|---|---|
| **win** (this one) | Claude, the "ADI Suite Project" session on ORC-VST | coordination: ADRs, FEATURES, the Master Reference, reviews and merges of every PR, the README count, the collab tables, the drone's missions, measurements against Live and the measurement harness (MX-33; `src/adi/measure/**` with the engine), the Windows build the director opens | engine or UI code of its own beyond small fixes and one-hunk merge conflicts; it reviews |
| **engine** | Claude Max or Codex, its own session ("ADI engine lane") | the graph, the hosts, the format and export code: `src/adi/engine/**`, `src/adi/*.cpp` store and ops, `src/juce/*host*`, `src/juce/play*`, `src/juce/device_model*`, `docs/format/**`; `src/adi/sync/**` (MIDI clock and timecode decoding, the clock-out scheduler), the engine's MIDI queues and the Track tap, `src/adi/surfaces/**` (codecs, the resolver, profiles), `src/adi/measure/**` (with win), `src/juce/net_*` (the one network stack), `src/adi/collab/**` (P2), and `src/juce/pd_patch_host.*` with the patch device's dispatch; **first the window's needs table (ADR-0210 d2)**, then the milestones of `docs/PLAN.md` §2 in order; queue §3. **On loan while the devices seat is silent (XC-46, from 5 November):** `src/adi/dsp/loudness` and `src/adi/dsp/limiter` (the line in the devices row) | ADR numbers (asks win), `docs/FEATURES.md`, the README count, the Master Reference, the UI, the device tier, `plugins/`, `src/adi/control/**` and `tools/adi-mcp` (the control seat's) |
| **window** (ADR-0208) | a Claude work seat of its own ("ADI window lane") | the window: `src/adi/ui/**`, `src/juce/ui_*`, `src/juce/app*`, their tests, `docs/STEP-7-PLAN.md`, `docs/UI-ARCHITECTURE.md`, `collab/window.md` and its needs table; `src/juce/app_midi_ports.*` (the one MIDI port host, on JUCE's UMP API; the Windows MIDI Services backend stays off until SRF-16), `src/juce/app_control.*`, `src/juce/app_surfaces.*`, `src/juce/ui_control.*` (the one component model for tests and the API); the Mix View (ADR-0209 d11: the shell, STACK as the director answered Q-B, and the ★ Reference at P1); queue §3b | the engine, the device tier, `plugins/`, ADR numbers; **waiting** (ADR-0210 d3) |
| **devices** (ADR-0208) | a Codex work seat of its own ("ADI devices lane"), re-seated with the prompt of 5 November (XC-46: it had not logged since 28 October) | Live's devices as ADI's built-ins: `pd/**`, `src/adi/dsp/**`, `src/adi/pd_builtins/**`, `cmake/devices/**`, their tests; `src/juce/pd_*`: the Pd runtime (`pd_engine.*`, `pd_device.*`, `pd_declarations.*`, `pd_loader_fix.cmake`) and the zero-delay adapter; the engine owns `pd_patch_host.*` and dispatch; queue §4b. **On loan while the devices seat is silent (XC-46, from 5 November):** `src/adi/dsp/loudness` and `src/adi/dsp/limiter` are the engine seat's, and the Pd safety overlays (DEV-09) go to the seat that is running; the loan ends the day the devices seat logs again, and win strikes this line then | the graph, the UI, `plugins/`, ADR numbers |
| **clones** | Codex Pro, the director's own programmer and reverse-engineer session | the director's plug-ins, built from measurements as ordinary VST3 and CLAP plug-ins under his names. Nothing in ADI waits for them and no slot is reserved; when he declares them all finished and sends his list, their source becomes one unified built-in feature, designed then (the ruling of 5 November); the FilmCraft contribution; then the reference reading | everything outside `plugins/`; ADR numbers |
| **control** (Track C; Q-C, answered (a) on 5 November: now, on a seat of its own) | a work seat of its own ("ADI control seat", Claude or Codex), in parallel with the lanes | the control core: `src/adi/control/**`, `tools/adi-mcp/**`, their tests, `collab/control.md`; `src/juce/app_control.*` (the app host, named in Q-C's answer) as a PR of its own that the window lane reads before win merges, since `src/juce/app*` is the window's ground; the window's test kit with the window (the WTS rows of Track C); queue §3c | the UI beyond that one file pair (`src/juce/ui_control.*` and the Clients page are the window's), the engine's graph and store, the device tier, `plugins/`, ADR numbers, the registry rows and the `third_party` pins (win's, proposed in its PR); **the director's desktop** (never ADI.exe there) |
| **measurement seat** (G-1, answered 5 November) | the director himself, with a Claude session on the second Windows machine (Live 12.4), from Sunday 9 November; his brief is in his private `Live12-readings` folder | the Live 12 readings through `tools/measure/**` and the interaction logger, each delivered as a dated record labelled with the Live build; factory-default readings from a fresh profile only (an existing account's settings are not defaults); win relays records into AWAITING and the parity rows (the relay rule, MX-33) | ADI.exe on his desktop; a reading from the manual, a palette or memory called measured |
| **sidecar seat** (P2) | a seat named when the agent's user side starts | `adi_daw/agent/**` | everything else; ADR numbers |
| **Stream Deck seat** (P2) | a seat named when the surfaces start | `adi_daw/tools/streamdeck/**` | everything else; ADR numbers |

The drone (qwen on the 1070 and the A2000) keeps summarising files and manuals; win feeds it.

## 2. Rules that keep the lanes apart (all existing rules, collected)

- One branch per item, prefixed `engine/`, `window/`, `devices/`, `clones/` or `control/` (`collab/README.md`
  rule 1); a worktree per item; never the main checkout; never `git add -A`.
- A PR per item, CI green on the PR gate, `tools/test_all.sh` green, MSVC `/WX` clean; the parity
  gate for anything the user sees (ADR-0108). **win merges.** A lane never merges its own PR.
  The PR gate is every Linux and Windows leg and the three light macOS legs (the validators, JUCE
  on, the GUI shell); the two heavy macOS legs, the universal build and the apple clang sweep, run
  on main after the merge, on dispatch, and on a PR labelled `full-ci` (since 2026-11-03: GitHub
  serves one macOS job at a time, and those two were the queue). A heavy leg red on main is
  win's, at the head of the queue, before any merge.
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

Standing prompt: `collab/prompts/2026-11-01-engine-lane.md` (the Codex seat's issue:
`collab/prompts/2026-11-04-engine-lane-codex.md`); log `collab/engine.md`; branches `engine/*`.

1. **The window's needs table, oldest open first** (`collab/window.md`, "Needs from other lanes";
   ADR-0210 d2), each need its own PR, the window striking the row when it merges. On 1 November
   the open rows the engine owns, in the window's order: N1, N2, N5, N11, N16 (withdrawn by the
   director on 5 November), N3 and N30, N9, N13, N19, N22, N23 with the ★ Reference route of
   ADR-0209 d4, N25, N26, N29, N32 to N34, N7, N8, N12, N17, N18, N20, N28, N31, N35. Rows win owns
   (N4, N14, registry rows) are win's. **One dated exception to ADR-0210 d2 (P-9, 5 November):**
   the Link consumer, in flight when the plan was approved, finishes to its gate first; from then
   on d2's order holds exactly: the oldest open need from the window's table before anything of
   this section, so N29, N17's open part and the edit kernel (the plan's M2), then tempo ramps on
   Link's effective-tempo path, then the rest in the order of `docs/PLAN.md` §2.
2. **ADR-0209's engine part, ruled 1 November (§10):** at P1, with the needs above, `tracks.role`
   with `reference` and its op, and the exclusive reference route on the cue path (N23); at P2,
   after the first-window gate, the other roles, the Quick Kick's routing half (a compensated
   sidechain from the chosen tap, Pre FX by default; what the button engages waits for the
   director's list), multi-track tap subscription batching.
3. **M0, now** (`docs/PLAN.md` §2): the NI Stems refusal (EX-10: a job with normalization is
   refused until the after-AAC check exists); #456 re-pinned to `Link-4.1.1` inside the same
   stack, Link Audio off (XC-31); held notes keyed by the stored note identity (XC-25); the rebases
   of #445, #447 and #441; the reference-pitch guard on #443's branch (TU-01: a source whose
   reference pitch would move a degree off its 12-TET anchor is refused with a verdict naming the
   line). The Drive's "blocked" entry lifts once the records of 5 November merge. **The stale
   texts the engine owns, from the plan's R-e and §8.4, as one docs PR on `engine/*`** (win cannot
   edit `docs/OPS.md` or `docs/format/**`, §1): OPS §9.2 names `track.setFreeze` and
   `group.setFreezeScope` as the freeze ops and strikes `track.freeze` / `track.unfreeze`, which
   `ops_catalog.cpp` does not have (ADR-0223 d4); OPS §6.4's 300 ms merge gives way to the gesture
   contract, the quiet window only as the end detector for sources that send no release
   (ADR-0216 d5; the window's N44); OPS §11 item 4 closes with the comp verbs named, `lane.*` as
   §9.2 plus `clip.setLane`, the auditioned lane as session state and not an op (ADR-0225 d2);
   the six tuning and key ops of #444 (`tuning.create`, `tuning.delete`, `tuning.replace`,
   `project.insertKeyEvent`, `project.removeKeyEvent`, `project.setKeyScale`) listed at P1
   (TU-01; ADR-0203 d7, ADR-0227 d1); OPS §9.7's rack ops marked as capturing their whole subtree
   (parameters, state, panels, routes, macros and the mappings that target them; DEV-03, P2;
   ADR-0110, ADR-0203 d10); SPEC §6.2 reworded to Live's model, one main lane plays, take lanes
   are auditioned, a comp is copies on the main lane, a NULL `lane_id` is the main lane, ADR-0037's
   comping wording superseded (ADR-0225 d1, d2). SPEC §3.7's rewording rides with the window's
   N45, SPEC §8.3's with HI-02's PR (ADR-0213), SPEC §6.5's with TM-SV-02's minor and SPEC §12
   item 6 with CO-01 (both P2).
4. **M2, the edit kernel and the gesture (P0)**, right after the Link consumer reaches its gate
   (item 1's exception) and once win's gesture and EditScope ADRs are in: N29 and N17's open part;
   ED-03, ED-04 (the anchored left trim; Crop), MI-01, MI-02; HI-01, HI-02 and HI-03 (the history:
   compaction best-effort to a low-water mark, reporting and never failing a commit, P-10; the
   director's Q-A1 answer A: every named snapshot and every dead end kept once as a deduplicated
   stored state, so the cap of ADR-0203 d13 always holds; the daily snapshot as d13; `moveTo`);
   AU-01 and AU-05 (Lock Envelopes read by the planner); the one allocator function (XC-22).
5. **M3, time, media, MIDI in and safe files:** the Link line (#454, #455, #456, each retargeted
   to main in that order), then the consumer with XC-25's continuity, then tempo ramps on the same
   effective-tempo path (TM-TT-01); Global Quantization (TM-SV-01); the media line (the
   AudioSpecificConfig fix on #452, then #451, #452, #450; then #448, #457, #449; MP4, MOV and ALAC
   playing while they decode); relink, PD-04, PD-03, PD-11, EX-01; the Track tap; SRF-03, SRF-04;
   **key detection behind one analyser interface** (TM-MD-04; the director's Q-D answer (a): qm-dsp
   in the DAW meanwhile, behind one interface, so the suite's analyser can replace it without a
   second cache format).
6. **M4 to M7, in `docs/PLAN.md` §2's order.** M4: the taps and keys into devices at Live's three
   taps (DEV-05, MX-24), the In/Out choosers' engine side (MX-25), monitoring with Arm and the
   switches (MX-09), the loopback input (MX-17), N13, export normalization and Auto Gain Stage
   rewired onto the one loudness meter (DEV-28; the meter itself while it is on loan), the
   Reference's matched loudness (MX-29), the Loudness Drive as one automatable gain over the leaf
   tracks' input gain, engaging nothing (P-1; ADR-0203 d14), the visual taps. M5: the crash marker
   and recovery (PD-05), one capture for every render, the bounces and Consolidate, the recipe key,
   freeze (track, Freeze All Channels, Freeze Group; renders placed Warp off, ADR-0203 d16), the
   export core (LUFS targeting of ADR-0203 d14, NI Stems export and import of ADR-0205 d8, the
   Export settings; the `.adiviz` pass rides on it at P2; ADR-0071), DAWproject, the time base, job
   states, Save As and Save a Copy's remaining engine parts, Live's Backup folder (the director's
   Q-A2 answer (a): ten timestamped copies in the project's Backup folder from the second Take
   Snapshot, the oldest to the Recycle Bin; PD-07). M6: DEV-17 and DEV-18 (note identity and the
   transform phase), take lanes and comping as Live's, the automation passes, tuning (TU-02's
   stored reference), **the analyser benchmark before Auto-Warp's analysis core lands** (TM-WT-01,
   with win: a labelled local corpus, beat_this as the independent comparator; the director's Q-D
   answer (a): then one beat, tempo and key analyser for ADI and ADiJ, chosen by the numbers), warp
   analysis and the short-sample rule (TM-WT-05; the director's Q-E answer (a): every drop arrives
   clean, Live's short-sample result on Warp or from the import prompt, the setting offers Live's
   four choices). M7: Graph threads and the CPU timings, the modulation nodes and routes, the Pd
   user tier on his answers of 5 November (Q-K1 A: vanilla Pd's editor attached to the running
   device, edits heard at once; Q-K2 1: no added delay when the buffer is a multiple of 64, events
   on Pd's tick, one reported block otherwise; Q-K3 1: editing a built-in makes a copy "<Device>
   (edited)" on the Pd path; Q-GRID (a): the hybrid on Pd with host-owned timing, DEV-15), the
   scanner's registry v3 and scan service, AudioGridder. Then P2 by priority (`docs/PLAN.md` §2,
   "After M7").

Merged before this version: precision (ADR-0207 d2, #227 to #237), the sends and returns (#242 to
#249), the bypassed device's delay and the master summed (#297, #298), the export jobs (#300 to
#302), the snapshot's hierarchy fields (#299); the stack of 4 and 5 November (#380 to #430).

### 3b. The window lane's queue (a Claude seat), in order

Standing prompt: `collab/prompts/2026-11-01-window-lane.md` (resumed through
`collab/prompts/2026-11-03-window-lane.md`); log `collab/window.md`; branches `window/*`.

1. **The worklist of 5 November** (`collab/window/worklist-2026-11-05.md`): what
   `docs/STEP-7-PLAN.md` 7.2 to 7.6 still lists and no merged PR covers, in its groups and order
   (A1 to A5 the document, the File menu and the history; B1 to B8 the mixer's own Live items; C1
   to C7 the arrangement and the control bar; D1 to D5 the browser and the devices), each with its
   headless test, its parity checklist from the Live 12 manual text, and a picture of ADI beside
   Live's in the PR body. (The six rebased step-7 PRs and forty more merged 1 November, #246 to
   #296.) **The first-window gate passed on build `add5102` (2 November): his remarks R1 to R7 are
   in `collab/director.md`, and his remarks on every Windows build win hands him come before new
   items** (item 5).
2. **The Mix View (ADR-0209, ruled 1 November, §10), as the director answered Q-B on 5 November:**
   `MixViewPanel`, F4 and the top-bar button, docked or detached; the ★ Reference strip, the
   [👁 Reference Track] toggle and the [REF] button with Shift+R, tap or hold (P1, d11), each from
   what the snapshot has today with stubs for the fields the engine has not written; then, after
   the mixer's own Live items (B1 to B8), the strip scope and spectrum, the analyser's view and the
   Scope window; then **the STACK tab, in his words:** "each track's own devices, Live's and VST3s
   loaded as plug-ins, like in the Ableton device area, only with the FabFilter Pro-Q instance view
   where what I am standing on and tinkering with gets a zoomed-in vibe, bigger than the rest, and
   I can move to the rest and they get bigger instead; VST3s as live tiles with the fisheye; when
   the processor lands, it joins the row." A live list of every track's own devices, Live's and his
   VST3s as ordinary plug-ins, with the fisheye, navigator, search and pins; no slot and no Q or L
   switch is built, and the drawn slot columns stay off (the ruling of 5 November). The other four
   tabs, the CTZ navigator, the Info View switcher and the device strip's pinned analyser are P2;
   [⚡ Kick] waits for his list; OVERVIEW, SCOPE & PHASE and SPECTROGRAM from the existing analyser
   and Scope (d12). One PR per tab or part. **The window's own texts that go with it, in the first
   Mix View PR** (ADR-0212 d2 and its amendments table; `docs/PLAN.md` §8.4, MX-34; win does not
   edit `docs/UI-ARCHITECTURE.md` or `collab/window/**`, §1): UI-ARCHITECTURE §12 and the two parity
   checklists, `collab/window/parity-7.6-mixer.md` row 11 and `collab/window/parity-mix-view.md`
   row 6, say that the slot drawings (the two slots on every mixer row, STACK's dormant slots, the
   sidechain-slot and ADI-L drawers, the pinned slot's ADI-Q / RMSC curve, `CascadeTab`'s ADI-L
   state) stay as drawn and off, unremoved, until his list; that STACK's rows are his Q-B answer
   above; and that nothing of the unified feature is designed or hosted there.
3. **The Settings window's pages** in Live's ten tab names and order, then ADI's own (ADR-0203
   d11, Master Reference Part V); then the window parts of phase C (the worklist's E1 to E6: split
   and the range edits that split a crossing clip, Cut, Copy, Paste and Duplicate of clips and
   ranges, the …Time commands and Insert Silence, the fade handles with Create Fade and Crossfade,
   the import prompt, the bounce prompts).
4. **The MIDI port host** on JUCE's UMP API with the Windows MIDI Services backend off (SRF-05; the
   service is SRF-16 at P2), **the Master Focus Dial** (SRF-06) and **the command palette**
   (WIN-06). **The Export page** (EX-11), Live's dialog first, on stubs until the engine's capture
   lands.
5. **Parity passes** of every built surface against the Master Reference's Part II and the Live 12
   manual (the worklist's F1 and F2 first); the Clip View (G1 to G3) once the director approves its
   picture; the director's remarks on the window as they arrive, each a row in the log and an item
   ahead of everything new. `collab/window/parity-7.6-mixer.md` row 6 is amended in WIN-01's PR
   (M1): F1 to F8 lose the activator defaults, the eight commands stay unbound for the palette and
   the Shortcuts page, F4 is the Mix View, F3 and F7 are held for Session View and offline
   processing (ADR-0203 d6, d12; ADR-0209 d9; `docs/PLAN.md` §8.4).

### 3c. The control seat's queue (Track C; a seat of its own since Q-C, 5 November), in order

Standing prompt: `collab/prompts/2026-11-05-control-api-seat.md`; log `collab/control.md` on
`control/log`; branches `control/*`. It runs in parallel with the lanes and never on the
director's desktop; the rules of §2 apply unchanged. The app host `src/juce/app_control.*` is
its one PR into the window's ground, read by the window lane before win merges; what else it
needs from the window (`src/juce/ui_control.*`, the Clients page) is a row in the window's needs
table, never its own edit.

1. **The registry describes itself** (CTL-02): the JSON Schema export, descriptions first for the
   ops the agent may emit.
2. **The control core and its one listener** (CTL-03, CTL-04) under win's "one control stack" ADR
   (CTL-01: one dispatcher, one listener on loopback, off by default; `adi-mcp` over stdio on the
   same server and port; every project method names its project).
3. **`adi-mcp`, headless first** (CTL-08's headless backend over the core), then its app-host
   backend through the discovery record; **where the secrets live is the director's Q-O**, so the
   first PR that needs it writes the discovery mechanism as he answers, and nothing before.
4. **The test kit with the window** (WTS-03 to WTS-06, WTS-10: the goldens, the render review job,
   `UiControl`, the headless script runner, the running app over the API) and the test grant; the
   app host (`src/juce/app_control.*`) as its own PR read by the window lane; the Clients page is
   the window's (CTL-09).
5. **The changeset as a service and the Observe tier** (AGT-01 with the 6,000-op cap of ADR-0203
   d20, AGT-02's stable project identity); the evaluation corpus (AGT-08) is win's.

Consumers of the API (the agent's user side, the surfaces, visual streams, scripts, the ArtCraft
bridge) stay P2 in their own lanes.

## 4. The clones lane's queue (Codex Pro), in order

1. **His plug-ins** (ADI-Q, ADI-L, ADI-C, ADI Still, ADI Prism, ADI Spacer, ADI Fuser): finished in
   his tree as ordinary VST3 and CLAP plug-ins, measured against their references. ADI loads them
   like any plug-in (DEV-26). Nothing in ADI waits for them. When he declares them all finished and
   sends his list, their source becomes one unified built-in feature with a device-like window; its
   design starts then (the ruling of 5 November).
2. **Reading the references** in `adi_daw/reference/` (the DAWs: Ardour, Zrythm, Tracktion, Helio,
   magda; SoundCraft and FilmCraft for ADR-0206; the DSP references of the plug-in line): each a
   short report in `collab/clones.md` naming what ADI should copy (behaviour) and what it already
   does better; the drone's summaries in `D:\adi-drone\missions\` are the starting point.
3. **The FilmCraft contribution** (the stale-waveform PR 1, `collab/prompts/2026-10-25-codex-filmcraft-pr1-waveforms.md`), on **adi-ubuntu** (192.168.1.97) for the Linux gate: the director's own Linux box, so no new machine is needed.
4. **ADI Spacer and ADI Fuser (ADR-0209 d12, ruled 1 November):** the SPAN, s(M)exoscope and VISU
   work is a parity pass on the existing analyser and Scope, not this lane's; **ADI Spacer** (the
   Trackspacer class) and **ADI Fuser** (the FUSER class) are new DSP after ADI-C, the Bluprint
   matcher last, each from measurements, original C++ under the
   director's names, handed over through `plugins/` with its measurement report.

### 4b. The devices lane's queue (a Codex seat), in order

Standing prompt: `collab/prompts/2026-11-05-devices-lane-codex.md` (it replaces
`collab/prompts/2026-10-27-devices-lane.md`; the seat had not logged since 28 October, XC-46); log
`collab/devices.md`; branches `devices/*`. Until the re-seated seat logs, the loan of §1 holds.

1. The zero-delay Pd adapter (ADR-0203 d4): merged 1 November (#250). Remainders reported in the log.
2. **The one loudness meter, first** (`src/adi/dsp/loudness`, DEV-28): the meter every reading uses
   (Main's meter, export normalization, Auto Gain Stage's LUFS mode, the Reference's match), proved
   against synthesised Tech 3341 and 3342 cases and libebur128 as test-only oracles; on the
   critical path of M4 and M5.
3. **The Pd runtime's safety (DEV-09) at once**, beside the meter; the user-patch tier (DEV-10 to
   DEV-16) on the director's Q-K answers of 5 November (vanilla Pd's editor attached to the running
   device; no added delay when the buffer is a multiple of 64; editing a built-in makes a copy
   "<Device> (edited)" on the Pd path) and Q-GRID (a) (the hybrid on Pd with host-owned timing,
   DEV-15), in M7.
4. The four device PRs of 30 September (#206 OneShot, #207 the MIDI effects, #208 Sampler, #209
   Hybrid Reverb), rebased and brought to win: Hybrid Reverb merged (#253), OneShot in draft (#278),
   #207 and #208 closed as "to be ported": their kernels are ported onto the engine's note identity
   and transform phase (DEV-17, DEV-18) once those land, and Sampler onto OneShot (#278).
5. Every Live device not yet built, in the manual's order, P1 before P2 (ADR-0169): the audio
   effects, then the instruments, then Live Suite's Max for Live devices as native devices; one PR
   per device with its measured-render test and its plants. (Limiter merged, #259.)
6. The parity passes of the fifteen built effects against Live 12 renders as they arrive.
7. **The limiter's true-peak path**, one change shared with export (EX4, LD5).
8. The DJ Filter device (ADR-0203 d8) on Auto Filter's cores, a pure bypass at centre.

## 5. The director's gates (the only things that wait on him)

| Gate | When | What he does |
|---|---|---|
| Rust toolchain on adi-ubuntu for the FilmCraft work | approved 2026-10-25 | the box has 8 GB of RAM and a 3rd-gen i5 with its iGPU; builds are slow but unattended (`CARGO_BUILD_JOBS=2`, the target dir on its disk); no discrete GPU needed (Mesa's software Vulkan, as FilmCraft's own CI) |
| Open the FilmCraft PR | when the clones lane reports PR 1 green on Linux | "go", then the PR is opened from his GitHub account |
| His list of plug-ins to rework | when he declares all his plug-ins finished | sends the list; the unified feature's design starts then |
| The first window | passed 2 November on build `add5102` (ADR-0210 d6; R1 to R7 in `collab/director.md`); every later Windows build win hands him the same way | open it, say what is wrong; each remark becomes a window-lane item |
| Live measurements | any time | `docs/AWAITING.md` rows 6 to 17, on his Mac (Live 11) and the PC (Live 12) |
| The Live 12 measurement seat | answered 5 November (G-1) | he runs the readings himself with a Claude session on the second Windows machine from Sunday 9 November; the records reach win dated and labelled with the Live build |
| The plan's choices | Q-A1 to Q-E, Q-K and Q-GRID answered 5 November in chat; the rest as their milestone comes (`docs/PLAN.md` §6.3) | answers in `collab/director.md` |
| ADR-0209, the Mix View | ruled 1 November | strike the placements if wrong: Shift+R for the ★ Reference (d9), `tracks.role` |
| Master Reference v1.3 | built 1 November | read it; strike the placements marked "win's" |
| Strikes | any time | any "win's placement, to strike if wrong" he disagrees with |

## 6. How win keeps it honest

**Whenever the lanes push, not weekly (ADR-0210 d5):** read the lane logs, merge every green PR the
same day in stack order (a one-hunk conflict between lanes is merged main-into-branch by win and
noted in `collab/win.md`), open the PRs a seat without the API requested, measure main and update
the README count, build main on Windows and tell the director where the executable is, re-queue
the drone, fold rulings into the ADR log and the Master Reference, and bring the director only the
gates above. The lanes never wait for each other: the engine reserves nothing for his plug-ins
and the window draws no slot for them (the ruling of 5 November); the window builds on stubs and
logs the need; the clones lane tests its plug-ins in any CLAP host until ADI's window exists.
Windows-only work from a seat without MSVC is proven on the Windows CI legs by SHA and on the
director's Windows workstation by win (XC-45). The Windows MIDI Services backend (SRF-16) needs a
Windows 11 seat with the service, a Windows SDK pin of 10.0.20348 or later, the service's NuGet
package in a local repository on the Windows legs and an installer prerequisite; nothing of it is
claimed on Windows 10.
