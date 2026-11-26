# Basics clips

Queue claimed. SC-191 is first, per the round 2 ordering.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| DF-W56 (B13) | [#795](https://github.com/arieladi/Adi/pull/795), merge `302b3239` | Default track names renumber when the track order changes. | The original change was walked in the window. Its follow-up passed 632 Stack model checks in MSVC Debug. | Landed |
| SC-191 | [basics/sc-191-waveform-source-offset](https://github.com/arieladi/Adi/tree/basics/sc-191-waveform-source-offset) `1730f7b3` | Split pieces and edited Starts draw their own sample regions with Warp on and off. Engine snapshot and window waveform code/tests. | Test-only run reproduced the waveform offsets. Linux Debug with JUCE, Pd and warnings as errors passed the seven affected suites, the peerless window suites and all three acceptance tests. The app smoke rendered under Xvfb with an isolated ADI_HOME. | PR requested |
| SC-11 | [basics/sc-11-unwarped-tempo](https://github.com/arieladi/Adi/tree/basics/sc-11-unwarped-tempo) `0d7c0906` | Tempo edits preserve the authored physical duration of unwarped audio drops, recorded takes and trims, with exact geometry captured for Undo and Redo. Engine operation and playback tests. | Windows Debug test-only builds failed the faster/slower tempo cases. The fixed head passed nine Linux Debug suites with JUCE, Pd and warnings as errors, the peerless window suites, all three acceptance tests and the Xvfb app smoke. | PR requested |

## For the director

- **SC-196 / N53.** Where does Live place its crossfade when only one side has hidden source material, or when neither does? I recommend keeping the visible seam and limiting a linked crossfade to valid source on both sides. A PCM regression for the full-handles case is prepared.
- **SC-198.** Does Live audition audio outside a clip's placed Start and End when the Sample Editor scrub strip is clicked there? I recommend limiting the existing Arrangement playback action to the placed clip until that routing is measured.
