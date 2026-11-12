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
