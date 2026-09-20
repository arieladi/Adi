# win — log

Windows 11 desktop · MSVC 19.44 (VS 2022 Community) · x64 · Claude Opus 5.
Only the `win` agent writes to this file. Newest entry at the top.

---
## 2026-09-20 — this project left the monorepo; your clone path changes

**Read this before your next pull.** `adi-vst` is no longer `VST-ADI/` inside
`arieladi/Adi`. It is its own repository, its own directory, its own branches
(ADR-0014). If you pull the monorepo you will simply see `VST-ADI/` disappear —
that deletion is an ordinary commit, nothing was lost, and there is nothing for
you to recover.

Why, plainly: sharing one repo with `adi_daw` meant sharing one *working tree*.
A branch checkout swaps both projects' files, and one branch namespace means a
history operation lands on everyone. That cost real work rather than being a
theoretical risk — see ADR-0014 for the full account, including the part where I
destroyed unstaged `adi_daw` edits with a `reset --hard` that was aimed at a
branch I was not actually on. The separation is structural now because discipline
demonstrably was not enough.

History came with us. The six commits were extracted with `git filter-repo`, so
the ADR log and tooling keep their provenance; the ADR numbering is unbroken and
ADR-0001…0013 still read against the old `VST-ADI/` paths, deliberately, because
the log is append-only and those paths were correct when written.

**What this means for you.** Everything in `collab/README.md` about cloning the
monorepo is now wrong and has been rewritten. The two repos are:

```
adi-vst/            this project (standalone)
└── vital/          the Vital fork, still its own repo, still gitignored
```

`vital/` moved across unchanged and still has only an `upstream` remote, so
ADR-0011 is still open and you still cannot see the C++.

**Also since you last looked** — the parameter contract changed and it matters
for `backend/**`:

- The model-facing schema is **452 parameters, not 474**. 22 entries in Vital's
  `ValueDetails` table are never registered by the engine
  (`synth_plugin.cpp:28-30` skips them), so setting them validates, applies, and
  does nothing. All 22 are legacy migration artifacts: 13 pre-1.0 filter
  input-routing toggles, 9 sub-oscillator parameters, and one literally named
  `compressor_low_band_unused`. ADR-0012.
- The list is **derived, never hardcoded** — `tools/find_phantom_params.py`
  reads it out of a running plugin via a new `--dump-params` flag on the
  validator. If you touch anything under `vital/src/common/`, regenerate the
  chain rather than trusting the committed JSON.
- **Do not disable `JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS`.** I had
  suggested it earlier as a way to shrink the 2852-parameter list. That was
  wrong and ADR-0013 withdraws it: VST3 has no native MIDI-CC input path, so
  that emulation *is* the CC path, and disabling it silently breaks MPE while
  leaving notes working. The dumped list confirms the block is per-channel and
  runs to `|129`, i.e. CC 0–127 plus aftertouch and **pitch bend**.

One thing I got wrong that the tooling caught rather than me: the first run of
the phantom oracle reported 54, including every random-LFO parameter. That was a
hardcoded display-name prefix — `kRandomNamePrefix` is `"Random LFO"`, not
`"Random"`. Prefixes are read from source now, and the tool exits non-zero on any
unmapped host name, because without that check we would have shipped a
420-parameter contract and not noticed.

## 2026-09-18 — bootstrap, and what mac can pick up

Set up `collab/` and `docs/DECISIONS.md` on `main` as a one-off bootstrap (no
branch — there was nothing to conflict with). Claims row is in the README and
comes out when this lands.

**Everything below happened before you joined**, so rather than make you read
back through it: here is the state, what is verified versus merely believed, and
where I think you can do things I cannot.

### What exists and is verified

The toolchain works end to end on Windows. Unmodified Vital builds as both a
standalone app and a VST3, and both were checked rather than assumed:

- **Standalone** — builds Release x64, runs, and Adi confirmed clean audio from
  the on-screen keyboard.
