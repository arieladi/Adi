# Basics device parity

SC-481, SC-482 and SC-483 are the review queue in the [28 November parity prompt](../prompts/2026-11-28-device-parity-lists.md). The review records the code and tests on `origin/main` and places each model feature on the proposed device card.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-481 | `basics/parity-lists` [21878d0af328](https://github.com/arieladi/Adi/commit/21878d0af32804e701b09f6380ef88efdeb86cee) | 178 features with card placements and PLAN coverage. Recorded the sound comparison and measured difference for each of the seven implemented Shapers. | Cableguys v3.6 manual and the clones seat’s measurements. Shaper core, stage, registered Pd and Session suites passed. | PR requested |
