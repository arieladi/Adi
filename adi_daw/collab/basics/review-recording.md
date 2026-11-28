# Review, with proof: recording

The stream reviews recording: audio and MIDI takes, the recorder's ring and its writer, take files,
punch and loop recording, the waveform drawn while recording, and what Undo does to a take. A finding
counts only when a headless test reproduces it.

- Code: `basics/review-recording-chain` (one chain, every finding a failing test and then its fix).
- Base: `origin/land/basics-1128c` until it is an ancestor of origin/main, then origin/main.
- Checks: the build PC's slot 10 (MSVC Debug, warnings as errors). Linux and macOS have not run this chain.

## Findings

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|

## Suspected, not proved

Nothing yet.

## For the director

Nothing yet.

## For the next session

Round 1 started on 28 November 2026. Nothing is done yet.
