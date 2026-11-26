# Basics project

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-148 | `basics/sc-148-project-folders` `c820054b` | New reserves a folder per Untitled song. Save As puts the named file in a matching folder. New recordings use Samples/Recorded and existing recordings still resolve from audio/. Touched the window/window-fn shell and the engine-win recording producer. | Linux JUCE/Pd Debug with WERROR. The recording regression failed four checks with the old producer. Recording commands, project files, UI document and offscreen UI shell passed. All three acceptance tests passed across the acceptance and WTS runs. Under Xvfb, New allocated a second directory and Save As wrote Sessions v1.2/Sessions v1.2.adi. Running the full shell suite with DISPLAY set hit X11 BadWindow, so the suite passed using the Linux CI offscreen invocation. | PR requested |
