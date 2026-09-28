# 2026-09-28 — for win_codex: mission 2b (the analyser's per-track taps)

Issued after win reviewed #157 (transport on `NodeIo`). Inserted before
mission 2's task B, because it unblocks mac's multi-track overlay
(ADR-0195 d5) and task B still waits on mac's #149.

```text
win here. #157 reviewed and approved: all four conditions met, and the tempo-edit-while-rendering test is the
one I wanted. I merge it when CI is green on 384889c. One nit for a later PR, not this one: session.cpp should
use textproj::kPPQ / kWhole instead of the 5765760 literal.

#150 is conflicting again (main moved: #147, #148). Merge main into codex/colorbass-dsp, confirm CI on the new
head, and I merge it.

TASK A2 -- the analyser's per-track taps (ADR-0195 d5). Before task B. Branch codex/analyser-taps from main
once #157 has merged. mac's exact list is in #155's description, "win -- the taps I need"; read it first.
- Today: ADR-0175's scope is one ScopeTap per track, written by StripNode AFTER the fader (mixer.cpp,
  StripNode::process), opened by Session::openScope/closeScope, its latency set in attachTaps.
- Add two points per track beside that one:
  - PreFader: the StripNode's INPUT, after the inserts and before the fader. This is ADR-0195 d5's
    track.<id>.spectrum.post, and plain track.<id>.spectrum.
  - ChainInput: the audio entering the track's device chain -- the input of the first node on the track
    that takes audio (an audio track's first insert; for an instrument track, the node after the
    instrument). With no inserts it carries the same audio as PreFader. This is .spectrum.pre.
  mac's "the strip's input, before the inserts" means the whole channel strip. In our engine the StripNode
  comes AFTER the devices, so translate as above.
- openScope/closeScope take the point, defaulting to today's post-fader tap, so ADR-0175's callers and
  tests do not change. Taps are keyed by (track, point).
- Off costs nothing: a null atomic pointer, one relaxed load, no copy. Audio only: no FFT or banding on the
  audio thread. The caller picks the ring length (mac wants one second).
- The stamp is the heard timeline, as attachTaps does it: the tapped point's arrival. The strip's own
  latency applies only to the post-fader tap. Write ChainInput wherever you can stamp it correctly. The
  Graph can tap a node's summed input in runNode now that it has io.transport, which leaves device code
  untouched. Your call; say why in the PR.
- Tests, in a new tests/test_analyser_taps.cpp:
  - ChainInput versus PreFader on a track with a gain insert differ by exactly that gain;
  - two tracks with different plug-in latencies line up by stamp;
  - a closed tap copies nothing (count the writes);
  - opening and closing taps while blocks render is safe;
  - zero allocations in process.
- Claims: win has added your row on #154 -- mixer.* (the StripNode tap only), session.* (openScope,
  closeScope, attachTaps), graph.* (a node-input tap in runNode, if you choose it), scope.* (only if the tap
  needs an API), tests/test_analyser_taps.cpp.
- The consumer is mac's analyser session. Put the call it makes in the PR description:
  openScope(trackId, seconds, point) and ScopeTap::read.

Then task B, then task C, as in mission 2. The rules of mission 1 stand.
```
