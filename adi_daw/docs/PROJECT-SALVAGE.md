# Closed-project salvage (PD-13, first CLI slice)

`adi_tool salvage <in.adi> <out.adi>` opens a closed project read-only and
publishes a separate copy at an unused destination. SQLite recovers the
input schema and data. ADI verifies the result through its existing read-only
`Store` and `checkProject` under PLAN PD-13 and ADR-0033. A clean checker
result describes the recovered copy. Some original rows may still be missing
or altered.

## Admission and result

- A regular, closed input with a complete SQLite header, ADI application ID and
  supported schema major is required. Its schema catalogue must remain readable.
  Older/newer minor versions retain their original `user_version`; the existing
  read-only Store compatibility path does not migrate the saved file.
- Any `-wal`, `-shm` or `-journal` sidecar is a refusal, including an empty one.
  A WAL-format header is also refused even when its sidecars are absent, before
  SQLite can create them on a nominally read-only connection. Normal ADI close
  already converts a project to rollback journal format.
  WAL-bearing and unreadable-schema inputs are outside this command's
  admission rules.
- The existing exclusive project lease excludes cooperating live Stores.
  The raw SQLite input connection is read-only, with trusted schema disabled.
  A source BLAKE3 comparison and repeat sidecar checks detect ordinary external
  changes before publication. A writer outside ADI can still race these checks.
- SQLite writes only a uniquely reserved private directory beside the output.
  Recovery's overwrite-capable destination API receives only that private path.
  The recovered header must match the source's application ID and user version.
- `checkProject(..., false)` checks the recovered file, including SQLite
  integrity and FK checks. A SQLite integrity finding, an unreadable checker
  query/schema, or a Store admission error refuses publication. Semantic
  findings (for example missing undo parents or malformed note blobs) remain
  visible in the result and accompany publication of the readable copy.
- SQLite's freelist-corrupt option is explicitly **off**, rowid recovery is on.
  A previously unused `lost_and_found` name (or numbered suffix) is selected
  from the readable source schema. Only that recovery table's rows are counted;
  a pre-existing extension table keeps its own contents and name.
  The lost-and-found count covers orphan records.
- After all SQLite handles close, existing `publishFile(..., AtomicOnly)`
  publishes the single file without replacement or a visible copy fallback.
  Output `-wal`, `-shm` and `-journal` sidecars are refused before staging and
  again immediately before publication, even when the output file is absent.
  A writer outside ADI can still create a sidecar after the last check.
  An existing or late-created destination survives. Post-publication durability
  warnings retain the successful publication status.

Exit codes are **0** for a published copy with no checker errors/warnings and no
lost-and-found rows, **1** for a published copy with those findings, and **2** for
refusal or failure with nothing published, including command usage errors.
The CLI states the recovery uncertainty and media boundary on both
publication outcomes.

The checker uses the private staging folder for structural and semantic
verification. Relative and absolute media references are retained verbatim.
Moving the recovered `.adi` to a different folder may require the ordinary
verified relink workflow.

## One pinned SQLite runtime

The preceding pin change `8a8f9a00` selects SQLiteCpp 3.4.0
(`df0a28ca9733f687ab7ce77f070c0df84368a45e`) with SQLite 3.53.4.
PLAN's older R5 reference to 3.49.2 predates that upgrade; this slice uses the
matching 3.53.4 recovery extension.

- Official Git mirror release `version-3.53.4` at
  `b09c88c14082339b66c7b7158d609a771e64ca69`.
- Fossil release/source ID
  `2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc`.
- Sparse checkout `third_party/sqlite-recover` contains
  `ext/recover/sqlite3recover.c`, `.h`, `dbdata.c` and release metadata
  `manifest.uuid`. The three upstream files disclaim copyright (public domain).
- CMake validates the three source hashes after CRLF-to-LF normalization, and
  the amalgamation header source ID. The command also validates the linked
  runtime source ID/version. Recovery and ordinary ADI use that single runtime.
- A hash-guarded build-local adaptation disables DEFENSIVE only on the
  recovery API's dedicated private output connection. Upstream needs
  `writable_schema` to reconstruct `sqlite_sequence` before AUTOINCREMENT
  tables. With the defensive default enabled on that connection, the
  last-created-table lookup maps the `ops` root to the implicit sequence
  table and drops intact history rows. A healthy ADI project reproduced the
  failure. CMake requires exactly one patch site, prints the adapted file
  hash, and leaves all three pinned upstream files intact.
- The shared `sqlite3` target enables `SQLITE_ENABLE_DBPAGE_VTAB` and
  `SQLITE_DEFAULT_DEFENSIVE`. Ordinary connections reject raw-page SQL writes.
  The recovery sources form a separate static target for `adi_tool` and its
  salvage tests. Third-party warnings follow the existing policy.
  ADI's wrapper and tests retain the strict project warning wall.

SQLite documents the recovery limits, the file format and the connection rules.
[Recovery integration](https://sqlite.org/recovery.html),
[pinned API](https://github.com/sqlite/sqlite/blob/b09c88c14082339b66c7b7158d609a771e64ca69/ext/recover/sqlite3recover.h),
[pinned implementation](https://github.com/sqlite/sqlite/blob/b09c88c14082339b66c7b7158d609a771e64ca69/ext/recover/sqlite3recover.c),
[WAL read-only rules](https://sqlite.org/wal.html),
[DEFENSIVE](https://sqlite.org/c3ref/c_dbconfig_defensive.html),
[SQLite b-tree file format](https://sqlite.org/fileformat2.html#b_tree_pages).

## Regression boundary

`adi_salvage_tests` creates real ADI projects and journal transactions, then
corrupts owned copies. A small test-only SQLite table-btree reader identifies
which actual `ops` leaf owns which IDs before damaging it. The test compares
every intact row's ID, column storage types and values (including CBOR bytes,
labels, timestamps, actor attribution, transaction IDs and parent links).
It also exercises healthy and diagnosed CLI exits, unknown extension data,
orphan-page recovery and name collisions, default defensive refusal, live and
sidecar admission, invalid headers, missing required schema, publication failure
and a late target collision. Input bytes and private-stage cleanup are checked.

The raw schema-corruption fixture disables DEFENSIVE on its owned test
connection. Production uses DEFENSIVE on ordinary and read-only input
connections. Physical recovery remains best-effort.
