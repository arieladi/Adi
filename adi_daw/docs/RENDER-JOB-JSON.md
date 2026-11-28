# Render job JSON

`render::jobToJson` and `render::jobFromJson` implement the value codec portion
of PLAN EX-09, under ADR-0071 d1/d3/d4, ADR-0205 d9 and ADR-0222 d3. They do not
open a project, create files, enqueue work, load devices or start rendering.
Queue execution, user/project storage, field-group application and the control
API remain separate operations. This slice does not close EX-09.

The document is a complete job, not a patch. Its root contains
`"format":"adi-render-job"` and integer `"version":2`. Every field below is
required. Every object is closed: an unknown field is reported and refused,
including within a source or an NI stem. Duplicate keys, including escaped
spellings of the same key, and literal NUL bytes are refused before building a DOM. The reader bounds
input to 1 MiB and 16 nested containers. The writer also refuses output above
1 MiB. Input and output are UTF-8; paths retain their native spelling, including
relative paths and Windows verbatim/UNC prefixes. There is no path resolution,
canonicalization, portability remapping or existence check. NUL in a path is
refused.

| Object | Fields | Representation |
| --- | --- | --- |
| Root | `format`, `version`, `project`, `directory`, `collision`, `nameTemplate`, `beginSample`, `endSample`, `session`, `source`, `regionId`, `encoding`, `normalization`, `niStems` | Paths/name are strings; `collision` is `fail`, `increment` or `replace`, matching the run manifest; bounds are exact signed 64-bit integers; `regionId` is null or a positive signed 64-bit integer; `niStems` is null or its object. |
| `session` | `channels`, `sampleRate`, `maxFrames`, `precision`, `performanceState`, `delayCompensation`, `exporting`, `offline` | Exact integer channel/block counts; finite numeric rate; `float32` or `float64`; four exact booleans. |
| `source` | `trackId`, `tap` | Exact nonnegative signed 64-bit ID; `preFader` or `postFader`. Zero means Main as in the existing runner. |
| `encoding` | `format`, `dither`, `ditherSeed` | `pcm16`, `pcm24`, `float32`; `none`, `triangular`, `rectangular`; exact unsigned 64-bit seed. |
| `normalization` | `mode`, `targetLufs`, `truePeakCeilingDb`, `channelWeights` | `off`, `peak`, `loudness`; finite numbers; finite-number array, retaining order. |
| `niStems` | `bitRate`, `stems` | Exact unsigned 32-bit bitrate; one to four stem objects. |
| NI stem | `source`, `name`, `color` | Source as above, positive track ID; metadata strings retained exactly, in the original order. |

Integer fields require JSON integer tokens, not a floating-point approximation
or numeric string. All integer bounds are checked before conversion; a seed of
18446744073709551615 and signed 64-bit boundary values do not pass through a
double. Versions are checked before narrowing, and unsupported enums never
fall back to a default. Unknown/new versions are refused rather than partially
imported.

Legacy integer version 1 is accepted only with its exact old field set, which
omits `collision`. It imports as `Collision::Fail`. A version-1 document with
any collision field is refused, including an explicit `fail`; version 2 must
include the field. Case variants, numeric ordinals, unknown policies and missing
fields refuse. Writers always emit version 2. Project/user storage envelopes
remain version 1 and accept either supported embedded Job version; a later
explicit save writes embedded version 2. Merely loading performs no migration
write, output-file replacement or rendering. Stored `replace` is data, not a
publication action or a way around the renderer's identity/content checks.

Both directions use `validateJob`, the same pure input validator used by
`Queue::add`, `Queue::update` and rendering. This preserves the existing format,
normalization, source/tap, NI and naming rules. In particular, when `regionId`
is present the stored begin/end bounds survive without an additional range
rule: the runner resolves that marker at job start. Nothing adds an enqueue-time
revision snapshot or resolves the naming template early.

The codec additionally refuses nonfinite stored normalization numbers even
when Off would ignore them: JSON must not silently turn them into null. A
non-null `SessionSpec::progressiveDecoder` is a runtime service and cannot be
persisted. A non-null `externalTempoBpm` is explicitly a live performance clock;
the existing offline render path cannot prepare it after disabling performance
state, so the codec refuses it with a specific diagnostic. These two fields have
no wire keys and decode to null. All remaining SessionSpec value fields are
retained; the runner still applies its normal offline/export overrides later.

The APIs throw `std::invalid_argument` for malformed or unsupported job inputs,
matching queue validation. A failed decode returns no partially filled Job and
cannot mutate the caller's job or queue. Allocation failures may propagate.

`adi_render_job_json_tests` covers default and nondefault complete jobs, all
supported encoding/dither/normalization enum combinations, NI sources and
metadata, native Unicode paths, exact integer limits, queue admission,
unknown/duplicate/missing fields, invalid types/enums/numeric boundaries,
runtime-only state, bounded parsing and no filesystem side effects. Existing
render and NI tests exercise the shared validator's unchanged callers.

The codec integrates the EX-07 collision Job field from `fa9534a6` with the
EX-09 value/storage stack at `ea37af92`; see [integration notes](EXPORT-PRESET-INTEGRATION.md).
Future Job additions
must extend the codec, schema version/compatibility policy and round-trip tests
together; fields from unmerged export branches are not silently accepted.
