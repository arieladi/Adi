# Native tuning and key-scale edits

All six operations run on the Store owner through OpJournal. Identities and
all musical values are explicit payloads, independently of selection, grid,
playhead, presets and files. Submit related edits in one journal transaction
for one undo step. No schema or ANOT change; notes keep their existing pitch.

- `tuning.create {id, definition}` creates a new explicit definition.
- `tuning.replace {id, definition}` replaces a present definition, after
  validating every key membership that references it. Its complete old rows
  are the inverse. It cannot remove a still-selected degree.
- `tuning.delete {id}` removes an unreferenced definition. Its inverse restores
  every authored value. A tuning in use refuses, even if a caller planned to
  remove its references later: detach keys first in the same transaction.
- `project.insertKeyEvent {id, pos, scale}` inserts at nonnegative integer ticks.
  Position and identity must be unused. There is no implicit upsert.
- `project.removeKeyEvent {id}` captures and removes the whole key, including
  tuning membership, for complete undo.
- `project.setKeyScale {id, scale}` changes a present key's scale and tuning
  membership, retaining its identity and position. The complete former scale
  is captured, including an empty degree list or absent tuning reference.

`definition` is `{name, source, period, description, degrees}`. Every degree
is `{id, cents, name}`. `source` is `edo` or `scala`; this identifies authored
provenance and does not fabricate an equal division. The degree rules are
those of ADR-0178 and ENGINE-SCALE-CONTEXT.md: positive finite period, unison
0 at 0 cents, strictly rising IDs/cents within the period, sparse IDs allowed.
At most 65,536 degrees, 65,536 bytes per text value, 8 MiB combined text.

`scale` is `{root, scale, mask}` for implicit 12-TET, adding `{tuning, degrees}`
for a tuned scale. Root is an integer 0..11; `scale` is its authored identifier.
Mask has twelve bits even for tuned keys, so older readers see a valid fallback;
a tuning-aware reader ignores it. Selected degree IDs must be unique and exist
in that tuning. Their ordering is not musical content: relational rows return
in degree order. Empty membership is intentionally empty. A missing tuning
means implicit 12-TET; no `degrees` key is then accepted. Unknown nested fields,
noninteger identities/degrees, overflow and invalid domains refuse.

`tunings::definitionPayload` and `scalePayload` build value objects on the owner
without ID reservation or document writes. The outer operation supplies its
identity. Handler/inverse callback guards run before SQL; the journal and
payload construction themselves remain owner-only. Error paths use the
journal's transaction rollback, including failure after partial row writes.
The normal journal veto/head/document policy remains the caller's responsibility
for async preparation.

The typed read model and text projection now cover all five scale tables.
Tuning definitions have content-labelled nodes, with exact double cents and
escaped author names/descriptions; key changes refer to content designators,
not row IDs. Degree numbers are authored coordinates, not database identities.
Projected key masks and degree membership remain distinct. This projection is
not an import format. Editor/device wiring, Scala file parsing, preset parity,
KBM/reference frequency, tuning bypass and Retune Set On Loading are separate.
