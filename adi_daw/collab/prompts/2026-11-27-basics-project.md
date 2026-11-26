# basics-project — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/project.md` on
`basics/project-log`.

**Your part:** the project and its files, Settings and the Browser. You may run on Linux: the core and
the window's headless suites build and run there (the window suites with no display at all, as
`ui.yml` runs them: see the howto). A deliberate difference from Live that is ruled, not a fault: ADI has no Save step, every
edit is already in the file, and Ctrl+S takes a named snapshot.

## Queue, in this order

1. **SC-100: the way back to a project.** Today every launch makes a new `Untitled(n).adi` in a hidden
   application-data folder, there is no Open Recent, and Open's chooser starts nowhere
   (`src/juce/ui_main.cpp`, about 151 and 1374; `src/juce/ui_shell.cpp`, about 4605).
   - File > Open Recent, as Live's.
   - The Open chooser starts in the projects folder.
   - The default projects folder is one the user can see, under his documents.
   - An Untitled project that was never edited is not left behind when it is closed.
2. **SC-07: Save As takes the recordings along.** Read in code, not yet run: confirm it first. Takes go
   to the project folder's audio folder (`src/adi/recording_commands.cpp`), and Save As copies the
   database and rewrites paths only (`src/adi/project_files.cpp`). After Save As to another folder the
   project must not depend on the old one, as when Live first saves a Set.
3. **SC-09: a plug-in's own state is kept.** `ParamOps::snapshotAll` (`src/adi/engine/param_ops.hpp`)
   has no caller. Call it at Take Snapshot, Save As, Save a Copy and quit (PD-03), and settle a knob
   edit that is still pending at quit.
4. **SC-103: a log file.** One readable file per run under the application's data folder, with the
   version, what was opened, and every error the Status Bar or an alert showed. `privacy.diagnosticLog`
   is a setting nothing reads: it switches this.
5. **SC-102: an About box** with the version and the licences in `third_party_notices/`, and an icon for
   the program.
6. **SC-104 and SC-105: the user's own samples.** A folder is chosen with a button in Settings, and the
   Browser reads it without a restart (`src/juce/ui_settings.cpp`, about 128; `ui_main.cpp`, about
   1342). A double-click or Enter on a sample in the Browser loads it onto the selected track, as Live
   (`src/juce/ui_browser.cpp`, about 433: it does nothing today).
7. **SC-34: Create > Import Audio File,** onto the selected track at the insert marker, through the
   path a dropped file takes.
8. **SC-08: Settings that do nothing.** About 160 of 222 rows have no reader. List them (a script over
   the registry and the sources), then dim each with a line saying it is not built, or wire it where
   that is a few lines (Test Tone, Open on start). Put the list in your report.

## After these (wave 2)

SC-106 (Save As over an existing name asks and replaces), SC-108 (the Browser's edge dragged to resize
it), SC-40 (a missing file marked on its clip, and relinked), SC-107 (an installer and the `.adi` file
association; say what it needs from a Windows machine).

## Added by round 2 (26 November): the director's answers change this queue's order

- **First of all, SC-148 (ADR-0258 d1): each song has a folder of its own.** Saving makes a folder named
  after the song with the song file and its media inside, as a Live Project; recordings under Samples
  in a Recorded folder (d2). A song that has an `audio` folder is still read.
- **Then SC-149 (d3): the first Ctrl+S on a song with no name asks its name and place,** once. That
  first save takes the Untitled file and its recordings along and leaves no second copy (d4, SC-152).
  This settles SC-07 for a new song; for a song already saved, Save As behaves as Live's (run and
  confirmed by round 2: today the takes stay behind and the song plays silence when the old place goes).
- **SC-150: a song file moved by hand still finds samples that never moved.** A same-drive sample is
  stored only as a path relative to the song's folder.
- **SC-100's last part waits:** the default projects folder moves under the user's documents only after
  the safety queue's first item (a project whole in a synced folder; d7). Open Recent, the chooser's
  start and the untouched Untitled file do not wait.
- **Collected samples keep their own names (d5, SC-154),** and a User Library exists from the first
  launch (d6, SC-135).

Your second queue, when this one is empty: `2026-11-27-basics-safety.md`.
