# basics-edit — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/edit.md` on
`basics/edit-log`. Each row's detail and its code pointers are in `docs/SANITY-CHECK.md`.

**Your part:** selecting, right-click menus, the clipboard and the keys in the Arrangement
(`src/juce/ui_arrangement.cpp`, `src/juce/ui_shell.cpp`). The cloud Codex session takes this queue after
its tracks queue. Several rows wait on a reading of Live: the answers are written to
`collab/basics/live-readings.md` on `basics/live-readings-log` as they come; fetch it before each item.

## Queue, in this order

1. **SC-174: a right-click keeps what is selected.** Today it first selects again as a plain click
   does: a time selection, several clips or several tracks are lost before the menu opens, and while
   the song plays the playback jumps (with SC-02 of the recording queue: agree in your reports).
2. **SC-177: a click on a track's name selects the track and nothing else.** Read, not run: the time
   range left from an earlier clip click stays, and Delete then cuts that range on the new track.
3. **SC-175: a drag inside a clip selects from where the pointer was pressed** (confirmed by a probe:
   today from the clip's start, so Delete removes the whole clip). By ADR-0258 d12 a click in a clip's
   body places the insert marker and the clip's title selects the clip, as Live: build it when reading
   R37 confirms Live's side.
4. **SC-176: the right-click menus hold what Live's do:** on a clip Delete, Deactivate, Loop Selection,
   Crop, Consolidate, fades and its colour; on a time selection Cut, Copy, Delete, Duplicate and the
   Time commands. Offer only what works; a command that is not built is left out, not greyed.
5. **SC-394: Export with nothing selected** writes the four-bar loop brace today. Reading R36 says what
   Live does; then make ADI do the same.

## After these

The wave 2 rows of the edit queue in `docs/SANITY-CHECK.md`, in their order. The first of them: Cut,
Copy and Duplicate take every selected clip, as Delete does.
