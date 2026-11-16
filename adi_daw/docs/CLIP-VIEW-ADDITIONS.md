# The Clip View beyond Live: what other DAWs add

**Status:** a decision list for the director, written by win on 16 November 2026 after his word that
day: keep building the Clip View, and do not forget what Cubase, Bitwig and Reason have that Live
lacks (audio pitch editing, Cubase's editor tools, modulation, MPE and per-note expression). Nothing
in sections 3 and 4 is built until he rules on it (ADR-0203 d1: an addition needs "a yes and a
priority"). Tracked as DF-W10 in `docs/DEFERRED.md`.

**Sources:** the ADR log, `docs/PLAN.md`, `docs/FEATURES.md`, the Master Reference (Part III
chapters 4 to 6) and the Dream Reference for what is already decided; the manuals under
`reference/DOCS` and the director's DAW research (not in the repository) for what each DAW does.

---

## 1. What is already decided, so nobody asks again

| Topic | Decided | Where |
|---|---|---|
| The rule | Live's way unless Live lacks the ability; then ADI adds, shown where the user looks, never behind a setting | ADR-0203 d1, d2 |
| The Clip View today | Live 12's, nothing added | ADR-0240 d1 |
| Names | ADI uses none of another maker's feature names for its own features and tables | ADR-0176 d5, ADR-0178 d6 |
| Pitch editing of audio (Cubase's VariAudio class) | Wanted, in the audio editor, P3; no schema, no plan card, no milestone. A neural pitch model, if used, runs as a remote service | ADR-0203 d2; FEATURES line 114; MR 4.7 |
| Warp markers and the Sample Editor (Live) | Planned, M6, P1; not built | PLAN TM-WT-02, TM-WT-06 |
| Hitpoints, slices, Slice to MIDI (Cubase) | P2; "hitpoints are warp markers" | ADR-0176 d4; TM-WT-08 |
| Audio Alignment (Cubase) | P2, written as warp markers | ADR-0176 |
| Convert Harmony, Melody, Drums to MIDI (Live) | P2; needs a pitch tracker, none chosen | TM-WT-09 |
| Per-note expression in the file | Built: one curve per note and dimension (pitch, pressure, timbre, gain, pan, and plug-in defined) | SPEC 6.3.2; ADR-0054 |
| Per-note expression ops and playback | P1, not built (`noteExpression.write`, `.clear`); stored curves do not play yet | OPS 9.5; ADR-0155 d4; MI-08 |
| MPE editing | "As Live 12's MPE tab", M6 | MI-08 |
| Scale Mode and tunings | As Live 12, P1 | ADR-0203 d7; MI-06, TU-01 to 03 |
| Step input, Humanize, MIDI Tools | Exactly Live's, P2 | ADR-0203 d7 |
| Logical Editor, Expression Maps (Cubase) | P3, no schema | ADR-0203 d7 |
| Chord track (Cubase) | P3 | FEATURES line 48 |
| Notation | Not an editor in ADI | ADR-0103 |
| One MIDI editor | No separate drum, list or in-place editor exists in any ruling | SETTINGS-CATALOGUE; MR V.3 |
| Modulators (Bitwig) | Decided: nodes in the graph, routes to device parameters, mono or global now, M7 | ADR-0046, ADR-0230; DEV-07 |
| Clip envelopes | Modulation, with Live's law per parameter | ADR-0230 d4; AU-06 |
| The Grid (Bitwig) | The patching tier is Pure Data; his answer of 5 November: the hybrid on Pd with host-owned timing. No cable diagram on screen | ADR-0035; ADR-0113 d2; DEV-15; Q-GRID |
| Reason | A comparison source for racks and macros; no feature taken | PLAN 1.1 |
| Mouse gestures of other DAWs | Tool-modifier presets named Live, Cubase, Bitwig, FL | ADR-0125 |

Two things the plan decided by win's placement, not in the director's words, which he may strike:
Bitwig's operators (occurrence, recurrence, repeats) are rejected as note properties (MI-07, MI-09),
and Bitwig's automation clips are deferred (AU-06).

## 2. What the Live baseline still needs, with no ruling required

In build order. The first is the window's alone; the rest wait for the engine.

| Step | What | Waits for |
|---|---|---|
| a | The rest of the note editor: the insert marker and time selection in the editor, dragging the rulers, the loop brace and the Start and End markers, Follow, tearing the view out, each clip's zoom remembered | nothing |
| b | The Envelopes tab: clip envelopes and MIDI controller lanes | the engine's controller lanes (#409) and clip envelopes (#434) are merged; ADR-0230 d4's change of clip envelopes to modulation is not |
| c | Hearing a note on a click, and playing a MIDI clip at all | an instrument on a MIDI track (DF-W9) |
| d | The MPE tab as Live 12's: pitch drawn on the note, Slide and Pressure lanes | the engine's `noteExpression` ops and playback (MI-08) |
| e | The Sample Editor's warp markers, transients and warp modes | the engine's warp tables and commands (TM-WT-02) |

## 3. The additions that need the director's yes and a priority

Each row says what the research supports. "Manual" means the behaviour is described well enough in
the DAW's manual to specify; "measured" means it was tested on the DAW. Nothing outside Live has been
measured.

### 3.1 Cubase

| # | Addition | Needs | Research | Win's recommendation |
|---|---|---|---|---|
| C1 | **Pitch editing of audio** (VariAudio class): segments on a piano roll over the waveform, pitch and time per segment, straighten, tilt, formant, volume, pitch quantize to the scale, extract MIDI | A monophonic pitch detector (shared with Convert Melody to MIDI), new tables, a per-segment pitch and formant engine, an ADI name, a tab in the Sample Editor | Manual, for the workflow and every gesture; 30 test cases written, none run | Yes, as the audio editor's headline addition; start with detection and display, then pitch and time, then formant and volume. Needs the research in section 5 first |
| C2 | **Line tools** in the Velocity lane and controller lanes: line, parabola, sine, triangle, square, and paint | Nothing new in the file | Manual | Yes, early: small, window only, used daily |
| C3 | **Smart controls on a selection of controller or velocity events**: tilt, compress, scale, stretch | Nothing new in the file | Manual for the gestures; the formulas are not written down | Yes, after C2 |
| C4 | **Chord editing**: make triads and four-note chords from a note, inversions, drop 2, drop 3 | Nothing new in the file | Manual | Yes; each is a composition of note moves, like the Pitch and Time tools |
| C5 | **Trim tool and the length functions** beyond Live's (fixed lengths, delete overlaps mono and poly) | Nothing new in the file | Manual; two functions' exact rules are not written down | Yes, small |
| C6 | **Drum editor with drum maps** | A second view of the same notes; drum maps need a table | Manual | Later; Live's Fold with a Drum Rack covers the common case |
| C7 | **List editor, in-place editor** | Views only | Manual | No for now: "one MIDI editor" stands |
| C8 | **Sample Editor ranges, regions and offline processing in the editor** | Offline processing is P2 already; regions need a table | Manual; shared-copy and history behaviour is open | With offline processing, P2 |

### 3.2 Bitwig

| # | Addition | Needs | Research | Win's recommendation |
|---|---|---|---|---|
| B1 | **Per-note gain and pan curves**, edited as Bitwig's expressions, beside Live's pitch, slide and pressure | The file stores them already; ops and playback with MI-08 | Manual, but for version 5.2; the editor was redesigned in 6.0 | Yes, with the MPE tab: the storage is already there |
| B2 | **Micro-pitch drawn on the note with semitone snap, and Spread on a point** | As B1 | As B1 | Yes for micro-pitch (it is Live's pitch curve); Spread is his call |
| B3 | **Operators**: repeats and ratchets, occurrence, recurrence | A side table and engine playback | Manual; the evaluation order is not written down | His call: the plan rejected them, the Dream shows them |
| B4 | **Modulators aimed at clip and note parameters**, not only at device parameters | An amendment to ADR-0230's routes | Manual | Later, with the modulators (M7) |
| B5 | **Relative automation** (additive and multiplicative) | Engine | Manual; behaviour at range ends unknown | Later; clip envelopes as modulation cover the first need |
| B6 | **The Grid** | Already answered: Pd | 130 test cases written, none run; no module is documented beyond its name | Keep the answer of 5 November |

### 3.3 Reason

| # | Addition | Research | Win's recommendation |
|---|---|---|---|
| R1 | **The rack with cables instead of the Grid tier** | Rack, Combinator and CV workflow are described; what a CV value is, and every device's jacks, are not. Reason patches closed devices; it has no environment of primitives and no per-voice path, so it is not a replacement for a Grid | No: it contradicts ADR-0113 d2 (no cable diagram on screen), and Pd already is the patching tier. Take from Reason only what is already decided: macros with a range per target |
| R2 | **Players** (note devices chained before the instrument, with Send to Track) | Six documented | Covered by Live's MIDI effects and by recording what they play (DEV-19, DEV-20) |
| R3 | **Its pitch editor** as a second reference for C1 | Its per-note controls and ranges are documented | Use it for C1's ranges (drift, transition, formant, level) |

### 3.4 Others

| # | Addition | Research | Win's recommendation |
|---|---|---|---|
| O1 | REAPER: non-destructive quantize with unquantize; several controller lanes at once | Manual | Several lanes at once: yes, with the Envelopes tab. Unquantize: his call |
| O2 | FL Studio: slide notes, chord stamp, strum | One guide of 88 pages; FL is not installed | Needs research before anything can be said |
| O3 | Studio One, Logic | Nothing | Needs research |

## 4. The four decisions that shape everything else

1. **Pitch editing of audio (C1):** raise it from P3, and to what? Win recommends P1, after the
   Sample Editor's warp markers, since both stand on the same analysis.
2. **The first batch of note-editor additions:** win recommends C2, C4, C5 and C3, in that order;
   none needs a file change, so they can follow the Live baseline directly.
3. **Per-note expression beyond Live's (B1, B2):** build gain and pan with the MPE tab, yes or no.
4. **Operators (B3):** take Bitwig's repeats, occurrence and recurrence, or keep the plan's no.

The Grid and Reason need no new decision unless he wants to reopen his answer of 5 November.

## 5. What the research still lacks

Nothing outside Live has been tested on the DAW itself: every Cubase, Bitwig, REAPER and Reason row
is a reading of a manual, and the test cases written for them have not been run.

| For | Missing |
|---|---|
| C1, pitch editing | How segments are detected (range, minimum length, voiced or unvoiced); the ranges and the mathematics of straighten, tilt, formant and volume; where each smart control sits and when it appears; what split, glue and delete do to earlier edits; the 30 written cases, run on Cubase |
| C2 to C5 | The curve between two controller events and how densely it plays; the formulas of the smart controls; whether drawing replaces or merges |
| B1, B2 | A current Bitwig manual: the one at hand is 5.2 and the installed build is 6.1, whose detail editor, automation and layered editing changed |
| MPE in every DAW | The recording path, channel and voice allocation, bend range, the lanes' units; nothing on Logic |
| B3 | The order in which the four operators are evaluated; what "previous" means for a chord |
| Reason | A manual; the device and CV chapters; the Players beyond six |
| Others | FL Studio's piano roll beyond names; anything on Studio One and Logic |
