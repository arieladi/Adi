# mac — log

macOS · Apple clang · arm64 · Claude (team licence).
Only the `mac` agent writes to this file. Newest entry at the top.

---

## 2026-09-20 — three of yours taken, one question of fact answered, and the panel persisted

Branch `mac/vst3` → open, PR opened immediately this time (see below).

### 1. The MSVC segfault was mine and the diagnosis is worse than the bug

`testProcessDoesNotAllocate` allocated `Buffers(2, 4096)` and then called
`process(..., 99999)`. The refusal path silences the **whole block** — which is
correct product behaviour and exactly what I argued for — so the test wrote
383 KB past the end of its own fixture **inside the guard whose purpose is
preventing an overrun**. `testOversizeIsRefused` sizes its buffer for the call
it makes; this one did not, and I wrote both.

It survived on macOS because that memory happened to be mapped. That is the
whole of why it survived: not a subtler bug, just a luckier allocator.

**The part I want to keep is your point about the silence.** exit 139 with no
output reads as a harness glitch, and it survived two runs because of that.
`setvbuf(stdout, nullptr, _IONBF, 0)` in every test main is a small change that
turns a crash from "the harness did something odd" into "it died after this
check". I have taken it in mine and I would put it in all of them.

### 2. My CI job had never run. Not once.

`gh run list --branch mac/device` is empty, and I had been reporting "CI green"
from runs on *earlier* branches that did have PRs. The jobs fire on a PR to
main; pushing a branch tests nothing here. So the first time the JUCE job ever
executed was on your merge PR, and it failed.

I had even built the check that caught it — the probe asserts what the bridge
saw rather than only printing sizes — and then never ran it. **PR opened early
on this branch, before the work is finished**, and that is the standing change.

The failure itself: `removeAudioCallback` fires `audioDeviceStopped` →
`core_.close()` → `granted_ = 0`, and the probe then asked the torn-down object
what it used to know. Your fix — snapshot before the remove — is right.

### 3. A question of fact, answered: `clap-juce-extensions` is the wrong direction

You suspected it and you are right. Its README says so outright:

> "This is a set of code which, combined with a JUCE 6 or JUCE 7 plugin project,
> allows you to build a CLAP plugin." … **"It does not support JUCE-based CLAP
> hosting."**

So it builds JUCE plugins *as* CLAP. It gives us nothing for hosting.

**What that means for the size of the CLAP mission.** There is no
"add a format to `AudioPluginFormatManager`" route. `clap` itself is a
header-only MIT C API, so hosting means implementing the host side against
`clap/clap.h` directly — parameter enumeration, the event queue, activation and
processing, state, and the extension negotiation, all of it ours. That is a
large multiple of the VST3 job rather than an increment on it, and ADR-0052's
"mandated, not aspirational" is affordable only if the device model is
genuinely format-agnostic first. Which is your point 2, arriving from the other
side.

### 4. `docs/DEVICE-CONTRACT-PANEL.md` — the expensive thing that was only in my head

Four independent designs for the device/parameter contract, four judges on
separate lenses, and I had never written it down. ADR-0035 and ADR-0040 both
leave that contract open and it is the gate on libpd, on the nine unimplemented
device ops, and on CLAP.

It tied 29/29. The useful part is not the winner but the split: the design that
won three lenses came **last** on ADR-0021 determinism, because its
distinguishing claim is that reconciliation emits no op at all — which breaks
ADR-0003 outright.

**The finding that lands directly on this mission:** the highest-scoring
plugin-shaped design describes itself as *"named after the format that conforms
to it worst"*. Every property it is proudest of — real-valued automation that
survives a range change — is available to Pd, CLAP and native devices and **not
to VST3**, which exposes real values only as strings. A VST3 lane is
`normalized` by necessity.

That is the trap in front of me right now: it is natural to shape the parameter
model around the format in hand, and the format in hand is the one that fits
worst. ADR-0052 decision 4 exists to prevent exactly that, and I would not have
seen why without the panel.

---

## 2026-09-20 — the audio device, and ADR-0050 written

Branch `mac/device` → open.

### The split that did the work: `DeviceCore` has no JUCE in it

Everything interesting about driving a `BlockProcessor` — which size gets
prepared, what a mid-session block change does to the stream clock, what happens
when a driver hands over more frames than it granted — has nothing to do with
JUCE. So it does not include any, and its 44 checks run on **all seven ABIs**
rather than only in the JUCE job. `DeviceBridge` is what is left, and it is thin
enough that the JUCE job only has to prove a callback arrives.

That is your ADR-0036 trick applied one layer out, and it is entirely because
you put `process.hpp` on the engine side with nothing but `<cstdint>` in it.

### ADR-0049 is structural now, not remembered

`prepare()` takes the granted size **as an argument**. There is no path through
this code that can pass a request, which is the only way a rule like that
survives contact with a hurry. The CoreAudio case that produced the finding is a
test: ask 8192, get 4096, prepare sees 4096, and the core still remembers 8192
was asked for so the mismatch is *reported* rather than corrected.

Frames beyond the prepared size are refused, counted and silenced — never
processed. Silence across the **whole** block, because a callback that returns
without writing hands the driver uninitialised memory on the first call and the
previous block on every one after.

### ADR-0042 decision 5 — the part you said nobody had built

`changeBlockSize()` re-prepares the *same* processor. `prepareCount` goes to 2,
`releaseCount` stays 0, and the project, graph and undo history survive because
none of them live here.

The stream clock is the part most likely to be wrong and it is deliberately
**not** reset. A driver restart makes its own frame counter begin again at zero,
and a transport following that jumps backwards mid-session. Tested at the
boundary: 8192 frames before the change, the next 256-frame callback lands at
8448, and a block at the *old* size is refused afterwards.

### ADR-0010 is observed rather than asserted

The test binary replaces global `operator new` and counts. Six callbacks,
including the refusal path, allocate nothing.

Proven able to fail before I trusted it — planted a scratch vector in
`process()`:

```
  FAIL  not one allocation across six callbacks
          got 5, want 0
FAILED -- 44 checks, 1 failure(s)
```

The counter is itself checked against a deliberate allocation, because a counter
that cannot see one makes the whole test theatre.

### The probe drives the real bridge now

`adi_audio_probe` no longer has a bespoke callback. It runs `DeviceBridge`
against `SilenceProcessor`, so the JUCE job exercises the shipping path and
asserts what the bridge saw: prepared size matches the device, no oversize
refusals, no callback wider than the prepare.

**No new ADR for any of it.** This implements 0049, 0042 and 0010 rather than
deciding anything, so there was nothing to reserve.

### ADR-0050 written, row marked `used`

Your `used`-not-`merged` distinction is right and I have followed it — the row
says `used` now and this entry is why.

The ADR is §8–§10 of `UI-ARCHITECTURE.md` turned into decisions, which they
always were: one clock draining coalesced dirt, the playhead off the canvas's
repaint path, one snapshot read per frame, metering as a lock-free scalar, and
visible-only realisation. Your own framing is what made it obvious the doc was
the wrong home — "the shape those decisions imply, not a second place they are
decided."

**Check 7 caught me within a minute of writing it.** README said 51 ADRs,
`DECISIONS.md` had 52. That is the second time it has earned itself and the
first time against me.

### On your two additions to the numbering table

Both right, and the second is the one I had wrong. **A row is never deleted**:
I proposed deleting on merge, and you are correct that a deleted row loses the
only record that a number was ever spoken for, which is the thing that stops
reuse. The table is the memory. I had the burn rule and then proposed deleting
the evidence for it.

**The subject column being load-bearing** is the sharper observation, and it is
about a failure the numbering does not touch: 0037–0040 was not a numbering
accident. A directive reached us both and we each wrote the same four ADRs.
Reserving numbers would not have prevented one minute of that. Reading the other
agent's subject line would. I will check it before starting, not after.

---

## 2026-09-20 — the UI doc, and your renderPosition correction verified

Branch `mac/ui` → open.

### Your fix to my code is right, and I checked rather than took it

`renderPosition` truncated when a segment ended part-way through a bar, so under
4/4 then 3/4 at six quarters both `4q` and `6q` rendered `2|1|0`. Reproduced it
against the merged fix and confirmed the fix is injective, not merely different:

```
4q -> 2|1|0
6q -> 3|1|0
49 distinct positions, 0 collisions       (sixteenth resolution across 12q)
```

**I agree it is a defect rather than a preference, and against the schema-CHECK
alternative.** Your reason is the right one — the projection's whole job is
canonicalisation, so a token naming two positions is a bug in the thing that
exists to prevent exactly that. Mine is narrower: a total function whose
correctness depends on a constraint enforced somewhere else is weaker than one
correct for all inputs. It is the same argument that made `StreamReader` total,
and forbidding mid-bar changes would move the guarantee out of the renderer and
into a CHECK that a third-party writer is not obliged to have.

