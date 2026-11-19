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
analysis records; this component does not add an orphan reaper or an automatic
LRU policy. A caller may remove an individual validated hash file for cleanup.

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
