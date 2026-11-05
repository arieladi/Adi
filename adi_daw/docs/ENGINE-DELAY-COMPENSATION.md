# Delay Compensation switch (N24)

The engine accepts the app's choice in SessionSpec.delayCompensation (true by
default) and Session::setDelayCompensation(bool) on the message thread. Like a
precision change, a changed choice builds and publishes a new graph; repeating
the current value does nothing. Existing loaded devices are retained. The
window feeds this from the win-owned app setting/Options menu row; the proposed
registry key is audio.delayCompensation, default true. No project op, schema,
registry row or UI production code is added here.

With compensation enabled, existing main/sidechain phase alignment and signed
Track Delay behavior are unchanged. With it disabled, main and sidechain edge
alignment delays are zero, with no compensation headroom rings. Latency reports
can still update without creating those rings. Scope input stamps use heard
global time while alignment is disabled.

Intrinsic device latency is still physically present and cached/reported. The
maximum path arrival/latency remains available for diagnostics; with unaligned
inputs it is the latest path, not a claim that every path was aligned. MIDI
through a node still follows that node's physical latency, but receives no edge
alignment delay. A sidechain still carries audio only. Delayed dry bypass stays
at the device's own latency (ADR-0203 d4), including its independent headroom and
retap/escalation requirements. The setting never changes a device's DSP delay.

Session also suspends every track's effective Track Delay, including the delay
read by its sends/feedback path. Model and SQL delay_samples stay unchanged.
Editing Track Delay while the switch is off stores the new value without playing
it; re-enabling uses that latest value. Standalone MixerStrips::sync callers can
pass the same bool as their graph; its default remains true for compatibility.
Standalone Graph and GraphHost expose setDelayCompensation before preparation or
publication respectively. Never mutate the policy of a graph concurrently with
audio; publish a new one as Session does.

Failure is reported through the existing rebuild result/error path. Graph
handover and optional fade behavior are unchanged. Re-enabling alignment after
it was off may need new delay history, as any newly introduced delay does; the
existing graph policy owns that transition. No hardware or Live parity timing
is claimed by the synthetic signal/event tests. The window/registry integration
remains their lane's part of N24.
