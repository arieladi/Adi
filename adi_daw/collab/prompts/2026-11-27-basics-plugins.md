# basics-plugins — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/plugins.md` on
`basics/plugins-log`. Each row's detail and its code pointers are in `docs/SANITY-CHECK.md`.

**Your part:** the plug-in host. You are the Windows 10 walker: that machine has real VST3 and CLAP
plug-ins installed, which this queue needs. Build and prove headless (a probe that drives a plug-in
through ADI's own host needs no window); drive the real window only while the director has written that
the desk is free. Another session builds on the machine too: 2 jobs. This queue comes before your clips
queue.

## Queue, in this order

1. **SC-310: a value ADI sets on a VST3 reaches its sound.** Measured by round 2: a parameter set from
   ADI's panel, by Undo, by Delete-to-default, or replayed when a project opens never reaches the
   processor of several plug-ins, and Surge XT loses it while the song plays. ADR-0073 decided the cure
   (the host hands parameter changes to the processor in the audio block, as VST3 asks) and it was
   never built. The round's probe and its numbers are described in the row; write your own test first,
   against Surge XT's VST3, then the fix in `src/juce/vst3_host.cpp`. Prove it also at project open and
   in an offline render. CLAP was read as fine: prove that with the same test.
2. **SC-311: an edit made in the plug-in's own window reaches the sound,** and an automated parameter
   moves in ADI's panel. Read as suspect for a VST3 with a separate processor: it enters the same queue
   as item 1. One run settles it.
3. **SC-312: sustain pedal, mod wheel and pitch wheel reach a VST3 synth.** Measured: dropped for Surge
   XT's VST3. VST3 has no raw controller events: the host maps them through the plug-in's own MIDI
   mapping interface. Do that; CLAP takes the events as they are.
4. **SC-313: a plug-in's window stays with ADI and does not cover other programs.** "Always on top" is
   on by default and is desktop-wide today. Make the window float above ADI's own window only.
5. **SC-375: ADI started without its saved audio device,** then a device chosen in Settings: sound
   without a restart. Read, not run: the audio callback is attached only when a project is installed
   (`src/juce/ui_main.cpp`, about 1095). The walk's A35 is the same place: the box names neither the
   device nor the cause, and the free input is dropped with the output.

## After these

The wave 2 rows of the plugins queue in `docs/SANITY-CHECK.md`, in their order. Then your clips queue
(`2026-11-27-basics-clips.md`).
