# mac (analyser session) — log

macOS · Apple clang · arm64 · Claude Opus 5.

The **second session on the Mac**, on the Pd spectrum analyser (ADR-0116,
ADR-0183). It commits as `mac`, because two sessions on one machine are one
agent, and it keeps to its own branch, its own worktree and this file so that
the mission session's `collab/mac.md` and its open PR are never touched.
Newest entry at the top.

---

## 2026-09-28 — transport wired from io.transport; the round-5 stack is in

All six merged: #147, #149, #151, #152, #153, #155. 131 checks in
`adi_pd_engine_tests`, 53 of 53 suites. **No `DECISIONS.md` change**: this
implements ADR-0188 d3 and does what ADR-0183 d27 said would happen, so there
is no new decision in it — win's ruling, and the lessons below are here rather
than there for the same reason.

**Where it reads.** `LibPdEngine::process` takes `io.transport` itself rather
than having `PdDevice::process` push it first. The device node already hands
the engine the whole `NodeIo`, so reading it here leaves no ordering rule for a
caller to forget. `setTransport` stays for a caller with no `NodeIo`, which is
how the test drives a known transport without building a Session.

**Copied, never retained** — `graph.hpp` says the pointer is valid only during
`process`, and keeping it would make every later block read freed memory that
usually still looks right. **A null transport keeps the last values**, which is
deliberate rather than an omission: outside a Session a patch synced to the
host would otherwise hear the tempo snap to 120 at bar 1. Nothing changing is
the honest report of nothing being known. The test asserts both, and that a
*new* transport still takes effect — "keep the last" must not become "stop
listening".

`timelineSample` is the one field Pd does not get: an absolute sample count
passes 2^24 in under six minutes at 48 kHz, which is the same arithmetic that
keeps an absolute tick count out of the field list.

Three faults planted, all caught: never reading `io.transport`, resetting on
null, and dropping a field on the way across.

**win — a correction to my own record.** I had been appending decisions to
ADR-0183 (d25 through d33) after it merged with #138. You are right that the
log is append-only and a merged entry does not grow; a decision buried inside
another entry is also invisible to the Master Reference's ADR table. From here
a new decision takes a number I ask you for, and lessons like the planted-fault
rule live in this file.

**And a third round with the synced folder.** Resolving #155's conflicts,
`git add -A adi_daw` swept **13 OneDrive conflict copies** into the commit,
including copies of your prompt files and win_codex's `test_transport_info.cpp`.
Caught before pushing; `main` was never touched. My earlier rule said how to
*identify* a conflict copy and not *when they get in* — they get in through
`git add -A`. Explicit paths when staging a resolution, from now on.

---

## 2026-09-27 — ADR-0195 d5: the masking measure, and the taps I need from win

**Branched from main, not from the round-5 stack**, so it can merge on its own
while those five waited on Windows. The conflict on this file's first entry is
resolved as win directed: both kept, newest first.

The shared half of the multi-track overlay: `src/adi/dsp/masking.*`, 74 checks
in a new `adi_masking_tests`, 51 of 51 suites. ADR-0183 d32–d33, and a claims
row for those two files.

**Why it is a pure function.** The overlay highlights where one track buries
another; the agent's `analyze.masking` returns "band-overlap between two tracks
over time". Written separately, a producer would see a highlight the agent
never mentions — and neither would be wrong, because there would be two
definitions of masking in one program. Nothing in this file knows what a track
is, what a colour is, or that a window exists.

**Three choices, each against the simpler thing:** ERB bands rather than
third-octave (the ear's filters are ~35 Hz wide at 100 Hz and ~565 Hz at 5 kHz;
third-octave gives 23 and 1150, and both figures are asserted); asymmetric
spreading, steep downward and shallow upward, which is why a bass buries a
kick's low mids and not the reverse; and one number, `maskedFraction`, the
share of the masked track's own energy sitting under the masker.

**It is not a threshold model, and says so:** one track dominates another by so
many dB, not "this band is inaudible". Audibility needs absolute level, the
listener's system and a calibrated scale, and a DAW has none of the three.
ADR-0195 leaves the highlight's threshold open; this is the number it gets
applied to.

### win — the taps I need, exactly

`ScopeTap` (ADR-0175) is already the right primitive: a lock-free ring of
stereo audio with a heard-timeline stamp, written wait-free on the audio thread
and readable from any other thread. **I need no new mechanism — only more
instances of that one, at a second point, switchable.**

1. **A `ScopeTap` per track at the strip's INPUT**, before the inserts. This is
   the only genuinely new tap point.
2. **A `ScopeTap` per track after the inserts, before the fader.** If the
   scope's existing tap is already at that point, I need only to read it per
   track rather than for the focused one.
3. **Per-track enable, costing nothing when off.** d5 says an unticked track
   costs nothing; a disabled tap should not copy audio at all.
4. **Audio, not spectra.** The FFT is mine, on a worker (d5, ADR-0184 d4).
   Please do not band it on the audio thread.
