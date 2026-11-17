<!-- Written on 16 November 2026 beside live-clip-view-audit-2026-11-16.md (ADR-0242 d5): the parameters of each of Live 12's MIDI Tools, from the Live 12 manual, chapter 11. Nothing here is measured. -->

# Live 12 MIDI Tools: every transformation and generator, with its parameters

From the Live 12 manual, chapter 11 "MIDI Tools", read in full by a sub-reader of the Clip View audit of 16 November 2026. Each group is headed by its manual section; every line under it comes from that section. Shortcuts are Windows / Mac as printed. "[flag]" marks a conflict or a gap in the manual itself. The audit's rows C58 to C62 point here.

**ch 11 "11. MIDI Tools" (intro)**
- MIDI Tools are scale-aware utilities reached via the Transform and Generate panels of the Clip View.
- Transformation Tools modify note properties of existing notes, including MPE data; they require a selection of notes as input (11.1 adds fallbacks).
- MPE MIDI Tools are a subset of Transformations (Glissando, LFO); they add note expression or transform existing expression data; results are viewed in the MIDI Note Editor's MPE view mode.
- Generative Tools use no input; they add new notes to the clip loop or time selection. The intro says generated notes replace existing ones; 11.1 refines this to overlap only.
- Two classes: native tools (built in, properties not editable) and Max for Live MIDI Tools (bundled ones editable; you can build your own or install third-party).
- Saved or built Max for Live MIDI Tools are found in the browser via the MIDI Tools filter group or the MIDI Tool tag in the Content filter group.

**ch 11 "11.1 Using MIDI Tools"**
- Workflow: open the Transform or Generate panel, pick a tool in the Transformation/Generator Selector chooser, adjust its settings.
- Auto Apply button: active by default for all MIDI Tools; notes are transformed or generated immediately on any parameter change and shown in the MIDI Note Editor straight away.
- Toggling Auto Apply off restores the notes to their original state; you then tune parameters and press the Apply button.
- Apply the current MIDI Tool without leaving the MIDI Note Editor: Ctrl Enter / Cmd Enter.
- Transformations apply to the time selection, the note selection, or the clip loop when there is neither; transformed notes replace the existing notes.
- Generators apply to the time selection, or the clip loop when there is none; generated notes sit alongside existing notes where they do not overlap and replace them where they do.
- Scale awareness: if a scale is enabled for the clip, every pitch-related tool parameter uses scale degrees instead of semitones.
- Undo/Redo (Edit menu) undo and redo the note changes only, not changes to tool parameters.
- Reset button restores the selected tool's parameters to default; it is greyed out when already default; it has no effect on the notes.
- Multi-clip behaviour of MIDI Tools: not stated anywhere in ch 11 [flag].

**ch 11 "11.1.1 Using Max for Live MIDI Tools"**
- Max for Live MIDI Tools are AMXD files used in the Transform or Generative tab/panel; they are edited or created in a Max patcher like any Max device.
- Two are built in with Live Standard and Suite: Velocity Shaper and Euclidean.
- Suite, or Standard with the Max for Live add-on, can edit and build own tools or use third-party ones.
- Third-party tools appear when saved in `~/Music/Ableton/User Library/MIDI Tools/Max Transformations` or `.../Max Generators`, or in any folder within Places.

**ch 11 "11.2.1 Arpeggiate"**
- Splits the note selection into arpeggiated notes; uses the core of the Arpeggiator MIDI effect.
- Style drop-down: the same 18 style patterns as Arpeggiator.
- Distance: transposition of steps in the pattern, in scale degrees or semitones depending on the clip scale.
- Steps slider: number of transposed steps.
- Rate: rate of the pattern (also affects note length).
- Gate: note duration; below 100% shortens, above 100% lengthens.

**ch 11 "11.2.2 Chop"**
- Divides selected notes into a maximum of 64 parts; can design division patterns (some notes extended relative to others, random variation of start/end times).
- Parts: 2 to 64.
- Gaps: positive value = number of notes after which a gap is added (2 = a gap replaces a note after two successive notes); negative value = number of gaps added after each note (-2 = two gaps after each note); min/max depend on Parts.
- A pattern has at most 16 parts; if Parts is higher, the pattern repeats.
- Pattern toggles: visualise the pattern and let you add/remove gaps manually; after a manual change the Gaps control shows a dash; adjusting Gaps then replaces the manual gaps.
- Emphasis toggles: choose which notes or gaps are emphasised; emphasised elements are stretched by Stretch Chunk(s), 2 to 8 times longer; their Pattern toggles are greyed out.
- Variation slider: random variation of note start and end times.

**ch 11 "11.2.3 Connect"**
- Generates new notes that fill the gaps between existing notes; placement is randomised.
- Spread: maximum random pitch shift of connecting notes relative to the original pitches (scale degrees or semitones).
- Density: percentage of the time interval between original notes to fill; 100% fills all gaps.
- Rate: length of the interpolated notes.
- Tie: probability that a generated note is extended to the next original note.

