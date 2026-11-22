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

For a plain clip with a valid saved BLAKE3, Session first inspects the source
header and can pin the complete named decode when rate, channels and frames
agree. Those PCM bytes are the project's saved media. This warm route does no
whole-source digest. A dimension difference falls through to source hashing
before any cache hit, pin, eviction or job admission: changed bytes report
`decode.unreadable: progressive source differs from its saved BLAKE3`, exactly
as after the saved WAV is evicted. If the source hash still agrees, a wrong-size,
wrong-rate or wrong-channel cache WAV remains a `decode.cache_write` failure;
the damaged entry is neither overwritten nor treated as a source mismatch.
A cold route verifies the current bytes before sharing or starting a progressive
job, then rechecks them at the end of decoding. An absent or legacy digest uses
the ordinary hash path. This read policy leaves filesystem relink and Offline
diagnostics separate.

**Remaining DF-E34(a)/TM-MD-03 boundary:** a same-shape edit at the original
pathname still makes a plain warm clip play the saved cached audio, while Warp's
`playableFile` path hashes and plays the current bytes. After eviction, the plain
route refuses the changed source. The planned source-size/modification-time memo
is needed to make these routes agree; this dimension-mismatch fix does not add
that memo or claim same-shape mutation detection. Sources must remain stable
during inspection, verification and decoding.

Warp preparation and compressed sources without a cheap exact nonempty length
still use complete decoding before playback. Warp also retains its existing
full transient-analysis import path. The job system is not a general-purpose
inference/export queue and adds no project schema, opcode, blob or UI contract.

The focused suite holds real Vorbis and native AAC jobs after a usable prefix,
then plays actual Sessions at 32/64 without completing or warming that cache.
It compares 44.1 kHz resampling, source offsets and loop seeks against a separate
complete WAV, and exercises shared leases, queue/worker bounds, cancellation,
source mutation, concurrent publication, disk limits, pins and callback I/O and
allocation guards. The saved-digest acceptance opens 50 distinct FLAC sources
with completed cache entries, checks zero whole-file source digests and matches
the Session's first PCM block. It also replaces a saved source with a valid FLAC
of different dimensions, verifies the same refusal and silent real Session output
with a retained completed lease and after normal eviction, and checks that no
prefix or cache hit is created. An unchanged-source/damaged-WAV control retains
the cache-fault diagnosis after one digest. Cold verification and final verified
publication remain covered. Platform execution and final gate details are in the engine
lane log; Linux evidence does not establish Windows/macOS execution or Live parity.
