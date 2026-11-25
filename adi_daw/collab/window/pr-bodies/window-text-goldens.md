Goldens that check text by its place, not its pixels: the recorder and the masked comparison (WTS-03, first half; ADR-0232 d3).

What it does (`tests/ui_golden_text.hpp`)
- **`GlyphBoxRecorder`:** renders as JUCE's software renderer does and keeps each run of glyphs' box in the image's pixels, through origins, transforms, saved states and transparency layers. The glyph bounds are the typeface's own.
- **`compareMasked`:** two renders compared pixel for pixel outside the union of both renders' text boxes (EXACT, as memcmp), so a word moved by a pixel passes while a divider moved by one fails.
- **`diffImage`:** what differs, red outside the mask and amber inside, over the golden greyed.
- **Text beside a golden:** the boxes as one "x y w h" a line, read back.
- **Not yet:** the font experiment (one pinned Latin and Hebrew face, each scene recorded per OS) needs CI's three legs. The shell's hand-placed masks and the eight goldens move onto this after it.

Tests
- `adi_ui_shell_tests` `textGoldens`:
  - the word's box is where it was drawn;
  - AT-2: a word moved 1 px passes, with what changed inside the mask, and a divider moved 1 px fails with its two columns counted;
  - the diff draws the divider in red;
  - the boxes read back from their text;
  - under a 2x zoom the box is in the image's pixels.
- Pd on, JUCE 9.0.5, the gate's own build of `b88bd09a` (main merged after #739): shell 3163 checks, seams 580, document 337, window_state 177, 0 failures; the full build is clean.
- Pd off as `ui.yml` builds it (`-DADI_WITH_PD=OFF -DADI_WERROR=ON`): adi_app and the six UI suites, 6 of 6 passed.
- The shadow and clang warning sweeps flag only main's lines (`tests/test_ui_shell.cpp` 6352, 13433, 17096); `ui_golden_text.hpp` is clean through the shell's test.
- **For win:** AT-1 and AT-24's definitions; this lane finds only ADR-0232 d9's line.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_013uiiLKvKvN8fsTi5qRHzxp
