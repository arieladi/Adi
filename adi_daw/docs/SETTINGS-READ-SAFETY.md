# Settings read failures (PD-11)

`AppSettings` distinguishes missing settings, unavailable settings, and bytes
that were read successfully but are malformed. This is a data-safety boundary,
not a new corruption-recovery or automatic-retry mechanism.

- A genuinely absent path still yields `LoadStatus::Defaults` and may be saved.
- An inspection error, non-regular file, failed open, or incomplete read yields
  `LoadStatus::Unavailable`. The original path is not renamed or written. The
  diagnostic is available from `loadError()`. `get()` can return registry
  defaults for startup, but these are not a successfully loaded document.
- An unavailable instance refuses `set`, `reset`, page reset writes and `save`.
  Releasing the lock or repairing access does not silently enable that same
  defaults-based instance. Construct a fresh `AppSettings` to retry loading and
  use the successfully loaded instance for subsequent edits.
- Bytes read to a clean end of file still pass through the existing schema and
  migration checks. A malformed document is `CorruptKept` only after its
  `.corrupt-N` move succeeds and `keptAside()` names that recovery file. If the
  move fails, it is `Unavailable`, `keptAside()` is empty, and `loadError()` gives
  the recovery failure and OS error. This instance stays read-only even after
  the obstruction is released; construct a fresh instance to retry recovery.
  Foreign/newer documents and failed migrations retain their existing protection.
- A previously loaded instance that encounters a read failure during `save`
  keeps its pending edits. Publication and backup are refused, and `save` may be
  retried after access is restored. It re-reads and merges the current document
  through the existing snapshot checks and authoritative publisher.

Construction and saving share the existing settings snapshot reader. Its
regular-file rule also refuses a symlink instead of treating its target as
corrupt; save already used this rule. A failure during reading is never parsed
as an empty or partial JSON document. Both readers reject literal NUL bytes
before parsing. The load path keeps the complete malformed document according
to the existing corrupt-copy behaviour. The save reread refuses replacement.

`adi_settings_read_failure_tests` uses owned temporary files. Windows cases hold
a native handle that denies competing reads while permitting rename, exercising
the actual harmful constructor path, and verify save retry after releasing the
handle. A second native case locks a later byte range of an otherwise valid
JSON document, proving that a successful open and valid prefix do not establish
a complete read. Missing, malformed and non-file paths are separate controls.
Existing settings and migration suites remain unchanged.
Another Windows handle permits reads and writes but denies delete/rename. The
regression reads the complete malformed bytes, checks failed-recovery status and
write refusal, then releases the handle. A fresh instance moves those exact
bytes aside and saves normal settings; the old failed instance stays read-only.
The existing earlier `.corrupt-N` copy is preserved throughout.

This slice does not redesign corrupt-file quarantine. The existing choice of a
`.corrupt-N` name followed by a path-based rename is not an atomic identity-bound
move. That concurrent-writer boundary remains separate work; reporting a failed
move as a successful kept copy is now closed. This does not add an I/O retry
thread, a lock-wait policy or UI recovery controls.
