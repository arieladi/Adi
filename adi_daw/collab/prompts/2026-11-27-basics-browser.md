# basics-browser — queue, 27 November 2026

Rewritten on 27 November after the check of the Browser and the AI (ADR-0261). If you read an earlier
copy of this file, read this one whole.

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/browser.md` on
`basics/browser-log`. Each row's detail and its code pointers are in `docs/SANITY-CHECK.md`.

**Your branch.** One chain branch, `basics/browser-chain`, started **from `origin/basics/integrate-1`
and not from main.** Five other chains changed the Browser this week and are gathered there: the listing
index (SC-421), Add Folder's chooser and loading by Enter and double-click (SC-104, SC-105), the edge
drag (SC-108), the User Library (SC-135), the Info box in the column (SC-424). Merge that branch into
yours when it moves. The integrator merges your chain.

**One owner.** The queue is yours from your first row in the report. The claim of 26 November on
`basics/browser-log` is closed: that session built nothing. Write over its table and answer its question
with one line: this session owns the queue.

**Your part:** the Browser (`src/juce/ui_browser.*` and what it calls), the preview player with its
engine half, and one new service, the library query. A new user's first scenario, "find a kick and hear
it", stops here today.

**The look.** The Browser's sheet is not agreed with the director (ADR-0243 d1, ADR-0244). Build what
each part does in today's look: today's colours, fonts and row sizes, nothing new for the eye beyond
what an item names. What a user will see and Live does not show goes under "For the director", with its
measured place, before it lands. Not yours, because they wait for the sheet (ADR-0261 d19): the AI
label, the filter panel, Sync and Raw buttons, a server's mark.

**Four rules from ADR-0261 that shape the code.**
- **The bar has modes.** It searches. A label can take it over: Hot-Swap does today, the AI label will.
  Keep the bar's text per mode and keep "which mode" as one state the panel reads. Nothing typed for a
  search may ever be handed to another mode.
- **One search for the whole program.** Build the query as a service outside `BrowserPanel`: the
  floating search will call it. Today `BrowserLibrarySnapshot` is a friend of the panel and matches the
  typed text as one phrase.
- **One preview player for the whole program.** A service of the engine and the shell with three callers
  in time: your row, the floating search, the map. Not a member of the panel.
- **Every key is a command** with a stable id from its first commit (WIN-04): Preview on and off,
  Preview Selected, Hot-Swap.

## Queue, in this order

1. **SC-121: the Browser shows a list when ADI opens.** Today it is empty at every launch and after New
   or Open until a label is clicked. The label chosen last is remembered; a remembered label that is not
   shown falls back to All.
2. **SC-120 and SC-122: the search finds a thing wherever it is.** As Live: an All label, and every word
   matched on its own ("kick 808" finds "808 Kick.wav"). Build it on SC-421's snapshot as the query
   service, and keep SC-421's measured time at 200,000 files. Ctrl+F goes to All and into the bar.
3. **SC-124: the left list shows all its labels and scrolls.** At the default window size Add Folder and
   every added folder are cut off.
4. **SC-125: Add Folder adds a folder,** a folder dragged in from the file manager does too, and a
   folder is removed from the list there. Read what SC-104 built on your starting branch and finish it.
   SC-132 moves up to here: a folder ADI cannot write to (a network share, the folder of sounds that
   ships with the program) is browsed all the same, where today it is left out because ADI's marker file
   cannot be written; and the new User Library lies under the user's documents, which a sync service may
   carry to his other machines with the marker in it: say what you do about that.
5. **SC-126: a sample folder shows as folders that open and close,** as on the disk.
6. **SC-123: a clicked sample is heard.** The engine's part was DF-E82's and N76's, with MX-08; ADR-0261
   d14 lends it to you. Build:
   - the player: the plain file, played once, to Cue Out at Cue Volume. With no song open it plays on
     outputs 1 and 2 at a level kept with the program. A compressed file is streamed, never written into
     the decoding cache for a preview;
   - the headphones switch, its state kept with the program and not with a song;
   - Live's keys: with the switch on, moving through the list plays each sample; with it off, Shift+Enter
     and the right arrow play the selected one. Space is not a preview key;
   - the sample's waveform in the preview row, with a click to play from there;
   - nothing of a preview is ever written into a take.
   Draw no Sync and no Raw button: in time and in key come later (TM-MD-06). Live's own preview is not
   read yet (R68 to R76 of the readings list): where you had to guess, leave a named constant and say so.
7. **SC-429: the preview row is hidden until it is needed** (the director: "i don't want to see that
   until i need that"). The selected row decides, not the label: today an audio file. The row appears
   with the first such selection and stays while that list is shown, so the list does not change height
   as the selection moves. It is never shown before item 6's player exists.
8. **SC-431: a missing Cue output is said.** When the song's Cue Out names a pair this interface lacks,
   the metronome and the preview are silent today and nothing says so. ADI says so and names what is
   silent.
9. **SC-127: every label in the left list shows something, or is not shown.** Read as: a label is hidden
   while its feature is not built, and shown, empty or not, once it is. Keep the roster in your report:
   each label, in order, and the row that brings it. Favorites, Current Project and the User Library are
   shown when their rows land, empty or not.
10. **SC-128: sounds to start with (ADR-0258 d10).** A generator in `tools/` that synthesises a first
    dozen drum hits by its own arithmetic (no outside sample) and four short loops made from those hits
    by fixed patterns, so that something that ships is a loop. The files ship with the program (add them
    to SC-107's install list) and list under Drums and Samples; Sounds is for presets, as in Live. The
    shipped folder cannot be written to: item 4's read-only half comes first. Say in your report how
    each sound is made.
11. **SC-432: Hot-Swap is a command.** Q is a key test inside the device panel today
    (`src/juce/ui_devices.cpp`, about 1871), in no key map and not rebindable.
12. **SC-424, its remainder.** With the Browser hidden, the Info key, its View menu item and the Status
    Bar's corner button change a setting and nothing on screen. They bring the column back.

## After these

The wave 2 rows of the browser queue in `docs/SANITY-CHECK.md`, in their order. SC-135 is done (the
project chain).
