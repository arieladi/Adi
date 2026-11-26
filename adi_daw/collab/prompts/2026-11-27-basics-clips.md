# basics-clips — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/clips.md` on
`basics/clips-log`.

**Your part:** audio clips and the meters. You are the session that walked the real window on Windows
10 (the walker of `collab/walk/`). The director's standing rule holds: you drive the real window on that
machine only while he has written that the desk is free. Everything else you build and prove headless.
Another session shares the machine: build with 2 jobs.

## Queue, in this order

1. **Your own DF-W56 (B13):** the default set's track names renumber as Live's.
2. **SC-11: an unwarped clip after a tempo change.** Read as suspect, not run: a dropped clip's length
   is stored in ticks (`src/adi/ui/audio_drop.hpp`, about 133) and its end is computed from ticks
   (`src/adi/engine/clip_playback.cpp`, about 590). Drop a file, raise the tempo, play: Live keeps the
   whole sample. Write the test, then the fix if it fails.
3. **SC-30: trimming a clip's start.** The engine has `edit_plan::prepareTrim`; the window hit-tests the
   right edge only (`src/juce/ui_arrangement.cpp`, about 3220).
4. **SC-31: clip gain.** `clip.setGain` exists and plays, and the window has no control. Give the audio
   clip its Gain in the Clip View as Live, and draw the waveform's height by it in the lane.
5. **SC-32: Warp on and off** on a clip already placed (`clip_edit::prepareSetWarp` is built). The
   switch in the audio clip's Clip View, as Live.
6. **SC-36: waveform detail.** One resolution today, 4096 bins per file
   (`src/adi/ui/project_document.cpp`, about 54; `src/adi/media/peaks.hpp`), so a long file is drawn as
   blocks when zoomed in. Peaks at several resolutions, or read from the file for the visible range,
   built on the worker and never on the message thread's frame. The Sample Editor uses the same.
7. **SC-81: meters that show left and right.** The engine publishes one peak and one RMS per strip
   (`src/adi/engine/mixer.hpp`, about 146) and the window draws two equal bars
   (`src/juce/ui_mixer.cpp`, about 964). Publish each channel; the audio thread still allocates nothing
   and takes no lock.

## After these (wave 2)

SC-33 (reversing a clip: today it is silent), SC-35 (a file dropped onto a clip replaces what it
covers), SC-39 (a looped clip's repeats marked).
