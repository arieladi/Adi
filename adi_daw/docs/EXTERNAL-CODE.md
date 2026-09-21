# External code — inventory and licence boundary

We are GPLv3 (ADR-0015). That makes *what we may copy from* a design constraint,
not a footnote, so external code lives in two directories with different rules.

Both are gitignored — each clone is its own git repo, same arrangement as
`VST-ADI/vital`. Nothing here is committed to the monorepo.

Refresh everything with `tools/fetch_external.sh`.

---

## `third_party/` — we link against these and ship them

Permissive licences, all GPLv3-compatible in the direction we need.

| Repo | Licence | Size | Purpose |
|---|---|---|---|
| [`SQLiteCpp`](https://github.com/SRombauts/SQLiteCpp) | MIT | 14 MB | RAII C++ wrapper over the SQLite C API. Transactions, prepared statements, BLOB binding. Removes an entire class of leak and error-handling bug from the `.adi` reader/writer. |
| [`json`](https://github.com/nlohmann/json) | MIT | 29 MB | nlohmann/json. We want it for its **CBOR** codec (`to_cbor`/`from_cbor`), not its JSON — for `ops.payload` and `ops.inverse`. See ADR-0016. |
| [`bungee`](https://github.com/bungee-audio-stretch/bungee) | MPL-2.0 | 426 KB | Time-stretch / pitch-shift. Feeds `audio_clips.warp_markers` (SPEC §6.4). Handles continuous rate change and zero/negative speed, which is what scrubbing needs. |
| [`lockfree`](https://github.com/DNedic/lockfree) | MIT | 2.2 MB | Header-only bounded SPSC/MPMC queues, no dynamic allocation, cache-line padded. For the message-thread→audio-thread handoff in ADR-0010. |
| [`JUCE`](https://github.com/juce-framework/JUCE) | **AGPL-3.0** or commercial | 117 MB shallow | Audio device I/O, the plugin graph and VST3 hosting for step 6, and the UI toolkit after it. **Pinned 9.0.2 / `72782788`. Fetched only by `--with-juce`; `ADI_WITH_JUCE` is OFF by default** so the whole tree builds and every suite passes with no JUCE present (ADR-0036). **The one dependency whose licence is stronger than ours** — see ADR-0048 and the note below. |

**MPL-2.0 note.** Bungee is file-level copyleft. Fine to combine with GPLv3, but
modifications *to Bungee's own files* must stay open and carry MPL headers. Keep
any patches in a separate patch file rather than editing in place, so the
boundary stays obvious.

**On `lockfree`.** It gives us the queue, not the design. ADR-0014 chose C++,
which means the snapshot handoff has no borrow checker behind it — a correct
SPSC queue is necessary and nowhere near sufficient. The lifetime question
(*when is the old snapshot safe to free, given the audio thread may still be
reading it*) is ours to solve and is the part that will be subtly wrong if we
are casual about it.

---


### A note on JUCE, because it is the exception

Every other entry above is permissive — MIT or MPL — and imposes nothing on us
beyond attribution. **JUCE is AGPLv3**, which is stronger than this project's
own GPLv3 (ADR-0015).

The combination is explicitly permitted rather than merely tolerated: GPLv3 §13
grants permission to link with an AGPLv3 work and AGPLv3 §13 grants the mirror.
No exception is needed and neither licence is being stretched.

What changes is what we can say about the result. **A binary that links JUCE is
a GPLv3 + AGPLv3 combination, not "a GPLv3 program"**, and AGPL §13's
network-interaction clause attaches to the JUCE part. That is inert for a
desktop DAW and *not* inert for ADR-0039's RPC boundary, where remote clients
drive the program over a network.

JUCE's **examples** are ISC, not AGPL. Copying from `examples/` is a different
and much easier question than copying from `modules/`.


## `reference/` — we read these. They are never on the include path.

All GPL-family. Reading them to understand a problem is normal engineering.
Copying code from them is a licensing act with consequences, so the boundary is
explicit.

| Repo | Licence | Size | Copy? | Read it for |
|---|---|---|---|---|
| [`magda-core`](https://github.com/Conceptual-Machines/magda-core) | GPL-3.0 | 134 MB | ✅ with attribution | The closest existing thing to what we are building. See below. |
| [`tracktion_engine`](https://github.com/Tracktion/tracktion_engine) | GPL-3.0-or-later / commercial | 112 MB | ✅ with attribution | Plugin hosting, latency compensation math, audio graph construction. The most directly applicable JUCE-side reference we have. |
| [`ardour`](https://github.com/Ardour/ardour) | GPL-2.0-**or-later** | 164 MB | ✅ with attribution | Sub-sample fades, region splitting, comping, varispeed. Thirty years of getting the hard editing maths right. |
| [`helio-sequencer`](https://github.com/helio-fm/helio-sequencer) | GPL-3.0 | 19 MB | ✅ with attribution | Readable, modern, pure-JUCE UI. Piano roll rendering and timeline drawing without a UI framework underneath. |
| [`zrythm`](https://github.com/zrythm/zrythm) | **AGPL-3.0** + trademark | 161 MB | ❌ **design only** | Its undoable-action architecture — the best open-source event-sourced DAW model. Read the *shape*, write our own code. |

### DSP references for the plugin roadmap (ADR-0093)

Fetched for specific maths, not for architecture. The **Copy?** column is what
matters: it depends on the licence, and for two of these the licence forbids
what the roadmap was going to do with them.

| Repo | Licence | Size | Copy? | Read it for |
|---|---|---|---|---|
| [`ZLEqualizer`](https://github.com/ZL-Audio/ZLEqualizer) | **AGPL-3.0** | 6 MB | ❌ **design only** | Dynamic EQ structure, matched-phase ("de-cramped") bells, linear-phase mode. The Zrythm rule applies. Implement matched-phase from Vicanek, *Matched Second Order Digital Filters* (2016), not from this code. |
| [`lsp-dsp-units`](https://github.com/lsp-plugins/lsp-dsp-units) | LGPL-3.0-or-later | 19 MB | ✅ with attribution | `dynamics/Limiter.h`, `util/Oversampler.h`, `meters/TruePeakMeter.h`, `meters/LoudnessMeter.h` — the true-peak limiter maths. LGPL is GPL-compatible. |
| [`lsp-plugins-limiter`](https://github.com/lsp-plugins/lsp-plugins-limiter) | LGPL-3.0-or-later | 4 MB | ✅ with attribution | How the limiter core is driven: lookahead, oversampling ratio, and its Classic/Mixed/Modern modes. |
| [`vitOTTx`](https://github.com/Sakhnovkrg/vitOTTx) | GPL-3.0 | 1 MB | ✅ with attribution | Vital's OTT multiband upward/downward compressor, extracted standalone. Matt Tytel's original is also in `adi-vst/`'s Vital fork. |
| [`ADAA`](https://github.com/jatinchowdhury18/ADAA) | BSD-3-Clause | 33 MB | ✅ with attribution | The antiderivative anti-aliasing derivations: first- and second-order ADAA, and the ill-conditioned case when consecutive inputs are close. |
| [`chowdsp_utils`](https://github.com/Chowdhury-DSP/chowdsp_utils) | **per module**; `chowdsp_waveshapers` is GPL-3.0 | 19 MB | ✅ waveshapers, with attribution — **check each module's own header first** | `ADAAHardClipper`, `ADAASoftClipper`, `ADAASineClipper`: production ADAA, the part a clipper actually ships. |
| [`KlonCentaur`](https://github.com/jatinchowdhury18/KlonCentaur) | BSD-3-Clause | 220 MB (training data and PDFs; the code is small) | ✅ with attribution | ChowCentaur. A wave-digital-filter diode clipper and the paper describing it. **It contains no ADAA**, which is what it was requested for. |
| [`Audio-Soft-Clip-Distortion`](https://github.com/JDSherbert/Audio-Soft-Clip-Distortion) | MIT | 0.1 MB | ✅ with attribution | Basic hard and soft clip curves with oversampling. Introductory, but MIT. |

**Decided by `OPEN_SOURCE_POLICY.md` (ADR-0094).** Every project is open source:
MIT by default, GPLv3 once it copies from a GPL or LGPL source. So everything above
except ZLEqualizer may be copied, keeping the original headers — and a plugin that
takes the LSP limiter, the chowdsp waveshapers or vitOTTx is a GPLv3 plugin.
ZLEqualizer is AGPL and stays design-only.

### The Zrythm rule

Zrythm is **AGPLv3**, not GPLv3. The incompatibility runs one way: GPLv3 code can
go into an AGPL project, AGPL code cannot come into ours without dragging AGPL
§13 network obligations onto the combination. It also carries a trademark clause
under AGPL §7.

So: **read Zrythm for its action vocabulary and undo-stack design, and write our
own implementation.** Architecture and API shape are not what copyright protects;
source lines are. This is a real and useful distinction, not a formality — the
thing we actually want from Zrythm is *how they decomposed edits into undoable
actions*, and that is an idea, not a file.

If in doubt about a specific piece, don't.

### Licence corrections worth recording

Two of the repos came to us mislabelled, and both mistakes mattered:

- **Helio.** The suggested `Ahornberg/helio-workstation` is a fork last pushed
  **2022-01-08**, nearly five years stale. The canonical repo is
  `helio-fm/helio-sequencer` (renamed), last pushed 2026-09-15. We cloned the
  canonical one.
- **Zrythm.** Described to us as "built in C (using GTK4)" and implicitly GPL.
  It is **C++**, and it is **AGPLv3**. The language detail is cosmetic; the
  licence detail is not, and it is the single most important constraint on this
  whole list.

Neither was obvious from the repo page — GitHub reports `NOASSERTION` for
Zrythm, Tracktion and Ardour alike, and the actual terms differ sharply between
those three. Check `COPYING`/`LICENSE` in the tree, not the sidebar.

---

## MAGDA is prior art, and we should be honest about it

[MAGDA](https://magda.land/) is an open-source DAW built on C++20, JUCE and
Tracktion Engine, GPL-3.0, actively developed. It already ships:

- Arrangement, **Session and Mix views** with clip launching
- Nestable racks with per-chain volume/pan/mute/solo, 16 macros and 16 bezier
  LFOs per device
- Piano roll with pitchbend and CC lanes
- **An in-app AI agent** that turns natural language into a DSL and executes it

That is a large overlap with the design in this repository, and pretending
otherwise would be silly. Two things follow.

**What we should take.** Their `magda::remote::OperationRegistry` is a
transport-neutral, JSON-Schema-validated operation registry, with every operation
declaring one of five scopes (`read`, `edit`, `transport`, `session`,
`hardware-midi`), an unknown client defaulting to `read` alone, and the registry
*refusing to start* if a write operation has no scope entry. WebSocket/JSON-RPC
and MCP are both projections of that one registry and neither defines operations
of its own.

This is the idea in [`AI-AGENT.md`](AI-AGENT.md) §3 — one op vocabulary, many
consumers — already built and in production. Their scope model is more granular
and better than the three capability tiers in §2, and the "registry refuses to
start with an unscoped write" check is the kind of thing you only think of after
being burned. **Step 3 should start by reading this.**

**Where we still differ, and why the project is still worth doing.** MAGDA
inherits Tracktion Engine's persistence: projects are JUCE `ValueTree`s
serialized to XML, and undo is JUCE's in-memory `UndoManager`. SQLite appears in
their tree only for the plugin-metadata cache and media database — *not* for the
project file. So every argument in [`format/RATIONALE.md`](format/RATIONALE.md)
still stands: incremental saves, atomic commits, forward compatibility by
default, and — the one that is load-bearing for the agent — **an undo history
that persists across restarts and branches**. A `ValueTree` undo stack dies with
the process.

That is a real differentiator, and it is precisely the one we chose to build the
project around. It is also, notably, the thing that is hardest to retrofit into
MAGDA later.

**The strategic question this raises** — contribute to MAGDA, fork it, build on
Tracktion Engine independently, or stay fully independent — is not answered here.
It is a genuine fork in the road and belongs in an ADR. It is currently
unrecorded and open.

Note also that MAGDA requires a **CLA** from contributors, which bears directly
on the open question at the end of ADR-0015.
