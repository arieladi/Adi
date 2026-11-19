# Render job preset field groups

`render::applyJobPreset(target, preset, fields)` is the pure field-group portion
of PLAN EX-09, following ADR-0071 d1/d3/d4 and ADR-0205 d9. It edits a job value,
not a queue or project. It never reads files, resolves IDs, enqueues or renders.
Both jobs can be ordinary values returned by the strict Job JSON codec. This
does not define preset storage, field-group wire values, a settings key, UI or
control RPC, and does not close EX-09.

The plan names Bounds and Output, Options and Format, and All but does not
enumerate every current C++ member. This slice uses the explicit mapping below.
It is a bounded implementation choice, not a new director ruling or a claim
about the source DAW's exact preset format.

| Group | Fields copied from the preset | Fields kept from the target |
| --- | --- | --- |
| `BoundsAndOutput` | `directory`, `nameTemplate`, `collision`, `beginSample`, `endSample`, `regionId`, complete `source`; for NI, the ordered stem list including each source, name and color | All processing/encoding/normalization settings, NI bitrate and container presence; project and runtime bindings |
| `OptionsAndFormat` | Session channels, sample rate, maximum block frames, precision and delay compensation; complete encoding and normalization; for NI, bitrate | Directory, naming, collision policy, source and region, both numeric bounds, NI stem list/metadata and container presence; project and runtime bindings |
| `All` | Both groups, including the presence/absence and complete value of NI Stems | Project and runtime bindings |

The target always owns `project`, `SessionSpec::progressiveDecoder`,
`externalTempoBpm`, `performanceState`, `exporting` and `offline`. A preset never
replaces them, even under All. A decoder service is a borrowed runtime binding,
not transferable preset data. A target with a live external tempo is refused,
consistent with the codec and the existing offline preparation contract; an
ignored external tempo in the preset is not imported. Processing precision,
delay compensation and block size are explicitly options in this mapping.

The helper copies the target into a local candidate, applies only the selected
groups, calls the existing complete `validateJob` contract and checks the live
clock exclusion, then replaces the target with a statically verified nonthrowing
move. Validation/allocation failure leaves the target and preset untouched.
Self-application is safe. Unselected preset fields are not applied or validated:
an options-only application cannot fail because an ignored preset destination
or source was incomplete. The resulting candidate must pass the queue's input
rules; project existence, source/marker existence, graph preparation and codec
availability remain render-time checks. The mask helper does not replace the
strict codec's separate wire/UTF-8/size checks.

There are three explicit boundaries rather than implicit reinterpretations:

- **Project-bound selections.** Bounds and Output or All require identical
  native project-path spelling. They keep the target's project path. This is a
  conservative admission check, not proof of database identity: no canonical
  path lookup or ID relocation is attempted. Even an alias for the same file is
  refused. Options and Format can be reused across projects because no source,
  region or NI stem IDs are transferred. Portable/cross-project bounds presets
  need a separate reference-binding contract.
- **Sample clock.** Explicit numeric bounds use `session.sampleRate`. Bounds
  and Output refuses a different preset clock unless the incoming bounds use
  a region. Options and Format refuses changing the clock unless the target
  already uses a region. Regions keep their stored unused numeric bounds and
  resolve at render start. All can copy explicit bounds and their clock together.
  No rounding/rescaling policy or automatic source-time relocation is invented.
- **WAV/NI layout.** A partial group requires matching NI presence. NI source
  selections and metadata belong to Bounds and Output; its bitrate belongs to
  Options and Format. Changing container presence requires All, with a complete
  valid source/layout. This avoids importing NI source IDs through a format-only
  preset or discarding the target's stem selection. A format-only transition
  that synthesizes or preserves dormant NI selections remains unspecified.

Only the three named enum values are admitted; zero and unknown combinations
are refused. The enum is an in-memory interface, not a new persistent schema.
Future Job fields need an explicit
group assignment and tests when integrated.

`adi_render_job_preset_tests` proves that options/encoding leaves the target's
destination, naming, source, region and bounds untouched; checks both groups and
All, owner binding retention, self-aliasing and no I/O; exercises ordered NI
sources separately from bitrate; and checks complete rollback on invalid groups,
project/clock/layout mismatches, invalid resulting normalization/encoding and
unavailable NI normalization. The result is also admitted through the actual
Queue without rendering. User preset-file and project saved-job storage are
separate APIs. A project preset envelope, field masks on the wire and queue RPC
remain later EX-09 slices.
