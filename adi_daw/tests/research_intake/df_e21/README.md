# DF-E21 hosted transport acceptance

This is an opt-in acceptance subset for ADR-0245 d8, using ADI's real `Session`,
VST3 adapter, scanner child and export runner. It does not implement the cloud
engine work. No reference DAW, installed third-party plug-in, audio device or
personal plug-in registry is used. All project files, media, scanner state and
trace mappings belong to the test and are removed on exit.

The test loads the owned VST3 reporting-delay classes (1024 and 4800 samples),
followed by the owned zero-latency observer. It uses the same mapped trace ABI as
the director's research. The readout is copied after synchronous processing;
the plug-in records into bounded memory during processing. An absent or missing
device is a failure, not a substituted mock. Discovery is restricted to the
built fixture file through the production scanner's isolated child.
The loader checks each real instance's reported latency after preparation,
when JUCE has refreshed its latency cache. Both fixture devices have the existing
`always_process` project setting enabled so the observer also runs during the
delay's initial silent fill.

`targets.cmake` is included after the production hosting and research-probe
targets. It builds `adi_df_e21_acceptance` on Windows with `ADI_WITH_JUCE=ON` and
the research time-probe target present. The owned harness uses the project's
strict warnings and `/WX` on MSVC. The initial validation target is Windows x64.

## Gate

`ADI_ENABLE_DF_E21_ACCEPTANCE` defaults to **OFF**. The implementing cloud branch
must explicitly set it to **ON** in its CI configuration to register
`adi_df_e21_host_acceptance`. An enabled gate with unavailable platform/hosting
dependencies is a configuration error. There is no branch-name guessing, expected
failure inversion, or silent runtime skip. Main's missing transport must fail
when this test is deliberately enabled.

`adi_df_e21_oracle` remains enabled on the supported build leg. It checks the
independent mathematical oracle and bounded fixture readout; it does **not**
claim that hosted transport works. The executable's `--acceptance` argument also
provides an explicit local invocation, useful for proving the pre-implementation
failure without turning that failure into the normal default CTest result.

## Covered by the strict subset

Each case uses both reporting delays at 48 kHz, 2048-frame driver callbacks:

* Negative input positions at project start, including negative musical and bar
  positions. No negative authored timeline is invented: the device's incoming
  sample clock is negative because of its upstream latency.
* A seek issued between callbacks, with independently tracked expected position.
* A 120-to-90 BPM change at sample 48000. Musical time is a piecewise analytic
  function of the device's input sample, not beats subtracted at the current tempo.
* A 1531-sample loop, shorter than the 4800-sample delay. After filling the
  pipeline, every device callback must stay inside its own input-clock loop
  interval; modulo wrapping is independent of production conversion helpers.
* Compensation off, with the observer receiving the global heard-time clock.
* `render::renderWav`, with mode 2, contiguous input positions, original sparse
  source markers and exact stereo float PCM including every intervening zero.
  The offline clock's origin is anchored to unique input audio markers, not to
  the plug-in's claimed first timestamp.

All contexts must contain the required validity flags, playing state, tempo,
4/4 meter and bar origin. The real-time source is an engine source node producing
constant audio so silence suspension cannot hide the hosted calls. Its dense
sample arrays are not interpreted as a sparse marker measurement. The offline
case uses an original temporary WAV with unique sparse markers.

## Remaining contract coverage

This subset does **not** close DF-E21 by itself. CLAP's full transport/readout and
reporting-delay variants are still required, as are independent automation and
note-event clock assertions, latency changes during playback, meter changes,
and the freeze/bounce paths. Those omissions must remain visible in the engine
seat's completion report. The gate does not authorize an ahead-of-time engine
or a processor-efficiency conclusion.

## Baseline validation

The Windows x64 MSVC build passes with the owned harness compiled under `/W4
/WX`. The default oracle/readout CTest passes. Explicit `--acceptance` runs
discover and prepare both real reporting-delay classes and the observer, then
fail all twelve cases at `VST3 ProcessContext is missing` on the pre-DF-E21
engine. This is the expected baseline failure, not a passing implementation or
proof that assertions after that first failure have executed. The implementing
branch must enable the gate and pass every later clock and PCM assertion.
