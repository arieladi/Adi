# Step 7.2 Windows continuation

Read the handoff first from #194 (it is not on main), then the updated plan and UI architecture. #193 is still OPEN, despite the handoff saying merged: merged its head as an explicit dependency with #194 and current main. No decided architecture is being reopened.

Checklist before code, from the approved 7.2 plan: one JUCE root/TransportBar per window; per-window VBlank and coalesced dirty drain; one ProjectView reference per frame, with cross-window actions re-reading on arrival; commands through OpSubmitter, app-scoped shortcut preset from settings; panel identities/widths, dock state and zoom persisted in ui_view; peerless software snapshots, keyboard synthesis, allocation-free steady frame drain; GUI compilation/testing on Windows, macOS and DISPLAY-less Linux.

Transport commands are ephemeral journal entries whose catalog apply is deliberately a no-op. The UI must queue them to the driver, never mutate Session::transport from the message thread. This slice adds a bounded SPSC handoff and atomic driver feedback, with no audio device opened by tests. ProjectView supplies committed tempo/track state; driver feedback supplies playing/position. Automatic file dialogs/audio-device selection and arrangement gestures belong to the remaining 7.2/7.3 integration, not invented placeholder success handlers here.

The requested A2000 parity rows have been asked for; this slice builds the approved structural checklist and existing command seam rather than authoring a duplicate Live manual checklist.

## Implemented and boundaries

`AdiWindow` owns its VBlankAttachment and installs the shared app command layer; `AdiRootComponent` owns only its presented frame reader and dirty set. Children borrow that reader. JUCE paints asynchronously after the drain, so the root keeps it alive until the next drain. Play/stop queue to a driver-owned Transport; the driver calls `drain` between blocks and `publish` after advancing. No audio device is opened. Two queued toggles use pending intent, not stale feedback. A full queue refuses before journaling.

`ViewStateStore` is the database boundary; components receive this service rather than a Store. One versioned `ui_view` value per window carries ordered panel identities/widths, zoom and device dock/height. It creates no edit or undo entry; refused saves restore visible command state. Resize compression preserves each panel's minimum and stored preferred width. These desktop dimensions are ADI provisional defaults, not the unverified 169 px measurement.

The command layer implements transport and undo/redo, plus panel swapping. Space toggles transport; Escape is an ADI stop convenience. Undo is Ctrl/Cmd+Z. Live's redo is Ctrl+Y on Windows, Cmd+Shift+Z on macOS; Cubase uses Ctrl/Cmd+Shift+Z. Verified against [Live 12 keyboard shortcuts](https://www.ableton.com/en/manual/live-keyboard-shortcuts/) and [Cubase edit commands](https://www.steinberg.help/r/cubase-pro/15.0/en/cubase_nuendo/topics/key_commands/key_commands_edit_category_c.html). Bitwig currently shares the shell's Shift+Z redo default; full preset parity and editing are not claimed. ADR-0129 arrangement/navigation handlers wait for the arrangement; no fake `W`/fit or zoom success handler is exposed. Zoom is persisted now for that consumer.

This is the requested next 7.2 slice, not the completion of all 7.2: browser/arrangement/mixer/device panes are structural placeholders. Project dialogs, audio-device chooser, native entry-point/session ownership, parameter key mapping and arrangement interactions remain integration work. The window and driver handoff are reusable library seams, tested without speakers.

## Validation

Windows MSVC /W4 /WX builds both tiers. Headless persistence/mailbox suite: 38 checks; peerless JUCE suite covers three structural PNG references, real KeyPress dispatch, stale-window undo, per-window generations and tempo, preset reload, queue saturation/rapid toggles, read-only rollback, and zero allocations on unchanged/dirty/new-generation drains and driver handoff. References explicitly use SoftwareImageType and mask text/button typography; independent label/keyboard assertions cover those controls. This is not a typography or native mouse-routing claim. Native monitor/VBlank timing remains a platform integration check, while the exact callback drain is covered offscreen.

New `Step 7 UI` CI compiles and runs both tiers on Windows/macOS/Linux. Windows uses a short path with a configure guard; Linux runs with DISPLAY unset, explicitly testing the handoff's prior source-level prediction. Local Windows success does not stand in for the pending remote jobs.
