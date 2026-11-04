# The window's parameter producer (N7)

`ParamOps::uiEvent(deviceId,paramId,kind,normalized,nowMs,out)` is the engine
lift of the interface in `537c56d`. Forward Begin/Value/End on the owner/message
thread, using the descriptor id and the timer drain's monotonic clock.
Call `attachSession(session)` after load and refresh; it also detaches retired
devices. Each device's synchronous UI capture is separate from its plug-in's
single-producer callback ring, and is never handed to the plug-in.

Values reach the live plug-in immediately. End appends the gesture's final
`device.setParam` plus its first-touch opener, when needed. Commit the batch as
one undo transaction. Keyboard/unbracketed values close on drain's 150 ms quiet
timer, even without another UI event. A removed/replaced device, external
parameter edit or recalled state cancels stale UI completion. Normalized input
clamps to 0..1; invalid/nonfinite, missing and non-automatable targets are refused.
A refused backend set is not captured as an accepted UI value.

Use the timer drain's commit path for UI events too: commit every appended
request, writing `writeBlobs`/`takeBlobs` first. The bool return describes the
current event's acceptance; an older edit/state interval can have appended
requests even when this event is refused. Devices already hold the accepted
final values; do not replay each own opener back into them.

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

No schema or op name is added. The window owns wiring its existing
`ProjectDocument::parameterGesture` stub. The engine tests use headless devices
and the CLAP protocol fake; JUCE/native editor and hardware parity require
their platform validation.
