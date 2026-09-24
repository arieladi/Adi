# Next missions — paste each into its own session. They can run at the same time.

Claims and ADR numbers are on main once PR #107 merges (win merges it). win is doing the plug-in panel
as project state (ADR-0154), then the engine's sample-rate audit (ADR-0157 d3).

---

## Codex (linux)

```text
You are agent linux on arieladi/Adi, project adi_daw. Two parts, one or two PRs, branch
linux/midi-clips. ADR-0155 is reserved for you. You may use Gemini CLI with full access as a tool; you
review everything it writes, and you commit. Don't stop to ask; decide, and write it down.

BEFORE YOU START
git checkout main && git pull. Read collab/README.md (claims from PR #107), the top of collab/win.md,
your ADR-0151, ADR-0157 (sample rates, new), ADR-0054 (the 500 Hz rule), ADR-0091 (event flow), and
the note rows (rows::Note, decoded from the clip's ANOT blob; SPEC section 6.3).

YOUR FILES
src/adi/engine/clip_* (except the clip worker's media-open call, which cloud changes for its decoder:
don't restructure that one function), src/adi/engine/transport.*, new src/adi/engine/midi_* files,
tests/test_clip_playback.cpp, tests/test_midi_clips.cpp (new). Lent for small logged hooks:
src/adi/engine/session.*, graph.*, realize.*. CMakeLists.txt is shared: your own targets only.
NOT YOURS: src/adi/store*, src/adi/ops_catalog.cpp, docs/format/**, src/adi/panel.* (win);
src/adi/audio/decode*, src/adi/settings/** (cloud); src/juce/** (mac).

PART 1 -- SAMPLE RATES TO 768 kHz (ADR-0157 d2; the director's ruling)
Clip playback refuses a session or source rate above 192 kHz, and reads ahead 16,384 frames whatever the
rate: 341 ms at 48 kHz, 43 ms at 384 kHz. Sessions run from 44.1 to 768 kHz and nothing lower (the
director's floor). Sources may be any rate up to 768 kHz: a 22.05 kHz sample still plays, converted
up. Make read-ahead a time, at least the 341 ms it gives at 48 kHz, at every rate, with pages sized to
match. Tests at 192 and 384 kHz: placement, conversion (22.05, 44.1 and 96 kHz sources), locate, loop;
a session rate below 44.1 kHz is refused with a clear error.

PART 2 -- MIDI CLIPS PLAY INTO INSTRUMENTS
A MIDI clip's notes reach the track's instrument, sample-accurately, through the event stream.
- Note on and off at their positions through the tempo map; velocity, channel, and per-note tuning
  cents where the event model carries it. Say which note fields you don't honour yet, and why.
- No hanging notes, ever: a locate, a stop, a loop wrap, a muted clip, a clip end and a clip deleted
  while playing each end the notes it started. Overlapping clips on one track both play.
- Clip loop and content offset, as for audio clips. The 500 Hz sub-block floor still holds (ADR-0054).
- The audio thread never reads SQLite or allocates: notes are prepared off it, like clip pages.
- Tests headless with a fake instrument that records events: exact frames across block sizes 64 to
  4096 and rates 44.1 to 192 kHz; every no-hanging-notes case above; TSan clean.
- Plants, each failing first: note-off lost at locate; note-off lost at loop wrap; placement off by
  one block; an allocation on the audio thread (instrument it); tuning cents dropped where it's claimed.

MSVC /WX RULES (win builds MSVC /WX after each merge)
No bare std::getenv (use the envVar helper); std::setvbuf, never setbuf; no fopen/strcpy/sprintf family;
watch size_t-to-int narrowing and signed/unsigned compares. Check counts must not depend on the
platform: tools/test_all.sh compares the total with the README.

DONE MEANS
tools/test_all.sh green with sanitizers as usual, validators clean, README counts updated, plants
recorded, log in collab/linux.md. Verify CI green by head SHA, release your claims row, then merge your
own PR. Report the PR number(s), the checks count, the high-rate results, the plants, and anything you
couldn't do.
```

---

## Claude cloud session

