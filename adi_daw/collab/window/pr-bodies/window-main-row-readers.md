A take lane's clips stay off the Arrangement's main row: a seams check (DF-E35a, the window's half; queue item 9).

What it does
- The engine already keeps a take lane's clips apart in the snapshot (`TrackNode::takeClips`, `430ea825`, with #677's lane id in `ClipNode`), and every Arrangement reader (the hit test, the painters, the song's end) walks `TrackNode::clips`. So the window needs no change; this check pins it, so a reader that walked the take clips would fail here.
- `tests/test_ui_seams.cpp` `takeClipsStayOffTheMainRow`: a project with clip 1 on the main row and clips 2 and 3 on take lane 7 (clip 3 at quarter 64), read through `ProjectView` and the `SnapshotReader`:
  - the main row holds clip 1 only, the one with no lane; clips 2 and 3 are the take lane's, apart;
  - a click at 480 on the track hits clip 1, not the take under it;
  - nothing is hit where only a take lies;
  - the song's end ignores a take further right.
- When the take lanes' own view comes (CT-04), it draws `takeClips` in its lanes; the main row stays as it is.

Tests
- GCC 13, Debug, `-DADI_WERROR=ON`, JUCE 9.0.5 (main's pin), the gate's own build of `b93f43c5`: seams 587, shell 3157, document 337, window_state 177, 0 failures. The full build is clean.
- Pd off as `ui.yml` builds it: adi_app and the six UI suites, 6 of 6 passed.
- The shadow and clang sweeps flag only main's lines (`tests/test_ui_shell.cpp` 6351, 13432, 17051).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
