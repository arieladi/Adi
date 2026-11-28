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
along with the durable annotations. Use `portableSampleMetadata` to prepare
local measurements for exchange. `Index::importJson` stages observations and
publishes them after the authored transaction commits. The caller can receive
the original observations for explicit re-export. Imported fields last for
the receiving Index's lifetime. Later imports preserve higher-priority
observations and fill or replace supplied fields at equal priority.

The catalog and short Warp resolve the same verified content hash through
the Index. A user value precedes a file value, then ACID, then detection.
A local measurement wins an equal-priority tie with an exchanged value.
ADIL stores authored values and `sample_settings`. Observation-only imports
write no authored rows and leave ADIA intact.

The catalog keeps an immutable measured record and overlays the current
authored settings and exchanged observations from its borrowed `Index`. The
same listing reflects a completed save, clear, or JSON import on its next read. Previous query
results retain the immutable values they received. The Index revision poll does
no SQLite work while annotations are unchanged. Recent updates read only the
changed hashes from a bounded journal. An import or an interval too large for
the journal refreshes the current resolution inputs. Neither path
decodes audio or writes ADIA. The in-process Index owner publishes revisions
after committing the authored transaction.

An owner can give `SampleAnalysisCatalog` a captured `WarpTimingSettings`.
The worker passes each decoded block to both the descriptor analysis and the
fixed-rate Warp analyser. ALIB and AWTM publish under the same verified
source hash. A warm job reuses both. A timing configuration change keeps
the existing library descriptors and computes the requested timing once.

## Browser analysis publication

Use `BrowserSamplePublication` on the listing owner worker. Give `replace`
the immutable Browser sample listing, its increasing generation, optional
indexed size and timestamp observations, and the shown row offsets. It
captures native file revisions before installing the catalogue queue. The
shown rows go first once the replacement commits. `prioritize` can move new
shown rows ahead while the generation is active. Existing pause state holds.
An indexed hash can identify a cached measurement when the caller supplies
the exact native revision from byte verification or an earlier publication
verified that revision. For rows with size and modified time alone, the
catalogue worker hashes the current bytes before consulting the cache.

`snapshot(generation)` returns an immutable listing and aligned catalogue
results with filename matches available while measurements are pending.
Measurements may attach only if the captured native revision still matches.
A file that changes before its measurement attaches stays pending until a
new listing. Earlier immutable snapshots remain valid for their callers.
Use the same snapshot in both search callers. For All, `queryCombined`
joins the shared and Current Project results before ranking and limiting.
