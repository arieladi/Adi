# Render publication and run reports

`render::Result::publication` is present only after successful publication.
`durabilityConfirmed` says whether the publisher confirmed directory durability;
`usedFallback` retains its fallback-path decision. A nonempty warning is compatible
with `JobState::Completed`. It must not cause automatic retry or removal of an
already published file. An absent publication report means this attempt did not
publish successfully, even when `output` names its intended destination.

WAV and NI Stems use the same commit helper. It allocates warning storage before
publication and performs only nonthrowing state/move operations afterward. If the
publisher cannot confirm durability but returns no warning text, the result uses
an explicit generic warning. Per-call `Control::publicationHooks` are diagnostic
injection only; NI's private intermediate WAVs use normal publisher behavior.
Authoritative WAV/NI publication uses `PublishPolicy::AtomicOnly`: a rename
refusal fails without publishing a copy. Successful outputs therefore report
`usedFallback=false`; retaining the publisher flag does not opt into fallback.

`Result::stageCleanup` records the directory and native error when private
staging cannot be completely removed. WAV and NI check cleanup on every exit
after a stage is created. The run manifest carries it in `stage_cleanup`, with
`null` when there is no recorded failure. It is separate from the delivery
state, publication warning and primary error. A completed file remains usable
even if a private staging directory needs attention.

`Job::collision` supports `Fail` (the default) and explicit `Increment` for one
WAV or NI delivery. The original complete filename is tried first. If occupied,
Increment selects `name (2).wav`, `name (3).wav`, and so on, or
`name (2).stem.mp4` for NI. Files, directories and dangling symlinks all occupy
a name. After the original, the search tries at most the observed directory-entry
count plus one numbered candidate, and fails if concurrent changes prevent
finding a free name within that bound. This places no arbitrary small limit on
existing numbered exports. Filesystem enumeration/lookup is synchronous; a stop
requested during preflight is observed at the next ordinary render/encode
cancellation checkpoint, not between each directory entry.
The 200-byte UTF-8 limit includes the reserved-name prefix, increment text and
the complete format suffix; names are never truncated to fit.

Selection occurs once at delivery preflight. The selected path is then fixed:
an arrival at that path before atomic publication fails the attempt and preserves
the arriving file, rather than silently incrementing again. NI's internal WAVs
always use Fail; delivery numbering does not alter the private Main/stem names.
Case-only collisions follow the actual destination directory's native lookup
semantics, including Unicode names. A case-sensitive directory may retain two
case-only names; this slice does not implement EX-07's broader case-folded
delivery-set policy. There is no multi-format/group renumbering, save-dialog
confirmation or batch policy selection. Replace remains unavailable pending a
separate explicit opt-in and destination-identity contract inside `replaceFile`.

`Queue::lastRun()` describes the pending membership frozen at the beginning of
the latest `run()`. Callback-added jobs belong to a later run. Removed pending
members remain listed as **unstarted**. A stop request leaves unattempted members
unstarted and Pending in the queue; the attempted cancelled member is Cancelled.
Retry starts a new report, and completed members are not silently rerun.

The report stores job values at run entry for unstarted members and at job start
for attempted members, including any permitted pending edit. `job_capture` marks
the basis. Reentrant callbacks can inspect the current report, which may contain
running members. The reference is valid only until the next run or destruction;
copy it to retain an earlier run independently of subsequent queue edits.

Report copies are best effort. Allocation/copy failures set `captureComplete`
false and leave unavailable fields or members absent. They do not interrupt
rendering, cancel later members, or change a successful queue result. Callers must
check this flag and `plannedCount`; an empty incomplete report is not a complete
empty run. Queue lifecycle allocations that already existed are not made
infallible by this reporting API.

`serializeRunManifest(report)` returns version-1 JSON. The caller chooses whether,
where, and how to persist it; rendering writes no manifest sidecar. Serialization
or persistence errors must be reported separately from the render result. The
serializer may throw, including for allocation failure or invalid UTF-8, and
does not change the report, queue, or output files.

Version 1 records:

- Stable job IDs, ordered run membership, completed/failed/cancelled/unstarted
  states, removal-before-start, and capture completeness.
- Requested project/output directory/template, source track/tap, sample range
  and optional cycle-region ID; format, precision, rate, channels, block size,
  compensation, optional external tempo, dither/seed, normalization and NI
  stem metadata. These are explicitly **requested** values. For example, NI
  uses Float32 intermediate WAVs, while its requested profile may still contain
  the ordinary job's WAV encoding. Float32 ignores requested dither.
- The requested `job.collision` (`fail` or `increment`); `result.output` is the
  actual selected path, including any preflight increment. A failed preflight
  can retain its rejected candidate path without having published it.
- Actual output path, written-frame count, error, publication flags/warning,
  and available normalization measurements. The stable job ID is also the
  runner's dither-seed mixing ordinal.
