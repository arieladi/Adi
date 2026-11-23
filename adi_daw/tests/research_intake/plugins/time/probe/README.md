# Transport observer and reported-delay fixture

Original diagnostic code from the director's research, MIT licensed. This module is a test fixture, not a production effect or a performance comparator.

Build with an x64 MSVC developer terminal and VST3_SDK_ROOT pointing at a directory containing pluginterfaces/. Run build.cmd. The external Steinberg vst3_pluginterfaces dependency is pinned to 4f547e8e102b47de4a8b8aaf343c73b700786372 (MIT). No vendor binaries are bundled.

The three VST3 classes are:

| Class | Canonical VST3 class ID | Reported delay and tail |
|---|---|---:|
| ADI Time Observer | 41444954494D4550524F424500000001 | 0 samples |
| ADI Pure Delay 1024 | 41444954494D4550524F424500000002 | 1024 samples |
| ADI Pure Delay 4800 | 41444954494D4550524F424500000003 | 4800 samples |

All support stereo float32 and float64. The optional auxiliary stereo input is declared but does not affect the output. At default parameters the delay classes perform only a fixed, sample-exact stereo delay; activation clears their memory. There is no automatic reset triggered by the transport context. The host may deactivate/reactivate at a seek, so retain those circumstances when interpreting a run.

Parameter IDs equal their index: 0 is Probe ID, normalized id/4095; 1 is linear Gain, 0..1, default 1; 2 is the inherited synthetic Work control, default 0; 3 is the inherited 997-Hz generator switch, default off. Keep Work=0 and Generate=off for all transport tests. There is no latency parameter: instantiate the desired fixed-delay class. Put a separately identified zero-delay observer before and after it and on an independent reference path.

The existing engine/PCM headers support optional older instrumentation; neither of those mappings is required for the new time records. The new recorder uses a separate named shared mapping chosen by ADI_TIME_MAP. Create and hold that mapping before launching the host, and pass the same environment value to the host process. Plug-in construction opens it once. A scanner process can inherit the environment too: preserve the captured process IDs and distinguish scanner and audio-host records.

From the folder containing time_capture.py:

    python time_capture.py serve --name Local\ADI_Time_Test --stop-file stop-keeper
    python time_capture.py capture --name Local\ADI_Time_Test --seconds 8 --output results --label case

The keeper stays alive until the named stop file exists. On a later launch, use a fresh stop-file path or remove your own old sentinel first. An importing caller can use Capture(name), arm(), stop(), save(folder,label), close() instead; this is preferable when playback and capture must be ordered explicitly.

The records preserve raw context bytes, context presence, all validity/state flags, sample and musical positions, continuous position, tempo, time signature, bar and loop bounds, process mode, block size, precision, process/thread identity and QPC. Fields whose VST3 validity bit is absent are retained as raw values and **must not be treated as valid**. Project sample time/sample rate still require a present context. A context's existence does not prove that it describes the plug-in's input audio.

Each record retains up to 32 input and 32 output samples whose absolute value is strictly greater than `1e-7` from main channel zero, plus 64 queued parameter points. The same threshold applies to total counts, so they reveal truncation of qualifying markers only. Samples at or below the threshold are intentionally omitted from this sparse trace, including tiny anti-denormal signals; audio processing, RMS/PCM instrumentation and output silence flags remain unchanged. Threshold comparisons use the represented float32/float64 sample converted to double, so an apparent decimal boundary can lie slightly above or below the threshold. Use sparse impulses with unique amplitudes; this is not a general PCM recorder. Input markers are captured before any in-place output overwrite. Parameter points preserve their offsets and reported values; processing applies this fixture's gain as a step at the supplied sample offset.

The bounded map holds 16384 records of 2488 bytes (4096-byte reserved header). It never overwrites an old record: additional records increment the lost counter. The caller must reject traces with loss, bad commits or relevant marker/parameter truncation. A capture should arm only while instances are quiescent if resetting the shared counter; otherwise an in-flight writer can straddle the reset. Stop capture, allow writers to finish, and check commits. Do not use the logger's cost to rank hosts.

Standalone selftest.cpp loads the actual compiled module. It checks fixed delays across irregular block lengths in both precisions, reset on activation, latency/tail reporting, negative and musical raw context, and gain automation at offset 3. The threshold tests additionally exercise exact silence, alternating tiny input, a later impulse and threshold boundaries in both precisions and all three delay classes, checking unchanged audio samples and output silence flags. See the current standalone receipt for the check count; all 134 records must commit without loss. This establishes the fixture's behavior only; it establishes nothing about any host.

For host results, pair markers by their encoded source identity, then compare source sample index to context projectTimeSamples + marker offset. Keep input versus output markers distinct. Compare zero and latent paths, routes and discontinuities; do not compare wall clocks from independently scheduled tracks and call that PDC. An offline process-mode observation does not establish the host's rendering quality or exact export latency trimming.

The public fixture serves ADR-0245 d8 / MX-04 / DEV-26 and the transport part of DEV-23. Host projects, renders, captures and machine-specific launch scripts belong outside the repository.

