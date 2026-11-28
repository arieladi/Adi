# basics-device — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first (it changed on 27 November: one chain branch per
queue). Your report is `collab/basics/device.md` on `basics/device-log`; your work is one chain branch,
`basics/device-chain`, from origin/main. Each row's detail and its code pointers are in
`docs/SANITY-CHECK.md`, part "Audio device and file formats".

**Your part:** the audio device and the files a user drops in, on a real Windows machine. You are the
Windows 10 walker: this queue needs what no Linux session has, a real audio stack, a real interface,
real driver types. Build and prove headless where a row allows it; the rows marked "at the desk" need
the real window and the real device, and you do those only while the director has written that the desk
is free. Another session may build on the machine: 2 jobs.

## Queue, in this order

1. **SC-379: the driver type is remembered.** Windows Audio Exclusive and Low Latency are both saved
   under one name and reopen as shared mode (`src/juce/ui_main.cpp`, about 1403 and 1080). At the desk:
   choose each, restart, read what opened.
2. **SC-378, with SC-28: an interface unplugged and plugged back.** Read, not run: the system's default
   is opened by itself, ADI saves it over the chosen device, and nothing returns to the interface. ADI
   must keep the user's choice, say what happened, and come back when the device does. At the desk, with
   a real USB interface if one is connected; otherwise disable and enable the device in Windows.
3. **SC-376: an M4A or AAC file plays.** On this machine every such file is refused as "native AAC codec
   unavailable", though Windows has the decoder; ADI's own self-test fails
   (`src/adi/audio/aac_windows.cpp`, about 65), and the two AAC suites print "unavailable" and pass. Find
   why the self-test fails here, fix it, and make the suites fail when the codec is missing on a machine
   that should have it.
4. **SC-377: an hour-long file dropped in leaves the window alive.** Hashed twice and decoded on the
   message thread today: 8.8 s for a 2 GB file in a Release build, longer in Debug. Move it to the
   worker with a progress line and a way to cancel.
5. **SC-405: every switch in the MIDI ports list does something,** or is not a switch. Sync and Remote on
   every port, and all four on every output row, toggle and nothing reads them.
6. **SC-380: the ASIO driver's own control panel** opens from the audio settings.
7. **SC-381: Bluetooth headphones.** Read: a first launch opens the system's default input beside the
   output, and when that input is the headset's own microphone Windows drops the headphones to
   telephone quality. To run at the desk if a headset is paired; otherwise make the first launch open no
   input until the user arms a track or chooses one, and say so.

## After these (wave 3)

SC-382 (a sample rate change keeps the song's position), SC-383 (No Device stays off), SC-384 (a rate or
buffer ADI cannot use is said plainly), SC-385 (the audio engine switched off and on), SC-387 to SC-391
(files with more than two channels, long paths, one bad file among ten, plain words for an unreadable
file, a download cut short). SC-393 is Linux's and not yours.

## One more, from your own walk

A plug-in's window can reopen partly off the screen: it reopens at its saved place at full size. Keep it
on a screen that exists (the row will be SC-428; win registers it).