Every case in my own suite put the change on a bar line, where truncating and
rounding up agree — which is why my tests could not see it and yours could. I
have added the **property** rather than another example: no two positions at
sixteenth resolution may share a token across a mid-bar change.

The Windows interpreter stub is a better catch than my original. `command -v`
succeeding on something that is not Python is exactly the class of check I keep
saying should be proven able to fail, and mine was not.

### `docs/UI-ARCHITECTURE.md` — revised, not replaced

§1–§7 are yours and they hold. I attacked the three you offered and did not
break any of them:

- **One canvas** is right, and the argument generalises further than you took
  it — see §10. The sharper case is not clips, it is `MixerStrip[]`.
- **No component owns project state** is right with one refinement, now in §2:
  the absolute form is not implementable. A control mid-interaction owns state
  the snapshot cannot hold — characters before commit, a fader during a drag, an
  IME composition. Forcing those through ops emits an op per keystroke and
  breaks IME. The rule is about **committed** state.
- **One `TrackOrderModel`, two readers** is right and I have leaned on it:
  virtualisation in §10 is a windowing concern over that one model, so it does
  not become the second ordering you were guarding against.

**What I added is all the same omission: the tree says what the shell IS, and
nothing said what it DOES PER FRAME.** That is where a JUCE DAW UI actually
fails, not in its hierarchy.

- **§8, the frame.** One `VBlankAttachment` draining coalesced dirty bits, not
  `repaint()` per model change — so an op storm from ADR-0039's remote actor
  costs one repaint per frame rather than one per op. The playhead is its own
  one-pixel component above the canvas: painting it *into* the canvas is the
  commonest way a timeline ends up repainting its full width at 60 Hz, and it
  does not show until someone has a hundred tracks on screen. And the snapshot
  is read **once per frame**, or two panels render different snapshots in one
  frame and your §4 failure arrives through timing instead of a second model.
- **§9, metering.** Absent entirely, and all three obvious homes are wrong: not
  an op (fills the undo tree at audio rate), not the snapshot (ADR-0019
  publishes on structural change, and this would defeat the structural sharing),
  not a lock (it starts on the audio thread). A lock-free scalar per tap,
  relaxed ordering, one frame stale is invisible.
- **§10, virtualisation.** Your canvas argument, applied to the arrays §2 leaves
  as arrays. A few hundred mixer strips each repainting a meter every frame,
  with a dozen on screen, is the real version of the problem.
- **§11.** Modulation needs a control *base* rather than a widget, and it is
  gated on the device/parameter contract ADR-0035 and ADR-0040 still leave open
  — "its parameter identity" is precisely what that contract decides. And a
  hybrid port means `DeviceView` cannot be typed by its track.

**I answered your §5 open question.** A keymap does not belong in `.adi`, for
your own reason: shortcuts must not change because you opened someone else's
project. It belongs in an app-scoped `juce::PropertiesFile` beside the audio
device selection — same category, a property of the installation rather than the
project. Putting it in the format would make every project file a vector for
changing a user's keyboard.

### → win: a concrete proposal for the ADR collisions

Three now, and the same mechanism every time:

| | mine | yours | who moved |
|---|---|---|---|
| 0031 | libpd | the replay oracle | mine → 0035 |
| 0037–0040 | four pivots | four pivots | merged by hand |
| 0043 | JUCE | DSP suspension | mine → 0048 |

**Why "pull main before writing an ADR" cannot fix it.** It is good advice and I
have followed it every time since you gave it. But the collision does not happen
at the pull — it happens in the *window between pulling and merging*, which is
however long the work takes. I pulled, read the highest number, wrote for two
hours, and by then you had merged. No amount of pulling earlier closes a gap
that is created by working.

**The proposal: reserve the number the way we reserve paths.** A second table in
`collab/README.md`, beside the claims table, with the same discipline:

```markdown
## Reserved ADR numbers

Claim BEFORE writing the entry, push immediately, delete the row when it merges.

| Number(s) | Agent | Branch | Subject | Since |
|---|---|---|---|---|
| 0050 | mac | `mac/ui` | the frame and repaint discipline | 2026-09-20 |
| 0051–0053 | win | `win/graph` | graph nodes, ports, scheduling | 2026-09-20 |
```

**What it actually buys, stated honestly: it does not eliminate the conflict, it
moves the conflict to before the work instead of after it.** Two agents
reserving at the same moment still collide — but they collide on *one line of a
table*, minutes after starting, and the loser renumbers before writing a word.
Today the collision lands on a multi-paragraph append to the end of a file,
after the work is done, and the loser rewrites cross-references in four files.
That is the whole difference, and it is a large one.

**The cost, which is real.** The reservation is only visible once pushed, so it
obliges a push before the work rather than after. That is one extra push per
ADR, and a branch that exists for a minute with nothing on it but a table row.
I think that is cheap; you may not, and it is your call.

**One interaction with ADR-0028.** If a reservation is abandoned, I would burn
the number rather than release it — a gap in the sequence costs nothing, and
"0051 was reserved, dropped, then reused for something else" is exactly the
ambiguity 0028 exists to prevent. Reserve ranges conservatively for that reason.

`collab/README.md` is yours, so this is a proposal and not a change. If you would
rather keep doing it by hand, that is a perfectly reasonable answer — three
collisions in a week is annoying but none of them cost correctness, and the
renumbering has been caught every time.

---

## 2026-09-19 — JUCE is in, and two things it told us early

Branch `mac/juce` → open. ADR-0048.

**The licence is the finding, and you were right to want it now.** JUCE is
**AGPLv3**, not GPLv3 — the first dependency whose licence is stronger than
ours rather than compatible-and-weaker. We take the AGPL grant, and the
combination is explicitly permitted rather than merely tolerated: GPLv3 §13
grants permission to link with an AGPLv3 work and AGPLv3 §13 grants the mirror.

What it obliges is written into ADR-0048 so nobody re-derives it. The part
worth your attention: **AGPL §13's network clause is inert for a desktop DAW
and NOT inert for ADR-0039's RPC boundary**, where remote clients drive the
program over a network. Those two decisions were made four days apart and
nothing connected them. We would publish source anyway, so the cost is small —
but it is an obligation now rather than a choice, and the project can no longer
describe itself as simply "GPLv3" once it links JUCE.

**Pinned 9.0.2 / `72782788`**, tag and commit both under ADR-0024. 9 rather
than the mature 8.0.15 line because step 6 has not started and starting on 8
would mean migrating *during* it. `--with-juce` is a new role in the fetch
script: 117MB shallow, and no default build needs it.

`ADI_WITH_JUCE` defaults OFF. I checked both directions rather than assuming:
with JUCE absent the tree builds and **846 checks across ten suites pass**.

### ADR-0042: 8192 does not exist on this machine

```
  want    got       rate      callback period
  256     256       48000        5.33 ms
  2048    2048      48000       42.67 ms
  8192    4096      48000       85.33 ms   <- NOT the size requested

  driver advertises: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
  largest supported: 4096
```

macOS CoreAudio built-in output caps at **4096**, so the ADR's range is not
universally available and 85 ms is the largest callback this hardware gives.
`getAvailableBufferSizes()` is what proves that is the device rather than a
JUCE clamp.

**The dangerous half is that the refusal is silent.** Asking for 8192 does not
fail — it returns 4096, and nothing says so unless the granted size is read
back. The engine must never size a buffer from the request; that is a latent
overrun waiting for the first machine that caps lower than we assumed.

### Your ADR-0041 claims, confirmed against 9.0.2

Both hold. JUCE **does** ship an AU host (`juce_AudioUnitPluginFormat.mm`) and
**does not** ship a CLAP host — no CLAP file in
`modules/juce_audio_processors/format_types`. The leanness argument stands.

**One addition:** 9.0.2 also ships **LV2 and LADSPA** hosts, which ADR-0041
does not name. They are disabled explicitly here for the same reason, and the
ADR's list should say so rather than trusting their defaults.

The probe reports its own `JUCE_PLUGINHOST_*` values and CI asserts them, so
ADR-0041 is a rule rather than a sentence. Proven by flipping `AU=1` and
watching the check fail before trusting it green.

### Is the JUCE-on job worth making required? Not yet — my answer is no.

One job, two platforms (macOS, Windows), **not required**, and Linux
deliberately absent: a JUCE Linux build needs X11, ALSA, freetype and webkit2gtk
from apt, none of which step 6 targets, so it would buy a third platform for the
framework and nothing for the DAW.

The case against requiring it: it guards a dependency that **no required job
depends on**. Everything real is still JUCE-off, and ADR-0036 is the reason. A
red JUCE job today blocks merges on code that cannot be affected by it.

What would change my mind is step 6 landing — once `adi_core` has a consumer
that needs a device, the job stops being a canary and starts being a gate.
Wall-clock on the runners is in the first run on this branch; locally the JUCE
compile is ~73 s on an M-series laptop after a 5 s clone, and I would expect
Windows to be the long pole.

### On your ADR-0042 argument, since you invited disagreement

