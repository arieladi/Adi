# Sampler up to Live 12's Sampler (SC-483): the log

Queue: the 116 rows of `docs/review/device-parity-2026-11-28.md`, part SC-483. Sound, controls and
tests only; the card's look is the devices seat's.

- Code: branch `basics/sampler-chain` (one chain, a commit or a few per item, SC-483 in each message).
- This log: `adi_daw/collab/basics/sampler.md` on `basics/sampler-log`.
- No pull request is opened: win merges the chain into a landing stack.

## Where things are (read this first if you continue the work)

- The device a user loads is the built-in `org.adidaw.builtin.sampler`: `src/juce/builtin_host.cpp`
  (`BuiltinDevice`) hosts `SamplerNative` (`src/adi/pd_builtins/native_devices.cpp`), which wraps the
  core `device::Sampler` in slot mode (`src/adi/dsp/sampler.cpp`): 32 layers, each a `OneShot` voice
  engine (`src/adi/dsp/oneshot.cpp`) playing the host's sample slot.
- Its parameters are the lines of `src/adi/sampler_declarations.hpp`. Ids are fixed: add at the end,
  never renumber. A state without a parameter gives that parameter its default (DF-E57), so a project
  saved before a parameter existed opens with the default, which must be the old sound.
- The devices lane's Sampler branches (`devices/sampler`, `devices/sampler-loadable`,
  `devices/perf-sampler`, the `devices/oneshot*` ones) are all merged into main: nothing unlanded was
  left to take over.

## Round 1 (28 November 2026)

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|

## For the director

Nothing yet.

## Reserve

Nothing yet.

## Next

Round 1 starts with what belongs to one sample.
