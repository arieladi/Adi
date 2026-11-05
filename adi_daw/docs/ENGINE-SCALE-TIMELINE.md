# Per-key scale timeline

`engine::readScaleTimeline(store)` builds the global key/scale timeline on the
Store owner. It reads keys, tuning references and selected degrees, then only
referenced definitions, in one coherent deferred transaction. A caller-owned
transaction remains open. It reads no notes or clip-local properties, reserves
no IDs and makes no project/history changes. Orphan references, missing used
definitions and invalid used degrees refuse the whole result, including a bad
tuning after an otherwise valid prefix. Keep the prior good generation when a
rebuild fails; an error is not a new implicit 12-TET map.

`compileScaleTimeline(changes, definitions, error)` accepts plain owner values.
Changes must have positive unique key IDs and strictly increasing nonnegative
int64 tick positions. Each carries a global `ClipScale` (`local=false`), with
the same implicit mask or explicit tuning-degree rules as ScaleContext.
Definition IDs are positive and unique; only referenced contents are compiled.
Unused definitions do not change the active map. Input order is explicit,
rather than silently reordered.

Hold the immutable timeline for its reader/graph generation and retire it on
the owner, so the final shared data is never freed on the callback. Views and
returned context pointers borrow that generation. Later owner edits or source
value changes cannot alter it. A moved-from holder has no contexts/events.

`atTicks(t)` uses inclusive key boundaries: the key at t is already in effect.
Before the first event, including negative pre-roll, it returns C major/12-TET.
An empty map therefore has that default everywhere. `nextChangeAfterTicks(t)`
returns the next strictly later key boundary or no value; it never adds one to
t, so INT64_MAX is safe. Adjacent ticks beyond 2^53 remain distinct. Both queries
use bounded binary search, with no allocation, SQL, locks or file I/O.
`events()` exposes explicit key IDs, exact positions and held contexts.

Consumers can use these boundaries to schedule their own existing musical
clock; this API does not convert ticks to sample times, publish device events
or promise a new graph delivery path. Each returned ScaleContext still requires
an explicit absolute tonic for pitch membership/ruler operations. Stored notes
are not implicitly retuned. A clip-local scale remains a separate explicit
12-TET override through N32/N33; global lookup does not erase or apply that
local policy. No UI/device DSP change or automatic Session hook is implied.

Compilation preflights at most 65,536 events and 1,024 supplied definitions,
1,048,576 compiled degree rows including the default, 1,048,576 selected members,
and 8 MiB authored text. Repeated definitions/strings count for every event
that copies them, before any compiled contexts are allocated. A single explicit
definition still obeys ScaleContext's 65,536-degree bound. The Store reader also
bounds row/string accumulation before retaining whole input tables. Compilation
and reads refuse callbacks before SQL/copying. All errors return no partial map.
No schema/opcode/ANOT change; editor/device integration and measured Live parity
remain separate from this immutable lookup contract.
