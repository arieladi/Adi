# basics-safety — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/safety.md` on
`basics/safety-log`. Each row's detail and its code pointers are in `docs/SANITY-CHECK.md`.

**Your part:** whether a musician can trust ADI with his work: the project file's safety, undo steps
and notices. The cloud Claude session takes this queue after its project queue.

## Queue, in this order

1. **A project stays whole in a synced folder (SC-159; PD-08 moves up, ADR-0258 d7).** Round 2's probe:
   while a project is open its newest work can sit only in the write-ahead file, and the project file
   copied alone had lost it. This lands before the default projects folder moves under the user's
   documents (SC-100).
2. **A second copy exists (SC-157).** No backup is ever written today, and a project file short of one
   page cannot be opened. Live keeps a Backup folder, and ADR-0255 d6 ruled it on by default (PLAN
   Q-A2 (a)). Build that: a copy at open and on a schedule, in the song's folder, with a way to open
   one from the window.
3. **Whatever cannot play says so (SC-151).** A missing file, a missing plug-in or a clip the engine
   dropped is silent with no word: the engine gathers every case in `Session::problems()` and nothing
   in the window reads it. Show them: a mark on the clip or device, and one line in the Status Bar that
   opens the list. SC-40 and SC-89 are two cases of this.
4. **The Undo History's bottom row** returns to the project as it was opened, not to its creation (the
   round's row on the history spanning every session).
5. **One gesture is one Undo:** a click-and-drag on an automation line, Split over several clips, and a
   drop with Warp, fades or copy ticked are each several steps today.
6. **By ADR-0258:** an older project is copied before it is upgraded (d9, SC-169); Save a Copy and the
   collected export leave the history out, with a box to keep it (d8, SC-172).

## After these

The other wave 2 rows of the safety queue in `docs/SANITY-CHECK.md`, in their order.
