# basics-automation — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/automation.md` on
`basics/automation-log`. Each row's detail and its code pointers are in `docs/SANITY-CHECK.md`.

**Your part:** automation. Codex on the Windows 11 machine takes this queue after its midi queue.

## Queue, in this order

1. **SC-334: automation on a track does not block clip editing.** With the default setting, once a
   track has one automation point, Paste, Duplicate, a range Cut and Ctrl-drag copy of clips on it are
   refused with an engine sentence, a move stops replacing what it lands on, and a range Delete stops
   cutting clips (`src/adi/edit_plan.cpp`, about 107; the tree's own tests assert the refusal). As
   Live: the edit happens, and with Lock Envelopes off the automation moves with the clips.
2. **SC-335: moving a control while recording writes automation** (AU-03; the director's F of
   ADR-0256, worked now by ADR-0257 d1). Automation Arm is lit by default and the document always tells
   the session "not recording" (`src/adi/ui/project_document.cpp`, about 357). The modes as Live's:
   what is written while a control is held, what happens on release, the Re-Enable Automation button
   after an override.
3. **In Automation Mode a time selection is the automation's** (the round's row on Delete, Copy and
   Paste taking the clips there). Reading R56 of Live confirms the target before it is built.
4. **A breakpoint's value is shown and can be typed.**
5. **Choosing what a lane shows:** the two choosers of the approved sheet (today a right-click submenu
   only), and more than one lane per track (the fold triangle is SC-259 of the tracks queue).

## After these

The wave 2 rows of the automation queue in `docs/SANITY-CHECK.md`, in their order.
