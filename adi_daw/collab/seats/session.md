Session engine seat taken on 27 November 2026.

## For win

- Reserve the next available schema minor for `scenes`, `clip_slots`, `clip_launch`, and the essential extension `org.adidaw.session`. Main is already at 1.23, and the 1.20 proposed in the plan went to checkpoints. The slot clips must stay out of the Arrangement accessor and its SQL readers.
- Reserve the former edit names `scene.create`, `scene.delete`, `scene.reorder`, `scene.rename`, `scene.setTempo`, `session.setSlotClip`, `session.clearSlot`, `session.setLaunchMode`, `session.setLaunchQuant`, and `session.setFollowAction`, and the ephemeral names `session.launchClip`, `session.launchScene`, `session.stopTrack`, and `session.stopAll`. Confirm the names and payloads for scene signature/enable, Stop-button and clip-setting edits, scene cancellation and slot record control before adding any additional names.
- Confirm reservation of `transport.setLaunchQuantization` from TM-SV-01 and reserve `capture.sessionPerformance` from TM-SV-04. No catalogue entries or migrations allocated in this chain so far.

## Queue

| Item | Change | Proof | State |
| --- | --- | --- | --- |
| TM-SV-02 | In-memory scenes, sparse slot rows and launch settings, with stable-ID lookup and zero-origin ownership checks (a1da1bd9b). Shared edit: CMake only. | 71 strict GCC checks. 20,000 lookup passes in 867 µs, zero allocations. The test failed to compile against main's missing data contract. | Engine rows built. Schema, older-reader warning and operation replay await reservations. |
| TM-SV-03 | Fixed-capacity launch lane, modes, quantized scene/slot switches, Stop All, scene cancellation, Legato, and resident fixed-tempo audio through the track graph (1d32a673f). Standalone MIDI voice and audio/MIDI slot recording seams. Shared edits: CMake, Session, ClipPlayback, gesture preview and a two-line recorder friendship. | Two scenes switched at frame 96,000 (0 sample error at 48 kHz) in WAV and in float32/float64 Session graphs. Seven WAV block sizes and 16 scheduler rate/block pairs, zero counted callback allocations, I/O or Linux mutex calls in the WAV path. The recording seam captured 80 exact samples and MIDI offsets 0, 20 and 79. The fractional-loop test fails if the old phase calculation is restored. | Audio graph path built for owner-prepared PCM and an explicitly started transport. MIDI graph binding, recording commands and slot commit, outgoing Fade and running-tempo stretch remain pending. |
| TM-SV-04 | Clip follow groups and deterministic pass draws, literal scene follow resolution, performed transition observer, and bounded capture trace/planner (1d32a673f). | 34 follow and 8 capture checks. The launcher captured samples 1,000, 1,340 and 1,540 exactly in both 600- and 32-frame partitioning. A 30% second-action setting drew 19,830 of 65,536 times (30.258%). | Clip follow wired to Session audio. Scene follow dispatch and precedence, scene tempo/signature through the performed tempo path, Capture and Insert Scene, and undoable Arrangement capture await their scheduler and persistence bindings. |

## Readings owed

- TM-SV-02: Live 12.4 Stop-button, dormant scene-value and clip-setting defaults.
- TM-SV-03: Live 12.4 exact-boundary and near-boundary presses, queued release, Repeat with None, Stop All, velocity and Arrangement hand-over, Session outgoing Fade, and Legato with unequal loop lengths (PLAN N2, N3, N5, N8, N10–N12). The launch-in uses the measured ED-10 curve provisionally. The below-bar grid follows the measured N4 song-origin rule.
- TM-SV-04: Live 12.4 Previous at the first slot, Other with one slot, scene Stop, moved/deleted queued targets and scene tempo undo (PLAN L3, L4, N6, N7, N9). The manual documents that clip follow bypasses Global, honors explicit clip quantization and yields to due scene follow.

## Verification

- Linux GCC 14 Debug, `ADI_WERROR=ON`. All 288 CTest tests passed in 190.66 s. After rebuilding the final Session audio configuration checks, all ten seat suites passed again in 0.32 s.
- The final 48 kHz WAV fixture switched at frame 96,000. Preparing the two-second Re-Pitch PCM took 248.45 ms. The WAV path made zero counted callback allocations, file calls or Linux pthread mutex calls. The Session graph proof switched both tracks at the same frame in both processing widths.
- Schema, operation, parity, Pd and CI validators passed. The work through 27 November is on `seats/session-chain`. All five seats have a claim on origin.
