# Closed-project salvage (PD-13, first CLI slice)

`adi_tool salvage <in.adi> <out.adi>` reads a closed project and recovers a new
copy using SQLite's recovery extension. It never repairs the source, overwrites
the destination, migrates the schema, rewrites history links, or copies media.
The output is a salvage artifact; even a clean checker result does not establish
that every original row survived, or that recovered content is unaltered.

This slice implements the command part of PLAN PD-13 / ADR-0033. It does not
implement the separate unclean-open lifecycle, quick-check scheduling or window
offer. No project schema is duplicated: upstream recovers the input schema and
data, and ADI uses its existing read-only `Store` and `checkProject` afterward.

## Admission and result

- A regular, closed input with a complete SQLite header, ADI application ID and
  supported schema major is required. Its schema catalogue must remain readable.
  Older/newer minor versions retain their original `user_version`; the existing
  read-only Store compatibility path does not migrate the saved file.
- Any `-wal`, `-shm` or `-journal` sidecar is a refusal, including an empty one.
  A WAL-format header is also refused even when its sidecars are absent, before
  SQLite can create them on a nominally read-only connection. Normal ADI close
  already converts a project to rollback journal format.
  This command does not discard or checkpoint sidecars. Use a separately agreed
  recovery procedure for WAL-bearing or damaged-schema-catalogue input.
- The existing exclusive project lease excludes cooperating live Stores.
  The raw SQLite input connection is read-only, with trusted schema disabled.
  A source BLAKE3 comparison and repeat sidecar checks detect ordinary external
  changes before publication. These observations are not a filesystem snapshot
  or an atomic defence against noncooperating rename/write races.
- SQLite writes only a uniquely reserved private directory beside the output.
  Recovery's overwrite-capable destination API receives only that private path.
  The recovered header must match the source's application ID and user version.
- `checkProject(..., false)` checks the recovered file, including SQLite
  integrity and FK checks. A SQLite integrity finding, an unreadable checker
  query/schema, or a Store admission error refuses publication. Semantic
  findings (for example missing undo parents or malformed note blobs) remain
  visible; the readable copy can still be useful and is published with findings.
- SQLite's freelist-corrupt option is explicitly **off**, rowid recovery is on.
  A previously unused `lost_and_found` name (or numbered suffix) is selected
  from the readable source schema. Only that recovery table's rows are counted;
  an existing extension with the unsuffixed name is not misreported as new loss.
  Lost-and-found counts orphan records, not all missing or changed records.
- After all SQLite handles close, existing `publishFile(..., AtomicOnly)`
  publishes the single file without replacement or a visible copy fallback.
  Output `-wal`, `-shm` and `-journal` sidecars are refused before staging and
  again immediately before publication, even when the output file is absent.
  These checks do not atomically reserve all possible sidecar names against a
  noncooperating external writer after the final check.
  An existing or late-created destination survives. Post-publication durability
  warnings retain the successful publication status.

Exit codes are **0** for a published copy with no checker errors/warnings and no
lost-and-found rows, **1** for a published copy with those findings, and **2** for
refusal/failure with nothing published (also command usage errors). Informational
checker findings alone do not make exit 1. The CLI always states the recovery
uncertainty and media boundary, even on exit 0.

The media check is deliberately disabled in the private staging folder: its
temporary location is not the project's media root. Relative and absolute media
paths are retained verbatim. Moving the recovered `.adi` to a different folder
may require the ordinary verified relink workflow. No missing media or history
indirection is guessed or silently fixed.

## One pinned SQLite runtime

The preceding pin change `8a8f9a00` selects SQLiteCpp 3.4.0
(`df0a28ca9733f687ab7ce77f070c0df84368a45e`) with SQLite 3.53.4.
PLAN's older R5 reference to 3.49.2 predates that upgrade; this slice uses the
matching 3.53.4 recovery extension, not the older files.

- Official Git mirror release: `version-3.53.4`,
  `b09c88c14082339b66c7b7158d609a771e64ca69`.
- Fossil release/source ID:
  `2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc`.
- Sparse checkout: `third_party/sqlite-recover`; only
  `ext/recover/sqlite3recover.c`, `.h`, `dbdata.c` and release metadata
  `manifest.uuid`. The three upstream files disclaim copyright (public domain).
- CMake validates the three source hashes after CRLF-to-LF normalization, and
  the amalgamation header source ID. The command also validates the linked
  runtime source ID/version. Another SQLite library is never linked.
- A hash-guarded build-local adaptation disables DEFENSIVE only on the
  recovery API's dedicated private output connection. Upstream needs
  `writable_schema` to reconstruct `sqlite_sequence` before AUTOINCREMENT
  tables; with the defensive default unchanged there, its last-created-table
  lookup maps the `ops` root to the implicit sequence table and drops intact
  history rows. A real healthy ADI project exposed this (the two-column
  non-AUTOINCREMENT smoke fixture did not). CMake requires exactly one patch
  site, prints the adapted file hash, and leaves all three pinned upstream
  files untouched. No global auto-extension or global defensive toggle is used.
- The shared `sqlite3` target enables `SQLITE_ENABLE_DBPAGE_VTAB` and
  `SQLITE_DEFAULT_DEFENSIVE`: ordinary connections reject raw-page SQL writes.
  The two recovery C sources are a separate static target linked only into
  `adi_tool`/its salvage tests; third-party warnings follow the existing policy.
  ADI's wrapper and tests retain the strict project warning wall.

Primary references: [SQLite recovery integration and limits](https://sqlite.org/recovery.html),
[pinned API](https://github.com/sqlite/sqlite/blob/b09c88c14082339b66c7b7158d609a771e64ca69/ext/recover/sqlite3recover.h),
[pinned implementation](https://github.com/sqlite/sqlite/blob/b09c88c14082339b66c7b7158d609a771e64ca69/ext/recover/sqlite3recover.c),
[WAL read-only rules](https://sqlite.org/wal.html),
[DEFENSIVE](https://sqlite.org/c3ref/c_dbconfig_defensive.html),
[SQLite b-tree file format](https://sqlite.org/fileformat2.html#b_tree_pages).

## Regression boundary

`adi_salvage_tests` creates real ADI projects and journal transactions, then
corrupts owned copies. A small test-only SQLite table-btree reader identifies
which actual `ops` leaf owns which IDs before damaging it; the oracle compares
every intact row's ID, column storage types and values (including CBOR bytes,
labels, timestamps, actor attribution, transaction IDs and parent links).
It also exercises healthy and diagnosed CLI exits, unknown extension data,
orphan-page recovery and name collisions, default defensive refusal, live and
sidecar admission, invalid headers, missing required schema, publication failure
and a late target collision. Input bytes and private-stage cleanup are checked.

The test-only raw schema corruption explicitly disables DEFENSIVE on its owned
fixture connection. Production only disables it on the private recovery output
connection described above; ordinary and read-only input connections retain the
default. Source stability,
schema/version compatibility and physical recovery remain best-effort; this is
not a guarantee against arbitrary corruption or a substitute for backups.
