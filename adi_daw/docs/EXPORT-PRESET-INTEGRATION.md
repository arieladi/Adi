# EX-07 / EX-09 integration

This local integration starts from `engine-win/export-format-name-tokens` at
`fa9534a672f6c5926387b4a46c82af08774335ae` and merges the exact user-preset topic
head `ea37af92aaa3ebe13b0c220f0fc378ac41328c14`. It does not merge `main`.
The complete-byte-stream rejection and two literal-NUL regressions from
`cd265a59d80913dbcf2e6b7cd663ae25d6468fda` are retained.

Three overlapping hunks have explicit resolutions:

1. `queue.cpp`: EX-07's private folded-name/replacement helpers stay anonymous;
   `validateJob` is exposed in `adi::render` for EX-09. It retains collision
   validation and `{format}`, `{rate}`, `{index}` validation context. All queue,
   render, codec and mask callers use that same validator.
2. WAV final publication retains EX-07's `detail::publishVerifiedOutput`, including
   prepared identity/content/folded-name binding, publication warning/durability
   reporting and existing final checks. The older direct publish block is not
   restored.
3. NI final publication also retains `publishVerifiedOutput`. Its final name is
   prepared from the frozen Main metadata with the final NI format, and the Main
   intermediate WAV snapshot does not overwrite that final naming receipt.

Job JSON now writes version 2 with required lowercase `collision` values
`fail`, `increment`, `replace`, identical to the run manifest. Strict old Job
version 1 without that field imports as Fail. Version 1 with the field, version
2 without it, unknown versions/keys, bad enum values, duplicate keys and literal
NUL bytes refuse. There is no default for malformed version-2 collision data.

Collision is part of Bounds and Output, hence also All. Options and Format
preserves the target's policy, even if the unselected preset policy is invalid.
Candidate validation still precedes application. Project/user envelope versions
stay 1 while their strict embedded Jobs can be legacy v1 or new v2; explicit save
writes v2. Load, codec conversion and mask application perform no rendering or
output-file replacement. User preset-file publication keeps its separate guarded
whole-list contract regardless of any stored Job's collision field.

The native core validation covers both cohorts together: existing export queue,
NI metadata/prepared final naming and publication/replacement tests, plus the
strict codec, masks, project saved jobs and user preset stores. New regressions
cover each collision enum, legacy import/explicit resave through both stores,
ambiguous version shapes, and mask preservation/refusal. Runtime codec-dependent
or privilege-dependent cases retain their own explicit test availability limits.
