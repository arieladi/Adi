# basics-midi — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/midi.md` on
`basics/midi-log`.

**Your part:** getting notes in and hearing them. The Note Editor itself is in good shape; the gaps are
around it. You run on a Windows machine with MSVC and a virtual MIDI port, so notes can be sent into ADI
from a script.

## Queue, in this order

1. **SC-50: recording MIDI into a clip.** Today Record refuses on a MIDI track
   (`src/adi/ui/project_document.cpp`, about 1222) and the engine has only the loop commit
   (`recording::prepareMidiLoopCommit`). Build the plain take: the engine's linear MIDI commit, and
   Arrangement Record on an armed MIDI track making one clip, one undo step, as Live. **The notes appear
   in the growing clip as they are played**, as the audio waveform does (SC-20). No plan row owned this:
   say in your report what you named the engine's parts.
2. **SC-10: the Monitor switch on a MIDI track.** Live notes follow Arm alone today
   (`src/adi/engine/session.cpp`, about 960-973). In, Auto and Off must mean what they mean in Live.
3. **SC-52: a track that plays as soon as it has an instrument.** *Reading owed:* whether Live arms a
   MIDI track when an instrument is loaded or the track is selected, and whether its MIDI input ports
   start with Track on. Build both as switchable defaults (`record.autoArmOnSelect` exists and nothing
   reads it; the ports start off in `src/adi/settings/registry.cpp`), and set them as Live once win
   passes the reading.
4. **SC-51: notes from the computer keyboard** (Live's M key and its key rows; WIN-05's card). It is a
   placeholder in `src/juce/ui_controlbar.cpp`.
5. **SC-53: hearing a note that is clicked, drawn or moved, and the piano keys.** No engine path plays
   a note into a track that is not armed: build that preview path first, then the window's side in
   `src/juce/ui_clipview.cpp`. The key lights while its note sounds, in playback too.
6. **SC-55: the sustain pedal, pitch wheel and mod wheel.** OneShot and Sampler ignore them
   (`src/adi/dsp/oneshot.cpp`, `sampler.cpp`; an ordinary keyboard's bend arrives with note id 0 and
   matches no voice, `src/adi/engine/mpe_input.cpp`). Then prove all three into one VST3 and one CLAP
   instrument through the virtual port, and report what each received.

## After these (wave 2)

SC-57 (a .mid file dragged in; a clip exported as one), SC-58 (the Capture MIDI button: the engine is
built), SC-60 (overdub and record quantization on top of SC-50), SC-54 (Space in the Note Editor plays
from where the user clicked; *reading owed*).

## Added by round 2 (26 November)

- **First of all, SC-220: a new MIDI clip loops.** Today its Loop switch is off, so a clip pulled
  longer plays once and then silence (`src/adi/ui/clip_edits.hpp`, about 46).
- **With SC-50, three things its row now carries:** a MIDI track and an audio track armed together both
  record (today Record says "Recording." and drops the notes, `project_document.cpp` about 1222); a
  note lands where it was heard when the track monitors through ADI and where it was played when it
  does not; the take keeps per-note expression.
- **The Note Editor's grid goes below a quarter note (SC-71's part in the editor).** Every drawn note
  is a beat long today and Ctrl+U quantises to beats. A fixed 1/16 with the narrower and wider keys is
  enough until the reading of Live's own steps arrives.
- **A dragged note lands on the grid lines** (the round's row: drags move by whole steps from where the
  note was, so an off-grid note stays off-grid; it bites every recorded note once SC-50 lands).
- **T is answered (ADR-0258 d17):** a MIDI track with no instrument stays silent, as Live. SC-62 is
  closed.

Your second queue, when this one is empty: `2026-11-27-basics-automation.md`.
