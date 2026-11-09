# The window's parameter producer (N7)

`ParamOps::uiEvent(deviceId,paramId,kind,normalized,nowMs,out)` is the engine
lift of the interface in `537c56d`. Forward Begin/Value/End on the owner/message
thread, using the descriptor id and the timer drain's monotonic clock.
Call `attachSession(session)` after load and refresh; it also detaches retired
devices. Each device's synchronous UI capture is separate from its plug-in's
single-producer callback ring, and is never handed to the plug-in.

Values reach the live plug-in immediately. End offers the gesture's final
`device.setParam` plus its first-touch opener, when needed. Commit the batch as
one undo transaction. Keyboard/unbracketed values close on drain's 150 ms quiet
timer, even without another UI event. Standalone legacy capture cancels stale
UI completion after device replacement, an external parameter edit or state
recall. Session-bound capture retains accepted input through foreign receipts;
it reapplies or reports a conflict/source-loss outcome instead of silently
dropping completion. Normalized input
clamps to 0..1; invalid/nonfinite, missing and non-automatable targets are refused.
A refused backend set is not captured as an accepted UI value. Session-bound
capture now retains each gesture through the common
[HI-01 owner](ENGINE-GESTURES.md); one output vector offers at most one gesture,
and later completed input remains queued.

Use the timer drain's commit path for UI events too: commit every appended
request. For Session-bound capture, `writeBlobs` validates/stages without SQL;
the ordinary journal admits the sealed whole vector and writes its retained
state resources in the same transaction. Standalone legacy capture still
writes `writeBlobs`/`takeBlobs` before submission. The bool return describes the
current event's acceptance; an older edit/state interval can have appended
requests even when this event is refused. Devices already hold the accepted
final values; do not replay each own opener back into them.

After a refused submission, inspect `pendingGestures()` and explicitly retry
or cancel the retained owner. Discarding an output vector does not discard
accepted input, and a durable vector cannot be submitted twice. Open and
undrained gestures reserve History navigation and source replacement.

A Session attachment overrides the device's automation lane on the first
accepted change, before End, so playback cannot fight the drag. This is runtime
state, with no extra history step. Existing Re-Enable Automation resumes it.
If the overridden set changed, refresh N26's ProjectView with
`session.overriddenLanes()` even before a gesture commits. Lane refs remain
decimal (ADR-0165); lookup translates CLAP's hexadecimal control/stored-parameter
keys for UI changes and external edits observed on refresh.

Delayed host echoes retain up to 256 recent expected values within the TTL.
The original one-shot `expectEcho` replace/rearm contract remains. Refused sets
clear their guards. A set is skipped only when current value and last accepted
intent agree, so immediate undo supersedes an asynchronous set whose getter
still reports the old value.

CLAP now uses a bounded message-producer/audio-consumer control queue with
256 usable slots. A smaller callback event list retains the rest for later
blocks. Getter reads expose the last accepted intent until a successful callback
acknowledges it. Stop and format reactivation flush remaining controls while
inactive. Successful state recall clears older controls under the reactivation
gate; equal-state detection checks the actual chunk and pending controls. A
cached hash cannot hide a newer manual edit. No callback allocates, locks or logs.

N47's [live parameter replay](ENGINE-PARAM-REPLAY.md) connects successful
journal/history receipts through the existing Session attachment and owner
drain. The first-touch opener's clear reaches the same bookkeeping on Undo;
Redo acknowledges row presence even for an equal value. An owned transient
producer proof distinguishes an unchanged self-submission from history replay,
so accepting an already heard gesture does not replay its opener into the device.
The proof is absent from the saved operation and every History receipt.

No schema or op name is added. The existing window-owned
`ProjectDocument::parameterGesture` and timer drain supply the normal calls.
Headless devices and the CLAP protocol fake cover the ordinary engine tests;
the VST3 probe fixture exercises the real JUCE host callback path separately.
Native editor interaction, hardware and measured Live parity require their
platform validation.
