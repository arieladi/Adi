# Export normalization

Job.normalization captures a NormalizeMode (Off, Peak, Loudness), targetLufs,
truePeakCeilingDb and optional channelWeights. Off is the default and retains
the previous rendering/encoding path. The engine never reads app settings while
a job runs; the window maps the Export defaults into each job before enqueue.
Queue add/update validates the values and preserves them in its snapshots.
Peak targets 0 dB sample peak, as Live's Normalize. Loudness defaults to -14 LUFS
and a -1 dBTP ceiling, with the registry's -36..0 LUFS and -12..0 dBTP ranges.

A normalized job renders its selected compensated source and cropped range
once, with the existing preroll and project snapshot. Private double PCM keeps
a wide graph intact until final quantization; plug-ins never run a second time
to obtain normalization gain. Analysis excludes preroll and includes intentional
silence at negative compensated positions. Capture and all subsequent I/O,
measurement and conversion run off the audio callback.

Peak gain divides by the actual sample peak. It does not confuse sample peak
with true peak or impose the Loudness ceiling on Peak mode. Silent Peak audio
keeps unity gain; configured PCM dither still behaves as before. Ratio-based
application also handles tiny double sources without an infinite reciprocal.

Loudness uses N13's K-weighting, 400-ms/100-ms-hop integrated absolute/relative
gates and reconstructed true peak. Source LUFS must be measurable over at least
400 ms; otherwise the job fails with a reason to choose Peak or Off. All output
channels are full-range by default, matching N13. Explicit weights use front 1,
surround 1.41 and LFE 0; no LFE role is inferred from an ordinal. One weight per
channel, finite in 0..2, is required when supplied. Meter support now spans the
renderer's 8--768-kHz range. Its bounded histogram has N13's 0.1-LU gate precision;
these are mathematical/generated-signal tests, not certified or measured Live
parity.

Loudness gain initially targets the requested LUFS, capped to the available
true-peak headroom. This is uniform gain, preserving the source's dynamics.
When the ceiling prevents the requested target, the result says so and reports
the achieved encoded loudness. It does not claim a limiter or an exact target
that the source cannot reach. A large boost can change absolute-gate membership;
the worker corrects gain against the encoded result within the meter's 0.1-LU
precision when the ceiling permits it.

The worker decodes and measures the actual encoded WAV before publication.
Quantization and dither can move its true peak past the requested ceiling;
if necessary the worker lowers gain and re-encodes the same private capture.
The original dither seed restarts for each attempt, so the completed file has
one destination dither stream, not accumulated noise. Attempts are bounded:
a target/ceiling it cannot satisfy fails without publishing. Source and encoded
true-peak measurement flush the FIR tail; that padding does not extend the WAV
or its integrated-loudness window.

A completed Result.normalization contains optional sourceLufs,
sourceSamplePeakDb, sourceTruePeakDb, outputLufs and outputTruePeakDb, plus
appliedGainDb, ceilingLimited and encodingPasses. Missing values mean no
measurable loudness/peak, not numeric zero. encodingPasses counts conversions
of the captured PCM, never repeated device renders. Off leaves the report absent.

Cancellation and host servicing continue between conversion/verification
chunks. Rendering progress can reach its final frame before encoding and
verification finish; the job stays Running until no-clobber publication.
Failed/cancelled jobs remove only their private staging. An output created
concurrently remains untouched. Queue retries create a fresh normal job.

RMS targeting, a brickwall limiter, linked batch normalization, other containers
and the broader REAPER export parity set remain separate increments. NI Stems
uses its own subsequent codec/container work.

NI Stems jobs require normalization Off until verification after AAC encoding
exists (PLAN EX-10). Peak and Loudness are refused by the shared NI validator,
including Queue add/update and direct `renderNiStems`, before opening the source,
servicing a host or creating output. A refused queue edit leaves its pending job
unchanged. Ordinary WAV normalization retains the behavior above.
