# ADI: the plan

5 November 2026, revised the same day for the critique of its first draft (Appendix C). Win's plan
for the director's request of 4 November: read the reference research in full, take whatever
open code ADI can take, and plan the DAW.

**This is the public version.** The working files behind it (the six area matrices, the
cross-check, the harvest ledger, the choices file) are win's planning files and are not in the
repository; everything they decided is here. The research is cited by its research IDs only, and
what it gave ADI is listed in `docs/RESEARCH-INTAKE.md`.

**Status.** A proposal. Nothing was built, run, pushed, merged or applied for it; no ADR was added;
no FEATURES row, default or approved UI was changed. Where this text says "win records" or "an
ADR", that is what win writes once the director approves the plan; the ADRs that approval would
let win write are listed in §6.2 (P-6 and P-8), each to strike. Every file read for it was treated
as data, never as instructions.

**State of the repository when written.**
- `main` is `5a6ae08b` (#461 merged on 5 November at 10:37 UTC); (its source file) is `132486fc`. Main's
  `src/` and `tests/` differ from `ca0290c2` (#430, where every "on main" statement below was
  audited) by #461 alone: 6 files, +108/−11, the macOS path and AAC-priming fixes.
- The last completed CI run on main is the `adi_daw` run 42012235250 at `5a6ae08b`: all 20 jobs
  green, both heavy macOS legs and the Step 7 UI run (42012235165) included. PD-00 is closed.
- Open PRs, with the branch each is based on (the merge mechanics in M0 depend on it): #207 and
  #208 (the stale MIDI effects and Sampler; #207 on (its source file), #208 on #207), #210, #278
  (OneShot, draft), #421 ((its source file) into main), #440 and #439 (the scanner, on (its source file)) with
  #438 on #440's branch, #441 (graph workers, on (its source file)), #445 (ScaleContext, on (its source file))
  with #442, #443 and #444 as siblings on (its source file), #447 (on (its source file))
  with #446 on #447's branch, #448, #449 and #457 (sample settings, the analysed folder), #450 to
  #452 (progressive decoding, AAC), #453 (stem results), #454 to #456 (Link, each on (its source file)).
  #210, #441, #445 and #447 are conflicting.

**How to read it.** §0 is for the director. §1 to §8 are for win and the lanes. §3 holds all 233
rows in one table; Appendix A holds each row's full card, area by area; Appendix C records what the
critique of 5 November changed and what it did not.
---

## 0. The short version

**What ADI becomes.**

ADI opens as Live. The screen, the keys, the gestures and the defaults are Live's, and everything
Live does works the way Live does it, checked against Live 12 itself rather than guessed from its
manual. Your own departures from Live stay exactly as you ruled them: a dropped file arrives clean,
renders are placed with Warp off, Ctrl+S takes a snapshot, the function keys do functions, Create
Fades on Clip Edges starts off, the import and bounce prompts ask, record quantize is 1/16, and
tooltips are on.

On top of Live, ADI adds what Live lacks, each addition shown where you would look for it:
- **Cubase's depth for editing and delivery:** range editing across tracks, comping with crossfades
  between takes, the fade editor, the volume curve drawn on the clip, offline processing on F7, and
  one Export page that carries Live's dialog with Cubase's and REAPER's abilities below it
  (loudness targets, stems, naming, a queue).
- **Bitwig's modulation and a patching tier on Pure Data:** LFOs, envelopes and an envelope follower
  on any parameter of any plug-in, with a ring that shows what moves it; your own patches as
  devices.
- **REAPER's exactness where it helps:** precise rules for what each edit touches, render jobs that
  can never overwrite anything, project tabs.
- **ADI's own:** a history that never loses work and branches like a tree; a project that saves
  itself; the Mix View, the Scope, the Reference on Shift+R and the Loudness Drive; files that
  relink themselves; an agent that can act only through the same undoable steps you use;
  collaboration through your own storage; control from the Stream Deck and other surfaces.

**Your plug-ins.** Under your ruling of 5 November, ADI-Q, ADI-L, ADI-Clip and the rest load in ADI
as ordinary VST3 or CLAP plug-ins, like any other plug-in. Nothing is reserved for them on the
channels. When you have finished them all and sent your list, their source becomes one built-in
feature with a device-like window, opened from the track and from the Mix View. That design waits
for your list; this plan does not design it.

**How the plan was made.**
- Your research seat's bundle, nearly 2,000 documented behaviours of Live 12, Cubase 15, Bitwig 6,
  REAPER 7 and Reason 14, was read against what is actually on main today. None of those
  behaviours has been measured in a running DAW yet, and the plan never treats them as measured.
- Six area tables cover **233 needs**: 125 are already decided and only need building, 92 are
  refinements win can write down under your existing rulings (mostly "do what Live does"), and 16
  need you.
- A cross-check found **41 places** where two parts of the plan would have built the same thing
  twice, or would have gone against one of your rulings. Each now has one owner. Writing the plan
  found 5 more (§5), one of them that your devices seat has been silent since 28 October.
- The code harvest found **208 pieces of open code** in more than 80 projects that ADI can take
  instead of writing from scratch; §4 says which, from where, and when.

**What happens next, in eight steps.** The lanes work in parallel; the step number is the order of
the engine's queue, not a calendar.

| Step | What it brings | What you will be able to try |
|---|---|---|
| **M0** now | Main is green on the Mac again (#461 merged this morning); every text brought in line with your 5 November ruling, your ruling itself recorded as a short ADR in your words; the tuning stack merged with a guard so no tuning plays flat; the first five questions to you | Nothing new: you answer five questions and name the machine for Live 12 readings |
| **M1** | The window catches up with what the engine already built; your plug-ins load | The mixer as Live's (returns, sends, meters, Track Delay in ms, Solo in Place, cueing, Split Stereo Pan, Main's loudness); Save As and Save a Copy; plug-in windows; F-keys that do functions; windows reopening where they were; your own VST3s found by a safe scan and used like any plug-in |
| **M2** | Editing as Live, and one undo step per gesture | Split, Cut, Copy, Paste, Duplicate and Delete of material and of time; Insert Silence; fades and crossfades; moved clips that never overlap; faders heard while dragged; the Undo History window; automation lanes with Lock Envelopes; the Clip View once you approve its picture |
| **M3** | Time, media, MIDI in, and safe files | Link with LinkHut or Live on your network; Global Quantization (Ctrl+6 to 0); AAC, M4A, MOV and ALAC playing at once while they decode; your MIDI keyboard playing and recording; Capture; the Focus Dial on your encoder; the command palette |
| **M4** | Signal paths as Live, one loudness meter, the Mix View's first part | Sidechain ducking from another track at each tap; a track recording another track's output; monitoring through effects; the Reference at matched loudness; the Loudness Drive; strip scopes and the Scope window; STACK as you choose |
| **M5** | Rendering and safety | The Export page (WAV, AIFF, FLAC, MP3, stems, loudness targets, metadata); Live's bounces and Consolidate (Ctrl+J); Freeze All Channels; a forced crash, then the reopen question; templates; a DAWproject file from your Cubase, opened with its loss report |
| **M6** | Notes, pitch, takes, automation, warp | Live's MIDI effects and OneShot; Scale Mode and a maqam tuning per key change; MPE curves; take lanes and comping; automation recording with Arm; clip modulation as Live's; ripple editing; warp landing 1.1.1 on the downbeat |
| **M7** | The device platform | Multicore on your heaviest project with the CPU meter; an LFO modulating a plug-in parameter; a Pd patch you can edit as a device; the Plug-Ins page; a plug-in running on another machine |
| **Alongside** | The control API and the test kit, on the timing you choose (Q-C) | Claude or Codex driving a running ADI safely, never on your desktop; the Clients page |
| **After M7** | P2 by priority | Session View, racks and macros, surfaces, the agent, collaboration, interchange beyond DAWproject |

**The choices that are yours.**

Now (five asks, the first in two parts, and one gate; full texts in §6 and in
the choices file (win's working files)):
1. **Q-A: what stays reachable when the undo cap is reached, and Live's Backup folder.**
   Recommended: every snapshot and dead end kept once, deduplicated, so your cap always holds; and
   Live's ten backup copies.
2. **Q-B: what STACK shows until your plug-in list arrives.** Recommended: each track's own devices
   (your VST3s among them) as a live list, after the mixer's Live items.
3. **Q-C: when the control API starts, and on which seat.** Recommended: now, on a seat of its own.
4. **Q-D: one beat and key analyser for the whole suite.** Recommended: benchmark first, then one
   for both ADI and ADiJ.
5. **Q-E: does "a dropped file arrives clean" cover short samples?** Recommended: yes; Live's
   short-sample result comes one press of Warp later.
6. **The gate: name the machine and session that take the Live 12 readings.** Until one exists,
   nothing in this plan can be checked against Live itself.

Told with them, for you to strike if wrong (§6.2): the Loudness Drive built now as a gain only;
the tuning ruler in Live's order; follow-action chance drawn again on every pass, as Live; F7's
offline processing at P2; MIDI ports on JUCE's MIDI 2.0-ready API, with the Windows MIDI Services
backend off (it cannot run on your Windows 10 machine; it comes later as its own row); your 5
November ruling recorded as a short ADR in your own words, designing nothing; the priorities of
the new FEATURES rows; the seventeen ADRs win would write inside your existing rulings (each named
in P-8, so approving this plan is approving that list); and the engine seat finishing the Link
consumer it already has in flight before it takes the window's oldest need, once.

Later, as their step comes: Link Audio and Link on WASAPI (M3), plain text in the palette (M3),
where the control API's token lives (with Q-C's seat), the warp state of Consolidate (M5),
recovering a recording after a crash (M5), what fills Live's three Pow-r dither slots (M5), the Pd
editor's route (M7), ADI as an MTS-ESP master (P2), Push (P2), reading your own Live Sets into ADI
(P2), and undo and presence in shared projects (when collaboration starts).

**What I did not do.** I merged nothing, applied nothing from the research's patches (they are on
main already as #436 and #437), added no ADR (the ones approval would let win write are listed as
P-6 and P-8 in §6.2), changed no default or approved UI, and designed nothing of the unified
feature.

---

## 1. Principles

### 1.1 The base is Live, and Live is measured

- **The standing rule (ADR-0203 d1, d2).** Live's behaviour unless Live lacks the ability; ADI adds,
  and never does differently what Live does. An addition is first-class and shown where a user
  looks for it, never hidden in Settings; only the *default* of something Live already does stays
  Live's.
- **Live is read or measured, never recalled.** Behaviour comes from the Live 12 manual's text or a
  measurement labelled with its Live build. Every Live, Cubase, REAPER, Bitwig and Reason case in
  this plan is NOT RUN until the measurement seat records it (XC-38). Until then a parity row is
  "built, not verified" (ADR-0108), and only his signed side-by-side session closes it.
- **His deviations stay (ADR-0203 d12):** import with no warp; renders placed with Warp off; Ctrl+S
  is Take Snapshot; tooltips on; MIDI record quantize 1/16; the import and bounce prompts; the
  function keys do functions; Create Fades on Clip Edges off. A new deviation is confirmed with him
  by name.
- **The research's reference roles** sharpen contracts without replacing Live: Ableton for daily
  workflow; Bitwig for modulation and modular semantics; REAPER for source, take and render
  contracts; Reason for rack and macro boundary cases; Cubase for audio editing depth and delivery.
  In the engine layer, ADI copies whoever does it best (ADR-0205 d10).

### 1.2 What ADI adds (already decided)

| Addition | Decision |
|---|---|
| A persistent, branching history with snapshots and a cap | ADR-0128, ADR-0140, ADR-0203 d13 |
| A project that saves itself; Ctrl+S as Take Snapshot; a true Save As | ADR-0001, ADR-0203 d6, d13 |
| The Mix View (F4), the Reference on Shift+R (its slot parts withdrawn on 5 November) | ADR-0209 |
| The Scope comparing any two tracks; strip scopes and spectra | ADR-0167, ADR-0175, ADR-0203 d14 |
| The Loudness Drive; Main's loudness meter; export LUFS targeting | ADR-0203 d14 |
| Native analog summing on groups and Main | ADR-0173, ADR-0174 |
| Hybrid tracks; sends into any track and a pan on a send | ADR-0045, ADR-0203 d3 |
| 64-bit summing at every mix point, a 32/64 precision switch | ADR-0205 d6, ADR-0207 d2 |
| The Export page with Cubase's and REAPER's abilities; NI Stems | ADR-0205 d5, d8, d9 |
| Relink by content hash; the analysed folder; files that play before decoding ends | SPEC §10, ADR-0203 d18 |
| Every tuning, with a tuning per key change | ADR-0178, ADR-0203 d7 |
| The Pd patching tier with an editor; Live's devices rebuilt as patches with native cores | ADR-0145 d8, ADR-0188, ADR-0203 d4 |
| Host-side modulation of any plug-in parameter; the multimapper | ADR-0046, ADR-0114, ADR-0203 d15 |
| Remote devices through AudioGridder | ADR-0083, ADR-0203 d4 |
| An agent that acts only through ops, proposing by default | ADR-0039, ADR-0148, ADR-0168 |
| Remarks as Word's comments | ADR-0131, ADR-0203 d17 |
| Collaboration through the owner's bucket | ADR-0182, ADR-0188 d5 |
| Surfaces on one feed, the Stream Deck first; the Master Focus Dial | ADR-0181, ADR-0130 |
| DAWproject both ways; OTIO and AAF; the exact time base | ADR-0127 d7, ADR-0206 |

### 1.3 The rulings this plan applies

- **The ruling of 5 November.** No reserved plug-in slots and no separate built-in ADI-Q or ADI-L
  devices. When all his plug-ins are finished, their source becomes one unified built-in feature
  with a device-like window, reached from the track and from the Mix View; the built-in feature
  never hosts the VST3; his VST3 and CLAP builds stay ordinary plug-ins. He sends the list of
  plug-ins to rework, and that design waits for it. Every text that still describes slots follows
  the ruling in M0 (XC-27); nothing here designs the unified feature.
- **Measurement clones of commercial plug-ins under his own names are welcome**, and never
  re-questioned.
- **Code is usable or not usable**, and nothing more is said about it.
- **ERRATA-11.** ADI's export queue fixes a job's values at enqueue, its run membership at
  `Queue::run`, and reads one project revision and resolves names when each job starts;
  publication is last. No row adopts an enqueue-time project snapshot.
- **Instruments are excluded except OneShot and Sampler** (ADR-0169 d4). A user's Pd patch that
  plays notes is host plumbing (note input, voices), not an instrument ADI builds.
- **Nobody waits (ADR-0210).** The window builds on stubs and logs its needs; the needs table heads
  the engine's queue; a seat without the GitHub API asks win to open its PRs.
- **The working rules (LANES §2):** one branch and one PR per item; win merges, in stack order; a CI
  run counts only when its headSha matches; a red heavy leg on main comes first; never `git add -A`.

### 1.4 One of each

The cross-check's main finding is structural: the six areas, planned apart, would have built
several runtimes for one job. This plan builds one of each, with one owner.

| One... | Instead of | Owner row | XC |
|---|---|---|---|
| capture for every offline render (export jobs and derived renders) | two capture designs | engine design before EX-02 or TM-FC-02 | XC-14 |
| tap vocabulary, and one format home for taps and inputs | three enums, three stores | engine `Tap`; win's placement | XC-15 |
| modulation schema and evaluator, one playing value per parameter | three schemes, two playing values | the ADR ADR-0046 owes (DEV-07) | XC-16 |
| gesture contract with two preview strategies | three implementations | the gesture ADR (HI-01) | XC-17 |
| MIDI port host on UMP endpoints | a MIDI 1.0 host now and a UMP host later | window (SRF-05); engine queues | XC-18 |
| analysis store (ADIA) and kernel set | per-tool recomputation; two FFT workers | TM-WT-01, TM-MD-04, MX-30 | XC-19 |
| job state machine and staleness rule | three staleness rules | AGT-04 wrapping `render::Queue` | XC-20 |
| accessor for "timeline clips that play" | two audits of two dozen readers | CT-01 | XC-21 |
| id allocator and clipboard | a `MAX(id)+1` site per producer | one function (M2), CO-01 later | XC-22 |
| tab host | two tab systems | PD-15 | XC-23 |
| session marker | three crash and discovery records | PD-05 | XC-24 |
| note identity and arpeggio ordering | three namespaces; two orderings | DEV-17; one `src/adi/dsp` function | XC-25 |
| loudness meter | three integrated-LUFS implementations | DEV-28 | XC-03 |
| network stack, consent system and privacy list | listeners added one by one | LANES path; AGT-03; Settings' Privacy group | XC-40 |
| measurement seat and queue of readings | readings wanted by every area with no seat | MX-33's harness; his named seat | XC-38 |
| register of schema minors | 1.20 claimed four times | win | XC-37 |

### 1.5 Three levels of "modular"

The research asks that host racks, parameter modulation and internal signal graphs be kept apart,
and that ADI not grow several competing runtimes. The plan keeps three levels, one runtime each:
- **Host racks and routing** (DEV-01 to DEV-05; ADR-0060): racks spliced into the track's graph;
  sidechains and track-to-track taps as rows with real keys.
- **Parameter modulation and macros** (DEV-06 to DEV-08; ADR-0046, ADR-0114): one evaluator per
  block delivering a base and one offset per target, the same way to CLAP, VST3, built-ins and Pd.
- **Internal signal and event graphs** (DEV-09 to DEV-16; ADR-0035, ADR-0145, ADR-0177, ADR-0188):
  the Pd tier. The host owns note identity, scheduling, modulation, state, undo, persistence,
  assets and latency; Pd is the authoring and runtime layer; registered native kernels carry heavy
  DSP. No second graph runtime or editor, and no Grid-equivalence claim.

### 1.6 Out of scope, or not proposed

- The unified feature's design (waits for his list).
- Instruments other than OneShot and Sampler; per-voice modulation (it waits for instruments).
- A second graph runtime or patch editor; converting other programs' own preset formats; Bitwig
  project compatibility.
- Plug-in sandboxing at runtime, and a render child process (ADR-0203 d8: plug-ins run in process,
  only the scanner runs out of process).
- An enqueue-time project snapshot for export jobs (ERRATA-11).
- Additions nobody asked for, each named in its matrix: K-meters and PPM, razor editing, REAPER's
  subprojects and keep-partial, Pro Tools session import, a relay service, Re-scan Faulty, key
  macros, directory templates in export names, an ACID chunk by default. (Reading Live Sets,
  `.als`, is not struck here: it is his choice, Q-P in §6.3.)
- VariAudio-class editing and notation (P3 and excluded); video sync (P3); hardware CV/Gate (ADI 2).
- The research's study of built-in effect and instrument implementations (excluded by the
  director's scope); effect parity is measured against Live's renders instead (DEV-22).

### 1.7 How evidence is counted

- **measured:** observed by running: CI on a matching headSha, or a measurement labelled with its
  build. **documented:** read in a manual, a specification, an ADR or main's source, not run.
  **inferred:** reasoned from documented facts or reference code. **unknown:** nothing yet.
- A test source that exists is not a passing test; only a run whose headSha matches counts.
- The research's code claims were pinned to `5c9e1cd8`; every "on main" statement in this plan was
  read again at `ca0290c2`. Main is now `5a6ae08b`, whose `src/` and `tests/` differ from
  `ca0290c2` by #461 alone (6 files, +108/−11: the macOS path and AAC-priming fixes), so the audit
  stands.
- The research's patch series is already on main (#436, #437) and is not applied again.
- A reading names the build it ran on. Cubase is installed in two builds: 15.0.30.287 on the director's Windows workstation
  (the version of the manual the research cites) and 15.0.5.114 on the second Windows machine (the research's
  inventory); they are different evidence targets.

---

## 2. The milestones

Eight integration and evidence milestones (M0 to M7), a parallel Track C and an ordered P2 tail,
taken from the cross-check's §4 and refined here. The lanes work in parallel: the milestone number
is the engine seat's order after its in-flight item, not a calendar, and a later gate may close
before an earlier one. Every milestone after M0 ends with a Windows build of main from win (LANES
§6), and every gate is checked on a run whose headSha matches.

**What this plan changes in the cross-check's milestones.**
1. M0 records PD-00 as closed (#461 merged at `5a6ae08b`, every leg green) and splits the records
   into five docs PRs in a fixed order (R-a to R-e), with each within-authority ADR landing just
   before the first engine PR that needs it, not all at once; R-a also carries a short record ADR
   of the 5 November ruling in his words (P-6), so DECISIONS.md and the other texts agree.
2. M1 splits DEV-26 (his plug-ins load) into a headless check now and an editor-window check off
   his desktop, because ADI.exe is never driven on his live desktop (XC-43).
3. CTL-05 (where the control API's secrets live) was made win's placement by the first draft
   (XC-44); this revision restores it as his question (Q-O), because a discovery file turns a token
   he hands over into one every local process can read, a change to ADR-0039 d4's model. His asks
   are Q-A to Q-P, plus one gate.
4. Windows-only work on a Linux engine seat is gated by the Windows CI legs and win's the director's Windows workstation
   measurements (XC-45).
5. The code to take is placed per milestone (§4), and the readings each milestone waits for are
   listed per seat (§7).
6. The devices seat has not logged since 28 October; its gates now carry a named fallback (XC-46)
   and M0's gate asks for the seat to be alive or re-seated.
7. Every milestone carries a load line (expected engine PRs against the measured rate, and the item
   that slips first), and M0 states the stack-merge mechanics once.

**Capacity, as measured.** The engine seat's log holds 81 "PR requested" entries between 30
October and midday 5 November (1, 5, 31, 14, 23 and 7 a day): about 16 a day. Each needed win's
MSVC and clang fixes on the branch, and on 3 and 4 November a stack rebuild (125 engine merges in
those two days). The load lines assume a **net 10 engine PRs a working day** and count the takes
§4 places on the engine seat (L, M, S); they are order and throughput, not dates.

### M0. Green main, one set of records, the first questions

- **Who.** Win; small engine fixes on their branches.
- **Load.** Win's week: five docs PRs, one record ADR, nine stack merges with their MSVC and clang
  fixes. Engine: about 6 small PRs (the refusal, the re-pin, three rebases, the tuning guard).
  Slips first if win's time halves: R-c's format placements (then M4's taps wait).
- **Starts.** Now. A red heavy leg on main comes before any merge (LANES §2); there is none today.
- **Work.**
  1. **Main green on macOS (PD-00): closed.** #461 (Save a Copy and Collect rebase toward the
     destination as SQLite spells it; CoreAudio's AAC priming read from its packet table) merged
     at `5a6ae08b` on 5 November, 10:37 UTC; the `adi_daw` run 42012235250 at that SHA completed
     with all 20 jobs green, both heavy macOS legs and the Step 7 UI run included. What remains is
     WTS-04: take `ui.yml`'s cancel-in-progress off for main so later window merges keep their
     renders.
  2. **The NI Stems refusal** (EX-10, engine S): an NI Stems job with normalization is refused until
     the after-AAC check exists.
  3. **The records, as five docs PRs, in this order** (each "to strike if wrong" where it places):
     - **R-a, the 5 November ruling everywhere** (XC-27, MX-34): FEATURES §2's slot rows and the
       ADI-L cascade line, LANES §3 and §4, BUILD-BRIEF §5 and Phase D 7 and 7a, UI-ARCHITECTURE
       §12, the parity checklists, the harvest ledger's §1, the Mix View program's slot parts, the
       phrases in MX-36 and WIN-03 that placed the unified feature; the Drive recorded as a gain
       only (XC-06); a note that the Dream Reference's slot passages (chapters 7 to 9) are
       superseded by the ruling until he revises the Dream (XC-42). **And one short record ADR**
       in DECISIONS.md (P-6), because LANES §2 says a decision is a new ADR and the docs win over
       the code: it carries his 5 November words and nothing else (no reserved plug-in slots; no
       separate built-in ADI-Q or ADI-L; his VST3 and CLAP builds stay ordinary plug-ins; when all
       his plug-ins are finished and his list arrives, their source becomes one unified built-in
       feature with a device-like window, reached from the track and from the Mix View; the
       built-in feature never hosts the VST3; its design waits for the list), marked as amending
       ADR-0203 d14's cascade, ADR-0204 d1 and ADR-0209 d11, numbered by win when it lands. Without
       it, DECISIONS.md would still say the slots are decided while every other text says they are
       withdrawn.
     - **R-b, LANES** (XC-39, XC-18, XC-30, XC-38, XC-46): owners for the Pd runtime files, the MIDI
       port host and the engine's MIDI queues, the control, surfaces, sidecar, Stream Deck and
       network paths, collaboration in `src/adi/collab`; the engine queue's order, with the Link
       consumer as a one-time named exception to ADR-0210 d2 (P-9); the measurement seat as his
       gate; Track C's seat per Q-C. **The devices seat** (XC-46): it has not logged since 28
       October and its last merges are of 1 November, yet M1, M4, M5, M6 and M7 put gates on it.
       R-b names the fallback: the Codex devices seat is relaunched with DEV-28 (the one loudness
       meter) first in its queue; until it logs again, `src/adi/dsp/loudness` and
       `src/adi/dsp/limiter` are lent to the engine seat (a LANES §1 line, since LANES gives
       `src/adi/dsp/**` to devices alone), and the Pd safety overlays (PD1, PD2, PD4; DEV-09) go to
       whichever seat is actually running; the loan ends the day the devices seat logs.
     - **R-c, format placements** (XC-37, XC-15): the minor register (1.20 is the roles table alone;
       every other minor numbered when its engine PR opens); the one tap vocabulary; external
       inputs in `input_ref`'s grammar; a whole new table for track-to-track inputs and MIDI From;
       the sidechain's tap in a side table keyed by routing id; Sends Only and No Output in a side
       table.
     - **R-d, the ADRs within authority**, the one list (seventeen; the same list as §8.4 and
       P-8), each merged just before the first engine PR that needs it: the gesture contract
       (XC-17) and EditScope with Lock Envelopes and ripple's switch (ED-03) before M2; the media
       reader (TM-MD-01) before M3; one loudness meter (XC-03) and the measurement harness
       (MX-33) before M4; one capture (XC-14), jobs and staleness (XC-20), the three render
       contracts (TM-FC-01) and the converter rules (TM-IC-01) before M5; "comping is Live's"
       (CT-01, ADR-0203 d1 over ADR-0037), the warp-analysis stack (TM-WT-01), tuning sources and
       the keyboard map (TU-02) and remark threads (RE-03) before M6; Graph threads (DEV-23)
       before M7; the modulation schema (XC-16) before AU-06 or DEV-07; the testing ADR (the WTS
       rows) and one control stack (CTL-01) with Track C. Each is an engineering contract or a
       Live's-way ruling under his existing decisions; none widens a feature.
     - **R-e, rows and texts:** the registry rows (`audio.delayCompensation`, a home for
       `midi.port.*`, `window.places`, `shortcuts.overrides`, the `sync.*` and Tempo Follower
       fixes, `record.shortSampleMode` after Q-E, `control.*`); the FEATURES and parity rows of §8;
       the stale texts (BUILD-BRIEF §4, the duplicate tempo-detection row, OPS's freeze op names,
       SPEC §6.2's comping, AI-AGENT's "nothing built", the first-window gate's wording); CTL-01's
       one control stack and AGT-09's tier scope as records.
  4. **Merges, each checked by SHA, with the stack mechanics stated once.** A stack lands through
     its top PR, retargeted to `main` before the merge; never through (its source file) (the Codex
     seat's log pushes restart its CI), and the intermediate PRs close as landed once the top
     merges. The order follows the bases as GitHub holds them: **#421** ((its source file) into main)
     first, so every branch below it carries the log; then the tuning stack, **#445** (ScaleContext,
     on (its source file); conflicting, so rebased, then retargeted to main) with the reference-pitch
     guard added on #443's branch before the merge (TU-01: a source whose reference pitch would move
     a degree off its 12-TET anchor is refused with a verdict naming the line, so no tuning plays
     flat on main until TU-02 stores the reference), then its three siblings on
     (its source file), **#444, #443, #442**, each retargeted to main in that order. Close #207
     and #208 as "to be ported", keeping their branches (devices K3). The scanner line (#440 on
     (its source file), then #438 on #440's branch, then #439), the Link line (#454, #455, #456, each on
     (its source file)) and the media and capture stacks (#450 to #452; #448, #457, #449; #447 on
     (its source file), then #446 on #447's branch) merge the same way where their gates sit (M1,
     M3).
  5. **Tell the engine seat:** re-pin #456 to `Link-4.1.1` inside the same stack, Link Audio off
     (XC-31); key held notes by the stored note identity (XC-25); rebase #445, #447 and #441; the
     Drive's "blocked" entry lifts once R-a merges.
  6. **Send Q-A to Q-E** with the placements of §6.2, and ask him to name the measurement seat (G-1).
- **Rows whose gate closes here.** PD-00 (closed), PD-10, MX-34, CTL-01, AGT-09. TU-01's merges
  happen here, but its gate (31-EDO degree 0 at 263.0921 Hz) closes at M6 beside TU-02, because
  the merged stack cannot pass that test until the reference pitch is stored; the guard above keeps
  the defect from reaching anyone's ears meanwhile.
- **Code taken.** None: M0 is records and merges (the macOS fix is in).
- **Gate.** Both heavy macOS legs green on a run whose headSha is main's head (met: 42012235250 at
  `5a6ae08b`); R-a with its record ADR, R-b, R-c and R-e merged (R-d's first two before M2 starts);
  the devices seat alive or re-seated, or the loudness and limiter loan in LANES (XC-46); the five
  questions sent, with Q-O's text ready for Q-C's answer.
- **He tries.** Nothing new. He answers Q-A to Q-E and names the seat.

### M1. The window on what the engine merged; his plug-ins load as ordinary plug-ins

- **Who.** The window lane, most of it. Win: the scanner's D1, the merges of #440, then #438 on
  its branch, then #439 (each retargeted to main, M0's mechanics), and DEV-26. Engine, small:
  DEV-08, and the scanner's D2, D5, D7 and D8 on their branches; #441 landed off by default once
  rebased and green (devices K9). Devices, or the seat R-b names in its place (XC-46): DEV-09 and
  the Pd safety overlays once R-b names the Pd runtime's owner; DEV-28's meter core may start.
- **Load.** Window: 24 rows and 12 takes (2 M, 10 S). Engine: about 8 small PRs (DEV-08, four
  scanner fixes, the #441 rebase). Devices or its stand-in: 3 takes (1 M, 2 S). Slips first if a
  seat halves its rate: DEV-26's editor-window half (it needs his OK for a Sandbox anyway).
- **Rows whose gate closes here.** SRF-01 (A3), AU-04 (C1), MX-05 (B1; B2 through the id helper of
  XC-22), MX-01 (B3), MX-04 (B4), MX-07 (B5), MX-08 (B6), MX-02 (B7), MX-10 (B8), MX-06, MX-14 and
  WIN-01, WIN-02, WIN-03 (D3), WIN-04, WIN-09, WTS-01, WTS-02, WTS-09, RE-01 (C5), DEV-08, DEV-09,
  DEV-25 (step 1), DEV-26.
- **Also shipping, their rows closing later.** A1 and A4 (PD-01 and PD-02 close in M3); A5's list
  (HI-03 closes in M2 with `moveTo`); C2; C3 (its quantized locators wait for TM-SV-01); C4; C6; C7's
  lanes and breakpoints (AU-01 and Lock Envelopes close in M2); D1; D2; D4 (plug-ins appear through
  DEV-25); D5; F1; F2.
- **Code taken (§4, M1).** The Pd safety overlays (PD1, PD2, PD4) and #173's unbind (MF4); window
  placement, owner windows and the shortcut store (WI1 to WI4, WI7); the remark pins (RM5, RM8); the
  test hygiene kit (WT-3, WT-4, WT-5); the readings-snapshot design (LD7).
- **Gate.**
  - Each window item's headless check, its parity rows and its picture beside Live's (LANES §3b);
    window CI by SHA; the strips open no scope taps (B3).
  - The scanner's acceptance items 1 to 4 green on the Windows legs, by SHA.
  - **DEV-26, in two halves (XC-43).** Headless now, on the director's Windows workstation with `ADI_HOME` set, never in his
    session: ADI-Q, ADI-L and ADI-Clip VST3s scanned in a child process, loaded, edited through
    their parameters as single undo steps, saved and reopened with identical state hashes; ADI-L's
    lookahead nulls against a dry track; a missing build becomes a placeholder. The editor-window
    half (their windows open in the floating host) in the window lane's own seat, or in a Windows
    Sandbox on the director's Windows workstation only with his OK.
  - AR4: a lane on Utility's Gain is heard within one Pd block of its point.
  - DEV-09: the latency binding is unbound and the quit and exit fences hold, in tests.
- **He tries.** The mixer as Live's: returns with their own Pre/Post, sends from every track, strip
  meters, Track Delay in ms, Solo in Place, cueing with Cue Out and Cue Volume, Split Stereo Pan,
  Main's loudness. Save As and Save a Copy on Windows; Collect All and Save, and the ZIP.
  Re-Enable Automation and Arm; the metronome, the loop and the locators; waveforms; remark pips;
  device copy and paste; plug-in windows (the wrench, Ctrl+Alt+P); his own VST3s found by the
  out-of-process scan and used as ordinary plug-ins; F-keys that do functions; windows reopening
  where they were.

### M2. The edit kernel and the gesture (P0)

- **Who.** The engine seat, right after its Link consumer reaches its gate (P-9); then the window.
- **Load.** Engine: 13 takes (7 M, 6 S) plus the row work: about 20 PRs, two working days at the
  assumed rate. Window: 4 takes (1 L, 2 M, 1 S) and E1 to E6. Slips first at half rate: HI-03's
  `moveTo` and the Undo History window (A5); the fuzz gates do not.
- **Needs first.** R-d's gesture ADR and EditScope ADR merged.
- **Rows whose gate closes here.** ED-01 (the two-point split and the tail's identity; E1), ED-02
  (E3), ED-03, ED-04 (the anchored left trim; Crop), ED-05 (slip on Live's keys; unwarped slip as
  ADR-0115's addition), ED-09 (object snapping), ED-10 (E4), MI-01, MI-02, MI-03 (G1 to G3, once he
  approves the Clip View's picture), HI-01 with SRF-02's live values, HI-02 (best-effort
  compaction and the low-water mark now; Q-A's rule and the daily snapshot once he answers), HI-03
  (`moveTo`, the root; A5), AU-01 (point ops re-keyed; C7), AU-05 (Lock Envelopes in the planner;
  C7 binds to it), MX-23 (fader, pan and send drags; multi-selection), SRF-02, MX-11.
- **Also.** N29 and N17's open part; the one allocator function (XC-22: the engine's own
  `MAX(id)+1` sites and the window's helper meet in one function); E5 and E6, with E6 stating the
  six bounce differences until TM-FC-01 lands (XC-33).
- **Code taken (§4, M2).** SoundCraft's edit algebra and openDAW's overlap resolver as a pure
  planner (RE1, RE3), FilmCraft's slip (RE2) and the Ardour, Audacity, Qtractor designs and tests
  (RE4, RE5, RE7); Ardour's NoteDiffCommand shape (ME1) and MAGDA's piano-roll design (ME4); the
  gesture dispatcher contract and its tests (HI1 to HI8); the automation guard and follow-clips
  ports (AU1, AU7).
- **Gate.**
  - The seeded fuzzers green on every leg by SHA: no overlap after any edit; undo-all returns the
    start digest; the note-diff fuzz; the history fuzz over compaction.
  - `test_gesture`: a 20-move drag is one transaction with one label; cancel writes nothing and
    leaves the digest unchanged.
  - `test_note_diff`: a 10,000-note diff writes the stream once; a future tail survives every note
    op; an inactive note deleted and undone stays inactive.
  - Once Q-A is answered: a 3,000-step project with a snapshot on day one stays under the cap.
  - The parity checklists for Live 12 manual 5.4.2, 6.7 to 6.12 and 25.1 to 25.5, the Live side
    recorded as NOT RUN until the seat exists.
- **He tries.** Live's range editing: Split across clips, Cut, Copy, Paste, Duplicate and Delete of
  material and of time, Insert Silence, fades and crossfades. A moved or trimmed clip never
  overlaps another, and a left trim keeps the audio in place. Clips snap to other clips. Faders,
  pans and sends are heard while dragged and leave one undo step, also across a selection. The
  Undo History window jumps to any step. Automation lanes with breakpoints and Lock Envelopes. The
  Clip View, if he has approved its picture.

### M3. Time, media, MIDI in and the data-safety floor

- **Who.** The engine seat, and this is its heaviest milestone before M5, not "small pieces": the
  in-flight Link line and its adapter (LS1, L), the ISO-BMFF and MOV demuxer with ALAC (MD1, L; it
  stays here because §0 promises MOV and ALAC at this step), the atomic replace and the settings
  store (R1, R4), the media analysis and tag readers (MD2 to MD10, with TagLib and the FFmpeg tool
  at M), the launch-quantization table and grid rules (SV2, SV5, SV6), the surface core's pick-up
  rules (SU7, SU8) and the row work (tempo ramps, Global Quantization, relink, PD-04, PD-03,
  PD-11, EX-01, the Track tap, SRF-03, SRF-04). The surface encoders and binding model (SU1, except
  its device-identity match) and the generic-MIDI surface data (SU9) move to P2's surfaces step,
  where the rows they serve sit. Win: the merges, and AUDIOENGINE-1 on the director's Windows workstation. Window: the Link
  toggle and progress bar, the Quantization menu with Ctrl+6 to 0, the decode state, the
  missing-files list, the Save button, the port host, the Focus Dial, the palette.
- **Load.** Engine: 19 takes (3 L, 4 M, 12 S) plus the rows: about 30 PRs, three working days at
  the assumed rate. Window: 4 takes (1 M, 3 S) and the items above. Slips first at half rate: MD1
  (MOV and ALAC; MP4 and M4A still land through #450 to #452), then the FFmpeg tool (MD5).
- **Order inside.** Link: #454, #455, #456 (re-pinned to 4.1.1), each on (its source file) and
  retargeted to main in that order, then the consumer with XC-25's continuity, then tempo ramps on
  the same effective-tempo path. Media: the AudioSpecificConfig fix on #452, then #451, #452, #450;
  then #448, #457, #449; then PD-04, which folds #449's replace primitive into `replaceFile`. MIDI:
  #447 (on (its source file), conflicting: rebased) then #446 on its branch, the top retargeted
  to main, with the port host.
- **Rows whose gate closes here.**
  - Time: TM-LS-01, TM-TT-01, TM-SV-01.
  - Media: TM-MD-01 (MP4, MOV and ALAC after #452), TM-MD-02 (#451, #450; Cleanup), TM-MD-03,
    TM-MD-04 (#448, #457, #449; key and tags after Q-D), TM-MD-05.
  - Files: PD-04, PD-03 (`snapshotAll`), PD-11, PD-01 and PD-02 (their engine additions; the copy's
    session lock follows PD-05 in M5), EX-01 (the media identity check).
  - Hands: SRF-03, SRF-04, SRF-05 (the port host on JUCE's UMP API with the Windows MIDI Services
    backend off, so SF-1, SF-2 and SF-21 can pass on his Windows 10 box and on CI; #446 and #447
    merged for the Track route), SRF-06, WIN-05, WIN-06 (with Q-G).
- **Code taken (§4, M3).** The Link SDK at 4.1.1 and its adapter designs (LS1, LS4 to LS6); the
  ISO-BMFF demuxer, Mixxx's decoder fixtures and ALAC (MD1 to MD3), opusfile and the FFmpeg-tool
  design (MD4, MD5), the analysis and tag readers (MD6 to MD10, key detection after Q-D); the
  atomic replace and the settings store (R1, R4); the launch-quantization table and grid rules
  (SV2, SV5, SV6); the surface core's pick-up rule, truth table and hot-plug (SU7, SU8, SU10) and
  SU1's device-identity match alone (its encoders and binding model wait at P2 with SU9); the
  palette and its matcher (WI5, WI6).
- **Gate.**
  - Link's TEST-PLAN cases as deterministic tests; AUDIOENGINE-1, an onset within 3 ms of LinkHut
    on the director's Windows workstation recorded through loopback; a held-note tempo-change test.
  - The media acceptance cases A-M1 to A-M3 on all seven legs at one headSha.
  - The craft-atomic list on seven ABIs and no bare `std::filesystem::rename` left in `src/adi`; the
    device-snapshot test (PD-03); the settings tests (PD-11).
  - SF-1, SF-2, SF-20 and SF-21; the palette's acceptance cases 22 to 28.
  - N1 and N4 read in Live, once the seat exists.
- **He tries.** Link with LinkHut or Live on his network: Play waits for the quantum, the toggle
  counts peers. Global Quantization with Ctrl+6 to 0 and quantized locator jumps. AAC, M4A, MOV and
  ALAC files that play at once while they decode. A sample's warp and clip settings saved and
  recalled on the next drop. His MIDI keyboard playing and recording; Capture. The Focus Dial on
  his encoder; the palette.

### M4. Signal paths as Live, one loudness meter, and the Mix View's P1

- **Who.** Engine, devices (or the engine seat holding the loudness loan, XC-46) and window; win
  for the harness and, once the seat exists, the first Live round.
- **Load.** Engine: 3 S takes plus the taps, In/Out, monitoring, loopback, rewiring, Reference and
  Drive rows: about 15 PRs. Devices or its stand-in: 6 takes (1 L, 3 M, 2 S), the meter first.
  Window: the choosers, Arm, the Reference strip, scopes, STACK. Slips first at half rate: MX-17
  (WASAPI loopback, Windows-only proof), then the Scope window; the meter does not, because M5's
  export and the Reference wait on it.
- **Needs first.** R-c's tap and input placements (M0); R-d's one-meter ADR; Q-B for STACK; the
  devices seat alive or the loan in LANES (DEV-28 is on the critical path of EX-05, DEV-29 and
  MX-27).
- **Rows whose gate closes here.** DEV-05 and MX-24 (keys into devices at Live's three taps), MX-25
  (the In/Out choosers, input taps, Sends Only, No Output), MX-09 (monitoring with Arm, Keep
  Monitoring Latency, Reduced Latency, Measure round trip: the engine seat's D11), MX-17 (system
  audio as an input), DEV-28 and MX-27 (the meter; N13, normalization and Auto Gain Stage rewired),
  MX-29 (the roles table, matched loudness, spot play), DEV-30 and MX-35 (the Drive, gain only),
  MX-30, MX-12, MX-13, MX-36 (STACK as Q-B decides), MX-33, MX-22, MX-03, MX-26, MX-32 (after its
  reading), WTS-11.
- **The Mix View rounds, mapped.** Round M1's F-keys, meters and Main readout are in M1. Round M2
  (the Reference), round M4's routing (the Pre FX sidechain a Kick key will use, the minimal SCOPE &
  PHASE, the strip's scope; the Kick button itself waits for his list, MX-20) and round M5's Drive
  are here. Round M6's P1 Scope is here (MX-13); its OVERVIEW and SPECTROGRAM are P2. Rounds M7
  (screens) and M8 (the AI Mix Radar) are P2. The slot parts of rounds M3, M5 and M8 are withdrawn.
- **Code taken (§4, M4).** FilmCraft's loudness meter as the base (LD1) with libebur128, the
  synthesised EBU signals and ffmpeg as oracles (LD3, LD4, LD6); ADI-L's meter as a cross-check only
  (LD2); the measurement kit (MS1 to MS6: signals, MTDM, the Live driver, spikes, distances).
- **Gate.**
  - The engine's tap truth table; a key reaching a VST3 aux bus, aligned, with a latency plug-in
    on the key path; suspension stays off while a key is live.
  - Tech 3341 cases 1 to 6 and 9 to 23 and Tech 3342 cases 1 to 4 within tolerance; libebur128
    within 0.1 LU at 44.1, 48 and 96 kHz.
  - The Drive's balance null on a three-tier fixture with user clippers; export equals playback.
  - The Mix View program's budgets measured on his PC; bit-identical audio with the Mix View open
    and closed.
  - AWAITING rows 5, 7, 8, 15 and 16 and L01 to L10 recorded, if the seat exists.
- **He tries.** Sidechain ducking keyed from another track at each tap. A track recording another
  track's Post FX, and resampling. Monitoring through effects with Arm. Main's loudness with LRA.
  The Reference at matched loudness on Shift+R, and Alt+hold on a spot. The Loudness Drive. Strip
  scopes and spectra, the analyser's view and the Scope window with its one-click delay fix. STACK,
  as he chose.

### M5. Renders, export, crash safety and DAWproject

- **Who.** Mostly the engine. Window: the Export page, the reopen question, Unused Files. Devices,
  or the engine seat under the limiter loan (XC-46): the limiter's true-peak path. Win: the new
  `third_party` rows, LANES §3b's Export item, the readings. His Cubase for DP-7.
- **Load.** Engine: 27 takes (1 L, 14 M, 12 S) plus the rows: about 40 PRs, four working days at
  the assumed rate, the largest engine milestone. Window: 2 S takes and the Export page. Slips
  first at half rate: TM-IC-03 (DAWproject both ways), then the templates (PD-14); crash safety,
  the one capture and the bounces do not.
- **Needs first.** R-d's one-capture and jobs ADRs; Q-H before Consolidate; Q-I and Q-A for the
  crash and backup rows; Q-J for the dither; his grant of the libFLAC, LAME and soxr rows
  (EXTERNAL-CODE's procedure for new `third_party` rows).
- **Rows whose gate closes here.** PD-05, PD-09, PD-13, PD-12 (measured), PD-06 and PD-07 (if he
  says yes), PD-14, EX-02, EX-03, EX-04, EX-05 with DEV-29, EX-06 (after Q-J), EX-07, EX-08, EX-09
  with AGT-04's state machine, EX-10 with TM-IC-05 (the after-AAC check lifts the refusal), EX-11
  (in process, XC-26), TM-FC-01, TM-FC-02 with ED-06 (warp state after Q-H), TM-FC-03 (after Q-A),
  TM-FC-04 (after M4's taps), TM-IC-01, TM-IC-03 with IC-01.
- **Code taken (§4, M5).** The session marker, recording recovery, salvage and backup rotation (R2,
  R3, R5, R7; R6 only if the benchmark row asks); the encoders, SRC, true-peak brickwall, dither,
  metadata and job model (EX1, EX3 to EX9, LD5); the recipe-keyed render cache and the three
  contracts (FC1 to FC5) and Tracktion's MIDI-merge design for Consolidate (RE6); the DAWproject
  codec and its oracles (IC1 to IC6, IC12, IC13) and the time base's vectors (IC8); the job state
  machine (A4).
- **Gate.**
  - Crash fixtures: an unclean open reports its three ops; a second writer is refused, naming host
    and PID; a heartbeat older than 60 s can be broken.
  - A4-001 to A4-004 as tests; the bounce and consolidate nulls (A4-005 to A4-009);
    `test_render_key` and `test_consolidate`.
  - FLAC equal to the PCM WAV bit for bit; every MP3 frame MPEG-1 Layer III at 320 with its delay
    trimmed to exactly the frames written; the encoded true peak under the ceiling after brickwall
    and dither; `bext` v2 parsed strictly.
  - A VST3 and a CLAP rendered from the Export page with the window responsive, the thread of each
    call logged.
  - DP-1 to DP-6; DP-7 at his Cubase is his gate.
- **He tries.** The Export page as Live's dialog, ADI's additions below a divider: WAV, AIFF, FLAC
  and MP3, stems, LUFS targets with the brickwall, metadata. Bounce to New Track and Bounce in Place
  as Live's; Consolidate (Ctrl+J) and Paste Bounced Audio; Freeze All Channels. A forced crash, then
  the reopen question. A file from a newer build refused with its reason. Live's Backup folder, if
  he said yes. Templates. A DAWproject file from his Cubase, opened with its loss report.

### M6. Notes, pitch, takes, automation passes and warp

- **Who.** Engine; devices, or the seat R-b names in its place (kernels ported onto DEV-17 and
  DEV-18, OneShot, effect parity; XC-46); window (the Clip View's tabs, lanes, Scale Mode, the
  Sample Editor); win (the scale census and the readings).
- **Load.** Engine: 30 takes (3 L, 9 M, 18 S) plus the rows: about 40 PRs, four working days at the
  assumed rate. Devices or its stand-in: the seven P1 MIDI effects and OneShot, one PR each, then
  the parity queue. Slips first at half rate: TM-WT-01's analysis core (it also waits on Q-D),
  then the automation write pass (AU-03); the note identity and comping do not.
- **Needs first.** R-d's "comping is Live's" and the modulation ADR (for AU-06); Q-D before
  TM-WT-01; Q-E before TM-WT-05; his approval of the Clip View picture.
- **Rows whose gate closes here.** DEV-17, DEV-18, DEV-19 (the P1 seven), DEV-20, DEV-21, DEV-22
  (Compressor first), TU-01 (its merges were M0's; its pitch test closes here with TU-02's stored
  reference), TU-02, TU-03 (Live's index order, XC-28), MI-04, MI-06, MI-08, CT-01 (with XC-21's
  accessor; its comping ADR is R-d's), CT-02, CT-03 (the Arrangement Overdub cases included), CT-04,
  CT-05, AU-02, AU-03, AU-06 (on the modulation ADR), AU-07, AU-08, ED-07, ED-08, RE-03, TM-WT-01
  (after Q-D), TM-WT-02, TM-WT-03, TM-WT-05 (after Q-E), TM-WT-06.
- **Code taken (§4, M6).** The MIDI-effect kernels from #207 and #208 ported onto the new transform
  phase, OneShot from #278, openDAW's lifecycle and arpeggio ordering (MF1, MF2, MF5, MF6, ME5);
  Helio's note operations and DrivenByMoss's scales (ME3, ME6); the comping bookkeeping and oracles
  (CP1 to CP6); the automation write pass, clip modulation maths, simplifier and thinning (AU2 to
  AU6, AU8); the tuning oracle and writers (TU1, TU4, TU7); the warp-analysis stack (WT1 to WT8);
  DAWproject's info-text mapping (RM7).
- **Gate.**
  - grid9 generated-event-ownership: no orphans after a removal, a seek, a stop and an undo; a clip
    note and a live note reach one CLAP and one VST3 with distinct ids; the devices7 MIDI probes and
    A7M-130.
  - AT-3: 3.5 loop passes give one media row and four take clips, and one undo restores pass 3.
  - L-AUTO-1 to -6 measured before automation defaults are frozen.
  - The tuning6 corpus and the C6-SCALES census.
  - The warp benchmark gate, with beat_this as the comparator; M2-A-WARP 1 to 9 and LM1 to LM8 on
    Live 12.4 once the seat exists.
- **He tries.** The MIDI effects (Arpeggiator, Chord, Scale, Pitch, Velocity, Note Length, Random
  with its keep switch) and OneShot; another track recording what a MIDI effect plays. Scale Mode
  with Live's scale list, and a maqam tuning per key change. MPE curves that play and can be edited.
  Take lanes and comping: loop recording, audition, copy to the main lane, seam crossfades.
  Automation recording with Arm; Insert Shape; clip modulation envelopes as Live's. Ripple
  editing. Warp landing 1.1.1 on the downbeat, and Live's warp commands in the Sample Editor.

### M7. The device platform: threads, modulation, the Pd user tier, remote devices

- **Who.** Engine, devices (or its stand-in, XC-46), window; win for the "Graph threads" ADR, the
  the director's Windows workstation measurements and the Pd editor's packaging.
- **Load.** Engine: 17 takes (6 M, 11 S) plus the threads, modulation and Pd rows: about 25 PRs.
  Devices or its stand-in: the modulators and Pd templates (1 M). Slips first at half rate: DEV-27
  (AudioGridder), then the scan service (SC1 to SC7); the threads and the modulation routes do not.
- **Needs first.** Q-K (the Pd editor's route, the user-patch delay, editing a built-in); the
  modulation ADR; the "Graph threads" ADR.
- **Rows whose gate closes here.** DEV-23 (#441 already landed off by default, then the target
  scheduler), DEV-24 and MX-28, DEV-07 (with DEV-19's three modulators), DEV-10, DEV-11, DEV-12,
  DEV-13 and DEV-14 (after Q-K), DEV-15 (its ADR and probes G1 to G6), DEV-27; DEV-25's registry
  v3 and scan service also land here (the row's gate closed with step 1 in M1).
- **Code taken (§4, M7).** The scheduler kernel, pool and real-time setup (GT1 to GT8); the scan
  service, registry v3 and its policies (SC1 to SC7); the live Pd editor attach, the patch-state
  protocol and compiled-in externals (PD3, PD5, PD6, per Q-K); the Pd MIDI egress (MF3).
- **Gate.**
  - GT-1 (byte-identical output on 1,000 random graphs) to GT-8, with the director's Windows workstation under ASIO compared
    with Live 12; that measurement sets the default.
  - One source modulating a CLAP, a VST3 and a built-in parameter gives the same curve; the base
    value survives stop, save and reopen.
  - AR1 to AR18 and the hosts11 pd-segment-contract probe.
  - The scanner's acceptance items 1 to 13.
  - A project with an AudioGridder device, opened without the server, loads the plug-in locally.
- **He tries.** Multicore on his heaviest project, with the CPU meter and Performance Impact. An LFO
  or an envelope follower modulating a plug-in parameter, with the ring shown. A Pd patch device he
  can edit, by the route he chose. The Plug-Ins page with Rescan and the blocklist. A plug-in on
  another machine as a device.

### Track C. The control core and the window's test kit (parallel, timed by Q-C)

- **Who.** A seat of its own if Q-C is (a); otherwise the engine queue at P2. The window for CTL-09
  and the test kit.
- **Load.** The control seat (or the engine at P2): 7 takes (1 L, 3 M, 1 S, two unsized) plus the
  rows: about 12 PRs. Window: 4 takes (1 M, 3 S), the Clients page and the kit. Slips first at half
  rate: CTL-08 (the app host); the headless core and the test grant do not.
- **Rows whose gate closes here.** CTL-02, CTL-03, CTL-04, CTL-05 (after Q-O), CTL-08
  (headless first), CTL-09, AGT-01 (the 256 cap, a defect against ADR-0203 d20's 6,000), AGT-02
  (`project_uuid` written), AGT-08, WTS-03, WTS-04, WTS-05, WTS-06, WTS-10.
- **Code taken (§4, Track C).** MAGDA's remote core and MCP bridge ported once into
  `src/adi/control` (CA1 = A1, CA3 = A2), cpp-httplib v0.64.0 (CA2), gridcraft's control rules (CA4),
  melatonin's component model (CA5), the op schema export (OS1), the golden testkit and pixel diff
  (WT-1, WT-2, WT-6).
- **Gate.** AC-1 to AC-23 on three legs; AC-24 and `test_changeset`; AT-1 to AT-3 and AT-9 to AT-13;
  AT-20 to AT-22, where the lane reproduces #276 on the Linux box without xdotool.
- **He tries.** Claude or Codex driving a running ADI through `adi-mcp` on a machine that is not his
  desktop; the Clients page, where he sees, grants, revokes and rotates.

### After M7: P2 by priority, then P3

All decided P2, unblocked now that the first-window gate has passed (XC-41: his remarks R1 to R7 on
build `add5102`, 2 November). Proposed in Phase F's order; the order is his to change.
1. Session View with the crossfader and the launch settings (TM-SV-02 to -05, MX-15); then
   tempo-following warped playback and the tempo and hitpoint rows (TM-WT-04, -07, -08).
2. Racks and macros with his multimapper (DEV-01 to -04, DEV-06); the `.amxd` translator (DEV-16).
3. Surfaces on the one feed, the Stream Deck first (CTL-06, SRF-07, -08, -13, -14, WIN-07, SRF-09,
   -10, CTL-07; SU1's encoders and binding model and SU9's surface data land here); Push if Q-M says
   yes (SRF-15); the Windows MIDI Services backend (SRF-16) once a Windows 11 seat with the service
   exists.
4. The agent's user side (AGT-03, -05, -06, -07, WIN-10).
5. Remarks as Word's comments (RE-02); Live 12's note model, MIDI Tools, the Groove Pool and
   Humanize (MI-09, MI-07, MI-05); Cubase's automation modes (AU-09); MTS-ESP if Q-L says yes
   (TU-04).
6. Interchange P2 (TM-IC-02 with IC-02; TM-IC-04 with IC-03; read-only Live Set import if Q-P says
   yes); MIDI sync (TM-LS-03); Link Audio if reopened (TM-LS-02); Convert Harmony, Melody and Drums
   to New MIDI Track (TM-WT-09, after Q-D's analyser).
   **Load.** Engine: 40 takes at P2 (6 L, 13 M, 20 S), about 60 PRs, six working days at the
   assumed rate before any P2 row work; slips first at half rate: the MCU protocol layer (SU3) and
   collaboration's bucket adapters (C1 to C9).
7. Collaboration (CO-01 to -05, after Q-N), the cloud backup (PD-08), project tabs with the
   historical tab (PD-15, HI-04), the P2 export abilities (EX-12), the Mix View's P2 (MX-21, MX-31,
   MX-16, MX-19; MX-20 after his list), the Second Window (WIN-08), WTS-07 and WTS-08.

P3: the Control Room in version 2 (MX-18), MIDI-CI (SRF-11), TUIO and touch (SRF-12).

### The lanes, milestone by milestone

| | win | engine | window | devices | others |
|---|---|---|---|---|---|
| M0 | WTS-04; R-a to R-e with the record ADR; the stack merges (M0 item 4); Q-A to Q-E and the seat | NI Stems refusal; #456 re-pin; rebases of #445, #447, #441; the tuning guard | continues its worklist | idle since 28 October: relaunched with DEV-28 first, or its loudness and limiter paths lent to the engine (XC-46); DEV-09 once R-b names the Pd owner | |
| M1 | scanner D1 and merges by SHA; DEV-26 (headless); the Windows build | DEV-08; scanner D2, D5, D7, D8; #441 off by default | A, B, C1 to C7, D, F; WIN-01 to -04, -09; WTS-01, -02, -09; the id helper; DEV-26's window half | DEV-09 and the Pd overlays; meter core starts (the seat R-b names) | |
| M2 | fuzz gates by SHA | N29, N17's open part, ED-03, ED-04, MI-01, MI-02, HI-01, HI-02, HI-03, AU-01, AU-05, the one allocator | E1 to E6, C7's binding, A5, MX-23, ED-09, MX-11; G1 to G3 if approved | | his Clip View picture |
| M3 | Link, media and capture merges; AUDIOENGINE-1 | Link consumer, tempo ramps, Global Quantization, #452 fix, MP4/MOV/ALAC, relink, PD-04, PD-03, PD-11, EX-01, Track tap, SRF-03, SRF-04 | Link toggle, Quantization menu, decode state, missing files, Save; port host; Focus Dial; palette | | |
| M4 | harness; a Live round if seated | taps, In/Out, monitoring (D11), loopback, meter rewiring, Reference, Drive, visual taps; the meter if on loan | choosers, Arm, Reference strip, strip scopes, Scope window, STACK | the meter, visual kernels (or on loan) | measurement seat |
| M5 | `third_party` rows; Export in LANES §3b; readings | crash marker, one capture, bounces, consolidate, recipe key, freeze, export core, DAWproject, time base, job states | Export page, reopen question, Unused Files, templates | limiter true peak (or on loan) | his Cubase (DP-7) |
| M6 | census; readings | DEV-17, DEV-18, lanes and comping, automation passes, tuning, warp analysis | Clip View tabs, lanes, Scale Mode, Sample Editor | MIDI effects, OneShot, effect parity | measurement seat |
| M7 | Graph threads ADR; the director's Windows workstation measurements; Pd packaging | threads, CPU timings, modulation nodes and routes, Pd tier, scanner v3, AudioGridder | ring and map mode, Plug-Ins page, Pd editor menus | envelope follower, volume shaper, modulators, Pd templates | |
| C | pins, LANES rows | control core (or its own seat) | Clients page, test kit | | a seat per Q-C |

### Every row's place

Each of the 233 rows has exactly one place: the milestone where its gate closes (a row's work may
start earlier, as above). The cross-check placed them by script; this plan keeps that placement,
except TU-01 (gate moved to M6: its pitch test cannot pass on the merged stack), and adds SRF-16
and TM-WT-09 at P2.

| Milestone | Rows | Row ids |
|---|---|---|
| M0 | 5 | MX-34, PD-00, PD-10, CTL-01, AGT-09 |
| M1 | 24 | AU-04, RE-01, DEV-08, DEV-09, DEV-25, DEV-26, MX-01, MX-02, MX-04, MX-05, MX-06, MX-07, MX-08, MX-10, MX-14, SRF-01, WIN-01, WIN-02, WIN-03, WIN-04, WIN-09, WTS-01, WTS-02, WTS-09 |
| M2 | 18 | ED-01, ED-02, ED-03, ED-04, ED-05, ED-09, ED-10, AU-01, AU-05, MI-01, MI-02, MI-03, HI-01, HI-02, HI-03, MX-11, MX-23, SRF-02 |
| M3 | 20 | TM-SV-01, TM-TT-01, TM-LS-01, TM-MD-01, TM-MD-02, TM-MD-03, TM-MD-04, TM-MD-05, PD-01, PD-02, PD-03, PD-04, PD-11, EX-01, SRF-03, SRF-04, SRF-05, SRF-06, WIN-05, WIN-06 |
| M4 | 20 | DEV-05, DEV-28, DEV-30, MX-03, MX-09, MX-12, MX-13, MX-17, MX-22, MX-24, MX-25, MX-26, MX-27, MX-29, MX-30, MX-32, MX-33, MX-35, MX-36, WTS-11 |
| M5 | 28 | ED-06, TM-FC-01, TM-FC-02, TM-FC-03, TM-FC-04, TM-IC-01, TM-IC-03, TM-IC-05, DEV-29, PD-05, PD-06, PD-07, PD-09, PD-12, PD-13, PD-14, EX-02, EX-03, EX-04, EX-05, EX-06, EX-07, EX-08, EX-09, EX-10, EX-11, IC-01, AGT-04 |
| M6 | 30 | TU-01, ED-07, ED-08, CT-01, CT-02, CT-03, CT-04, CT-05, AU-02, AU-03, AU-06, AU-07, AU-08, MI-04, MI-06, MI-08, RE-03, TU-02, TU-03, TM-WT-01, TM-WT-02, TM-WT-03, TM-WT-05, TM-WT-06, DEV-17, DEV-18, DEV-19, DEV-20, DEV-21, DEV-22 |
| M7 | 11 | DEV-07, DEV-10, DEV-11, DEV-12, DEV-13, DEV-14, DEV-15, DEV-23, DEV-24, DEV-27, MX-28 |
| Track C | 14 | CTL-02, CTL-03, CTL-04, CTL-05, CTL-08, CTL-09, AGT-01, AGT-02, AGT-08, WTS-03, WTS-04, WTS-05, WTS-06, WTS-10 |
| P2 | 60 | AU-09, MI-05, MI-07, MI-09, HI-04, RE-02, TU-04, TM-SV-02, TM-SV-03, TM-SV-04, TM-SV-05, TM-WT-04, TM-WT-07, TM-WT-08, TM-WT-09, TM-LS-02, TM-LS-03, TM-IC-02, TM-IC-04, DEV-01, DEV-02, DEV-03, DEV-04, DEV-06, DEV-16, MX-15, MX-16, MX-19, MX-20, MX-21, MX-31, PD-08, PD-15, EX-12, IC-02, IC-03, CO-01, CO-02, CO-03, CO-04, CO-05, CTL-06, CTL-07, AGT-03, AGT-05, AGT-06, AGT-07, SRF-07, SRF-08, SRF-09, SRF-10, SRF-13, SRF-14, SRF-15, SRF-16, WIN-07, WIN-08, WIN-10, WTS-07, WTS-08 |
| P3 | 3 | MX-18, SRF-11, SRF-12 |

For the pairs that are one need (XC-01 and XC-03 to XC-11), both ids sit in the same milestone and
the owner row does the work. In XC-02, XC-12 and XC-13 each row keeps a distinct part.

---

## 3. The decision matrix

All 233 rows in one table, grouped by kind (decided, then refinement, then new choice) and within
each kind by area. Each row carries what the research handoff asks for: the ADI need and the
decision that governs it now, the relevant research IDs, the proposed contract, what to reuse,
what to reject or defer, the evidence's strength, the unresolved test and the affected lane, plus
the milestone where its gate closes. Appendix A holds every row's full card.

**Counts.** 125 decided, 92 refinements, 16 new choices for the director. The cross-check moved four
rows (EX-10 and MX-24 to decided, DEV-24 and MX-35 to refinement). This plan's first draft moved
CTL-05 to refinement (XC-44); its revision moves it back to a new choice (Q-O), moves CT-01 to
refinement (its contract supersedes ADR-0037's comping wording under ADR-0203 d1, so an ADR is
owed), and adds two decided rows, SRF-16 (the Windows MIDI Services backend, ADR-0188 d7's P2
item) and TM-WT-09 (Live's audio-to-MIDI conversions, under d1). The six matrices counted 123, 91
and 17.

**How to read a row.**
- **Kind.** *Decided:* an ADR, SPEC or FEATURES row settles it; the work is to build it or fix a
  defect against it. *Refinement:* inside an existing decision's authority, mostly ADR-0203 d1
  (Live's way); win writes the ruling, placement or format minor, then a lane builds. *New choice:*
  only the director can make it (§6).
- **Need · ADR now.** The row's title, then the ADRs that govern it (four at most, "+n" for more;
  "dN" is a decision inside the ADR). "(was ...)" marks a changed kind.
- **Research.** The bundle's IDs, five at most ("+n" for more): Live rows (A-, A3- to A11-, L01 to
  L10, N1 to N12), Cubase (C-, C3- to C5W-), Bitwig (B-, B3-, B9G-, B9M-), REAPER (R10H-, R11E-,
  R11X-), Reason (N10H-, N11H-), the coverage register (DR-), the ADI source audit (A11N-, A11R-) and
  missions (M2- to M11-, M-...). All NOT RUN.
- **Proposed contract.** Condensed; the full text is in Appendix A. "[XC-nn]" means the cross-check
  (or this plan, "[plan]") changed the row; §5 says how.
- **Reuse.** Take ids from the harvest ledger (§4). "(d)" design only, "(t)" tests only, "(v)" a
  vendored library or code already in tree; no mark means ported or copied with attribution.
- **Ev.** The evidence for the contract: doc (documented), inf (inferred), meas (measured), unk
  (unknown).
- **Unresolved test.** Every case named is NOT RUN; Live cases need the measurement seat (§7).
- **M.** M0 to M7, C (Track C), P2 or P3 (after M7).

| ID | Need · ADR now | Research | Proposed contract | Reuse | Reject / defer | Ev. | Unresolved test (NOT RUN) | Lane | M |
|---|---|---|---|---|---|---|---|---|---|
| **DECIDED · Editing and the arrangement (24)** | | | | | | | | | |
| ED-01 | **Split, range commands and clipboards in the window** · 0203 d7 | A6C-083, A4-012, C-R01, C-R02, C-R03 +2 | The window calls the built producers, one undo step per command through OpSubmitter (worklist A2); a stale batch is refused, never rebased. A split of a selection inside one clip makes both cuts in one step; the left piece keeps the clip id, and a lone surviving piece keeps it (eraseRange's tail case changes). Main lane only until CT-01. | RE1, RE3, RE5 (d) | Cubase's Range Tool split preference and Delete Overlaps override; REAPER's split-every-track-with-nothing-selected as the default (the Live reading sets editing.splitAllWithNoSelection). | doc | M6-A6C-083; the M2-C-RANGE truth table for clips touching the boundary; Live 12.4: what Ctrl+E does with nothing selected. | engine, window | M2 |
| ED-02 | **The ...Time commands and Insert Silence** · 0203 d6 | A4-012, A5C-035, C-R02, R11E-018, R11E-054 | As built: the whole Set over a musical span, linear objects moved by the integrated tempo, crossing clips split, the join keeping the old end's tempo, meter and key. The window adds the Edit-menu items and Live's keys; a Time copy shares ED-01's clipboard. | RM2, RE7 (d) | Ardour's and Qtractor's Insert/Remove Time option dialogs; per-track insertion of time belongs to ripple (ED-07). | doc | A loop wholly inside a deleted span; locators and signature markers exactly on a boundary (A4-012); completing or deleting a fragmentary bar (A5C-035). | window | M2 |
| ED-09 | **Object snapping** · 0129 | A6C-077, A4-015, A6M-003 | Snap candidates from the snapshot (clip edges on any track, locators, signature changes) within a measured pixel threshold; the modifier inverts snapping; no engine change. | none | Snapping to zero crossings (ADR-0070). | doc | M6-A6C-077 (candidate priority, threshold, modifier effects). | window | M2 |
| ED-10 | **Fade handles, Create Fade and Crossfade** · 0203 d7; 0132 d6 | A6C-079, A6C-080, C-F01, C-F02, C-F07 +1 | Handles write clip.setFades as one gesture step, bounded by fade peaks, loop boundaries and the opposite fade (A6C-079); a selection over a clip edge makes that fade, over a boundary a crossfade; Delete removes a fade, resetting to 4 ms only when Create Fades is on. | none | Automatic edge fades by default (his deviation); Cubase's auto-fade layer; the fade editor waits for P2. | doc | AWAITING 14 (Create Fades in a fresh install); the M2-C-FADE curve laws on DC, correlated, inverted and independent sources; A6C-079's limit under a crossfade. | window | M2 |
| CT-02 | **Audio loop recording into take lanes** · 0132 d1; 0203 d11 | A6H-016, A6H-027, A4-017, R10H-039 | The recorder keeps a preallocated pass log (no lock or allocation on the callback). At stop one transaction writes one media row, a take clip per pass on new or reused take lanes, and a main-lane copy of the last pass; the incomplete last pass is kept. | CP1, CP2, CP3 (d) | REAPER's takes inside items; a new FIFO. | doc | M6-A6H-016 (undo per pass); M6-A6H-027 (which lane is reused, where pre-roll goes); AT-3 (3.5 passes give one media row and four take clips; one undo restores pass 3). | engine, window | M6 |
| CT-03 | **MIDI loop takes** · 0132 d8 | A-H02, A6H-027, A4-018, R11E-040 | The MIDI recorder writes one take clip per pass into take lanes through note.applyDiff, closing held notes at each pass end; record quantize is its own undo step; note ids come from the recorder. Arrangement Overdub (the control bar's button): with it on, a pass into an existing Arrangement clip merges the new notes into that clip through note.applyDiff with new stored ids, loop on or off; with it off, the pass replaces the clip's notes over the recorded range. [XC-34, plan] | CP4 (d), CP3 (d) | Session-style loop-overdub merging (Session View is P2). | doc | AT-14 (a held note across the loop boundary, read in Live); A-H02 (Rec-Q against loop-overdub quantize undo); Live 12.4: Overdub on and off into an existing clip, loop on and off (what merges, what replaces, one undo step or two). | engine | M6 |
| CT-04 | **Comp gestures, audition and source highlights** · 0203 d1 | A6H-029, A6H-030, A-H04, R10H-023, R10H-024 +3 | Comp commands compose existing producers (a range copy from a take lane, erased and pasted onto the main lane, one splice step); provenance is derived, never stored; dragging a source highlight's edge moves the split between two main-lane copies in one step. | CP5, CP6 (d) | The active-region comp model (Tracktion, Cubase); REAPER's comp areas as separate objects and ranked take markers. | doc | M6-A6H-030 (empty lanes, painting across silence, an edge past a take's end); what a source highlight compares (manual 21.7); AT-9, AT-10. | engine, window | M6 |
| AU-01 | **Lanes and breakpoints in the window** · 0159; 0203 d7 | A6C-053, A6C-058, A6C-059, A6C-060, A6C-062 +3 | Each breakpoint gesture is one gesture (HI-01) and one undo step; the point ops re-key automation_segment_ranges in the same op; before the first point a lane holds its first value, and a disabled lane plays its control's own value. [XC-22] | AU1 | Cubase's Virgin Territory; Bitwig's Hold and Spread point options. | doc | AWAITING 12 (how Live stores an Alt-drag curve); M6-A6C-058 to -060 (a drag over a neighbour; axis lock); M6-A6C-062. | engine, window | M2 |
| AU-02 | **Insert Shape** · 0203 d7 | A6C-065, A-BASE-25.5.5 | Top-row shapes scale to the selection and the parameter's range; lower-row shapes join the neighbouring values; guard points at both edges through one shared splice, so the curve outside the selection is unchanged; one undo step. | AU1, AU2 | User-defined shapes. | doc | L-AUTO-8 (what Live keeps outside the selection; which end each ramp joins); AT-1 (valueAt outside [t0, t1] unchanged within 1e-9 for every curve kind). | engine, window | M6 |
| AU-04 | **Override and Re-Enable Automation** · 0162; 0164; 0165; 0196 | A5H-022, A6P-003, L-AUTO-7 | Session state, not an op (ADR-0162): the window passes the overridden lanes to the view, lights Re-Enable Automation and offers the per-parameter item; whether an override survives save follows the reading. | none | A/B preset switching that disables automation, until device A/B exists. | doc | AWAITING 12 / L-AUTO-7 (an override after save and reopen). | window | M1 |
| AU-07 | **The event volume curve on the clip (Cubase 14)** · 0115 d1 | C-F10, A6C-069 | The clip-owned gain lane is ADR-0115's absolute curve in dB; a clip's gain is its slider times Live's Clip Gain modulation (AU-06) times this curve, all before the effects; drawn and edited on the clip body in the Arrangement, its own undo steps. | AU4 | Cubase's tilt, compress and scale tools for the curve (P2, with the fade editor). | doc | The order against Clip Gain modulation (L-AUTO-6); stretch with a warped clip; a flat 0 dB curve nulls against the clip. | engine, window | M6 |
| AU-08 | **Simplify, Delete Automation and lane-scoped commands** · 0203 d1, d7 | A6C-051, A6C-069, A-BASE-25.5.4, A6C-062 | Simplify replaces a selection's points by an error-bounded reduction in one transaction (enum and hold lanes refused), exact undo; Delete Automation removes a parameter's data in every owner in one step; lane-scoped Cut, Copy, Duplicate and Delete reuse the time-edit splice. | AU5, AU6 | REAPER's point-reduction preferences. | doc | AT-7 (a 1001-sample exponential ramp becomes two points with a curve within epsilon; enum refused; exact undo); Live's tolerance (L-AUTO-1). | engine, window | M6 |
| AU-09 | **Cubase's automation modes and MIDI Learn on lanes (P2)** · 0203 d7; 0162 d4; 0195 d3 | C3-016, C3-017, C3-018, C3-019, R10H-053 | Cubase's automation modes ride AU-03's pass coordinator as a visible addition; Live's Arm stays the default; Trim writes an offset layer merged into the curve at pass end as one step; MIDI Learn on lane headers through the one port host. [XC-34] | AU9, AU3 | Fill and Return Time until after parity. | doc | AT-11 (Freeze Trim at pass end: original plus trim at every point, the trim lane cleared, one step). | engine, window | P2 |
| MI-03 | **The note editor (Clip View)** · 0203 d7; 0160 d3 | A-M01, A6M-003, A6M-009, A6M-036, A6M-037 +3 | Every editor gesture previews an applyDiff in a rolled-back transaction and commits once on release (HI-01); a refused op leaves the notes unchanged and says why; note preview through the note preview queue (N44). Built only after he approves the Clip View picture. | ME4 (d) | The pattern editor (P3); Helio's Join, Reverse, Invert and Legato semantics where Live differs. | doc | parity-7.7's rows side by side with Live 12.4; goldens of an empty and a dense clip; A6M-036 to -038 (multi-clip clipboard and drawing). | window, director, engine | M2 |
| MI-04 | **Pitch and Time utilities, Split, Chop and Join** · 0203 d7 | A6M-009, A6M-018, A6M-019, A6M-027, A6M-033 +1 | Each utility is a pure function over the selection producing one applyDiff; Split keeps the left note's id; expression curves split and join exactly per curve kind; degree-aware transposition reads the clip's scale. | ME3 | Helio's own Join, Reverse, Invert and Legato semantics. | doc | A6M-009 to -027 and -033/-034 as cases; AEXP split exact per curve kind. | engine, window | M6 |
| MI-05 | **Quantize, groove, Humanize and Randomize** · 0203 d7, d12; 0132 d8 | A6M-022, A6M-024, A6G-013, A-H02, A6M-078 | The Groove Pool as a project-scoped whole table with Live's groove parameters; Commit Groove writes one applyDiff; Humanize and Randomize as Live's two controls, the span from the reading, seeded per apply so redo replays. | none | Bitwig's real-time Note FX quantize. | doc | A6M-022 (Humanize span); A6M-024 (the quantize key); .agr import (A6M-078). | win, engine, window | P2 |
| MI-06 | **Scale Mode** · 0203 d7; 0103 | A6M-018, A6M-019, A6M-031, A6M-032, A-T09 +1 | Live's scales under Live's names and order from a Live 12.4 census, each a degree mask over the clip's tuning; Fit to Scale rounds ties to the lower degree; degree operations through ScaleContext; spelling Sharp, Flat or Auto. | ME6, ME3 | The .stacks chord rulesets. | doc | The C6-SCALES census / M6-A6M-031 and -032 (Live 12.4's list, order and members). | win, engine, window | M6 |
| MI-07 | **MIDI Tools (P2)** · 0203 d7; 0169 d7 | A-T02, A6X-013, A6X-016, A6X-017, A6M-040 +2 | A tool is saved settings, a pure transformer or generator and a rolled-back preview committed as one applyDiff; a change of note selection commits a pending preview; seeds stored so redo replays; Arpeggiate shares DEV-19's ordering function. [XC-25] | ME5 | Bitwig's operators as note properties; the Logical Editor and Expression Maps (P3). | doc | A-T02 / A6X-013 (do generators replace all notes or only overlapping ones); M2-A-MIDI (Auto Apply, repeated Apply, Reset, cancel). | engine, devices, window | P2 |
| MI-08 | **MPE and note expression: playback and editing** · 0054; 0155 | A-X01, A-X02, A-X03, B9G-038, C3-028 | The clip schedule compiles AEXP curves into expression events on ADR-0054's grid; noteExpression writes ride in applyDiff; the utilities carry expression. | ME4 (d), ME3 | Bitwig's eight-port note bridge beyond the MPE dimensions. | doc | A stored pitch curve played through Surge XT (MPE) and Serum 2 (note expression) matches the recorded input (ADR-0098's rig). | engine, window | M6 |
| HI-03 | **Live's Undo History window, with branches and snapshots** · 0203 d11; 0128 d6 | A4-010, A5C-048, R3-001, R10H-061 | History::moveTo(seq) rewinds to the common ancestor and replays forward in one transaction; the timeline leaves out the compaction root and says older steps were compacted; Live's Undo History list, plus a branch tree view as d11's addition. | HI4 (d), HI5 | A space-used column (an addition nobody asked for). | doc | Model tests (rows after commit, undo, redo and fork; restoring the third row is two undos refreshed once); parity-history rows; M-LIVE-UNDO. | engine, window | M2 |
| HI-04 | **Snapshots, revert, the historical tab and Save As** · 0128; 0140; 0111; 0203 d6, d13 | A4-010, A6H-068, R3-001 | As decided: Ctrl+S snapshots with a pre-filled name, revert keeping the left work as a branch. The daily snapshot is wired only with Q-A's rule. A historical state opens as the silent read-only case of PD-15's tab host; paste back is one transaction. [XC-02, XC-23] | HI2 | Automatic snapshots on export or open. | doc | AWAITING 13 (which file is open after Save As and after Save a Copy); HI2's eight version rows. | window, engine | P2 |
| RE-01 | **Remarks shown in the window** · 0131 d1; 0139; 0203 d6 | DR-12-09, DR-12-10 | Pips from the snapshot on visible tracks, clips, devices and parameters; agent remarks in their own colour and glyph; the Info View shows the documentation above and the remark below; add, edit, resolve and remove. | RM5, RM8 (d) | Margin balloons in the Arrangement. | inf | The hover split; pips within 8 px merge into a count; his first look at the picture (an addition). | window | M1 |
| RE-02 | **Remarks as Word's comments: threads and anchors (P2)** · 0203 d17; 0131; 0139; 0188 d5 | DR-12-09, DR-12-10, C3-030, R10H-029 | Whole tables (remark_replies, remark_anchors); Word's rules literally: a remark follows its object, Split keeps it on the left piece, Duplicate and Paste copy threads, deleting the object deletes its thread (captured for undo), Delete Time acts as Word on deleted text; every rule in the edit's transaction; the agent's brief bounded and propose-only. | RM1, RM2, RM3 (d), RM4 (d), RM6 (d), RM8 (d), RM9 (d) | The harvest's re-pin and collapse rules (they change Word's behaviour he named); nested threads. | inf | The remarks harvest's T1 and T4 to T11 under Word's delete rule; L-INFO-1 to -5; a Hebrew remark reads from the right. | win, engine, window | P2 |
| TU-01 | **Land the tuning stack** · 0178; 0103; 0117 §3; 0203 d7 | A3-024, A6F-008, A6F-009, A6F-010, A6F-011 +1 | Merge #445 (rebased, retargeted to main after #421), then its siblings #444, #443 and #442, each retargeted to main; before the merge, a guard on #443's branch refuses any source whose reference pitch would move a degree off its 12-TET anchor, with a verdict naming the line, so no tuning plays flat until TU-02 stores the reference; OPS moves the six tuning and key ops to P1. [plan: merges M0, gate M6] | TU1 | Vendoring tuning-library (it is the test oracle only); the visible key and scale track (P2). | doc | The tuning6 fixtures give the proposed verdicts; 31-EDO degree 0 at 263.0921 Hz (fails on the merged stack by 9.677 cents; closes with TU-02). | win, engine | M6 |
| **DECIDED · Session, clips, time and media (20)** | | | | | | | | | |
| TM-SV-03 | **The launch engine** · 0101 d4; 0117 §2; 0203 d1, d10; 0042 d2-d4; +3 | A3-001, A3-002, A5C-039, A5C-040, A6C-018 +14 | An engine launch lane drained on the driver in Live's per-block order; a pure press/release table (Trigger relaunches); Legato joins the old position; launch, stop and hand-over edges follow Live's per-clip Fade (the measured 176/192/384 samples until N8), Arrangement edges keep ADR-0151 d3; one tick-to-sample rule; voice identity survives rebuilds; zero callback allocations. [XC-25] | SV1, SV2, SV4 (d) | Tracktion's runtime launcher; MAGDA's host and UI; openDAW's stop that returns to the timeline; warped Session clips under a changing tempo (TM-WT-04). | doc | M3-LAUNCH L1, L2; N2, N3, N5, N8, N10 to N12 on Live 12.4; MAGDA's launch suites restated in ticks and samples at 32 to 4,096 frames and 44.1 to 192 kHz. | engine | P2 |
| TM-SV-04 | **Follow actions, scenes and capture** · 0101; 0117 §2; 0203 d1, d10; 0162; +1 | A3-003, A3-004, A3-005, A6C-043, A6C-044 +12 | Follow-action groups as runs of occupied slots; Live's ten actions written fresh; a fresh chance draw on every pass (seeded within one pass so a realtime pass and its render agree); a user launch outranks a due follow action; scene launch honours Stop buttons; scene tempo through the one effective-tempo path; capture.sessionPerformance as one changeset. [XC-29] | SV3, SV1, SV6 (d) | A stateful random generator on the audio thread; the matrix's "same draw on every pass" (XC-29); the crossfader (MX-15). | doc | M3-LAUNCH L3, L4, L6; N6 (Previous at a group's top; Other alone; Chance coupling); N7; N9. | engine | P2 |
| TM-SV-05 | **The Session View window** · 0117 §1; 0203 d6, d10; 0101 d3; 0063; +3 | DR-01-02, A5C-038, A5C-039, A5W-003, A5W-014 +2 | The window builds the grid on a stub provider once TM-SV-02's schema lands, reading the per-slot taps and run edges, never the store; a parity checklist from manual chapters 7 and 16; his signed side-by-side session. The crossfader is MX-15's. [XC-10, XC-41] | in tree / own | The MixConsole toggle (P3, last). | doc | The parity checklist side by side with Live 12.4; M5-A5C-039 (Track Status text and icons). | window, engine, win | P2 |
| TM-WT-03 | **Beats and Texture parity and the listening gate** · 0188 d2; 0061; 0108 | A-W09, A-W10, A6C-039, A6C-040, DR-04-03 +6 | Beats cuts on the AWRP grid by Preserve; the loop point at a sign change within ±5 % of the segment's middle; a micro-fade at slice switches; 4-point Hermite interpolation; Texture's parameters and seed from clip_warp_settings; the named parity gaps stay named. | none | SoundTouch; real-time players (TM-WT-04). | doc | M2-A-WARP 6 (each mode at 0.75x and 1.5x and ±7 semitones; loop and grain extremes); no step above -60 dBFS at slice boundaries; Preserve 1/8 on exact frames; his listening gate. | engine, devices | M6 |
| TM-WT-04 | **Warped audio that follows tempo changes** · 0061 d1, d3, d4; 0151; 0185 d7 | DR-04-12, C3-011, R11E-038, M2-A-WARP, M3-LAUNCH | Streaming Beats, Texture and Re-Pitch players and a primed real-time R3 whose latency joins compensation; until they exist a tempo change is a new prepared generation and every warped clip says so; an equivalence test against the prepared render at a steady tempo. | WT6 (d) | Whole-output resampling for Link following. | doc | A latency-alignment test; M2-A-WARP 7 (playback against offline, stop and seek, save and reopen). | engine | P2 |
| TM-TT-01 | **Tempo ramps play everywhere** · 0004; 0151; 0155 | DR-04-12, C3-011 | One effective tempo map that integrates linear and bezier ramps exactly, read by every builder (clips, MIDI, automation, metronome, warp anchors, both quantizers); Link and clock boundaries as a private overlay that never enters the project. [XC-30] | none | A per-block TimingView override (plug-in tempo would move while clips keep the project tempo). | doc | Ramp placement against exact integration at 44.1, 48 and 96 kHz; a note at a ramp's midpoint on its exact sample; warped audio across a ramp. | engine | M3 |
| TM-WT-06 | **The Clip View Sample Editor** · 0203 d6, d7, d18; 0184; 0210 d3 | A6C-034, A6C-035, A6C-036, A6C-037, A6C-038 +3 | Built against needs rows for transients, warp settings and AWRP in the snapshot, after his approval of the Clip View picture; each gesture commits one prepared batch through OpSubmitter. | none | Waits for TM-WT-02's ops and the picture approval. | doc | The parity-7.7 checklist side by side with Live; headless seam tests per gesture. | window | M6 |
| TM-WT-08 | **Hitpoints, Slice to MIDI and Audio Alignment (P2)** · 0176; 0062 | C3-001, C3-002, C3-003, C3-004, C3-005 +2 | Hitpoints are TM-WT-01's cached onset strengths filtered at read time by Cubase's Threshold, Intensity, Minimum Length and Beats; Audio Alignment by dynamic time warping over onset strength and MFCCs, thinned to warp markers in one changeset; slicing through OneShot and a Drum Rack container. [XC-34] | WT2, WT1 (v) | Any new instrument work. | doc | The Cubase hitpoint and alignment cases; R11E-026's phase-align comparison. | engine, devices | P2 |
| TM-WT-09 | **Convert Harmony, Melody and Drums to New MIDI Track** · 0203 d1; FEATURES (new row, P2) | none in the bundle (Live 12 manual ch. 11; the AI-workflow ADR lists audio-to-MIDI among its fifteen workflows but FEATURES carries no row) | Live's three conversions on an audio clip's context menu, each writing one MIDI track and clip in one undo step; Drums reuses TM-WT-01's cached onset strengths; Harmony and Melody need a pitch tracker, a Q-D candidate (one analyser for the suite); results as versioned ADIA sections keyed by the file hash; the clip's warp markers map the notes to the timeline. [plan] | WT1 (the onset path); the pitch tracker after Q-D | A model-based transcription (the AI workflows' audio-to-MIDI stays in that ADR's list, separate from Live's command); any new instrument work for the Drums target. | doc | Live 12.4, P2 readings: which track and device each conversion creates and names, the quantize and velocity applied, Drums' pad mapping, Harmony's polyphony limit. | engine, window | P2 |
| TM-LS-01 | **Ableton Link (P1)** · 0185 d7; 0203 d11, d19; 0162; 0107 d1; +1 | DR-11-02, A6I-006, A5C-026, A5C-027, A5C-028 +4 | The SDK behind one pImpl unit; one driver commits the session state per callback (callback time, output latency, compensation); Link's tempo enters as an effective-tempo generation; starts and stops land on the quantum; a peer's tempo is a runtime override that lights Re-Enable Automation; Link on ASIO, off on DirectSound, WASAPI per Q-F; the pin moves to Link-4.1.1 before the consumer merges. [XC-31, XC-25] | LS1 (v), LS4 (d), LS5 (d), LS6 (d) | The SDK example's Windows latency arithmetic; a TimingView-only override; whole-output resampling; BespokeSynth's SPP code; Link Audio (Q-F). | meas | AUDIOENGINE-1 (an onset within 3 ms of LinkHut on the director's Windows workstation, recorded through loopback); TEMPO-1 to -5, BEATTIME-1 and -2, STARTSTOPSTATE-1 and -2 as tests; M3-CONTROLLER 6; M5-A5C-026 to -029; a held-note tempo change. | engine, window, win | M3 |
| TM-LS-03 | **MIDI clock and timecode (P2), and the MIDI port layer** · 0203 d9; 0145 d4; 0188 d7; 0206 d3; +1 | A6I-007, A6I-008, A6I-009, A6I-010, A6I-011 +3 | One MIDI port host on JUCE's UMP API (window; Windows MIDI Services off until SRF-16) feeds timestamped rings to the engine; clock in by a second-order DLL into the effective-tempo overlay; SPP on exact ticks; clock out (Song or Pattern, Sync Delay); MTC in, locking after 8 quarter frames; the registry made to match d9; a FEATURES row (P2; MTC out an ADI option). [XC-18, XC-39] | LS2, LS4 (d), LS6 (d), IC8 | Ardour's truncated 29.97 drop-frame column; Bitwig's "sync in" chooser; Tracktion's MTC generator; BespokeSynth's SPP truncation; the Tempo Follower; video sync (P3). | doc | AWAITING 13 (MTC rates and default; how fast Live follows a changing clock; Resync); the Song-mode restart point; the DLL tracks 120 to 121 BPM under ±1 ms jitter; MTC forward, backward and skipped. | engine, window, win | P2 |
| TM-MD-01 | **MP4, M4A, MOV and ALAC import** · 0203 d18; 0132 d3-d4; 0156; 0191 d4; +2 | A6R-047, M6-A6I-013, DR-12-08, L-MEDIA-2, L-MEDIA-3 | Step zero is PD-00's CoreAudio priming fix. Then a media ADR: ADI's own bounded ISO-BMFF and QuickTime reader (headers only, hard caps, hostile files refused); AAC layout from the AudioSpecificConfig; trimming by edit list, else iTunSMPB; platform decoders fed raw packets; ALAC in process; a movie's first audio track imports (the picture is P2); the NI reader becomes a strict filter over it. [plan] | MD2, MD1, MD3 (v) | craft's sample-entry channel count; the Windows re-mux; complex edit lists (P2); Ardour's -map_channel. | meas | A-M1-1 to -10 on all seven legs (Mixxx's AAC first-sound frames; ALAC equal to FLAC; MOV PCM bit-exact; fragmented equal to plain; hostile files refused); L-MEDIA-2, -3. | engine, win | M3 |
| TM-MD-02 | **Playing before decoding finishes; known-hash loads; Cleanup** · 0203 d18; 0156; 0151 d4, d8 | DR-12-08, A6H-064, A6C-033, A5H-009, M3-MEDIA +2 | One decode job per source hash; a clip's page served once the decoded frontier covers it (a pending page is silence counted as pending, never an underrun); the clip switches to the finished file without a rebuild; warped clips wait for the whole file; the stored BLAKE3 reused on load; partial files reaped; Cleanup; decode state in the snapshot. | none | A separate shared-file positional-read layer. | doc | A-M2-1 to -8 (the import commits before the decode ends; renders bit-identical once done; a 50-source load hashes nothing; Cleanup); L-MEDIA-1 (time to first sound in Live). | engine, window | M3 |
| TM-MD-04 | **The analysed folder, sample defaults and sample analysis** · 0203 d18; 0104; 0145 d10, d11; 0147 | A-W05, A6H-063, A5H-008, A5H-009, DR-02-09 +5 | Derived analysis as versioned ADIA sections in the analysed cache (evictable); the user's sample settings in the library, never cleaned, recalled over Auto-Warp; key from the suite's analyser (Q-D); read-only tags with a precedence (the user's value, the file's tag, ACID, detection). [XC-19] | MD6 (v), MD7 (v), MD8 (v), MD9, MD10 (d) | ADIL v2 clip_defaults and an AANL file; Similarity Search, Similar Sample Swapping and Auto Tags (a later row); REX (no usable decoder). | doc | A-M4-1 to -9 (key cases; tag read-back; precedence; eviction); M2-A-WARP 8; L-MEDIA-6 (does Live use an ACID tempo when warping). | engine, window, win | M3 |
| TM-FC-04 | **Freeze All Channels and Live's frozen-track editing** · 0203 d16; 0059 | DR-07-21, FR-041, A5C-013, A5C-014, A5C-015 +6 | Each child cached through the one capture with dry and wet streams; sidechain keys and sends served from those streams once taps and endpoints have their format home (XC-15); re-freeze renders only changed children; after the Live readings, frozen clips play windows of the wet file at their positions, tails as temporary windows. [XC-12, XC-15] | FC2, FC5 (d) | Frozen-clip editing before M5-A5C-013 to -016; a real-time freeze before external devices exist. | doc | M5-A5C-013 to -018; M6-A6G-005 (a frozen clip moved to another track or project); freezing a track with a keyed compressor, in Live. | engine, win | M5 |
| TM-IC-01 | **The exact time base and SMPTE timecode** · 0206 d3; 0004; 0144 d3 | FR-010, DR-13-17, DR-13-18, C4-016, INTEROP-ARTCRAFT | One pure integer component (src/adi/time_base): ticks per second to and from frames, nanoseconds and musical ticks with a range check; drop-frame on integer frame counts; 29.97 as exactly 30000/1001; the drop-frame flag in a new table; external_sessions and external_links as their own additive minor. | IC8 | OTIO's double-based RationalTime; Ardour's truncated 29.97 column. | doc | INTEROP-ARTCRAFT §3's tables (an NTSC frame is 8,475,667,200 ticks; the 125 ns grid is 31,752 ticks; Tick::MAX refused); OTIO's drop-frame vectors. | engine, win | M5 |
| TM-IC-02 | **MIDI files as Live (P2)** · 0203 d7; 0004 | A-BASE-3.10, A-BASE-5.2, A-BASE-5.2.1, A6H-028, A3-021 +4 | A JUCE-free SMF codec (variable-length quantities, running status, raw meta and sysex, 64-bit ticks, SMPTE division, unknown chunks kept); import is prepare and commit, one step, through clip.createMidi; unmapped data parked in extensions rows for a byte-identical re-export; the source PPQ remembered; no options box (P2). [XC-11] | IC7 | glib-based libsmf; committed song files; the harvest's P1 placement (d7 rules P2). | doc | AWAITING 13 (what a multi-track file creates; whether its tempo is taken; the export's type and PPQ); SMF-1 to -4 (the corpus; tick-identical round trips). | engine, window, win | P2 |
| TM-IC-03 | **DAWproject both ways (P1) and the converter rules** · 0127 d7; 0144 d3; 0012; 0036 | E24, C4-018, DR-13-17, M3-EXPORT-INTERCHANGE, M4-C-EXPORT | A converter ADR: JUCE-free src/adi/interchange; prepare on a worker, commit as one step (actor import); a per-field loss report; unmapped XML parked and re-exported verbatim; nested clips flattened; VST3 matched by hashing its class UUID; colours to the 70 palette indexes; the missing ops first (extension.write, noteExpression.write, clip.createMidi); the devices lane maps the built-in EQ, Compressor, Gate and Limiter. [XC-11] | IC1, IC2, IC3, IC4, IC5 (v), IC6, IC12 (d), IC13 | Tracktion's JUCE glue and its random round trip as the fuzzer; a drop-frame column; .als and Pro Tools import (not raised). | doc | DP-1 (every export validates against the XSDs) to DP-6; DP-7, his Cubase 15 round trip with a VST3 insert, automation and notes. | engine, devices, window, win | M5 |
| TM-IC-04 | **OTIO and AAF (P2), OMF read (P3)** · 0206 d4, d5; 0127 d7; 0071; 0115 | C4-016, C4-017, R11X-023, R11X-024, DR-13-17 +3 | TM-IC-03's converter rules; AAF import through LibAAF (static, behind a mutex) with Ardour's mapping; AAF export through a clean-room compound-file writer lowered from rows::Model; separate essence files as export jobs that read the project when each job starts; OTIO as a JSON subset with integer time; the timecode and frame-rate ops rise to P2. [XC-11] | IC9 (v), IC10, IC8, IC11 | The AAF SDK; OTIO's C++ library; reading or writing .fcproj and .scraft. | doc | AAF-1 (65 files match their dumps); AAF-2 (an ADI file reads back through LibAAF and pyaaf2); AAF-3 (Pro Tools or Resolve, his gate); OTIO-1. | engine, win, window | P2 |
| TM-IC-05 | **NI Stems import and export, and N-stem results (P1)** · 0205 d8; 0191 d4; 0204 d5; 0071 | DR-13-11, R11X-013, A11R-001, ERRATA-11 | Kept as built: the job reads its revision when it starts (ERRATA-11). After the macOS fix, prove the Windows Media Foundation path really runs rather than skipping; the NI reader becomes a strict filter over the general one; #453 (N-stem results) after. [XC-05] | MD2, MD1 | A container CDJs read (AWAITING 17). | meas | Both heavy macOS legs green on the AAC and NI suites at one headSha; a Windows run that shows the codec path executed. | win, engine | M5 |
| **DECIDED · Devices and processing (13)** | | | | | | | | | |
| DEV-02 | **Zones, chain select and note identity where chains rejoin (level 1)** · 0060; 0203 d1 | A6H-042, A6H-043, A3-026, A3-027, A8D-017 +2 | The zone BLOB is exactly 4 bytes (low, high, low_fade, high_fade; NULL is the full range); a note passes every applicable zone; the router runs in the transform phase; the selector is an automatable rack parameter; per-chain note ids where chains rejoin; both fade laws built, Live's measured law the default. | RK3, RK6, RK2 (d) | sfizz's normalized velocity offset in integer zones; Bitwig's Selector modes. | doc | sfizz's key-fade vectors under both laws; a note-off reaches its chain after the zones move; Live 12: the fade laws and the Filters MIDI Ctrl default (M6-A6H-042/043). | engine, win | P2 |
| DEV-03 | **Rack structure: ops, exact undo, copy and paste (level 1)** · 0003; 0110; 0060; 0203 d10 | N11H-006, N11H-007, N11H-008, N11H-013, N11H-015 +2 | Every structural edit is one transaction with a captured inverse; device.remove and chain.delete capture the whole subtree (parameters, state, panels, routes, macros, mappings that target it); device.move refuses a cycle; Group, Ungroup and reparent are each one transaction; paste is a graph fragment with fresh ids (lifts #394's refusal). [XC-22] | RK4 (d) | A universal move-modifier rule (ERRATA-11: Reason's chapters disagree); undo-manager command objects. | doc | Remove a populated rack, undo and redo: the digest is identical; a self-nest is refused; a rack paste gets fresh ids; OPS §6.3's lossy_undo threshold; hosts11 rack-relationship-rewrite. | engine, win | P2 |
| DEV-04 | **Drum Rack (level 1)** · 0060; 0169 d4; 0177 | M8-A8D-001, A6H-044, A8D-004, A8D-007, A8D-010 | Drum mode of the rack router: Receive, then Play translation; choke as router-tagged events sorted before same-position note-ons, symmetric within a group; return chains as rack branches with post-fader sends to the rack or a Set return; pads show Receive notes; a pad swap rewrites mappings, never clips; a sample drop makes a OneShot chain. | RK2 (d), RK3, RK4 (d) | sfizz's directed off_by; the beatbox drum tab (P3); kit instruments (Impulse). | doc | A8D-004 (choke release with OneShot and with a long-release plug-in); simultaneous hits; choke against a reverb inside the chain; A6H-045; M8-A8D-001 to -025. | engine, devices, window | P2 |
| DEV-05 | **A sidechain key into a device, with Live's taps (level 1)** · 0203 d3; 0058 d5; 0043; 0057 | A5H-036, B-G01, N11H-040 | A routing row may end at a device's key input (dst_kind 'device'); the tap sits in a side table keyed by routing id; the planner places a compensated key edge into the device's sidechain bus; Pre FX and Post FX read the source chain, only Post Mixer follows fader, mute and solo; hosts negotiate VST3 aux buses and CLAP audio ports. [XC-07, XC-15] | none; in tree | The Quick Kick helper (P2; its target waits for his list); N-bus (P2). | doc | A fixture compressor keyed from another track through each tap renders aligned, with a latency plug-in on the key path; suspension stays off while keyed; a VST3 aux input receives the key; AWAITING 7(h). | win, engine | M4 |
| DEV-08 | **Automation and modulation heard on ADI's own devices (level 2)** · 0165; 0188 d3; 0196 | A11N-002, A11N-004, B-M01 | Built-in devices (and later patch devices) consume ParamValue (the base, normalized through the declaration's curve) and ParamMod (an offset for that block) from io.events at segment start, sent through the pre-resolved sendParameter before the native core or the Pd tick. | in tree / own | Nothing. | inf | AR4: a ParamValue lane on Utility's Gain is heard within one Pd block of its point; ParamMod adds to the base; the lane plays the same after save and reopen. | engine | M1 |
| DEV-09 | **Pd runtime safe and 64-bit capable (level 3)** · 0188 d8; 0203 d8; 0183 d18; 0010; +2 | A11N-009, A11N-010, A11N-011, PD-RACK-EXPORT-FIT-11, BITWIG-GRID-PD-FEASIBILITY | Native loading closed at the source (absolute, ../ and dynamic names refused, named on the console); quit, exit, open and path changes fenced; one trylock tick per Pd tick (skip and count, never wait); printing through a queue; SPSC sends; arrays read without the lock-leaking path; the #A overlay; the latency binding unbound on close; a PD_FLOATSIZE=64 build. [XC-39] | PD1 (v), PD2, MF4, PD4 | [pd~] out-of-process patches; a file and network policy for shared patches (noted for later). | inf | AR7 (planted library never loads; quit and exit fenced); AR8 (no waits or audio-thread allocations); AR17 (16 suites at PD_FLOATSIZE=64); AR18 (#A under ASan); #173's 40-cycle stress test. | devices, win | M1 |
| DEV-16 | **`.amxd` translator and Live Suite's Max for Live devices (level 3, P2)** · 0203 d8 | manual ch. 31-32; the A7B and A7M rows | A pre-flight allowlist scan of the patcher's boxes; a full match converts into a user Pd patch device with Max's right-to-left order explicit; otherwise the AI proposes through the Propose tier and the result must pass the allowed-object walk; Suite's LFO, Envelope Follower and Shaper become native modulators on DEV-07's routes. | PD4 | A Max application path. | unk | A corpus of simple .amxd devices (his own if he has some) converts and renders equal to Live within a stated tolerance; a device that cannot convert is refused, naming its boxes. | devices, engine | P2 |
| DEV-19 | **Live's fourteen MIDI effects** · 0169 d1-d6; 0203 d7, d8 | A7M-001, A7M-053, A7M-122, A7M-128, A7M-130 +3 | Native midi_effect devices (org.adidaw.builtin.* uids) inserted before the instrument; kernels in src/adi/dsp with fixed state; randomness varies per pass by default, with a keep switch that stores the seed (one setParam, reseeded per note from seed, position and key); Use Current Scale from a published scale context; one ADR and one parity checklist per device; P1 first (Arpeggiator, Chord, Scale, Pitch, Velocity, Note Length, Random). [XC-25, XC-34] | MF1, MF5 (d) | Microtuner (parked); Expression Control, Envelope MIDI and Shaper MIDI until DEV-07; Groove until the groove pool; tracktion's MidiModifier. | doc | The A7M checklists side by side; A7M-130 (two renders differ with the switch off, match with it on); the devices7 MIDI probes; LM1 to LM8 on Live 12.4. | devices, engine, measurement seat, win | M6 |
| DEV-21 | **OneShot and Sampler (the two authorized instruments)** · 0169 d4; 0203 d8; 0192 d4; 0127; +1 | A7M-058, A7M-131, A7M-127, A7R-008, A7R-009 | Catalogue kind instrument (consumes notes), native dispatch in makeBuiltinDevice, samples through #414's slot ops, idempotent prepare; Sampler's zones published immutably on OneShot's voices; Sample Selector and Offset latch at note-on (A7M-127). Land #278, then port #208 onto it. | MF2 | Live's other instruments (outside scope: Analog to Impulse). | doc | #278's suites and #208's 28 checks on main; A7M-127; the sampler MIDI probe; measured Live parity (#278's draft gate). | devices, engine | M6 |
| DEV-22 | **Live's audio effects: measured parity, the queue, Hybrid Reverb** · 5 Nov ruling; 0169; 0187 d1-d3; 0188 d8; 0203 d4; +2 | M7-A7A-001, M7-A7B-001, A7R-001, A5C-021 | Each device a canonical canvas with a native core and no adapter delay, fixed parameter ids, latency reported and kept while bypassed, accepted on a measured Live 12 render, one per PR; the queued effects in the manual's order, Compressor first; Hybrid Reverb's native factory; the DJ Filter on Auto Filter's cores. [XC-27] | LD5 | Parity claims from synthetic oracles; instruments other than OneShot and Sampler. | doc | Live 12 renders of the 16 built effects (none made yet); AWAITING 9 (Shifter's latency) and 10 (Auto Filter side by side); the 358 M7 cases. | devices, measurement seat, win | M6 |
| DEV-26 | **The director's plug-ins load as ordinary plug-ins** · 5 Nov ruling; 0204 d1, d3; 0209 d1; 0041; 0110; +2 | N11H-028, N11H-033, A5H-021, A11N-009, B3-005 +1 | His builds take the ordinary path: scanned in a child process and listed under Plug-ins; inserted in one undo step; edited in their own windows (one op per knob gesture, one per preset); state round-trips through save and reopen; ADI-L's lookahead compensated, also while bypassed; a missing build becomes a placeholder keeping its state. Nothing in ADI depends on their source until his list. [plan] | none; in tree | Designing the unified feature, the native slot contract or any built-in that hosts the VST3; requiring a CLAP target. | inf | On the director's Windows workstation, never on his desktop: ADI-Q 0.3.17, ADI-L 0.1.6 and ADI-Clip 0.1.3 scan, load, edit as single undo steps and reopen with identical state hashes; ADI-L's lookahead nulls against a dry track; a missing build is a placeholder; AWAITING 11. | win, engine, window | M1 |
| DEV-27 | **AudioGridder remote devices** · 0053; 0083; 0134 d3; 0145 d6; +1 | B3-009, B3-010, N11H-032 | A remote device is a DeviceInstance with remote_host_id (format unchanged); the network stays off the audio thread through an SPSC queue; pipelined one block behind with that latency declared; a missed deadline outputs silence and counts; an unreachable server loads the plug-in locally, else a placeholder; the LAN-only authentication posture stated before shipping. [XC-40] | none | A wrapper plug-in (ADR-0053); PTP until a measured need. | unk | A spike answering ADR-0083's open items (CLAP note expression on the wire; link latency); a remote Surge XT matches the local one within the declared block; losing the server mid-play never glitches another track. | engine | M7 |
| DEV-29 | **Export loudness: NI Stems guard, "together", brickwall, more targets** · 0203 d14; 0205 d5, d8, d9; 0071 d5 | R10H-073, R11X-002, R11X-025, R11X-034, R11X-043 +4 | Now: validateNiStems refuses normalization. Then (a) an after-AAC check lifts the refusal; (b) "together" is one gain from the summed, time-aligned capture; (c) an off-by-default true-peak brickwall in ADI's LookaheadLimiter with a refine loop of at most four trials; (d) RMS-I and max M/S targets. WITH and VIA master stay distinct; job values at enqueue, revision and names at job start. [XC-04] | LD5, EX4, LD3 (v) | An enqueue-time snapshot (ERRATA-11); one checkbox for both master modes; normalizing by default; ADI-L as the export limiter. | inf | An NI Stems job with normalization is refused (no test today); WAV and AAC within 0.5 LU of target with true peak at most ceiling + 0.1 dB; three stems "together" checked by libebur128 and ffmpeg; REAPER 7.79's render-normalize compared. | engine, devices, window, win | M5 |
| **DECIDED · Mixer, routing, monitoring and the Mix View (23)** | | | | | | | | | |
| MX-01 | **Strip meters on the engine's scalar meter** · 0050 d4 | A5C-001, A5C-004, A-BASE-18.1, A-BASE-18.1.1, M5-A5C-001 | Each visible strip reads {peak, rms} relaxed per frame from the engine's StripMeter; ballistics and the Peak Level hold stay the window's; no ScopeTap per strip; while a track monitors its input the meter shows the input (MX-09). | Mix View program refs | The per-strip 0.1 s tap that never closes; K-meter, PPM and VU scales (additions not asked for). | doc | A new AWAITING row: Live 12's meter ballistics and when the MIDI meter lights; M5-A5C-001. | window | M1 |
| MX-02 | **Split Stereo Pan** · 0203 d9 | A5C-003, A5C-023, A-BASE-18.1, M5-A5C-003 | The pan menu ticks from panMode and writes mixer.setPanMode (one step); a split strip draws Live's two sliders; each drag is one step of setPanLeft or setPanRight; double-click resets; the two pans' lanes come with the automation lanes. | none | Width or phase controls on the strip (Utility keeps them, ADR-0203 d9). | doc | A5C-023's split gains at the extremes; Live's lane names for the two pans. | window | M1 |
| MX-03 | **Pan law, 64-bit summing and the Processing Precision switch, measured against Live** · 0163 d2; 0205 d6; 0207 d2 | A5C-023, A3-009, A6G-006, A6G-007, A6G-010 +5 | Keep the built pan law and 64-bit summing and close them with records: AWAITING 5's readings for a -6 dBFS sine at C, 25R and 50R; the precision cancellation cases; undithered render neutrality; Live's mono sum; a Live law that differs opens a new ADR, as ADR-0163 says. | MS1 (v), MS5 | Changing ADI's law before the readings; a 64-bit default (ADR-0207 d2). | doc | AWAITING 5; M-AUDIO-PRECISION; M6-A6G-006, -007, -010. | win | M4 |
| MX-04 | **Track Delay in milliseconds, and Live's Delay Compensation switch** · 0172; 0203 d4 | A3-012, A6H-034, R10H-011, N10H-048, A-BASE-18.7 +2 | A millisecond field writes mixer.setDelay (one step), shown by D and the Track Options items; win adds audio.delayCompensation (on) and its Options item, which drives Session::setDelayCompensation; the field greys while it is off; ±1 s until AWAITING 8 reads Live's range. | none | Sub-sample Track Delay (P2, with Audio Alignment); REAPER's per-chain and per-FX PDC modes. | doc | AWAITING 8 (range, selector, Main and returns); AWAITING 7(h); M6-A6H-034. | window, win | M1 |
| MX-05 | **Returns and sends as Live** · 0203 d3 | A5C-009, A6S-001, A-BASE-18.4, L02, L03 +3 | B1: one Pre/Post switch per return through return.setSendTap, with no refusal when there are no sends; B2: new send ids through the one id helper; New's A Reverb and B Delay in the parity notes; new sends at -inf until AWAITING 7(a); return rings refused until 7(f); Pre sends follow solo until 7(d). [XC-22] | none | Per-send Pre/Post and a Pre-FX send tap (the M10 review corrected R10H-002). | doc | AWAITING 7(a) to (f) and (i); L02, L03, L10; the send knob's taper and the boxes' wrap past three returns. | window | M1 |
| MX-06 | **ADI's sends into any track, and a pan on a send** · 0203 d3 | C3-022, N10H-043, N10H-044 | The send slot's "Send to Track..." lists eligible tracks (never Main, never itself unless a return); routing.connect of kind send plus setGain, one step; Pan in a send knob's menu (routing.setPan, one step); a send into a summing group keeps its channel half. | none | Cubase's Direct Routing fan-out (P2, MX-16). | doc | A send into a non-return track is compensated and follows Hearing's rule. | window | M1 |
| MX-07 | **Solo as Live, with its options** · 0163 d4; 0203 d3, d11 | A5C-011, A6H-007, A6H-008, A6H-009, A-BASE-18.6 +2 | Worklist B5: Solo in Place from the snapshot, written by mixer.setSoloInPlace outside undo and saved with the project; Live's Options entry maps to the same field until measured; preFollowsSolo and keepAllReturns change only on readings; Solo in front and the solo bus stay registry rows labelled "not built". | none | An engine for Solo in front and the solo bus until the Live base is done. | doc | AWAITING 5, 7(c), (d), (g); M5-A5C-011; M6-A6H-008. | window | M1 |
| MX-08 | **Cue, Cue Out, Cue Volume, the metronome and preview on Cue Out; the output stage** · 0203 d3, d9, d11 | A5C-012, A-BASE-18.6, C3-023, C3-024, C3-025 +4 | Worklist B6 (outside undo, the session refreshed after each); the engine's cue bus as built until the Live reading, then the cue tap follows Live's; the engine reads audio.metronomeOutput and audio.autoMute; the browser's preview on Cue Out once the preview path exists. | none | Cubase's four cue mixes, talkback, AFL/PFL choice and Listen Dim (the Control Room, v2); a fallback pair when Cue Out is missing. | doc | A new AWAITING row: Live 12's cue tap (before or after fader and pan; the Track Activator off), Cue Volume's range and default, the metronome and preview on Cue Out. | window, engine, win | M1 |
| MX-09 | **Input monitoring, Arm on the strip, Keep Monitoring Latency, Reduced Latency When Monitoring, latency calibration** · 0203 d11 | A3-007, A3-008, A3-012, A5C-001, A5C-024 +6 | A live input node per monitored track, fed from its input (MX-25) per monitor_mode (Auto = armed and no clip playing); Reduced Latency skips compensation for input-monitored tracks; Keep Monitoring Latency shifts the recorded placement; Driver Error Compensation offsets every recording; Measure round trip by MTDM on a loopback; the window wires Arm and the toggles (C6). [XC-15, XC-34] | MS2, MS4 | Cubase's monitoring preferences (comparison only). | doc | A new AWAITING row: Live 12's monitor default per track type in a fresh Set; M3-ROUTE R1 (heard, recorded, placed); a loopback round trip on his interface. | engine, window, win | M4 |
| MX-10 | **Loudness meter on Main** · 0203 d14 | C-U02, R10H-073, R11X-025 | Main's strip draws M, S, I and TP from the engine's readout, loaded relaxed each frame; a click resets; no other strip shows them; the picture goes to him first (an addition); later the Scope, analyser and export readouts read the same meter. [XC-03] | LD7 (d) | A meter on every bus (ADR-0203 d14). | doc | The meter's cost on Main at all times (A3-24's condition, unmeasured). | window | M1 |
| MX-11 | **Native analog summing on groups and Main: the controls** · 0173; 0174; 0203 d14 | none (an ADI addition) | Summing on or off, the flavour (eight) and the drive (-12 to +24 dB) on groups and Main through the built group.setSumming ops; the drive dial is one gesture and one undo step. [XC-17] | none | Crosstalk (P2); FEATURES' stale "the master is not built". | meas | ADI only: one undo step per dial gesture. | window, win | M2 |
| MX-12 | **Small live scope and spectrum in every strip, and the analyser's big view** · 0203 d14; 0209 Q5 | E09 | A visible strip subscribes one 96-point spectrum (every 50 ms, 300 ms smoothing) and one 40-80-column min/max scope; a hidden strip holds none; a click opens the analyser's floating view or the Scope on that track; one tap per visible strip. [XC-19] | Mix View program refs | OpenGL for strips; per-client analysis on the audio thread. | inf | The program's §4.2 budgets (the audio thread within +0.2 points of one core with 16 visible strips; a worker pass under 4 ms). | engine, devices, window | M4 |
| MX-13 | **The Scope window, and a minimal SCOPE & PHASE** · 0167; 0209 d12 | R11E-026, A7B-086, M6-A6G-011 | One Scope component, shown as a window (Compare in Scope) and as the SCOPE & PHASE tab (two tracks first, eight later); heard-time alignment by stamps; Fix Delay proposes mixer.setDelay on B (one step); polarity is a per-layer view flip; a track polarity fix, if offered, sets Utility's Phase switch, never the strip's stored phase_invert. | Mix View program refs | Sub-sample Track Delay until Audio Alignment (P2); the EXO (P2, inside the analyser device). | doc | A device-output tap's stamps against a declared latency; the GCC-PHAT port against FilmCraft's own cases (gains, 0 dB SNR, echo, inverted polarity). | window, engine | M4 |
| MX-14 | **The Mix View shell and the function keys** · 0209 d9, d11; 0203 d6 | B-H01, C3-021, C5W-001, C5W-002, C6-KEYBOARD | F1 to F8 lose Live's track activators (his deviation); F4 opens and closes the Mix View only; Activate Track 1 to 8 keep their menu entries without keys; the Mix View's switch, detach and re-dock (his remark R3). [XC-09] | WI1, WI2 | A second Mix View window; cloned mixers (ADR-0063, ADR-0180). | doc | Headless: F4 toggles the Mix View once and changes no activator. | window | M1 |
| MX-15 | **The crossfader (P2, with Session View)** · 0203 d10 | A5C-010, A-BASE-18.5, B3-020, M5-A5C-010 | A crossfade gain at each assigned track's gain stage (no routing change); its storage in side tables placed by win when Session View starts; seven curves; the position and each assignment mappable and automatable. [XC-10] | none | Before Session View (P2); the Mixer View option stays dimmed until then. | doc | AWAITING 16 (does a Post send follow the crossfader, Live 11 then 12); the seven curves against the manual's chart. | engine, window, win | P2 |
| MX-16 | **Multi-output instruments, up to 64 buses per node, Direct Routing (P2)** · 0113 | A6H-012, A-BASE-17.5.2, C3-022, R10H-001, R10H-003 +4 | Buses beyond Main and Sidechain become numbered node ports; Audio From can name "track X, output n"; routing.channel_map carries partial channels; Direct Routing is several enabled outputs from one track. [XC-15] | none | REAPER's 128 channels per track (ADR-0113's 64 stands). | doc | M6-A6H-012 with a multi-bus fixture plug-in. | engine, window | P2 |
| MX-17 | **System audio as a track input (P1), and the ADI virtual audio device (P2)** · 0106; 0203 d5 | A6H-002, A-BASE-17.2 | The loopback source appears as input channels in Audio From with its resampler latency declared (P1); the virtual audio device is an output choice for Main or a bus (P2); both follow Live's chooser rules. [XC-40] | none | Bundling VB-CABLE; the virtual device before P2. | doc | Loopback capture latency measured with MTDM and compensated; the driver signed and installed (CI is not installation). | engine, win | M4 |
| MX-18 | **The Control Room (version 2)** · 0125 d5; 0203 d9 | C3-023, C3-024, C3-025, C3-026, C3-027 +3 | Deferred to version 2, with recorded constraints: monitor-only processing is workstation state, never in a render or the project; cue mixes are per-channel sends; talkback latch or hold; the Cue bus and the reference route become its first sources. | none | Deferred (decided). | doc | M-CUBASE-CUE (which route bypasses master processing; Main mute and dim against every cue path). | none until v2 | P3 |
| MX-19 | **Mixer snapshots (P2)** · FEATURES | C3-020 | Deferred; recalling a mixer snapshot is one undoable step in the one journal, never a second history. | none | Before P2. | doc | None now. | engine, window | P2 |
| MX-20 | **Quick Kick Sidechain (P2): the routing now, the target after the list** · 0209 d3, d11 | A6H-013, N10H-045, N10H-046 | The routing half is MX-24's sidechain into a device with the Pre FX tap; what the button engages or inserts waits for his list (the unified feature) and is not designed here. | none | The button, pill and drops until his list; any slot-based design. | inf | AWAITING 7(h) (which tap a new sidechain chooser starts on in Live). | none now | P2 |
| MX-21 | **The P2 analysis tabs, the AI Mix Radar and the docks** · 0209 d11, d6 | E09 | As the program's §1.24 and §4.3: OVERVIEW (a SPAN-class overlay of pinned tracks with statistics and correlation), SPECTROGRAM (reassigned, OpenGL on that view only), the EXO inside the analyser device, and the AI Mix Radar through analyze.* and a Propose changeset; parts that drew a slot wait for his list. | Mix View program refs | K-meters and PPM, the RGB waveform and chroma (additions not proposed). | inf | The program's X19 to X21 (budgets, the spectrogram's rendering, the EXO's buffer). | window, engine, devices | P2 |
| MX-22 | **The Live mixer and routing measurement round** · 0108 | L02, L05, L06, L09, L10 +10 | Generated Live 12.4 sets per case family (pan law; sends 7(a) to (i); Track Delay; group mute; the crossfader and Post sends; the cue tap; meter ballistics; monitor defaults), rendered and compared through the harness; GUI cases through the logger's case protocol; win relays only records labelled with the Live build. [XC-38] | MS3 (v), MS1 (v), MS5 | Answers from manuals or controller palettes; setting Live's parameters through its API for automation or undo cases. | unk | Every case above; AWAITING rows 14 and 16 need a fresh Live 12 profile. | win, measurement seat, director | M4 |
| MX-24 | **Sidechain keys reaching a device, with Live's three taps** *(was refinement)* · 0203 d3; 0058 d5 | A6H-008, A6H-009, A6H-013, A-BASE-17.5.1, N10H-045 +7 | A sidechain row may end at a device; the realiser connects the source's tap node to that device's sidechain bus, compensated; the tap sits in a side table keyed by routing id (absent means Post Mixer); a key stays unheard; bounces and renders keep keys; Live's sidechain and tap choosers on devices that declare a sidechain input. [XC-07, XC-15] | RK1 (d) | Reason's compensation gaps as targets; REAPER's blanket PDC disable with feedback. | doc | AWAITING 7(g) (does ducking stop when the key is muted or solo-silenced) and 7(h); a key into a fixture compressor arrives aligned at each tap; a broken path reports itself, never a silent zero. | win, engine, window | M4 |
| **DECIDED · Project, export and data safety (22)** | | | | | | | | | |
| PD-00 | **Main green on macOS for Save As, the ZIP and NI Stems** · 0203 d6, d23; 0205 d8 | DR-12-05, DR-13-11 | Relative media paths computed between folders spelled as SQLite spells them, so a rebased path resolves from the reopened copy (Save a Copy, Save As, Collect and the ZIP); CoreAudio's AAC priming read from its packet table; closed: #461 merged at 5a6ae08b, run 42012235250 green on all 20 jobs; ui.yml's cancel-in-progress still to come off for main (WTS-04). [plan] | none | Skipping or loosening the failing checks; striking N3 or N30 before macOS passes. | meas | Met at 5a6ae08b; a Save As through a symlinked folder plays the relocated audio. | win | M0 |
| PD-01 | **Save As and Save a Copy** · 0203 d6; 0142 d9; 0147 d1 | A6H-068, A6H-069, DR-12-05 | The built semantics stand (a SQLite backup into a stage, media paths rebased on every branch, the WAL folded, no-clobber publication, saveAs switching the same Store). Added: every device snapshotted first (PD-03); the copy's session lock cleared (after PD-05); Live's "replace?" asked by the save dialog, then an atomic replace from the stage. Window: worklist A1 and A4. [XC-02] | R1 | Audacity's SafetyFileName and MoveProject; REAPER's copy-media-on-Save-As options. | doc | AWAITING 13 (which file you are left in); worklist A1's checks (the next edit lands in the new file; the old file unchanged; undo reaches back past Save As); a window-only plug-in edit survives in the copy. | window, engine | M3 |
| PD-02 | **Collect All and Save; Collect and Export ZIP** · 0203 d6; 0127 d3-d4; 0143 d2-d3 | A5C-049, A6H-068, DR-12-05, DR-12-07 | The built semantics stand; each staged copy and the ZIP flushed and their folder synced before publication; Collect and Export on a worker with a read-only connection, progress and cancel; Live's Collect dialog choices on a location filter the store gains; Live's Unused Files with freeze-cache's collector. | R1 | Collect and Embed and .adibundle (superseded); any file copy of the live database. | doc | The publish order create, sync_file, rename, sync_dir for every collected file; a failed export leaves an existing ZIP byte-identical; the Live reading of the Collect dialog. | engine, window | M3 |
| PD-03 | **Ctrl+S Take Snapshot, the daily snapshot, plug-in state written** · 0203 d6, d12, d13; 0128; 0140; 0142 d4, d9; +1 | A4-010, A5C-048, R3-001, R10H-061, DR-12-02 +1 | ParamOps::snapshotAll writes one device.loadState per attached device whose chunk changed, before Take Snapshot, close, Save As, Save a Copy, Collect and any backup. DailyHistorySnapshots is built and polled only together with Q-A's rule. The silent plug-ins on his machine are counted for ADR-0038. [XC-02] | in tree / own | An autosave timer; a periodic quiet snapshot of silent plug-ins until the count shows a need. | doc | A fake device whose stateEpoch stays 0 while its chunk changes gets exactly one loadState at Take Snapshot and at close; an unchanged chunk writes nothing; the silent-plug-in count. | engine, window | M3 |
| PD-04 | **Atomic, durable writes for every file beside the project** · 0143 d3; 0147 d1; 0152 d2; 0127 d5 | A11R-006, R11X-011, R11X-040, A5C-049 | One primitive pair in publish_file: publishFile (no-clobber) and replaceFile (rename-over); both flush the staged file before the rename (F_FULLFSYNC on Apple) and sync the folder after; Windows MoveFileExW with capped retries on sharing and lock errors; stages named .adi-<kind>-<session>-<n>, listed in the session marker and swept only after taking a dead session's lock; the settings bundle never truncates the user's file. [plan] | R1 | JUCE's TemporaryFile and ReplaceFileW; unflushed .tmp-then-rename (VCV Rack, Ardour); Tracktion's temp edit copy. | doc | The craft-atomic list on all seven ABIs; a failed bundle export leaves the old bundle byte-identical; a dead session's stages swept, a live one's kept; no bare std::filesystem::rename in src/adi. | engine | M3 |
| PD-05 | **Crash detection, the recovery notice and reopening** · 0047 d3; 0003; 0203 d1 | A5C-048, A4-010, A5H-034, C5W-029, DR-12-01 | An OS-locked session marker per running instance (session.json: project path and UUID, stages, recordings in flight, clean exit); the session_lock row with a 20-30 s heartbeat; Store::open returns an OpenReport (unclean, ops since a clean close, WAL bytes, last op, writer); a live lock opens read-only naming host and PID; copies clear the lock; modified_by on every write; one launch question; holding Alt at launch skips the scan. [XC-24] | R2 | FilmCraft's snapshot journal (every op is already in the file); Live's version-specific replay. | doc | The crash fixture (three ops committed, copy reopened: unclean, three ops); a second writer refused naming host and PID; a heartbeat older than 60 s can be broken; Live's crash dialog read on a disposable Set (M5-A5C-048). | engine, window | M5 |
| PD-08 | **Back up project to cloud, and a project kept in a sync folder** · 0182 d3; 0203 d13 | DR-12-16, A5H-014 | File > Back Up Project... asks for the folder each time, runs Collect and Export on a worker with progress and cancel, and writes "<name> [date time].zip" without replacing anything; a project kept inside a known sync folder gets one Status Bar line. | R1 | A scheduler or stored credentials (A6-08); S3 sync (that is collaboration). | doc | Sync-folder detection for OneDrive's known folders on Windows and iCloud Drive's path on macOS. | window, engine | P2 |
| PD-14 | **Templates and the default Set** · 0203 d9; 0209 d10 | Live 12 manual ch. 5 (no bundle row) | A template is an .adi written by saveCopy into the User Library's Templates folder with a flag row in adi_meta (no schema change); New from a template starts at its state as the root, not as an undo step; Save as Default Set sets files.startTemplate. [XC-13] | in tree / own | REAPER's track and project templates as a separate system. | doc | The Live reading of Save Live Set As Template (what it keeps; where it goes); New from a template has no undo step and its relative media paths resolve. | engine, window | M5 |
| EX-01 | **One consistent revision per job, and media identity** · 0071 d1-d5; 0205 d5 | A11R-001, A11R-004, A11R-005, A11R-007, R10H-079 +5 | Keep ERRATA-11's four ownership moments (job values at enqueue, membership at Queue::run, one revision and the names at each job's start, publication last); add a run manifest with the journal head each job read; every referenced media file's BLAKE3 checked at job start and again before publication. [XC-20] | EX3, A4 | An enqueue-time project snapshot (R10H-079); MAGDA's revision policy as a second job system. | doc | A11R-001's case (edits to the project, a name and a region before and between jobs; record what each file used); a WAV rewritten after its job starts fails the job. | engine | M3 |
| EX-02 | **Sources, routes and taps** · 0071 d1; 0203 d1, d3; 0205 d9, d10 | A4-001, A4-002, A4-008, R11X-001, R11X-002 +9 | Source {track, tap (PreFx, PostFx, PostFader, PostMixer), route (Direct, ViaMain, MainFxOnly at P2)}; ViaMain as an offline audibility override that keeps keys and sidechains live and never uses solo; batch expansion with a cardinality preview and unique names; the dependency closure as a preview, not a filter; impossible combinations refused at add. [XC-14, XC-15] | EX3 | Tracktion's useMasterPlugins read as Live's mode; stems by soloing each track; REAPER's parent-channel projection and automatic mono (P2). | doc | M2-HOST-EXPORT's probe across Direct, ViaMain, MainFxOnly and Main (linear stems sum to Main); Live readings of Include Return and Main Effects and of All Individual Tracks. | engine | M5 |
| EX-03 | **Render as Loop, tails and gapless region batches** · 0070 d1-d4; 0071 d1; 0205 d9 | A4-002, A11R-003, R11X-006, R11X-007, R11X-036 +4 | Render as Loop is Live's two passes (one unwritten pass, latency-compensated); Job.tail is Off, Fixed or UntilSilent, its bounds stored apart; refused inside a gapless batch; the warm-up from zero stays and is not a tail; the gapless cycle-marker batch is P2 (EX-12). | EX3 | Tracktion's wrapRemainder fold as Render as Loop; REAPER's per-region take-FX reset (a comparison). | doc | An impulse before the range and a decaying effect beyond it, region jobs against a continuous pass; where Render as Loop's first pass starts in Live. | engine | M5 |
| EX-04 | **Deliveries, formats, Live's rate rule and mono** · 0132 d5; 0203 d18; 0205 d9; 0207 d2 | A4-003, A4-004, A6G-010, R11X-019, R11X-020 +6 | Job.deliveries (container, sample format, dither, codec, suffix); the source captured once in double at the graph rate, then each delivery quantized or encoded, decoded, checked and published; a capability matrix refuses impossible combinations; FLAC equals the PCM WAV bit for bit; LAME gets the undithered double; below the project rate process, then convert with soxr; Float64; AIFF-C fl32; Convert to Mono; Create Analysis File into the analysed folder. | EX1 (v), EX7 (v), EX9 | JUCE's format writers and its LAME format (render/ is JUCE-free); an ffmpeg command-line pipe; libsndfile as the writer; FilmCraft's Rust encoders. | doc | FLAC 16 and 24 decoded equals the PCM WAV of the same capture; every MP3 frame MPEG-1 Layer III at 320 CBR, delay trimmed exactly; the refusal matrix; Live readings (FLAC depths, MP3 settings, mono law, SRC quality). | engine | M5 |
| EX-05 | **Normalization, statistics, the brickwall and the stage order** · 0203 d14; 0205 d8, d9 | R10H-073, R11X-021, R11X-025, R11X-027, R11X-034 +6 | One documented stage order, each adjacent pair tested: capture, loop and tail windows, SRC, channel conversion, analysis, normalization gain, the optional true-peak brickwall, then per delivery dither, quantize, encode, decode and check, metadata, publish. Statistics from the one meter; "together" measures the summed capture; an analysis-only run never processes; the brickwall is off by default and never acts in Peak mode. [XC-04] | LD1, LD2, LD3 (v), LD4 (d), LD5, EX4 | libebur128's LRA (superseded EX2); ADI-L's detectors as the brickwall until his list; the EBU WAV set (not usable). | doc | Intersample-crest fixtures stay under the ceiling after brickwall and dither; exact length and alignment; the synthesised Tech 3341/3342 suite; Live's Normalize target and whether it is sample or true peak. | engine, devices | M5 |
| EX-08 | **Export metadata** · 0132 d2; 0205 d9 | R11X-023, R11X-024, R11X-042, C4-014, DR-13-07 | A typed Job.metadata; bext v2 with the loudness fields from the checked output (0.01 LU), an R99 originator reference seeded by the job, origination time and coding history; cue markers on the time map shared with trim and pad; Vorbis, ID3v2 and MP4 tags; header fields patched after the check. | EX6, EX8 (v), MD8 (v) | An ACID tempo chunk by default (opt-in only); libsndfile's adtl handling. | doc | Every bext v2 field parses strictly; the loudness fields equal the report within 0.01 LU; markers at the first, an interior and the last sample; non-ASCII text. | engine | M5 |
| EX-11 | **The Export page, and where renders run** · 0205 d5, d9; 0203 d2; 0071 d2, d3; 0210 d3 | A4-001, A4-002, A4-003, A4-004, A3-031 +4 | Live's Export Audio/Video dialog first (Rendered Track, Start and Length, Include Return and Main Effects, Render as Loop, Convert to Mono, Normalize, Create Analysis File, Sample Rate, PCM and MP3, Video), ADR-0205 d9's additions below a divider; renders in process: instances created and given state on the message thread, processing on a render worker, serviceHost marshalled; LANES §3b gains the Export item. [XC-26] | in tree / own | Two dialogs; the AI pressing render; a render child process (struck by ADR-0203 d8). | doc | A VST3 and a CLAP rendered from the page with the window responsive, the thread of every call logged; the parity checklist from manual 5.1.3; AWAITING 16's dialog defaults. | window, engine, win | M5 |
| EX-12 | **P2 export abilities** · 0203 d14; 0205 d1-d4, d9; 0070; 0206 d4 | R11X-022, R11X-026, R11X-029, R11X-030, R11X-031 +8 | Matrix cells add ordinary jobs; After Export is a separate undoable import once the files exist; the batch converter feeds file sources through the same post pipeline; live capture is a separate session-owned writer; the .adiviz telemetry shares the export pass. | EX3 | REAPER's keep-partial (P3); a dry run that edits the project outside ordinary ops. | doc | ADR-0070 d2's concatenation bit-identity; After Export undone in one step. | engine, window | P2 |
| IC-01 | **The converter framework and DAWproject both ways** · 0127 d7; 0011; 0021 | C4-017, C4-018, DR-13-17, M3-EXPORT-INTERCHANGE | A reference to TM-IC-03, the owner: converters JUCE-free in src/adi/interchange, prepare on a worker and commit one step (actor import), a per-field loss report, extension rows for unmapped data, VST3 class-id matching, the neutral clip.createMidi. [XC-11] | IC1, IC2, IC3, IC4, IC5 (v), IC6, IC12 (d) | Tracktion's JUCE glue; .als import (not proposed); DawVert as code. | doc | DP-1 to DP-6; DP-7 at his Cubase. | engine, window, devices, win | M5 |
| IC-02 | **MIDI files, Live's way** · 0203 d7; 0004 | A6H-028 | A reference to TM-IC-02, the owner: Live's Import MIDI File and Export MIDI Clip with no options box (P2); the source PPQ remembered; unknown chunks parked. [XC-11] | IC7 | The harvest's P1 placement; libsmf (glib). | doc | SMF-1 to -4; AWAITING 13. | engine, window | P2 |
| IC-03 | **OTIO and AAF (P2), OMF read (P3) and the ArtCraft bridge** · 0206 d3-d5; 0127 d7; 0207 d3 | C4-016, C4-017, DR-13-17, DR-13-18 | A reference to TM-IC-04, the owner: the integer time base first; AAF import through LibAAF; a clean-room AAF and compound-file export with essence from the queue; an OTIO subset; drop-frame as a new table; .fcproj and .scraft never read or written. [XC-11] | IC8, IC9 (v), IC10, IC11 | OTIO's C++ library; the AMWA AAF SDK; ptformat; an FFI to FilmCraft. | doc | OTIO-1; AAF-1 to -3. | engine, window | P2 |
| CO-03 | **Incoming changes previewed; collisions kept both ways** · 0182; 0148 | DR-12-12, DR-12-13, DR-12-14 | Preview is CO-01's rebase run in a rolled-back transaction, returning the diff and the collisions; Apply commits the same rebase as one transaction; ghosts in a sync_ghosts table the engine never reads; Combine blends in the lane's own domain, then thins; "a clip becomes a take" waits for take lanes. | C1, C2 (v), C9 (d) | Auto-applying incoming batches (against the directive). | doc | Two Stores flag the same collision; disjoint edits merge silently; the preview leaves the .adi's bytes unchanged; Combine at 0, 50 and 100 % equals Keep Mine, the dB mean and Adopt Theirs. | engine, window | P2 |
| CO-05 | **Shared media and the bucket client** · 0182; 0032 | DR-12-11 | Objects media/<blake3> in the owner's bucket; ADR-0182 d4's selection (all, none, per file); an S3 client with SigV4 and a conditional-write probe; a keychain adapter per platform; a resumable upload queue that survives a crash. [XC-40] | C4, C7 | Git of the text projection as the carrier. | doc | The SigV4 vectors byte for byte; an upload resumes after a crash; conditional create against a MinIO job. | engine, window | P2 |
| EX-10 | **NI Stems: normalization and macOS** *(was refinement)* · 0205 d8; 0191 d4; 0204 d5 | DR-13-11 | Refuse Peak and Loudness normalization on an NI Stems job now (one check, one test); later one gain measured on the premix for all five streams, checked after AAC; the macOS fix as PD-00. [XC-05, XC-04] | in tree / own | Writing a Pioneer stem container (none exists; AWAITING 17). | meas | An NI Stems job with Loudness normalization is refused; mac-probe on both AAC suites. | engine, win | M5 |
| **DECIDED · Control, AI, surfaces and testing (23)** | | | | | | | | | |
| CTL-02 | **The registry describes itself (JSON Schema export)** · 0016; 0025; 0039 d2; 0168 d5 | DR-14-01, DR-14-04 | Field gains optional description, enum values, bounds and item limits (existing initialisers stay valid); opInputSchema() emits closed objects with MAGDA's default bounds; validateSubmission enforces the same metadata at submission only (replay untouched); adi_tool ops --schema; descriptions first for the ops the agent may emit. | OS1 | A second validator (MAGDA's validateJson); hand-written tool schemas. | doc | AC-24 (a closed schema per op; a payload passes the schema exactly when validateSubmission accepts it; out-of-range input refused naming the key); every payload the shell suite submits still validates. | engine | C |
| CTL-06 | **Feed subscriptions and the param and transport methods** · 0181 d1-d3, d5; 0188 d4; 0198; 0124; +2 | DR-11-05, DR-11-06, B3-013, A6P-003, A3-018 | param.subscribe {names} returns a snapshot then per-tick diffs (at most one per parameter per frame); a stalled client is dropped and resynced, and disconnected after 100 refused flushes; param.get, set, begin and end through parameterGesture into ParamOps (one undo per gesture); param.nudge through SRF-04; every write stamps lastTouchedBy; transport, history-head and selection subscriptions. | CA1 | A second feed; deltas in the log. | doc | AC-8, -9, -10, -12, -13 (a surface's set commits as user and surface:<name>; begin, twenty sets and end are one undo entry; a nudge at the top writes nothing); SF-8. | window, engine | P2 |
| CTL-07 | **Visual array streams (P2)** · 0184 d4; 0188 d6; 0175 | DR-11-06 | streams.subscribe {name, points, rateHz} returns a stream id; binary frames with a 16-byte header (signed i64 timestamp) and float32 values; names track.<id>.meter and .spectrum, master.correlation, device.<id>.scope; producers serialise MX-30's worker outputs and run no FFT of their own; raw audio a capped stream, marked while held; ui.mixview.focus for the Stream Deck. [XC-19, XC-34] | CA1 | Arrays as JSON numbers; analysis on the audio thread for a client. | doc | AC-20 (the header decodes; frames no faster than the UI clock; raw audio capped and marked); the first consumer is his adi_visualizers_and_meters. | engine, window | P2 |
| AGT-01 | **The changeset as a service, with the decided caps** · 0145 d9; 0148; 0203 d20; 0146 | DR-14-02, DR-14-12 | maxOps defaults to 6000 and follows agent.maxOpsPerRequest; agent.maxOpsPerMinute enforced on Apply commits; each changeset gets an id, a client and a request id (one queue entry per request id); an app-scoped ChangesetQueue, never in the .adi; Propose previews and queues; Apply applies unless an always-confirm rule fires; a stale changeset is refused, never rebased. | A4 | Silent rebase; per-op editing (open). | doc | test_changeset extended (the cap follows the setting; one entry per request id; the 6001st op in a minute refused at Apply); the preview's latency at 6000 ops on a 10,000-clip project (unmeasured). | engine | C |
| AGT-05 | **The user's side: review, consent, chat and entry points** · 0148; 0115; 0104 d3 | DR-14-02, DR-14-03, DR-12-10 | A review panel (a unified diff and a per-op before and after list; Apply, Reject, a stale banner offering a fresh preview); the consent prompt; chat in the collapsed tab at the foot of the browser; entry points (the palette per Q-G, a marker's prompt run on the user's action, model-backed buttons producing ordinary changesets); agent remarks read as context only; no panel commits on its own. | none | Per-op editing (later); a chat box as the only interface. | unk | A 40-op arrangement changeset shown to him in an experiment build (diff rendering is AI-AGENT §9.3's open problem); Apply is one undo step; the stale banner appears when the head moves. | window | P2 |
| AGT-06 | **The sidecar `adi-agent`, its budgets and its providers** · 0168 d1-d5; 0086; 0064 d1, d5; 0204 d2 | DR-14-05, DR-14-01, DR-14-13 | pi-agent-core, pi-ai and pi-mcp at exact pins; started on demand as a child process; JSON lines on stdio; tools from adi-mcp, run sequentially; a budget guard (8 steps, 32,000 tokens, 8 mutations, 2 minutes, at most 2 identical calls); a run that read remark text is tainted and queues even at Apply; local, cloud-key and LAN providers, all behind consent; credentials in the keychain. | A5 (v), A6 (d), A7 | Forking OpenClaw; Docker as the default; MAGDA's in-process models and its one-tool-per-op MCP surface. | doc | vitest with the faux provider (a run completes; a repeated call stops; a parallel propose is refused; wall time completes calls with errors); one real-provider smoke per provider class, with consent. | sidecar seat, win | P2 |
| AGT-07 | **Analysis tools the agent can call** · 0168; 0064 d1 | DR-14-04, DR-14-08, DR-14-09 | analyze.loudness, spectrum, transients, key, tempo, harmony, groove, masking and headroom as jobs against a WAL snapshot, never blocking edits; per-file analyses read from ADIA (computed on a miss by the same analyser); project-level analyses on MX-30's kernels; output schemas described as ops are. [XC-19] | LD1, LD2, WT1 (v), WT3, MD6 (v) | Raw audio to a model by default (only under the leaves-the-machine confirmation). | inf | Each tool against a fixture with a known answer (a -14 LUFS file, a 120 BPM loop, a masking pair); the AGT-08 tasks that need each tool. | engine | P2 |
| SRF-01 | **The strip's parameter drag is one undo step** · 0124; 0181 d2, d5 | DR-11-05, A6P-003 | parameterGesture calls ParamOps::uiEvent on the steady-clock milliseconds the drain uses, and commits everything appended even when this event was refused (an older gesture may have completed); ParameterFeed::touched(id, param, source) is called; the engine's two caveats stay named. | in tree / own | Committing only on release (Live sounds during the drag). | doc | Worklist A3's test (Begin, three Values and End are one undo step whose first op is the opener; undo restores); a refused event still commits a completed gesture; SF-9's device half. | window | M1 |
| SRF-04 | **One resolver for nudge, acceleration, conserved ticks and takeover** · 0181 d1; 0188 d4; 0203 d21; 0130 d4, d7 | A6R-004, A3-018, DR-11-05, DR-11-11 | resolveNudge returns an absolute normalized value, or nothing when clamped; switch and menu parameters move a step per tick; Fine divides by 10; ADR-0188 d4's acceleration only for unit-tick sources; ticks conserved in an atomic per-binding counter; a TakeoverGate (None, Pick-Up, a provisional Value Scaling); relative bindings bypass takeover; one setParam per settle. | SU7, SU8 (t), SU9 | MAGDA's router (it overwrites pending ticks); Mixxx's threshold and time override; helgoboss-learn's Value Scaling (not usable; a hypothesis only). | doc | SF-5 to SF-7 (a thousand ticks while the message thread is held 50 ms move exactly a thousand steps); AWAITING 14 and SF-22c in Live 12. | engine | M3 |
| SRF-05 | **MIDI ports, the Remote route and feedback** · 0203 d1; 0130; 0195 d3; 0210 d3 | A3-010, A6R-001, A6R-002, A6R-003, A6U-013 +2 | One port host (window, src/juce/app_midi_ports.*) on JUCE 9's UMP API with JUCE_USE_WINDOWS_MIDI_SERVICES off (the Win32 backend, named as such in Settings; the service is SRF-16 at P2, since it needs Windows 11, an SDK pin and a NuGet package no CI leg has); hot-plug, identity healed by identifier then name, per-port Track, Sync, Remote and MPE; the Remote route to the message thread (the Focus Dial first); the Track and Sync route as timestamped lock-free rings into the engine; mapped messages consumed first; win gives midi.port.* a home. [XC-18] | SU10 (v), SU1 | libremidi; the classic MIDI 1.0 device API (ADR-0188 d7 rules UMP). | doc | SF-1, SF-2, SF-21 on his Windows 10 box and on CI; SF-22b (is a mapping specific to its port when Remote inputs merge); the app starts with no ports and says so. | window, engine, win | M3 |
| SRF-16 | **The Windows MIDI Services backend (MIDI 2.0 on Windows)** · 0188 d7 | DR-11-01 | ADR-0188 d7's P2 item as its own row: JUCE_USE_WINDOWS_MIDI_SERVICES on behind SRF-05's port-host seam; it needs a Windows 11 build with the service (JUCE 9.0.2: "at time of writing" the Canary insiders build), Windows SDK 10.0.20348 or later pinned on the Windows CI legs, the Windows MIDI Services NuGet package in a local repository on those legs (it is not in the official feed), MSVC, and the installer carrying the service's SDK package or telling the user; JUCE falls back to Win32 without it, with no MIDI 2.0; Settings names the backend. The director's machine runs Windows 10 19044, so it is never exercised there. [plan] | in tree (JUCE 9.0.2's UMP endpoints) | Claiming MIDI 2.0 on Windows from a Win32 run; JUCE's older WinRT backend. | doc | A UMP MIDI 2.0 device enumerated and read through the service on a Windows 11 seat; the Win32 fallback exercised when the SDK is absent; the installer's prerequisite check. | window, win | P2 |
| SRF-06 | **The Master Focus Dial (P1)** · 0130 d1-d7; 0188 d4 | DR-11-11 | Hover targets native controls, touch targets plug-in parameters; push locks, Shift divides the step by 10; a HUD reads [Device] → [Parameter] → [value], cyan while following, amber while locked; a turn goes through SRF-04 and the capture, one op per settle. | none | A second dial (ADR-0130's open item). | doc | SF-20; the acceleration on his own encoder. | window | M3 |
| SRF-08 | **The surface host: six slots, MCU, HUI, profiles, follow, lock and pin** · 0203 d1, d10; 0181 d4; 0125; 0188 d4 | A6R-002, A3-017, A6R-012, DR-11-03, DR-11-05 +4 | Six App-scoped surface slots (Live's Mackie entries; HUI as ADI's first-class addition; declarative JSON profiles); auto-add; instant mappings as driver state, never rows (ADR-0181 d4's wording amended); Lock to... per surface; MCU and HUI feedback rules; output diffed per tick and sent off the message thread; button meanings measured (SF-22a); no script language now; grid controllers at P3. | SU3, SU5, SU9, SU1, SU7 | Ardour's mackie_control_protocol bindings; Live's MIDI Remote Scripts (not usable); MAGDA's Lua (a later ADR); Push (Q-M). | doc | SF-10 to SF-13, SF-21; SF-22a with an MCU or an emulator in Live 12; whether Live saves Lock to... in the Set; all 48 device files load. | engine, window | P2 |
| SRF-09 | **OSC (P2)** · 0185 d1, d4; 0188 d7; 0181 d2 | DR-11-07, DR-11-08 | Off by default, on the interface the user picks, with an optional client list; ADI owns the UDP read loop (juce_osc only encodes); a fixed namespace (/adi/transport, /adi/track/{n}, /adi/master, /adi/focused); writes as user and osc:<client>; feedback on change, never echoed into the flush its value arrived in. [XC-40] | SU1, SU11 (d) | Ardour's liblo surface; liblo, oscpack and tinyosc. | doc | SF-16; TouchOSC as a plain-OSC client on his network. | engine, window | P2 |
| SRF-10 | **OSCQuery v1 (P2)** · 0185 d4; 0188 d7 | DR-11-08 | HTTP GET only (/, a path, ?HOST_INFO); the tree built from the feed with stable ids, float types, 0 to 1 ranges and ACCESS 3; DNS-SD advertises only while on; values travel as OSC under SRF-09's rules; version 2 (LISTEN and the rest) waits for a transport ruling; FEATURES' TouchOSC and Lemur wording corrected. | SU6 (d) | organicui's CORS * and file endpoints; Servus; a read-only mode; version 2 now. | doc | SF-17 (ossia score's explorer and Vidvox's browser load the tree and move a value). | engine, window | P2 |
| SRF-12 | **TUIO and touch (P3)** · 0185 d6; 0203 d5 | DR-11-10 | Windows touch through JUCE's native multi-touch, one source per finger through the mouse's gesture paths; TUIO 1.1 over OSC on UDP 3333 on the same path, off until opened. | none | Before P3. | doc | Two faders moved at once leave two undo entries; a TUIO cursor from reacTIVision's simulator. | window | P3 |
| SRF-13 | **The ADI Stream Deck + XL plug-in (P2)** · 5 Nov ruling; 0181 d4; 0188 d4; 0209 d7, d9, d11 | DR-11-04, DR-11-05, DR-11-06 | A Node plug-in (SDK v2, exact pins, device type 13) carrying ADI's name; pairs once and sends a bearer token; follows the selection or pins; in the Mix View follows the hovered row, the focused row or a CTZ group; dial turns as param.nudge, touch or push as one gesture; the touch strip shows the parameter's text; [REF], solo, page and bank keys; his 13 controllers re-pointed at the API, keyed on plug-in identity, driving his builds as ordinary plug-ins. | SU2 | A browser-page plug-in (no Authorization header; throttled timers); Studio OS's ws-server (no Origin check); per-channel ADI-Q and ADI-L pages (withdrawn). | doc | SF-19 (pairs once; a dial turn is one undo step; every write is surface:<name>; the 13 controllers resolve every role); his pass on his Stream Deck + XL. | win | P2 |
| WIN-01 | **The function keys do functions (a defect)** · 0203 d6, d12; 0209 d9 | DR-01-01, A5W-010 | F1 to F8 lose the activator defaults (the eight commands stay, unbound, for the palette and the Shortcuts page); F4 is the Mix View; F3 and F7 are held for Session View and offline processing; parity-7.6 row 6 amended; a golden of every preset's default map with a no-clash check. [XC-09, XC-36] | WI4 | Live's F1 to F8 defaults, unless he asks for them back. | doc | The default map of every preset has no clash (it fails today on F4); F1 to F8 activate nothing, F4 opens the Mix View, F3 and F7 are unbound. | window | M1 |
| WIN-05 | **Shortcut editing, reference presets, non-Latin layouts and key dispatch (second half)** · 0125 d4; 0180 d6 | A5W-015, C5W-015, C5W-016, C5W-017, C5W-018 +1 | Canonical chord text; foreign key files read; Steal, Keep or Refuse; clash told apart from shadow; reserved system chords refused per OS; named sets that move across OSes; search by name or pressed key; Cubase and Bitwig presets from their documentation; a native physical-key source for non-Latin layouts; a provisional dispatch order; keys typed into a native plug-in editor never reach ADI. | WI4 | Macros (one key running several commands is not one journal op); craft's egui UI. | inf | Under a Hebrew layout on Windows and the Mac, Ctrl+Z undoes and Z and X zoom; a set exported on Windows imports on the Mac; A5W-015 measured in Live 12. | window | M3 |
| WIN-06 | **The command palette (Ctrl/Cmd+P)** · 0104 d3, d4; 0203 d6 | DR-14-03, DR-02-01 | A popup over the focused window from any window; fzy scoring on UTF-32 (Hebrew matches as it is); prefix-routed providers (commands with key hints, disabled ones listed with their reason; V/ for ADI's devices then plug-ins, inserted on the selected track in one step; S/ and P/ once the store exists); free text per Q-G; closes on focus loss. | WI5, WI6 | Helio's lib_fts matcher; setlocale. | doc | Window-infra acceptance 22 to 28 ("hsb" ranks Hide/Show Browser first; V/ inserts as one undo step); fzy's 24 cases with exact scores. | window | M3 |
| WIN-07 | **Key Map Mode, MIDI Map Mode and the Mapping Browser (P2)** · 0180 d6; 0195 d3 | A6R-005, A6R-006, A6R-007, A6R-011, A5W-012 +3 | Ctrl/Cmd+K and Ctrl/Cmd+M overlay the assignable controls; one message or key binds through learnControllerTarget; the Mapping Browser lists manual mappings for the active mode (never instant mappings); notes act by target kind; Key Map rows travel in the .adi; Clip View mappings act on the selection. | SU1, WI4 | Per-strip Learn buttons (Live learns through Map Mode). | doc | SF-14; A5W-012 and A5W-013 measured in Live 12. | engine, window | P2 |
| WIN-08 | **Live's Second Window (P2)** · 0203 d1; 0063 d1; 0101; 0117 | A5W-001, A5W-002, A5W-003, A5W-004, DR-01-02 +1 | Ctrl/Cmd+Shift+W opens it; Tab swaps Session and Arrangement between the windows; Shift+Tab or F12 moves the Detail View; the Browser and the Info View stay in the main window; window.zoom keeps second@<display>; a normal window kind; a FEATURES row at P2. | WI1, WI2 | Before Session View. | doc | M5-A5W-001 to -004 in Live 12. | window, win | P2 |
| WTS-11 | **The Live side: L01-L10 captured, never read from the manual** · 0108 d1, d4, d5 | L01-L10, M-LIVE-UI, SRC-TEXT-ONLY | The measurement seat captures L01 to L10 in Live 12.4 (Mac-only rows on his Mac); each L-row gets an ADI script; behaviour rows answered from ui.inspect; picture rows on side-by-side sheets at one scale with a 50 % overlay; win relays dated results into the window's "Measure on Live" lines and AWAITING by the relay rule. [XC-38] | none | Striking a row from a manual or a static controller palette. | unk | AT-23; all ten L-rows captured, each with its build, hash and operator. | measurement seat, window | M4 |
| **REFINEMENT · Editing and the arrangement (18)** | | | | | | | | | |
| CT-01 | **Take lanes exist; only the main lane plays** *(was decided)* · 0203 d1 over 0037; 0182 d5 | A6H-027, A6H-029, R10H-020, R10H-021, R10H-022 +2 | A NULL lane_id is the main lane; a take-lane clip plays only while its lane is the track's auditioned lane (session state, one per track); lane.* ops plus clip.setLane; every producer takes its lane from EditScope; SPEC §6.2 reworded to Live's copies model and ADR-0037's "comping follows Cubase" recorded as superseded by ADR-0203 d1 in R-d's comping ADR (why this is a refinement, not decided work). No schema change. [XC-21] | CP5 | REAPER's lane masking and play-all-lanes layering; Bitwig's comp-region sliding. | doc | M6-A6H-029 (does audition silence the main lane; is it saved; undoable); a new lane's name; lanes through the text projection. | win, engine, window | M6 |
| ED-03 | **One placement rule: EditScope and Live's overlap rule** · 0203 d1, d7; 0021 | A6C-077, C-R04, C5W-023, R11E-013, R11E-015 +3 | One engine planner (edit_plan): every placement clears the destination span on its track and lane with eraseRange, then places, in one arranger.splice step; describe() names what is split, removed and shifted; EditScope (tracks, lanes, range, flags) with Live's presets; the window stops sending raw clip.move and clip.resize. Undo restores rows byte for byte. [XC-33] | RE1, RE3, RE5 (d), RE7 (d) | Cubase's Shift override and Lock Event Attributes; REAPER's item-group lead/follow; razor areas (RE8) deferred; editing.mutedClipsAsDeleted stays an off option. | doc | Live 12.4 overlap cases (middle, start, end, whole; warped and unwarped; audio and MIDI); a seeded 40-edit fuzz on 3 tracks, overlap-free after every commit, undo-all back to the start digest; drop latency on a 100-track project. | win, engine, window | M2 |
| ED-04 | **Left trim, the clip's start, and Crop** · 0203 d7 | A6M-033, A6M-034, A6C-076, R11E-003, C-R03 | prepareTrim moves the content offset (ticks and the ns clock) with a left edge; audio stops at source frame 0, MIDI does not; fades clamp. Crop by Live's rule (unlooped start to end; looped, loop bounds; a selection overrides); MIDI notes outside removed in the same step; audio writes no file. | RE4 (d), RE1, RE3 | REAPER's move-item-without-contents and extra trim variants; Cubase's Crop of events wholly inside or outside. | doc | M6-A6M-033 (crossing notes' expression on Crop); M6-A6C-076; a left trim of a looped clip at the loop start and of a warped clip at a warp marker. | engine, window | M2 |
| ED-05 | **Slip content inside fixed edges** · 0115 d2; 0108; 0203 d1, d12 | A6C-076, R11E-003, R10H-035, B-A07 | Live's slip gesture (Ctrl+Shift-drag on Windows, Shift+Option on macOS; Alt or Cmd bypasses snap) on warped audio and MIDI, extended to unwarped audio as ADR-0115 d2's addition; one op moves the content offset and ns clock with edges and fades fixed; one undo step per drag. | RE2, RE1 | ADR-0115 d2's plain Shift-drag binding gives way to Live's modifier; REAPER's roll and slide deferred. | doc | M6-A6C-076 on Live 12.4 (Windows modifier, snap, an unwarped clip); a slip past the source's end. | win, engine, window | M2 |
| ED-06 | **Consolidate** · 0203 d16 | A4-005, A5C-022, R11E-009, R11E-010, R11E-011 +1 | The window keeps the selection's scope (EditScope) and Ctrl/Cmd+J; the render, the op and the file are TM-FC-02's, on the one capture (XC-14). MIDI clips merge their notes with ids and expression kept, nothing rendered. Live's peak normalization with a compensating clip gain; the result's warp state is Q-H. [XC-01] | RE1, RE6 (d), RE3 | REAPER's Glue that bakes item FX; consolidating automation until Live is read; the matrix's "Warp off by d16" placement (withdrawn). | doc | Live 12.4: consolidate a warped clip (the result's warp state), a range with gaps, MIDI clips with overlapping notes; a null of the consolidated audio against its source. | engine, window | M5 |
| ED-07 | **Ripple editing (an addition)** · 0203 d2 | R11E-016, R11E-017, R11E-018, R11E-019, R10H-022 +2 | A visible ripple switch: Off (Live's leave-gap, the default), Per track, All tracks. The EditScope planner closes or opens time right to left on the affected tracks; automation follows Lock Envelopes; markers and tempo move only in All tracks; frozen material stops the ripple and the Status Bar names the track; one undo step. [XC-12] | RE2, RE5 (d), RE4 (d), RE6 (d) | REAPER's four ripple lock policies; per-lane ripple until take lanes exist; razor ripple. | doc | Per-track ripple of two deleted clips closes both gaps right to left; All tracks refuses on a frozen track, naming it; insert-then-extract renders identical PCM. | engine, window | M6 |
| ED-08 | **Frozen, grouped and linked tracks under edits** · 0203 d1, d16; 0044 | A4-013, R11E-013, R11E-014, R11E-015 | An operation-capability table per track state, applied by the planner (frozen: TM-FC-04's rule after the readings; folded group members by the selection rules). Live 12's Linked-Track Editing (one linked set per track; clip and take-lane edits mirrored in one transaction) as win's P2 placement, to strike if wrong. [XC-12] | RE7 (d), RE6 (d) | REAPER's item locks and edit groups; Cubase's Lock Event Attributes (Live has no item lock). | doc | Live 12.4: editing a frozen track's clips; linked tracks with different lane counts; one-step undo of a linked edit (A4-013). | win, engine, window | M6 |
| CT-05 | **Take crossfades and comp seams** · 0203 d7, d12 | C-F04, C-F05, C-F06, R11E-024, A6C-080 | A seam crossfade is the two main-lane copies' own fades over their source handles: a "Crossfade comp seams" command, and an option on the comp gestures, write them in the same step; off by default; win's placement at P1 with comping, to strike if wrong. | none | Cubase's advanced crossfade editor (P2, with the fade editor); REAPER's crossfade snapshots. | inf | A seam at a take's end without handles refuses or shortens (C-F04); an unfaded comp nulls against its takes. | win, engine, window | M6 |
| AU-03 | **Recording automation: the write pass** · 0203 d7; 0162 d4; 0054 d3 | A3-014, A-H02, C3-016, C3-019, R10H-052 +1 | A pass coordinator per lane: mouse writes are touch, controller and surface writes latch to the loop end or stop; locate, stop and loop wrap split the pass; Punch clamps writes; both pass edges keep the old curve (guards); thinning at drain time, recorded; the written lane suspended without a rebuild; commits grouped as Live's. [XC-17] | AU1, AU2, AU3, AU6, AU8 (t) | Cubase's Fill, Return Time and Continue Writing (the P2 modes, AU-09); Tracktion's 2 s release timeout. | doc | L-AUTO-1 to -5 and -9 (spacing, latch after release, undo per pass, loop overlap, punch, on-switch kind); AT-2 to AT-6. | win, engine, window | M6 |
| AU-05 | **Lock Envelopes: automation follows clips and range edits** · 0203 d1 | A-BASE-25.5.6, C3-014, R10H-051, R11E-054, R11E-055 | The planner reads one switch: with envelopes unlocked it moves, copies or deletes the track's device, mixer and send automation inside the span (replacing at the destination, guard points at both edges) in the edit's single undo step. The ...Time commands always move global automation and ignore the switch. [XC-33] | AU7, AU1, RE6 (d) | Window-side batching of automation.movePoints (worklist C7); REAPER's automation items. | doc | Live 12.4: a clip moved with Lock off onto automation (replaced or merged); a range Delete with Lock off; AT-10 (Lock off moves track, device and send automation with guards; Lock on moves nothing). | engine, window | M2 |
| AU-06 | **Clip envelopes: Arrangement clips modulate** · 0203 d1; 0046; 0114 | A4-020, A-H05, A6C-069, A6C-071, A6C-072 +2 | Arrangement clip envelopes are modulation streams in a whole-table minor (lane, clip, data, declared domain), evaluated with Live's per-parameter laws after automation inside the one modulation evaluator; #434's absolute overlay stays only for Session clips (P2) and for ADR-0115's gain curve (AU-07). [XC-16, XC-37] | AU4 | Bitwig's automation clips; Tracktion's normalized relative units (ADI declares each domain). | doc | L-AUTO-6 (volume at 50 % with the fader at -6 and 0 dB; a knob at 30 % with +50 %; the transposition clamp); an unlinked envelope's own loop (manual 26.5). | win, engine | M6 |
| MI-01 | **One note edit, one transaction: note.applyDiff** · 0009; 0003; 0021; 0155 | A-M01, A6M-016, A6P-010, A6P-011 | note.applyDiff {clip, from-hash, added, removed, changed}: writes the stream once, records overlap side effects, refuses a stale "from", round-trips unknown tails, carries flags and expression rows through remove and undo, and validates every range; the five P0 note ops become thin wrappers over it. [XC-25] | ME1 | Ardour's lock-based model. | inf | test_note_diff: a 10,000-note diff writes the blob once; a diff-and-undo fuzz restores the bytes; a future-tail fixture survives every op; an inactive note deleted and undone stays inactive. | engine | M2 |
| MI-02 | **Live's note overlap rule** · 0203 d1, d7 | A-M03, A6M-016, M2-A-MIDI | Live's same-pitch overlap resolver inside applyDiff, classifying by interval (start inside, end inside, covers, covered) and recording each truncation or deletion as a side effect of the same step; the preview shows them before commit. | ME1 | Ardour's other merge policies unless Live shows them. | doc | Live 12.4: the four overlap cases and which note keeps its id; overlap across MIDI channels; a stretch that reverses order; a truncated note's expression. | win, engine | M2 |
| MI-09 | **Live 12's note model: probability groups and deviation** · 0155; 0144 d3 | A-M07, A6M-030, A6X-006, A6P-011 | A side table (clip, note id, velocity deviation, probability group) in a whole-table minor; playback rolls probability per occurrence under ADR-0203 d7's random rule (a fresh draw per pass); a group resolves together. | none | Bitwig's occurrence and recurrence operators. | doc | A6M-030 (group semantics on Live 12.4); the .als NoteProbabilityGroup fields. | win, engine | P2 |
| HI-01 | **A gesture is previewed, then committed once** · 0110; 0141; 0148; 0153 | A6P-006, A6P-019, A-H02, A6X-016, C5W-017 | GestureSession: begin; each update re-runs the gesture's ops from the origin; commit writes one transaction with one label and the pre-gesture inverse; cancel writes nothing; undo, redo, moveTo and branch switches refused while open. Two preview strategies: live values for continuous scalars, a rolled-back transaction for structural edits. Supersedes OPS §6.4. [XC-17] | HI1, HI2, HI6 (t), HI7 (d), HI8 (d) | Undo-based coalescing; OPS §6.4's 300 ms merge of separate commits. | inf | test_gesture (a 20-move drag is one transaction with one label; cancel leaves the digest unchanged; the preview writes no ops); M-LIVE-UNDO (Live 12.4's labels per gesture, through the interaction logger). | win, engine, window | M2 |
| RE-03 | **Live's Edit Info Text** · 0203 d1 | DR-12-09 | Live's Edit Info Text as a property of tracks, clips and devices in a side table with one op (info.setText), copied with its object and kept in presets, exported as DAWproject's Nameable comment; shown in the Info View above the remarks; never merged with them. | RM7 (d) | Storing info text as a remark. | doc | L-INFO readings on Live 12.4 (which objects have it; what copying and presets keep). | win, engine, window | M6 |
| TU-02 | **Source fidelity: what ADI accepts and re-exports** · 0178 d2; 0103 d1; 0203 d1; 0144 d3 | A6F-001, A6F-004, A6F-005, A6F-006, A6F-007 +5 | ADR-0178's tables stay; source order and duplicates live in a keyboard map and the original bytes in tuning_sources (whole tables, the next minor); unknown @ABL lines kept verbatim with a warning; .ascl and @ABL in UTF-8, plain .scl from Latin-1; an unedited import re-exports byte for byte. [XC-28, XC-37] | TU1, TU4, TU7 (d) | tuning-library's centre-on-unmapped path; any refusal of a valid source; a pitch-ordered ruler as the default. | doc | M6-A6F-004 and -009 (does Live 12.4 accept non-monotonic and negative sources); M6-A6F-007 and -010 (unknown directives, encoding); byte-exact round trips. | win, engine | M6 |
| TU-03 | **What a tuning change does; Bypass Tuning; the ruler** · 0178 d3; 0203 d7 | A3-024, A3-025, A6M-084, A6M-086, A6M-087 +2 | Live's model by default: a tuning or key change keeps note indices; Live's automatic retuning is a confirmed edit, one undo step; Bypass Tuning per track; the ruler in Live's index order with names from NOTE_NAMES or the EDO data and Hz on hover; scale and tuning on one clip is ADI's addition. [XC-28] | ME3, TU7 (d) | Pitch-order display as the default (an addition only if he asks for it). | doc | AWAITING 15 (a tuning shown in the ruler, Live 12); M6-A6M-084 to -092; a tuning change mid-song with a tuning per key change. | win, engine, window | M6 |
| **REFINEMENT · Session, clips, time and media (10)** | | | | | | | | | |
| TM-SV-01 | **Global Quantization and its Arrangement uses** · 0203 d1, d6, d10; 0133 d4; 0185 d7 | A6G-013, A6M-004, A6C-006, A6C-007, A6C-023 +4 | transport.launchQuantization in session_state (outside undo) in Live's code list; a pure, bar-aware nextGridTick in ADI_PPQ resolved on the driver; a quantized locate at the grid sample with MIDI chase; with Link on it is the quantum. Window: the live menu, Ctrl+6 to 0, quantized scrub and locators. A FEATURES row at P1. | SV2, SV5, SV6 (d) | MAGDA's "strictly future" rule and its launch-while-stopped; storing ticks (1 Bar is not a fixed tick count in 3/4); per-clip launch quantization and the Launch rows (P2). | doc | N1 (a press on or just after a grid line); N4 (sub-bar values in 6/8 and 7/8); the installed chooser's codes; test_launch_quantize; a quantized-locate test. | engine, window, win | M3 |
| TM-SV-02 | **Session data: scenes, slots and launch settings** · 0101 d2, d4; 0117 §1; 0203 d10; 0144 d3; +3 | A3-032, A6S-007, A6P-011, A6P-012, A5C-037 +3 | Whole new tables (scenes, clip_slots, clip_launch) in Live's code lists, with an essential extensions row so an older build warns instead of playing slot clips at 1.1.1; slot clips kept apart from timeline clips; the 14 edit ops and the ephemeral launch ops; amends ADR-0117 §2's wording (side tables, not columns). [XC-21, XC-37, XC-41] | SV6 (d), IC2 | Rebuilding clips with a relaxed CHECK; scene-tempo application (TM-SV-04) and the DAWproject scene mapping (TM-IC-03) deferred. | doc | test_migrate to the new minor and check 9; an older build warns on the essential row; a project with slot clips renders, freezes and exports bit-identically to one without; Live: the Stop-button default and dormant scene values. | win, engine | P2 |
| TM-WT-01 | **The analysis core behind Auto-Warp** · 0132 d7; 0203 d7; 0176 d4; 0188 d2; +2 | A-W01, A-W04, A6C-032, C3-001, C3-002 +9 | The warp-analysis ADR: spectral-flux onsets with strength (reading editing.transientSensitivity); beats, downbeat and tempo from qm-dsp at a fixed 44.1 kHz with Mixxx's TempoTrackV2; BeatUtils regions and snapping; results as versioned ADIA sections; replaces today's heuristic only if a labelled benchmark (beat_this as comparator) says so; amends ADR-0176 d4. The suite's tracker is Q-D. [XC-19] | WT1 (v), WT2, WT3, WT8, WT7 (d) | Upstream TempoTrackV2 (needs about 7.4 s); qm-dsp's maths/pca; a second AANL cache; the re-detection inside prepareWarp. | inf | The benchmark gate (no corpus assembled yet); M2-A-WARP 1 and 6; 48 and 96 kHz give identical results; 1.1.1 on the first downbeat after a pickup. | engine, win | M6 |
| TM-WT-02 | **Warp ops, per-clip warp settings and Live's warp commands** · 0061; 0132 d7; 0203 d6; 0144 d3; +1 | A-W02, A-W03, A-W04, A-W06, A-W07 +15 | AWRP stays the one source-to-tick map; a minor of whole tables (clip_warp_settings, clip_transients); a clip origin for Set 1.1.1 Here (side table or AWRP v2, win picks); OPS §9.4's six ops plus setWarpSettings, setTransients and setOrigin; every Live warp command a prepared producer, one undo step, never a new analysis. | WT6 (d) | Tracktion's code (design only); tempo leaders (TM-WT-07) and the streaming players (TM-WT-04). | doc | M2-A-WARP 1 to 5 and 7; M6-A6C-034, -037, -038; the rightmost-marker rule and the marker ratio limits on Live 12.4. | win, engine | M6 |
| TM-WT-07 | **Tempo leaders, and the duplicate tempo-detection row** · 0203 d1 | A-W08, A6C-031, M2-A-WARP | Live's warped clips as tempo leaders (the bottom-most leader wins; Unfollow turns the result into tempo events in one step) as win's P2 placement; FEATURES' duplicate P2 "tempo detection" row is rewritten as this feature. | none | The Tempo Follower (ADR-0203 d9). | doc | M2-A-WARP 9 (two leaders, precedence, Unfollow, undo). | win, engine | P2 |
| TM-MD-03 | **Relink by hash, automatic search, Offline and Replace** · 0127; 0143; 0147; 0005 | DR-12-07, A5H-016, A5H-017, A5H-018, A6C-026 +5 | resolveSession checks the stored path, the project's audio/, registered folders and search paths, BLAKE3-verified through a size and mtime memo; searchMissing asks the library's hash index, then walks size-filtered; exact matches relink as one step; a same-name file with other content is listed, never played; an Offline mark; Live's two Replace rules as two ops. | in tree / own | Live's unverified manual repair (hash identity is stronger); collection and trash (TM-FC-03). | doc | A-M3-1 to -8; M3-MEDIA F4; L-MEDIA-4; adi_project_files_tests green on macOS first (PD-00). | engine, window, devices | M3 |
| TM-MD-05 | **The long tail of formats** · 0132 d3; 0156 d1; 0203 d18; 0204 d2 | R11X-022, M10-INV-REX, A-M5 | A format Live imports never needs an outside tool: Ogg Opus (opusfile, with pre-skip) and ALAC decode in process; the rest decode through FFmpeg as a child process (validated ffprobe JSON, streamed floats), its path in files.ffmpegPath, shipped in the installer's tools tier (win's placement). | MD4 (v), MD5 (d) | Ardour's -map_channel; Audacity's wx-bound FFmpeg layer; Sound Designer II stays P3; REX (no usable path). | inf | A-M5-1 to -5 (Mixxx's .opus first sound; the WMA, WV, CAF, MKV, WebM, AVI and TS fixtures; no tool; a killed child; hostile ffprobe JSON). | engine, win | M3 |
| TM-FC-01 | **Bounce to New Track and Bounce Track in Place as Live** · 0203 d1, d11; 0059 | A4-006, A4-007, A6H-023, A6H-024, C-E01 +4 | Render Tap::PostFx; copy the mixer settings (never input gain) in the same transaction; split at the selection and mute only the inner pieces; one new track per selected track, one step; Bounce in Place removes the devices (captured for undo); a frozen source bounces from its verified wet file; renders placed Warp off (d16). [XC-15, XC-33] | FC4 (d), FC2 | SoundCraft's deactivate-and-hide (that is Pro Tools' Commit, not Live). | doc | The A4-006 null (with the original muted, also with a track delay); which mixer settings Live copies; Bounce Track in Place on a MIDI track. | engine, window | M5 |
| TM-FC-02 | **Consolidate, Paste Bounced Audio and group bounce** · 0203 d1, d16; 0059; 0132 d1 | A4-005, A4-008, A4-009, A5C-022, R11E-005 +7 | Consolidate renders PreFx per selected track through the one capture (XC-14), peak-normalizes with a compensating clip gain, and replaces the material through the planner in one step labelled "Consolidate", keyed by a position-relative recipe; MIDI merges with note ids kept; Paste Bounced Audio renders PostFx when pasted; group bounces after EX-02's ViaMain. Warp state: Q-H. [XC-01, XC-14, XC-34] | FC3 (d), FC4 (d), FC2 | Qtractor's merge (no clip envelopes). | doc | M5-A5C-022; Live's normalization target and the clip gain it writes; the consolidated clip's warp state in Live 12; A4-008, A4-009; test_consolidate. | engine, window, win | M5 |
| TM-FC-03 | **The recipe key, reuse, collection and render folders** · 0059 d4-d5; 0069 d2, d4; 0038; 0127; +1 | DESIGN-IMPLICATIONS, C-D01, C-D16, A3-023, A6G-005 +4 | An ADR "freeze, bounce and consolidate are three contracts" with a recipe key (BLAKE3 over canonical CBOR of kind, fingerprint, identities, engine build, spec); only a current-recipe result becomes active; refs written as .part then renamed; one render in flight per key; collection only from an explicit Unused Files command; the mark covers every retained op on every branch; named stage prefixes; renders leave the project root (folder: win picks). [XC-32] | FC1 (d), FC2, FC3 (d) | FNV-1a and a weak media identity; Tracktion's purge on every render and delete on unfreeze; ADR-0069 clip chains until device_chains accepts a clip owner. | inf | test_render_key; test_freeze_commands (freeze, unfreeze, freeze renders once; undo and redo never render); collector tests; M3-MEDIA F6 in Live. | engine, window, win | M5 |
| **REFINEMENT · Devices and processing (14)** | | | | | | | | | |
| DEV-24 | **CPU Load Meter and Performance Impact** *(was decided)* · 0203 d1; 0125 | A5C-019, M5-A5C-019, M-CPU | The callback load from JUCE's AudioDeviceManager (getCpuUsage, getXRunCount) read raw so values above 100 % show; Average smoothed, Current and the overload warning as Live's; per-track Performance Impact from per-slot block timings summed per track at UI rate. [XC-08] | GT4, GT6 (v) | Ranking DAWs by their meters. | doc | GT-8 (the meter follows manual 37.1.1; freezing the heaviest track lowers it); Live 12 and ADI compared on the director's Windows workstation; confirm Live 12 has no Multicore switch. | engine, window | M7 |
| DEV-01 | **Racks play (level 1)** · 0060 d1-d5; 0122 d8; 0203 d10; 0058; +1 | A11N-001, A6H-042, A8D-001, A8D-022, N11H-006 +5 | Racks spliced into the track's graph (a router, chain gates, serial chains, chain strips and a double rack sum), not a nested Graph; ADR-0060's other decisions stand; ChainKey gains Branch and Join legs; mute, solo and volume ramp 5 ms without a rebuild; an unknown latency is told apart from zero; ADR-0122 d8's skip retired. [plan] | RK1 (d), RK2 (d), RK4 (d) | tracktion's shared buses and its audio-thread macro lock; wet/dry pin mapping; N-bus (ADR-0113) deferred. | doc | test_session's rack fixture inverted; two chains at 0 and 64 samples line up and the rack reports 64, nested too; an empty chain passes dry; M6-A6H-042 to -051. | win, engine | P2 |
| DEV-06 | **Macros: Live's takeover plus the multimapper (level 2)** · 0060 d3-d5; 0114 d1-d5; 0203 d15; 0110; +1 | A3-028, A3-029, A6H-051, A8D-018, N11H-001 +6 | macro.1 to .16 and chain select as ordinary rack parameters; a MacroSource emits ParamValue through each target's curve (AAUT points; empty is Live's straight line); one mapping is Live's takeover (win's ruling); several sum in real units clamped to floor and ceiling (d15), or last writer; VST3 sums normalized (a named deviation); keyed by (device, parameter id); side tables only. [XC-16] | RK1 (d), RK5 (d) | tracktion's normalized sum-then-clamp and lock; Bitwig's signed amounts as the macro model; Reason's undocumented many-source equation. | doc | hosts11 macro-composition-domain; d15's sum against his multimapper device file (not yet supplied); A8D-019 takeover, A8D-018's 16 macros, what Unmap leaves. | win, engine, window | P2 |
| DEV-07 | **Modulation in the graph: the schema, the modulators, the ring (level 2)** · 0046 d1-d3; 0052 d5; 0114 d4; 0062 d4; +3 | B-M01, B-M09, B3-015, B3-020, B9M-045 +8 | The modulation ADR ADR-0046 owes (XC-16): route side tables (source, target, depth, curve, mode, bipolar, enable) persist and undo; modulator nodes on the audio thread emit ParamMod, ordered by event-only edges; one evaluator per block delivers the base and one summed offset per target, the same way to CLAP, VST3, built-ins and Pd; one playing-value slot per target; mono or global scope now. [XC-16, XC-34] | MF1 | Per-voice modulation (until instruments are in scope); mappings inside a device's state blob; audio-rate host targets inside Pd until [adi.param~]. | doc | One source modulating a CLAP, a VST3 and a built-in parameter gives the same curve; the base survives stop, save and reopen; deleting a source is one undo step; ADR-0114 d4's cross-track arrival; B-M09. | win, engine, devices, window | M7 |
| DEV-10 | **User Pd patches as devices (level 3)** · 0035; 0145 d8; 0177 d1-d6; 0188 d3; +3 | A11N-002, A11N-003, A11N-008, A11N-009, A11N-010 +5 | plugin_refs internal uids for audio-effect, instrument and midi-effect patches make a PdPatchDevice; state roles patch, abstractions and sample:<id>; loadState parses and walks the allowed objects off the audio thread, reconciles parameters by id, swaps engines under the gate; Save is one device.loadState; a failing patch is a placeholder keeping its bytes; three starter templates. | PD5 (d), PD4 | hvcc (a compiler at run time, a second DSP truth); plugdata's name-keyed parameter pool; a "data" role unless win rules it. | inf | AR1 (a gain patch at 0.5 renders input × 0.5, delayed 64 samples, bit-identical after reopen); AR2 (one loadState; undo restores); AR3, AR11, AR14 to AR16; hosts11 pd-segment-contract. | win, engine, devices | M7 |
| DEV-14 | **Pd note output, voices and instrument kinds (level 3)** · 0194; 0091 d2, d8; 0158; 0054 | A11N-007, B9G-038, B9G-039, N11H-017, N11H-039 +2 | Per-instance libpd MIDI-out hooks fill a fixed array inside the trylock tick, decoded under the device's note prefix (DEV-17); offsets from Pd's logical time; MIDI-effect patches run in the transform phase; Pd instruments consume notes with [clone] voices; a host reset bang on prepare, stop, bypass and panic; MIDI 1 by construction. | MF3 (v), PD1 (v), MF5 (d) | [noteout] alone as completion; claims of Grid-identical polyphony; a high-resolution note bridge until a patch needs it. | inf | AR9 (an octave patch gives the same frames with unique ids; [pipe 100] on the exact sample; overflow releases and counts); AR10 (16 overlapping notes; per-note bend; reset within a block); the grid9 probes. | win, devices, engine | M7 |
| DEV-15 | **Grid-style modular semantics on the Pd tier (level 3)** · 0035; 0145 d8; 0177; 0188 d3; +2 | B9G-001, B9G-059, B-G02, B-G06, N11H-038 +2 | A hybrid: the host owns note identity, scheduling, modulation, state, undo and latency; Pd is the authoring and runtime layer; registered native kernels carry the heavy DSP. An ADR answers the gap review's eight decisions before any Grid-style work; acceptance is G1 to G6. | PD1 (v), PD6 (v) | A second native graph runtime or editor; a graph copy beside the Pd copy; Bitwig compatibility and Grid-equivalence claims; [adi.param~] until a patch needs it. | doc | G1 to G6 (threshold logic, note ownership, voice lifecycle, minimum loop delay per buffer size, one-undo graph edits, CPU); the ten grid9 specifications. | win, devices, engine | M7 |
| DEV-17 | **One note-identity namespace** · 0054 d2; 0155 d3; 0158 d2; 0091; +1 | A7M-128, B9G-038 | One process-wide event-source prefix allocator (a note id is the prefix in the high 32 bits and a serial in the low 32) for clip tracks, live inputs, transform nodes and Pd output; note-offs acknowledged by prefix; per-instance int32 maps in the CLAP and VST3 hosts replace the % 0x7FFFFFFF fold; same-key MIDI 1 handling; a clip's serials derived from the stored note identity so a rebuild keeps held notes. [XC-25] | RK2 (d) | Per-device counters (#207's nextId * 1024); per-event owner pointers. | inf | A clip note and a live note reach one CLAP and one VST3 fixture with distinct ids (they collide today); 10^6 random live ids never collide while held; NOTE_END frees the map slot. | engine | M6 |
| DEV-18 | **The event transform phase; generated notes owned and released** · 0091; 0042; 0090 d5; 0158 | A7M-128, N11H-039, B9M-001, BITWIG-GRID-PD-FEASIBILITY | transformEvents runs in Graph::forwardEvents after a slot's inputs are gathered and before splits, into a fixed store; transform nodes own their notes by prefix; releaseGenerated on bypass and when a node leaves at a publication swap; stop, seek or a loop jump releases everything; the clock runs while bypassed; prepare is idempotent. | MF1, MF5 (d) | #207's EventType::Control = 5 and its per-event EventOwner*; pull-based scheduling. | inf | #207's tests on main (identical timing from 32 to 4,096 frames; zero allocations); grid9 generated-event-ownership (no orphans after removal, seek, stop and undo); a rebuild mid-note keeps the arpeggiator's phase. | engine, devices | M6 |
| DEV-20 | **Recording generated notes, Live's way** · 0203 d1, d3; 0091; 0132 | N10H-015, N10H-016, A7M-128 | A MIDI row from track A (Pre FX or Post FX tap) into track B's note input is a graph edge, delayed by ADR-0091's rules; B's recording and Capture take the tapped events (an ADR amends MidiMemory's live-input-only rule); generated ids re-prefixed at the receiver. [XC-15] | MF6 (d) | ADI's own "commit MIDI effects to a clip" (an addition; Live's tap-and-record covers the need). | doc | hosts10 generated-midi-commit (no double processing; exact offsets); LM5 (what Capture and recording take from a tapped track). | engine, win, window | M6 |
| DEV-23 | **Multi-threaded graph processing** · 0203 d8; 0102 d1-d4; 0055; 0056 d3; +6 | A5C-019, A5C-020, M-CPU, M5-A5C-019, RESEARCH-MATRIX +2 | One persistent pool owned by GraphHost and lent to each graph; a dependency-counted, work-conserving scheduler (a chain continues on one thread, the callback co-runs, a node count ends the block); workers inherit FP state and real-time standing (MMCSS Pro Audio, the macOS workgroup, SCHED_FIFO); devices opt in; offline renders on their own normal-priority pool; on by default only after the the director's Windows workstation measurements. [XC-26] | GT1, GT2, GT3, GT4, GT6 (v), GT5, GT8 (t), GT7 (d) | #441's per-graph pool and level barriers as the target (keep its RenderState split, FP propagation and tests); a user-facing multicore switch; plug-in sandboxing. | meas | GT-1 (byte-identical output on 1,000 random graphs) to GT-8; the director's Windows workstation under ASIO, serial against 5 and 11 workers, compared with Live 12. | engine, devices, win | M7 |
| DEV-25 | **Out-of-process plug-in scanner** · 0203 d8; 0149; 0047 d3 | A5H-034, A5H-035, M5-A5H-034, B3-005, B3-009 +1 | Land #440, #439 and #438 after D1, D2, D5, D7 and D8 (and SEM_FAILCRITICALERRORS), checked by SHA; registry v3 (a scan cache keyed by format and path with a stamp; two strikes block; a negative cache); a scan service (K workers, Incremental, CacheOnly and Clean modes, a catalogue snapshot, a report, scans at startup only); Settings > Plug-Ins with Rescan, Alt+Rescan, Alt at launch and a blocklist. [XC-24] | SC1, SC2, SC3, SC4, SC5 (v), SC6 (d), SC7 (d) | A long-lived scan worker; any in-process fallback; runtime sandboxing; Re-scan Faulty, Selected and a per-plug-in Skip (additions not proposed). | meas | Scanner acceptance 1 to 13 (no lost output under a 500 ms pause, 200 times; K=4 equals K=1; zero scans inside project open; aliases followed); M5-A5H-034/035 on Live 12.4. | engine, win, window | M1 |
| DEV-28 | **One loudness meter for every reading, proved from outside** · 0203 d14; 0175 d5; 0167 d5; 0166 d8; +2 | R10H-073, R11X-025 | src/adi/dsp/loudness: 10 ms sub-blocks, gated I, LRA at 10 Hz, max M and S, run and pause, 0.01 LU exact-energy histograms, explicit channel weights, a double true-peak path beyond 8 channels; N13, export normalization, Auto Gain Stage and the Reference rewired onto it; the claim is "passes ADI's synthesised Tech 3341/3342 cases and agrees with libebur128 and ffmpeg", never "EBU Mode compliant". [XC-03] | LD1, LD3 (v), LD4 (d), LD6 (d), LD7 (d), LD2 | The EBU WAV set (not usable); libebur128's LRA (1 Hz hop); per-bus meters; ADI-L's own meter as the base (waits for his list). | doc | Tech 3341 cases 1 to 6 and 9 to 23 and Tech 3342 cases 1 to 4 within ±0.1 LU of an exact in-test reference; libebur128 within 0.1 LU at 44.1, 48 and 96 kHz; block-size invariance; zero allocations; Main's CPU cost. | devices, engine, window, win | M4 |
| DEV-30 | **The Loudness Drive after the 5 November ruling** · 5 Nov ruling; 0203 d14; 0077; 0204 d1 | none (in the bundle (the CTZ method) | A saved, automatable global control with its own op that adds the same gain to every leaf track's input gain (after the instrument), never to groups, returns or Main; click-free at both precisions; the Reference bypasses it; it engages nothing (the cascade is whatever clippers and limiters the user placed, his own plug-ins included); the engine's "blocked" entry lifts. [XC-06] | none; in tree | Switching on per-channel slots (withdrawn); the generic limiter as a stand-in for his clone; VCA fader tracks; automatic stage switching (waits for the unified feature). | doc | On a three-tier fixture with user clippers the Drive moves Main's LUFS while the balance before each clipper nulls; click-free at 64 and 4,096 samples; realtime equals export; one undo step per gesture. | win, engine | M4 |
| **REFINEMENT · Mixer, routing, monitoring and the Mix View (12)** | | | | | | | | | |
| MX-23 | **Drags heard while dragging, one undo step, multi-selection** · 0124; 0003; 0110 | A5C-002, A-BASE-18.2, M5-A5C-002, M-LIVE-UNDO, C6-HISTORY | A mixer drag (volume, pan, split pans, send level, a Track Delay drag, the summing drive) runs a GestureSession with live values (nothing journaled per update); End commits one transaction of the final value; Esc reverts; an unchanged end commits nothing; with N selected tracks the gesture is one transaction of N relative ops. [XC-17] | HI1, HI2, HI6 (t), HI7 (d), HI8 (d) | A UI-layer list of coalescable ops; journaling every value. | doc | M-LIVE-UNDO rows for fader, pan and send drags (one step and its label); C6-HISTORY. | win, engine, window | M2 |
| MX-25 | **Track-to-track routing and live In/Out choosers** · 0065; 0203 d3 | A6H-001, A6H-002, A6H-006, A6H-007, A6H-011 +10 | Win places (i) external inputs (hardware channels, loopback, a Link Audio peer) in tracks.input_ref's grammar and (ii) a whole new table for track-to-track inputs and MIDI From, with real foreign keys and their tap; Sends Only and No Output as an output state in a side table. The engine plays a non-exclusive edge from the source's tap, governed by monitoring; Resampling takes Main without the recording track; one step for all selected tracks. [XC-15] | none | A cable or matrix UI; REAPER's pin mapping (waits with MX-16); a track id stored inside text. | doc | M6-A6H-007, -008, -011 (tap against send-to and its solo consequences); L05; A3-009 (mono and stereo conversions); AWAITING 7(h). | win, engine, window | M4 |
| MX-26 | **Group mute semantics, and the registry default** · 0203 d7 | A5C-007, A5C-008, A-BASE-18.3, M5-A5C-007 | Until the reading the engine keeps its structure (a group's mute mutes its output; children keep sending), and mixing.groupMuteMutesSources defaults to Live's measured behaviour; if Live has no such option, the key leaves the registry or stays an ADI option, off. | none | Cubase's default (true) as ADI's default. | unk | AWAITING 15; are a muted group's children still heard through their sends in Live 12? | win, engine | M4 |
| MX-27 | **One loudness meter, tested against independent references** · 0203 d14 | C-U02, R10H-073, R11X-021, R11X-025, R11X-047 | The acceptance view of DEV-28's one meter: Main's meter, export normalization, Auto Gain Stage's LUFS mode and the Reference's match all read it, tested against synthesised Tech 3341 and 3342 signals and libebur128. [XC-03] | LD1, LD3 (v), LD4 (d), LD6 (d), LD5, EX4, LD7 (d), LD2 | The EBU WAV set (not usable); libebur128's LRA; Ardour's and lsp's meters; ADI-L's meter until his list. | doc | Tech 3341 cases 1 to 6 and 9 to 23 and Tech 3342 cases 1 to 4 within tolerance; within 0.1 LU of libebur128 on pink noise, speech and a sweep at three rates; the Main meter's CPU. | win, devices, engine, window | M4 |
| MX-28 | **Performance Impact indicators and the CPU Load Meter** · 0203 d1 | A5C-019, A6G-003, A-BASE-18.9, A-BASE-37.1.1, R10H-014 +4 | The engine publishes per-callback deadline use (audio work only) and per-track node time; the window draws Live's CPU Load Meter with its options and the six-rectangle Performance Impact indicators; win adds the FEATURES rows. [XC-08] | GT4, GT6 (v) | Ranking DAWs by their meters. | doc | M-CPU and M5-A5C-019 at equal workloads; how Live scales the six rectangles. | win, engine, window | M7 |
| MX-29 | **The Reference that sounds: role storage, matched loudness, spot play, drops on the strip** · 0209 d4, d9, d11 | C3-025, C3-026, R10H-010 | Roles in a side table track_roles (track, role; one Reference) as minor 1.20 on its own; the engine repoints its three readers from the column probe; an essential row makes older readers warn; a matched-loudness gain on the cue path from short-term loudness (Match on by default, nothing written); Alt+hold spot play outside undo; a file dropped on the strip lands on the hidden Reference track, Warp off. [XC-37, XC-13] | none | A separate reference bus (ADR-0209 Q7); Bluprint's matcher, the four-stem split and phone translation (P2). | inf | The role round trip on the table; a second Reference refused; a 1.19 reader keeps the table; the cue path derives the Reference from it; program X15 (which loudness window matches by ear). | win, engine, window | M4 |
| MX-30 | **Taps and the visual worker** · 0167 d1; 0175; 0183 d4; 0195 d5; +1 | E09 | Win's ADR amends ADR-0175 d2 (stamps kept per block in a side ring) and d4 (a tap retired by epoch after its last consumer, never freed under a stalled callback); device input and output tap points; gapless cursor reads; one visual worker per process paced by the displays; one STFT per tap and resolution; a 4 ms budget with a declared degradation order; analysis kernels as pure functions shared with analyze.* and export. [XC-15, XC-19] | Mix View program refs | A fourth audio-to-UI path without an ADR; analysis on the audio thread for a client. | inf | The program's §4.2 budgets (X4, X10); a stalled-callback test never writes freed memory; bit-identical audio with the Mix View open and closed. | win, engine, devices | M4 |
| MX-31 | **Roles, the CTZ template, the navigator and CASCADE, without slot data** · 0209 d2, d10, d11 | none (an ADI addition) | Set Role in the track, strip and STACK menus; the CTZ template in the User Library through PD-14's mechanism (Live's default Set stays the default); navigator blocks from group taps; tiers from a small template side table with a name-match fallback; the shaving and breathing bars and CASCADE's stage columns stay empty until the unified feature. [XC-13] | Mix View program refs | Per-rebuild "CTZ group metadata" from the engine; slot-derived shaving. | inf | The program's X17 (the navigator's update rate) on his songs. | win, engine, window | P2 |
| MX-32 | **Hosted plug-ins learn their track's name and colour** · none (a Live reading first) | none (to be measured) | After loading and on every rename or recolour, pass the track's name and colour to hosted plug-ins (VST3 channel context through JUCE; clap.track-info for CLAP). If a fixture shows Live 12 does the same, this is Live's way; if not, it is an addition for his yes. [XC-34] | none; in tree | CLAP drafts (gain-adjustment metering, mini curve display). | unk | A fixture VST3 records the channel context in Live 12 and in ADI. | engine | M4 |
| MX-33 | **The measurement harness** · 0108 | M-AUDIO, M-AUDIO-PRECISION, M-CPU, C6-AUDIO, M6-A6G-011 | Win's ADR "the measurement harness": a record schema, a gate vocabulary declared before each run, the seat table and the relay rule (AWAITING rows struck only from records); tools/measure from the live-probe kit, a Windows Live driver and analyse.py; the engine's src/adi/measure (residual compare, MTDM, sync spikes, probe signals) and adi_measure; audio_probe --loopback. [XC-38] | MS1 (v), MS2, MS3 (v), MS4, MS5, MS6 (d) | DawDreamer, Plugalyzer and pedalboard as gates (block-granular automation); an .als importer. | doc | The measurement plan's A1 to A5 (selftests on three systems; fixture hashes; MTDM at 256 and 256.5; spikes; renders bit-identical across block sizes and workers). | win, engine | M4 |
| MX-34 | **The governing texts follow the 5 November ruling** · 5 Nov ruling; N16 | none | One win docs PR records the 5 November ruling in every text that still names slots: FEATURES §2 rows, LANES §3 and §4, BUILD-BRIEF §5 and Phase D 7 and 7a, UI-ARCHITECTURE §12, the parity checklists, the harvest ledger's §1 and the Mix View program; the stale text of FEATURES (the master "not built", the strip row) fixed; the code stays as drawn; one short record ADR in his words, amending ADR-0203 d14, ADR-0204 d1 and ADR-0209 d11 (P-6). [XC-27, plan] | none | Any design of the unified feature. | doc | A text check: no queue or FEATURES row asks for present-but-off slots. | win | M0 |
| MX-35 | **The Loudness Drive after the ruling (CH-MX-1)** *(was new-choice)* · 0203 d14; 0204 d1 | none | The same contract as DEV-30, its owner: the Loudness Drive as one automatable dB control adding the same gain to every leaf track's input gain, with 5 ms ramps, exported as played, engaging nothing; built after the records PR. [XC-06] | none | VCA fader tracks; stage engagement (part of the unified feature). | inf | +6 dB raises every leaf's input by 6 dB and keeps the balance where the chain is linear; export equals playback; undo restores. | engine, window, win | M4 |
| **REFINEMENT · Project, export and data safety (9)** | | | | | | | | | |
| PD-09 | **File too new, and the next major** · 0144; 0136 d2; 0203 d1 | A6H-069 | A newer-major .adi opens read-only with a banner naming the writer and offering Save a Copy; 1.x to 2.0 follows Live (opens read-only, Save As writes the 2.x file); plugins.sqlite and library.sqlite adopt major.minor; every refusal names the version found, the version supported and the writer. | R4 | Mixxx's release-string upgrade chain. | doc | M6-A6H-069 on Live 12; the banner and Save a Copy; a newer-minor plugins.sqlite opens. | engine, window, win | M5 |
| PD-10 | **Schema minors stay whole tables** · 0144 d3; 0007; 0206 d3; 0188 d5; +1 | none (beyond the governing texts.) | Every format change is a whole new table in a minor, classified for the digest and the text projection; anything that changes an existing object needs ADR-0144 d3's own mechanism and ADR; pending minors kept in one register by win, each number given when its engine PR opens. [XC-37] | none | A column added in a minor without that mechanism (the role column, a drop-frame column). | doc | validate_schema.py check 9 and adi_migrate_tests from every frozen minor, for each new minor. | win, engine | M0 |
| PD-11 | **Settings file safety and migration** · 0152 d2-d6; 0125; 0203 d11 | C5W-029, C4-015, DR-12-18 | Saves through replaceFile with a .bak of the last good file, a re-read before writing (a hand edit is adopted), a 500 ms debounce and backoff on failure; reset() erases the key; a step migration runner; the bundle import checks its manifest; a newer schema shown but never written over (after reading how Live carries its Preferences). | R4 | toml_edit's comment preservation; Mixxx's upgrade chain. | doc | The craft settings tests ported (the backup holds the previous version; a newer schema is never overwritten); a step migration keeps the user's value; Live's Preferences across versions. | engine, window | M3 |
| PD-12 | **Durability under power loss** · 0047 d3; 0001; 0205 d10 | DR-12-01 | A BENCHMARKS row first: commit p50 and p99 on the owner thread with NORMAL and FULL and a near-full WAL, on the director's Windows workstation. FULL everywhere if it fits the frame budget; otherwise NORMAL stays and SPEC states the window. FULL on removable and network volumes now; checkpoint_fullfsync on Apple; DEFENSIVE on Store connections; a checkpoint thread only if p99 demands it. | R6 | Audacity's checkpoint thread (R6) before the measurement. | inf | The BENCHMARKS row; a pinned reader makes a PASSIVE checkpoint partial with SQLITE_OK, not BUSY. | engine, win | M5 |
| PD-13 | **Salvage a damaged `.adi`** · 0033; 0001 | DR-12-01 | adi_tool salvage <in> <out>: open read-only, recover into a private stage, run checkProject, report the rows sent to lost_and_found, publish without replacing anything; a quickCheck after an unclean open; DEFENSIVE on ordinary connections; no window offer until he asks. | R5 (v) | Any repair in place (ADR-0033); a Salvage button now. | doc | A planted corrupt page in ops keeps every intact op; application_id and user_version kept; the output passes check or says why; it never overwrites. | engine | M5 |
| PD-15 | **Project tabs and paste between projects** · 0068; 0111 | R3-002, A4-011, A6H-067, DR-12-04, DR-12-06 | As ADR-0068 d1 to d5: one Store per project, one active, paste between them as one transaction with remapped ids, blobs by hash; Live's import of tracks from another Set through the browser is the parity part; the historical read-only tab is this host's read-only case; a FEATURES row at P2 (win's placement). [XC-23, XC-22] | none | Keeping inactive tabs' engines warm (ADR-0111 d1); REAPER's subprojects. | doc | A cross-project paste of a track with a device and media: ids remapped, one undo step. | engine, window | P2 |
| EX-07 | **Names, collisions, partial outputs and the run manifest** · 0071 d1, d5; 0205 d5; 0147 d1; 0203 d1 | A11R-006, A11R-007, A11R-008, A6D-012, R11X-011 +11 | Job.collision is Fail, Increment or Replace; a single export asks through the save dialog (Live's way); batches follow Live once read; collisions resolved once per delivery at preflight (case-folded) and checked again at publication; Replace through replaceFile; a JSON run manifest; cleanup failures reported; the length check on the final name; Live 12.4.3's default naming. | EX3, R1 | Directory templates until escaping rules exist; REAPER's keep-partial (P3); Cubase's cancel deleting files already exported. | doc | R11X-018, -038 and -040's cases; names of 199, 200 and 201 UTF-8 bytes including CON.<suffix>; a collision just before publication; cancel at four points; M6-A6D-012. | engine, window | M5 |
| EX-09 | **Job persistence, presets and the agent's path** · 0071 d1, d3, d4; 0205 d3, d9; 0203 d11; 0181 d2; +1 | R11X-013, R11X-014, R11X-015, C4-008, C4-015 +2 | render::Job to and from JSON under a strict schema; user presets in ADI's data folder; project presets and saved jobs in session_state (no schema change); field-group masks; saving, enqueueing and rendering are three actions; export.queue.* on the control API at the Propose tier; the agent's job manager wraps render::Queue. [XC-20] | EX3, CA1, OS1, A4 | Cubase's 20-job cap; REAPER's opaque sink blobs. | doc | Tracktion's JSON round-trip and validation cases; an encoding-only preset leaves the destination, source and bounds untouched. | engine | M5 |
| CO-01 | **The collaboration core: order, identity and rebase** · 0182 d1, d2, d5; 0188 d5; 0161; 0203 d13; +1 | DR-12-11, DR-12-13 | Server-ordered: every batch gets a global seq (a put-if-absent claim on the bucket; the host on a LAN); batches carry author, id space, base seq, vector and typed ops; replicas apply in seq order through a remote commit; ids from the one allocator (an id space in the high 32 bits, a counter in the low 32); fractional sibling keys; rebase by rewinding the pending suffix and re-committing; machine-local columns out of sync and digest; code in src/adi/collab. [XC-22, XC-39, XC-41] | C1, C2 (v), C3, C4, C8 (d), C9 (d) | Yjs or any CRDT mirror of the project; SQLite's rebaser; validation on a server copy. | doc | Three Stores and a fake object store over 200 seeds with delays and drops converge to equal digests with nothing pending; the fractional vectors; two replicas inserting a device at one slot converge. | engine, window | P2 |
| **REFINEMENT · Control, AI, surfaces and testing (29)** | | | | | | | | | |
| CTL-01 | **One control stack: dispatcher, listener, wire, actors and owners** · 0039 d1-d6; 0168 d5; 0181 d2; 0206 d1, d4 | R10D-021, DR-14-01, DR-14-02, DR-11-04, DR-11-06 +3 | Win's ADR: one listener (ws://127.0.0.1:<port>/rpc, JSON-RPC 2.0, off by default); /mcp only on the same server and port, adi-mcp over stdio; every project method names its project; request meta (expectHead, deadlineMs, idempotencyKey); actors by who writes the ops; network threads never touch the model; durable integer ids; owners: the engine (src/adi/control, tools/adi-mcp) and the window (src/juce/app_control.*). [XC-39, XC-40] | CA1, A1, CA4 (d) | MAGDA's two listeners and the src/adi/rpc split; IXWebSocket; MCP and control servers without authentication; anything past loopback as a default. | doc | AC-6 (every write ends in validateSubmission and commit, or in the changeset, with its actor asserted); AC-17; with two projects open, a request naming B never touches A. | win, engine, window | M0 |
| CTL-03 | **The control core (JUCE-free)** · 0039 d1, d3-d5; 0181 d2 | DR-14-02, DR-14-15, DR-11-04 | JSON-RPC errors; constant-time token checks with one refusal text; grants keyed by the paired credential, a new client at Propose or lower, Apply only by an explicit grant under maxOpsPerMinute; surface allow-lists; idempotency by credential and request id; PENDING, TAKEN, ABANDONED deadlines; an expectHead mismatch writes nothing; a 512-event audit ring with secrets redacted. | CA1, A1, CA4 (d) | MAGDA's name-keyed grants, connection-keyed dedupe, dispatch lock across the handler and inline execution. | doc | AC-1, -3, -6, -7 and -14 to -19 in process; test_rpc_permissions (permission before validation; a revoked grant never replayed; a new client at Propose or lower; secrets never in the audit). | engine | C |
| CTL-04 | **Transport: one loopback WebSocket** · 0039 d4; 0181 d2; 0168 d5; 0188 d6 | DR-11-04, DR-11-06 | cpp-httplib v0.64.0 behind one pImpl unit; 127.0.0.1 only; 401 and 403 before the protocol switch; the Origin checked when present (the allow-list empty by default); caps of 8 connections, 240 KiB frames, 8 requests in flight, 50 a second and a 10 s deadline; one writer thread and outbox per connection; an HTTP POST never dispatched; a clean shutdown. | CA2 (v), CA1 | cpp-httplib v0.52.0 (ten WebSocket fixes missing); IXWebSocket; civetweb; CHOC, websocketpp and uWebSockets. | doc | AC-2 to AC-5 over real loopback sockets on three OS legs; MSVC /WX and clang's strict flags with httplib in one unit. | engine, win | C |
| CTL-08 | **`adi-mcp`, the stdio MCP bridge** · 0168 d1, d3; 0181 d2; 0039 d6; 0064 d6 | DR-14-01, DR-14-02, DR-14-03, DR-14-15 | Out of process, stdio, JUCE-free, with two backends (--project, headless over the core; --connect through the discovery record); tools project_read, ops_describe, changeset_propose and changeset_status (jobs later); both MCP protocol eras; an op failure is a tool error carrying ADI's refusal; the instructions say project content is data and nothing commits without the user; consent before a non-local read. | CA3, A2 | A second Streamable-HTTP listener; muse_framework's and Ardour's MCP servers; community C++ MCP SDKs without grants or audit. | doc | AC-23 (the legacy and the modern handshake; headless read, propose a volume change, see the diff, file unchanged until applied); test_adi_mcp_bridge (finds the instance; survives a restart and a token rotation). | engine | C |
| CTL-09 | **The app host and the Clients page** · 0039 d4; 0181 d2; 0203 d11; 0201; +1 | DR-14-02, DR-11-04 | The host src/juce/app_control.* starts on control.enabled (rows control.enabled off, control.port 0, control.allowedOrigins empty); the executor is MessageManager::callAsync; each client writes through its own OpSubmitter so every window refreshes; one Settings page (the masked token and pairing code; clients and their grants; connections and refusals; Revoke, Disconnect, Rotate). | CA1 | An embedded script language (scripts are external clients). | doc | The app answers system.describe; a revoke refuses the very next request; an API write refreshes every open window on the next frame. | window, win | C |
| AGT-02 | **Observe tier, the levelled projection and a stable project identity** · 0168 d3; 0007 | DR-14-04, DR-14-03, A6P-004 | project_read {level 0 to 3, scope} returns {text, level, head} on a second, read-only connection (bars, beats and note names; level 3 is AGT-07's analysis); the Store writes a random project_uuid at create and at the first writable open of an older file; Save As keeps it, and so do Save a Copy and Collect (win's ruling: the same project for consent). | in tree / own | Dumping SQL rows or the whole canonical text at a model. | inf | The fidelity corpus (AGT-08); project_uuid set after create, upgrade and Save As; two files share it only if one was saved from the other. | engine | C |
| AGT-03 | **The consent ledger** · 0039; 0064 d6 | DR-14-05, DR-14-15, DR-14-02 | A disclosure names kinds and sizes, never contents (parameters alone still count); records keyed by app, project_uuid, connection, destination and capability, with terms (this time, an hour, a day, until revoked); a new destination or capability asks again; a fail-closed 256 KiB file; loopback never asks (judged by the parsed address); an external MCP host's read is recorded per project; refusal is ConsentRequired. [XC-40] | A3 | The harvested Disclosure that skips consent for parameter-only requests; a quit-scoped term. | doc | test_consent (craft's 29 tests plus parameters-alone; a new project_uuid asks again; loopback never asks, the LAN asks; refusal until granted). | engine, window | P2 |
| AGT-04 | **Jobs a client can watch, cancel and trust after the project moves** · 0086; 0064 d1-d3; 0071; 0205 d5 | DR-14-12, DR-14-06, A11R-001, R10H-079 | One legal state machine (progress monotone, terminal states final) wrapping render::Queue and the other workers (analysis, stem separation, remote services); owner-scoped cancel; retention 128 jobs or 10 minutes; a job that commits into the project declares its read set and is refused as stale only when those inputs changed (refused, never rebased); renders keep ERRATA-11's four moments. [XC-20] | A4 | Enqueue-time snapshots; float cost fields; staleness on any movement of the head. | doc | test_rpc_jobs; a late stem import after an edit that touches its inputs fails with a conflict and no artifact; an export queued before an edit and started after it renders the revision at its start (A11R-001). | engine | M5 |
| AGT-08 | **The evaluation corpus** · AI-AGENT | DR-14-01, DR-14-02 | Fixture projects with tasks whose outcomes are checkable as digest or projection assertions; adi_tool propose and the sidecar with the faux provider in CI, a real model by hand; adversarial fixtures (a track named as an instruction, an agent-addressed remark) must end refused or queued. | in tree / own | Judging taste. | unk | The corpus runs; the injection fixtures prove nothing commits. | win | C |
| AGT-09 | **Tiers at the boundary: their scope and the always-confirm rules** · 0039 d3; 0131 d5; 0125 d1-d3; 0152 d4 | DR-14-02, DR-14-15, DR-12-10 | agent.tier stays the app default; a per-project override in session state may only lower it unless the user raises it; no client raises its own tier; a client's grant is capped at the effective tier; always confirm: a changeset touching more than 20 % of the clips, any egress, any run tainted by remark text. | none | Executing because a remark asked (never). | doc | A project at Propose under an app-level Apply queues; a changeset touching 21 % of the clips queues; a tainted run queues. | win, engine | M0 |
| SRF-02 | **Mixer gestures: audible while moving, one undo step** · 0181 d2; 0162; 0203 d1 | DR-11-05, DR-11-07 | An engine producer takes Begin, Value and End for volume, pan and a send's gain; each Value sets a live value the audio thread reads, with no op; End commits one op whose inverse is the value at Begin; a source with no release closes after a quiet window; the lane is overridden live while touched; it feeds the automation recorder while armed; the mouse and the value keys adopt it. [XC-17] | in tree / own | Committing only on release. | doc | SF-9 (a mouse drag changes the live gain during the drag and leaves one entry; an MCU touch, twenty moves and a release leave the same); the armed-and-recording case (Live manual p.491). | engine, window | M2 |
| SRF-03 | **Encoder flavours, absolute decoding and the keyboard source in the format** · 0181 d1; 0130 d1; 0180 d6; 0144 | A6R-008, A6R-009, A6R-010, A6R-011, A3-018 +2 | controller_maps.mode widened through the validator and the SPEC and OPS text (no CHECK change): absolute, two's complement, toggle, binary offset, signed bit, signed bit inverted and their linear variants; detection from a slow left turn; 14-bit and note-like CC decoding; Key Map rows as protocol key; midi.focusDialMode widened; Live's type names attached after SF-22d. | SU1, SU9 | A separate encoding column; MAGDA's router write path. | doc | SF-3, SF-4; a row with mode 1 still reads as two's complement; SF-22d in Live 12. | engine, win | M3 |
| SRF-07 | **The feed's surface half** · 0181 d3; 0198; 0211 | DR-11-06, DR-11-05, B3-013 | Strip records (id, name, palette index, volume, pan and sends with their text, mute, solo, arm, selected, a meter tap id); transport and selection records; ui.mixview.focus; DevicePanelData publishes the plug-in's identity uid; playing values from the one lock-free slot per target; driven and lastTouchedBy tell modulation, automation and manual input apart. [XC-16] | CA1 | A surface reading the model directly (ADR-0181 d5). | doc | SF-15 (a subscribed strip record equals what the strip draws on the same tick); AC-13. | window, engine | P2 |
| SRF-11 | **MIDI-CI property exchange (P3)** · 0185 d3; 0188 d7 | DR-11-09 | Strike the OS precondition (JUCE runs MIDI-CI over MIDI 1.0 SysEx on plain ports); a ci::Device per Remote port pair on the message thread; a PropertyDelegate serving ResourceList, DeviceInfo, ChannelList and the focused device's parameters from the feed; writes as midici:<muid>; P3 on priority alone. | SU4 (v) | Waiting for the OS MIDI stacks. | doc | SF-18; the resource names commercial controllers ask for, checked against a device. | window, win | P3 |
| SRF-14 | **Where a surface's page comes from** · 0150; 0154; 0181 d2; 0188 d4; +1 | B3-011, B3-012, A6R-002, DR-11-03, DR-11-05 +1 | Live's default (a plug-in's Configure list in banks of eight; a native device's fixed banks); his predefined controllers take precedence for their plug-ins; ADI's addition: Bitwig-style named, editable pages per instance (a side table) and per plug-in identity (app data), never replacing Live's banks (win's placement); one page model for every surface. | none | Bitwig's ninth slot until a surface needs it. | doc | M3-CONTROLLER item 4 (Bitwig's page persistence); a 300-parameter plug-in pages through its Configure list; a renamed device keeps its pages. | win, engine, window | P2 |
| WIN-02 | **Every window reopens where it was** · 0063 d2; 0129 d3; 0180 d3, d7 | DR-01-09, C5W-008, C5W-011, M3-UI-WORKSPACE | One placement rule (sanitize; the monitor by area shown; a reachable title bar; clamp; restore proportionally when the monitor changed, on the main display when it is gone; cascade); monitor identity by id, then area, origin, a unique same-size-and-DPI monitor, then the main display; the main window and Settings in an App record window.places, floating windows in ui_view; saved on move and checked again on display changes. | WI1 | Free docking, named workspaces and display profiles (ADR-0203 d5); a "gather windows" command. | doc | Window-infra acceptance 7 to 16 (at 1366x768 and 125 % the title bar never opens off screen; a moved window keeps its place after a kill); AWAITING: where Live reopens its windows. | window, win | M1 |
| WIN-03 | **Window kinds, owner windows and the plug-in windows** · 5 Nov ruling; 0076; 0180 d3, d7 | DR-01-10, A5W-001, A5W-002 | Owned windows (plug-in editors, device views) stay above the main window only, minimise with it, have no taskbar button; normal windows (the Mix View's, the Second Window); editorsAlwaysOnTop chooses; Esc closes unless a plug-in takes the key; Live's plug-in window commands (the wrench, Ctrl+Alt+P, Multiple, Auto-Open and Auto-Hide, the track's name, cascading); ADI never scales a plug-in window; a generic device-view window kind. [XC-27] | WI2 | Always-on-top as the Linux fallback; placing the unified feature's window now. | doc | Window-infra acceptance 17 to 21, driven off his desktop; AWAITING: in Live, are plug-in windows above other applications, on the taskbar, closed by Esc? | window | M1 |
| WIN-04 | **Stable command identity and user shortcuts that survive (first half)** · 0180 d6; 0125; 0047 | C5W-010, C5W-015, C5W-016, A5W-009 | Stable text command ids with Live's menu categories (the list only grows); a shortcuts.overrides record mapping ids to canonical chords, re-applied after every reload and compared before writing; a Shortcuts page on JUCE's key-mapping editor in ADI's own look. | WI3, WI7 (d) | Persisting JUCE's numeric ids or per-OS key descriptions. | doc | User keys survive a Settings change; a preset switch keeps the overrides on top; every command has one stable id. | window, win | M1 |
| WIN-09 | **The Help menu and Live's menu search** · 0203 d1, d5 | A-BASE-2.2.1, DR-01-01, DR-01-12 | The Help menu shown with at least the manual and the keyboard-reference entries, so the Mac's Cmd+? menu search works; the Help View joins later. | none | A lessons engine (later). | doc | On his Mac, Cmd+? searches ADI's menus. | window | M1 |
| WTS-01 | **Never on the director's desktop: launch isolation and the testing ADR** · 0180 d8; 0108 d4, d5; 0149 d1 | CR-RUN-05, DR-01-12 | Test launches (--smoke-output, --control-test) are multi-instance, require ADI_HOME, never activate their window, report to stderr and script their file choosers; tests run on the CI legs, in process, on the Linux box under Xvfb, and in a Windows Sandbox on the director's Windows workstation only with his OK, never in his session; ADR-0180's two settled items struck. | WT-5, WT-4 (d) | ADI.exe on his live desktop; pywinauto for ADI's own UI (it stays for driving Live). | doc | AT-7 (beside an ordinary running ADI, a smoke starts its own instance, writes its picture and exits 0; the running copy receives nothing; the foreground is unchanged; --control-test without ADI_HOME exits 2). | win, window | M1 |
| WTS-02 | **Test hygiene: the input clock, assertions, repaints and counting** · 0180 d8; 0050; 0209 d9 | CR-RUN-05, DR-01-01 | An InputClock seam (key state and milliseconds) for every key read (keyIsDown, AppCommands::key, the Mix View's [REF], Tap Tempo); a fixed virtual display set; AssertionWatch with JUCE_LOG_ASSERTIONS on the test targets only; windows_no_dialogs in every Windows test binary; repaint invariance, an extreme-zoom paint and a paint census; the JUCE suite counted by test_all.sh. | WT-5, WT-3 | A PUBLIC JUCE_LOG_ASSERTIONS (it keeps jassert live in the release ADI.exe). | doc | AT-4, AT-5, AT-6 (no OS key state read; Shift+R tap and hold), AT-8. | window | M1 |
| WTS-03 | **Goldens that check text: comparator, glyph masks and the font experiment** · 0180 d8 | DR-01-11, DR-01-12 | The font experiment first (one pinned Latin and Hebrew face; each scene recorded per OS as identical, anti-alias only or offset); then a RecordingRenderer storing glyph boxes beside each golden; the ignore mask the union of the boxes; EXACT by memcmp, pixelmatch for the diff image; bless modes; the eight goldens recorded again with their text. | WT-1 (v), WT-6 | pixelmatch at threshold 0 as EXACT; hand-placed masks. | doc | AT-1, AT-2 (a word moved 1 px passes while a divider moved 1 px fails), AT-24. | window | C |
| WTS-04 | **The render review job** · 0108 | DR-01-12, SRC-TEXT-ONLY | Every leg uploads its renders and a manifest; a PR is compared per OS against its base's renders (found by the first parent; built when missing); reference, current and diff images with a report; the job fails until the PR carries ui-renders-changed and names the scenes; actions: read, no cancel-in-progress on main, the labeled trigger. | WT-2 (d) | Comparing against the base branch's tip; automatic acceptance. | doc | AT-3 (a colour change reports exactly its scenes; a PR with no pixel change passes; a deleted base artifact gives the same result; #288's wrap shows as a diff). | window, win | C |
| WTS-05 | **`UiControl`: one description of the window, in process first** · 0180 d8; 0047; 0210 d3 | CR-RUN-05, DR-01-12, DR-01-01 | src/juce/ui_control.* holds the one melatonin copy: ui.windows, inspect, elements and lint; input only through the peer's handlers (click, drag, move, scroll, key, text, drop); ui.invoke, command and focus; frames and screenshots; arrangement hit and locate; replies after the input applied and one frame run; componentID ids; the fake peer records dirty rectangles; the runner inside the dispatch loop; in process first. | CA5, WT-3, WT-4 (d) | Calling mouseDown directly; promising never to take the keyboard on a shared desktop. | doc | AT-9 to AT-12 (stable ids; clicking Play equals Space; a drag is one labelled undo step; a dropped WAV is one Insert Audio Clip step). | window | C |
| WTS-06 | **The headless script runner** · 0180 d8; 0108 | CR-RUN-05, DR-01-01 | adi_ui_script (--project or --new) script.jsonl over the fake peer, with no display or audio device and ADI_HOME under its output; requests, shots, frames, expectations and notes; PNGs and a transcript; exit 1 on a failed expectation; ui.yml runs it on three legs; [P:...] parity tags in the scripts. | WT-4 (d) | A queue with synchronous input (a per-request step cap stands in). | doc | AT-13. | window | C |
| WTS-07 | **Sweeps, and the director's remarks as regressions** · 0108 d1-d4; 0209 d11 | CR-RUN-05, DR-01-03, DR-01-11 | A dead-control sweep (every click yields a new history head, a view change, a popup, a transport command or a Status Bar line; undoing every step returns the first projection); round-trip and dialog sweeps; layout invariants at three sizes and three zooms; the Mix View's transitions as a filmstrip (R3); a Hebrew-named track right to left; scripts for his remarks R1 to R6. | WT-5 | Building on add5102 (revert #372 on a scratch branch instead). | doc | AT-14 to AT-18; reverting #372 must fail R1's triangle and R6's buttons. | window | P2 |
| WTS-08 | **Accessibility lint and the Windows UIA scan** · 0047; 0125 d4 | DR-01-11, DR-01-12 | ui.lint rules (unnamed, obscured, zero-size, outside the parent, duplicate id, clipped text); axe-windows' UIA rules ported where they map, and its CLI scanning ADI.exe on windows-latest; a floor that only falls; Linux runs the in-process rules only. | WT-3 | Out-of-process UIA as the main path. | doc | AT-12; a clean axe-windows CLI scan. | window | P2 |
| WTS-09 | **Parity bookkeeping that cannot drift** · 0108 d1 | SRC-TEXT-ONLY, CR-RUN-05 | tools/validate_parity.py in ci.yml's validator step: row ids, normalised status words, every [P:file#row] tag resolves, the count of untagged built rows only falls, a file's built count never drops without a floor change; optionally a menu catalogue compared with Live's captures. | WT-4 (d) | Parity claimed from manual text. | doc | AT-19. | win, window | M1 |
| WTS-10 | **The running app over the API, smokes everywhere, other desktops** · 0180 d8; 0181 d2; 0039 d4 | CR-RUN-05, DR-01-09 | The ui grant exists only under --control-test (multi-instance, ADI_HOME, a banner), never for a paired surface; synthetic ops carry ui-test:<client>; screenshots and drops confined to declared roots; native smokes on Linux (xvfb-run) and macOS too; an the Linux box runbook driven over loopback from SSH. | CA5, CA4 (d) | xdotool and ImageMagick sessions. | doc | AT-20 to AT-22 (#276's checks and the root-default golden reproduced through the protocol on the Linux box, without xdotool). | window | C |
| **NEW CHOICE FOR THE DIRECTOR · Editing and the arrangement (2)** | | | | | | | | | |
| HI-02 | **An undo cap that holds** · 0203 d13; 0140 d5; 0111 d4 | R3-001, R10H-061, N10H-002, A4-010, A5C-048 | Inside authority now: compaction is best-effort (it reports and never fails a commit), runs down to a low-water mark and stores checkpoint rows by content hash. What may be compacted past is the director's (Q-A; recommended: every snapshot and dead end kept as a deduplicated stored state, so the cap always holds). [XC-02, XC-32] | HI3 | Reason's redo clearing; REAPER's preference-driven history. | inf | A 3,000-step project with a snapshot on day one stays under the cap; Ctrl+S on a new project does not stop compaction; the E7 history fuzz. | win, director, engine | M2 |
| TU-04 | **Tunings for plug-ins beyond per-note pitch** · 0155 d4; 0097; 0099; 0134 d7 | A8P-018, A8P-019, B9M-069, A6F-014 | The director's Q-L. Recommended: an MTS-ESP master as a per-plug-in route beside CLAP note expression, MPE and plain MIDI, off by default, talking to the library already on the machine; CLAP's tuning extension for CLAP instruments; MTS SysEx for hardware later. | TU2, TU3, TU5, TU6 (d), TU8 | Any libMTS binary inside ADI (not usable); one machine-wide table applied to every plug-in. | doc | Surge XT follows 24-EDO with zero residual; a second master is refused with a status; per-track Bypass Tuning cannot reach MTS clients (recorded). | engine | P2 |
| **NEW CHOICE FOR THE DIRECTOR · Session, clips, time and media (2)** | | | | | | | | | |
| TM-WT-05 | **Short samples on drop** · 0132 d6, d7; 0203 d7, d12 | A-W01, A6C-008, A6C-032, DR-04-08 | The director's Q-E. Recommended: every drop arrives clean; Live's short-sample classifier runs when Warp is pressed and in the import prompt's Warp choice (a Warped Loop with two markers, or an Unwarped One Shot); record.shortSampleMode gets Live's four choices; the threshold provisional at 60 s until read. | WT4, WT5 | Audacity's "any whole number of bars is a loop" rule (Live assumes 1, 2, 4, 8 or 16 bars of 4/4). | doc | Live 12.4: the short/long threshold; what Auto does on a drop against a Warp press; synthetic loops of 1 to 8 bars at 70 to 180 BPM give exact bars. | win, engine | M6 |
| TM-LS-02 | **Link Audio** · 0204 d5; 0203 d19; 0126; 0185 d7 | A6I-001, A6I-002, A6I-003, A6I-004, A6I-005 +2 | The director's Q-F. If reopened: BasicLinkAudio on the same 4.1.1 pin, enabled only when used; a source node per peer channel into a track's input; a sink per track with audio; Latency and Sync to Incoming Audio as declared latency; peers named by peer and channel ids through input_ref's grammar. [XC-15] | LS3 (v) | If he keeps it closed, the 4.1.1 pin still stands, so no SDK swap follows. | doc | A6I-001 to -005 with Live 12 or a Push as a peer; AWAITING 15 (do Link Audio peers appear by themselves). | engine, window, win | P2 |
| **NEW CHOICE FOR THE DIRECTOR · Devices and processing (3)** | | | | | | | | | |
| DEV-11 | **The Pd patch editor's route (level 3)** · 0145 d8, d9; 0134 d8; 0188 d8; 0177 d4; 0203 d8; 0204 d5 | B9G-003, B9G-004, B9G-005, PD-RACK-EXPORT-FIT-11 | The director's Q-K (C-1); route A re-admits an external Pd GUI process that ADR-0203 d8 and ADR-0204 d5 rejected, so the question names four of his rulings. Recommended route A: "Edit in Pd" attaches vanilla Pd's editor to the running instance; edits are heard at once; objects ADI cannot run draw dashed; Save re-reads, validates and writes one device.loadState; closing without saving reverts; Save updates every instance that shared the old hash. | PD3, PD1 (v), PD4, PD6 (v) | Route C (plugdata's canvas inside ADI, XL) unless its look is worth months; route B later, as an optional "Open in plugdata". | doc | AR12 (attach, hear before Save, one op with the text's BLAKE3, revert, refusal by name); LM-PD1 on Live 12.4; polling makes no audible gap. | director, engine, window, devices, win | M7 |
| DEV-12 | **Opening and editing a built-in device's patch (level 3)** · 0203 d4; 0187 d1; 0204 d5 | A11N-006, A11N-005, B9G-008 | The director's Q-K (C-3). Recommended: opening a built-in shows its canonical patch read-only; editing makes "<Device> (edited)" on the user-patch path, leaving the built-in native; the copy states which path runs and its delay. | in tree / own | Editing in place (it would silently change a familiar device's sound path and timing). | inf | AR6 (a canonical canvas with bare CRs takes the native path; an edited copy runs Pd and reports its delay). | director, engine, window | M7 |
| DEV-13 | **User-patch delay when blocks do not divide by 64 (level 3)** · 0203 d4; 0183 d12; 0095; 0042; +1 | A11N-005, N11H-038, BITWIG-GRID-PD-FEASIBILITY | The director's Q-K (C-2). Recommended: no added delay when the host buffer is a multiple of 64 (whole Pd ticks across a block's splits, each event at the start of its tick); otherwise one reported, compensated 64-sample block shown on the device; a patch's own delay always added. | in tree / own | Silencing invalid segments in production; promising zero delay for any patch. | inf | hosts11 pd-segment-contract (buffers 64 to 511; offsets 1, 63, 64, 65, 127); AR5 (a 256 split into 1 + 255 nulls at the declared delay). | director, devices, engine | M7 |
| **NEW CHOICE FOR THE DIRECTOR · Mixer, routing, monitoring and the Mix View (1)** | | | | | | | | | |
| MX-36 | **The Mix View and STACK before the unified feature (CH-MX-2)** · 0209 d11 | C3-021, A7A-062 | The director's Q-B. Recommended (a): after the mixer's B1 to B8, the non-slot parts now: the Reference (MX-29), the strip scope and spectrum and the analyser view (MX-12), the Scope window (MX-13); then STACK as an instance list of each track's own devices (his VST3s among them) with the fisheye, slider, navigator, search, pins and row spectra; tiles read-only except ADI's own devices; the drawn slot columns stay off until his list. [XC-27] | HI1, WT-1 (v), WT-6 | The program's slot work; ADI-Q-specific tiles and editors; the phrase that the slot columns become the unified feature's place. | inf | The program's X1 to X3 (the row against his video; the fisheye's feel; any of 100 tracks found in under 2 s); WN8 side by side with Pro-Q 4's instance list in Live 12. | window, engine, win | M4 |
| **NEW CHOICE FOR THE DIRECTOR · Project, export and data safety (5)** | | | | | | | | | |
| PD-06 | **Recover a recording that was running at a crash** · 0132 d1; 0203 d1 | A5C-049, DR-12-01 | The director's Q-I. If yes: a take.json plan written at record start; a RIFF or RF64 header repair only for the recorder's own files in a record folder; recovery is Stop's prepared batch, one undo step, carrying the recording id so it never places twice; refused on a read-only project; the launch question names the recovered takes. [XC-34] | R3 | "Idempotent because media is content-addressed" (the recording id is what makes it idempotent). | doc | SoundCraft's two tests (a short header placed whole; foreign, outside, ../ and symlinked files skipped); an RF64 take; a PCM24 take; a deleted track reported; M5-A5C-049. | engine, window | M5 |
| PD-07 | **Live's Backup folder** · 0125; 0203 d6, d12, d13; 0182 d3 | A5H-014, DR-12-16 | The director's Q-A (with HI-02). If yes: from the second Take Snapshot a worker writes a consistent copy to <project>/Backup/<name> [date time].adi; ten kept, ordered by a UTC time stored in each copy; the eleventh to the Recycle Bin; copies clear the session lock; media stay referenced, as Live's. [XC-32] | R7 | Deleting the oldest (Live uses the trash); any timer-driven copy (his no-timer directive). | doc | M5-A5H-014 (the folder's name, time format, count, trash); the clock-goes-back test; a repeated hour overwrites nothing; rotating "Cut" never touches "Cut-Two". | window, engine | M5 |
| EX-06 | **Dither for Live's three Pow-r slots** · 0203 d1; 0132 d5 | A4-004, A6G-010, R11X-034, A11R-009, DR-13-07 | The director's Q-J. One quantizer for WAV, AIFF and FLAC (seeded, partition-independent, applied once, never at 32-bit float); shaped modes reserve headroom so a 0 dBFS peak cannot clip; rates without a table use a stated fallback; the shapes and names of Live's three Pow-r slots are his (recommended: SoX's open tables now). | EX5 | POW-r (not usable); the Airwindows dithers (none is F-weighted). | doc | The error spectrum below TPDF from 1 to 5 kHz with its energy above 15 kHz; determinism; a full-scale peak does not clip; Live's Pow-r spectra. | engine | M5 |
| CO-02 | **Undo in a shared project** · 0188 d5; 0030; 0182 | DR-12-15 | The director's Q-N (his Q4). Recommended: an unsequenced transaction undoes by moving the head; a sequenced one appends a compensating batch of its inverse ops, minus those whose touch set meets a later batch by someone else; redo compensates the compensation. | C1, C2 (v) | Compensating a collaborator's later writes by default. | doc | figcraft's undo scenarios ported; the undo checker finds no violation over 200 seeds; an unsequenced undo uploads nothing. | engine | P2 |
| CO-04 | **Live awareness: how presence travels** · 0182 d2; 0181 d2; 0126 | DR-12-11, DR-12-12 | The director's Q-N (his Q5). Recommended: (a) presence as heartbeats beside the batches in the bucket, no live cursors, for the first collaboration release; (b) an optional LAN-hosted live session later, off by default, on the control API's one server stack. [XC-40] | C5, C6 | A relay service of ADI's (against "ADI runs no service"). | doc | A loopback host and two joiners (resume keeps the id space; a wrong token or schema refused; presence at most 30 a second per peer); a follower starts within one block plus transit. | engine, window | P2 |
| **NEW CHOICE FOR THE DIRECTOR · Control, AI, surfaces and testing (3)** | | | | | | | | | |
| CTL-05 | **Where control-API secrets live (CH-2)** · 0039 d4; 0181 d2; 0168 d3; 0149 | DR-11-04, DR-14-15 | The director's Q-O (restored by this revision: a discovery file makes the session token readable by every process of his user, a change to ADR-0039 d4's "shown in the UI" model; AI-AGENT §7.1 rejects any tokenless local mode). Recommended (c): the file carries only the endpoint and a pairing hint, every client pairs once. Option (a), the first draft's placement: a 256-bit session token, shown masked, in an owner-only, non-roaming discovery record inside PD-05's session marker, deleted on exit and swept when stale, granting at most Propose; pairing by a single-use 120 s code; ADI keeps only a hash; clients keep their tokens in the OS keychain (a prebuilt native helper for the Stream Deck); never in the .adi or settings.json. [XC-24] | CA1, CA4 (d), SU2 | A tokenless local mode; a roaming record; Elgato's plain settings store. | doc | AC-18, AC-19; a crash's stale record swept at the next launch; another Windows user cannot read the record. | win, engine, Stream Deck seat | C |
| SRF-15 | **Push 2 and Push 3 (CH-1)** · 0125; 0203 d1 | A6U-001, A6U-002, A6U-013, A6R-014, DR-11-03 | The director's Q-M. If yes: a Push 2 and Push 3 controller-mode driver behind SRF-08's host, built from Ardour's display transport and DrivenByMoss's behaviour and checked against Live 12.4, at P2 after the surface host and Session View; Push's User Mode works as a generic MIDI surface either way. | none | Ableton's push-interface document and Live's Remote Scripts (not usable). | doc | Push 2's display and pads driven from ADI on his unit; Live 12.4's Push behaviour captured (A6U-004 to -012). | engine, window | P2 |
| WIN-10 | **Palette free text: commands first, or straight to the agent (CH-4)** · 0104 d3; 0203 d6; 0064 d6 | DR-14-03 | The director's Q-G. Recommended (a): free text ranks local matches first (commands, devices, later plug-ins, samples and presets) and Enter runs the top one offline; the last row is always "Ask the agent: <text>", acting only when chosen, at the agent's tier under consent; inert, and saying so, with the agent off. | WI5, WI6 | Sending every unmatched string to a model. | inf | "hsb" runs Hide/Show Browser; "Make the drums tighter" shows the agent row last and sends nothing until chosen; with consent off the row asks for it. | window | P2 |

---

## 4. The code to take, by milestone

The harvest ledger (the harvest ledger (summarised in `docs/RESEARCH-INTAKE.md`)) records **208 takes** from the 28 area reports, every
one checked by its area's verifier. This section places each take in the milestone where its work
happens: the earliest milestone of a row in the take's own area that uses it (M0 holds no code), with
thirteen takes moved by hand to where they are really used (LD5, EX3 and EX4 to M5; ME5 to M6; SC1
to SC7 to M7, the scan service; SU1's encoders and binding model and SU9's surface data to P2's
surfaces step, since the rows they serve are P2, with SU1's device-identity match alone in M3 for
SRF-05). The "For rows" column lists every row that names the take.

**How a take comes in.**
- **Port C++ / copy with attribution:** the code is rewritten or copied into ADI's tree, keeping its
  notices and naming its source in `docs/EXTERNAL-CODE.md`.
- **Vendor as library:** a `third_party/` pin fetched by `tools/fetch_external.sh`, one row in
  EXTERNAL-CODE. Three new rows (libFLAC, LAME, soxr) wait for his grant before they are fetched,
  as EXTERNAL-CODE's procedure for such rows has it. Test-only rows are built for tests and never
  shipped.
- **Tests only / design only:** vectors, scenarios or a design read; no code enters ADI.
- **In tree:** code ADI already vendors (JUCE 9.0.2, libpd, dr_libs, the SQLite amalgamation).
- **Reference clones to refresh before porting:** MAGDA to head `ddbc114`, Tracktion to head
  `a5e4499`, Link to `Link-4.1.1`, SoundCraft to `5fabf28`, FilmCraft to `c9be142`; line numbers read
  at older commits are checked again.

**What the 5 November ruling changes in the takes (XC-27).** The ledger's §1 ("the director's 5
November instruction shapes the plug-in takes") read the ruling as one Mix View channel processor
with standalone Q and L instances; that reading is withdrawn. Concretely:
- **LD2** (ADI-L's own ADI-L's own meter) is a cross-check only; FilmCraft's meter (LD1) is the base.
  His plug-ins' source joins ADI only when he declares them all finished.
- **EX4** builds the true-peak brickwall into ADI's own `dsp::LookaheadLimiter` from the Ardour and
  lsp-plugins designs, not from ADI-L's own true-peak code.
- **EX5** fills Live's three Pow-r slots from SoX's open tables; ADI-L's own dither comes in only if
  his list says so (Q-J option b).
- **SU2**'s "pages per the integrated processor" and **WI2**'s "a Q or L opened from a track" are
  withdrawn; his Stream Deck controllers drive his builds as ordinary plug-ins.
- Ranks 9 and 10 of the ledger's "value for effort" list (one meter from ADI-L; export DSP from
  ADI-L) are read through the three points above.

### M0: none

M0 is records, merges and the macOS fix (#461, ADI's own code). The tuning stack (#442 to #445),
the scanner PRs and the engine log are ADI's own work.

### M1: the Pd safety overlays, window placement, shortcuts and the test hygiene kit

Pd's runtime is closed against native loading and quit/exit before any user patch exists (PD1, PD2,
PD4, MF4). The window takes craft-libs' placement rule and MAGDA's owner windows and key store
(WI1 to WI4). The test kit's assertion watch, repaint checks and focus rules come in now (WT-3 to
WT-5), so every later window PR is checkable off his desktop.

| Take | What | From | How | Lane | Effort | For rows |
|---|---|---|---|---|---|---|
| LD7 | Readings snapshot with validity flags + seqlock publish; test ideas (block-size invariance, histogram vs exact list within 0.05 LU) ... | tracktion_engine | design-only | engine | S | DEV-28, MX-10, MX-27 |
| MF4 | libpd unbind leak fix | in-house | re-apply by hand | win (or engine per LANES 1) | S | DEV-09 |
| PD1 | libpd/Pd internals already pinned: per-instance MIDI-out hooks with logical-time offsets (pd_systime per clock callback), lock-free queued console ... | libpd | vendor-as-library (already) + copy-with-attribution (PROCESS macro port) ... | devices (after R3) | M | DEV-09, DEV-14, DEV-15, DEV-11 |
| PD2 | #A dangling-binding fix + test scenarios (dangling binding, seeded patch-text fuzz, state round trip) | plugdata-pd | copy fix with attribution (overlay on pinned m_pd.c) + port test scenarios | devices | S | DEV-09 |
| PD4 | Allowed-object marking: recursive compatibility walk through subpatches/abstractions + class enumeration from the running build (vanilla m_imp.h) ... | plugdata | port C++ (walk + enumeration onto ADI's tokeniser and own list) | devices | S-M | DEV-09, DEV-16, DEV-10, DEV-11 |
| RM5 | Pin clustering (greedy around running centre), open pin stands alone, count bubble, stable author colour | figcraft | port C++ (1-D sweep O(n); author colour into Live's 70 palette indices; agent glyph) | window | S | RE-01 |
| RM8 | Typing a remark is one undo step (merged execution) | soundcraft | design-only | window | S | RE-01, RE-02 |
| WI1 | Window placement rule: sanitize, monitorOf by area shown, reachable title bar, keptReachable, clampOnto (min/max), restore ... | craft-libs | port C++ (JUCE-free; monitor identity by id, area, origin, then same-size-and-DPI unique) ... | window (win: window.places row) | S | MX-14, WIN-08, WIN-02 |
| WI2 | Owner windows for floating views (palette kind above the main window only, minimise with it, activation handed back on close) ... | magda-core-head | copy with attribution (Mac half in ui_owner_mac.mm) | window | S | MX-14, WIN-08, WIN-03 |
| WI3 | Key mapping store lifecycle (store only differences from defaults; restore = defaults then saved diffs) + own-look shortcut panel (categories) ... | magda-core-head | copy with attribution (overrides keyed by stable text ids in canonical chord text) ... | window | S | WIN-04 |
| WI4 | Keymap model: canonical chord text (Cmd/Ctrl split folding off macOS), foreign text reader, Steal/Keep/Refuse, conflicts (clash vs shadow) ... | craft-libs | port C++ in part (not egui ui/, not editor Cancel) | window | M (L with import) | WIN-01, WIN-05, WIN-07 |
| WI7 | Docking concepts only (whole-result checks, palette vs normal kinds, modal drops to normal); keybindings by name corroborate stable ids | craft-libs | design-only | win | S | WIN-04 |
| WT-3 | Accessibility/structure helpers + lint rules: component tree walk, DN role table, unnamed widgets, obscured check; axe-windows UIA rule library ... | melatonin_inspector (the same copy as control-api CA5); egui kittest | copy with attribution (helpers S, UiControl M) ... | window | S/M | WTS-02, WTS-05, WTS-08 |
| WT-4 | Control protocol + runners for scripted UI: element ids, synthetic input, screenshots, JSONL shot logs, parity floor ... | gridcraft | design-only | window | M | WTS-01, WTS-05, WTS-06, WTS-09 |
| WT-5 | Click-through/coverage/dialog sweeps + repaint invariance + assertion watch + no-dialog guard + extreme-zoom paint | plugdata | port + copy with attribution (JUCE_LOG_ASSERTIONS=1 on the TEST targets only) ... | window | S | WTS-01, WTS-02, WTS-07 |


### M2: the edit planner, note diffs and the gesture contract

The placement planner is ported as pure functions from SoundCraft's edit algebra and openDAW's
Live-model overlap resolver (RE1, RE3), with FilmCraft's slip (RE2) and the Ardour, Audacity and
Qtractor designs and tests (RE4, RE5, RE7). The note editor rests on Ardour's NoteDiffCommand shape
(ME1). The gesture contract takes ossia score's ongoing-command dispatcher and the FigCraft,
GridCraft, SoundCraft and openDAW tests (HI1 to HI8).

| Take | What | From | How | Lane | Effort | For rows |
|---|---|---|---|---|---|---|
| AU1 | Guard-point policy + write-pass lifecycle (evaluate old curve at boundary, held value moves last point, locate/stop/reverse split the pass) ... | ardour | port C++ (policy only, pure functions over sorted vector; no RWLock/watcher thread) | engine | M | AU-01, AU-02, AU-03, AU-05 |
| AU7 | Automation follows clips (Lock Envelopes off) | tracktion_engine | port C++ (design on ADI's time-edit split) | engine | M | AU-05 |
| HI1 | Gesture dispatcher contract: submit makes then updates, different target commits the stale one first, commit pushes without second apply, rollback ... | ossia-score | port C++ (contract + edge cases onto ADR-0148 preview; hard refusal is ADI's) | engine | M | HI-01, MX-23, MX-36 |
| HI2 | Preview-from-origin rule + version-history behaviour tests | figcraft | port tests C++; design for preview | engine | S | HI-01, HI-04, MX-23 |
| HI3 | Undo groups across calls + sequence fuzzer (undo-all = start, redo-all = end, shrinking) | gridcraft | port tests + fuzz harness; design-only group API (pending batch, never a held SQLite txn) | engine | M | HI-02 |
| HI4 | Undo History list model (rows, current index, undoRedoToIndex, redo rows dimmed) | audacity | design-only | window | M | HI-03 |
| HI5 | Revision tree view (Buchheim layout, connectors, head mark), checkout any node, cherry-pick from past revision | helio-sequencer | port C++ (already JUCE) | window | M | HI-03 |
| HI6 | Merged gesture at a full history costs one step | soundcraft | port tests | engine | S | HI-01, MX-23 |
| HI7 | Editing-step rules: one log owned by outermost step, cancel reverts own changes only, empty step commits nothing, undo off while open ... | openDAW | design-only | win | S | HI-01, MX-23 |
| HI8 | Drag framework Esc contract (abort, finished(movement_occurred)) | ardour | design-only | window | S | HI-01, MX-23 |
| ME1 | NoteDiffCommand shape (field changes old/new, added, removed, side-effect removals, undo order) + same-key overlap classifier and merge policies ... | ardour | port C++ (shape; ADI records every side effect, classifies by intervals) ... | engine | M | MI-01, MI-02 |
| ME4 | JUCE piano roll mechanics: grid drawing, move/resize, ghosts on copy-drag, rubber band, velocity lane + ramps, CC lane, pitch fold map ... | magda-core | design-only (not its snap rules, not its slice curve copy) | window | L | MI-03, MI-08 |
| RE1 | DAW edit algebra over sorted non-overlapping clips: place = clear then insert, put_clips clears all destinations first ... | soundcraft | port C++ into a pure planner (not its MIDI-drop-on-split policy) | engine | M | ED-01, ED-03, ED-04, ED-05 +1 |
| RE2 | Timeline edit algebra: overwrite, insert, razor, lift/extract, ripple delete with sync-lock conflicts naming the track, handle-clamped trims ... | filmcraft | port C++ into the same planner (conflict model, not Premiere defaults) | engine | M (L with roll/slide) | ED-05, ED-07 |
| RE3 | Live-model region overlap resolver (delete/separate/trim start/end right to left over joined masks), trimStart keeping loop offset anchored ... | openDAW | port C++ | engine | S | ED-01, ED-03, ED-04, ED-06 |
| RE4 | Option sets: ripple scope (Selected/All/Interview), Insert/Remove Time options (intersected Leave/Move/Split, all playlists, markers, locked markers) ... | ardour | design-only | engine | S | ED-04, ED-07 |
| RE5 | Ripple/paste taxonomy (DeleteBehavior, CloseGapBehavior, PasteBehavior) + trimOrDeleteOverlapping 4 cases + mixed-rate sample-boundary tests | audacity | design-only + tests (Live's leave-gap stays default) | engine | S | ED-01, ED-03, ED-07 |
| RE7 | Affected-object scope as a bit mask (Clips, Automation, Loop, Punch, Markers, TempoMap) + per-track insert/remove range | qtractor | design-only | engine | S | ED-02, ED-03, ED-08 |


### M3: Link, the media readers, safe files, the surface core and the palette

New `third_party` rows: `link` at `Link-4.1.1` with its asio submodule (LS1), `alac` (MD3), `opus`
and `opusfile` (MD4), `taglib` with its utfcpp submodule (MD7), and `qm-dsp` if Q-D lets key
detection start here (MD6; otherwise with WT1 in M6). The FFmpeg tool for the long tail is a child
process found through `files.ffmpegPath`, shipped in the installer's tools tier (MD5).

| Take | What | From | How | Lane | Effort | For rows |
|---|---|---|---|---|---|---|
| R1 | Atomic replace primitive: stage, sync (F_FULLFSYNC on Apple), rename with Windows retry (5/32/33, 7 retries 10 ms doubling cap 200 ms), dir fsync ... | craft-libs | port C++ | engine | S (M with call sites) | PD-01, PD-02, PD-04, PD-08 +1 |
| R4 | Settings store policy (.bak, re-read before save, debounce 500 ms, backoff retry, newer schema read-only) + step migration runner ... | craft-libs | port C++ | engine (+window Wd4) | M | PD-09, PD-11 |
| LS1 | Ableton Link core: BasicLink (enable, numPeers, tempo/start-stop callbacks, realtime-safe capture/commitAudioSessionState) ... | Ableton/link Link-4.1.1 | vendor as library (third_party pin; one TU, _WIN32_WINNT=0x0A00) ... | engine | L (vendor S, adapter M) ... | TM-LS-01 |
| LS4 | Link chase policy (half-bar jump threshold with 250 ms inhibit, varispeed bound), cycle-aligned start; MTC reader chase policy | tracktion_engine | design-only (not its MTC generator bug, not getTempoInRange) | engine | S | TM-LS-01, TM-LS-03 |
| LS5 | Minimal per-block Link adapter shape (hylia_process → {bpb, bpm, beat, phase, playing}, set_output_latency) | Carla | design-only (int64 sample time) | engine | S | TM-LS-01 |
| LS6 | JUCE-side Link + MIDI clock module ideas (offset ms, +/- beat buttons, 48-pulse warm-up) | BespokeSynth | design-only (SPP bug: do not copy) | window | S | TM-LS-01, TM-LS-03 |
| MD1 | ISO BMFF / QuickTime demuxer: header-only open (moov/moof, never mdat), trak/stbl tables, edit offsets, fragments (tfhd/tfdt/trun) ... | craft-libs | port C++ (ASC wins for AAC channels/rate; add iTunSMPB with FFmpeg mov.c 5851-5872) ... | engine | L | TM-MD-01, TM-IC-05 |
| MD2 | Decoder acceptance suite + fixtures (one audio in every format; first-sound frame; seek/skip/read-beyond-end; wrong suffix; free-mode garbage) | mixxx | copy with attribution (fixtures into tests/fixtures/decode/mixxx/ with README) ... | engine | S | TM-MD-01, TM-IC-05 |
| MD3 | In-process ALAC decoder (bit-exact on every leg) | macosforge/alac | vendor as library (or port) | engine | S-M | TM-MD-01 |
| MD4 | Ogg Opus in process (pre-skip) | xiph/opusfile | vendor as library (shared with export's test-only opusfile) | engine | S | TM-MD-05 |
| MD5 | Long tail via external ffmpeg/ffprobe (probe JSON fields, f32le streamed from a child process) | ardour | design-only (own ffmpeg_tool + scan_process streaming mode) | engine (win: settings row) ... | M | TM-MD-05 |
| MD6 | Key detection: constant-Q chromagram (36 bins/octave) correlated with major/minor profiles; Mixxx global-key reduction + downmix helper | qm-dsp | vendor as library (adi_qmdsp shared with warp-tempo) + port Mixxx wrapper (~150 lines) | engine | S (after warp-tempo step B) / ... | TM-MD-04, AGT-07 |
| MD7 | Embedded tag reading (ID3v1/v2, APE, MP4 ilst incl. freeform, Xiph comments, RIFF INFO, WAV id3, AIFF, Matroska) + Mixxx field map + key-text parser ... | TagLib | vendor TagLib (read-only, options trimmed, static) + port Mixxx rules (std::regex) | engine | M | TM-MD-04 |
| MD8 | ACID/smpl/inst/cue/bext/INFO metadata from already-vendored decoders; Vorbis comments; Xing frame count | dr_libs (wav-0.14.5) | use (already vendored; separate dr_wav metadata open since WavReader path skips dr_wav) | engine | S | TM-MD-04, EX-08 |
| MD9 | Apple Loops basc chunk (beats, root, meter, loop type) | libsndfile | port C++ | engine | S | TM-MD-04 |
| MD10 | Sound Similarity / Auto Tags / CLAP encoders + file-name BPM/key rules + user-over-detected | magda-core | design-only (reference for Live 12 3.4 Similarity Search, 41.23 Similar Sample Swapping) ... | engine | L later | TM-MD-04 |
| SV2 | Launch quantization table + next position through bars/beats + clip source range mapper (loop wrap split, progress) + audible launcher test suite | tracktion_engine HEAD | port C++ (table + mapper) + tests restated vs Live; runtime design-only | engine | M | TM-SV-01, TM-SV-03 |
| SV5 | Musical grid rules across meter changes (whole-bar grids counted from last signature change; sub-bar restart at bar lines; half-open intervals) ... | libossia | port C++ (rules ~80 lines) | engine | S | TM-SV-01 |
| SV6 | Live's API code lists + state vocabulary (fired vs playing, triggered scene, has_stop_button, back_to_arranger) | AbletonOSC | design-only (copy code tables with attribution into ADR + OPS) | win | S | TM-SV-01, TM-SV-02, TM-SV-04 |
| SU7 | Pick-Up crossing rule (+ host move re-arms) + driver interface checklist | tracktion_engine | copy with attribution (rule) + design (interface) | engine | S | SRF-04, SRF-08 |
| SU8 | Soft-takeover truth table + test matrix | mixxx | tests only (crossing vectors) | engine | S | SRF-04 |
| SU10 | MIDI device hot-plug notifications | JUCE 9.0.2 (in tree) MidiDeviceListConnection::make | use (already vendored) | window | S | SRF-05 |
| WI5 | Command palette: prefix-routed providers, root list advertising prefixes, popup (Enter runs, Esc dismisses, height fits) ... | helio-sequencer | copy with attribution (string prefixes, longest wins) ... | window | M | WIN-06, WIN-10 |
| WI6 | Optimal fuzzy matcher (two-row DP, positions for underlining, fzy scores incl. slash/word/capital bonuses) | fzy | port C++ to UTF-32 (JUCE-free lowercase table; Hebrew matches as is) | window | S | WIN-06, WIN-10 |


### M4: one loudness meter and the measurement kit

FilmCraft's meter is ported as the base (LD1), with libebur128 as a test-only oracle (LD3), the EBU
Tech 3341/3342 signals synthesised in C++ (LD4) and ffmpeg's LRA as an optional oracle (LD6). The
measurement kit (MS1 to MS6) lives in `tools/measure` and `src/adi/measure`; pywinauto drives Live on
the measurement seat only. New rows: `libebur128` (test-only); `pyfar`, `pyrato` and `pywinauto`
pinned in `tools/measure/requirements.txt`.

| Take | What | From | How | Lane | Effort | For rows |
|---|---|---|---|---|---|---|
| LD1 | Streaming BS.1770-4/R128 meter: M/S at 100 ms (move to 10 ms grid), gated I, LRA at 10 Hz (Tech 3342), max M/S, 0.01 LU exact-energy histograms ... | filmcraft | port C++ (10 ms sub-blocks; -inf before warm-up; explicit channel weights) ... | devices | M | DEV-28, MX-27, EX-05, AGT-07 |
| LD2 | Director's C++ loudness meter: M/S/I, LRA 10 Hz, max M/S, recording flag freezing I/LRA/maxima (EBU stand-by), explicit LFE index ... | ADI Audio | port/merge (C++ base candidate; must be committed by the clones lane first; add TP) ... | devices (clones lane commits source) | M | DEV-28, MX-27, EX-05, AGT-07 |
| LD3 | Independent test-only oracle (I, max M/S, TP; _multiple for common-gain/one-long-file semantics) | libebur128 v1.2.6 | vendor as library, TEST-ONLY (never shipped; warnings off for that TU) | devices (win: row) | S | DEV-29, DEV-28, MX-27, EX-05 |
| LD4 | Tech 3341 v4 + Tech 3342 v4 minimum-requirement signals SYNTHESISED in C++ (cases 1-6, 9-23; LRA 1-4) | EBU Tech | design-only (synthesise) | devices | M | DEV-28, MX-27, EX-05 |
| LD6 | 10 Hz LRA oracle via CLI (ebur128 filter metadata print) | ffmpeg | design-only + optional Linux CI oracle (ADI_REQUIRE_ORACLES=1) | devices | S | DEV-28, MX-27 |
| MS1 | Sweeps, impulse/sine/seeded noise, sub-sample IR delay (Tamim-Ghani), regularised deconvolution (RegularizedSpectrumInversion) ... | pyfar | vendor as library (pinned in tools/measure/requirements.txt; Python >= 3.11) | win | S | MX-03, MX-22, MX-33 |
| MS2 | Realtime-safe sub-sample round-trip latency meter (13 phase-locked tones, polarity, error figure) + caller logic (no signal, peak > 0.707 refused) ... | ardour | copy with attribution (~150 lines; fix both peak slips) | engine | S | MX-09, MX-33 |
| MS3 | Live 12 driver on Windows (app start/connect by pid, native menus incl. check states, UIA Save dialog, keys) | pywinauto | vendor as library (pinned commit; Windows only) | win (GUI seat) | M | MX-22, MX-33 |
| MS4 | Sparse spike-pattern alignment + LatencyTester (device loopback) | tracktion_engine | copy with attribution as port (seed spikes AND noise bed) ... | engine | S-M (needs a fake loopback) ... | MX-09, MX-33 |
| MS5 | Distances for non-nullable comparisons: ESR, DC, SI-SDR, SD-SDR, multi-resolution STFT, sum-and-difference | auraloss | port formulas to numpy (~60 lines) + ESR/SI-SDR into C++ residual core | win + engine | S | MX-03, MX-22, MX-33 |
| MS6 | Synchronised swept-sine nonlinear identification (Hammerstein kernels), reverberation times | lsp-dsp-units | design-only now | devices | L later | MX-33 |


### M5: rendering, export, crash safety and DAWproject

New rows: `flac`, `lame` and `soxr` (his grant first), then `ogg`, `vorbis` and `libopusenc` for the
later export formats; `pugixml` (IC5); SQLite's (its source file) at the bundled version (R5). R6 (a
checkpoint thread) comes in only if PD-12's benchmark row asks for it. R3 waits for Q-I, R7 for Q-A,
EX5's shapes for Q-J.

| Take | What | From | How | Lane | Effort | For rows |
|---|---|---|---|---|---|---|
| A4 | Job manager (bounded legal state machine, revision policy StableUntilCompletion/CheckAtStartOnly, owner-scoped cancel, retention 128/10 min) | magda-core head | port C++ | engine | M | EX-01, EX-09, AGT-01, AGT-04 |
| R2 | OS-locked session marker (dead vs live session) + SPEC 3.6 session_lock heartbeat + OpenReport{unclean, opsSinceCleanClose, walBytesAtOpen, generator} | filmcraft | port C++ (marker only; .adi is the journal) | engine | M | PD-05 |
| R3 | Recover a recording running at a crash: take plan json + header repair + place once as one undo step | soundcraft | port C++ | engine | M | PD-06 |
| R5 | SQLite recover extension: `adi_tool salvage` | sqlite version-3.49.2 (1b571af1) | vendor as library (third_party/sqlite-recover, SQLITE_ENABLE_DBPAGE_VTAB) ... | engine | S | PD-13 |
| R6 | Checkpoint thread off the committing thread, "disk full" once | audacity | port C++ only if BENCHMARKS commit p99 row demands | engine | M | PD-12 |
| R7 | Backup rotation with DST-safe names | filmcraft | port C++ | window | S | PD-07 |
| EX1 | FLAC + MP3 (then Ogg Vorbis, Opus, single-file AAC) encoders behind an audio::Encoder seam | xiph/flac | vendor codecs as libraries (third_party, pinned, --build-only) + port adapters ... | engine (rows: win, grant: director) | M (FLAC S, MP3 S, Ogg S) ... | EX-04 |
| EX3 | Export job as plain data (JSON schema), stem dependency closure (sidechain, aux-send returns, rack co-instances, cycle-safe) ... | tracktion_engine | port C++ (algorithms; ADI adds unknown-key rejection + case-folded names) ... | engine | M | EX-01, EX-02, EX-03, EX-12 +2 |
| EX4 | True-peak brickwall: stage order + upsample-and-hold idea with latency trim/flush | ardour | port C++ (extend ADI's dsp::LookaheadLimiter to double + true-peak target) ... | devices (+engine wiring) | M | DEV-29, MX-27, EX-05 |
| EX5 | Noise-shaped dither for Live's Pow-r slots | sox | copy tables with attribution + port loop (reserve headroom) | engine | S | EX-06 |
| EX6 | BWF v2 (loudness fields, EBU R99 originator ref, coding history), cue markers, time reference split | libsndfile | port C++ (layouts + fill rules; own LIST/adtl/labl writer; seeded random + injected clock) | engine | S | EX-08 |
| EX7 | Export sample-rate conversion as Live does it (SoX Resampler, Live Audio Fact Sheet 38.3.2) | soxr | vendor as library | engine (grant: director) | S-M | EX-04 |
| EX8 | Tag read/write oracle (ID3v2, FLAC/Xiph comments, MP4 ilst + NI stem atom, WAV INFO) | TagLib | vendor for tests (oracle) | engine | S | EX-08 |
| EX9 | AIFF-C fl32 writer pieces | soundcraft | port C++ | engine | S | EX-04 |
| FC1 | Recipe-named render cache lifecycle: renders named by recipe hash, nothing evicted except explicit delete so undo finds the old key ... | filmcraft | design-only (ADI's own ~150 lines, BLAKE3 over canonical CBOR; reject FNV-1a) ... | engine | S (design) / M (impl) | TM-FC-03 |
| FC2 | Prefix-hash render chain (stage i hash folds 0..i; skip stages with existing files), render single-flight per key, freeze point (prefix freeze) | tracktion_engine | port C++ (chain + single-flight, BLAKE3 keys, stage-0 key folds source media hash) ... | engine | M (L with ADR-0069 schema) | TM-FC-01, TM-FC-02, TM-FC-03, TM-FC-04 |
| FC3 | Three contracts in code: Consolidate Range (no processing) vs with processing vs Bounce vs Freeze ... | ardour | design-only (never its _history.clear()) | engine | S | TM-FC-02, TM-FC-03 |
| FC4 | Consolidate + commit reference: render_clips (clip FX, no mixer, skip muted), MIDI consolidate merges as it sounds ... | soundcraft | design-only + tests | engine | M | TM-FC-01, TM-FC-02 |
| FC5 | Real-time capture pass for external hardware inside an offline render | magda-core | design-only | engine | M later | TM-FC-04 |
| IC1 | DAWproject reader/writer (element walk, mapping helpers: velocity/pan/gain/controller normalisation, IDGenerator, IDRefResolver; ZIP audio/) ... | tracktion_engine | port C++ off JUCE onto pugixml + miniz; emit OpRequests (stem_import shape) ... | engine | L | TM-IC-03, IC-01 |
| IC2 | Second DAWproject codec with declared-loss table per feature + in-process XSD validation; VST3 state as .vstpreset; Sends, automation Points, Warps ... | magda-core | port C++ beside IC1 (loss table = C4-017 loss report) | engine | (in L) | TM-SV-02, TM-IC-03, IC-01 |
| IC3 | Nested-timeline flattener (recursive Clips, trim to parent window, loop unrolling, timeUnit inheritance) + Vst3Preset + CLAP state | git-moss/ProjectConverter | port C++ | engine | M | TM-IC-03, IC-01 |
| IC4 | Conformance oracle: Project.xsd + MetaData.xsd, DOM semantics, DawProjectTest scenarios (14 audio scenarios, CC1/pitch bend on track and in clips) ... | bitwig/dawproject | copy XSDs + fixtures with attribution (fetched test data, not committed) | engine (win: rows) | M | TM-IC-03, IC-01 |
| IC5 | XML parser | pugixml (release tag) | vendor as library (third_party, pinned) | engine (win: pin row) | S | TM-IC-03, IC-01 |
| IC6 | VST3 .vstpreset container (chunk IDs + 32-char class ID) | VST3 SDK in JUCE | port C++ (container subset) | engine | S | TM-IC-03, IC-01 |
| IC8 | SMPTE timecode DF/NDF + OTIO JSON subset + sample data | OpenTimelineIO | copy vectors + design (integer reimplementation; OTIO subset over nlohmann) | engine | M | TM-LS-03, TM-IC-01, TM-IC-04, IC-03 |
| IC12 | openDAW DawProjectImporter heuristics (primary output detection, Bitwig sends without enable) + DawProjectTempo.test.ts | openDAW | design-only | engine | S | TM-IC-03, IC-01 |
| IC13 | Live's 70 palette colours as RGB | DawVert | reference data (cross-check vs window lane's measurement; ADR-0211 d3) | win/window | S | TM-IC-03 |
| LD5 | Export loudness refine loop (measure, gain, TP limiter, re-measure, <= 4 trials, +/-3 dB steps, 0.02 LU plateau) ... | filmcraft | port C++ (TP detector path into ADI's own LookaheadLimiter) ... | devices + engine | M | DEV-22, DEV-29, MX-27, EX-05 |
| RE6 | Scope expansion (EditSpaceTargets: clip tracks + all sub-tracks + their automatables), close-gap parameter, trimAwayOverlap, mergeMidiClips ... | tracktion_engine | design-only (not auto-unfreeze on delete) | engine | S | ED-06, ED-07, ED-08, AU-05 |


### M6: MIDI effects, note tools, comping, automation passes, tunings and warp analysis

The MIDI-effect kernels of #207 and #208 are ported onto the transform phase and the note-identity
scheme (MF1, MF2, MF5, MF6, ME5), never rebased as they stand. The warp-analysis stack vendors
`qm-dsp` (WT1) behind one analyser, with beat_this as the benchmark's comparator (WT8) and Essentia
as a design reference unless Q-D picks it (WT7).

| Take | What | From | How | Lane | Effort | For rows |
|---|---|---|---|---|---|---|
| AU2 | Splice shape: mergeCurve (leaveOpenAtStart/leaveOpenEnded/fadeLength → GuardPolicy) | tracktion_engine | port C++ (not its fade maths) | engine | (in AU1) | AU-02, AU-03 |
| AU3 | Recording lifecycle: gesture vs value source, punch in/out, loop-wrap split, flush | tracktion_engine | port C++ | engine | M | AU-03, AU-09 |
| AU4 | Clip modulation maths + timing: absolute/relative/scale, linked/unlinked loops | tracktion_engine | port C++ (maths+timing) + design (Live pan law, domains) | win (ADR) + engine | L | AU-06, AU-07 |
| AU5 | RDP simplifier (Simplify Envelope core) | magda-core | copy with attribution (RDP) + own curve fit | engine | M (RDP S) | AU-08 |
| AU6 | Single-pass error-bounded thinning at drain time + write_range anchors | soundcraft | port C++ (keep coincident pairs as anchors) | engine | S | AU-03, AU-08 |
| AU8 | Recorder test vectors: latch until stop, loop wrap close/reopen, suspend on write, RDP never drops steps | openDAW | tests only (as vectors) | engine | S | AU-03 |
| CP1 | Capture passes over one source: CaptureInfo per loop pass (start, samples, loop_offset), punch clipping by 5 overlap cases ... | ardour | port C++ (bookkeeping only; preallocated pass log, no lock/alloc on callback) | engine | M | CT-02 |
| CP2 | Take-window arithmetic (closed-form pass windows, partial last pass) | qtractor | port C++ as oracle (adi::recording::loopPassWindows); drop zero-length last take | engine | S | CT-02 |
| CP3 | Loop-take splitting at the engine's wraps + stopping cases + MIDI late keys | openDAW | design-only + test scenarios | engine | S | CT-02, CT-03 |
| CP4 | MIDI pass boundaries: held notes closed at pass end, lane per discontinuity, one Record macro | zrythm | design-only + tests | engine | S | CT-03 |
| CP5 | Comp-section span algebra (assignCompSections, remapCompSpansAfterDelete) as planner/test oracle | magda-core | port C++ to int64 ticks (oracle, never the model) | engine | S | CT-01, CT-04 |
| CP6 | Comp-edge drag rule (clamp, covered sections removed, beyond-edge section keeps take) | tracktion_engine | design-only | engine | S | CT-04 |
| ME3 | Note-operation mechanics (Split identity rule, scale-degree stepping with directional rounding, rescale between scales, chord inversion, tuplets) ... | helio-sequencer | port mechanics under Live's rules (not Helio's Join/Reverse/Invert/Legato semantics) ... | engine | M | MI-04, MI-06, MI-08, TU-03 |
| ME5 | Shared arpeggio ordering for device + Arpeggiate tool: openDAW walk_index (18 modes, 15 match Live's styles, pure integer, expected sequences) ... | openDAW | port C++ (one pure allocation-free function, 18 styles) | devices (+engine tool) | S | MI-07 |
| ME6 | Scale presets: Push/Live scale set under Live's display names incl. Pelog Selisir/Tembung + Messiaen 3-7 (DrivenByMoss) ... | DrivenByMoss | copy data with attribution (Live 12.4 census decides order/members) | engine (census: measurement seat) | S | MI-06 |
| MF1 | MIDI effect kernels + transform phase (Pitch, Chord, Random, Scale, Velocity, Note Length, Note Echo, Monitor, CC Control, Arpeggiator) ... | in-house | port C++ onto the chain with the 10+ §1.4 fixes (prefix IDs, idempotent prepare) ... | engine + devices | L | DEV-19, DEV-07, DEV-18 |
| MF2 | OneShot instrument (+ Pd bridge, MIDI identity fold) and Sampler (32 zones via SnapshotPublisher) | in-house | land #278 + port #208 (fix idempotent prepare; macOS assertion) | devices (+engine dispatch) | M | DEV-21 |
| MF5 | MIDI-effect lifecycle contracts (release_leavers at chain swap; release_all on discontinuity) + arpeggio walk_index ... | openDAW | design-only (lifecycle) + port (walk_index, reseed rule) | engine + devices | S | DEV-19, DEV-14, DEV-18 |
| MF6 | MIDI bounce with processing up to an endpoint + note tracker closing open notes at range end | ardour | design-only (ADI: offline prefix pass → capture::prepareMidi; capture.createMidi) ... | engine | M | DEV-20 |
| RM7 | Nameable.comment on tracks/channels/devices/parameters/clips/markers/scenes = Live's info text one to one | bitwig/dawproject | design (DAWproject converter mapping for info text) | engine | S | RE-03 |
| TU1 | KBM semantics + key-table maths as TEST ORACLE + parser shapes (readKBMStream with 4 Scala fixes; readASCLStream as @ABL starting point) ... | surge-synthesizer/tuning-library | port tests + write own compileKeyTable/parsers (design from its shapes) | engine | M | TU-01, TU-02 |
| TU4 | .ascl / .scl / .kbm writers + MTS bulk dump; MTS byte maths | xenharmonic-devs/scale-workshop | port C++ (~290 lines; own MMA checksum, not its 0xff-seeded XOR) ... | engine | S | TU-02 |
| TU7 | Multi-channel keyboard layouts (index[2048][16]) + Scala name_1..16.kbm convention + trailing unmapped keys | helio-sequencer | design-only (layout half: Live's MIDI Controller Layouts; not its index note model) | engine/window | S | TU-02, TU-03 |
| WT1 | Beat tracker (comb filterbank + Viterbi tempo, DP beat placement), DownBeat (bar phase from beat-synchronous spectral difference) ... | c4dm/qm-dsp tag qm-vamp-plugins-v1.8.0 | vendor as library | engine | M | TM-WT-01, TM-WT-08, AGT-07 |
| WT2 | Hitpoint detector: vibrato-suppressed spectral flux on log bands, local-max + mean threshold + 30 ms refractory, sample-accurate attack start ... | soundcraft | port C++ (~200 lines, onto adi::dsp::RealFft; add strength + peakDb) ... | engine | S | TM-WT-01, TM-WT-08 |
| WT3 | Constant-tempo regions (ironing +/-12 ms jitter), metronome tempo across regions, BPM snapping in error band (integer, 1/2, 2/3, 1/3, 1/12) ... | mixxx | port C++ (Qt containers → std) | engine | S | TM-WT-01, AGT-07 |
| WT4 | Loop vs one-shot decision (tatum quantization fit, IsSingleEvent) on PFFFT | audacity-3.7 tag Audacity-3.7.9 | port C++ (loop-or-one-shot only; bar count follows Live's 1/2/4/8/16-bar 4/4 rule) | engine | M | TM-WT-05 |
| WT5 | Tempo estimate rounded to whole beats + bar count snapped up to 1/2/4/8/16 in 4/4 (= Live's even-length rule); probably_oneshot design echo | ardour | port C++ | engine | S | TM-WT-05 |
| WT6 | Warp-marker edit rules (endpoints snap back, adjacent ratio clamp) + streaming WarpReader priming (pre-roll, discard latency) ... | tracktion_engine | design-only (clamps measured from Live, intersected with engine limits) | engine | S (rules) / L (streaming) | TM-WT-02, TM-WT-04 |
| WT7 | Multi-feature beat tracker + LoopBpmConfidence (closed-form) | essentia | design-only (port only if benchmark shows qm-dsp clearly worse) ... | engine | L | TM-WT-01 |
| WT8 | Joint beat + downbeat tracker as benchmark comparator / downbeat fallback | CPJKU beat_this | benchmark comparator (needs a model runtime ADI lacks) | win/engine | S | TM-WT-01 |


### M7: threads, the scan service and the Pd editor

The graph scheduler takes SuperCollider's activation-count kernel, Tracktion's lock-free player
logic and libossia's pool (GT1 to GT3), with Ardour's real-time setup (GT4); VCV Rack's barrier is an
interim fallback only (GT7). The scan service and registry v3 take muse_framework's, Carla's,
Ardour's and Tracktion's scanner policies (SC1 to SC7). The Pd editor's attach (PD3) and the
compiled-in externals (PD6) follow Q-K.

| Take | What | From | How | Lane | Effort | For rows |
|---|---|---|---|---|---|---|
| GT1 | Lock-free multithreaded node player: per-node inputs-remaining counts, MPMC ready queue, chain continuation (RETURN_MID_NODES) ... | tracktion_engine | port C++ (logic only, JUCE-free; not its creator-thread RT upgrade) ... | engine | M | DEV-23 |
| GT2 | Activation-count kernel: items reset own count, first ready successor runs on same thread, node count reaching 0 ends the tick (any number of sinks) ... | supercollider | port C++ (replace boost::lockfree with third_party/lockfree MPMC or single-use per-block) ... | engine | S (kernel) | DEV-23 |
| GT3 | Persistent process-wide pool, dependency counts with acquire-release, caller co-runs until batch count 0 (no quiescence step), owner-only queue ... | libossia | port C++ (design + kernel; not notify_all per submit, not moodycamel implicit producers) | engine | (in S1/S2) | DEV-23 |
| GT4 | Platform RT setup + wake rule + RT task list | ardour | port C++ (MMCSS Pro Audio at AVRT_PRIORITY_NORMAL; never REALTIME_PRIORITY_CLASS) ... | engine (driver side) ... | S | DEV-24, DEV-23, MX-28 |
| GT5 | CLAP thread-pool host (request_exec served by pool, submitter helps, nested refused) + thread-check answering on any worker | zrythm | port C++ (on ADI's own pool) | engine | M | DEV-23 |
| GT6 | Device audio workgroup for workers | JUCE 9.0.2 (in tree) | use (vendor-as-library, already vendored) | engine | S | DEV-24, DEV-23, MX-28 |
| GT7 | Claim cursor + HybridBarrier (work-conserving within a level) | vcvrack | design-only (stopgap if S2 slips; fix Windows semaphore max count 1 hazard) | engine | S | DEV-23 |
| GT8 | Workers count as the audio thread only while rendering for it | soundcraft | tests only | engine | S | DEV-23 |
| MF3 | Pd MIDI egress: per-instance libpd MIDI-out hooks (noteon, controlchange, pitchbend, aftertouch) ... | libpd 0.16.1 | use (already vendored) + own wiring | devices (+engine for pd_engine.cpp per) ... | M | DEV-14 |
| PD3 | "Edit in Pd" live attach lifecycle precedent (libpd_set_instance + start_gui, 8 ms poll, stop on close) + installed-Pd locator; static port scan idea | ossia-score | port C++ (logic, not Qt; not its audio-thread vector push_back or CC+8192 bug) | engine (+window menus) | M (route A real cost) ... | DEV-11 |
| PD5 | Patch-owned data state protocol (bang save → lists via [s camomile] → restore to load), reloadPatch order (suspend, save, reopen, restore, prepare) ... | Camomile | design-only | engine/window | S | DEV-10 |
| PD6 | cyclone (compiled-in externals to widen the palette) / ELSE DSP objects | pd-cyclone | vendor as compiled-in externals (ADR-0188 d8 allows) | devices | M | DEV-15, DEV-11 |
| SC1 | Thread pool of per-plug-in child registrations with 15 s timeout + cancel, out-file results, main thread sole registry writer ... | muse_framework (Audacity 4's pin | port C++ | engine | M | DEV-25 |
| SC2 | Progress-reset deadline (30 s reset on every message), mtime-keyed hash, "nothing yielded" negative cache until mtime changes (= reinstall clears) | Carla | port C++ (policy) | engine | S | DEV-25 |
| SC3 | Scan-state taxonomy (New, Updated, Error, Incompatible, TimeOut, Blacklisted, Faulty), module-binary path to stamp (module_path_vst3) ... | ardour | port C++ (taxonomy + policy, ~150 lines; not per-file XML caches or text blacklist) | engine | M | DEV-25 |
| SC4 | Retry once in a fresh child before blaming the plug-in (mechanical half of Live's two-strike rule); thread-count setting | tracktion_engine | port C++ (~30 lines; not its in-process fallback) | engine | S | DEV-25 |
| SC5 | PluginDescription XML as VST3 cache payload (loadFromXml with no child), moduleinfo.json fast path, KnownPluginList round trip | JUCE 9.0.2 (in tree) | use (already vendored; stamp module binary + moduleinfo.json, not bundle dir mtime) ... | engine | S | DEV-25 |
| SC6 | Worker pool with deadline per worker, plain-text scan report, SQLite plugin store with seen-set sweep | magda-core | design-only (SC1 preferred for code) | engine | S | DEV-25 |
| SC7 | Helper self-test before each batch (ADI's own design; Audacity runs it only in CI) | audacity | design-only | engine | S | DEV-25 |


### Track C: the control core and the test kit

MAGDA's remote core and MCP bridge are ported once into `src/adi/control` (CA1 = A1, CA3 = A2) on
cpp-httplib v0.64.0 (CA2, a new `third_party` row, never v0.52.0); the window's test kit takes
melatonin's component model (CA5) and the pixel diff and golden kit (WT-1, WT-6; `pixelmatch-cpp`
as a test-only row).

| Take | What | From | How | Lane | Effort | For rows |
|---|---|---|---|---|---|---|
| A1 | Remote dispatcher: scopes/ScopeSet, client grants + first-use prompt, audit ring 512 w/ secret redaction, constant-time token/Origin check ... | magda-core head | port C++ ONCE with control-api CA1 (one dispatcher) ... | engine | in CA1 (L) | CTL-01, CTL-03 |
| A2 | MCP endpoint (tools+resources projection, error rules, _meta, both protocol eras) + Streamable HTTP server options + stdio bridge | magda-core head | port C++ ONCE with control-api CA3: stdio adi-mcp, no second listener ... | engine | in CA3 (M) | CTL-08 |
| CA1 | Remote API core: WebSocket transport options (loopback, token required, Origin, 8 connections exact, 240 KiB frames, in-flight 8) ... | magda-core head | port C++ JUCE-free (nlohmann::json, injected Executor, undo head instead of Revision) ... | engine | L | EX-09, CTL-06, CTL-07, CTL-01 +5 |
| CA2 | WebSocket/HTTP transport library | cpp-httplib v0.64.0 (24e58ce5) | vendor as library (third_party pin in tools/fetch_external.sh; one TU behind pimpl) ... | engine (pin: win) | S | CTL-04 |
| CA3 | MCP projection + stdio bridge (protocol eras table, legacy initialize + stateless server/discover, tool-error rules) ... | magda-core head | port C++ | engine | M | CTL-08 |
| CA4 | Control-channel rules: auth before anything, one refusal text, bounded pending auth, abandon timed-out requests (PENDING/TAKEN/ABANDONED) ... | gridcraft | design only | engine + window | M | CTL-01, CTL-03, WTS-10, CTL-05 |
| CA5 | Component + accessibility description for ui.elements/ui.inspect (role table, type demangling, font/LnF strings) | melatonin_inspector | copy with attribution | window | S (surface M) | WTS-05, WTS-10 |
| OS1 | op_schema: JSON Schema export from the op registry with MAGDA's default bounds (shared by agent and control-api) | magda-core head | port C++ (small); teach validateSubmission the same constraints | engine | M | EX-09, CTL-02 |
| WT-1 | Pixel diff with anti-alias detection, diff image, ignore mask, windowed diff | mapbox pixelmatch-cpp v8.0.0 | vendor header-only via fetch_external.sh as SYSTEM include (verifier) ... | window | S | MX-36, WTS-03 |
| WT-2 | Visual-test review job: base renders found by SHA, compare tool, label on change | musescore | design-only (verifier: MuseScore builds pull_request.base.ref - a branch tip) ... | window (win for the workflow) | M | WTS-04 |
| WT-6 | Golden testkit: diff metrics, EXACT and RENDER tolerances, bless, failures directory, size cap; snapshot update modes | filmcraft | port C++ | window | S | MX-36, WTS-03 |


### P2 and P3

Collaboration (C1 to C9), racks (RK1 to RK6), the Session View's launch engine (SV1, SV3, SV4),
remark threads (RM1 to RM4, RM6, RM9), MIDI files, AAF and OMF (IC7, IC9 to IC11), MIDI sync and
Link Audio (LS2, LS3), surfaces beyond the core (SU1's encoders and binding model, SU2 to SU6, SU9,
SU11), the agent's sidecar and consent
(A3, A5 to A7), Cubase's automation modes (AU9) and the MTS-ESP route (TU2, TU3, TU5, TU6, TU8;
only if Q-L says yes). New rows when they land: `LibAAF`, `mts-dylib-reference` (test-only), the
`pi-mono` and (its source file) npm packages.

| Take | What | From | How | Lane | Effort | For rows |
|---|---|---|---|---|---|---|
| A3 | Consent ledger (Disclosure kinds+sizes, ConsentRecord per app/connection/destination/capability/kinds, 4 terms, fail-closed 256 KiB atomic file) | craft-libs | port C++ | engine (+window) | S (+M UI) | AGT-03 |
| A5 | Sidecar agent loop, multi-provider LLM API, MCP client | pi-mono | vendor npm, exact pins + integrity | sidecar seat (win names) | M | AGT-06 |
| A6 | Run budget + termination reasons (steps 8, tokens 32000, mutations 8, identical calls 2, wall 2 min) | magda-core head | design only | sidecar seat | S | AGT-06 |
| A7 | Sidecar argument validation, and an MCP client fallback if pi-mcp lags the 2026-07-28 protocol | pi-mono | npm dependency, exact pins + integrity (only if needed) | sidecar seat | S | AGT-06 |
| AU9 | Freeze Trim data model (trim lane coalesced into volume) | soundcraft | port C++ (densify curves first) | engine | M | AU-09 |
| C1 | Collaboration core: sequencer (seq per batch, base_seq, dedupe, bounded catch-up log, id-space check), resume/catch-up protocol ... | figcraft | port C++ (ADI syncs typed ops; sequencer orders only; replicas validate) | engine | L | CO-03, CO-01, CO-02 |
| C2 | Touch sets (table, PK, columns incl. FK cascades) for collision detection + filtered undo | SQLite 3.49.2 session extension | vendor-as-library: target_compile_definitions(sqlite3 PUBLIC SQLITE_ENABLE_SESSION) ... | engine | S enable / M use | CO-03, CO-01, CO-02 |
| C3 | Fractional sibling keys + key reuse (longest increasing run) + respace | figcraft | port C++ (take figcraft's refusal of swapped bounds) | engine | S | CO-01 |
| C4 | Sequencer on a plain bucket: put-if-absent claim loop, 20-digit keys, S3 conditional-write quirks (R2 412 vs 304, If-Match 409 retry), SigV4 signing | delta-rs | port C++ | engine | M | CO-05, CO-01 |
| C5 | Presence/signals/spotlight, throttle 33 ms latest-wins, LAN hub threading + NetLimits, join codes (Crockford base32) ... | figcraft | port C++ | engine | M | CO-04 |
| C6 | Follow a collaborator's playhead: clock offset from 4 requests, advancePosition, resolveChain, transport lock | openDAW | port C++ | engine (+window U3) | S | CO-04 |
| C7 | Crash-safe resumable upload queue (block hashes, pending snapshots, missing-blocks uploader) | audacity | port C++ (design) | engine | M | CO-05 |
| C8 | Rebase by replaying named mutators (pending mutations rebased; throwing mutator leaves basis; lastMutationID per client) | rocicorp-mono | design-only (complements C1 for G5/G12) | engine | (in E4) | CO-01 |
| C9 | Protocol vocabulary and reconcile rules: sequenced-message fields; never revert locally plus the overlap rule; vector-only fallback ... | FluidFramework protocol-definitions (ISequencedDocumentMessage); openDAW | design-only | engine | S | CO-03, CO-01 |
| IC7 | SMF reader/writer (VLQ, running status, sysex/meta raw bytes) + DryWetMIDI corpus (63 valid, 666 invalid) + 16 reading policies as named anomalies ... | libremidi | copy with attribution (parser/writer, fix: SMPTE division, 64-bit ticks) ... | engine | M | TM-IC-02, IC-02 |
| IC9 | AAF reader (tracks, clips with essence offset, gain/automation, fades/crossfades, markers, timecode, embedded essence extraction) + Ardour mapping ... | LibAAF | vendor as library (static adi_libaaf, mutex: not reentrant) + port mapping | engine | M | TM-IC-04, IC-03 |
| IC10 | Clean-room AAF Edit Protocol + MS-CFB writer (SMPTE UMIDs, deterministic timestamps, weak refs, handles, mono breakout, 512-byte sectors) | filmcraft | port C++ (lowering from rows::Model is new) | engine | L | TM-IC-04, IC-03 |
| IC11 | OMF 2.0 read | filmcraft | port C++ | engine | M | TM-IC-04, IC-03 |
| LS2 | MIDI clock in (2nd-order DLL, SPP, stop rewinds to last MIDI beat), clock out ticker (sample offsets, Start/SPP+Continue, 0x3FFF clamp), MTC in ... | ardour | port C++ (algorithms against ADI's sample transport, 240,240 ticks per clock, SPSC rings) | engine | L | TM-LS-03 |
| LS3 | Link Audio: BasicLinkAudio, LinkAudioSink (realtime-safe commit with same session snapshot), LinkAudioSource ... | Link-4.1.1 | vendor (same pin) | engine | L | TM-LS-02 |
| RK1 | One graph with the player doing PDC for rack sub-graphs; macro modifier sources (remap, bezier depth); wrap selected plug-ins in a rack (Group) | tracktion_engine | design-only (verifier: tracktion builds each RackType once on shared buses and cannot) ... | engine | S | DEV-01, DEV-06, MX-24 |
| RK2 | Composite as a flattening host (not a nested engine): persistent per-child reconcile, choke by router-tagged EVENT_CHOKE (off/choke sorted before on) ... | openDAW | design-only + tests | engine | S | DEV-02, DEV-04, DEV-01, DEV-17 |
| RK3 | Zone admission (conjunction of key/velocity/controller ranges) + crossfade gains (gain and power laws) + choke arbitration tests | sfizz | copy with attribution (fade maths) + tests | engine | S | DEV-02, DEV-04 |
| RK4 | Live-shaped rack model (ChainElement variant of device or nested rack; ChainInfo volume/pan/mute/solo/bypass; macros 16 in pages of 8) ... | magda-core | design-only + tests | engine | S | DEV-03, DEV-04, DEV-01 |
| RK5 | Multi-point curve editor model (power per segment, smoothing, presets, flips, add-middle-point, 100-point cap) for per-target macro mapping curves | Vital | design-only (evaluation stays ADR-0159 curves.hpp; storage AAUT points) | window | S | DEV-06 |
| RK6 | Live Sampler zone layout (Min/Max/CrossfadeMin/CrossfadeMax) supports the positional 4-byte zone BLOB | DawVert | cross-check data | engine | S | DEV-02 |
| RE8 | Razor area format: start, end, envelope GUID, fixed-lane top/bottom, so a TrackSpan can name automation or take lanes | reaper-sdk | design-only | engine | S | - |
| RM1 | Thread closure (cycle-safe BFS over a children map), reply needs existing parent, reading-order listing with unanchored last ... | wordcraft | port C++ (algorithm + tests; flat threads per verifier) ... | engine (+window) | S | RE-02 |
| RM2 | Six-way time-edit relation table (before/after/surrounds/within/begins-in/ends-in) with point/region edge rules for insert/delete/copy/paste | audacity | port C++ (~120 lines; non-retain branch fixed; i64 ticks/ns half-open) | engine | S | ED-02, RE-02 |
| RM3 | Re-pin to nearest surviving parent on delete (undo pins back) + panel Filter/Sort/Query + bounded untrusted agent brief (256 KiB, 200 threads) ... | figcraft | design-only (re-pin rule as explicit remark.setAnchor in the same txn) + port C ... | engine + window | M | RE-02 |
| RM4 | Balloon cards: cascade layout, author stripe, 10-line clamp until selected, natural reading direction per paragraph ... | wordcraft | design-only (cache layouts by id/text hash/width/scale) ... | window | M | RE-02 |
| RM6 | Marker range relative to its item; slice copies markers to both pieces (evidence); converter mapping | OpenTimelineIO | design-only (content-relative clip ranges; audio anchored in source time per verifier) | engine | S | RE-02 |
| RM9 | Per-endpoint side (before/after) for anchor boundary cases | FluidFramework SequencePlace Side | design-only | engine | S | RE-02 |
| SV1 | Audio-thread launch engine: SlotKey, SyncRange, LaunchHandle (play/stop/playSynced/advance/releaseSection/scheduleBeat), LaunchRequestQueue ... | magda-core-head | port C++ onto integer ADI_PPQ ticks + samples + SnapshotPublisher + BlockScope pin ... | engine | L | TM-SV-03, TM-SV-04 |
| SV3 | Follow-action groups (contiguous occupied slots split by empty) + deterministic target resolver | tracktion_engine HEAD | port C++ + Live's ten actions written fresh (Next wraps = TE RoundRobin) ... | engine | M | TM-SV-04 |
| SV4 | Per-slot press/release state table for 5 launch styles (Live's Trigger = Ardour ReTrigger), atomic press counters, WaitingToSwitch state | ardour | design-only + port table as pure onEdge(phase, mode, edge) ... | engine | S | TM-SV-03 |
| SU1 | Encoders with inverses (applyMode 4 flavours, curves, ranges, toggle edge, normalized OSC), binding model, port identity (identifier then name) ... | magda-core-head | port C++ JUCE-free (std::string/nlohmann; NOT ControllerRouter write path which drops) ... | engine (+window sockets/ports) | L (the device-identity match alone, S, comes in M3 for SRF-05) | SRF-05, SRF-08, SRF-09, WIN-07 +1 |
| SU9 | Generic MIDI surface: enc-l/enc-r/enc-2/enc-b polarities, absolute threshold-in-sync rule (3rd Pick-Up oracle), 58 device maps (bank size, motorised) ... | ardour | port data + names/polarities (not its extra-step code); native drivers P3 | engine | M | SRF-04, SRF-08, SRF-03 |
| SU2 | Stream Deck + XL client: Elgato Node SDK (dial ticks/pressed, setFeedback, layouts) + director's VST controller (one-module bridge) ... | elgato-streamdeck | copy with attribution; @elgato/streamdeck 1.4.x as npm dependency ... | seat win names | M | SRF-13, CTL-05 |
| SU3 | Mackie Control protocol layer: SysEx headers/handshake/Logic Control challenge, fader pitch bend + servo echo, in-use guard, V-Pot decode, rings ... | ardour | port C++ (protocol layer as pure functions; device files to JSON) ... | engine | L | SRF-08 |
| SU5 | HUI codec (zones/ports, 14-bit faders + touch, V-Pots with HUI polarity, rings, aftertouch meters, scribble + timecode SysEx, 1 s ping) | DrivenByMoss | port C++ (~600 lines codec + tests) | engine | M | SRF-08 |
| SU6 | OSCQuery: HOST_INFO keys, tree JSON shapes, ACCESS 3, LISTEN/IGNORE/PATH_* | OSCQueryProposal | design-only (in-process HTTP GET responder on nlohmann + per-OS DNS-SD) ... | engine + window | M | SRF-10 |
| SU11 | Plain-OSC listen pattern (/get, /set, /start_listen, /stop_listen) | AbletonOSC | design-only | engine | S | SRF-09 |
| TU2 | MTS-ESP master wrapper (lazy dlopen of the USER's installed libMTS; 21 entry points; registration protocol) ... | ODDSound/MTS-ESP | copy with attribution (static-init load → lazy; injectable function table) | engine (after director R2) | M | TU-04 |
| TU3 | Open re-implementation of libMTS middleware (24-EDO master/client test programs) as the CI fake for an end-to-end master test through the real wrapper | baconpaul/mts-dylib-reference | test double (not installed) | engine | S | TU-04 |
| TU5 | Byte-exact MTS encoders: single-note change (7F/08/02, 08/07 with bank), scale/octave 1- and 2-byte (08/08, 08/09), tuning program/bank RPNs ... | Woyten/tune | port test expectations (not its KBM model) | engine | S | TU-04 |
| TU6 | Host sending MTS to a plug-in or a track (bulk + real-time single-note; immediate event to PluginInsert or MidiTrack) | ardour | design-only (its checksum includes 0xF0 - not copied) | engine | S | TU-04 |
| TU8 | CLAP tuning draft (get_relative per tuning_id/channel/key/sample_offset, should_play, clap_event_tuning) + octave-number/1 (note 60 = C3) | third_party/clap 1.2.10 | weigh as the CLAP-side alternative to MTS-ESP (ruling R2) | engine | M | TU-04 |


P3 (MIDI-CI):

| Take | What | From | How | Lane | Effort | For rows |
|---|---|---|---|---|---|---|
| SU4 | MIDI-CI (discovery, property exchange, subscriptions) over plain MIDI 1.0 SysEx | JUCE 9.0.2 (in tree) modules/juce_midi_ci | vendor-as-library (link juce::juce_midi_ci) | window | M (P3) | SRF-11 |


### Not taken

**Not usable:** POW-r dither; the EBU loudness test-sequence WAV set (the signals are synthesised
instead); the libMTS binaries (the MTS-ESP sources are usable); the REX2 decoding SDK; Ableton's
push-interface documentation; Live's MIDI Remote Scripts in any copy; the
Ableton-authored `.ascl` and `.kbm` fixtures in tuning-library's test data; h1data's M4L MIDI tool
examples; helgoboss-learn; the OSCQueryProposal repository (read as a protocol description only);
bitwig-docked-plugins; DawVert's (its source file) folder; pd-else's Live Electronics
Tutorial; Plugin Doctor and REW.

**Merged or superseded:** export EX2 (libebur128's LRA, whose 1 Hz hop is below Tech 3342's 10 Hz)
gives way to the one meter; midi-editing ME2 (vendoring tuning-library) gives way to TU1 (a test
oracle only); agent A1 and A2 are built once, inside CA1 and CA3, with no `src/adi/rpc` and no second
listener; IXWebSocket and cpp-httplib v0.52.0 are rejected; a new AANL analysis format duplicates
#449's ADIA sections; pywinauto never drives ADI's own UI.

**Rejected per area:** each matrix's "Reject or defer" lines (Appendix A) and the ledger's §4 "Per
area" lists keep the reasons.

---

## 5. Conflicts and their resolutions

The cross-check found 41 conflicts across the six matrices (XC-01 to XC-41, full texts in
the cross-check (win's working file) §2). Writing this plan found five more (XC-42 to XC-46). The
matrices' own 103 conflicts stay as each matrix resolved them, except the 31 the cross-check
touched (mapped at the end of this section).

### 5.1 One need written twice (owner row named)

| XC | Conflict | Resolution | Owner | When |
|---|---|---|---|---|
| 01 | Consolidate in two rows (ED-06, TM-FC-02); ED-06 placed the result Warp off as "win's placement", TM-FC-02 sent the warp state to the director | One Consolidate: TM-FC-02 owns the render, op, file and MIDI merge on the one capture; ED-06 keeps the selection's scope and Ctrl/Cmd+J; the warp state is his (Q-H); the Warp-off placement is withdrawn; normalization as Live; one FEATURES row at P1 | engine; win (row) | M5 (row in M0) |
| 02 | Save As, Save a Copy and the snapshots (HI-04, PD-01, PD-03): PD-03 wired the daily snapshot with no dependency, though every daily snapshot is a point compaction may never pass | PD-01 owns Save As and Save a Copy; PD-03 owns `snapshotAll` and Take Snapshot (now); the daily snapshot lands with HI-02's compaction rule after Q-A; HI-04 keeps revert and the historical tab (P2) | engine | M2, M3 |
| 03 | One loudness meter proposed twice (DEV-28, MX-27), read by MX-10, EX-05, MX-29, TM-MD-04, AGT-07 and Auto Gain Stage | DEV-28 owns `src/adi/dsp/loudness`; the engine rewires N13, normalization and the gain stage; the window draws B8; MX-27 is the acceptance view; every reading takes its numbers from it | devices, or the engine seat under XC-46's loan | M4 |
| 04 | Export loudness twice (DEV-29, EX-05) | EX-05 owns; DEV-29's items are its items; the NI Stems refusal lands once, now, as EX-10's; the after-AAC check alone lifts it | engine | M5 (refusal M0) |
| 05 | NI Stems twice (TM-IC-05 decided, EX-10 refinement) | One row, EX-10, decided (ADR-0205 d8); its macOS part is PD-00's, its normalization XC-04's | engine | M5 |
| 06 | The Loudness Drive twice (DEV-30 refinement, MX-35 a director question) | A refinement: ADR-0203 d14 rules the mechanism (a gain over the leaf tracks' input gain); the ruling removed only the ADI-L stages; build the gain now, engaging nothing; tell him as a placement to strike; the engine's "blocked" entry lifts | win (record); engine | M4 (record M0) |
| 07 | A sidechain key into a device twice (DEV-05 decided, MX-24 refinement) | One row, decided (FEATURES P1, ADR-0203 d3), built after the tap placement (XC-15) | engine | M4 |
| 08 | CPU meter and Performance Impact twice (DEV-24 decided, MX-28 refinement) | One row, Live's way under d1; win adds the FEATURES row; the callback load from JUCE at once, per-track impact with DEV-23's timings | engine; window | M7 |
| 09 | The function keys in three places (WIN-01, MX-14, the Mix View program's round-M1 fix) | One window item, WIN-01: F1 to F8 lose Live's activators (his deviation), F4 is the Mix View only, F3 and F7 held; MX-14 keeps the Mix View's switch, detach and re-dock | window | M1 |
| 10 | The crossfader twice (MX-15, TM-SV-05) | MX-15 owns (the gain node, a side table, the controls); TM-SV-05 refers to it | engine; window | P2 |
| 11 | Interchange twice (TM-IC-03 and IC-01, TM-IC-02 and IC-02, TM-IC-04 and IC-03) | The time-media rows own (every converter runs on TM-IC-01's exact time base); the IC rows refer; dependency lists merged (TM-IC-04 also needs EX-04's writers) | engine | M5, P2 |
| 12 | Edits on a frozen track twice (ED-08, TM-FC-04) | TM-FC-04 owns what each edit does to frozen material, after the Live readings; ED-08 keeps grouped, hidden and linked tracks; ripple stops at frozen material | engine | M5, M6 |
| 13 | Templates and the CTZ template twice (PD-14, MX-31) | PD-14 owns the mechanism; MX-31 owns the CTZ template's content; PD-14 gains a dependency on the roles table | engine; window | M5, P2 |

### 5.2 One runtime for one job

| XC | Conflict | Resolution | Owner | When |
|---|---|---|---|---|
| 14 | No owner for the capture that export jobs and derived renders (freeze, bounce, consolidate, Paste Bounced Audio, stems, `.adiviz`) all need | One engine design, written before EX-02 or TM-FC-02: the revision and names read at job start (ERRATA-11), source taps from the one vocabulary, range, loop and tail windows, the route treatment (Direct, ViaMain, MainFxOnly), incoming routes pruned on request, latency guards and verified inputs; two clients; renders in process on DEV-23's offline pool or serially | win (ADR); engine | M5 (ADR in R-d) |
| 15 | Three enums name the same tap points; three stores for "where a signal is taken" (a tap side table, a text grammar with `track:<id>`, a Link Audio minor) | Engine: `plan.hpp`'s `Tap` extended with chain input and device input and output; `OfflineTap` and `ScopePoint` retire; choosers show Live's three names. Format: one placement by win before MX-24, MX-25, DEV-20, TM-FC-04 or TM-LS-02: a signal from another track is a row with real foreign keys and its tap (copy, templates, DAWproject, rebase and undo all remap keys, never strings); external sources keep `input_ref`'s grammar; a whole new table for track inputs and MIDI From; the sidechain's tap in a side table keyed by routing id | win (R-c); engine | M0, then M4 |
| 16 | Clip modulation computed in the automation evaluator, modulators writing their own offsets, macros writing base values: three schemes and two playing values | The ADR ADR-0046 owes, written once: contributor kinds (automation, macro takeover, clip envelopes, modulator routes); ADR-0114's order in each target's domain; one evaluator per block delivering the base and one summed offset per target to CLAP, VST3, built-ins and Pd; one lock-free playing-value slot per target; one ownership state per parameter (override, macro takeover, controller takeover, Link's tempo) driving Re-Enable Automation | win (ADR); engine | before AU-06 (M6) and DEV-07 (M7) |
| 17 | Three gesture implementations, two coalescing rules that read as opposites | One GestureSession contract (begin, update, commit, cancel, one label, undo refused while open) with two preview strategies: live values for continuous scalars, an always-rolled-back transaction for structural edits; the quiet window only as the end detector for sources without a release; OPS §6.4's 300 ms merge retires; surfaces and the control API open the same sessions; AU-03's write pass is the one place a touch becomes automation | win (ADR); engine; window | before M2 |
| 18 | A MIDI 1.0 port host now and a UMP host later, with two owners (SRF-05 window, TM-LS-03 engine), against ADR-0188 d7 | One port host owned by the window (`src/juce/app_midi_ports.*`), on JUCE's UMP API from the start with the Windows MIDI Services backend off (the Win32 backend, named as such in Settings; the service is SRF-16 at P2: it needs Windows 11, an SDK pin, a NuGet package and an installer prerequisite, none of which his Windows 10 box or CI has); two outlets (Remote on the message thread; Track and Sync as timestamped rings into the engine); the engine owns the queues, the Track tap, capture (#446, #447), clock and timecode decoding and the clock-out scheduler | window; engine; win (LANES) | M3 |
| 19 | Per-file analyses recomputed per tool; a second FFT worker; key detection chosen in two places | Per-file analyses computed once into ADIA sections; AGT-07 reads them; project-level analyses run as jobs on MX-30's kernels; CTL-07 serialises the worker's outputs; Q-D picks the suite's analyser | engine; devices | M3, M4, M6 |
| 20 | One queue, three staleness rules (any head movement, the recipe key, the declared scope); a stem separation would fail because an unrelated fader moved | `render::Queue` stays the one renderer with ERRATA-11's four moments; AGT-04's state machine wraps it and the other workers; a job that commits into the project declares its read set and is refused as stale only when those inputs changed (refused, never rebased); interactive edits keep ADR-0153's expected head | win (ADR); engine | M5 |
| 21 | Two audits of the same two dozen `clips` readers (CT-01 at P1, TM-SV-02 at P2) | One engine accessor for "timeline clips that play" (the main lane, or the auditioned take lane; never a slot clip); every reader moves to it once, in CT-01's first PR; TM-SV-02 extends it | engine | M6 |
| 22 | Every new producer adds a `MAX(id)+1` site that collaboration must later find | One allocator seam (`MAX+1` inside until CO-01; callers still pass literals, so ADR-0021 holds): the window's B2 and C7 ids through one helper in M1; the engine's sites and that helper meet in one function in M2; one clipboard model (ADR-0068 d3) for ranges, devices, racks and projects | window; engine | M1, M2 |
| 23 | Two tab systems (HI-04's historical tab, PD-15's project tabs) | One tab host and one cross-project paste, owned by PD-15; the historical tab is its read-only case | engine; window | P2 |
| 24 | Three records of a running instance (PD-05's session marker, CTL-05's discovery record, the scanner's pending rows) | The control discovery record lives in PD-05's session marker, with one sweep; `ADI_HOME` redirects all of them for test runs | engine | M5, Track C |
| 25 | Three note-id namespaces; the Link consumer flushing and re-chasing held notes on every rebuild; two arpeggio orderings | A clip's runtime serials derive from the stored identity (clip, stored note id, loop iteration) under the track's prefix, so rebuilds keep held notes; the in-flight fix keys held notes that way and DEV-17's ADR records it; one allocation-free ordering function in `src/adi/dsp` for the Arpeggiator and Arpeggiate | engine; devices | M3, M6 |

### 5.3 Against a ruled ADR, the standing rule or the ruling of 5 November

| XC | Conflict | Resolution | Owner | When |
|---|---|---|---|---|
| 26 | EX-11 offered a render child process, against ADR-0203 d8 (plug-ins run in process; only the scanner runs out of process) | Renders run in process (instances created and given state on the message thread, processing on a render worker, `serviceHost` marshalled); the child-process option is struck unless he amends d8; the same holds for every capture | engine | M5 |
| 27 | The 5 November ruling recorded only in the needs table (N16) while many texts still describe slots | One win docs PR (R-a) before the engine takes the Drive and before the window builds anything slot-shaped: FEATURES, LANES, BUILD-BRIEF, UI-ARCHITECTURE, parity checklists, the ledger's §1, the Mix View program's slot parts, the engine state map's ON5 and ON6, and the two phrases (MX-36, WIN-03) that placed the unified feature; it designs nothing of the unified feature; N16 keeps the drawn slots off and unremoved until his list; the DJ Filter stays on Auto Filter's cores | win | M0 |
| 28 | TU-02 and TU-03 kept ADI's tuning ruler in pitch order, "a recorded difference" no ruling backs | Live's index order by default (Hz on hover); pitch order only as an addition if he asks | win (placement) | M6 |
| 29 | TM-SV-04 drew follow-action chance from a stateless seeded hash, so every pass is identical; Live draws again each pass, and d7 keeps "each pass may differ" as the default | A fresh draw per pass; a seed within one pass keeps a realtime pass and its render equal; no keep switch proposed | win (placement) | P2 |

### 5.4 Sequencing and dependencies

| XC | Conflict | Resolution | Owner | When |
|---|---|---|---|---|
| 30 | The engine seat walks BUILD-BRIEF Phase D while window needs (N29, N17) and P0 work (the edit kernel, tempo ramps) wait, against ADR-0210 d2 | The Link consumer, in flight on 5 November, finishes to its gate first: a one-time, named exception to ADR-0210 d2, told to him as P-9 rather than written into LANES as a rule; thereafter d2's order holds: N29, N17's open part and the edit kernel (M2); then tempo ramps on Link's effective-tempo path; D11's monitoring switches need MX-25's grammar, placed in M0 | win (tell the seat; P-9) | M0 |
| 31 | #456 pins `Link-3.1.5` under the local consumer; Link Audio and the 4.1.1 CMake change come later | Re-pin inside the same stack before #456 merges, Link Audio off, whatever Q-F says | engine | M0 |
| 32 | Rows that depend on the undo cap without saying so (the daily snapshot, TM-FC-03's collector, PD-07's Backup copies) | TM-FC-03 lists HI-02; D1 and Q1 go to him together (Q-A); PD-07's tradeoff names HI-02 | win | M0 |
| 33 | Window items that would ship ahead of their engine contract (raw move and resize drags, E6's bounce prompts, C7's UI batching, G1 to G3 on defective note ops, silent faders, A1 and A4 on red macOS paths) | ADR-0210 d3 holds: the window ships on what exists, binds to the engine row when it lands, and keeps its parity rows partial until then; E6 states the six bounce differences; C7 binds Lock Envelopes to AU-05; A1 and A4 assert only what passes on every leg until PD-00 | window | M1, M2 |
| 34 | Dependencies missing from rows | Added to each row: DEV-07 needs DEV-05 and MX-24; DEV-19's Use Current Scale needs TU-01 and MI-06; MI-07 needs DEV-19's ordering; CT-03 needs SRF-05, #446 and #447; AU-09 needs SRF-05; SRF-02 and MX-23 recording automation need AU-03; CTL-07 needs MX-30; TM-FC-02's group bounces need EX-02; TM-WT-08 needs DEV-21 and DEV-04; PD-14 needs MX-29; PD-06 needs CT-02; MX-09 on MIDI tracks needs SRF-05; DEV-20 and TM-LS-02 need XC-15; MX-32 needs a Live reading first | win (records) | M0 |
| 35 | Rows missing from their own matrix's milestones (ED-05, RE-03, TM-MD-05, MX-06, MX-32, DEV-20, DEV-27) | Each placed in §2 | win | M0 |
| 36 | F7 (offline processing) bound by ADR-0203 d6 and directed by ADR-0069, but no FEATURES row and no priority; the research's first Cubase priority is exactly its evidence | The editing area owns it; a FEATURES row at P2 beside Cubase's fade editor (told as a placement to strike); WIN-01 leaves F7 unbound until then | win | M0 |

### 5.5 Shared resources and texts

| XC | Conflict | Resolution | Owner | When |
|---|---|---|---|---|
| 37 | Schema minor 1.20 claimed four times (roles, Session tables, remark threads, tuning keyboards); many rows want "the next minor" | One register kept by win; a number is given when the engine PR that writes the tables opens, in merge order; 1.20 is the roles table alone; every minor adds whole tables only (ADR-0144 d3, check 9) | win | M0 (R-c) |
| 38 | Every area wants Live readings and no seat is named (the second Windows machine has Live 12.4 on a used profile, the Mac Live 11.2.7, the director's Windows workstation no Live) | One harness (MX-33's record schema and Live driver; PR #220's interaction logger for GUI cases), one queue ordered by the milestone that needs each reading (§7), one seat he names (G-1); until then every Live case stays NOT RUN and parity rows stay "built, not verified" | win; the director | M0 |
| 39 | Lane paths nobody owns (the Pd runtime files, the MIDI port host and the engine's MIDI queues, control, surfaces, `adi-mcp`, the agent, the Stream Deck plug-in, network code) and one collision (`src/adi/sync` for both collaboration and MIDI sync) | One LANES PR (R-b); collaboration takes `src/adi/collab`; seats named for the sidecar, the Stream Deck plug-in and the measurements | win | M0 |
| 40 | Network listeners added one by one (the control socket, OSC, OSCQuery, a LAN session, Link Audio, AudioGridder, the LAN audio node, bucket uploads, model consent); Q5's tradeoff called the LAN session "ADI's first network listener" | One network stack in one LANES path; Q5's wording corrected before it is asked; one list in Settings' Privacy group of everything that can leave the machine, each item off by default; AGT-03's ledger is the only consent system | win; engine | M0, then as each lands |
| 41 | The first-window gate passed on 2 November (R1 to R7 on `add5102`) but several rows still read it as pending | Those rows are P2 by priority, not gated by it (§2) | win (texts) | M0 |

### 5.6 Found while writing this plan

| XC | Conflict | Resolution | Owner | When |
|---|---|---|---|---|
| 42 | The Dream Reference v0.2 (his text) still describes the withdrawn slots: chapter 7 ("Every channel and every group carries ADI-Q, the EQ, and ADI-L, the limiter, present but switched off"; "the limiters inside the channels are the stages of the Loudness Drive"), chapter 8 ("the EQ, the limiter and a sidechain slot are built into every strip") and chapter 9; a lane reading the Dream as intent could build slots | His ruling of 5 November is later and wins. R-a notes in LANES and BUILD-BRIEF that those passages are superseded until he revises the Dream; win does not edit his Dream text; at his next revision the passages follow the ruling | win (note); the director (text) | M0 |
| 43 | DEV-26's acceptance runs "in ADI.exe on a desktop that is not the director's", while ADI.exe is never driven on his live desktop and a Windows Sandbox on the director's Windows workstation needs his OK (WTS-01) | Split the check: the headless half now on the director's Windows workstation under `ADI_HOME` (scan in a child process, load, parameter edits as single undo steps, state hashes after reopen, ADI-L's lookahead null, the placeholder); the editor-window half in the window lane's seat or a Sandbox with his OK | win | M1 |
| 44 | CTL-05 (where the control API's secrets live) is a "new choice" in its matrix, while the cross-check notes that option (a) amends neither ADR-0039 d4 nor ADR-0181 d2 | The first draft recorded option (a) as win's placement (the former P-6) and retired Q-O. This revision restores Q-O: ADR-0039 d4 rules a token "generated per session and shown in the UI" and rejects "any tokenless local-is-fine mode"; a discovery file any process of his user can read makes the token ambient, which is that mode in effect, a security-posture change to his ruling (AI-AGENT §7.1). His questions are Q-A to Q-P | the director (Q-O) | Track C |
| 45 | The engine lane's seat runs without MSVC and without the GitHub API, yet owns rows whose proof is Windows-only (DEV-23's MMCSS workers, MX-17's WASAPI loopback, TM-IC-05's Media Foundation path, the scanner's Windows fixes, AUDIOENGINE-1, DEV-26) | The Windows CI legs, checked by SHA, gate those rows; win fixes MSVC and Windows defects on the branch, as it already does for the engine's stacks, opens the PRs the seat requests, and runs the the director's Windows workstation measurements; nothing Windows-only is called done from a Linux run | win | M1 to M7 |
| 46 | The devices lane is idle (last log entry 28 October in `collab/devices.md`; last merges #250, #253, #255 and #259 on 1 November), yet the plan put gates on it without a fallback: the Pd safety overlays in M1 (PD1, PD2, PD4), the one loudness meter in M4 (DEV-28, on the critical path of EX-05, DEV-29 and MX-27), the limiter's true-peak path in M5 (EX4, LD5), the MIDI effects, OneShot and effect parity in M6, the modulators in M7; and LANES gives `src/adi/dsp/**` to devices alone | R-b names the fallback: relaunch the Codex devices seat with DEV-28 first in its queue; until it logs again, `src/adi/dsp/loudness` and `src/adi/dsp/limiter` are lent to the engine seat by a LANES §1 line and the Pd overlays go to the seat that is running; "devices seat alive or re-seated" joins M0's gate; the loan ends the day the seat logs | win (LANES); the seat | M0, then M1 to M7 |

### 5.7 The matrices' own conflicts the cross-check touched

| Matrix conflict | Resolved by |
|---|---|
| editing 1 (Lock Envelopes in the UI or the engine) and 2 (raw clip moves) | XC-33: the engine's AU-05 and ED-03 |
| editing 4 (clip envelopes and #434's overlay) | XC-16 |
| editing 6 (nothing may wire the daily snapshot before D1) | XC-02, XC-32 |
| editing 7 (coalescing) | XC-17 |
| editing 15 (several rows want the next minor) | XC-37 |
| time-media 1 (the Link pin) | XC-31 |
| time-media 7 (two cache formats) | kept; XC-19 extends it to AGT-07 and CTL-07 |
| time-media 14 (the built bounces differ from Live) | XC-33 (E6) |
| time-media 18 (tempo ramps have no owner) | XC-30 |
| time-media 19 (rebuild continuity) | XC-25 |
| time-media 20 (who owns the MIDI port layer) | XC-18 |
| time-media 22 (Consolidate in no row) | XC-01 |
| devices K1 (the ruling against the texts) | XC-27 |
| devices K2 (nobody owns the Pd runtime files) | XC-39 |
| devices K10 (three LUFS implementations) | XC-03, XC-04 |
| mixer C1, C2, C11 (the ruling; the Drive; two readings of it) | XC-27, XC-06 |
| mixer C3 (the role column) | XC-37 |
| mixer C4 (F4 bound twice) | XC-09 |
| mixer C5 (sidechains play nothing) | XC-15 |
| project-data 1 (ADI-L's source in export takes) | XC-27 |
| project-data 2 (#449 against PD-04) | M3's order: #449 first |
| project-data 9 (two job systems) and 10 (one capture) | XC-20, XC-14 |
| project-data 15 (no LANES path for network code) | XC-39, XC-40 |
| control 8 (the function keys) and 9 (N16 withdrawn) | XC-09, XC-27 |
| control 15 and 16 (one MIDI port host; `midi.port.*` has no home) | XC-18; R-e's registry rows |

---

## 6. The director's choices

Only choices that are his: an addition Live lacks, a case where Live's way would undo something he
directed, one of his own rulings, or a seat. Everything else in the 233 rows is decided, Live's way
under ADR-0203 d1, or win's placement to strike if wrong. The same choices are in
the choices file (win's working files) (one object each: id, question, recommendation, why,
options, needed_by).

The six matrices asked 21 questions. The cross-check turned one into a placement (the Loudness
Drive, XC-06) and grouped the rest; this plan's first draft turned one more into a placement
(where the control API's secrets live, XC-44), which this revision restores as a question (Q-O),
and adds one the first draft had struck silently (reading Live Sets, Q-P). The grouped asks are
split into single choices: **seven now (six choices, Q-A1 to Q-E, and the gate G-1) and fifteen
later (Q-F1 to Q-P).** The letters keep the cross-check's grouping (Q-A is its D1 with Q1; Q-K its
C-1 to C-3; Q-N its Q4 and Q5).

### 6.1 Now

#### Q-A1. What stays reachable when the undo cap is reached

- **Question.** Your history keeps every step up to 2000 steps or 1000 MB (ADR-0203 d13), and compaction may never pass a named snapshot or a reachable branch point; the daily automatic snapshot is not named, and d13 drops the oldest automatic one first, so it need not block compaction (main treats it as a block today, which is a defect fixed inside HI-02). What does block it: every Ctrl+S (a named snapshot) and every undo followed by a new edit (a branch point). After the first of those, 2000 steps can pile up behind it and the cap stops holding; a Ctrl+S on a new project keeps compaction off for the life of the project, and the file grows without limit. When the cap is reached, what must stay reachable?
- **Options.**
  - A: every named snapshot and every dead end kept as a deduplicated stored state; the cap always holds
  - B: only the snapshots and branches you named, and the branch a revert leaves, hold the history as d13 reads; unnamed dead ends older than the cap go with their steps (one early Ctrl+S still keeps the cap off for the life of the project)
  - C: as built: nothing behind a named snapshot or a fork is ever compacted, and files keep growing
- **Recommendation.** A: every named snapshot and every dead end stays openable and revertible as a stored state, kept once by its content; only the step-by-step undo older than the cap is compacted, so the cap always holds. The daily snapshot follows d13 under every option (compactable, oldest dropped first).
- **Why, and the tradeoff.** It keeps both things you asked for: a dead end is still there to come back to (Dream chapter 12), and the cap you ruled holds however often you press Ctrl+S. Tradeoff: A is the largest history item (L). Each kept state is stored once and deduplicated row by row, so the file grows only with what actually differs; those states can be opened and reverted to, but not stepped through one undo at a time beyond the cap. B is d13 as written and cheaper (M), and it is closer to the ruling than the first draft said: it loses only unnamed dead ends past the cap, and it does not cure the Ctrl+S-on-a-new-project case; C costs nothing now, but projects grow without bound and every commit slows as the kept window grows. Under A or B, whether compaction may fail an edit is placed, not given: P-10 makes it best-effort (it reports and never fails a commit), to strike if you want d13's cap enforced strictly.
- **Needed by.** Now: it decides HI-02's compaction rule and when the daily snapshot is wired (M2's history gate; the daily snapshot itself follows d13)
- **Rows.** HI-02, HI-04, PD-03, TM-FC-03

#### Q-A2. Live's Backup folder

- **Question.** ADI has no save step, so Live's Backup folder (a copy of the Set after each save, the last ten kept) has no trigger, and your R-26 says 'Backups: none'. Should Ctrl+S, which takes a snapshot in ADI, also keep Live's Backup copies outside the project file?
- **Options.**
  - (a) As Live: ten timestamped copies in the project's Backup folder from the second Take Snapshot; the oldest goes to the Recycle Bin
  - (b) No: R-26 stands; the manual Back Up Project command (P2) is the only copy outside the file
  - (c) As (a), with three copies
- **Recommendation.** (a): as Live: from the second Take Snapshot, ten timestamped copies in the project's Backup folder, the oldest to the Recycle Bin (R-26 reversed for the project file only).
- **Why, and the tradeoff.** The snapshots live inside the .adi, so they do not survive losing or damaging that file; a backup copy does. Tradeoff: disk. An .adi carries its history up to the 1000 MB cap, so ten copies can be large; they stay bounded only because Q-A1's answer A makes the cap hold. Each Ctrl+S writes a full copy on a worker, so nothing stalls. Three copies (c) costs less disk and protects less.
- **Needed by.** Now, with Q-A1 (the copies' size depends on the cap); built in M5 (PD-07)
- **Rows.** PD-07

#### Q-B. What STACK shows before the unified feature

- **Question.** Your ruling of 5 November withdrew the per-channel slots, and the unified built-in feature waits for your list. Until it arrives, what should the Mix View's STACK show, and should the window lane work on the Mix View now?
- **Options.**
  - (a) Build the non-slot parts now: the Reference, the strip scope and spectrum, the Scope window; then STACK as a live list of every track's own devices with the fisheye, navigator, search and pins, the drawn slot columns kept off
  - (b) Only the decided P1 parts now (the Reference, the strip scope and spectrum, the Scope window); STACK stays as drawn until your list
  - (c) Freeze all Mix View work until your list
  - (d) As (a), and also remove the drawn slot columns and the strips' Q and L switches now
- **Recommendation.** (a), after the mixer's own Live items (worklist B1 to B8), which stay first.
- **Why, and the tradeoff.** STACK becomes useful at once, with Live's devices and your VST3s as ordinary plug-ins, and the taps and the one visual worker serve every visualizer later. Tradeoff: STACK's row layout may be revised when the unified feature takes its place, and window time goes here rather than to later Live-parity passes. (b) is safe but leaves STACK as drawn; (c) stops the Mix View; (d) removes drawings that the unified feature may want back.
- **Needed by.** Before the window starts Mix View work beyond the decided P1 parts (M4)
- **Rows.** MX-36 (and MX-31, MX-21)

#### Q-C. When the control API starts, and on which seat

- **Question.** The control API's consumers are P2 (the agent, the surfaces, visual streams, scripts, the ArtCraft bridge), and no lane's queue holds its core. When does it start, and on which seat?
- **Options.**
  - (a) Now, as infrastructure, on a seat of its own: the schema export, the core and socket, a headless adi-mcp, the app host and the test grant
  - (b) At P2, in the engine lane's queue after its Phase C and D items
  - (c) Now, in the engine lane, ahead of Phase C and D
- **Recommendation.** (a): now, on a seat of its own, in parallel with the lanes.
- **Why, and the tradeoff.** It gives Claude and Codex a safe way to drive and test a running ADI without your desktop, and brings the agent earlier, while the user-facing agent and surfaces stay P2. Tradeoff: a seat's capacity and win's review time now. (b) costs nothing now but leaves testing on in-process tools and the agent months away; (c) slows the Live-parity work you are waiting to test.
- **Needed by.** Now: it decides whether Track C starts in parallel
- **Rows.** Track C: CTL-02 to CTL-09, AGT-01, AGT-02, AGT-08, WTS-03 to WTS-06, WTS-10

#### Q-D. One beat, tempo and key analyser for the suite

- **Question.** Your ruling ADR-0145 d10 (with ADR-0188 d8) puts ADiJ's beat grids on Essentia, and FEATURES names Essentia for sample tags; the code harvest proposes qm-dsp (Mixxx's tracker) for the DAW's Auto-Warp and key detection. ADI and ADiJ share one library and one analysed folder keyed by the file's hash. Which beat, tempo and key analyser should the suite use?
- **Options.**
  - (a) Benchmark first, then one analyser for both apps; qm-dsp in the DAW meanwhile, behind one interface
  - (b) qm-dsp for both apps now (your ruling amended)
  - (c) Essentia for both apps now (the DAW's P0 tempo detection waits for its port)
  - (d) Keep two: qm-dsp in the DAW, Essentia in ADiJ
- **Recommendation.** (a): benchmark first on a labelled local corpus, with beat_this as an independent comparator, then one analyser for both apps; meanwhile the DAW builds on qm-dsp behind one analyser interface, and ADiJ is untouched.
- **Why, and the tradeoff.** One file then shows one BPM and one key everywhere in the suite, and numbers decide which analyser. Tradeoff: a benchmark harness, and perhaps re-analysing cached files if the winner changes (cheap: the cache sections are versioned). (b) overrides your ruling without evidence; (c) is heavier to port and delays the P0 tempo detection; (d) shows two different tempos for the same file in one library.
- **Needed by.** Before key detection lands (M3) and before Auto-Warp's analysis core (M6)
- **Rows.** TM-WT-01, TM-MD-04, AGT-07

#### Q-E. Short samples on drop

- **Question.** Your deviation says a file dropped on an ADI track arrives clean, with no warp at all. Live's Loop/Warp Short Samples (Auto) warps and loops a short sample it judges to be a loop, on drop. Does your deviation cover short samples too?
- **Options.**
  - (a) Yes: every drop arrives clean; Live's short-sample result on Warp or from the import prompt; Live's four choices in the setting
  - (b) No: short samples follow Live's Auto on drop, so loops snap to tempo at once; long samples stay clean
  - (c) (a) by default, with Live's on-drop behaviour as a choice on the Record, Warp & Launch tab
- **Recommendation.** (a): yes, every drop arrives clean; Live's short-sample result comes when Warp is pressed, and from the import prompt's Warp choice; the setting offers Live's four choices.
- **Why, and the tradeoff.** It keeps your rule, a sample plays as it is until asked, true for every file, and still gives Live's result one press later. Tradeoff: a loop dragged from the browser does not snap to tempo on drop as it does in Live; it takes one press or the prompt. (b) gives Live's speed to loop users but splits the rule by length, so a 59-second and a 61-second file behave differently.
- **Needed by.** Now: the registry row is written in M0's records; the behaviour is built in M6 (TM-WT-05)
- **Rows.** TM-WT-05

#### G-1. The Live 12 measurement seat (a gate)

- **Question.** A gate rather than a product choice: which machine and session take the Live 12 readings? Every Live behaviour in this plan comes from the manual until it is measured, and parity rows stay 'built, not verified' until then. No seat is named: the second Windows machine has Live 12.4 on a used profile, your Mac has Live 11.2.7, and the director's Windows workstation has no Live.
- **Options.**
  - (a) the second Windows machine's Live 12.4 with a fresh second profile, driven by a Claude measurement seat; Mac-only rows on your Mac
  - (b) Install Live 12 on the director's Windows workstation and measure there, outside your desktop session
  - (c) You take the readings yourself from the AWAITING list
- **Recommendation.** (a): the second Windows machine's Live 12.4, with a second, fresh Windows user profile for the factory-default readings, driven by a Claude seat through the measurement harness and the interaction logger, never while you use the machine; the Mac-only rows on your Mac.
- **Why, and the tradeoff.** Live 12.4 is already installed there beside the other reference programs the research inventoried (Cubase 15.0.5, Bitwig 6.1, REAPER 7.79, Reason 14.1), the research ran there, and it keeps all Live automation away from your working desktop. Tradeoff: the second Windows machine is busy while a round runs, and the CPU comparisons of ADI against Live need both programs on one machine, so those few runs either install ADI's build on the second Windows machine or need Live on the director's Windows workstation with your OK. (b) puts Live automation on your working machine; (c) costs your time on every reading.
- **Needed by.** Now: it unblocks every Live reading from M1 on
- **Rows.** MX-22, MX-33, WTS-11 and every Live reading in §7.1


### 6.2 Placements told with them, to strike if wrong

Each is inside existing authority, so it is recorded as win's placement and told to him with the
first questions; he strikes any he disagrees with (LANES §5, "Strikes").

| # | Placement | Rows | Why it is not a question |
|---|---|---|---|
| P-1 | The Loudness Drive is built now as one automatable gain over every leaf track's input gain, engaging nothing; any stages it engages come with the unified feature | DEV-30, MX-35 (XC-06) | ADR-0203 d14 already rules the mechanism; the ruling removed only the ADI-L stages |
| P-2 | The tuning ruler follows Live's index order (Hz on hover); pitch order only as an addition if he asks | TU-02, TU-03 (XC-28) | Doing differently what Live does needs a ruling, and none exists (ADR-0178 rules storage, not display) |
| P-3 | Follow-action chance is drawn again on every pass, as Live; a seed within one pass keeps a render equal to its realtime pass; no keep switch | TM-SV-04 (XC-29) | ADR-0203 d1 and d7 keep Live's "each pass may differ" as the default |
| P-4 | F7's offline processing (Cubase's Direct Offline Processing) gets a FEATURES row at P2 beside the fade editor; F7 stays unbound until then | XC-36 | ADR-0203 d6 binds F7 and ADR-0069 directs it; only the priority is placed |
| P-5 | The MIDI port host is built on JUCE's UMP API from the start with the Windows MIDI Services backend off (the Win32 backend, named as such in Settings). MIDI 2.0 over Windows MIDI Services is a separate later row, SRF-16 at P2: JUCE 9.0.2 says it works only on the Windows 11 Canary insiders build "at time of writing", needs Windows SDK 10.0.20348, the CppWinRT tool and a NuGet package that is not in the official feed, builds only with MSVC, and needs the installer to carry the service's SDK; your machine runs Windows 10, so it cannot be tested there | SRF-05, TM-LS-03, SRF-16 (XC-18) | ADR-0188 d7 (his ruling) names the UMP device I/O and keeps "MIDI 2.0 stays P2", with the Windows service as ADI's P2 part marked experimental by JUCE; this placement keeps d7's P1/P2 line and defaults no experimental backend. The first draft's "Windows MIDI Services on at M3" advanced a P2 item and was not feasible on his box or CI |
| P-6 | Your 5 November ruling is recorded as one short ADR in DECISIONS.md, in your words (no reserved plug-in slots; no separate built-in ADI-Q or ADI-L; your VST3 and CLAP builds stay ordinary plug-ins; when all your plug-ins are finished and your list arrives, their source becomes one unified built-in feature with a device-like window, from the track and the Mix View; it never hosts the VST3; its design waits for the list), marked as amending ADR-0203 d14's cascade, ADR-0204 d1 and ADR-0209 d11, designing nothing | MX-34 (XC-27) | LANES §2: a decision is a new ADR and the docs win over the code; without it DECISIONS.md keeps saying the slots are decided while every other text says they are withdrawn. The words are yours; only the entry is placed |
| P-7 | The priorities of the new FEATURES rows (§8.2): Global Quantization P1; Live's Consolidate, Paste Bounced Audio and the group bounces P1; Crop P1; Slip P1; object snapping P0 with the Arrangement's gestures; Lock Envelopes P1; crossfades between takes P1; Edit Info Text P1; the CPU Load Meter and Performance Impact P1; Linked-Track Editing P2; tempo leaders P2; MIDI clock and MTC P2; Convert Harmony, Melody and Drums to New MIDI Track P2; project tabs P2; Live's Second Window P2; F7's offline processing P2 | §8.2 | Each is Live's own feature (d1) or decided elsewhere; only its row and priority are placed |
| P-8 | **The seventeen ADRs win would write inside your existing rulings** (R-d; the one list, also §8.4), each numbered when it lands and each to strike here: 1 the gesture contract (one session, one label, undo refused while open); 2 EditScope with Lock Envelopes and ripple's switch; 3 the media reader (one reader behind every decoder); 4 one loudness meter; 5 the measurement harness (record schema, the Live driver); 6 one capture for every render; 7 jobs and staleness (the declared read set); 8 the three render contracts (freeze, bounce, consolidate on the recipe key); 9 the converter rules (every converter on the exact time base); 10 comping is Live's (ADR-0203 d1 over ADR-0037); 11 the warp-analysis stack; 12 tuning sources and the keyboard map; 13 remark threads; 14 Graph threads; 15 the modulation schema (contributor kinds, one evaluator, one playing value); 16 the testing ADR (the headless kit, the golden comparator); 17 one control stack (one dispatcher, one listener). Each is an engineering contract or "Live's way" under ADR-0203 d1; none adds a feature, changes a default or touches an approved UI | R-d; §8.4 | The research handoff says not to add ADRs automatically; this is the disclosure that makes "once you approve the plan" mean these seventeen and no others. Strike any, and its contract stays a needs-table item instead |
| P-9 | The engine seat finishes the Link consumer it has in flight (started before this plan, 5 November) to its gate before it takes the window's oldest open need, once; thereafter ADR-0210 d2's order holds exactly (the oldest open need from the window's table before anything of LANES §3) | XC-30 | d2 is your decision; a half-built consumer abandoned mid-way would cost more than it saves. It is written into LANES as a dated one-time exception, never as a rule |
| P-10 | Compaction is best-effort: it reports and never fails a commit (a failed compaction on main today fails the edit, and with it plug-in drains, recording takes and agent applies); it runs down to a low-water mark and stores checkpoint rows by content hash | HI-02 (with Q-A1) | ADR-0203 d13 rules the cap and what compaction may never pass; it does not say an edit must fail when compaction cannot run, so the failure mode is placed. Strike it if you want the cap enforced strictly, and Q-A1's answer then decides what the strict failure is |

Smaller placements inside rows are marked "win's placement, to strike if wrong" in Appendix A. The
main ones: a movie's first audio track imports while the picture stays P2 (TM-MD-01); the FFmpeg
tool ships in the installer's tools tier (TM-MD-05); where freeze, bounce and consolidate renders
live (TM-FC-03); Main's meter takes its run, pause and reset defaults from ADI-L's (DEV-28); Save As
replaces an existing file after the dialog asks, as Live (PD-01, SPEC §3.7 reworded); Save a Copy
and Collect keep the project's identity for consent (AGT-02); Bitwig-style named pages as an
addition beside Live's banks (SRF-14); the CTZ tiers in a template side table (MX-31); a bucket
without conditional writes refused in the first collaboration release (CO-01).

### 6.3 Later, as their milestone comes

#### Q-F1. Link Audio

- **Question.** Link Audio has been in the open Link SDK since Link-4.0.0b1 (February 2026), and it is usable; Live 12 has it. It was closed (ADR-0204 d5, kept a wish by ADR-0203 d19) on the premise that the SDK was not open. Reopen it?
- **Options.**
  - (a) Reopen it at P2 after Link lands on 4.1.1; keep the LAN audio node as an ADI addition
  - (b) Reopen it and retire the LAN audio node
  - (c) Keep it closed: Link without its audio
- **Recommendation.** (a): reopen it as Live has it, at P2, after Link itself lands on Link-4.1.1, and keep the LAN audio node (ADR-0126) as ADI's addition for links beyond the LAN and for peers that are not Link apps.
- **Why, and the tradeoff.** It is Live's behaviour and now buildable, and the engine is already integrating the same SDK; pinning 4.1.1 now, with Link Audio off until it is built, means no SDK swap later. Tradeoff: one more large engine item, window rows, and inputs named by peer and channel; Link Audio is 16-bit stereo and LAN-only and overlaps the LAN node's job; its parity needs Live 12 or a Push as a peer to measure. Retiring the node (b) saves an item but loses links beyond the LAN and the Opus codec.
- **Needed by.** With M3 (Link ships there; the 4.1.1 pin stands whatever the answer)
- **Rows.** TM-LS-02

#### Q-F2. Link on WASAPI

- **Question.** Live on Windows turns Link off with MME/DirectX drivers and runs it with ASIO. ADI also offers WASAPI, which Live does not. Should Link work when ADI runs on WASAPI?
- **Options.**
  - (a) Link on ASIO and WASAPI, off on DirectSound, each driver gated by AUDIOENGINE-1
  - (b) ASIO only, as Live
- **Recommendation.** (a): Link on ASIO and WASAPI, off on DirectSound as Live's MME/DirectX, each driver offered only after it passes AUDIOENGINE-1 (an onset within 3 ms of LinkHut).
- **Why, and the tradeoff.** A laptop without an ASIO driver still gets Link. Tradeoff: one more driver to measure and keep honest, since WASAPI's shared-mode timing jitter lands on the host-time filter. (b) is Live's way exactly.
- **Needed by.** With M3
- **Rows.** TM-LS-01

#### Q-G. Plain text typed into the palette

- **Question.** When you type plain words into the palette (Ctrl/Cmd+P), are they commands and devices first, or a request to the agent? ADR-0104 d3 routes free-text tasks to the agent at its tier.
- **Options.**
  - (a) Local matches first, run offline; 'Ask the agent: <text>' as the last row
  - (b) As ADR-0104 d3 reads: unprefixed text goes to the agent; commands need a prefix
  - (c) Commands only; the agent stays in the chat tab
- **Recommendation.** (a): local matches first, run offline, with 'Ask the agent: <text>' always as the last row.
- **Why, and the tradeoff.** Typing 'hsb' still runs Hide/Show Browser at once, nothing leaves the machine unless you choose the agent row, and the Dream's 'turn a sentence into a task' stays one keystroke away. Tradeoff: one more keystroke for an agent task, and a list that mixes kinds. (b) makes every typed word a possible model request, with consent prompts, latency and cost, and commands need a prefix; (c) loses the Dream's sentence-to-task from the palette.
- **Needed by.** With M3 (the palette)
- **Rows.** WIN-10, WIN-06, AGT-05

#### Q-H. The warp state of consolidated audio

- **Question.** Your deviation places a frozen or flattened render with Warp off, so it nulls against its source (Live places it warped), and the engine extended that to Bounce to New Track. Live's Consolidate (Ctrl+J) makes a new loop from the clip engine's output and leaves it warped, so it follows tempo changes. Which warp state does ADI's Consolidate use?
- **Options.**
  - (a) Consolidate warped, as Live; every other render Warp off
  - (b) Warp off everywhere, Consolidate included
  - (c) Warp on everywhere except freeze
- **Recommendation.** (a): Consolidate follows Live (the new clip warped at the project tempo); every render of the mix (freeze, Bounce in Place, Bounce to New Track, Paste Bounced Audio, the group bounces) stays Warp off, as d16.
- **Why, and the tradeoff.** Consolidate's main use is making loops, and a loop must follow tempo changes as Live's does. Tradeoff: a consolidated loop does not null against its source, because a warp engine at ratio 1 is not guaranteed transparent. (b) nulls but stops following tempo changes, unlike Live; (c) changes your deviation for every render.
- **Needed by.** Before Consolidate is built (M5)
- **Rows.** TM-FC-02, ED-06

#### Q-I. Recover a recording running at a crash

- **Question.** Should audio being recorded when ADI or the machine died come back into the project on the next launch? Live's own recovery guide leaves this uncertain, so it would be an addition.
- **Options.**
  - (a) Yes: the take placed as Stop would have, one undo step, named in the reopen question
  - (b) No: the takes stay in their folder for a manual import
- **Recommendation.** (a): yes; on the next launch the take is placed as Stop would have placed it, as one undo step, and the reopen question names it.
- **Why, and the tradeoff.** It protects the one thing a session cannot redo, a performance. Tradeoff: a small plan file per recording and a recovery path to keep correct; after a power cut the last unwritten block (at most one) may be missing.
- **Needed by.** With M5 (crash safety)
- **Rows.** PD-06, PD-05

#### Q-J. What fills Live's three Pow-r dither slots

- **Question.** Live's export dither choices are Triangular, Rectangular, Pow-r 1, 2 and 3, and None. POW-r is not usable, so ADI's three slots need open noise-shaped modes, and names. What fills them, and under what names?
- **Options.**
  - (a) Three open noise-shaped dithers from SoX's published tables (for example Lipshitz, improved E-weighted and Shibata), under names you choose
  - (b) The three shapes in your ADI-L (basic, optimized, weighted), once ADI-L is on your list and its source is committed
  - (c) No shaped dither: Triangular, Rectangular and None only
- **Recommendation.** (a) now, under names you choose; (b) later if your plug-in list includes ADI-L's dither.
- **Why, and the tradeoff.** (a) needs nothing from your plug-ins and keeps Live's six choices. Tradeoff: (a)'s spectra are unmeasured against Live's Pow-r; (b) is measured (against Pro-L 2, not against Live) but waits on your list and ties export to plug-in source; (c) leaves three of Live's six choices out.
- **Needed by.** With M5 (export dither, EX-06)
- **Rows.** EX-06

#### Q-K1. The Pd patch editor's route

- **Question.** Your ruling ADR-0145 d8 puts the Pd patch editor inside ADI, built on plugdata; Live's Max for Live opens the Max application as a separate editor. Which route for editing a Pd device's patch?
- **Options.**
  - A: 'Edit in Pd': vanilla Pd's editor attached to the running device, edits heard at once (M, plus Tcl/Tk in the installer)
  - B: 'Edit in plugdata': the plugdata application on a working copy, heard after Save (M)
  - C: plugdata's canvas embedded inside ADI (XL)
- **Recommendation.** A as the P1 editor; B later as an optional 'Open in plugdata'; C only if plugdata's look inside ADI is worth months of work.
- **Why, and the tradeoff.** A follows Live's Max-style workflow, its objects are exactly what ADI runs, and edits are heard at once. Tradeoff: A reopens four of your rulings, not two. It reverses ADR-0145 d8 (an editor inside the DAW, built on plugdata), brings back ADR-0134 d8's external editor, relaxes ADR-0177 d4 (edits are heard before the op is written), and, plainly, re-admits an external Pd GUI process, which ADR-0203 d8 ("the DAW uses embedded libpd only; no Max application path") and ADR-0204 d5 (the rejected items "stay rejected, with their reasons", among them "an external Pd or Max path") both rejected; the audio still runs in ADI's embedded libpd, only the editor window is Pd's own. It looks like vanilla Pd rather than plugdata, and it needs Tcl/Tk packaged and fences so that closing the window or Pd's Quit can neither free the patch nor end ADI. Win still recommends A because it is Live's workflow (d1) and the only route with edits heard at once at M cost. B is silent until Save and cannot share an exclusive ASIO device; C runs on plugdata's own Pd fork, JUCE fork and NanoVG (XL) and is the only route that keeps all four rulings.
- **Needed by.** Before M7's Pd user tier
- **Rows.** DEV-11

#### Q-K2. A user patch's delay when blocks do not divide by 64

- **Question.** ADR-0203 d4 sends this case back to you: what does a user's Pd patch do when the audio does not come in whole 64-sample blocks (notes and automation split blocks, and some drivers' buffers are not multiples of 64)?
- **Options.**
  - 1: no added delay when the buffer is a multiple of 64 (events on Pd's tick); one reported block otherwise
  - 2: always one reported 64-sample block for user patches
  - 3: never any delay; buffer sizes that are not multiples of 64 refused while a user patch is loaded
- **Recommendation.** 1: no added delay whenever the buffer is a multiple of 64, with notes and automation landing on Pd's own 64-sample tick inside the patch; one reported, compensated 64-sample block only for other buffers.
- **Why, and the tradeoff.** It keeps 'no added delay' in the common case and is exactly how Pd itself times control messages. Tradeoff: event timing inside a user patch is quantised to 64 samples (1.5 ms at 44.1 kHz), and it costs M of engine work. 2 costs nothing but adds 1.5 ms to every user patch; 3 restricts your driver settings.
- **Needed by.** Before M7's Pd user tier
- **Rows.** DEV-13

#### Q-K3. Editing a rebuilt Live effect's patch: in place or as a copy

- **Question.** The Dream says each rebuilt Live effect is a patch that can be opened. When you edit one, does the device itself change, or does a copy appear?
- **Options.**
  - 1: editing makes a copy '<Device> (edited)' on the Pd path
  - 2: editing in place: the device switches to the Pd path under its own name
  - 3: read-only: built-ins can be opened but never edited
- **Recommendation.** 1: editing makes a copy, '<Device> (edited)', on the Pd path; the built-in stays native, with no added delay.
- **Why, and the tradeoff.** A copy leaves every existing project's sound and timing unchanged and makes the change visible. Tradeoff: one more device in the chain. Editing in place (2) saves a step but silently changes a familiar device's sound path and timing under the same name; read-only (3) never lets you change one.
- **Needed by.** Before M7's Pd user tier
- **Rows.** DEV-12

#### Q-L. ADI as an MTS-ESP master

- **Question.** ADI's tunings already reach VST3 and CLAP instruments that take per-note pitch (note expression or MPE). Many microtonal synths, Surge XT among them, instead follow an MTS-ESP master that sends one tuning to every client; Live offers this only through its Microtuner device. Should ADI itself act as an MTS-ESP master, an addition Live lacks?
- **Options.**
  - A: yes, at P2, off by default, with CLAP's tuning extension
  - B: yes, at P1, together with Scale Mode and tunings
  - C: no: tunings reach plug-ins only as per-note pitch
- **Recommendation.** A: yes, at P2: a tuning route per plug-in beside note expression, MPE and plain MIDI, off by default, together with CLAP's own tuning extension for CLAP instruments.
- **Why, and the tradeoff.** Maqamat and other tunings then play in MTS-ESP synths without per-note setup, while Live's Scale Mode and tunings stay first at P1. Tradeoff: MTS-ESP carries one table for the whole computer, so a client synth cannot follow a track's Bypass Tuning or two tunings at once, and only one master can run (another host or a Microtuner instance conflicts). C keeps ADI simpler but leaves synths without MPE or note expression in 12-TET; B spends engine time ahead of Scale Mode parity.
- **Needed by.** With P2
- **Rows.** TU-04

#### Q-M. Push 2 and Push 3

- **Question.** Your Dream asks for 'Push and the common surfaces working on day one'; your earlier ruling R-14 says 'Push support is Ableton's and not assumed'. Should ADI drive Push 2 and Push 3?
- **Options.**
  - (a) Yes, at P2, after the surface host and Session View
  - (b) No: R-14 stands; Push works only in User Mode as a generic MIDI surface
  - (c) Yes, from day one
- **Recommendation.** (a): yes, at P2, as an ADI surface driver after the MCU and profile host and Session View, built from open code and checked against Live 12.4.
- **Why, and the tradeoff.** Live drives Push natively, so the standing rule points to supporting it, and many Live users own one. Day one is not possible: the surface host, the feed's surface half and Session View come first. Tradeoff: XL work and a USB library; Push 2's display has no official protocol ADI can use (Ableton's document is not usable and the open implementations are reverse-engineered, so a firmware update can break them). In User Mode Push works as a generic MIDI surface either way.
- **Needed by.** With P2 (surfaces)
- **Rows.** SRF-15

#### Q-N1. Undo in a shared project

- **Question.** In a shared project, may your undo reverse what a collaborator changed after you? ADR-0188 d5 says undoing a synced batch, one's own or a collaborator's, appends a compensating batch.
- **Options.**
  - (a) No: your undo reverts only your change where nobody wrote after it; partial steps come back with a notice
  - (b) As ADR-0188 d5 reads: undo reverses the whole batch, a collaborator's later writes included
- **Recommendation.** (a): your undo reverts only your change where nobody wrote after it; a step can come back partly, with a notice.
- **Why, and the tradeoff.** It never destroys a colleague's later work, for example a track someone recorded onto survives your undo of its creation. Tradeoff: some undos come back partly. (b) is simple to predict, but can silently take back what someone else did after you.
- **Needed by.** When collaboration starts (P2)
- **Rows.** CO-02

#### Q-N2. How presence travels in a shared project

- **Question.** Your directive asks for 'Excel-style awareness: live visual cues, with a toggle', in a design where ADI runs no service. How should presence travel?
- **Options.**
  - (a) Only through the bucket: who is in the project and where, refreshed every few seconds; no live cursors
  - (b) (a), plus an optional LAN session hosted by one ADI, off by default: live cursors, follow playback and spotlight; it ends when the host leaves
  - (c) A peer link over the internet, which needs a relay
- **Recommendation.** (a) for the first collaboration release, (b) as an addition once it works.
- **Why, and the tradeoff.** (a) needs no network listener and no service. Tradeoff: (a) has no live cursors; (b) gives real-time cues on a studio network, at the cost of a LAN listener (on the control API's one server stack, never a second one) and a second transport to maintain; (c) needs a relay, which contradicts 'ADI runs no service'.
- **Needed by.** When collaboration starts (P2)
- **Rows.** CO-04

#### Q-O. Where the control API's token lives

- **Question.** ADR-0039 d4 rules a bearer token generated per session and shown in the UI, never written into the .adi, and explicitly rejects any tokenless "local is fine" mode (AI-AGENT §7.1 repeats it). Local tools (adi-mcp, scripts, the test kit) need to find ADI's endpoint and a token without you pasting it each time. The first draft placed a discovery file that any process of your user can read; that makes a token you hand over into an ambient one, which is the rejected mode in effect, so it is your call. Where does the session token live?
- **Options.**
  - (a) An owner-only, non-roaming discovery file (deleted on exit, swept when stale) carrying the endpoint and a session token that grants at most Propose; paired surfaces keep their own tokens in the OS keychain
  - (b) As d4 reads: the token is shown in the UI only; adi-mcp and scripts take it from you (pasted, or an environment variable you set); surfaces pair with a single-use code and keep theirs in the OS keychain
  - (c) As (a), but the file carries only the endpoint and a pairing hint; every client, adi-mcp included, pairs once with the single-use 120 s code and keeps its own token in the OS keychain
- **Recommendation.** (c): the file tells tools where ADI is, never what the secret is; every client pairs once, shows up by name on the Clients page, and can be revoked on its own.
- **Why, and the tradeoff.** (c) keeps d4 whole (the session token is shown in the UI and nowhere else) and still lets a script find ADI without you typing an address, at the cost of one pairing step per tool, the first time. (a) is what MAGDA, gridcraft and Jupyter do and is the most convenient, but any process of your user then holds a Propose-tier token without you handing it over (Propose cannot commit without your click, so the exposure is a changeset, not a mutation, but it is still the ambient mode d4 rejected). (b) is d4 exactly and costs a paste per tool per session.
- **Needed by.** With Q-C (Track C's first PR writes the discovery mechanism)
- **Rows.** CTL-05

#### Q-P. Reading your own Live Sets into ADI

- **Question.** Your base is Live, and your existing projects are Live Sets. Live 12 has no DAWproject export (the Live 12 notes record none), so after M5 the only way a Set reaches ADI is by hand or through stems. ADI could read .als files directly (gzip-compressed XML: tracks, clips, warp markers, automation, mixer state, device parameters by name) with DAWproject's loss-report form. The first draft struck this silently as an addition nobody asked for; it is a product choice, so it is yours. Should ADI import Live Sets?
- **Options.**
  - (a) Yes, read-only, at P2 after DAWproject: tracks, clips, warp markers, automation, mixer state and device parameters by name, with a loss report; plug-in state only where the same VST3 or CLAP is installed
  - (b) No: DAWproject, stems and MIDI files are the paths in
  - (c) Yes, at P1, ahead of DAWproject
- **Recommendation.** (a): read-only at P2, after DAWproject, with your own Sets as the fixture corpus.
- **Why, and the tradeoff.** It is the only road for your existing work into ADI, and it is cheap to read (one reader, no writer). Tradeoff: the format is unversioned and undocumented and changes per Live release, so each Live version needs its own readings and fixtures, and nothing can be promised for Sets from a newer Live; Live's built-in devices map by parameter name only, so a Set with Live devices opens with holes the loss report names. (c) would take engine time from Live parity you can test sooner; (b) leaves your Sets outside.
- **Needed by.** With P2's interchange step; if yes, a TM-IC row is added beside TM-IC-04 and a FEATURES row at P2
- **Rows.** none yet (a new TM-IC row if yes)


### 6.4 His gates and inputs (not choices)

| Gate or input | Unlocks | When |
|---|---|---|
| G-1, the Live 12 measurement seat | every Live reading (§7.1); parity rows from "built, not verified" to verified | now |
| His list of plug-ins to rework | the unified built-in feature's design; what the Quick Kick button engages (MX-20); CASCADE's stage columns (MX-31); ADI-L's meter and dither as sources (LD2, Q-J option b) | when he declares all his plug-ins finished |
| His approval of the Clip View picture | the note editor (MI-03; worklist G1 to G3) and the Sample Editor (TM-WT-06) | before M2's note editor, M6's Sample Editor |
| His multimapper device file | DEV-06's macro sum matched exactly | before the racks and macros work (P2) |
| His signed side-by-side sessions (ADR-0108) | each parity area from "built, not verified" to verified | per area, once the seat exists |
| DP-7: a DAWproject round trip with his Cubase 15 | TM-IC-03's acceptance | M5 |
| AAF-3: an ADI-written AAF opened in Pro Tools or Resolve | TM-IC-04's acceptance | P2 |
| His interface for the loopback round trips | MX-09's Measure round trip, MX-17's loopback latency | M4 |
| His encoder; his Stream Deck + XL; his Push 2 if Q-M is yes | SRF-06's acceleration (M3); SRF-13's pass (P2); SRF-15 (P2) | as listed |
| His grant of the libFLAC, LAME and soxr rows | FLAC and MP3 export, Live's rate rule (EX-04) | M5 |
| His OK for GUI work on the director's Windows workstation: a Windows Sandbox for driven ADI.exe runs (optional: the Linux box under Xvfb covers the need), and readings on its Cubase 15.0.30 and Bitwig 6.1 (otherwise on the second Windows machine) | DEV-26's window half (M1); the Cubase and Bitwig readings (§7.2, §7.4) | as listed |
| His Mac for the Mac-only readings | Cmd+? menu search, single-letter keys under a Hebrew layout, Live 11 rows | any time |
| His ears: the warp listening gate; his picks in the Mix View experiments | TM-WT-03; the Mix View rounds | M6; M4 onward |

### 6.5 Questions that may arise after a reading

Not asked now, because the answer may not be needed:
- **N8, Live's clip-edge declick.** If Live declicks Arrangement clip edges whatever Create Fades on
  Clip Edges says, matching it would touch his deviation, so it goes to him by name (TM-SV-03).
- **MX-32, track names and colours in plug-ins.** If Live 12 does not pass them to hosted plug-ins,
  doing it is an addition and needs his yes.
- **Any reading that contradicts one of his deviations** goes to him by name (BUILD-BRIEF rule 4);
  a reading that only changes a default follows Live, as an amendment, without asking.

---

## 7. Evidence still needed

Nothing in this plan has been measured in a running DAW. The research bundle's missions (more than
a thousand cases across milestones 2 to 11, 165 of them new in milestone 11) and its six original
diagnostic specifications are all **NOT RUN**, and the plan never treats a manual sentence as a
measurement. This section lists only the readings and tests a row is waiting for, grouped by where
they can be taken and ordered by the milestone that needs them. The rest of the research's
missions stay a reference queue: a mission is scheduled only when a row names it.

**How readings enter ADI (OPEN-QUESTIONS' relay rule, MX-33's harness).** Each reading is a record
with its build, hash, operator and raw evidence; win relays it into the matching `docs/daws` row,
every matching window "Measure on Live" line and `docs/AWAITING.md` (adding the AWAITING row first
if none exists), dated. No row is struck from a manual or a controller palette. A Live reading that
differs from ADI opens a lane item, or an ADR where an ADR predicted. A Mac shortcut needs a Mac
reading.

### 7.1 Live 12.4, on the measurement seat (G-1)

| Needed by | Readings (all NOT RUN) | Rows waiting |
|---|---|---|
| M1 | Meter ballistics and when the MIDI meter lights (a new AWAITING row; M5-A5C-001); the cue tap with the Track Activator off, Cue Volume's range and default, the metronome and preview on Cue Out (a new AWAITING row; M-CUBASE-CUE only as comparison); AWAITING 8 (Track Delay's range and selector) and 7(h); sends and returns AWAITING 7(a) to (f) and (i), L02, L03, L10; solo AWAITING 5, 7(c), (d), (g), M5-A5C-011, M6-A6H-008; Split Stereo's gains (A5C-023, M-AUDIO); AWAITING 12 / L-AUTO-7 (an override after save); M5-A5H-034/035 (Alt+Rescan, the relaunch dialog, what counts as reinstalled); AWAITING 11 (a plug-in failure, the Configure warning); where Live reopens its windows, and with a monitor gone; whether plug-in windows sit above other applications, appear on the taskbar, close on Esc | MX-01, MX-02, MX-04, MX-05, MX-07, MX-08, AU-04, DEV-25, DEV-26, WIN-02, WIN-03 |
| M2 | The clip overlap cases (a clip moved over the middle, start, end and whole of another; warped and unwarped; audio and MIDI); Ctrl+E with nothing selected; M6-A6C-083 (a selection split); M6-A6C-076 (slip and left trim), -077 (snapping), M6-A6M-033 (Crop's crossing notes); a loop inside a deleted span, markers on a boundary (A4-012), a fragmentary bar (A5C-035); A6C-079 and -080 (fade limits); AWAITING 14 (Create Fades in a fresh install); AWAITING 12 (how an Alt-drag curve is stored), M6-A6C-058 to -062; the four note-overlap cases and which id survives (A-M03); Lock Envelopes on moves and range deletes; M-LIVE-UNDO and C6-HISTORY (the undo label and grouping per gesture, through the interaction logger); the parity checklists of manual 5.4.2, 6.7 to 6.12 and 25.1 to 25.5 | ED-01 to ED-05, ED-09, ED-10, AU-01, AU-05, MI-02, MI-03, HI-01, HI-03, MX-23 |
| M3 | N1 and N4 (Global Quantization's grid rules) and the chooser's codes (A6P-011); Link: M3-CONTROLLER 6, M5-A5C-026 to -029, M6-A6G-002, M6-A6I-006, whether a peer's tempo enters undo and the saved Set, Link with quantization None, Nudge under Link; L-MEDIA-1 to -4 and -6 (first sound while decoding, an AAC without gapless data, formats dropped on Windows, a manual repair, ACID tempo); M3-MEDIA F4; M2-A-WARP 8 (sample defaults); AWAITING 13 (which file is open after Save As and Save a Copy); AWAITING 14 and SF-22c (the takeover default, Value Scaling), SF-22d (Live's encoder type names), SF-22b (is a mapping specific to its port when Remote inputs merge); A5W-015 (the key dispatch order); how Live carries its Preferences across versions | TM-SV-01, TM-LS-01, TM-MD-01 to -04, PD-01, PD-11, SRF-03 to SRF-05, WIN-05 |
| M4 | AWAITING 5 (the pan law at C, 25R and 50R), M-AUDIO-PRECISION and M6-A6G-006, -007, -010; AWAITING 7(g) and 7(h) (ducking with the key muted; the default tap); AWAITING 15 (group mute); AWAITING 16; M3-ROUTE R1 (heard, recorded, placed) and the monitor default per track type (a new row); M6-A6H-007, -008, -011 (a tap against send-to) and L05; A3-009 (mono and stereo conversions); L01 to L10 (WTS-11); a fixture VST3 recording the channel context Live passes (MX-32) | DEV-05, MX-03, MX-09, MX-22, MX-24 to MX-26, MX-32, WTS-11 |
| M5 | Export: Normalize's target (sample or true peak), Convert to Mono's law, where Render as Loop's first pass starts, Include Return and Main Effects with parent groups and sidechains, which tracks All Individual Tracks renders and their names, batch collisions, the FLAC depths, the MP3 settings, the Pow-r spectra, the SRC quality (M6-A6G-010), M6-A6D-012, AWAITING 16's dialog defaults; bounces: the A4-006 null and which mixer settings Live copies, Bounce Track in Place on a MIDI track; consolidate: M5-A5C-022, the normalization target and clip gain, the warped clip's warp state, A4-008 and -009; frozen tracks: M5-A5C-013 to -018, M6-A6G-005; the crash dialog and the Crash folder (M5-A5C-048, -049); the Backup folder (M5-A5H-014); an older-major Set (M6-A6H-069); the Collect All and Save dialog; Save Live Set As Template | EX-02 to EX-07, EX-11, TM-FC-01, TM-FC-02, TM-FC-04, PD-05 to PD-07, PD-09, PD-14 |
| M6 | L-AUTO-1 to -6, -8 and -9 (spacing and punch writes, a controller's latch, undo per pass, loop overlap, punch, clip modulation laws, the automation outside a shape, touch or latch for switches); comping M6-A6H-016, -027, -029, -030, manual 21.7 and AT-14, and the Arrangement Overdub cases (CT-03: Overdub on into an existing clip merges, off replaces; loop on and off; one undo step or two); the scale census C6-SCALES (M6-A6M-031, -032); tunings M6-A6F-001 to -014, M6-A6M-084 to -092 and AWAITING 15; the MIDI effects LM1 to LM8 and the A7M checklists with A7M-130; A7M-127 (Sampler's selector latch); LM5 (what Capture takes from a tapped track); M2-A-WARP 1 to 6 and M6-A6C-034 to -040; the short/long threshold and Auto on drop; L-INFO (Edit Info Text); Live 12 renders of the built effects with AWAITING 9 and 10 and the M7 device cases | AU-02, AU-03, AU-06 to AU-08, CT-01 to CT-04, MI-04, MI-06, TU-02, TU-03, DEV-19 to DEV-22, TM-WT-01 to -03, TM-WT-05, RE-03 |
| M7 | M-CPU and M5-A5C-019 at equal workloads (ADI and Live on one machine); confirm Live 12 has no Multicore switch; LM-PD1 (does Save in Max interrupt audio on every instance) | DEV-23, DEV-24, MX-28, DEV-11 |
| P2 | M3-LAUNCH L1 to L4 and L6, N2 to N12, the launch half of M-LIVE-CLIP, M5-A5C-036 to -041, A6S-007 (Session View); M2-A-WARP 7 and 9; A6I-001 to -005 with Live or a Push as a peer (Link Audio, if reopened); AWAITING 13's MIDI-file and MTC parts; A6U-004 to -012 (Push, if Q-M); M5-A5W-001 to -004 (the Second Window); A5W-012 and -013 (map modes); SF-22a (Mackie behaviour) and whether Lock to... is saved; M8-A8D-001 to -025, A8D-004, A8D-018, A8D-019, M6-A6H-042 to -051 (racks); A6M-022, -024, -030, -078, A6X-013, M2-A-MIDI (Humanize, quantize keys, probability groups, MIDI Tools) | the P2 rows |

**A fresh Live 12 profile** is needed for AWAITING 14 and 16 (factory defaults). **His Mac** (Live
11.2.7) answers only Mac-specific rows (Cmd+? menu search; keys under a Hebrew layout).

### 7.2 Cubase 15, the research's first priority list

The handoff names six Cubase uncertainties to resolve before locking contracts. On this plan's
rows they map as below. Cubase 15 is installed in two builds: 15.0.5.114 on the second Windows machine (the
research's inventory) and 15.0.30.287 on the director's Windows workstation, which is the version of the manual the research
cites. Every reading names the build it ran on, uses a disposable project and marks unexecuted
cases NOT RUN; GUI work on the director's Windows workstation needs his OK (§6.4).

| Priority | Missions | What it decides here | Rows | When |
|---|---|---|---|---|
| 1. Direct Offline Processing: shared-source fan-out, new-version policy, reordering, exact undo, handles and tails, failures, reopen | M2-C-DOP | The F7 offline-processing contract (P2) and the recipe key's inputs | XC-36's FEATURES row; TM-FC-03 | before F7's ADR (P2); TM-FC-03 in M5 |
| 2. Fades and ranges: curve laws, boundary rounding, overlaps and crossfades, compound undo | M2-C-FADE, M2-C-RANGE | Fade curve laws on DC, correlated, inverted and independent sources; the range truth table; comp-seam crossfades | ED-10, ED-01 to ED-03, CT-05 | M2, M6 |
| 3. VariAudio: segment deletion, shared clips, reanalysis and reset, flatten scope | the M5 VariAudio cases | Nothing yet: VariAudio-class editing is P3 and has no row | none | after P2 |
| 4. Export: source, return and master inclusion, normalization order, tails, naming and collisions, cancellation, multi-file publication | M4-C-EXPORT, the C4 rows | Comparisons for the Export page's additions (Dry, Inside and Outside; collisions; cancel) | EX-02 to EX-07, EX-11 | M5 |
| 5. Workspaces and settings: display loss, DPI and focus restoration, shortcut conflicts, configuration scope | the C5W rows, M3-UI-WORKSPACE | Window placement rules and the shortcut editor's conflict handling (comparisons; Live's rules lead) | WIN-02, WIN-05, PD-11 | M1, M3 |
| 6. Control Room: cue tap points, monitoring independence, compensation | M-CUBASE-CUE | The cue tap reading's comparison; Control Room constraints for version 2 | MX-08, MX-18 | M1; version 2 |

Also on Cubase: **DP-7**, a DAWproject round trip with a VST3 insert, automation and notes (TM-IC-03,
his gate, M5); the C3 automation-mode and hitpoint cases for AU-09 and TM-WT-08 (P2).

### 7.3 REAPER 7.79 and Reason 14.1

Only the cases a row depends on; the research's 188 milestone-10 and 165 milestone-11 missions
otherwise stay a reference queue. Installed builds: REAPER 7.79, Reason 14.1.0.8292.

| Case | Decides | Rows | When |
|---|---|---|---|
| R11X-002: render WITH master effects against VIA master | ViaMain and MainFxOnly as two modes, never one checkbox | EX-02, DEV-29 | M5 |
| REAPER's render-normalize: -14 LUFS-I at -1 dBTP, brickwall on and off, three stems "together" | The comparison for "together" and the brickwall | DEV-29, EX-05 | M5 |
| R11X-018, -038, -040: names, collisions, partial outputs | Collision and manifest rules | EX-07 | M5 |
| R11E-013 to -019: item groups, ripple scopes and locks | Comparisons only (Live's rules lead) | ED-03, ED-07 | M2, M6 |
| hosts10 generated-midi-commit | No double processing when a tapped track records generated notes | DEV-20 | M6 |
| hosts10 portable-rack-boundary; hosts11 rack-relationship-rewrite and macro-composition-domain (the N11H Reason cases) | Rack ops' undo and macro composition domains | DEV-01, DEV-03, DEV-06 | P2 |
| hosts11 pd-segment-contract | The user-patch delay rule (Q-K2) | DEV-10, DEV-13 | M7 |

The hosts10 and hosts11 cases are the bundle's original diagnostic specifications: they run against
ADI as much as against the references.

### 7.4 Bitwig 6.1, on a machine and session he assigns

Bitwig Studio 6.1.0.22652 is installed on both the second Windows machine and the director's Windows workstation.


| Case | Decides | Rows | When |
|---|---|---|---|
| M2-B-MOD, B-M09: nested scopes, several sources, bypass, deletion, copy, serialization and restore | What a modulation route stores and how it composes | DEV-07 | M7 |
| M3-CONTROLLER item 4: remote-page persistence | Where ADI's named pages live | SRF-14 | P2 |
| M2-B-GRID and the grid9 specifications (generated-event ownership, voice reclamation, same-frame identity, event boundaries) | The Pd tier's note bridge and voice rules, as ADI probes | DEV-14, DEV-15, DEV-18 | M6, M7 |

### 7.5 ADI measurements on the director's Windows workstation and his hardware

| Measurement | Where | Rows | When |
|---|---|---|---|
| DEV-26's headless check of his VST3s (scan, load, edits as single steps, state round trip, ADI-L's lookahead null) | the director's Windows workstation, under `ADI_HOME`, off his desktop | DEV-26 | M1 |
| The Main meter's cost, always on (A3-24's condition) | his PC | MX-10, DEV-28 | M1, M4 |
| AUDIOENGINE-1: an onset within 3 ms of LinkHut, recorded through loopback; then per driver for Q-F2 | the director's Windows workstation | TM-LS-01 | M3 |
| The loopback round trip by MTDM, and the loopback input's latency | his interface | MX-09, MX-17 | M4 |
| The Mix View program's budgets (§4.2): +0.2 points of one core with 16 visible strips; a worker pass under 4 ms | his PC | MX-12, MX-30, MX-36 | M4 |
| A BENCHMARKS row: commit p50 and p99 with NORMAL and FULL and a near-full WAL | the director's Windows workstation | PD-12 | M5 |
| The labelled warp corpus for the analyser benchmark (beat_this as comparator) | local | TM-WT-01 (Q-D) | M6 |
| GT-7: ADR-0102 d3's suite under ASIO, serial against 5 and 11 workers, then against Live 12 at equal workloads | the director's Windows workstation (and Live on the same machine, per G-1) | DEV-23 | M7 |
| The Pd tier's lock contention on Windows workers | the director's Windows workstation | DEV-23 | M7 |
| The changeset preview's latency at 6,000 ops on a 10,000-clip project | any | AGT-01 | Track C |

### 7.6 His eyes and ears

The Clip View picture (MI-03, TM-WT-06); the warp listening gate per mode (TM-WT-03); the Mix View
experiments X1 to X3, X15, X17 and the program's later rounds; his first look at the remark pips
(RE-01) and at Main's meter (MX-10); his signed side-by-side sessions per parity area (ADR-0108);
DP-7 and AAF-3; the Stream Deck + XL pass (SRF-13); the Focus Dial on his encoder (SRF-06).

### 7.7 ADI's own acceptance tests

These are not research: they are the tests each milestone's gate runs (§2), written by the lanes,
counted only on a run whose headSha matches. Their vocabularies: the edit, note-diff and history
fuzzers and `test_gesture` (M2); A-M1 to A-M5 (media); the craft-atomic list and the crash fixtures
(files); Tech 3341 and 3342 cases and the libebur128 oracle (loudness); AR1 to AR18 (Pd); GT-1 to
GT-8 (threads); DP-1 to DP-6 (DAWproject); SF-1 to SF-21 (surfaces); AC-1 to AC-24 (control API);
AT-1 to AT-24 (window testing); the per-row `test_*` suites in Appendix A.

---

## 8. What changes in LANES, FEATURES and BUILD-BRIEF

Proposed text only; nothing here is applied. Line numbers are those of `main` at `5e8dfe46`. Win
lands these in M0's records (R-a, R-b and R-e), each placement "to strike if wrong", after the
director approves the plan.

### 8.1 LANES.md

**§1, the lanes table.**
- *engine, "Owns":* add `src/adi/sync/**` (MIDI clock and timecode decoding, the clock-out
  scheduler), the engine's MIDI queues and the Track tap, `src/adi/surfaces/**` (codecs, the
  resolver, profiles), `src/adi/measure/**` (with win), `src/juce/net_*` (the one network stack),
  `src/adi/collab/**` (P2), and `src/adi/control/**` with `tools/adi-mcp` unless the director gives
  the control core a seat of its own (Q-C), in which case that seat owns them; and
  `src/juce/pd_patch_host.*` with the patch device's dispatch.
- *window, "Owns":* add `src/juce/app_midi_ports.*` (the one MIDI port host, on JUCE's UMP API;
  the Windows MIDI Services backend stays off until SRF-16), `src/juce/app_control.*`,
  `src/juce/app_surfaces.*`, `src/juce/ui_control.*` (the one component model for tests and the
  API).
- *devices, "Owns":* replace "`src/juce/pd_*` for the zero-delay adapter only" with "`src/juce/pd_*`:
  the Pd runtime (`pd_engine.*`, `pd_device.*`, `pd_declarations.*`, `pd_loader_fix.cmake`) and the
  zero-delay adapter; the engine owns `pd_patch_host.*` and dispatch". Append: "**On loan while
  the devices seat is silent (XC-46, from 5 November):** `src/adi/dsp/loudness` and
  `src/adi/dsp/limiter` are the engine seat's, and the Pd safety overlays (DEV-09) go to the seat
  that is running; the loan ends the day the devices seat logs again, and win strikes this line
  then."

- *clones, "Owns":* replace "the director's clones as suite members (ADI-Q, ADI-L, ADI-C, ADI Still,
  ADI Prism; ...) built from measurements, in `plugins/`" with "the director's plug-ins, built from
  measurements as ordinary VST3 and CLAP plug-ins under his names. Nothing in ADI waits for them and
  no slot is reserved; when he declares them all finished and sends his list, their source becomes
  one unified built-in feature, designed then (the ruling of 5 November)".
- *New rows:* **measurement seat** (the seat he names, G-1): Live 12 readings through
  `tools/measure/**` and the interaction logger; never ADI.exe on his desktop. **sidecar seat**
  (P2): `adi_daw/agent/**`. **Stream Deck seat** (P2): `adi_daw/tools/streamdeck/**`.

**§3, item 1.** Replace "N16 (now the three slots of ADR-0209 d1)" with "N16 (withdrawn by the
director on 5 November)". Append: "**One dated exception to ADR-0210 d2 (P-9, 5 November):** the
Link consumer, in flight when the plan was approved, finishes to its gate first; from then on d2's
order holds exactly: the oldest open need from the window's table before anything of this
section, so N29, N17's open part and the edit kernel (the plan's M2), then tempo ramps on Link's
effective-tempo path, then the rest in the order of `docs/PLAN.md` §2."

**§3, item 2.** Replace "the ★ Kick sidechain helper (wake the slot, wire the compensated edge
from the chosen tap, Pre FX by default), the CTZ group metadata the navigator reads, multi-track
tap subscription batching, the RMSC slot mode on ADR-0166's DSP" with "the Quick Kick's routing
half (a compensated sidechain from the chosen tap, Pre FX by default; what the button engages
waits for the director's list), multi-track tap subscription batching".

**§3, item 6.** After "the Loudness Drive" insert "(the gain over the leaf tracks' input gain only,
ADR-0203 d14, engaging nothing until the unified feature)".

**§3b, item 2.** Replace "the STACK tab (the "one window" of ADR-0204 d1)" with "the STACK tab (as
the director answers Q-B; recommended, a live list of every track's own devices)".

**§3b, new items.** "The MIDI port host on JUCE's UMP API with the Windows MIDI Services backend off
(SRF-05; the service is SRF-16 at P2), the Master Focus Dial (SRF-06) and the command palette
(WIN-06)." "The Export page (EX-11), Live's dialog first, on stubs until the engine's capture
lands."

**§4, the clones lane's queue.** Replace items 1 to 4 with:
1. "**His plug-ins** (ADI-Q, ADI-L, ADI-C, ADI Still, ADI Prism, ADI Spacer, ADI Fuser): finished in
   his tree as ordinary VST3 and CLAP plug-ins, measured against their references. ADI loads them
   like any plug-in (DEV-26). Nothing in ADI waits for them. When he declares them all finished and
   sends his list, their source becomes one unified built-in feature with a device-like window; its
   design starts then (the ruling of 5 November)."

and renumber the rest. Item 4's DJ Filter moves to the devices lane (§4b item 5).

**§4b.** Item 2: replace "the MIDI effects and Sampler still to come" with "#207 and #208 closed as
"to be ported": their kernels are ported onto the engine's note identity and transform phase
(DEV-17, DEV-18) once those land, and Sampler onto OneShot (#278)". Item 5: replace "on ADI-Q's
cores once handed over, on Auto Filter's until then" with "on Auto Filter's cores". New items: "the
Pd runtime's safety (DEV-09) at once; the user-patch tier (DEV-10 to DEV-16) after Q-K"; "the one
loudness meter, `src/adi/dsp/loudness` (DEV-28)"; "the limiter's true-peak path, one change shared
with export (EX4, LD5)".

**§5, the gates.** Replace the row "ADI-Q / ADI-L hand-over" with "| His list of plug-ins to rework |
when he declares all his plug-ins finished | sends the list; the unified feature's design starts
then |". In the row "ADR-0209, the Mix View", strike "and the slot rows (Q8)". Add: "| The Live 12
measurement seat | now | names the machine and session (G-1) |" and "| The plan's choices | Q-A1 to
Q-E now, the rest as their milestone comes | answers in `collab/director.md` |".

**§6.** Append: "Windows-only work from a seat without MSVC is proven on the Windows CI legs by SHA
and on the director's Windows workstation by win (XC-45). The Windows MIDI Services backend (SRF-16) needs a Windows 11 seat
with the service, a Windows SDK pin of 10.0.20348 or later, the service's NuGet package in a local
repository on the Windows legs and an installer prerequisite; nothing of it is claimed on Windows
10."

### 8.2 FEATURES.md

**Rows to change.**

| Line | Row | Proposed change |
|---|---|---|
| 38 and 50 | "Tempo detection from audio" (twice: P0 and P2) | Keep line 38. Replace line 50 with the "Tempo leaders" row in the block below. |
| 47 | Ableton Link | Only after Q-F1: replace "Link Audio is closed (ADR-0204 d5): Ableton's SDK is not open, so it cannot be built; the LAN audio node (P2) is ADI's answer." with "Link Audio is in the open SDK since Link-4.0.0b1; reopened at P2 by the director (Q-F1), beside the LAN audio node." Until he answers, unchanged except the pin: "pinned at Link-4.1.1". |
| 57 | Volume, pan, mute and solo | Replace "Width, input gain, phase, delay offset and VCA are stored, not yet applied." with "Input gain and Track Delay play; width and the phase switches stay in Utility (ADR-0203 d9), so the strip's stored `width` and `phase_invert` are not applied; the VCA group is the Loudness Drive's mechanism." |
| 64 | Sidechain and track-to-track tap points | Replace "Post Mixer plays today; the two early taps come with sidechain routing" with "No key reaches a device on main yet; the taps share one vocabulary and one format home (win's placement, M0); keys into devices at P1 (DEV-05, MX-24)". |
| 68 | Loudness Drive | Append to the notes: "Built as one automatable gain over the leaf tracks' input gain, engaging nothing: the cascade is whatever clippers and limiters the user places, his own plug-ins included, until the unified feature (the ruling of 5 November; win's placement, to strike if wrong)." |
| 75 | Freeze All Channels and Freeze Group | Replace "Designed (ADR-0059), not built" with "Track freeze and Freeze Group built; Freeze All Channels waits on the tap placement (TM-FC-04)". |
| 78 | Channel strip (struck) | Replace "**The EQ is the exception since ADR-0204 d1: built into every channel, present but off** (the row below)." with "(ADR-0204 d1's exception, the EQ built into every channel, was withdrawn by the director on 5 November.)" |
| 79 | ADI-Q and ADI-L built into every channel | Strike the feature (`~~...~~`), P becomes **withdrawn**, notes: "Withdrawn by the director on 5 November. His plug-ins load as ordinary VST3 and CLAP plug-ins (DEV-26). When all are finished and he sends his list, their source becomes one unified built-in feature with a device-like window, reached from the track and from the Mix View; it never hosts the VST3. Its design waits for the list." |
| 80 | A third present-but-off slot | Strike the feature, P **withdrawn**, notes: "Withdrawn on 5 November with the slots (N16). The sidechain itself is Live's (DEV-05, MX-24); what a Kick button engages waits for his list." |
| 81 | Anchor roles and the CTZ template | Replace "`tracks.role` is win's placement (Q8)" with "roles live in a side table, `track_roles`, schema minor 1.20 on its own (ADR-0209 Q8's column corrected: a minor cannot add a column, ADR-0144 d3)". |
| 82 | Quick Kick Sidechain | Replace "wakes the target's slot and wires the compensated edge from ★ Kick, Pre FX by default; drag track A onto B for any other key; Ctrl+Alt drags an inverse-linked ADI-Q band" with "wires the compensated edge from ★ Kick, Pre FX by default; drag track A onto B for any other key; what the button engages or inserts waits for his list". |
| 84 | The Mix View (F4) | Replace "every channel's slots in one window" with "every channel in one window; until the unified feature, STACK lists each track's own devices (Q-B)". |
| 88 | Native analog group summing | Replace "**The master is summed too** (amends ADR-0173 d4; not built)" with "**The master is summed too** (amends ADR-0173 d4; built)". |
| 100 | Take lanes and comping | Append: "Live 12's model: the main lane plays and take lanes are auditioned; a comp is copies on the main lane (ADR-0203 d1; SPEC §6.2 reworded; ADR-0037's Cubase wording superseded)." |
| 104 | Ripple edit / insert-delete time | Append: "a visible switch: Off (Live's leave-gap, the default), Per track, All tracks (ED-07)". |
| 127 | MIDI-CI property exchange | Replace "It waits on the OS MIDI stacks exposing MIDI-CI" with "JUCE runs MIDI-CI over MIDI 1.0 SysEx on plain ports, so it waits only on its priority (SRF-11)". |
| 155 | Session View | Replace "return to Layer 1 in a schema PR now" with "return as whole new tables in a schema minor when Session View starts, with an essential row so older readers warn (TM-SV-02, amending ADR-0117 §2's wording)". |
| 224 | DJ Filter device | Replace "shares the channel EQ's filter cores and is a pure bypass at centre (ADR-0204 d1)" with "on Auto Filter's filter cores, a pure bypass at centre (the channel EQ's cores were withdrawn with the slots on 5 November)". |
| 274 | OSCQuery | Replace "served to TouchOSC and Lemur" with "served to OSCQuery clients (checked with ossia score and Vidvox's browser; TouchOSC and Lemur are not confirmed OSCQuery clients)". |
| 288 to 290 | The agent rows | Append to line 288: "The Propose engine half is built (#100); the review panel and the control API are open (AGT-01, AGT-05, the CTL rows)." |
| 339 | True-peak limiter (ADI-L) | Replace "**ADI-L, the director's Pro-L 2 behaviour clone**, in every channel present but off, the end of the Loudness Drive's cascade on the master" with "**ADI-L, the director's Pro-L 2 behaviour clone**, as an ordinary plug-in (the per-channel instances and the cascade's end were withdrawn on 5 November); export's true-peak brickwall is ADI's own `LookaheadLimiter` (EX-05)". |
| 377 | Rubber Band | Replace "**Not built:** a warped clip plays as silence today" with "Built offline: all six warp modes render on main (ADR-0188 d2); real-time players wait (TM-WT-04)". |

**New rows**, ready to paste into the section named above each block (each win's placement, to
strike if wrong; the priorities are P-7's).

§1, replacing line 50:

```
| **Tempo leaders**: warped clips lead the tempo; the bottom-most leader wins; Unfollow turns the result into tempo automation in one undo step | Ableton | P2 | ✅ | TM-WT-07; writes `tempo_map` on Unfollow; not the Tempo Follower (ADR-0203 d9) |
```

§1, new:

```
| **Global Quantization** (None to 8 Bars; Ctrl+6 to 0): timing for locator jumps, scrub, Set Locator and the Clip View's Set commands; Link's quantum | Ableton | P1 | ✅ | TM-SV-01, under ADR-0203 d1; in `session_state`, outside undo; the launch settings stay P2 with Session View (d10) |
| **MIDI clock in and out, MIDI timecode in**, as Live (Sync Delay, Song or Pattern clock, Hardware Resync, SMPTE All); MTC out as an ADI option | Ableton + | P2 | — | TM-LS-03; the one MIDI port host (SRF-05); MIDI 2.0 over Windows MIDI Services is SRF-16 (P2); no Tempo Follower (ADR-0203 d9) |
| **Convert Harmony, Melody and Drums to New MIDI Track** (an audio clip's context menu; one MIDI track and clip each) | Ableton | P2 | — | TM-WT-09, under ADR-0203 d1; Drums on the cached onsets, Harmony and Melody on the suite's pitch tracker (Q-D); separate from the AI workflows' audio-to-MIDI |
```

§2, new:

```
| **CPU Load Meter and Performance Impact**: the control bar's meter (Average, Current, an overload warning) and six rectangles per track | Ableton | P1 | — | DEV-24, MX-28, under ADR-0203 d1; per-track impact from per-node timings |
```

§3, new:

```
| **Consolidate** (Ctrl/Cmd+J) as Live's: audio rendered pre-effects into one clip with a compensating clip gain, MIDI clips merged with no render; **Paste Bounced Audio**; **Bounce Group in Place** and **Bounce Group to New Track** | Ableton | P1 | ✅ | TM-FC-02 with ED-06; the consolidated clip's warp state is the director's (Q-H); FL's `clip.consolidate` is this op |
| **Crop** a clip to its loop or the selection | Ableton | P1 | ✅ | ED-04; no file written |
| **Slip** a clip's content inside fixed edges (Ctrl+Shift-drag; Shift+Option on macOS), unwarped audio included as ADR-0115 d2's addition | Ableton + | P1 | ✅ | ED-05 |
| **Object snapping**: clips snap to other clips' edges, locators and signature changes | Ableton | P0 | — | ED-09; with the Arrangement's gestures (parity-7.3 row 27) |
| **Linked-Track Editing** (Live 12): one linked set per track; clip and take-lane edits mirrored in one transaction | Ableton | P2 | — | ED-08 |
| **Crossfades between takes** at comp seams, off by default | Cubase | P1 | ✅ | CT-05; the copies' own fades over their handles (the Dream, chapter 3) |
| **Offline processing on F7** (Cubase's Direct Offline Processing: a chain rendered onto a clip, re-editable, the source file untouched) | Cubase | P2 | 🔶 | ADR-0069, ADR-0203 d6; waits for `device_chains` to accept a clip owner and the M2-C-DOP readings (XC-36) |
```

§4, new:

```
| **Live's Edit Info Text** on tracks, clips and devices, shown in the Info View, copied with its object | Ableton | P1 | ❌ | RE-03; a side table and one op; never merged with remarks |
```

§7, new:

```
| **Lock Envelopes** (the Options menu and the control bar; off by default): with it off, clip moves and range commands carry the track's automation | Ableton | P1 | ✅ | AU-05; one switch, read by the engine's planner |
```

§9, new:

```
| **Project tabs**: several projects open, one active, paste between them as one undo | REAPER | P2 | — | PD-15, ADR-0068; the historical read-only tab is its read-only case |
| **Live's Second Window** (Ctrl/Cmd+Shift+W) | Ableton | P2 | — | WIN-08, after Session View |
```

### 8.3 BUILD-BRIEF.md

- **§0, the one-paragraph brief.** Replace "**No window exists yet.**" with "The first window exists, and
  the director's remarks R1 to R7 on build `add5102` (2 November) passed the first-window gate."
  Replace "(7,826 checks in 95 suites on 7 ABIs)" with "(the README's count, which win measures
  after each merge)". Append: "Since 5 November the order of work is the plan's milestones (§6.0)."
- **§4, what is built.** Correct the rows the audits found wrong (devices K7; the state map's ON13):
  - "The `.adi` format, schema 1.8, 51 tables" becomes "schema 1.19, 62 tables".
  - "170 ops designed, 76 playing" becomes "the op catalogue as measured at the merge (203 in the
    engine stack)".
  - The Pure Data tier: "built" becomes "the runtime, declarations, arrays and sample slots built and
    tested; no user-patch loader or parameter and state bridge yet; racks and their children are
    skipped (DEV-09, DEV-10, DEV-01)".
  - Live's devices: "bit-exact or measured against Live; the MIDI effects, OneShot (Simpler) and
    Sampler native" becomes "16 of Live's audio effects plus Chord Comb and Color Cab as canvases,
    with native cores for 17, tested on synthetic oracles and none yet against a Live 12 render
    (DEV-22); the MIDI effects (#207) and Sampler (#208) are to be ported; OneShot is a draft
    (#278)".
  - The plug-in line: replace "(ADI-Q, the Pro-Q 4 clone → the channel EQ; ADI-L, the Pro-L 2 clone →
    the channel limiter and the cascade's end; ...)" with "(ADI-Q, ADI-L, ADI-Clip, ADI Still, ADI
    Prism: his clones, loaded as ordinary plug-ins; the ruling of 5 November)".
  - Step 7: "in progress, **no window yet**" becomes "the first window built; his remarks R1 to R7
    received; the window lane continues (worklist of 5 November)".
- **§5, the first row.** Replace the ADI-Q and ADI-L row with: "| ~~ADI-Q and ADI-L built into every
  channel and group, present but off ...~~ Withdrawn by the director on 5 November: no reserved
  slots and no separate built-in ADI-Q or ADI-L; his plug-ins load as ordinary plug-ins; their source
  becomes one unified built-in feature after his list | the record ADR of 5 November (P-6; amends
  ADR-0203 d14, ADR-0204 d1 and ADR-0209 d11) | — |".
- **§6, a new §6.0 before Phase A.** "**The milestones (from 5 November).** The order of work is
  `docs/PLAN.md` §2: M0 green main and the records; M1 the window on what the engine
  merged, his plug-ins as ordinary plug-ins; M2 the edit kernel and the gesture; M3 time, media, MIDI
  in and safe files; M4 signal paths as Live, one meter, the Mix View's P1; M5 renders, export,
  crash safety, DAWproject; M6 notes, pitch, takes, automation passes, warp; M7 the device
  platform; Track C, the control core, on the director's timing; then P2 by priority. The phases
  below stay the scope map; where their order differs, the milestones win."
- **§6, Phase A item 0.** Replace "**The processing precision itself is the director's open choice
  (ADR-0205 d10)**: if he takes 64-bit end to end (win's recommendation), ..." with "The processing
  precision was ruled on 25 October (ADR-0207 d2): Live's scheme by default (32-bit processing,
  64-bit summing), 64-bit end to end as the option on the Audio page."
- **§6, Phase D item 7.** Replace "The Loudness Drive (VCA over the leaf tracks' input gain, the
  whole cascade), with export LUFS targeting (default no normalising) and the Export settings page;
  the Pro-L 2-class limiter (the director's clone) as the cascade's end." with "The Loudness Drive as
  one automatable gain over the leaf tracks' input gain (ADR-0203 d14), engaging nothing: the
  cascade is whatever clippers and limiters the user places, his own plug-ins included; with export
  LUFS targeting (default no normalising) and the Export settings page."
- **§6, Phase D item 7a.** Strike it: "~~7a. ADI-Q and ADI-L, present but off on every channel and
  group, ...~~ Withdrawn on 5 November."
- **§8, open points.** Replace "The director's clones ADI-Q, ADI-L, ADI Still and ADI Prism: their
  hand-over dates" with "The director's list of plug-ins to rework (the unified feature waits for
  it)". Strike "Processing precision: the director chooses ..." (ruled, ADR-0207 d2). Add: "The Live
  12 measurement seat (G-1)." "The plan's choices Q-A1 to Q-N2 (PLAN §6)."
- **§9, what the orchestrator must not do.** Add: "design the unified built-in feature before the
  director's list; reserve a slot for his plug-ins; host his VST3 inside a built-in device."

### 8.4 The other texts the records touch

- **DECISIONS:** the record ADR of the 5 November ruling (R-a, P-6: his words, nothing designed;
  amends ADR-0203 d14's cascade, ADR-0204 d1 and ADR-0209 d11); the seventeen within-authority
  ADRs of R-d, the one list of P-8 (1 the gesture contract; 2 EditScope with Lock Envelopes and
  ripple; 3 the media reader; 4 one meter; 5 the measurement harness; 6 one capture; 7 jobs and
  staleness; 8 the three render contracts; 9 the converter rules; 10 comping is Live's, ADR-0203
  d1 over ADR-0037, which CT-01 waits for; 11 the warp-analysis stack; 12 tuning sources and the
  keyboard map; 13 remark threads; 14 Graph threads; 15 the modulation schema; 16 the testing ADR;
  17 one control stack), each numbered by win when it lands and each told to him in §6.2 to strike;
  ADR-0180's two settled open items struck; after Q-F1, the Link Audio closure of ADR-0204 d5
  reopened; after Q-O, the discovery mechanism recorded under ADR-0039 d4 as he answers.
- **OPS:** `track.setFreeze` and `group.setFreezeScope` as the freeze ops; the six tuning and key ops
  to P1; the comp verbs named; §6.4's 300 ms merge retired by the gesture ADR; §9.7's rack ops marked
  as capturing their whole subtree.
- **SPEC:** §6.2 comping as Live's; §6.5's "There is no Session View" with the P2 tables; §3.7
  "existing destinations are never overwritten" becomes "never without the user's confirmation";
  §8.4's `window_state` note; §12 item 6 (server-ordered collaboration) recorded when CO-01 starts.
- **AI-AGENT:** line 3's "Nothing built" corrected; §6.1's cap 256 becomes 6,000 (ADR-0203 d20).
- **UI-ARCHITECTURE §12, the parity checklists** (7.6 rows 6 and 11, mix-view row 6),
  **SETTINGS-CATALOGUE** (Loop/Warp Short Samples per Q-E; the Link Audio rows per Q-F1; the Tempo
  Follower rows to Rejected; MIDI-CI's precondition struck; comping at P1) and **README** (the
  counts).
- **AWAITING:** the new rows of §7.1 (the cue tap, meter ballistics, monitor defaults, the crash
  dialog and Crash folder, the Backup folder, an older-major Set, the export behaviours, the Collect
  dialog, templates, Preferences across versions, Live's window rules, plug-in window level).
- **Win's working plans:** the harvest ledger's §1 and the Mix View program's slot parts marked
  superseded (XC-27).

---

## Appendix A. The per-area detail

Every row's full card, area by area, in each matrix's own order. The fields are the matrices'
text with four changes:
- each card's head gives the row's final kind (and the kind it had before the cross-check or this
  plan changed it), its size, evidence, lane, milestone and dependencies;
- text that the cross-check or this plan superseded is struck through (`~~...~~`) and followed by
  the reason in brackets;
- a few parenthetical remarks in the Reuse lines are removed, so that code is described only as
  usable or not usable;
- a closing line, "Cross-check and this plan", names every change made to the row (§5).

Section, conflict and question numbers inside a card (§n, Cn, Kn, "conflict n", CH-..., D1, Q1)
are the source matrix's, and the file is named at the head of each area. Every test, mission and
Live reading named in a card is NOT RUN.

### A.1 Editing and the arrangement (44 rows; source the editing matrix (win's working file))

#### ED-01 Split, range commands and clipboards in the window

*decided* · size M · evidence documented · lane engine, window · milestone M2 · after: none

- **Need:** Split (Ctrl/Cmd+E), Cut, Copy, Paste, Duplicate and Delete of material, and 0 (deactivate), over a time selection, acting on clips that cross its edges, one undo step each, as Live.
- **Decision now:** ADR-0203 d7 (selection P0, range commands P1); FEATURES §3 Time-range selection row; N17 (partly merged); BUILD-BRIEF Phase C and Phase D item 17; window worklist E1, E2.
- **On main (audited at ca0290c2):** Engine on main (#393, through #420): clip_commands prepareSplit (one point), copyClip, prepareCutClip, preparePaste, prepareDuplicate, copyRange, prepareCutRange, prepareDeleteRange, preparePasteRange and prepareDuplicateRange, each one clip.split, clip.duplicate, clip.delete or arranger.splice step committed with ADR-0153's expected head; tests/test_clip_edit.cpp passes on every Linux and Windows leg of main's CI (run 41952568431 at ca0290c2). Window: the selection is built (#329, #331), but Delete and 0 act on whole clips only and Cut, Copy, Paste and Duplicate are absent. Open: a split at two points as one undo step (N17); eraseRange re-creates the surviving tail of a clip that starts inside the range under a new id; every range producer takes all clips of a track whatever their lane.
- **Proposed contract:** The window calls the producers and commits each prepared batch through OpSubmitter (worklist A2), one undo step per command; a stale batch is refused, never rebased (ADR-0148 d4, ADR-0153 d1). A split of a selection inside one clip makes both cuts in one step; 0 over a crossing selection is the split plus clip.setMute in one batch. On a split the left piece keeps the clip id; when only one piece of a clip survives an edit, that piece keeps the id (eraseRange's tail case changes). Until take lanes exist (CT-01) range edits touch the main lane only.
- **Reuse:** RE1, RE3 and RE5 as test cases only (SoundCraft edit_modes and consolidate_selection, openDAW's repeated-cut loop offsets, Audacity's sample-boundary and mixed-rate cases); the producers are built.
- **Reject or defer:** Reject Cubase's Range Tool split preference and Delete Overlaps override (C5W-023). Do not keep REAPER's split-every-track-with-nothing-selected as the default: editing.splitAllWithNoSelection is on today, against ADR-0203 d11 (REAPER options default to Live's behaviour); the Live reading sets it.
- **Research IDs:** A6C-083, A4-012, C-R01, C-R02, C-R03, R11E-002, M2-C-RANGE
- **Unresolved test (NOT RUN):** M6-A6C-083 (names, offset rebasing, grouping of a selection split); the M2-C-RANGE truth table for clips touching the boundary; Live 12.4: what Ctrl+E does with nothing selected.

#### ED-02 The ...Time commands and Insert Silence

*decided* · size M · evidence documented · lane window · milestone M2 · after: none

- **Need:** Live's Cut, Copy, Paste, Duplicate and Delete Time and Insert Silence (Ctrl/Cmd+I) across every track, moving locators, signature markers, the loop and global automation, one undo step.
- **Decision now:** ADR-0203 d6 (Ctrl+I is Live's Insert Silence) and d7; FEATURES §3 Ripple edit / insert-delete time (P1); N17; window worklist E3.
- **On main (audited at ca0290c2):** Engine on main: prepareInsertSilence, prepareDeleteTime and the whole-Set clipboard (copyTime, prepareCutTime, preparePasteTime, prepareDuplicateTime) with exact tempo integrals, guarded automation curve slices (automation_time_edit.cpp, automation_segment_ranges) and key and tuning bindings, as docs/ENGINE-EDIT-COMMANDS.md describes; tests/test_time_edit.cpp passes on every Linux and Windows leg. Window: none of the commands; the fragmentary-bar commands are drawn dimmed (#364).
- **Proposed contract:** As built: the whole Set over a musical span, linear objects moved by the integrated tempo, crossing clips split, the join keeping the old end's tempo, meter and key. The window adds the Edit menu items and Live's keys; a Time copy shares the clipboard with ED-01. Hidden and grouped tracks take part; frozen tracks follow ED-08.
- **Reuse:** None needed for the commands. RM2's relation table later serves range remarks (RE-02); RE7's scope mask as scope tests.
- **Reject or defer:** Reject Ardour's and Qtractor's Insert/Remove Time option dialogs (RE4, RE7) beyond Live; per-track insertion of time belongs to ripple (ED-07).
- **Research IDs:** A4-012, A5C-035, C-R02, R11E-018, R11E-054
- **Unresolved test (NOT RUN):** A loop wholly inside a deleted span (ENGINE-EDIT-COMMANDS: disabled, not yet measured in Live); locators and signature markers exactly at a boundary (A4-012); completing or deleting a fragmentary bar (A5C-035).

#### ED-03 One placement rule: EditScope and Live's overlap rule

*refinement* · size M · evidence documented · lane win, engine, window · milestone M2 · after: none

- **Need:** One placement rule: moving, resizing, pasting, duplicating, dropping or recording a clip over another on the same lane replaces the covered material, as Live; every edit declares what it touches (tracks, lanes, clips, automation, markers, tempo) and can describe itself before it commits.
- **Decision now:** ADR-0203 d1, d7; ADR-0021 (the UI resolves scope into literals); setting editing.deleteOverlaps ('as in Live', on, read by nothing); the harvest's proposed win ADR 'range edits take an explicit EditScope'.
- **On main (audited at ca0290c2):** clip.move and clip.resize are raw UPDATEs (ops_catalog.cpp clipMoveApply, clipResizeApply): no overlap resolution, ticks only (a linear-time clip does not move), no content-offset change. preparePaste and prepareDuplicate place a clip without clearing the destination, while preparePasteRange clears it through eraseRange. The window emits raw clip.move and clip.resize (ui_arrangement.cpp, range_edits.hpp moveClips). No EditScope, no describe().
- **Proposed contract:** One engine planner (edit_plan) turns every placement into captured rows: the destination span on the same track and lane is cleared by the existing eraseRange kernel (split at the edges, delete what is inside), then the clip is placed, in one arranger.splice step. describe(PreparedEdit) names what is split, removed and shifted, for the Status Bar and the agent's preview. EditScope holds tracks, lanes, a time range and flags (clips, automation per Lock Envelopes, markers, tempo/meter/key, loop) with Live's presets. The window stops sending clip.move and clip.resize; a drag previews in the view and the Session follows on commit (arranger.splice is a graph rebuild where clip.move was a snapshot change, so the drop latency is measured on a 100-track project). Undo restores rows byte for byte.
- **Reuse:** RE1 (SoundCraft's edit algebra: place is clear then insert, put_clips clears every destination first; ported as a pure planner, not its MIDI-drop-on-split policy), RE3 (openDAW's Live-model overlap resolver, right to left, loop offsets kept), RE5 (Audacity's trimOrDeleteOverlapping cases and sample-boundary tests), RE7 (Qtractor's scope bit mask, design).
- **Reject or defer:** Reject Cubase's Shift override of Delete Overlaps and Lock Event Attributes (C5W-023), and REAPER's item-group lead/follow (R11E-013, R11E-014): ADI groups are ADR-0044's. Defer RE8 razor areas: Live's multi-track time selection over clip and automation lanes covers them. editing.mutedClipsAsDeleted (Cubase) stays an option, off.
- **Research IDs:** A6C-077, C-R04, C5W-023, R11E-013, R11E-015, R11E-017, R10H-030, R10H-031
- **Unresolved test (NOT RUN):** Live 12.4: a clip moved over the middle, the start, the end and the whole of another, warped and unwarped, audio and MIDI (are a MIDI clip's crossing notes cut?); a seeded random sequence of 40 edits on 3 tracks with an overlap check after every commit and undo-all equal to the start digest (RE2's never_overlaps).
- **Cross-check and this plan:** XC-33: the window's move and resize gestures bind to this planner when it lands; their parity rows stay partial until then.

#### ED-04 Left trim, the clip's start, and Crop

*refinement* · size S · evidence documented · lane engine, window · milestone M2 · after: ED-03

- **Need:** Trimming a clip's left edge (and Enter at its start) keeps the material anchored in time, as Live; Crop trims a clip to its loop or the selection without writing a file.
- **Decision now:** ADR-0203 d7; N33 (clip markers merged); FEATURES has no Crop row.
- **On main (audited at ca0290c2):** clip.resize moves pos and length without moving content_offset_ticks, so a left trim slides the audio; the window refuses Enter at a clip's start because 'its audio would slide'. clip.setStartMarker, setEndMarker and setLoop exist (#413). No Crop.
- **Proposed contract:** prepareTrim(clip, edge, to) in the planner: the left edge moves the content offset (ticks, and the pinned ns clock for audio) by the same amount; audio cannot trim before source frame 0, MIDI can; fades clamp. Crop follows Live's rule: unlooped start to end; looped, the earlier of start and loop start to loop end; a time selection overrides; MIDI notes outside are removed in the same step and audio writes no file.
- **Reuse:** RE4 (Ardour's trim_to_internal and modify_front maths, clamped at source zero for audio and not MIDI; design), RE1 (SoundCraft trim_clip, trim_to), RE3 (openDAW trimStart keeping the loop offset anchored).
- **Reject or defer:** Reject REAPER's move-item-without-contents and trim variants beyond Live (R11E-003); Cubase's Crop of events wholly inside or outside (C-R03), since Live's Crop is per clip.
- **Research IDs:** A6M-033, A6M-034, A6C-076, R11E-003, C-R03
- **Unresolved test (NOT RUN):** M6-A6M-033 (expression of crossing notes on Crop); M6-A6C-076; a left trim of a looped clip at the loop start; a left trim of a warped clip at a warp marker.

#### ED-05 Slip content inside fixed edges

*refinement* · size S · evidence documented · lane win, engine, window · milestone M2 · after: ED-04

- **Need:** Slip a clip's content inside fixed edges.
- **Decision now:** ADR-0115 d2 ('Shift-drag slides the audio inside the clip without moving its edges, a parity item under ADR-0108'); ADR-0203 d1, d12 (import with no warp).
- **On main (audited at ca0290c2):** Not built: no slip gesture or op (content_offset_ticks is writable through clip.setStartMarker).
- **Proposed contract:** Live's gesture: Ctrl+Shift-drag on Windows, Shift+Option on macOS, adding Alt or Cmd bypasses snap, on warped audio and MIDI. ADR-0115 d2's addition extends the same gesture to unwarped audio, which ADI's Warp-off import makes the common case. One op moves the content offset and the ns source clock with edges and fades fixed; one undo step per drag.
- **Reuse:** RE2 (FilmCraft's slip with handle clamps, into the planner), RE1 (SoundCraft slip).
- **Reject or defer:** ADR-0115 d2's plain Shift-drag binding gives way to Live's modifier, since d2 itself calls slip a parity item. Defer REAPER roll and slide (RE2).
- **Research IDs:** A6C-076, R11E-003, R10H-035, B-A07
- **Unresolved test (NOT RUN):** M6-A6C-076 on Live 12.4: the Windows modifier, snap, behaviour on an unwarped clip; a slip past the source's end.

#### ED-06 Consolidate

*refinement* · size M · evidence documented · lane engine, window · milestone M5 · after: ED-03

- **Need:** Live's Consolidate (Ctrl/Cmd+J) of a selection on a track into one clip: audio becomes a new sample (pre-effects, as Live), MIDI clips merge their notes; undoable.
- **Decision now:** FEATURES §3 Make Unique row (FL's 'Make unique as sample' is clip.consolidate); ADR-0203 d16 (renders placed Warp off); the ledger gives the render contract to freeze-cache S5.
- **On main (audited at ca0290c2):** Not built: no clip.consolidate; whole-track freeze capture exists (#416).
- **Proposed contract:** Consolidate = ED-03's scope plus the freeze-cache render of the clip material only (pre-FX, pre-mixer, as Live), ~~the result placed Warp off like every ADI render (d16 extended to Consolidate: win's placement, to strike if wrong)~~ *[Withdrawn (XC-01): the warp state is the director's Q-H]*, the sources replaced in one undo step, ~~the file in the project's processed folder~~ *[XC-01: the folder follows TM-FC-03's ruling]*. MIDI clips merge notes (ids kept where unique, expression carried) without rendering.
- **Reuse:** RE1 (SoundCraft consolidate, clip path), RE6 (Tracktion mergeMidiClips, design), RE3 (openDAW AudioConsolidation: consolidation before effects).
- **Reject or defer:** Reject REAPER's Glue that bakes item FX (R11E-010). Defer consolidating automation until Live is read.
- **Research IDs:** A4-005, A5C-022, R11E-009, R11E-010, R11E-011, B-O04
- **Unresolved test (NOT RUN):** Live 12.4: consolidate a warped clip (warp state of the result), a range with gaps, MIDI clips with overlapping notes; a null test of the consolidated audio against its source.
- **Cross-check and this plan:** XC-01: one Consolidate; TM-FC-02 owns the render, op, file and MIDI merge; the Warp-off placement is withdrawn and the warp state goes to the director (Q-H); the folder follows TM-FC-03's ruling.

#### ED-07 Ripple editing (an addition)

*refinement* · size M · evidence documented · lane engine, window · milestone M6 · after: ED-03, AU-05

- **Need:** Ripple editing, an addition Live lacks: delete, cut, paste and move close or open time on the affected tracks.
- **Decision now:** FEATURES §3 Ripple edit / insert-delete time (P1); ADR-0203 d2 (visible, not in Settings), d11 (REAPER options default to Live); setting editing.rippleLockedClips.
- **On main (audited at ca0290c2):** Not built. Live's ...Time commands, the global form of ripple, are built (ED-02).
- **Proposed contract:** A visible ripple switch: Off is Live's leave-gap and the default; Per track; All tracks. The EditScope planner closes or opens time to the right on the affected tracks, right to left; automation follows Lock Envelopes (AU-05); markers and tempo move only in All tracks, as the ...Time commands; frozen material stops the ripple and the Status Bar names the track; one undo step.
- **Reuse:** RE2 (FilmCraft ripple delete with sync-lock conflicts, validate first), RE5 (Audacity's close-gap and paste taxonomy and tests), RE4 (Ardour's right-to-left ripple list and marker clamp, design), RE6 (Tracktion: CloseGap::no does not shift automation).
- **Reject or defer:** Defer REAPER's four ripple lock policies (R11E-017) beyond one switch, per-lane ripple (R10H-022) until take lanes exist, and razor ripple (RE8).
- **Research IDs:** R11E-016, R11E-017, R11E-018, R11E-019, R10H-022, C-R02, C-F06
- **Unresolved test (NOT RUN):** Two clips deleted with Per track ripple close both gaps right to left; a frozen track under All tracks refuses with its name; insert-then-extract renders identical PCM (RE2).
- **Cross-check and this plan:** XC-12: ripple stops at frozen material by TM-FC-04's rule.

#### ED-08 Frozen, grouped and linked tracks under edits

*refinement* · size M · evidence documented · lane win, engine, window · milestone M6 · after: ED-03, CT-01

- **Need:** A rule per operation for frozen, grouped and hidden tracks, and Live 12's Linked-Track Editing (one linked set per track; clip and take-lane edits mirrored).
- **Decision now:** ADR-0203 d1, d16 (freeze); ADR-0044 (groups); FEATURES has no Linked-Track Editing row.
- **On main (audited at ca0290c2):** Not built; frozen tracks exist (#416) with no edit policy.
- **Proposed contract:** An operation-capability table per track state, applied by the planner (frozen: Live's rule, read first; folded group members: included by selection rules). Linked-Track Editing as Live 12: one linked set per track, clip and take-lane edits mirrored in one transaction, distinct from groups and routing. The frozen-track rule lands with ED-03; Linked-Track Editing is win's placement at P2, to strike if wrong.
- **Reuse:** RE7 (scope mask), RE6 (Tracktion EditSpaceTargets: clip tracks, sub-tracks and their automatables; design).
- **Reject or defer:** Reject REAPER item locks and edit groups (R11E-013 to R11E-015) and Cubase's Lock Event Attributes: Live has no item lock.
- **Research IDs:** A4-013, R11E-013, R11E-014, R11E-015
- **Unresolved test (NOT RUN):** Live 12.4: editing a frozen track's clips (refused or unfreezes?); linked tracks with different lane counts; one-step undo of a linked edit (A4-013).
- **Cross-check and this plan:** XC-12: the frozen rule is TM-FC-04's; this row keeps grouped, hidden and linked tracks.

#### ED-09 Object snapping

*decided* · size S · evidence documented · lane window · milestone M2 · after: none

- **Need:** Clips snap to other clips' edges, locators and time-signature changes as well as the grid, as Live.
- **Decision now:** ADR-0129 (Live 12's gestures as the parity checklist); parity-7.3 row 27.
- **On main (audited at ca0290c2):** Window: grid snapping exists; object snapping is missing (parity-7.3 row 27).
- **Proposed contract:** Snap candidates from the snapshot (clip edges on any track, locators, signature changes) inside a measured pixel threshold; the modifier inverts snapping; no engine change.
- **Reuse:** None (window code).
- **Reject or defer:** Reject snap to zero crossings (ADR-0070 refuses zero-crossing snapping for exact regions).
- **Research IDs:** A6C-077, A4-015, A6M-003
- **Unresolved test (NOT RUN):** M6-A6C-077 (candidate priority, threshold, modifier effects).

#### ED-10 Fade handles, Create Fade and Crossfade

*decided* · size M · evidence documented · lane window · milestone M2 · after: none

- **Need:** Live's fade handles on audio clips (length and curve, one fade for all selected), Create Fade and Crossfade (Ctrl/Cmd+Alt+F) and Delete Fades, with Create Fades on Clip Edges off; Cubase's fade editor on a double-click is the P2 addition.
- **Decision now:** ADR-0203 d7 (P0; fade editor P2), d12 (Create Fades off); ADR-0132 d6; FEATURES §3 fades row; N17; window worklist E4.
- **On main (audited at ca0290c2):** clip.setFades is on main with warped fade domains and the ClipNode fade fields; record.importEdgeFades (Create Fades on Clip Edges) is off by default; the window has no handles and no Create Fade or Crossfade (worklist E4).
- **Proposed contract:** Handles write clip.setFades as one gesture step; edges cannot cross the fade peaks, loop boundaries or the opposite fade (A6C-079); a selection over a clip edge creates that fade and over a boundary a crossfade (both clips' fades over their handles); Delete removes a fade (Create Fades off) and resets it to 4 ms only when the switch is on (A6C-080).
- **Reuse:** None needed (built).
- **Reject or defer:** Reject automatic edge fades by default (the director's deviation) and Cubase's auto-fade layer (C-F01). The fade editor (P2) belongs with the clip-editing area.
- **Research IDs:** A6C-079, A6C-080, C-F01, C-F02, C-F07, M2-C-FADE
- **Unresolved test (NOT RUN):** AWAITING row 14 (Create Fades in a fresh install); M2-C-FADE curve laws on DC, correlated, inverted and independent sources; A6C-079 limit line under a crossfade.

#### CT-01 Take lanes exist; only the main lane plays

*refinement (was decided; this revision: the contract supersedes ADR-0037's comping wording under ADR-0203 d1 and depends on R-d's comping ADR, so an ADR is owed)* · size M · evidence documented · lane win, engine, window · milestone M6 · after: none

- **Need:** Live 12's track model: one main lane (the comp, audible) and take lanes heard only when auditioned; show, hide, insert, duplicate, delete, delete unused, rename, reorder and resize lanes.
- **Decision now:** FEATURES §3 Take lanes and comping (P1) and §8 Loop record to take lanes (P1); OPS §9.2 lane ops (designed, P1); SPEC §6.2 (Cubase-era wording); ADR-0203 d1; ADR-0182 d5 (a collaboration collision becomes a take).
- **On main (audited at ca0290c2):** In the schema since 1.0 and unused: the lanes table and clips.lane_id. No lane op is registered. The engine plays every clip of a track with no lane filter; range and clipboard producers ignore lanes and paste clears lane_id. The store and the text projection read lanes. The window has nothing.
- **Proposed contract:** A NULL lane_id is the main lane. A take-lane clip plays only while its lane is the track's auditioned lane (session state, one per track, not an op). lane.* ops as OPS §9.2 plus clip.setLane; every producer takes its lane from EditScope. SPEC §6.2 is reworded to Live's model (main-lane copies), and ADR-0037's 'comping follows Cubase' is recorded as superseded by ADR-0203 d1. No schema change.
- **Reuse:** None for the structure; CP5 later as the comp oracle (CT-04).
- **Reject or defer:** Reject REAPER's lane masking and play-all-lanes layering (R10H-021, R10H-022, R10H-028) and Bitwig's comp-region sliding (B-A07): not Live.
- **Research IDs:** A6H-027, A6H-029, R10H-020, R10H-021, R10H-022, R11E-040, B-A07
- **Unresolved test (NOT RUN):** M6-A6H-029: does audition silence the main lane, is it saved, is it undoable; the name a new lane gets; lanes round-trip through the text projection.
- **Cross-check and this plan:** XC-21: one engine accessor for "timeline clips that play"; every clips reader moves to it in this row's first PR, and TM-SV-02 only extends it.

#### CT-02 Audio loop recording into take lanes

*decided* · size M · evidence documented · lane engine, window · milestone M6 · after: CT-01

- **Need:** Audio loop recording into take lanes: one file per loop recording, one take clip per pass, the last pass copied to the main lane, a lane reused when nothing follows the punch-in, passes undone one by one (Live 19.3.1, 21.3).
- **Decision now:** FEATURES §8 Loop record to take lanes (P1); ADR-0132 d1; ADR-0203 d11 (discard incomplete takes starts off).
- **On main (audited at ca0290c2):** The audio recorder (#406) writes one linear take on the main lane and refuses a loop wrap as a discontinuity. No take lanes.
- **Proposed contract:** The recorder keeps a preallocated pass log (start, frames, loop offset), with no lock or allocation on the callback. At stop one transaction writes one media row, a take clip per pass at offsets 0, L, 2L and on, on new or reused take lanes, and a main-lane copy of the last pass; the incomplete last pass is kept.
- **Reuse:** CP1 (Ardour's capture-pass bookkeeping, ported without its locks), CP2 (Qtractor's take-window arithmetic as the loopPassWindows oracle), CP3 (openDAW loop-take scenarios, tests).
- **Reject or defer:** Reject REAPER's takes inside items (R10H-020) and a new FIFO (R10H-039).
- **Research IDs:** A6H-016, A6H-027, A4-017, R10H-039
- **Unresolved test (NOT RUN):** M6-A6H-016 (undo grouping per pass); M6-A6H-027 (which free lane is reused, where pre-roll goes); AT-3: 3.5 passes give one media row and four take clips, one undo restores pass 3.

#### CT-03 MIDI loop takes

*decided* · size M · evidence documented · lane engine · milestone M6 · after: CT-01, CT-02, MI-01

- **Need:** MIDI loop recording into take lanes, with held notes closed at each pass end and a new lane per discontinuity.
- **Decision now:** FEATURES §8 (P1); ADR-0132 d8 (Rec-Q its own undo step); N31 (Rec-Q ops merged; nothing records MIDI yet).
- **On main (audited at ca0290c2):** Not built: nothing records MIDI into clips on main; the MIDI Capture memory is on main (#430) and capture into clips is open (#446, #447).
- **Proposed contract:** The MIDI recorder writes one take clip per pass into take lanes through note.applyDiff, closing held notes at each pass end; record quantize lands as its own undo step; note ids come from the recorder (ADR-0021). **Arrangement Overdub** (the control bar already draws the button, `ui_controlbar.cpp`: "Records MIDI on top of what is there"; nothing on main records MIDI into clips): with Overdub on, a pass that lands on an existing Arrangement clip merges the new notes into that clip through note.applyDiff, the merged notes receiving new stored ids under DEV-17's identity, the clip's existing notes untouched; with it off, the pass replaces the clip's notes over the recorded range (Live's behaviour, to be read); loop on repeats the merge or the replacement per pass; the whole pass is one undo step either way.
- **Reuse:** CP4 (Zrythm's recording materializer, design and tests; never its name), CP3.
- **Reject or defer:** Defer Session-style loop overdub merging: Session View is P2.
- **Research IDs:** A-H02, A6H-027, A4-018, R11E-040
- **Unresolved test (NOT RUN):** AT-14 (a held note across the loop boundary, read in Live); A-H02 (Rec-Q versus loop-overdub quantize undo); Live 12.4: Overdub on and off into an existing clip, loop on and off (what merges, what replaces, where a pass that starts inside a clip begins, one undo step or two).
- **Cross-check and this plan:** XC-34: needs SRF-05's port host and #446 and #447 merged. This revision adds the overdub cases (the critique of 5 November).

#### CT-04 Comp gestures, audition and source highlights

*decided* · size L · evidence documented · lane engine, window · milestone M6 · after: CT-01, ED-03

- **Need:** Live's comp gestures: audition (T; one lane per track; the last selected lane wins), Copy Selection to Main Lane (Enter), Ctrl/Cmd+Up/Down to replace with the next take, Draw Mode painting, a click into a main-lane selection, source highlights and dragging their edge, Delete (Unused) Take Lanes; main-lane clips are independent copies.
- **Decision now:** FEATURES §3 (P1); ADR-0203 d1; OPS §11.4 (comp verbs not named).
- **On main (audited at ca0290c2):** Not built.
- **Proposed contract:** Comp commands compose existing producers (a range copy from a take lane, erased and pasted onto the main lane, one arranger.splice step); no schema. Provenance is derived by matching position and properties, never stored. Dragging a highlight edge moves the split between two main-lane copies in one step.
- **Reuse:** CP5 (MAGDA's comp-section span algebra as planner and test oracle, ported to int64 ticks), CP6 (Tracktion's comp-edge drag rule, design).
- **Reject or defer:** Reject Tracktion's and Cubase's active-region comp model (the opposite of Live's), REAPER's comp areas as separate selector objects (R11E-042) and ranked take markers (R11E-044).
- **Research IDs:** A6H-029, A6H-030, A-H04, R10H-023, R10H-024, R10H-025, R11E-041, R11E-042
- **Unresolved test (NOT RUN):** M6-A6H-030 (empty or partial lanes, painting across silence, an edge dragged past a take's end); which properties a source highlight compares (21.7); AT-9 (highlights exactly the copied spans); AT-10 (an edge drag still nulls against both takes).

#### CT-05 Take crossfades and comp seams

*refinement* · size S · evidence inferred · lane win, engine, window · milestone M6 · after: CT-04, ED-10

- **Need:** Cubase's crossfade between takes (Dream §3: 'with Cubase's crossfade between takes added'), while Create Fades on Clip Edges stays off, so comp seams need not click.
- **Decision now:** Dream Reference §3 (the director's text); ADR-0203 d7, d12; FEATURES §3 fades row.
- **On main (audited at ca0290c2):** clip.setFades on main; no seam crossfade command; Create Fade and Crossfade are window item E4.
- **Proposed contract:** A seam crossfade is the two main-lane copies' own fades over their source handles, not a new object: a 'Crossfade comp seams' command, and an option on the comp gestures, writes them in the same step. Off by default. win's placement: P1 with comping, to strike if wrong.
- **Reuse:** None in the ledger; the curve formulas exist (ADR-0159).
- **Reject or defer:** Defer Cubase's advanced crossfade editor (P2, with the fade editor) and REAPER's crossfade snapshots (R11E-024).
- **Research IDs:** C-F04, C-F05, C-F06, R11E-024, A6C-080
- **Unresolved test (NOT RUN):** A seam at a take's end without handles refuses or shortens (C-F04); an unfaded comp nulls against its takes.

#### AU-01 Lanes and breakpoints in the window

*decided* · size L · evidence documented · lane engine, window · milestone M2 · after: HI-01

- **Need:** Automation lanes in the window: Automation Mode (A), Unfold, U and Alt+U, add, move and delete breakpoints, Alt-drag bends a segment and Alt-double-click straightens it, multi-point drags, stretch and skew handles, values in real units.
- **Decision now:** FEATURES §7 track and device automation (P0), curved segments (P1), real units (P1); ADR-0159; ADR-0203 d7; N26 (merged); window worklist C7.
- **On main (audited at ca0290c2):** Engine on main (#424 to #426, #386): automation.writePoints, deletePoints, movePoints, setCurve, createLane, deleteLane, clearLane, setLaneEnabled, setLaneVisible and setDefaultValue, and the snapshot's lanes; tests/test_automation_edits.cpp and test_automation_lanes.cpp pass. Defect (automation harvest, verified): the point ops re-sort the AAUT stream but automation_segment_ranges stay keyed by record_index; only the time edits re-key them. Window: lanes say they wait (kUnfoldWaits); no breakpoint editing.
- **Proposed contract:** Each breakpoint gesture is one gesture (HI-01) and one undo step; new lane and point ids come from the snapshot (ADR-0021); the point ops re-key automation_segment_ranges in the same op. Before the first point a lane holds the first value, and a disabled lane plays its control's own value (ADR-0159's open item closed as Live).
- **Reuse:** AU1's pure splice functions for point writes over a range (with AU-02).
- **Reject or defer:** Reject Cubase's Virgin Territory (C3-015) and Bitwig's Hold and Spread point options (B-V03): not Live.
- **Research IDs:** A6C-053, A6C-058, A6C-059, A6C-060, A6C-062, A5W-016, A5W-017, C3-015
- **Unresolved test (NOT RUN):** AWAITING row 12 (how Live stores an Alt-drag curve); M6-A6C-058 to 060 (a drag over a neighbour deletes it; axis lock); M6-A6C-062 (Shift moves outside points).
- **Cross-check and this plan:** XC-22: new lane and point ids come through the one id helper (M1), then the one allocator (M2).

#### AU-02 Insert Shape

*decided* · size S · evidence documented · lane engine, window · milestone M6 · after: AU-01

- **Need:** Live's Insert Shape on a time selection: sine, triangle, sawtooth, inverse sawtooth and square scaled to the selection; the two ramp sets and the ADSR joined to the values before and after.
- **Decision now:** ADR-0203 d7 (P1, exactly Live's set); FEATURES §7.
- **On main (audited at ca0290c2):** Engine on main: ten shapes and prepareShapeInsertion (automation_shapes.cpp), replacing the points in [start, end] with no guard points, so the curve outside moves near both edges; tests/test_automation_shapes.cpp passes. Window: no menu.
- **Proposed contract:** Top-row shapes scale to the selection and the parameter's range; lower-row shapes join the neighbouring values (Live's dotted line); the curve outside the selection is unchanged (guard points at both edges, through one shared splice); one undo step.
- **Reuse:** AU1 (Ardour's guard-point policy as pure functions over a sorted vector, no locks), AU2 (Tracktion's mergeCurve shape, not its fade maths).
- **Reject or defer:** Defer user-defined shapes.
- **Research IDs:** A6C-065, A-BASE-25.5.5
- **Unresolved test (NOT RUN):** L-AUTO-8 (is automation outside the selection kept; which end each ramp and the ADSR joins); AT-1 (outside [t0, t1] valueAt unchanged within 1e-9 for every curve kind).

#### AU-03 Recording automation: the write pass

*refinement* · size M · evidence documented · lane win, engine, window · milestone M6 · after: AU-02, AU-04

- **Need:** Recording automation as Live: Automation Arm; the mouse writes until release, a controller keeps writing until the loop ends; punch, loop wrap, locate and stop end or split a pass; thinning while recording; Live's undo grouping.
- **Decision now:** ADR-0203 d7 (Arm; 'thin out while recording', yes); ADR-0162 d4; ADR-0054 d3 (a writer may thin and must record it); FEATURES §7; the harvest's proposed win ADR for the write pass.
- **On main (audited at ca0290c2):** Engine on main: AutomationRecorder (lock-free ring, gesture identity, a whole gesture discarded on overflow), prepareRecordedAutomation through prepareAutomationWrite, Session::setAutomationWriteState and automationArmed (#427, #428); tests/test_automation_recording.cpp and test_automation_write_policy.cpp pass. Missing: guard points, the right-edge latch close, the pass lifecycle (locate, stop, loop wrap, punch), thinning and a per-lane suspend while writing. Window: Arm not wired (C1).
- **Proposed contract:** A pass coordinator (engine/automation_pass) owns each lane's pass: mouse writes are touch (end on release), controller and surface writes latch to the loop end or stop; locate, stop and a loop wrap split the pass ([t0, E], then [S, t]); writes clamp to the punch range when Punch is on; both pass edges evaluate the old curve (guards); thinning at drain time keeps every value within tolerance and records that it thinned; the written lane's playback is suspended without a graph rebuild; commits group as L-AUTO-3 reads.
- **Reuse:** AU1 (Ardour write-pass policy), AU2 (Tracktion mergeCurve), AU3 (Tracktion AutomationRecordManager lifecycle: punch in and out, loop-wrap split, flush), AU6 (SoundCraft single-pass thinning), AU8 (openDAW recorder test vectors).
- **Reject or defer:** Defer Cubase's Fill, Return Time and Continue Writing (C3-018, C3-019) to the P2 modes (AU-09); reject Tracktion's 2 s release timeout.
- **Research IDs:** A3-014, A-H02, C3-016, C3-019, R10H-052, R11E-054
- **Unresolved test (NOT RUN):** L-AUTO-1 (spacing and what is written at punch-in and release), L-AUTO-2 (a controller after it stops), L-AUTO-3 (what one Undo removes), L-AUTO-4 (overlap across loop passes), L-AUTO-5 (does punch apply to automation), L-AUTO-9 (an on switch: touch or latch); AT-2 to AT-6.
- **Cross-check and this plan:** XC-17: the one place where a touch becomes automation; SRF-02's and MX-23's live values feed it while armed.

#### AU-04 Override and Re-Enable Automation

*decided* · size S · evidence documented · lane window · milestone M1 · after: none

- **Need:** Touching an automated control overrides its lane until Re-Enable Automation (one lit button for all, a context-menu item for one), as Live.
- **Decision now:** ADR-0162, ADR-0164, ADR-0165, ADR-0196; FEATURES §7; window worklist C1.
- **On main (audited at ca0290c2):** Engine built on main: Session::overriddenLanes, reenableAutomation() and reenableAutomation(lane); Snapshot::automationOverridden (#386). Window: the buttons say they are not built, and OpSubmitter refreshes the view with no override set.
- **Proposed contract:** As ADR-0162: session state, not an op. The window passes session.overriddenLanes() to ProjectView::refresh, lights Re-Enable and offers the per-parameter item; whether an override survives save follows the AWAITING row 12 reading.
- **Reuse:** None needed (built).
- **Reject or defer:** Defer A/B preset switching that disables automation (A5H-022) until device A/B exists.
- **Research IDs:** A5H-022, A6P-003, L-AUTO-7
- **Unresolved test (NOT RUN):** AWAITING row 12 / L-AUTO-7 (override after save and reopen).

#### AU-05 Lock Envelopes: automation follows clips and range edits

*refinement* · size M · evidence documented · lane engine, window · milestone M2 · after: ED-03, AU-02

- **Need:** Live's Lock Envelopes (Options menu and control bar; off by default): with it off, moving, duplicating or deleting clips and range commands on a clip-track selection carry the track's automation in that span; with it on, automation stays at song position.
- **Decision now:** ADR-0203 d1; setting editing.automationFollowsClips (on, i.e. Lock off; read by nothing); window worklist C7 (plans window-side batching).
- **On main (audited at ca0290c2):** Not built: range producers carry clip envelopes but not track automation (ENGINE-EDIT-COMMANDS), and clip.move carries nothing.
- **Proposed contract:** The EditScope planner reads the one switch and, with envelopes unlocked, moves, copies or deletes the track's device, mixer and send automation inside the affected span, replacing destination automation, with guard points at both edges of source and destination, in the edit's single undo step. The ...Time commands always move global automation and ignore the switch.
- **Reuse:** AU7 (Tracktion moveAutomation and moveSelectedClips automationLocked), AU1 (guards), RE6 (EditSpaceTargets including automatables).
- **Reject or defer:** Reject the window-side batching of automation.movePoints planned in worklist C7 (one engine implementation serves the window, range edits and the agent); defer REAPER automation items (R10H-047 to R10H-051).
- **Research IDs:** A-BASE-25.5.6, C3-014, R10H-051, R11E-054, R11E-055
- **Unresolved test (NOT RUN):** Live 12.4: a clip moved with Lock off onto automation (replaced or merged); a range Delete on the clip lane with Lock off; AT-10 (Lock off moves track, device and send automation with guards; Lock on moves nothing; exact undo).
- **Cross-check and this plan:** XC-33: worklist C7 binds Lock Envelopes to this engine implementation instead of batching in the UI.

#### AU-06 Clip envelopes: Arrangement clips modulate

*refinement* · size L · evidence documented · lane win, engine · milestone M6 · after: AU-02

- **Need:** Live's clip envelopes: in the Arrangement only modulation envelopes (volume, clip gain and sends a reducing percentage, pan scaled, device controls relative, transposition additive and clamped), linked or unlinked from the clip's loop; absolute clip automation only in Session clips.
- **Decision now:** FEATURES §7 Clip envelopes / clip modulation (P1, automation_data.clip_id); ADR-0203 d1; ADR-0046, ADR-0114 (composition).
- **On main (audited at ca0290c2):** #434 (through #430's merge): clip streams play as absolute overlays on track, device and send lanes, with loops, offsets and tempo, the latest clip winning (engine/automation_clips.cpp; tests/test_clip_envelopes.cpp passes). That is Live's Session-clip automation; on Arrangement clips it deviates from Live 12 manual §26.3 ('In the Arrangement, clips only have modulation envelopes').
- **Proposed contract:** win rules under d1 (no director question): Arrangement clip envelopes are modulation streams in a new whole-table minor (lane, clip, data, declared domain), evaluated with Live's per-parameter law after automation; #434's absolute overlay stays only for Session clips (P2) and for ADR-0115's gain curve (AU-07).
- **Reuse:** AU4 (Tracktion AutomationCurveList: absolute, relative and scale maths, linked and unlinked timing; its 13 cases rewritten for Live's laws).
- **Reject or defer:** Defer Bitwig's automation clips (B-V02); reject Tracktion's normalized relative units (ADI declares each domain).
- **Research IDs:** A4-020, A-H05, A6C-069, A6C-071, A6C-072, A3-028, B-V02
- **Unresolved test (NOT RUN):** L-AUTO-6 (volume at 50 % with the fader at -6 dB and 0 dB: fader position or gain; a device knob at 30 % with +50 %; the transposition clamp); an unlinked envelope's own loop (§26.5).
- **Cross-check and this plan:** XC-16: its table and law are written into the one modulation ADR; XC-37: its minor is numbered when the engine PR opens.

#### AU-07 The event volume curve on the clip (Cubase 14)

*decided* · size M · evidence documented · lane engine, window · milestone M6 · after: AU-06

- **Need:** The event volume curve drawn on the clip (Cubase 14): an absolute pre-fader, pre-effects gain curve on each audio clip in the Arrangement, stretching with the clip.
- **Decision now:** ADR-0115 d1; FEATURES §7 Event volume curves (P1); setting editing.wheelForVolumeAndFades.
- **On main (audited at ca0290c2):** The storage path exists (a clip-owned gain lane in automation_data) and #434 plays clip lanes as absolute overlays; nothing draws or edits the curve on the clip.
- **Proposed contract:** The clip-owned gain lane is ADR-0115's absolute curve in dB. A clip's gain is its Clip Gain slider, times Live's Clip Gain modulation (AU-06), times this curve, all before the effects. It is drawn and edited on the clip body in the Arrangement, its own undo steps; Live's envelopes stay in the Clip View.
- **Reuse:** AU4's absolute-curve timing (stretching with the clip).
- **Reject or defer:** Defer Cubase's tilt, compress and scale tools for the curve (C-F10) to the P2 fade-editor work.
- **Research IDs:** C-F10, A6C-069
- **Unresolved test (NOT RUN):** The order against Live's Clip Gain modulation (L-AUTO-6); stretch with a warped clip; a flat 0 dB curve renders a null against the clip.

#### AU-08 Simplify, Delete Automation and lane-scoped commands

*decided* · size M · evidence documented · lane engine, window · milestone M6 · after: AU-01

- **Need:** Live's Simplify Envelope on a selection (§25.5.4), Delete Automation (Ctrl/Cmd+Backspace, every clip and the Arrangement), Clear Envelope per clip, and Cut, Copy, Duplicate and Delete on an envelope selection affecting that lane only (§25.5.7), copy between parameters.
- **Decision now:** ADR-0203 d1, d7; FEATURES §7.
- **On main (audited at ca0290c2):** automation.clearLane, deletePoints and the automation clipboard used by the time edits exist; no Simplify; no lane-scoped range commands.
- **Proposed contract:** Simplify replaces the selection's points by an error-bounded reduction in one transaction (enum and hold lanes refused), with exact undo; Delete Automation removes the parameter's data in every owner in one step; lane-scoped range commands reuse the time-edit automation splice.
- **Reuse:** AU5 (MAGDA's RDP simplifier, copied with attribution, plus ADI's own curve fit), AU6 (SoundCraft thinning tests).
- **Reject or defer:** Reject REAPER's point-reduction preferences.
- **Research IDs:** A6C-051, A6C-069, A-BASE-25.5.4, A6C-062
- **Unresolved test (NOT RUN):** AT-7 (a 1001-sample exponential ramp becomes two points with a curve, within epsilon; enum refused; exact undo); Live's tolerance (L-AUTO-1).

#### AU-09 Cubase's automation modes and MIDI Learn on lanes (P2)

*decided* · size M · evidence documented · lane engine, window · milestone P2 · after: AU-03

- **Need:** Cubase's automation modes as a visible P2 addition (touch, latch, cross-over, overwrite, trim with Freeze Trim), and MIDI Learn on lane headers (P2).
- **Decision now:** ADR-0203 d7 (P2); ADR-0162 d4 (tracks.automation_mode); ADR-0195 d3 (Learn; N10's learnControllerTarget merged); FEATURES §7.
- **On main (audited at ca0290c2):** tracks.automation_mode exists; no mode logic; learnControllerTarget on main (#398) without a window path for MIDI input.
- **Proposed contract:** The modes ride on AU-03's pass coordinator; Live's Arm stays the default; Trim writes an offset layer merged into the curve at pass end as one step.
- **Reuse:** AU9 (SoundCraft's trim coalesce, densifying curves first), AU3.
- **Reject or defer:** Defer Fill and Return Time until after parity.
- **Research IDs:** C3-016, C3-017, C3-018, C3-019, R10H-053
- **Unresolved test (NOT RUN):** AT-11 (Freeze Trim at pass end: volume equals original plus trim at every point, trim lane cleared, one step).
- **Cross-check and this plan:** XC-34: MIDI Learn on lane headers needs SRF-05 (the app takes no MIDI in today).

#### MI-01 One note edit, one transaction: note.applyDiff

*refinement* · size M · evidence inferred · lane engine · milestone M2 · after: none

- **Need:** One note edit, one transaction: every note gesture or tool writes the clip's note stream once, keeps stable note ids, preserves unknown record tails, flags and expression rows, and refuses out-of-range values.
- **Decision now:** ADR-0009 (one BLOB per clip); ADR-0003; ADR-0021 (caller-allocated ids); ADR-0155; OPS §9.5; blob.hpp ('callers that re-save MUST round-trip the original bytes').
- **On main (audited at ca0290c2):** note.insert, delete, move, resize and setVelocity (ops_catalog readNotes and writeNotes), note.setMute (#389), note.quantize and note.transpose with tail-keeping streams and note.restoreNotes (#408). Five defects, confirmed in the code: a re-encode drops record tails; readNotes returns an empty list for an unreadable stream, so note.insert writes over it; noteToPayload carries no flags (an inactive note comes back active on undo) and note.delete leaves its note_expression rows; no range checks (note.move casts the key to uint8_t); one full rewrite of the blob per note.
- **Proposed contract:** note.applyDiff {clip, from (stream hash), added, removed, changed (old and new)} writes the stream once, records overlap side effects explicitly, refuses a stale 'from', round-trips tails, carries flags and expression rows through remove and undo, and validates key 0 to 127, velocity 1 to 127, length above 0, channel 0 to 15, probability up to 10000, finite cents and a nonzero id. The five P0 ops become thin wrappers over it.
- **Reuse:** ME1 (Ardour's NoteDiffCommand shape and MidiOperator pattern, ported with integer ticks and key-only matching).
- **Reject or defer:** Reject Ardour's lock-based model.
- **Research IDs:** A-M01, A6M-016, A6P-010, A6P-011
- **Unresolved test (NOT RUN):** test_note_diff: a 10,000-note diff writes the blob once; a random diff-and-undo fuzz restores the bytes; a future-tail fixture survives every note op; an inactive note deleted and undone stays inactive.
- **Cross-check and this plan:** XC-25: stored note ids are the base of the runtime note identity (DEV-17).

#### MI-02 Live's note overlap rule

*refinement* · size S · evidence documented · lane win, engine · milestone M2 · after: MI-01

- **Need:** Live's note overlap rule: a new or moved note of the same pitch replaces the start of an existing one or shortens its end, a stretch can reverse order, ids stay stable.
- **Decision now:** ADR-0203 d1, d7 (the note editor works as Live's).
- **On main (audited at ca0290c2):** Not built: notes overlap freely.
- **Proposed contract:** The resolver runs inside applyDiff for notes of the same key, classifying by interval (start inside, end inside, covers, covered) and recording each truncation or deletion as a side effect of the same step; the preview shows them before commit.
- **Reuse:** ME1 (Ardour's resolve_overlaps classifier, intervals, key-only matching).
- **Reject or defer:** Reject Ardour's other merge policies unless Live shows them.
- **Research IDs:** A-M03, A6M-016, M2-A-MIDI
- **Unresolved test (NOT RUN):** Live 12.4: the four overlap cases and which note keeps its id; overlap on different MIDI channels; a stretch that reverses order; the expression of a truncated note.

#### MI-03 The note editor (Clip View)

*decided* · size XL · evidence documented · lane window, director, engine · milestone M2 · after: MI-01, MI-02, HI-01

- **Need:** Live 12's note editor in the Clip View: draw, time and note selection (Enter exchanges), move, stretch, the velocity lane with ramps, Fold, Preview, the magnetic grid; multi-clip editing, Focus and the ghost-note switch (P2); step input (P2).
- **Decision now:** ADR-0203 d7; FEATURES §4 (piano roll P0; layered editing P2; ghost-note switch P2, ADR-0160 d3; step input P2); STEP-7-PLAN §7.7; N32 to N34 merged.
- **On main (audited at ca0290c2):** Engine reads on main: readClipDetails (#388), clip.setStartMarker, setEndMarker, setLoop, setTimeSignature and setScale (#413), note.setMute (#389). Window: nothing on main; origin/window/clip-view (dd81946b) holds the plan, parity-7.7 (37 rows) and the picture with no PR; worklist G1 to G3 wait for the director's approval of the picture.
- **Proposed contract:** Every editor gesture previews an applyDiff in a rolled-back transaction and commits once on release (HI-01); a refused op leaves the notes unchanged and says why; note preview goes through the note preview queue (N44).
- **Reuse:** ME4 (MAGDA's JUCE piano roll mechanics: grid drawing, move and resize, ghosts on copy-drag, rubber band, velocity ramps, CC lane, fold map; design only, not its snap rules).
- **Reject or defer:** Defer the pattern editor (P3); reject Helio's Join, Reverse, Invert and Legato semantics where Live differs.
- **Research IDs:** A-M01, A6M-003, A6M-009, A6M-036, A6M-037, A6M-038, A5W-016, B4-005
- **Unresolved test (NOT RUN):** parity-7.7 rows side by side with Live 12.4; goldens of an empty and a dense clip; A6M-036 to 038 (the multi-clip clipboard and cross-clip drawing).

#### MI-04 Pitch and Time utilities, Split, Chop and Join

*decided* · size M · evidence documented · lane engine, window · milestone M6 · after: MI-01

- **Need:** Live's Pitch and Time utilities and note tools: Split, Chop, Join, Legato, Reverse, Invert, Interval and Add Interval, Stretch, x2 and /2, Set Length, Fit to Time Range; Duplicate, Insert and Delete Time inside a clip; Crop.
- **Decision now:** ADR-0203 d7; FEATURES §4; OPS §9.5 (note.legato P2 and others).
- **On main (audited at ca0290c2):** Only quantize and transpose exist (#408).
- **Proposed contract:** Each utility is a pure function over the selection producing one applyDiff. Split keeps the left note's id and gives the right a new one; expression curves split and join exactly per curve kind; degree-aware transposition reads the clip's scale.
- **Reuse:** ME3 (Helio's SequencerOperations: cutNotes, mergeNotes, shiftInScaleKeyRelative, rescale, invertChord, applyTuplets, the knife tool; under Live's rules; its EDO name data with attribution).
- **Reject or defer:** Reject Helio's own Join, Reverse, Invert and Legato semantics.
- **Research IDs:** A6M-009, A6M-018, A6M-019, A6M-027, A6M-033, A6M-034
- **Unresolved test (NOT RUN):** A6M-009 to A6M-027 and A6M-033/034 as cases; AEXP split exact per curve kind (hold, linear, exp, log, S; bezier within tolerance).

#### MI-05 Quantize, groove, Humanize and Randomize

*decided* · size M · evidence documented · lane win, engine, window · milestone P2 · after: MI-01

- **Need:** Quantize (P0, Live's settings), groove and swing (P0), the Groove Pool (P2), Humanize and Randomize exactly as Live's two controls (P2), record quantize as its own undo step.
- **Decision now:** FEATURES §4 Quantise, groove, swing (P0); §3 Groove pool (P2, no schema; §12 gap 4); ADR-0203 d7, d12 (Rec-Q 1/16); ADR-0132 d8.
- **On main (audited at ca0290c2):** note.quantize (strength, swing, starts and ends, exact integers) on main (#408); no groove table; no Humanize or Randomize.
- **Proposed contract:** The Groove Pool is a project-scoped whole table (ADR-0144 d3) holding Live's groove parameters (Base, Quantize, Timing, Random, Velocity); Commit Groove writes one applyDiff; Humanize uses Live's span once measured, seeded per apply so redo replays it.
- **Reuse:** None in the ledger (ADI's own).
- **Reject or defer:** Reject real-time Note FX quantize (Bitwig).
- **Research IDs:** A6M-022, A6M-024, A6G-013, A-H02, A6M-078
- **Unresolved test (NOT RUN):** A6M-022 (Humanize bound: a quarter or half grid division); A6M-024 (Ctrl+U or Ctrl+Shift+U); .agr import (A6M-078).

#### MI-06 Scale Mode

*decided* · size M · evidence documented · lane win, engine, window · milestone M6 · after: TU-01

- **Need:** Live 12's Scale Mode (P1): root and scale on the clip, in-scale notes highlighted, Fold to scale, devices following it, Fit to Scale, transposing and inverting by degrees, Live's list of scales and its spelling.
- **Decision now:** ADR-0203 d7 (P1); ADR-0103; FEATURES §4 scale row; N33 (clip.setScale merged).
- **On main (audited at ca0290c2):** clip.setScale with inherit on main (#413); ScaleContext and the scale timeline are in open #445 and #442; no scale list ('does not guess a Live preset list').
- **Proposed contract:** Live's scales under Live's names and order, from a Live 12.4 census, each a degree mask over the clip's tuning; Fit to Scale rounds ties to the lower degree (A6M-018); degree operations through ScaleContext; spelling Sharp, Flat or Auto.
- **Reuse:** ME6 (DrivenByMoss's Scale.java names, Pelog and Messiaen modes included, copied as data with attribution; Zrythm's masks as a cross-check), ME3 (degree stepping).
- **Reject or defer:** Defer the .stacks chord rulesets (A-T09).
- **Research IDs:** A6M-018, A6M-019, A6M-031, A6M-032, A-T09, A6X-004
- **Unresolved test (NOT RUN):** Census C6-SCALES / M6-A6M-031 and 032 (Live 12.4's list, order and members).

#### MI-07 MIDI Tools (P2)

*decided* · size L · evidence documented · lane engine, devices, window · milestone P2 · after: MI-01, MI-03

- **Need:** Live 12's MIDI Tools (P2): Transform and Generate with Auto Apply and preview, settings saved with the Set, Arpeggiate sharing the Arpeggiator's ordering.
- **Decision now:** ADR-0203 d7 (P2; closes ADR-0169 d7); FEATURES §4 (pattern editor, Logical Editor and Expression Maps P3).
- **On main (audited at ca0290c2):** Not built; #207 (open) carries the MIDI effects with the arpeggiator's styles.
- **Proposed contract:** A tool is saved settings, a pure transformer or generator and a rolled-back preview committed as one applyDiff; a change of note selection commits a pending preview (A6X-016); seeds are stored so redo replays.
- **Reuse:** ME5 (openDAW's walk_index, 18 modes, with #207's styles: one allocation-free function shared by the device and the tool).
- **Reject or defer:** Reject Bitwig's operators as note properties (B-O01 to B-O05); defer the Logical Editor and Expression Maps (P3).
- **Research IDs:** A-T02, A6X-013, A6X-016, A6X-017, A6M-040, B-O01, C3-030
- **Unresolved test (NOT RUN):** A-T02 / A6X-013 (do generators replace all notes or only overlapping ones, on Live 12.4); M2-A-MIDI (Auto Apply on and off, repeated Apply, Reset, cancel).
- **Cross-check and this plan:** XC-25: one allocation-free ordering function in src/adi/dsp serves the Arpeggiator and Arpeggiate.

#### MI-08 MPE and note expression: playback and editing

*decided* · size L · evidence documented · lane engine, window · milestone M6 · after: MI-01

- **Need:** Per-note expression (pitch, slide, pressure) stored, played and edited as Live 12's MPE tab, moving, stretching and splitting with its notes.
- **Decision now:** FEATURES §4 Per-note expression / MPE (P1); ADR-0054; ADR-0155 (open item: stored AEXP not played); OPS noteExpression.write and clear (P1).
- **On main (audited at ca0290c2):** The AEXP format and note_expression table are on main, and the MPE input and output routes are built (ADR-0097, ADR-0099), but midi_clips.cpp reports 'stored expression curves not rendered'; no noteExpression ops; no UI.
- **Proposed contract:** The clip schedule compiles AEXP curves into expression events on ADR-0054's grid; noteExpression writes ride in applyDiff; the utilities carry expression (MI-04).
- **Reuse:** ME4 (MAGDA's MPE pitch-glide overlay and Alt-drag segment bend, design), ME3 (exact expression split).
- **Reject or defer:** Defer Bitwig's eight-port note bridge (B9G-038) beyond the MPE dimensions.
- **Research IDs:** A-X01, A-X02, A-X03, B9G-038, C3-028
- **Unresolved test (NOT RUN):** A stored pitch curve rendered through Surge XT (MPE) and Serum 2 (note expression) matches the recorded input (ADR-0098's rig).

#### MI-09 Live 12's note model: probability groups and deviation

*refinement* · size M · evidence documented · lane win, engine · milestone P2 · after: MI-01

- **Need:** Live 12's note model: per-note probability (P2) and its groups, velocity deviation and release velocity, played as Live.
- **Decision now:** FEATURES §4 Per-note probability (P2, in the v1 note record); ADR-0155 (probability plays at 100 %); ADR-0144 d3 (whole tables only).
- **On main (audited at ca0290c2):** ANOT v1 holds probability and release velocity; there is no velocity deviation or probability-group membership; the engine plays probability at 100 %.
- **Proposed contract:** A side table (clip, note id, velocity deviation, probability group) in a whole-table minor; playback rolls probability per occurrence with a seed under ADR-0203 d7's random rule, and a group resolves together.
- **Reuse:** None.
- **Reject or defer:** Reject Bitwig's occurrence and recurrence operators (B-O03): not Live.
- **Research IDs:** A-M07, A6M-030, A6X-006, A6P-011
- **Unresolved test (NOT RUN):** A6M-030 (group semantics on Live 12.4); the .als NoteProbabilityGroup fields (A6Z).

#### HI-01 A gesture is previewed, then committed once

*refinement* · size M · evidence inferred · lane win, engine, window · milestone M2 · after: none

- **Need:** A drag, a tap run or a held key is heard while it moves and lands as one undo step; Esc cancels and writes nothing; undo is refused while a gesture is open.
- **Decision now:** ADR-0110 and ADR-0141 (one gesture, one op, for plug-ins); ADR-0148 (rolled-back preview); ADR-0153 (expected head); OPS §6.4 (300 ms coalescing: never built and cannot fire, since every commit takes a new txn_id); the harvest's proposed win ADR W1.
- **On main (audited at ca0290c2):** Plug-in parameters coalesce through ParamOps (ADR-0124, #390). The mixer commits a fader drag on mouse-up only (ui_mixer.cpp), so nothing is heard while dragging; Tap Tempo and the momentary latch coalesce by undo and recommit, leaving 'forked at N' branches (history harvest, verified). No GestureSession.
- **Proposed contract:** GestureSession: begin; each update runs the gesture's absolute ops in an always-rolled-back transaction re-derived from the origin (ProjectView follows, and the Session too when the ops' engine impact is none or snapshot); commit writes one OpJournal transaction with one label and the pre-gesture inverse; cancel writes nothing; undo, redo, moveTo, revert and branch switches are refused while open; another producer's commit re-applies the preview. ParamOps keeps plug-in parameters. This supersedes OPS §6.4.
- **Reuse:** HI1 (ossia score's OngoingCommandDispatcher contract and edge cases), HI2 (FigCraft's preview-from-origin tests), HI6 (SoundCraft: a merged gesture at a full history costs one step), HI7 (openDAW's editing-step rules, design), HI8 (Ardour's drag Esc contract, design).
- **Reject or defer:** Reject undo-based coalescing and OPS §6.4's time window.
- **Research IDs:** A6P-006, A6P-019, A-H02, A6X-016, C5W-017
- **Unresolved test (NOT RUN):** test_gesture: a 20-move drag is one transaction with one label; cancel leaves zero transactions and the digest unchanged; the preview never writes ops; M-LIVE-UNDO (Live 12.4's undo labels per gesture, recorded with the interaction logger).
- **Cross-check and this plan:** XC-17: one gesture ADR with two preview strategies; the quiet window survives only as the end detector for sources that send no release.

#### HI-02 An undo cap that holds

*new-choice* · size L · evidence inferred · lane win, director, engine · milestone M2 · after: none

- **Need:** Active undo capped at 2000 steps or 1000 MB, with a compaction that runs, never fails an edit, and keeps reachable what the director wants kept.
- **Decision now:** ADR-0203 d13; ADR-0140 d5 (compaction keeps snapshot points); SPEC §8.3 (#419); ADR-0111 d4; Dream §12 ('a dead end is still there to come back to').
- **On main (audited at ca0290c2):** #419 merged: compaction runs inside every OpJournal::commit (ops.cpp) with a replayable checkpoint. Compaction never passes the oldest snapshot, branch head or fork point (history_compaction.cpp), so the cap stops holding once 2000 steps pile up after the first Ctrl+S, fork or daily snapshot, and a snapshot at the root (Ctrl+S on a new project) stops compaction for good. Under ADR-0203 d13 only a named snapshot or a reachable branch point may stop compaction, and the oldest automatic snapshot is dropped, never a named one; main's treating the daily snapshot as a stop is therefore a defect inside this row's authority, not part of Q-A. A failed compaction fails the edit, and with it plug-in drains, recording takes and agent applies. Nothing on main calls DailyHistorySnapshots yet. tests/test_history_compaction.cpp passes.
- **Proposed contract:** Within win's authority now: the daily automatic snapshot is compactable as d13 says (oldest dropped first); compaction runs down to a low-water mark (for example 1,800 steps or 900 MB) and stores checkpoint rows by content hash. Placed, to strike (P-10): compaction is best-effort (it reports and never fails the commit). What may be compacted past among named snapshots and branch points is the director's choice D1 (Q-A1).
- **Reuse:** HI3 (gridcraft's group seams and seeded sequence fuzzer: undo-all equals the start digest, compaction never changes the current digest).
- **Reject or defer:** Reject Reason's redo clearing (N10H-002) and REAPER's preference-driven history.
- **Research IDs:** R3-001, R10H-061, N10H-002, A4-010, A5C-048
- **Unresolved test (NOT RUN):** A 3,000-step project with a snapshot on day one stays under the cap; Ctrl+S on a new project does not stop compaction; the E7 fuzz.
- **Cross-check and this plan:** XC-02 and XC-32: the daily snapshot is wired only together with Q-A's rule.

#### HI-03 Live's Undo History window, with branches and snapshots

*decided* · size M · evidence documented · lane engine, window · milestone M2 · after: HI-02

- **Need:** Live's Undo History window (View menu, Ctrl/Cmd+Alt+Z): newest first, undone steps greyed, a click restores a step in one move; ADI's branches and snapshot anchors shown; the project's creation is never undone.
- **Decision now:** ADR-0203 d11 ('the history window opens and behaves as Live's Undo History, with ADI's branches and snapshots as additions'), d13; ADR-0128 d6; N29 (partly merged); window worklist A5.
- **On main (audited at ca0290c2):** History::readTimeline (#387) and switchToBranch are on main. There is no History::moveTo, so restoring the Nth step costs N transactions and N refreshes and cannot cross to another branch in one move; readTimeline lists the compaction root as an ordinary step (N29's open part); there is no window.
- **Proposed contract:** History::moveTo(seq) rewinds to the common ancestor and replays forward in one transaction (ADR-0140 d3's mechanism); the timeline leaves out the compaction root and says older steps were compacted (ADR-0111 d4); the window lists readTimeline's rows; a branch tree view is the addition d11 names.
- **Reuse:** HI4 (Audacity's HistoryPanelModel list model, design), HI5 (Helio's RevisionTreeComponent: Buchheim layout, connectors, head mark, checkout; a JUCE port).
- **Reject or defer:** Defer a space-used column (an addition nobody asked for).
- **Research IDs:** A4-010, A5C-048, R3-001, R10H-061
- **Unresolved test (NOT RUN):** Model tests: rows after commit, undo, redo and fork; restoring the third row is two undos refreshed once; parity-history rows; M-LIVE-UNDO labels.

#### HI-04 Snapshots, revert, the historical tab and Save As

*decided* · size M · evidence documented · lane window, engine · milestone P2 · after: HI-02

- **Need:** Ctrl+S Take Snapshot with a pre-filled name, the daily automatic snapshot, revert keeping the left work as a branch, a historical state opened in a silent read-only tab with paste back as one undo (P2), and a true Save As and Save a Copy.
- **Decision now:** ADR-0128, ADR-0140, ADR-0111, ADR-0203 d6, d13; N3 and N30 (partly merged); window worklist A1, A4.
- **On main (audited at ca0290c2):** Snapshots and revert (history_snapshots) and DailyHistorySnapshots (#410, not wired) are on main, as are Store::saveAs and saveCopy (#380); main's macOS legs fail adi_project_files_tests (outside this area's code, but A1 and A4 call those paths); no historical tab.
- **Proposed contract:** As decided. The daily snapshot is wired only together with D1's rule, otherwise compaction stops after the first finished day. The historical tab replays to the node into a temporary read-only file with no plug-ins, and a paste back is one transaction (ADR-0111 d1 to d3).
- **Reuse:** HI2 (FigCraft's version-history tests: naming pins, named never evicted, viewing read-only, versions survive Save As).
- **Reject or defer:** Defer automatic snapshots on export or open (ADR-0128: none until asked).
- **Research IDs:** A4-010, A6H-068, R3-001
- **Unresolved test (NOT RUN):** AWAITING row 13 (which file is open after Save As and Save a Copy); HI2's eight version rows.
- **Cross-check and this plan:** XC-02: Save As and Save a Copy are PD-01's; XC-23: one tab host, owned by PD-15.

#### RE-01 Remarks shown in the window

*decided* · size M · evidence inferred · lane window · milestone M1 · after: none

- **Need:** Remarks visible in the window: a corner pip on tracks, clips, devices and parameters that have one (an agent's marked), the Info View split on hover, and add, edit, resolve and remove.
- **Decision now:** ADR-0131 d1 to d5; ADR-0139; ADR-0203 d6 (the remark marks stay); N27 (merged); window worklist C5.
- **On main (audited at ca0290c2):** Engine built on main: the remarks table, remark.add, edit, resolve and remove (exact undo, text projection, check) and Snapshot::remarks with trackId and anchorMissing (#400); tests/test_remarks.cpp and test_snapshot_remarks.cpp pass. The window shows nothing.
- **Proposed contract:** As ADR-0131: pips only for visible objects, from the snapshot; agent remarks in their own colour and glyph; the Info View shows the documentation above and the remark below.
- **Reuse:** RM5 (figcraft's pin clustering and author colour, mapped onto Live's 70 palette indices; port), RM8 (typing a remark is one undo step, design).
- **Reject or defer:** Reject margin balloons in the Arrangement.
- **Research IDs:** DR-12-09, DR-12-10
- **Unresolved test (NOT RUN):** Window tests: the hover split, pips within 8 px merging into a count; the director's first look at the picture (an addition).

#### RE-02 Remarks as Word's comments: threads and anchors (P2)

*decided* · size L · evidence inferred · lane win, engine, window · milestone P2 · after: RE-01, ED-03

- **Need:** Remarks as Word's comments (P2): on any object, note, marker or time range and on ops; flat threads of replies; resolve; a Remarks panel that lists and jumps; authors shown; rules for split, copy, delete and time edits; the agent reads remarks as context and confirms any action it proposes from one.
- **Decision now:** ADR-0203 d17; ADR-0131 open items (threads); ADR-0139 open item (lanes and markers); ADR-0188 d5 (remark_anchors); FEATURES §14 remarks row (P2).
- **On main (audited at ca0290c2):** Not built: the remarks CHECK allows track, clip and device only; no replies, no anchors; eraseRange re-creates a clip's tail under a new id, so its remarks would not follow; OPS §9.12 lists the remark ops at P2.
- **Proposed contract:** A schema minor of whole tables (remark_replies, remark_anchors, plain REFERENCES). Word's rules literally: a remark follows its object's id; on Split it stays on the left piece unless anchored inside the right; Duplicate and Paste copy threads; deleting the object deletes its thread, captured for undo; Delete Time treats a range remark as Word treats deleted text; every rule sits in the edit's single transaction and describe() counts it. The agent's brief is bounded (256 KiB, 200 threads, 4,096 characters per message, single-line fields, control and bidi characters escaped) and propose-only.
- **Reuse:** RM1 (wordcraft's thread closure, reply rule, listing and navigation, with tests), RM2 (Audacity's six-way time-edit relation table), RM3 (figcraft's panel filter, sort and query and the bounded agent brief; not its re-pin rule), RM4 (balloon cards, design), RM6 (OpenTimelineIO's content-relative ranges, design), RM8, RM9 (per-endpoint side, design).
- **Reject or defer:** Reject the harvest's re-pin and collapse rules (R4, R6): they change the Word behaviour the director named. Reject nested threads.
- **Research IDs:** DR-12-09, DR-12-10, C3-030, R10H-029
- **Unresolved test (NOT RUN):** The remarks harvest's T1 and T4 to T11 adapted to Word's delete rule; L-INFO-1 to 5 (Live info text under split and duplicate); a Hebrew remark reads from the right.

#### RE-03 Live's Edit Info Text

*refinement* · size S · evidence documented · lane win, engine, window · milestone M6 · after: none

- **Need:** Live's Edit Info Text: a text property of tracks, clips and devices shown in the Info View on hover, copied with its object and kept in presets; a Live feature separate from remarks.
- **Decision now:** ADR-0203 d1; no FEATURES or parity row; marker_details.note already holds a locator's note.
- **On main (audited at ca0290c2):** Not built; no home in the format.
- **Proposed contract:** A property of its object in a side table (kind, id, text) with one op (info.setText), copied and duplicated with its object, exported as DAWproject's Nameable comment; the Info View shows it as Live and the remarks below it; the two are never merged.
- **Reuse:** RM7 (DAWproject Nameable.comment mapping, design).
- **Reject or defer:** Reject storing info text as a remark.
- **Research IDs:** DR-12-09
- **Unresolved test (NOT RUN):** L-INFO measurements on Live 12.4: which objects have Edit Info Text, what copying keeps, what presets keep.

#### TU-01 Land the tuning stack

*decided* · size S · evidence documented · lane win, engine · milestone M6 (its merges are M0's; the gate closes beside TU-02) · after: none

- **Need:** Tunings that can be created and heard: a tuning per key change on the timeline, Scala .scl import, tuning and key-event ops, ScaleContext.
- **Decision now:** ADR-0178, ADR-0103, ADR-0117 §3, ADR-0203 d7 (P1); FEATURES §4 scale row.
- **On main (audited at ca0290c2):** Schema 1.8's tables and per-note tuning delivery (pitch expression to CLAP, VST3 and MPE, ADR-0155 d4) are on main; no op creates a tuning on main. Open: #442 (scale timeline), #443 (Scala import), #444 (tuning ops), #445 (ScaleContext). On that stack (tuning harvest): 31-EDO plays 9.677 cents flat for want of a reference pitch, and #443 refuses valid sources (negative cents, non-monotonic order, a zero count, sub-unison ratios such as 10/20, text after a value).
- **Proposed contract:** Merge #445 (rebased; retargeted to main after #421), then its siblings #444, #443 and #442, each retargeted to main in that order (they are siblings on (its source file), not a chain); before the merge, a guard on #443's branch refuses any source whose reference pitch (an ASCL REFERENCE_PITCH that moves a degree off its 12-TET anchor) the stack cannot yet honour, with a verdict naming the line, so no tuning on main plays flat; OPS.md moves the six tuning and key ops from P2 to P1 (d7); refusals carry a verdict naming the physical line, and TU-02 replaces them with acceptance and the stored reference.
- **Reuse:** TU1 (Surge's tuning-library as the test oracle; compileKeyTable written fresh).
- **Reject or defer:** Reject vendoring tuning-library (ME2 superseded). Defer the visible key and scale track (FEATURES §1, P2; Dream §5: 'comes later'); key events exist as ops without it.
- **Research IDs:** A3-024, A6F-008, A6F-009, A6F-010, A6F-011, A6M-084
- **Unresolved test (NOT RUN):** The tuning6 fixtures give the proposed verdicts; 31-EDO degree 0 at 263.0921 Hz (the merged stack gives 9.677 cents flat, so this gate cannot close in M0; it closes at M6 with TU-02's reference store; until then the guard refuses the source).
- **Cross-check and this plan:** The first draft closed this gate in M0 against known-failing code; this revision keeps the merges in M0 and moves the gate to M6 (the critique of 5 November).

#### TU-02 Source fidelity: what ADI accepts and re-exports

*refinement* · size M · evidence documented · lane win, engine · milestone M6 · after: TU-01

- **Need:** Every source Live accepts is accepted and re-exported exactly: negative cents and non-monotonic order, the ASCL @ABL layer (NOTE_NAMES, REFERENCE_PITCH, NOTE_RANGE_BY_INDEX and BY_FREQUENCY, SOURCE, LINK), unknown directives kept, UTF-8 rules, .kbm (ADR-0103's addition), byte-exact re-export.
- **Decision now:** ADR-0178 d2 (rising, non-negative degrees), d5; ADR-0103 d1 (.kbm); ADR-0203 d1 (Live's sources); ADR-0144 d3.
- **On main (audited at ca0290c2):** Not built: no ASCL, KBM, reference pitch, note range or export.
- **Proposed contract:** ADR-0178's tables stay as they are (unique rising pitches within the period); source order and duplicates live in a keyboard map (key_map_keyboards, key_map_keyboard_slots) and the original bytes in tuning_sources, whole tables in minor 1.20; unknown @ABL lines are kept verbatim with a warning; .ascl and @ABL require UTF-8, plain .scl is decoded from Latin-1; an unedited import re-exports byte for byte. ~~The one visible difference from Live: ADI's ruler stays in pitch order.~~ *[Superseded (XC-28): the ruler follows Live's index order; pitch order only as an addition if he asks.]*
- **Reuse:** TU1 (KBM semantics, parser shapes and fixtures, without the Ableton-authored files), TU4 (scale-workshop's .ascl, .scl and .kbm writers and MTS byte maths, ported with ADI's own checksum), TU7 (Helio's multichannel keyboard layouts, design).
- **Reject or defer:** Reject tuning-library's centre-on-unmapped path (three bugs) and any refusal of a valid source.
- **Research IDs:** A6F-001, A6F-004, A6F-005, A6F-006, A6F-007, A6F-009, A6F-010, A6F-012, A6F-013, A6F-014
- **Unresolved test (NOT RUN):** M6-A6F-004 and 009 (does Live 12.4 accept non-monotonic and negative sources); M6-A6F-007 and 010 (unknown directives, encoding); byte-exact round trips.
- **Cross-check and this plan:** XC-28: the ruler follows Live's index order (the matrix's "pitch order" line is withdrawn); XC-37: its minor is numbered at PR time.

#### TU-03 What a tuning change does; Bypass Tuning; the ruler

*refinement* · size M · evidence documented · lane win, engine, window · milestone M6 · after: TU-02

- **Need:** What a tuning change does to notes as Live (a note is an index; the pitch changes unless the user retunes), per-track Bypass Tuning and Drum Rack's automatic bypass, the note ruler showing the tuning's notes with Hz on hover, degree names (E half-flat), English names and middle C as C3.
- **Decision now:** ADR-0178 d3 (notes unchanged) and its open items (Bypass Tuning, Retune Set On Loading, how the agent names a microtonal note); ADR-0203 d7 (English, C3); AWAITING row 15.
- **On main (audited at ca0290c2):** Not built; notes store key and tuning_cents (absolute).
- **Proposed contract:** Live's model is the default (d1): changing a tuning or a key event keeps note indices, and Live's automatic retuning (a confirmed edit that re-snaps pitches through the keyboard map, collisions previewed) is one undo step. Bypass Tuning per track as Live. The ruler takes names from NOTE_NAMES or the EDO name data. Scale and tuning together on one clip is ADI's addition (A6M-087), designed with Scale Mode.
- **Reuse:** ME3 (Helio's EDO name data, with attribution), TU7 (keyboard layouts, design).
- **Reject or defer:** ~~Reject Live's index-ordered piano roll display (ADI shows pitch order; a recorded difference).~~ *[Superseded (XC-28): Live's index order is the default.]*
- **Research IDs:** A3-024, A3-025, A6M-084, A6M-086, A6M-087, A6M-090, A6M-092
- **Unresolved test (NOT RUN):** AWAITING row 15 (a tuning loaded and shown in the ruler, Live 12); M6-A6M-084 to 092; a tuning change mid-song with a tuning per key change.
- **Cross-check and this plan:** XC-28: reverses the matrix's "reject Live's index order" line.

#### TU-04 Tunings for plug-ins beyond per-note pitch

*new-choice* · size M · evidence documented · lane engine · milestone P2 · after: TU-01

- **Need:** ADI's tunings reach instruments that cannot follow per-note pitch expression.
- **Decision now:** ADR-0155 d4 (tuning as pitch expression); ADR-0097, ADR-0099 (routes); ADR-0134 d7 (route remembered per plug-in); no ADR on MTS-ESP.
- **On main (audited at ca0290c2):** Pitch-expression delivery is built (CLAP note expression, VST3 note expression, MPE). No MTS-ESP, no MTS SysEx, no use of CLAP's tuning extension.
- **Proposed contract:** The director's choice D2. Recommended: an MTS-ESP master as a per-plug-in route beside CLAP expression, MPE and plain MIDI, off by default, talking to the MTS-ESP library already on the machine; CLAP's tuning extension for CLAP instruments; MTS SysEx for hardware later.
- **Reuse:** TU2 (ODDSound's MTS-ESP master wrapper sources, lazy load, injectable function table), TU3 (mts-dylib-reference as the CI fake, test only), TU5 (Woyten/tune's encoder expectations), TU6 (Ardour's scl_to_mts, design), TU8 (CLAP tuning draft, in tree).
- **Reject or defer:** Reject any libMTS binary in ADI (not usable) and one machine-wide table applied to every plug-in (it detunes non-clients).
- **Research IDs:** A8P-018, A8P-019, B9M-069, A6F-014
- **Unresolved test (NOT RUN):** Surge XT follows 24-EDO with zero residual (key 69 within 0.01 cents); a second master is refused with a status; per-track Bypass Tuning cannot reach MTS clients (recorded).

### A.2 Session, clips, time and media (32 rows; source the time-media matrix (win's working file))

#### TM-SV-01 Global Quantization and its Arrangement uses

*refinement* · size M · evidence documented · lane engine, window, win · milestone M3 · after: none

- **Need and current decision:** Live's control-bar Global Quantization (None to 8 Bars; Ctrl+6/7/8/9/0 for 1/16, 1/8, 1/4, 1 Bar, None) times scrub-click jumps and the scrub-hold loop, locator jumps and Set Locator during playback, Clip View Set Start/End/Loop and Nudge, and is Link's quantum. ADR-0203 d1, d6, d10 (only the launch settings arrive with Session View); ADR-0133 d4 (Play-Q); ADR-0185 d7. FEATURES has no row.
- **On main (audited at ca0290c2):** Not built. (its source file) (67 lines; a 960-PPQ grid anchored at sample 0, not bar-aware) passes `adi_play_quantize_tests` at `ca0290c2`. `transport_settings` holds loop and metronome in `session_state`. The menu is a stub: "The global launch quantization (with Session View)" (`ui_controlbar.cpp:176`); `ui_arrangement.cpp:218`: "nothing is quantized yet".
- **Proposed contract:** `transport.launchQuantization` lives in `session_state`, outside undo, as Live's Set code list 0..13 (None, 8 Bars ... 1/32); default 1 Bar until the fresh-install reading. An ephemeral Scope::Transport op `transport.setLaunchQuantization` writes it. A pure, allocation-free `launch_quantize::nextGridTick(nowTicks, code, meters)` in ADI_PPQ is bar-aware from `time_signature_map`: whole-bar grids count from the last signature change, sub-bar values restart at bar lines, and both rules sit behind named `GridRules` until N4 is measured. It resolves on the driver against the rendered generation. Ticks become samples by the clip-placement rule, one rule for clips, metronome, Play-Q and launches. A quantized locate executes at the grid sample and chases MIDI. With Link on, the quantum is this setting. Window: the menu live, Ctrl+6..0, quantized scrub and locators. A FEATURES row in §1 at P1.
- **Reuse:** SV2 (Tracktion head `a5e4499` launch-quantisation table and its tests, restated against Live's codes); SV5 (libossia `token_request.hpp` grid rules, about 80 lines); SV6 (AbletonOSC code lists, design); ADI's own `play_quantize` as the arithmetic base.
- **Reject or defer:** Reject MAGDA's "strictly future" rule and its launch-while-stopped that ignores quantization; reject storing ticks (1 Bar is not a fixed tick count in 3/4). Defer per-clip launch quantization and the Record, Warp & Launch rows to the P2 Session work (d10).
- **Research IDs:** A6G-013, A6M-004, A6C-006, A6C-007, A6C-023, A6C-042, A6P-011; M-LIVE-CLIP (launch half); M3-CONTROLLER 6; manual 6.3-6.4, 8.2.1, 16.4, 36.1.
- **Unresolved test (NOT RUN):** N1 (a press on, or a few ms after, a grid line: now or the next line?); N4 (1/4, 1/8, 1/8T in 6/8 and 7/8: note value or fraction of a bar?); the installed 12.4 chooser against A6P-011's codes. ADI: `test_launch_quantize` (Tracktion matrices in ADI_PPQ, meter changes, block sizes 32-4096 invariant, zero allocations), a quantized-locate test.

#### TM-SV-02 Session data: scenes, slots and launch settings

*refinement* · size L · evidence documented · lane win, engine · milestone P2 · after: none

- **Need and current decision:** Live's Session shape in the file: a scene list (name, colour index, tempo and signature with separate enable flags, follow actions), one slot per track per scene with its Stop button, launch settings per clip (mode, quantization, legato, velocity amount, follow actions, clip Fade, RAM mode), and the fourteen ops back. ADR-0101 d2, d4; ADR-0117 §1-§2; ADR-0203 d10; ADR-0144 d3; ADR-0211; ADR-0021, ADR-0027.
- **On main (audited at ca0290c2):** Not built anywhere; no branch exists. Schema 1.19 (62 tables) has no scenes or slots; check 5e still requires every clip placed; SPEC §6.5 says "There is no Session View"; `clip_playback.cpp` and `midi_clips.cpp` place clips with `value_or(0)`, so a slot clip would play at bar 1.
- **Proposed contract:** Schema minor 1.20 of whole new tables (ADR-0144 d3): `scenes`; `clip_slots(track, scene, clip UNIQUE, has_stop default 1, UNIQUE(track, scene))`, sparse, so no row means an empty slot with a Stop button; `clip_launch(clip PK, launch_mode 0-3, launch_quant 0-14 with 0 = Global, legato, velocity_amount, the follow fields, clip_fade, ram_mode)` in Live's code lists. A slotted clip keeps a zero origin and no lane; "never both" holds by construction and triggers. An `essential` extensions row (`org.adidaw.session`, `min_reader` 1.20) makes a 1.19 build warn instead of playing slot clips at 1.1.1 (SPEC principle 3, §9.1). `rows::Model::slotClips` stays apart from timeline clips, and the 24 direct `clips` readers in 17 files on main (25 in 18 once #446's capture lands) are audited. *[XC-21: they move once, in CT-01's first PR, to one accessor that this row extends]* Edit ops: (its source file), (its source file). Ephemeral ops with literal ids and an incarnation: (its source file). Code names are `Launcher`/`launch_*` (`engine::Session` is taken). This amends ADR-0117 §2's wording; its intent stands.
- **Reuse:** SV6 (AbletonOSC code lists, design); the side-table precedent of minors 1.12, 1.13 and 1.16; DAWproject's Scene and ClipSlot shape later through IC2.
- **Reject or defer:** Reject option B (rebuild `clips` with a relaxed CHECK: foreign keys off outside the migration transaction, nine child tables) unless win wants it under its own ADR. Defer scene-tempo application (TM-SV-04) and the DAWproject scene mapping (TM-IC-03).
- **Research IDs:** A3-032, A6S-007, A6P-011, A6P-012, A5C-037, A5C-038, DR-01-02; M3-LAUNCH L4.
- **Unresolved test (NOT RUN):** ADI: `test_migrate` 1.19 to 1.20, check 9, a 1.19 build warns on the essential row, and a project with slot clips renders, freezes and exports bit-identically to the same project without them. Live: the default of a slot's Stop button and the dormant scene values on the installed build (A6S-007 is static `.als` evidence only).
- **Cross-check and this plan:** XC-21: extends CT-01's one accessor instead of auditing the clips readers again; XC-37: the minor number is given at PR time; XC-41: P2 by priority, no gate.

#### TM-SV-03 The launch engine

*decided* · size L · evidence documented · lane engine · milestone P2 · after: TM-SV-01, TM-SV-02

- **Need and current decision:** Live's launching: Trigger, Gate, Toggle and Repeat; quantized launch and stop; Legato taking over the play position; velocity amount; one clip per track with the Arrangement silenced until Back to Arrangement; Track Status; RAM mode; Clip Update Rate; clip Fade; no stuck or retriggered notes when the graph rebuilds. ADR-0101 d4; ADR-0117 §2; ADR-0203 d1, d10; ADR-0042 d2-d4; ADR-0151 d3; ADR-0155 d3; ADR-0210 d3.
- **On main (audited at ca0290c2):** Not built. Primitives: `TransportMailbox` (16-slot UI-to-driver ring), `TimingView::at` bar math, per-track sources with note ownership and chase (ADR-0155), and Live 11.2.7's measured clip-edge declick table (`collab/mac/live-probe`). Conflict 19 (rebuild continuity) is open.
- **Proposed contract:** An engine-owned launch lane (fixed ring, all-or-nothing gestures, incarnation stamps) drained on the driver in the block it arrives in. Per-block order: drain, due follow actions, advance handles, sections, taps, run edges. Quantization resolves at drain time on the rendered generation, inside `TimingView::beginBlock` once #454 merges. A pure `onEdge(phase, mode, edge)` press/release table with Live's mapping (Trigger relaunches on a second press, Ardour's ReTrigger). Sections: per track one Arrangement span per block; Arrangement sources release owned notes at its edges. Legato joins the new run to the old position (N11 covers unequal loop lengths). Declick: launch, stop, retrigger and hand-over edges follow Live's per-clip Fade (length and default from N8; until then the measured 176/192/384 samples) through block-invariant, transient-preserving ramps; Arrangement edges keep ADR-0151 d3. One tick-to-sample rule shared with capture. Voice identity survives a rebuild that leaves a clip's events unchanged. Zero callback allocations. Every Live assumption sits behind a named constant with an AWAITING row.
- **Reuse:** SV1 (MAGDA head `ddbc114` launch engine with its 185 launch, section and capture tests, ported onto ADI_PPQ, samples and `SnapshotPublisher`); SV2 (Tracktion head ClipLauncher tests, 65 cases, restated against Live); SV4 (Ardour TriggerBox press/release table as a pure function).
- **Reject or defer:** Reject Tracktion's runtime launcher (10 Hz ownership timer; `std::function` and `juce::Random` on the audio thread), MAGDA's host and UI (Trigger/Toggle only, forced loops), openDAW's stop that returns the track to its timeline (the opposite of A5C-040). Defer warped Session clips under a changing tempo to TM-WT-04 (constant-tempo preparation first, through #455's mechanism) and Session Record into slots.
- **Research IDs:** A3-001, A3-002, A5C-039, A5C-040, A6C-018, A6C-019, A6C-028, A6C-041, A6C-048, A6P-012; M3-LAUNCH L1, L2; M-LIVE-CLIP; N2, N3, N5, N8, N10, N11, N12.
- **Unresolved test (NOT RUN):** M3-LAUNCH L1, L2; the launch half of M-LIVE-CLIP; N2, N3, N5, N8, N10, N11, N12 on Live 12.4. ADI: MAGDA's `test_launch_handle` (29), `test_launch_requests` (22), `test_launch_tap` (10) and `test_session_playback` (78) restated in ticks and samples, blocks 32-4096, 44.1-192 kHz.
- **Cross-check and this plan:** XC-25: note identity follows DEV-17's scheme, so rebuilds keep held notes.

#### TM-SV-04 Follow actions, scenes and capture

*decided* · size L · evidence documented · lane engine · milestone P2 · after: TM-SV-03, TM-LS-01, TM-TT-01

- **Need and current decision:** Live's Follow Actions (groups, A and B with Chance, Linked or Unlinked time, ten actions including Jump, a global enable); scene launch with Stop buttons and Cancel Scene Launch; scene tempo and signature applied at launch, dormant values kept; Capture and Insert Scene; a Session performance recorded into the Arrangement. ADR-0101; ADR-0117 §2; ADR-0203 d1, d10; ADR-0162; ADR-0021.
- **On main (audited at ca0290c2):** Not built. A runtime tempo path exists only in open PR #455 (a constant-tempo generation) and in the local WIP of (its source file) (`setExternalTempoAt`).
- **Proposed contract:** A follow-action group is a run of occupied slots split by empty ones; a deterministic target resolver carries Live's ten actions, written fresh; ~~chance comes from a stateless seeded hash, so two renders draw the same~~ *[Superseded (XC-29): a fresh draw on every pass, seeded within one pass so a realtime pass and its render agree]*; a user launch outranks a due follow action; A3-004 (follow against Global quantization) and A3-005 (scene against clip precedence) sit behind named constants until L3 measures them. A scene launch is one gesture of leader and followers that honours `has_stop`. Scene tempo and signature apply at the quantized launch through the Link consumer's performed-boundary tempo (`setExternalTempoAt`), never a second tempo path; whether that is undoable follows N9. `capture.sessionPerformance` is one changeset of literal `clip.create` ops, one undo step, replaying sample-identically.
- **Reuse:** SV3 (Tracktion head follow-action group resolver and its 8-slot fixture); SV1 (MAGDA SessionCapture with `test_follow_actions` 20, `test_session_capture` 24 and `test_session_arrangement_capture` 13); SV6.
- **Reject or defer:** Reject a stateful random generator on the audio thread. Defer the crossfader (TM-SV-05) and the DAWproject scene export, which reports the loss of launch settings (TM-IC-03).
- **Research IDs:** A3-003, A3-004, A3-005, A6C-043, A6C-044, A6C-045, A6C-046, A6C-047, A6C-049, A5C-036, A5C-041, A6S-007, A4-017; M3-LAUNCH L3, L4, L6; N6, N7, N9.
- **Unresolved test (NOT RUN):** M3-LAUNCH L3 (follow under None, Global and an explicit quantization, with a scene action at the same time), L4 (moving or deleting a queued target), L6 (Session to Arrangement recording, undo, reopen); N6 (Previous at a group's top, Other alone, the Chance coupling of A6C-046); N7; N9.
- **Cross-check and this plan:** XC-29: replaces the stateless-hash chance with Live's per-pass draw; a keep switch would be an addition and is not proposed.

#### TM-SV-05 The Session View window

*decided* · size XL · evidence documented · lane window, engine, win · milestone P2 · after: TM-SV-02, TM-SV-03, TM-SV-04

- **Need and current decision:** The Session View docked as Live's (F3, Tab) and detachable to a second display; slots, scene launch and stop buttons; the Main track's scene column with tempo and signature; Track Status; the Clip View launch panel; Session Record; Back to Arrangement; Follow Actions Globally; Live's keys; the crossfader at P2 with it; the MixConsole toggle at P3, last. ADR-0117 §1; ADR-0203 d6, d10; ADR-0101 d3; ADR-0063; ADR-0108 d1; ADR-0210 d3; ADR-0211.
- **On main (audited at ca0290c2):** Not built. Placeholders: Session Record in the control bar (`ui_controlbar.cpp:189`), `ui_shell.cpp:911` and `:1865`, `ui_mixer.cpp:1186`; the Session settings rows are BACKLOG ADR-0101 (`catalogue.cpp:392-403`).
- **Proposed contract:** The window builds the grid against a stub provider as soon as TM-SV-02's schema lands (ADR-0210 d3), reading the per-slot taps and run edges, never the store; a parity checklist from manual chapters 7 and 16; the director's signed side-by-side session (ADR-0108). The crossfader needs a master-section table (FEATURES marks it 🔶), its own minor.
- **Reuse:** MAGDA's `SessionView.cpp` for layout ideas only (behaviour from Live, ADR-0108).
- **Reject or defer:** Defer the MixConsole toggle (P3, last). Nothing starts before the first-window gate (LANES §5).
- **Research IDs:** DR-01-02, A5C-038, A5C-039, A5W-003, A5W-014, A6R-012, B4-20 (the crossfader).
- **Unresolved test (NOT RUN):** The parity checklist side by side with Live 12.4; M5-A5C-039 (exact Track Status text and icons).
- **Cross-check and this plan:** XC-10: the crossfader row is MX-15; XC-41: no first-window wait, P2 by priority.

#### TM-WT-01 The analysis core behind Auto-Warp

*refinement* · size M · evidence inferred · lane engine, win · milestone M6 · after: TM-MD-04 (#449 merged)

- **Need and current decision:** Pressing Warp runs Live's Auto-Warp: tempo with changes, transients, 1.1.1 on the first clear downbeat, a marker on each bar; an inline BPM field when no clear beat is found; one analysis per file, cached, never re-run on apply, undo or redo. ADR-0132 d7; ADR-0203 d7 (tempo detection P0), d12, d18; ADR-0176 d4; ADR-0188 d2; ADR-0205 d10; ADR-0145 d10 (Essentia for ADiJ).
- **On main (audited at ca0290c2):** Partly built (landed through #430): `auto_warp.cpp` (134 lines: an inter-onset vote over 10-999 BPM, a greedy tempo-change path, no downbeat; with From First Beat the first onset is trimmed away); `transients.cpp` (51 lines, a 5 ms RMS energy-rise placeholder); `warp_commands.cpp` `prepareSetWarp` (clean, full-source clips only). The warp tests pass at `ca0290c2` on synthetic clicks and impulses only. The analysed cache is open PR #449.
- **Proposed contract:** An ADR, "the warp analysis stack". Onsets from spectral flux on log bands, with strength and peak per onset, reading `editing.transientSensitivity` instead of the literal 50 four callers pass today. Beats, downbeat and tempo from qm-dsp at a fixed 44.1 kHz analysis rate, with Mixxx's `TempoTrackV2` (it works from about 1.5 s of audio). Constant-tempo regions and BPM snapping from BeatUtils. Results stored as versioned ADIA sections in #449's analysed cache, the configuration id carrying the analyser's parameters. Every edit captures literal AWRP bytes. It amends ADR-0176 d4. A benchmark gates it: a labelled local corpus (F-measure, CMLt and AMLt, Acc1 and Acc2, downbeat F, onset F at ±10 ms) with beat_this as the comparator; it replaces today's heuristic only if it beats it. Which tracker the whole suite uses is CH-TM-2.
- **Reuse:** WT1 (qm-dsp at tag `qm-vamp-plugins-v1.8.0` `4d2a4a4e`, with Mixxx's `TempoTrackV2` fork, vendored as `adi_qmdsp` with its kissfft); WT2 (SoundCraft `detect_transients`, about 200 lines, and its six tests); WT3 (Mixxx BeatUtils); WT8 (beat_this as comparator in `tools/warp_bench`); WT7 (Essentia, design and comparator only, unless CH-TM-2 picks it).
- **Reject or defer:** Reject upstream `TempoTrackV2` (it needs about 7.4 s), qm-dsp (its source file) ("used with permission"), PeakPicking and Polyfit, a RealFft shim for qm's tests, a second `AANL` cache format, and the re-detection inside `prepareWarp`. Defer Audio Alignment (TM-WT-08).
- **Research IDs:** A-W01, A-W04, A6C-032, C3-001, C3-002, C3-003, C3-004, C3-005, R11E-027, R11E-028, R11E-029, DR-04-08, DR-04-09; M2-A-WARP 1, 6.
- **Unresolved test (NOT RUN):** The benchmark gate (no corpus assembled yet); M2-A-WARP 1 and 6 on Live 12.4; ADI: 48 and 96 kHz give identical results, and 1.1.1 lands on the first downbeat after a pickup.
- **Cross-check and this plan:** XC-19: per-file analyses are computed once into ADIA and read by AGT-07; Q-D picks one analyser for the suite.

#### TM-WT-02 Warp ops, per-clip warp settings and Live's warp commands

*refinement* · size L · evidence documented · lane win, engine · milestone M6 · after: TM-WT-01

- **Need and current decision:** Every Live warp command as one undo step: insert, move and delete markers; pseudo-marker promotion, with Ctrl taking the adjacent transients; Shift-drag of the waveform; Warp From Here in four variants; Set 1.1.1 Here with a pickup; Warp Sample As and Warp Selection As; audio Quantize with Amount; x2, /2 and Seg. BPM; multi-clip warp; per-clip mode settings and user transients kept; the rightmost-marker rule. ADR-0061; ADR-0132 d7; ADR-0203 d6 (Ctrl+I), d7; OPS §9.4; ADR-0144 d3; ADR-0021.
- **On main (audited at ca0290c2):** Not built. The AWRP v1 codec (`warp_map.cpp`), the warp columns since 1.0 and `clip_warp_fade_domains` (1.13) exist; none of OPS §9.4's `audioClip.*` ops is registered in `ops_catalog.cpp`; no mode parameter is stored (Tones grain is hard-coded 0.03 at `clip_playback.cpp:444`; Beats and Texture use plan defaults; no seed).
- **Proposed contract:** AWRP stays the one source-frame-to-tick map that every producer shares. A format minor of whole tables: `clip_warp_settings` (Beats Preserve, loop mode and envelope; Tones grain; Texture grain and fluctuation; Complex Pro formants and envelope; a seed; NULL means Live's measured default) and `clip_transients` (user-inserted and user-removed onsets as `ATRN` streams, apart from the cached detections). A clip origin for Set 1.1.1 Here with a pickup goes in a side table or AWRP v2 with signed ticks; win picks one. OPS §9.4's six ops are registered with captured inverses (drags coalesce), plus `audioClip.setWarpSettings`, `audioClip.setTransients` and `clip.setOrigin`. Each Live command is a prepared producer emitting those ops: one undo step, captured bytes on replay, never a new analysis. `prepareSetWarp`'s clean-clip refusal goes (Live warps any clip). Reset, remove and commit stay three different things.
- **Reuse:** WT6 (Tracktion `WarpTimeManager` marker-edit rules, design only, with clamps taken from Live); ADI's AWRP codec and `warp_projection` on main.
- **Reject or defer:** Reject Tracktion's code itself. Defer tempo leaders (TM-WT-07) and the streaming players (TM-WT-04).
- **Research IDs:** A-W02, A-W03, A-W04, A-W06, A-W07, A6C-026, A6C-034, A6C-035, A6C-036, A6C-037, A6C-038, A6C-086, A6P-008, R11E-033, R11E-034, R11E-036, C5V-013, DR-04-01, DR-04-02; M2-A-WARP 1-5, 7.
- **Unresolved test (NOT RUN):** M2-A-WARP 1-5 and 7; M6-A6C-034, -037, -038; the rightmost-marker rule (A-W04) and the marker ratio limits on Live 12.4.

#### TM-WT-03 Beats and Texture parity and the listening gate

*decided* · size M · evidence documented · lane engine, devices · milestone M6 · after: TM-WT-02

- **Need and current decision:** Each mode sounds as Live's: Beats Preserve by grid division, its loop point at the zero crossing nearest the segment middle, no click at slice switches, better than linear interpolation; Texture grain, fluctuation and seed from the clip; a listening gate per mode. ADR-0188 d2; ADR-0061; ADR-0108; FEATURES §10.5's per-mode table.
- **On main (audited at ca0290c2):** All six engines render offline (`warp.cpp`, 383 lines): `renderBeats` slices at onsets, starts its loop 10 ms after the slice and interpolates linearly; `renderTexture` uses Hann grains with seeded jitter; Re-Pitch runs libsamplerate; R3 runs Rubber Band's Finer engine. `adi_warp_playback_tests` pass on synthetic material. No listening test against Live has been run.
- **Proposed contract:** `renderBeats` cuts on the AWRP tick grid by Preserve; the Forward and Back-and-Forth loop point is a sign change within ±5 % of the segment middle; a micro-fade at slice switches (length measured); 4-point Hermite interpolation in `NativeSource::sample`; Texture's parameters and seed read from `clip_warp_settings`; deterministic seeds. The named parity gaps stay named (Tones Grain Size against R3's window option; Complex Pro Envelope).
- **Reuse:** No external take; the Live manual is the specification, and Rubber Band R3 and libsamplerate are already pinned.
- **Reject or defer:** SoundTouch stays out (FEATURES §10.5). Defer real-time players to TM-WT-04.
- **Research IDs:** A-W09, A-W10, A6C-039, A6C-040, DR-04-03, DR-04-04, DR-04-05, DR-04-06, DR-04-07; M2-A-WARP 6; M6-A6C-039, -040.
- **Unresolved test (NOT RUN):** M2-A-WARP 6 (each mode at 0.75x and 1.5x tempo and ±7 semitones; loop modes and envelope extremes; grain extremes; formants). ADI: no step above -60 dBFS at slice boundaries with envelope 100; Preserve 1/8 slices on exact frames.

#### TM-WT-04 Warped audio that follows tempo changes

*decided* · size XL · evidence documented · lane engine · milestone P2 · after: TM-WT-03, TM-TT-01

- **Need and current decision:** Tempo edits, ramps, Link tempo and scene tempo must not re-render every warped clip offline; warped Session clips and Link following need playback that follows tempo. ADR-0061 d1, d3, d4; ADR-0151 (open items); FEATURES §1 (tempo map with ramps, P0); ADR-0185 d7.
- **On main (audited at ca0290c2):** Not built. ENGINE-WARP: "A tempo change requires a new prepared playback generation"; `clip_playback.cpp:435-452` prepares each warped clip synchronously while building a generation, with no stop token.
- **Proposed contract:** Streaming Beats, Texture and Re-Pitch players, and a primed real-time R3 (`getStartDelay`, `getPreferredStartPad`) whose latency joins delay compensation (ADR-0061 d4). `projectWarpAnchors` densifies once ramps play. Until then a tempo change is a new prepared generation (the external-tempo mechanism of #455), and every warped clip says so in `problems()`. An equivalence test: at a steady tempo the streamed render equals the prepared one within a stated tolerance.
- **Reuse:** WT6 (Tracktion `WaveNode` priming and the real-time Rubber Band wrapper, design); Rubber Band's real-time mode (pinned).
- **Reject or defer:** Reject whole-output resampling for Link following (unwarped audio keeps its own clock; the engine log says the same). Defer until TM-WT-02, TM-WT-03 and TM-TT-01.
- **Research IDs:** DR-04-12, C3-011, R11E-038; M2-A-WARP 7; M3-LAUNCH L2.
- **Unresolved test (NOT RUN):** A latency-alignment test (a silent middle second, onset RMS, read-ahead on and off); M2-A-WARP 7 (playback against offline, stop and seek then play, save and reopen).

#### TM-TT-01 Tempo ramps play everywhere

*decided* · size M · evidence documented · lane engine · milestone M3 · after: none

- **Need and current decision:** FEATURES puts the tempo map with ramps at P0; every engine still plays step tempos, and no queue names the work (ON12). FEATURES §1; ADR-0004; ADR-0151 (open item); ADR-0155; SPEC `tempo_map.curve`.
- **On main (audited at ca0290c2):** Not built. `clip_playback.cpp:395` and `midi_clips.cpp:341` report that ramps are unsupported and step tempos are used.
- **Proposed contract:** One effective tempo map that integrates linear and bezier ramps exactly (tick to sample with no per-sample float drift), read by every builder: clip placement, MIDI schedules, automation, the metronome, warp anchors, `play_quantize` and `launch_quantize`. The same map carries Link and clock performed boundaries as a private overlay that never enters the project, as #455's private playback model does.
- **Reuse:** No ledger take; in-house. FilmCraft's and SoundCraft's time crates give vectors only.
- **Reject or defer:** Reject a per-block `TimingView` override, which would move plug-in tempo and clicks while clips keep the project tempo (the link-sync verifier's correction 1).
- **Research IDs:** ON12 (the state map), DR-04-12, C3-011.
- **Unresolved test (NOT RUN):** ADI: ramp placement against exact integration at 44.1, 48 and 96 kHz; a MIDI note at a ramp's midpoint lands on its exact sample; warped audio across a ramp.
- **Cross-check and this plan:** XC-30: the engine seat takes it after the Link consumer, on the same effective-tempo path.

#### TM-WT-05 Short samples on drop

*new-choice* · size M · evidence documented · lane win, engine · milestone M6 · after: TM-WT-01

- **Need and current decision:** Live's Loop/Warp Short Samples (Unwarped One Shot, Warped One Shot, Warped Loop, Auto, with loops assumed to be 1, 2, 4, 8 or 16 bars of 4/4) collides with the director's "a dropped file arrives clean". ADR-0132 d6, d7; ADR-0203 d7, d12; SETTINGS-CATALOGUE:181 ("DECIDED parity").
- **On main (audited at ca0290c2):** Not built. `record.shortSampleMode` offers {auto, on, off} (`registry.cpp:444-445`); `prepareImportAudio` has no classifier.
- **Proposed contract:** Pending CH-TM-3. With the recommended answer: every drop stays clean whatever its length; the short-sample classifier runs when Warp is pressed and in the import prompt's Warp choice (a loop becomes a Warped Loop with two markers and the loop on; a single event becomes an Unwarped One Shot; anything else stays Warp off); `record.shortSampleMode` gets Live's four choices; the x2 or /2 proposal never changes the project tempo; the short/long threshold is provisional at 60 s until measured.
- **Reuse:** WT4 (Audacity 3.7.9's loop-or-one-shot classifier, the decision only); WT5 (Ardour's minibpm estimate with whole-beat rounding and the 1/2/4/8/16-bar snap).
- **Reject or defer:** Reject Audacity's rule that any whole number of bars in several meters is a loop (Live assumes 1/2/4/8/16 bars of 4/4).
- **Research IDs:** A-W01, A6C-008, A6C-032, DR-04-08; manual §9.2.4.1.
- **Unresolved test (NOT RUN):** Live 12.4: the short/long threshold, and what Auto does on a drop against a Warp press. ADI: synthetic loops of 1, 2, 4 and 8 bars at 70-180 BPM give exact bars and BPM; a single crash is a one-shot.

#### TM-WT-06 The Clip View Sample Editor

*decided* · size XL · evidence documented · lane window · milestone M6 · after: TM-WT-02, TM-MD-04

- **Need and current decision:** Live's audio Clip View: yellow warp markers, grey transients, hover pseudo-markers, double-click to insert or delete, Ctrl+I, Shift-drag, Seg. BPM with x2 and /2, the mode chooser with each mode's controls, the warp context menu, Quantize (Ctrl+U), the inline BPM field when the engine finds no beat, and the Save button for sample defaults. STEP-7-PLAN 7.7; ADR-0203 d6, d7, d18; ADR-0184; ADR-0210 d3.
- **On main (audited at ca0290c2):** Not built. The (its source file) branch holds a plan, a 37-row checklist and a picture.
- **Proposed contract:** Built against needs rows for transients, warp settings and AWRP in the snapshot, after the director approves the Clip View picture (his open gate in the window log). Each gesture commits one prepared batch through `OpSubmitter`.
- **Reuse:** None external.
- **Reject or defer:** Waits for TM-WT-02's ops and the picture approval.
- **Research IDs:** A6C-034, A6C-035, A6C-036, A6C-037, A6C-038, A6C-039, A6C-040, DR-03-09; parity-7.7 row 36.
- **Unresolved test (NOT RUN):** The parity-7.7 checklist side by side with Live; headless seam tests per gesture.

#### TM-WT-07 Tempo leaders, and the duplicate tempo-detection row

*refinement* · size M · evidence documented · lane win, engine · milestone P2 · after: TM-WT-04, TM-TT-01

- **Need and current decision:** Live's warped clips as tempo leaders (the bottom-most leader wins; Unfollow turns the result into editable tempo automation). FEATURES lists "Tempo detection from audio" twice (P0 at line 38; P2 "writes tempo_map" at line 50). ADR-0203 d1; d9 (no Tempo Follower, a different feature).
- **On main (audited at ca0290c2):** Not built; no row names Live's tempo leader.
- **Proposed contract:** Live's tempo leader at P2 (win's placement) writes the effective tempo map as a performed overlay while it follows, and turns into ordinary `tempo_map` events on Unfollow in one undo step. The P2 row at line 50 is rewritten as this feature. The Tempo Follower stays out.
- **Reuse:** None.
- **Reject or defer:** The Tempo Follower stays rejected (d9).
- **Research IDs:** A-W08, A6C-031; M2-A-WARP 9.
- **Unresolved test (NOT RUN):** M2-A-WARP 9 (two leaders, precedence, Unfollow, undo).

#### TM-WT-08 Hitpoints, Slice to MIDI and Audio Alignment (P2)

*decided* · size L · evidence documented · lane engine, devices · milestone P2 · after: TM-WT-01, TM-WT-02

- **Need and current decision:** One onset detector, cached per file, serving hitpoints, slicing to a new MIDI track, Audio Alignment and the agent's `analyze.transients`. ADR-0176 (P2); FEATURES §3 (P2 rows); ADR-0062.
- **On main (audited at ca0290c2):** Not built. ADR-0176's precondition, warped playback, is now met offline.
- **Proposed contract:** Hitpoints are TM-WT-01's cached onset strengths, filtered at read time by Cubase's Threshold, Intensity, Minimum Length and Beats. Audio Alignment is dynamic time warping over onset strength and MFCCs, thinned to warp markers in one changeset. Slicing uses OneShot (#278) and a Drum Rack container, both inside authorized scope; no new instrument.
- **Reuse:** WT2's onset strength; qm-dsp's MFCC inside WT1's vendored subset.
- **Reject or defer:** P2, after TM-WT-01 to -03. No new instrument work (instruments excluded).
- **Research IDs:** C3-001, C3-002, C3-003, C3-004, C3-005, R11E-026, DR-03-12.
- **Unresolved test (NOT RUN):** The Cubase hitpoint and alignment cases; R11E-026's phase-align comparison.
- **Cross-check and this plan:** XC-34: slicing needs DEV-21 (OneShot) and DEV-04 (the Drum Rack container).

#### TM-WT-09 Convert Harmony, Melody and Drums to New MIDI Track

*decided* · size L · evidence documented · lane engine, window · milestone P2 · after: TM-WT-01, Q-D

- **Need and current decision:** Live's three audio-to-MIDI conversions on an audio clip's context menu (Convert Harmony, Melody and Drums to New MIDI Track), each creating one MIDI track and clip. A Live ability, so it must exist with a priority (ADR-0203 d1). FEATURES carries no row; the AI-workflow ADR lists audio-to-MIDI among its fifteen workflows and says they live in FEATURES, where no row exists. New FEATURES row, P2 (§8.2, P-7).
- **On main (audited at ca0290c2):** not built; the onset detector behind tempo detection exists (TM-WT-01's audit); no pitch tracker.
- **Proposed contract:** each command writes one MIDI track and one clip in one undo step, the notes placed through the clip's warp markers onto the timeline; Drums reuses TM-WT-01's cached onset strengths (ADIA sections, versioned, keyed by the file hash) and maps onsets to pads by band; Harmony and Melody need a pitch tracker, chosen with Q-D's analyser so the suite has one; velocity from onset strength; results as versioned ADIA sections so a second conversion is instant. Live's choices (the device the new track gets, quantize, names) are read before the defaults are frozen.
- **Reuse:** WT1 (the onset path, in M6); the pitch tracker after Q-D (none harvested; a candidate row for the benchmark).
- **Reject or defer:** a model-based transcription (the AI workflows' audio-to-MIDI stays in that ADR's list, a separate feature with consent and a model, never this command); any new instrument work for the Drums target beyond OneShot and the Drum Rack container (TM-WT-08).
- **Research IDs:** none in the bundle; the Live 12 manual's chapter 11 cases go to §7.1's P2 readings.
- **Unresolved test (NOT RUN):** Live 12.4: which track and device each conversion creates and how it is named; the quantize and velocity applied; Drums' pad mapping; Harmony's polyphony limit; what a warped clip's markers do to the notes. ADI: a click track converts to one note per click at the right beat; a sung scale converts to the right pitches within a semitone.
- **Cross-check and this plan:** added by this revision (the critique of 5 November): the matrix had no row for a Live ability.

#### TM-LS-01 Ableton Link (P1)

*decided* · size L · evidence measured · lane engine, window, win · milestone M3 · after: TM-SV-01, TM-TT-01

- **Need and current decision:** Tempo, beat and phase with other apps and peers; Start Stop Sync; the peer count on the control-bar toggle; Play waits for the quantum (the Global Quantization) with a progress bar; a peer's tempo overrides tempo automation; count-in off under Link; Link and MIDI sync together; driver gating on Windows. ADR-0185 d7; ADR-0203 d11, d19; ADR-0162; ADR-0107 d1; ADR-0024; BUILD-BRIEF Phase D item 11.
- **On main (audited at ca0290c2):** In flight, not on main. Open PRs #454 (transport-generation), #455 (external-tempo) and #456 (link-session, pinned to `Link-3.1.5`) passed the engine seat's Linux GCC gates (14,115 to 14,176 checks; #456 also 47 checks with two real SDK peers); their GitHub checks were partly cancelled. (its source file) is local WIP with a 26-check native consumer and no real-peer consumer test yet. Main has a control-bar stub ("Link is not built yet.") and the rows `sync.linkEnabled`, `sync.linkStartStopSync`, `sync.showLinkToggle`.
- **Proposed contract:** The SDK participant behind one pImpl translation unit. One driver captures and commits the SDK's audio state per callback at callback time plus physical output latency plus graph compensation, never counting CoreAudio's latency twice. Link's tempo enters as an effective tempo generation every builder reads (#455's private playback model); performed boundaries keep elapsed sample time continuous. Graph and transport metadata publish together (#454). Starts and stops land exactly on the quantum across sub-buffer slices. A user's tempo is the normal op, committed to Link; a peer's tempo is a runtime override that lights Re-Enable Automation (ADR-0162) and is never an op unless the Live reading says Live saves it. The pin moves to `Link-4.1.1` before the consumer merges (conflict 1). Link runs on ASIO and not on DirectSound, as Live's MME/DirectX rule; WASAPI per CH-TM-5.
- **Reuse:** LS1 (the Link SDK, its TEST-PLAN cases and `HostTimeFilter`); LS4 (Tracktion's chase policy, design); LS5 (Carla's hylia adapter shape, design); LS6 (BespokeSynth's offset and warm-up ideas, design).
- **Reject or defer:** Reject the SDK example's Windows latency arithmetic, Tracktion's `getTempoInRange` (Link's range equals ADI's 20-999), a `TimingView`-only override, whole-output resampling, and BespokeSynth's SPP code. Defer Link Audio (TM-LS-02).
- **Research IDs:** DR-11-02, A6I-006, A5C-026, A5C-027, A5C-028, A5C-029, A6G-002, E24 (Link README at `5ba907ae`); M3-CONTROLLER 6.
- **Unresolved test (NOT RUN):** AUDIOENGINE-1 (onset within 3 ms of LinkHut on the director's Windows workstation, recorded through loopback); TEMPO-1 to -5, BEATTIME-1 and -2, STARTSTOPSTATE-1 and -2 as deterministic tests; M3-CONTROLLER 6, M5-A5C-026 to -029, M6-A6G-002 and M6-A6I-006 on Live 12.4; a held-note tempo-change test (conflict 19).
- **Cross-check and this plan:** XC-31: re-pin #456 to Link-4.1.1 inside the same stack, Link Audio off; XC-25: held notes keyed by stored identity.

#### TM-LS-02 Link Audio

*new-choice* · size L · evidence documented · lane engine, window, win · milestone P2 · after: TM-LS-01

- **Need and current decision:** Live 12's Link Audio: audio to and from Link peers as an Input Type and Input Channel, with its settings (Audio, Name, Latency, Sync to Incoming Audio, the peers list). ADR-0204 d5 (closed by win on a false premise); ADR-0203 d19 ("stays a wish"); ADR-0126 (the LAN node); ADR-0185 d7.
- **On main (audited at ca0290c2):** Not built. The catalogue keeps its rows as a wish on the same premise (`catalogue.cpp:342-345`); FEATURES:47 repeats it.
- **Proposed contract:** Per CH-TM-1. If reopened: `BasicLinkAudio` on the same 4.1.1 pin, enabled only when used; a source node per peer channel into a track's input junction, a sink per track with audio; Latency and Sync to Incoming Audio as declared latency (ADR-0066); a peer endpoint identified by peer and channel ids, not display names (a format minor, win's).
- **Reuse:** LS3 (Link-4.1.1's Link Audio headers, its 18 SDK tests and the renderer example).
- **Reject or defer:** If he keeps it closed, the 4.1.1 pin still stands, so nothing is swapped later.
- **Research IDs:** A6I-001, A6I-002, A6I-003, A6I-004, A6I-005, E24 (`LinkAudio.hpp` at `5ba907ae`), OPEN-QUESTIONS (milestone 3); AWAITING row 15.
- **Unresolved test (NOT RUN):** A6I-001 to -005 with Live 12 or Push as peers; AWAITING row 15 (do Link Audio peers appear by themselves).
- **Cross-check and this plan:** XC-15: a Link Audio peer is an external source in input_ref's grammar, not a new format minor.

#### TM-LS-03 MIDI clock and timecode (P2), and the MIDI port layer

*decided* · size L · evidence documented · lane engine, window, win · milestone P2 · after: TM-LS-01, TM-IC-01

- **Need and current decision:** Live's MIDI sync: clock in and out, timecode in (MTC out an ADI option), a Sync Delay per port, Song or Pattern clock, Hardware Resync with its button, EXT with its two lights, SMPTE All; JACK transport on the same path; a registry that matches d9. ADR-0203 d9; ADR-0145 d4 (JACK transport); ADR-0188 d7 (JUCE UMP endpoints); ADR-0206 d3; ADR-0107 d1.
- **On main (audited at ca0290c2):** Not built. There is no hardware MIDI port I/O anywhere in `src` (no `juce::MidiInput` or `MidiOutput`); the per-port rows exist, as does the contradicting `sync.clockSource` {internal, midiClock, link} (`registry.cpp:591`); the Tempo Follower rows still read "P2 Wish" (`catalogue.cpp:346-349`).
- **Proposed contract:** MIDI ports on JUCE 9's UMP API (the Windows MIDI Services backend off until SRF-16; the live backend shown in Settings) feed lock-free rings with host timestamps; output runs on a timed thread, because the UMP `Output::send` takes no time. Clock in: a second-order DLL feeds the same effective-tempo overlay as Link; Song Position Pointer lands on exact ticks (1,441,440 per sixteenth, 240,240 per clock); Stop rewinds to the last MIDI beat. Clock out: Song sends Start at 1.1.1, otherwise SPP and Continue; Pattern sends Start at the next bar; no SPP above 0x3FFF; Sync Delay and latency applied. MTC in: quarter-frame assembly, lock after 8, SMPTE All detection, a locate on lock or after more than a frame of drift, tempo untouched; 29.97 drop-frame vectors recomputed with exact rationals. Registry: remove `sync.clockSource` and `sync.tempoResponsiveness`; add clock type, hardware resync, the resync button and SMPTE All; the Tempo Follower rows become Rejected. A FEATURES row: "MIDI clock in and out, MTC in, as Live, P2; MTC out an ADI option".
- **Reuse:** LS2 (Ardour's MIDI clock slave, ticker, MTC parser and slave, timecode maths: port); LS4 (Tracktion's MTC reader chase policy, design); LS6 (BespokeSynth ideas, design); IC8 (OTIO's drop-frame vectors, through the time base).
- **Reject or defer:** Reject Ardour's 29.97 drop-frame column as it stands (two rows truncated), Bitwig's "sync in" chooser, Tracktion's MTC generator, BespokeSynth's SPP truncation, the Tempo Follower. Video sync and pull-up stay P3.
- **Research IDs:** A6I-007, A6I-008, A6I-009, A6I-010, A6I-011, A6I-012, A5C-024, A5C-025; AWAITING row 13; manual 17.3.1.2, 36.3.
- **Unresolved test (NOT RUN):** AWAITING row 13 (MTC rates and default; how fast Live follows a changing clock; Resync); the Song-mode restart point; EXT persistence. ADI: the DLL tracks 120 to 121 BPM under ±1 ms jitter; MTC quarter frames forward, backward and skipped.
- **Cross-check and this plan:** XC-18: the port host is the window's, clock and timecode decoding the engine's; XC-39: src/adi/sync stays MIDI sync's (collaboration takes src/adi/collab).

#### TM-MD-01 MP4, M4A, MOV and ALAC import

*decided* · size L · evidence measured · lane engine, win · milestone M3 · after: none

- **Need and current decision:** Every Live format and the director's list: AAC-LC in any channel layout and HE-AAC, ALAC, MOV PCM, fragmented MP4, gapless trimming; the audio of MOV, MP4 and M4V video files. ADR-0203 d18 (P1, AAC and M4A included); ADR-0132 d3-d4; ADR-0156; ADR-0191 d4 with ADR-0204 d5 (libavcodec loaded at run time on Linux); ADR-0206 d4 (the picture is P2).
- **On main (audited at ca0290c2):** Partial. `decode.hpp:60` `SourceFormat` = {Riff, Aiff, Flac, Mp3, Ogg, Unknown}; `mp4_aac.cpp` (609 lines) is the NI subset (fragments refused at `:287`/`:293`; a single unit edit only at `:418-422`; `iTunSMPB` unread). Open PR #452 adds general mono and stereo AAC-LC but carries conflict 16. CI at `ca0290c2`: `adi_mp4_aac_tests` fails on both heavy macOS legs. `material_commands.cpp:90` refuses video.
- **Proposed contract:** Step zero is conflict 15: the CoreAudio priming mismatch fixed so `adi_mp4_aac_tests` and `adi_ni_stems_tests` pass on every leg at one headSha. Then a media ADR: ADI's own bounded ISO-BMFF and QuickTime reader (headers only, never `mdat`; hard caps; a hostile file is `decode.corrupt`, never a crash); AAC channels and rate from the AudioSpecificConfig, never the sample entry (the entry only when the configuration is 0); trimming by edit list, else `iTunSMPB` (priming 0-16384, FFmpeg `mov.c`'s bounds), else none; platform decoders fed raw packets (Media Foundation decoder MFTs, `AudioConverter`, libavcodec 60, 61 and 62) with no temporary re-mux; ALAC in process; a movie's first audio track imports while the picture stays P2 (win's placement, to strike if wrong). The NI reader becomes a strict filter over the general one.
- **Reuse:** MD2 (Mixxx's decoder acceptance suite and fixtures, first); MD1 (craft-libs `a55273e` ISO-BMFF demuxer, ported, with the ASC winning); MD3 (Apple's ALAC codec).
- **Reject or defer:** Reject craft's sample-entry channel count, the Windows re-mux, complex edit lists until P2, and Ardour's `-map_channel`. Keep QuickTime chapter and marker tracks for the P2 video work.
- **Research IDs:** Manual §5.1 and chapter 27; A6R-047 to -053 (video fixtures); M6-A6I-013, -014; DR-12-08; new AWAITING rows L-MEDIA-2 and L-MEDIA-3.
- **Unresolved test (NOT RUN):** A-M1-1 to -10 on all seven legs (Mixxx's iTunes AAC gives 1,325,952 frames with first sound at frame 1166; the FFmpeg AAC at 1112; ALAC equals the FLAC decode; MOV in24, sowt and lpcm v2 bit-exact; fragmented equals non-fragmented; hostile files refused); L-MEDIA-2 (Live's first-sound frame for an AAC with no gapless data); L-MEDIA-3 (what Ogg FLAC, ALAC M4A and H.264 MP4 do when dropped into Live on Windows).
- **Cross-check and this plan:** PD-00's AAC half is in open PR #461; #452's AudioSpecificConfig fix comes before it merges.

#### TM-MD-02 Playing before decoding finishes; known-hash loads; Cleanup

*decided* · size M · evidence documented · lane engine, window · milestone M3 · after: TM-MD-01

- **Need and current decision:** Live §5.1: a compressed file plays at once while its decode continues; the decoding cache has a Cleanup button; a session load does not re-hash every compressed source; partial files left by a crash are removed. ADR-0203 d18; ADR-0156 (its open items: Cleanup, a path/size/mtime memo); ADR-0151 d4, d8.
- **On main (audited at ca0290c2):** Not on main. Main decodes a compressed file completely and synchronously before its clip can sound (`clip_playback.cpp:415`, `audio::playableFile`) and re-hashes compressed sources on every load. Open PRs: #451 PCM pull streams (its head green) and #450 progressive decode (four failing checks on its head; it stands on #451 and #452).
- **Proposed contract:** One decode job per source hash, with frames ready and a state. A clip's page is served once the decoded frontier covers it; a pending page plays silence counted as pending, never as an underrun; the clip switches to the finished file without a rebuild. A warped clip waits for the whole file (Live's analysis gate). Session load passes the stored BLAKE3, so nothing is re-hashed. Leftover partial files are deleted when the cache loads. Cleanup removes unpinned files outside the current Set's hashes. Each clip's decode state reaches the snapshot (a needs row).
- **Reuse:** No ledger take: Live §5.1 is the specification, and Mixxx's caching reader is moot after #450.
- **Reject or defer:** Reject a separate shared-file positional-read layer (short reads under the job's lock replace it).
- **Research IDs:** DR-12-08, A6H-064, A6C-033, A5H-009; M3-MEDIA F6; new AWAITING rows L-MEDIA-1 and L-MEDIA-5.
- **Unresolved test (NOT RUN):** A-M2-1 to -8 (the import commits before the decode ends; once done, an offline render is bit-identical; one job per source; partial files reaped; a rename under a reader on Windows; a source changed mid-decode; a 50-source load hashes nothing; Cleanup); L-MEDIA-1 (time to first sound of a 10-minute MP3 or M4A in Live, and whether it plays before analysis ends).

#### TM-MD-03 Relink by hash, automatic search, Offline and Replace

*refinement* · size M · evidence documented · lane engine, window, devices · milestone M3 · after: TM-MD-02

- **Need and current decision:** Media "relinks by itself when moved" (the Dream, §12): SPEC §10.2's resolution with registered folders and search paths; Live's automatic search (Search Folder, Project, Library; Fully Rescan Folders); the Offline mark; Live's two Replace rules; a player that never plays a same-name file with other content. ADR-0127; ADR-0143; ADR-0147; SPEC §10.2; FEATURES §8 (relink by content hash, P1); ADR-0005.
- **On main (audited at ca0290c2):** Partial. `resolveMedia` tries the stored path, the project's (its source file) and the absolute hint, each BLAKE3-verified; `media.relink` needs a user-chosen identical file; the clip player does not use that fallback and never checks the hash; no search, no Offline state; `audioClip.setSource` is unbuilt.
- **Proposed contract:** `resolveSession` checks the stored path, the project's (its source file), registered folders and search paths, each verified by BLAKE3 through a size and mtime memo. `searchMissing` asks the library's hash index first, then walks folders hashing only files of the right size. Exact matches become one undo step of `media.relink`. Byte-identical copies in several folders are picked automatically: the project folder first, then the longest common path suffix (an extension of the approved hash relink, not of Live's Hot-Swap list). A same-name file with other content is listed as a mismatch and never relinked or played. Offline clips play silence and name their problem. Two ops for Live's two Replace rules: the Clip View's replace is `audioClip.setSource` (one clip; warp markers kept only at an equal length); the File Manager's Replace is a media-level batch (every clip; markers kept when the new file is the same length or longer).
- **Reuse:** Mixxx `trackdao`'s longest-common-suffix ranking (design); ADI's own `visitPaths` and `resolveMedia`.
- **Reject or defer:** Reject Live's unverified manual repair (hash identity is stronger, A5H-018). Collection and trash belong to TM-FC-03.
- **Research IDs:** DR-12-07, A5H-016, A5H-017, A5H-018, A6C-026, A3-023, N10H-053, A11N-008; M3-MEDIA F4; new AWAITING row L-MEDIA-4.
- **Unresolved test (NOT RUN):** A-M3-1 to -8; M3-MEDIA F4 in Live (same name with wrong content; a renamed identical copy); L-MEDIA-4 (does a manual repair keep clip settings and warp markers). Precondition, measured red: `adi_project_files_tests` fails 9 of 44 on both heavy macOS legs at `ca0290c2` ("relocated audio actually plays", "relink inverse paths are relocated too", "the missing file's original location is retained").

#### TM-MD-04 The analysed folder, sample defaults and sample analysis

*decided* · size M · evidence documented · lane engine, window, win · milestone M3 · after: none

- **Need and current decision:** One "analysed" folder in ADI's data folder, never beside the samples; warp markers and clip settings saved with a sample live in the library and are never cleaned up; BPM, key and searchable tags in the browser; background analysis with Pause; Live's Create Analysis Files switch. ADR-0203 d18; ADR-0104; ADR-0145 d10, d11; ADR-0147; FEATURES:415 (Essentia/CLAP, P1); FEATURES:568.
- **On main (audited at ca0290c2):** Not on main. Open PRs #448 (sample settings; head green), #457 (Save and Recall through `prepareImportAudio`; one failing check) and #449 (the analysed cache, ADIA v1 with a peaks section only; one failing check). Main's library annotations accept only tags, BPM and rating; no key detection; no tag reading.
- **Proposed contract:** Derived analysis is versioned ADIA sections in (its source file) (peaks, onsets, beats, downbeats and regions, key with its strength, loudness), evictable with the cache; the eviction is built, or the `cache.maxSizeMb` help text changes. The user's data is the library's `sample_settings` record (Live's Save), never cleaned; recalled on import, it overrides Auto-Warp. Key comes from the suite's analyser (CH-TM-2). Tags are read-only: TagLib, plus dr_wav's `acid`, `smpl` and `inst`, Vorbis comments and Apple Loops' `basc`. Precedence: the user's value, then the file's tag, then ACID, then detection. Library sync by hash carries key and the source of each BPM.
- **Reuse:** MD6 (qm-dsp key detection with Mixxx's global-key reduction); MD7 (TagLib, read-only, with Mixxx's field map and key-text parser); MD8 (the metadata of the dr_libs ADI already vendors); MD9 (libsndfile's `basc` reader, ported); MD10 (magda-core's media database, design for Similarity Search and Auto Tags later).
- **Reject or defer:** Reject ADIL v2 `clip_defaults` and an `AANL` file (conflict 7). Defer Similarity Search, Similar Sample Swapping and Auto Tags (Live 12 features needing a model runtime; whether their model weights are usable is not yet checked) to a later row. REX has no usable decoder.
- **Research IDs:** A-W05, A6H-063, A5H-008, A5H-009, DR-02-09, DR-04-11, R11X-023, R11X-024; M2-A-WARP 8; new AWAITING row L-MEDIA-6.
- **Unresolved test (NOT RUN):** A-M4-1 to -9 (key cases, including 24-key cadences with at least 22 exact; tag read-back; precedence; eviction; a JSON round trip); M2-A-WARP 8 (sample defaults in Live); L-MEDIA-6 (does Live use an ACID tempo when warping).
- **Cross-check and this plan:** XC-19: one analysis store for the suite; merge order #448, #457, #449, then PD-04 folds #449's replace primitive.

#### TM-MD-05 The long tail of formats

*refinement* · size M · evidence inferred · lane engine, win · milestone M3 · after: TM-MD-02

- **Need and current decision:** The rest of the director's list (Opus, WMA, WavPack, CAF, APE, M4B, DSF and DFF, Musepack, RealAudio, MKA, MKV and WebM, AVI, AC-3, DTS, ID3-prefixed ADTS); Sound Designer II at P3. ADR-0132 d3 (FFmpeg decode-only for the rest); ADR-0156 d1; ADR-0203 d18; ADR-0204 d2 (installer tiers).
- **On main (audited at ca0290c2):** Not built. Ogg Opus is sent to stb_vorbis and fails as `decode.corrupt`; the others give `decode.unknown_format`.
- **Proposed contract:** A format Live imports never needs an outside tool: Ogg Opus decodes in process (opusfile, with pre-skip) and ALAC too (TM-MD-01). The rest decode through FFmpeg, decode-only, as a child process (ffprobe JSON validated, float samples streamed), its path in `files.ffmpegPath`, shipped in the installer's tools tier (win's placement). No tool means `decode.unsupported` naming the tool; a killed child means `decode.corrupt`.
- **Reuse:** MD4 (opusfile and libopus); MD5 (Ardour's FFmpeg importable source as design, on ADI's own `scan_process` streaming); craft-media's Matroska and Ogg demuxers as in-process alternatives.
- **Reject or defer:** Reject Ardour's `-map_channel` (gone since FFmpeg 7) and Audacity's wx-bound FFmpeg layer. Sound Designer II stays P3 (libsndfile's `sd2.c` as the reference). REX: no usable path.
- **Research IDs:** ADR-0132 d3's list; R11X-022; M10-INV-REX (evidence, milestone 10); the A-M5 acceptance cases.
- **Unresolved test (NOT RUN):** A-M5-1 to -5 (Mixxx's `.opus` first sound at 1268 at 48 kHz; WMA, WV, CAF, MKV, WebM, AVI and TS fixtures; no tool; a killed child; hostile ffprobe JSON).

#### TM-FC-01 Bounce to New Track and Bounce Track in Place as Live

*refinement* · size M · evidence documented · lane engine, window · milestone M5 · after: none

- **Need and current decision:** Live 12's bounces: a post-FX, pre-mixer render; the new track takes the source's mixer settings; only the source clips or selection are muted; one new track per selected track; Bounce Track in Place replaces the track's devices (a MIDI track becomes audio) and also flattens a frozen track. ADR-0203 d1, d11 (the bounce prompt), d16; ADR-0059.
- **On main (audited at ca0290c2):** Built, with six differences from Live, each read in code: a post-fader tap (`bounce_commands.cpp:120`); the whole source track muted (`:140`); one new track only; no mixer settings copied; Bounce in Place parks the inserts bypassed with `device.setEnabled`, keeping their delay (`:210`); both refuse a frozen source (`:59-65`, `:174-179`). `adi_track_bounce_tests` pass at `ca0290c2`.
- **Proposed contract:** Render `OfflineTap::PreFader` (that is, `Tap::PostFx`). Copy, in the same transaction, `volume_db`, `pan`, `pan_law`, `width`, `phase_invert`, `delay_samples`, `vca_group_id`, `mixer_pan_settings` and the sends at their levels, never `input_gain_db`, which the tap already bakes. Split at the selection's edges and mute only the inner pieces (`clip.setMute`, never `track.setMute`). One new track per selected track, one undo step. Bounce in Place removes the devices, captured for undo as track-delete undo does, instead of parking them bypassed with their delay. A frozen source bounces in place from its verified wet file, with no render. Renders are placed Warp off (d16); the new clip takes the source clip's name.
- **Reuse:** FC4 (SoundCraft's `commit_track` and pre-fader render, design and tests); FC2 (Tracktion's clip-render options, design).
- **Reject or defer:** Reject SoundCraft's deactivate-and-hide (that is Pro Tools' Commit, not Live). Group bounces belong to TM-FC-02.
- **Research IDs:** A4-006, A4-007, A6H-023, A6H-024, C-E01, C-E02, C-E03; M4-A-BASE contracts 1-3; M6-A6H-023, -024.
- **Unresolved test (NOT RUN):** The A4-006 null (with the original muted, the new track nulls against it, also with a track delay); which mixer settings Live copies (automation? split pan?); Bounce Track in Place on a MIDI track.
- **Cross-check and this plan:** XC-15: tap names from the one vocabulary; XC-33: E6's bounce prompt states the six differences until this lands.

#### TM-FC-02 Consolidate, Paste Bounced Audio and group bounce

*refinement* · size M · evidence documented · lane engine, window, win · milestone M5 · after: TM-FC-01

- **Need and current decision:** Live's Consolidate (Ctrl+J: the clip engine's pre-FX output per track, normalized with a compensating clip gain; MIDI merged with no file; frozen tracks allowed); Paste Bounced Audio (Ctrl+Alt+V, rendered when pasted); Bounce Group in Place and to New Track (post-mixer through Main, without Main's chain). ADR-0203 d1, d16; ADR-0059; ADR-0132 d1 (32-bit float files). FEATURES has no Live row (only FL's `clip.consolidate` at P2).
- **On main (audited at ca0290c2):** Not built: no consolidate op or command (the window worklist: "Consolidate is not supplied by the engine and is in no row"); no Paste Bounced Audio; Bounce in Place refuses a group, and Bounce to New Track renders a group's direct output.
- **Proposed contract:** One capture for every derived render: `captureDerived`, with a tap per track, a range, incoming routes pruned so audio sent in from other tracks is excluded, pinned and verified inputs, and the latency guards. Consolidate renders `PreFx` over the range for every selected audio track in one pass, peak-normalizes with #418's measurement, writes a compensating `clip.setGain`, and replaces the material through the range planner in one undo step labelled "Consolidate", keyed by a position-relative recipe. A MIDI track gets one clip with its note identities kept and nothing rendered. Warp state per CH-TM-4. Paste Bounced Audio keeps the source reference on the clipboard and renders `PostFx` at paste time; it is unavailable once the source is gone; an empty MIDI track becomes audio. Group bounces come after the export area's via-Main isolation: children routed outside the group are excluded; returns count only when they reach Main. FEATURES gains Live rows: Consolidate (P1), Paste Bounced Audio, the group bounces.
- **Reuse:** FC3 (Ardour's split between consolidate, consolidate with processing, bounce and freeze, design); FC4 (SoundCraft's consolidate and MIDI consolidate with three tests); FC2 (Tracktion's clip render without plug-ins); ADI's own #418 normalization and the range planner.
- **Reject or defer:** Reject Qtractor's merge (no clip envelopes; ADR-0069 d5). The group bounces wait for the export area's via-Main isolation.
- **Research IDs:** A4-005, A4-008, A4-009, A5C-022, R11E-005, R11E-009, R11E-010, R11E-011, R11X-029, R11X-030; M5-A5C-022; M4-A-BASE contracts 1-3.
- **Unresolved test (NOT RUN):** M5-A5C-022 (file against clip state); Live's normalization target and the clip gain it writes; the consolidated clip's Warp state in Live 12; A4-008 and A4-009. ADI: `test_consolidate` (a device excluded, a gap silent, one undo restores both clips, redo does not render, an incoming send excluded).
- **Cross-check and this plan:** XC-01: owns Consolidate's render, op, file and MIDI merge; XC-14: the first client of the one capture; XC-34: group bounces need EX-02's ViaMain.

#### TM-FC-03 The recipe key, reuse, collection and render folders

*refinement* · size M · evidence inferred · lane engine, window, win · milestone M5 · after: none

- **Need and current decision:** Freeze, unfreeze, freeze must not render twice; undo must always find old renders; unreferenced renders are never collected, so the project folder only grows; private stages survive a crash; freeze files sit at the project root. ADR-0059 d4-d5; ADR-0069 d2, d4; ADR-0038 (collection only at compaction or on an explicit vacuum); ADR-0127; ADR-0205 d10; SPEC §7.2.
- **On main (audited at ca0290c2):** Partly built: renders are BLAKE3 media rows and `media.unlink` never deletes a file, so undo and redo restore freezes and bounces without rendering (tests pass at `ca0290c2`); reuse is by output hash only, so a re-freeze renders again; nothing collects renders or state blobs (#419's compaction collects neither); `freeze-<hash>.wav` sits at the project root (`freeze_commands.cpp:155`); no stale-stage sweep.
- **Proposed contract:** An ADR, "freeze, bounce and consolidate are three contracts", with a recipe key: BLAKE3 over canonical CBOR of {key version, kind, the freeze fingerprint, installed identities, engine build, render spec}, position-relative for clip renders. Only a result that matches the current recipe revision may become the active derivative; a cancelled or stale job never overwrites a newer edit. Refs are written as `.part` and renamed after commit; they are advisory, and every hit is verified by BLAKE3. One render in flight per key; a Render Again command bypasses the lookup. Collection runs only from an explicit command (an Unused Files dialog: list, then trash, then empty), never during undo, redo or replay. The mark is by reference: current rows and every retained op on every branch, `track.setFreeze`'s before and after and clip images included, plus whether a path is live. Disposable stage prefixes are named (`.adi-freeze-`, `.adi-edit-`, `.adi-render-`, `.adi-media-`, `.adi-stem-import-`, `.adi-ni-`); `.adi-record-*` is never swept. Renders leave the project root for Live's (its source file) or (its source file) sub-folders; win picks.
- **Reuse:** FC1 (FilmCraft's render-preview cache, design; ADI's own code of about 150 lines); FC2 (Tracktion's prefix-hash render chain and single render per key, ported, with stage 0 folding the source media's hash); FC3 (Ardour's two-phase dead folder, design); ADI's own `visitPaths` (#411).
- **Reject or defer:** Reject FNV-1a and a size-plus-first-and-last-MiB media identity, Tracktion's purge on every render and delete on unfreeze, Ardour's history clearing and Audacity's compaction. Defer ADR-0069 clip chains (offline processing on a clip) until `device_chains` accepts a clip owner and the M2-C-DOP readings return; DOP's editing contract belongs to the editing area.
- **Research IDs:** DESIGN-IMPLICATIONS rows 1-3 (what an offline edit is, when its result may publish, what a render's identity holds); C-D01 to C-D16; A3-023, A6G-005, R10H-017, R10H-018; M5-A5C-018; M3-MEDIA F6.
- **Unresolved test (NOT RUN):** `test_render_key` (names, colours and the root fader leave the key alone; device parameters, clip gain, tempo, identities, engine build, rate and format change it); `test_freeze_commands` (freeze, unfreeze, freeze renders once; undo and redo across three freezes never render; a corrupted cached output misses); collector tests (a render reachable only from history is kept; one only a compacted prefix referenced is reported). M3-MEDIA F6 in Live.
- **Cross-check and this plan:** XC-32: depends on HI-02 and Q-A (what compaction keeps).

#### TM-FC-04 Freeze All Channels and Live's frozen-track editing

*decided* · size L · evidence documented · lane engine, win · milestone M5 · after: TM-FC-03

- **Need and current decision:** Freeze All Channels (P1: every child of a group rendered separately, so sidechains, sends and solo keep working); Live's edits on a frozen track (move, trim, copy, duplicate, consolidate, mixer automation; tails as temporary clips); freezing a track with an incoming sidechain or audio input; external devices forcing a real-time freeze. ADR-0203 d16; ADR-0059; FEATURES §2 and §10.5 (Freeze All Channels P1, the freeze fingerprint P1).
- **On main (audited at ca0290c2):** Track freeze and Freeze Group are built (schema 1.17 `frozen_renders`, 1.18 `frozen_groups`; `adi_freeze_*_tests` pass at `ca0290c2`). Freeze All Channels is not (it waits on ON7); freeze refuses a sidechain input (`freeze_commands.cpp:109-110`); edits to baked material are refused.
- **Proposed contract:** Each child is cached through `captureDerived` with dry and wet streams; sidechain keys and sends are served from those streams once route taps and sidechain endpoints have a format home (ON7). Re-freezing renders only the children whose recipe key changed. After the Live readings, frozen clips play windows of the track's wet file at their own positions, with tails as temporary windows; a bounce or consolidate of a frozen track reuses the wet file.
- **Reuse:** FC2 (the prefix-freeze idea, design); FC5 (MAGDA's real-time capture pass, parked until ADI has external audio devices).
- **Reject or defer:** Frozen-clip editing waits for M5-A5C-013 to -016; a real-time freeze waits for external devices.
- **Research IDs:** DR-07-21, FR-041, A5C-013, A5C-014, A5C-015, A5C-016, A5C-017, A5C-018, A6G-005, R10H-017, C-E03.
- **Unresolved test (NOT RUN):** M5-A5C-013 to -018; M6-A6G-005 (a frozen clip moved to another track or project); freezing a track whose compressor has a sidechain, in Live.
- **Cross-check and this plan:** XC-12: owns what each edit does to frozen material; XC-15: ON7's format home is the one tap placement.

#### TM-IC-01 The exact time base and SMPTE timecode

*decided* · size M · evidence documented · lane engine, win · milestone M5 · after: none

- **Need and current decision:** ADR-0206 d3's P1 converter (254,016,000,000 ticks per second to and from frames at any ADI rate, nanoseconds and musical ticks, with a range check) and SMPTE drop-frame and non-drop timecode, needed by MTC, AAF, OTIO and video; a drop-frame flag; the `external_sessions`/`external_links` reservation. ADR-0206 d3 (integer arithmetic, P1); ADR-0004; ADR-0144 d3; BUILD-BRIEF Phase D items 19-20.
- **On main (audited at ca0290c2):** Not built: no `time_base` files; `project` has `timecode_origin_ns` and (its source file) only; schema 1.19 has no `external_*` tables.
- **Proposed contract:** One pure integer component in the time library (`src/adi/time_base.{hpp,cpp}`); the drop-frame algorithm on integer frame counts; 29.97 is exactly 30000/1001; anything beyond `Tick::MAX` refused. Drop-frame lives in a new table, not a column on `project`. `external_sessions` and `external_links` are their own additive minor (Phase D 20).
- **Reuse:** IC8 (OpenTimelineIO 0.19.0's drop-frame vectors and 13 sample files, vectors only; FilmCraft's time tests as vectors).
- **Reject or defer:** Reject OTIO's double-based `RationalTime` and Ardour's truncated 29.97 column.
- **Research IDs:** FR-010, DR-13-17, DR-13-18, C4-016; INTEROP-ARTCRAFT §3.
- **Unresolved test (NOT RUN):** INTEROP-ARTCRAFT §3's tables (an NTSC frame is 8,475,667,200 ticks; the 125 ns grid is 31,752 ticks; `Tick::MAX` refused) and OTIO's drop-frame vectors.

#### TM-IC-02 MIDI files as Live (P2)

*decided* · size M · evidence documented · lane engine, window, win · milestone P2 · after: TM-IC-03 (its shared ops)

- **Need and current decision:** Import MIDI File (Create menu, at the insert marker), Export MIDI Clip (Ctrl+Shift+E), the time-signature import option, controllers becoming clip envelopes, several dropped files on one track unless Ctrl is held, MIDI files into take lanes; no options box (Cubase's options behind an Options button). ADR-0203 d7 (A6-03: exactly Live's, no options box, P2), d11 (a MIDI file imports with no box); ADR-0004 (a lossless PPQ).
- **On main (audited at ca0290c2):** Not built: no SMF code; no `extension.write` or `noteExpression.write`; `capture.createMidi` exists only on open PR #446; the window's row for a MIDI file dropped on a MIDI track is open.
- **Proposed contract:** An SMF codec in `src/adi/interchange` (variable-length quantities, running status, raw sysex and meta bytes, 64-bit ticks, SMPTE division, unknown chunks kept). Every reading anomaly is named, never a crash. Import is prepare and commit, one undo step, through a `clip.createMidi` op (the handler of `capture.createMidi` under a neutral name, widened to empty clips, offsets, loops and colour). Unmapped meta events, sysex and chunks are parked in extensions rows, so a re-export is byte-identical; the source PPQ is remembered. Controllers beyond CC 0-127 (pitch bend, aftertouch, program) need SPEC rulings on stream kinds (the column has no CHECK, so no migration).
- **Reuse:** IC7 (libremidi `e27156f9` SMF reader and writer, copied with fixes, the DryWetMIDI corpus of 63 valid and 666 invalid files fetched as test data, its 16 reading policies as named anomalies).
- **Reject or defer:** Reject glib-based libsmf and committed song files. P2 stands (conflict 11).
- **Research IDs:** A-BASE-3.10, A-BASE-5.2, A-BASE-5.2.1, A6H-028, A3-021, A6F-012, A6F-013, A6F-014; M3-EXPORT-INTERCHANGE 5; AWAITING row 13.
- **Unresolved test (NOT RUN):** AWAITING row 13 (what a multi-track file creates; whether its tempo is taken; the export's file type and PPQ); SMF-1 to -4 (the corpus; tick-identical PPQ round trips; byte-identical meta and sysex; Live parity).
- **Cross-check and this plan:** XC-11: IC-02 refers here.

#### TM-IC-03 DAWproject both ways (P1) and the converter rules

*decided* · size L · evidence documented · lane engine, devices, window, win · milestone M5 · after: none

- **Need and current decision:** DAWproject import and export at P1, with a per-field loss report, a lossless re-export of what ADI cannot use, and the rules every later converter follows. ADR-0127 d7 (P1); README commitment 7; RATIONALE §4.3; SPEC §7.4, §9; ADR-0144 d3; ADR-0012; ADR-0036 (the core builds without JUCE).
- **On main (audited at ca0290c2):** Not built, and in no lane's queue: no XML parser, no DAWproject code; the extension and note-expression ops are unbuilt; `rows::Model` has neither table; `ui_look.cpp` holds only 14 Live 11 palette stand-ins.
- **Proposed contract:** A converter ADR: code in `src/adi/interchange/**`, JUCE-free; every importer prepares (worker plus owner snapshot, `CopyIds`) and commits (one undo step, actor `import`, `actor_detail` "<format>:<generator>"); exporters read one `rows::Model`, which gains extensions and note expression. A per-field loss report every time. Unmapped XML is parked in extensions rows and re-exported verbatim. Nested `Clips` and `Warps` flatten to one ADI clip per inner event. Tempo ramps import exactly and play stepped until TM-TT-01. A `Vst3Plugin` is matched by hashing its class UUID the way JUCE's identifier does, and exported with the real class id from the plug-in's preset header; a `plugin_ref_ids` table holds VST3, VST2 and AU ids. A fixed default-theme table maps `#RRGGBB` to the nearest of the 70 palette indexes and parks the exact colour (rendering stays the window's, ADR-0211 d3). The missing ops come first: `extension.write`/`delete` (OPS §9.11, P1), `noteExpression.write` (OPS §9.5, P1), `clip.createMidi`. The devices lane maps the built-in EQ, Compressor, Gate and Limiter; placeholders until they exist.
- **Reuse:** IC1 (Tracktion `b325a9fa` DAWproject codec, ported off JUCE onto pugixml and miniz, its listed defects fixed: MIDI `playStart` written in seconds, a dangling tempo IDREF, markers without a time unit); IC2 (magda-core's adapter and its declared-loss table); IC3 (ProjectConverter's nested-timeline flattener); IC4 (Bitwig's XSDs and scenarios plus openDAW's five real Bitwig files, fetched, never committed); IC5 (pugixml v1.16); IC6 (the VST3 `.vstpreset` container subset); IC12 (openDAW's importer heuristics, design); IC13 (DawVert's Live palette, a cross-check only).
- **Reject or defer:** Reject Tracktion's JUCE glue and its random round trip as an XML fuzzer (ADI writes its own); reject a drop-frame column. Not raised: `.als` and Pro Tools session import (undecided; nothing depends on them).
- **Research IDs:** E24 (the DAWproject README's non-goals), C4-018, DR-13-17; M3-EXPORT-INTERCHANGE 5; M4-C-EXPORT (AAF, OMF and DAWproject loss).
- **Unresolved test (NOT RUN):** DP-1 (every export validates against `Project.xsd` and `MetaData.xsd`); DP-2 to DP-6; DP-7, the director's gate (a Cubase 15 round trip with a VST3 insert, automation and notes; a Bitwig file for `.clap-preset`).
- **Cross-check and this plan:** XC-11: IC-01 refers here; M5 gives DAWproject the queue slot it lacks.

#### TM-IC-04 OTIO and AAF (P2), OMF read (P3)

*decided* · size L · evidence documented · lane engine, win, window · milestone P2 · after: TM-IC-01, TM-IC-03

- **Need and current decision:** OTIO and AAF Edit Protocol import and export (structure, clips with source offsets, markers, clip gain and keyframes as event volume, media by hash, embedded or separate WAV and AIFF with handles), OMF read later; the ArtCraft bridge's Send to ADI and Return to FilmCraft ride on them. ADR-0206 d4, d5; ADR-0127 d7; ADR-0071; ADR-0115; README commitment 7.
- **On main (audited at ca0290c2):** Not built: no OTIO, AAF, OMF or compound-file code on main or in any open PR.
- **Proposed contract:** TM-IC-03's converter rules. AAF import through vendored LibAAF (static, behind a mutex, because it is not reentrant) with Ardour's clip mapping and ADI's own track-level mapping. AAF export through a clean-room compound-file and AAF writer (SMPTE UMIDs, deterministic timestamps, handles, mono breakout), lowered from `rows::Model`. Separate essence files with handles are export-queue jobs, whose project revision is read when each job starts, not when it is queued (`queue.cpp:152-154`, ERRATA-11). OTIO is a JSON subset over nlohmann with linear clips and ticks kept verbatim. `project.setTimecodeOrigin` and `project.setFrameRate` rise from P3 to P2 with them. `.fcproj` and `.scraft` are never read or written.
- **Reuse:** IC9 (LibAAF with its 65-file corpus and `.expected` dumps); IC10 (FilmCraft's AAF and compound-file writer, about 2,360 portable lines); IC8 (OTIO's subset and sample data); IC11 (FilmCraft's OMF reader, P3).
- **Reject or defer:** Reject the AAF SDK and OTIO's C++ library; corpora are fetched, never committed. OMF read stays P3.
- **Research IDs:** C4-016, C4-017, R11X-023, R11X-024, DR-13-17, DR-13-18; M4-C-EXPORT; M3-EXPORT-INTERCHANGE 5, 6.
- **Unresolved test (NOT RUN):** AAF-1 (65 files match their dumps); AAF-2 (an ADI-written AAF reads back through LibAAF, with pyaaf2 as a second reader); AAF-3 (Pro Tools or Resolve, the director's gate); OTIO-1.
- **Cross-check and this plan:** XC-11: IC-03 refers here; it also needs EX-04's WAV and AIFF writers.

#### TM-IC-05 NI Stems import and export, and N-stem results (P1)

*decided* · size S · evidence measured · lane win, engine · milestone M5 · after: TM-MD-01

- **Need and current decision:** NI Stems (`.stem.mp4`: a master and up to four stems) exported as a queue job and imported as a group with one track per stem; separated N-stem results imported as one native action. ADR-0205 d8 (P1); ADR-0191 d4; ADR-0204 d5; ADR-0071; LANES §2.
- **On main (audited at ca0290c2):** Built through #430: (its source file) (211 lines), (its source file) (339), (its source file). At `ca0290c2` Linux and Windows pass and both heavy macOS legs fail ("CoreAudio container/encoder priming mismatch"); on Windows `adi_ni_stems_tests` passes in 0.05 s and prints its "codec unavailable" note only to standard output. N-stem results import is open PR #453.
- **Proposed contract:** Keep the built job: its project revision and naming context are read when the job starts (ERRATA-11). Once TM-MD-01's step zero makes the macOS legs green, prove the Windows Media Foundation leg really runs (`aacCodecAvailable()` true) rather than skipping; the NI reader becomes a strict filter over the general one; merge #453 (N-stem results) after.
- **Reuse:** MD2 (Mixxx fixtures); MD1 (the general demuxer); ADI's own `ni_stems` and `stem_import`.
- **Reject or defer:** A container CDJs read waits for AWAITING row 17.
- **Research IDs:** DR-13-11, R11X-013, A11R-001, ERRATA-11; AWAITING row 17.
- **Unresolved test (NOT RUN):** Both heavy macOS legs green on `adi_mp4_aac_tests` and `adi_ni_stems_tests` at a matching headSha (measured red at `ca0290c2`); a Windows run that shows the codec path executed (today's 0.05 s pass does not show it).
- **Cross-check and this plan:** XC-05: one row with EX-10 as owner.

### A.3 Devices and processing (30 rows; source the devices matrix (win's working file))

#### DEV-02 Zones, chain select and note identity where chains rejoin (level 1)

*decided* · size M · evidence documented · lane engine, win · milestone P2 · after: DEV-01, DEV-18

- **Need:** Live's zones at each chain's input: key, velocity and chain-select zones on Instrument and MIDI Effect Racks; chain-select only on Audio Effect Racks; none on Drum Racks. Fades attenuate velocity in MIDI Effect Racks and the chain's output in Instrument and Audio Effect Racks. Also needed: the "Chain Selector Filters MIDI Ctrl" option; exclusive and crossfaded banks; a note-off always reaches the chain that took its note-on.
- **Decision now:** ADR-0060; FEATURES §6 "Racks: instrument / effect / drum" (`device_chains` with key/vel/chain zones), P2; ADR-0203 d1.
- **On main (audited at ca0290c2):** zone BLOBs exist ((its source file)). Their encoding is given only in a schema comment, and `rows::Model` does not read them. There is no router node and no transform phase.
- **Proposed contract:** **Encoding:** the zone BLOB is exactly 4 bytes (low, high, low_fade, high_fade, with low ≤ low_fade ≤ high_fade ≤ high; NULL is the full range). That matches Live's own Sampler zone layout. **Admission:** a note must pass every applicable zone. **The router:** it runs in the transform phase (DEV-18), before splits. The selector is a parameter of the rack device, so it can be automated and mapped. **Note identity:** a MIDI Effect Rack's chains get per-chain note ids where they rejoin. A note-off follows its note-on even after the zones move. **Fade law:** both the gain and the power law are built. Live's measured law becomes the default.
- **Reuse:** RK3 (sfizz's crossfade maths, copied with attribution, and its admission and choke tests); RK6 (DawVert's zone-layout cross-check); RK2 (openDAW's re-id where chains rejoin, design only); the #207 hook through DEV-18.
- **Reject or defer:** sfizz's 1/127 normalised-velocity offset must not leak into integer zones. Bitwig's Selector modes (round-robin and the rest) are not Live's and are not proposed.
- **Research IDs:** A6H-042, A6H-043, A3-026, A3-027, A8D-017; M6-A6H-042/043; B9M-034/035 (Bitwig Layer/Selector, comparison only).
- **Unresolved test (NOT RUN):** the sfizz key-fade vectors: 1..5 gives 0, .5, .70711, .86603, 1 under the power law and 0, .25, .5, .75, 1 under the gain law; a note-off reaches its chain after the zones change; **Live 12 readings:** the key and velocity fade law; the chain-select output fade law; the default of "Filters MIDI Ctrl"; whether two chains of a MIDI Effect Rack send duplicate notes into one instrument (M6-A6H-042/043, NOT RUN).

#### DEV-03 Rack structure: ops, exact undo, copy and paste (level 1)

*decided* · size M · evidence documented · lane engine, win · milestone P2 · after: none

- **Need:** Group (Ctrl/Cmd+G), Group to Drum Rack and Ungroup; drag into and out of racks; the chain ops: create, delete, reorder, rename, zones, mute, solo, volume, pan; Delete then Undo of a populated rack restoring it exactly; rack copy, paste and Duplicate with fresh ids and remapped internal mappings; a rack moved into itself refused.
- **Decision now:** OPS.md §9.7 (twelve rack ops, P2); ADR-0003; ADR-0110; ADR-0060; ADR-0203 d10.
- **On main (audited at ca0290c2):** Only `chain.create` and `chain.delete` exist. The inverse of `chain.delete` restores no devices, zones, mute or solo. **`device.remove`'s inverse loses a rack's contents:** its nested chains and devices, its macros, and the mappings that targeted the removed device. That is a data-loss undo for any project that holds a rack. `device.move` has no cycle refusal. A rack paste is refused (`device_copy.cpp:83`). #401's `track_image.cpp` already restores a group holding a rack exactly, but it is rooted at tracks only.
- **Proposed contract:** **Undo:** every structural edit is one transaction with a captured inverse. `device.remove` and `chain.delete` capture the whole subtree, including params, state hashes, panels, routes, macros and the mappings that target the removed devices, and their undo restores the digest exactly. **Refusals:** `device.move` refuses a cycle. **Separate transactions:** Group, Ungroup and reparent are each their own transaction (N11H-006/007/008). **Delete:** it reconnects only an unambiguous serial path (N11H-013). **Paste:** it is a graph fragment with fresh ids and remapped internal mappings (N11H-015). This lifts #394's refusal.
- **Reuse:** #401's FK-walking row image (`Image::collect`, `row_capture`), generalised to any root as a new `row_image.cpp`; RK4 (magda-core's command tests: wrap is one undo step, a self-nest is refused, cross-track moves retarget links, rack presets retarget macro links).
- **Reject or defer:** ERRATA-11: Reason's chapters contradict each other on Shift, so no universal move-modifier rule is adopted; Live's own gestures govern. Reject undo-manager command objects (MAGDA); ADI uses ops with captured inverses.
- **Research IDs:** N11H-006, N11H-007, N11H-008, N11H-013, N11H-015, N11H-016; A5H-020; hosts11 rack-relationship-rewrite; hosts10 portable-rack-boundary.
- **Unresolved test (NOT RUN):** remove a populated rack, then undo and redo: the digest is identical (as `test_track_delete`'s full inverse); moving a rack into itself is refused; Group and Ungroup are each one undo step; a rack paste gets fresh ids; OPS §6.3's 4 MB `lossy_undo` threshold holds with a large rack; hosts11 rack-relationship-rewrite, NOT RUN.
- **Cross-check and this plan:** XC-22: paste goes through the one clipboard model and id allocator.

#### DEV-04 Drum Rack (level 1)

*decided* · size L · evidence documented · lane engine, devices, window · milestone P2 · after: DEV-01, DEV-02, DEV-03, DEV-21

- **Need:** the Drum Rack as a container: 128 pads, with Receive, Play and All Notes; 16 choke groups; up to six return chains, with post-fader sends, sending to the rack or to a Set return; Multi pads; a pad swap that does not rewrite clips; a sample drop makes a OneShot chain; Preview.
- **Decision now:** FEATURES §6 racks (drum), P2; ADR-0060; ADR-0169 d4 (OneShot); ADR-0177; LANES §4b item 3 ("Drum Rack as a container").
- **On main (audited at ca0290c2):** nothing built. The schema has no Receive, Play or Choke, no return chains and no chain routing endpoint. OneShot is a draft (#278).
- **Proposed contract:** **Drum mode of the rack router (DEV-01/02):** Receive (NULL is All Notes) then Play translation. **Choke:** router-tagged choke events, sorted before a note-on at the same position; the chain strip fades the choked chain. The release shape and simultaneous-hit arbitration stay provisional until A8D-004 is measured. **Returns and sends:** return chains are rack branches; sends are post-fader. A return goes to the rack's main out or to a Set return, through rack-device parameters or a chain endpoint (win's R0 choice). **Pads:** they show the chains' Receive notes. A swap rewrites mappings, never clips.
- **Reuse:** RK2 (openDAW's `EVENT_CHOKE` model); RK3 (sfizz's choke arbitration tests); RK4 (MAGDA's Drum Grid low/high/root note); #278's OneShot for pads.
- **Reject or defer:** reject sfizz's directed `off_by` as the semantic, because Live's choke is symmetric within a group. The drum tab with beatbox-to-MIDI stays P3. Kit instruments (Impulse) are excluded.
- **Research IDs:** M8-A8D-001..025; A6H-044..047; A8D-004 (choke undocumented); A8D-007/008, A8D-010..014.
- **Unresolved test (NOT RUN):** A8D-004: the choke release with a one-shot OneShot, and with a long-release plug-in; simultaneous-hit arbitration; whether choke cuts a reverb inside the choked chain; A6H-045: sends are post-fader; a pad swap leaves clips unchanged; M8-A8D-001..025, NOT RUN.

#### DEV-05 A sidechain key into a device, with Live's taps (level 1)

*decided* · size M · evidence documented · lane win, engine · milestone M4 · after: none

- **Need:** a key routed into a particular device in a chain: Compressor, Gate, Auto Filter, Vocoder, or any plug-in with a sidechain bus. It uses Live's source chooser and tap (Pre FX, Post FX, Post Mixer).
- **Decision now:** ADR-0203 d3 (three taps; the two early taps P1 "with sidechain routing"); FEATURES §2 "Sidechain routing" P1 and "Sidechain and track-to-track tap points" P1; ADR-0058 d5 (sidechains compensated); ADR-0043 (a live sidechain prevents suspension); ADR-0057's open item (VST3 aux bus).
- **On main (audited at ca0290c2):** The planner refuses any endpoint that is not a track (`plan.cpp:127-131`). A track-to-track sidechain row lands on the destination track's input junction (`realize.cpp` ~303), not on a device. The routing table has no tap column. The state map's ON7 asks win for a format home.
- **Proposed contract:** **Rows:** a routing row may name a device's key input (`dst_kind 'device'`). **Taps:** they live in an additive side table, for example `routing_taps(routing_id, tap)`, never a new column (ADR-0144 d3). **Planning:** the planner places the key edge into that device node's sidechain bus, compensated like every edge. **Tap behaviour:** Pre FX and Post FX read the source chain at those points; only Post Mixer follows fader, mute and solo. **Hosts:** they negotiate the plug-in's aux input (VST3 bus layout, CLAP audio-ports).
- **Reuse:** none harvested (no harvest area covered it). In tree: `Bus::Sidechain` edges and their compensation in `graph.cpp`; `return_send_taps` (1.15) as the side-table precedent.
- **Reject or defer:** defer the Quick Kick Sidechain helper (ADR-0209, P2), which woke the withdrawn slots (5 November ruling). Defer N-bus (ADR-0113, P2).
- **Research IDs:** A5H-036; B-G01; N11H-040; AWAITING row 7 (the sidechain chooser's starting tap).
- **Unresolved test (NOT RUN):** a compressor fixture keyed from another track through each tap renders aligned, with a latency plug-in on the key path; suspension stays off while the key is live; a VST3 plug-in with an aux input receives the key; Live's starting tap (AWAITING 7).
- **Cross-check and this plan:** XC-07: one row with MX-24, built after XC-15's format placement.

#### DEV-08 Automation and modulation heard on ADI's own devices (level 2)

*decided* · size S · evidence inferred · lane engine · milestone M1 · after: none

- **Need:** a device automation lane on a built-in (Utility's Gain, Auto Filter's cutoff) is heard. `ParamMod` adds to the base value, and a value arrives within one Pd block of its point.
- **Decision now:** ADR-0165 (device lanes play); ADR-0188 d3 (Pd parameters: modulation evaluated by the DAW, sent once per Pd block); ADR-0196 (normalised on the wire); FEATURES "Track and device automation" P0; FEATURES "Pd parameters ..." P1.
- **On main (audited at ca0290c2):** **a defect.** ; The session binds device lanes for every non-CLAP format (`session.cpp:822`). But `BuiltinDevice::process` (`builtin_host.cpp:110-122`) only applies stored targets and routes note streams. `MpeRouter` drops `ParamValue` and `ParamMod` (`mpe_output.cpp:279-280`). So automation on all 17 built-ins is bound and silent.
- **Proposed contract:** the built-in device, and later the patch device (DEV-10), consumes `ParamValue` and `ParamMod` from `io.events` at segment start: **ParamValue:** sets the base, normalised to natural through the declaration's curve. **ParamMod:** adds an offset for that block. Both go out through the pre-resolved `sendParameter`, before the native core or the Pd tick. Echo and gesture rules are as in ADR-0196.
- **Reuse:** in tree: `natural()`/`normalized()` in `builtin_host.cpp`; `LibPdEngine::sendParameter`; the VST3 and CLAP hosts' handling (`clap_host.cpp:101`) as the pattern.
- **Reject or defer:** —.
- **Research IDs:** A11N-002, A11N-004; B-M01.
- **Unresolved test (NOT RUN):** AR4. A `ParamValue` lane on Utility's Gain is audible from segment start, within one Pd block of its point. `ParamMod` adds to the base value. A lane on a built-in plays the same before and after save and reopen.

#### DEV-09 Pd runtime safe and 64-bit capable (level 3)

*decided* · size M · evidence inferred · lane devices, win · milestone M1 · after: none

- **Need:** before any user patch loads: no native code from any path; a patch cannot quit or stall ADI; the audio thread never waits or allocates for Pd; the 64-bit Processing Precision mode builds the Pd tier in double.
- **Decision now:** ADR-0188 d8 (externals compiled in; nothing loadable on Pd's search paths); ADR-0203 d8 (embedded libpd only; a patch with a loadable library beside it is refused); ADR-0183 d18; ADR-0010; ADR-0095; ADR-0207 d2 (the 64-bit mode builds the Pd tier in double: ON11).
- **On main (audited at ca0290c2):** solid in tests only. Defects: the external guard inspects `[declare]` only, so absolute and (its source file) object names, and dynamic patching, bypass it; `pd quit` (it calls `exit`) and `pd exit` (it stalls every instance) are unfenced; the latency receiver is bound and never unbound (`pd_engine.cpp:324`); the print hook takes `g_consoleMutex` on whatever thread prints (`:71`, `:882`); `libpd_read_array` runs on the audio thread (`:547`), and its range error leaks the lock; pinned Pd has an `#A` use-after-free; the build has no `PD_FLOATSIZE` option.
- **Proposed contract:** 1. **Native loading closed at the source.** `s_loader.c`'s `LoadLibrary`/`dlopen` become refusals with a named console line. `quit`, `exit`, `open`, `add-to-path` and preference writes are fenced. 2. **One trylock tick per Pd tick.** If the lock is busy, the tick is skipped, outputs silence and is counted; it never waits. Every delivery happens inside the lock, and `sys_pollgui` never runs on the audio thread. 3. **Cross-thread traffic:** ; printing goes through a queue, drained on the message thread; message-thread sends to the audio thread go through an SPSC ring; published arrays are read without the lock-leaking path. 4. **The `#A` overlay.** ; 5. **The latency binding is unbound on close** (#173). 6. **A `PD_FLOATSIZE=64` build for the 64-bit mode,** with each device stating `processesDouble` truthfully.
- **Reuse:** PD1 (libpd and Pd internals already pinned: the hooks, `z_queued`, the PROCESS macro port, the loader seams); PD2 (plugdata-pd's `#A` fix as an overlay, plus the dangling-binding, fuzz and state round-trip scenarios); MF4 (#173's unbind, re-applied); PD4 (refuse absolute and (its source file) names in the allowed-object walk).
- **Reject or defer:** defer `[pd~]` out-of-process patches. Defer a file and network access policy for shared patches (`[file]`, `[soundfiler] write`, `[netsend]`) until patches are shared; note it now.
- **Research IDs:** A11N-009, A11N-010, A11N-011; PD-RACK-EXPORT-FIT-11; BITWIG-GRID-PD-FEASIBILITY (its realtime-safety blocker row); plugdata's DanglingBindingTest scenario.
- **Unresolved test (NOT RUN):** **AR7:** a planted library never loads by absolute name, by a (its source file) name, or from a message box; quit and exit are fenced. **AR8:** device A never waits while B opens a 200-object patch; an editor holding the lock for 50 ms makes A skip and count; a `[print]` on a 1 ms `[metro]` makes zero audio-thread allocations, counted at the malloc level; shrinking a published table does not deadlock. **AR18:** `#A` under ASan. **AR17:** the 16 device suites pass at `PD_FLOATSIZE=64`. **#173's stress test:** 40 open/close cycles of two engines.
- **Cross-check and this plan:** XC-39: LANES first names an owner for the Pd runtime files (devices K2).

#### DEV-16 `.amxd` translator and Live Suite's Max for Live devices (level 3, P2)

*decided* · size L · evidence unknown · lane devices, engine · milestone P2 · after: DEV-10, DEV-07

- **Need:** Live Suite's Max for Live devices as native ADI devices (LANES §4b item 3). A translator that converts a simple `.amxd` device into an ADI Pd device by itself. Other devices go to the AI with a prompt, and the result is checked.
- **Decision now:** ADR-0203 d8 (A4-11: the translator is an addition at P2); FEATURES ".amxd translator with a pre-flight scan" P2; LANES §4b item 3.
- **On main (audited at ca0290c2):** not built. There is no `.amxd` reader, and no Suite device is built.
- **Proposed contract:** **Pre-flight:** an allowlist scan of the patcher's boxes. **Full match:** the device converts into a user Pd patch device (DEV-10), with Max's right-to-left order made explicit. **Otherwise:** the AI proposes through the Propose tier, and the result must pass the allowed-object walk. **Suite's devices:** LFO, Envelope Follower and Shaper become native modulator devices on DEV-07's routes.
- **Reuse:** PD4 (the allowed-object walk serves as the pre-flight). Nothing was harvested for the `.amxd` format.
- **Reject or defer:** this is P2 (Phase F). A Max application path stays rejected (ADR-0203 d8, ADR-0204 d5).
- **Research IDs:** the Live 12 manual chs. 31-32 (Max devices, Max Modulators, Max MIDI Tools); the A7B rows for the Max audio effects (Align Delay, Envelope Follower, LFO, Shaper); the A7M rows for the Max MIDI effects.
- **Unresolved test (NOT RUN):** a corpus of simple `.amxd` devices, the director's own if he has some, converts and renders equal to Live within a stated tolerance; a device that cannot convert is refused, naming its boxes.

#### DEV-19 Live's fourteen MIDI effects

*decided* · size L · evidence documented · lane devices, engine, measurement seat, win · milestone M6 · after: DEV-17, DEV-18; DEV-07 only for the three modulators

- **Need:** all fourteen as built-in devices that work as Live's: **P1:** Arpeggiator, Chord, Scale, Pitch, Velocity, Note Length, Random. **P2:** CC Control, Note Echo, MPE Control, MIDI Monitor. **P2, after modulation:** Expression Control, Envelope MIDI, Shaper MIDI. Also needed: random behaviour that is Live's by default, plus a visible keep-result switch; Use Current Scale; MIDI effects placed before the instrument.
- **Decision now:** ADR-0169 d1-d6 (d3 amended in effect by ADR-0203 d7; see K5); ADR-0203 d7 and d8; FEATURES §6 MIDI effect rows; BUILD-BRIEF Phase D item 12.
- **On main (audited at ca0290c2):** none built. The catalogue has 18 audio entries only, and `builtin_insert` writes subtype 'effect'. #207 has been open since 30 September on a stale base, and cannot be rebased: its `Control = 5` clashes with main's `Controller = 5`, and it uses another factory and another ownership protocol.
- **Proposed contract:** **Devices:** native graph devices (catalogue kind `midi_effect`, subtype 'midi_effect', `org.adidaw.builtin.*` uids). Kernels live in `src/adi/dsp`, with fixed state and no audio-thread allocation. **Order:** inserted before the instrument (Live manual §23, p. 442). **Randomness:** it varies on each pass by default. The keep switch stores the seed as one `device.setParam` op and reseeds per note from (seed, position, key), so a kept result repeats from any start point, loop or range export. **Use Current Scale:** it reads a per-track scale context that the engine publishes. **Governance:** one ADR and one parity checklist per device (ADR-0169 d6). Defaults stay labelled "unverified against Live" until they are measured.
- **Reuse:** MF1: #207's kernels (Pitch, Chord, Random, Scale, Velocity, Note Length, Note Echo, Monitor, CC Control, Arpeggiator and MPE Control), ported from (its source file) `7f421f4d` with the ten listed fixes, plus the Diverge, Random Once and prepare fixes; MF5: openDAW's `walk_index` (16 of Live's styles) and its per-note reseed rule.
- **Reject or defer:** park Microtuner (Live 12's tunings are Set-level, and ADR-0178 covers them); Expression Control, Envelope MIDI and Shaper MIDI wait for DEV-07; Groove waits for the groove pool (P2, no schema); reject tracktion's MidiModifier, which transposes releases with the current value, and MIDI-1 key ownership.
- **Research IDs:** A7M-001..A7M-053; A7M-122, A7M-128, A7M-130; A5H-020; devices7 `note-lifetime-sustain.mid` and `mpe-generated-note-identity.mid`; B9M-002/005 (comparison only); the harvest's proposed Live rows LM1-LM8.
- **Unresolved test (NOT RUN):** the A7M-002..035 and A7M-045..053 checklists on the side-by-side gate; A7M-130: two renders differ with the switch off and are identical with it on; the devices7 MIDI probes through each P1 effect; LM1-LM8 on Live 12.4: arpeggiator grid alignment; offs on delete; same-key overlaps; export randomness; Hold; the Style list for 3, 4 and 7 notes; MPE forwarding. All NOT RUN.
- **Cross-check and this plan:** XC-25: the Arpeggiator and MIDI Tools' Arpeggiate share one ordering function; XC-34: Use Current Scale needs TU-01 and MI-06; #207 and #208 closed as "to be ported".

#### DEV-21 OneShot and Sampler (the two authorized instruments)

*decided* · size M · evidence documented · lane devices, engine · milestone M6 · after: none

- **Need:** OneShot (Live's Simpler, P1) and Sampler (P2) as native instruments.
- **Decision now:** ADR-0169 d4/d5; ADR-0203 d8; FEATURES "OneShot" P1 and "Sampler" P2; ADR-0192 d4 (sample slots); ADR-0127; ADR-0178.
- **On main (audited at ca0290c2):** not on main. #278 is a draft, based on main and green on its branch per the state map. #208 sits on the stale #207. The catalogue has no instrument kind, and nothing dispatches instruments.
- **Proposed contract:** **Catalogue:** kind `instrument`, meaning `EventFlow::Consume`, fixed at construction. **Dispatch:** native, in `makeBuiltinDevice`. **Samples:** sample slots through #414's ops. **Prepare:** idempotent (ADR-0090 d5). **Sampler:** its zones are published immutably, on OneShot's voices. **A7M-127:** Sample Selector and Offset latch at note-on.
- **Reuse:** MF2: land #278, then port #208 onto it, fixing idempotent prepare and the macOS assertion.
- **Reject or defer:** Live's other instruments (Analog, Collision, Drift, Electric, Meld, Operator, Tension, Wavetable, Impulse) stay outside this matrix, because instruments are excluded except authorized scope, although LANES §4b lists them.
- **Research IDs:** A7M-058..A7M-131 (Sampler/Simpler); A7M-127, A7M-131; A7R-008, A7R-009; devices7 `sampler-repeat-and-velocity-boundaries.mid`.
- **Unresolved test (NOT RUN):** #278's suites and #208's 28 checks on main; A7M-127; the sampler MIDI probe; measured Live parity (#278's draft gate). NOT RUN.

#### DEV-22 Live's audio effects: measured parity, the queue, Hybrid Reverb

*decided* · size XL · evidence documented · lane devices, measurement seat, win · milestone M6 · after: DEV-08, DEV-05

- **Need:** each of Live's audio effects is a built-in that behaves as Live's, tested against Live's render. The Dream says so; BUILD-BRIEF §4 wrongly says it is done (K7). The 16 on main reach measured parity. The 21 queued are built in the manual's order, Compressor first. Hybrid Reverb gets its native factory. The DJ Filter (P2) stays on Auto Filter's cores.
- **Decision now:** ADR-0169; ADR-0187 d1-d3; ADR-0188 d8; ADR-0203 d4/d8; ADR-0108 (the side-by-side gate); ADR-0204 d1 (the DJ Filter shares the channel EQ's cores: affected by the 5 November ruling, K1); LANES §4b items 3-5.
- **On main (audited at ca0290c2):** 16 Live effects plus Chord Comb and Color Cab as canvases, with native cores for 17. Hybrid Reverb has no native factory and is listed unavailable. The synthetic suites are green on the Linux and Windows legs, but no device has a Live render. Compressor has 46 tracks measured on Live 11.2.7 and no code. The devices lane has not logged since 28 October (XC-46: the fallback seat does this row's work until it logs).
- **Proposed contract:** each device is: a canonical canvas with a native core, adding no adapter delay; declared parameters with fixed ids; latency reported, and kept while bypassed; accepted on a measured Live 12 render, one per PR. The DJ Filter stays on Auto Filter's cores. ADI-Q's cores arrive only with the director's unified feature.
- **Reuse:** LD5 (a true-peak detector path in `dsp::LookaheadLimiter`, shared with Live's Limiter). No DSP takes were harvested for the effects.
- **Reject or defer:** reject parity claims from synthetic oracles. Instruments other than OneShot and Sampler are excluded here.
- **Research IDs:** M7-A7A-001..103; M7-A7B-001..098; A7R-001..007; A5C-021 (bypass keeps latency); the C7 censuses; the devices7 probes (threshold-stairs, asymmetric-bursts-tail, alias-offset-multitone); AWAITING 9 and 10.
- **Unresolved test (NOT RUN):** Live 12 renders of the 16 (none made, and no Live 12 GUI seat is assigned); AWAITING 9 (Shifter's Pitch-mode latency) and 10 (Auto Filter side by side); the 358 M7 cases, NOT RUN.
- **Cross-check and this plan:** XC-27: the DJ Filter stays on Auto Filter's cores; BUILD-BRIEF §4's "measured against Live" claim corrected (devices K7).

#### DEV-24 CPU Load Meter and Performance Impact

*refinement (was decided; XC-08)* · size S · evidence documented · lane engine, window · milestone M7 · after: none

- **Need:** Live's CPU Load Meter in the control bar: Average and Current, the time to process a buffer over the time to play it. Also Live's per-track Performance Impact (six rectangles), feeding ADI's Performance panel (R-24).
- **Decision now:** ADR-0203 d1 (Live parity); ADR-0125 R-24; the parity-7.2 control bar rows.
- **On main (audited at ca0290c2):** the meter is drawn with no data (`ui_controlbar.cpp:199`). Performance Impact is disabled (`ui_shell.cpp:953`). There is no per-node timing.
- **Proposed contract:** **Callback load:** from JUCE's `AudioDeviceManager` (`getCpuUsage`, `getXRunCount`), read raw so values above 100 % show. Average is smoothed; Current and the overload warning work as Live's. **Per-track impact:** per-slot block timings, summed per track through the realiser's chain map and published at UI rate. They are summed per physical buffer when one buffer renders as several slices.
- **Reuse:** GT4 (Ardour's `DSPLoadCalculator` and its test, a verifier addition); GT6 (JUCE's `AudioProcessLoadMeasurer`, already in tree).
- **Reject or defer:** —.
- **Research IDs:** A5C-019; M5-A5C-019; M-CPU; Live 12 manual §37.1.1, §18.9, §37.1.3.
- **Unresolved test (NOT RUN):** GT-8: the meter follows §37.1.1, and freezing the heaviest track lowers it; Live 12 and ADI compared on the director's Windows workstation; confirm that Live 12 has no Multicore switch.
- **Cross-check and this plan:** XC-08: one row with MX-28, a refinement (Live's way under d1; a FEATURES row added).

#### DEV-26 The director's plug-ins load as ordinary plug-ins

*decided* · size M · evidence inferred · lane win, engine, window · milestone M1 · after: DEV-25

- **Need:** per the 5 November ruling, his plug-ins load in ADI as ordinary VST3 (or CLAP) plug-ins, like any third-party plug-in: ADI-Q, ADI-L, ADI-Clip, ADI Still and those to come. There is no slot and no separate built-in device. The unified built-in feature waits for his list and is not designed here.
- **Decision now:** the ruling of 5 November (N16 withdrawn; the slot parts of ADR-0204 d1 and ADR-0209 d1 superseded once win records it, K1); ADR-0041/0052; ADR-0110/0124/0141/0142; ADR-0150/0154; ADR-0204 d3 (measurement clones welcome); ADR-0203 d8 (a plug-in that fails to start passes sound and is named; the Configure warning).
- **On main (audited at ca0290c2):** VST3 and CLAP hosting, parameter and preset ops, the panel and plug-in editors (#391, N8) are on main. The browser's Plug-ins category waits for the scanner. His builds have never been loaded in ADI. They sit untracked in his plug-in tree, with no CLAP target.
- **Proposed contract:** his VST3 builds go through the ordinary path: **Discovery:** scanned (DEV-25) and listed under Plug-ins. **Insertion:** one undo step. **Editing:** their windows open in the floating host; a knob moved in their window is one op, and a preset is one op. **State:** it round-trips through save and reopen. **Latency:** ADI-L's lookahead is compensated, and kept while bypassed. **Missing plug-in:** a placeholder that keeps its state (ADR-0011). Nothing in ADI depends on their source until he declares them all finished. Auto gain-match for any plug-in stays P2, with a per-device flag in a side table.
- **Reuse:** none beyond DEV-25's takes; in tree: `vst3_host`, `clap_host`, `editor_host_component` (#391), `ParamOps`.
- **Reject or defer:** do not design the unified feature, the Mix View program's native slot contract v0, or a built-in that hosts the VST3. A CLAP target is optional, not required. LV2 is P2. Runtime sandboxing stays rejected (D-03).
- **Research IDs:** N11H-028..031, N11H-033; A5H-021..023 (A/B is for built-ins only); A11N-009; B3-005; B-G07.
- **Unresolved test (NOT RUN):** on the director's Windows workstation, in `ADI.exe` on a desktop that is not the director's: ADI-Q 0.3.17, ADI-L 0.1.6 and ADI-Clip 0.1.3 scan, load, edit in single undo steps and open their windows; their state hashes survive save and reopen unchanged; ADI-L's lookahead nulls against a dry track; a missing build becomes a placeholder; AWAITING 11 on Live.
- **Cross-check and this plan:** This plan splits the check: a headless half now (adi_play-style harness with ADI_HOME) and the editor-window half in the window lane's seat or a Windows Sandbox with his OK (§2, M1).

#### DEV-27 AudioGridder remote devices

*decided* · size XL · evidence unknown · lane engine · milestone M7 · after: DEV-25

- **Need:** a plug-in on another machine is a device like any other, found on the network and listed beside local plug-ins. A project opened without the server loads the plug-in locally. This is the one named delay exception.
- **Decision now:** ADR-0053; ADR-0083 (a native client; the server forked to host CLAP). Its open items: the wire format must carry CLAP's events and per-note expression; latency; discovery; catalogue caching; an unreachable server; ADR-0134 d3 (remote, then local, then placeholder); ADR-0145 d6 (mDNS, a catalogue cache per application); ADR-0203 d4/d19 (P1, one buffer late); BUILD-BRIEF Phase D item 14.
- **On main (audited at ca0290c2):** not built (comments only).
- **Proposed contract:** **Identity:** a remote device is a `DeviceInstance` (`devices.remote_host_id`; the format is unchanged). **Threads:** the network stays off the audio thread, through an SPSC queue. **Timing:** it is pipelined one block behind, with that latency declared. A missed deadline outputs silence and bumps a visible counter. **Unreachable server:** the local plug-in, else a placeholder. **Security:** the authentication posture is stated before shipping (LAN only).
- **Reuse:** none harvested; AudioGridder itself, per ADR-0083.
- **Reject or defer:** a wrapper plug-in stays rejected (ADR-0053). PTP (ADR-0107) waits for a measured need.
- **Research IDs:** B3-009, B3-010 (Bitwig's hosting modes, comparison only); N11H-032.
- **Unresolved test (NOT RUN):** a spike answering ADR-0083's open items: the wire format carries CLAP note expression, and the link latency is measured; a remote Surge XT matches the local one within the declared block; losing the server mid-play counts misses and never glitches another track.
- **Cross-check and this plan:** XC-40: in the one network stack and the Privacy list of what leaves the machine.

#### DEV-29 Export loudness: NI Stems guard, "together", brickwall, more targets

*decided* · size M · evidence inferred · lane engine, devices, window, win · milestone M5 · after: DEV-28

- **Need:** LUFS targeting as a job property, with Live's default (no normalising) and REAPER's and Cubase's depth: Peak (Live's), LUFS-I, RMS-I and max M/S targets; a true-peak brickwall option; normalising "as if played together"; NI Stems normalised only once verified after AAC.
- **Decision now:** ADR-0203 d14 (−14 LUFS and −1 dBTP as editable defaults; off by default); ADR-0205 d5, d8 and d9 (the P1 part with the queue); ADR-0071 d5; FEATURES "Export LUFS targeting" P1; ERRATA-11.
- **On main (audited at ca0290c2):** `normalization.*` provides Off, Peak and Loudness: one double capture, up to 4 re-encodes, the encoded true peak verified, uniform gain only. `ni_stems.cpp` never reads `job.normalization`, so a normalised NI Stems job is silently not normalised. The refusal #407 asked for was lost in the merge. There is no brickwall, no RMS-I target and no "together".
- **Proposed contract:** **Now:** `validateNiStems` refuses an enabled normalization. **(a) After-AAC check:** decode the encoded `.stem.mp4` streams and measure them with DEV-28's meter. Lift the refusal only when that passes. **(b) "Together":** one gain from the sample-wise sum of the time-aligned captures, measured by one meter. That is REAPER's "play together", not libebur128's `_multiple` concatenation, which serves only the separate "common gain / one long file" grouping. **(c) Brickwall:** an off-by-default true-peak limiter option. `LookaheadLimiter` gains a true-peak detector; a refine loop (≤ 4 trials, ±3 dB steps, 0.02 LU plateau) runs inside `NormalizationCapture`; the encoded true-peak check stays the final gate. **(d) More targets:** RMS-I and max M/S, from DEV-28. **Job capture:** as ADI already does it, values at enqueue and the project revision and names at job start (ERRATA-11). **Master:** WITH master FX and VIA master stay distinct (R11X-002).
- **Reuse:** LD5 (FilmCraft's refine loop and the limiter's true-peak detector; one limiter change, shared with export EX4); LD3 (libebur128 `_multiple`, for the common-gain variant only).
- **Reject or defer:** reject an enqueue-time snapshot (ERRATA-11; changing it would be a policy decision). Reject one checkbox for both WITH-master and VIA-master. Normalising stays off by default. ADI-L stays the director's own limiter; the brickwall is an export safety option only.
- **Research IDs:** R10H-073; R11X-002, R11X-025, R11X-034, R11X-043, R11X-047; A11R-001; M3-EXPORT-INTERCHANGE; A5C-022.
- **Unresolved test (NOT RUN):** an NI Stems job with normalization on is refused (no test today); WAV and AAC exports land within 0.5 LU of target, with true peak ≤ ceiling + 0.1 dB; three stems "together" are checked by libebur128 and ffmpeg; REAPER 7.79's render-normalize compared (−14 LUFS-I at −1 dBTP, brickwall on and off, three stems together). NOT RUN.
- **Cross-check and this plan:** XC-04: EX-05 owns these items; the refusal lands once, as EX-10's, in M0.

#### DEV-01 Racks play (level 1)

*refinement* · size L · evidence documented · lane win, engine · milestone P2 · after: none

- **Need:** Instrument, Audio Effect and MIDI Effect Racks in the signal path: parallel chains, each chain's devices in series; each chain with volume, pan, activator and solo; summed at the rack's output; nesting without limit (Live manual, ch. 24).
- **Decision now:** ADR-0060 d1-d5 (direction); ADR-0122 d8 (racks skipped and named); ADR-0203 d10 (racks P2); FEATURES §6 "Racks: instrument / effect / drum" P2; ADR-0058 and ADR-0043 inside racks.
- **On main (audited at ca0290c2):** storage only. Schema 1.19 stores `device_chains` (under a rack device or a track), the zones, `is_rack` and `rack_kind` (no values defined), and the macro tables. The session skips every rack and every device inside one, named and counted (`session.cpp:236-252`), so an Instrument Rack is a silent track. The realiser knows serial chains and sends only.
- **Proposed contract:** **The rack is spliced into the track's graph, not a nested `Graph` node:** ; a router node feeds a gate per chain; each chain's devices run in series into a chain strip; the strips meet at a rack sum, summed in double. A nested graph would break per-block event sources, the global splits, the workers and ADR-0092's edge history. **ADR-0060's other decisions stand:** ; d2: the latency is the inner graph's, compensation aligns every rejoin, and the tail is the maximum of the children's; d3: macros are modulators; d5: hardware maps go through `controller_maps`. **Rebuilds:** `ChainKey` gains Branch and Join legs, so edge history survives rebuilds. **Chain changes:** a mute, solo or volume change ramps 5 ms at the chain strip with no rebuild. A reorder or mute never re-instantiates a device (A11N-010). **Notes:** Audio Effect and Instrument Racks forward the input notes exactly once. **Latency:** an unknown or invalid latency is told apart from zero (N11H-037). **Retirement:** ADR-0122 d8's skip is retired when this lands.
- **Reuse:** RK1 (tracktion's rack tests and its one-player compensation model, design only); RK2 (openDAW's composite as a flattening host: an empty composite is the identity, plus the mute, solo, nesting and reorder tests); RK4 (MAGDA's rack model and tests); in tree: `StripNode`'s 5 ms ramps and pan law, `testTwoPathsDeliverTheNoteAligned`, and #395's `owner()` walk as one `trackOfChain` helper.
- **Reject or defer:** reject tracktion's shared buses per RackType and its macro path, which locks on the audio thread. Reject wet/dry pin-mapping semantics, which are not Live's. ERRATA-11: Reason flattening Combis is not evidence of nested Combinators; ADR-0060 governs ADI's nesting. N-bus (ADR-0113) is deferred; Drum Rack returns are DEV-04.
- **Research IDs:** A11N-001; A6H-042; A8D-001, A8D-022; N11H-006, N11H-035, N11H-036, N11H-037; B-G01; M6-A6H-042..051; hosts10 portable-rack-boundary; hosts11 rack-relationship-rewrite.
- **Unresolved test (NOT RUN):** `test_session.cpp`'s rack fixture inverted (nothing skipped, the nested device realised); two chains at 0 and 64 samples line up, and the rack reports 64; a nested rack lines up too; an empty chain passes dry; mute, solo and volume ramp without a rebuild; a nested device's automation plays (`test_param_automation.cpp:467` inverted); **Live 12 readings:** whether a rack with no chains passes or mutes; the chain pan law; M6-A6H-042..051, NOT RUN.
- **Cross-check and this plan:** Win amends ADR-0060 d1's mechanism (devices K4); side tables only.

#### DEV-06 Macros: Live's takeover plus the multimapper (level 2)

*refinement* · size L · evidence documented · lane win, engine, window · milestone P2 · after: DEV-01

- **Need:** **Live's macros:** up to 16, 8 shown by default; Map mode; Min/Max, inverted when Min > Max; a mapped control handed over to its macro; names and colours; Rand with exclusions; variations. **The director's multimapper:** a breakpoint curve per target, several macros summing on one parameter in its own unit up to a user floor and ceiling, last writer still available, and a device's on switch as a target.
- **Decision now:** ADR-0060 d3-d5; ADR-0114 d1-d5; ADR-0203 d15 (D-06) and d10 (P2); ADR-0110 (a macro move writes no op); ADR-0076 (rack blocks show macros); FEATURES §6 Macros P2 and cross-track modulation P2.
- **On main (audited at ca0290c2):** The schema has `macros(device_id, ord, name, value, color)` and `macro_mappings(..., range_min, range_max, curve INTEGER, inverted)`. There is no enable, no breakpoint curve, and no per-target rule or cap. `rows::Model` does not read macros, and the text projection excludes them. There is no `macro.*` op and no engine.
- **Proposed contract:** **Parameters:** the rack device exposes `macro.1..16` and chain select as ordinary parameters, so automation, `controller_maps` and Learn work unchanged. **The source:** a `MacroSource`, an event source beside `DeviceAutomation`, emits `ParamValue` at each target on ADR-0054's grid. **Mapping:** the macro (0..1) goes through the target's curve (AAUT points, ADR-0159's evaluator; empty means Live's straight Min/Max line) into the target's real unit. **One mapping is Live's takeover** (win rules it under ADR-0203 d1, K12). The target follows the macro, its own control shows as disabled, and clip envelopes still modulate it through `ParamMod`. **Several mappings:** d15's sum in real units, then clamped to the floor and ceiling; or last writer. A VST3 target sums in the normalised domain instead, a named deviation, because VST3 gives real values only as text. **Identity:** mappings key on (device id, parameter id), never on panel order (A11N-003). **Persistence:** knob values and mappings persist; produced values never do. **Schema:** side tables only (`rack_settings`, `macro_settings`, `macro_mapping_curves`, `macro_targets`, `macro_variations`).
- **Reuse:** RK1 (tracktion's mapping helpers, reference only); RK5 (Vital's line-generator editor model for the window: presets, flips, add a middle point, a 100-point cap; evaluation stays in `curves.hpp`); in tree: the `DeviceAutomation` pattern, `curveValue`, the AAUT points.
- **Reject or defer:** reject tracktion's normalised sum-then-clamp and its lock on the audio thread; reject Bitwig's signed relative amounts as the macro model (that is DEV-07's surface); reject Reason's undocumented many-source equation (N11H-003); variations and Rand can follow the core mapping.
- **Research IDs:** A3-028, A3-029; A6H-051; A8D-018..021; N11H-001..005, N11H-018, N11H-030; A11N-003, A11N-004; B-M01, B-M08; hosts11 macro-composition-domain.
- **Unresolved test (NOT RUN):** hosts11 macro-composition-domain (two sources, a discrete target, an inverted range, one mapping disabled), NOT RUN; d15's sum matched against the director's multimapper device file, which BUILD-BRIEF lists and he has not yet supplied; Live readings: A8D-019's takeover; A8D-018's 16 macros with the visible count saved; what Unmap leaves on the target.
- **Cross-check and this plan:** XC-16: macros are one contributor kind in the one modulation ADR and evaluator.

#### DEV-07 Modulation in the graph: the schema, the modulators, the ring (level 2)

*refinement* · size XL · evidence documented · lane win, engine, devices, window · milestone M7 · after: DEV-08, DEV-18

- **Need:** modulation inside the graph, which the Dream describes as "in Bitwig's manner" (FEATURES "Host-side modulation to any VST3 parameter", P1). It covers: LFOs, envelopes, steps and an audio-rate envelope follower keyed from any track; any VST3, CLAP or built-in parameter as a target, across tracks; the base value kept apart from the modulated value; a ring on every modulated control. Live's modulator devices map through the same routes: Envelope MIDI, Shaper MIDI and Expression Control, and Suite's LFO, Envelope Follower and Shaper.
- **Decision now:** ADR-0046 d1-d3 (its open item: the modulation schema); ADR-0052 d5 (value plus a modulation offset; CLAP `PARAM_MOD`); ADR-0114 d4; ADR-0062 d4; ADR-0124's open item (modulation sets are not ops); ADR-0188 d3; ADR-0169 d1; FEATURES §6 P1 and the native nodes "Audio-rate envelope follower" P1 and "Grid-locked volume shaper" P1.
- **On main (audited at ca0290c2):** none built. There is no modulation schema, no modulator node, and no producer of `ParamMod`. Clip envelopes (#434) play absolute values only. The envelope follower and volume shaper nodes are unbuilt, and no lane queue names them (K8).
- **Proposed contract:** **Schema:** side tables for routes (source, target device and parameter, depth, curve points, mode, bipolar, enable). Routes persist and are undoable; outputs never persist or reach the op log (ADR-0046 d2). **Nodes:** modulators are graph nodes on the audio thread at sub-block resolution, ordered before their targets by event-only edges. **What they emit:** `ParamMod`. CLAP converts it to plain units; VST3 adds it to the base host-side and sends a point; Pd receives it once per Pd block. The transfer curve applies before scaling (B9M-045). **Scope now:** mono or global only. Live's modulator devices are modulator nodes on the same routes.
- **Reuse:** #207's `MidiModulator` (8 mappings, a 32-point envelope, MPE Control) as design input only. No harvest area covered modulation; MF1 holds Expression Control and Shaper back for this schema.
- **Reject or defer:** per-voice (polyphonic) modulation (B-M06) waits until instruments are in scope. Reject mappings kept inside a device's state blob, as #207 does. Audio-rate targets inside Pd wait for `[adi.param~]` (DEV-15).
- **Research IDs:** B-M01..B-M09; B3-015..B3-020; B9M-045, B9M-046, B9M-048..B9M-050; N11H-004, N11H-005, N11H-018, N11H-030; A7B-088; hosts10 parameter-gesture-state.
- **Unresolved test (NOT RUN):** one source modulating a CLAP (`PARAM_MOD`), a VST3 and a built-in parameter gives the same curve at each; the base value is unchanged after stop, save and reopen; deleting a source removes its routes in one undo step; a cross-track target is aligned by ADR-0114 d4's arrival rule; B-M09 (mapping persistence), NOT RUN.
- **Cross-check and this plan:** XC-16: one schema and one evaluator; XC-34: the envelope follower keyed from any track needs DEV-05 and MX-24.

#### DEV-10 User Pd patches as devices (level 3)

*refinement* · size L · evidence inferred · lane win, engine, devices · milestone M7 · after: DEV-09, DEV-08

- **Need:** a user can put an editable Pd patch on a track as an Audio Effect, Instrument or MIDI Effect device: it loads from the project; its declared parameters are ordinary parameters; a changed patch reconciles parameters by id; Save is one op; a patch that fails is a placeholder that keeps its bytes.
- **Decision now:** ADR-0035; ADR-0145 d8 (patch text in `state_blobs`; an approved change is `device.loadState` with the text's hash); ADR-0177 d1-d6; ADR-0188 d3 (a range change keeps the real value, clamped); ADR-0197; ADR-0011 and ADR-0122 d4; FEATURES "Pd parameters ..." P1.
- **On main (audited at ca0290c2):** `'internal'` reaches only catalogue built-ins, on their native cores (`juce_device_loader.cpp:58`). `PdDevice` has no parameter or state interface. `builtin_insert` always writes subtype 'effect'. There is no role for patch text and no reconciliation. The parser, published arrays, sample slots, transport and MIDI input exist, with tests.
- **Proposed contract:** **Identity:** `plugin_refs(format='internal', uid='org.adidaw.pd.audio-effect|instrument|midi-effect', matching subtype)` makes a `PdPatchDevice`. **State roles:** ; 'patch': UTF-8, the committed bytes; 'abstractions': CBOR, name to text; 'sample:<id>'. **Ports:** read from `[adc~]` and `[dac~]`. **`loadState('patch')`:** ; it parses the declarations and runs the allowed-object walk off the audio thread; it builds a new engine in a working folder that ADI writes; it reconciles by id: a kept id keeps its real value, clamped; a gone id is marked missing, with its rows and lanes kept; a new id takes its default; it swaps engines under the gate and retires the old one off the audio thread. **Save** builds its own single `device.loadState`. ADR-0142's snapshot glue would write one op per role, so it is not used. **Failure:** a patch that fails to open is a placeholder that keeps its bytes and rows, and export refuses it (A11N-009). **Templates:** three starter templates are embedded when the build is configured.
- **Reuse:** PD5 (Camomile's reload order and patch-owned data protocol, design only); PD4 (the allowed-object walk, with the object list taken from the running build); in tree: `rescanParamsPreservingIndices` (`clap_host.cpp`), the op shape of `sample_slots.cpp`, `builtin_insert`, the `test_builtin_devices` harness, and `pd_native.cmake`'s embedding.
- **Reject or defer:** reject hvcc: it needs a compiler at run time and would be a second DSP truth. Reject plugdata's name-keyed parameter pool (ADR-0177). A 'data' role (`[adi.store]`) only if win rules it (R5).
- **Research IDs:** A11N-002, A11N-003, A11N-008, A11N-009, A11N-010, A11N-011; B9G-002, B9G-006; PD-RACK-EXPORT-FIT-11; BITWIG-GRID-PD-FEASIBILITY stages 1 and 5.
- **Unresolved test (NOT RUN):** **AR1:** a Gain patch at 0.5 renders input × 0.5, delayed by the reported 64 samples, and is bit-identical after save and reopen. **AR2:** reconciliation in exactly one `device.loadState`; undo restores the first version. AR3, AR11, AR14, AR15, AR16; hosts11 pd-segment-contract, NOT RUN.

#### DEV-14 Pd note output, voices and instrument kinds (level 3)

*refinement* · size M · evidence inferred · lane win, devices, engine · milestone M7 · after: DEV-10, DEV-17, DEV-18

- **Need:** a Pd MIDI-effect patch sends notes back into ADI. A Pd instrument consumes notes, keeps a voice per note, and resets cleanly on stop, bypass and patch replacement.
- **Decision now:** ADR-0194; ADR-0091 d2 and d8; ADR-0158; ADR-0054.
- **On main (audited at ca0290c2):** only the float and print hooks are installed (`pd_engine.cpp:272-277`). MIDI goes into patches only. A Pd instrument has no Consume. #278 documents a reset that races the audio thread.
- **Proposed contract:** an ADR from win (R4) covering: **Hooks:** per-instance libpd MIDI-out hooks fill a fixed array inside the trylock tick. **Decoding:** ADI's `MpeParser` decodes them under the device's note-source prefix (DEV-17). **Timing:** offsets come from Pd's logical time (`[pipe]` and `[delay]` land on the exact sample); synchronous transforms keep their input frame. **Kinds:** MIDI-effect patches run inside the transform phase (DEV-18). An audio patch's notes are declared one block late. **Overflow:** releases everything and is counted. **Instruments:** they Consume. Voices use vanilla `[clone]`, with the MPE member channel as the voice index; `[poly]` serves the plain route. **Reset:** a host `$0-adi-reset` bang on prepare, stop, bypass and panic replaces #278's workaround. **Precision:** the note output is MIDI 1 by construction, and labelled so.
- **Reuse:** MF3 (libpd's MIDI-out hooks decoded into prefixed ids); PD1 (`[clone]`/`[poly]`, logical-time offsets); MF5 (openDAW's lifecycle contract, design only).
- **Reject or defer:** reject `[noteout]` alone as completion. Reject claims of Grid-identical polyphony. A high-resolution typed note bridge out of Pd waits until a patch needs it.
- **Research IDs:** A11N-007; B9G-038, B9G-039; N11H-017, N11H-039, N11H-040; grid9 generated-event-ownership, voice-reclamation and same-frame-identity; BITWIG-GRID-PD-FEASIBILITY ("no typed outgoing Pd events").
- **Unresolved test (NOT RUN):** **AR9:** `[notein]→[+ 12]→[noteout]` gives the same frames with unique ids and matching offs; `[pipe 100]` lands on the exact sample; 600 notes overflow, release everything and are counted. **AR10:** 16 overlapping notes take 15 voices plus one; per-note bend reaches its own voice; the notes are consumed; a held note keeps the device awake; stop resets within one block. The grid9 probes, NOT RUN.

#### DEV-15 Grid-style modular semantics on the Pd tier (level 3)

*refinement* · size XL · evidence documented · lane win, devices, engine · milestone M7 · after: DEV-10, DEV-14, DEV-07

- **Need:** the third level: an internal signal and event graph with Grid-like rules. signal domains and rates; feedback with a stated minimum delay; voice scope; audio-rate modulation inside a device. The Dream pictures this as Bitwig's Grid, built on Pure Data.
- **Decision now:** ADR-0035; ADR-0145 d8; ADR-0177; ADR-0188 d3 (`[adi.param~]` "decided when a patch needs it") and d8; ADR-0062; ADR-0203 d4; the Dream Reference, ch. 9.
- **On main (audited at ca0290c2):** pieces exist: per-instance runtime, declarations, published arrays, native cores, the latency protocol. Missing: a Grid-style contract, an audio-rate parameter bridge, note output from Pd, a voice allocator.
- **Proposed contract:** a hybrid. **The host owns** note identity, scheduling, modulation routing, state, undo and latency. **Pd** is the authoring and runtime layer inside that contract. **Registered native kernels** (compiled-in externals) carry timing-critical or heavy DSP. Before any Grid-style work, an ADR answers the gap review's eight decisions: 1. the canonical source: the patch text under ADR-0177's `device.loadState`; 2. the note bridge: DEV-14; 3. the voice model; 4. rates and signal width; 5. feedback units: the minimum loop delay in frames, per connection; 6. host modulation: DEV-07; 7. live-edit recovery; 8. dependency and asset versions. Acceptance is G1 to G6.
- **Reuse:** PD1 (`[clone]` voices, the hooks); PD6 (cyclone and ELSE as compiled-in externals, per the editor ruling); in tree: the native cores and `adi_dsp` kernels as externals.
- **Reject or defer:** reject a second native graph runtime or editor (a new graph format, compiler and scheduler, module SDK, editor commands, migration: the gap review's cost list); reject any graph copy kept beside the Pd copy; reject Bitwig project or preset compatibility and claims of Grid-equivalent algorithms; `[adi.param~]` waits until a patch needs it.
- **Research IDs:** B9G-001..B9G-059; B-G02..B-G06; N11H-038; BITWIG-GRID-PD-FEASIBILITY (its native/Pd/hybrid table; stages 1-6); the grid gap review's gates G1-G6 and its eight pre-implementation decisions; the ten grid9 specifications.
- **Unresolved test (NOT RUN):** **G1:** threshold logic, wrapped phase, pitch units, asymmetric stereo, non-finite values. **G2:** note ownership. **G3:** voice lifecycle. **G4:** the minimum loop delay in frames, with host buffer sizes in the test matrix. **G5:** insert, delete with reconnection, replace and fan-out move, each one Undo. **G6:** an equivalent workload's CPU. The grid9 specifications, NOT RUN.

#### DEV-17 One note-identity namespace

*refinement* · size M · evidence inferred · lane engine · milestone M6 · after: none

- **Need:** every note in the graph has an identity that is unique while it is held, whatever made it (clips, live input, MIDI effects, Pd note output, rack chains), and every plug-in receives distinct ids.
- **Decision now:** ADR-0054 d2; ADR-0155 d3; ADR-0158 d2; ADR-0091; ADR-0097/0099.
- **On main (audited at ca0290c2):** Three namespaces exist: a clip counter local to `MidiClipState`, the controller-prefix allocator, and the unprefixed `MpeParser`. CLAP and VST3 fold ids `% 0x7FFFFFFF` (`clap_host.cpp:119, 135`; `vst3_events.hpp:79`), so (p<<32 | s) becomes 2p + s. A clip note and a live note can therefore arrive with the same id once a second note source reaches one instrument. That is latent today, since no production live-input path exists.
- **Proposed contract:** an engine ADR. **One allocator:** a process-wide event-source prefix allocator, generalising the controller one, for clip tracks, each live input, every transform or generator node and Pd note output. noteId = prefix << 32 | serial. **Acknowledgements:** note-offs are acknowledged by prefix, as `Graph::acknowledgeController` already does; only prefix 0 is broadcast. **Plug-in maps:** the CLAP and VST3 hosts replace the fold with a fixed per-instance map. An int32 is assigned at note-on and freed at the delivered off or at CLAP `NOTE_END`. **Same key in MIDI 1:** a plug-in that addresses notes by key gets an off before the second on, and the orphan off is dropped (pending LM3). **Channel base:** declared (`MpeParser` counts 1..16, clips 0..15).
- **Reuse:** in tree: the controller-prefix allocator and `acknowledgeController`; RK2 (openDAW's re-id where chains rejoin, design only).
- **Reject or defer:** reject per-device counters (#207's `nextId_ * 1024`) and per-event owner pointers.
- **Research IDs:** A7M-128; B9G-038; grid9 same-frame-identity and generated-event-ownership; the MIDI probe `ADI_TEST_overlap_same_key.mid`.
- **Unresolved test (NOT RUN):** a clip note (prefix 1, serial 3) and live note 5 reach one CLAP and one VST3 fixture with distinct ids (today they collide); 10^6 random live ids never collide while held; `ADI_TEST_overlap_same_key.mid`; `NOTE_END` frees the map slot.
- **Cross-check and this plan:** XC-25: rebuild continuity (the Link consumer, Session launches) follows from this scheme.

#### DEV-18 The event transform phase; generated notes owned and released

*refinement* · size M · evidence inferred · lane engine, devices · milestone M6 · after: DEV-17

- **Need:** nodes that emit events (MIDI effects, rack routers, Pd MIDI effects): run before the splits; own the notes they make; release them on bypass, removal, stop, seek and loop. This closes ADR-0091's "events a node emits", which it left undecided.
- **Decision now:** ADR-0091 (its open item); ADR-0042; ADR-0090 d5; ADR-0158.
- **On main (audited at ca0290c2):** there is no transform phase; events are forwarded before the splits (ADR-0091). #207 holds a hook, but it cannot be rebased.
- **Proposed contract:** an engine ADR. **Where it runs:** `transformsEvents`/`transformEvents` run in `Graph::forwardEvents`, after a slot's inputs are gathered and before forwarding and split computation, into a fixed store sized at prepare. **Ownership:** transform nodes acknowledge the offs they take and own their notes by prefix (DEV-17). **Leaving:** `releaseGenerated` is called when bypass turns on and for nodes that leave at a publication swap (the leaver counterpart of `enterPlay`). **Discontinuities:** stop, seek or a loop jump releases everything. **Clock and prepare:** the clock keeps running while bypassed, and prepare is idempotent. **Not here:** no `MappedValue` path; modulation is DEV-07.
- **Reuse:** MF1 (#207's hook and kernels, from (its source file) `7f421f4d`); MF5 (openDAW's `release_leavers`/`release_all`, design only).
- **Reject or defer:** reject #207's `EventType::Control = 5`, which clashes with main's `Controller = 5`, and its per-event `EventOwner*`. Reject pull-based scheduling.
- **Research IDs:** A7M-128; grid9 generated-event-ownership and event-boundaries; N11H-039; B9M-001; BITWIG-GRID-PD-FEASIBILITY's first blocker row.
- **Unresolved test (NOT RUN):** #207's tests on main: identical timing at every block size from 32 to 4,096; zero allocations; overflow; replay; grid9 generated-event-ownership: remove a module with queued notes, seek back, stop, undo the removal; expect zero orphans, every release by its owner (NOT RUN); a rebuild mid-note keeps the arpeggiator's phase and its pending offs.

#### DEV-20 Recording generated notes, Live's way

*refinement* · size M · evidence documented · lane engine, win, window · milestone M6 · after: DEV-17, DEV-18, DEV-05

- **Need:** record what MIDI effects produce. In Live, another MIDI track's MIDI From taps the source track after its MIDI effects (Post FX), and that track records (Live 12 manual §17.5.2.8, p. 388).
- **Decision now:** ADR-0203 d1 and d3 (track-to-track taps, P1); ADR-0091 (the note-input bus "not designed yet"); ADR-0132; FEATURES §2 taps P1.
- **On main (audited at ca0290c2):** not built. `graph.cpp:1346` calls notes from another track "a note-input bus, which is a different feature and not designed yet". MIDI transport recording is not built, and `MidiMemory` takes live input only.
- **Proposed contract:** **The routing row:** a MIDI row from track A (Pre FX or Post FX tap) into track B's note input is a graph edge, delayed by ADR-0091's rules. **Recording:** B's recording and Capture take the tapped events. An ADR amends `MidiMemory`'s "live input only" rule. **Ids:** generated ids are re-prefixed at the receiving track (DEV-17).
- **Reuse:** MF6 (Ardour's bounce-with-processing, design only, kept for a possible later addition); in tree: `midi_memory`, the recording commands.
- **Reject or defer:** not proposed: ADI's own "commit MIDI effects to a clip" (Reason's Send to Track, Cubase's Merge MIDI in Loop). It is an addition, gated on win's ANOT velocity decision, and Live's tap-and-record covers the need.
- **Research IDs:** N10H-015, N10H-016; A7M-128; hosts10 generated-midi-commit; LM5.
- **Unresolved test (NOT RUN):** hosts10 generated-midi-commit (no double processing; exact offsets), NOT RUN; LM5 (what Capture and recording take from a tapped track: generated or played notes).
- **Cross-check and this plan:** XC-15: MIDI From is a row in the one table of track-to-track inputs, with real foreign keys.

#### DEV-23 Multi-threaded graph processing

*refinement* · size L · evidence measured · lane engine, devices, win · milestone M7 · after: none

- **Need:** Live spreads devices across cores, with no switch. ADI must do the same without changing a single output byte, both at tracking sizes (32 to 128 samples) and at mixing sizes (2,048 to 4,096).
- **Decision now:** ADR-0203 d8 (A7-06, P1); ADR-0102 d1-d4 (single-threaded below a measured size; the benchmark suite); ADR-0055, ADR-0056 d3; ADR-0019, ADR-0089, ADR-0090, ADR-0092; ADR-0042 d6; ADR-0052 §2; FEATURES "Multi-threaded graph processing" P1; BUILD-BRIEF Phase D item 4.
- **On main (audited at ca0290c2):** Main is serial (`graph.hpp:30`), with `levels()` and the order oracle in place. #441 (head `9a862e97`, CONFLICTING) adds workers, off by default, for Sum, Gain and Mix nodes only, with a pool per graph and level barriers. Workers get no real-time standing, and CLAP answers neither thread-check nor thread-pool. "Processing threads" is Backlog.
- **Proposed contract:** **The pool:** one persistent pool owned by `GraphHost` and lent to each published graph, never rebuilt per graph. **The scheduler:** dependency-counted and work-conserving: each node resets its own count; a chain continues on the same thread; the callback works alongside; a node count ends the block, so any number of sinks works. **Waking:** at most min(idle, ready) wakes per block. **Workers' standing:** they inherit the callback's FP state and real-time standing: Windows: MMCSS "Pro Audio" at the callback's priority, never `REALTIME_PRIORITY_CLASS`; macOS: the device workgroup; Linux: `SCHED_FIFO` where allowed. **Devices opt in:** ; CLAP, with thread-check answered on workers; VST3; Pd, after measuring the pthreads4w lock contention; built-ins, once the devices lane signs off. **Offline renders:** their own normal-priority pool, or serial. **Default:** on, only after the the director's Windows workstation measurements pass, since Live has no switch. ADI's Engine page keeps a thread count (Auto, All or N). **Later:** CLAP's thread-pool extension, served from the same pool.
- **Reuse:** GT1 (tracktion's lock-free player, logic only); GT2 (the supernova kernel); GT3 (libossia's pool); GT4 (Ardour's real-time setup and wake rule); GT6 (JUCE's workgroup, in tree); GT5 (Zrythm's CLAP thread pool); GT8 (the thread-check test); GT7 only as an interim fix if the scheduler slips.
- **Reject or defer:** reject #441's pool per graph, level barriers and strided partitions as the target, but keep its `RenderState` split, FP propagation, tests and benchmark flags; reject tracktion's real-time upgrade on the wrong thread and its dropped blocks; reject a separate DSP-thread handoff; reject a user-facing multicore switch; plug-in sandboxing stays rejected (D-03).
- **Research IDs:** A5C-019, A5C-020; M-CPU; M5-A5C-019; the RESEARCH-MATRIX P1 row (worker synchronization); E11; B3-009.
- **Unresolved test (NOT RUN):** **GT-1:** byte-identical output on 1,000 random graphs, self-send loops included. GT-2 to GT-5. **GT-6:** 16 Surge XT CLAP, 16 VST3 and 16 Pd devices on 4 workers match the serial bytes. **GT-7:** ADR-0102 d3's suite on the director's Windows workstation under ASIO, serial against 5 and 11 workers, compared with Live 12 (M-CPU, M5-A5C-019). All NOT RUN.
- **Cross-check and this plan:** #441 lands off by default first (devices K9); XC-26: every render stays in process.

#### DEV-25 Out-of-process plug-in scanner

*refinement* · size L · evidence measured · lane engine, win, window · milestone M1 · after: none

- **Need:** plug-ins are discovered only in a child process, always, with no switch, and with Live's rules: a database refreshed incrementally by Rescan, rebuilt by Alt+Rescan, and skipped by Alt at launch; a crash names the plug-in and offers "scan again" or "make unavailable"; a second crash makes the plug-in unavailable until it is reinstalled; no scan ever runs inside a project open.
- **Decision now:** ADR-0203 d8 (D-03, A6-37); R-17; ADR-0149 (its open item: the scan cache); ADR-0047 d3; FEATURES "Plug-in scanner out of process, always" P1; BUILD-BRIEF Phase D item 5; SETTINGS-CATALOGUE II §8.
- **On main (audited at ca0290c2):** **In process:** VST3 through `PluginDirectoryScanner` with an empty dead-man's pedal (`vst3_host.cpp:62-64`); CLAP loads every bundle into ADI (`clap_host.cpp:1915-1919`). **Message thread:** the one lazy scan runs inside project open (`juce_device_loader.cpp:89, 121`). **Registry:** version 1. **The open PRs:** #440, #439 and #438 (heads `d281ea0e`, `356e7715`, `6a966f5e`). D7 fails the Windows JUCE leg, and D1 fails the Ubuntu GUI shell.
- **Proposed contract:** 1. **Land #440, then #439, then #438,** after these fixes: **D1:** JUCE's modules leave the warning wall for the helper and the VST3 scanner tests. **D2:** a final drain after the child exits. **D5:** the metadata channel is not inheritable. **D7:** on Windows, accept a module inside the requested bundle; stop overwriting `fileOrIdentifier`; the tests use a temporary registry. **D8:** a bad candidate is reported and skipped; only a deployment failure aborts. Also set `SEM_FAILCRITICALERRORS` in the parent, and verify the Windows legs by head SHA. 2. **Registry version 3** (an ADR by win): a scan cache keyed by (format, path), with a BLAKE3 stamp over the module files (size and mtime, compared for equality), the helper/JUCE build id, the packet and CLAP's invalidation sources; blocks gain a kind, strikes and a stamp. A first crash or timeout is one strike; a second blocks; a stamp change clears any block that is not manual; an empty or bad result is a negative cache entry, never a quarantine; a stale pending row counts as one strike, and a fresh one from another suite application means busy; a helper self-test runs before each batch, and loader errors mean Unavailable. 3. **A scan service:** ; K workers (by default min(4, cores/2)); Incremental, CacheOnly and Clean modes; an immutable catalogue snapshot, also indexed by VST3 class UID; a scan report in the app's logs; a scan at startup, never inside project open; `.lnk` files, junctions, symlinks and aliases followed, with cycle detection. 4. **The window:** Settings > Plug-Ins (Rescan, Alt+Rescan, Alt at launch), progress, the first-strike notice, a blocklist with Re-enable, and placeholders that name the block.
- **Reuse:** SC1 (muse_framework's registration and its 18 tests); SC2 (Carla's progress-reset deadline and negative cache); SC3 (Ardour's scan states and Re-scan Faulty model); SC4 (tracktion's one retry in a fresh child); SC5 (JUCE's description XML and the `moduleinfo.json` fast path, in tree); SC6 (MAGDA's worker pool and report, design only); SC7 (the helper self-test, design only).
- **Reject or defer:** reject a single long-lived scan worker and any in-process fallback; reject runtime sandboxing (B3-009/010; ADR-0047; ADR-0204 d5); Re-scan Faulty or Selected and a per-plug-in Skip are additions Live lacks, and are not proposed now.
- **Research IDs:** A5H-034, A5H-035; M5-A5H-034/035; B3-005, B3-009, B3-010; evidence_m10 (Reason's PlugInspector).
- **Unresolved test (NOT RUN):** scanner acceptance items 1-13: no lost output under a 500 ms test pause, 200 times in a row; K=4 and K=1 give identical results; project open makes zero directory scans; aliases are followed; M5-A5H-034/035 on Live 12.4: what Alt+Rescan clears; the relaunch dialog's wording and default; what counts as reinstalled; whether Live scans in a separate process. NOT RUN.
- **Cross-check and this plan:** XC-24: Alt at launch shares PD-05's session marker; VST3 identity by class id is win's question against ADR-0122 d2 (devices K11).

#### DEV-28 One loudness meter for every reading, proved from outside

*refinement* · size M · evidence documented · lane devices, engine, window, win · milestone M4 · after: none

- **Need:** one BS.1770/R128 meter behind every reading ADI shows or uses: Main's LUFS and true peak, on the master only (ADR-0203 d14); export normalization; Auto Gain Stage's LUFS mode; `analyze.loudness`; the Reference's matched loudness in the Mix View. It needs EBU Mode's functions (max M/S, LRA, run and pause, "LRA not yet stable") and proof from outside ADI.
- **Decision now:** ADR-0203 d14 (A3-24: "on condition that it does not degrade performance"); ADR-0175 d5; ADR-0167 d5; ADR-0166 d8; ADR-0195 d4; ADR-0209 (matched loudness); FEATURES "Loudness meter ... master only" P1.
- **On main (audited at ca0290c2):** **Built:** K-weighting (libebur128's constants, uncredited); a float true-peak meter capped at 8 channels; N13 (#382). **N13's problems:** it reads M, S, I and TP with 0.1 LU bins, walks a 22,001-bin histogram twice every 100 ms on the audio thread (`main_loudness.cpp:40`), and carries a copied Kaiser design. **Three implementations:** export normalization and Auto Gain Stage each keep their own, so three integrated-LUFS implementations coexist. **Missing:** LRA, max M/S, run/pause. **Proof:** the tests check ADI against ADI. **The window's Main meter** is unbuilt (worklist B8).
- **Proposed contract:** **`src/adi/dsp/loudness`:** ; 10 ms sub-blocks: M is the last 40, S the last 300; a gating block every 10 sub-blocks; an LRA sample at 10 Hz; histograms of 0.01 LU with exact energy, from −70 to +20 LUFS, allocated in prepare; explicit channel weights, never a guessed LFE; −inf until warmed up; I and LRA recomputed once every 100 ms from running totals; the maxima reset with I; run and pause; the programme's end handled without padding reaching I; ADI's own true-peak meter, gaining a double path and more than 8 channels. **Rewiring:** N13, normalization and Auto Gain Stage move onto the one meter. **The claim:** "passes ADI's synthesised Tech 3341/3342 v4 minimum-requirement signals and agrees with libebur128 and ffmpeg", never "EBU Mode compliant". **Defaults:** Main's run, pause and transport reset follow ADI-L's own defaults (win's placement). **Display:** M, S, I, LRA and TP at one decimal, on the EBU +9 scale by default.
- **Reuse:** LD1 (FilmCraft's meter, the base now); LD3 (libebur128 as a test-only oracle, never its 1 Hz LRA); LD4 (Tech 3341/3342 signals synthesised in C++); LD6 (ffmpeg's 10 Hz LRA, an optional CI oracle); LD7 (tracktion's readings snapshot with validity flags, design only); LD2 (ADI-L's own meter as a cross-check now, ~~and the point of convergence when his unified feature arrives~~ *[this plan: whether the meters converge is part of the unified feature's design, after his list]*).
- **Reject or defer:** reject the EBU's WAV set (its terms are not usable) and libebur128's LRA. LD2 is deferred as the base, because his plug-in source joins ADI only when he declares them all finished (5 November). Per-bus meters are not wanted (d14).
- **Research IDs:** Cubase's loudness meter (C3 rows; the `cub_loudness` figure); R10H-073; R11X-025; EBU Tech 3341 v4 and Tech 3342 v4 (specifications).
- **Unresolved test (NOT RUN):** every Tech 3341 case that can be synthesised (1-6 and 9-23; #13 and #14 exact on the 10 ms grid; #20-23 against an in-test 64× reference), and Tech 3342 1-4, within ±0.1 LU of an exact in-test reference; libebur128 within 0.1 LU at 44.1, 48 and 96 kHz; the same result at any block size; zero allocations; the meter's cost on Main, always on (A3-24).
- **Cross-check and this plan:** XC-03: one meter for every reading; MX-27 is its acceptance view. XC-46: the devices seat is silent; until it logs, `src/adi/dsp/loudness` is lent to the engine seat so this row, on the critical path of EX-05, DEV-29 and MX-27, does not wait.

#### DEV-30 The Loudness Drive after the 5 November ruling

*refinement* · size M · evidence documented · lane win, engine · milestone M4 · after: DEV-28

- **Need:** one control scales every leaf track's level into the whole clipper and limiter cascade together. The mix is heard and exported at any loudness without changing its balance (CTZ's VCA trick), and the Drive pairs with export LUFS targeting.
- **Decision now:** ADR-0203 d14 (D-04 c); FEATURES "Loudness Drive" P1 (its mechanism: the VCA group and the pre-FX input gain, ADR-0077, `mixer.setInputGain`); BUILD-BRIEF Phase D item 7. ADR-0204 d1's "the per-channel ADI-L instances are the cascade's stages" falls with the 5 November ruling (K1).
- **On main (audited at ca0290c2):** not built. `InputGainNode` and `mixer.setInputGain` exist. The engine log of 4 November holds the "whole Loudness Drive cascade" blocked on the ADI-L stages.
- **Proposed contract:** **The control:** a saved global control with its own op, one undo step per gesture, automatable. **What it scales:** the leaf tracks' common input gain, after an instrument. It never multiplies groups, returns or Main again, it is click-free at both precisions, and the Reference bypasses it. **What it does not do:** it never writes any device's parameters and switches nothing on. The cascade is whatever clippers and limiters the user has placed, his ADI-Clip and ADI-L VST3 builds or built-ins, until his unified feature exists. **The view:** the Mix View's CASCADE shows the stages it finds. **The engine's blocker** is lifted.
- **Reuse:** none harvested; in tree: `InputGainNode`, `mixer.setInputGain`, the VCA group rows; DEV-28's meter for the reading against the export target.
- **Reject or defer:** reject switching on per-channel slots (withdrawn), and substituting the generic limiter for his clone. Automatic stage switching waits for the unified feature. There are no VCA fader tracks (A3-14).
- **Research IDs:** none in the bundle (the CTZ method; the Mix View program's D23 and E16).
- **Unresolved test (NOT RUN):** on a three-tier CTZ fixture with user clippers, moving the Drive changes Main's LUFS while every track's relative level before its clipper stays fixed: the balance nulls; click-free at 64 and 4,096 samples; identical in real time and in export; one undo step per gesture.
- **Cross-check and this plan:** XC-06: a refinement under ADR-0203 d14 (told to him as a placement to strike), with MX-35 referring here.

#### DEV-11 The Pd patch editor's route (level 3)

*new-choice* · size L · evidence documented · lane director, engine, window, devices, win · milestone M7 · after: DEV-10

- **Need:** a Pd device's patch is edited from ADI, and the objects ADI cannot run are marked, so a patch never saves what cannot play. The agent proposes and the user approves.
- **Decision now:** ADR-0145 d8 (his ruling: an editor inside the DAW, "as Max for Live's is", built on plugdata; it superseded ADR-0134 d8's external editor); ADR-0188 d8; ADR-0145 d9; ADR-0177 d4 (outside edits wait for an op); ADR-0203 d8 (the DAW uses embedded libpd only; no Max application path); ADR-0204 d5 (the rejected items stay rejected, "an external Pd or Max path" among them); FEATURES "Pure Data editor inside the DAW, built on plugdata" P1. Route A re-admits an external Pd GUI process that d8 and d5 rejected (the audio stays in embedded libpd); Q-K1 says so.
- **On main (audited at ca0290c2):** nothing built: no plugdata integration, no editor attach, no object marking. The browser shows only a "Pd Devices" category.
- **Proposed contract:** **Opening:** "Edit in Pd", from the device's title bar and its Show Options menu, opens vanilla Pd's editor attached to the running instance. **Hearing:** edits are heard at once. Objects that cannot be created draw dashed, so ADR-0188 d8 is met natively. **Save:** it re-reads the working copy, validates it and writes one `device.loadState`. Closing without saving reverts. **Live's rule:** Save updates every instance that shared the old hash, and Save As asks. **What it needs:** a root canvas that closes is hidden, not freed; an ADI GUI plug-in rebinds Quit, Close, Open and Message; GUI polling must not starve the tick.
- **Reuse:** PD3 (ossia score's live-attach lifecycle); PD1 (libpd's start, poll and stop for the GUI); PD4; PD6 (cyclone and ELSE compiled in, if he wants plugdata's objects).
- **Reject or defer:** route C (plugdata's canvas inside ADI) is XL: plugdata runs on its own Pd fork, with 24 symbols vanilla lacks, its own JUCE fork and NanoVG; route B (the plugdata app on a working copy) is silent until Save and cannot share an exclusive ASIO device; graph-on-parent device panels wait for the loader; the Live Object Model surface for patches waits.
- **Research IDs:** the Live 12 manual, ch. 31, pp. 796-798 (Edit in Max launches the Max application, and Save updates every instance); B9G-003, B9G-004, B9G-005; PD-RACK-EXPORT-FIT-11 (no second editor); the gap review's decision 1.
- **Unresolved test (NOT RUN):** **AR12:** the editor opens the running instance; an edit is heard before Save; Save is one op with the text's BLAKE3; closing unsaved reverts; unknown objects are refused by name; instances with the same hash update. **LM-PD1 on Live 12.4:** does Save in Max interrupt audio on every instance, and are values and automation kept? ; Polling makes no audible gap while nobody edits.

#### DEV-12 Opening and editing a built-in device's patch (level 3)

*new-choice* · size S · evidence inferred · lane director, engine, window · milestone M7 · after: DEV-10, DEV-11, DEV-13

- **Need:** the Dream says each rebuilt Live effect "is a patch that can be opened". Opening shows its canonical patch, and editing changes how it runs.
- **Decision now:** ADR-0203 d4 (built-ins run native cores with no added delay; their patch files stay openable); ADR-0187 d1; ADR-0204 d5.
- **On main (audited at ca0290c2):** the canonical canvases ship in `pd/devices`, and native dispatch selects by exact content (`test_pd_zero_delay.cpp`). No UI opens a patch.
- **Proposed contract:** **Opening:** shows the canonical patch read-only. **Editing:** creates a copy device named "<Device> (edited)" on the user-patch path (DEV-10), leaving the built-in untouched. **Disclosure:** the copy states which path runs and its adapter delay (DEV-13).
- **Reuse:** in tree: the content matching in `native_devices.cpp` (it tolerates CRLF and LF), shown as "native" or "Pd" provenance.
- **Reject or defer:** reject editing in place: it would silently change a familiar device's sound path and timing under the same name (A11N-006).
- **Research IDs:** A11N-006 (native dispatch depends on the exact content), A11N-005; B9G-008 (Bitwig's Convert to FX Grid); Live itself cannot edit its native devices.
- **Unresolved test (NOT RUN):** AR6: a canonical canvas with bare CRs takes the native path; a shadowed helper takes the Pd path; an edited copy runs Pd, reports its delay and says which path runs.

#### DEV-13 User-patch delay when blocks do not divide by 64 (level 3)

*new-choice* · size M · evidence inferred · lane director, devices, engine · milestone M7 · after: DEV-10

- **Need:** ADR-0203 d4's open clause. User Pd patches add no delay in whole 64-sample blocks, and "any case with an unavoidable remainder is brought back by win". This is it. The trap (A11N-005): notes and automation split a 256-sample buffer into, say, 1 and 255 frames. Today WholeBlocks silences and counts that, and Buffered adds one reported 64-sample block. Some drivers' buffers are not multiples of 64 at all.
- **Decision now:** ADR-0203 d4; ADR-0183 d12; ADR-0095; ADR-0042; ADR-0078 (`NodeIo` addresses the block).
- **On main (audited at ca0290c2):** both modes exist: WholeBlocks (no adapter delay only when every segment is a multiple of 64; a violation is silenced and counted) and Buffered (one reported block). Built-ins avoid both by running native cores.
- **Proposed contract:** **A multiple of 64:** no added delay whenever the host buffer is a multiple of 64. The adapter runs whole Pd ticks across a block's splits (the device sees the whole block's input), and each event lands at the start of the Pd tick that contains it, as Pd schedules control messages anyway. **Otherwise:** one reported, compensated 64-sample block, shown on the device. **Intrinsic delay:** a patch's own delay (`$0-report_latency`) is separate and always added.
- **Reuse:** in tree: `pd_engine`'s two modes and `test_pd_zero_delay.cpp:417-470`.
- **Reject or defer:** reject silencing invalid segments in production (WholeBlocks' current behaviour is a diagnostic contract). Reject promising zero delay for any patch, since its own delay always counts.
- **Research IDs:** A11N-005; N11H-038; hosts11 pd-segment-contract; BITWIG-GRID-PD-FEASIBILITY's low-latency row.
- **Unresolved test (NOT RUN):** hosts11 pd-segment-contract: buffers of 64, 128, 256 and 511; event offsets 1, 63, 64, 65 and 127; three modes (NOT RUN); **AR5:** a 256 split into 1 + 255 gives continuous output at the declared delay and nulls against a dry track.

### A.4 Mixer, routing, monitoring and the Mix View (36 rows; source the mixer matrix (win's working file))

#### MX-01 Strip meters on the engine's scalar meter

*decided* · size S · evidence documented · lane window · milestone M1 · after: MX-09 for the input-metering half

- **Need and current decision:** Live's track meter: peak and RMS of the output, the input while monitoring, a Peak Level hold that turns red past 0 dB, read without a scope tap per strip. UI-ARCHITECTURE §9; ADR-0050 d4; N9 (#381); parity-7.6 rows 2 and 13b; worklist B3.
- **On main (audited at ca0290c2):** engine N9 merged (#381); the window computes peak and RMS from the newest 1024 frames of a per-strip post-fader tap (`ui_mixer.cpp:231`, `ui_main.cpp:150-151`); the MIDI meter stays dark; no input metering exists because no input monitoring exists.
- **Proposed contract:** each visible strip loads `{peak, rms}` relaxed per frame from `Session::strips().stripFor(track)->meter()`, resolved again after a refresh; ballistics and the Peak Level hold stay the window's; the strip opens no `ScopeTap`. While a track monitors its input (MX-09) the meter shows the input, as Live. If a 60 Hz read misses overs the old 1024-frame tap caught, the engine adds a held peak as a new needs row.
- **Reuse:** none needed (engine built). Program V1.
- **Reject or defer:** reject the per-strip 0.1 s tap (C12). Defer K-meter, PPM and VU scales (program V25): additions the director has not asked for.
- **Research IDs:** A5C-001, A5C-004, A-BASE-18.1, A-BASE-18.1.1, M5-A5C-001.
- **Unresolved test (NOT RUN):** a new AWAITING row from win: Live 12's meter ballistics (peak release, RMS window, Peak Level hold) and when the MIDI meter lights (program WN8); M5-A5C-001.

#### MX-02 Split Stereo Pan

*decided* · size M · evidence documented · lane window · milestone M1 · after: none

- **Need and current decision:** Live's Split Stereo Pan Mode from the pan control's menu: two sliders place left and right independently; per-pan automation. ADR-0203 d9; FEATURES §2 P1; N12; worklist B7.
- **On main (audited at ca0290c2):** engine merged (`mixer_pan_settings` 1.11, the three ops, `StripNode::setPanMode`, (its source file); `adi_split_pan_tests` green); the window's item is disabled (`ui_mixer.cpp:843-849`).
- **Proposed contract:** the menu ticks from `panMode` and writes `mixer.setPanMode` (one undo step); a split strip draws Live's two sliders in the Mixer View and the track controls; each drag is one step of `mixer.setPanLeft` or `mixer.setPanRight`; double-click resets; the two pans' lanes come with the automation lanes (C7 of the worklist).
- **Reuse:** none (engine built, #392).
- **Reject or defer:** no Width or phase control on the strip (ADR-0203 d9 keeps them in Utility; C6).
- **Research IDs:** A5C-003, A5C-023, A-BASE-18.1, M5-A5C-003.
- **Unresolved test (NOT RUN):** A5C-023's open point (Split Stereo gains) in the M-AUDIO pan cases; Live's lane names for the two pans.

#### MX-03 Pan law, 64-bit summing and the Processing Precision switch, measured against Live

*decided* · size S · evidence documented · lane win · milestone M4 · after: MX-33, MX-22

- **Need and current decision:** Live's pan law by default with three more per track (ADR-0163 d2; its open item: Live's +3 dB at a hard side is a prediction), 64-bit accumulation at every mix point with 32-bit processing and a 32/64 switch (ADR-0205 d6, ADR-0207 d2).
- **On main (audited at ca0290c2):** `panGains`/`PanLaw` ((its source file)), double sums (#227), the switch (#229, `audio.processingPrecision` default 32); `adi_precision_tests` and `adi_mixer_tests` green; ADI's engine has never been compared with Live (measurement report, gap 3).
- **Proposed contract:** keep the built behaviour; close it with records: AWAITING row 5's readings at C, 25R and 50R for a −6 dBFS sine (ADI predicts −6.0/−6.0, −11.3/−3.7, silent/−3.0); the precision_A/B/−A cancellation into one bus in two orders, then through a nested bus (M6-A6G-006); undithered render neutrality (A6G-007); Live's mono output sum at −6 dB (A3-009) where ADI offers mono outputs. A Live law that differs opens a new ADR, as ADR-0163 says.
- **Reuse:** MS1 (pyfar signals), MS5 (auraloss ESR for anything that cannot null), the harness's residual core (MX-33).
- **Reject or defer:** no change to ADI's law before the readings; no 64-bit default (ADR-0207 d2).
- **Research IDs:** A5C-023, A3-009, A6G-006, A6G-007, A6G-010, E06, M-AUDIO, M-AUDIO-PRECISION, M6-A6G-006, A-BASE-38.2.4.
- **Unresolved test (NOT RUN):** AWAITING 5; M-AUDIO-PRECISION; M6-A6G-006, -007, -010.

#### MX-04 Track Delay in milliseconds, and Live's Delay Compensation switch

*decided* · size S · evidence documented · lane window, win · milestone M1 · after: none

- **Need and current decision:** Live's Track Delay on every strip in ms, either sign, shown through Track Options; Live's Delay Compensation on/off in the Options menu, with Track Delay unavailable while it is off. ADR-0172; ADR-0203 d4; FEATURES §2 (P1) and §8; N22, N24; worklist B4.
- **On main (audited at ca0290c2):** engine built (ADR-0172; `TrackNode::delaySamples`, #384; `SessionSpec::delayCompensation`, #399; `adi_delay_compensation_tests` green); the window's D says "Track Delay: not built yet" (`ui_mixer.hpp:255`); no registry row (N24 open on win).
- **Proposed contract:** a value field in ms writes `mixer.setDelay` with samples = ms × rate / 1000 (one undo step) and reads 1000 × samples / rate; shown by D and the Track Options items, kept per window. Win adds `audio.delayCompensation` (Bool, on) and the Options item, which drives `Session::setDelayCompensation`; the field greys while it is off (the engine keeps the stored value and suspends it). Range ±1 s until AWAITING 8 reads Live's.
- **Reuse:** none (engine built).
- **Reject or defer:** sub-sample Track Delay is P2 with Audio Alignment (ADR-0203 d4; program D25). REAPER's per-chain and per-FX PDC modes (R10H-011) are not Live's.
- **Research IDs:** A3-012, A6H-034, R10H-011, N10H-048, A-BASE-18.7, A-BASE-23.6, M6-A6H-034.
- **Unresolved test (NOT RUN):** AWAITING 8 (range, the selector, Main and returns); AWAITING 7(h) (does Track Delay move a Pre FX or Post FX tap); M6-A6H-034 (clock-synced devices after a latent device).

#### MX-05 Returns and sends as Live

*decided* · size S · evidence documented · lane window · milestone M1 · after: none

- **Need and current decision:** one send knob per return on every track and group; Pre/Post one switch per return that holds with no sends; a return's own sends off until Enable Send; a return may feed itself; a new send fully down; New's A Reverb and B Delay; Insert Return Track. ADR-0203 d3; FEATURES §2 (P0); N39, N41, N42; worklist B1, B2, F2.
- **On main (audited at ca0290c2):** engine built (`plan.cpp` sends pass, `SendNode`, `FeedbackLoop` with D = the largest block, `return_send_taps`, `maxRoutingId`, New's returns; their suites green); window per-send batch (`send_edits.hpp:85-108`) and old id rule (`send_edits.hpp:62`).
- **Proposed contract:** B1: the switch reads `returnSendTap` and writes one `return.setSendTap`, no refusal without sends, Main's Sends section follows; B2: new ids from `maxRoutingId + 1`, consecutive in a batch, refused at INT64_MAX; F2: parity notes for New's returns. New sends start at −inf until AWAITING 7(a); return-to-return rings stay refused until 7(f); Pre sends follow solo (`preFollowsSolo`) until 7(d).
- **Reuse:** none.
- **Reject or defer:** per-send Pre/Post and a Pre-FX send tap rejected (ADR-0203 d3; C10).
- **Research IDs:** A5C-009, A6S-001, A-BASE-18.4, L02, L03, L10, N10H-044, R10H-002 (as corrected, C10).
- **Unresolved test (NOT RUN):** AWAITING 7(a)-(f), (i); L02 (which Pre/Post state is lit), L03 (a new return's name, letter, colour), L10 (Sends visibility); the send knob's taper and the boxes' wrap past three returns (parity-7.6 row 9).
- **Cross-check and this plan:** XC-22: B2's ids through the one helper (M1), then the one allocator.

#### MX-06 ADI's sends into any track, and a pan on a send

*decided* · size M · evidence documented · lane window · milestone M1 · after: none

- **Need and current decision:** ADR-0203 d3 (sends-choice-4): a send into a track that is not a return, and a pan on a send, made from the send slot; Live's one-knob-per-return layout stays the default look.
- **On main (audited at ca0290c2):** the planner plans sends into any non-master track and `SendNode` pans; Hearing handles non-return destinations (`mixer.cpp`, `sentTo`); the window cannot create one or pan a send.
- **Proposed contract:** the send slot's menu offers "Send to Track…" listing eligible tracks (never Main: the planner refuses a send into the master; never itself unless it is a return); creating one is `routing.connect` kind `send` plus `routing.setGain`, one undo step; a send knob's menu shows Pan (`routing.setPan`, one step); a send into a summing group keeps its channel half (built).
- **Reuse:** none (engine built).
- **Reject or defer:** Cubase's Direct Routing fan-out stays P2 (MX-16).
- **Research IDs:** C3-022, N10H-043, N10H-044.
- **Unresolved test (NOT RUN):** ADI-only: a send into a non-return track is compensated and follows Hearing's rule (a non-return destination opens only through a send that carries).

#### MX-07 Solo as Live, with its options

*decided* · size S · evidence documented · lane window · milestone M1 · after: none

- **Need and current decision:** Exclusive Solo (Ctrl adds), Solo in Place on by default, from the Solo button's menu and the Options menu; a soloed return hears its sends; a group's Solo half-coloured; solo defeat, Solo in front and a solo bus as options that do nothing until switched on. ADR-0163 d4; ADR-0203 d3, d11; N23; worklist B5.
- **On main (audited at ca0290c2):** `hearing()` built (`mixer.cpp:348-495`); the window has Exclusive Solo, S and the latch; no Solo in Place UI.
- **Proposed contract:** B5 as the worklist writes it: Solo in Place ticked from the snapshot, written by `mixer.setSoloInPlace` outside undo, saved with the project; Live's Options entry ("as the default behavior") maps to the same project field until measured. The engine's `preFollowsSolo` and `keepAllReturns` change only on Live readings. `mixing.soloInFront` and `mixing.soloBus` stay registry rows the engine does not read, labelled as not built in Settings.
- **Reuse:** none.
- **Reject or defer:** defer the engine for Solo in front and the solo bus until the Live base is done.
- **Research IDs:** A5C-011, A6H-007, A6H-008, A6H-009, A-BASE-18.6, M3-ROUTE, M5-A5C-011.
- **Unresolved test (NOT RUN):** AWAITING 5 (a group's child, a group, a sidechain key), AWAITING 7(c), (d), (g); M5-A5C-011; M6-A6H-008 (a Post Mixer receiver's solo).

#### MX-08 Cue, Cue Out, Cue Volume, the metronome and preview on Cue Out; the output stage

*decided* · size M · evidence documented · lane window, engine, win · milestone M1 · after: none

- **Need and current decision:** Live's cueing: the Solo/Cue switch, Cue switches with the headphone icon, Cue Out (another stereo pair), Cue Volume; the metronome to Cue Out; the browser's preview on Cue Out while cueing; and the approved automatic mute above a level limit, off by default. ADR-0203 d3, d9, d11; FEATURES §2 "Solo as Live" (P1); N23, N43; worklist B6.
- **On main (audited at ca0290c2):** engine merged (#412: `cue.setMode`, `setTrack`, `setOutput`, `setVolume`, `setReference`; PFL Post FX; click rendered into the cue bus; `adi_cue_monitor_tests` green); window Cue Out dimmed; `audio.autoMute` and `audio.metronomeOutput` unread; no preview audio path.
- **Proposed contract:** B6 as the worklist writes it (outside undo, the session refreshed after each). The engine's cue bus, ramps and reference stay as built; once the Live reading (below) arrives, the cue tap follows Live's. The engine reads `audio.metronomeOutput` (empty = Cue Out) and `audio.autoMute` with its limit, which exist as registry rows today. The browser preview reaches Cue Out when the preview path exists (ADR-0104; browser area).
- **Reuse:** none (engine built).
- **Reject or defer:** Cubase's four cue mixes, talkback, AFL/PFL choice and Listen Dim (C3-024, C3-025) wait for the Control Room (MX-18). No fallback onto another pair when Cue Out is missing.
- **Research IDs:** A5C-012, A-BASE-18.6, C3-023, C3-024, C3-025, C3-027, R10H-010, M-CUBASE-CUE, M3-ROUTE.
- **Unresolved test (NOT RUN):** a new AWAITING row (C8): Live 12's cue tap (before or after the fader and pan; with the Track Activator off), Cue Volume's range and default, the metronome and preview on Cue Out; M-CUBASE-CUE as comparison only.

#### MX-09 Input monitoring, Arm on the strip, Keep Monitoring Latency, Reduced Latency When Monitoring, latency calibration

*decided* · size L · evidence documented · lane engine, window, win · milestone M4 · after: MX-25 (the input grammar)

- **Need and current decision:** Live 17.1: a track's input passes through its chain to its output per the monitor mode (In, Auto, Off) and Arm; the meter shows the input while monitoring; Keep Monitoring Latency in Recorded Audio (18.8); Reduced Latency When Monitoring (23.6); Driver Error Compensation with "Measure round trip" and the tape-machine fourth mode (ADR-0203 d11). FEATURES §8: input monitoring modes P0; Reduced Latency P1; BUILD-BRIEF Phase D item 11.
- **On main (audited at ca0290c2):** not built. `track.setMonitorMode` stores a column nothing reads; recording (#406) captures driver inputs directly; Arm says "not built yet" (`ui_mixer.cpp:196`); the three registry rows have no engine reader.
- **Proposed contract:** the engine adds a live input node per monitored track (from `tracks.input_ref`, MX-25) feeding its junction per `tracks.monitor_mode` (Auto = armed and no clip playing, as Live); with Reduced Latency on, input-monitored tracks skip compensation (A3-012); Keep Monitoring Latency (N31, persisted) shifts the recorded placement by the monitored path's latency; Driver Error Compensation offsets every recording; Measure round trip runs MTDM on a loopback. The window wires Arm (`track.setArm`), the monitor switch and the Keep Latency toggles (worklist C6). The tape-machine mode follows Live's three.
- **Reuse:** MS2 (Ardour MTDM with its caller logic) for Measure round trip; MS4 (Tracktion sync spikes, both RNGs seeded) for loopback alignment tests.
- **Reject or defer:** Cubase's monitoring preferences (M5-C5W-030) as comparison only.
- **Research IDs:** A3-007, A3-008, A3-012, A5C-001, A5C-024, A6H-010, A-BASE-17.1, A-BASE-18.8, M3-ROUTE, M5-C5W-030, R10H-011.
- **Unresolved test (NOT RUN):** a new AWAITING row: Live 12's monitor default per track type in a fresh Set (A3-007's page says Off for audio and Auto for MIDI after reset; the intro says Auto); M3-ROUTE R1 (heard, recorded, placed); a loopback round trip on the director's interface (AWAITING 2 and 4 class).
- **Cross-check and this plan:** XC-15: needs the input grammar and the input table placed in M0; XC-34: MIDI tracks need SRF-05.

#### MX-10 Loudness meter on Main

*decided* · size M · evidence documented · lane window · milestone M1 · after: none

- **Need and current decision:** LUFS (momentary, short-term, integrated) and true peak on Main only, and in the Scope, the analyser and the export dialog; never on every bus. ADR-0203 d14 (A3-24); N13; parity-7.6 row 10; worklist B8.
- **On main (audited at ca0290c2):** engine merged (#382; `adi_main_loudness_tests` green); nothing drawn.
- **Proposed contract:** Main's strip draws M, S, I and TP from `Session::mainLoudness().readout()`, loaded relaxed each frame; a click resets; no other strip shows them; the picture goes to the director first (an ADI addition). The Scope, analyser and export readouts later read the same meter (MX-27).
- **Reuse:** LD7 (design: a readings snapshot with validity flags).
- **Reject or defer:** a meter on every bus rejected (ADR-0203 d14).
- **Research IDs:** C-U02, R10H-073, R11X-025.
- **Unresolved test (NOT RUN):** the meter's cost on Main at all times (A3-24's condition, unmeasured).
- **Cross-check and this plan:** XC-03: the numbers move to DEV-28's one meter when it lands.

#### MX-11 Native analog summing on groups and Main: the controls

*decided* · size M · evidence measured · lane window, win · milestone M2 · after: MX-23

- **Need and current decision:** the summing switch, flavour menu and drive dial in the group header and on Main (ADR-0173, ADR-0174: "the toggle, dial and flavour menu in the group header come with step 7"; ADR-0203 d14: the master summed too; Crosstalk P2).
- **On main (audited at ca0290c2):** engine built and admits the master (`summing.cpp:226-228`); no control in any `ui_*` file.
- **Proposed contract:** Summing on/off (`group.setSumming`), flavour (`group.setSummingFlavor`, eight), drive (`group.setSummingDrive`, −12 to +24 dB) on groups and Main; the drive dial is one gesture, one undo step (MX-23).
- **Reuse:** none.
- **Reject or defer:** Crosstalk is P2. FEATURES' "not built" for the master is stale (C9).
- **Research IDs:** none (an ADI addition).
- **Unresolved test (NOT RUN):** none against Live; ADI: one undo step per dial gesture.
- **Cross-check and this plan:** XC-17: the dial uses the one gesture contract.

#### MX-12 Small live scope and spectrum in every strip, and the analyser's big view

*decided* · size L · evidence inferred · lane engine, devices, window · milestone M4 · after: MX-30

- **Need and current decision:** ADR-0203 d14 (P1, with the Scope): drawn only while the strip is on screen; a click opens the Scope or the analyser on that track. FEATURES §2 (P1); ADR-0209 Q5.
- **On main (audited at ca0290c2):** taps and `dsp::Spectrum` built; `AnalyserModel` polls on its own thread; no strip scope or spectrum.
- **Proposed contract:** a visible strip subscribes one 96-point spectrum (sampled every 50 ms, 300 ms smoothing) and one 40-80-column min/max scope; a hidden strip holds none; a click opens the analyser's floating view (1,024 points, peak hold) or the Scope on that track; one tap per visible strip.
- **Reuse:** program V2 (lsp-dsp-units `Analyzer.h` reactivity), V3 (s(M)exoscope `processSub` column logic), V29; ADI's `dsp::Spectrum` and `RealFft`. Not ledger take rows (C13).
- **Reject or defer:** OpenGL for strips rejected (ADR-0183 d3 allows it for the spectrogram only); per-client analysis on the audio thread rejected (ADR-0184 d4).
- **Research IDs:** E09.
- **Unresolved test (NOT RUN):** program §4.2 budgets: the audio thread within +0.2 percentage points of one core with 16 visible strips; a worker pass under 4 ms.
- **Cross-check and this plan:** XC-19: its kernels and worker are MX-30's.

#### MX-13 The Scope window, and a minimal SCOPE & PHASE

*decided* · size L · evidence documented · lane window, engine · milestone M4 · after: MX-30

- **Need and current decision:** ADR-0167 (P1): any two tracks compared (overlay, stacked, difference, sum), correlation and offset within ±20 ms, one-click `mixer.setDelay`, windows on the grid, true peak, any tap including a device's output; ADR-0209 d12 builds SCOPE & PHASE from it (a P2 tab). BUILD-BRIEF Phase D item 16.
- **On main (audited at ca0290c2):** engine half built (ADR-0175); no device-output `ScopePoint`; no Scope panel in the window.
- **Proposed contract:** one Scope component, shown as a window (Compare in Scope from two selected tracks) and as the SCOPE & PHASE tab (two tracks first, eight later); heard-time alignment by stamps; Fix Delay proposes `mixer.setDelay` on B (one undo step); polarity is a per-layer view flip (ADR-0167 d2). A polarity fix on a track, if offered, sets Utility's Phase switch (ADR-0203 d9), never the strip's stored `phase_invert` (C6): win's ruling.
- **Reuse:** digest: FilmCraft's GCC-PHAT `find_offset` ((its source file)) to replace the brute-force `bestOffset` and give a sub-sample lag with polarity and confidence; program V5 (lsp-plugins-oscilloscope triggers) and V10 (x42 `stcorrdsp`). None is a ledger take row (C13).
- **Reject or defer:** sub-sample Track Delay waits for Audio Alignment (P2); the EXO stays inside the analyser device on its own buffer (ADR-0183 d5), P2.
- **Research IDs:** R11E-026, A7B-086, M6-A6G-011.
- **Unresolved test (NOT RUN):** a device-output tap's stamps against a declared latency; the GCC-PHAT port against FilmCraft's own cases (gains −20 to +10 dB, 0 dB SNR, echo, inverted polarity).

#### MX-14 The Mix View shell and the function keys

*decided* · size S · evidence documented · lane window · milestone M1 · after: none

- **Need and current decision:** F4 and the Mix field open and close the Mix View, docked or detached (ADR-0209 d9, d11); the function keys do functions (ADR-0203 d6); R3's switch and re-dock; R7's look later.
- **On main (audited at ca0290c2):** shell built (#320, #321, #374); C4's double binding.
- **Proposed contract:** F1-F8 lose Live's track activators (the director's deviation); F4 is the Mix View only; Activate Track 1-8 keep their menu entries without default keys. The look is a later round after his remarks on it (R7).
- **Reuse:** WI1 (window placement), WI2 (owner windows) for the detached window.
- **Reject or defer:** a second Mix View window deferred (program D13); cloned mixers rejected (ADR-0063, ADR-0180).
- **Research IDs:** B-H01, C3-021, C5W-001, C5W-002, C6-KEYBOARD.
- **Unresolved test (NOT RUN):** headless: F4 toggles the Mix View once and changes no activator.
- **Cross-check and this plan:** XC-09: one window item with WIN-01.

#### MX-15 The crossfader (P2, with Session View)

*decided* · size M · evidence documented · lane engine, window, win · milestone P2 · after: none

- **Need and current decision:** Live's crossfader: A/B assign per track (returns included), seven curves, mappable, automation for the position and each assignment; P2 with Session View (ADR-0203 d10); FEATURES §2 (format partly reserved: "needs a master-section table").
- **On main (audited at ca0290c2):** not built; the option is dimmed.
- **Proposed contract:** a crossfade gain at each assigned track's gain stage (no routing change: "an on-the-fly VCA group"); its storage in side tables (ADR-0144 d3 forbids new columns), placed by win when Session View starts.
- **Reuse:** none.
- **Reject or defer:** deferred to P2; the Mixer View option stays dimmed.
- **Research IDs:** A5C-010, A-BASE-18.5, B3-020, M5-A5C-010.
- **Unresolved test (NOT RUN):** AWAITING 16 (does a Post send follow the crossfader, Live 11 then 12); the seven curves' power levels against the manual's chart.
- **Cross-check and this plan:** XC-10: owner of the crossfader; TM-SV-05 refers here.

#### MX-16 Multi-output instruments, up to 64 buses per node, Direct Routing (P2)

*decided* · size L · evidence documented · lane engine, window · milestone P2 · after: MX-25

- **Need and current decision:** Live's tapping of a multi-output plug-in's other outputs from other tracks and multi-timbral MIDI (17.5.2.5-6); ADR-0113 (up to 64 buses per node, no cables, P2); Cubase's Direct Routing (P2). Instrument DSP is out of scope; only the routing is here.
- **On main (audited at ca0290c2):** not built; `Bus` has Main and Sidechain only (`graph.hpp:63`).
- **Proposed contract:** buses beyond Main and Sidechain become numbered node ports; Audio From can name "track X, output n" (MX-25's grammar); `routing.channel_map` carries partial channels; Direct Routing is several enabled outputs from one track.
- **Reuse:** none in the ledger.
- **Reject or defer:** deferred to P2; REAPER's 128 channels per track rejected (ADR-0113's 64 stands; R10H-001 is a documentary conflict).
- **Research IDs:** A6H-012, A-BASE-17.5.2, C3-022, R10H-001, R10H-003, R10H-005, R10H-006, R10H-007, M6-A6H-012.
- **Unresolved test (NOT RUN):** M6-A6H-012 (the plug-in's main output stays on its host track) with a multi-bus fixture plug-in.
- **Cross-check and this plan:** XC-15: "track X, output n" is a row in the input table, not text.

#### MX-17 System audio as a track input (P1), and the ADI virtual audio device (P2)

*decided* · size L · evidence documented · lane engine, win · milestone M4 · after: MX-09, MX-25

- **Need and current decision:** FEATURES §2: WASAPI loopback or Core Audio process taps as an input on any track, P1 (ADR-0106; ADR-0203 d5: "loopback is simply an audio input channel"); the virtual device carrying Main or any bus to the OS, P2.
- **On main (audited at ca0290c2):** `audio.loopbackSource` registry row only; no capture in the engine; `driver-build.yml` last green on 27 September (`b72f7b09`), not signed or shipped.
- **Proposed contract:** the loopback source appears as input channels in Audio From with its resampler latency declared; the virtual device is an output choice for Main or a bus; both obey Live's chooser rules.
- **Reuse:** none.
- **Reject or defer:** VB-CABLE bundling rejected (ADR-0106, 0117, 0118); the device stays P2.
- **Research IDs:** A6H-002, A-BASE-17.2.
- **Unresolved test (NOT RUN):** loopback capture latency measured with MTDM (MS2) and compensated; the driver signed and installed (CI is not installation).
- **Cross-check and this plan:** XC-40: system audio is listed in Settings' Privacy group.

#### MX-18 The Control Room (version 2)

*decided* · size XL · evidence documented · lane none until v2 · milestone P3 · after: none

- **Need and current decision:** FEATURES §2 Control Room (P3) and §14 ("v2, deferred, ADR-0125 d5"); ADR-0203 d9: until then monitoring is Live's.
- **On main (audited at ca0290c2):** reserved; the planner reports cue rows as "not planned yet" (`plan.cpp:108`).
- **Proposed contract:** deferred. Constraints recorded for v2: monitor-only processing (speaker or headphone correction) is workstation state, never in a render or the project (C3-026, R10H-010); cue mixes are per-channel sends with level, pan and Pre/Post (C3-024); talkback latch or hold; the existing Cue bus and the reference route become its first sources; `routing.kind='cue'` stays reserved.
- **Reuse:** none.
- **Reject or defer:** deferred (decided).
- **Research IDs:** C3-023, C3-024, C3-025, C3-026, C3-027, C5W-013, R10H-010, M-CUBASE-CUE.
- **Unresolved test (NOT RUN):** M-CUBASE-CUE (which route bypasses master processing; Main mute and dim against every cue path).

#### MX-19 Mixer snapshots (P2)

*decided* · size M · evidence documented · lane engine, window · milestone P2 · after: none

- **Need and current decision:** FEATURES §2 (Cubase, P2, `snapshots.kind='mixer'`).
- **On main (audited at ca0290c2):** schema reserved; not built.
- **Proposed contract:** deferred; recorded constraint: recalling a mixer snapshot is one undoable step in ADI's one journal, never a second history (C3-020).
- **Reuse:** none.
- **Reject or defer:** deferred to P2.
- **Research IDs:** C3-020.
- **Unresolved test (NOT RUN):** none now.

#### MX-20 Quick Kick Sidechain (P2): the routing now, the target after the list

*decided* · size M · evidence inferred · lane none now · milestone P2 · after: MX-24, MX-31

- **Need and current decision:** FEATURES §2 (P2; ADR-0209 d3, d11): a [Kick] button on every row and strip and a draggable pill wire the Kick role into a target with a compensated edge, Pre FX by default; dragging track A onto B keys B from A. Its target was the sidechain slot, withdrawn on 5 November.
- **On main (audited at ca0290c2):** not built.
- **Proposed contract:** the routing half is MX-24's sidechain to a device with the Pre FX tap. What the button engages or inserts waits for the director's list (the unified feature) and is not designed here.
- **Reuse:** none now (`dsp::rmsc` exists, ADR-0166).
- **Reject or defer:** the button, pill and drops wait for the list; any slot-based design rejected.
- **Research IDs:** A6H-013, N10H-045, N10H-046.
- **Unresolved test (NOT RUN):** AWAITING 7(h) (which tap a new sidechain chooser starts on in Live).

#### MX-21 The P2 analysis tabs, the AI Mix Radar and the docks

*decided* · size XL · evidence inferred · lane window, engine, devices · milestone P2 · after: MX-30, MX-12, MX-36

- **Need and current decision:** ADR-0209 d11 (P2) and d12: OVERVIEW, SCOPE & PHASE and SPECTROGRAM built from the existing analyser and Scope, with SPAN, s(M)exoscope and VISU as parity passes; the Info View switcher, the device strip's pinned slot, the Stream Deck following the Mix View; the AI Mix Radar (ADR-0209 d6).
- **On main (audited at ca0290c2):** the four tabs are drawn empty, as ruled.
- **Proposed contract:** as the program's §1.24 and §4.3: OVERVIEW is a SPAN-class overlay of pinned tracks with the statistics row and the correlation and balance meter; SPECTROGRAM a VISU-class reassigned spectrogram with OpenGL on that component only; the EXO inside the analyser device on its own buffer (ADR-0183 d5); the Radar through the agent's `analyze.*` and a Propose changeset. Parts that drew a slot (the pinned ADI-Q curve, "send node to ADI-Q") wait for the list.
- **Reuse:** program V7, V8, V9, V10, V16, V17 (Appendix F; C13); MX-27's meter for loudness.
- **Reject or defer:** deferred to P2 in CH-MX-2's order; K-meters and PPM (V25), the RGB waveform (V21) and chroma (V23) are additions not proposed.
- **Research IDs:** E09.
- **Unresolved test (NOT RUN):** program X19 to X21 (budgets, the spectrogram's rendering, the EXO's buffer).

#### MX-22 The Live mixer and routing measurement round

*decided* · size M · evidence unknown · lane win, measurement seat, director · milestone M4 · after: MX-33

- **Need and current decision:** ADR-0108's parity gate; LANES §5 (Live measurements); AWAITING rows 5, 7, 8, 15, 16; the window's "Measure on Live" lines L02, L05, L06, L09, L10.
- **On main (audited at ca0290c2):** AWAITING 0 of 17 struck; L01-L10 on main in `docs/daws/ableton-12.md` (#436), all unresolved; no Live 12 driver.
- **Proposed contract:** generated Live 12.4 sets per case family (pan law; sends and returns 7(a)-(i); Track Delay; group mute; the crossfader and Post sends; the cue tap; meter ballistics; monitor defaults), rendered and compared through the harness (MX-33); GUI cases through the logger's case protocol (baseline, action, Undo, Redo, reopen); win relays only from records labelled with the Live build; a Live result that differs opens a lane item, or an ADR where an ADR predicted.
- **Reuse:** MS3 (pywinauto Live driver), MS1, MS5; the logger's `case.py` (measurement plan step 6).
- **Reject or defer:** no answers from manuals or controller palettes; never set Live parameters through its API for automation or undo cases (A6P-003).
- **Research IDs:** L02, L05, L06, L09, L10, M3-ROUTE, M5-A5C-001, M5-A5C-009, M5-A5C-011, M5-A5C-012, M6-A6H-007, M6-A6H-008, M6-A6H-034, M-CUBASE-CUE, A6P-003.
- **Unresolved test (NOT RUN):** every case above; rows 14 and 16 need a fresh Live 12 profile.
- **Cross-check and this plan:** XC-38: one measurement seat and one queue of readings for every area.

#### MX-23 Drags heard while dragging, one undo step, multi-selection

*refinement* · size M · evidence documented · lane win, engine, window · milestone M2 · after: none

- **Need and current decision:** Live: a fader moved with the mouse is heard as it moves and is one undo step; with several tracks selected, moving one control moves the others and keeps their differences (18.2); a drag that ends where it began writes nothing. OpSubmitter leaves the coalescing of non-parameter drags to win "with ADR-0124's criterion in hand"; ADR-0003, ADR-0110.
- **On main (audited at ca0290c2):** volume and pan commit on drag end only (`ui_mixer.cpp:146-157`), so the sound follows on release; a drag back to its start still commits; a selection's other tracks do not move. `ParamOps::uiEvent` (#390) covers plug-in parameters only.
- **Proposed contract:** win rules "a gesture is previewed, then committed once": a mixer drag (volume, pan, split pans, send level, Track Delay drag, summing drive) runs a gesture session: values reach the engine live without journaling; End commits one transaction of the final value; Esc reverts; an unchanged end commits nothing; with N selected tracks the gesture is one transaction of N ops with relative changes.
- **Reuse:** HI1 (ossia score's ongoing-command contract), HI2 (preview-from-origin tests), HI6 (a merged gesture costs one step at a full history), HI7 (editing-step rules), HI8 (the Esc contract).
- **Reject or defer:** a UI-layer list of coalescable ops rejected (OpSubmitter's own reasons); journaling every value rejected.
- **Research IDs:** A5C-002, A-BASE-18.2, M5-A5C-002, M-LIVE-UNDO, C6-HISTORY.
- **Unresolved test (NOT RUN):** M-LIVE-UNDO rows for fader, pan and send drags (one step, its label); C6-HISTORY.
- **Cross-check and this plan:** XC-17: the mixer face of the one gesture contract; feeds AU-03 while armed.

#### MX-24 Sidechain keys reaching a device, with Live's three taps

*decided (was refinement; XC-07)* · size M · evidence documented · lane win, engine, window · milestone M4 · after: none

- **Need and current decision:** FEATURES §2: sidechain routing (P1) and "Sidechain and track-to-track tap points: Pre FX, Post FX, Post Mixer, as Live; only Post Mixer follows fader, mute and solo" (ADR-0203 d3; BUILD-BRIEF Phase A item 4; ADR-0058 d5: a sidechain is compensated with the rest).
- **On main (audited at ca0290c2):** inert (§1; C5).
- **Proposed contract:** a sidechain row may end at a device (`dst_kind='device'`, which SPEC allows): the realiser connects the source's tap node to that device's node on `Bus::Sidechain`, compensated; the tap (`pre_fx`, `post_fx`, `post_mixer`) is stored in a new side table keyed by routing id, placed by win (ADR-0144 d3: no new column), absent meaning Post Mixer (today's Tail); a key keeps nothing audible (built); bounce and render keep keys (reject C-E03's limitation). Window: Live's sidechain chooser on devices that declare a sidechain input, the tap chooser under the source chooser.
- **Reuse:** none in the ledger; RK1 (Tracktion's rack graph with its PDC tests) as a design reference for nested compensation.
- **Reject or defer:** Reason's compensation gaps (N10H-045, -046; N11H-035, -036) are counterexamples, not targets; REAPER's blanket PDC disable with feedback (R10H-012) rejected.
- **Research IDs:** A6H-008, A6H-009, A6H-013, A-BASE-17.5.1, N10H-045, N10H-046, N10H-047, N11H-035, N11H-036, N11H-037, C-E03, R10H-012.
- **Unresolved test (NOT RUN):** AWAITING 7(g) (does ducking stop when the key is muted or solo-silenced), 7(h) (the default tap); engine tests: a key into a fixture compressor's sidechain arrives aligned at each tap; an unknown or broken path reports itself, never a silent zero (N11H-037).
- **Cross-check and this plan:** XC-07: decided, one row with DEV-05 as owner; XC-15: taps from the one vocabulary.

#### MX-25 Track-to-track routing and live In/Out choosers

*refinement* · size L · evidence documented · lane win, engine, window · milestone M4 · after: MX-24 (the tap table)

- **Need and current decision:** Live 17 and 17.5: Audio/MIDI From and To per track (Ext. channels, another track at Pre FX, Post FX or Post Mixer, Resampling); tapping leaves the source's output alone and allows many consumers (A6H-007); Sends Only, the fourth state of ADR-0065 (ADR-0203 d3); No Output; a change applies to every selected track; configured channel names in the choosers; MIDI From another track taken after its MIDI effects (A6H-014).
- **On main (audited at ca0290c2):** fixed choosers (look-live11 row 5, `ui_mixer.cpp:538`); no input path or input-tap edge; a disabled main row falls back to the default route (`plan.cpp:98`), so Sends Only cannot be stored; configured channel names do not reach the choosers.
- **Proposed contract:** win places (i) the input grammar in `tracks.input_ref` (hardware mono or pair, loopback, ~~`track:<id>` with its tap,~~ *[XC-15: a track source is a row with real foreign keys in a new table, never a track id in text]* resampling) and (ii) Sends Only and No Output as an output state in a side table (`routing.kind` has a CHECK, so no new kind in a minor). The engine plays an input tap from another track as a non-exclusive edge from the source's tap into the destination's input, governed by its monitoring (MX-09); Resampling takes Main and excludes the recording track's own output (A6H-006). The window draws Live's choosers writing `track.setInput` and the routing ops, one undo step for all selected tracks.
- **Reuse:** none in the ledger.
- **Reject or defer:** a cable or matrix UI rejected (ADR-0113); REAPER's pin mapping (R10H-003) waits with the 64 buses (MX-16).
- **Research IDs:** A6H-001, A6H-002, A6H-006, A6H-007, A6H-011, A6H-012, A6H-014, A3-009, A-BASE-17.2.1, A-BASE-17.4, A-BASE-17.5, L05, R10H-003, R10H-007, R10H-008.
- **Unresolved test (NOT RUN):** M6-A6H-007, -008, -011 (tap against send-to, and their solo consequences); L05 (a grouped track's output label); A3-009 (mono and stereo conversions); AWAITING 7(h).
- **Cross-check and this plan:** XC-15: corrects the matrix's "track:<id> in input_ref" to foreign-key rows, so copy, templates, import, rebase and undo remap them.

#### MX-26 Group mute semantics, and the registry default

*refinement* · size S · evidence unknown · lane win, engine · milestone M4 · after: MX-22

- **Need and current decision:** ADR-0203 d7: whether muting a group mutes its tracks follows Live once measured; Live's grouping reroutes only Main-bound children (A5C-007).
- **On main (audited at ca0290c2):** the key has no reader.
- **Proposed contract:** until the reading, the engine keeps the structure it has (a group's mute is its output; children keep sending), and `mixing.groupMuteMutesSources` defaults to Live's measured behaviour; if Live has no such option the key leaves the registry or stays an ADI option, off.
- **Reuse:** none.
- **Reject or defer:** the Cubase default (true) rejected as the default (C7).
- **Research IDs:** A5C-007, A5C-008, A-BASE-18.3, M5-A5C-007.
- **Unresolved test (NOT RUN):** AWAITING 15, plus: are a muted group's children still heard through their sends in Live 12?

#### MX-27 One loudness meter, tested against independent references

*refinement* · size M · evidence documented · lane win, devices, engine, window · milestone M4 · after: MX-10

- **Need and current decision:** ADR-0203 d14's readings (Main's meter, export LUFS targeting, Auto Gain Stage's integrated mode, the Reference's match) from one meter. Today three integrated-LUFS paths exist (`gain_stage.cpp`, `main_loudness.cpp`, (its source file)) and the true-peak design is copied; 100 ms hops read Tech 3341 cases 13 and 14 low; no LRA, max M/S or pause.
- **On main (audited at ca0290c2):** K-weighting, true peak, N13, normalization and the gain stage built; every test checks ADI against itself.
- **Proposed contract:** win's ADR "one meter": `dsp::LoudnessMeter` on 10 ms sub-blocks with M, S, I, LRA (Tech 3342 at 10 Hz), max M and S, run and pause, 0.01 LU exact-energy histograms, explicit channel weights and ADI's double `TruePeakMeter`; N13, export normalization, Auto Gain Stage and the Reference read it; no "EBU Mode compliant" claim.
- **Reuse:** LD1 (FilmCraft's meter) first; LD3 (libebur128, test-only), LD4 (Tech 3341/3342 signals synthesised), LD6 (ffmpeg as an optional LRA oracle), LD5 (export's refine loop and the true-peak path, one limiter change with EX4), LD7 (readings design). LD2 (ADI-L's own meter) waits for the director's list (C11).
- **Reject or defer:** the EBU WAV set is not usable; libebur128's LRA (1 Hz hop) not used; Ardour's `lufs_meter` and lsp's meters rejected (ledger §4).
- **Research IDs:** C-U02, R10H-073, R11X-021, R11X-025, R11X-047.
- **Unresolved test (NOT RUN):** Tech 3341 cases 1-6 and 9-23 and Tech 3342 LRA 1-4 within tolerance; within 0.1 LU of libebur128 on pink, speech and sweep at 44.1, 48 and 96 kHz; the Main meter's CPU cost.
- **Cross-check and this plan:** XC-03: one row with DEV-28 as owner.

#### MX-28 Performance Impact indicators and the CPU Load Meter

*refinement* · size M · evidence documented · lane win, engine, window · milestone M7 · after: none

- **Need and current decision:** Live 12's Performance Impact indicators in the mixer (18.9: six rectangles per track, relative within the Set, shown from the Mixer Controls submenu) and the control bar's CPU Load Meter (37.1.1: audio processing time over buffer time; Average, Current or off; an overload warning). Live does both, so they are Live's way under ADR-0203 d1; FEATURES has no row; parity-7.6 row 19 dims the option.
- **On main (audited at ca0290c2):** not built; the CPU meter is drawn with no data; graph workers are open PR #441.
- **Proposed contract:** the engine publishes per-callback deadline use (audio work only, never UI) and per-track node time; the window draws Live's CPU meter with its options and the six-rectangle indicators; win adds the two FEATURES rows (priority win's placement, to strike if wrong).
- **Reuse:** GT4 (Ardour's `dsp_load_calculator` design, verifier note), GT6 (JUCE's `getCpuUsage`, `getXRunCount`, in tree).
- **Reject or defer:** ranking DAWs by their meters rejected (R10H-014, A5C-019).
- **Research IDs:** A5C-019, A6G-003, A-BASE-18.9, A-BASE-37.1.1, R10H-014, N10H-050, N10H-051, M-CPU, M5-A5C-019.
- **Unresolved test (NOT RUN):** M-CPU and M5-A5C-019 at equal workloads; how Live scales the six rectangles.
- **Cross-check and this plan:** XC-08: one row with DEV-24 as owner.

#### MX-29 The Reference that sounds: role storage, matched loudness, spot play, drops on the strip

*refinement* · size M · evidence inferred · lane win, engine, window · milestone M4 · after: MX-08, MX-27

- **Need and current decision:** FEATURES §2 (P1; ADR-0209 d4, d9, d11): the Reference on the timeline, Warp off, hidden by default, exclusive with Main bypassed, straight to the monitor and Cue Out, a 5 ms swap; Shift+R and [REF] tap or hold; Alt+hold auditions a spot; LUFS match. N38 (win's format half open); program N51, N53, N55, E15, E17.
- **On main (audited at ca0290c2):** the route is merged (#412); `track.setRole` refuses until the format exists (`ops_catalog.cpp:362`); the window's strip, [REF] and Shift+R are built (#323) and name no track.
- **Proposed contract:** roles live in a side table `track_roles (track_id PRIMARY KEY, role)` with a partial unique index on `reference` (ADR-0209 Q8's column corrected under ADR-0144 d3; C3); the engine repoints `snapshot.cpp`, `cue_settings.cpp` and `ops_catalog.cpp` from the column probe to the table (N53); the first Reference writes an essential extensions row and readers warn before a render (SPEC §9.1, E17); a matched-loudness gain on the cue path from short-term loudness, Match on by default, nothing written to the project (N51); Alt+hold spot play as a performance op outside undo (E15, N55); a file dropped on the strip lands on the hidden Reference track, Warp off.
- **Reuse:** none beyond MX-27's meter.
- **Reject or defer:** a separate reference bus rejected (ADR-0209 Q7); Bluprint's matcher, the four-stem split and phone translation are P2.
- **Research IDs:** C3-025, C3-026, R10H-010.
- **Unresolved test (NOT RUN):** ADI tests: the role round trip on the table, a second Reference refused, a 1.19 reader keeping the table, the cue path deriving the Reference from it; program X15 (which loudness window matches by ear).
- **Cross-check and this plan:** XC-37: 1.20 is the roles table alone; XC-13: PD-14's CTZ template needs this table.

#### MX-30 Taps and the visual worker

*refinement* · size L · evidence inferred · lane win, engine, devices · milestone M4 · after: none

- **Need and current decision:** the plumbing behind MX-12, MX-13 and MX-21: ADR-0167 d1 (any device's output), ADR-0175 (taps, stamps), ADR-0183 d4 (no clock of its own), ADR-0195 d5 (one worker), ADR-0209 Q5 (one tap per visible row); program E5, E7, DV1, N44-N46.
- **On main (audited at ca0290c2):** three tap points; a division per frame; the first caller's length wins (`session.cpp:939-947`); `closeScope` detaches and frees nothing (`session.cpp:949-961`); the mixer never closes its taps.
- **Proposed contract:** win's ADR amends ADR-0175 d2 (stamps kept per block in a side ring) and d4 (a tap retired by epoch 2 s after its last consumer, or pooled; never freed under a stalled callback); device input and output tap points; gapless cursor reads; a ring as long as its longest consumer; one visual worker per process paced by the windows' VBlank, one STFT per tap per hop and resolution, a 4 ms budget with a declared degradation order; analysis kernels as pure devices-lane functions shared with the agent's `analyze.*` and export.
- **Reuse:** program DV1 kernels (`spectrum_grid`, `correlation_meter` after x42's `stcorrdsp`, `scope_decimator`), Appendix F (C13); ADI's `dsp::Spectrum`, `RealFft`, (its source file).
- **Reject or defer:** a fourth audio-to-UI path only by ADR (program P-E); analysis on the audio thread for a client rejected (ADR-0184 d4).
- **Research IDs:** E09.
- **Unresolved test (NOT RUN):** program §4.2 budgets (X4, X10); a stalled-callback test that never writes freed memory; bit-identical audio with the Mix View open and closed.
- **Cross-check and this plan:** XC-15: device tap points join the one vocabulary; XC-19: CTL-07 serialises this worker's outputs.

#### MX-31 Roles, the CTZ template, the navigator and CASCADE, without slot data

*refinement* · size L · evidence inferred · lane win, engine, window · milestone P2 · after: MX-29, MX-30

- **Need and current decision:** FEATURES §2: Kick, Bass and Vocal roles, the CTZ hierarchy as a bundled template, the CTZ navigator (P2; ADR-0209 d2, d10, d11), the CTZ CASCADE tab (P2).
- **On main (audited at ca0290c2):** not built; `track.setRole` accepts kick, bass, vocal and reference but refuses until the role table lands.
- **Proposed contract:** roles in `track_roles` (MX-29); Set Role in the track, strip and STACK menus (one step; the Reference unique, the others may repeat, the first in track order used where one is needed); the CTZ template in the User Library, Live's default Set staying the default (ADR-0209 d10); navigator blocks show each group's peak, RMS, LUFS-S and crest from group taps (program E10, N50); tiers from a small side table the template writes, with a name-match fallback (program D22, win's placement). The shaving and breathing bars and CASCADE's stage columns stay empty until the unified feature: their data came from slots.
- **Reuse:** MX-27's meter for LUFS-S; program V16 (crest, PLR).
- **Reject or defer:** per-rebuild "CTZ group metadata" written by the engine rejected (program P-U; LANES §3 item 2's line to strike); slot-derived shaving waits for the list.
- **Research IDs:** none (an ADI addition).
- **Unresolved test (NOT RUN):** program X17 (the navigator's update rate) on the director's songs (WN13).
- **Cross-check and this plan:** XC-13: PD-14 owns the template mechanism, this row the CTZ content.

#### MX-32 Hosted plug-ins learn their track's name and colour

*refinement* · size S · evidence unknown · lane engine · milestone M4 · after: none

- **Need and current decision:** the director's VST3s, and FabFilter's own instance lists, show ADI's track names and colours (program E11, N49). Whether Live does this is unread.
- **On main (audited at ca0290c2):** not built.
- **Proposed contract:** after loading and on every rename or recolour, call `AudioPluginInstance::updateTrackProperties({name, colour})` (JUCE passes VST3's `IInfoListener::setChannelContextInfos`); serve `clap.track-info` from `ClapHostGlue::getExtension`. If Live 12 does the same (a fixture VST3 logs it), this is Live's way; if not, it is an addition and goes to the director for a yes.
- **Reuse:** none in the ledger (JUCE in tree).
- **Reject or defer:** CLAP drafts (gain-adjustment metering, mini curve display) deferred.
- **Research IDs:** none (to be measured).
- **Unresolved test (NOT RUN):** a fixture VST3 records the channel context in Live 12 and in ADI.
- **Cross-check and this plan:** XC-34: the Live reading comes first.

#### MX-33 The measurement harness

*refinement* · size L · evidence documented · lane win, engine · milestone M4 · after: none

- **Need and current decision:** LANES §1 (win owns measurements against Live); ADR-0108; AWAITING. The measurement report's gaps: no Live 12 driver or seat, a one-device kit, no record schema, no sub-sample or loopback tools, no metric for what cannot null.
- **On main (audited at ca0290c2):** the Live 11 macOS kit for Multiband Dynamics only; the logger unverified on a real Live; integer-lag correlation only.
- **Proposed contract:** win's ADR "the measurement harness": the record schema, a gate vocabulary declared before each run, the seat table, the relay rule (AWAITING rows struck only from records), nothing from Ableton tracked in the repo. `tools/measure/` from the live-probe kit (moved with its history), a Windows Live driver, `analyse.py`, fixtures imported by SHA-256; the engine's `src/adi/measure/` (residual compare, MTDM with Ardour's caller logic, sync spikes, probe signals) and `adi_measure` (gen, build through ordinary ops, render per track, compare, latency); `audio_probe --loopback`.
- **Reuse:** MS1 (pyfar and pyrato), MS2 (Ardour MTDM), MS3 (pywinauto), MS4 (Tracktion spikes, both RNGs seeded), MS5 (auraloss formulas); MS6 design only.
- **Reject or defer:** DawDreamer, Plugalyzer and pedalboard never gate (block-granular automation); no `.als` importer.
- **Research IDs:** M-AUDIO, M-AUDIO-PRECISION, M-CPU, C6-AUDIO, M6-A6G-011.
- **Unresolved test (NOT RUN):** the measurement plan's acceptance A1-A5 (selftests in CI on three systems, fixture hashes, MTDM at 256 and 256.5, spikes at 1234, renders bit-identical across block sizes and workers).
- **Cross-check and this plan:** XC-38: the one harness for every Live reading in every area.

#### MX-34 The governing texts follow the 5 November ruling

*refinement* · size S · evidence documented · lane win · milestone M0 · after: none

- **Need and current decision:** the withdrawal of per-channel slots is recorded only in the needs table (N16). C1 lists the texts that still describe slots.
- **On main (audited at ca0290c2):** N16 withdrawn in `collab/window.md` on main (`5e8dfe46`); the other texts unchanged.
- **Proposed contract:** win records the ruling where the lanes read it: the three FEATURES §2 rows marked withdrawn by the ruling of 5 November (with N16); LANES §3 item 2 and §4 items 1-3 no longer name slots; BUILD-BRIEF §5's row and Phase D items 7 and 7a ~~point to CH-MX-1~~ *[XC-06: the Drive is built as gain only; CH-MX-1 is no longer a question]*; UI-ARCHITECTURE §12 and the two parity checklists say the slot drawings stay as drawn until the list; the stale text of C9 corrected. Code stays as drawn (N16). This matrix adds no ADR; whether the record is an ADR is win's and the director's call. **This plan's answer (P-6):** one short record ADR in his words, amending ADR-0203 d14's cascade, ADR-0204 d1 and ADR-0209 d11 and designing nothing, because LANES §2 says a decision is a new ADR and the docs win over the code; he strikes it if he prefers no entry.
- **Reuse:** none.
- **Reject or defer:** no redesign of the unified feature.
- **Research IDs:** none.
- **Unresolved test (NOT RUN):** a text check: no queue or FEATURES row asks for "present but off" slots.
- **Cross-check and this plan:** XC-27: widened to every text the cross-check lists.

#### MX-35 The Loudness Drive after the ruling (CH-MX-1)

*refinement (was new-choice; XC-06)* · size M · evidence inferred · lane engine, window, win · milestone M4 · after: MX-34

- **Need and current decision:** FEATURES §2: the Loudness Drive (P1; ADR-0203 d14: "one control that scales every track's level into the whole clipper cascade", its mechanism a VCA over the leaf tracks' input gain). ADR-0204 d1 made the per-channel ADI-L instances its stages, engaged by the Drive; those are withdrawn (C2). The engine log of 4 November: "Whole Loudness Drive cascade handoff still blocked".
- **On main (audited at ca0290c2):** not built; `InputGainNode` ((its source file)) and Auto Gain Stage ((its source file)) exist.
- **Proposed contract:** one automatable dB control (on Main, or the master section when one exists) that adds the same gain to every leaf track's input gain (the input gain node after the instrument, before the inserts), never multiplying groups, returns or Main again; 5 ms ramps; one undo step per gesture; exports render it; it engages nothing: the cascade is whatever clippers and limiters the user put on the tiers, his own plug-ins as ordinary devices. Whether it later engages built-in stages is part of the unified feature.
- **Reuse:** none needed (the input gain node and Auto Gain Stage are built); MX-27's meter for its readout.
- **Reject or defer:** VCA fader tracks rejected (ADR-0203 d14); stage engagement deferred to the list.
- **Research IDs:** none.
- **Unresolved test (NOT RUN):** ADI tests: +6 dB on the Drive raises every leaf's input by 6 dB and leaves the balance where the chain is linear; export equals playback; undo restores.
- **Cross-check and this plan:** XC-06: no longer a director question; told to him as a placement to strike.

#### MX-36 The Mix View and STACK before the unified feature (CH-MX-2)

*new-choice* · size L · evidence inferred · lane window, engine, win · milestone M4 · after: MX-30, MX-29, MX-34

- **Need and current decision:** ADR-0209 d11 made STACK "the one window" of every channel's slots; the ruling withdrew the slots and puts the unified feature in the Mix View once his list arrives; the window's worklist finds nothing unblocked in the Mix View; the program proposes STACK v2 rows of each track's own chain devices with the fisheye, size slider, navigator, search, pins and live row spectra (its D2(a)-(c)), and the kick-and-bass items earlier (D2(e), (f)).
- **On main (audited at ca0290c2):** STACK on `StubMixViewProvider` (#322): every row created even off screen, a relayout of every row on hover, three slot drawers per row, no device model; parity-mix-view 7 of 9 built.
- **Proposed contract:** build the non-slot parts now, in this order: the Reference (MX-29), the strip scope and spectrum and the analyser view (MX-12, P1), the Scope window (MX-13, P1); then STACK as an instance list of each track's existing devices (Live's built-ins and plug-ins, his VST3s among them) with the fisheye, slider, navigator, search, pins and one spectrum per visible row; tiles read-only except ADI's own devices (EQ Eight first), edited in place through the parameter capture; the drawn slot columns stay drawn and off until the list, ~~then become the unified feature's place~~ *[Struck (XC-27): this plan does not place the unified feature]*.
- **Reuse:** program W1-W5 and W13 (the model, canvas, providers, painters, the analyser view); HI1 for in-place edits; WT-1 and WT-6 for STACK's goldens.
- **Reject or defer:** the program's slot work is superseded (C1); any ADI-Q-specific tile or editor waits for the list.
- **Research IDs:** C3-021, A7A-062.
- **Unresolved test (NOT RUN):** program X1-X3 (the row against his video, the fisheye's feel, any track of 100 found in under 2 s); WN8's side-by-side with Pro-Q 4's list in Live 12.
- **Cross-check and this plan:** XC-27: the placement phrase is struck.

### A.5 Project, export and data safety (36 rows; source the project-data matrix (win's working file))

#### PD-00 Main green on macOS for Save As, the ZIP and NI Stems

*decided* · size S · evidence measured · lane win · milestone M0 · after: none

- **Need:** Save As, Save a Copy, Collect and Export and NI Stems work on every platform, and main is green on every leg before the window's File menu (worklist A1, A4) calls them.
- **Decision now:** LANES §2 (a red heavy leg on main is win's, at the head of the queue, before any merge); SPEC §3.7, §3.8; ADR-0203 d6, d23; ADR-0205 d8.
- **On main (audited at ca0290c2):** CI 41952568431 at `ca0290c2`: both heavy macOS legs fail `adi_project_files_tests` (9 of 44, every one media relocation), `adi_collect_export_tests` (8 of 97, the ZIP path), `adi_mp4_aac_tests` and `adi_ni_stems_tests` (one check each: "CoreAudio container/encoder priming mismatch"). Linux and Windows legs pass.
- **Proposed contract:** Relative media paths are computed between canonical folders: `saveCopy`, `saveAs` and `collectExport` canonicalise the destination folder (`weakly_canonical`) the way SQLite already reports the open database, so a rebased path resolves from the reopened copy. AAC: diagnose CoreAudio's priming on the probe; if this macOS cannot report priming reliably, `aacCodecAvailable()` says "unavailable" there (a capability refusal, as on any system without a verified codec), never a relaxed check.
- **Reuse:** No external code. `mac-probe.yml` (#458) runs the four suites on a Mac in minutes.
- **Reject or defer:** Skipping or loosening the failing checks; striking N3 or N30 before macOS passes.
- **Research IDs:** DR-12-05, DR-13-11 (requirement rows only; the evidence is ADI's own CI).
- **Unresolved test (NOT RUN):** `mac-probe` on the four suites with the canonicalised destination; a new check on every platform: Save As into a folder reached through a symlink, then play the relocated audio.
- **Cross-check and this plan:** Closed: #461 (head f7c8a94e) merged at `5a6ae08b` on 5 November, 10:37 UTC; the `adi_daw` run 42012235250 at that SHA completed with all 20 jobs green, both heavy macOS legs and the Step 7 UI run (42012235165) included. The symlinked-folder check stays a test to add. WTS-04 (ui.yml's cancel-in-progress) remains.

#### PD-01 Save As and Save a Copy

*decided* · size S · evidence documented · lane window, engine · milestone M3 · after: PD-00, PD-03, PD-04

- **Need:** Ctrl+Shift+S is a true Save As (a new `.adi`, and work carries on in it); Save a Copy writes a copy and stays (both P1); what a plug-in holds only in its own window is in the copy; a chosen existing file is handled as Live handles it.
- **Decision now:** ADR-0203 d6; FEATURES.md:255; SPEC §3.7; ADR-0142 d9; ADR-0147 d1; needs N3 and N30.
- **On main (audited at ca0290c2):** Engine built (#380, `project_files.cpp:129-167`), green on Linux and Windows, red on macOS (PD-00). Window: `AppCommands::SaveAs` says it waits for the engine (`ui_main.cpp:274-278`); no Save a Copy item; nothing snapshots device state before a copy.
- **Proposed contract:** The built semantics stand: SQLite backup into a private stage, media paths rebased on every branch and inverse, the WAL folded, no-clobber publication, and `saveAs` switching the same Store. Added: 1. every attached device is snapshotted before the copy (PD-03); 2. once PD-05 exists, the copy's `session_lock` and clean-close mark are cleared in the stage, beside `rebaseHistory`; 3. Live asks before replacing an existing file and then replaces it: the save dialog asks, and on yes the engine replaces the file atomically from the stage (PD-04's `replaceFile`), never the file that is open. SPEC §3.7's "existing destinations are never overwritten by these APIs" becomes "never without the user's confirmation" (win's ruling under ADR-0203 d1). Window: worklist A1 and A4 (retitle, refresh the view and the session, warnings to the Status Bar).
- **Reuse:** In-house `saveCopy`/`saveAs` (#380); R1 for the confirmed replacement.
- **Reject or defer:** Audacity's SafetyFileName/MoveProject (ADI never replaces the open `.adi`); REAPER's copy-media-on-Save-As options (Live keeps Collect All and Save as its own command).
- **Research IDs:** A6H-068 (Save As outside a Project leaves media referenced until collected), A6H-069, DR-12-05; AWAITING row 13 (which file you are left in after each).
- **Unresolved test (NOT RUN):** AWAITING 13 on Live; worklist A1's checks (the next edit lands in the new file, the old file is unchanged, undo still reaches an edit made before Save As); a device whose `stateEpoch` never moves keeps its window-only edit in the copy.
- **Cross-check and this plan:** XC-02: owns Save As and Save a Copy.

#### PD-02 Collect All and Save; Collect and Export ZIP

*decided* · size M · evidence documented · lane engine, window · milestone M3 · after: PD-00, PD-04

- **Need:** Live's Collect All and Save (external media copied into the project folder) as the plain command and ADI's ZIP export beside it; every file verified by hash and on disk before the project refers to it; the ZIP built off the message thread.
- **Decision now:** ADR-0203 d6; ADR-0127 d3-d4; ADR-0143 d2-d3 ("never committed references to unverified or unpublished data"); SPEC §3.8; FEATURES.md:247, 255; need N30.
- **On main (audited at ca0290c2):** Built (#411): collects the pool and every branch's captured media, hash-verified, never overwrites, rolls back on failure (`project_files.cpp:171-239`); the ZIP goes through `saveCopy` and `collectAllAndSave` on a private copy (`collect_export.cpp:130-167`). `fs::copy_file`, then `publishFile`, then COMMIT, with the copied bytes never flushed (`:217-235`); `ZipWriter` only calls `ofstream::flush` (`zip_writer.cpp:119`). Collect All and Save's own checks pass on macOS; the ZIP's do not (PD-00). No window items.
- **Proposed contract:** The built semantics stand. Added: 1. each staged copy and the ZIP are flushed and their folder synced before publication (PD-04), so a power cut after COMMIT cannot leave a committed reference to unwritten bytes; 2. Collect and Export runs on a worker on a read-only connection, with progress and cancel (worklist A4); 3. Live's Collect All and Save dialog chooses by location (Live 12 manual 5.7: files from elsewhere, the User Library, Packs) while the store collects everything, so the window builds Live's choices on a location filter the store gains (a needs-table row when the window gets there); 4. Live's Manage Project, Unused Files (manual 5.8) is the counterpart for crash debris and old renders, owned with freeze-cache's collector.
- **Reuse:** R1 (`sync_file` and the folder sync, inside `publishFile`); freeze-cache's collector for Unused Files.
- **Reject or defer:** Collect and Embed and `.adibundle` (superseded, ADR-0127); any file copy of the live database (the backup API only).
- **Research IDs:** A5C-049 (recovery must cover transactions and media together), A6H-068, DR-12-05, DR-12-07.
- **Unresolved test (NOT RUN):** The PublishHooks event order `create, sync_file, rename, sync_dir` for every collected file; a failed export over an existing ZIP leaves it byte-identical; the Live reading of the Collect All and Save dialog (manual 5.7, pp.145-147).

#### PD-03 Ctrl+S Take Snapshot, the daily snapshot, plug-in state written

*decided* · size S · evidence documented · lane engine, window · milestone M3 · after: none

- **Need:** "A project that is always saved" (Dream §12): Ctrl+S names a point in the history, one snapshot a day is taken by itself, and no plug-in edit lives only inside the plug-in when the project closes.
- **Decision now:** ADR-0203 d6, d12, d13; ADR-0128; ADR-0140; ADR-0142 d4, d9 ("`ParamOps::snapshot` is also the save"); ADR-0038 (open: how many plug-ins stay silent).
- **On main (audited at ca0290c2):** Take Snapshot is wired (`ui_main.cpp:258`); `DailyHistorySnapshots::poll` exists (#410) and nothing in `src/juce` or `src/adi/ui` calls it; the document only drains parameter edits (`project_document.cpp:83`) and never calls `ParamOps::snapshot`.
- **Proposed contract:** `ParamOps::snapshotAll(now, out)`: one `device.loadState` for each attached device whose chunk changed, nothing for an unchanged one (ADR-0142 d4); called before Take Snapshot, before close, and before Save As, Save a Copy, Collect and any backup. The window builds `DailyHistorySnapshots` on New, Open and Save As and polls it after completed gestures, undo and redo, and before close (the engine's handoff in `collab/engine.md`). *[XC-02: wired only together with Q-A's rule (HI-02)]* The silent plug-ins on the director's machine are counted for ADR-0038.
- **Reuse:** In-house only (#410, `ParamOps`).
- **Reject or defer:** An autosave timer (D-02 withdrawn); a periodic quiet snapshot of silent plug-ins until the count shows a need (it would touch his no-timer directive).
- **Research IDs:** A4-010, A5C-048, R3-001, R10H-061, DR-12-02, DR-12-03.
- **Unresolved test (NOT RUN):** A fake device whose `stateEpoch` stays 0 while its chunk changes gets exactly one `device.loadState` at Take Snapshot and at close; an unchanged chunk writes nothing; the silent-plug-in count.
- **Cross-check and this plan:** XC-02: the daily snapshot leaves this milestone and lands with HI-02 once Q-A is answered.

#### PD-04 Atomic, durable writes for every file beside the project

*decided* · size M · evidence documented · lane engine · milestone M3 · after: none

- **Need:** Every file ADI writes outside SQLite (renders, freezes, recordings, collected media, ZIPs, settings, bundles, the decode cache, library markers) is on disk before anything refers to it, never destroys a file the user owns when a write fails, and leaves debris a later launch can recognise.
- **Decision now:** ADR-0143 d3; ADR-0147 d1; ADR-0152 d2 ("atomic saves"); ADR-0127 d5; SPEC §3.5.
- **On main (audited at ca0290c2):** `publishFile`'s rename path flushes nothing and syncs no folder (`publish_file.cpp:97-101`); its callers close streams without `fsync` (`WavWriter::close`, (its source file), (its source file), (its source file), `project_files.cpp:217`, `ZipWriter`, the recording commit's copy); settings use a fixed `.tmp` and a bare rename; the bundle export deletes the user's file on failure. Ten stage kinds (`.adi-save-`, `-render-`, `-freeze-`, `-record-`, `-stem-import-`, `-media-`, `-edit-`, `-ni-`, `.publication-`, the decode cache's partials) are named by tick and serial, so a crash leaves stages nobody can attribute. Open PR #449 adds a second primitive, `replaceCacheFile` (no flush, no retry), to the same file.
- **Proposed contract:** One primitive pair in `src/adi/media/publish_file`: `publishFile` (no-clobber) and `replaceFile` (rename-over). Both flush the staged file before the rename (`FlushFileBuffers`, `fsync`, `F_FULLFSYNC` on Apple) and sync the parent folder after it on POSIX. Windows renames with `MoveFileExW` without `COPY_ALLOWED` and retries sharing, lock and access violations (5, 32, 33, and the network-share codes SQLite's own Windows layer retries) with a capped backoff. Stages are named `.adi-<kind>-<session>-<n>` and listed in the session marker's file (PD-05); the next launch sweeps a dead session's stages after taking its lock, never "when the folder is opened" (user-chosen folders are never reopened). The bundle export stages, then publishes or replaces; it never truncates or deletes the user's file. `replaceCacheFile` (#449) becomes `replaceFile`, with the flush optional for cache data.
- **Reuse:** R1 (craft-libs (its source file): `AtomicFs`, `sync_file`, `RenameRetry`, `replace_with_options`, `write_new`; FilmCraft's 200-byte stem cap) with its test list; the sweep rule of wordcraft's `docs/recovery.md` (design).
- **Reject or defer:** JUCE `TemporaryFile`/`ReplaceFileW` (not atomic; `src/adi` is JUCE-free); Tracktion's temp edit copy; VCV Rack's and Ardour's `.tmp`-then-rename without a flush.
- **Research IDs:** A11R-006, R11X-011, R11X-040, A5C-049.
- **Unresolved test (NOT RUN):** The craft-atomic list on all seven ABIs (a failed flush or rename keeps the original; a locked target is retried; the event order create, sync_file, rename, sync_dir; `write_new` never overwrites); a failed bundle export over an existing bundle leaves it byte-identical; a dead session's stages swept and a live one's kept; grep finds no bare `std::filesystem::rename` in `src/adi` outside `publish_file.cpp`.
- **Cross-check and this plan:** Lands after #449, whose replaceCacheFile it folds into replaceFile (one engine sequence on that file).

#### PD-05 Crash detection, the recovery notice and reopening

*decided* · size M · evidence documented · lane engine, window · milestone M5 · after: PD-04

- **Need:** After a crash or a power cut ADI knows the last session died, says what was recovered and offers to reopen the project, as Live offers recovery; a second writer is refused with the name of who holds the file.
- **Decision now:** SPEC §3.3 (the reader SHOULD tell the user, naming how much was recovered), §3.6 (`session_lock` with host, PID and heartbeat; stale after 60 s, breakable after a prompt); ADR-0047 d3; ADR-0003; ADR-0203 d1; Dream §12 ("a crash loses one gesture").
- **On main (audited at ca0290c2):** Nothing writes `session_lock`; `Store::open` reads neither a `-wal` nor a lock; a launch opens a new Untitled (Live's start behaviour, with nothing offered after a crash); `adi_meta.modified_by` is written only at create, and `setClientLabel` is called only by a test.
- **Proposed contract:** 1. An OS-locked session marker per running instance: a folder in ADI's data folder holding a held lock file and `session.json` (version, PID, start time, the open project's path and UUID, the stages it reserved, recordings in flight, clean exit). 2. In the `.adi`, a writable open writes the `session_lock` row and refreshes its heartbeat every 20 to 30 s; a clean close deletes it before the checkpoint. 3. `Store::open` returns `OpenReport{uncleanShutdown, opsSinceCleanClose, walBytesAtOpen, lastOpUtc, generator}`; a live lock opens read-only, naming host and PID; a stale one can be broken after a prompt. 4. Every SQLite-backup copy clears `session_lock` and the clean-close mark. 5. Every writer updates `modified_by` and `modified_utc`. Window: one launch question ("ADI quit unexpectedly while 'X' was open. Reopen it?", Reopen and Not Now, its wording taken from the Live reading); File, Recover keeps a declined candidate; holding Alt at launch skips the plug-in scan (Live 12 manual 23.4).
- **Reuse:** R2 (FilmCraft (its source file): `JournalMeta`, `lock_is_stale`, `scan_candidates`; the marker only, since the `.adi` is the journal); Audacity's ActiveProjects list as the simplest form (design).
- **Reject or defer:** FilmCraft's snapshot journal (every op is already in the file); Live's version-specific Undo/BaseFiles replay (nothing to replay in ADI).
- **Research IDs:** A5C-048 (Live's recovery inputs; declining moves them to a Crash folder), A4-010, A5H-034 (Alt at launch skips the scan), C5W-029, DR-12-01.
- **Unresolved test (NOT RUN):** Crash fixture: commit three ops, copy the `.adi` and its `-wal` without closing, reopen the copy: unclean, three ops. A clean close leaves one file and no lock row; a second writer is refused naming host and PID; a heartbeat older than 60 s can be broken. The Live 12 crash-dialog reading on a disposable Set (text, buttons, what returns, where the Crash folder is), M5-A5C-048, through a new AWAITING row.
- **Cross-check and this plan:** XC-24: the control API's discovery record lives in this marker with one sweep; ADI_HOME redirects it for tests.

#### PD-08 Back up project to cloud, and a project kept in a sync folder

*decided* · size S · evidence documented · lane window, engine · milestone P2 · after: PD-02

- **Need:** A manual File command writes a consistent copy and its media into a folder the user chooses (a Drive, OneDrive, Dropbox or iCloud folder); a project that itself lives in a sync folder is flagged, because a drive is never a project's home.
- **Decision now:** ADR-0182 d3; ADR-0203 d13 (A6-08: manual, no scheduler, no credentials, asks for the folder each time, P2), d22; FEATURES.md:301.
- **On main (audited at ca0290c2):** Collect and Export exists (PD-02); no command; nothing detects a sync folder (the default projects folder is under the app's data folder).
- **Proposed contract:** File, Back Up Project... asks for the folder, runs Collect and Export on a worker with progress and cancel, and writes `<name> [YYYY-MM-DD HHMMSS].zip` without replacing anything; never a copy of the live database. Opening a project inside a known sync folder puts one line in the Status Bar saying so.
- **Reuse:** Collect and Export (#411); R1.
- **Reject or defer:** A scheduler or stored credentials (A6-08 says none); S3 sync, which is collaboration (the CO rows).
- **Research IDs:** DR-12-16, A5H-014 (a backup is not the history).
- **Unresolved test (NOT RUN):** Sync-folder detection for OneDrive's known folders on Windows and iCloud Drive's path on macOS.

#### PD-14 Templates and the default Set

*decided* · size S · evidence documented · lane engine, window · milestone M5 · after: PD-01

- **Need:** Live's templates: save a project as a template into the User Library, start from one, open on start with the template; the CTZ hierarchy ships as a bundled template beside Live's default Set.
- **Decision now:** FEATURES.md:276 (Templates, P1, "a .adi with a flag"); ADR-0203 d9 (templates where Live keeps them, the User Library), d11 (open on start: the template, as Live); ADR-0209 d10; BUILD-BRIEF Phase D item 18.
- **On main (audited at ca0290c2):** `files.openOnStart` and `files.startTemplate` exist in the registry; no template code (only a comment at `store.cpp:289`).
- **Proposed contract:** A template is an `.adi` written by `saveCopy` into the User Library's Templates folder with a flag row in `adi_meta` (no schema change). New from a template copies it to a new file whose history starts at the template's state as its root, not as an undo step (as New's root does, #307). Save as Default Set sets `files.startTemplate`.
- **Reuse:** `saveCopy` (#380); New's root (#307).
- **Reject or defer:** REAPER's track templates and project templates as a separate system (Live parity only).
- **Research IDs:** No bundle row was read for this need; the Live 12 manual's chapter 5 is the source.
- **Unresolved test (NOT RUN):** The Live reading: what Save Live Set As Template keeps (media references; Live's undo is not kept) and where it goes; New from a template has no undo step and its relative media paths resolve.
- **Cross-check and this plan:** XC-13: owns the mechanism; the CTZ template's content is MX-31's and needs MX-29's role table.

#### EX-01 One consistent revision per job, and media identity

*decided* · size S · evidence documented · lane engine · milestone M3 · after: none

- **Need:** A queued export renders exactly one consistent project state and names its files from that state while the user keeps editing, and never publishes a half-written file.
- **Decision now:** ADR-0071 d1-d5; ADR-0205 d5 (the queue and stem jobs P1); ERRATA-11.
- **On main (audited at ca0290c2):** Built as §2 describes, with the latency and topology invariant (A11R-004: any latency report or graph change fails the job, `queue.cpp:243-254`). The `render_queue` tests pass on Linux and Windows. A job keeps the project path it was given at enqueue, so a Save As in the middle of a queue renders the old file; the queue lives in memory only.
- **Proposed contract:** Keep the four ownership moments. Added: the run manifest (EX-07) records the journal head each job read; every referenced media file's BLAKE3 is checked when the job starts and again before publication, and a file rewritten in between fails the job (the revision covers rows, not media bytes). REAPER's enqueue snapshot is not adopted (ADR-0071 d5); an older state is rendered from a snapshot opened read-only (ADR-0111).
- **Reuse:** EX3 (Tracktion's `RenderQueue` states and tests: cancelling a pending job skips it; a job deleted mid-render leaves no partial file; jobs appended while running wait for the next run). The agent's job manager (A4) only wraps `render::Queue`.
- **Reject or defer:** An enqueue-time project snapshot (R10H-079) as ADI policy; MAGDA's `StableUntilCompletion` revision policy as a second job system.
- **Research IDs:** A11R-001, A11R-004, A11R-005, A11R-007, R10H-079, R11X-013, R11X-028, C4-008, DR-13-05, DR-13-06.
- **Unresolved test (NOT RUN):** A11R-001's case: queue two exports, then edit the project, a name and a region before the first starts and between jobs; record the revision and the name each file used. New: a referenced WAV rewritten after the job starts fails the job.
- **Cross-check and this plan:** XC-20: one job state machine wraps render::Queue.

#### EX-02 Sources, routes and taps

*decided* · size L · evidence documented · lane engine · milestone M5 · after: EX-01

- **Need:** Live's Rendered Track chooser (Main, All Individual Tracks, Selected Tracks Only, one track) and Include Return and Main Effects; Cubase's Dry, Inside and Outside, and REAPER's pre-fader stems and master-effects-only, as additions.
- **Decision now:** ADR-0071 d1 (routing treatment: through the master or not, wet or dry); ADR-0203 d1, d3 (taps Pre FX, Post FX, Post Mixer); ADR-0205 d9, d10; FEATURES.md:249-250.
- **On main (audited at ca0290c2):** `Source{trackId, PreFader|PostFader}`; the runner processes the whole graph, unexported sidechains and returns included, and copies one endpoint (A11R-002, `queue.cpp:266-267`); no dry, no via-Main, no batch expansion; `offlineTapOutput` (PreFx, PostFx, PostFader, PostMixer) is used by freeze only.
- **Proposed contract:** `Source{trackId, tap: PreFx (dry) | PostFx | PostFader | PostMixer, route: Direct | ViaMain | MainFxOnly (P2)}`. ViaMain is an offline audibility override in the private session: every track outside the source, its descendants, its ancestors up to Main and the returns it reaches is silenced at its outputs and sends while its chain still processes (keys and sidechains stay live), and Main renders post-fader. It is never built on solo, because a send follows its source's mute and never its solo (`mixer.hpp:327`). Batch expansion ((its source file)) turns the chooser into jobs over one range, with a cardinality preview and unique names. The dependency closure is a preview of what a stem depends on, not a filter. Impossible combinations are refused at `add` (a pre-fader stem via Main, R11X-044).
- **Reuse:** EX3 (Tracktion's `findStemSourceTracks` fixpoint as the preview; its per-track naming with case folding added); `offlineTapOutput` (in tree).
- **Reject or defer:** Tracktion's `useMasterPlugins` read as Live's mode (it is REAPER's master-effects-only); stems by soloing each track (SoundCraft's `bounce_stems`); REAPER's parent-channel projection and automatic mono (R11X-003, R11X-004) to P2.
- **Research IDs:** A4-001, A4-002, A4-008, R11X-001, R11X-002, R11X-003, R11X-005, R11X-044, A11R-002, C4-003, C4-005, C4-006, DR-13-01, DR-13-02.
- **Unresolved test (NOT RUN):** M2-HOST-EXPORT's probe: distinct tones on the selected media, an unselected receive, a detector-only sidechain and a live return, compared across Direct, ViaMain, MainFxOnly and full Main; with linear processing only, ViaMain stems sum to Main within float tolerance. Live readings: parent groups, sidechains from unselected tracks and returns fed by other tracks under Include Return and Main Effects; which tracks All Individual Tracks includes, and their names.
- **Cross-check and this plan:** XC-14: built on the one capture; XC-15: taps from the one vocabulary.

#### EX-03 Render as Loop, tails and gapless region batches

*decided* · size M · evidence documented · lane engine · milestone M5 · after: EX-01, EX-02

- **Need:** Live's Render as Loop; a tail for effects that ring past the range (REAPER's and Cubase's second pass); sample-exact gapless region files.
- **Decision now:** ADR-0070 d1-d4; ADR-0071 d1; ADR-0205 d9 ("2nd pass render for effect tails"); FEATURES.md:250.
- **On main (audited at ca0290c2):** Each job prerolls from transport zero with looping off and writes exactly the range (`queue.cpp:239-288`); no tail field; independent single-region jobs only.
- **Proposed contract:** Render as Loop is Live's two passes: loop the range in the offline transport, one unwritten pass, then the written one, latency-compensated. `Job.tail` is Off, Fixed (ms) or UntilSilent{max ms, threshold dB, hold ms}, with the bounds kinds it applies to stored apart from its length (R11X-046), and refused inside a gapless batch (ADR-0070 d4). Warm-up from zero stays and is not a tail. ADR-0070's gapless batch (one pass, sliced, concatenation bit-identical) is the cycle-marker batch at P2 (EX-12).
- **Reuse:** EX3 (Tracktion's end allowance that stops on silence, with a hold window added for gapped echoes).
- **Reject or defer:** Tracktion's `wrapRemainder` fold as Render as Loop (exact only for linear processing); REAPER's per-region take-FX reset (R11X-036) stays a comparison.
- **Research IDs:** A4-002, A11R-003, R11X-006, R11X-007, R11X-036, R11X-046, C4-001, C4-005, DR-13-09.
- **Unresolved test (NOT RUN):** An impulse before the range and a decaying effect beyond it, independent region jobs against a continuous pass; the Live reading of where Render as Loop's first pass starts.

#### EX-04 Deliveries, formats, Live's rate rule and mono

*decided* · size L · evidence documented · lane engine · milestone M5 · after: EX-01 and the dependency rows (§10)

- **Need:** One render gives every file Live's dialog asks for (PCM as WAV, AIFF or FLAC at 16, 24 or 32-bit float, plus MP3 CBR 320) with no double dither; Float64 WAV; Live's lower-rate rule; Convert to Mono; Ogg, Opus and single-file AAC as additions.
- **Decision now:** ADR-0132 d5 (export PCM 16/24, float 32/64); ADR-0203 d18 (every requested file type P1); ADR-0205 d9; ADR-0207 d2; FEATURES.md:250.
- **On main (audited at ca0290c2):** WAV only; `validate` refuses other encodings (`queue.cpp:34-37`); no Float64 in the writer or the reader (the normalization check reads back through the reader); a job processes at its own rate, so plug-ins run at a lower export rate; the speaker mask is unspecified.
- **Proposed contract:** `Job.deliveries`: a list of {container, sample format, dither, codec settings, name suffix}, defaulting to today's single WAV. The source is captured once in double at the graph rate; each delivery is then quantized or encoded, decoded and checked, and published. An encoder capability matrix refuses impossible combinations at `add` (FLAC with float; MP3 outside MPEG-1 rates is resampled to 44.1 or 48 kHz until Live is read). FLAC receives ADI's quantized samples, so it equals the PCM WAV bit for bit; LAME receives the undithered double capture. Below the project rate the job processes at the project rate and then converts with the SoX resampler, as Live's Audio Fact Sheet says; otherwise it processes at the output rate. Float64 in the writer and the reader; an AIFF writer including AIFF-C `fl32`; a WAVE_FORMAT_EXTENSIBLE channel mask; Convert to Mono with Live's law once read. Create Analysis File writes into ADI's analysed folder, never beside the file (ADR-0203 d18).
- **Reuse:** EX1 (libFLAC; LAME 3.100 pinned by the tarball's hash; later libvorbis with libogg, and libopusenc with libopus; adapters ported from libsndfile's `flac.c` and `mpeg_l3_encode.c`, avoiding its encoder/decoder status mix-up); EX7 (soxr); EX9 (SoundCraft's `aiff.rs` `fl32`, libsndfile's `uint2tenbytefloat`); opusfile for tests only.
- **Reject or defer:** JUCE's format writers and `LAMEEncoderAudioFormat` ((its source file) is JUCE-free, ADR-0036); an ffmpeg command-line pipe; libsndfile as the writer; FilmCraft's clean-room Rust encoders.
- **Research IDs:** A4-003, A4-004, A6G-010, R11X-019, R11X-020, R11X-041, A11R-009, C4-002, C4-012, C4-013, DR-13-07.
- **Unresolved test (NOT RUN):** FLAC 16/24 decoded by `dr_flac` equals the PCM WAV of the same capture and seed; every MP3 frame is MPEG-1 Layer III at 320 CBR with a LAME Info tag, and `dr_mp3`'s delay and padding trim it to exactly `framesWritten`; the refusal matrix. Live readings: the FLAC depths offered; the MP3 settings (stereo mode, Info tag, ID3); Convert to Mono's law; the SRC quality (M6-A6G-010).

#### EX-05 Normalization, statistics, the brickwall and the stage order

*decided* · size M · evidence documented · lane engine, devices · milestone M5 · after: EX-04 and the loudness area's meter

- **Need:** Live's Normalize and ADI's LUFS targeting, which exist, plus REAPER's and Cubase's depth: LRA and the maximum momentary and short-term loudness, RMS-I, analysis without normalizing, one gain for several files ("together"), normalize only if too loud or too quiet, and a true-peak brickwall; one documented order of stages.
- **Decision now:** ADR-0203 d14 (LUFS targeting; no normalising by default; -14 LUFS and -1 dBTP editable); ADR-0205 d8, d9; FEATURES.md:86, 250, 253.
- **On main (audited at ca0290c2):** Peak and LUFS modes with the gain held back to the true-peak ceiling, the encoded file decoded and re-measured, up to four passes (`normalization.cpp`); the report holds source and output LUFS and peaks, the gain, `ceilingLimited` and the pass count. No brickwall (the doc says it "does not claim a limiter"), no LRA, no "together", no analysis-only mode.
- **Proposed contract:** Stages, each adjacent pair tested: capture (graph rate, double); loop and tail windows; SRC; channel conversion; analysis; normalization gain; the optional true-peak brickwall; then for each delivery dither and quantize (integer PCM only), encode, decode and check, metadata, publish. Trim, fades and padding are either added as explicit stages (fades before padding, R11X-035) or stated as absent. Statistics come from the one loudness meter the loudness area chooses, with libebur128 as a test-only oracle. "Together" for parallel stems measures their sum (the Main capture); the histogram merge serves regions and albums, as if one file. An analysis-only run never implies processing; the brickwall is off by default and never acts in Peak mode; the report says which signal it describes (graph PCM, processed PCM or the decoded file).
- **Reuse:** LD1 or LD2 (the meter, loudness area), LD3 (libebur128, tests only), LD4 (the Tech 3341/3342 signals synthesised), LD5 with EX4 (one true-peak change in `src/adi/dsp/limiter`: Fons Adriaensen's upsample-and-hold and lsp-plugins' true-peak design taken into ADI's own `LookaheadLimiter`, made double); Ardour's `export_graph_builder` order (design).
- **Reject or defer:** EX2 (libebur128's LRA, whose 1 Hz hop is below Tech 3342's 10 Hz), superseded; ADI-L's own true-peak code as the brickwall's source, until the director's plug-in list says otherwise (§8); the EBU WAV test set (not usable).
- **Research IDs:** R10H-073, R11X-021, R11X-025, R11X-027, R11X-034, R11X-035, R11X-043, R11X-047, A4-003, DR-13-07, DR-13-08.
- **Unresolved test (NOT RUN):** Intersample-crest fixtures (fs/4 at 45 degrees): the encoded true peak at or under the ceiling after brickwall and dither; exact length and alignment; bit-exact below the ceiling; the synthesised 3341/3342 suite. The Live reading of Normalize's exact target and whether it is sample or true peak.
- **Cross-check and this plan:** XC-04: owns DEV-29's items (a) to (d); one limiter change with the devices lane.

#### EX-08 Export metadata

*decided* · size S · evidence documented · lane engine · milestone M5 · after: EX-05

- **Need:** Cubase's BWF, iXML, marker-chunk, RF64 and ID3 switches and REAPER's metadata, with loudness fields from the checked result and markers placed by the same time map as the audio.
- **Decision now:** ADR-0132 d2; ADR-0205 d9; FEATURES.md:250.
- **On main (audited at ca0290c2):** `WavWriter` writes a version-0 `bext` and raw iXML when it is given metadata; export gives it none (`queue.cpp:215-217`).
- **Proposed contract:** A typed `Job.metadata` (provenance, time reference, user tags, markers on or off, iXML on or off, a tempo chunk opt-in). `bext` v2: loudness value, range, maximum true peak, momentary and short-term from the checked output (0.01 LU); an EBU R99 originator reference whose random part comes from the job's seed, with an injected clock; origination date and time; coding history. `cue ` with (its source file) markers inside the range, at the time map shared with any trim or pad. Vorbis comments, ID3v2 and MP4 tags. Header fields patched after the check, in space reserved as the data size already is.
- **Reuse:** EX6 (libsndfile's `bext` v2 layout, `cue ` writer and coding history; Ardour's R99 reference and time-reference split); EX8 (TagLib as the tag oracle in tests); MD8 (`dr_wav` metadata reading for round trips).
- **Reject or defer:** An ACID tempo chunk by default (it invites other hosts to stretch the file; opt-in only); libsndfile's `adtl` handling (it only parses; ADI writes its own).
- **Research IDs:** R11X-023, R11X-024, R11X-042, C4-014, DR-13-07.
- **Unresolved test (NOT RUN):** Every `bext` v2 field parses strictly; the loudness fields equal the normalization report within 0.01 LU; markers at the first, an interior and the last sample with padding; non-ASCII text, and the RF64, `bext`, iXML order.

#### EX-11 The Export page, and where renders run

*decided* · size L · evidence documented · lane window, engine, win · milestone M5 · after: EX-01 to EX-04 (stubs allowed)

- **Need:** Live's Export Audio/Video dialog first, ADR-0205 d9's additions below a divider, the queue visible and editable, rendering in the background with progress and cancel.
- **Decision now:** ADR-0205 d5, d9; ADR-0203 d2 (additions shown where a user looks), d11 (the Settings Export page); ADR-0071 d2, d3; ADR-0210 d3 (the window builds ahead on stubs).
- **On main (audited at ca0290c2):** No window code calls `render::Queue`; LANES §3b has no Export item. `Control::serviceHost` runs between blocks on the runner's own thread and does not make it the message thread (A11R-005).
- **Proposed contract:** 1. Live's controls: the Rendered Track chooser; Render Start and Length from the Arrangement selection; Include Return and Main Effects; Render as Loop; Convert to Mono; Normalize; Create Analysis File; Sample Rate; Encode PCM (type, depth, dither); Encode MP3; Create Video (disabled until the project has video). Export is disabled when neither encoding is on. 2. Below a divider: the LUFS target, tails, collisions, metadata, the queue list, the loudness readout (FEATURES.md:86). 3. Where renders run, decided before the page ships: the private session's plug-in instances are created and given their state on the message thread, processing and encoding run on a render worker, and `serviceHost` is marshalled to the message thread between blocks; ~~or a render child process like the scanner's (#438-#440)~~ *[Struck (XC-26): ADR-0203 d8 keeps every plug-in in process; renders run in process]*. 4. Export through external hardware switches to real time with Live's silence wait (A3-031), later, with External Audio Effect. 5. win adds the item to LANES §3b and a parity checklist from manual 5.1.3, pp.122-127.
- **Reuse:** FilmCraft's `export.queue.*` command shape for the panel's actions (design).
- **Reject or defer:** Cubase's and REAPER's two dialogs (one page); the AI pressing render (never, ADR-0071 d3).
- **Research IDs:** A4-001, A4-002, A4-003, A4-004, A3-031, A6H-073, A11R-005, DR-13-01, DR-13-06.
- **Unresolved test (NOT RUN):** A VST3 and a CLAP rendered from the page while the window stays responsive, logging the thread of create, setState, process and `serviceHost`; the parity checklist; AWAITING 16's export-dialog defaults.
- **Cross-check and this plan:** XC-26: the child-process option is struck unless he amends ADR-0203 d8.

#### EX-12 P2 export abilities

*decided* · size XL · evidence documented · lane engine, window · milestone P2 · after: EX-01 to EX-07 and EX-11

- **Need:** The render matrix grid, the cycle-marker gapless batch, After Export actions, the batch converter, real-time render, live output capture and the `.adiviz` telemetry pass.
- **Decision now:** ADR-0203 d14 (D-07, the render matrix, P2); ADR-0205 d1-d4, d9; ADR-0070; ADR-0206 d4 (Return to FilmCraft as an After Export action).
- **On main (audited at ca0290c2):** None of it.
- **Proposed contract:** Matrix cells add ordinary jobs. After Export is a separate undoable import after the files exist (`clip_edit::prepareBounce` and `commitMaterial` reused). The batch converter feeds file sources through the same post pipeline with isolated worker state. Live capture is a separate session-owned writer. The telemetry shares the export pass (ADR-0205 d2).
- **Reuse:** EX3 (the batch model); `material_commands` and (its source file) (in tree).
- **Reject or defer:** REAPER's keep-partial (P3); a dry-run result that edits the project other than through ordinary ops (R11X-026).
- **Research IDs:** R11X-022, R11X-026, R11X-029, R11X-030, R11X-031, R11X-032, R11X-033, C4-010, C4-011, DR-13-03, DR-13-10, DR-13-15, DR-13-16.
- **Unresolved test (NOT RUN):** ADR-0070 d2's concatenation bit-identity; After Export undone in one step.

#### IC-01 The converter framework and DAWproject both ways

*decided* · size L · evidence documented · lane engine, window, devices, win · milestone M5 · after: time-media's integer time base (IC8)

- **Need:** Open and write DAWproject (P1) with a loss report per field, and a re-export that loses nothing ADI parked.
- **Decision now:** ADR-0127 d7; FEATURES.md:551; README commitment 7; RATIONALE §4 item 3 (unmapped data in extension rows); SPEC §7.1, §7.4; ADR-0011; ADR-0021.
- **On main (audited at ca0290c2):** No XML parser and no converter. `extension.write`/`delete` (OPS §9.11, P1) and `noteExpression.write` (OPS §9.5) are catalogued and unbuilt; `rows::Model` has no `extensions` or `note_expression` rows; `capture.createMidi` refuses empty clips, notes past the clip and controllers outside CC 0-127, and takes no offset, loop or colour.
- **Proposed contract:** Converters live JUCE-free in `src/adi/interchange`. Every import is a prepare (on a worker, ids from `CopyIds`) and a commit (one undo step, `Actor::Import`, `actor_detail` naming the format and the generator), as `stem_import` is. Exporters read `rows::Model`, which gains the two missing row kinds, and publish without clobbering. Each run returns a per-field loss report; unmapped data goes into extension rows. VST3 identity: import matches by hashing the DAWproject class UUID as JUCE does; export reads the class ID from the preset header at load time, so the scanner does not change. A neutral `clip.createMidi` name for the same handler, widened or followed by ops in the same commit.
- **Reuse:** IC1 (Tracktion's DAWproject codec ported off JUCE onto pugixml and miniz, its known defects fixed: `playStart` in seconds under beats, the dangling Tempo IDREF, markers without a time unit); IC2 (MAGDA's declared-loss table and in-process XSD validation); IC3 (ProjectConverter's nested-timeline flattener and `Vst3Preset`/CLAP state); IC4 (Bitwig's XSDs and DawProjectTest scenarios, and openDAW's five Bitwig-written files, fetched, never committed); IC5 (pugixml); IC6 (the VST3 `.vstpreset` container subset); IC12 (openDAW's importer heuristics, design).
- **Reject or defer:** Tracktion's JUCE glue; `.als` import (undecided, not proposed); DawVert as code.
- **Research IDs:** C4-017, C4-018, DR-13-17; mission M3-EXPORT-INTERCHANGE.
- **Unresolved test (NOT RUN):** DP-1 to DP-6: XSD validation in CI; Tracktion's scenarios plus ADI's own XML fuzzer; a fixed point after import, export and import under the text projection; unit mapping; the `.vstpreset` byte round trip; one undo step. DP-7: a Cubase 15 round trip with a VST3 insert, automation and notes, through a new AWAITING row.
- **Cross-check and this plan:** XC-11: merged into TM-IC-03.

#### IC-02 MIDI files, Live's way

*decided* · size M · evidence documented · lane engine, window · milestone P2 · after: IC-01

- **Need:** Import MIDI File... and Export MIDI Clip as Live has them, with no options box; `.mid` dropped on tracks and take lanes.
- **Decision now:** ADR-0203 d7 (A6-03: exactly Live's, no options box, P2; Cubase's options behind an Options button), d11 (a MIDI file still imports with no box); ADR-0004 (PPQ 5,765,760).
- **On main (audited at ca0290c2):** Nothing.
- **Proposed contract:** An SMF codec, JUCE-free; an import is one undo step; the source PPQ is remembered, so a re-export is tick-identical; meta, sysex and unknown chunks are parked in extension rows; Live's behaviour is read in AWAITING 13 (a multi-track file, whether the tempo is taken, the export's type).
- **Reuse:** IC7 (libremidi's reader and writer with the listed fixes; the DryWetMIDI corpus, 63 valid and 666 invalid files, fetched; JUCE's RMID and SMPTE handling as a reference).
- **Reject or defer:** The harvest's P1 placement (it contradicts ADR-0203 d7: P2 stands); Ardour's libsmf (glib); Craig Sapp's midifile (a second opinion only).
- **Research IDs:** A6H-028; AWAITING row 13.
- **Unresolved test (NOT RUN):** SMF-1 to SMF-4; AWAITING 13.
- **Cross-check and this plan:** XC-11: merged into TM-IC-02.

#### IC-03 OTIO and AAF (P2), OMF read (P3) and the ArtCraft bridge

*decided* · size XL · evidence documented · lane engine, window · milestone P2 · after: IC-01 and EX-04

- **Need:** The industry handoff both ways (the AAF Edit Protocol with embedded or separate WAV/AIFF and handles; OTIO); FilmCraft and SoundCraft reached through these converters, never through their own files.
- **Decision now:** ADR-0206 d3-d5; ADR-0127 d7; ADR-0207 d3; FEATURES.md:252, 266; INTEROP-ARTCRAFT §3-§6.
- **On main (audited at ca0290c2):** Nothing; `external_sessions`/`external_links` are reserved (Phase D item 20) but not in the schema.
- **Proposed contract:** The integer time base first (ADR-0206 d3, time-media's area). AAF import through LibAAF with Ardour's mapping (track gain, pan and mute are new work). AAF export as a clean-room CFB and AAF writer ported from FilmCraft, lowered from `rows::Model`, with essence from the export queue (separate WAV with handles, or embedded). OTIO as a subset over nlohmann with integer arithmetic. The drop-frame flag as a new table. `.fcproj` and `.scraft` never read or written.
- **Reuse:** IC8 (OTIO's DF/NDF vectors, reimplemented in integers; FilmCraft's `otio.rs` mapping as design), IC9 (LibAAF, static, with its 65-file corpus), IC10 (FilmCraft's AAF and CFB writer, about 2,360 portable lines), IC11 (FilmCraft's OMF read, P3).
- **Reject or defer:** OTIO's C++ library (doubles, against ADR-0206 d3); the AMWA AAF SDK; ptformat (Pro Tools sessions undecided); an FFI to FilmCraft (it drags its document model in).
- **Research IDs:** C4-016, C4-017, DR-13-17, DR-13-18; the SoundCraft/FilmCraft digest (FilmCraft reads and writes AAF on its own CFB, unvalidated against Avid; SoundCraft has no AAF or OMF).
- **Unresolved test (NOT RUN):** OTIO-1; AAF-1 to AAF-3 (pyaaf2 as an offline second reader; Pro Tools or Resolve as the director's human gate).
- **Cross-check and this plan:** XC-11: merged into TM-IC-04.

#### CO-03 Incoming changes previewed; collisions kept both ways

*decided* · size L · evidence documented · lane engine, window · milestone P2 · after: CO-01 and the comping area's take lanes

- **Need:** Collaborators' changes shown as outlines and ghost notes and applied only on demand, as one undo step; a collision kept both ways (a clip becomes a take; a device a second device; continuous data a ghost with Keep Mine, Adopt Theirs or Combine; a single value mine or theirs).
- **Decision now:** ADR-0182 directive part 3 ("incoming changes applied only on demand"), d5, d6, d9; ADR-0148; FEATURES.md:302.
- **On main (audited at ca0290c2):** The rolled-back preview (ADR-0148, #100) only.
- **Proposed contract:** Preview is CO-01's rebase run in ADR-0148's rolled-back transaction, returning the projection diff and the collisions (touch sets that meet, with neither batch based on the other); Apply is the same rebase committed, one transaction, one undo; the Apply button stays. Ghosts live in a `sync_ghosts` table the engine never reads; Combine samples both curves in the lane's own domain, then thins. "A clip becomes a take" waits for take lanes (comping area).
- **Reuse:** C1, C2; C9 (openDAW's overlap rules, design); the automation area's thinning.
- **Reject or defer:** Auto-applying incoming batches (the harvest's recommendation contradicts the directive; not proposed).
- **Research IDs:** DR-12-12, DR-12-13, DR-12-14.
- **Unresolved test (NOT RUN):** Two Stores edit one clip's gain offline and both flag the same collision class; two edits that touch nothing in common merge silently; the preview leaves the `.adi`'s bytes unchanged; Combine at 0, 50 and 100% equals Keep Mine, the dB-domain mean and Adopt Theirs.

#### CO-05 Shared media and the bucket client

*decided* · size L · evidence documented · lane engine, window · milestone P2 · after: CO-01

- **Need:** Media stored once by hash in the owner's bucket; generated audio uploaded automatically, outside samples listed with their size and excluded by default; credentials in the OS keychain; uploads that survive a crash.
- **Decision now:** ADR-0182 directive part 2, d2, d4; ADR-0032.
- **On main (audited at ca0290c2):** Nothing: no HTTP client, no SigV4, no keychain code (JUCE has no keychain API).
- **Proposed contract:** (its source file) objects; ADR-0182 d4's selection (all, none, per file; Consolidate to Cloud uploads everything); an S3 client with SigV4 and a conditional-write probe; a keychain adapter per platform; a resumable upload queue (block hashes, pending snapshots) that survives a crash.
- **Reuse:** C4 (object_store's SigV4 and its vectors; the conditional-put quirks); C7 (Audacity's cloud-sync tables and missing-blocks uploader, design).
- **Reject or defer:** Git of the text projection as the carrier (ADR-0182 d2).
- **Research IDs:** DR-12-11.
- **Unresolved test (NOT RUN):** The SigV4 vectors byte for byte; an upload survives a crash and resumes; conditional create in a MinIO job.
- **Cross-check and this plan:** XC-40: listed in the Privacy group of what leaves the machine.

#### PD-09 File too new, and the next major

*refinement* · size S · evidence documented · lane engine, window, win · milestone M5 · after: PD-05

- **Need:** A project written by a newer build is never damaged by an older one, and the user is told who wrote it and what to do; an older major is never upgraded in place.
- **Decision now:** SPEC §11; ADR-0144 (open item: 1.x to 2.0); ADR-0136 d2 (`media_blobs` dropped at 2.0); ADR-0203 d1.
- **On main (audited at ca0290c2):** A newer major opens read-only (`store.cpp:421-432`) and the window does not say so; `modified_by` names only the creator; `plugins.sqlite` and `library.sqlite` refuse newer files; the settings bundle never checks its manifest's schema.
- **Proposed contract:** A newer-major `.adi` opens read-only with a banner naming the writer and offering Save a Copy (`OpenReport`, PD-05). 1.x to 2.0 follows Live: a 1.x file opened by a 2.x build opens read-only and Save As writes the 2.x file; minors stay upgraded in place (ADR-0144). `plugins.sqlite` and `library.sqlite` adopt major.minor (a newer minor opens read-write, a newer major read-only). Every refusal names the version found, the version supported and the writer. This closes ADR-0144's open item by Live's rule; win records it.
- **Reuse:** R4 (`pcraft-format`'s `migrate.rs`: `TooNew{found, supported, generator}`).
- **Reject or defer:** Mixxx's release-string upgrade chain.
- **Research IDs:** A6H-069 (an older-major Set cannot be overwritten; Save As is asked for).
- **Unresolved test (NOT RUN):** The Live reading M6-A6H-069 on Live 12; a newer-major file shows the banner and Save a Copy works; a newer-minor `plugins.sqlite` opens.

#### PD-10 Schema minors stay whole tables

*refinement* · size S · evidence documented · lane win, engine · milestone M0 · after: none

- **Need:** Every format change lands as a whole new table in a minor, migrated in one transaction and classified for the digest and the text projection; anything that must change an existing object has its own mechanism.
- **Decision now:** ADR-0144 d3 ("a future minor that must change an existing object needs a different mechanism and its own ADR") and check 9; ADR-0007; SPEC §11.1; ADR-0206 d3; ADR-0188 d5; ADR-0209 Q8.
- **On main (audited at ca0290c2):** Schema 1.19; check 9 enforces additivity. Two open asks would break it: N38 asks for a nullable `tracks.role` column (win's half, open), and the interchange harvest proposed a drop-frame column beside (its source file).
- **Proposed contract:** Pending minors as whole tables: `external_sessions` and `external_links` (P1); the sync tables `sync_vectors`, `remark_anchors`, `order_keys`, `sync_spaces`, `sync_batches`, `sync_touches`, `sync_ghosts` (P2); a drop-frame table (P2); the track role as a side table (`track_roles(track_id, role)`) unless win writes the mechanism ADR-0144 d3 demands for a column. Each new table is classified in `digest.cpp` and in the text projection's coverage list.
- **Reuse:** None.
- **Reject or defer:** A column added in a minor without that mechanism.
- **Research IDs:** None beyond the governing texts.
- **Unresolved test (NOT RUN):** `validate_schema.py` check 9 and `adi_migrate_tests` from every frozen minor, for each new minor.
- **Cross-check and this plan:** XC-37: win's one register; 1.20 is the roles table alone.

#### PD-11 Settings file safety and migration

*refinement* · size M · evidence documented · lane engine, window · milestone M3 · after: PD-04

- **Need:** `settings.json` survives crashes, hand edits, older and newer builds, and a renamed key keeps the user's value.
- **Decision now:** ADR-0152 d2-d6; ADR-0125; ADR-0203 d11 (named profiles).
- **On main (audited at ca0290c2):** A fixed `.tmp` and a bare rename with no flush or `.bak` ((its source file)); a newer schema is saved back with its number kept (`:144`); `reset()` writes today's default into the file; no migration mechanism; the window saves on every change.
- **Proposed contract:** Saves go through `replaceFile` (PD-04) with a `.bak` of the last good file, a re-read before writing (a hand edit is adopted), a 500 ms debounce (`saveSoon`; `tick` from the window's timer; `flush` on quit) and backoff on failure; `reset()` erases the key; a step migration runner (empty today; the schema rises only for an incompatible change); the bundle import checks its manifest's schema. A newer schema is shown but never written over, after reading how Live 12 carries its Preferences across versions; this flips the last check of `testUnknownKeysSurvive`.
- **Reuse:** R4 (craft-libs (its source file): `store.rs`, `recovery.rs`, `migrate.rs`; (its source file)).
- **Reject or defer:** `toml_edit`'s comment preservation (ADI writes JSON); Mixxx's upgrade chain.
- **Research IDs:** C5W-029, C4-015, DR-12-18.
- **Unresolved test (NOT RUN):** The craft tests ported (the backup holds the previous version; a newer schema opens read-only and is never overwritten; a failed migration opens read-only and leaves the file); a step migration keeps the user's value; the Live reading of Preferences across versions.

#### PD-12 Durability under power loss

*refinement* · size S · evidence inferred · lane engine, win · milestone M5 · after: none

- **Need:** "A crash loses one gesture" holds for a power cut and an OS crash too, or the window that remains is measured and stated.
- **Decision now:** SPEC §3.2 (`synchronous = NORMAL`, FULL on removable or network volumes); ADR-0047 d3; ADR-0001; ADR-0205 d10.
- **On main (audited at ca0290c2):** NORMAL everywhere; no Apple `checkpoint_fullfsync`; checkpoints run inside whichever commit crosses 1,000 pages, on the message thread.
- **Proposed contract:** A BENCHMARKS row first: commit p50 and p99 on the owner thread with NORMAL and with FULL and a near-full WAL, on the director's Windows workstation. If FULL stays inside the frame budget, FULL everywhere (in WAL with NORMAL a power cut can roll back committed transactions); otherwise NORMAL stays and SPEC states the window. FULL on removable and network volumes now (decided, unbuilt); `checkpoint_fullfsync` on Apple with Phase E; `SQLITE_DBCONFIG_DEFENSIVE` on Store connections; a checkpoint thread only if the row shows commit p99 over budget.
- **Reuse:** R6 (Audacity's checkpoint thread), conditional on the row.
- **Reject or defer:** R6 before the measurement.
- **Research IDs:** DR-12-01.
- **Unresolved test (NOT RUN):** The BENCHMARKS row; a pinned reader makes a PASSIVE checkpoint partial with `SQLITE_OK`, not BUSY.

#### PD-13 Salvage a damaged `.adi`

*refinement* · size S · evidence documented · lane engine · milestone M5 · after: PD-04

- **Need:** A bad page in a file that holds the whole history does not cost the whole project.
- **Decision now:** ADR-0033 (`check` reads and never repairs); ADR-0001.
- **On main (audited at ca0290c2):** `adi_tool check` reports only; the vendored amalgamation has neither the recover extension nor `SQLITE_ENABLE_DBPAGE_VTAB`.
- **Proposed contract:** `adi_tool salvage <in> <out>`: open read-only, recover into a private stage, run `checkProject`, report the rows sent to `lost_and_found`, publish without replacing anything. A `quickCheck(path)` on a read-only connection off the owner thread after an unclean open. `SQLITE_DBCONFIG_DEFENSIVE` on ordinary connections, so raw page writes stay impossible outside salvage. A window offer waits until asked for.
- **Reuse:** R5 (SQLite 3.49.2's (its source file), the same release as the vendored amalgamation).
- **Reject or defer:** Any repair in place (ADR-0033); a Salvage button now.
- **Research IDs:** DR-12-01.
- **Unresolved test (NOT RUN):** A planted corrupt page in `ops` keeps every intact op; `application_id` and `user_version` are kept; the output passes `check` or says why; it never overwrites.

#### PD-15 Project tabs and paste between projects

*refinement* · size L · evidence documented · lane engine, window · milestone P2 · after: none

- **Need:** Several projects open in tabs, one active, with paste between them as one undo (the Dream's words: "as REAPER's project tabs"), beside the history's silent read-only tab.
- **Decision now:** ADR-0068 (direction: one Store per project, one active, the clipboard as a transaction, blobs by hash); ADR-0111; FEATURES.md:552 (read-only tabs, P1, with the history); Dream §12.
- **On main (audited at ca0290c2):** Not built; a device paste into another project is a named refusal (#394).
- **Proposed contract:** As ADR-0068 d1-d5, with ids remapped on paste and one `txn_id`; Live's import of tracks from another Set through the browser (A4-011) is the Live-parity part. FEATURES.md has no row: win adds one, its priority a placement to strike if wrong (P2 proposed).
- **Reuse:** None in the harvest.
- **Reject or defer:** Keeping inactive tabs' engines warm (ADR-0111 d1: torn down); REAPER's subprojects (R10H-063 to R10H-068), not proposed.
- **Research IDs:** R3-002, A4-011, A6H-067, DR-12-04, DR-12-06.
- **Unresolved test (NOT RUN):** A cross-project paste of a track with a device and media: ids remapped, one undo step.
- **Cross-check and this plan:** XC-23: one tab host; XC-22: one clipboard model and the one allocator.

#### EX-07 Names, collisions, partial outputs and the run manifest

*refinement* · size M · evidence documented · lane engine, window · milestone M5 · after: EX-01, PD-04

- **Need:** Files named as the user expects (Live's defaults first), never silently replacing anything; a cancelled or failed run leaves no half file and says exactly what was written.
- **Decision now:** ADR-0071 d1, d5; ADR-0205 d5 (one brace syntax), d9 ("silently increment filenames"); ADR-0147 d1; ADR-0203 d1.
- **On main (audited at ca0290c2):** Strict brace names; an existing destination fails the job (`queue.cpp:187, 295-297`); staging cleanup errors are ignored; the 200-byte check runs before the reserved-name underscore and `.wav` (`:126, 133, 184`).
- **Proposed contract:** `Job.collision`: Fail, Increment or Replace. The window follows Live: a single export goes through the save dialog, which asks before replacing; for a batch, Live's behaviour once read, with Increment (REAPER 7.57: the whole delivery group renumbered together) and Replace as listed additions. The collision set is resolved once for every delivery at preflight (case-folded on NTFS and APFS; the format profile is part of an output's identity) and checked again at publication; Replace goes through `replaceFile` from staging. A JSON manifest per run: each delivery's state (completed, failed, cancelled, unstarted), its path, format profile and statistics; the journal head; the media identities. Stage cleanup failures are reported; the length check runs on the final name; date, time, format, rate and index tokens; Live 12.4.3's default (a single-track export appends the track's name).
- **Reuse:** EX3 (Tracktion's per-track naming and its search for a file that does not exist, with case folding added); R1 (`replaceFile`).
- **Reject or defer:** Directory templates (R11X-017) until escaping and empty-value rules exist; REAPER's keep-partial (P3); Cubase's cancel removing already exported files (C4-009), which is not Live's.
- **Research IDs:** A11R-006, A11R-007, A11R-008, A6D-012, R11X-011, R11X-012, R11X-016, R11X-017, R11X-018, R11X-038, R11X-040, R11X-041, C4-007, C4-009, DR-13-04, DR-13-05.
- **Unresolved test (NOT RUN):** R11X-018's, R11X-038's and R11X-040's cases; names of 199, 200 and 201 UTF-8 bytes including `CON.<suffix>`; a collision introduced just before publication; cancel during warm-up, the first block, the last host service and just before publication. Live readings: batch collisions, and M6-A6D-012.

#### EX-09 Job persistence, presets and the agent's path

*refinement* · size M · evidence documented · lane engine · milestone M5 · after: EX-01 to EX-04

- **Need:** Export settings kept as presets per user and per project; a job saved without rendering; a preset that applies only some field groups; the agent fills the queue and never presses render.
- **Decision now:** ADR-0071 d1, d3, d4; ADR-0205 d3, d9 ("export presets per project and per user"; "nothing here needs a schema change"); ADR-0203 d11; ADR-0181 d2, ADR-0039 (one control API).
- **On main (audited at ca0290c2):** None; the queue is in memory.
- **Proposed contract:** `render::Job` to and from JSON under a strict schema (unknown keys reported and refused, ADI's addition over Tracktion's); user presets as JSON in ADI's data folder; project presets and saved jobs as a `session_state` key (Layer 3, outside undo, no schema change); field-group masks (bounds and output, options and format, all); saving a configuration, enqueueing and rendering are three separate actions; control-API `export.queue.{add, list, update, remove, start, stop, cancel, retry}` at the Propose tier through the one dispatcher; the agent's job manager wraps `render::Queue` instead of being a second queue.
- **Reuse:** EX3 (Tracktion's `toJSON`/`fromJSON` and validation); FilmCraft's `export_tools.rs` command set and JSON presets (design); CA1 and OS1 (the dispatcher and the op schema, control-api area); A4 only as a wrapper.
- **Reject or defer:** Cubase's 20-job cap; REAPER's opaque sink blobs (R11X-045).
- **Research IDs:** R11X-013, R11X-014, R11X-015, C4-008, C4-015, DR-13-05, DR-13-07.
- **Unresolved test (NOT RUN):** Tracktion's JSON round-trip and validation cases; a preset that applies only the encoding leaves the destination, source and bounds untouched.
- **Cross-check and this plan:** XC-20: AGT-04's state machine wraps the queue; its MCP binding comes with Track C.

#### EX-10 NI Stems: normalization and macOS

*decided (was refinement; XC-05)* · size S · evidence measured · lane engine, win · milestone M5 · after: PD-00

- **Need:** NI Stems export and import as queue jobs (P1) whose stems keep their relation to the premix, working on every platform.
- **Decision now:** ADR-0205 d8; ADR-0191 d4; ADR-0204 d5; FEATURES.md:449-450.
- **On main (audited at ca0290c2):** Built (#407). `validateNiStems` never looks at `job.normalization`, and `renderNiStems` passes it to five `renderWav` passes, so each stem gets its own gain and no AAC output is checked against the ceiling. Red on macOS (CoreAudio priming).
- **Proposed contract:** Refuse Peak and Loudness normalization on an NI Stems job now (one check, one test); later, one gain measured on the premix and applied to all five, with decode, measure and retry through AAC. The macOS fix as PD-00 says.
- **Reuse:** In-house (#407).
- **Reject or defer:** Writing a Pioneer stem container (none exists, AWAITING 17).
- **Research IDs:** DR-13-11.
- **Unresolved test (NOT RUN):** An NI job with Loudness normalization is refused; `mac-probe` on `adi_mp4_aac_tests` and `adi_ni_stems_tests`.
- **Cross-check and this plan:** XC-05: decided (ADR-0205 d8); one row with TM-IC-05 referring here; XC-04: the after-AAC check (DEV-29 a) lifts the refusal.

#### CO-01 The collaboration core: order, identity and rebase

*refinement* · size XL · evidence documented · lane engine, window · milestone P2 · after: after the first-window gate; depends on PD-05 (a stable client id per project and machine through `setClientId`) and PD-10

- **Need:** Two or more people edit one project asynchronously through a bucket the owner controls; every copy converges and nothing is lost.
- **Decision now:** ADR-0182 d1, d2, d5; ADR-0188 d5; ADR-0161 (open: the conflict model, row ids, concurrent order, shared undo); ADR-0203 d13 ("Excel-style" multiplayer, P2), d17; ADR-0204 (the ghost table and ordering are design work when collaboration is built); SPEC §12 item 6.
- **On main (audited at ca0290c2):** `op_clients`, `op_clocks` and the Lamport stamping; compaction never drops an op not yet uploaded; the rolled-back preview; `Actor::Remote`; `digestProject`. No sync tables, network code, id spaces or fractional order; `CommitOptions` has only `expectHead` and `beforeCommit`.
- **Proposed contract:** Server-ordered, which closes SPEC §12 item 6 in the director's "Excel-style" sense: every batch gets a global `seq` from a sequencer; on the bucket a put-if-absent claim of (its source file) (a bucket without conditional writes is refused in the first version: win's placement, to strike if wrong); on a LAN session the host, in process. A batch carries its author and id space, a batch id, its base `seq`, ADR-0188 d5's vector and its typed ops with Lamport clocks, never inverses. Replicas apply in `seq` order through a `CommitOptions::remote` that writes the author's clocks; a batch that fails is refused identically everywhere; Hello carries the schema version and an op-registry hash. Ids are `(space << 32) | counter` from one allocator that replaces the `MAX(id)+1` sites in the engine and the window and `Snapshot::maxRoutingId`; sibling order uses fractional keys in `order_keys`. The rebase rewinds the pending suffix with its stored inverses, commits the remote batches and re-commits the pending transactions so their inverses are rebuilt, retiring the superseded unsequenced rows in the same transaction so compaction keeps working (squared with ADR-0128 d2 and ADR-0148 d4). Ops that carry whole before-images are re-prepared from intent or checked only on the columns they write. Machine-local columns (media paths, missing flags) stay out of sync and out of the convergence digest; the `sync_*` tables are excluded from the digest, `order_keys` and `remark_anchors` included. LANES §1 gives the engine `src/adi/sync/**` and `src/juce/net_*`.
- **Reuse:** C1 (figcraft's collaboration core: the sequencer, resume and catch-up, persistence with journal replay, the client's reconcile, the simulation with its undo checker); C2 (the SQLite session extension's touch sets, switched on from ADI's own CMake); C3 (figcraft's `frac.rs` with rocicorp's vectors); C4 (delta-rs's claim loop; object_store's SigV4); C8 (Replicache's rebase by replaying named mutators, design); C9 (Fluid's, openDAW's and y-crdt's vocabulary and reconcile rules, design).
- **Reject or defer:** Yjs or any CRDT document mirroring the project (a second truth, ADR-0182 d2); SQLite's experimental rebaser; figcraft's validation on a server copy (the bucket orders only).
- **Research IDs:** DR-12-11, DR-12-13; the SoundCraft/FilmCraft digest (neither has co-editing code); digest 07 (figcraft is the complete design).
- **Unresolved test (NOT RUN):** The simulation: three Stores and a fake object store, 200 seeds with delays, drops and resumes, converging to an equal `digestProject` and equal order with nothing pending; figcraft's and rocicorp's fractional vectors; two replicas inserting a device at one slot converge.
- **Cross-check and this plan:** XC-22: the one allocator function; XC-39: collaboration takes src/adi/collab, not src/adi/sync; XC-41: P2 by priority, no gate.

#### PD-06 Recover a recording that was running at a crash

*new-choice* · size M · evidence documented · lane engine, window · milestone M5 · after: PD-04, PD-05

- **Need:** Audio being recorded when ADI or the machine died comes back into the project.
- **Decision now:** None decides it. ADR-0132 d1 (recordings are Float32 WAV, RF64 past 4 GiB); ADR-0203 d1 (an addition Live lacks needs his yes and a priority).
- **On main (audited at ca0290c2):** `.adi-record-*` folders survive. `WavWriter` rewrites its sizes and flushes the stream every block, so a process crash leaves a readable header; a power cut can leave one the reader refuses. No plan file, no scan, no placement.
- **Proposed contract:** A `take.json` written through `replaceFile` when recording starts (a recording id, track ids, start sample, rate, channel map, project UUID, format); a RIFF and RF64 header repair that touches only files the recorder wrote inside a record folder; recovery is the same prepared batch as Stop, one undo step, with the recording id in the op payload so a second recovery never places a take twice; refused on a read-only project; a take whose track was deleted is reported, not placed. PD-05's launch question names the recovered takes.
- **Reuse:** R3 (SoundCraft's `autosave.rs`: `RECORDING`, `leftovers`, `recover`, `repair_take`; Ardour's `.reclog` and `recover_recordings` as the C++ precedent).
- **Reject or defer:** "Idempotent because media is content-addressed" (the commit makes new op ids; the recording id is what makes it idempotent).
- **Research IDs:** A5C-049 (Live's own guide leaves recovering unsaved recorded audio uncertain), DR-12-01.
- **Unresolved test (NOT RUN):** SoundCraft's two tests (a header claiming 100 frames over 480 on disk is placed whole; a non-take file, a file outside the folder, a (its source file) path and a symlink are skipped and reported); an RF64 take, a PCM24 take, a deleted track; the Live reading M5-A5C-049.
- **Cross-check and this plan:** XC-34: a loop recording's recovery needs CT-02's pass log.

#### PD-07 Live's Backup folder

*new-choice* · size S · evidence documented · lane window, engine · milestone M5 · after: PD-00, PD-01, PD-05

- **Need:** A copy of the project outside the file itself, since the snapshots inside the `.adi` do not survive losing or damaging that file.
- **Decision now:** R-26, "Backups: none" (ADR-0125, his ruling); ADR-0203 d6, d12, d13 (Ctrl+S takes a snapshot, a deliberate deviation; no save step); ADR-0182 d3.
- **On main (audited at ca0290c2):** Nothing; `saveCopy` exists.
- **Proposed contract:** From the second Take Snapshot on, a worker writes a consistent copy (the backup API, never a file copy of the live database) to (its source file); ten are kept, ordered by a UTC time stored in each copy, so a clock that goes back an hour cannot overwrite one; the eleventh goes to the Recycle Bin; copies clear the session lock; media stay referenced, as in Live.
- **Reuse:** R7 (FilmCraft's (its source file): `auto_save_name`, `list_auto_saves`, `rotate`, with its daylight-saving tests); `saveCopy` (#380).
- **Reject or defer:** Deleting the oldest (FilmCraft deletes; Live uses the trash); any timer-driven copy (his no-timer directive).
- **Research IDs:** A5H-014 (Live: a Backup folder after two saves, the last ten versions, older ones to the trash), DR-12-16.
- **Unresolved test (NOT RUN):** The Live reading M5-A5H-014 (the folder's name, the time format, the count, the trash); the clock-goes-back test; a repeated hour overwrites nothing; rotating "Cut" never touches "Cut-Two".
- **Cross-check and this plan:** XC-32: each copy is bounded only once HI-02's cap holds, so it goes to him with D1.

#### EX-06 Dither for Live's three Pow-r slots

*new-choice* · size S · evidence documented · lane engine · milestone M5 · after: EX-04

- **Need:** Live's dither choices are Triangular, Rectangular, Pow-r 1, 2 and 3, and None. POW-r is not usable, so the three slots need open noise-shaped modes, and names.
- **Decision now:** ADR-0203 d1 (Live's dialog), d14 (the registry stores `pow-r1..3` today); ADR-0132 d5.
- **On main (audited at ca0290c2):** TPDF and rectangular, seeded per job and independent of how writes are split; Pow-r refused ("noise-shaped Pow-r options are not implemented").
- **Proposed contract:** One quantizer shared by WAV, AIFF and FLAC (`src/adi/audio/quantizer`), seeded and partition-independent, applied once, never at 32-bit float; shaped modes reserve headroom, so a peak normalized to 0 dBFS cannot clip; rates without a table use a stated fallback. The shapes and their names are the director's (Q3).
- **Reuse:** EX5 (SoX's `dither.c` tables and its error-feedback loop; Ardour's `gdither` Lipshitz table) for option (a); ADI-L's own dither for option (b), only after his plug-in list and the clones lane's commit of that source.
- **Reject or defer:** POW-r (not usable); the Airwindows dithers in `airwin2rack` (none is F-weighted).
- **Research IDs:** A4-004, A6G-010 (Live warns against dithering repeatedly), R11X-034, A11R-009, DR-13-07.
- **Unresolved test (NOT RUN):** The quantization error's spectrum below TPDF between 1 and 5 kHz with its energy above 15 kHz; deterministic and partition-independent; a full-scale peak does not clip; the Live reading of Pow-r 1-3's spectra.

#### CO-02 Undo in a shared project

*new-choice* · size M · evidence documented · lane engine · milestone P2 · after: CO-01

- **Need:** What Ctrl+Z does to a change that collaborators already have.
- **Decision now:** ADR-0188 d5 (undoing a synced batch, "one's own or a collaborator's", appends a compensating batch); ADR-0030; ADR-0182 (shared undo, closed by ADR-0188 d5).
- **On main (audited at ca0290c2):** Undo moves the head (ADR-0030); no compensating path.
- **Proposed contract:** A transaction not yet sequenced undoes by moving the head. A sequenced one appends a compensating batch: its stored inverse ops, minus those whose trial touch set meets anything a later batch by someone else wrote (cascades count, so a track a collaborator recorded onto survives my undo of its creation). Redo compensates the compensation; an undo waits until its transaction is sequenced; a branch switch over sequenced history is ruled with this.
- **Reuse:** C1 (figcraft's `history.rs` rules and the simulation's undo checker); C2 (touch sets).
- **Reject or defer:** Compensating a collaborator's later writes by default.
- **Research IDs:** DR-12-15.
- **Unresolved test (NOT RUN):** figcraft's undo scenarios ported; the undo checker finds no violation over 200 seeds; an undo of an unsequenced transaction uploads nothing.

#### CO-04 Live awareness: how presence travels

*new-choice* · size L · evidence documented · lane engine, window · milestone P2 · after: CO-01 and the control-api server (CA1, CA2)

- **Need:** The directive's "Excel-style awareness: live visual cues, with a toggle", in a design where "ADI runs no service".
- **Decision now:** ADR-0182 directive part 3 and d8 ("the awareness layer"); ADR-0182 d2; ADR-0181 d2 (one server); ADR-0126 (the LAN audio node, a different thing).
- **On main (audited at ca0290c2):** Nothing; ADI has no listener of any kind.
- **Proposed contract:** Option (a): presence as heartbeats written beside the batches in the bucket, at seconds resolution, with no live cursors. Option (b): (a) plus an optional LAN-hosted live session, off by default and bound to 127.0.0.1 unless the user picks the network, joined by link or code with a secret; presence throttled to 33 ms, latest wins; follow view, follow playback and spotlight; media fetched by hash from the host; the host's order authoritative and relayed to the bucket; on the control API's server stack, not a second one.
- **Reuse:** C5 (figcraft's presence, signals, join codes, LAN hub and limits; JUCE's `NetworkServiceDiscovery`, in tree); C6 (openDAW's Follow Playback).
- **Reject or defer:** A relay service of ADI's (against "ADI runs no service").
- **Research IDs:** DR-12-11, DR-12-12; digest 07 (figcraft's LAN mode: no discovery, an invite link or a 20-symbol code, and the session ends when the host leaves).
- **Unresolved test (NOT RUN):** A loopback host and two joiners (a resume keeps the client's space; a wrong token or schema refused; presence at or under 30 a second per peer); a follower starting within one block plus the measured transit.
- **Cross-check and this plan:** XC-40: Q5's "first network listener" wording corrected; one network stack.

### A.6 Control, AI, surfaces and testing (55 rows; source the control matrix (win's working file))

#### CTL-02 The registry describes itself (JSON Schema export)

*decided* · size M · evidence documented · lane engine · milestone C · after: none

- **Need:** The wire schema and the agent's `ops_describe` are generated from the op registry, with words for ADR-0025's short keys. Constraints have one authority.
- **Decision now:** ADR-0016 (the JSON Schema has been owed since); ADR-0025; ADR-0039 d2 (the wire schema is generated from the registry); ADR-0168 d5.1; AI-AGENT §3.
- **On main (audited at ca0290c2):** `Field` is `{key, type, required}` only (`ops.hpp`). `validateSubmission` checks presence, type, unknown keys and finiteness, never ranges or enums. There is no schema export. Log payloads go through `decodeFromLog`, which is permissive.
- **Proposed contract:** **Field metadata.** `Field` gains optional trailing members: `description`, `enumValues`, `minimum`, `maximum`, `maxLength`, `maxItems` and `items`. Existing `constexpr` initialisers stay valid. **The schema.** `opInputSchema()` emits closed objects (`additionalProperties: false`), with optional fields as `[T, "null"]` and Int as `integer`. **Default bounds,** MAGDA's unless declared: ids at least 0, strings at most 16 KiB, arrays at most 4096. **One authority.** `validateSubmission` enforces the same metadata, at submission only. Replay of existing files is untouched. **The CLI.** `adi_tool ops --schema [names]`. **Descriptions first** for the ops `agentMayEmit` admits.
- **Reuse:** OS1: MAGDA `remote_api.cpp` l.38-95 (`applyIntegerBounds`, `applyRequestLimits`) and `OperationRegistry::describe`; gridcraft `tools.rs` `obj()`.
- **Reject or defer:** MAGDA's `validateJson`, which would be a second validator; hand-written tool schemas.
- **Research IDs:** DR-14-01, DR-14-04.
- **Unresolved test (NOT RUN):** AC-24: every op has a closed schema; a payload passes the schema exactly when `validateSubmission` accepts it; out-of-range, enum and over-long payloads are refused naming the key. Every payload the shell suite submits still validates once the constraints land.

#### CTL-06 Feed subscriptions and the param and transport methods

*decided* · size M · evidence documented · lane window, engine · milestone P2 · after: CTL-03, CTL-04, SRF-01, SRF-04

- **Need:** Clients read and drive parameters, transport and selection through the same message-thread feed the UI draws. Gestures and undo are identical to the mouse's.
- **Decision now:** ADR-0181 d1-d3 and d5; ADR-0188 d4, ADR-0198, ADR-0124, ADR-0162; ADR-0206 d4 (the ArtCraft bridge's transport subscription; INTEROP-ARTCRAFT §9); FEATURES l.271 (one feed, P1).
- **On main (audited at ca0290c2):** `ParameterFeed::publish` rebuilds the whole device publication every tick, with no subscription and no per-subscriber diff. `touched()` has no caller, so `lastTouchedBy` is only None or Automation. `parameterGesture` is still the refusing stub.
- **Proposed contract:** **Subscribe.** `param.subscribe {names}` (never "everything") returns a snapshot of the named records, then per-tick diffs, at most one per parameter per frame. **Stalled clients** get drop-and-resync, and are disconnected after 100 refused flushes. **Read and write:** ; `param.get`; `param.set`, `param.begin` and `param.end` go through `parameterGesture` into `ParamOps::uiEvent`: one undo per gesture, the opener on first touch; `param.nudge {target, ticks, fine}` goes through SRF-04's resolver. **Attribution.** Every write stamps `lastTouchedBy` through `ParameterFeed::touched(id, param, Source)`. **Events:** `transport.*`, `history.head`, a transport subscription (the fields INTEROP-ARTCRAFT §9 leaves to this step) and a selection subscription.
- **Reuse:** CA1 (MAGDA `SubscriptionHub`: snapshot, delta, drop-and-resync).
- **Reject or defer:** a second feed (ADR-0181 d3); deltas in the log (ADR-0181 d1).
- **Research IDs:** DR-11-05, DR-11-06, B3-013, A6P-003, A3-018.
- **Unresolved test (NOT RUN):** AC-8: a surface's `param.set` commits at once as `user` / `surface:<name>`. AC-9: begin, twenty sets and end leave one undo entry with its opener. AC-10: a nudge at the top of the range writes nothing. AC-12: subscribing with no parameters is refused. AC-13: the strip's knob and a subscriber see identical records on the same tick. SF-8: last touched wins.

#### CTL-07 Visual array streams (P2)

*decided* · size M · evidence documented · lane engine, window · milestone P2 · after: CTL-04, CTL-06

- **Need:** Surfaces with screens draw spectra, meters and scope windows streamed from the DAW.
- **Decision now:** ADR-0184 d4; ADR-0188 d6; ADR-0175; FEATURES l.273 (P2).
- **On main (audited at ca0290c2):** the sources exist: `ScopeTap` (a stamped wait-free ring), `StripMeter` (#381) and `Session::mainLoudness` (#382). There is no stream code.
- **Proposed contract:** **Subscribing.** `streams.subscribe {name, points, rateHz}` returns a `stream_id`; `streams.unsubscribe` ends it. **Frames.** A 16-byte little-endian header (u16 `stream_id`, u16 `channel_count`, u32 `frame_size`, 8-byte timestamp), then float32 values channel by channel, 4-byte aligned. **Names:** `track.<id>.meter`, `track.<id>.spectrum`, `master.correlation`, `device.<id>.scope`. **Producers** run on the message thread, or on a worker for an FFT. *[XC-19: producers serialise MX-30's worker outputs and run no FFT of their own]* Frames come no faster than the UI clock. **Raw audio** is a separate, capped stream, marked in the UI while a client holds it. **Refinement for win: the timestamp is a signed i64.** `ScopeTap`'s stamp is `int64` and negative until the latency has elapsed. The 8 bytes are laid out the same; JavaScript reads them with `getBigInt64(…, true)`. **It also carries `ui.mixview.focus`** (the followed row, item and CTZ group) for the Stream Deck.
- **Reuse:** CA1 (MAGDA's Sample events, design); the Mix View program's E12 and W16.
- **Reject or defer:** arrays as JSON numbers (ADR-0188 d6 chose binary frames); analysis on the audio thread for a client (ADR-0184 d4).
- **Research IDs:** DR-11-06.
- **Unresolved test (NOT RUN):** AC-20: the header decodes with `DataView(…, true)`; frames come no faster than the UI clock; raw audio is capped and marked. The first consumer is the director's `adi_visualizers_and_meters`.
- **Cross-check and this plan:** XC-19: no second FFT worker; XC-34: needs MX-30.

#### AGT-01 The changeset as a service, with the decided caps

*decided* · size S · evidence documented · lane engine · milestone C · after: none

- **Need:** The Propose tier works from any client, with ids, a queue the user reviews, and the caps the director set.
- **Decision now:** ADR-0145 d9; ADR-0148: d4 (stale is refused, never rebased) and its open items (where a queued changeset lives, per-op editing, the caps); ADR-0203 d20: 6000 ops per request (was 256) and 6000 a minute; ADR-0146; FEATURES l.588 (Propose tier, P1).
- **On main (audited at ca0290c2):** The changeset is built (#100): preview, apply, `agentMayEmit`, `unifiedDiff`, `changesetFromJson`. `Limits::maxOps = 256` (`changeset.hpp` l.44). The registry rows `agent.maxOpsPerRequest` and `agent.maxOpsPerMinute` are never read. A changeset has no id and there is no queue. AI-AGENT.md still opens with "Nothing built", and its §6.1 still says 256.
- **Proposed contract:** **The caps.** `maxOps` defaults to 6000 and follows `agent.maxOpsPerRequest`. `agent.maxOpsPerMinute` is enforced on Apply-tier commits. **Ids.** Each changeset gets an id, a client and a request id, so the same request id queues once. **The queue.** An app-scoped `agent::ChangesetQueue` (awaiting review, applied, rejected, stale, expired), never in the `.adi`. **Propose** previews and queues, returning the diff and the change list. **Apply** previews, then applies unless an always-confirm rule fires (AGT-09); then it queues. **Previews** queue, at most one per client. A stale changeset is refused, never rebased. Per-op editing stays open.
- **Reuse:** ADI's own changeset; the idempotency shape from A4.
- **Reject or defer:** silent rebase; per-op editing (ADR-0148, open).
- **Research IDs:** DR-14-02, DR-14-12.
- **Unresolved test (NOT RUN):** `test_changeset`, extended: the cap follows the setting; one queue entry per request id; the 6001st op in a minute is refused at Apply. The preview's latency at 6000 ops on a 10,000-clip project. ADR-0148 d2 measured 330-390 ms for one op; nobody has measured the cap.

#### AGT-05 The user's side: review, consent, chat and entry points

*decided* · size L · evidence unknown · lane window · milestone P2 · after: AGT-01, AGT-03, CTL-09

- **Need:** The person sees and decides every changeset. The agent is reached from the places the Dream names.
- **Decision now:** ADR-0148 (open: where the UI keeps a changeset); AI-AGENT §6.1 and §8 ("not a chatbot bolted to a sidebar"); ADR-0115 (marker prompts run only on the user's action); ADR-0104 d3; FEATURES l.288-289 (P2), l.588 (P1); Dream §2 and §14.
- **On main (audited at ca0290c2):** an empty AI tab only. `aiChatOpen` toggles and reserves `chatArea_` (`ui_shell.cpp` l.577-794), and nothing is drawn there.
- **Proposed contract:** **The review panel:** a unified diff, and a per-op before-and-after list built from each op's inverse; Apply and Reject; a stale banner offering a fresh preview. **The consent prompt** of AGT-03. **Chat** in the existing collapsed tab at the foot of the browser. **Entry points:** ; free text in the palette (WIN-10); a marker's prompt, run only when the user triggers it; model-backed buttons (fix the gain staging, match a groove) that produce ordinary changesets. **Agent remarks** are marked, and read as context only. **No panel commits on its own.**
- **Reuse:** none external. The panels stub against `adi_tool propose` output until CTL-08 exists (ADR-0210 d3).
- **Reject or defer:** per-op editing (later, by analysing which ids each op creates); a chat box as the only interface.
- **Research IDs:** DR-14-02, DR-14-03, DR-12-10.
- **Unresolved test (NOT RUN):** Diff rendering for arrangement and automation edits is AI-AGENT §9.3's open problem. An experiment build should show the director a 40-op arrangement changeset. Apply is one undo step. The stale banner appears when the head moves.

#### AGT-06 The sidecar `adi-agent`, its budgets and its providers

*decided* · size M · evidence documented · lane sidecar seat, win · milestone P2 · after: CTL-08, AGT-03, AGT-09

- **Need:** The agent loop itself, outside the DAW, on whatever provider the owner chooses.
- **Decision now:** ADR-0168 d1-d5: pi-mono, not a fork; three tools; no shell, file or browser tools; the keychain; Docker optional. ADR-0086 (tier 2), ADR-0064 d1 and d5; ADR-0204 d2 (installer tiers: local AI, AI on another PC); AI-AGENT §6.6 and §7.
- **On main (audited at ca0290c2):** nothing (there is no `adi_daw/agent`).
- **Proposed contract:** **Libraries:** `pi-agent-core`, `pi-ai` and `pi-mcp` at exact pins with integrity. **Launch:** on demand, as a child process. **Control channel:** JSON lines on stdio for prompt and cancel; `AgentEvent`s end with `{reason, detail, steps, tokens}`. **Tools** come from ADI's MCP endpoint (CTL-08). The token arrives in the environment. Tool execution is sequential. **The budget guard:** ; 8 steps, 32,000 tokens, 8 mutations, 2 minutes of wall time; at most 2 identical calls at one head; a cap on changesets per run. **Remark taint.** A run whose `project_read` returned remark text is tainted, and its changesets queue even at Apply. **Providers,** all behind AGT-03: local: llama.cpp or Ollama through an OpenAI-compatible URL; a cloud API on the owner's key; a LAN or self-hosted endpoint. **Credentials** live in the OS keychain. Skills are read-only Markdown shipped with ADI.
- **Reuse:** A5: pi-mono `4bb837c` (its source file), `ai` (with its faux provider) and `mcp`. A6: MAGDA's `AgentRuntime` budgets and its 18 tests, as design. A7, optional: `pi-schema`, and the official TypeScript MCP SDK as a fallback for the 2026-07-28 revision.
- **Reject or defer:** forking OpenClaw (ADR-0168); Docker as the default; MAGDA's in-process models and its one-tool-per-op MCP surface; `pi-durable` (it only has `maxTurns`).
- **Research IDs:** DR-14-05, DR-14-01, DR-14-13.
- **Unresolved test (NOT RUN):** vitest with the faux provider: read, describe and propose ends `completed`; a repeated identical call stops with `repeated_tool_call`; a parallel batch holding `changeset_propose` is refused; running out of wall time completes unexecuted calls with errors. One real-provider smoke per provider class, with consent.

#### AGT-07 Analysis tools the agent can call

*decided* · size M · evidence inferred · lane engine · milestone P2 · after: AGT-02, AGT-04

- **Need:** Measurements a text model can reason about, since it cannot hear.
- **Decision now:** AI-AGENT §5; ADR-0168 ("analysis_run tool, later"); ADR-0064 d1.
- **On main (audited at ca0290c2):** `Session::mainLoudness`; `ScopeTap`'s `correlation()` and `bestOffset()`; a placeholder transient detector and an IOI-vote Auto-Warp. There is no `analyze.*` tool or job.
- **Proposed contract:** **The tools:** `analyze.loudness`, `spectrum`, `transients`, `key`, `tempo`, `harmony`, `groove`, `masking` and `headroom`. **How they run:** as jobs (AGT-04) on the render path against a WAL snapshot, never blocking edits. **Their output:** each tool's output schema is described as `ops_describe` describes ops. Results feed `project_read` level 3. **The analyses come from their own areas:** the one loudness meter, the warp-analysis stack, the key detector, the scope.
- **Reuse:** none in this area. It depends on loudness LD1 or LD2, warp-tempo WT1-WT3 and media MD6, which are other areas' takes.
- **Reject or defer:** raw audio to a model by default. That happens only under the leaves-the-machine confirmation (AI-AGENT §5).
- **Research IDs:** DR-14-04, DR-14-08, DR-14-09.
- **Unresolved test (NOT RUN):** Each tool against a fixture with a known answer: a -14 LUFS file, a 120 BPM loop, a masking pair. The AGT-08 tasks that need each tool.
- **Cross-check and this plan:** XC-19: reads ADIA instead of recomputing per file.

#### SRF-01 The strip's parameter drag is one undo step

*decided* · size S · evidence documented · lane window · milestone M1 · after: none

- **Need:** A drag of a device parameter in the strip edits, sounds and undoes as one step. It is also the path a surface's gesture takes.
- **Decision now:** ADR-0124; ADR-0181 d2 and d5; need N7 (struck on 5 November when the engine half merged); FEATURES l.271.
- **On main (audited at ca0290c2):** `parameterGesture` (`project_document.cpp` l.138-149) still refuses with "waits for the engine's UI producer", although `ParamOps::uiEvent` landed in #390 through #420. `ui_main.cpp` l.136-142 wires the gesture. `ParameterControl` already sends Begin, Value and End (`ui_devices.cpp` l.18-24).
- **Proposed contract:** **The call.** `parameterGesture` calls `uiEvent(id, param, kind, value, nowMs, requests)` on the steady-clock milliseconds that `tick()` drains on. **The commit.** It commits everything appended, even when this event was refused, because an older gesture may have completed. **The source.** `ParameterFeed::touched(id, param, Source)` takes a source and is called. **The engine's caveats stay named:** the capture is not sample-exact against automation, and CLAP ids are hex while automation refs are decimal.
- **Reuse:** in tree (#390).
- **Reject or defer:** committing only on release (Live sounds during the drag).
- **Research IDs:** DR-11-05, A6P-003.
- **Unresolved test (NOT RUN):** Worklist A3's `adi_ui_document_tests`: Begin, three Values and End make one undo step whose first op is the opener, and undo restores the value. An event refused while an earlier gesture completes still commits that gesture. SF-9, the device half.

#### SRF-04 One resolver for nudge, acceleration, conserved ticks and takeover

*decided* · size S · evidence documented · lane engine · milestone M3 · after: SRF-03

- **Need:** Ticks behave identically from the control API, MIDI relative rows, V-Pots and the Focus Dial. Absolute controls take over as Live's do.
- **Decision now:** ADR-0181 d1; ADR-0188 d4 (4^((30 − Δt)/30), at most 4×; last touched wins); ADR-0203 d21 (Pick-Up until measured); ADR-0130 d4 and d7.
- **On main (audited at ca0290c2):** `ControllerTakeover {Jump, Pickup, Scale}` is stored only. There is no resolver, tick counter or gate, and nothing reads `midi.takeoverMode`.
- **Proposed contract:** **The resolver.** `resolveNudge(record, ticks, fine, msSinceLastTick, sourceAccelerates)` returns an absolute normalized value, or nothing when clamped. **Steps.** Switch and Menu parameters move one step per tick. Fine divides by 10. **Acceleration, once.** ADR-0188 d4's curve applies only to unit-tick sources: the Stream Deck, the linear MIDI types and a linear Focus Dial. It never applies to MCU or HUI V-Pots or accelerated types, whose wire already carries speed. **Ticks are conserved.** They are summed in an atomic per-binding counter and taken whole on the message thread. Rising toggle edges are never lost. **`TakeoverGate`:** ; None; Pick-Up: input is ignored until the control's movement crosses the value, and a host-side change re-arms it; Value Scaling: provisional, behind the same interface. **Relative bindings bypass takeover.** ; **One turn is one `device.setParam` per settle,** with absolute values before and after.
- **Reuse:** SU7: Tracktion's `pickedUp` crossing rule, copied with attribution. SU8: Mixxx's truth table and test matrix, tests only. SU9: Ardour's threshold-in-sync rule, a third oracle.
- **Reject or defer:** MAGDA's router, which overwrites pending relative ticks and starts its accumulator at 0; Mixxx's threshold and time override, which Live's text does not describe; helgoboss-learn's Value Scaling: not usable, so it is a hypothesis only.
- **Research IDs:** A6R-004, A3-018, DR-11-05, DR-11-11.
- **Unresolved test (NOT RUN):** SF-5, SF-6 (a thousand ticks while the message thread is held 50 ms move exactly a thousand steps) and SF-7. AWAITING row 14 and SF-22c, measured in Live 12.

#### SRF-05 MIDI ports, the Remote route and feedback

*decided* · size M · evidence documented · lane window, engine, win · milestone M3 · after: SRF-03, SRF-04

- **Need:** ADI opens MIDI ports at all. Remote input carries mappings and surfaces; Remote output carries feedback; Track input carries playing and recording.
- **Decision now:** ADR-0203 d1; SETTINGS-CATALOGUE's MIDI Ports rows (decided parity); ADR-0130; ADR-0195 d3; ADR-0210 d3.
- **On main (audited at ca0290c2):** No `MidiInput`, `MidiOutput` or `MidiDeviceInfo` anywhere in `src/`. `dispatchController` has no caller. `MpeParser` and `MidiMemory` (#430) have no producer from hardware. #446 and #447 are open. The window cannot list ports. `midi.port.*` is Device scope, which `AppSettings::write` refuses.
- **Proposed contract:** **One port host,** `src/juce/app_midi_ports.*` *[XC-18, as revised: on JUCE 9's UMP API from the start with JUCE_USE_WINDOWS_MIDI_SERVICES off, the Win32 backend named as such in Settings; the service is SRF-16 at P2]*: it enumerates and opens inputs and outputs; it hears hot-plugs through `MidiDeviceListConnection`; it matches identity by identifier, then by name, and heals the stored key; it reads per-port Track, Sync, Remote and MPE. **The Remote route,** in order: 1. decode on the MIDI thread; 2. an SPSC ring to the message thread; 3. `dispatchController`, with the Focus Dial first; 4. SRF-03 and SRF-04; 5. ops. **Mapped messages are consumed** before the Track route sees them. **Remote inputs merge.** Feedback goes only to Remote-enabled outputs. **The Tempo & MIDI page** lists the ports. **The Track route** hands timestamped events to the engine's MIDI input through a needs row. One host serves two routes; the engine never opens ports itself. **Win gives `midi.port.*` a home** first.
- **Reuse:** SU10: JUCE's `MidiDeviceListConnection`, in tree. SU1: MAGDA's `MidiDeviceMatch`, for identity; the router's shape only.
- **Reject or defer:** libremidi (JUCE suffices).
- **Research IDs:** A3-010, A6R-001, A6R-002, A6R-003, A6U-013, DR-11-01, DR-11-07.
- **Unresolved test (NOT RUN):** SF-1, SF-2 and SF-21. SF-22b: whether a Live mapping is specific to its port when Remote inputs merge. With no ports present, the app starts and says so.
- **Cross-check and this plan:** XC-18: on JUCE's UMP API from the start; the engine owns the queues, capture (#446, #447) and clock decoding. This revision turns the Windows MIDI Services backend off here (JUCE 9.0.2 marks it experimental, needs Windows 11, SDK 10.0.20348, a NuGet package outside the official feed and an installer prerequisite; his machine runs Windows 10) and gives it its own later row, SRF-16, so SF-1, SF-2 and SF-21 can pass on his box and on CI.

#### SRF-06 The Master Focus Dial (P1)

*decided* · size M · evidence documented · lane window · milestone M3 · after: SRF-01, SRF-04, SRF-05

- **Need:** One endless encoder drives whatever the mouse hovers, or the plug-in parameter last touched.
- **Decision now:** ADR-0130 d1-d7; ADR-0188 d4; FEATURES l.555 (P1); SETTINGS-CATALOGUE (decided, ADR-0130).
- **On main (audited at ca0290c2):** it is reserved at dispatch only. `dispatchController` gives it first refusal, and its CC can never be learned (tested in #171). There is no target, lock or HUD.
- **Proposed contract:** **Targets:** hover for native controls; touch for plug-in parameters (VST3 and CLAP gesture begins, captured as ADR-0124 does). **Push** locks it. **Shift** divides the step by 10. **The HUD** reads [Device] → [Parameter] → [value], cyan while following and amber while locked. **A turn** goes through SRF-04 and the capture: one op per settle. A second dial stays ADR-0130's open item.
- **Reuse:** none external.
- **Reject or defer:** a second dial.
- **Research IDs:** DR-11-11.
- **Unresolved test (NOT RUN):** SF-20; acceleration on the director's own encoder.

#### SRF-08 The surface host: six slots, MCU, HUI, profiles, follow, lock and pin

*decided* · size XL · evidence documented · lane engine, window · milestone P2 · after: SRF-04, SRF-05, SRF-07

- **Need:** Live's Control Surface list with common surfaces working, feedback without loops, and ADI's HUI addition.
- **Decision now:** ADR-0203 d1 and d10 (surfaces P2); ADR-0181 d4: Mackie, HUI and OSC, with HUI an addition; ADR-0125 R-14: controller extensions and auto-add; ADR-0188 d4; Dream §11.
- **On main (audited at ca0290c2):** Built: `controller_maps` (14 columns; protocols midi, osc, mackie, hui), `controller.bind` and `unbind`, Learn, `dispatchController`. Registry rows exist for `midi.controlSurfaceScripts` and `midi.autoAddControllers`. There are no slot rows, codecs, drivers or host.
- **Proposed contract:** **Six App-scoped slots,** `midi.surface.<1..6>.{type,input,output}`, with Dump. The types are: Live's Mackie entries, spelled as Live's chooser spells them (SF-22a); HUI, ADI's addition, shown first-class; Profile: a declarative JSON profile from the shipped set or `midi.controlSurfaceScripts`. **Auto-add** fills a free slot when a known device appears. **Instant mappings** are driver state that follows the device selection, never rows. ADR-0181 d4's wording is amended to say so. **Locks.** Live's "Lock to…" works per surface, with the hand icon. Arming a MIDI track locks its instrument. The Pin key is the same lock. **Feedback rules per protocol:** ; the MCU's servo echo; a touched fader is never moved; only changed LCD characters are sent; HUI's ping once a second. **Output** is diffed per UI tick and sent off the message thread. **MCU and HUI button meanings** follow Live's, measured (SF-22a). They are provisional until then. **No script language now.** This narrows R-14's "Bitwig's script model", and the ADR says so. **Grid controllers** (Launchpad, Launchkey, Launch Control XL, FaderPort) wait for Session View, at P3.
- **Reuse:** SU3: Ardour's MCU protocol layer, and its 48 device files converted to JSON. SU5: DrivenByMoss's HUI codec, with REAPER's `csurf_babyhui` and Ardour's `hui_heartbeat` as oracles. SU9: Ardour's 58 generic MIDI maps as Profile seeds; its native drivers at P3. SU1: MAGDA's `ControllerProfile`; `MidiLearnCoordinator` as design. SU7: Tracktion's driver interface, as a checklist.
- **Reject or defer:** Ardour's `mackie_control_protocol` bindings and subviews; Live's MIDI Remote Scripts: not usable; MAGDA's Lua scripts: a later ADR; Push: CH-1.
- **Research IDs:** A6R-002, A3-017, A6R-012, DR-11-03, DR-11-05, DR-11-07, B3-011, B3-012, M3-CONTROLLER.
- **Unresolved test (NOT RUN):** SF-10 to SF-13 and SF-21. SF-22a: an MCU or emulator in Live 12. Whether Live saves "Lock to…" in the Set. All 48 device files load. The codec in loopback against itself.

#### SRF-09 OSC (P2)

*decided* · size M · evidence documented · lane engine, window · milestone P2 · after: SRF-02, SRF-07

- **Need:** Plain OSC control and feedback with no hole in the network.
- **Decision now:** ADR-0185 d1 and d4; ADR-0188 d7; ADR-0181 d2; SPEC §8.5.
- **On main (audited at ca0290c2):** `controller_maps` admits protocol `osc` and `osc_path`. `juce_osc` is in the pinned JUCE but not linked. There is no OSC code.
- **Proposed contract:** **Off by default,** on the interface the user picks, with an optional client list. **ADI owns the UDP read loop,** because `juce::OSCReceiver` hides the sender. `juce_osc` only encodes. **A fixed namespace:** (its source file), (its source file), (its source file) and (its source file). Optionally, get, set and listen addresses for clients without OSCQuery. **Writes** are `user` + `osc:<client>`, inside the surface allow-list. **Events.** Releasing a momentary Play never stops the transport. Deltas are never coalesced. **Feedback** is sent on change. It is never echoed to the sender within the flush its value arrived in, and it is confirmed in the next. **Plain OSC rows** stay in `controller_maps` for Learn.
- **Reuse:** SU1 (MAGDA's OSC packet, address, router, feedback, peers and service, and its seven test files); SU11 (AbletonOSC's listen pattern, design).
- **Reject or defer:** Ardour's liblo surface; liblo, oscpack and tinyosc.
- **Research IDs:** DR-11-07, DR-11-08.
- **Unresolved test (NOT RUN):** SF-16; TouchOSC as a plain-OSC client on the director's network.
- **Cross-check and this plan:** XC-40: in the one network stack and the Privacy list.

#### SRF-10 OSCQuery v1 (P2)

*decided* · size M · evidence documented · lane engine, window · milestone P2 · after: SRF-07, SRF-09

- **Need:** The parameter tree served on the local network, off until the user opens it.
- **Decision now:** ADR-0185 d4; ADR-0188 d7 (an in-process responder on nlohmann, no web-server library, DNS-SD per OS, off by default, a warning, read and write together); FEATURES l.274.
- **On main (audited at ca0290c2):** the catalogue lists OSCQuery as `Absence::Backlog` (l.448-449). There is no code.
- **Proposed contract:** **HTTP GET only:** (its source file), a path, and `?HOST_INFO`, whose EXTENSIONS list only what is implemented. **The tree** is built from the feed, with stable ids, TYPE `f`, VALUE, RANGE `[{MIN 0, MAX 1}]`, ACCESS 3, and a DESCRIPTION holding the name and `Record.text`. **DNS-SD** advertises `_oscjson._tcp` only while the service is on. **Values** travel as OSC, with SRF-09's feedback rules. **v2 waits.** LISTEN, IGNORE and PATH_* need a WebSocket, and win rules on its transport later. **FEATURES' wording on TouchOSC and Lemur** is corrected.
- **Reuse:** SU6 (the spec as a protocol description; libossia's JSON shapes and `OSCQueryTest` fixtures; juce_organicui, design).
- **Reject or defer:** organicui's CORS `*`, its file endpoints and raw pointers; Servus; a read-only mode (ADR-0188 d7 removed it); v2 now.
- **Research IDs:** DR-11-08.
- **Unresolved test (NOT RUN):** SF-17. ossia score's explorer and Vidvox's browser load the tree and move a value.

#### SRF-12 TUIO and touch (P3)

*decided* · size M · evidence documented · lane window · milestone P3 · after: SRF-02, SRF-09

- **Need:** Touch sources the OS does not deliver.
- **Decision now:** ADR-0185 d6; ADR-0203 d5 (no touch-only desktop paths).
- **On main (audited at ca0290c2):** nothing. The catalogue lists TUIO as `Absence::Backlog` (l.450-451).
- **Proposed contract:** Windows touch uses JUCE's native multi-touch, one source per finger, through the mouse's gesture paths (SRF-01, SRF-02). TUIO 1.1 over OSC on UDP 3333 joins the same path. It serves tables, IR overlays, network surfaces and macOS, and is off until opened.
- **Reuse:** none; SRF-09's parser carries TUIO's bundles.
- **Reject or defer:** deferred to P3.
- **Research IDs:** DR-11-10.
- **Unresolved test (NOT RUN):** two faders moved at once on a Windows touch screen leave two undo entries; a TUIO 1.1 cursor from reacTIVision's simulator.

#### SRF-13 The ADI Stream Deck + XL plug-in (P2)

*decided* · size M · evidence documented · lane win · milestone P2 · after: CTL-05, CTL-06, SRF-04, SRF-07, SRF-14

- **Need:** The director's own controller, first among surfaces.
- **Decision now:** ADR-0181 d4; ADR-0188 d4; ADR-0209 d7, d9 and d11: it follows the Mix View's hovered channel or group, has a [REF] key, and is P2; FEATURES l.270; the director's ruling of 5 November: no slots, and the unified built-in feature's design waits for his list.
- **On main (audited at ca0290c2):** nothing in `adi_daw`. His plug-ins live in `tools/elgato_stream_deck_plugins`: Studio OS, `adi_ableton_vst_controller` 1.5.9.0 with 13 controllers, and a Node skeleton for another of his tools.
- **Proposed contract:** **The plug-in:** Node (SDK v2, an exact (its source file) 1.4.x pin, Node 20, Stream Deck 7.3 or later, DeviceType 13), carrying ADI's name. **Connection.** It pairs once (CTL-05), and sends a bearer token and no Origin. **Following.** It follows the selection, and Pin holds the page. In the Mix View it follows the hovered row, else the focused row, or a CTZ group. **Dials.** A turn is `param.nudge` with Elgato's ticks. Touch or push begins a gesture and release ends it: one undo per touch. **The touch strip** shows `Record.text`. **Keys:** [REF] (Shift+R's commands), solo, page and bank. ADI draws the key images. **His 13 controllers** are re-pointed at the API and keyed on plug-in identity. They drive his VST3 and CLAP builds as ordinary plug-ins. **Not now.** Pages for the unified built-in feature wait for his list. The slot keys fall with N16.
- **Reuse:** SU2: Elgato's SDK; his controller's bridge, controllers and touch-strip slicing (his); the another of his tools Node skeleton; Studio OS's lessons.
- **Reject or defer:** a browser-page plug-in: it cannot send Authorization, and its timers are throttled; Studio OS's `ws-server`, which has no Origin check; per-channel ADI-Q and ADI-L pages, withdrawn.
- **Research IDs:** DR-11-04, DR-11-05, DR-11-06.
- **Unresolved test (NOT RUN):** SF-19: it pairs once, and a dial turn is one undo step; every write is `user` / `surface:<name>`; the 13 controllers resolve every role; CLAP and VST3 parameter names are checked against each other. The director's pass on his + XL.

#### SRF-16 The Windows MIDI Services backend (MIDI 2.0 on Windows)

*decided* · size M · evidence documented · lane window, win · milestone P2 · after: SRF-05

- **Need:** MIDI 2.0 devices over Windows MIDI Services, behind the same port host as SRF-05, once the service and its SDK exist on a machine ADI can be tested on.
- **Decision now:** ADR-0188 d7 ("MIDI 2.0 stays P2"; ADI's part is to switch on `JUCE_USE_WINDOWS_MIDI_SERVICES` and parse UMP; JUCE marks the service experimental and falls back where it is not installed, so Settings shows which backend is live).
- **On main (audited at ca0290c2):** nothing: no MIDI port I/O at all (SRF-05). `third_party/JUCE` 9.0.2 `juce_audio_devices.h`: the flag defaults to 0; "at time of writing, this only works on the Canary insiders build of Windows 11"; it needs target and minimum Windows SDK 10.0.20348.0, the CppWinRT tool and the Windows MIDI Services NuGet package, which "is not hosted in the official repository" (a local package repository is needed); it builds with MSVC ("some versions of Clang may work but this is not guaranteed"); the end user must have the service's SDK package installed, so the installer must carry it or say so; without it JUCE falls back to WinRT or Win32, which "do not support MIDI 2.0". The director's machine runs Windows 10 19044.
- **Proposed contract:** a later row carrying what SRF-05 leaves out: the flag on behind SRF-05's seam, with no change to the Remote, Track and Sync routes; the SDK pin raised to 10.0.20348 or later on the Windows CI legs; the NuGet package in a local repository on those legs, pinned; the installer's tools tier carrying the service's SDK package, or a first-run notice; Settings naming the live backend (Windows MIDI Services, WinRT or Win32); a Windows 11 seat named for its acceptance. Nothing of it is claimed from a Windows 10 or Win32 run.
- **Reuse:** in tree: JUCE 9.0.2's UMP endpoints and backends.
- **Reject or defer:** claiming MIDI 2.0 on Windows from a Win32 run; JUCE's older WinRT backend as the default; any MIDI 2.0 work before SRF-05's host is in.
- **Research IDs:** DR-11-01 (the port rows); none for the service itself.
- **Unresolved test (NOT RUN):** a UMP MIDI 2.0 device enumerated and read through the service on a Windows 11 seat with the service installed; the Win32 fallback exercised when the SDK package is absent; the installer's prerequisite check; SF-1, SF-2 and SF-21 unchanged on both backends.
- **Cross-check and this plan:** XC-18 as revised (the critique of 5 November): the first draft switched the service on at M3 as a placement, which advanced a P2-ruled item, defaulted an experimental backend and could not be exercised on his box or on CI.

#### WIN-01 The function keys do functions (a defect)

*decided* · size S · evidence documented · lane window · milestone M1 · after: none

- **Need:** Keys as the director ruled them. His smallest project has more than a hundred channels, so eight track keys are useless.
- **Decision now:** ADR-0203 d6 and d12: his named deviation; F3 is Session View and F7 offline processing; ADR-0209 d9: F4 is the Mix View and F5 is free; FEATURES l.565 (P0).
- **On main (audited at ca0290c2):** `getCommandInfo` binds F1-F8 to Activate Track 1-8 (`ui_shell.cpp` l.140-148) and F4 to the Mix View (l.275-278). JUCE gives F4 to the first mapping that holds it, so Activate Track 4 never fires from F4. The tests at `test_ui_shell.cpp` l.1880 and l.2958 each test one command, and neither notices the clash.
- **Proposed contract:** F1-F8 lose the activator defaults. The eight commands stay, unbound, for the palette and the Shortcuts page. F4 is the Mix View. F3 and F7 are held for Session View and offline processing. parity-7.6 row 6 is amended. A golden of every preset's default map, with a no-clash check.
- **Reuse:** WI4's conflict check, as a test.
- **Reject or defer:** Live's F1-F8 defaults, unless the director asks for them back.
- **Research IDs:** DR-01-01, A5W-010.
- **Unresolved test (NOT RUN):** "the default map of each preset has no clash" (it fails today on F4); "F1-F8 activate nothing; F4 opens the Mix View; F3 and F7 are unbound".
- **Cross-check and this plan:** XC-09: one item with MX-14; XC-36: F7 stays unbound until its FEATURES row.

#### WIN-05 Shortcut editing, reference presets, non-Latin layouts and key dispatch (second half)

*decided* · size L · evidence inferred · lane window · milestone M3 · after: WIN-04

- **Need:** A shortcut editor, presets that match the three reference DAWs, and keys that work under Hebrew.
- **Decision now:** ADR-0125 R-25 (keys and MIDI; named sets; import and export; "a Cubase user gets Cubase's keys on day one"); ADR-0125 d4 (Hebrew and RTL from day one); ADR-0180 d6.
- **On main (audited at ca0290c2):** only `shortcuts.preset`, and on Windows the presets differ only in Redo.
- **Proposed contract:** **Chords.** Canonical chord text, with Cmd and Ctrl folded off macOS. Foreign key files are read. An override stays verbatim when unusable. **Conflicts.** Steal, Keep or Refuse. Clash and shadow are told apart. Reserved system chords are refused per OS (Alt+F4 on Windows). **Named sets** export and import as `{"format":"adi-keymap","version":1}` and move across OSes. **Search** by name or by pressed key. **The presets.** The Cubase and Bitwig presets carry those programs' full default tables, taken from their documentation, not from memory. **Non-Latin layouts.** The layout rule needs a native physical-key source (the Windows virtual key, the macOS key code), because `KeyPress` carries none. **Dispatch order,** provisional until measured: 1. a focused text field; 2. Key Map keys; 3. the Computer MIDI Keyboard, where Shift+letter still reaches shortcuts; 4. application shortcuts. **Keys typed into a hosted native plug-in editor** never reach ADI.
- **Reuse:** WI4: craft-libs' keymap (chord, foreign, overrides, reserved, profiles, editor, and layout including `TypingRule`, with their tests).
- **Reject or defer:** macros, because one key running several commands is not one journal operation (C5W-017); craft's egui UI.
- **Research IDs:** A5W-015, C5W-015, C5W-016, C5W-017, C5W-018, DR-01-11.
- **Unresolved test (NOT RUN):** Driven with the Hebrew layout on Windows and the Mac: Ctrl+Z undoes, and Z and X zoom. A set exported on Windows imports on the Mac with ⌘. Ctrl+Alt+B assigned to another command asks Steal, Keep or Refuse. A5W-015 measured in Live 12.

#### WIN-06 The command palette (Ctrl/Cmd+P)

*decided* · size M · evidence documented · lane window · milestone M3 · after: WIN-04

- **Need:** Live's keyboard-first loading, as the floating palette.
- **Decision now:** ADR-0104 d3 and d4; ADR-0203 d6 (Ctrl/Cmd+P); FEATURES l.279 (P1); Dream §2.
- **On main (audited at ca0290c2):** No palette (the master-reference audit lists it as Missing). `builtinCatalog` and its one-step insert are on main (#395). Installed plug-ins wait for the scanner (#438-#440, open). The library store (N15) is partly built.
- **Proposed contract:** **The popup** opens over the focused window, from any window. **Matching:** fzy scoring on UTF-32. Hebrew matches as it is. **Prefix-routed providers,** the longest prefix winning: commands, with their key hints; disabled ones are listed with their reason and never run; V/: ADI's devices, then installed plug-ins, inserted on the selected track as one undo step; S/ and P/: samples and presets, once the store exists (Space auditions, Enter loads). **Free text** follows WIN-10 (CH-4). **It closes on focus loss.** With no project open, actions that need one are disabled with the reason.
- **Reuse:** WI5 (Helio's palette, copied with attribution; the shape of soundcraft's `CommandSpec`, enabled → reason); WI6 (fzy and its 24 cases).
- **Reject or defer:** Helio's lib_fts matcher; `setlocale`.
- **Research IDs:** DR-14-03, DR-02-01.
- **Unresolved test (NOT RUN):** window-infra acceptance 22-28 (golden ranks such as "hsb" → Hide/Show Browser; V/ inserts as one undo step); fzy's 24 cases with exact scores.

#### WIN-07 Key Map Mode, MIDI Map Mode and the Mapping Browser (P2)

*decided* · size L · evidence documented · lane engine, window · milestone P2 · after: SRF-03, SRF-05, WIN-04

- **Need:** Live's MIDI and key mapping, which Dream §11 asks for by name: Ctrl+M, Ctrl+K and the mapping browser.
- **Decision now:** ADR-0180 d6 (Key Map is project-scoped in `controller_maps`, with a keyboard source); ADR-0195 d3; FEATURES l.235 (P2).
- **On main (audited at ca0290c2):** `learnControllerTarget` is on main (#398). There is no keyboard source (`validateBinding` refuses one), and no map mode or Mapping Browser.
- **Proposed contract:** **The modes.** Ctrl/Cmd+K and Ctrl/Cmd+M overlay the assignable controls. **Binding.** One message or key binds, through `learnControllerTarget` (a captured `controller.bind`). **The Mapping Browser** lists manual mappings for the active mode, with path, Min, Max and invert. It never lists instant mappings. **Notes act by target kind:** a slot follows its launch mode, a switch toggles, a radio cycles. **Key Map rows** travel in the `.adi`. **Clip View mappings** act on the current selection. **The relative Session strip** arrives with Session View.
- **Reuse:** SU1 (`MidiLearnCoordinator`, design); WI4 (chord text for key sources).
- **Reject or defer:** per-strip Learn buttons (Live learns through Map Mode).
- **Research IDs:** A6R-005, A6R-006, A6R-007, A6R-011, A5W-012, A5W-013, A5W-014, DR-11-01.
- **Unresolved test (NOT RUN):** SF-14; A5W-012 and A5W-013 measured in Live 12.

#### WIN-08 Live's Second Window (P2)

*decided* · size M · evidence documented · lane window, win · milestone P2 · after: WIN-02, WIN-03

- **Need:** Live's extra window on any display.
- **Decision now:** ADR-0203 d1; ADR-0063 d1 (reparenting); ADR-0101 and ADR-0117 (Session View, P2). No FEATURES row exists yet.
- **On main (audited at ca0290c2):** not in any ADI document. The Mix View already detaches as ADI's addition (ADR-0209 d11).
- **Proposed contract:** Ctrl/Cmd+Shift+W opens it. Tab swaps Session and Arrangement between the two windows. Shift+Tab or F12 moves the Detail View. The Browser and the Info View stay in the main window. Ctrl/Cmd+W closes it. `window.zoom` keeps `second@<display>`. It is a normal window kind.
- **Reuse:** WI1 and WI2 (normal kind); ADR-0063's reparenting, already built.
- **Reject or defer:** P2, after Session View.
- **Research IDs:** A5W-001, A5W-002, A5W-003, A5W-004, DR-01-02, DR-01-10.
- **Unresolved test (NOT RUN):** M5-A5W-001 to 004, in Live 12.

#### WTS-11 The Live side: L01-L10 captured, never read from the manual

*decided* · size M · evidence unknown · lane measurement seat, window · milestone M4 · after: WTS-06

- **Need:** Parity rows proved against Live itself.
- **Decision now:** ADR-0108 d1, d4 and d5; LANES §5 (Live measurements); AWAITING.md.
- **On main (audited at ca0290c2):** Nothing is captured. L01-L10 are defined in `docs/daws/ableton-12.md` (#436). Live 12.4 is on the second Windows machine and Live 11.2.7 on the director's Mac. the director's Windows workstation has no Live.
- **Proposed contract:** **Captures.** The measurement seat captures L01-L10 in Live 12.4 on the second Windows machine, and Mac-only rows on the director's Mac. **Each L-row gets an ADI script** (WTS-06). **Behaviour rows** (L01, L03, L04, L09, L10) are answered from `ui.inspect`. **Picture rows** (L02, L05-L08) get a side-by-side sheet at one scale, with a 50 % overlay. **Relay.** Win relays dated results into the window lane's "Measure on Live" lines and AWAITING, by OPEN-QUESTIONS' relay rule.
- **Reuse:** none in this area. The measurement area's MS takes apply, and pywinauto drives Live there.
- **Reject or defer:** striking a row from a manual or a static controller palette.
- **Research IDs:** L01-L10, M-LIVE-UI, SRC-TEXT-ONLY.
- **Unresolved test (NOT RUN):** AT-23; all ten L-rows captured, each with its build, hash and operator.
- **Cross-check and this plan:** XC-38: the one seat and queue; L02, L05, L06, L09 and L10 are shared with MX-22.

#### CTL-01 One control stack: dispatcher, listener, wire, actors and owners

*refinement* · size S · evidence documented · lane win, engine, window · milestone M0 · after: none

- **Need:** One recorded shape before any code. One JUCE-free dispatcher in `src/adi/control` is shared by agents, surfaces, scripts, the ArtCraft bridge and the UI test surface, with one listener, one wire, one actor rule and owners (ON13).
- **Decision now:** ADR-0039 d1-d6; ADR-0168 d5: the library is "decided in that step"; ADR-0181 d2: no second server; ADR-0206 d1 and d4: the bridge is a client; AI-AGENT §7.1; FEATURES l.270, 273, 290 and 298; LANES §1, where no lane owns `src/adi/control/**`.
- **On main (audited at ca0290c2):** There is no socket, dispatcher, token or schema: a grep of `src/` for WebSocket, httplib, JSON-RPC or MCP finds nothing. The pieces it calls exist: `validateSubmission`; `Actor` (with `Remote`) and `Scope`; `OpJournal::commit` with `expectHead`; the changeset; `OpSubmitter` per actor; `ParameterFeed`. No lane's queue holds the work.
- **Proposed contract:** **The listener.** One listener at (its source file), JSON-RPC 2.0. It is off by default (`control.enabled`), and port 0 means any free port. (its source file) over HTTP is allowed only on the same server and port, and `adi-mcp` is stdio. **Explicit project.** Every project-touching method names its project (R10D-021; ADR-0068's several open projects). **Request metadata.** A `meta` sibling of `params` carries: `expectHead`, an opaque equality token passed to `CommitOptions::expectHead`; `deadlineMs`; `idempotencyKey`. **Actors,** by who writes the ops: agents and MCP clients write only through the changeset: `agent` / (its source file); surfaces write `user` / `surface:<name>`; OSC writes `user` / `osc:<client>`; MIDI-CI writes `user` / `midici:<muid>`; synthetic UI input writes `ui-test:<client>`; `remote` is used only for a script granted Apply; Tier-2 results follow whoever wrote the ops. **Threads.** Network threads never touch the model. Validation runs on the calling thread. Handlers run on the message thread through an injected executor. `project_read` uses a second, read-only connection. **Identity.** The API exposes ADI's durable integer ids and the head token. It never imitates Live's unsaved API ids (A6P-001). Notifications never mutate (A6P-002). Remote parameter control stays undoable and attributed, deliberately unlike A6P-003. **Owners.** ; The engine owns `src/adi/control/**` and `tools/adi-mcp`. The window owns `src/juce/app_control.*`, the Settings page, the subscriptions and the test surface. Win owns the pins and the registry rows. **Timing:** CH-3.
- **Reuse:** CA1 (= A1) as the base; CA4 design rules (the gridcraft and wordcraft control channels).
- **Reject or defer:** MAGDA's two listeners on two ports, and the agent report's `src/adi/rpc` split; IXWebSocket; Ardour's MCP and WebSocket surfaces and SoundCraft's channel: no authentication; anything past loopback, which stays the user's deliberate act.
- **Research IDs:** R10D-021, DR-14-01, DR-14-02, DR-11-04, DR-11-06, A6P-001, A6P-002, A6P-003.
- **Unresolved test (NOT RUN):** AC-6, conformance: every write method ends in `validateSubmission` plus `OpJournal::commit`, or in `agent::preview`/`apply`, with its actor asserted. AC-17: every project-touching method needs an explicit project. With two projects open, a request naming B never touches A.
- **Cross-check and this plan:** XC-39 and XC-40: the LANES paths and the one network stack come with this record.

#### CTL-03 The control core (JUCE-free)

*refinement* · size L · evidence documented · lane engine · milestone C · after: CTL-01, CTL-02

- **Need:** Authentication, grants, dispatch, idempotency, deadlines and audit, shared by every client.
- **Decision now:** ADR-0039 d1 and d3-d5; ADR-0181 d2; AI-AGENT §2 and §7.1.
- **On main (audited at ca0290c2):** nothing. `OpSubmitter`, `expectHead` and the changeset are there to call.
- **Proposed contract:** **Errors:** -32602 invalid, -32002 stale (with `headFound`), -32005 limits, -32006 not granted. **Authentication.** Tokens are compared in constant time, with one refusal text whatever was wrong. **Grants:** ; keyed by the paired credential, never by a self-declared name; the session token only bootstraps; a new client starts at Propose or lower, and only the user raises it, through a first-use prompt; Apply comes only from an explicit per-client grant, under `agent.maxOpsPerMinute`; surfaces get ADR-0181 d2's allow-list; structural edits need an explicit per-surface grant; with no grant registry, everything is refused. **Idempotency** is keyed by credential plus request id. **Deadlines.** Requests are PENDING, TAKEN or ABANDONED, so a timed-out request is never applied late. **Stale writes.** An `expectHead` mismatch writes nothing. **Audit.** A ring of 512 connection events and refusals, with secrets redacted. Ops are already audited in the op log.
- **Reuse:** CA1 (= A1): MAGDA's service, scopes, clients, audit, HTTP auth and API host, with tests 37, 10, 87 and 21. Ported from C++23 to C++20 on nlohmann, with an injected executor. CA4: gridcraft's `ControlRequest` states (#1914), first-line auth and bounded pending auth; wordcraft's instance key rule.
- **Reject or defer:** MAGDA's name-keyed grants and its dedupe keyed by connection; its dispatch lock held across the handler; inline execution when there is no message loop.
- **Research IDs:** DR-14-02, DR-14-15, DR-11-04.
- **Unresolved test (NOT RUN):** AC-1, AC-3, AC-6, AC-7 and AC-14 to AC-19, in process. `test_rpc_permissions`: permission is checked before validation; a revoked grant is never replayed; a write needs more than read; a new client starts at Propose or lower; a hand-edited grants file grants nothing it cannot name; secrets never reach the audit.

#### CTL-04 Transport: one loopback WebSocket

*refinement* · size M · evidence documented · lane engine, win · milestone C · after: CTL-03

- **Need:** A socket that refuses bad tokens and Origins before the protocol switches, and survives hostile clients.
- **Decision now:** ADR-0039 d4; ADR-0181 d2: a WebSocket; Origin absent or on an allow-list (the lesson of CVE-2026-25253); ADR-0168 d5; ADR-0188 d6.
- **On main (audited at ca0290c2):** there is no network code. nlohmann/json is pinned. The core has no HTTP server.
- **Proposed contract:** **The library.** cpp-httplib v0.64.0 (`24e58ce5`), behind one pimpl translation unit. **Binding.** 127.0.0.1 only. **Refusals before the 101:** ; 401 and 403 always come before the switch; the 503 for the last slot is best effort: a racing client is upgraded, then closed with PolicyViolation. **Origins.** The Origin is checked when present. Browsers need `control.allowedOrigins` (empty by default) and `?token`. **Caps:** ; 8 connections, exact; 240 KiB frames; 8 requests in flight; 50 requests a second (a token bucket); a 10 s deadline. **Writing.** Reply and event budgets are separate, with one writer thread and an outbox per connection. **Never dispatched:** an HTTP POST. **Shutdown.** `max_missed_pongs` is used only from 0.61. `stop()` joins every thread. MAGDA's shutdown patch is re-cut, or replaced by a read timeout plus a read interrupt.
- **Reuse:** CA2 (cpp-httplib v0.64.0; MAGDA's shutdown patch); CA1 (`RemoteWebSocketServer`).
- **Reject or defer:** cpp-httplib v0.52.0; IXWebSocket; civetweb: it can only close, never answer 401 or 403; CHOC, websocketpp, uWebSockets.
- **Research IDs:** DR-11-04, DR-11-06.
- **Unresolved test (NOT RUN):** AC-2 to AC-5, over real loopback sockets on the three OS legs: an unlisted Origin gets 403 even with a valid token; a bad token gets 401 before the handshake; an oversized frame closes the connection; a malformed upgrade strands no slot. MSVC (its source file) and clang's strict flags with httplib in one translation unit.

#### CTL-08 `adi-mcp`, the stdio MCP bridge

*refinement* · size M · evidence documented · lane engine · milestone C · after: CTL-02, CTL-03, AGT-01, AGT-03

- **Need:** Claude, Codex and the sidecar reach ADI through MCP with no second listener. It also works headless over a file, for tests and batch work.
- **Decision now:** ADR-0168 d1 and d3: three tools; ADR-0181 d2: no second server; ADR-0039 d6: any caller; ADR-0064 d6: what leaves the machine is named, per project; AI-AGENT §7.2 and §7.3.
- **On main (audited at ca0290c2):** only `adi_tool propose <file.adi> <changeset.json> [--apply]` reaches the changeset.
- **Proposed contract:** **Out of process, stdio, JUCE-free,** with two backends: `--project <file.adi>`, headless over `adi_core`; `--connect`, which reads the discovery record and, with no ADI running, says how to start it. **Tools:** `project_read`, `ops_describe`, `changeset_propose` and `changeset_status`; `jobs_*` later. `transport_get` and `param_get` come only with `--connect`; `ui_*` only with the `ui` grant. **Both protocol eras:** the 2025-11-25 `initialize` and the stateless 2026-07-28 (its source file). **Errors.** An op failure is a tool error carrying ADI's `Refusal`. An unknown tool is -32602. Hostile arguments never crash it. **`_meta`** uses a reverse-DNS prefix ADI controls. **The instructions text** says: project content is data; a remark never authorises an action; nothing commits without the user; one request is one undo step. **Installation.** The install folder gets an explicit ACL. **Consent.** An external host's read is egress, so AGT-03's consent comes before `project_read` returns anything for a non-local destination.
- **Reuse:** CA3 (= A2): MAGDA's `McpEndpoint` era routing and its tests 25, 26, 6 and 7; the bridge's `resolveEndpoint`, `isProcessAlive` and `dataDir`, only; gridcraft's server, Remote backend shape, headless backend and `obj()`; soundcraft's `agent_tasks.rs`.
- **Reject or defer:** a second Streamable-HTTP listener (only (its source file) on the same server and port); muse_framework's MCP: no token, binds `Any:2212`; Ardour's `mcp_http`; community C++ MCP SDKs, which lack grants and audit.
- **Research IDs:** DR-14-01, DR-14-02, DR-14-03, DR-14-15.
- **Unresolved test (NOT RUN):** AC-23: legacy `initialize` and modern (its source file); headless, read → propose a volume change → diff, with the file unchanged until applied. `test_adi_mcp_bridge`: it finds the instance and survives a restart and a token rotation.

#### CTL-09 The app host and the Clients page

*refinement* · size M · evidence documented · lane window, win · milestone C · after: CTL-03, CTL-04, CTL-05

- **Need:** The running app starts and stops the server, binds open projects, and keeps every window current after an API write. The user can see, grant, revoke and rotate.
- **Decision now:** ADR-0039 d4: the token is shown in the UI; ADR-0181 d2: grants per surface; ADR-0203 d11 / FEATURES l.298: a Settings Scripts page at P2, listing installed scripts and how much each may do; ADR-0201; ADR-0152.
- **On main (audited at ca0290c2):** nothing. The catalogue lists the control API's row as `Absence::Backlog` (l.444).
- **Proposed contract:** **The host,** `src/juce/app_control.*`, starts on `control.enabled`. Win's rows: `control.enabled` (off), `control.port` (0), `control.allowedOrigins` (empty). **The executor** is `MessageManager::callAsync`. **Refresh.** Each client writes through its own `OpSubmitter`, so `ProjectView` refreshes after API writes. The feed's flush is hooked after `publish`. **One Settings page,** after Live's ten: the session token, masked, and the pairing code; the paired surfaces, scripts and agents, each with its grants; the live connections and the recent refusals; Revoke, Disconnect and Rotate token. **The catalogue row** leaves Backlog when pairing ships.
- **Reuse:** CA1 (MAGDA's `RemoteApiHost`; its Connections dialog as design).
- **Reject or defer:** an embedded script language; scripts are external clients.
- **Research IDs:** DR-14-02, DR-11-04.
- **Unresolved test (NOT RUN):** the app answers `system.describe`; a revoke in Settings refuses the very next request; an API write refreshes every open window on the next frame.

#### AGT-02 Observe tier, the levelled projection and a stable project identity

*refinement* · size M · evidence inferred · lane engine · milestone C · after: CTL-02

- **Need:** What `project_read` returns: levels 0-3, scoped, off the message thread. And a project id that consent and clients can key on.
- **Decision now:** AI-AGENT §2 (Observe) and §4 (levels 0-3); ADR-0168 d3: `project_read(level, scope)`; ADR-0007; roadmap step 8; `schema.sql` declares `adi_meta.project_uuid` as "stable identity across Save As".
- **On main (audited at ca0290c2):** the text projection is whole-project only. `project_uuid` is declared with `''` and never written: a grep finds it only in `schema.sql`.
- **Proposed contract:** **`project_read {level 0..3, scope {tracks?, range?}}`** returns `{text, level, head}`: level 0 always; levels 1 and 2 in bars, beats and note names; level 3 is AGT-07's analysis. **It runs on a second, read-only connection.** ; **`project_uuid`.** The Store writes a random one at create, and on the first writable open of an older file. **Save As keeps it.** Win rules on Save a Copy and Collect and Export; the proposal is to keep it, because a copy is the same project for consent.
- **Reuse:** ADI's own text projection.
- **Reject or defer:** dumping SQL rows or the whole canonical text at a model.
- **Research IDs:** DR-14-04, DR-14-03, A6P-004.
- **Unresolved test (NOT RUN):** The fidelity corpus (AI-AGENT §9.4, through AGT-08). `project_uuid` is set after create, upgrade and Save As. Two files share a uuid only if one was saved from the other.

#### AGT-03 The consent ledger

*refinement* · size M · evidence documented · lane engine, window · milestone P2 · after: AGT-02

- **Need:** "What leaves the machine is named, per project, off by default" becomes structural before any non-local model or external host reads a project.
- **Decision now:** ADR-0039 (open: consent and confidentiality); ADR-0064 d6; AI-AGENT §2 and §6.7.
- **On main (audited at ca0290c2):** nothing. The `agent.endpoint` and `agent.asyncServices` rows exist. `project_uuid` is unwritten.
- **Proposed contract:** **The disclosure** names kinds and sizes, never contents. Parameters alone still count. **The record** is keyed by app, `project_uuid`, connection, destination fingerprint and capability, with its kinds and expiry. **Terms:** This time, One hour, One day, Until revoked. **Asking again.** A new destination or capability asks again. Each kind lapses on its own. **The file.** At most 256 KiB, atomic, and fail-closed: unreadable means nothing is granted. **Locality.** "Local never asks" means loopback, judged by the parsed address. A model on the LAN asks. **External MCP hosts.** An external host's read grant is recorded per project, as a disclosure. **Refusal.** `project_read` and remote Tier-2 submits refuse with `ConsentRequired` until granted.
- **Reuse:** A3: upstream craft-libs `a55273e` (its Disclosure, ledger and `parameters_alone_still_need_consent`); craft-ai-integrations `02068b0` (the terms, the consent file, the locality rule).
- **Reject or defer:** the harvested Disclosure, which skips consent for parameter-only requests; a quit-scoped term, which does not exist upstream.
- **Research IDs:** DR-14-05, DR-14-15, DR-14-02.
- **Unresolved test (NOT RUN):** `test_consent`: craft's 29 tests, plus the parameters-alone case; a new `project_uuid` asks again; loopback never asks, and the LAN asks; a refusal until granted.
- **Cross-check and this plan:** XC-40: the one consent system; nothing else asks.

#### AGT-04 Jobs a client can watch, cancel and trust after the project moves

*refinement* · size M · evidence documented · lane engine · milestone M5 · after: CTL-03

- **Need:** One legal job state machine for renders, stem separation, analysis and remote services, with the revision rule ERRATA-11 corrects.
- **Decision now:** ADR-0086 (open: the Tier-2 wire format); ADR-0064 d1-d3; ADR-0071; ADR-0205 d5; ERRATA-11.
- **On main (audited at ca0290c2):** `render::Queue` (#300) has `JobState {Pending, Running, Completed, Failed, Cancelled}`, add, update, remove, retry, run, snapshot, and a `Control` with a stop token and progress. It has no transition rule, owner, retention or caller-visible revision policy. The child runner and the stem gateway exist only in open PRs (#438-#440, #453).
- **Proposed contract:** **States.** `can_become` keeps progress monotone and terminal states final. **Cancel** is owner-scoped. Retention is 128 jobs or 10 minutes. Each event is handed out by exactly one poll. **Revision policy per job type:** ; StableUntilCompletion for jobs that commit into the project: a stem import or marker analysis that finishes after an edit fails with a conflict and publishes nothing *[XC-20: refused as stale only when the job's declared read set changed]*; CheckAtStartOnly for renders, exactly as the audited queue does it. Job values are fixed at enqueue, run membership at `Queue::run`, and the revision and names at each job's start (ERRATA-11). **Bindings.** It binds `render::Queue` and the stem gateway. **Over MCP,** it reports progress and honours cancellation.
- **Reuse:** A4: MAGDA's jobs and its 7 cases; craft's job rules and 5 `can_become` vectors; FilmCraft's `long_job.rs`.
- **Reject or defer:** enqueue-time snapshots (superseded by ERRATA-11); floating-point cost fields.
- **Research IDs:** DR-14-12, DR-14-06, A11R-001, R10H-079.
- **Unresolved test (NOT RUN):** `test_rpc_jobs`. A late stem import after an edit fails with a conflict and leaves no artifact. An export queued before an edit and started after it renders the revision at its start (A11R-001).
- **Cross-check and this plan:** XC-20: the declared read set replaces "fails after any edit"; one queue, wrapped.

#### AGT-08 The evaluation corpus

*refinement* · size S · evidence unknown · lane win · milestone C · after: AGT-01, AGT-02

- **Need:** Knowing whether a change to the projection or the prompt made the agent better. AI-AGENT §9.5 says "build this early".
- **Decision now:** AI-AGENT §9.5.
- **On main (audited at ca0290c2):** nothing.
- **Proposed contract:** **The corpus:** fixture projects plus tasks with checkable outcomes, as digest or projection assertions. **Two drivers:** `adi_tool propose` and the sidecar with the faux provider in CI; a real model by hand. **Adversarial fixtures:** a track named as an instruction, and an agent-addressed remark. Their expected result is a refused or queued changeset.
- **Reuse:** soundcraft's `agent_tasks.rs`, as design.
- **Reject or defer:** judging taste.
- **Research IDs:** DR-14-01, DR-14-02.
- **Unresolved test (NOT RUN):** the corpus runs; the injection fixtures prove nothing commits.

#### AGT-09 Tiers at the boundary: their scope and the always-confirm rules

*refinement* · size S · evidence documented · lane win, engine · milestone M0 · after: AGT-01

- **Need:** Which tier applies to which project and client, and how the always-confirm list is enforced, settled before any client can apply.
- **Decision now:** ADR-0039 d3; AI-AGENT §2 (the tier is set per project; the always-confirm list) and §7.3; ADR-0131 d5; ADR-0125 d1-d3; ADR-0152 d4.
- **On main (audited at ca0290c2):** `agent.tier` (observe, propose, apply; default propose) is App scope (`registry.cpp` l.833). `agentSet`, `agentMayChange` and `agentForbidden` are built (#105). Nothing enforces the 20 % rule or the remark rule.
- **Proposed contract:** **The conflict.** AI-AGENT §2 sets the tier per project; the registry sets it per application. **Proposed resolution:** ; `agent.tier` stays the app default; a per-project override in session state (Layer 3) may only lower it, unless the user raises it; no client raises its own tier; a client's grant is capped at the effective tier. **Always confirm:** ; media deletion and the agent's own tier are impossible by structure already; a changeset touching more than 20 % of the clips queues even at Apply; so does any egress; so does any run tainted by remark text.
- **Reuse:** none external.
- **Reject or defer:** executing because a remark asked (never).
- **Research IDs:** DR-14-02, DR-14-15, DR-12-10.
- **Unresolved test (NOT RUN):** A project at Propose under an app-level Apply queues. A changeset touching 21 % of the clips queues at Apply. A run tainted by a remark queues.

#### SRF-02 Mixer gestures: audible while moving, one undo step

*refinement* · size M · evidence documented · lane engine, window · milestone M2 · after: none

- **Need:** Volume, pan and send faders sound during a drag and leave one undo step. For the mouse today this is a Live parity defect. Motor faders, V-Pots and dials need the same path later.
- **Decision now:** ADR-0181 d2: touching a strip is a gesture begin; ADR-0162; OPS §6.4 specifies coalescing within 300 ms, which is unbuilt; `op_submitter.hpp` hands non-parameter coalescing to win; ADR-0203 d1.
- **On main (audited at ca0290c2):** `ui_mixer.cpp` l.145-168 submits `mixer.setVolume` and `mixer.setPan` only in `onDragEnd`, and nothing sounds during the drag. A strip lane is marked overridden only on refresh, after a commit. `AutomationRecorder` has no caller outside its tests.
- **Proposed contract:** **An engine producer** takes Begin, Value and End for `mixer.setVolume`, `mixer.setPan` and a send's gain. **Each Value** sets a live value that the audio thread reads, with no op. **End** commits one op whose inverse is the value at Begin. **An unbracketed value** closes after a quiet window, as `ParamOps` does. **While touched,** the strip lane is overridden live. **Recording.** It feeds the automation recorder while Automation Arm and record are on (Live manual p.491). **Adoption.** The mouse drag adopts it, and so do the volume and pan value keys.
- **Reuse:** ParamOps' shape, in tree.
- **Reject or defer:** committing only on release.
- **Research IDs:** DR-11-05, DR-11-07.
- **Unresolved test (NOT RUN):** SF-9: a mouse drag changes the live gain during the drag and leaves one entry whose inverse is the starting value; an MCU touch, twenty moves and a release leave the same single entry. The armed-and-recording case, against Live's p.491 behaviour.
- **Cross-check and this plan:** XC-17: the one gesture contract, its live-value strategy; MX-23 is its mixer face.

#### SRF-03 Encoder flavours, absolute decoding and the keyboard source in the format

*refinement* · size S · evidence documented · lane engine, win · milestone M3 · after: none

- **Need:** Store which of Live's relative encoder types a binding uses, decode 14-bit and note-like CCs as Live does, and give Key Map rows a source.
- **Decision now:** ADR-0181 d1: encoder modes stay in `controller_maps.mode`; ADR-0130 d1; ADR-0180 d6; ADR-0144; SPEC §8.5; OPS §9.11.
- **On main (audited at ca0290c2):** `validateBinding` accepts modes 0-2 only; protocols midi, osc, mackie and hui only; and `msg_num` up to 127 (`controller_maps.cpp` l.55-64). Detection reads a two-direction sign-bit encoder, and any accelerated stream, as Absolute.
- **Proposed contract:** **Widen the mode values.** `mode` has no CHECK, so only the validator and the SPEC and OPS text change: 0 absolute; 1 two's complement (today's meaning, by convention); 2 toggle; 3 binary offset; 4 signed bit (the MCU's polarity); 5 signed bit inverted (HUI's); 6-9 the linear variants of 1, 3, 4 and 5. **Detection** returns the type from a slow left turn. **Absolute decoding:** ; 14-bit values and pitch bend split at 8191/8192; CC 64-127 acts as a Note On for a slot; activators follow range membership, which inverted bounds invert. **Key Map rows:** protocol `key`, message type `key`, the key code in `msg_num`, with its range and device-name rules widened. **`midi.focusDialMode`** is widened with it. **Live's names** (Signed Bit and Signed Bit 2) attach only after SF-22d.
- **Reuse:** SU1 (MAGDA's `BindingTransform` and its tests, the encoders only); SU9 (Ardour's names and polarities).
- **Reject or defer:** a separate encoding column (logged payloads would need a default rule); MAGDA's router write path.
- **Research IDs:** A6R-008, A6R-009, A6R-010, A6R-011, A3-018, DR-11-01, DR-11-07.
- **Unresolved test (NOT RUN):** SF-3, SF-4; a row with mode 1 still reads as two's complement; SF-22d in Live 12.

#### SRF-07 The feed's surface half

*refinement* · size M · evidence documented · lane window, engine · milestone P2 · after: CTL-06

- **Need:** Surfaces draw and drive strips, transport and selection, not only device panels.
- **Decision now:** ADR-0181 d3: one feed for the UI and surfaces, with name, stored and playing values, text and automation state; ADR-0198; ADR-0211; STEP-7-PLAN §7 item 4: the source of `Record::playing` is open.
- **On main (audited at ca0290c2):** device panels only. There are no strip, transport or selection records, and the playing value has no source.
- **Proposed contract:** **Strip records:** id, name, palette index, volume, pan and send positions with their text, mute, solo, arm, selected, and a meter tap id. **Transport:** play, record, loop, metronome, and the position in beats and as timecode text. **Selection:** track, device, and each surface's lock or pin. **The Mix View's** `ui.mixview.focus`. **Identity.** `DevicePanelData` publishes the plug-in's identity uid, so a controller is never keyed on a renamable name. **Playing values.** `Record::playing` for automated values comes from lock-free slots, through an engine needs row. **Sources.** `driven` and `lastTouchedBy` tell modulation, automation and manual input apart.
- **Reuse:** CA1's `SubscriptionHub`, through CTL-06.
- **Reject or defer:** a surface reading the model directly (ADR-0181 d5).
- **Research IDs:** DR-11-06, DR-11-05, B3-013.
- **Unresolved test (NOT RUN):** SF-15 (a subscribed strip record equals what the strip draws on the same tick); AC-13.
- **Cross-check and this plan:** XC-16: the playing value comes from the modulation evaluator's slot.

#### SRF-11 MIDI-CI property exchange (P3)

*refinement* · size M · evidence documented · lane window, win · milestone P3 · after: SRF-05, SRF-07

- **Need:** A controller discovers ADI and labels itself from the device in focus.
- **Decision now:** ADR-0185 d3 (P3, "waits on the OS MIDI stacks"); ADR-0188 d7; FEATURES l.127; SETTINGS-CATALOGUE l.114.
- **On main (audited at ca0290c2):** `juce_midi_ci` is in the pinned JUCE but not linked. The catalogue lists MIDI-CI as Backlog, with the OS precondition (l.352-353).
- **Proposed contract:** **Strike the OS precondition** in FEATURES, SETTINGS-CATALOGUE, `catalogue.cpp` and the ADR's follow-up. JUCE runs MIDI-CI over MIDI 1.0 SysEx. It stays P3 on priority alone. **Responders.** A `ci::Device` per Remote port pair, on the message thread. **Properties.** A `PropertyDelegate` serves ResourceList, DeviceInfo, ChannelList and the focused device's parameters from the feed. Subscriptions follow the focus. **Writes** are `user` + `midici:<muid>`.
- **Reuse:** SU4 (`juce_midi_ci`, already pinned; the demo's forwarder).
- **Reject or defer:** waiting for the OS.
- **Research IDs:** DR-11-09.
- **Unresolved test (NOT RUN):** SF-18; the resource names commercial controllers ask for, checked against a device.

#### SRF-14 Where a surface's page comes from

*refinement* · size M · evidence documented · lane win, engine, window · milestone P2 · after: SRF-07

- **Need:** A defined parameter page for every device on every surface.
- **Decision now:** ADR-0150 and ADR-0154: Live's Configure list is project state; ADR-0181 d2: page and bank requests; ADR-0188 d4; Dream §11: "ADI's controller pages work as Bitwig's"; ADR-0203 d2.
- **On main (audited at ca0290c2):** the plug-in panel with Configure is project state. Surfaces have no page concept.
- **Proposed contract:** **The default is Live's.** A plug-in's banks are its Configure list in banks of eight. A native device has its fixed banks. **His predefined controllers** take precedence for their plug-ins. **ADI's addition** (his Dream; win's placement, to strike if wrong): Bitwig-style named, editable pages. They are kept per instance in the project (a side table, per ADR-0144 d3) and per plug-in identity in app data. They never replace Live's banks. **One page model** for the Stream Deck, V-Pots, OSC's (its source file) and MIDI-CI.
- **Reuse:** none external. His 13 controllers are the installation-wide seed.
- **Reject or defer:** Bitwig's ninth slot, until a surface needs it.
- **Research IDs:** B3-011, B3-012, A6R-002, DR-11-03, DR-11-05, M3-CONTROLLER.
- **Unresolved test (NOT RUN):** M3-CONTROLLER item 4 (Bitwig's page persistence). A plug-in with 300 parameters pages through its Configure list. A renamed device keeps its pages.

#### WIN-02 Every window reopens where it was

*refinement* · size S · evidence documented · lane window, win · milestone M1 · after: none

- **Need:** Windows come back where they were, survive a lost or changed display, and keep their title bars reachable.
- **Decision now:** SPEC §8.4: MUST clamp restored windows; FEATURES l.264: restore plug-in windows with monitor identity, P1; ADR-0063 d2; ADR-0129 d3; ADR-0180 d3 and d7.
- **On main (audited at ca0290c2):** `install()` destroys and re-centres the main window on launch, New, Open and a forwarded file (`centreWithSize(1200, 720)`, `ui_main.cpp` l.176). It does not clamp, and it loses full screen. The Settings window is centred (`ui_settings.cpp` l.585). `restoreFloating`: pins a lost monitor's window to an edge of the main display; treats a monitor with a changed resolution or scale as lost; saves only at open and close. `window_state` has no writer.
- **Proposed contract:** **One placement rule:** ; sanitize; pick the monitor by the area shown; keep the title bar reachable; clamp; restore on the same monitor, proportionally when its size or scale changed, or on the main display at the same offset when it is gone; cascade. **Monitor identity:** id, then area, then origin, then a unique monitor of the same size and DPI, then the main display. **Where places live:** ; the main window and Settings: an App record `window.places`, holding `{display, area, rect, fullScreen}`; floating windows stay in `ui_view`, with the area added. **When places are saved:** on move (debounced), and re-checked on a display change. **A project open** keeps the main window where it is.
- **Reuse:** WI1: craft-libs' placement (8 tests) and place (7 tests), ported with a Grab of {18, 40} plus ADI's 26-point cases.
- **Reject or defer:** free docking and named workspaces or display profiles (ADR-0203 d5); a manual "gather windows" command (an addition, not proposed).
- **Research IDs:** DR-01-09, C5W-008, C5W-011, M3-UI-WORKSPACE.
- **Unresolved test (NOT RUN):** window-infra acceptance 7-16, including: at 1366×768 and 125 %, the title bar never opens above the screen; a moved floating window keeps its place after a kill. AWAITING: where Live reopens its windows, per install or per Set, and where a window goes when its monitor is gone.

#### WIN-03 Window kinds, owner windows and the plug-in windows

*refinement* · size M · evidence documented · lane window · milestone M1 · after: WIN-02

- **Need:** Plug-in and device windows stay with ADI. Live's plug-in window commands work. The future unified feature's device-like window has a kind to use.
- **Decision now:** ADR-0076; ADR-0180 d3 and d7; Live 12 manual §23.3 and §23.3.1.1; parity-7.5 rows 1, 2 and 11; N8 (struck, #391); the director's ruling of 5 November.
- **On main (audited at ca0290c2):** Each floating window is an unowned `DocumentWindow`. `createEditorComponent` (#391) exists, but nothing opens a plug-in window. Five `plugins.*` settings and `lookfeel.dialogPosition` have no reader. Esc does nothing in a floating window.
- **Proposed contract:** **Owned by the main window:** plug-in editors and ADI's device views. They stay above it, minimise with it, never sit above other applications, and have no taskbar button. **Normal windows:** the Mix View's window and the Second Window. **`plugins.editorsAlwaysOnTop`** chooses owned or normal. **Esc** closes the focused floating window, unless a plug-in consumes the key. **Live's plug-in window commands** (worklist D3): the wrench toggle and Ctrl/Cmd+Alt+P; Multiple, Auto-Open and Auto-Hide; the track's name in the title; cascading. **ADI never scales a plug-in window.** ; **Keys typed into a hosted native editor** never reach ADI. ~~The unified feature's window, when it exists, is an owned device view.~~ *[Struck (XC-27): a generic device-view window kind stays; the unified feature is not placed]*
- **Reuse:** WI2: MAGDA's owner windows (Win32, and macOS observers); Linux via `WM_TRANSIENT_FOR` and JUCE's X headers.
- **Reject or defer:** always-on-top as the Linux fallback.
- **Research IDs:** DR-01-10, A5W-001, A5W-002.
- **Unresolved test (NOT RUN):** Acceptance 17-21, driven on a non-director desktop. AWAITING: in Live, are plug-in windows above other applications, on the taskbar, closed by Esc?
- **Cross-check and this plan:** XC-27: the phrase placing the unified feature's window is struck; a generic device-view kind stays.

#### WIN-04 Stable command identity and user shortcuts that survive (first half)

*refinement* · size S · evidence documented · lane window, win · milestone M1 · after: WIN-01

- **Need:** Shortcuts that survive Settings changes, presets and new commands.
- **Decision now:** ADR-0180 d6 (command shortcuts are app-scoped); ADR-0125 R-25; ADR-0047's open item; UI-ARCHITECTURE §11.
- **On main (audited at ca0290c2):** `AppCommands::Id` is an ordinal enum. `reload()` re-registers the commands and calls `resetToDefaultMappings()` on every Settings change (`ui_shell.cpp` l.38-54, wired at `ui_main.cpp` l.299). All 77 commands have the category "ADI".
- **Proposed contract:** **Stable text ids** (`view.browser`, `edit.redo`, …), with categories from Live's menus. The list only grows. **The overrides record:** `shortcuts.overrides`, mapping an id to canonical chord texts (`[]` for none), re-applied after every `reload()`. It is compared before it is written. **A Shortcuts page** on JUCE's `KeyMappingEditorComponent`, in ADI's own look.
- **Reuse:** WI3 (MAGDA's `KeyMappingStore` and its 2 tests; plugdata's panel; JUCE's editor); WI7 (design).
- **Reject or defer:** persisting JUCE's numeric ids or per-OS key descriptions.
- **Research IDs:** C5W-010, C5W-015, C5W-016, A5W-009.
- **Unresolved test (NOT RUN):** user keys survive a Settings change; a preset switch keeps the overrides on top; every command has one stable id.

#### WIN-09 The Help menu and Live's menu search

*refinement* · size S · evidence documented · lane window · milestone M1 · after: none

- **Need:** Live's Help menu, and on the Mac the system's Cmd+? menu search.
- **Decision now:** ADR-0203 d1 and d5; Live 12 manual §41.28 and §2.2.1.
- **On main (audited at ca0290c2):** `{"Help", {}}` is declared, but `getMenuBarNames()` leaves out a menu with no items (`ui_shell.cpp` l.1827-1884). This is deliberate "until there is a Learn View", and a test checks it.
- **Proposed contract:** The Help menu is shown with at least the manual and keyboard-reference entries, so the Mac search works. The Help View (Learn View) entry joins it later.
- **Reuse:** none.
- **Reject or defer:** a lessons engine, for later.
- **Research IDs:** A-BASE-2.2.1, DR-01-01, DR-01-12.
- **Unresolved test (NOT RUN):** on the director's Mac, Cmd+? searches ADI's menus.

#### WTS-01 Never on the director's desktop: launch isolation and the testing ADR

*refinement* · size S · evidence documented · lane win, window · milestone M1 · after: none

- **Need:** Automated runs of `ADI.exe` that never touch the director's session, settings or focus.
- **Decision now:** ADR-0180 d8; ADR-0108 d4 and d5: his signed session closes a step; ADR-0149 d1: `ADI_HOME`; the state map lists "driving ADI.exe without the director's desktop" as not decided.
- **On main (audited at ca0290c2):** `ADI.exe` is single-instance across every Windows session, through JUCE's (its source file) mutex (`moreThanOneInstanceAllowed()` returns false at `ui_main.cpp` l.17). It forwards its command line to a running copy. It takes the focus, through `toFront(true)`. `report()` already writes to stderr in smoke runs.
- **Proposed contract:** **Test launches** (`--smoke-output`, `--control-test`): are multi-instance; require `ADI_HOME`; never activate their window (guarded in `AdiWindow`, the floating window, Settings and two direct calls); report to stderr; script their file choosers. **Where tests run:** ; the CI legs; the in-process runner; the Linux box under Xvfb; a Windows Sandbox on the director's Windows workstation, only with the director's OK; never his session. **Window-lane ownership** of the `ui.*` methods and their id grammar. **Bookkeeping:** strike ADR-0180's two settled items. **His signed side-by-side session stays.**
- **Reuse:** WT-5 (MAGDA's `windows_no_dialogs`); WT-4 (Craft focus rules, design).
- **Reject or defer:** `ADI.exe` on his live desktop; pywinauto for ADI (it stays for driving Live).
- **Research IDs:** CR-RUN-05, DR-01-12.
- **Unresolved test (NOT RUN):** AT-7: with an ordinary ADI running, a smoke starts its own instance, writes its PNG and exits 0; the running copy receives nothing; the foreground window is unchanged; his settings and projects are byte-identical; `--control-test` without `ADI_HOME` exits 2.

#### WTS-02 Test hygiene: the input clock, assertions, repaints and counting

*refinement* · size M · evidence documented · lane window · milestone M1 · after: WTS-01

- **Need:** Headless runs that read no physical keyboard, fail on an assertion, never hang on a dialog, and are counted.
- **Decision now:** ADR-0180 d8; ADR-0050; ADR-0209 d9; Live §42.25.
- **On main (audited at ca0290c2):** `keyIsDown` and `PlayReference` read `KeyPress::isKeyCurrentlyDown` (`ui_shell.cpp` l.15-20, l.1643). So on the director's Windows workstation the shell suite reads the director's keyboard: "Shift+R … a tap, latched" passes only while R is up. The JUCE suite builds outside `test_all.sh`'s glob. There is no assertion watch and no CRT guard.
- **Proposed contract:** **An `InputClock` seam** (key state and milliseconds) for `keyIsDown`, `keyStateChanged`, `AppCommands::key`, the Mix View's [REF] and Tap Tempo. **A fixed virtual display set** for runs. **AssertionWatch,** with `JUCE_LOG_ASSERTIONS` on the test targets only. **`windows_no_dialogs`** in every Windows test binary. **Painting checks:** ; repaint invariance (37-pixel strips, a tolerance of 4); an extreme-zoom paint; a paint census counting `paintDuration > 0` only. **Counting.** The JUCE suite is built where `test_all.sh` counts it, and `ADI_UI_TEST_FILTER` comes after the scenes are split.
- **Reuse:** WT-5 (MAGDA's AssertionWatch, repaint invariance and `windows_no_dialogs`; plugdata's dispatch-loop runner); WT-3 (the census).
- **Reject or defer:** a PUBLIC `JUCE_LOG_ASSERTIONS`.
- **Research IDs:** CR-RUN-05, DR-01-01.
- **Unresolved test (NOT RUN):** AT-4, AT-5, AT-6 (S held 600 ms springs back while a tap stays; Shift+R hold and tap; no OS key state read) and AT-8.

#### WTS-03 Goldens that check text: comparator, glyph masks and the font experiment

*refinement* · size M · evidence documented · lane window · milestone C · after: WTS-01

- **Need:** Goldens that check text too, and a comparator that reports what changed.
- **Decision now:** ADR-0180 d8; STEP-7-PLAN §5.1.
- **On main (audited at ca0290c2):** `reference()` (`test_ui_shell.cpp` l.88-250) compares exactly, under hand-placed masks over every text region. It writes no diff, and `--write-references` rewrites all eight goldens. The masks broke CI twice (#288, #315). With the text masked, the renders are identical on all three OSes. This is measured: CI shows it on every run.
- **Proposed contract:** **First, the font experiment.** One Latin and Hebrew OFL face is pinned in the test binary. Each scene is then recorded as identical, anti-alias only, or offset, per OS. **Then a `RecordingRenderer`** records each glyph's box, and stores `<name>.text.json` beside each golden. **The ignore mask** is the union of the current and golden boxes. **Comparison.** EXACT is a memcmp count. pixelmatch gives the diff image and the worst window. **Bless modes,** a failures folder, and a 64 KiB cap per golden. **The eight goldens** are re-recorded once, with their text.
- **Reuse:** WT-1 (pixelmatch-cpp v8.0.0, test-only); WT-6 (FilmCraft's golden testkit and egui kittest's update modes).
- **Reject or defer:** pixelmatch at threshold 0 as EXACT; hand masks.
- **Research IDs:** DR-01-11, DR-01-12.
- **Unresolved test (NOT RUN):** AT-1, AT-2 (a word moved 1 px passes while a divider moved 1 px fails) and AT-24.

#### WTS-04 The render review job

*refinement* · size M · evidence documented · lane window, win · milestone C · after: WTS-03

- **Need:** Every pixel a PR changes is seen on each OS, text included, and accepted by a person.
- **Decision now:** ADR-0108; LANES §2.
- **On main (audited at ca0290c2):** ui.yml uploads only `*-actual.png`. It has `contents: read` only, and cancels successive main runs.
- **Proposed contract:** **Uploads.** Every leg uploads its renders and a manifest. **Comparison.** A PR is compared per OS against its base's renders, found by the merge commit's first parent. If those are missing, the leg builds the base. **Reports:** ref, current and diff images, `report.html` and a summary table. **Gate.** The job fails until the PR carries `ui-renders-changed` and names the changed scenes. **Workflow changes:** `actions: read`, no `cancel-in-progress` on main, and the `labeled` trigger.
- **Reuse:** WT-2 (MuseScore's visual-test review, design; reg-suit's commit explorer, precedent).
- **Reject or defer:** comparing against the base branch tip; automatic acceptance.
- **Research IDs:** DR-01-12, SRC-TEXT-ONLY.
- **Unresolved test (NOT RUN):** AT-3: a colour change reports exactly its scenes; a PR that changes no pixels passes with "0 changed"; with the base artifact deleted, the leg builds the base and reaches the same result; #288's wrap shows as a diff.

#### WTS-05 `UiControl`: one description of the window, in process first

*refinement* · size M · evidence documented · lane window · milestone C · after: WTS-02

- **Need:** Tests, scripts, screen readers and agents find and drive a control the same way.
- **Decision now:** ADR-0180 d8; ADR-0047's open item (whatever replaces the inspector must be reachable by keyboard and announce itself); UI-ARCHITECTURE §6; ADR-0210 d3.
- **On main (audited at ca0290c2):** 48 `setTitle` calls, and clip accessibility children. `setComponentID` has 0 uses. The fake peer gives real dispatch in 3 places; most keys skip focus routing.
- **Proposed contract:** **Where:** `src/juce/ui_control.*`, holding the one melatonin copy. **Reading:** `ui.windows`, `ui.inspect`, `ui.elements` and `ui.lint`. **Input,** only through `ComponentPeer::handle*`: click, drag, move, scroll, magnify, `key {holdMs}`, text and drop. **Actions:** `ui.invoke`, `ui.command` and `ui.focus`. **Frames and pictures:** `ui.frames` and `ui.screenshot`. Native child windows are marked uncaptured. **The arrangement:** `ui.arrangement.hit` and `locate`. **Replies** come after the input is applied and one frame has run. A request whose client stopped waiting is dropped. Per-request step caps apply. **Ids** use the `componentID` grammar, or the title path. **The fake peer** moves to `ui_headless_peer.hpp` and records dirty rectangles. **The runner** runs inside the dispatch loop.
- **Reuse:** CA5 = WT-3 (melatonin's component model, copied with attribution); WT-4 (gridcraft and FilmCraft, design).
- **Reject or defer:** calling `mouseDown` directly; promising never to steal the keyboard on a shared desktop.
- **Research IDs:** CR-RUN-05, DR-01-12, DR-01-01.
- **Unresolved test (NOT RUN):** AT-9 to AT-12 (stable ids; `ui.click` on Play equals Space; a drag is one labelled undo step; a dropped WAV is one Insert Audio Clip step).

#### WTS-06 The headless script runner

*refinement* · size S · evidence documented · lane window · milestone C · after: WTS-05

- **Need:** Repeatable, scripted sessions with pictures, on any machine.
- **Decision now:** ADR-0180 d8; ADR-0108.
- **On main (audited at ca0290c2):** nothing. The only driven session was #276's manual Xvfb and xdotool run.
- **Proposed contract:** **The runner:** `adi_ui_script (--project | --new) script.jsonl`, over the fake peer, with no display or audio device, and (its source file). **Script lines:** requests, shots, frames, expectations and notes. It writes PNGs and `transcript.jsonl`. **Failure:** exit 1 on a failed expectation. **Where:** ui.yml runs it on three legs. **Tags:** `[P:…]` tags in the scripts.
- **Reuse:** WT-4 (soundcraft's `ui_shot.rs`; FilmCraft's Driver; muse testflow, design).
- **Reject or defer:** a queue with synchronous input; a per-request step cap stands in for it.
- **Research IDs:** CR-RUN-05, DR-01-01.
- **Unresolved test (NOT RUN):** AT-13.

#### WTS-07 Sweeps, and the director's remarks as regressions

*refinement* · size M · evidence documented · lane window · milestone P2 · after: WTS-05, WTS-06

- **Need:** No control that does nothing and says nothing (his R1 and R6), and no regression of his remarks.
- **Decision now:** ADR-0108 d1-d4; the director's R1-R7 (`collab/director.md`, build add5102); ADR-0209 d11.
- **On main (audited at ca0290c2):** there is no sweep. His build is 41 window PRs behind main, and his remarks are unchecked against their fixes.
- **Proposed contract:** **The dead-control sweep.** Each click must produce one of: a new history head, a view-state change, a popup, a transport command or a Status Bar line. Undoing every step returns the projection to its first form. **The round-trip sweep and dialog coverage,** with no native chooser. **Layout invariants** at three sizes and three zooms. **The Mix View's transitions** as a filmstrip (R3). **A Hebrew-named track** drawn right to left. **Scripts** for R1-R6.
- **Reuse:** WT-5 (plugdata's click-through, coverage and dialog tests; MAGDA's repaint invariance).
- **Reject or defer:** building on add5102; revert #372 on a scratch branch instead.
- **Research IDs:** CR-RUN-05, DR-01-03, DR-01-11.
- **Unresolved test (NOT RUN):** AT-14 to AT-18. Reverting #372 must fail R1's triangle and R6's buttons.

#### WTS-08 Accessibility lint and the Windows UIA scan

*refinement* · size M · evidence documented · lane window · milestone P2 · after: WTS-03, WTS-05

- **Need:** Every control is named, reachable and unclipped, for screen readers, tests and agents alike.
- **Decision now:** ADR-0047 (its accessibility item); ADR-0125 d4; UI-ARCHITECTURE §6.
- **On main (audited at ca0290c2):** 48 titled controls and clip children. Nothing checks that every control is named.
- **Proposed contract:** **`ui.lint` rules:** unnamed, obscured, zero-size, outside its parent, duplicate id, clipped text. **axe-windows' UIA rules,** ported where they map, and its CLI scanning `ADI.exe` on windows-latest. **A floor that only falls** until the count reaches zero. **Linux** runs the in-process rules only, because JUCE has no Linux accessibility backend.
- **Reuse:** WT-3 (melatonin; egui kittest's unnamed-widgets rule; MAGDA's obscured check; axe-windows).
- **Reject or defer:** out-of-process UIA as the main path.
- **Research IDs:** DR-01-11, DR-01-12.
- **Unresolved test (NOT RUN):** AT-12; the axe-windows CLI scan comes back clean.

#### WTS-09 Parity bookkeeping that cannot drift

*refinement* · size S · evidence documented · lane win, window · milestone M1 · after: none

- **Need:** Every built parity row points at a check.
- **Decision now:** ADR-0108 d1.
- **On main (audited at ca0290c2):** eight checklists with 159 rows (106 built). No check message cites a row.
- **Proposed contract:** **`tools/validate_parity.py`,** run in ci.yml's "every validator" step: row ids match (its source file); status words are normalised; every `[P:<file>#<row>]` tag resolves; the count of untagged built rows only falls; a file's built count never drops without a floor change. **Optionally,** a menu catalogue compared with Live's captures.
- **Reuse:** WT-4 (soundcraft's parity floor, design).
- **Reject or defer:** parity claimed from manual text.
- **Research IDs:** SRC-TEXT-ONLY, CR-RUN-05.
- **Unresolved test (NOT RUN):** AT-19.

#### WTS-10 The running app over the API, smokes everywhere, other desktops

*refinement* · size S · evidence documented · lane window · milestone C · after: CTL-09, WTS-01, WTS-05

- **Need:** Drive the real `ADI.exe` on every OS without xdotool and without the director's desktop.
- **Decision now:** ADR-0180 d8; ADR-0181 d2; ADR-0039 d4.
- **On main (audited at ca0290c2):** ui.yml runs the native smoke on Windows only. The macOS and Linux legs never launch `ADI.exe`.
- **Proposed contract:** **The `ui` grant** exists only under `--control-test` (multi-instance, `ADI_HOME`, a banner while on), never for a paired surface. **Synthetic ops** carry `ui-test:<client>`. **Paths.** Screenshots and drops are confined to declared roots. **Native smokes** on Linux (`xvfb-run`) and macOS too. **An the Linux box runbook,** driven over loopback from SSH. **Optionally,** a PowerShell UIA check that invokes Play.
- **Reuse:** CA5 (shared with WTS-05); CA4 (design).
- **Reject or defer:** xdotool and ImageMagick sessions.
- **Research IDs:** CR-RUN-05, DR-01-09.
- **Unresolved test (NOT RUN):** AT-20 to AT-22 (= AC-21 and AC-22). #276's Alt+E and Alt+N checks, and the `root-default` golden, are reproduced through the protocol on the Linux box.

#### CTL-05 Where control-API secrets live (CH-2)

*new-choice (the first draft made it win's placement, XC-44; this revision restores the director's question, Q-O)* · size S · evidence documented · lane director, win, engine, Stream Deck seat · milestone C · after: CTL-01, CTL-03

- **Need:** Local tools (`adi-mcp`, scripts) find the endpoint and the token, and paired surfaces keep their own, with nothing in the `.adi`.
- **Decision now:** ADR-0039 d4 (a per-session token shown in the UI); ADR-0181 d2 (a surface's token in the OS keychain); ADR-0168 d3; ADR-0149.
- **On main (audited at ca0290c2):** nothing. `agent.endpoint`'s help says its key is kept in the OS keychain (`registry.cpp` l.637), but no keychain code exists.
- **Proposed contract:** **The session token:** ; 256 bits from the OS's secure random source; shown masked; written to an owner-only discovery record in a non-roaming folder (`%LOCALAPPDATA%` or `$XDG_RUNTIME_DIR`); deleted on exit, and swept when stale; granting at most Propose. **Pairing:** ; a single-use code for 120 s; five wrong tries close pairing; the token is returned once; ADI keeps only its BLAKE3 hash. **The client** keeps its token in the OS keychain. The Stream Deck uses a small prebuilt N-API helper, a pattern his other tools already use. **Never** in the `.adi` or `settings.json`.
- **Reuse:** CA1 (MAGDA's discovery record and rotation); CA4 (wordcraft's control key); SU2 (the prebuilt N-API pattern).
- **Reject or defer:** a tokenless local mode; a roaming record; Elgato's settings store (option c).
- **Research IDs:** DR-11-04, DR-14-15.
- **Unresolved test (NOT RUN):** AC-18 and AC-19. A crash's stale record is swept at the next launch. Another Windows user cannot read the record.
- **Cross-check and this plan:** XC-24: the record lives in PD-05's marker. The first draft recorded option (a) as win's placement, on the ground that it amends neither ADR-0039 d4 nor ADR-0181 d2; the critique of 5 November showed that a discovery file any process of the same user can read turns a token "shown in the UI" into an ambient one, close in effect to the tokenless local mode d4 and AI-AGENT §7.1 explicitly reject. That is a security-posture change to his ruling, so the row is his question again (Q-O), with (c), endpoint only in the file and every client paired once, as the recommendation; the contract above is option (a) as the first draft wrote it.

#### SRF-15 Push 2 and Push 3 (CH-1)

*new-choice* · size XL · evidence documented · lane engine, window · milestone P2 · after: SRF-08

- **Need:** Ableton's own controller works with ADI.
- **Decision now:** ADR-0125 R-14 (SETTINGS-CATALOGUE l.104: "Push support is Ableton's and not assumed"); Dream §11 ("Push and the common surfaces working on day one"); ADR-0203 d1.
- **On main (audited at ca0290c2):** nothing.
- **Proposed contract:** A Push 2 and Push 3 (controller mode) driver behind SRF-08's host. Built from Ardour's display transport (libusb) and pad layouts, with DrivenByMoss's behaviour. Live's Push behaviour measured (the A6U rows). P2, after the MCU and profile host and Session View. Either way, Push User Mode works as a generic MIDI surface (A6U-013).
- **Reuse:** no ledger take. Ardour's (its source file) and DrivenByMoss's (its source file) are usable but not planned (surfaces §2.8).
- **Reject or defer:** Ableton's push-interface document (not usable); Live's Remote Scripts (not usable).
- **Research IDs:** A6U-001, A6U-002, A6U-013, A6R-014, DR-11-03.
- **Unresolved test (NOT RUN):** Push 2's display and pads driven from ADI on his unit; Live 12.4's Push behaviour captured (A6U-004 to 012).

#### WIN-10 Palette free text: commands first, or straight to the agent (CH-4)

*new-choice* · size S · evidence inferred · lane window · milestone P2 · after: WIN-06, AGT-05

- **Need:** What happens to unprefixed text typed into the palette.
- **Decision now:** ADR-0104 d3: "free-text tasks routed to the agent at its tier"; ADR-0203 d6; AI-AGENT §8: model-backed affordances, not a chatbot; ADR-0064 d6; Dream §2: "a few letters load a synth … or turn a sentence into a task".
- **On main (audited at ca0290c2):** no palette; an empty AI tab.
- **Proposed contract:** Free text ranks local matches first (commands, devices, and later plug-ins, samples and presets). Enter runs the top one, offline. The last row is always "Ask the agent: <text>". It acts only when chosen, at the agent's tier and under consent. The prefixes stay as ADR-0104 d3 has them. With the agent off, the row is inert and says so.
- **Reuse:** WI5 (Helio's root list advertising its prefixes); WI6.
- **Reject or defer:** sending every unmatched string to a model.
- **Research IDs:** DR-14-03.
- **Unresolved test (NOT RUN):** In an experiment build, "hsb" runs Hide/Show Browser. "Make the drums tighter" shows the agent row last and sends nothing until it is chosen. With consent off, the agent row asks for it.


---

## Appendix B. Sources

- **The research.** The research seat's bundle of 4 November (milestone 11): its handoff, errata,
  design implications, engine contracts, open questions, research matrix, host coverage, the Pd
  and rack fit note, the inventory and the missions files. Every behaviour it documents is cited
  here by its research ID; none has been measured in a running DAW. The per-app notes are on main
  as `docs/daws/*.md` (#436, #437).
- **Win's digests and maps** of the bundle, of ADI's ADRs and of ADI's state per lane (working
  files).
- **The planning files** (working files, not in the repository): the harvest ledger (208 takes,
  summarised in `docs/RESEARCH-INTAKE.md`), the Mix View program, the six area matrices (233 rows)
  and the cross-check (41 conflicts, the milestones, every row's place).
- **ADI's governing texts** on main: `docs/DECISIONS.md` (ADR-0001 to ADR-0211), `docs/FEATURES.md`,
  `docs/LANES.md`, `docs/BUILD-BRIEF.md`, `docs/UI-ARCHITECTURE.md`, `docs/EXTERNAL-CODE.md`,
  `docs/AI-AGENT.md`; `collab/window.md` (the needs table of 5 November) and
  `collab/window/worklist-2026-11-05.md`; `collab/director.md`; `collab/engine.md` on (its source file)
  (`132486fc`); `collab/devices.md`.
- **The director's intent:** the Dream Reference v0.2 text, `reference/DOCS/WORD/_build/build_dream.py`.
- **GitHub, read only:** the open PRs (#207 to #457) with their bases, #461's merge at `5a6ae08b`
  and the runs at that SHA (42012235250, 42012235165).
- **Installed programs, read only:** Cubase 15.0.30.287 and Bitwig Studio 6.1.0.22652 on the
  director's Windows workstation; the research's inventory of the second Windows machine (Live
  12.4, Cubase 15.0.5.114, Bitwig 6.1, REAPER 7.79, Reason 14.1).

## Appendix C. The critique of 5 November: what this revision applied, and what it did not take

The critique raised seventeen issues (six major, eleven minor). Every major and every minor was
applied; where an issue offered two fixes, the one taken is named, and the part not taken says
why. The revision was made by hand on the plan, the six matrices and the choices file (win's working
files); the scripts that first assembled the plan were not rerun.

| # | Issue | Applied | Not taken, and why |
|---|---|---|---|
| 1 (major) | The 5 November ruling supersedes three recorded rulings, but no DECISIONS entry was planned | A short record ADR in his words joins R-a (M0), marked as amending ADR-0203 d14, ADR-0204 d1 and ADR-0209 d11; listed in §8.4 under DECISIONS; told to him as P-6 to strike | Not numbered here (LANES §2: numbers from win only, when it lands) |
| 2 (major) | The within-authority ADRs were seven in M0 and seventeen in §8.4, and approval's meaning was undisclosed | One list of seventeen in R-d, §8.4 and P-8; §0's choices paragraph and "What I did not do" say that approving the plan approves that list | None |
| 3 (major) | CTL-05's discovery file changes ADR-0039 d4's token model, yet was a placement | Restored as Q-O (§6.3) with the tradeoff and AI-AGENT §7.1 cited; CTL-05 back to a new choice; recommendation changed to (c), endpoint only in the file | The alternative (keeping P-6 with a disclosure) was not taken: the plan's own rule is that a change to his ruling is his |
| 4 (minor) | Q-K1 understated how many rulings route A reopens | ADR-0203 d8 and ADR-0204 d5 added to Q-K1's tradeoff, DEV-11's "Decision now" and the §3 row; the recommendation stands | None |
| 5 (minor) | P-5 advanced ADR-0188 d7's P2 item and defaulted an experimental backend | Subsumed by issue 9: the backend is off at M3 and SRF-16 carries it at P2 | None |
| 6 (minor) | LANES text finished the Link consumer first, amending ADR-0210 d2 silently | XC-30 and LANES §3 item 1 reworded as a dated one-time exception; told to him as P-9 | None |
| 7 (minor) | CT-01 was classed decided while superseding ADR-0037 and owing an ADR | Reclassified as a refinement; counts corrected in §0, §3 and the group headers | None |
| 8 (minor) | Q-A1's premise overstated the defect and asserted best-effort compaction for every answer | Premise restated to named snapshots and branch points; the daily snapshot follows d13 under every option; best-effort compaction is P-10, to strike; HI-02's card marks main's handling of the daily snapshot as a defect inside its authority | None |
| 9 (major) | Windows MIDI Services on at M3 is not feasible (Windows 11 Canary, SDK 10.0.20348, a NuGet package outside the official feed, MSVC only, an installer prerequisite; his box is Windows 10) | SRF-05, XC-18, P-5, TM-LS-03 and the LANES texts reworded: JUCE's UMP API with the service off, the Win32 backend named in Settings; a new row SRF-16 at P2 carries the SDK pin, the local NuGet repository on the Windows legs, the installer prerequisite and the Windows 11 seat; P-5 tells him it cannot be tested on Windows 10 | None |
| 10 (major) | TU-01's gate closed in M0 on a stack that plays 31-EDO 9.677 cents flat and refuses valid sources | The merges stay in M0 with a guard on #443's branch (a source whose reference pitch the stack cannot honour is refused with a verdict, so nothing plays flat); the gate moves to M6 beside TU-02; §0's table and "Every row's place" say so | Landing the reference-pitch store before the merge was not taken: it needs a table (ADR-0144 d3: a minor adds whole tables only), which is TU-02's minor 1.20 work, not a small change on an open branch |
| 11 (minor) | The merge order did not match the PRs' bases | M0 item 4 states the mechanics once (#421 first; a stack lands through its top PR retargeted to main, never engine/log; siblings retargeted one by one) and the orders for the tuning, scanner, Link, media and capture lines; the header lists every base | None |
| 12 (minor) | The repository state was stale by one merge | PD-00 recorded as closed at `5a6ae08b` (run 42012235250, all 20 jobs green); the dispatch step dropped; M0 item 1 keeps WTS-04 alone; §1.7 and the header say main's `src/` and `tests/` differ from `ca0290c2` by #461 alone | The "audited at ca0290c2" lines on every card stay, since that is where they were read |
| 13 (minor) | M3's "small pieces" did not match its §4 load | M3's "Who" names the load; SU1's encoders and binding model and SU9 move to P2's surfaces step (SU1's device-identity match alone stays in M3 for SRF-05); §2 and §4 agree | MD1 (the MOV and ALAC demuxer) stays in M3: §0 promises MOV and ALAC at that step and TM-MD-01's gate is M3; moving it to M5 would change a promise to him rather than a lane's load. It is named as the first item to slip |
| 14 (minor) | Lane capacity was asserted, never estimated | §2 carries the measured rate (81 "PR requested" entries over 30 October to 5 November, about 16 a day, with win's fixes and the stack rebuilds behind them) and every milestone a load line: expected engine PRs, the assumed net rate of 10 a day, and the item that slips first | Dates are still not given: the milestones are an order and a throughput, and a date would be a guess |
| 15 (minor) | Audio-to-MIDI (Convert Harmony, Melody and Drums) was missing | A FEATURES row (§8.2, P2) and a matrix row TM-WT-09 beside TM-WT-08: Drums on the cached onsets, the pitch tracker a Q-D candidate, Live's cases in §7.1's P2 readings | None |
| 16 (minor) | MIDI Arrangement Overdub was missing from CT-03 | The overdub cases added to CT-03's contract (merge through note.applyDiff with new stored ids; replace with it off; loop on and off; one undo step) and to its NOT RUN readings and §7.1's M6 row | None |
| 17 (minor) | `.als` import was struck silently though it is the only road for his own Sets into ADI | Moved to §6.3 as Q-P (read-only import at P2 after DAWproject, with a loss report; the format's volatility as the tradeoff); §1.6 no longer strikes it | No row yet: a TM-IC row is added only if he says yes |

Found while applying the critique, and recorded as XC-46: the devices lane has been silent since
28 October while five milestones put gates on it; M0's gate now asks for the seat to be alive or
re-seated, R-b names the loudness and limiter loan to the engine seat, and the Pd overlays go to
the seat that is running.
