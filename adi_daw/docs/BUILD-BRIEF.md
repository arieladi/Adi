# The build brief: what is built, what the rulings change, and the order of work

**For whoever builds next**: the orchestrator and its agents, a new session, a returning one. Read
this first, then the three documents it points at. It is derived from the decision log (ADR-0001
to ADR-0203), `docs/FEATURES.md` and the roadmap in `README.md`; where it and they disagree, they
win and this file is corrected. Written by win on 2026-10-12, the day the director's review of
Master Reference v1.0 closed (ADR-0203). Dated statements of "built" and "not built" are read off
`main` at `cf20417` plus the two PRs of that day (`win/v1.0-rulings`, `win/v1.0-settings`).

## 0. The one-paragraph brief

ADI is an arrangement-first DAW in C++ with JUCE, AGPLv3, whose project is a SQLite file of
undoable ops (`.adi`), whose devices are Pure Data patches and native DSP, and whose parity base
is **Ableton Live**: Live's layout, gestures and defaults, with the editing and mixing depth of
Cubase and a short list of things no DAW has (a built-in scope that compares any two tracks, the
Loudness Drive, projects that stay local with branching history, collaboration through the user's
own storage, an AI agent that works through the same ops). The format, the op vocabulary, the store,
the engine, both plug-in hosts, the Settings registry and most of Live's devices are built and
tested headless (7,826 checks in 95 suites on 7 ABIs). **No window exists yet.** The next things,
in order: make sends and returns play (closes step 6); the first window, purely as Live (step 7);
the P0 editing set; the P1 set; then the Mac and Linux builds immediately, then P2.

## 1. The documents, and which one wins

| Document | What it is | Wins over |
|---|---|---|
| `docs/DECISIONS.md` | the decision log, append-only; **ADR-0203** is the director's rulings of October 2026 and reaches everything | everything |
| `docs/review/v1.0-verdicts.md` | the director's verdicts in his words; evidence for ADR-0203 | ADR-0203's wording, where they differ |
| `docs/FEATURES.md` | the scope with priorities P0 to P3 and the format check per row | this file |
| `docs/format/SPEC.md`, `docs/OPS.md` | the file format and the op vocabulary | the code |
| `docs/STEP-7-PLAN.md`, `docs/UI-ARCHITECTURE.md` | the first window: build order, seams, tests, the parity gate | |
| `docs/SETTINGS.md`, `docs/SETTINGS-CATALOGUE.md` | every setting, generated from the registry | |
| `docs/AWAITING.md` | what only the director can measure or hear; rows 6 to 15 are the Live readings the rulings wait for | |
| `collab/README.md` | the rules of work: roster, branches, claims, ADR reservations, the build, what "done" means | |
| `OPEN_SOURCE_POLICY.md` (repo root) | licensing, closed: §4a | |

