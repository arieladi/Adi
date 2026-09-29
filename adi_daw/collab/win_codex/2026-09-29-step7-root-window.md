# Step 7.2 Windows continuation

Read the handoff first from #194 (it is not on main), then the updated plan and UI architecture. #193 is still OPEN, despite the handoff saying merged: merged its head as an explicit dependency with #194 and current main. No decided architecture is being reopened.

Checklist before code, from the approved 7.2 plan: one JUCE root/TransportBar per window; per-window VBlank and coalesced dirty drain; one ProjectView reference per frame, with cross-window actions re-reading on arrival; commands through OpSubmitter, app-scoped shortcut preset from settings; panel identities/widths, dock state and zoom persisted in ui_view; peerless software snapshots, keyboard synthesis, allocation-free steady frame drain; GUI compilation/testing on Windows, macOS and DISPLAY-less Linux.

Transport commands are ephemeral journal entries whose catalog apply is deliberately a no-op. The UI must queue them to the driver, never mutate Session::transport from the message thread. This slice adds a bounded SPSC handoff and atomic driver feedback, with no audio device opened by tests. ProjectView supplies committed tempo/track state; driver feedback supplies playing/position. Automatic file dialogs/audio-device selection and arrangement gestures belong to the remaining 7.2/7.3 integration, not invented placeholder success handlers here.

The requested A2000 parity rows have been asked for; this slice builds the approved structural checklist and existing command seam rather than authoring a duplicate Live manual checklist.
