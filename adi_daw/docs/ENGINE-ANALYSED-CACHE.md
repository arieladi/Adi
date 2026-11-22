# Central per-sample analysis cache

`media::AnalysisCache` holds optional rebuildable analysis under the suite's
shared cache folder, `analysed/<source BLAKE3>.adia`. It creates no files beside
samples. Each sample has one bounded file containing named analysis sections;
there is no combined database of analysis and no per-analysis sidecar file.
The application may supply a different absolute app-owned cache directory.
Missing app-data paths disable persistence instead of creating a relative folder.

The normal `buildMediaPeaks` consumer reads the existing legacy pool BLOB first,
then the central APKS section when the pool supplies an exact source identity
and dimensions, then builds from verified source audio. New results enter the
central file, never a project pool BLOB or operation/history row. Read-only
project workers can draw valid cached content even when its source is missing,
just as they can with the legacy pool cache. These bins describe the pool's
known content hash, not proof that an unrelated currently changed disk file
still matches it. Source rebuilding still verifies hashes before and after.
`maxBinsPerChannel` applies to newly built data; an existing valid cached
resolution may be reused, as with the legacy pool cache.

An overload takes an explicit `AnalysisCache*`; nullptr disables central
persistence. Tests and worker services can isolate their cache this way. Cache
corruption, unavailable disk, permission or write failures do not discard peaks
successfully computed from source. A stopped write remains cancelled. Cache
files have no authority over project data. Unknown legacy project BLOBs remain
opaque and are never overwritten, including when central data replaces their
visualization. No project schema or opcode changes.

All codec, filesystem and removal calls refuse the audio callback before
allocation, free, I/O, locking or mutation of caller diagnostics. Call them on
an owner worker. Cache operations serialize within a process; atomic same-volume
replacement prevents other applications seeing a partial file. Concurrent
applications may replace each other's optional records after reading an older
snapshot, so the last complete writer wins and a section may need rebuilding.
This is not a cross-process transactional store for user data.

Writers reserve an exclusive owner-only staging directory under `analysed`,
write/flush/close a complete new envelope and atomically rename/replace the
closed file through the native filesystem adapter. The replacement API has no
copy fallback exposing partial bytes and is reserved for optional app-owned
cache data. Failed/cancelled calls remove only their private stage. Ordinary
retirement removes staging directories. Crash leftovers are not consumed as
analysis records; this component does not add an orphan reaper. A caller may
remove an individual validated hash file for cleanup.

`trimToBytes(maximumBytes, error, stop)` is an explicit owner-worker maintenance
endpoint. It inventories only real, regular, direct-child `<hash>.adia` files,
validates each complete envelope and payload digest, and requires its embedded
source hash to match its filename before considering deletion. Unknown names,
damaged or unsupported envelopes, symlinks, directories and private stages are
left untouched. The root must be an absolute real directory. A missing root is
an empty cache, without creating it. The budget counts eligible logical file
bytes, not physical disk space or ignored entries. Zero removes all eligible
entries; it does not mean unlimited.

Eviction orders eligible entries by oldest modification stamp, then filename.
A successful read refreshes that stamp best-effort; writes already refresh it.
Failure to update this optional metadata does not invalidate readable analysis,
so recency is approximate on read-only or coarse-timestamp filesystems. Inventory
finishes before deletion starts. An entry that cannot be inspected/read or
deleted is counted in `skippedEntries`; trimming continues past that entry and
retains the first error with its UTF-8 path. The result is `Io` whenever an entry
was skipped, even if other deletions reached the known budget. An unreadable
entry is never charged or deleted, and prevents claiming `budgetMet`: its
eligibility/size has not been validated. `inventoryComplete` means directory
enumeration and validation of all accessible candidates finished, not that every
entry was readable. Invalid/foreign entries instead count as `ignoredEntries`.

Cancellation or a root/inventory failure can leave a partially completed
eviction: the result records bytes before/after, removed files and both skip
counts. Only successful removals reduce bytes. Cancellation stops immediately,
including when returned from a file read, without blanking the caller's existing
diagnostic. Pre-cancellation and callback refusal do not mutate it either. An
observed type/size/stamp change or disappearance of an already inventoried
candidate still aborts the run; it is not treated as permission to skip a race.

The same process mutex covers reads, updates and trim. Reads close their file
before releasing the mutex, and returned analysis owns all its bytes even if a
later trim removes the disk copy. A later read may instead report Missing and
rebuild. Native refusal to delete an open file is reported rather than counted
as reclaimed bytes. Size/type/stamp rechecks detect ordinary intervening changes,
but are not a cross-process compare-and-delete; applications sharing the folder
must coordinate maintenance separately. This endpoint does not wire the global
`cache.maxSizeMb` / `cache.minFreeSpaceMb` settings, divide a combined decoded and
analysed budget, schedule automatic cleanup, or guarantee minimum free space.
Those settings integration and cross-process ownership steps remain open.

`adi_analysis_cache_tests` exercises exact/zero budgets, read promotion and ties,
ignored/unowned files, cancellation and callback refusal, retained returned data,
concurrent reads/updates/trim, Windows non-delete-sharing reads, and actual
persistent library defaults surviving eviction. `adi_analysis_cache_trim_tests`
adds owned Windows read/delete sharing refusals, continuation to healthy siblings,
unreadable/retained byte accounting, cancellation inside the third file read and
an actual timestamp change before inventory completes. Symlink checks run when
the platform permits creation; they report a skip otherwise.

Persisted Warp maps and clip settings are the user's data. They belong in
`data/library.sqlite` or another explicit persistent library store, outside
`cache/analysed`, and must never be passed as optional analysis sections. This
change does not implement saved sample settings or claim that a cached onset
map is an authored Warp map. Those are the next native library item. The only
new consumer here is waveform peaks; other analysis algorithms can add bounded
sections with explicit version/configuration identifiers later.

See [the envelope format](format/ANALYSIS-CACHE.md) for byte bounds and validation.
Actual compiler, mutation, sanitizer and full-gate evidence belongs in the engine
log after the gate; a syntax preflight alone is not a readiness claim.