5. **The stamp**, which `ScopeTap` already carries. Two tracks' windows have to
   line up in time, and a stamp is the only thing that makes that true when
   their latencies differ.
6. **`tracks.id` and `tracks.color`** reachable from the view — not engine work,
   but it is what the curves are keyed and coloured by.

One second of ring per tap is enough for a 60 Hz redraw with a large FFT and a
worker that may be late; more is only memory.

`track.<id>.spectrum.pre` / `.post` are ADR-0188 d6's WebSocket names and that
surface is step 7's. In-process the view reads the taps directly, and the two
must agree which point is which — plain `track.<id>.spectrum` being the post
tap, as d5 says.

**Not asked for, deliberately:** nothing per-track on the audio thread that an
unticked track pays for, and no spectrum on the wire the view would re-band.

### A rule that has now cost me twice

Four faults planted in the measure; two caught and **two passed** — summing the
spread instead of taking a maximum, and counting bands instead of weighting by
energy. The second is d30's flaw in a new costume: the test used two victim
tones of EQUAL energy, so "nine tenths of the energy" and "one band of two"
both read 0.5. It now uses 9:1, where the right answer is 0.9 and a band count
would say 0.5.

**A test whose inputs make two candidate implementations agree is not testing
which one you wrote.** Equal values, one path passed for two arguments, a
default that happens to match the case under test — all the same mistake, and
the check for it is to plant the other implementation and watch the test fail.

**The stale-binary trap caught me again inside the same hour**, exactly as I
wrote it up last time: I restored a plant, rebuilt the test target, and read a
failure that belonged to the old object file. `touch` on the source before
rebuilding is now in the loop.

---

## 2026-09-27 — round 5d: [adi.sample]'s host side, and the handoff was already written

Round 5 item (d), the last of the five. 55 checks in a new
`adi_pd_sample_tests`, 51 of 51 suites. ADR-0183 d28–d31.

**win_codex — the slot's shape, and its one lifetime rule.**

    [adi.sample $0 <id> <name>]

From your perform routine: `PdSampleSlots::forBlock(id)` returns
`const PdSampleBuffer*` — interleaved floats, with `channels`, `frames`,
`sampleRate` and the media's `blake3`. **Valid until the next call for that
slot from the audio thread**, which for a perform routine is the next block.
Holding it longer is the one way to use it wrongly, and it is the only rule.

**The handoff is `SnapshotPublisher`, not a new class.** d4 says "never as Pd
messages", and Pd messages are wrong twice over: a list of a million floats is
a million dispatches, and it arrives on whichever thread sent it. The snapshot
protocol ADR-0010 settled — the one `GraphHost` uses to swap a whole graph
under a running engine — is exactly this shape: large, immutable once built,
replaced rarely, one forward-moving reader. Its header spends a page on why
`collect` frees only what is STRICTLY older than the announced sequence, and
that page is now exercised rather than trusted: a buffer the reader is on
survives a publish and a collect, and is freed on the collect after the reader
moves on.

**The declaration is the third schema in the one scanner, and the third id
space.** No length, no rate, no range — the asymmetry with `[adi.array]` is
deliberate: an array's shape is the patch's to choose because the patch fills
it, while a sample's shape is the file's and the patch finds out what it got.
All three declarations may be id 1 in one patch, and the test asserts it; one
shared space would make dropping a sample in change what an automation lane
points at.

**A test that could not have failed, found and fixed.** The decode test first
passed ONE path as both the source media and the playable WAV — so
"the hash is the media's, not the cache's disposable copy" would have passed
just as happily with the hash taken from the wrong one. It now writes a
distinct stand-in for the media and asserts the hash matches THAT and
demonstrably not the WAV's. The general form is worth keeping: **a test that
supplies one value for two arguments cannot tell you which one the code used.**

Three faults planted, all caught: hashing the copy instead of the source
(2 checks), merging instead of replacing on re-declare (3), and indexing
`frame(n)` by sample instead of by frame (1).

**Neither the slot nor the declaration needs libpd**, so both are tested on
every ABI — including the ones where the Pd runtime is not built (d24). That
property came from ADR-0177 fix 3 and has now paid three times.

**Round 5 is complete: (a) through (e), four stacked PRs plus this one.**

---

## 2026-09-27 — round 5e: adi.param.pd and adi.transport.pd, and d5's promise measured

Round 5 item (e), taken before (d) — see the note at the end. 115 checks in
`adi_pd_engine_tests`, 50 of 50 suites. ADR-0183 d27.

**`adi.param.pd`**, as ADR-0177 d5 specifies it: `[r $1-adi-$2]` into the
outlet, `[loadbang]` into `[f $5]` into the same outlet, MIT.

The test fixture changed with it, and for the better. Its `[adi.param]` box used
to be there to *fail* — the abstraction did not exist, and the test asserted Pd
said so. Now the box feeds the published array directly, so what is read back is
what the abstraction produced: **0.5 before anything sends to it.** That is d5's
"vanilla Pd opens the patch and every parameter plays at its default", with a
number on it instead of a claim. Planted by cutting the `[loadbang]`
connection — that check fails and nothing else does.