- `result.resolved_snapshot` and the member's `journal_head`: the current journal
  branch head, resolved source ID/tap, sample rate, output-range bounds, region ID
  and name-expansion values. The read occurs inside the same SQLite transaction
  as the graph, before device loading can service another writer. Zero is the
  captured SQL NULL head before any operation (also possible after Undo); null
  means no captured revision is available in this report, including an incomplete
  result copy, never a fabricated revision zero. The
  journal head is not the largest sequence in the operation log. Invalid/missing
  current-branch metadata fails capture instead of being reported as zero.
- NI Main uses `resolved_snapshot`; `ni_stem_snapshots` records each supplied
  source in stream order. NI's existing private SQLite backup pins the original
  database revision before individual renders. Main and actual stems must have
  identical journal heads, ranges, rates and region IDs. Its rewritten private
  media paths do not alter the original head. Unused silent slots have no project
  source and therefore no fabricated snapshot entry.
  A failed NI attempt can retain Main and only the stems completed before the
  failure; the list alone is not a completed-delivery claim.
- For WAV, `media_identities` records the external audio-clip files actually
  selected by the loaded ClipPlayback generation. Its immutable off-callback
  source view supplies the path and media/clip IDs from that generation's read
  snapshot, including Session's frozen-source substitutions. It does not rerun
  a separate resolver against mutable rows. Unopened muted clips are excluded;
  other constructed clip sources are checked even if outside the selected range.
- Each distinct resolved file is hashed with BLAKE3 after Session preparation,
  before offline processing, and again immediately before publication. A native
  `(volume,file)` identity key joins hardlinks, symlinks and volume-native case
  aliases. Each distinct selected path incurs one native identity read per pass;
  keyed lookup replaces pairwise filesystem-equivalence checks. All referring
  clip/media IDs, selected paths and stored hash values are retained. Every
  selected path must still name its captured native identity when rechecked,
  even when a replacement has identical bytes. `native_identity` records that
  run-local key, not a persistent or portable content identifier. The shared
  publisher identity reader uses Windows `GetFileInformationByHandle` or POSIX
  `stat`, follows symlinks, and refuses callbacks. An unavailable native identity
  fails with its filesystem error; there is no path-only fallback claiming the
  same alias/rebinding guarantees. Cancellation is checked
  between files and between the hasher's fixed 64 KiB reads.
- A stored 64-digit hexadecimal BLAKE3 is compared with the measured start hash
  (uppercase spelling is normalized for comparison). A mismatch fails before
  rendering. Legacy non-digest labels are not treated as proof: `matches_stored`
  is null and actual measured bytes supply the baseline. The second digest must
  match that baseline; a same-size rewrite is a failure too. A failed job can
  retain partial identities or its mismatching second digest without publishing.
- `result.media_verification` states `scope: external_audio_clips`,
  `start_complete` and `publication_verified`. The latter means the recheck
  succeeded before the publication attempt, not that publication itself succeeded.
  An empty identities array with both flags true means no external clip inputs;
  null means this report has no such verification. Media reporting does not write
  hashes back to the project or change the input files.

Resolved bounds are output sample positions before latency compensation/preroll,
with an exclusive end. They are distinct from the job's requested bounds when a
cycle marker selects the range. Captured names are the metadata used for each
render; NI's supplied stem labels remain in the requested profile. A snapshot
can remain on an attempt that later fails or is cancelled: it proves which rows
were read, not that a file was published. No project snapshot is made at enqueue;
job values, run membership, per-job revision/names and publication retain their
four separate ownership moments.

These are the warning/reporting, journal-revision and WAV external-clip input
slices of EX-07/EX-01, not their full provenance or collision contracts. Hashing
is an endpoint comparison, not a locked byte snapshot: it cannot rule out a
write-and-restore between checks, a preparation-time source race, or a change
after the final check. Decode/warp caches remain the existing engine's concern;
the new identities describe their original external inputs, not cache contents.
Native keys inherit the publisher's filesystem-identity contract: they are not
leases and may be reused after deletion. Network providers returning incomplete
or unstable identities are not covered by the local filesystem regression tests.
Plugin-owned samples/impulses and other loader-private external files are outside
the source view. Embedded clip audio already requires extraction in ClipPlayback.

NI's internal WAV renders receive the same per-render guard, but the final NI
container does not yet aggregate one shared media baseline or recheck it after
all AAC work. Its `media_identities` and `media_verification` remain null rather
than claiming final-container verification. Each internal render currently hashes
its own unique files; a shared NI-wide baseline/recheck is separate work.
Rendered-output hashes, codec version, cleanup-failure reporting, run
timestamps/identity, and durable manifest storage are also not captured. The
journal head identifies row history independently of measured media bytes.
Existing no-clobber publication and volume-native case behavior
remain; Replace and folded-name/multi-format delivery preflight need
their separate policy/implementation. The caller still owns naming and storage of
the JSON report. No new project operations, schema, Undo behavior, or sidecar
scope are introduced.