The Master Reference document (Word, the director's) is outside the repository and is built from
these; it never contradicts them on purpose.

## 2. The rules every agent works under

1. **The standing rule (ADR-0203 d1).** Always the Ableton way unless Live lacks the ability, in
   which case ADI adds. Never do something Live does in a different way. Read Live's behaviour in
   the manual text (`reference/DOCS/_text/Ableton_live12/`, 41 chapter files) or measure it; never
   recall it from memory. Label every measurement with the Live version (the director's Mac has
   Live 11; Live 12 is coming to his PC).
2. **Additions are first-class (ADR-0203 d2).** An ability Live lacks is shown where a user looks
   for it, never hidden in Settings. Only the *default* of something Live already does stays Live's.
3. **The director's deviations stay (ADR-0203 d12):** import with no warp; renders placed with
   Warp off; Ctrl+S = Take Snapshot; tooltips on; MIDI record quantize 1/16; import and bounce
   prompt; function keys do functions; Create Fades on Clip Edges off. Never undo one because of
   rule 1. Confirm a new deviation with him by name.
4. **Ask the director only** for a yes and a priority on an addition, or when Live's way would undo
   something he directed. Everything else is decided: look it up.
5. **Licensing is closed (ADR-0203 d25; `OPEN_SOURCE_POLICY.md` §4a).** ADI is AGPLv3, all of it.
   Any open-source code may be used; keep notices, name the source, add the row to
   `docs/EXTERNAL-CODE.md`. Refuse only non-commercial-only and proprietary code (and GPL-2.0-only),
   silently, as "not usable". **Never raise licensing with the director.**
6. **The parity gate (ADR-0108, STEP-7-PLAN §6).** From step 7 on, a piece is "verified", not
   "done", when its checklist has been run side by side against Live and the director signed the
   session. Every deviation is a defect unless he approved it (dated) or an ADR rejects it.
7. **No added delay anywhere (ADR-0203 d4).** If Live adds none, ADI adds none. A bypassed device
   keeps its delay in the compensation. The one exception is a plug-in sent to AudioGridder.
8. **The collab rules (`collab/README.md`):** never commit to `main`; branch per task with the
   agent's prefix; a worktree per task, never the main checkout; `git add` explicit paths only; ADR
   numbers only from the reservation table, through win; logs newest-first in the agent's own file;
   never edit the README's "N checks across M suites" line (win measures `main` after merges);
   never touch another agent's claim rows. The MSVC `/WX` build and `tools/test_all.sh` green are
   the bar; prove a new check can fail.
9. **Docs over code.** If the code disagrees with SPEC, OPS or an ADR, one of them is a bug; fix
   that one and say which. A decision is a new ADR, never an edit of an old one.

## 3. Toolchain and build

- **Windows first.** VS 2022 (MSVC 19.44), CMake 3.31, Ninja; JUCE 9.0.2 (`third_party/JUCE`,
  AGPL); libpd; the VST3 and CLAP SDKs; the pinned externals of `docs/EXTERNAL-CODE.md`, fetched by
  `tools/fetch_external.sh`. Build: `adi_daw\tools\build.bat werror` (run with
  `NoDefaultCurrentDirectoryInExePath` unset); test: `bash adi_daw/tools/test_all.sh`. Keep paths
  short (JUCE). CI builds seven ABIs including macOS and Linux on every PR.
- **The Mac and Linux versions are built immediately after the Windows version works, and alongside
  it wherever that is easier (ADR-0203 d23).** Not "after the suite ships".
- Python 3 with `python-docx`, `pypdf`, `Pillow` for the document tools; a local LLM (qwen2.5-coder
  on the drone) for mechanical edits only, never review.

## 4. What is built (read off `main`, 2026-10-12)

| Area | State | Where |
|---|---|---|
| The `.adi` format, schema 1.8, 51 tables; five layers; older files upgrade in place | built, validated by two schema checkers | `docs/format/`, `src/adi/store*.cpp`, `tools/validate_schema.py` |
| The op vocabulary: 170 ops designed, 76 playing; undo, branches, snapshots, digests | built | `docs/OPS.md`, `src/adi/ops*.cpp`, `tools/validate_ops.py` |
| The store, the op log, history as a snapshot tree, text export, remarks (anchors only) | built | `src/adi/store*.cpp`, `history.cpp`, `textproj*.cpp` |
| The engine: graph, snapshot handoff, transport, delay compensation (8,192 samples headroom), suspension of silent nodes, track delay ±1 s, Live's pan law and three more, mixer automation and device automation playing, group analog summing (8 flavours), Auto Gain Stage | built, headless | `src/adi/engine/`, `docs/BENCHMARKS.md` |
| VST3 and CLAP hosting, plug-in parameter ops (one undo step per gesture), the plug-in panel contract, MPE routing (VST3), missing-plug-in placeholders, `adi_play` | built | `src/juce/` |
| The Pure Data tier: libpd inside the engine, top-level patches, `[adi.param]`, `[adi.transport]`, `[adi.array]`, `[adi.sample]`, MIDI into patches | built | `src/juce/pd_*`, `pd/` |
| Live's devices: 15 audio effects as Pd devices (Auto Filter, Chorus-Ensemble, Delay, EQ Eight, Echo, Multiband Dynamics, Overdrive, Phaser-Flanger, Redux, Resonators, Reverb, Saturator, Shifter, Utility, Vocoder), bit-exact or measured against Live; the MIDI effects, OneShot (Simpler) and Sampler native; the two color-bass devices; the analyser | built, each with a test against Live's render | `pd/devices/`, `src/adi/dsp/`, `src/adi/pd_builtins/` |
| The plug-in line: Dynamic EQ (ZL Equalizer 2's DSP, Pro-Q 3's gestures), ADI RMSC, Smartelectronix ×11, ADI Airwindows (160 algorithms in 11 suites, auto gain) | built as CLAP | `plugins/` |
| The Settings registry (App, Project, Device scopes), the catalogue, bundles, Safe Mode, the Find box's matching | built, no window | `src/adi/settings/`, `docs/SETTINGS.md` |
| The library index (`.adi-volume-id`), the decoding cache, relink by content hash | built | `src/adi/library/`, `src/adi/media/` |
| The control API, the one parameter feed, surface clients (Stream Deck first), MIDI 2.0/MIDI-CI/OSCQuery/CV/TUIO/Link protocol design | designed; the feed built | ADR-0181, ADR-0184, ADR-0185 |
| The Windows virtual audio device (sysvad-derived, MS-PL under `drivers/` only) | built in CI, unsigned | `drivers/` |
| Step 7, the first window: 7.1 the seams (headless) merged; 7.2 `AdiRootComponent` + `TransportBar` begun | in progress, **no window yet** | `docs/STEP-7-PLAN.md` |
| ADiJ (the DJ application), ADI Mobile | designed, not started | ADR-0105/0133/0191, ADR-0200 |

## 5. What the rulings change in code that is already built

Do these as their phase comes; each is a small ADR-cited PR with a test that can fail.

| Change | Ruling | Where |
|---|---|---|
| Sends and returns play: the planner accepts send rows, compensates across them, allows the Live loop (a return's enabled send to a return or itself), refuses every other loop; Pre/Post per return; Enable Send; Solo in Place; the three tap points; sends into any track and a pan on a send (ADI's addition) | ADR-0203 d3 | `src/adi/engine/` (the planner), the `mixer.*` and routing ops in `src/adi/ops*.cpp` |
| A bypassed device is not calculated and keeps its delay as a plain delay line | d4 (amends ADR-0057 d6) | the graph's device node |
| The Pd adapter's fixed 64-sample block removed for the built-ins; user patches in 64-multiples run without it; remainders reported | d4 (ADR-0183 d12) | `src/juce/pd_*` |
| The master track summed (console flavours) beside groups; a Crosstalk model (P2) | d14 (amends ADR-0173 d4) | `group_summing`, the ops that refuse non-group tracks |
| Chase MIDI Notes as a switch (on); Reduced Latency When Monitoring; the recording threshold gated by its new switch (off) | d21 | the engine's chase and compensation paths; registry keys landed in `win/v1.0-settings` |
| Zoom around the selection by default; the device strip's minimum height is Live's (169 units, to be measured) | d7 | step 7 code, `lookfeel.zoomOnSelection` |
| History caps: 2000 steps or 1000 MB, compaction never past a named snapshot or a branch point; a daily snapshot; 10,000 snapshots | d13 | `src/adi/history.cpp` |
| Ctrl+S = Take Snapshot; Ctrl+Shift+S = Save As (a new file, carry on in it); Save a Copy; Collect All and Save; the ZIP export unchanged | d6, d13 | file commands (step 7) |
| `project.sampleRate` default 44.1 kHz | d18 | the next schema minor (`docs/format/`) |
| SPDX headers of ADI's own files to `AGPL-3.0-or-later`, leaving every third-party-provenance file alone | d25 | a mechanical PR; `docs/EXTERNAL-CODE.md` is the list to respect |
| Random MIDI effects: Live's behaviour by default, a "keep this result" switch that stores the seed | d7 | Random, Chord, Velocity devices |
| The learned-tone rule for Color Cab (keeps its tone when the reference file is missing) | d8 | `src/adi/dsp/colorcab.*` |
| `agent.asyncServices` = Built-in by default; the stem model in its own process | d8 | the agent's async services; `tools/` for the model download |

## 6. The order of work

Each phase ends with: `/WX` clean, `test_all.sh` green, CI green on 7 ABIs, the new checks proved
able to fail, the agent's log entry, and from step 7 on the parity checklist run against Live and
signed. win measures `main` and updates the README count after each merge.

### Phase A — close step 6: sends and returns play (P0)

1. The planner: send rows planned, delay-compensated, the Live loop allowed, all other loops refused
   (test: a ring of two returns is refused; a return's self-send plays with measured latency).
2. Pre/Post per return; Enable Send / Enable All Sends; a new send fully down (until measured).
3. Solo in Place and the return rules; Solo/Cue, Cue Out, Cue Volume; the metronome to Cue Out.
4. Tap points Pre FX / Post FX / Post Mixer on sidechain and track-to-track routes.
5. ADI's sends into any track and a pan on a send; the converted-project test reversed to "plays".
6. A send tapped before the console colouring on summed groups.
7. The master summed (d14); Crosstalk is P2 and waits.

### Phase B — step 7, the first window, purely as Live (P0)

Follow `docs/STEP-7-PLAN.md` parts 7.2 to 7.6 with these rulings applied as they are met:
- keys: Tab switches views, F3 Session View, F7 offline processing, Ctrl+P the palette, Ctrl+I
  Live's inserts, Ctrl+S Take Snapshot, Ctrl+Shift+S Save As, Ctrl+Alt+F Create Fade;
- zoom around the selection; per-window interface zoom; the device strip's fixed minimum; it
  grows upward only; no touch-only paths (they wait for ADI Mobile);
- the Info View as Live's, tooltips on at 600 ms, remark marks; the AI chat a collapsed tab at the
  foot of the browser; the Status Bar, where a failing plug-in and a failed rebuild are named;
- the mixer strip (7.6) with send knobs and return strips, Split Stereo Pan, the loudness meter on
  the master only, the small scope and spectrum in each strip (drawn only while visible), the
  summing dial and flavour menu on groups and the master;
- full right-click menus, hover states and modifiers as Live; the side swap optional, Live's sides
  the default; the Arrangement is the opening screen.

### Phase C — the P0 editing set

Time-range selection as Live; Live's fade handles (length and curve, one fade for all selected);
tempo detection from audio with warping (Warp on a clean import = Live's Auto-Warp, markers on
each bar); Insert Silence; clip loop windows; the note editor's five actions already built, then
quantize, transpose and controller lanes; Live's Collect All and Save; the import prompt (audio,
video) and the bounce prompt with "don't show this again".

### Phase D — the P1 set (the first release anyone would use)

In this order unless a dependency says otherwise:
1. Automation shapes, exactly Live's set; Live's Automation Arm; clip envelopes play; send levels
   automated like a fader.
2. Capture: MIDI as Live and the 30-second audio memory.
3. Scale Mode exactly as Live 12, with the note editor; tunings and a tuning per key change.
4. Multi-threaded graph processing (ADR-0102 d4).
5. The plug-in scanner out of process, always (R-17, D-03).
6. Stem separation built in: the 4-stem model in the installer, its own process; services for N
   stems; the model's licence under the two refusals.
7. The Loudness Drive (VCA over the leaf tracks' input gain, the whole cascade), with export LUFS
   targeting (default no normalising) and the Export settings page.
8. Freeze (track; Freeze All Channels; Freeze Group with its warning), Bounce in Place, Bounce to
   New Track; renders placed with Warp off; freeze fingerprint.
9. Save As, Save a Copy; the history window as Live's Undo History plus branches and snapshots;
   the caps of d13; the daily snapshot.
10. Every requested file type imports and converts (AAC/M4A included); a file plays before its
    decoding finishes; the "analysed" folder; warp markers kept with a sample in the library.
11. Ableton Link (the SDK, GPL-2.0-or-later); Keep Monitoring Latency; the Delay Compensation and
    Reduced Latency switches; Track Delay's selector as Live's.
12. Live's remaining devices at P1 (the MIDI effects' parity checks, OneShot), the random "keep
    result" switch, Color Cab's learned-tone rule.
13. The Settings window with Live's ten tabs and order, ADI's pages after, the Export page, no New
    track page, the Find box with related words, Device-scoped marks.
14. AudioGridder with its fallback (the one delay exception).
15. System audio as a track input; the loopback channel.
16. The scope (ADR-0167) as an editor-style window, with the strip mini-views; the analyser's
    windows (ADR-0183), PerceptoMap ported directly now that licensing allows it.
17. Range commands (cut, copy, delete, silence); ripple edit; take lanes and comping; the hitpoint
    detector serving slicing, Audio Alignment (P2) and tempo detection.
18. Templates in the User Library; Collect and Export to ZIP (built) beside Collect All and Save.

### Phase E — Mac and Linux, immediately (ADR-0203 d23)

As soon as Phase B runs on Windows: CoreAudio and the macOS bundle (Gatekeeper, codesign); ALSA /
PipeWire, LV2 at P2, Wayland/X11, packaging. Wherever a piece is easier to build for all three at
once (the audio device layer, the installer), do it then, not later.

### Phase F — the P2 set

Session View (docked, detachable) with the crossfader and the launch settings; racks and macros as
adi_multimapper (curves, N:1 summing with a cap); external control surfaces (Stream Deck first) on
the one feed; remarks as Word's comments with a panel; collaboration through the user's bucket
(the op stream, 100,000-snapshot cap); the render matrix; Crosstalk; the DJ Filter device (after
reading rekordbox's options); Cubase's fade editor on double-click; step input, Humanize/Randomize,
Live 12's MIDI Tools; auto gain-match for any plug-in; the `.amxd` translator; the LAN audio node
(SonoBus); MIDI clock and timecode sync as Live; Save as Default Audio/MIDI Track; settings profiles
and the change log; the Scripts page with the Scripting API; the cloud backup command; the
sub-sample nudge with Audio Alignment; the ADI virtual audio device; VariAudio-class editing is P3.

### Phase G — P3 and ADI Mobile

The pattern editor; Expression Maps and the Logical Editor; ARA; chord track; video sync; Sound
Designer II; then ADiJ (step 13) and ADI Mobile (step 15, ADR-0200).

## 7. What is measured before a text calls it finished

`docs/AWAITING.md` rows 6 to 15: the device strip's height; nine send and return readings; Track
Delay's range; Shifter's latency; Auto Filter side by side (Live 12); plug-in failure and the
Configure-mode warning; automation after save and the curve's shape; Save As / Save a Copy; MIDI
file import and export; fresh-install defaults (fades, driver order, takeover, record quantize,
chase, the Settings tabs, the theme); the editing behaviours that follow Live once measured. A
registry default that says "once measured" changes on the reading, as an amendment, without asking.

## 8. Open points that are not blockers

- Live 12 on the director's PC (for the Live 12-only readings); his Mac has Live 11.
- The stem model's choice and licence (the two refusals apply; no question for the director).
- The schema minor that changes `project.sampleRate`'s default to 44.1 kHz.
- The mechanical SPDX header PR (d25).
- The README's test count after each merge (win's).
- `adi_changeset_tests`' preview bound, raised in `win/v1.0-settings`.
- The Vital fork's folder (`adi-vst/vital` → `adi-vital/vital`), outside this project.
- The director's macro device file, for matching D-06's summing exactly.

## 9. What the orchestrator must not do

Commit to `main`; use the main checkout (`D:\Documents\GitHub\Adi`) for work; edit the README's
count line, another agent's log or claim rows; take an ADR number outside the reservation table;
change a licence or ask the director about one; publish anything under `AdiGuard/` or `adi-vst/`;
undo a listed deviation because Live differs; hide an addition in Settings; call a step "done" that
the director has not signed as verified; add delay anywhere Live has none.
