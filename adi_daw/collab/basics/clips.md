# Basics clips

Queue claimed. SC-191 is first, per the round 2 ordering.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| DF-W56 (B13) | [#795](https://github.com/arieladi/Adi/pull/795), merge `302b3239` | Default track names renumber when the track order changes. | The original change was walked in the window. Its follow-up passed 632 Stack model checks in MSVC Debug. | Landed |

| SC-191 | [basics/sc-191-waveform-source-offset](https://github.com/arieladi/Adi/tree/basics/sc-191-waveform-source-offset) `1730f7b3` | Split pieces and edited Starts draw their own sample regions with Warp on and off. Engine snapshot and window waveform code/tests. | Test-only run reproduced the waveform offsets. Linux Debug with JUCE, Pd and warnings as errors passed the seven affected suites, the peerless window suites and all three acceptance tests. The app smoke rendered under Xvfb with an isolated ADI_HOME. | PR requested |

## For the director