**`adi.transport.pd`**: `[r $1-aditr]` into `[unpack f f f f f f f]` and seven
outlets, ADR-0188 d3's field list in order. The engine sends it with `pd_list`
and seven stack atoms, through a symbol resolved once in `open` — never
`libpd_list`, which would take `sys_lock()` and build its atoms on libpd's own
allocating message stack.

**Exactness, asserted rather than trusted:** 5,765,759 ticks — the largest a
quarter note holds — survives the 32-bit float round trip intact. That is the
whole reason your field list is bar, beat and ticks-within-the-quarter: an
absolute count passes 2^24 within three quarter notes and comes back rounded
with nothing to say so.

**win — one line of your file is the only thing missing.** `NodeIo` carries a
sample rate and nothing else (`graph.hpp`), so **the engine has no transport to
give a device.** Everything from the device host to the patch is built and
proved; `LibPdEngine::setTransport` is the seam and it is what the test drives.
The engine carrying transport to a node is `src/adi/engine/**`, yours and
standing, so I have not touched it.

**Why (e) before (d).** `[adi.sample]`'s handoff has to meet an external that
does not exist yet and whose slot API is win_codex's to agree, while (e)
depended on nothing and unblocks the analyser's own scope. (d) is next and I
will do the half that does not need the external: the declaration, the
lock-free handoff, the BLAKE3 hash in device state, and a test with a raw
buffer — the same pattern that worked for `[adi.param]` before its abstraction
existed.

**A wasted half hour, recorded because the cause is mundane and repeatable.** I
restored a planted fault, rebuilt, and read eight failures that were the *stale
binary's*. I went looking for a bug in a patch that was correct all along, and
only a diagnostic print showed the values arriving perfectly. **After restoring
a plant, rebuild and re-run before reading anything into the output** — and if
a result contradicts a file you have just read, suspect the build before the
file.

---

## 2026-09-27 — round 5c: MIDI into Pd devices (ADR-0194), and the encoder was already written

Round 5 item (c). 100 checks in `adi_pd_engine_tests`, 50 of 50 suites.

**win — the instruction ran into ADR-0054, and the way out was already in the
tree.** "The track's notes, CCs and pitch bend reach `[notein]`, `[ctlin]` and
`[bendin]`" assumes the engine has MIDI to forward. It does not, by design:
ADR-0054's parser is titled *no MIDI byte survives it*, and `EventType` is
`NoteOn`, `NoteOff`, `NoteExpression`, `ParamValue`, `ParamMod`. There is no CC
event and no pitch-bend event anywhere. A `[ctlin]` wants a controller number
and a 0..127 value and the engine holds neither.

So it cannot be forwarding; it has to be **encoding** — and the question is
whose encoder. **A Pd patch is an output edge like a VST3 plugin, so it is fed
by `MpeRouter` (ADR-0097),** which is per-device, stateful, allocation-free and
audio-thread safe already. Nothing is re-encoded twice: the quantisation a
patch sees is the one a JUCE-built MPE synth sees. A second encoder here would
have been a second set of rounding rules to keep in step with the first, and
they would have diverged at the first bug fixed in only one.

**The route is `MpeMidi`, and that is what makes all three objects fire.**
`Plain` puts every note on channel 1 and DROPS pitch and timbre, so `[bendin]`
would never fire once under it. A patch that ignores channel still hears every
note, so the member-channel spread costs a naive patch nothing.

**`inmidi_*`, not `libpd_*`** — every libpd MIDI entry point wraps its call in
`sys_lock()`, the same trap as `libpd_float` in d21. Underneath,
`inmidi_noteon` builds three stack atoms and dispatches through a symbol the
instance already holds: no lock, no `gensym`.

**Three things worth keeping:**

1. **`MpeOut::word` is filled only for a `Control`.** On a note the router
   leaves it zero and puts velocity in `value` as a 0..1 double. Reading `word`
   would have made **every note-on a note-off** — a silent instrument, with
   nothing in any log to say why. Caught by reading `mpe_output.cpp`, then
   confirmed by planting it: two checks fail.
2. **A quiet note must not become a note-off.** Velocity 0 *is* a note-off, so
   the conversion floors at 1.
3. **A test for "the value changed" passes when nothing arrives.** My bend
   check first read "moved off centre" — and the array holds 0 before anything
   is written, so `|0 - 8192| > 1` was true and it passed under a planted fault
   that dropped every control message. It now requires a bend ABOVE centre and
   inside 14 bits, which 0 fails. **Assert the value that should be there,
   never merely that the initial one is gone.** That one is general enough that
   I would take it as a rule.

**Note for whoever merges second:** ADR-0194's row is added here as `used`;
`win/color-bass` adds it as `reserved`. Same one-line conflict as 0183, same
resolution — `used`, or check 8 fails.

---

## 2026-09-27 — round 5b: the built-ins hook, and a claim of mine that a planted fault disproved

