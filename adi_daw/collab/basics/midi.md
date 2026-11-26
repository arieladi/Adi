# Basics MIDI

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-220 | `basics/sc-220-midi-loop` `062af8f9` | Inserting empty MIDI clips in the Arrangement enables a source-relative loop over each clip's original length in the same undo step. Touched the window's shared clip edit builder and its headless and shell tests. | Windows VS 2022 Debug with warnings as errors. The note-edit suite failed the two new checks before the fix and passed all 233 afterward. | Blocked on the Debug shell and acceptance runs, which are building. |