```text
You are agent cloud on arieladi/Adi, project adi_daw. Two missions, in order, one PR each, branch
cloud/decoder then cloud/settings-catalogue. ADR-0156 is reserved for you. You're in a Linux container:
build and test the Linux legs, not MSVC or JUCE. Don't stop to ask; decide, and write it down.

BEFORE YOU START
Pull main (claims from PR #107). Read collab/README.md, the top of collab/win.md, ADR-0132 (d3-d4:
one decoder, decoded into the cache), ADR-0149 (application data: appdata::pathsFor(app).cache),
ADR-0151 (clip playback, and how its worker opens media), ADR-0157 (sample rates to 768 kHz), your
ADR-0152, and docs/SETTINGS-CATALOGUE.md (new: the Settings Reference's 207 settings).

YOUR FILES
Mission A: src/adi/audio/decode* (new), tests/test_decode.cpp (new), the ONE call site in the clip
worker where it opens a media file (linux owns the rest of clip_*), and one dependency line each in
tools/fetch_external.sh and docs/EXTERNAL-CODE.md per library (mac's area, allowed on the director's
instruction; say so in your log). Mission B: src/adi/settings/**, tests/test_settings.cpp,
docs/SETTINGS.md. CMakeLists.txt is shared: your own targets only.
NOT YOURS: the rest of src/adi/engine/** (linux); src/adi/store*, src/adi/panel.*, docs/format/**
(win); src/juce/** (mac).

MISSION A -- ONE DECODER, DECODED INTO THE CACHE (ADR-0132 d3-d4)
Non-WAV clips are named silence today. Make them play.
- Decode FLAC, AIFF/AIFC, MP3 and Ogg Vorbis, plus any WAV the WAV reader refuses. Decoding happens
  off the audio thread, into the shared cache (appdata cache folder), as a file the existing
  streaming path can read. The cached file is keyed by the SOURCE file's BLAKE3, so a second project
  using the same file decodes nothing. The cache obeys the Decoding Cache settings (maximum size,
  minimum free space): least recently used goes first, never a file a live session is reading.
- Libraries under OPEN_SOURCE_POLICY.md. Candidates: dr_flac and dr_mp3 (public domain or MIT-0),
  stb_vorbis (MIT or public domain), or libsndfile (LGPL, which escalates to GPLv3 as adi_daw
  already is). Record the choice and why in ADR-0156.
- Any rate the file carries up to 768 kHz, lower-rate files included (ADR-0157); the clip path
  converts it.
- The clip worker's one media-open call asks the decoder for a playable file. That is your only
  change in linux's code.
- Tests: fixtures you generate, small and committed with their provenance. FLAC decodes bit-exact to
  its source PCM; MP3 and Vorbis above a stated SNR against their source; AIFF big-endian correct.
  Cache hit on the second open, eviction order, a corrupt file refused with a named problem.
- Plants, each failing first: decoding on the audio thread (instrument it); a cache keyed by path
  instead of hash; eviction of a file in use; FLAC off by one sample; the size limit ignored.

MISSION B -- THE SETTINGS REGISTRY FROM THE CATALOGUE
docs/SETTINGS-CATALOGUE.md lists all 207 rows of the Settings Reference. Your registry holds 40. Fill it:
every row that is a setting ADI has gets an entry (type, default, legal values, scope, page, help text
from the ADI column). Rows that are REJECTED, NOTE, "not a setting" or BACKLOG are listed as
deliberately absent, with the reason. A test proves every catalogue row is in exactly one of the two
lists, so the catalogue and the registry cannot drift. The sample-rate chooser offers ADR-0157's
ladder, 44.1/48, 88.2/96, 176.4/192, 352.8/384, 705.6/768 kHz, and nothing below 44.1 kHz.

MSVC /WX RULES (win builds them after each merge)
No bare std::getenv (use the envVar helper); std::setvbuf, never setbuf; file streams, not fopen;
explicit size conversions. No platform-only checks: tools/test_all.sh compares the total with README.

DONE MEANS, PER MISSION
tools/test_all.sh green, validators clean, README counts updated, plants recorded, log in
collab/cloud.md. Verify CI green by head SHA (PR CI skips silently when mergeable is UNKNOWN: use
workflow_dispatch then). Release your claims row, then merge your own PR. Report the PR numbers, the
checks count, the libraries and why, the plants, and anything you couldn't do.
```
