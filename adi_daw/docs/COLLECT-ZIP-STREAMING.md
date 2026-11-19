# Cancellation while writing a ZIP entry (PD-02)

`ZipWriter::addFile` retains its normal source/path/compression arguments and
adds an optional `ZipEntryControl`. Existing callers, including settings-bundle
export, keep the same default STORE/optional DEFLATE behavior, ZIP64 metadata,
bounded streaming and required `finish()`.

The worker checks stop before and after each input read of at most 64 KiB,
including after the progress observer returns. There is also an initial
zero-byte checkpoint, even for an empty entry, and a stop check after miniz
returns. Cancellation is sticky `ZipError::cancelled`: no subsequent entry or
`finish()` can turn the partial archive into success. Miniz can treat a zero
callback return as end-of-entry, so the explicit cancellation/observer flags
take precedence over its success result and any apparent full input length.

Progress reports uncompressed source bytes **read**, not compressed ZIP bytes,
bytes accepted by miniz, durable data or completion. An observer may request
stop after a read that is consequently not handed to miniz. Counts are
monotonic within an entry and end at its actual source length on success.
Callbacks run synchronously on the writer's owner worker and must not reenter
that writer. Standard and nonstandard observer exceptions are caught before
returning through miniz's C callback and become sticky `ZipError::progress`.
They are failures, not cancellation. There is no exception-based C unwind.

`collectExport` forwards these checkpoints through its existing `ArchiveControl`
and reports `FileResult.cancelled`. `ArchiveProgress` keeps its phase and
completed-entry count, adding the current entry name and read/total bytes.
During an entry, `entriesWritten` counts only earlier completed entries; the
current entry increments it only after `addFile` succeeds. Empty entry name
means a phase or completed-entry boundary. The owning name string is safe to
copy when posting progress elsewhere. A callback failure is reported as such.

Cancellation during an entry destroys the private ZIP, copied project and staged
media. Publication remains the existing atomic/no-clobber operation, with no
public target before its commit. Existing destination files remain untouched.

This is a byte-work bound between stop checks, not a wall-clock latency promise:
an OS read, compression step, observer or finalization can block. SQLite snapshot,
media extraction/copy/hash/check phases remain cancellable only at their existing
phase boundaries. Central-directory finalization checks stop at the next phase
before publication; once final atomic publication starts, there is no rollback
or post-commit cancellation claim. Those other phases, GUI, throttling and
concurrent filesystem identity redesign are outside this slice.

Tests cover normal STORE/DEFLATE progress and exact CRC/byte round trips,
pre-cancel, empty-entry cancel, mid-entry stop, sticky failures and both exception
categories. The real collect regression imports a 4 MiB media entry, requests
stop at its fourth 64 KiB read, and verifies no output, clean private stage and
unchanged source bytes/project. Existing settings, copy and collection suites
remain compatibility checks. Large optional ZIP64 fixtures keep their explicit
opt-in; normal tests do not allocate multi-gigabyte archives.
