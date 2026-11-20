# Rollback of published collected/extracted media

PD-02 / PD-04: an aborted SQL transaction must not delete another writer's file
merely because it now occupies a pathname this operation previously published.

`Store::collectAllAndSave` and legacy `media::extractMedia` retain each successful
`PublishResult.identity` beside its pathname. Path bookkeeping is allocated
before publication; recording the resulting identity does not allocate. They
still roll back the whole database transaction on failure. They remove public
media only through `removePublishedFile`, and include retained/changed paths in
the existing result's `warning`. The collection document includes that warning
in its failure text; the extraction CLI already displays failure warnings.

## Windows contract

The remover opens the pathname with `DELETE | FILE_READ_ATTRIBUTES`, share mode
zero and `FILE_FLAG_OPEN_REPARSE_POINT`. It rejects directories and reparse
points, compares volume/file identity obtained from that handle, then uses
`SetFileInformationByHandle(FileDispositionInfo)` and closes the same handle.
It never calls pathname deletion after a separate identity check. Share mode
zero conservatively refuses busy files and prevents another open for rename,
delete or write while the handle is held. A different object, inaccessible file
or unsupported filesystem operation is retained and reported. An already
absent pathname needs no cleanup. An alternate hard link is left intact.

This is object-identity protection, not a check that an external writer has not
modified the same object's bytes before rollback. It does not chase a moved
publication or remove files by searching for their identity.

The native API behavior is specified by Microsoft's
[CreateFile documentation](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
and [SetFileInformationByHandle documentation](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle).

## Other platforms and remaining work

POSIX pathname `unlink` does not provide an atomic identity comparison and
removal. This helper returns `operation_not_supported` there and the two callers
retain/report public rollback leftovers. They do not add a check-then-unlink
fallback. Native macOS/POSIX execution is not claimed by the Windows validation.
Reordering all source validation before any publication, stronger recovery
ownership and an unused-files collector remain separate work. Crash recovery,
other callers' rollback policies and private staging cleanup are unchanged.

## Acceptance

The ordinary APIs are exercised through SQL callbacks that rename the published
file, write a later replacement at its pathname, then abort the SQL update.
Before the fix, both collection and extraction delete that replacement. After
the fix, both leave its bytes intact, preserve the database and report the path.
A real collection document also exposes that warning in its failure text.

Controls cover normal SQL rollback, busy and absent files, different-object and
directory replacements, and alternate hard links. A symbolic-link fixture runs
where the native account permits creating one; a privilege-related skip is
reported explicitly and is not a measured symlink pass.
