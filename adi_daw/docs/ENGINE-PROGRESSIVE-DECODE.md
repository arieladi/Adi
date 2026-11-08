# Progressive compressed clip playback

`engine/progressive-decode` integrates the independent native AAC import and
bounded PCM stream prerequisites. Plain compressed clips with an exact cheap
container length can play a flushed PCM prefix before the source finishes
converting. The callback still consumes only the existing PCM page buffers.

`inspectPcm` opens the native codec/transport off the callback and returns rate,
channels and an optional exact frame count. It does not scan a headerless MP3
for its duration. The normal unwarped `prepareImportAudio` command captures
these dimensions and source hashes without warming the complete decode cache.
The older `media::prepareImport` overloads retain complete validation; the new
explicit header-only overload is for this material command. Header-only import
can therefore commit a file with a damaged later PCM payload: playback reports
the eventual worker error. Source files must be closed and stable throughout.
There is no guessed duration and no growing-recording-source support.

`ProgressiveDecoder` owns two codec workers by default (configurable 1..8) and
admits at most 32 running plus queued jobs. Exact byte hashes coalesce live
leases, including paths with identical bytes. A later open can retry a terminal
failed/cancelled job while old playback generations still retain its diagnostics. The cache must outlive the service
and its returned files. `SessionSpec::progressiveDecoder` optionally selects an
app-owned service; null uses the process service/cache. Its lifetime includes
retired graph generations. No Store or SQLite connection enters a decode job.
Opening/hashing/probing, reading, cancellation, diagnostics and all destruction
belong off the callback. Immutable metadata and atomic snapshots can be read
without the job mutex. The open stop token governs admission, hashing and probe;
`ProgressiveFile::cancel` cancels the shared job for every lease. Dropping the last
lease cancels automatically. Service destruction cancels and joins its workers.
A test/service checkpoint must return promptly when its job token requests stop.

Workers use the bounded owner-thread PCM stream for RIFF/AIFF/FLAC/MP3/Ogg, or
`decodeAacPcm` for supported mono/stereo AAC-LC in M4A/MP4/ADTS. The AAC sink is
synchronous, interleaved and valid only during the call; exceptions stop native
delivery. The old AAC WAV API wraps this sink, retaining gapless edit-list
priming and authored duration (raw ADTS keeps every coded frame). Windows/macOS
first make the existing private native compressed rewrap; Linux uses the pinned
FFmpeg 60/61 adapters. No external decoder executable or Python runtime is used.

Each job reserves an exclusive owner-only directory under `decoded/progressive`.
Its Float32 WAV is private. Every writer block updates and flushes a complete
prefix header before releasing the ready-frame count. Page-worker reads lock
this file state, open a short-lived WavReader, read only an available whole
range and close it before unlocking. The writer, reader and final publication
share this lock, so there is no partial header, unwritten-hole read or open
prefix reader during a Windows rename. The final source hash pass runs outside
this lock so already decoded pages remain available during that pass.

A missing prefix returns Pending, preserving the destination. The page worker
retries after its normal idle yield; it publishes no silent ready page and
increments no error for waiting. Source-window offsets and resampler context
must all be available before a page is published. Genuine failures/cancellation
produce a terminal read error once per clip; `prime` returns false promptly.
The owner can copy `ClipPlayback::decodeProblems()` to display the actual terminal
asynchronous error; construction problems remain in the existing `problems()`.
The callback never opens a codec/file or waits on this job. Without driver-side
priming, ordinary page underruns remain possible and use the existing zero/count
policy; the engine does not silently block a running audio driver.

Admission checks the complete expected Float32 size plus header allowance,
queued/running reservations, cache budget and minimum disk headroom before
creating the prefix. It does not guarantee space against other processes using
the disk later. When decoding and final source identity succeed, the private
writer closes and no-clobber publication makes the complete hash WAV visible.
An existing real target must have the exact expected rate, channels and frames;
its bytes are never overwritten. These checks trust the app-owned cache, as the
legacy cache does; they are not authentication against malicious same-shape PCM.
Each completed job owns one ordinary cache pin until its last public lease closes;
multiple leases to that job share that pin. Separate services hold separate pins.
Ordinary LRU eviction/settings apply to completed targets. Private files are
removed when their job retires; crash leftovers need cache maintenance and are
never mistaken for complete entries. This change adds no startup orphan reaper.

Warp preparation and compressed sources without a cheap exact nonempty length
still use complete decoding before playback. Warp also retains its existing
full transient-analysis import path. The job system is not a general-purpose
inference/export queue and adds no project schema, opcode, blob or UI contract.

The focused suite holds real Vorbis and native AAC jobs after a usable prefix,
then plays actual Sessions at 32/64 without completing or warming that cache.
It compares 44.1 kHz resampling, source offsets and loop seeks against a separate
complete WAV, and exercises shared leases, queue/worker bounds, cancellation,
source mutation, concurrent publication, disk limits, pins and callback I/O and
allocation guards. Platform execution and final gate details are in the engine
lane log; Linux evidence does not establish Windows/macOS execution or Live parity.
