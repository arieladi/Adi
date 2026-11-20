# Owned host diagnostics

Provenance: the director's research. Original code is MIT; the standalone builder's
header dependencies are the exact public SDK revisions in `fetch_sdk.py`. These are diagnostics, not musical
effects or production plug-ins. No reference-host result is distributed here.

- `time/probe/`: VST3 **ADI Time Observer** (zero latency), **ADI Pure Delay 1024** and
  **ADI Pure Delay 4800**. Use default gain 1, generator off and work 0 for pure delay.
  Delay is in samples and is reported through the audio-processor interface. The observer
  preserves raw context bytes, validity bits, block/process mode, sparse input/output
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

The standalone `build.py` remains a Windows x64 MSVC developer-terminal builder
(C++20). The project CMake targets support Windows, macOS and Linux. Run all three self-tests
from the output directory. They load only these owned diagnostic modules. The time test
checks exact delays across irregular blocks, 32/64-bit samples, automation offset and raw
transport fields. Passing does not prove a DAW's compensation or transport policy.

## Project CMake targets

`targets.cmake` is included by the hosting build after `adi_warnings` and
`ADI_THIRD_PARTY` exist. It performs no download or installation. Unlike the standalone
historical builder, it uses the same JUCE-embedded VST3 public headers as ADI's host,
plus the project's pinned CLAP headers. No second VST3 implementation or SDK object
library is linked into these diagnostics. SDK headers are system includes; all owned
sources use `adi_warnings` and warnings-as-errors, including MSVC `/W4 /WX`.

| CMake target | Windows module |
|---|---|
| `adi_research_clap_probe` | `ADIEngineClapProbe.clap` |
| `adi_research_vst3_probe` | `ADIEngineProbe.vst3` |
| `adi_research_time_probe` | `ADITimeProbe.vst3` |
| `adi_research_scan_probe` | `ScanProbe.vst3` |

Build `adi_research_probe_targets` to build all four modules and, by default, their
three original standalone self-test executables. CTest names are
`adi_research_clap_selftest`, `adi_research_vst3_selftest`, and
`adi_research_time_selftest`. Each receives its actual module path; they do not search
installed plug-ins. `ADI_RESEARCH_PROBE_SELFTESTS=OFF` omits only those executables.
The scanner is a metadata/failure fixture; its behavioral acceptance belongs to the
isolated scanner tests, not to an audio self-test.

Windows hosts and acceptance harnesses must use
`$<TARGET_FILE:adi_research_time_probe>` (also exposed as
`ADI_RESEARCH_TIME_PROBE_PATH`), a flat PE VST3 module. Never assume a single-config
output directory. The three class identities, shared trace layout and marker threshold are
unchanged by this build integration. macOS emits `.vst3`/`.clap` bundles with
`Contents/Info.plist` and `Contents/MacOS/<executable>`; self-tests receive the
actual executable path and invoke the appropriate VST3 bundle entry/exit.

`platform.h` implements Windows named mappings and POSIX `shm_open`/`mmap`,
exclusive keeper ownership, open-existing recorders, release/acquire publication,
monotonic clocks and real module loaders. The ABI rejects non-little-endian or
non-64-bit platforms and requires lock-free shared counters. Self-tests verify
invalid and missing maps, ownership cleanup, bounded overflow and actual module
trace records. See [PORTABILITY.md](PORTABILITY.md) for the name/clock contracts
and the distinction between implemented support and native validation. CI builds
and runs these diagnostics on both hosting legs; the DF-E21 host-level acceptance
gate remains Windows-only.

The historical Python capture helpers remain **Windows-only**; the C++ port does
not claim to port those external recording workflows.
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
