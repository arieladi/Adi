# Dynamic EQ in ADI's own host — delegated by win

The external build driver accepts dynamic-eq and delegates through
plugins/external/dynamic-eq to the existing adaptation of ZLEqualizer
3468a3ac85f5c1f9d16083acbee5b1339984d53b. No DSP or upstream source changed.
A fresh external build delivered ADI Dynamic EQ.clap with its own identity.

The opt-in dynamic_eq_adi_play CTest creates isolated project files, resolves
CLAP IDs and values through the delivered binary, and loads it through the
real JuceDeviceLoader/Session/ClapDevice path. The first run caught my fixture's
decimal parameter IDs: ADI persists them as eight-digit lowercase hex. The
corrected fixture asserts all four stored parameters were applied.

Win approved only --render-output WAV in mac DAW's src/juce/play.cpp. The
flag requires a positive finite --render; otherwise it returns usage exit 2
without opening anything. It captures the existing stereo offline output,
then, only after render and EditWatch succeed, writes a 32-bit IEEE float WAV
beside the destination under a temporary name and renames it. Failed gates
preserve old output; failed writes/renames cannot publish an incomplete WAV.
The capture is memory-bounded and rejects more than INT_MAX frames. Existing
flags and the EditWatch result (including exit 4) are preserved; no output
capture is allocated when the new option is absent.

Windows validation: MSVC Release host with ADI_WERROR=ON, external CLAP build,
and all four CTests passed (gestures, upstream processor oracle, CLAP ABI,
real ADI host render). At 48 kHz with 32/512-sample blocks and 96 kHz with
4096-sample blocks, dry/bypass were -18.1 dBFS and the 220 Hz -12 dB bell was
-30.1 dBFS. Bypass matched dry exactly; maximum settled sample error against
dry times the expected bell gain was 1.97e-6 at 48 kHz and 4.94e-7 at 96 kHz.
The test requires the 7011 Hz bin below 1e-7 before checking wanted-tone energy,
float format, exact frame count/rate, finite samples and stereo agreement.
Missing-device rejection, output-without-render, failed peak gates with/without
an existing destination, replacement on success, invalid output path and the
legacy no-output --render/--expect-no-edits invocation are covered.

Planted a bypassed cut: response test failed at 0 dB instead of -12 dB.
Planted capture.clear before writing: actual zero WAV failed wanted-tone energy
although the far bin was quiet. Both restored; final CTest 4/4 passes. All audio
was rendered offline to files, never speakers. Evidence is under the external
build's dynamic-eq/host-render directory; no artifacts are committed.

The heavy external test is opt-in via ADI_PLAY/ADI_TOOL and is not part of the
headless CI suite count. The top-level README count and win_codex.md index
were left untouched, per win's new rules. mac DAW session: existing CI calls
keep their prior behavior; only the new flag writes audio, after the existing
edit guard returns success.
