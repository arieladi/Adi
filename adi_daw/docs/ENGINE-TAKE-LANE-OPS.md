# CT-01 take-lane journal slice

This slice adds stored take-lane editing on top of the arrangement main-lane
selection. It does not close CT-01. Main remains `clips.lane_id IS NULL`; stored
takes remain available to editing and persistence while the shared playback
accessor selects main. No schema, UI, audition or recorder change is included.

The implemented operations and payloads are specified in OPS §9.2–9.3:

| Operation | Behavior | Undo |
|---|---|---|
| `lane.create {id,track,row}` | `createTakeLane` prepares a full typed native row, default name `Lane`; insert at top and compact existing take ordinals | Exact old ordinals and removal of the created identity |
| `lane.delete {id}` | Delete the lane's clips and known material children explicitly, then the lane; retain pooled source media | Full typed lane/clip rows, child streams, alias links and old ordering |
| `lane.reorder {track,lanes}` | Full explicit permutation of this track's take lanes; ordinal order starts at zero | Exact prior ordinals, including gaps |
| `lane.rename {id,name}` | Store the name | Full prior typed row |
| `lane.setMute {id,on}` | Store the mute flag; no audition selector is implied | Full prior typed row |
| `clip.setLane {id,lane?}` | Attach to a same-track take lane, or detach to main with null/omission | Exact previous clip row and identity |
| `lane.restore {changes}` | Guarded captured row changes, used by the inverses | Symmetric captured changes |

Only audio/MIDI/instrument tracks and native `kind='take'` lanes are admitted by
these commands. Legacy comp/automation kinds are not silently reinterpreted.
`lane.setCompTarget` remains unregistered until its semantics are resolved under
Live's copies-to-main comp model. It must not be used as an alternate playback
selection. The current mute op stores metadata for later audition work.

Creation's complete row is part of the request, not inferred from a later schema
default during replay. The owner-side helper evaluates native defaults once;
callers allocate persistent identities through the existing owner allocator.
Reordering captures all affected typed rows. Unchanged requests are refused
without appending a no-op history step.

Deletion reuses clip images and guarded row edits. Lanes are created before
their clips on restore and deleted after clips on removal. External aliases
are explicitly unlinked and captured; internal alias chains/cycles are restored
after all clip identities exist. Unknown foreign-key effects refuse before any
mutation rather than destroying future extension data. A late SQL failure rolls
back ordering, children, aliases and the journal together.

Cross-track lane attachment refuses. The existing primitive `clip.move` now
also refuses moving an attached clip to another track; a deliberate
detach → move → attach batch remains valid and has one Undo. There is no automatic
promotion or transfer of another track's lane.

The new journal tests exercise creation/reorder/scalars, same-track/main
attachment, exact child-stream and future-column preservation, alias cycles,
source path retention, nonempty lane deletion, reopened Undo, foreign-key
checks, malformed requests, unknown extension children and a late-trigger
rollback. A trigger specifically rejects any take-to-main `SET NULL`, proving
that lane deletion does not rely on that side effect. The existing playback,
clip editing, time editing and placement suites remain the regression boundary.

Remaining CT-01 work: session-only audition and its interaction with main/mute;
take-lane EditScope support in range/clipboard producers; lane duplication and
delete-unused semantics; lane visibility/height and window gestures; the legacy
comp-target question and the required comping ADR/SPEC refinement. Loop recording
and comp gestures remain their separate CT-02/03/04 tasks. No new deferred-register
number is assigned in this slice.
