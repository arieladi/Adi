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

Resolved bounds are output sample positions before latency compensation/preroll,
with an exclusive end. They are distinct from the job's requested bounds when a
cycle marker selects the range. Captured names are the metadata used for each
render; NI's supplied stem labels remain in the requested profile. A snapshot
can remain on an attempt that later fails or is cancelled: it proves which rows
were read, not that a file was published. No project snapshot is made at enqueue;
job values, run membership, per-job revision/names and publication retain their
four separate ownership moments.

These are the warning/reporting and journal-revision slices of EX-07/EX-01, not
their full provenance or collision contracts. `media_identities` remains null:
referenced-media BLAKE3 checks at start and immediately before publication,
rendered-output hashes, codec version, cleanup-failure reporting, run
timestamps/identity, and durable manifest storage are not captured. The journal
head identifies row history; it does not identify mutable external media bytes.
Existing no-clobber publication and volume-native case behavior
remain; Fail/Increment/Replace selection and folded-name delivery preflight need
their separate policy/implementation. The caller still owns naming and storage of
the JSON report. No new project operations, schema, Undo behavior, or sidecar
scope are introduced.