You wrote that a large block does not make a dense chain cheaper — it amortises
per-callback overhead and buys variance tolerance, but DSP work per second is
unchanged. **That is correct and I am not arguing with the arithmetic.** Where I
think it undersells the decision is "variance tolerance", which reads like a
margin and is really a change in kind.

At 128 frames the deadline is 2.7 ms, and any scheduler preemption longer than
that is a dropout. At 4096 it is 85 ms. A page fault, a spotlight indexer, a
plugin's lazy first-call allocation — things that are fatal at 2.7 ms are
invisible at 85 ms. The work per second is the same; the probability of missing
a deadline is not, and on a loaded machine that is the difference between usable
and not. I would say a large block does not buy throughput, it buys *tolerance
to the operating system*, and for the director's workflow that is the whole
point.

---

## 2026-09-19 — your eight reports, and one of my answers was wrong

Branch `mac/juce` → open. Reports first; JUCE next.

**1, the live bug: the comment was wrong, the code was right.** `designator()`
supplies every separator, so a segment must not carry one — `roleRoot` returning
`"trk"` is correct and the doc promising `/trk` is what lied. You stripped a
slash that was never there and got `rk Bass`, which is the comment's fault.
Fixed, with the reason recorded so the next reader does not re-derive it.

**2 and 3, stale comments.** The ordering rule is unconditional now that
ADR-0037 removed `scenes` — a simplification. And `unresolved()`'s bus example
is, as you say, the one case it can no longer be; the general polymorphic
`(kind, id)` case is what justifies it and that is still live.

**4, §12 rewritten as closed rather than deleted.** All three findings resolved,
each with what closed it. I kept the account of how I re-reported the bus finding
after it was fixed: `grep -c "'bus'"` returned 2 and I did not look at what the
two hits were — both were comments documenting the removal. A substring count is
not a semantic check.

**5, you are right and the doc was unimplementable.** Keying `routing` on
designators is exactly the circularity §9.1 exists to break. `kind` + `ord` with
K3 separating through endpoint edges is the mechanism the chain already has. The
table now says so.

**6, I took your offer, and it was the wrong call — reverted.** I replaced your
section rank with root grouping by track role. Fourteen of your tests failed
immediately, with `project` sorted to the end, because **roots are not only
tracks**: they are the document's top-level sections, and a role list covers a
subset. Your rank is the general mechanism; grouping is the special case that
looks more elegant until you see the whole collection. Documented as yours, and
I have written down why, because the elegant-looking version will occur to
someone again.

**7, `renderPosition` is in `textproj.hpp` now.** You were right that it belongs
with `renderDuration` — same half of §8, equally determinism-critical. The thing
that kept it out was a type, not a principle: there is now a pure `Meter`
struct, which is a fact about music rather than a schema shape, so the header
stays free of SQLite. Move yours over when convenient; mine walks the map
segment by segment, because dividing the whole position by the current meter is
right up to the first change and wrong after it. There is a test asserting it is
not the naive answer — and my first version of that test compared against a
different *correct* answer that happened to coincide, which is worth
remembering: a negative assertion is only worth having if it is aimed at the
actual wrong answer.

**8, both choices adopted.** No raw-tick gloss: `bar|beat|tick` is exact, so a
gloss duplicates rather than clarifies. Note positions clip-relative under the
signature in effect at the clip's start, held constant — following the global
map would rewrite every note line in a clip when an unrelated meter changed,
which defeats the point of relative positions. Both are rules in §8 now.

### One finding of my own, from running your gate

`tools/test_all.sh` calls `python`, and **macOS has shipped without a bare
`python` since 12.3 removed python2**. All three validators reported FAIL on
this machine for that reason alone. Since this script is the project's own
definition of done, the gate could not be green on any Mac.

It now detects `python3` then `python`, fails loudly if neither exists, and
prints which it chose so a surprising validator result is one line from being
explained. Windows is usually the mirror image — `python` present, `python3`
absent — so detection rather than a rename.

```
=== validators ===
  interpreter: python3 (Python 3.9.6)
PASS -- 846 checks across 10 suites, validators clean
```

---

## 2026-09-18 (third) — pinned dependencies, and 78 million goes at the reader

Branch `mac/pin-deps-and-fuzz` → open. Tasks A and B.

### A — `third_party/` is pinned now, and the pin is checked rather than declared

You were right that it was worse than it looked. `--single-branch` took whatever
the default branch pointed at, and for `nlohmann/json` that branch is `develop`.
So ADR-0022's seven-ABI result was a true statement about an upstream that can
move between two runs of the same commit of our code.

```
SQLiteCpp   3.3.3     59a047b8d3fe8574406ed73ab9fac0474e87bd03
json        v3.12.0   55f93686c01528224f448c19128836e7df245f72
bungee      v2.4.30   8cb6977d0c1a1b411ac320493b3c7f5182ed2d22
lockfree    3.0.1     ae6c4df124536218b0b1adfc21ab4921810a00a5
```

All four, not just the two we link today — `bungee` and `lockfree` are marked
`later` in the table and skipped by `--build-only`, but pinning them now costs
nothing and means step 5 does not start this argument again.

**The one design decision worth your attention: checkout is by TAG, and the
commit is the assertion.** Checking out the pinned commit directly would force
the tree to the right bytes and make the verification tautological — it would
paper over a re-pointed tag instead of reporting it, which is the only thing the
check is for. A tag is a mutable ref; matching its name proves nothing about the
bytes.

Three paths tested, not reasoned about:

```
happy       SQLiteCpp  cloning...    59a047b8d 2025-05-20  3.3.3     [MIT]
            json       cloning...    55f93686c 2025-04-11  v3.12.0   [MIT]
            -> cmake -> PASS -- 54 checks, 0 failure(s)

moved tag   FATAL: SRombauts/SQLiteCpp is not at its pinned commit.
              pinned : 55f93686c015...   (tag 3.3.3)
              fetched: 59a047b8d3fe...
            exit 1

bad tag     FATAL: ... fetched: unavailable
            exit 1
```

**Pinning cost us something visible, which I did not hide.** SQLiteCpp's `master`
was ahead of its own `3.3.3` release, so the vendored sqlite3 amalgamation went
**3.53.4 → 3.49.2**. That is the trade working: we now build against something
someone released. The new `provenance` job prints that number on every run so it
stays legible rather than becoming folklore.

**Two flags added, and CI now goes through your script.** `--build-only` fetches
the two entries the CMake tree links; `--third-party-only` skips `reference/`.
CI calls the script instead of cloning by hand, which deleted the parallel
dependency list `ci.yml` was carrying — there is nothing left for the two to
drift apart on, and the pin is now enforced before anything compiles rather than
checked afterwards.

`reference/` stays unpinned deliberately, and ADR-0024 says so out loud: it is
read for design and never compiled, and the point of having Ardour and Zrythm on
disk is to see what they do *now*.

ADR-0024 spends most of its length on the policy for **moving** a pin, since
that is the part that decides whether a pin means anything: never as a fix for a
red build (a re-pointed release tag is a supply-chain event, and copying the new
hash in destroys the only evidence it happened), one dependency per commit, a
named reason, and the full seven-ABI matrix green before it merges.

### B — the fuzzer found nothing, and here is why I believe that

```
Done 78205208 runs in 301 second(s)
stat::number_of_executed_units: 78205208
stat::average_exec_per_sec:     259817
stat::new_units_added:          482
stat::peak_rss_mb:              611
```

78.2 million executions under ASan **and** UBSan together, on the hardened reader
as merged. Zero crashes, zero timeouts, zero OOMs, no artifacts written.

The 30-minute run finished the same way:

```
Done 305126806 runs in 1801 second(s)
stat::new_units_added:          80
stat::peak_rss_mb:              489
```

305 million executions, still nothing. The number that makes that worth
something is `new_units_added`: **482 new coverage units in the first five
minutes, 80 in the following thirty.** The search is saturating rather than still
climbing, which is what you would expect of a parser this small and is the
difference between "found nothing" and "did not look long enough". The corpus
finished at 166 entries.

A clean fuzzing run is the easiest result in the world to fake, so two checks
before I ask you to believe it.

**1. The harness can fail.** I reinstated exactly the defect ADR-0023 removed —
deleted the `i >= h_.count` bound from `at()` — rebuilt, and ran it against the
seed corpus:

```
==44365== ERROR: libFuzzer: deadly signal
SUMMARY: libFuzzer: deadly signal
Test unit written to ./crash-36abbf4c17cff83a065bbd07b20b3c9933404347
```

Seconds, from the seeds alone, before any mutation.

**2. The corpus is actually deep.** A campaign that bounces off the fourcc check
80 million times proves nothing about the striding path. So I measured what the
106 surviving corpus entries do when fed to the real reader:

```
NoteRecord       accepted  25   records decoded 354
AutomationPoint  accepted  24   records decoded 575
ExpressionPoint  accepted  21   records decoded 348
```

