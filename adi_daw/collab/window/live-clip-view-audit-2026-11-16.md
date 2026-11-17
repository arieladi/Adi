<!-- Written on 16 November 2026 from an audit the director ordered that day (ADR-0242 d5): Live 12's clip editing, ability by ability, against ADI's parity list, plan and code. Sources: the Live 12 manual, the director's Ableton research rows and his measured notes of 10 November. The status column was true on 16 November; parity-7.7-clip-view.md is the living list. -->

# Live 12 Clip View audit against ADI's parity list, plan and code

The owner's example is confirmed as a gap: multi-clip editing is one row in our parity list ("Multi-clip editing and Focus Mode (N) | later") and one clause in PLAN MI-03. The manual has about 30 distinct behaviours for it, none spelled out on our side. The research never measured it either; the cases were specified on 4 Nov and not run.

Three other things matter most:
- **Not mentioned anywhere on our side:** Find and Select Notes, the note clipboard (cut/copy/paste at the insert marker), the MIDI bank/program controls, the Sample Editor's "Crop Clip Sample" (writes a file), and Live's own drawing in the Velocity lane.
- **Named but not spelled out:** every MIDI Tool, the MPE tab's gestures, the Envelopes tab's controls, the Chance lane and probability groups, groove hot-swap/commit/extract.
- **Measured but not carried into the plan:** PLAN lists at least seven Live tests as "NOT RUN" that the 10 Nov measured notes appear to have run (section 4).

How to read the status column:
- **BUILT**: on origin/main or the open clip-view branches (chords, lines, trim).
- **PLANNED**: spelled out in a parity row or plan card, not built.
- **PARTLY**: some of the behaviour is built or spelled out; the missing part is named.
- **MISSING**: not in the parity list, not in the plan, not built; or only a name with no behaviours.

Sources:
- "M §x" is the Live 12 manual text.
- "A6M-xxx / A6C-xxx / A-Wxx" are rows of the director's Ableton research (all manual-derived, none measured).
- "N8 Mx-yy" are his measured notes of 10 November 2026 (Live 12.4), read by a sub-reader; I did not open the notes myself.
- I read manual chapters 8, 9, 10, 12 and 26 myself; chapters 11, 13–16, 19, 21, 25, 40, 41 and all the research came through three sub-readers.

One staleness note: the parity list on main lags the code. Fold (F), Z/X, Ctrl+D on notes, the loop brace drawn in the ruler and the Scale choosers are in `ui_clipview.hpp` but their parity rows still say "open".

---

## 1 + 2. Inventory with status

### A. Opening and layout

| # | Live does | Source | ADI |
|---|---|---|---|
| A1 | Opens by double-click on a clip, the Clip View Selector, its toggle, or Ctrl+Alt+3; always shows the selected clip | M §8 | BUILT (parity 1) |
| A2 | Toggle used with Device View open stacks Clip View above it | M §8 | BUILT (parity 2) |
| A3 | Shift+Tab or F12 toggles Device/Clip View; Alt-click on the view toggle shows both | M §41.1 | MISSING for one window (WIN-08 covers the second-window move) |
| A4 | Top border resizes; dragged to the bottom closes | M §8 | BUILT (parity 3) |
| A5 | Ctrl+Alt+E / View > Expand Clip View toggles full height (measured the same key) | M §10.2; N8 M6-01 | PLANNED (parity 4) |
| A6 | Second window: opens in the focused window, F12 moves it, shows the clip selected in the other window | M §8 | PLANNED P2 (WIN-08); parity 37 is ADI's tear-out, a different thing |
| A7 | Session: click on a Track Status Display opens the playing clip; Select on Launch makes a launched clip the shown one | M §8, §7.4.1 | MISSING detail (Session, P2; not listed) |
| A8 | Title bar, panels left, editor right | M §8.1 | BUILT |
| A9 | Panels horizontal or vertical: drag the panels' right edge, View menu entries, or "Arrange Clip View Panels Automatically" (also in the title bar menu); remembered | M §8.1.2 | PARTLY (parity 6: vertical "later"; Automatic, the title-bar entry and remembering not listed) |
| A10 | Double-click the title bar, or Fold in its menu, minimises the panels | M §8.1.2 | PLANNED (parity 5) |
| A11 | Tabs Notes/Envelopes/MPE and Sample/Envelopes; switch by click, View menu, Alt+Shift+1/2/3, or a Tab key combo (manual disagrees with itself, see section 4) | M §8.1.3, §41.10 | PARTLY (parity 7: tabs drawn, "Alt/Option+Tab"; Alt+Shift+1/2/3 and View menu missing) |
| A12 | Focus keys: Alt+3 to the editor, Alt+Shift+P to the panels, Alt+8 to the selected panel | M §41.2, §40.4.1.4 | MISSING |
| A13 | Clip View Selector overview: whole clip, black outline; drag in it sideways to scroll, vertically to zoom; drag the outline's edges | M §10.2, §8.7 | PARTLY (parity 10: miniature and click built; drags open; edge-drag not listed) |
| A14 | Frozen clip's Clip View is drawn under a blue tint | N8 M5-part2 (measured) | MISSING |
| A15 | Clip Update Rate setting quantises Clip View changes to a running clip | M §8.14 | PLANNED (TM-SV-03, Session) |
| A16 | Key/MIDI mappings of Clip View controls act on whatever clip(s) are selected | M §33.2.4.2; A5W-013 | PLANNED (PLAN Key Map card) |

### B. Title bar and clip properties

