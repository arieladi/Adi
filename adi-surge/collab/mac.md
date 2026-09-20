# mac — log

macOS · Apple clang / arm64 · Claude Opus 5.
Only the `mac` agent writes to this file. Newest entry at the top.

---

## 2026-09-20 — bootstrap, and what win can pick up

Adi's call: a **second** AI synth, on Surge XT, shipping a CLAP, alongside
`adi-vst`'s Vital/VST3. This is the setup commit. Branch
`mac/adi-surge-bootstrap`, claims row is in the README and comes out when this
lands.

Nothing has been **built** yet, on either platform. Everything below is read out
of source, and every number has the command that produced it written next to it
in `ARCHITECTURE.md`. Treat "not built" as the honest status, not modesty.

### What exists

- `adi-surge/surge/` — full clone of `surge-synthesizer/surge`, remote named
  `upstream`, at `58914e59c` on `main`, **Surge XT 1.4.0**, GPLv3. All 22
  submodules initialised, 27 entries including nested, **zero drift**.
  1.5 GB on disk. Gitignored by the monorepo (ADR-0002).
- `tools/fetch_surge.sh` — clone + submodule init, with both traps absorbed.
- `ARCHITECTURE.md`, `docs/DECISIONS.md` (ADR-0001…0006), `collab/`.

### The two traps, because they will hit you identically on Windows

1. **`git submodule update --init --recursive` fails** with
   `fatal: transport 'file' not allowed`, despite every URL in `.gitmodules`
   being `https`. Git ≥ 2.38's CVE-2022-39253 mitigation; git's submodule clone
   path tries the superproject's object store first. Needs
   `-c protocol.file.allow=always` **scoped to that one command**. Do not set it
   globally. Just run `tools/fetch_surge.sh` and it is handled.
2. **`.gitmodules` declares 23 submodules but only 22 are gitlinks.**
   `src/surge-rs/surge-rs` is a stale declaration with nothing behind it. Not a
   broken checkout — upstream's state, and nothing in CMake references it.

### Why this synth is a better substrate than Vital, with evidence

This is the part worth your time, because it changes what we should build rather
than just where:

| | Vital / `adi-vst` | Surge XT / here |
|---|---|---|
| CLAP | unreachable; adi-vst ADR-0017 dropped it | `surge-xt_CLAP` is a CMake target, `src/surge-xt/CMakeLists.txt:66` |
| Build | Projucer, patched JUCE 6.0.5, no VS2022 exporter | CMake ≥3.15, **JUCE 8.0.12** |
| GUI | entirely OpenGL-composited; needs `OpenGl*` component variants | plain JUCE software rendering — `grep -rn OpenGLContext src/` returns **0** |
| New overlay | 2 new files + 4 edited + unity stub + 2 `.jucer` + Projucer resave | enum value + class + one `case` in `createOverlay` (`SurgeGUIEditor.h:456-481`) |
| Undo | none upstream; adi-vst is building its own | `Surge::GUI::UndoManager` ships, plus an `ACTION_HISTORY` overlay |
| Schema | hand-written Python that parses C++ tables | OSC `/doc` emits name/type/min/max **per parameter** at runtime (`OpenSoundControl.cpp:1825-1875`) |
| Headless | adi-vst had to write `VitalValidator` | `surge-testrunner` + `ctest -j 4`, and a `surge-xt-cli` with `--osc-in-port` |
| Threading | ours to get right: network thread → `callAsync` → `weak_ptr` guard | `oscRingBuf.push(oscToAudio(...))`, a lock-free queue to the audio thread, already shipped |

766 parameters (`11 + 16*(12+1) + 2*273 + 1`, `SurgeStorage.h:155-161`), 170
`ct_*` control types, `ff_revision = 30`. Vital is 772 real parameters — the two
synths are nearly the same size, which is a useful check on any AI design meant
to serve both.

### What I want you to do, and in what order

1. **Build `surge-xt_CLAP` on Windows and tell me what happened.** Commands are
   in `ARCHITECTURE.md` §2.3, taken from upstream's own
   `.github/workflows/build-pr.yml:129-132`. I cannot verify anything you do
   here and you cannot verify my macOS side, so both baselines have to be
   recorded rather than assumed.
2. **Run `ctest -j 4` and record the count.** That becomes our regression gate,
   the way `adi-vst` uses "15/15". Write the number in this log's counterpart.
3. **Load the built CLAP in a host and confirm it makes sound.** Reaper hosts
   CLAP natively. This is the thing `adi-vst` could not do at all.

Check as you go whether anything in `ARCHITECTURE.md` §2 is wrong — it is
written from source, not from a build, and a build is the only real test of it.
Specifically: I have **not** verified whether Surge has a post-build copy step
that needs admin rights, which is the exact trap that cost `adi-vst` four builds
on Windows (their ADR-0007) and four more on macOS. If you hit "Build FAILED"
after a clean compile and link, that is where to look first.

### What is still open, and where I would rather you pushed back than agreed

**ADR-0005 is reserved and unwritten: what the AI write path actually is.** Three
candidates, and I have deliberately not decided:

- **(a) A C++ overlay in the plugin**, like `adi-vst` §6. Ships the real product
  UX. Most work.
- **(b) Drive Surge over its existing OSC surface**, possibly with **zero** C++
  changes. `/param/<oscName>` sets any parameter by name,
  `parameterFromOSCName` is the reverse lookup, `/doc` dumps the schema, and it
  all lands on the audio thread through `oscRingBuf`. I do not yet know whether
  the OSC server is on by default, what the ports are, or how it behaves with
  several instances in one DAW — which is exactly what decides whether this is a
  product path or only a development one.
- **(c) `surgepy` offline**, for generating training data and proving the
  prompt→patch loop before any C++ exists.

My instinct is that (b) or (c) is a genuine stepping stone to (a) rather than
throwaway, because the patch representation and the prompt work are the same
either way and only the transport differs. But that is an instinct, and the
multi-instance question could kill it. **If you build first and find the OSC
settings, you will know more than I do — say so rather than deferring.**

One more, smaller: `surge/` has **no `origin`** (ADR-0006). That is fine right
now because our clone is byte-identical to upstream and you can fetch the same
tree from the public URL. It becomes blocking the moment either of us commits
inside `surge/`. Keep work in `adi-surge/tools/` until Adi creates the fork repo.
