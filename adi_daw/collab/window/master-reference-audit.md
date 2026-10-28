# The Master Reference against the window

Every interface item in the Master Reference v1.2 (26 October 2026, 190 pages,
`adi_daw/5_Adi-Daw Master Reference (claude draft v1.2) preview.pdf` on main), read in full, checked
against the window's code on `window/arrangement-nav` (#290), 28 October 2026. The window lane keeps
this file; each row moves when its item does.

**Status words.**
- **Built**: in the window and working as the document says.
- **Partial**: there, with what is missing named.
- **Hidden**: built, but not where a user looks for it (ADR-0203 d2: additions are shown, never
  hidden).
- **Missing**: the window can build it alone; it is in the window's queue below.
- **Waits**: it needs another lane first; the Need is named (the window log's table, N1 to N21,
  and new ones marked "new").
- **Later**: the document itself places it later (P2 or P3, Session View, the agent's steps 8 and 9,
  ADiJ, Mobile).
- **Ruling**: the document leaves a choice that is the director's.

MR § is the Master Reference's section.

## 1. The window's shape (MR 1.1, 3, 3.7, 10)

| Item | MR § | Window today |
|---|---|---|
| Live's single window: browser left, arrangement centre, track controls right, devices along the bottom, Info View bottom left, Status Bar, control bar on top | 1.1, 3 | Built |
| Swap the browser and the track controls to the other side, instantly, for Bitwig and Cubase users | 1.1, 3.2 (ADR-0080) | Built (window/swap-panels): a Swap button in the Status Bar beside Mixer, lit while swapped, and View › "Swap side panels"; each panel keeps its own width. Swapped, the director's layout C (28 October): names, track controls, strip, lanes; the browser right |
| Hide one or both side panels to give the arrangement the whole width | 3.2 | Built (View › Hide/Show Browser, the track controls' toggle) |
| The narrow-window rule: each panel keeps its minimum, a swap that cannot fit is refused with a message | 3.2 (ADR-0180 d4) | Built |
| The mixer and the editors undock into their own windows, the same components reparented | 1.1, 3.2 (ADR-0063) | Partial: device panels and the analyser float in the floating host; the mixer and the editors do not undock |
| The mixer panel and the device strip collapse, remembered with the widths | 3.2, 3.7 (ADR-0112 d5) | Built |
| The device strip: Live's height at least, dragged taller, never shorter | 3.4 (ADR-0184) | Built |
| Interface zoom per window, Ctrl/Cmd and + or − | 1.1, 10 | Built for the session; remembered across launches waits (N4) |
| The space bar reaches the transport from every window | 1.1, 3.7 | Built |
| Three view states in one value, Global / Group focus / Detailed zoom (Macro / Main / Micro); the mixer and the timeline always show the same tracks | 1.1, 3.2, 10 (ADR-0112) | Missing. Global is the window's alone; Group focus needs the snapshot's groups (N6: a track's parent and kind); Detailed zoom needs the detail editor (section 5) |
| Named view filters, Bitwig's project filter (figure 32: a menu at the top of the track-header column listing the project's group tree; choose one to see only it), a manual filter tab and an un-filter toggle, applied to the timeline and the mixer | 1.1, 3.2, 10 (ADR-0112 d2) | Missing. Explicit membership and name patterns are the window's alone; filters by group, kind or colour wait for N6. A new control: its picture goes to the director first |
| The AI builds a view from a sentence ("put all percussion in a view group") | 3.2, 10 | Later (the agent, steps 8 and 9) |
| Every window, zoom and selection restored on open | 1.6, 11.3 | Built (`ui_view`); plug-in windows on their monitor wait for N8 |
| Hebrew and right-to-left text in text fields; the timeline stays left to right | 1.1 (ADR-0125 d4) | Unchecked: JUCE shapes it, no test yet |

## 2. The browser (MR 3.1)

| Item | MR § | Window today |
|---|---|---|
| Collections, Categories (Pd Devices in Max for Live's place), Places (Packs, User Library, Current Project, Add Folder…) | 3.1 | Partial: all listed, but the sidebar does not scroll, so on a short window Places is cut off below Templates. Missing: a scrolling sidebar |
| Search; Ctrl/Cmd+F from anywhere | 3.1 | Built |
| The lists under each label; double-click or drag to load | 3.1 | Waits (N15) |
| The AI chat as a collapsed tab at the browser's foot | 3.1 | Built (the tab); the chat is the agent's (Later) |
| The floating palette, Ctrl+P / Cmd+P: V/serum, S/snare, P/ presets, arrows, Space auditions, Enter loads; sentences become tasks | 1.1, 3.1 (ADR-0104) | Missing. Commands by name are the window's alone; plug-ins, samples and presets wait for N15; sentences for the agent |
| AudioGridder servers' plug-ins beside the local ones, with a server icon | 3.1, 9.5 | Later (engine, P1) |
| Semantic sample search | 3.1 | Later (P1, a service) |
| The preview row; transpose to key, sync to scale and preview loudness in the browser | 3.1, V.2 §7 | Partial: the row and its headphones switch are drawn; previews wait for N15 and the engine's preview path |
| Actions that need a project are disabled with the reason shown | 3.1 | Built where they exist (the Status Bar says why) |

## 3. The track controls and the mixer (MR 1.3, 3.2, 7, 8)

| Item | MR § | Window today |
|---|---|---|
| Volume, pan, Track Activator, solo, arm; In/Out with monitoring In / Auto / Off | 8.1 | Built |
| One send knob per return on every track and group; return strips after the tracks; Insert Return Track; show and hide returns and sends from the mixer's view menu | 3.2, 8.2, 10 (ADR-0203 d3) | Waits (N11, and N6 for a track's kind) |
| Pre/Post as one switch on each return; a return's own sends off until Enable Send / Enable All Sends | 8.2 | Waits (N11) |
| A send into any track, and a pan on a send, made from the send slot | 1.3, 8.2 | Waits (N11) |
| Sends Only in the output menu | 8.2 | Waits (N11) |
| ADI-Q and ADI-L on every strip, present but off, one click each | 0.3, 1.3, 3.2, 8.1 | Built as Live-sized switches, off; engaging them waits (N16) |
| The one EQ window listing every channel's EQ and limiter, opened from the strip | 1.3, 3.2, 8.1 | Built (View › Channel EQ; the slots wait for N16) |
| Split Stereo Pan from the pan control's context menu | 3.2, 8.1 | Built: listed, disabled, says why (N12) |
| Solo in Place, a switch in the Solo button's context menu and in the Options menu; Exclusive Solo; Solo/Cue, Cue Out and Cue Volume | 8.1 | Waits: Solo in Place has no setting and the engine has no cue path (new Need). Exclusive Solo exists in the registry (`record.exclusiveSolo`) and the window does not follow it yet: Missing |
| Track Delay in milliseconds on every strip; the master's shown, not played | 7, 8.1 | Waits: `mixer.setDelay` exists, but the snapshot does not carry a track's delay (new Need) |
| Delay Compensation, Live's switch in the Options menu; Track Delay off while it is off | 7, 8.4 | Waits: no setting and no engine switch (new Need) |
| Reduced Latency When Monitoring in the Options menu | 8.4, V.2 §3 | Missing: the setting exists (`audio.reducedLatencyWhenMonitoring`); the menu item is the window's |
| Auto Gain Stage, one command | 1.3, 8.1 | Built (the track controls' right-click menu, two choices) |
| The Loudness Drive, on the master strip | 1.3, 8.1 | Waits (engine; ADI-L, N16) |
| Analog summing: a toggle, a drive dial and a flavour menu in a group's and the master's header | 7, 8.1 | Waits: the ops exist (`group.setSumming*`), but the snapshot does not say which tracks are groups or the master (N6) |
| A small live scope and spectrum inside every strip; a click opens the Scope or the analyser on that track; Compare in Scope for two selected tracks, or Alt-click a second track's scope button | 1.3, 3.2, 8.1, 8.6 | Partial: the analyser's big window and the engine's scope taps exist; the strip's view and the Scope window are Missing |
| A loudness meter, LUFS and true peak, on the master only | 1.3, 8.6 | Waits (N13, and N6 for the master) |
| Live's CPU meter; the per-track Performance Impact meter | V.3, V.4 §1 | Partial: the CPU meter is drawn; the per-track meter waits for the engine's numbers |
| Rec-Q ([1/16] or [Free]) and Play-Q on every MIDI track header | V.2 §9 | Waits: no op, and no track kind (N6) |
| Keep Monitoring Latency from the monitor button's right-click | V.2 §9 | Waits: no op (new Need) |
| Freeze, Freeze All Channels, Freeze Group; Bounce in Place, Bounce to New Track, with a prompt | 1.3, 7 | Waits (engine, P1) |
| Group Tracks; a group as a folder and its bus | 7 | Waits (N6: a track's parent and kind) |
| Save as Default Audio/MIDI Track in the header's menu | V.2 §9 | Later (P2) |
| Moving tracks by dragging their title bars | 3.3 (Live §6.1, §18.2) | Missing, in progress (`window/track-move`); Main stays where the snapshot puts it until N6 |

## 4. The arrangement (MR 3.3, 4, 6.1)

| Item | MR § | Window today |
|---|---|---|
| Tracks, clips, the ruler, the scrub area, the Overview, Live's zoom, Follow, the grid, moving and resizing clips, Live's keys | 3.3 | Built (#262 to #290) |
| A time range across one or more tracks | 3.3, 4.4 | Partial: one track only. Missing: the selection across tracks |
| Cut, copy, delete and silence over a range; Insert and Delete Time; Ctrl+I Insert Silence | 3.3, 4.4 | Waits (N17) |
| Live's fade handles, one fade for all selected clips; Create Fade In/Out and Create Crossfade on Ctrl+Alt+F / Cmd+Option+F | 4.3 | Waits (N17: no fade op) |
| The fade editor on a double-click | 4.3 | Later (P2) |
| A gain handle on every audio clip | 4.3 | Waits: `clip.setGain` exists, but the snapshot does not carry the gain (N6) |
| Markers and locators, a marker holding a note and a prompt | 3.3 | Waits: no marker ops (new Need) |
| The loop brace, punch in and out | 3.6 | Waits (N19) |
| Automation lanes over the clips or in sub-lanes; Re-Enable Automation lit while a lane is overridden; Automation Arm; Alt-drag bends a segment | 6.1 | Partial: the two buttons are drawn; the lanes wait for the automation data in the snapshot (new Need) |
| MIDI Learn on every lane header | 6.1 | Later (P2) |
| Clip names; the Main track last | 3.3 | Waits (N6) |
| Take lanes and comping | 4.6 | Later (P1, engine first) |
| Dropping an audio file on a track | 3.3 | Waits (N5). The director, 28 October, as Ableton: files, synths and plug-ins drop on an audio, MIDI or synth track or the empty area; a device or plug-in dropped on a track goes to its device chain below, never into the lanes (N5, N15) |

## 5. Devices and the editors (MR 3.4, 4, 5, 9)

| Item | MR § | Window today |
|---|---|---|
| The device chain: DAW-drawn panels, fold, delete, rename, Configure | 3.4, 9.2 | Built |
| ADI's searchable Parameter List button beside Configure | 3.4, 9.2 (ADR-0150 d3) | Missing |
| The parameter context menu: Edit MIDI Map, Show Automation, Return to Default | 3.7 | Missing: the menu is the window's; Return to Default waits for N7 |
| A drag on a control is one undo step | 9.1 | Waits (N7) |
| A plug-in's own window, floating, from the wrench | 3.4 | Waits (N8) |
| The expression route in the device header (Auto / VST3 Expression / MIDI-MPE / Poly-AT) | V.2 §5 | Missing: `device.setExpressionRoute` exists; whether the snapshot carries the route is to check |
| Color Cab's sample drop tile on the device panel, "with step 7" | 9.8 | Waits: the sample slot has no op the window can call (new Need) |
| The missing-file mark on a device whose sample is gone | 9.3 | Waits (new Need: the mark in the snapshot) |
| The clip view, Live's layout (properties, loop, warp, gain, transposition, envelopes) | 4.1 | Missing, and not in step 7's plan; the window's next item after the view filter's picture (the director, 28 October), plan and pictures first |
| The MIDI note editor: piano roll, velocity and CC lanes, Fold and Scale (P0, Scale Mode P1) | 5.1 | Missing, as above; the note ops exist |
| Layered editing; the editors torn out | 3.4, 4.1 | Later, with the editors |
| The Pd patch editor | 9.3 | Later (P1, its own step) |

## 6. The Info View, the Status Bar and tooltips (MR 3.5)

| Item | MR § | Window today |
|---|---|---|
| The Info View, docked bottom left, instant | 3.5 | Built |
| Floating tooltips after 600 ms (500 to 800) | 3.5 | Built |
| An object with a remark shows a corner pip, and the Info View splits into the documentation and the remark | 3.5 | Waits: remarks are ops, but not in the snapshot (new Need) |
| A failed rebuild or a failing plug-in named in the Status Bar | 8.4, 9.1 | Built where the engine reports it |

## 7. The control bar (MR 3.6)

| Item | MR § | Window today |
|---|---|---|
| Link and Tap, BPM, nudge, time signature, metronome, quantization, Follow, position, transport, Automation Arm, Re-Enable, loop and punch, Draw Mode, computer MIDI keyboard, Key and MIDI map modes, CPU meter, MIDI indicators | 3.6 | Built as Live's controls; the metronome and the loop wait (N19); Link waits for the engine (P1) |
| Count-In in the metronome's menu | V.2 §9 | Missing (the menu); counting in waits with the metronome (N19) |
| **ADI's additions:** project tabs for several open projects | 3.6, 11.1 | Missing (the window's, large: one store per open tab, inactive tabs offline) |
| **ADI's additions:** the current view state | 3.6, 3.7 | Missing, with the view states (section 1) |
| **ADI's additions:** the agent's tier (Observe / Propose / Apply) and what, if anything, leaves the machine | 3.6 | Later (the agent, steps 8 and 9) |

## 8. Menus and keys (MR 1.1, 1.6, 3.6, 11.3, V.4 §4)

| Item | MR § | Window today |
|---|---|---|
| File: New, Open, Take Snapshot (Ctrl+S, the name pre-filled), Save As (Ctrl+Shift+S) | 3.6, 12 | Built; Save As says it waits (N3) |
| File: Save a Copy, Collect All and Save, the ZIP export beside it (no key) | 1.6, 11.3 | Missing as commands; each waits on the store (new Need) |
| File: Back up project to cloud; Link ArtCraft app… | 11.4, 9.5 | Later (P2) |
| Edit: Undo, Redo, Delete, Rename | 3.6 | Built |
| Edit: Cut, Copy, Paste, Duplicate, Select All | — | Waits (N17, N18) |
| Create: Insert Audio / MIDI Track | — | Built |
| Create: Insert Return Track; Import MIDI File | 8.2, V.2 §5 | Waits (N11); Later (P2) |
| View: Undo History (Ctrl+Alt+Z / Cmd+Option+Z), newest on top, pick a line to go back, with ADI's branches and snapshots; any node opens read-only in a silent tab | 1.6, 11.1, V.4 §4 | Waits: `History` lists branches and snapshots, but no undo steps (new Need) |
| Options: Follow and the grid | 6.10 | Built |
| Options: Chase MIDI Notes, MIDI Envelope Auto-Reset | V.2 §5 | Missing: both settings exist (`midi.chaseNotes`, `midi.envelopeAutoReset`) |
| Options: Edit MIDI Map, Edit Key Map | 3.6 | Partial: the control bar's Key and MIDI buttons; the menu items are Missing |
| Options: Solo in Place, Delay Compensation, Reduced Latency When Monitoring | 8.1, 8.4 | See section 3 |
| Navigate: Wrap Tab Navigation; Move Clips with Arrow Keys off | V.2 §1 | Built (`window/settings-nav`), from the existing settings (`lookfeel.wrapTabNavigation`, `editing.arrowKeysMoveClips`). The window log's N21 said they did not exist; that was wrong and is withdrawn |
| Help | — | Waits for a Help View |
| F3 Session View, Tab between the views, Alt+1 | 3.6, 3.8 | Later (Session View, P2); Alt+2 is built |
| F7 offline processing | 4.5 | Later (with Direct Offline Processing) |
| A Shortcuts page, with presets named after Live, Cubase and Bitwig | 6.3, V.3 | Missing (the page); its rows are the registry's (N14's kind) |
| Tool-modifier presets (Live, Cubase, Bitwig, FL), chosen once on first launch | V.3 | Missing (the first-launch choice); the rows are the registry's |

## 9. The Settings window (MR Part V)

| Item | MR § | Window today |
|---|---|---|
| A page chooser on the left, one page at a time, a floating window that remembers itself, Esc closes; its keys (Tab, arrows, Ctrl+Tab between pages) | V.1 §1, §2 | Built |
| Live's ten tab names in Live's order, ADI's pages after them | V.2 | Waits (N14) |
| The Find box | V.1 §1 | Built; related words ("beat" finds "tempo") are Missing |
| One help line at the window's foot | V.1 §1 | Built |
| A Defaults button per page | V.1 §1 | Built |
| A scope mark on every setting (App / Project / Device) | V.1 §3 | Built |
| Project pages at the bottom under their own heading, greyed with no project open | V.1 §3 | Missing: the chooser lists the registry's pages with no Project heading and greys nothing |
| Presets (a named subset of pages), named profiles, export and import as one bundle | V.1 §3 | Missing (the window's controls; the store has presets and bundles) |
| Safe Mode after a crash: default settings, no third-party plug-ins, devices as placeholders | V.1 §5 | Missing (the application's) |
| Restore "Don't Show Again" warnings, one button on the first page | V.1 §5 | Missing |
| The sample-rate bar: "Project is 48 kHz, hardware is 44.1 kHz", Switch Hardware, Resample Temporarily, Don't ask me again | V.2 §3 (F-05) | Partial: the mismatch is reported in the Status Bar; the bar and its buttons are Missing |
| The Audio page's channel configuration | V.2 §3 | Waits (N20) |

## Questions for the director

Ruled on 28 October 2026: 1, layout C (names, track controls, strip, lanes); 2, the Status Bar;
3, yes, its picture first; 4, yes, the clip view and the note editor next.

1. **The swap's track names.** After View › Swap side panels the track controls go left but the
   track names stay right of the clips, so a track's name and its controls are on opposite sides.
   Two shapes are possible:
   - the mirror of Live: track controls, then names, then the clips;
   - Bitwig's and Cubase's: names, then controls, then the clips.

   The window draws both for you to choose from before building either.
2. **Where the swap's control sits.** A visible control in the Status Bar beside its Mixer toggle,
   where the other view toggles are, is the window's proposal.
3. **The view filter's control.** As figure 32: a menu at the top of the track-header column with
   the group tree, plus "Only these tracks" for a manual filter and an un-filter toggle. Its picture
   comes first; the filters by group wait for N6.
4. **The clip view and the note editor** are P0 in the document but not in step 7's plan (7.2 to
   7.6). Are they the window lane's next phase, before phase C's parts?

## The window's queue from this audit (window alone, in order)

1. Done (window/swap-panels): the swap made visible, layout C.
2. Done (#292): Navigate's Wrap Tab Navigation and Move Clips with Arrow Keys off, from the
   existing settings; N21 withdrawn.
3. Next, as ruled: the view filter's picture, then the clip view and the note editor (plan and
   pictures first). The items below follow.
4. The Options menu: Chase MIDI Notes, MIDI Envelope Auto-Reset, Reduced Latency When Monitoring,
   Edit MIDI Map and Edit Key Map, from the existing settings and modes; Exclusive Solo followed.
5. A scrolling browser sidebar, so Places shows on every window height.
6. Moving tracks by their title bars (in progress).
7. A time range across several tracks.
8. The device parameter's context menu, and the searchable Parameter List.
9. The Settings window's presets, profiles, bundle, related words and Restore "Don't Show Again";
   Safe Mode.
10. The palette (Ctrl+P / Cmd+P), commands first.
11. The scope view in every strip and the Scope window (P1).
12. The view states and the named view filters, after their pictures are approved (question 3).
13. Project tabs.

New Needs for the log, from this audit:
- a track's delay in the snapshot;
- Solo in Place and a cue path;
- the Delay Compensation switch;
- markers;
- automation data in the snapshot;
- remarks in the snapshot;
- a sample slot op;
- the missing-file mark;
- History's list of undo steps;
- Save a Copy and Collect All and Save in the store;
- a Keep Monitoring Latency op.