Round 5 item (b): the registration hook for ADI's compiled-in externals
(ADR-0188 d8, ADR-0192 d1). ADR-0183 gains d26. 85 checks in
`adi_pd_engine_tests`, 50 of 50 suites.

**win_codex — the hook's shape, for you to use or to argue with.** An external
declares itself beside its own definition:

    extern "C" void adi_combchord_tilde_setup(void);
    ADI_PD_BUILTIN(adi.combchord~, adi_combchord_tilde_setup)

**Nothing in `src/juce/**` is edited to admit one**, which is the point: the
externals are yours and that directory is not (ADR-0192 d6). Add the source to
`ADI_PD_BUILTIN_SOURCES` in `adi_daw/CMakeLists.txt` — one line, and the list is
there with your two device names commented in it. If you would rather have an
explicit table than self-registration, say so in your PR; the table is four
lines either way and I have no attachment to this one.

**Two traps are already handled for you**, both of the silent kind:
- **A self-registering translation unit inside a STATIC library is dropped by
  the linker** when nothing references it — the external would simply not
  exist, with no error anywhere. So the list builds an OBJECT library, whose
  objects are always linked. (Exactly the failure pthreads4w's own
  `__ptw32_autostatic_anchor` exists to prevent, met twice in one day.)
- **The registry is a function-local static**, so a registrar in another
  translation unit that runs before this one still finds a constructed table.

**win — a correction to something I would have written down as fact.** I said
registration must happen before any instance exists, because `class_new`
registers with the current instance's `pd_objectmaker`. **I planted the fault
to prove it — moved `registerAll` into `open`, after `libpd_new_instance` — and
the test passed.** So I went back to `m_class.c` at the pinned commit:

- `pd_objectmaker` is **one object for the process** (`m_class.c:27`); what is
  per instance is the method list on each class, `c->c_methods`, indexed by
  instance.
- `class_doaddmethod` under PDINSTANCE loops
  `for (i = 0; i < pd_ninstances; i++)` — it adds to **every instance that
  exists at that moment**.
- `pdinstance_new` copies **instance 0's** list into each new instance.

Instance 0 is always present and always in that loop, so **a class registered
at any moment reaches every instance, earlier and later**. Your "right after
`libpd_init`" is still right, but not for reachability — for **determinism**:
every device opens against the same complete vocabulary, and ADR-0177 fix 3
says what a patch can do is knowable from its text, which a vocabulary that
depended on load order would break. `add` refuses after `registerAll` for that
reason and for no reason to do with Pd.

I would not have found this by reading. The plant that passed is what sent me
back.

---

## 2026-09-27 — round 5a: the Pd tier builds on Windows; d24 closed on pthreads4w

#138 merged green by head SHA (`9a1d461`). This is round 5 item (a): the
dependency the director granted in ADR-0192 d5. ADR-0183 gains d25.

**Pinned.** pthreads4w `8c1d612b376333619c564ef8dadd2410b9ae0563`, by commit
with tag `-` — the 3.0.0 line lives on a branch and was never tagged, which is
airwin2rack's case (ADR-0174). Fetched by `--build-only`, 3.6 MB.

**win, three corrections and a non-change, all of them worth your time:**

1. **The licence exceptions are FIVE, not four — and none is compiled.**
   ADR-0192 d5 says "Apache-2.0 except four files". `NOTICE` at the pinned
   commit names five: four `tests/rwlock*.c` from Butenhof's *Programming With
   POSIX Threads* and `tests/threestage.c` from Hart's *Windows System
   Programming*. **All five are under `tests/`**, and `pthread.c` includes no
   file from there — checked one by one. The count is wrong in the safe
   direction: nothing ADI ships carries anything but Apache-2.0.

2. **The pin is on a MIRROR, and I checked rather than trusted it.**
   pthreads4w's upstream is SourceForge; `fetch_external.sh` clones from
   GitHub. `git ls-remote` against both gives the same object for
   `refs/heads/version_3` — `8c1d612b…` at each. So the pin is upstream's own
   commit reached through a mirror, not a fork's idea of it. That equality is
   recorded in the table's comment and in EXTERNAL-CODE.md, to be re-checked
   before the pin ever moves.

3. **libpd links its system libraries into the SHARED target only, and we
   build the static one.** `libpd` (SHARED) gets `Ws2_32` and `${PTHREADS_LIB}`;
   `libpd_static` gets two INTERFACE targets carrying compile definitions and
   include directories and nothing else. So on Windows the pthreads and winsock
   symbols Pd needs — `s_inter.c` opens sockets — are `adi_core`'s to supply.
   Found by reading libpd's CMakeLists rather than by a red build, which is the
   only reason it is not one.

4. **CI needed no change, and that is the answer rather than an omission.** The
   brief asked to "make the Windows CI leg run the Pd tier's suites". The
   Windows leg already runs `fetch_external.sh --build-only`, configures,
   builds and runs `ctest`, so the tier turns itself on the moment the
   dependency is present and `adi_pd_engine_tests` runs there with nothing
   added to the workflow.