and every `StreamError` variant is represented across the corpus — `TooShort`,
`BadFourCC`, `ZeroRecSize`, `Truncated`, `RecSizeUnknown` — with one exception,
which is the finding below.

**Seeds.** `tests/fuzz_seeds.py` writes 18, generated rather than committed as
binaries so a reviewer can read what each is for. Four are your bugs
(`finding-truncated-claims-1000`, `finding-ilp32-overflow`,
`finding-amplification-recsize1`, `finding-midfield-tear-recsize29`); the rest are
the boundaries of the rules ADR-0023 introduced — `rec_size` one below and one
above a released size, `rec_size` 0 and 0xFFFF, `count` 0xFFFFFFFF, a v2-wide
record, all reserved flag bits set — plus a valid blob of each type for the
mutator to work outwards from.

### → win: one finding, and it is about a branch, not a bug

**`StreamError::TooLarge` is unreachable on every 64-bit host, by construction.**

```
largest 'need' any header can ask for : 281470681677841  (2^48)
SIZE_MAX on this host (64-bit)        : 18446744073709551615
TooLarge reachable on 64-bit? NO
TooLarge reachable on 32-bit? yes
```

`count` is `u32` and `rec_size` is `u16`, so their product cannot exceed 2^48,
which is never greater than a 64-bit `SIZE_MAX`. The check is correct and it
should stay — it is the guard that makes the ILP32 arithmetic safe — but it is
dead code on LP64 and LLP64, which has two consequences worth writing down:

- **The CI fuzz job runs on x86_64 and structurally cannot reach it.** No amount
  of fuzzing on a 64-bit runner will ever cover that branch. The corpus tally
  above shows every other error state hit and this one at zero, which is not the
  fuzzer being weak.
- **The only coverage it can have is the ILP32 leg**, where your new test lives.
  That is the right place for it; I am flagging it so that "the fuzzer is green"
  is never read as "every rejection path is exercised."

A 32-bit fuzz build would close it in principle, but 32-bit sanitiser runtimes
are not packaged on the Ubuntu runners, so I have not tried to.

### Toolchain, because it will bite you if you ever run this on a Mac

Apple clang **ships no libFuzzer runtime at all** —
`libclang_rt.fuzzer_osx.a` is simply absent from the Xcode toolchain, and
`-fsanitize=fuzzer` fails at link. Homebrew LLVM has it, but LLVM 23 emits
objects Apple's `ld` rejects outright (`invalid r_symbolnum`), so `lld` is not
optional either. `brew install llvm lld`, then point CMake at both. CMakeLists
now detects Apple clang at *configure* time and prints exactly that, rather than
letting it surface as a link error later.

This also explains the ASan hang I reported in my first entry: it is Apple's ASan
runtime specifically. Homebrew LLVM's ASan runs fine, which is how the 78M-run
campaign happened at all.

### CI went red on the first run, and it was mine

Sixteen of seventeen jobs passed; `fuzz` failed in 32 seconds. Not the thing I
had flagged as risky — Ubuntu's clang does ship a libFuzzer runtime, the
preflight passed and the target built. It was an ordering bug in my own YAML:
the replay step passes `-artifact_prefix=/tmp/fuzz-artifacts/` while the `mkdir`
for it sat in the *next* step, and libFuzzer refuses to start when that
directory does not exist.

```
ERROR: The required directory "/tmp/fuzz-artifacts/" does not exist
```

Worth recording because of *why* I missed it locally: every local run pointed
`-artifact_prefix` at a directory that already existed, so the one precondition
the CI got wrong was the one my testing never exercised. Reproduced it here
before fixing it, then re-ran the corrected sequence verbatim.

### What I touched

`tools/fetch_external.sh` (handed to me, claimed), `.github/workflows/ci.yml`
(+`provenance`, +`fuzz`), `.github/scripts/`, `tests/fuzz_blob.cpp`,
`tests/fuzz_seeds.py`, `docs/DECISIONS.md` (ADR-0024), `collab/README.md` (claims
row, and the build instructions now say `--build-only` since the documented
command would otherwise still pull 630MB).

**And `CMakeLists.txt`** — the `ADI_BUILD_FUZZERS` option you asked for. It is
`OFF` by default and gated on clang, so the MSVC build is untouched: a default
configure mentions fuzzing zero times and does not produce the target. Verified
both. It is in your claimed path, so pull before you continue there.

---

## 2026-09-18 (later) — the findings report

Branch `mac/portability-ci` → open. This is the report the previous entry
deferred. Every finding below survived an adversarial pass whose instruction was
to refute it; 13 of 57 did not survive and are not listed.

**How this was run, because it changes how much the list is worth.** Six
independent audits — struct/ABI, parser-against-untrusted-input, SPEC §6.3
conformance, CMake, tooling, doc drift — each followed by a separate agent whose
only job was to disprove that audit's findings by reading the file, compiling
something, or running it. Then one pass asking what all six had missed. The
refutations earned their keep: they killed a claim that a read-then-write
round-trip loses the unknown tail (it is documented behaviour, not a bug), a
claim that `version` and `rec_size` are never cross-checked, and a claim that the
FourCC constants are unverified — and they corrected several severities downward.

**I checked two of the worst by hand rather than trusting the report.**

`operator[]` on a reader that failed. A 16-byte blob whose fourcc is wrong leaves
`recSize()` and `header().count` readable while `body_` is null:

```
$ /tmp/oob
error=fourcc does not match the expected stream kind  ok=0  count()=0  but recSize()=40 header().count=1000
calling r[0] on this !ok() reader...
exit: 139                                   # SIGSEGV
```

And the allocation amplification, which is worse than "no cap":

```
rec_size=1 count=100000000   file  95.37 MB -> all() allocates 3814.70 MB  (x40)

probing vector<NoteRecord>::reserve(0xFFFFFFFF) = 160.0 GB ...
  it SUCCEEDED (overcommit) — capacity 4294967295
```

macOS grants the 160 GB of address space rather than throwing, so there is no
`bad_alloc` to catch: the process dies later, while filling pages, with no error
path at all. The ceiling is `sizeof(NoteRecord)/1` = 40×, not unbounded — but 40×
with no cap on a file parser, and an OOM kill instead of an exception, is the
shape of the bug.

---

### For you — `src/adi/blob.hpp`

Ranked. Every one of these is in your claimed path, so none is fixed.

1. **`operator[]` has no bounds check and no `ok()` check.** Segfault above. In
   the `Ok` state it is just as bad quietly: on a 2-record blob, `r[2]` returns a
   zeroed record with no error, and `r[1000000]` reads ~40 MB past a 96-byte
   buffer and still returns. The fix is one line before the memcpy:
   `if (!ok() || i >= h_.count) return Rec{};` — consistent with `count()`, which
   already guards with `ok()`.

2. **The 32-bit `size_t` overflow** in the length check at `blob.hpp:226-227`.
   `count` is u32 and `rec_size` is u16, so the product needs 48 bits:

   ```
   header claims count=131072 rec_size=32768
     need, 64-bit size_t : 4294967312
     need, 32-bit size_t : 16   <-- wrapped to header-only
   ```

   A 16-byte blob then passes validation while `count()` reports 131,072. Fifteen
   distinct power-of-two `rec_size` values admit an exact wrap. The division form
   is the fix — `h_.count != 0 && h_.rec_size > (SIZE_MAX - sizeof(StreamHeader)) / h_.count`
   — and note that **CI's ILP32 leg will stay green until a test exercises it.**
   Compilation proves layout; only a test proves this.

3. **`all()` has no cap.** See the 40× measurement above.

4. **A `rec_size` that lands mid-field tears that field.** ADR-0008 promises
   missing fields take their documented defaults. Demonstrated:

   ```
   NoteRecord.flags is u16 at offset 28; full record is 40 bytes.
   rec_size=28 -> ok=1  flags=0x0000   (writer wrote 0xBEEF)
   rec_size=29 -> ok=1  flags=0x00EF   <-- half a field: neither the value nor the zero default
   rec_size=30 -> ok=1  flags=0xBEEF
   ```

   `error()` is `Ok` throughout. An old writer would never emit 29; a hostile file
   will. Either the reader rejects a `rec_size` that is not a documented width, or
   ADR-0008 has to say what a partial field means.

5. **`Curve::Bezier = 5` is not in the spec.** SPEC §6.3.2 enumerates 0–4 only.
   A conforming third-party reader built from the document degrades 5 to `Hold`.
   Either the spec gains the value or the code loses it — and note `tempo_map.curve`
   in `schema.sql` is a *different* enum where bezier is 2, so whichever way this
   goes, say so explicitly.

6. **`fourcc` is `std::uint32_t` where SPEC says `char[4]`.** Byte-identical
   today — I verified all six constants byte by byte and they are correct. The
   problem is the remediation advice: `blob.hpp:33` says the fix for a big-endian
   host is "byte-swapping accessors", and byte-swapping a `char[4]` writes
   `'TONA'` to disk. A `char[4]` member compared against `{'A','N','O','T'}` is
   endian-free and needs no accessor.

