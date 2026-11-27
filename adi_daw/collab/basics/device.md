# Basics device

The Windows 10 walker's queue (`collab/prompts/2026-11-27-basics-device.md`): the audio device and the
files a user drops in, on a real Windows machine. One chain branch, `basics/device-chain`, from
origin/main; every item a commit with its SC id. Release builds with warnings as errors, 2 jobs beside
another session. Before each push: the five validators, the headless suites, the window suites on a
hidden desktop, and builds of `adi_app` and `adi_play`. Rows marked "at the desk" are run only while the
director has written that the desk is free.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-379 | `basics/device-chain` 12a065e7 | The driver's mode is kept: Settings take "WASAPI Exclusive" and "WASAPI Low Latency" beside "WASAPI", and the audio page writes the mode JUCE opened, so Exclusive comes back as Exclusive. | `test_settings` testDriverModeKept. At the desk: Windows Audio (Exclusive Mode) chosen on the Audio page, written as "WASAPI Exclusive", and shown again after a restart | pushed; desk passed |
| SC-378 (with SC-28) | `basics/device-chain` 0ba3a5d0 | An unplugged interface keeps the user's choice: ADI plays through the system device and says so, and opens the chosen one again when it is back. A pure rule, `deviceChange` (Keep, Hold, Return). | `test_settings` testDeviceUnpluggedAndBack. At the desk: no USB interface here; disabling the VB-Cable device stood in badly (Windows will not disable a device in use, it waits for a reboot), and its flag was put back | pushed; desk test deferred (the director, 27 Nov): no USB device at this desk |
| SC-376 | `basics/device-chain` c67e376a, a1980b33 | M4A and AAC play on Windows 10: the encoder probe asks for 192 kbit/s (the only rates the Windows 10 encoder takes are 96 to 192), an MP4 one tick short of its samples is accepted, and the packets go to Media Foundation with no priming edit (Windows 10 played the priming, 1,024 frames late) while ADI trims it. | `test_aac_import` against FFmpeg's independent decodes, `test_mp4_aac`, both on this Windows 10 | pushed |
| SC-377 | `basics/device-chain` 230111de, e8218e5e, 1c88590d | An hour-long file dropped in is read off the window on a worker, with progress by name ("Reading … (i of n)"), Esc cancelling; the worker opens the project read-write as the material import does. | `test_ui_shell` plainDropReadsOffTheWindow | pushed |
| SC-405 | `basics/device-chain` cbafaaaa | Every switch in the MIDI ports list does something, or is greyed with a tooltip saying why. | `test_ui_shell` MIDI ports checks | pushed |
| SC-380 | `basics/device-chain` a7a470a1, 3b457524 | Hardware Setup on the audio page opens the ASIO driver's own control panel, then reopens the device. | `test_ui_shell` audio page check. At the desk: off with Windows Audio and with no device open, on with Voicemeeter Virtual ASIO open; pressed, nothing showed, as Voicemeeter itself was not running; ASIO4ALL did not start on this PC | pushed; desk partly (a driver's panel not yet seen) |
| SC-381 | `basics/device-chain` 397f9080 | No input is opened until one is wanted: a first run opens output only (Bluetooth headphones stay in stereo), and arming a track opens the default input. | `test_ui_shell` firstRunAudioInput. At the desk: a first run showed No Device for the input; arming an audio track said the system's input was open for it, and the setting stayed empty | pushed; desk passed (no Bluetooth headset here) |
| SC-382 | `basics/device-chain` 4cc7e4fc | A sample-rate change keeps the song where it was: the position is located by time. | `test_session` testRateChangeKeepsThePlace | pushed |
| SC-383 | `basics/device-chain` 975f00fe, 3b457524, 1c88590d | "No Device" for the audio input is kept and stays off. | `test_ui_shell` audio page check | pushed |
| SC-384 | `basics/device-chain` fa156fc5 | A rate or buffer ADI cannot use (outside 44.1 to 768 kHz, over 4096 frames) is said in a plain sentence in the status bar. | `test_ui_document` unusableFormatSaid | pushed |
| SC-385 | `basics/device-chain` fa08362e | Audio Engine On/Off, Ctrl+Alt+Shift+E as in Live: the engine stops and the interface is free, and comes back. | `test_ui_shell` stable id; the three key map goldens | pushed |
| SC-387 | `basics/device-chain` d6e0bea5, 3b457524 | A file with more than two channels is refused in plain words, by its name. | `test_media_ops` moreThanTwoChannelsRefused | pushed |
| SC-388 | `basics/device-chain` 462d6836 | A sample at a path over 260 characters imports: the app and the media suite are long-path aware. | `test_media_ops` longPathImports | pushed |
| SC-389 | `basics/device-chain` 4f150024, 1c88590d | One bad file among ten no longer stops the drop: the nine come in and the bad one is named. | `test_ui_shell` plainDropReadsOffTheWindow | pushed |
| SC-390 | `basics/device-chain` bd215f3d | A file ADI cannot read says why in plain words by its name (a folder, damaged, not audio), never the decoder's words after its full path. | `test_media_ops` plainWordsForUnreadable | pushed |
| SC-391 | `basics/device-chain` 7f883d11, 543beae7 | A download cut short plays as far as it goes, and a WAV whose size fields a crashed recorder left at zero reads to the end of its data. | `test_decode` (truncated FLAC plays; the crashed recorder's 4800 frames); refusals checked with a FLAC holding no audio | pushed |
| SC-428 | `basics/device-chain` 72cd6bda | A plug-in window reopens wholly on a screen that exists: moved, never resized, onto the monitor it stands on most. | `test_ui_window_state` whollyOnScreen | pushed |

## For the director
- The landing check ran at the chain's head (every item in it), not once per item: a full build here is
  several hours at 2 jobs, and the items touch none of the JUCE modules that take that time.
- **Deferred, the director's word of 27 November ("skip"):** SC-378's desk test needs a real interface
  unplugged while ADI plays through it and plugged back, and this desk has none. It is owed for later, on
  the ADI studio PC with its interface, or by the Mac session on a Mac with a USB headset or interface, if
  that is sooner (the rule, `deviceChange`, is the same on both; the device-change messages are the
  platform's). The test: choose the interface as output, unplug it while playing (the Status Bar says it
  is not connected and what ADI plays through; the setting keeps the interface), plug it back (ADI opens
  it again and says it is back). **For win, a register row:** DF-E (next free) | SC-378's unplug and
  plug-back at a real desk | the studio PC or the Mac session | before SC-378 is closed in the sanity check.
- SC-380's panel is owed the same way: an ASIO driver whose own panel opens (the studio PC's interface).
  ASIO4ALL does not start here, and Voicemeeter's shows nothing unless Voicemeeter runs. A Mac cannot do
  this one: Core Audio drivers have no panel of their own.
- The VB-Cable device was disabled for the SC-378 try (with your approval); Windows held the disable for
  a reboot, and its flag was set back, so it stays enabled.
- The landing check at the chain's head (1c88590d): the five validators, the build of every suite,
  `adi_app` and `adi_play` with warnings as errors, and 296 of 297 suites pass. `adi_builtin_realtime_tests`
  (labelled performance) misses its latency deadlines on this PC, alone too (24 checks, all p99 or spike
  timings of the built-in devices) while another session builds beside it. The chain touches no device or
  DSP code; `session.cpp` changes only on a sample-rate change. Not proved against origin/main here (a
  full build of main is hours); CI's run of the suite is the one to read.
- Not in this queue: SC-393.