**One more thing that made it small:** the whole of pthreads4w is one
translation unit. `pthread.c` includes 145 of the other 146 sources — all but
`signal.c`, omitted upstream — so the target is one source file. `dll.c` is
among them and must stay: a static build has no `DllMain`, so `dll.c` puts
`on_process_init` in `.CRT$XCU` and `implement.h` anchors the module against
the linker dropping it.

**What I could not verify from here was the MSVC compile and link, and CI
found one of the two.** pthreads4w itself was right: the tier configured and
every Pd source compiled under MSVC. The LINK failed, with two dozen
`unresolved external symbol __imp_libpd_*`.

**`__imp_` is the tell.** libpd's `m_pd.h` makes `EXTERN` `dllexport` for
libpd's own sources and **`dllimport` for everyone else** — so `adi_core`
compiled imports against a STATIC archive, and the linker was looking for
symbols that were sitting in it all along. The compile being clean and only
the link failing is what a dllimport mismatch looks like, and is what made it
quick to place.

`PD_DEFINE_EXTERN`, set to `extern`, is libpd's own knob for exactly this ("a
custom string for special linking purposes"). Set unconditionally, because on
ELF and Mach-O `EXTERN` is already plain — one code path beats a conditional
only one platform exercises. Recorded in d25 as the one finding CI caught
rather than reading.

**A mistake of mine, recorded because the lesson is general.** OneDrive had
again littered the worktree with " 2" conflict copies. I wrote a cleanup that
matched on the NAME pattern and deleted 69 files — but `adi-surge`'s wavetables
are legitimately called things like `Bright Rise 2.wav`, and they are TRACKED.
Restoring with `git checkout -- .` then threw away this session's own uncommitted
edits along with the bad deletions, and all of round 5a had to be written again.
Two rules out of it: **a conflict copy is untracked, so `git ls-files` decides
and a name pattern never does**, and **commit before running any cleanup**.

---

## 2026-09-27 — round 4 part B: no Pd external loads from disk, and neither of the two ways to stop it works

win's round-4 brief (`collab/prompts/2026-09-27-mac-round4.md`), part B. #138 is
rebased on main at `5295c79` and both new rules are implemented rather than
noted. ADR-0183 gains d18–d23.

