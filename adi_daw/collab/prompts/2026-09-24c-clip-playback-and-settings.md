# Next missions — paste each into its own session. They can run at the same time.

Claims and ADR numbers are on main once PR #103 merges (win merges it). win is doing the journal's
in-transaction hook and BLAKE3's AVX2, AVX-512 and NEON paths (ADR-0153).

---

## Codex (linux)

```text
You are agent linux on arieladi/Adi, project adi_daw. Mission: audio clips play through the session,
streamed from disk. Branch linux/clip-playback. ADR-0151 is reserved for you. You may use Gemini CLI
with full access as a tool; you review everything it writes, and you commit. Don't stop to ask;
decide, and write it down.

BEFORE YOU START
git checkout main && git pull. Read collab/README.md (claims from PR #103), the top of collab/win.md,
ADR-0010, ADR-0042, ADR-0102, ADR-0122 (the session), ADR-0132 d6-d7 (no warp, no fade by default),
SPEC section 4 (time), and src/adi/engine/{session,snapshot,graph,realize}.hpp.

YOUR FILES
src/adi/engine/clip_* and src/adi/engine/transport.* (new, inside win's engine area),
src/adi/audio/**, tests/test_clip_playback.cpp (new). Lent for the hooks you need, kept small and
logged: src/adi/engine/session.*, src/adi/engine/graph.*, src/adi/engine/realize.*.
CMakeLists.txt is shared: your own targets only. A new third-party library means one line each in
tools/fetch_external.sh and docs/EXTERNAL-CODE.md (mac's area, allowed on the director's
instruction; say so in your log), and its licence must be one OPEN_SOURCE_POLICY.md allows.
NOT YOURS: src/adi/ops.*, src/adi/changeset.*, the BLAKE3 lines of CMakeLists.txt (win);
src/adi/settings/** (cloud); src/juce/** (mac).

THE GAP
The engine has no transport: there is no playhead that advances per block, locates or loops, and
nothing reads an audio clip's media. A project's audio clips are silent. Build that, headless.

1. A transport: playing or stopped, the playhead in samples, locate, and a loop range. It is advanced
   once per block by whoever drives the session, and read by nodes without locks. Clip positions are
   ticks or nanoseconds (SPEC section 4); ticks go through the tempo map (TempoMap::ticksToSeconds).
   Placement is sample-accurate across block boundaries and block sizes 64 to 4096.
2. Disk streaming. The audio thread never opens or reads a file: extend ADR-0010 from SQLite to all
   file I/O. A streaming thread fills per-clip buffers ahead of the playhead, and refills on locate
   and loop wrap. A buffer that is not ready is silence and a counter, never a wait.
3. A clip source per audio track. It mixes the track's audio clips at their positions, honouring
   src start and length, gain, mute, fades as the row states them (default none, ADR-0132 d6), clip
   loop, and channel mode. Feed it to the session through setSourcesFor, or a small hook you add to
   session.* and log.
4. Sample rate. A file whose rate differs from the session's is converted. Choose the resampler
   under the policy (candidates: r8brain-free-src, MIT; libsamplerate, BSD-2) and record the choice
   and its measured quality in ADR-0151.
5. Not yet built, and must not pretend: a warped clip, a reversed clip, and a non-WAV file. Decide
   whether each plays raw or silent, report it in the session's problems() either way, and write
   the decision down.
- Tests: headless renders of projects with clips. Exact sample positions across block sizes; gain,
  mute, fades and loop; locate and loop wrap mid-clip; a stalled streaming thread (silence, counter,
  no block); conversion accuracy on a sine; TSan clean.
- Plants, each failing first: file I/O on the audio thread (instrument it); placement off by one
  block; the buffer not refilled after locate; an underrun that blocks; conversion skipped.

MSVC /WX RULES (win builds MSVC /WX after each merge)
No bare std::getenv (use the envVar helper); std::setvbuf, never setbuf; no fopen/strcpy/sprintf
family; watch size_t-to-int narrowing and signed/unsigned compares. Check counts must not depend on
the platform: tools/test_all.sh compares the total with the README.

DONE MEANS
tools/test_all.sh green with sanitizers as usual, validators clean, README counts updated, plants
recorded, log in collab/linux.md. Verify CI green by head SHA, release your claims row, then merge
your own PR. Report the PR number, the checks count, the resampler and its numbers, the plants, and
anything you couldn't do.
```

---

## Claude cloud session

```text
You are agent cloud on arieladi/Adi, project adi_daw. One mission, one PR: the settings store.
Branch cloud/settings. ADR-0152 is reserved for you. You're in a Linux container: build and test the
Linux legs, not MSVC or JUCE. Don't stop to ask; decide, and write it down.

BEFORE YOU START
Pull main (claims from PR #103). Read collab/README.md, the top of collab/win.md, ADR-0125 (R-01 to
R-07 and d1-d3), ADR-0127 d5 (settings bundles), ADR-0145 (the new settings), ADR-0149 (where
application data lives: src/adi/appdata.*), and the Settings Reference text in ADR-0125's rows.

YOUR FILES
src/adi/settings/** (new), tests/test_settings.cpp (new), docs/SETTINGS.md (new).
CMakeLists.txt is shared: your own targets only. src/adi/appdata.* and src/adi/media/zip_writer.*
are read-only for you.
NOT YOURS: src/adi/ops.*, src/adi/changeset.* (win); src/adi/engine/**, src/adi/audio/** (linux);
src/juce/** (mac).

THE MISSION
The settings every page of the Settings window will read and write, as a core library. No UI.
- A typed registry of settings: key, type, default, scope (App or Project, R-03), page, and the help
  text the Find box searches (R-02). Project-scope settings are already rows in the .adi, changed by
  ops; the registry names the op that sets each one and never writes the file itself.
- App-scope values in one JSON file per application, at appdata::pathsFor(app).config /
  settings.json, with a schema version. ADI DAW, ADI Live and aDiJ never read each other's
  (ADR-0145 d10). Unknown keys from a newer build survive a save. A corrupt file falls back to
  defaults and is kept aside, never overwritten silently.
- Presets, whole and partial (R-04): a named subset of pages; applying one leaves the rest alone.
- Settings bundles (R-05, ADR-0127 d5): one .zip of the JSON files, paths stored as roles
  ("user library", "content folder 2") and resolved again on import.
- The agent's settings pipeline (ADR-0125 d1-d3): a whitelist of UI and workflow settings the agent
  may change, everything else refused; every agent change written to a settings-change log; no undo.
- Enter ADR-0145's new settings in the registry: Zoom on Selection, silent resampling, buffer sizes
  64 to 4096 (no 32), ASIO through JUCE only, the Linux backend and JACK transport sync.
- Plants, each failing first: one application reading another's file; an unknown key dropped on
  save; a partial preset touching a page it doesn't hold; an absolute path left in a bundle; the
  agent changing a setting outside its whitelist.

MSVC /WX RULES (win builds them after each merge)
No bare std::getenv (use the envVar helper, as appdata.cpp does); std::setvbuf, never setbuf; file
streams, not fopen; explicit size conversions. Check counts must not depend on the platform:
tools/test_all.sh compares the total with the README, so no platform-only checks.

DONE MEANS
tools/test_all.sh green, validators clean, README counts updated, plants recorded, log in
collab/cloud.md. Verify CI green by head SHA (PR CI skips silently when mergeable is UNKNOWN: use
workflow_dispatch then). Release your claims row, then merge your own PR. Report the PR number, the
checks count, the plants, and anything you couldn't do.
```
