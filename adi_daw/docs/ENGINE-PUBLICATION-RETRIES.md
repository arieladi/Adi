# Closed-stage publication retries

`media::publishFile` keeps its existing collision and fallback policy. On
Windows, the stage sync and exclusive rename share at most seven retries:
10, 20, 40, 80, 160, 200, 200 milliseconds (710 milliseconds of requested
waiting in total). This bounds the waits, not the duration of filesystem calls.
Only native system errors `ERROR_ACCESS_DENIED` (5),
`ERROR_SHARING_VIOLATION` (32), and `ERROR_LOCK_VIOLATION` (33) are retried.
Cross-volume, full-disk, generic permission errors and `file_exists` are not.
POSIX publication does not wait. The audio-thread guard precedes all work.

The source must be a private, closed ordinary file. Its lease identity and
ordinary-file type are checked before each sync or rename attempt, and before
entering an explicitly permitted copy fallback. A failed sync may be retried;
once sync succeeds, rename retries do not flush again. Replacing the staged
path or changing it into a directory during a retry refuses publication.
The source lease remains held across the waits.

`AtomicOnly` publishes only through the exclusive native rename. A destination
claimed by another writer returns `exists` and preserves both that destination
and the source. Exhaustion returns `failed`, retains the source, and does not
create a destination. The owning job may subsequently clean its private stage.
`VerifiedCopyFallback` retains its pre-existing verified private-copy and
exclusive-link behavior after an unsuccessful rename; this change does not
enable fallback for authoritative media callers.

Identity checks are observations around pathname operations, not an atomic
filesystem compare-and-rename. They cannot exclude an uncooperative pathname
change after the last check, or an in-place write to the same file identity.
The Windows identity check uses the full file ID where the volume supplies it.
The retry path does not change the destination collision policy.

`test_project_replace.cpp` covers each retryable code, the shared retry limit,
one successful sync, immediate non-transient refusal, a competing destination,
stage identity/type changes, and the audio-thread guard. Windows cases also
hold actual read-only-sharing and read/write-sharing handles: releasing the
holder from the retry wait allows publication; retaining it exhausts the budget
without a target, after which the owner cleans the stage. Test hooks replace
waits so these checks do not depend on scheduler timing. Null hooks use the
real sync, native rename and wait operations.
