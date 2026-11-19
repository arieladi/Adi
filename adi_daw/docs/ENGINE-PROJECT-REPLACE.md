# Confirmed project replacement (N45)

`Store::saveCopy` and `Store::saveAs` take an optional `StoreFileCollision`.
`Fail` is the default and preserves the existing no-clobber API. The caller may
pass `ReplaceConfirmed` only after obtaining the user's replacement consent.
The window supplies that interaction; this engine change does not add a dialog.

SQLite backup captures committed source WAL data into a private directory in the
destination folder. The existing media-history relocation runs on that backup,
including inverse payloads and inactive branches. The backup is checkpointed,
changed to DELETE journaling and closed before `media::replaceFile` runs. The
source, history and active Store stay intact on failure. Save As carries the
native file lease with its database, keeps the writer identity, and uses the
existing gesture/applied-source replacement fences.

The authoritative primitive syncs the closed stage first (`fsync`, Apple's
`F_FULLFSYNC`, Windows `FlushFileBuffers`), then holds an exclusive destination
lease across native rename-over. It never copies over the destination. An absent
endpoint uses native no-replace rename and reselects if another file appeared.
A live Store, an active-file alias, nonregular destination or existing SQLite
`-wal`, `-shm` or `-journal` refuses publication. Recovery material is neither
checkpointed nor deleted on someone else's behalf. A successful rename followed
by directory-sync failure is published with a warning, never an unchanged-file
failure. Save As compares the reopened file identity to the published identity
before replacing its active database.

Windows retries only access, sharing and lock errors (5, 32, 33), at most seven
times after the initial attempt: 10, 20, 40, 80, 160, 200, 200 milliseconds.
The owner thread waits; neither publication nor native lease acquisition may run
on the audio thread. POSIX failures are returned immediately. A directory flush
unsupported by Windows or a filesystem is an explicit durability warning.

Every file-backed Store holds a shared native lease before SQLite opens and
until it closes. Shared leases allow the existing SQLite reader/writer behavior;
only publication takes an exclusive lease. POSIX leases use `flock` on separate
private per-user `/tmp/adi-project-leases-UID/DEV-INODE.lock` inodes, with an
owner-only directory and no-follow opens. Lease files are never unlinked: a
pending opener may already hold their descriptors. Identity queries use `stat`,
not an extra open/close of the database: closing such a descriptor could release
all of this process's SQLite record locks. New-file exclusive creation closes
its initial descriptor before SQLite opens. Windows uses a shared or exclusive
byte lock at 2^60 (outside SQLite's maximum file length) and disallows deletion
while a Store holds its open handle. Replacement handles allow their own rename.

This protects cooperating Stores of the same user on the same machine, including
hardlink aliases and other processes. Raw SQLite connections, different users,
external tools that ignore the protocol, remote machines and changes to directory
symlinks by external processes are outside this local lease protocol. It is not
PD-05's one-writer/session ownership scheme. These tests establish process-exit
atomicity; they do not simulate power failure or certify remote filesystem
persistence. This is N45 plus the authoritative PD-04 primitive, not all PD-04:
session markers, dead-session stage recovery and other file writers remain work.

`adi_project_replace_tests` exercises actual replacement, consent defaults,
read-only and writer exclusion, source aliases, recovery material, history and
writer identity, in-memory/temporary projects, injected sync/rename failures,
post-publication warnings, bounded retries, and audio-thread refusal. POSIX-only
process tests check foreign lease exclusion, preservation of SQLite's record
locks, and exit immediately after rename. The existing project-files, collection,
gesture and applied-batch suites remain part of the full gate. Native Windows
and Apple execution belong to their seats; Linux cannot claim those results.
