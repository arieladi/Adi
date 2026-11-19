# Project-owned saved export jobs (EX-09)

`render::saveSavedJobs` and `loadSavedJobs` store an ordered list of existing
`render::Job` values in the project's `session_state` key `export.savedJobs`.
This is Layer 3 metadata, outside Undo/Redo, with no schema change. Neither call
enqueues, renders, opens a device, creates an output folder, snapshots project
content or changes any source IDs. An explicit later queue action remains the
caller's responsibility.

The version-1 envelope has exactly `format: "adi-project-export-jobs"`, integer
`version: 1`, `ownerProject` (native path spelling encoded as UTF-8), and `jobs`
(an ordered array of complete [strict Job objects](RENDER-JOB-JSON.md)). All
fields are required; duplicate keys at any depth, unknown fields, unsupported
versions, literal NUL bytes, malformed objects and unsupported Job values refuse
the whole list. Literal NUL rejection precedes the pinned JSON lexer, which
otherwise recognizes that byte as end-of-input.
No partial result or silent default is returned. An absent key succeeds with
`present == false`; a saved empty array has `present == true`.

Storage safety bounds are 128 jobs and 4 MiB of UTF-8 envelope bytes, including
syntax and whitespace. Each canonical Job also keeps the codec's 1 MiB bound; nesting is
limited before DOM construction. These bounds apply to this saved list, not to
the queue's retention or any product-wide maximum. Load checks SQLite type and
byte length before copying the row into C++ memory. Save checks aggregate size
while assembling the list. Allocation or SQL errors return failure; ordinary
error reporting follows the project's allocating `std::string` convention.

## Ownership and reopening

Call on the owner thread with an open, file-backed `Store` whose path is absolute.
`Store` retains the caller's spelling: a relative path could point to another
file after the working directory changes, so this API refuses it instead of
guessing the private lease's original filename. Reopen using an absolute path.
Every incoming
`Job.project.native()` must exactly equal `Store.path().native()`. Equivalent
filesystem aliases or stale paths are not silently accepted. The stored owner
path must match every embedded Job's path when decoding.

The row belongs to the containing project. On load, **only `Job.project` is
rebound to the current `Store.path()`**. Thus a Save As or copied project uses
its own project-local track/region IDs instead of opening the previous project
file. This binding is an ownership contract, not a global project identity or
cross-project import mechanism. Source IDs, bounds, destination paths, naming,
NI stem order and processing settings remain unchanged. Load does not rewrite
the stored receipt; an explicit later save writes the current owner path.
Passing an old in-memory list after Save As refuses; load the owned list first.
Transplanting an arbitrary saved-job row into an unrelated project is not a
supported import: the path receipt does not prove common project identity.

Destination paths retain their exact spelling. Relative destinations keep the
renderer/caller's existing working-directory interpretation; this slice does
not rebase them to either old or new project folders. No arbitrary cross-project
ID relocation is provided. Existence of tracks/regions, media, plug-ins and a
usable audio graph is still checked by later rendering: saving syntax-valid
job values is not a promise that the current project can render them.

## Failure and concurrency

Save requires a writable Store outside an enclosing SQL transaction. It builds
and validates the entire incoming list, takes an immediate SQLite transaction,
validates any existing saved row, then replaces the key and commits. An existing
invalid/newer row refuses replacement even by an empty list. There is no silent
repair, delete or downgrade. SQL failure rolls back the replacement; unrelated
session keys and the operation journal are unchanged. The same transaction
prevents a competing SQLite writer from changing the checked row before update.

Load works on a read-only Store and observes one SELECT result. It never writes.
Audio-callback calls refuse before SQL or error-string mutation. Otherwise each
call clears the supplied error and returns a diagnostic on refusal. Like other
Store helpers, an already-closed Store is outside the API's lifetime contract.

Tests exercise real SQLite save/close/reopen and Save As, exact job values and
owner-only rebinding, both storage bounds, duplicate/nested malformed data,
unsupported existing-row preservation, a real SQL abort trigger, read-only and
enclosing-transaction refusal, model/journal invariance and absent/empty state.
Project presets, application presets, queue history/state, migrations/recovery,
GUI and control RPC remain separate EX-09 work.
