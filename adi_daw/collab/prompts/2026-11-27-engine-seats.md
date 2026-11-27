# Five engine seats — queue, 27 November 2026

**What this is.** The fixes of the basics are being merged and landed by other sessions. By the director's
word of 27 November, sessions with tokens to spare build ahead: the engine half of five large features, each
in files of its own, each on its own branch, so that nothing here collides with the fixes. Each lands later
as one large bundle, after the basics.

**The same text goes to every session.** Take one seat.

1. Fetch origin. The seats are below in their order: stretcher, session, pitch, library, server.
2. A seat is taken when the branch `seats/<name>-log` exists on origin. Take the first seat that has none:
   make `collab/seats/<name>.md` with one line saying you took it, and push it as `seats/<name>-log`
   before anything else. If the push is refused because another session was faster, take the next seat.
3. If all five are taken, say so to the director and stop.
4. When your seat's queue is done, or blocked on someone else, take the next free seat the same way.

**How every seat works.**
- Your code is one chain branch, `seats/<name>-chain`, from origin/main. Merge origin/main into it once a
  day. Your report is `collab/seats/<name>.md` on your log branch: one row per item (what changed, how it
  was proved, its state), pushed after each item.
- Build and test as the repository does: warnings as errors, `ctest`, the five validators in `tools/`. Every
  item has a test that fails before your change. Measure what you claim: a time, a level, an error in cents.
- **Engine only.** Nothing a user sees: the look of every surface is agreed with the director from a picture
  first (ADR-0243 d1). Do not touch `src/juce/ui_*`. Where a feature needs the window, end at a seam the
  window will call, with a test that drives the seam. Keep your code in files of your own; where you must
  change a shared file (the session, the graph, the op catalogue, `CMakeLists.txt`), keep the change small
  and say it in your report, because that is where merges hurt.
- The audio thread takes no lock and allocates nothing. No network on it, ever.
- A new op name, a schema minor or a register id is reserved first: say what you need under "For win" in
  your report and go on with the next item.
- ADI behaves as Ableton Live 12 unless Live lacks the ability (ADR-0203 d1). Where you need to know what
  Live does and cannot read it in its manual, leave a named constant and list the question under "Readings
  owed".
- A choice that is the director's goes under "For the director": plain words, lettered, your
  recommendation. Then go on with what does not depend on it.
- No other maker's code that is not open source, and no other maker's name on anything ADI ships.
- Use sub-agents if you have them. Work until your limit; stop only for a broken environment, and say which.

Read first, every seat: `docs/DECISIONS.md` by search (never whole), `docs/PLAN.md` for your rows and
their appendix cards (`#### <ID> `), `docs/SPEC.md` where your rows cite it, and the engine design notes
`docs/ENGINE-*.md` that touch your ground.

## Seat 1, `stretcher`: audio that follows the tempo while it plays

The plan's row TM-WT-04, with ADR-0061, ADR-0151 and ADR-0185 d7. Today a warped clip is stretched ahead of
time by a worker (SC-194) and cannot follow a tempo that changes under it. Build the streaming players
(Beats, Texture, Re-Pitch) and the primed real-time stretcher with its latency reported to the graph, so
that a warped clip follows tempo automation and a tempo typed while playing. It also carries, later, the
Browser's preview in time and Session View's clips. First proof: a loop at 120 follows a ramp to 140 with
its beats on the grid within a measured tolerance, with no allocation on the audio thread.

## Seat 2, `session`: Session View's engine

The plan's rows TM-SV-02 (the data: scenes, clip slots, launch settings), TM-SV-03 (the launch engine) and
TM-SV-04 (follow actions, scenes, capture), with ADR-0101, ADR-0117 and ADR-0203 d10. The window
(TM-SV-05) is not yours. Build the tables, the ops, and the engine that launches and stops slots and
scenes in time (Live's launch quantization and launch modes), one clip per track at a time, with recording
into a slot. Drive it by ops and tests, and by the command-line player if that helps. Until seat 1 lands,
a slot's audio plays at the song's tempo through the stretcher that exists. First proof: two scenes
launched one after the other switch on the bar, sample-exact, in a rendered file.

## Seat 3, `pitch`: the Pitch Editor's engine

Asked for by the director on 27 November: an editor for the notes inside a sung or played line of audio,
of the kind other DAWs have and Live lacks (an addition; row C1 of `docs/CLIP-VIEW-ADDITIONS.md` says what
it holds: segments, pitch and time per segment, straighten, tilt, formant, volume, pitch quantize). There
is no plan row yet; win writes it from your first report. No neural model inside ADI's process
(ADR-0203 d2): start from a signal method, and say what a model would add. Build, for one voice at a time: the analysis that
finds the notes (start, end, pitch curve, drift, vibrato) and keeps them with the clip's analysis; the
edits as ops (move a note in pitch, straighten its curve, quantize pitch to a scale, move and stretch a
note in time, change formant); and the rendering that plays the edited line, offline first. The convert-to-
MIDI row TM-WT-09 can share your note finder: say how. First proof: a sung scale analysed into its notes
within a measured error in cents and milliseconds, one note moved a semitone, the render's pitch measured.

## Seat 4, `library`: listening to the library

The ground under the Browser's filters and "find similar" on the sheet the director agreed (ADR-0261 d10
to d12, ADR-0262; the plan's rows TM-MD-04, TM-MD-07, TM-WT-01, TM-WT-05). For every sample, measured once
in the background and kept with its analysis: length, tempo, key, loop or one-shot (one classifier, the
same answer for the filter, for Warp and for the preview's looping), brightness, hit density, and the
descriptors that rank similar sounds; no model. Then the one query service of ADR-0261 d12 over that
index, outside the Browser's panel: words, the six filters, similar-to and fits. Say how long 200,000
files take, measure the shown list first, and let it be paused. The Browser session
(`basics/browser-chain`) is building the search by name now: read its report and build beside it, not
over it. First proof: in a fixture folder, kicks rank kicks ahead of hats, and each filter's answer is
checked against files whose values are known.

## Seat 5, `server`: plug-ins on another computer, the first study

DF-E92 in `docs/DEFERRED.md`, with ADR-0053, ADR-0083, ADR-0107 d5 and ADR-0261 d20 to d23; the plan's
row DEV-27. Nothing of this was ever started. Read AudioGridder's protocol and server (open source) and
write down, in `docs/ENGINE-PLUGIN-SERVER.md`: what the wire carries and what it cannot (CLAP's events,
per-note expression), what it guarantees for authentication and encryption, and whether ADI should vendor
its client or write one against the wire format, with your recommendation. Build the latency probe of
ADR-0107 d5 and measure a real round trip. Then the client's engine half behind the device contract
(`src/juce/device_model.hpp` needs no change for it): a remote device that plays one block behind and
reports it, never a socket on the audio thread, an offline render that waits for the server and fails by
name. First proof: a plug-in hosted by a server on the same machine plays through ADI's player and
matches the local one within a measured difference.
