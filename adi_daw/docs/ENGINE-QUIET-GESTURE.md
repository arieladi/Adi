# Quiet-window End for a source without release (SRF-02, manual engine slice)

`gesture::QuietSession` owns a fresh, open common `gesture::Session` and an
explicit, nonzero quiet interval in milliseconds. Construct it on the Session's source owner thread. The source owner calls
`update(input, now)` and `poll(now)` on its owner thread. `now` is one monotonic
source clock, independent of transport position and journal UTC. There is no
default interval or background timer. Begin is still the common coordinator's
Begin, including its origin capture, claims and source/navigation reservation.

Only an accepted Value renews the interval. Before its expiry, the same live
preview is heard and no durable op is written. At the exact expiry boundary,
`poll` calls the ordinary common End and returns its complete CommitOutcome.
Successful or semantically empty End is never submitted again. A late Value
returns Busy until the old gesture ends; the caller starts a fresh common Begin
for the next source run. Delayed polling cannot merge two separated runs by
renewing an already expired interval. Explicit `end()` and `cancel()` retain
the ordinary release and Esc paths; destruction cancels, never commits.

Clock regressions, callback calls and foreign-thread calls refuse before timer
mutation. Unsigned elapsed-time comparison avoids absolute-deadline overflow.
Invalid values do not renew the last accepted Value. A refused End retains the
common owner and last accepted intent: the caller can inspect the returned
failure, remove its cause and poll again or Cancel. Caller transactions, sealed
admission, source loss, pending restoration and post-COMMIT presentation handling
remain the existing Session's responsibility. A source must keep polling from
its owner loop; audio callbacks do not run this allocating owner-side service.

This supplies manual scalar quiet-window End through the existing gesture
contract, not time-based op merging or Undo/recommit. ParamOps keeps its own
owned plug-in capture and existing UI end detection; do not substitute a scalar
producer for that path. The wrapper writes no automation: AU-03's armed write
pass is still the sole place a touch becomes automation. Surface/window binding,
Track Delay preview and that recording-pass integration remain separate work.
