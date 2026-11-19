# Owned host diagnostics

Provenance: the director's research. Original code is MIT; header dependencies are
the exact public SDK revisions in `fetch_sdk.py`. These are diagnostics, not musical
effects or production plug-ins. No reference-host result is distributed here.

- `time/probe/`: VST3 **ADI Time Observer** (zero latency), **ADI Pure Delay 1024** and
  **ADI Pure Delay 4800**. Use default gain 1, generator off and work 0 for pure delay.
  Delay is in samples and is reported through the audio-processor interface. The observer
  preserves raw context bytes, validity bits, block/process mode, input/output nonzero
  markers and parameter offsets. Fixed marker arrays have explicit total/truncation counts.
- `clap/`: zero-latency CLAP probe and hard-realtime-declaration variant. Plain-valued
  parameters: ID, gain, synthetic work and optional sine. The old CLAP trace records
  seconds-position when valid, flags and steady time; it is **not** the enhanced VST3 raw
  musical/context trace. Do not infer unrecorded musical fields from it.
- `vst3/`: the retained zero-latency VST3 engine probe, including declared sidechain,
  process-mode/prefetch capability, sample-offset gain and bounded PCM capture. This
  remains separate from the new time/delay module so its original diagnostic API survives.
- `scanner/`: metadata-only two-class VST3 factory; no working audio instance. Null factory,
  owned-child exit and bounded sleep modes test isolation. See `cases.json` before use.

Build all from a Windows x64 MSVC developer terminal with `build.py`. Run all three self-tests
from the output directory. They load only these owned diagnostic modules. The time test
checks exact delays across irregular blocks, 32/64-bit samples, automation offset and raw
transport fields. Passing does not prove a DAW's compensation or transport policy.

`time/time_capture.py serve --name NAME --stop-file FILE` creates a bounded mapping before
the host starts. Set `ADI_TIME_MAP` to the same NAME in the host's environment. In another
terminal, `capture --name NAME --seconds 8 --output DIR --label CASE` arms and saves it.
Use a unique mapping per session, inspect lost/commit/truncation fields and retain raw
context validity flags. An existing host does not inherit a later environment change.
Do not publish captured application data with this source bundle. The shared older trace
in `probe_shared.h` uses a separate `ADI_ENGINE_MAP`; optional host-extension metadata
falls back to unavailable sentinels when its provider is absent.

For ADR-0245 d8 acceptance, compare the observer before and after a fixed delay, including
negative preroll, tempo-map changes, loop wrap, seek, return/group placement and offline
mode. A process mode enum or position alone is not proof of audible timing. Use impulse
markers and stepped gain automation with declared offsets, and inspect all overflow flags.