**Rebased.** The conflicts were the four win predicted: `DECISIONS.md` (ADR-0183
appended after ADR-0190 — the log is append-only and numbers need not be in
order), the README count (188 on main, 189 with this), the 0183 row (`reserved`
to `used`, now that #136 has merged), and the collab logs. `validate_schema.py`
passes, checks 7 and 8 included.

**ADR-0188 d8, and the finding that matters: neither mechanism in the brief
works.** Both were read in Pd 0.56's `s_loader.c` at the pinned commit.

- **A registered loader cannot refuse.** `sys_register_loader` APPENDS, and
  `sys_do_load_lib` is the list's static head. Anything the host registers runs
  only after the default has already searched every path and loaded what it
  found. There is no hook that runs first.
- **libpd without dynamic loading does not cover Windows.** `HAVE_LIBDL` gates
  the `dlopen` branch only; the `#ifdef _WIN32` branch above it calls
  `LoadLibrary` whatever `HAVE_LIBDL` says. A guard that holds on two platforms
  of three is the kind that looks done.

**So the guarantee is made where it can be made portably: nothing loadable is
ever on a path Pd will search.** Before `libpd_openfile`, `LibPdEngine` refuses
the patch's own directory and every search path it was given if either holds a
file with an extension Pd would try, and it refuses a patch whose text carries
`[declare -lib]`, `-stdlib`, `-path` or `-stdpath` — the one object that
defeats the discipline from inside the patch, found by the same static parse the
declarations use, with Pd not running.

**The danger is measured, not argued.** With `adi_probe_external.pd_darwin`
dropped beside a patch that names that object, **Pd called `dlopen` on it** —
the console carries the path and dlopen's reply, "slice is not valid mach-o
file". The only reason nothing ran is that the planted bytes are not a library.
That test drives raw libpd deliberately *around* the engine, because what it
proves is that the refusal is refusing something that would otherwise happen.

**Abstractions are untouched, and that is Pd's ordering, not luck.**
`sys_loadlib_iter` runs every loader first and calls `sys_do_load_abs` only
when they have all failed. So `adi.array.pd` and `adi.param.pd` keep resolving
through exactly the paths whose externals are refused — asserted, not assumed.

**ADR-0188 d3, and a second finding. `libpd_float` cannot be used on the audio
thread, twice over.** `libpd_dofloat` is `gensym(name)->s_thing` inside a
`sys_lock()`: a mutex, and for a name Pd has not seen an allocation — which
happens even in the failing case, because the symbol is created before the null
`s_thing` is noticed. **win, this is why your d3 sentence "only to receive names
the patch declared" is load-bearing** rather than tidy: sending by name from the
audio thread is a dropout waiting for the first unrecognised parameter.

So `bindParameters` resolves each declared parameter's `t_symbol*` once on the
message thread and `sendParameter` reads `s_thing` from the audio thread — no
`gensym`, no lock. Cheaper *and* safer than the by-name call, not a trade. A
value sent that way comes back out of Pd through a published array and follows
when it changes.

**ADR-0177 d5's vanilla-Pd promise, measured.** `adi.param.pd` does not exist
yet, so `tests/pd/adi-param-proof.pd` is exactly d5's case: the `[adi.param]`
box does not create, Pd says so, **the patch opens anyway**, that parameter is
silent, and the declaration is still readable text. All four asserted.

**Pd reports almost everything only to its console,** so `LibPdEngine` now keeps
one, per instance — the hooks live in the instance under PDINSTANCE and `$0`
cannot tell two apart. Without it the engine's only report is the return value
of `open`, and a patch that opened missing half its objects looks exactly like
one that opened whole.

**Transport: a follow-up, and the analyser does need it.** The DJ-style scope's
whole behaviour is a waveform scrolling locked to the bar, and the Max for Live
build syncs that scroll to the host tempo. It is not built here because the
abstraction sits beside `adi.param.pd`, which is win's and unwritten, and the
host half is d21's audio-thread send with a different payload — building it
against an abstraction that does not exist would be building half of it twice.
The field list is recorded in ADR-0183 d23 so the follow-up starts from it.
**d3's range rule is not this engine's**: it is the device layer's, where the
parameter rows and the op live.

**Names:** nothing this session added says "ADI DAW" or "ADI Live"; the old
names in `DECISIONS.md` are in historic entries, which ADR-0190 keeps.
`adi-m4l-analyzer` keeps its own, as the brief says.

**CI found something reasoning would not have: libpd does not build under
MSVC.** Both Windows jobs failed in CONFIGURE, at
`third_party/libpd/CMakeLists.txt:29` — "Please provide a path to the pthreads
library and its headers". Pd threads with pthreads; GCC and clang on Windows
have winpthreads, MSVC has nothing.

The Pd runtime tier is now behind `ADI_WITH_PD`, ON everywhere it can build and
off on MSVC unless pthreads is given. **The declaration parse is not gated** —
ADR-0177 fix 3 put it behind a static parse with Pd not running, so it has no
libpd in it and its 76 checks build on every ABI including that one. A property
designed for a different reason paid for itself here. Measured both ways: 50 of
50 suites with the tier on, 49 of 49 with it off.

**win, this one is yours and the director's, and ADR-0183 d24 has the facts.**
Closing it means a SHIPPED dependency on your platform: `pthread-win32` through
`fetch_external.sh` (Apache-2.0, small, exactly ADR-0024's shape), or vcpkg's
copy in the Windows job (fewer files, but not pinned by commit and not fetched
by `fetch_external.sh`, which ADR-0024 requires). I did not take it, because
EXTERNAL-CODE.md's rows record that several dependencies were
"director-granted", and adding one unilaterally inside the PR that vendors libpd
is easier to do than to undo. **Until it is decided the Pd device tier does not
run on Windows**, and that is in the ADR rather than left to be found.

**Verified:** build clean, **50 of 50 suites**, `adi_pd_engine_tests` 74 checks
and `adi_pd_declaration_tests` 76. Every new guard was planted and confirmed to
fail its test when removed — the directory scan, the search-path scan, the
`[declare]` refusal, the print hook, and `sendParameter`'s send.

---

## 2026-09-27 — renumbered to ADR-0183, moved off the mission session's branch, rebased onto main

win's six corrections, all applied. Nothing had been pushed, so all of it was
cheap.

**1. ADR-0179 → ADR-0183.** The reservation table decides (ADR-0051), and 0179
was held for the mission session's CLAP host contract before this session spent
it. Renamed in the `DECISIONS.md` heading and in every reference across the
four commits — including one written `ADR-0177/0179`, which a search for
`ADR-0179` alone would have left behind. `collab/README.md` carries the 0183
row as `used`; 0179's row is untouched. `adi_daw/README.md` now says 182
entries. `validate_schema.py` passes, checks 7 and 8 included.

Note for whoever merges second: the 0183 row is added here as `used`, and win's
open PR #136 adds the same row as `reserved`. They will conflict on that one
line and the resolution is `used` — check 8 fails otherwise, because ADR-0183
is in `DECISIONS.md`.

**2. Its own worktree.** `Adi-wt/pd-analyser` on `mac/pd-analyser-wip`.
`git branch --show-current` before every commit from here on.

**3. Rebased onto main.** The four commits sat on top of the mission session's
`e07b8a6` and `39b5aee`; `git rebase --onto origin/main e07b8a6` moved them off
and they now sit on `76ddcc9`. Every `collab/mac.md` hunk was dropped in the
rebase and moved here instead — that file is the mission session's.

**The local `mac/ci-render` is left alone.** `origin/mac/ci-render` is
`e07b8a6`, which is exactly what PR #134 shows, so nothing of this session's is
on the pushed branch or in that PR. The local branch is still four commits
ahead, and all four are this session's; resetting it to `origin/mac/ci-render`
by name clears it, as win already told the mission session. That is theirs to
do, not this session's.

**Checked against ADR-0177, which merged while this ran.** It governs
unchanged, and `DECISIONS.md` now says so in its own section rather than
leaving two documents to be compared:
- nothing in `[adi.param]`'s grammar is amended;
- `[adi.array]` is a sibling with its own grammar, its own id space and its own
  receive stem (`-adiarr-` against `-adi-`), so a patch cannot address one
  where the other is meant and have it half work;
- **ADR-0177 shipped no code** — nothing on main reads or writes an
  `[adi.param]` — so `parsePdDeclarations()` is the first implementation of its
  grammar as well as the array's, which is the argument for one scanner rather
  than two;
- `adi.param.pd` (d5) and `tools/gen_pd_patches.py` (d6) stay win's;
- d15 applies to `[adi.param]` too: `$0` does not expand in a message box;
- the one real risk to ADR-0177's grammar is d11, and it is flagged, not taken.

**4. libpd recorded under ADR-0024.** `docs/EXTERNAL-CODE.md` gained the row it
was missing: **0.16.1 / `ba0dc63262901d658af8bbda5e619a60fa975e78`**, its
`pure-data` submodule at **`f009fd8d7b537e209e09898d487fdf1bf547da2b`** (Pd
0.56-5), fetched by `--build-only`, built `PD_MULTI` and static. Licences read
at the pinned commits, not off GitHub's labels: both `LICENSE.txt` files are
the Standard Improved BSD License, and `pure-data/src`, `pure-data/extra`,
`libpd_wrapper` and `cpp` contain no GPL or LGPL text at all — so nothing
compiled from this dependency is stronger than BSD-3, as ADR-0035 requires.
`third_party/` stays git-ignored and no libpd source is tracked.

**Verified after the rebase, not before it:** configure and build clean,
**50 of 50 suites pass**, `adi_pd_declaration_tests` 66 checks and
`adi_pd_engine_tests` 41. The M4L project's own `test/run_all.sh` passes too,
and its generators are deterministic — a full regeneration left every tracked
byte identical.

---

## 2026-09-27 — [adi.array] built as [adi.param]'s sibling; the M4L analyser imported

**[adi.array] (ADR-0183 d13–d17).** ADR-0177 is approved, so this is its
sibling, built to the instruction that the two move as identical siblings:
ONE scanner, two schemas. One tokeniser, one `$0` check, one id rule, one
problem list. `[adi.param]` is implemented here too rather than left for a
second parser to get subtly different later — win, the grammar is yours and
this follows it exactly.

    [adi.array $0 <id> <length> <rate> <min> <max> <unit> <name>]

min/max/unit are ADR-0177's fix 2 applied, not an addition: a renderer given
only a length and a rate still guesses the value range, and a guess about a dB
floor draws a picture that is wrong in a way nobody can see. Ids are a SEPARATE
space from parameters', because a parameter's id is `plugin_params.param_id`
and an array is never automated — one space would make adding a display change
what a lane points at.

The parse is static and includes no libpd at all: **all 66 declaration checks
run with Pd absent**, which is the property ADR-0177's fix 3 rests on.

**Four traps, all now in the ADR:**

1. **`$0` does not expand in a MESSAGE box**, only an object box. A message box
   `; $0-adiarr-1 0 0.5` targets `0-adiarr-1`; Pd says "no such object", the
   patch loads anyway and the array stays at zero. The `$0` has to live in
   `[s $0-adiarr-1]`. **`tools/gen_pd_patches.py` needs this before it emits a
   declaration**, and it applies to `[adi.param]` identically.
2. **Under PDINSTANCE the search path is per instance**, not process-wide. A
   path added before the instance exists goes nowhere, and the only symptom is
   the abstraction failing to create.
3. The double buffer's slot must come from ONE expression — writer and reader
   derived it separately, disagreed, and every read returned the previous
   array: a display one frame behind for ever, with nothing to show for it.
4. `#X obj <x> <y> <class>` puts the class at atom 4. Reading atom 3 gets the y
   coordinate and makes every declaration invisible.

`pd/adi.array.pd` ships as a vanilla MIT abstraction holding
`[table $1-adiarr-$2 $3]`. **`adi.param.pd` is still win's to write**; the
parser accepts its declarations today.

**The Max for Live analyser is imported** at `adi-m4l-analyzer/`, as its own
project beside adi-surge and adi-vital, with `PROJECT.md` separating what is
worth porting from what is not. History stays in the `AVC-Spectrum-Meter` repo.
The JS drawing layer is throwaway; the measurement is not, and `PROJECT.md`
tabulates it against the tests that hold it — coherent-gain calibration, the
4.5 dB/oct slope, bx-matched RMS, the goniometer's one-sample pairing, the
period detector's octave-error rule. The findings section is there so the same
days are not spent twice: `jit.gen` resampling mismatched dims, Max `expr`
having no ternary and no working exponent notation, undefined fan-out order,
and **Live's CPU meter measuring audio processing and excluding interface
redrawing** — two costs conflated for days until a macOS profile put
`jsui_paint` at 52% of the process with the device's MSP at zero samples.

---

## 2026-09-27 — libpd is in, running, and proved; three findings, one of them win's to settle

ADR-0183 decisions 9–12 are what building it found; all three are asserted in
`tests/test_pd_engine.cpp`.

**What landed.** libpd pinned at 0.16.1 (`ba0dc6326`, BSD-3-Clause) with its
pure-data submodule, built static, linked into `adi_core` so the Pd tier tests
on every ABI rather than in one JUCE job. `LibPdEngine` implements the
`PdPatchEngine` the contract left pure virtual. Audio in one end, out the
other, through Pd's own DSP.

**PD_MULTI is not optional.** Plain libpd is one instance per process: every
patch shares one DSP graph, one sample rate, one block size and one
`libpd_process_float`. A device has to be processed when the graph reaches its
track, at that node's block size, with its own latency.

**Three findings:**

1. **`$0` is per INSTANCE, not per process.** `canvas_getdollarzero()` is
   per-instance state, so two devices opening the same patch both get 1003 and
   both answer on `1003-report_latency`. A table keyed on `$0` cannot tell them
   apart — ADR-0095 d1 assumed otherwise. It is per instance now and the float
   hook asks `libpd_this_instance()`. Inside an instance ADR-0095's reasoning
   is untouched.

2. **The `[loadbang]` latency report cannot be received, by anyone.** The name
   needs `$0`, `$0` comes from the opened patch, `[loadbang]` fires inside
   `libpd_openfile`. ADR-0095 d2's query after `prepare` is therefore not a
   correction for a value sent at the wrong sample rate — it is the only report
   a host ever gets, which makes it load-bearing rather than belt-and-braces.

3. **The patches in `pd/` are abstractions and libpd cannot render one.**
   `inlet~`/`outlet~`, no `adc~`/`dac~`. `libpd_openfile` opens a top-level
   canvas where `inlet~` connects to nothing: `adi-rmsc.pd` opens, Pd renders
   its blocks, and every output sample is zero with no error anywhere to say
   why. **This one is win's** — `tools/gen_pd_patches.py` and ADR-0096 own
   those patches. Three ways out and one is a trap: a per-device wrapper
   instantiating the abstraction between `adc~` and `dac~` breaks the latency
   protocol, because the abstraction's `$0` is not the wrapper's and
   `libpd_getdollarzero` returns the wrapper's. Top-level device patches keep
   the protocol intact.

**Contract change, small:** `PdPatchEngine::adapterLatencySamples()`, defaulting
to 0 so the contract's own fakes are unaffected, added to
`PdDevice::latencySamples()`. `NodeIo` carries a segment and not a block, so an
event at frame 100 splits 512 into 100 and 412 and the engine practically never
sees a multiple of 64; the adapter between them costs one Pd block. A patch
reporting 0 through an engine that delays 64 is the misalignment ADR-0058
exists to remove.

---

## 2026-09-27 — the analyser: ADR-0183, four rulings, and two prerequisites that are not the analyser

**The director's instruction:** build the flagship analyser — the Max for Live
spectrum meter ported to a Pd DSP tier with a C++ UI, big-window mode, an EXO
tab cloned from sMexoscope, and a VISU-grade spectrogram. Five repositories
named to clone.

**Nothing was built.** Four questions went to him first and he ruled on all
four; ADR-0183 records them. The reason nothing was built is worth repeating:

- **libpd was not vendored** and was not in `CMakeLists.txt`. `pd_device.hpp`
  said so itself. The patches in `pd/` are artefacts of ADR-0035's direction;
  nothing opened them.
- **Published arrays did not exist.** ADR-0116 d2 says the contract gains them;
  no spelling of `publishedArray` was in `src/`.

So the analyser had two prerequisites and neither was the analyser. ADR-0183 d8
says it does not lead.

**His four rulings:**
1. OpenGL approved for the analyser now, overriding ADR-0050's step-7 deferral.
   The deferral still stands for `ArrangementCanvas`.
2. One clock. Rendering locked to `VBlankAttachment`; the 120 Hz timer refused.
3. The EXO tab uses sMexoscope's own buffer, not `scope.hpp`'s tap — against
   the recommendation, and for the better reason: the tab's value is being a
   behavioural clone, and the behaviour comes from the buffer. Audio-to-UI
   paths go to three and ADR-0183 d5 names all three so they stay counted.
4. AGPL-3.0 accepted at project level. PerceptoMap is AGPL; the binary already
   carries AGPLv3 through JUCE's grant, so the GPLv3 boundary being protected
   was not there. `LICENSE` says GPL-3.0 and now understates it — correcting it
   is part of this work.

**Licences, checked before anything was cloned:** sMexoscope GPL-3.0,
PerceptoMap AGPL-3.0, libtfr `GPL-2.0-or-later` — that last one reads as
"GPL-2.0" on GitHub's label and would have looked like a hard blocker; its
headers carry the `-or-later`. tfr and juce-spectroscope19 are reference-only
in the brief and were not checked.

---
