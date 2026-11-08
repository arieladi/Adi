# Captured material placement (ED-03)

`adi/edit_plan.hpp` is the owner-thread entry for a prepared group move and
anchored-start end resize. It implements the material foundation of
ADR-0217 d1–d2. Commit its `clip_edit::PreparedEdit` through `commitPrepared`,
or the document's existing prepared-edit submitter. Preparation writes no
project rows. The captured filename and journal head, including a null head,
guard the single `arranger.splice` transaction. Retain the document generation
when a worker returns a prepared value, as with the other clip commands.

This delivery is partial ED-03. It does not close AU-05/AT-10, the media and
recording producers, the window's raw-move/resize bindings, or the Live 12.4
measurements. Raw op handlers remain available for historical replay.

## Literal scope and group semantics

`EditScope` owns its clock, literal track/lane pairs, bounds and domain flags.
The clock is ticks or nanoseconds, never samples. `materialScope` constructs
main-lane targets; take-lane editing awaits CT-01. The supported domains are
Clips and Automation. Markers, MusicalMaps and Loop are represented by the
common type but refused by these ordinary material entry points.

Scope bounds admit the source and destination extents of moved material.
They are not a clearing rectangle. A group move clears the union of its
actual destination spans on each lane, leaving gaps between members intact.
All moved originals are excluded from clearing before any incoming member is
placed. A swap therefore retains both identities regardless of input order.
Overlapping incoming members on one lane refuse with a named diagnostic.
Copies receive new identities and do not exclude their originals from clearing.

A destination victim may extend beyond the declared bounds. Its outside
pieces remain. The sole or earliest surviving piece keeps the original clip
and material-child IDs; later pieces receive IDs from the shared `CopyIds`
allocator. Complete deletion captures external alias detachment for exact
undo. Splitting an alias still needs explicit alias resolution.

`edit_placement.hpp` contains the pure interval resolver: owned values in,
owned intervals and lineage out, with no Store, SQL, settings or filesystem
access. It allocates no persistent identities. The owner adapter captures
images, performs source-clock transforms, allocates identities and validates
the final typed row delta. The entire image/row transformation has not been
extracted into a storage-independent pure function.

All group members are captured before destination changes are staged. The
resolver unions masks and derives every survivor from the original extent;
it never re-reads the unchanged database between successive masks. Empty
incoming intervals clear nothing. An empty original is removed only strictly
inside a positive merged mask; either boundary survives. Dedicated Time
commands retain their separate boundary semantics.

## Playback clocks and resize

Pass the actual Session sample rate in `Options.referenceRate`. Audio source,
loop and fade domains are frozen before changing a visible boundary. Retained
outer endpoints remain in their original native clock. The owner resolves
intervals in physical nanoseconds, including when the public scope uses ticks,
so a rounded touching boundary cannot hide a physical overlap. Interior tick
cuts use directed conversion within the original native extent: a surviving
begin rounds inward toward the future, an end toward the past. A collapsed or
unrepresentable survivor refuses. A representable rounding gap can remain.
Descriptions still use the declared scope clock. There is no blanket exact
ns/tick round-trip test.

`prepareResizeEnd` keeps the native start, source offset, loop and source-end
marker. Left trim, Crop and source-zero extension belong to ED-04. An authored
fade-out follows the new visible end with its captured duration, clamped to
the new span. The anchored fade-in keeps its source origin. Retained fade
domains on a side whose visible handle is zero remain inherited source
envelopes, as after a split; resizing does not turn them into new edge fades.
Undo restores the original rows, including absence of optional clock rows.

## Captured settings and admission

An owner with `AppSettings` calls `capturePolicy(settings)` once and passes
`Options{referenceRate, policy}`. This bridge is linked through `adi_settings`;
the planner in `adi_core` receives only the owned policy value. Settings are
never read during commit or replay. The rate-only compatibility overloads
select the registry's default Live policy values; they cannot discover a
particular window's settings object. Window bindings must pass their snapshot.

`automationFollowsClips=true` means Lock Envelopes is off, the default. Until
AU-05's guarded replacement is integrated, such a request refuses if an
affected source or destination has a global automation lane. The check includes
track lanes, recursively owned device lanes and outgoing send lanes, including
default-only lanes and curves with no point inside the selection. It does not
silently perform a locked edit. With Lock on, global automation stays at song
position; clip-owned envelope rows travel with their clip image.

The initial policy admits Live, Ripple Off, Delete Overlaps on and
Muted Clips As Deleted off. Other values have explicit refusal diagnostics.
`deleteOverlaps=false` requires clarification of the retained setting against
ADR-0217 d1; this implementation does not invent a second overlap rule.
Frozen tracks and frozen groups refuse pending TM-FC-04's policy. Explicit
signed clip/track IDs and existing mixed clip/track kinds remain accepted where
the schema accepts them.

## Descriptions and callers

`describe(prepared)` returns owned scope, policy, names, before/after extents
and split/trim/remove/create/move/resize lineage. Each material state retains
its native clock and extent, physical nanoseconds and display projection into
the scope clock. A very short physical clip may have an empty tick projection
without losing its exact geometry. Description needs no live Store.
Descriptions are bound to the source, head and semantic request prefix.
Changing a captured payload, op or target invalidates the description. Actor
stamping and undo-label changes are allowed. Appended requests are listed
separately; their arbitrary effects are not presented as captured placement
lineage or silently added to its declared scope.

Ordinary clip paste/duplicate and material range delete/paste/duplicate use the
shared planner. Range paste adds the full clipboard destination as a clearing
mask, including its silence. Its generated scope also covers each incoming
clip's retained native duration across a tempo change; that duration can exceed
the requested musical span. Dedicated whole-Set Time editing and the existing
`insertTime=true` path retain their all-lane and global-timeline implementation;
ordinary placement does not substitute for them or apply Lock to them.

Still to migrate: import/bounce, audio recording, MIDI Capture, stem producers,
the document/drop binding and window move/resize producers. Media, track and
routing prefix operations remain necessary: they are outside `arranger.splice`'s
table whitelist. A future producer migration must preserve publication,
expected-head, one-use handle and retry/cleanup contracts while appending one
combined placement splice in the same transaction. The 100-track native
document measurement is evidence for the engine path; it does not replace a
window-driven drop measurement before retiring the window's raw producers.

## Reproducing the native document measurement

Enable `ADI_BUILD_BENCHMARKS` and build `adi_edit_placement_measure`. Run it with
one new output-directory path and an optional primed repetition count (1–100,
default 12). It creates a disposable 100-track project plus
Main, 200 clips, fixed and re-pitch Warp audio, MIDI and 100 lightweight native
probe devices. It measures the existing `ProjectDocument`/`OpSubmitter` path
for single and eight-member moves at 48 kHz with 256-frame callbacks and both
processing widths. The prepared path clears covered victims; the raw baseline
retains them, and the report records the resulting clip count.

The JSON separates preparation, journal commit, view/submit handling, Session
rebuild/publication, callback adoption and the first nonzero aggregate output
after adoption. It also records device retention, ordinary new/new[] counts,
callback file I/O, overruns, source underruns and read errors. A fresh document
has one sample. With one primed sample, report individual observations rather
than a latency distribution. Fresh does not mean OS-cold cache.
The probes are not VST3/CLAP performance measurements, aggregate audibility
does not prove every source ready, and the diagnostic driver is not hardware.
Use the build configuration and source hashes with the results; there is no
invented latency threshold. Runtime readings and their limits accompany the
engine log's PR request.
