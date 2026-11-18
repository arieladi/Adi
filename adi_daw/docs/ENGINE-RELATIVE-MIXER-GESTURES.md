# Relative mixer gestures (MX-23, engine selection slice)

`gesture::relativeScalarProducer(RelativeScalarSelection)` plugs into the existing
common GestureSession. It captures a driver, a literal selection and each original
native scalar once. If the driver belongs to the selection, every selected target
participates; otherwise only the driver participates. Subsequent changes to the
window's selection have no effect. Track and group controls take track IDs; send
controls take existing send-route IDs. Missing or duplicate IDs refuse Begin.

The caller supplies the existing control's finite coordinate domain at Begin and
`RelativeScalarInput(latestAbsoluteCoordinate)` at Value. The producer subtracts
the original driver coordinate, adds that total change to each original member
and clamps each result separately. Reversal after saturation therefore recovers
the original offsets. Pan domains stay inside [-1,1], summing drive inside
[-12,24]. Gain domains may supply their existing stored silence sentinel below
the display floor (the current volume fader uses -70..6 and -150 silence).
Members originally at/below that floor remain parked; the driver can leave it.
A member newly reaching the floor becomes silent and recovers on reversal.
Returning to the starting coordinate restores exact raw starting values,
including distinct legacy values represented at the display floor.

All eight existing scalar adapters are reused: track volume, pan, split left/right
pans, input gain, send gain/pan and summing drive. Their typed row/presence capture,
topology identity checks, guarded ordinary operations and endpoint claims apply
to every member. Each Value is one common preview: no material SQL or journal
publication survives it. End submits the final operations in one transaction;
one Undo restores the whole selection. Unchanged End adds no history. Cancel
restores the current committed base. A changed route identity or late failing
member refuses the whole batch. Existing single-control producers are unchanged.

The native playback suite exercises grouped gain and pan through the actual
EnginePresentation, audio graph and retained device at float32 and float64,
including reversal, no-op End, Cancel, Undo/Redo and second-member SQL failure.
Additional producer-level cases cover all eight mappings, control floors and
ceilings, absent split-pan rows, invalid inputs/domains and retargeted routes;
those raw Proposals alone are not sealed journal admission.

This is the MX-23 relative-selection engine slice. Window bindings, Track Delay
preview (currently a graph-rebuild operation), SRF-02's unbracketed quiet-window
owner and AU-03's armed recording-pass coordinator remain separate work. This
API does not create a timer, recorder, automation point, control/MCP field or DSP
implementation, and is not a claim that those full requirements are delivered.