**ch 11 "11.2.4 Glissando" (MPE)**
- Connects the pitch of one note to the next along a pitch bend curve, tying the notes; needs at least two selected notes.
- Start: starting point of the bend curve as a percentage of note length.
- Curve: shape of the bend curve.
- Display: drag the yellow breakpoint left/right to set Start; drag the line up/down to set Curve.
- The curve is only visible in the MPE Editor; in the MIDI Note Editor the transformation applies but is not shown.
- With folded notes, the curve can be shown below the editor by activating the Pitch Bend expression lane.

**ch 11 "11.2.5 LFO" (MPE)**
- Sets the value of one of three MPE parameters with a low-frequency oscillator.
- Target chooser: Pitch Bend, Slide, or Pressure.
- LFO curve display shows the modulation shape.
- Envelope Attack and Decay sliders: amplitude envelope relative to total oscillation length; they influence each other (Attack 100% resets Decay to 0%).
- Shape control adjusts the shape of the type chosen in the Shape Type chooser: Sine, Square, Triangle, Random.
- Reseed button (Random only): generates new random shapes.
- Rate: oscillator period in musical time, 1 to 1/128 (1 = 4 beats).
- Time Shift slider (below Rate): positive delays the start, negative adjusts phase.
- Amount: amplitude. Pitch Bend target: any value within double the clip's pitch bend range (range ±60 st gives -120 st to 120 st). Slide/Pressure: -127 to 127.
- Amplitude Base slider (below Amount): base value. Pitch Bend: within the clip's pitch bend range (-60 st to 60 st at ±60). Slide/Pressure: -127 to 127.
- Pitch Bend curve shows in the MIDI Note Editor or in the Pitch Bend expression lane; Slide and Pressure curves show in their lanes; all only in the MPE Editor.

**ch 11 "11.2.6 Ornament"**
- Adds ornamental notes to the beginning of selected notes; choose Flam or Grace Notes with their toggles; reapplying to the same selection adds more.
- Flam: one note added at the start of each selected note.
- Flam Position: positive = flam replaces the beginning of the original note; negative = flam is prepended; value is a percentage of the current grid, so 100% / -100% equals one grid division, shorter towards 0%.
- Flam Velocity: velocity of flam notes relative to the originals.
- Grace Notes: several equal-length notes added at the start of each note.
- Grace Notes Pitch buttons: High (every other grace note one semitone or scale degree above), Low (one below), Same.
- Grace Notes Position: positive replaces the beginning, negative prepends; percentage of grid; 100% / -100% fills one grid division.
- Grace Notes Velocity: relative to the originals.
- Grace Notes Chance: probability each grace note plays, relative to the original note's Chance.
- Grace Notes Amount: number of grace notes per selected note; always equal in size.

**ch 11 "11.2.7 Quantize"**
- Moves or stretches selected notes; an equivalent Quantize tool exists for audio clips.
- Quantise to the current grid size or a specific meter value, including triplets.
- Quantise note start, note end, or both; quantising the end stretches the note to end at the subdivision.
- Amount: moves notes only by a percentage of the quantisation value.
- Also opened via the Quantize Settings… command in the Edit menu.
- As printed here: Ctrl Shift U / Cmd Shift U "to apply quantization to the selected notes without opening the Transform tab/panel". [flag] ch 41 lists Ctrl U / Cmd U = Quantize and Ctrl Shift U / Cmd Shift U = Quantize Settings….
- The control labels of the Quantize panel are only described, not named, in the text.

**ch 11 "11.2.8 Recombine"**
- Rearranges position, pitch, duration or velocity across the selected notes.
- Dimension chooser: Position, Pitch, Duration, Velocity.
- Shuffle toggle: random permutation; a new one is created each time Apply is pressed.
- Mirror toggle: reverse order.
- Rotate: circular permutation; takes effect when Rotation Steps is not 0.
- Rotation Steps: set by click-dragging across the columns in the display or with the Rotate Step Down/Up buttons; available steps = number of selected notes minus one; positive = clockwise, negative = counter-clockwise.
- Rotate on Grid toggle (Dimension = Position only): bases the available steps on the number of grid cells in the selection; depends on grid settings.
- Types can be combined; order of application is Shuffle, then Mirror, then Rotate.

**ch 11 "11.2.9 Span"**
- Transforms note durations with three articulation types: Legato, Tenuto, Staccato.
- Offset: adjusts note end times up to a grid step; positive extends, negative shortens.
- Variation: random length variation; positive shortens, negative extends; any value other than 0% gives a new variation on each reapply.
- Legato: extends each note to the start of the next; the last note extends to the end of the time selection, or the loop end if none.
- Tenuto: keeps original lengths unless Offset/Variation are adjusted.
- Staccato: the smallest distance between start times is halved and used as the new length for all notes; Offset/Variation modify further.

**ch 11 "11.2.10 Strum"**
- Adjusts start times of notes in a chord along a shape set by Strum Low, Strum High and Tension.
- Strum Low: offset starting with the lowest note; positive moves starts forward, negative back; lowest note offset up to one grid step at 100% / -100%; other notes proportional to the Strum Position display.
- Strum High: the same, starting with the highest note.
- At least one of Strum Low / Strum High must differ from 0.0% for any effect.
- Both can be set by dragging their breakpoints in the Strum Position display or by typing values.
- Tension: 0.0% = equal distances; positive = larger distances at the start, decreasing exponentially; negative = shorter at the start, increasing exponentially.

