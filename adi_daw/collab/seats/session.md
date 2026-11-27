Session engine seat taken on 27 November 2026.

## For win

- Reserve the next available schema minor for `scenes`, `clip_slots`, `clip_launch`, and the essential extension `org.adidaw.session`. Main is already at 1.23, and the 1.20 proposed in the plan went to checkpoints. The slot clips must stay out of the Arrangement accessor and its SQL readers.
- Reserve the former edit names `scene.create`, `scene.delete`, `scene.reorder`, `scene.rename`, `scene.setTempo`, `session.setSlotClip`, `session.clearSlot`, `session.setLaunchMode`, `session.setLaunchQuant`, and `session.setFollowAction`, and the ephemeral names `session.launchClip`, `session.launchScene`, `session.stopTrack`, and `session.stopAll`. Confirm the names and payloads for scene signature/enable, Stop-button and clip-setting edits, scene cancellation and slot record control before adding any additional names.
- Confirm reservation of `transport.setLaunchQuantization` from TM-SV-01 and reserve `capture.sessionPerformance` from TM-SV-04. No catalogue entries or migrations allocated in this chain so far.

## Queue

| Item | Change | Proof | State |
| --- | --- | --- | --- |
| TM-SV-02 | In-memory scenes, sparse slot rows and launch settings, with stable-ID lookup and zero-origin ownership checks (a1da1bd9b). Shared edit: CMake only. | 71 strict GCC checks. 20,000 lookup passes in 867 µs, zero allocations. The test failed to compile against main's missing data contract. | Engine rows built. Schema, older-reader warning and operation replay await reservations. |

## Readings owed

- TM-SV-02: Live 12.4 Stop-button, dormant scene-value and clip-setting defaults.
- TM-SV-03: Live 12.4 exact-boundary and near-boundary presses, queued release, Repeat, Fade, Arrangement hand-over and Legato with unequal loop lengths (PLAN N2, N3, N5, N8, N10–N12).
- TM-SV-04: Live 12.4 follow-action boundary cases, moved/deleted scene target and scene tempo undo (PLAN L3, L4, N6, N7, N9).
