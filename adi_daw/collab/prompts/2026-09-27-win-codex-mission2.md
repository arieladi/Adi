# 2026-09-27 — for win_codex: mission 2 (transport on NodeIo, the color-bass devices, the Dynamic EQ)

Issued after #150's CI went green and #146 merged. Three tasks in order;
win reviews and merges each PR.

```text
win here, mission 2. #146 is merged (d66313c): run your follow-up (merge main into codex/colorbass-dsp,
confirm CI on the new head) and I merge #150. Then, in order:

TASK A -- transport on NodeIo. Delegated by win, now: it unblocks mac's analyser, and you are otherwise
waiting on mac's stacked PRs.
- src/adi/engine/** is win's; your claims row covers exactly: graph.hpp (NodeIo only), the session's
  per-block fill, and a new tests/test_transport_info.cpp. Branch codex/transport-info from main.
- Add a TransportInfo to NodeIo, filled once per block by the session. Fields:
  - playing;
  - the block's first timeline sample;
  - BPM at that sample;
  - time signature (numerator, denominator);
  - bar, beat, and ticks within the quarter note.
  Ticks within a quarter note stay below ADI_PPQ = 5,765,760 (SPEC 4.2), so they are exact as a Pd
  float. That is why ADR-0188 d3 sends them instead of an absolute tick count.
- Compute from the Transport (transport.hpp) and the snapshot's TempoMap (snapshot.hpp). Read how
  clip_playback.cpp and midi_clips.cpp already convert samples to ticks, and reuse that; do not write a
  second conversion. Find where the time signature lives in the snapshot, and say so in your log.
- Tests must prove:
  - bar and beat at a tempo change and at a time-signature change;
  - the loop wrap (the block after the wrap starts at the loop's start);
  - stopped versus playing;
  - identical values for block sizes 32 to 4096;
  - zero allocations in process.
- The consumer is mac's analyser session: its setTransport seam and adi.transport.pd are on #152. Agree
  the shape with it in your PR description, and read #152 first.

TASK B -- the color-bass devices, PR 2 (ADR-0192), once mac's #147 (pthreads4w) and #149 (the built-ins
hook) are merged.
- Register with ADI_PD_BUILTIN(adi.combchord~, adi_combchord_tilde_setup) beside your definition, plus
  one line in ADI_PD_BUILTIN_SOURCES. It is an OBJECT library, so nothing of mac's is edited.
- Color Cab's sample comes through [adi.sample]'s host side (mac's #153).
- Fold in my review points 3 to 5 on #150, each with its test: the State crossfade, the all-pass
  fraction range with coefficient smoothing, and Color Cab's make-up gain.

TASK C -- the Dynamic EQ (ADR-0195 d1, d2), after B.
- A CLAP in adi_daw/plugins/<name>/ (working name "ADI Dynamic EQ"; the director names it). It adapts
  ZL Equalizer 2's DSP directly (adi_daw/reference/ZLEqualizer; the pinned upstream build is in
  plugins/external/zlequalizer). The plug-in is AGPLv3: ZL's copied files keep their headers, and the
  source is named in the commit.
- The graph's gestures are ADR-0195 d2's table, from Pro-Q 3's manual (reference/DOCS/Plugins/
  FabFilter_Pro-Q_3, printed pages 9, 10 and 15). Read the manual, then write the behaviour in our own
  words; never copy its text or images. ZL's panel dials leave the main view.
- Make the gestures testable headless: pure functions from (drag delta, wheel, modifiers, filter type)
  to parameter changes, with tests for every row of the table. The GUI calls them.
- The DSP keeps ZL's response: test that the adapted filters match ZL's own magnitude response.
- By morning the drone will have summarised ZL's source: D:\adi-drone\missions\ref-zleq-dsp.md,
  ref-zleq-panel.md and ref-zleq-gui.md. Names are reliable there; any number must be checked in the
  source before you use it.

The rules of mission 1 stand: your own worktree, explicit paths, codex/ branches, never the main
checkout, CI by head SHA, renders to files and never to speakers.
```