**ch 11 "11.2.11 Time Warp"**
- Time-stretches selected notes along a speed curve (accelerando, ritardando).
- The curve is made in the Breakpoints display; its time range maps to the time selection.
- One to three breakpoints, each enabled by its toggle.
- Drag a breakpoint, or select it and use the Breakpoint Time and Breakpoint Speed sliders (they always show the selected breakpoint).
- Quantize toggle: warped notes are quantised to the grid.
- Preserve Time Range switch: results fit the same range as the original selection.
- Include Note End switch: note ends follow the curve too, changing durations.

**ch 11 "11.2.12 Velocity Shaper" (Max for Live Transformation)**
- Shapes velocities of selected notes with an adjustable envelope; click in the display to add breakpoints, drag to shape.
- Minimum and Maximum Velocity (right of display): velocity range for the result.
- Loop: number of times the envelope is applied across the selection.
- Rotate: number of steps the envelope is offset from the selection start.
- Division: size of a Rotate step (e.g. "Grid").

**ch 11 "11.3.1 Rhythm"**
- Generates a note pattern on one pitch or drum pad, repeated to fill the time selection.
- Pitch control, or Alt / Option-click on the piano ruler, picks the pitch or pad.
- Steps: number of steps, up to 16.
- Pattern knob: placement of notes; number of available patterns depends on Steps and Density.
- Density knob: number of notes; maximum is the Steps value.
- Step Duration slider: sets how often the pattern repeats in the selection (one bar, Steps 8: 1/8 = once, 1/16 = twice); it also limits the maximum Steps.
- Split: probability that a step is divided in half.
- Shift: moves notes by steps, positive right, negative left.
- Velocity and Accent sliders: velocity for normal and accented notes.
- Accent Frequency: number of notes between accents, 0 to the Density value; 1 = every note accented.
- Accent Offset arrows: shift the accent placement.
- To add another voice: deselect the generated notes and adjust again for a different pitch or pad.

**ch 11 "11.3.2 Seed"**
- Randomly generates notes within pitch, length and velocity ranges.
- Pitch range: Minimum and Maximum Pitch (or Key Track) sliders, or the triangular handles of the Pitch Range slider.
- Dragging one handle onto the other merges them (single pitch); click in the slider or set different values to split again.
- Alt / Option-click the piano ruler for a single pitch; click-drag for a range.
- The pitch slider is purple when the clip has an active scale.
- Duration Range slider: 1/128 note to one note; Velocity Range slider: 1 to 127; both behave like the Pitch Range slider.
- Voices: maximum simultaneous notes.
- Density: number of generated notes as a percentage of the pitch range populated.

**ch 11 "11.3.3 Shape"**
- Generates a note sequence following a shape in the display.
- Shape Presets drop-down, or draw your own shape in the display.
- Minimum and Maximum Pitch sliders; or hold Alt / Option and click-drag to select a range.
- The shape is purple when a clip scale is active.
- Rate: minimum note length.
- Tie: probability a note extends to the next note.
- Density knob: number of notes as a percentage of the shape populated.
- Jitter: pitch randomisation; 0% follows the shape exactly; results stay inside the min/max pitch range.

**ch 11 "11.3.4 Stacks"**
- Adds single chords or chord progressions within a selected scale; fills the time selection or the loop.
- Chord Selector Pad: click-drag, or Ctrl / Cmd with up/down arrows, to pick a chord pattern; diagrams follow the Tonnetz system; hovering shows chord info in the Status Bar.
- Custom chord banks: JSON text files with the `.stacks` extension, placed in a Places folder; double-click in the browser to replace the default bank; found via the Stacks tag in the MIDI Tool filter group.
- Add Chord (+) button to the right of the pads adds a chord; Delete Chord (−) removes one.
- Chord Root knob; or Alt / Option-click the piano ruler; or up/down arrow keys to cycle roots.
- With a clip scale, Chord Root is set to the scale root automatically and root choices are limited to notes in the scale.
- Chord Inversion knob: positive cycles the inversions, negative gives the same inversions an octave lower.
- Chord Duration and Offset: length and position of the chord, in eighths of the original chord length.
- The display shows the parameters of the currently selected chord only.

**ch 11 "11.3.5 Euclidean" (Max for Live Generator)**
- Generates Euclidean rhythms for up to four voices, in the time selection or the loop.
- Pattern tab: pattern visualisation; per-voice activation toggles; per-voice Rotation sliders (offset from the start of the time selection); a randomisation button in the middle sets all Rotation values at random.
- Voices tab: per-voice activation toggles; per-voice pitch (instruments) or drum pad (Drum Racks); up/down arrows left of the toggles change all pitches/pads together; per-voice Velocity sliders.
- Steps: pattern length; a shorter pattern repeats and may wrap around the time selection.
- Density: as printed, "the number of times the pattern is repeated within a time selection"; notes wrap if it does not fit.
- Division: length of a step.