7. Lower, briefly: `writeStream` stamps `SortedByTime` unconditionally without
   checking sortedness, and the reader never validates it. Nothing binds a FourCC
   to its record type, so `writeStream<NoteRecord>(FourCC::Automation, …)` is
   written and read back as valid. `writeStream`'s two narrowing casts are
   unguarded. "MUST be 0" on reserved fields is neither enforced on write nor
   checked on read. `Rec` has no `is_trivially_copyable` constraint, so a memcpy
   into a non-trivially-copyable type compiles silently. `StreamReader` holds a
   non-owning span, and the implicit `vector`→`span` conversion makes a one-line
   use-after-free compile clean and report `ok()`. `hasUnknownTail()` tells callers
   they MUST preserve the original bytes, but the class stores only the body and
   exposes no accessor to hand them back — the ADR-0008 mechanism is one accessor
   short of usable. The IEEE-754 `static_assert` checks only `sizeof`, so the
   claim it is captioned with is not actually proved.

### For you — `tools/`

- **Both blob fixtures in `validate_schema.py` are malformed.** Flagged in the
  previous entry; repeated here because it is the one that is actively wrong in a
  validator. 15 bytes and 14 bytes for a 16-byte header; `StreamReader` returns
  `TooShort` for both.
- `validate_schema.py` dies with an unhandled `TypeError` when check [5] fails,
  which truncates check [6] and the `FAILED` summary.
- `fetch_external.sh` cannot fetch a subset, which is why CI clones its two build
  dependencies directly rather than calling it. A `--third-party-only` flag would
  let CI use the script instead of maintaining a parallel list; until then the
  `deps-match-fetch-script` step fails the build if the two lists diverge.
- **`nlohmann/json` is being tracked on `develop`.** `git -C third_party/json
  rev-parse --abbrev-ref HEAD` → `develop`. SQLiteCpp is on `master`. So the
  reference implementation's ABI work is validated against an upstream unstable
  branch that can move under us between two runs of the same commit. Pinning both
  to a tag is the fix; that is your file, and it is the single highest-value
  change in it.

### Documentation, where docs disagree with docs

These are drift, not portability, and I have not touched them. Highest first:

- **SPEC §8.2 puts the current-undo-branch pointer in `session_state`;
  `schema.sql` puts it in `op_branches.is_current` and seeds no such key.** Two
  normative documents describing one pointer differently.
- **`AI-AGENT.md`'s safety table says everything the agent does is undoable;
  `OPS.md` grants the Apply tier ten explicitly non-undoable ops.** This one is
  load-bearing for the project's whole premise.
- **`FEATURES.md` §12 says "exactly five gaps" and that all five are in SPEC §12.**
  Its own tables mark eleven, and two of the five are not in SPEC §12. This is the
  "152 ops" failure mode again: a count in prose that its own tables contradict.
- `EXTERNAL-CODE.md` still calls the MAGDA/Tracktion strategy "unrecorded and
  open" after ADR-0018 decided it. `schema.sql` still marks `ops.payload`
  "encoding TBD" after ADR-0016 decided it. SPEC §12 still lists the op vocabulary
  as undecided. `AI-AGENT.md` §9 lists two questions that are closed. README's
  CBOR open question cites OPS.md §10, which is about non-undoable ops.
- **`README.md` still says "Status: design. No code."** and marks step 4 "next".
- ADR-0016 cites "ADR-0018" for the op registry, which is actually ADR-0020 —
  a direct consequence of the duplicated 0018 number.
- `collab/README.md` calls `fetch_external.sh` "the two dependencies"; it clones
  nine repos. Its claims table names a branch that never appears in your log,
  while the branch that did merge was never claimed.
- README says the validators need "any Python"; they need ≥ 3.7, and `schema.sql`
  needs SQLite ≥ 3.9.0.

The two you already logged both stand under scrutiny: OPS.md §8 rule 2 mandates
integer CBOR map keys that nlohmann cannot encode *or* decode, and the RFC 8949
§4.2 determinism claim is wrong because §4.2.1 orders by encoded bytes
(length-first) while nlohmann orders by `std::less<std::string>`. Three documents
publish the integer-key rule and no ADR amends it yet.

### Two "refutations" that are not refutations

The CMake verifier refuted the 3.21-vs-3.25 finding and the `add_compile_options`
leak with "already fixed; the finding re-reports a state that no longer exists."
True — I fixed both before it ran. They were real. Recorded here so the tally is
not read the wrong way round.

### What I changed in this second pass

All in my lane. `.github/workflows/ci.yml` gains a `strict` job: the project's own
flag set plus `-Werror` over our three translation units, under a hardened
standard library (`-D_GLIBCXX_ASSERTIONS`, and `_LIBCPP_HARDENING_MODE` on macOS).
The flag wall in CMakeLists.txt sets no `-Werror` on either branch, so it is
advisory and a build stays green with any number of new warnings; this applies the
gate from outside rather than changing a policy in your file. All three legs were
run locally first — Apple clang 17.0.0 and GNU 16.2.0 — and all were silent.

Also `adi_daw/.gitattributes`, which did not exist: `*.sh text eol=lf`. Git for
Windows and the Actions Windows runners default to `core.autocrlf=true`, which
delivers `fetch_external.sh` with CRLF endings, and it then dies on its own
`set -euo pipefail` because the shell reads the `\r`.

And `ADR-0022`, recording why CI is shaped by ABI, why one job is required to
fail, and why a green matrix is not evidence the 32-bit overflow is fixed.

### The one test that should exist and does not

A **golden-byte vector**: a hand-written hex literal of a known three-note `ANOT`
blob, asserted in both directions — `writeStream` must produce exactly those
bytes, and `StreamReader` over exactly those bytes must produce exactly those
records. Roughly thirty lines, and it is the only thing that would simultaneously
prove little-endianness, `fourcc` byte order, every §6.3.1 offset *on the wire*
rather than in memory, the absence of padding under `#pragma pack`, and the
header's own field placement.

None of that is proved today. `testLayout` memcpys a record into a buffer and
memcpys it back into same-endian scalars — it is byte-order tautological and
passes identically on a big-endian host, despite a comment claiming it checks
"that the bytes on the wire actually carry what we think they do". Every existing
check round-trips through the same struct that would be wrong. A golden vector is
also the artifact the repo is missing: a correct on-disk fixture, which is what
those two malformed hex strings in `validate_schema.py` were trying to be.

`tests/` is yours, so it is yours to write — but if you would rather I did it,
say so in your log and claim it over to me.

---

## 2026-09-18 — SPEC 6.3 holds on clang/arm64, and on two more ABIs besides

Branch `mac/portability-ci` → open.

**Headline: nothing in `blob.hpp` is wrong on this platform.** No `static_assert`
fired, no struct needed changing, and no spec claim turned out to be false. The
four record layouts are now proved on **three compilers and two architectures**
rather than one of each:

| compiler | arch | stdlib | StreamHeader | NoteRecord | AutomationPoint | ExpressionPoint |
|---|---|---|---|---|---|---|
| MSVC 19.44 (yours) | x86_64 | MS STL | 16 | 40 | 32 | 24 |
| Apple clang 17.0.0 | arm64 | libc++ | 16 | 40 | 32 | 24 |
| Apple clang 17.0.0 | x86_64 | libc++ | 16 | 40 | 32 | 24 |
| GNU 16.2.0 | arm64 | libstdc++ | 16 | 40 | 32 | 24 |

The x86_64 row is not a second machine: `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`
compiles both slices in one build, so the `static_assert`s are evaluated twice
against two different ABIs, and Rosetta then runs the x86_64 slice for real.

**Results.** All of this is verbatim.

```
$ cmake -S adi_daw -B adi_daw/build -DCMAKE_BUILD_TYPE=Debug && cmake --build adi_daw/build
$ ./adi_daw/build/adi_tests
adi_tests -- SPEC 6.3 binary layouts

[layout]
[round trip]
[ADR-0008 striding]
[malformed input]
[automation + expression]

PASS -- 31 checks, 0 failure(s)

$ ./adi_daw/build/adi_tool versions
sqlite3   3.53.4
SQLiteCpp 3.3.3
C++       202002
```

x86_64 slice, under Rosetta, same binary:

```
$ arch -x86_64 /tmp/adi-univ/adi_tests
PASS -- 31 checks, 0 failure(s)
```

GNU 16.2.0, whole tree through CMake:

```
$ CC=gcc-16 CXX=g++-16 cmake -S adi_daw -B /tmp/adi-gcc -DCMAKE_BUILD_TYPE=Debug
-- The CXX compiler identification is GNU 16.2.0
$ cmake --build /tmp/adi-gcc && /tmp/adi-gcc/adi_tests
PASS -- 31 checks, 0 failure(s)
  StreamHeader        16 bytes
  NoteRecord          40 bytes  (ANOT)
  AutomationPoint     32 bytes  (AAUT)
  ExpressionPoint     24 bytes  (AEXP)
```

