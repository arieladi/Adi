# basics-routing — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/routing.md` on
`basics/routing-log`. Each row's detail and its code pointers are in `docs/SANITY-CHECK.md`.

**Your part:** where sound and notes go, and takes. Codex on the Windows 10 machine takes this queue
after its recording queue.

## Queue, in this order

1. **The In/Out choosers choose.** Audio From and its channel, Audio To, MIDI From, MIDI To and Main Out
   are painted as choosers and take no click, in the track rows and in the Mixer View (MX-25 moves up;
   with SC-23 and SC-92). Each lists what the engine can route today and sets it; what the engine cannot
   route yet is not listed.
2. **Take lanes in the window (ADR-0258 d14; SC-252, SC-253).** The lane ops are built and the window
   draws none. A second take over the first keeps the first in a lane, as Live; loop passes (SC-27 of
   your recording queue) land in lanes that can be seen, auditioned and chosen from.
3. **MIDI out.** No output port is ever opened, so a MIDI track cannot play a hardware instrument, and
   the Settings' output switches read nothing. Open the ports the settings name and send a track's notes
   and controllers to its MIDI To.

## After these

The other rows of the routing queue in `docs/SANITY-CHECK.md`, in their order; then the device queue's
rows (the audio device lost and found again, exclusive mode remembered, a sample rate change).