- **VST3** — `VitalValidator` reports **15/15**: scans as
  `Vial / Vial Audio / Instrument|Synth`, instantiates, is silent before
  note-on, renders peak 0.357 / rms 0.144 on note 60 with no NaN/Inf, and
  round-trips 232,438 bytes of state.

That validator is `tools/validator` (ADR-0009), built with CMake against the
stock JUCE download. It is the regression gate — run it after any change to the
plugin. It matters more than it looks: the state round-trip exercises exactly
the `stateToJson`/`jsonToState` path the whole AI feature depends on.

`tools/extract_schema.py` reads Vital's parameter tables straight out of the C++
and emits the schema — **794 parameters, 0 unresolved**. Not hand-maintained,
deliberately: a third of the table entries are constant *expressions*
(`kMaxPolyphony - 1`, `factorial(kNumEffects) - 1`) that only resolve by
scanning the tree for constexprs and enum members.

### Four traps, all now written down

Getting the VST3 out meant fixing things that were already broken upstream and
would have bitten you identically on macOS. ADR-0005 through ADR-0007 have the
detail; the shape of them is what I would keep in mind:

- defines that are **dead** (`REQUIRE_AUTH`, referenced nowhere in `src/`),
- paths to SDKs that **are not in the repo** (`vst3Folder`, `vstLegacyFolder`),
- a JUCE default that pulls VST2 headers into the **VST3** wrapper,
- and a post-build step that failed the build *after a successful link* because
  it wanted admin rights.

The last one is the one to watch for. A green compile and link followed by
`Build FAILED` is not a code problem.

I also lost a run to my own test: `getTotalNumOutputChannels()` reports **0**
until `enableAllBuses()` is called on a fresh VST3 instance, so the validator
reported a false stereo-output failure. Mine, not the plugin's.

### What I think you should take, and one thing you cannot yet

Highest value first, on the assumption you want something you can start on
immediately:

1. **`backend/**` — the FastAPI server and the patch validator.** Fully
   portable, blocked on nothing, and it is on the critical path. ADR-0008 fixes
   the contract: the model emits a flat sparse patch (`params`, `modulations`,
   `remove_modulations`, `lfos`, `preset_name`), never raw `.vital`. The
   validator rules are in `ARCHITECTURE.md` §4.3 — including a real off-by-one
   in Vital's own table (`destination` max is 14, but `kDestinationNames` has 14
   entries, so 14 is out of bounds; clamp to 13). `tools/out/vital_schema_llm.json`
   is the input: 474 parameters with min/max/scale/enum labels resolved.

2. **CI.** There is none. A workflow that runs `extract_schema.py` and asserts
   794/0-unresolved would catch parameter drift the moment either of us touches
   `vital/src/common/`.

3. **macOS build of the fork — blocked, see below.**

**The blocker:** `VST-ADI/vital` has only an `upstream` remote. There is nowhere
to push it, so you cannot clone it. Until that is resolved (ADR-0011, open) the
C++ is invisible to you and the macOS build cannot start. I have deliberately
not decided this unilaterally — vendoring it into the monorepo would cost ~180 MB
permanently and lose `git diff upstream/main`, and it is Adi's GitHub account, so
creating a repo is his call.

### What I am doing next

`PromptSection` — the overlay, text editor and Undo button, no network
(ADR-0010). I will claim `vital/src/interface/**` when I branch. If you get the
fork before I finish, the thing I would most want checked is whether an
`OpenGlTextEditor` inside an `Overlay` behaves on macOS's GL path, because
Vital's entire GUI is OpenGL-rendered and I have no way to test that here.

One thing I would rather you pushed back on than accepted: I have assumed the
merge in ADR-0008 belongs in C++. It could equally live in the backend — send
the current state up, merge server-side, return a complete preset. That would
make the C++ trivial and the merge testable in Python, at the cost of shipping
~230 KB of wavetable base64 over the wire on every request. I chose C++ to keep
the request small, but if the backend is yours I would rather you had a view.
