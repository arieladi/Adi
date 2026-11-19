# User render presets (EX-09)

`render::UserPresets` persists an ordered whole list of `{name, job}` values.
The default is the existing ADI data root plus the fixed key
`ADI/render-presets.json`. This ADI-specific file is separate from ADiJ even
though `appdata::Paths::data` is suite-shared. The envelope also requires
`app: "ADI"`. An explicit constructor path supports portable callers and tests;
relative file paths resolve once at construction. Missing app-data paths refuse.
The constructor and default-path helper create no directories.

The version-1 envelope has exactly `format: "adi-user-render-presets"`, integer
`version: 1`, `app`, and `presets`. Each entry has exactly a UTF-8 `name` and a
complete [strict Job object](RENDER-JOB-JSON.md). Names never derive filenames;
empty/duplicate names and literal display punctuation remain ordered data,
without introducing rename, uniqueness, sorting or filename-sanitization rules.
Limits are 128 entries, 4 MiB raw UTF-8 file bytes, and 1,024 UTF-8 bytes per name.
Canonical embedded Jobs also pass the existing 1 MiB codec bound. Duplicate keys,
literal NUL, excess nesting, unknown fields, unsupported versions/applications,
bad enums and any invalid entry refuse the whole file. An absent file succeeds
with `present == false`; an explicitly saved empty array has `present == true`.

Unlike project-owned saved jobs, application presets do **not** rebind paths.
Project/destination paths (including relative spellings), track/region IDs and
all Job values are kept literally. They are data, not file access requests.
Loading never opens those projects, relocates IDs, applies a mask, enqueues or
renders. A separate explicit `applyJobPreset` call keeps the existing
[field-group rules](RENDER-JOB-PRESETS.md): Options/Format can apply across
projects without moving destination/source/bounds; Bounds/Output and All require
matching project spelling, and the existing clock/NI-layout checks remain.
Saving a preset is not a promise that its source project still exists or renders.

Writes are explicit **whole-list replacements**, not incremental list edits,
merges or stale-snapshot compare-and-swap. Callers coordinate their own list
editing. Save validates the candidate and existing file, creates an exclusively
owned sibling staging directory, writes/closes its fixed payload, and uses
`media::replaceFile`. The current destination is strictly validated again under
the publisher's exclusive cooperating lease; diagnostic hooks cannot override
that validation. First publication uses exclusive rename; replacement has no
copy fallback. Invalid/newer/corrupt/unreadable bytes are preserved, never moved
aside or downgraded, even for an empty replacement.

Load uses a bounded C-file read with explicit EOF/error distinction, including
Windows partial reads. Final symlink/nonregular targets refuse. Private cleanup
touches only the owned stage's fixed child and directory, never recursively or
through a preset name. Publication inherits the existing filesystem/lease
boundaries: cooperating replacements serialize; arbitrary external writers and
namespace changes are not a filesystem compare-and-swap guarantee. Failed
private cleanup can leave staging artifacts; it cannot undo publication.

`PublishResult.status == published` is success. A post-rename directory-sync
failure is published with a warning and `durabilityConfirmed == false`; callers
must not retry it as an uncommitted failure. No fallible result bookkeeping runs
after the publisher returns. Audio-callback calls refuse before I/O/error
mutation; these APIs belong on the owner thread.

Tests use owned explicit paths only, including real first/replacement writes,
reopen, mask application, malformed/newer-byte preservation, count/byte/name
boundaries, late unsupported data, real cooperating leases, sync/rename failures,
published-with-warning behavior and an actual Windows locked-tail read error.
No real user profile data is written. Project presets, migration/recovery, GUI,
control RPC, queue state and render execution remain separate work.
