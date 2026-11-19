The director can now open the Browser's Plug-Ins and see the VST3 and CLAP plug-ins installed on the machine, in Live's folders, by format and then by vendor (DF-W9, first half). Loading one onto a track waits for the engine's insert of a third-party plug-in (N57), and the Browser says so. Stacked on `window/arrangement-sheet` (step 4), for its picture's song.

![The Arrangement sheet beside the window with the Browser's Plug-Ins open, both 1920 x 1080](https://github.com/arieladi/Adi/blob/window/plugins-browser/adi_daw/collab/window/pictures/2026-11-19-plugins-browser.png?raw=true)

Left: the Arrangement sheet (`arrangement.png`), the agreed drawing that shows the Browser; the Browser's own sheet is a proposal still (`window/proposals`). Right: the window as built, 1920 x 1080 at real size, on the showcase song, grouped as in step 4's picture, Melody selected. The Browser's Plug-Ins lists nine plug-ins with invented names, as an app would list what its scan found:
- **CLAP:** Northwind Audio (Arc Synth and Glass Piano, instruments; Chord Weaver, a MIDI effect), Tidewater DSP (Plate Room and Tape Echo, audio effects).
- **VST3:** Copperline (Bus Glue, Saturator X) and Harbor Instruments (Chamber Strings, Low End). Their kind is not yet known (N58), so they show the plug-in icon.

Arc Synth is chosen. Every plug-in is dimmed, since none can load yet.

What it does
- **The list** (`src/adi/ui/plugin_listing.hpp`, `BrowserPanel`'s Plug-Ins): what the app's one loader found. VST3 comes from the lines `JuceDeviceLoader::knownPlugins()` writes, CLAP from `ClapHost::plugins()`.
  - It has a folder per format (CLAP, then VST3), in it a folder per vendor (no vendor: "Other"), in that the plug-ins by name.
  - Each row has its kind's icon: instrument, audio effect, or MIDI effect (a CLAP note effect).
  - The search keeps a plug-in by its name or its vendor.
- **The first scan:** the first time Plug-Ins is chosen, the Status Bar says "Scanning the plug-ins; they list when the scan is done", and the loader scans a moment later, once the window has painted. The list then fills in.
  - It is the loader the projects load from, so no second scanner races its registry (a key already pending is refused, and a plug-in would be lost).
  - Like the loader's own first scan, it runs on the message thread; N58 asks for it off the thread.
- **The Settings' plug-in folders** (`plugins.vst3Folders`, `plugins.clapFolders`) are searched for both formats, ahead of the system's, from the start.
- **A load:** a double-click or Enter says "Loading <name> waits for the engine's insert of a plug-in (window log, N57)". A plug-in drags nothing.
- **What the Status Bar says:**
  - outside the app (the tests): "Plug-ins list in the application, from its plug-in scan";
  - a scan that stopped: "The plug-ins could not all be scanned: <why>";
  - files read but none listed: "No plug-in could be listed: <n> plug-in files could not be scanned";
  - none found: "No VST3 or CLAP plug-ins were found. Add their folders in the Settings' Plug-Ins page";
  - some files unread: "<n> plug-in files could not be scanned; the rest are listed".

Not done
- **Loading a plug-in** waits for N57, an insert producer for a third-party plug-in (`preparePluginInsert`/`commitPluginInsert`, the sibling of the built-in one), which the window log describes. With it the load is one undo step, and a MIDI clip can sound (DF-W9's purpose).
- **N58:** the scan off the message thread, and the VST3 list with each plug-in's kind. A force quit during the first scan leaves the file being read marked in the registry, as a force quit during the loader's own scan already does.
- **The look** is today's Browser rows. The Browser's sheet is a proposal for the director.

Review
An adversarial review (correctness, behaviour, tests, a skeptic per finding) confirmed two defects. Both are fixed:
- **A scan that stopped, or read no file it found,** said to add folders: it says why, or how many files could not be read.
- **The first scan froze the window without a word:** it says it is scanning and runs once the window has painted.

The usage limit stopped part of the review's verification. The tests lens and seven skeptics did not run. One finding was refuted (vendors whose names differ only in case).

Tests
- Pd on:
  - `adi_ui_shell_tests`: 2768 checks, 0 failures:
    - the loader's VST3 lines read as their uid, name, vendor, version and file, a line of another shape refused;
    - a CLAP's kind from its descriptor;
    - Plug-Ins lists its folders in Live's order, no vendor in Other;
    - the search by vendor asks the loader nothing more;
    - a double-click says the load waits for N57;
    - the Status Bar's words for none found, files unread, a scan stopped, and the first scan, which lists when it is done.
  - `tools/test_all.sh build-juce`: PASS, 59459 checks across 245 suites, validators clean.
- Pd off, configured as `ui.yml` (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): `adi_app` and the six UI suites `ui.yml` runs build and pass.
- The shadow and clang warning sweeps are clean on this PR's files (`ui_browser.cpp`, `ui_main.cpp`, `tests/test_ui_shell.cpp`'s lines).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
