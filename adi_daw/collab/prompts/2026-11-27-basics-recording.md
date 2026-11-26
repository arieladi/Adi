# basics-recording — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/recording.md` on
`basics/recording-log`.

**Your part:** recording audio as every DAW does it. You run on a Windows machine with MSVC. It has a
virtual audio cable and a virtual MIDI port, so a take can be recorded and compared with what was fed.
Another session shares this machine: build with 2 jobs.

## Queue, in this order

1. **SC-02, SC-03: a click in the lanes must not move a running transport.** Traced by the audit, not
   yet run: confirm it first with a test. The path: a lane press (`src/juce/ui_arrangement.cpp`, the
   `select` at about 1474-1492, which locates) reaches `AdiRootComponent::locate` and the mailbox
   (`src/adi/ui/window_state.hpp`). While playing or recording Live moves only the insert marker. The
   same locate cuts a take short and zeroes a count-in. Continue (Shift+Space) must return to where
   playback stopped, whatever was clicked since.
2. **SC-22: a take lands where it was played.** `ProjectDocument` commits a take with no latency offset
   (`src/adi/ui/project_document.cpp`, the `recording::prepareCommit` call; the parameter is in
   `src/adi/recording_commands.hpp`). Pass the input and output latency the driver reports, as the
   plan's MX-09 describes. Prove it through the virtual cable: a click played out and recorded back
   lands on the click.
3. **SC-20: the take's waveform while it records** (DF-E90 and DF-W58). The recorder
   (`src/adi/engine/audio_recording.*`) computes min and max columns on its writer thread, never in the
   audio callback, with no allocation and no lock there, and publishes them. The window draws them in
   the growing take (`ui_arrangement.cpp`, where #773 draws the red take). When the take lands, the
   file's own peaks take over with no jump.
4. **SC-21: the count-in.** The engine has it (`transport.setCountIn`). Give the Metronome its menu as
   Live's (Count-In: None, 1 Bar, 2 Bars, 4 Bars; the other entries only where the engine has them), and
   show the count counting in the position field as Live does.
5. **SC-23: which input a track records.** Today every armed track records inputs 1 and 2 and the
   Audio From chooser is paint (`src/juce/ui_mixer.cpp`, about 947). Make the chooser real for external
   inputs: each mono input and each stereo pair that Input Config has on. A mono input gives a mono
   take. `track.setInput` exists and has no caller. Track-to-track routing stays with MX-25.
6. **SC-24: the recorded file's name and folder.** Today `recording-<id>-<hash>.wav` in the project's
   root. As Live: a Samples/Recorded folder beside the project, and the file named after its track with
   a running number. `files.recordFolder` is a setting nothing reads: read it or strike it.
7. **SC-28: an audio device that stops says so.** The error is stored and never read
   (`src/juce/device_bridge.hpp`; the change listener in `src/juce/ui_main.cpp` returns silently). A
   line in the Status Bar and a way back to Settings.

## After these (wave 2)

SC-25 (the recording's file type and bit depth as a setting), SC-26 (punch in and out: the engine half
and the two Control Bar switches), SC-27 (recording with the loop on: the window's half is on
`window-fn/loop-record`).
