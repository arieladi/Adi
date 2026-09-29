# Step 7.3 — arrangement checklist, before implementation

Mission 8; stacked on #198. Sources: Live 12 chapter 6, printed pp.160–164 and 170–177; ADR-0129; STEP-7-PLAN §7.3; ADR-0200. Descriptions below are our own words.

| Piece | Behaviour to implement and test |
|---|---|
| TimelineRuler | Musical bars/beats; horizontal drag pans, vertical drag zooms; double-click fits selection or all clips. |
| Navigation | +/- zoom at selection or playhead; command-wheel at pointer (ADR-0129 deviation); command+alt drag pans; shift-wheel pans horizontally; Z fits selection, X restores prior zoom; H/W fit height/width. Every command also has a menu/button path. |
| TrackHeaderList | Vertically aligned named tracks; selectable header; add audio track through track.create; adjustable lane height, alt-wheel under pointer and height fit. |
| ArrangementCanvas | One component paints and hit-tests position-sorted clips; click selects clip/time and insert point, drag selects time; no per-clip components. Keyboard selection and accessible description. Audio file drop uses one journal transaction and a real audio-clip binding op. |
| Playhead | Separate one-pixel component, ignores mouse; frame movement does not repaint the full arrangement or allocate. Shared root reader per frame. |
| Transport | Click locates the insert marker; Home returns to start; start/stop use the existing driver mailbox; no GUI access to the live Transport. |
| Persistence | View zoom/scroll/height persisted in ui_view, outside project edit history. |

The plan records missing engine work rather than authorizing UI SQL workarounds. Mission 8 explicitly asks for playable dropped audio; this PR claims the narrow snapshot metadata, media request preparation and catalogue attachment/inverse paths needed to deliver it. Components still read SnapshotReader and submit ops. File hashing/decoding is off audio. Scrub quantisation, locators, automation lanes, waveform editing and comping remain outside this arrangement slice, not claimed as Live parity.