Both validators, on Python 3.9.6:

```
$ python3 adi_daw/tools/validate_schema.py   ->  PASS -- 0 problem(s)
$ python3 adi_daw/tools/validate_ops.py      ->  PASS -- 0 problem(s)
```

Release build: clean, 31/31. UBSan (`-fsanitize=undefined
-fno-sanitize-recover=all`): clean, 31/31 — so nothing in the packed-struct
handling is actually misaligned at runtime on arm64, it really is memcpy all the
way down. ASan could not be used: it hangs before `main` on this host even for
`int main(){return 0;}`, which is an Xcode 17 / macOS 26.6 problem, not ours. CI
runs ASan-capable runners, so that gap closes there, not here.

**Found, and fixed, in `CMakeLists.txt`.** Both are portability defects rather
than preferences, and both are in your lane — pull before you continue.

1. **`cmake_minimum_required(VERSION 3.21)` was wrong.** The `SYSTEM` argument to
   `add_subdirectory` was added in **CMake 3.25** (`cmake --help-command
   add_subdirectory` → `.. versionadded:: 3.25`). Anyone on 3.21–3.24 gets a hard
   configure failure on line 59, not a warning. Declared minimum is now 3.25.

2. **The non-MSVC warning set was landing on `sqlite3.c`.** `add_compile_options`
   is directory-scoped and ran *before* `add_subdirectory`, so `-Wconversion
   -Wsign-conversion` applied to the dependencies too:

   ```
   before:  1306 warnings   (1305 from third_party/SQLiteCpp/sqlite3/sqlite3.c,
                             1 from SQLiteCpp/src/Column.cpp, 0 from ours)
   after:      0 warnings
   ```

   `SYSTEM` on `add_subdirectory` does not and cannot help — it marks the
   dependency's *include directories* as system, which silences diagnostics from
   its headers when we include them, not diagnostics raised while compiling its
   own `.c` files. The flags now live on an `adi_warnings` INTERFACE target that
   only our three targets link. Verified both directions from
   `compile_commands.json`: our three TUs still get the full set, all 8
   third_party TUs get none, and a deliberately-narrowing line added to
   `blob.cpp` still produced `-Wshorten-64-to-32` (then reverted).

   I did **not** add `-Werror`, because that is a policy change rather than a
   portability fix and it is your file. For the record it would pass today: the
   full set plus `-Werror` is silent on all three of our TUs under both Apple
   clang 17 and GNU 16.2. I had predicted gcc would flag the integer promotions
   at `blob.hpp:227` and `:231`; it does not.

**Added, in my lane.**

- **`.github/workflows/ci.yml`** — `macos-latest`, `windows-latest`, plus four
  more legs, because two compilers is still not a portability claim. Seven ABIs:
  clang/arm64, clang/x86_64, clang/x86_64+libstdc++, gcc/x86_64, gcc/arm64,
  gcc/i386 (ILP32), MSVC/x86_64 (LLP64). Both Python validators run on 3.9, 3.11
  and 3.13, and on Windows — the documents they parse are full of en-dashes, and
  a bare `open()` there would decode them as cp1252. They pass `encoding="utf-8"`
  explicitly; the Windows leg is there to keep it that way.

  CI does **not** run `fetch_external.sh`. It clones the two dependencies the
  build actually needs. Cloning ardour and zrythm on every push would make an
  unrelated upstream outage look like our failure. A `deps-match-fetch-script`
  step fails the build if CI's two repos stop matching the `THIRD_PARTY` table in
  your script, so the two lists cannot drift silently.

- **A leg that is supposed to fail.** `abi-big-endian` cross-compiles `blob.cpp`
  for s390x and requires the compile to fail *with the words "byte-swapping
  accessors" in the diagnostic*. `blob.hpp:34`'s endianness `static_assert` has
  never been watched to fire; until it is, it is a comment rather than a
  guarantee. If someone later deletes it, that job goes red instead of us
  shipping a reader that mis-decodes every field on a big-endian host.

- **`.github/scripts/check_spec_layout.py`** — this one matters more than the
  rest. `adi_tests` proves the layouts are self-consistent; the `static_assert`s
  prove they equal four numbers written in a header. **Nothing proved those
  numbers were the ones `SPEC.md` publishes** — and SPEC.md is what a third-party
  implementer reads. This parses the sizes out of the spec's own prose and
  headings and diffs them against `adi_tool layout`, per ABI. It is the standing
  rule applied to the four numbers that did not have a validator yet. I tested it
  against four ways of being wrong — spec and build disagreeing, the spec wording
  drifting out from under the regex, `adi_tool`'s output format changing, and the
  binary missing — and it exits non-zero on all four.

**Verified rather than assumed, while I was in there.** Every count README.md
asserts is correct: 38 tables, 27 explicit indexes, 174 ops, nine repos in
`fetch_external.sh`, 22 ADR entries. The validators are CWD-independent
(`__file__`-relative, so `python adi_daw/tools/...` works from anywhere) and both
exit 1 on real failure — I proved that by corrupting copies in `/tmp`, not by
reading the code.

**→ win:** three things for you, and one process note.

1. **`DECISIONS.md` has two `ADR-0018` entries** — the original `OPEN` one and
   `ADR-0018 (revised)`. Appending the revision was right; reusing the number was
   not, since `collab/README.md` asks for "a new numbered ADR". "ADR-0018" is now
   ambiguous to cite, and the highest number in the file is 0021, so the next
   free number is 0022. I have not touched it — superseding by editing is exactly
   what the rule forbids, and the fix is another append, which is yours to write.

2. **A detailed findings report is not in this entry yet.** I ran a structured
   audit across six dimensions with adversarial verification of each finding;
   three of the six verification passes were cut short by a credit limit, and I
   am re-running them rather than reporting findings that have not survived a
   refutation pass. One that has already been verified and independently
   reproduced, because it is in a validator and you will want it early:
   **both blob fixtures in `validate_schema.py` are malformed.**

   ```
   event_streams   X'414E4F540100280001000000010000'  -> 15 bytes
   note_expression X'4145585001001800000000000000'    -> 14 bytes
   ```

   SPEC 6.3 says the header is 16. The first is one byte short and declares
   `count=1, rec_size=40` with an empty body; the second is two bytes short.
   `StreamReader` returns `TooShort` for both. The schema validator is currently
   inserting blobs the reference reader rejects.

3. **I touched `CMakeLists.txt`**, which your claims row covers. Two changes
   only, both above, both verified by a clean rebuild. Pull before you continue
   in that file.

**Process note.** I own `.github/**` and have added a claims row for it. The
monorepo's existing workflows are named per project (`console-plugin.yml`,
`midi-helper.yml`); `ci.yml` is the name the mission specified, but if a third
C++ project ever lands here, `adi-daw.yml` is the convention it should have.

---

---

## (no entries yet)

First mission is described in `collab/README.md` and in the handoff at the
bottom of `collab/win.md`. Append your first entry here when you have something
to report — including a negative result. "Built clean on clang/arm64, no
portability issues found" is a valuable entry, not an empty one.

Suggested shape for an entry:

```markdown
## YYYY-MM-DD — short title

Branch `mac/...` → merged / open.

**Did.** What changed and why.
**Found.** Anything surprising, especially anything that contradicts a doc.
**Results.** Actual command output, not a summary of it.
**→ win:** anything the other agent needs to know or act on.
```

---

## 2026-09-20 — VST3 hosting, the nine device ops, and a finding about JUCE

Branch `agent/mac-dev` (was `mac/vst3`; the director moved both agents to
per-agent branches). ADR-0057. 1192 checks across 15 suites, 72 ADRs.

### The protocol change that produced the first finding

The director's instruction, after win's handoff and my own reports collided
once too often: **treat every technical claim in a handoff as an intention, not
a fact, and grep before overriding or calling anything.** It paid immediately.

win's brief said `latencySamples()` "joins `tailSamples()` on `Node`
(ADR-0058)". It did not:

```
grep -rn "latency\|Latency" src/ tests/   →   no matches
```

ADR-0058 is `DECIDED`, in detail, with the default and the reasoning — and no
line of C++ behind it. This is not carelessness, it is a **Blueprint vs Reality
gap**: by the time a decision reaches a handoff it reads like a landing. 42
ADRs were written in two days; 13 are marked `DECIDED (direction)` and ADR-0058
is marked plain `DECIDED` while being unbuilt, so the label that distinguishes
the two is not holding. Worth a mechanism, and it is not mine to invent alone.

I have implemented decision 1. Decisions 2–5 (the compensation pass over the
levelled schedule) remain unbuilt and are win's.

### What is built

`src/juce/device_model.{hpp,cpp}` — the contract, with **no JUCE and no VST3 in
it**, so it compiles into `adi_core` and its 55 checks run on all seven ABIs.
`DeviceInstance` is the format boundary, `DeviceNode` is an `engine::Node`
wrapping one, `MissingDevice` is ADR-0011's placeholder.

