# Persistent plug-in scan quarantine

R-17 requires crash-on-scan quarantine and a visible disabled list with explicit
re-enable. `PluginRegistry` now stores that state in the suite's existing shared
`plugins.sqlite`, beside route preferences. Registry version 2 migrates version
1 transactionally, preserving every route/time. Its application id stays ADIP.
Older version-1 applications will refuse this newer registry; update suite
applications together. This is application data, not a project schema/op log.

The scanner calls `beginScan(format, path, nowUtc, error)` **before launching a
child**. Failure means do not launch. Success returns an opaque ScanTicket only
after a pending row commits. The exact `(clap|vst3, path)` key is shared across
suite applications. Existing pending/blocked keys refuse a second attempt.
There is no automatic expiry, retry or clearing during open/close. Pending
therefore survives parent death and names an interrupted attempt on relaunch;
another live application sees the same pending state. Pending is deliberately
not labelled proof that the plug-in crashed.

Complete using the same still-open connection and ticket:

- `Succeeded`, empty reason: remove only this pending marker; accept metadata
  only if completion succeeds.
- `Abandoned`, empty reason: remove only this pending marker when the user
  cancelled or the helper could not launch. Those failures do not prove a bad
  plug-in. Never use this outcome for a plug-in crash, timeout or bad metadata.
- `Failed`, nonempty diagnostic: keep this marker as blocked with the explicit
  diagnostic and completion time.

`blockScan` explicitly blocks a path or replaces its pending state; `unblockScan`
is explicit, idempotent re-enable. A stale success/failure cannot clear or poison
a newer block/re-enabled attempt. The private ticket holds a weak connection
identity and an AUTOINCREMENT row identity; old ids are never recycled, even
after deletion. Tickets from another connection, another file with matching ids,
or a closed/reopened connection refuse. SQL failure leaves prior quarantine
intact; no metadata may be accepted after failed completion. There is no
in-memory-only fallback if the registry cannot be written.

The owner supplies nonnegative Unix microseconds. No clock is read here. Paths
are exact absolute UTF-8 keys up to 65,536 bytes; diagnostics are 1–4,096 bytes of
well-formed UTF-8. NUL refuses. The scanner must resolve aliases/canonical paths
before calling: this layer performs no filesystem lookup, normalization, Unicode
case folding or filename inference. Runtime instantiation policy remains D-03's
in-process policy; this quarantine concerns discovery, not running devices.

`scanBlocks(error, maximumRows)` exposes owned rows sorted by format/path,
including pending state, reason and timestamp. The default bound is 4,096 rows;
callers may request 1–65,536. Storage caps at 65,536 entries across formats; new
attempts/blocks refuse at capacity, while existing blocks can still be updated
or re-enabled. Reads also cap aggregate authored text at 16 MiB and preflight
individual stored column sizes/types before copying. Excess/invalid data returns
an error for the whole list, never a successful prefix. Held rows have ordinary
owner lifetimes; no live database pointer is exposed.

Use one owner at a time per connection, and separate connections for concurrent
suite jobs. SQLite serializes competing admissions; exactly one gets a ticket.
All registry entry points, including open and the older route APIs, now refuse
callbacks before SQL/filesystem/allocation or mutation of caller diagnostics.
Writes/reads otherwise remain blocking owner APIs with the existing busy timeout.

The new suite covers real version-1 migration/rollback, preserved preferences,
cross-connection visibility and admission, reopen interruption, copied/foreign/
expired/stale tickets, late user blocks and replacement failures, abandonment,
invalid inputs/callback guards, SQL-trigger failures, full 65,536-row capacity,
row/text read bounds and later invalid data. External recovery review additionally
uses a real `_Exit` after begin, bypassing all database destructors, then verifies
blocked reopen and explicit re-enable through the actual API.

This component provides persistence and owner handoff. ENGINE-PLUGIN-SCANNER.md
now connects it to the dedicated helper and all CLAP/VST3 discovery paths,
including direct VST3 path hints, without an in-process discovery fallback.
Descriptor caching and a visible blocklist UI remain consumer work; runtime
plug-in instantiation stays in process under D-03.
