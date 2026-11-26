# basics-speed — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/speed.md` on
`basics/speed-log`. Each row's detail, its numbers and its code pointers are in `docs/SANITY-CHECK.md`,
part "Large projects and speed" (SC-407 onward).

**Your part:** ADI at the size of a real song. Round 2's last audit built a Release probe against main's
libraries and timed projects of rising size: a new project is quick, a song is not. This queue is the
engine and the document layer, headless: Linux is fine. For each item first rebuild the measurement as a
test or a benchmark that fails today, then fix, then give the numbers before and after.

## Queue, in this order

1. **Files at 44.1 kHz on a 48 kHz device play without gaps.** Measured: six such tracks play clean,
   eight lose a track now and then, and at sixteen most of the audio is missing; files at the device's
   own rate are clean at thirty-two tracks (`src/adi/engine/clip_playback.cpp`, about 280 and 679). The
   rate conversion must never make the audio thread wait.
2. **A song with hundreds of audio clips plays every one.** Each clip holds its own open file per graph
   generation and the C runtime allows 512: at 200 clips some go silent after every third edit, at 600
   all do, waveforms too (`clip_playback.cpp`, about 531). Share one reader per file.
3. **An edit in a full song lands at once.** Every edit reads the whole project again: 11 ms in a new
   project, 0.4 to 0.7 s at 950 clips, about 1 s at 2,000 clips and 50,000 notes. Space and Stop pay it
   before the transport hears them, and a fader drag pays it per mouse move (the walk's A20, cured only
   for the idle case). Rebuild only what an op touched, and keep the transport off that path.
4. **SC-194: a warped clip does not make every edit slow.** A three-minute clip costs 19 s at import
   and at every open, then 1.4 s per edit of anything. Cache the stretched result by what it depends
   on, and render off the message thread. (The clips queue names the same row: say in your report who
   takes it.)
5. **Long MIDI parts.** Moving all the notes of a clip grows with the square of their number (21 s at
   2,000 notes, minutes and gigabytes of history at 5,000): one write per gesture (MI-01's contract).
   Reading the Undo History is quadratic in journal rows too.
6. **Stop after a take gives the window back at once (SC-251).** The take's file is hashed six times
   and copied once on the window's thread.

## After these

The other rows of the speed queue in `docs/SANITY-CHECK.md`, in their order: export many times faster
than real time, a large project that shows it is loading, and the rest.
