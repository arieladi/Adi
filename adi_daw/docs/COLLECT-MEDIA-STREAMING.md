# External media copy cancellation (PD-02)

Collect and Export's existing worker control now reaches external media copying
inside its private snapshot. The public no-argument `Store::collectAllAndSave()`
and `StoreFileResult` are unchanged. An internal `collectSnapshotMedia` entry
shares the collection body, existing hash verification, exclusive publication,
SQL relocation and identity-bound rollback. Ordinary unobserved Collect All
and Save retains its `copy_file` path.

The controlled path copies at most 64 KiB per read/write pair. It checks stop
before opening, before each pair, after each progress observer, after close,
after hash verification, between files and immediately before SQL COMMIT. Stop
after relocation therefore rolls back all location changes. Cancellation never
relabels a completed COMMIT; later archive phase checks still prevent the ZIP
from publishing. Earlier privately published media uses the existing rollback
identity guard; warnings are retained in the failed/cancelled result. The owned
outer archive stage is then discarded.

`ArchiveProgress` adds three fields used only during `CollectMedia`:

- `mediaBytesCopied`: source bytes read and written successfully into the current
  private copy, before close, hash verification or publication;
- `mediaBytesTotal`: that source stream's length when opened;
- `mediaFile`: an owning UTF-8 source path; empty at the ordinary phase boundary.

Counts reset per file, begin at zero even for empty files and include the final
possibly short chunk. They are not ZIP bytes, durable bytes, a total across the
project or an indication that collection completed. Existing ZIP entry fields
retain their meanings and stay zero/empty during copy notifications. Progress
observers run synchronously on the owner worker, must not close/mutate/reenter
the private Store, and may request stop. Exceptions, including nonstandard ones,
become copy-progress failures rather than cancellation.

No source project or source media is written. An altered/truncated source or
write/close error still fails; the completed private copy must pass the existing
expected BLAKE3 check before publication. The same sync/no-clobber publication
and rollback contracts apply. This does not introduce a namespace race fix or
replace the existing owned-stage assumptions.

The bound is byte work, not elapsed time: filesystem operations and observers
can block. SQLite backup, legacy embedded extraction, source resolution/hash,
final project verification, publication internals and ZIP finalization remain
separate phases. There is no GUI, location-filter or throttling policy in this
slice.

Tests use actual collection and ZIP readback: stop at zero, 256 KiB and the last
chunk of a 4 MiB external source; nonstandard observer failure; exact successful
archive contents; stop at a second file after the first was privately published;
an empty source; and a deterministic SQL trigger requesting stop after relocation
but before COMMIT. Cancellation preserves source bytes and project rows, creates
no ZIP and cleans private stages. The pre-COMMIT fixture also proves SQL rollback
and the existing Windows owned-file cleanup. Existing ordinary collection,
settings-bundle, ZIP and project-copy tests cover the unchanged entry points.
