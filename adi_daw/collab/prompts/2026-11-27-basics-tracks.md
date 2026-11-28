# basics-tracks — queue, 27 November 2026

Read `collab/prompts/2026-11-27-basics-howto.md` first. Your report is `collab/basics/tracks.md` on
`basics/tracks-log`.

**Your part:** tracks, the mixer's controls and devices. You may run on Linux: the core and the window's
headless suites build and run there (the window suites with no display at all, as `ui.yml` runs them:
see the howto).

## Queue, in this order

1. **SC-01: the Main track cannot be deleted.** Checked: `track.delete` on Main goes through, for a user
   (`AdiRootComponent::deleteTrack`, `src/juce/ui_shell.cpp`) and for an agent's changeset. Refuse it in
   the op, so every caller is covered, and say why in the window.
2. **SC-05: Delete in the menus acts on the selection.** Edit > Delete Track deletes the track while a
   clip is selected; with several tracks selected it deletes one. As Live: the command deletes what is
   selected, and all the selected tracks in one undo step.
3. **SC-80: duplicating a track.** Ctrl+D on a selected track makes a copy with its clips, devices,
   automation and sends, below it, in one undo step; then cut, copy and paste of a track. There is no
   op for a track copy today: build it from the ops that exist, or add one and say so.
4. **SC-82: the mixer's controls as Live's.** A double-click (or Delete) resets a fader and a send as it
   resets pan today (`src/juce/ui_mixer.cpp`, about 180); Shift gives a fine drag; a device knob takes a
   typed value and the arrow keys (`src/juce/ui_devices.cpp`). *Reading owed:* what resets a fader in
   Live 12; build it as double-click and Delete until win passes the reading. Check also that a value
   can be typed into a fader, pan and send box in the real window, not only set by a test.
5. **SC-83: reordering devices by dragging,** within a chain and to another track. `device.move` exists;
   the window starts a drag only for the native mix functions (`ui_devices.cpp`, about 1614).
6. **SC-84 and SC-61: drops as Live's.** A plug-in dragged from the Browser (`src/juce/ui_browser.cpp`,
   about 354: "drags nothing"); a device or instrument dropped below the tracks makes a track of the
   right kind; a drop on a track's header and on a Mixer View strip.

## After these (wave 2)

SC-86 (Freeze, Flatten and Bounce as commands in the window: the engine is built), SC-87 (the Track
Activator in the automation chooser; Draw Mode for automation; clearing an envelope), SC-85 (device
presets: wait for win's note, the file's form is the director's to rule).

## Added by round 2 (26 November)

- **With SC-05, SC-260: the Delete key deletes the selected tracks.** No key reaches Delete Track today.
- **SC-259: the triangle beside a track's name, and the U key, fold and unfold the track.** Dead on
  every track that is not a group, with a stale message about a need that merged.
- **SC-282: an effect added while the instrument is selected comes after the instrument.** Today a
  built-in audio effect is stored in front of it, where no sound reaches it
  (`src/adi/builtin_insert.cpp`, about 172); a plug-in effect in the same place is moved after it.
  Paste and Duplicate ignore the same rules (`src/adi/device_copy.cpp`, about 85): the round's next row.
- **SC-283: a sample dragged onto a MIDI track, or into its empty Device View, makes an instrument that
  plays it** (DEV-34 moves up). The empty Device View of a MIDI track also reads "Drop Audio Effects
  Here".
- **U is answered (ADR-0258 d18):** SC-85's presets are one file each in the User Library, listed in
  the Browser under their device, as Live. The file is ADI's own readable form: propose it in your
  report before you build it.

Your second queue, when this one is empty: `2026-11-27-basics-edit.md`.
