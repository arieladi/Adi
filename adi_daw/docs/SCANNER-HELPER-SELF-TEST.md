# Dedicated scanner helper self-test (DEV-25)

`adi_plugin_scanner --self-test` is an explicit deployment diagnostic. Only that
exact command is accepted: extra arguments, mixed scan/self-test commands, and
other spellings return exit 64 without loading a candidate or emitting metadata.
The normal `--scan FORMAT PATH` contract is unchanged.

Successful stdout is one JSON record, bounded to 1 KiB by the receiver contract:

```json
{"kind":"adi-plugin-scanner-self-test","protocol":1,"metadata_schema":1,"formats":["clap","vst3"],"evidence":"structural","plugin_loaded":false}
```

The non-JUCE helper reports only `clap`; a helper compiled with its VST3 path
reports `clap` and `vst3`. The protocol version belongs to this command and is
independent of plug-in metadata and registry versions. All fields describe
compiled structure. This command does not enumerate search roots, open a
registry, load a plug-in, negotiate a driver, or prove that any particular module
can be instantiated or processed. Exit 74 means an output-channel failure.

The parent Scanner and discovery service do not run this command automatically.
An unchanged candidate therefore keeps the existing success-cache behavior,
including across owner reopen. `adi_scan_self_test_tests` runs the real helper,
checks the exact packet and malformed arguments, then uses an owned CLAP positive
control to verify one module initialization across three unchanged probes. The
existing scan-cache gate separately counts child launches with its controlled
protocol helper; module-initialization counts are not relabelled as process counts.

PLAN's batch-preflight activation is not implemented by this command-only slice.
A future owner can invoke it once per explicit scan batch; reusing a validated
receipt while the helper's canonical identity/build hash is unchanged is another
option to resolve when that policy is implemented, not a policy adopted here.
Neither option should turn each cached candidate into a helper launch.
It must reject unknown protocol/metadata versions, unknown or duplicate fields,
invalid format sets and oversized output before treating deployment as ready.
No invocation belongs in an audio callback, project-open hydration, or each warm
probe. A self-test launch, timeout, malformed reply or unsupported-format failure
is helper infrastructure evidence, not a candidate failure: it must not create a
pending plug-in ticket, negative cache entry, strike or quarantine row. A helper
without this additive command needs an explicit compatibility decision rather
than silently triggering rescans. Activation, deployment-policy caching and
parallel scan scheduling remain separate work.
