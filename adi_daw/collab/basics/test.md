# The director’s test

This session owns the director’s test queue.

The chain started from `origin/basics/integrate-2`. Linux window suites use Debug with warnings as errors and DISPLAY unset.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| SC-433 | `basics/test-chain` at `722582fc8` | Keep hover instructions and keys inside Info View. Window and Control Bar seats. | The peerless checks show only an icon title in the tooltip and full hover text in Info View. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-439, SC-175 | `basics/test-chain` at `82af8bf4c` | Keep a held lane click as an insert marker and draw its flag. Arrangement seat. | Peerless replay covered stationary repeats, genuine drags, the status label and marker pixels. Debug Werror, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |
| SC-441, SC-445 | `basics/test-chain` at `f06b67887` | Play on from at or after Loop End. Ordinary stereo metronome playback was audible in the synthetic device fixture. Engine, recording and project seats. | The transport and document rehearsals reproduced the Loop fault and verified forward clip playback, click audio and a linear take. Debug Werror, 4 targeted suites, 18 no-display UI suites and 3 acceptance suites passed. | Pushed |

## Measurements and findings

### SC-433 · Hover text

The Arrangement previously sent its full description to both JUCE’s hovering tooltip and Info View. The tooltip now skips the Arrangement, value fields, and other components unless they are explicitly marked as icon controls. Play, Stop, Arrangement Record, Follow, Metronome, Loop, Info View and Detail View can still show their title alone. The same policy runs in the main and detached windows, with the existing user preference and hover delay. Info View retains the full phrases and keys, including Play’s Space hint. The component bounds and Info placement are unchanged.

The shell fixture checks the tooltip policy and the independent Info text without displaying a tooltip window.

### SC-439 and SC-175 · Arrangement insert marker

The starting branch includes the clip-body drag anchor from `afdf57e55` and marker-only primary click from `2d7a5181c`. A stationary press still enlarged the span when JUCE dispatched its held-drag repeat. At the fixture’s interior x=10 the anchor was 11,531,520 ticks and the synthetic repeat extended it to 17,297,280 ticks, one quarter note. The range now stays zero until the pointer moves.

The blue marker still uses a 1 px stem in the selected lane. A 7 × 5 px flag is centered over the stem at the lane top. The peerless fixture exercises repeat events with and without JUCE’s drag bit, the flag and stem pixels, an empty-lane click, the Insert Mark status, and real range drags in both directions.

At 1440 × 900 the focused clip-body fixture measures the stem at canvas-local `(9,0,1,86)` before and after. The flag occupies `(6,0,7,5)`.

The first integrated run surfaced two legacy fixture gestures that sent a MouseDown-shaped event into MouseDrag. Both now pass their actual initial and final pointer positions through the drag helper. The same checks still require the range to span three tracks.

### SC-441 / SC-445 Metronome and Loop

The director's follow-up on main (`500e1da66`) identified the empty four-bar brace behind the Loop symptom and set the expected behavior. At 48 kHz and 120 BPM, the saved brace covers samples 0–384,000. Starting on a clip at sample 408,000 formerly rendered from 24,000 and advanced to 24,256. The new engine renders 408,000–408,255 and advances to 408,256 with the saved switch still on. Play before the brace runs into it and wraps at End. Compensated audio and MIDI input follow that same clock. A recording beyond the brace is linear.

On Linux, the original engine produced an audible click on both channels of a synthetic physical 1/2 device after ordinary Play with Loop off, and clicked on the next grid beat after an off-grid locate. The same session reproduced the separate loss of the actual imported clip when Loop was enabled. The changes fix the platform-independent Loop fault and preserve the click. Existing SC-431 follows the explicitly selected physical Cue Out and reports an unavailable pair. Ordinary-click silence on the director's actual CoreAudio device and project remains unverified.

If the CoreAudio symptom persists, the missing reproduction data are the Cue Out pair, Cue Volume, and Click While Recording setting in the director’s project. The synthetic result supports keeping the selected Cue route while checking those settings.

The existing SC-27 shell fixture now locates explicitly inside a one-bar brace. It continues to check both refusal with MIDI armed and audio admission within the loop. The new document rehearsal covers the complementary start beyond the brace.

## For the director

SC-441. Retest ordinary Play on the Mac with the same project and device. If the click is silent, record the Cue Out pair, Cue Volume and Click While Recording setting. Keep the selected Cue route while checking those settings.
