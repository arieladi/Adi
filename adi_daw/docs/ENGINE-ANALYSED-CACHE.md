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
must coordinate maintenance separately.

The application analyses use the existing `cache.maxSizeMb` and
`cache.minFreeSpaceMb` preferences for automatic maintenance. The app settings
store publishes validated preferences on startup and after a successful save.
A saved absolute `cache.folder` selects the common root before the process first
opens its default `decoded` and `analysed` subfolders. An empty or invalid setting
selects the usual application data cache. A later folder change takes effect
on the next launch so running decoders and their active leases keep valid paths.
A successful cache access applies preference changes while the cache is open.
With the preference already loaded at startup, the first new publication
establishes the byte inventory, so ordinary warm reads can serve cached rows
immediately. A zero maximum clears valid records even on a read. Publication
checks byte growth and free space. The setting change itself performs no disk
scan. Explicitly constructed caches opt in through `setAutomaticLimits`.

Under a known byte limit, complete publications count conservatively until an
inventory is needed. Automatic trimming under a budget of at least 16 MiB leaves
5% headroom, capped at 8 MiB, after an overflow. The validated inventory and
eviction order are the same as for manual trimming. A low-space trim queries
available disk space again after each successful deletion. Maintenance is best
effort because other writers share the disk and some entries may be unavailable.
The default decoded admission and ADIA publication share the byte budget and
disk headroom. Under pressure they inventory both stores and evict the least
recently used eligible file in either folder. Admission accounts for the planned
decoded output and checks physical headroom after reclamation. App-local PCM
leases protect files from eviction. ADIA work releases its I/O lock before
coordinated maintenance acquires the decoder lock followed by the ADIA lock.

`cleanupMediaCache` accepts the current project's content hashes. It retains
those hashes and active decoded leases in both stores, and returns the total
owned bytes, removals and retained entries. The owner-worker call leaves
unrelated files and authored `library.sqlite` data intact. On Windows a native
sharing refusal keeps that file and is accounted as unavailable.

`adi_analysis_cache_tests` exercises exact/zero budgets, read promotion and ties,
ignored/unowned files, cancellation and callback refusal, retained returned data,
concurrent reads/updates/trim, Windows non-delete-sharing reads, actual
persistent library defaults surviving eviction, and automatic trimming on
publication, disk pressure and preference changes. `adi_settings_tests`
exercises startup, committed preference changes and failed saves using a
temporary application cache. `adi_analysis_cache_trim_tests`
adds owned Windows read/delete sharing refusals, continuation to healthy siblings,
unreadable/retained byte accounting, cancellation inside the third file read and
an actual timestamp change before inventory completes. Symlink checks run when
the platform permits creation; they report a skip otherwise.
`adi_decode_tests` drives shared automatic eviction in both directions with real
PCM plus ADIA, and verifies that Cleanup retains current and playing files and
their analyses until the last lease and current-project protection are released.

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
