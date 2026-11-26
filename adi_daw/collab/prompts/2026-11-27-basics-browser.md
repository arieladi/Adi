# basics-browser — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/browser.md` on
`basics/browser-log`. Each row's detail and its code pointers are in `docs/SANITY-CHECK.md`.

**Your part:** the Browser (`src/juce/ui_browser.cpp` and what it calls). A new user's first scenario,
"find a kick and hear it", stops here today. The Windows 11 walker takes this queue after its look
queue; any other session may take it first if the director starts one for it.

## Queue, in this order

1. **SC-121: the Browser shows a list when ADI opens.** Today it is empty at every launch and after New
   or Open until a label is clicked.
2. **SC-120 and SC-122: the search finds a thing wherever it is.** Today it looks only inside the chosen
   label, and with none chosen through Samples alone; two words are matched as one phrase. As Live:
   an All label, every word matched on its own. By ADR-0257 d12 this bar is the one place to search.
3. **SC-124: the left list shows all its labels and scrolls.** At the default window size Add Folder and
   every added folder are cut off.
4. **SC-125: Add Folder adds a folder** (it only answers in the Status Bar today), a folder dragged in
   from the file manager does too, and a folder is removed from the list there. With SC-104 of the
   project queue (the Settings button): agree in your reports who lands first. A read-only folder must
   be accepted: ADI writes a marker file into each folder today.
5. **SC-126: a sample folder shows as folders that open and close,** as on the disk.
6. **SC-123: a clicked sample is heard.** The headphones switch is paint today. The engine's part was
   owned by DF-E82 and N76 with MX-08 (the preview goes to Cue Out); it moves up and is yours: the
   preview player, the switch, the arrow keys playing the next one, Shift+Enter, and the sample's
   waveform in the preview row with a click to jump.
7. **SC-127: every label in the left list shows something, or is not shown.** Nine only answer "not
   listed yet".
8. **SC-128: sounds to start with (ADR-0258 d10).** A generator in `tools/` that synthesises a first
   dozen drum hits by its own arithmetic (no outside sample), the files shipped with the program and
   listed under Drums and Samples. Say in your report how each hit is made.

## After these

The wave 2 rows of the browser queue in `docs/SANITY-CHECK.md`, in their order.
