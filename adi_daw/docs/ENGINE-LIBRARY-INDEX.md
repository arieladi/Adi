# Library metadata scan cancellation (N50)

`library::Index::scan(volume, relativeFolder, stop, observer)` performs the
metadata pass before queuing content hashing. Existing one/two-argument callers
retain their behavior. A scan is an off-callback operation; its caller retains
the Index through the call and joins its scan task before destroying the Index.
`Index::cancel()` still stops the separate hash worker. The scan token stops only
that metadata scan and does not cancel unrelated hash work.

The optional observer runs synchronously on the scanning thread, outside the
Index database mutex. `Discovering` reports visited directory entries and files
accepted for indexing, initially zero and then after each entry (including
skipped entries). `Publishing` announces that discovery finished, before the
transaction or mutex acquisition. Counts are progress, not committed rows.
The observer may inspect `entries()` or request stop; it must not recursively
scan or mutate the Index. Standard or nonstandard observer exceptions become a
named failure before publication. There is no observer call after commit: the
returned `ScanResult` is the terminal notification.

A pre-cancelled token refuses before volume/path validation and sends no
notification. During discovery, cancellation stops before another entry. Mutex
acquisition for a stoppable scan polls cancellation; the unstoppably defaulted
call retains ordinary locking. Publication checks stop between file/missing-row
updates and immediately before commit. A cancelled transaction rolls back before
`cancelled=true`, `ok=false` and zero committed/queued counters are returned.
The previous complete index, missing flags and hash queue survive. A later scan
can retry normally. The commit is the completion boundary; a later stop does not
undo published metadata or suppress its hashing queue.

Any other failure (filesystem, SQLite or observer) also returns `ok=false` with
zero counters: the transaction rolled back and queued nothing. A file deleted or
renamed between enumeration and its metadata read is skipped, not a failure; it
stays out of the scan, so publication marks a previously indexed copy missing.
Every other filesystem error still fails the scan.

Cancellation is cooperative between filesystem/SQLite operations; it does not
interrupt an operating-system call already in progress. Hashing, source default
annotations, relative-folder confinement, normalization/collision checks and the
application library schema/version remain unchanged. No project op, format minor
or window implementation is introduced. The window supplies its task's stop token,
forwards/coalesces progress and handles the returned completion/cancellation.

## Hash-keyed metadata exchange

Library JSON v1 carries tags, user BPM and rating. V2 adds authored sample
settings. V3 adds an optional authored `key` (a `tonic` from C = 0 through
B = 11 and a boolean `minor`) and optional `analysis_metadata` per content
hash. Export selects the earliest version that can express its fields.
An explicit `key: null` in v3 clears the authored key. A measurement's tempo
and key use the `file`, `acid`, or `detected` source. An observed key also
carries its strength. The existing top-level BPM remains the user's value.

The owner passes local `SampleAnalysisMetadata` values to `Index::exportJson`
along with the durable annotations. `Index::importJson` writes authored data
in one transaction, then returns any observations to its caller. Use
`portableSampleMetadata` to serialize measurements and `applySampleMetadata`
to apply observations to the matching content hash. A user value precedes a
file value, then ACID, then detection. A local value wins when the sources
have equal priority. ADIL stores the authored values and `sample_settings`.
The owner can release observations alongside the rest of its library analysis.

An owner can give `SampleAnalysisCatalog` a captured `WarpTimingSettings`.
The worker passes each decoded block to both the descriptor analysis and the
fixed-rate Warp analyser. ALIB and AWTM publish under the same verified
source hash. A warm job reuses both. A timing configuration change keeps
the existing library descriptors and computes the requested timing once.