| # | Live does | Source | ADI |
|---|---|---|---|
| B1 | Activator toggle; 0 key; clip context menu; with several clips selected it deactivates all | M §8.1.1.1 | PARTLY (built for one clip; multi not listed) |
| B2 | Rename from the title bar menu or Ctrl+R; Edit Info Text; renaming does not rename the file | M §8.1.1.2 | PARTLY (name shown; Rename open; Edit Info Text missing) |
| B3 | Colour from a palette in the title bar or clip context menu; new clips take the track colour; "Assign Track Color to Clips" (and the Group variant), per view | M §8.1.1.3 | PARTLY (colour shown; menu open; Assign commands not listed) |
| B4 | Save Default Clip button (audio): writes settings to the sample's analysis file, applies to future drops only, own samples only | M §8.1.1.4, §9.2.3.2; N8 M3-07 | PLANNED in TM-WT-06/TM-MD-04; not in parity 36 |
| B5 | Start/End fields; Set buttons take the current playhead during playback, rounded to global quantization; warped shows bars.beats.16ths, unwarped min:sec:ms | M §8.2.1; A6C-006 | PARTLY (fields built; ADI's Set takes the arrangement's insert marker, see section 4; unwarped units not listed) |
| B6 | Loop toggle, Position, Length; Set Loop Position moves loop start to the playhead and switches the loop on; Set Loop Length moves loop end to the playhead (with the loop off it keeps the preset length and switches it on); audio needs Warp to loop | M §8.2.1; A6C-007/008 | PARTLY (same Set difference; the audio rule not listed) |
| B7 | Signature per clip, display only | M §8.2.2 | BUILT |
| B8 | Groove chooser from the Groove Pool | M §8.2.3 | PARTLY (parity 8 names it; MI-05 has the pool's parameters) |
| B9 | Hot-Swap Groove button; Commit writes the groove (MIDI: notes move; audio: warp markers, and a volume clip envelope for velocity, overwriting an existing one), then sets the chooser to None | M §8.2.3, §14.1.2 | MISSING detail (only "Commit Groove writes one applyDiff") |
| B10 | Grooves on audio only with Warp on; drag an .agr onto a clip; Extract Groove (context menu, or drag the clip to the pool); drag a groove to a MIDI track to get a clip; Global Amount to 130 % | M §14 | MISSING detail (no "extract" in PLAN) |
| B11 | Scale Mode toggle, Root and Scale Name choosers, in Clip View and Control Bar; on an audio clip the scale only feeds scale-aware devices | M §8.2.4, §3.13 | BUILT for the two choosers; PLANNED rest (MI-06) |
| B12 | Scale list: 35 scales in Live's order, 12 roots, measured with their semitone sets | N8 M6-01 (with a table of the scales) | PLANNED; MI-06 says the census is still to be done, but the research has it |
| B13 | Extended panel (Session clips): Launch Mode, Legato, Quantization, Velocity, Nudge, Follow Actions; the Follow Action Time marker shown and draggable in the editor | M §8.3, §16 | PLANNED P2 (TM-SV-03/04); the marker in the editor not listed |
| B14 | MIDI clips: Bank / Sub-bank / Program choosers (128 each, "—" for none), sent on launch; shown for Arrangement MIDI clips too | M §8.3.2 | MISSING (no hit in parity or PLAN) |

### C. MIDI Note Editor

**Navigation, zoom, grid**

| # | Live does | Source | ADI |
|---|---|---|---|
| C1 | Time ruler: drag vertically to zoom, sideways to scroll; Ctrl+wheel zooms | M §10.2 | PARTLY (wheel built; ruler drag open, parity 11) |
| C2 | Note ruler: wheel scrolls octaves; drag sideways zooms key tracks; Alt+wheel zooms them | M §10.2 | PARTLY (parity 12; ruler drag open) |
| C3 | Double-click note ruler / time ruler zooms to the selection; nothing selected: lowest-to-highest note / first-to-last note; an off-screen selection is brought into view | M §10.2; A6M-002 | PLANNED (parity 14) |
| C4 | Page Up/Down one octave; Shift one key track; Ctrl+Page Up/Down scrolls horizontally | M §10.2, §41.12 | PLANNED first two (parity 13); horizontal MISSING |
| C5 | + and − zoom around the time selection; Z zooms to selection, X back step by step; Alt + / − zoom the editor; W fit width, H fit height | M §10.2, §41.12 | PARTLY (Z/X built; +/− in STEP-7; W, H, Alt+/− missing) |
| C6 | Ctrl+Alt drag pans the display | M §41.9, §26.2.2 | MISSING |
| C7 | Follow scrolls with playback; an edit pauses it; resumes on the switch, on stop/restart, or on a scrub click | M §10.2.2; A6M-004 | PARTLY (parity 16) |
| C8 | Scrub area under the ruler: click jumps playback there, rounded to global quantization; holding repeats one quantum; Shift-click when Permanent Scrub Areas is off; Chase MIDI Notes | M §8.8, §10.2.2 | PARTLY (TM-SV-01 has quantized scrub and chase; not in parity) |
| C9 | Grid chooser in the header; adaptive Widest..Narrowest or fixed 8 Bars..1/32; Ctrl+1 narrow, 2 widen, 3 triplet, 4 snap, 5 fixed/adaptive; also from the editor's context menu | M §6.10, §41.13; N8 M2-16 | PARTLY (adaptive grid built; chooser and Ctrl+4 open; 1/2/3/5 not listed for the editor) |
| C10 | Magnetic snap: a note moves freely to the first grid line, then snaps; it also snaps to its original offset; Alt bypasses, or enables snap when the grid is off | M §10.2.1; A6M-003 | PARTLY (Alt bypass built; offset snap in STEP-7; "enables when off" not listed) |

**Adding, drawing, preview, step input**

| # | Live does | Source | ADI |
|---|---|---|---|
| C11 | New clip: double-click an empty slot or lane, Ctrl+Shift+M; MIDI clips loop by default; a Session slot clip is one bar | M §10.3, §19.3.3 | BUILT (#588) |
| C12 | Double-click adds a note, or deletes the one under it | M §10.4 | BUILT |
| C13 | Draw Mode (B; holding B about 500 ms is momentary); a click on a note deletes it; drawn velocity 100 | M §10.4.1, §41.25; N8 M2-11 | PARTLY (B and the switch built; momentary hold missing) |
| C14 | "Draw Mode with Pitch Lock" setting; Alt gives the other mode; melodic drawing erases notes drawn over; pitch-locked drawing erases as the cursor moves back; menu reads "Draw Mode (Pitch Lock On/Off)" | M §10.4.1; A6M-006 | PARTLY (pitch-locked only; setting and Alt open in parity 17; the two erase rules not listed) |
| C15 | Preview switch: hear a piano key, an added, selected or moved note; one state for all MIDI tracks | M §10.4.2 | PLANNED, waits (parity 18, DF-W9) |
| C16 | Step recording: track armed, Preview on, transport stopped; Right arrow advances by the grid and adds held notes; holding and pressing again lengthens; Left deletes; mappable | M §19.3.4; A4-018 | MISSING detail ("step recording" / "step input (P2)" only) |
| C17 | Overdub into a looping clip while drawing, moving or deleting notes | M §19.3.3 | MISSING (Session) |

**Selection**

| # | Live does | Source | ADI |
|---|---|---|---|
| C18 | A click places a flashing insert marker; arrows move it by the grid; Ctrl (Option) + arrows to note edges; Home/End to clip start/end | M §10.5.2; A6M-009 | PLANNED (parity 19, 20); Home/End not listed |
| C19 | A drag selects time and the notes inside; Enter swaps between the time and its notes; Esc or a click outside collapses | M §10.5.2 | PARTLY (note rubber band and Esc built; time selection and Enter open) |
| C20 | Shift+arrows select time by the grid; with Alt (Cmd), off the grid; Alt+Shift (Option+Shift) to the next note edge | M §10.5.2 | MISSING |
| C21 | Ctrl (Option) + Up/Down selects the nearest, then next/previous note in time; + Left/Right the next in the same key track; adding Shift extends the selection | M §10.5.2, §40.4.1.4 | PARTLY (parity 21 lists up/down; left/right and the Shift form missing) |
| C22 | Shift-click adds or removes a note; Shift-click a piano key adds or removes its key track; a plain click on a piano key selects that track's notes | M §10.5.2, §10.5.12.1 | PARTLY (Shift-click built; the piano forms not built, plain click not listed) |
| C23 | Ctrl+A; Invert Selection (Ctrl+Shift+A, Edit and context menus) | M §10.5.2 | PARTLY (Ctrl+A built) |
| C24 | Find and Select Notes: header toggle (Ctrl+Shift+F), a filter chooser with eight filters, Invert, Select, a yellow dot on applied filters, filters combine, a click in the editor clears them; Shift-drag makes evenly repeated time selections | M §10.5.3; A-M02, A6M-011/012; N8 M2-19 | MISSING (no hit anywhere) |
| C25 | The eight filters: Pitch (any octave), Time (Start, Length, Repeat), Chance (min/max), Condition (Active, Chance < 100 %, Velocity Deviation), Count (Every, Offset, Quantized), Duration (min/max), Scale (clip scale or a chosen one), Velocity (min/max) | M §10.5.3 | MISSING |
| C26 | Status Bar shows the selected note's time, pitch, velocity and probability (measured text: "Note Selection Time: 1.1.1 - 3.1.1 (2.0.0) Pitch: A#3 (70) Velocity: 100 Probability: 100%") | M §3.2; N8 M5-part2 | MISSING |

**Moving, copying, length, note operations**

| # | Live does | Source | ADI |
|---|---|---|---|
| C27 | Drag or arrows move and transpose; Shift+Up/Down by octaves; Alt (Cmd) + Left/Right nudges off the grid | M §10.5.4 | BUILT except the off-grid key nudge (not listed) |
| C28 | Cut, Copy, Paste of notes; paste lands at the insert marker | M §10.5.4; A6M-013 | MISSING (no clipboard row in parity; MI-03 does not spell it) |
| C29 | Ctrl (Option) drag copies, even pressed after the drag began; Ctrl+D duplicates | M §10.5.4 | BUILT |
| C30 | Overlap: the newer note survives whole; the older is cut at the new note's start, or removed | M §10.5.4; N8 M2-11 (measured, see section 4) | PLANNED (parity 23, MI-02) |
| C31 | Edges resize; Shift+Left/Right by the grid; Alt off the grid | M §10.5.5 | BUILT |
| C32 | Fit to Time Range (Ctrl+Alt+J; Edit and context menus; an entry in the Duration menu) | M §10.5.5 | PLANNED (STEP-7 step 4, MI-04) |
| C33 | Note Stretch markers under the scrub area for a selection; snap like notes; one can cross the other and mirror the notes | M §10.5.6; N8 M2-11 | PLANNED (parity 24, MI-02) |
| C34 | A "pseudo" stretch marker between the two stretches only the material inside; stretching also moves linked clip envelopes and per-note expression | M §10.5.6, §12.2; A6M-015, A-M05 | MISSING detail |
| C35 | 0 deactivates notes (grey, silent); also from the piano ruler's and editor's menus | M §10.5.7 | BUILT |
| C36 | Split: hold E and draw a line across notes, or E + drag inside a note to place the cut; Ctrl snaps it; Ctrl+E with nothing selected splits at the insert marker or the time selection's edges | M §10.5.8.1; A6M-017 | PARTLY (parity 26 names the keys; the E-drag gestures and availability rule not spelled) |
| C37 | Chop: Ctrl+E with notes selected, on the grid; keep Ctrl held and Up/Down changes the part count, Shift by powers of two; with the mouse E+Ctrl and drag up/down | M §10.5.8.2 | PARTLY (name only; part-count gestures missing) |
| C38 | Join (Ctrl+J): same-pitch notes into one, joining their MPE envelopes | M §10.5.8.3 | PLANNED (parity 26, MI-04) |
| C39 | Ctrl+Shift+E splits the Arrangement clip at the editor's time selection | M §41.12 | MISSING |

**Velocity, release velocity, chance**

| # | Live does | Source | ADI |
|---|---|---|---|
| C40 | Velocity lane: drag a marker; value in the lane header; the hovered note's marker is highlighted; note saturation shows velocity | M §10.5.12 | PARTLY (drag and saturation built; header value and hover highlight not listed) |
| C41 | Shift-click selects several markers; drag to the top or bottom and back to equalise; type a value + Enter; Ctrl+Up/Down by 10, Shift fine; Alt (Cmd) vertical drag on a note | M §10.5.12, §41.12; A6M-025 | PARTLY (parity 29 lists most as open; equalise not listed) |
| C42 | Randomize button and amount (± the amount; typing + Enter triggers it; all notes when none selected) | M §10.5.12 | PLANNED (parity 29) |
| C43 | Ramp Start / End sliders spread velocities evenly across the selection | M §10.5.12 | PLANNED (parity 29) |
| C44 | Velocity Deviation: a signed range rolled at each play; shaded area and a dot on the marker; double-click resets; mixed ranges shown; Ctrl-drag from a marker; Ctrl+Shift+Up/Down | M §10.5.12, §41.12; A6M-026 | PARTLY (MI-09 has the model; none of the editor gestures listed) |
| C45 | Draw Mode in the lane: sets all notes in the grid step (only the selected ones if any; affected notes turn blue on hover); Alt (Cmd) draws single markers; Alt (Option) draws a straight line, Shift makes it level | M §10.5.12.1; A6M-027 | MISSING (ADI's Cubase line tools, A3, are a different tool) |
| C46 | Lane Selector menu (Velocity, Release Velocity, Chance); Show/Hide All toggle; Swap Lane in the lane header's menu; divider and per-lane split lines; Alt+wheel resizes | M §10.1; A6M-001 | PARTLY (parity 30: selector and divider; the rest not listed) |
| C47 | Release Velocity lane | M §10.5.12.2 | PLANNED later (parity 30, MI-09) |
| C48 | Chance lane: markers 0–100 %; value in the lane and the Status Bar; ranges for mixed selections; type + Enter; arrows ±10 %, Shift fine (the shortcut table gives Ctrl+Alt+Up/Down); a corner triangle on notes below 100 % when rows are tall enough; Randomize amount and button | M §10.5.13, §41.12; A6M-029 | MISSING detail (parity 30 "Chance later (P2)") |
| C49 | Probability groups: Play All / Play One buttons; one shared marker (diamond / triangle); right-click changes the type; Ctrl+G groups with the last type; hover lights the group; Status Bar names the type (asterisk if mixed); Ungroup button, Ctrl+Shift+G | M §10.5.13.1; A-M07, A6M-030 | MISSING detail (MI-09 has the data model only) |

**Pitch and Time panel, quantize**

| # | Live does | Source | ADI |
|---|---|---|---|
| C50 | Transpose slider (shows the range; drag or type; degrees with a scale); Fit to Scale (nearest degree, ties go down; greyed with no scale); Invert | M §10.5.9.1–3; N8 M6-01 | BUILT |
| C51 | Interval Size + Add Interval: with a selection, changing the size adds and selects the notes at once; with none, Add Interval applies to every note | M §10.5.9.4; A6M-020 | PLANNED (parity 27, open) |
| C52 | Stretch factor; ×2 and /2 act on notes, the time selection, or the loop region | M §10.5.9.5 | PARTLY (×2 and /2 built for notes; the loop and time-selection cases not listed; knob open) |
| C53 | Duration chooser (fit to time range, grid, fixed lengths) + Set Length (all notes when none selected) | M §10.5.9.6 | PLANNED (open) |
| C54 | Humanize amount (default 10 %) + button | M §10.5.9.7; N8 M6-15 | PLANNED (open; MI-05) |
| C55 | Reverse (whole clip when none selected); Legato (each note to the next start, the last to the loop end) | M §10.5.9.8–9 | BUILT |
| C56 | Quantize: Ctrl+U applies the stored settings; Ctrl+Shift+U opens the Quantize tool; grid or a fixed value with triplets, note start and/or end, Amount | M §10.5.11, §11.2.7; N8 M6-15 | PLANNED (parity 28; `note.quantize` on main) |
| C57 | Record Quantization (Edit menu list; its own undo step) | M §19.5; N8 M6-04 | PLANNED (MI-05) |

**MIDI Tools (Transform and Generate panels)**

| # | Live does | Source | ADI |
|---|---|---|---|
| C58 | Tool chooser per panel; Auto Apply (on by default; off restores the notes and shows Apply); Ctrl+Enter applies from the editor; Reset restores defaults and does not touch the notes; undo covers the notes only | M §11.1 | PARTLY (MI-07 has Auto Apply, preview, settings saved; Ctrl+Enter and Reset not listed) |
| C59 | Scope: transformations act on the time selection, the note selection, or the loop, and replace the notes; generators act on the time selection or the loop, keep non-overlapping notes and replace overlapping ones; pitch parameters use scale degrees when a scale is on | M §11.1; A-T02 | PARTLY (MI-07 lists the generator rule as an open question; the manual answers it) |
| C60 | Transformations: Arpeggiate, Chop, Connect, Glissando (MPE), LFO (MPE), Ornament (Flam, Grace Notes), Quantize, Recombine, Span, Strum, Time Warp, Velocity Shaper (Max for Live) | M §11.2.1–12 | MISSING detail (MI-07 names only Arpeggiate) |
| C61 | Generators: Rhythm, Seed, Shape, Stacks (chord pads, `.stacks` banks), Euclidean (Max for Live) | M §11.3.1–5 | MISSING detail |
| C62 | Alt-click (or drag) on the piano ruler sets a generator's pitch or range; sliders turn purple with a scale; Max for Live tools load from the User Library's MIDI Tools folders | M §11.3, §11.1.1 | MISSING |

Each tool's parameters are in `live-clip-view-audit-2026-11-16-tools.md`, beside this file.

**Fold, scales, tuning**

| # | Live does | Source | ADI |
|---|---|---|---|
| C63 | Fold to Notes (F, header button, View menu); the setting applies to every MIDI clip in the Set | M §10.6 | BUILT (per-Set scope not listed) |
| C64 | Drum Rack track: the ruler lists pads; unfolded shows pads that hold devices, folded only rows with notes | M §10.1, §10.6; A6M-031 | MISSING |
| C65 | Scale Mode highlights in-scale keys on the piano; Highlight Scale (K, header button, menus; global) tints key tracks and marks the root; measured: in-scale rows are drawn lighter | M §10.6; N8 M6-01 | PLANNED later (parity 31, MI-06) |
| C66 | Fold to Scale (G, Scale button); rows holding out-of-scale notes stay visible | M §10.6 | PLANNED later |
| C67 | Note spelling from the piano ruler's menu: sharps, flats, both, Auto; MIDI note numbers; with a scale on, only out-of-scale notes follow the setting | M §10.6; N8 M6-01 | PARTLY (MI-06 has Sharp/Flat/Auto; numbers missing) |
| C68 | Tuning loaded: ruler shows the tuning's notes; Scale choosers hidden; Bypass Tuning per track; Drum Racks bypass | M §15; N8 M6-02 | PLANNED (TU-03) |

**Clip-level MIDI editing**

| # | Live does | Source | ADI |
|---|---|---|---|
| C69 | Crop Clip / Crop Clip to Time Selection (Ctrl+Shift+J); no file written | M §10.7.1; N8 M2-10 | PLANNED (parity 33, ED-04) |
| C70 | Duplicate, Delete and Insert Time; clip start/end and loop brace stay put | M §10.7.2 | PLANNED (parity 34) |
| C71 | Loop brace: drag its ends or the brace; Left/Right nudge by the grid; Ctrl+Left/Right change its length; greyed when the loop is off; adjustable during playback | M §8.9, §10.7.3 | PARTLY (drawn; drags and keys open, parity 32) |
| C72 | Up/Down move the brace by its own length; Ctrl+Up/Down double/halve it; Ctrl+Shift+L selects the material in the loop | M §8.9, §41.8 | MISSING detail |
| C73 | Ctrl+D on the brace (Duplicate Loop): doubles the loop, copies its notes, shifts later notes, zooms out | M §10.7.3 | PLANNED (parity 9) |
| C74 | Start/End markers: drag; arrows; Alt+arrows move both; Shift+arrows "move clip region with start marker"; Ctrl+F9/F10/F11/F12 set start, loop start, loop end, end; Ctrl-click and Ctrl+Shift-click move start/end to a point | M §8.8, §41.8, §41.11–12 | MISSING detail (parity 32 names the markers only) |

### D. Multi-clip editing and Focus mode

| # | Live does | Source | ADI |
|---|---|---|---|
| D1–D30 | Every behaviour written out in section 3 | M §10.8, §8.1.1, §8.13, §10.6; A-M10, A6M-035..038 | MISSING detail: parity 35 is one "later" row; MI-03 says "multi-clip editing, Focus and the ghost-note switch (P2)" and cites A6M-036..038 as untested |

### E. MPE tab ("Note Expression")

| # | Live does | Source | ADI |
|---|---|---|---|
| E1 | Five dimensions per note: Pitch drawn on the note; Slide, Pressure, Velocity, Release Velocity in lanes (Slide and Pressure shown by default); works on any MIDI clip | M §12, §12.1 | PARTLY (MI-08 and CLIP-VIEW-ADDITIONS 2d: pitch, slide, pressure; the velocity lanes here not listed) |
| E2 | Lane toggle buttons; a triangle shows/hides all (Alt-click shows all); lanes resize singly or together | M §12.1 | MISSING detail |
| E3 | Click a note: it becomes an overlay with its envelopes; other notes grey, their envelopes dimmed | M §12.2 | MISSING detail |
| E4 | Breakpoints: click a segment to add, click a point to delete, values shown on hover/drag, selection drags together, Edit Value / Add Value by right-click, Shift constrains an axis or goes fine, dragging over a neighbour removes it, Alt-drag curves a segment, Alt double-click straightens | M §12.2 | MISSING detail (MI-08's reuse note mentions "Alt-drag segment bend") |
| E5 | Own grid, off by default, saved with the clip; Ctrl+4 turns it on; with it off, Alt (Cmd) snaps pitch to semitones | M §12.2 | MISSING |
| E6 | Scale a whole Slide/Pressure/Velocity envelope by dragging it once it turns blue (also for several selected notes); Ctrl+A then drag shifts all points | M §12.2 | MISSING |
| E7 | Expression moves with its note and stretches with ×2, /2 and the stretch markers; Join joins it; Crop keeps and re-bases it (measured) | M §12.2, §10.5.8.3; N8 M2-10 | PLANNED (MI-04, MI-08: "utilities carry expression") |
| E8 | Pitch envelopes hidden when Fold is on; Z zooms to the pitch-bend range; Clear All Envelopes in the context menus | M §12.2 | MISSING |
| E9 | Draw Mode draws Pitch, Slide and Pressure freehand, or in grid-wide steps when the grid is on; Shift fine | M §12.3 | MISSING detail |
| E10 | Track meter's lowest dot turns blue on per-note controller data | M §12.1 | MISSING |

### F. Envelopes tab

| # | Live does | Source | ADI |
|---|---|---|---|
| F1 | Device chooser (audio: Clip, each effect, Mixer; MIDI: MIDI Ctrl, each device, Mixer) and Control chooser; LEDs beside controls that have envelopes (red automation, blue modulation); "Only show adjusted envelopes" | M §26.1, §26.3 | MISSING detail (AU-06 covers the playback law, not the editor) |
| F2 | Session clips: Automation / Modulation toggles; Arrangement clips: modulation only | M §26.1, §26.3 | PLANNED (AU-06) |
| F3 | Audio "Clip" envelopes: Transposition (added to Transpose, clipped at ±48), Gain (a percentage of Clip Gain), Sample Offset (Beats mode only, ±8 sixteenths, a sixteenth per grid line) | M §26.2 | PARTLY (AU-06 has transposition and gain; Sample Offset missing) |
| F4 | Mixer envelopes: Track Volume (a dot under the fader shows the result), Sends (reduce only), Pan (scaled by the knob); device controls relative | M §26.3 | PLANNED (AU-06) |
| F5 | MIDI Ctrl envelopes: controllers up to 119 plus Pitch Bend etc.; imported and recorded controller data shows here; MIDI Envelope Auto-Reset option | M §26.4, §26.1 | PARTLY (engine controller lanes merged, #409; Auto-Reset missing) |
| F6 | Linked / unlinked: an unlinked envelope gets its own Start/End, Loop switch, loop length and coloured brace; it can be longer or shorter than the clip | M §26.5; N8 M6-07 | PLANNED (AU-06) for playback; the editor's controls not listed |
| F7 | Linked envelopes follow warp-marker moves, and warp markers can be moved from inside the envelope editor | M §26.5.5 | MISSING |
| F8 | Clear Envelope from the editor's context menu (Ctrl+Backspace) | M §26.1 | MISSING detail |
| F9 | Editing gestures as Arrangement automation: breakpoints, Draw Mode steps, curves, stretch/skew handles on a time selection, Simplify, Insert Shape, keyboard breakpoint editing | M §25.5, §40.5 | PLANNED for the Arrangement (AU-01, AU-02, AU-08); not stated for the Clip View tab |

### G. Audio clip: Sample Editor and Audio Utilities

None of this is built (parity 36 "waits"). TM-WT-02 and TM-WT-06 spell out most warp behaviour.

| # | Live does | Source | ADI |
|---|---|---|---|
| G1 | Header: sample name, rate, bit depth, channels; clicking the name reveals the file in the browser; Show in Browser | M §8.10 | PARTLY (parity 36: the readings; the click not listed) |
| G2 | Warp switch; Seg. BPM typed or Shift-dragged; ×2 and ÷2 | M §8.4.1, §9.2.4.1 | PLANNED (TM-WT-02/06) |
| G3 | Warp markers: double-click in the upper half adds; Ctrl+I at the insert marker; drag; Shift-drag moves the waveform under it; double-click or select time + Delete removes | M §9.2.3; N8 M6-12 | PLANNED |
| G4 | Left/Right move the selected marker; Ctrl+Left/Right select the next marker; Ctrl-click selects several and moves them together | M §41.11, §9.2.5.1 | MISSING detail |
| G5 | Transients (grey); Ctrl+Shift+I and Ctrl+Shift+Delete; Reset Transients; pseudo-markers on hover, double-click or drag to promote, Ctrl also pins the neighbours, Shift-drag moves the transient; Ctrl+I over a time selection makes a marker per transient, or one at each edge | M §9.2.3.1; N8 M6-12 | PLANNED |
| G6 | Context menu: Set 1.1.1 Here, Warp From Here (plain, Start At, Straight, "… BPM From Here"), Warp Sample As…, Warp Selection As… | M §9.2.4.3, §9.2.5.1; N8 M6-12 | PLANNED (TM-WT-02) |
| G7 | Warp modes and controls: Beats (Preserve 1 Bar to 1/32 or Transients; loop Off / Forward / Back-and-Forth; envelope 0–100), Tones (grain), Texture (grain, fluctuation), Re-Pitch (transpose disabled), Complex, Complex Pro (Formants, Envelope 128) | M §9.3; N8 M6-13 | PLANNED (TM-WT-02, -03) |
| G8 | Quantize audio: Ctrl+U; the Transform panel's Quantize tool (grid or value, Amount) | M §8.6, §9.2.7 | PLANNED |
| G9 | Lower half of the waveform is a scrub area; upper half edits | M §8.8; N8 M6-12 | MISSING |
| G10 | Gain slider in dB; Pitch knob with a semitone field and a cents field | M §8.4.7 | PARTLY (parity 36 names Gain, Pitch) |
| G11 | Reverse: writes a new sample under Samples/Processed/Reverse; markers stay on the sample, loop/region flip, envelopes stay in time; R on an Arrangement time selection across audio clips; not for several Session clips | M §8.4.2; A6C-013..016 | MISSING detail (name only in parity 36; no plan card) |
| G12 | Edit button opens the external editor; markers kept only if the length is unchanged | M §8.4.3 | MISSING detail (name only) |
| G13 | Clip Fade switch (Session clips), RAM, Hi-Q | M §8.4.4–6 | PARTLY (Fade and RAM in TM-SV-03; Hi-Q is a name in parity 36 only) |
| G14 | Crop Clip Sample / Crop Clip Sample to Time Selection (Ctrl+Shift+J): writes a new, shorter sample under Samples/Processed/Crop | M §8.11 | MISSING (ED-04 plans a crop with no file; see section 4) |
| G15 | Replace the sample: drop one on the Clip View, or double-click/Enter in the browser; settings and envelopes kept; markers kept only at the exact same length; Show Similar Files; Manage Sample File | M §8.12, §26.2.5 | MISSING in parity (not confirmed in the plan) |
| G16 | Start/End markers, loop brace, Z/X, W/H, Follow, as the note editor | M §8.7–8.9, §41.11 | as C71–C74 |
| G17 | Tempo Leader/Follower switch on Arrangement clips | M §9.1.4 | PLANNED (TM-WT-07) |
| G18 | Multi-clip warping: same-length selected clips share marker edits | M §9.2.4.4 | PLANNED (TM-WT-02) |
| G19 | Loop-recorded audio: one long sample, the brace on the last pass; move it left to hear earlier passes | M §19.3.1 | MISSING |
| G20 | Envelopes tab on an audio clip appears only while Warp is on | N8 M6-07 (measured) | MISSING (parity 7 lists it without the condition) |

### H. Comping, conversion, shortcuts

| # | Live does | Source | ADI |
|---|---|---|---|
| H1 | Take lanes and comping live in the Arrangement; the manual gives them no Clip View part | M §21; N8 M6-03 | outside the Clip View; PLAN has its own comping cards |
| H2 | Slice to New MIDI Track (dialog: division, preset, Preserve warped timing; at most 128 slices) | M §13.1 | PLANNED P2 (TM-WT-08) |
| H3 | Convert Harmony / Melody / Drums to New MIDI Track (no dialog; use the clip's transients) | M §13.2–5 | PLANNED P2 (TM-WT-09) |
| H4 | Shortcuts with no home on our side: Alt+Shift+1/2/3, Alt+3, Alt+8, Alt+Shift+P, W, H, Ctrl+Page Up/Down, Ctrl+F9..F12, Ctrl+Shift+L, Ctrl+Enter, Ctrl+G, Ctrl+Shift+G, Ctrl+Shift+E, Ctrl+Shift+A, Ctrl+Alt+Up/Down, Ctrl+Shift+Up/Down, N, K, G | M §41.2, §41.8, §41.10–12 | MISSING except K and G (parity 31, later) and N (parity 35, later) |

---

## 3. Ranked: what a Live user would notice first

### 1. Multi-clip editing and Focus mode (everything the manual says; the research adds nothing measured)

**Choosing the clips**
- Select several MIDI clips (drag from an empty slot, or Ctrl/Shift-click) and open the Clip View: their notes appear together in one note editor.
- Session: up to eight clips, and all must be looped. Arrangement: clips from up to eight tracks across a span of time.
- It also works for clips on the same track (variations down the scenes).

**Loop bars**
- One loop bar per clip appears above the editor, in the clip's colour.
- Session order: by track, then by scene. Arrangement: vertically by track, horizontally by time.
- Drag a bar's loop marker to change that clip's loop length.
- Ctrl-click or Ctrl-drag selects several bars; Shift selects a contiguous run. Not possible in Focus mode, where an existing multi-selection is ignored.
- Ctrl+D or the context menu duplicates the selected loop bars.
- Drag just above the title bar's scrub area to change the bars' height.

**Which clip is being edited**
- Clicking a clip's note or its loop bar makes that clip the one being edited.
- The title bar shows that clip's name, or its track's name if the clip has none ("useful for identifying different clips with the same color").
- With several clips selected the title bar shows the number of clips and a striped pattern; rename and recolour still apply to all.

**Focus mode on** (Focus button, or N; holding N while using the mouse is momentary)
- The active clip's notes are in its colour; the other clips' notes are grey.
- The active clip's loop bar is in its colour, the others grey. A clicked active bar turns black; clicking elsewhere on its timeline returns it to the colour while it stays in focus.
- The active clip's name is shown below the loop bars.
- Hovering an inactive clip's loop bar reveals its colour and notes.
- All note and clip editing applies to the active clip only.
- The scale shown is the active clip's; it drives Fold to Scale.
- Fold to Notes hides every key track that no selected clip uses.

**Focus mode off**
- Every note is in its own clip's colour, and all are editable together.
- Clicking a loop bar turns it black; Randomize for Velocity or Chance then acts on that clip.
- Root and scale are shown only if all clips agree; otherwise an asterisk.
- Cut or copy notes from several clips and paste back into the same clips, as long as the clip selection and the foreground clip have not changed; after selecting another clip, the paste goes into that one.
- Copy, cut, paste and delete work on selections that cross clip and loop boundaries; a time selection can cross them too.
- Arrangement: notes can be drawn continuously across clip boundaries.

**Always**
- Loop, signature, groove and scale in the panels apply to all selected clips. The Loop button is half-coloured when looped and unlooped clips are mixed.
- Fold to Notes and Fold to Scale work for all selected clips.
- The Velocity and Chance lanes show and edit the foreground clip only, never all clips.
- With clips of different scales selected, the foreground clip sets the scale for new clips.

**Time display**
- Session: with different loop lengths the editor shows as many repeats as needed for the loops to realign. Outside a clip's loop the timeline is a darker shade of its colour. Hovering a clip shows its loop start and end as black vertical lines. A start marker before the loop shifts those lines.
- Arrangement: no silence before or after; the view runs from the first clip's start to the last clip's end.

**Left open by the manual and the research** (A6M-038 says so itself): who owns a note drawn across two clips or a gap; how the ninth clip is refused; how MIDI Tools behave with several clips.

### 2. Time selection, insert marker and the note clipboard
C18–C20, C28. Cut/copy/paste of notes is in neither the parity list nor a plan card.

### 3. Loop brace and markers in the editor
C71–C74, plus the Set buttons' meaning (B5, B6).

### 4. The overlap rule, Quantize (Ctrl+U), Split/Chop/Join
C30, C36–C38, C56. These are planned; the Chop part-count and E-drag gestures are not written down.

### 5. Velocity lane completeness
C40–C46: typed values, Alt-drag on a note, Randomize, Ramp, Deviation, Live's Draw Mode in the lane (missing), the lane selector.

### 6. Hearing notes, and step recording
C15, C16.

### 7. Scale highlighting, Fold to Scale, note spelling
C65–C67.

### 8. Find and Select Notes
C24, C25: missing everywhere.

### 9. Chance lane and probability groups
C48, C49: model planned, editor not written down.

### 10. MIDI Tools
C58–C62: seventeen tools, one of them named.

### 11. Envelopes tab
F1–F9: choosers, LEDs, MIDI controller lanes, unlinked loop controls, Sample Offset.

### 12. MPE tab
E1–E10.

### 13. Audio clip gaps beyond the planned warp work
G11 Reverse rules, G14 Crop Clip Sample, G15 replace by drop, G9 scrub half, G4 marker keys, B14 bank/program.

### 14. Editing properties of several clips at once (not notes)
M §8.13: only the properties the clips share are shown; a slider shows the range of values, and dragging to an extreme makes them equal; the Gain slider shows split handles; the sample header shows a count and asterisks; Save Default Clip saves all. Measured: the editor reads "8 audio clips in 8 tracks are selected." All missing.

### 15. Layout and navigation odds and ends
A3, A9–A13, C3–C6, C26, H4.

---

## 4. Where measurement, manual and our rows disagree

**Measured behaviour that differs from or sharpens the manual**

| Topic | Manual | Measured (N8, Live 12.4) |
|---|---|---|
| Note overlap | "overlaps the beginning: overwritten; overlaps the end: shortened" | The newer note always survives whole. An older note is cut at the new note's start and its tail after the new note is discarded (old 26–34, new 28–32 leaves 26–28). An older note starting inside the new one is deleted. The cut shows before release (M2-11). |
| Ctrl+Shift+U | §10.5.11 "opens the settings"; §11.2.7 "applies without opening the panel" | Does not quantize; opens the Transform panel on the Quantize tool. Ctrl+U quantizes (M6-15). |
| Humanize | §10.5.9.7 a quarter grid step; §8.5.2 half | ±1/4 grid at 100 %; default 10 %; some note lengths also changed, unexplained (M6-15). |
| Follow key in the editor | §41.11–12 print Ctrl+Shift+F | Ctrl+Shift+F toggles the note filter bar; Follow is Alt+Shift+F (M2-19). |
| New warp marker | "if no markers follow, the clip's tempo also changes" | Inserting a marker changed no tempo (M6-12). |
| Auto-warped long sample | a marker on each bar | Only a start marker (M3-07); no fixed short/long threshold fits. |
| Audio Envelopes tab | always listed | Shown only while Warp is on (M6-07). |
| Volume clip envelope | "relative percentage" | Scales fader position, not dB: 50 % gives −17.0 dB at a 0 dB fader (M6-07). |
| Device clip envelope | "relative" | Bipolar, ±50 % of knob travel, added to the knob (M6-07). |
| Transposition envelope | follows the envelope | In Beats mode, no effect inside one transient segment (M6-07). |
| Tuning: frequency on hover | pitch and frequency in the Status Bar | Hovering a ruler key showed no frequency (M6-02). |
| Tuning: Scale choosers | hidden whenever a tuning is loaded | Reported hidden "for non-12 tunings", keys drawn uniform, Fit to Scale greyed (M6-02). |
| Crop MIDI | silent on crossing notes | Loop on: crossing notes are cut at the loop edges and expression is re-based. Loop off: a note crossing the end marker is kept whole (M2-10). |
| Slip key | §6.7 Ctrl+Shift; §41.16 Shift+Alt | Ctrl+Shift, Alt unsnaps; unwarped audio does not slip (M2-06). |
| Enter + arrows resize a MIDI clip | Live's Info View text | Not observed (M2-20). |
| Beats loop mode default | not stated | Back-and-Forth (M6-13). |
| Imported channel bend/pressure | not stated | Become clip envelopes, not per-note expression (M2-10). |

**The manual against itself (unmeasured)**
- Tab switching: §8.1.3 says Alt+Tab (Win) / Option+Tab; §41.10 says Ctrl+Tab (Win) / Option+Tab. Parity row 7 uses "Alt/Option+Tab"; on Windows Alt+Tab belongs to the system.
- MPE tab key: §12.1 says Alt+3; §41.10 says Alt+Shift+3 (Alt+3 is "focus the Clip View").
- Note navigation: prose says Ctrl (Win), the shortcut table says Alt.
- Chance by keys: §10.5.13 says plain Up/Down; §41.12 says Ctrl+Alt+Up/Down.
- Delete envelope: Ctrl+Delete (§41.7) or Ctrl+Backspace (§25.3, §26.1).
- Marker survival on sample replace: "exact same length" (§8.12) or "at least as long" (§5.4.6).

**Our parity rows and plan against Live**
- **Row 8, Set buttons:** ADI's take the arrangement's insert marker. Live's take the current playhead during playback, rounded to global quantization, and Set Loop Position also switches the loop on.
- **Row 23:** "overwrites the old note's start" reads like a trim; measured, the old note is deleted, and a covered-by-old case discards the tail.
- **Row 33:** "note.delete outside the loop" misses the cutting of crossing notes, the loop-off rule and the re-basing of expression and envelopes.
- **ED-04:** says audio Crop "writes no file". The manual's Sample Editor command, Crop Clip Sample, writes a new sample; both commands share Ctrl+Shift+J. Worth a ruling on whether ADI has both.
- **MI-07:** lists "do generators replace all notes or only overlapping ones" as open; M §11.1 states overlapping only.

**PLAN items marked "NOT RUN" that the 10 Nov notes appear to have measured**
- MI-02: the four overlap cases (M2-11).
- MI-05: Humanize bound and the Ctrl+U question (M6-15).
- MI-06: the scale census (M6-01, with a table).
- ED-04: crop of crossing notes, left trim at the loop start, left trim at a warp marker (M2-10).
- ED-05: slip modifier and unwarped behaviour (M2-06).
- AU-06: volume as fader position, device +50 %, the ±48 clamp, an unlinked loop (M6-07).
- TM-WT-02: marker tempo limits, 5 and 999 BPM (M6-12).

The director's Ableton research rows were not updated with these either: every Clip View row in them is still "documented, unmeasured".

---

## 5. Not opened or not reached

- **Measured notes:** read by a sub-reader, not by me. Notes M2-07, M2-08, M2-09 and most M1, LK, M4 and DV notes were only grepped or summarised.
- **The director's Ableton research:** no archive opened; the capture logs, the saved Live Sets, the recordings and all but about seven screenshots untouched; the parts of the research that are not about Ableton were not opened.
- **Manual:** figures are absent from the text, so icon-only items (the ten Follow Action names, Quantize tool labels, automation shapes) are inferred. Chapter 9.1 (tempo leaders) and chapters 11, 13–16, 19, 21, 25, 40, 41 came through a sub-reader; chapter 3 was read by passages.
- **Checklist (`reference/DOCS/_checklists/Ableton_live12.md`):** it has no row ids. It has 11 rows for §10.8, several with mismatched quotes, no rows for the twelve transformations and none for the shortcuts chapter. Only the multi-clip and MIDI Tools rows were read in full.
- **ADI side:** `ui_clipview.hpp` read through its comments on `window/clip-view-trim` only, not the implementation; the chords and lines branches were not read separately. PLAN cards TU-03, TM-SV-03/04, WIN-08 and the comping cards were read by their headline line only. I did not confirm whether PLAN covers sample replacement by drop.
- **Nothing was written** to the research or to the manual text.