`src/juce/vst3_host.{hpp,cpp}` — the one adapter. `AudioPluginFormatManager`
with a single `VST3PluginFormat` added by hand; **not** `addDefaultFormats()`,
which is a one-line difference that would put an AU host on every Mac.

The nine device ops of OPS.md §9.7 exist for the first time, plus
`chain.create`/`chain.delete` — a device's `chain_id` is `NOT NULL` with
foreign keys on, so without those the other nine cannot be reached through the
op log at all, and nine ops the corpus cannot exercise are nine ops whose
freedom from ambient state is unproven.

### Three findings worth your time

**1. The round-trip corpus cannot see a per-row defect behind a cascade.**

I put the device ops into the corpus, planted a `setParam` inverse that records
`0.0` instead of absence, and the corpus stayed green — 58 checks, 0 failures.
Because undoing `device.insert` deletes the device and `plugin_params` cascades
on `device_id`, so the spurious row is swept away before the comparison against
a blank project happens.

The corpus is not wrong. But **a whole-project oracle cannot see a defect in a
row that something else is about to delete**, and that generalises past this
case. `tests/test_device_ops.cpp` undoes exactly one transaction per check;
five defects planted there, five caught.

One was planted wrong first time: the move fixture seeded its device at `ord 0`,
so a defect capturing `0` was indistinguishable from a correct capture. Second
time I have made that exact mistake — comparing a right answer against a
*different* right answer that coincides. The fixture seeds at `ord 2` now.

**2. `setPlayConfigDetails` would have disabled every sidechain.**

A JUCE assertion caught it during `prepare`. That function calls
`disableNonMainBuses()` — its own comment says "the user does not want any
side-buses or aux outputs". ADR-0043 requires a **live sidechain** to prevent
suspension and your ADR-0056 added `Bus::Sidechain` to express it, so a
compressor keyed from another track would have had its key input switched off
by the host, at prepare, silently. Now `setRateAndBufferSizeDetails`, which
sets the rate and block size and leaves the plugin's bus layout alone.

**3. JUCE's VST3 host path cannot carry MPE+, and the route out is not a rewrite.**

This one lands on ADR-0054 and I checked it against JUCE 9.0.2's source rather
than asserting it, having nearly written down the opposite conclusion first.

`processBlock` takes a `juce::MidiBuffer` and `juce_VST3Common.h`'s
`toEventList` iterates exactly that. Three facts settle it:

- `createNoteOnEvent` sets `e.noteOn.noteId = -1`, and so do `createNoteOffEvent`
  and the poly-pressure case. **VST3 anchors note expression to `noteId`**, so
  per-note values cannot be addressed even if they could be sent.
- Nothing in JUCE constructs a `kNoteExpressionValueEvent`. The only occurrence
  is the case on the way *in*, which converts one to `{}`.
- Velocity is `normaliseMidiValue`, which is `value / 127.0f`.

Also: `toEventList` caps at `maxNumEvents = 2048` and `break`s, silently. Same
number your ADR-0056 arithmetic derived, same silent-drop shape, in the
framework rather than in us.

**But JUCE 9.0.2 hands a host the raw interface.** I was wrong for about ten
minutes about this — `getExtensions` is `= delete`, which reads like the hatch
was removed, and it was in fact *replaced* by typed accessors.
`AudioPluginInstance::getVST3Client()->getIComponentPtr()` returns
`Steinberg::Vst::IComponent*`, and `IAudioProcessor` and
`INoteExpressionController` are a `queryInterface` from there. Confirmed on a
real plugin: the probe reads it back non-null from FabFilter Timeless 2.

So: discovery, instantiation, parameters and opaque state stay JUCE's; **the
event path for instruments becomes ours**, against an SDK already vendored
inside JUCE. That is the first half of the CLAP host ADR-0052 mandates rather
than a detour.

**Until it is built the event path is EMPTY, not approximate**, and
`supportsNoteExpression()` returns false. An empty MIDI buffer is a plugin that
makes no sound, which is a bug report. A 7-bit buffer is a plugin that sounds
nearly right, which is the thing that ships.

### The schema change, which is what made the ops possible

`plugin_params` and `plugin_state` each had a surrogate `INTEGER PRIMARY KEY`
beside a `UNIQUE` natural key, referenced by nothing. ADR-0021 §7.3 says an op
that INSERTs carries the row's id in its payload — so `device.setParam`, which
is coalescable and fires on every knob movement, would have had to invent an id
or ask SQLite whether the row existed, and the second is the ambient read §7.3
forbids.

Both are now keyed on their natural key, `WITHOUT ROWID`. Every write is an
UPSERT with nothing to allocate. It is your ADR-0065 move — absence and
subtraction rather than allocation — arrived at from the other end, and I only
saw it because the design panel's author-symbol entry had made the argument.

`validate_schema.py` fails if a surrogate reappears; proved by putting one back.

### Measured, not asserted

Real hosting, 38 plugins found on this machine. FabFilter Timeless 2: 842
parameters, tail reported as `9223372036854775807` — `kInfiniteTail` mapping
straight through, which is why ADR-0055 chose the same constant as VST3 — and
state byte-identical across save/load/save at 3985 bytes.

And the panel's finding, confirmed on a real plugin rather than argued: the
first parameter has a normalized value in range and **no real value**. A VST3
lane is `normalized` by necessity.

### → you

1. **ADR-0058 decisions 2–5 are still unbuilt.** `Node::latencySamples()` now
   exists for the compensation pass to read. A bypassed device reports 0 for
   both tail and latency, and `always_process` forces an infinite tail but
   deliberately does **not** touch latency — they answer different questions.
2. **`Vst3Device::latencyEpoch()` is ADR-0066's trigger.** It is an atomic
   counter bumped from `audioProcessorChanged`, and that handler does nothing
   else. It never calls into the graph.
3. **A plugin node attaches to a planned track node.** `GraphPlan::indexOf` is
   the interface I want, as you offered; I have not needed it yet because no
   realisation step exists.

---

## 2026-09-20 — ADR-0072 (aux sends) and both ends of the MPE+ pipeline

Same branch, three more commits. 1270 checks across 16 suites, 38 more in the
JUCE probe, 73 ADRs.

### ADR-0072 supersedes your ADR-0067, on the director's ruling

Aux sends are abolished. Your three arguments are in the superseded entry and
the new one answers them rather than ignoring them:

**Your strongest objection — a rack with parallel chains is a DAG too — is
correct about the arithmetic and misses where it lives.** A rack declares ONE
latency upward (ADR-0060, and ADR-0062 already requires the multiband splitter
to do it), so the parallelism is encapsulated and the top-level graph stays a
linear progression. With sends the top-level graph is itself an arbitrary DAG
and every path through it is a place the compensation can be wrong. One place
that must be right beats arbitrarily many.

**Your first objection turned out to be false in practice, and that is the part
worth your attention.** ADR-0067's load-bearing sentence was *"We already
compensate them. ADR-0058's rule is arrival = max over inputs..."* We do not.
Audited by type and function: zero occurrences of `arrival`, `compensat` or
`DelayLine` anywhere in `src/`. Decisions 2–5 are unbuilt and decision 1 was
written days later, by me. A statement about the design reading as a statement
about the code — the same gap as the `latencySamples()` one, this time holding
up the load-bearing argument of the ADR being superseded.

**Your third objection stands and is recorded as a real cost.** Partial sends
are genuinely gone. Forty tracks into one reverb is still one instance via a
group with a rack on it, but thirty percent of one track and ten percent of
another is not expressible any more.

**The format keeps admitting `'send'`,** and that is not a softening. It is
SPEC §7.4's existing split applied unchanged — ADI hosts VST3 and CLAP while
`plugin_refs.format` keeps admitting `au` forever, because refusing to host
costs us code we do not write and refusing to *name* costs a user their
session. Older files and converter output still open, the row survives the
save, and the planner reports it instead of silently rewiring.

One of your tests asserted the superseded behaviour (`"a send sums into its
destination"`). Updated in place and labelled, not deleted.

### Both ends of MPE+

`src/adi/engine/mpe_input.{hpp,cpp}` — bytes in, `Event` out, no MIDI byte
surviving. `src/adi/engine/note_expression.hpp` + `src/juce/vst3_events.*` —
`Event` out to a `kNoteExpressionValueEvent` with a real `noteId` and a double.

Four things that are wrong by default and would not be noticed:

- **MPE's per-note bend range is ±48, not MIDI's ±2.** Assuming the default
  transposes every gesture by a factor of 24.
- **VST3's tuning range is ±120**, so a full MPE bend lands at 0.7, not 1.0.
  Normalising both to 0..1 — the obvious thing — transposes a full bend by two
  and a half octaves.
- **Pitch-bend centre is 8192**, so there are 8192 steps below and 8191 above.
- **An LSB whose MSB was never seen is not promoted to 14 bits.** That invents
  precision the wire did not carry.

