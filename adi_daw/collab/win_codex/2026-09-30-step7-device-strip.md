# Step 7.4 — checklist before implementation

Source: Live 12, Working with Instruments and Effects, Device View/Using Devices, printed pp.438–444; plug-in panels pp.459–461. Own wording:
- The bottom device view follows the selected track and orders its devices left to right.
- A title selects the device; fold/unfold preserves it; its activator bypasses it; Delete removes it with undo.
- A DAW-drawn panel exposes the declared continuous/switch/menu shapes, value text, automation/override and missing controls. It resolves the <=64 rule through panel::resolve, and configure/search offers the larger parameter set.
- Configured order is preserved; Airwindows panels show only the active algorithm's controls.
- Device-strip resizing is ADI's approved departure: natural-height controls top-align, a configurable desktop floor applies, and transport/ruler/one row survive at the ceiling. 169 px remains UNVERIFIED and is not coded.

Implementation boundary: the document owns the shared message-thread parameter publication. UI gestures need their own ParamEditCapture producer, distinct from each plug-in's producer; a narrow ParamOps UI-event entry reuses its existing opener/inverse machinery. This closes the missing application feed, without making a component read SQLite or plug-in state. Clipboard/preset/browser workflows are separate from this requested strip/panel slice.
