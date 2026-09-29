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

Implemented the four components, position-sorted clip hit-testing (overlap precedence matches painting), shared root reader, per-track view heights and navigation menu/keys. Musical labels use the existing renderPosition against snapshot meter rows; driver locate uses the existing TempoMap conversion. Add/drop service commits media.import + clip.create + clip.attachAudio as one transaction, with clip.detachAudio as its exact inverse for default attachments. A detach refuses edited attachment fields rather than silently losing them. The media importer now exposes request preparation, so its original API and the UI share hashing/path capture. Snapshot additions are only clip name, track kind and meter rows.

Validation: native MSVC /W4 /WX application and UI suites, plus snapshot/ops/media suites. File-only dropped sine passes floor-bin then peak checks through Session. One undo/redo removes/restores the entire drop; malformed attachments do not write the journal. Peerless software reference images cover arrangement/selection geometry; a fake ComponentPeer drives actual JUCE hit dispatch and drag capture, and keys drive Z/X. Frame movement keeps the playhead one pixel, mouse-transparent and allocation-free. A planted no-op detach initially escaped because clip deletion cascaded; added an independent detach/inverse assertion, then the successfully built plant fails two checks and the byte-restored implementation passes.

Import preparation currently hashes/decodes synchronously on the message thread (never audio); a progress/cancellation UI is a future responsiveness improvement. This slice does not claim clip waveform editing, clip move/trim gestures, locators, scrub quantisation or automation lanes. Those are not substitutes for the requested add-track/playable-drop path.
