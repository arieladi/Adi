# NI Stems transport, queue jobs and import

ADR-0191 d4, ADR-0204 d5 and ADR-0205 d8 choose an ADI MP4 transport and native
platform AAC, dynamically loaded libavcodec on Linux. This increment supplies
that worker layer without JUCE, a queue job, and an undoable import producer.
It does not implement Pioneer's USB format or claim a Traktor certification.

Set `render::Job::niStems` to a `NiStems` containing one to four `NiStem` values:
each has an existing direct wet `Source`, a name and an optional `#RRGGBB` label.
Select two channels and 44.1 or 48 kHz in the session. The Main post-fader output
is the premixed master. All five AAC-LC tracks use the same rate and bit rate
(default 256000, supported range 64000..320000 bit/s per stereo track). The
queue calls `renderNiStems`; calling `renderWav` with NI options refuses the job.
Naming, region selection, stable queue identity, retry and no-clobber output
publication keep the WAV queue's contracts. The output suffix is `.stem.mp4`.
Progress measures sample-frame work over five streams; `framesWritten` remains
the audible frame count of one stream.

A SQLite Backup captures one database revision, including its WAL and device
state. Media paths are verified and made absolute in that private copy. Every
source renders from the same copy, using the ordinary compensated, cropped
WAV renderer and a fresh Session. Edits to the live project cannot change a
later stem. A plug-in's own nondeterministic or external state is still its
responsibility. These are direct wet stems; summing them need not reproduce
a nonlinear processed Main. The three unused slots of a one-source job contain
actual encoded silence: the writer emits four stem slots for existing NI/Mixxx
readers. Generated mastering compressor/limiter metadata is disabled because
Main already carries the project's processing. Float WAV staging feeds AAC;
this is a lossy delivery format, not an archival 64-bit PCM path. Nonfinite or
out-of-range AAC input fails with a request to lower render gain.

`prepareStemImport(Store&, path, options)` captures a complete private container,
parses its labels, decodes each stem and prepares native media/track/clip ops.
It changes no project rows. Allocate a group id, container media id and one
track/clip/media identity per actual stem in `StemImportOptions`. Supply the
position in project ticks, optional group parent/index, group name and the
caller's current document generation. `commitStemImport` checks that generation,
the actual project connection filename and the captured journal head, then
commits the group, tracks, clips and pool rows as one undo step. Musical extent
must round-trip the source's exact frame count; clips play Warp off. The
premixed master is retained inside the original container and is not another
playing track. Identical decoded assets deduplicate within the batch and
against the existing media pool.

NI RGB is not `tracks.color`: pass an explicit optional Live palette index
0..69 per new track, or leave its color NULL. The untouched original container
(including its opaque manifest bytes, unknown JSON fields and padding) is an
owned pool asset, returned by `containerMedia()`. Its hash and `format=stem.mp4`
identify it after reopen. There is no new group-to-container association table.
An enabled NI mastering compressor or limiter causes a named refusal by default.
The caller may explicitly choose `allowRawMastering`; `manifest().masteringEnabled`
then continues to report the unapplied processing. Import does not claim to
recreate the premixed master by applying that DSP.

Preparation, codecs and file publication belong to the worker/document owner,
never the driver callback. Stop tokens and worker service callbacks apply during
capture/decode/render/encode/mux. Publication inside a native import transaction
checks cancellation without pumping UI callbacks. Only a separate publication
copy is moved into the pool. Late SQL failure removes newly published assets
and retains the private originals, so the same prepared import can retry.
Existing destinations are reused only after full content verification; a
collision preserves the other file. Destroying a prepared job removes only its
own staging directory. Successfully imported pool files remain through Undo for
Redo/history. The caller must keep the Store on its owning thread and increment
the document generation when replacing/reopening its document.

`audio/mp4_aac.*` bounds every box, descriptor, table and packet reference.
It handles 32/64-bit boxes and chunk offsets, version-0/1 clocks and single
unit-rate edit lists, `mp4a/esds` AudioSpecificConfig, `stts/stsc/stsz/stco/co64`,
and `moov/udta/stem`. Packet addresses must lie in actual `mdat` payloads.
Priming and final padding are distinct from audible frames; writers use a
sample-rate movie clock for exact edit duration. Unknown boxes are skipped
within their parents. Limits: five audio tracks, one million packets per track,
1 MiB per compressed packet, 64 MiB movie metadata, 1 MiB manifest, JSON depth
32 and 100000 boxes at a level. Fragmentation, encryption, HE-AAC, non-stereo,
960-sample AAC, composition offsets, complex edits, mixed configs/rates and
unaligned tracks are named refusals. Ordinary non-NI AAC movies can use the
low-level transport but are not accepted by the NI import producer.

Linux shims use separately compiled, pinned headers for codec60/util58 and
codec61/util59; each checks both loaded majors before accessing structures.
A missing supported runtime is a named unavailable error. FFmpeg's command line
is not invoked by production code. macOS uses AudioToolbox; Windows uses Media
Foundation. The native adapters require explicit, sample-exact priming/duration
metadata. Some Media Foundation codec versions omit it: `aacCodecAvailable()`
probes the real native round trip once and returns false if alignment cannot be
verified. That Windows codec limitation remains open for export and import on those
versions; no delay constant is guessed. CoreAudio (measured on macOS 26) records
an export's priming/remainder as iTunSMPB rather than an edit list, and its MP4
reader ignores edit lists, assuming its own 2112-frame priming; the macOS adapter
therefore reads and sets the exact trim through CoreAudio's packet table, checked
against the encoder's reported priming. Native platform compilation and round-trip results require their CI
runners; local proof here is Linux GCC/Clang. Import still reports native decode
errors rather than substituting or renaming WAV bytes as AAC.

The format was checked against Mixxx `bc49961fd3e5a46cd46588090c652a0d7ec44e18`,
its StemInfoImporter/SoundSourceStem and its five-second sine fixture. That
independent fixture round-trips through ADI with all five FFmpeg-reported
streams at 5 seconds, 44100 Hz, stereo and preserved 1600-frame priming. No
reference audio or source was copied into this repository. Automated tests
generate their own signals, measure decoded zero-sample alignment, validate
container bounds and edits, render/import actual groups in both processing
widths, exercise one-step Undo/Redo/reopen and inject late publication failures.


Independent FFmpeg decoding of ADI-generated five-track signals at both rates
also aligns every stream at zero samples. ffprobe reports the exact requested
edit durations. FFmpeg's raw PCM command retains final AAC packet padding
(619/815 frames for those two fixtures); consumers must respect the audible
container duration. ADI's decoded imports trim it explicitly and test the exact
frame count, rather than treating all 1024 samples in the last packet as content.