The end-to-end assertion is the mandate in one line: all 16384 bend positions
stay distinct through `bendToSemitones → semitonesToVst3Tuning`. Zero
collisions. It fails the moment anything narrows.

**Planting exposed a weak test of mine.** Taking the note id from the channel
failed only one unrelated check, because "the same key on two channels gets two
ids" passes happily for a channel-derived id. The test that separates minted
from derived is the same key played twice on ONE channel. Two negative tests
in two sessions that did not actually test the thing they named.

Six type ids and `sizeof(NoteExpressionValue)` are static_asserted against the
real SDK in the JUCE build, because a wrong-but-valid type id makes a note
brighter instead of sharper. Proved by planting `Brightness = 6`.

### → you, one item

**Not wired into `process()` yet.** Driving `IAudioProcessor::process`
ourselves means taking over bus setup, `ProcessData` and `IParameterChanges`
from JUCE. Until then the MIDI buffer stays EMPTY rather than 7-bit, and
`supportsNoteExpression()` still reports false. That is mine and I am on it —
do not start it.

Your `Bus::Sidechain` survives `prepare` now; see the `setPlayConfigDetails`
finding in the previous entry. The compensation pass needs that, because
ADR-0058 d5 compensates sidechain edges separately and cannot if the host
switched the bus off.

---

## 2026-09-20 — ADR-0073: I had ADR-0057's split wrong, and CI caught my job placement

Three more commits on `agent/mac-dev`. 74 ADRs, 1270 checks across 16 suites,
four validators, 54 in the JUCE probe.

### ADR-0073 corrects ADR-0057, which is mine

ADR-0057 decision 7 said *"discovery, instantiation, parameters and opaque
state stay JUCE's; the event path becomes ours."* **The parameters half is
wrong**, and I found it by reading `processAudio` before building on it rather
than after.

One `ProcessData`, filled in one place: `inputParameterChanges`,
`outputParameterChanges`, the audio buses, and the `MidiBuffer → IEventList`
hop. Then `cachedParamValues` is flushed **into** `inputParameterChanges`, and
`outputParameterChanges` is read back out — both inside that same function. So
a host that calls `processor->process()` itself bypasses JUCE's parameter
plumbing in both directions.

Parameters and events are not adjacent paths. They are fields of one struct
passed to one call, and owning either means owning both.

Also checked, because it would have been the cheap answer: **JUCE 9.0.2's VST3
host has no `universal_midi_packets` reference at all**, so MIDI 2.0's 32-bit
per-note controllers are not a way round the `MidiBuffer` either.

`Vst3ParamChanges` and `Vst3ParamQueue` are built and tested. Three defects
planted, three caught: unsorted points, a second queue for a ParamID that
already has one, and unclamped values.

The consolation is real: this is most of the CLAP host ADR-0052 mandates, since
a CLAP host must own its process call, its event queue and its parameter events
regardless. Doing it for VST3 first produces the shape both need — ADR-0052
decision 4 arriving from a third direction.

### CI caught a job-placement bug of mine, and now a validator does

The VST3 probe step landed in `dependency provenance`, which never builds JUCE.
I had appended it by anchoring on "the next top-level key", assuming something
followed `juce:` — it is the **last** job in the file, so the step went into the
one before it. CI failed with "adi_vst3_probe was built but cannot be found",
which is true and useless.

Fixed, then made structural, because this class of error is only visible after
a push and a five-minute round trip:

- **`tools/validate_ci.py`**: a step referencing a build tree must be in a job
  that creates it, checked per directory rather than for JUCE specifically.
  Also duplicate step names in one job — the signature of an insert that ran
  twice — and a missing `runs-on`. No `yaml` module; macOS ships python3
  without PyYAML and this script is part of the definition of done.
- **`test_all.sh` and `ci.yml` now DISCOVER `tools/validate_*.py`** instead of
  listing them. That list was the same anti-pattern that made the `-Werror`
  gate skip every file added after it was written. `validate_ci.py` was picked
  up by both without either list being touched, which is the proof rather than
  the claim.

Planting the exact defect CI found fails the validator locally in under a
second, naming the job and the missing `ADI_WITH_JUCE=ON`.

**And the VST3 probe has now run in CI for the first time** — macOS runner, 28
checks, 0 failures, correctly skipping the plugin half with "found 0
plugin(s)". It had never executed anywhere but this Mac.

### → you, unchanged

`Node::latencySamples()` exists for ADR-0058 decisions 2–5. The takeover of
`IAudioProcessor::process` is mine and still unfinished — the event list and
the parameter queue are built and tested, the `ProcessData` assembly and bus
wiring are not. `supportsNoteExpression()` still returns false and the MIDI
buffer is still empty rather than 7-bit.

---

## 2026-09-20 — ADR-0083/0084, and the coalescer measured against real plugins

### → win: your defaults, measured

You asked for measurements to replace the 50 ms / 500 ms guesses. FabFilter
Pro-Q 3 is installed here, which is the canonical case you named.

**Method note first, because my own first attempt was wrong.** I set the mode
parameter, pumped the message loop 400 ms, set the next — and duly measured
~1000 ms gaps, which were *my sweep rate*, not the plugin's. Rapid-fire
changes are what a user dragging a control looks like, and those are the gaps
below.

| plugin | latency at rest | switched | swing | reports | largest gap | burst span |
|---|---|---|---|---|---|---|
| Pro-Q 3 3.2.4.0 | 0 | 320 / **5120** | **5120** | 5 | **26 ms** | **73 ms** |
| Pro-Q 2 | 0 | 320 / 5120 | 5120 | 5 | 26 ms | 75 ms |
| Pro-MB | 960 | 4032 | 3072 | 1 | — | — |
| Pro-L 2 | 3115 | (no mode param) | — | — | — | — |

**Your quiet period is fine and your ceiling never trips.** Largest observed
gap between consecutive reports is 26 ms, so 50 ms coalesces the burst with
about 2× margin. The longest burst is 75 ms against a 500 ms ceiling.

**Your headroom default is the problem, and it is not a tuning question.**
`latencyHeadroom_` defaults to **0**, so out of the box no latency change ever
fits the ring and every one escalates to a rebuild — ADR-0079's cheap path
never runs. Your comment says linear-phase EQ "sits in the low thousands of
samples"; the measured number is **5120**, and the swing from natural phase is
**4800**. A headroom of 2048 or 4096 — both plausible-looking round numbers —
would miss Pro-Q 3 entirely.

I would default it to 8192: covers the measured worst case with margin, is a
round block multiple, and by your own arithmetic costs 2 × 8192 × 4 = 64 KB
per compensated edge. That is your call, not mine, which is why I have not
changed it.

One more from the table: Pro-L 2 sits at **3115 samples at rest**. Static, not
a swing, but it says absolute latencies in the thousands are ordinary rather
than a corner, which matters for ADR-0058's compensation as much as for the
ring.

`adi_vst3_probe --latency-probe "<name>"` reproduces all of it.

### ADR-0084: neither of your two options was needed

CLAP already distinguishes restart causes. `clap_host_latency.changed()` and
`clap_host_audio_ports.rescan(flags)` arrive *before* the generic
`request_restart`. We only ever saw the generic one because
`ClapHostGlue::getExtension` returned `nullptr` for everything — we offered no
host extensions, so a plugin had no channel to tell us. That was my defect.

`latencyChanges()` is your cheap path, `portChanges()` escalates, and only
shape flags count: `CHANNEL_COUNT`, `PORT_TYPE`, `IN_PLACE_PAIR`, `LIST`.
`NAMES` and `FLAGS` are cosmetic and a rebuild for a renamed port is a graph
swap for a label. A bare restart with nothing before it escalates and is
counted as `unexplainedRestarts()`.

**One thing I could not settle, and it bears on your escalation work.** CLAP
says latency may change *only during `plugin->activate`*. So the CLAP sequence
is restart → deactivate → activate → new latency, while our cheap path
re-reads latency *without* reactivating. Right for VST3, possibly stale on
CLAP. It wants a CLAP plugin that moves its latency, and none of the 38 here
is CLAP.

### And the other gap you found

`request_callback` incremented a counter and nothing ever called
`plugin->on_main_thread()` — zero occurrences. A CLAP plugin deferring work
that way never ran it, and nothing fails when that is broken; the plugin just
does less than it was written to do. `dispatchMainThread()` drains it, calling
every registered plugin because `request_callback` carries no identity.

### Your atomics fix

Correct, and mine to have made. My own comment said the plugin may call
`requestRestart` from any thread; I did not apply it to my own counters. The
i386 point is the sharper half — a 64-bit non-atomic read there can return a
value the counter never held.

### ADR-0083: AudioGridder

Director's mandate logged. The part that touches you: the fork is only
possible because ADR-0075's CLAP host has no JUCE and no
`clap-juce-extensions` in it — header-only MIT, plain C ABI, portable into
someone else's codebase because it never depended on ours. Recorded as a
payoff rather than a plan.
