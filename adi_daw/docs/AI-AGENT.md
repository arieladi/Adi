# The in-DAW AI agent

**Status:** design sketch. Nothing built. This document exists now, at format
time, because the agent's safety model is a *format* decision — it depends on the
op log being in the file — and getting it wrong is not recoverable later.

---

## 1. The one rule

> **The agent has no privileged access to anything. Its only way to change a
> project is to emit ops — the same ops the user's own actions emit.**

Everything else in this document is a consequence of that rule.

An AI agent with direct mutable access to a DAW's project model is a genuinely
frightening thing to ship: it can silently corrupt a session, and the user finds
out at mixdown with no way back. An agent that can only append to the op log is
an ordinary feature, because it inherits, for free and by construction:

| Property | Why it holds |
|---|---|
| Everything it does is undoable | Ops have inverses (SPEC §8.1) |
| One agent action = one Ctrl-Z | All its ops share a `txn_id` |
| Everything it does is attributable | `ops.actor = 'agent'`, `actor_detail` names the model |
| Everything it does is auditable after the fact | `SELECT * FROM ops WHERE actor='agent'` |
| It cannot do what a user couldn't | There is no op for "corrupt the file" |
| It cannot cause a dropout | Ops are applied on the message thread, never the audio thread |
| Trying an idea costs nothing | The undo tree branches (SPEC §8.2) |

If a proposed agent feature cannot be expressed as ops, the answer is to add the
op — not to give the agent a side door.

---

## 2. Capability tiers

The agent runs at one of three levels, set per project and visible in the UI.
Default is **Propose**.

| Tier | Can | Cannot |
|---|---|---|
| **Observe** | Read the project, run analysis, answer questions, suggest in prose | Emit any op |
| **Propose** | Everything above, plus build a changeset and show it as a diff | Commit without an explicit click |
| **Apply** | Commit ops directly, in one undoable transaction | Exceed the op allowlist for its tier |

Even at **Apply**, the following always require explicit confirmation, every
time, regardless of tier:

- deleting or overwriting media files on disk (the agent never touches the media
  pool's underlying files — it can unlink a reference, not unlink an inode);
- anything that leaves the machine (uploading audio, calling an external service);
- destructive bulk ops above a threshold (e.g. > 20% of clips in one txn);
- changing the agent's own tier.

---

## 3. The op vocabulary *is* the tool schema

We do not write a separate "AI API". The op vocabulary the app already needs for
undo becomes the agent's tool definitions, generated from one source:

```
op:  clip.move          { clip_id, track_id?, pos_ticks }
op:  note.quantize      { clip_id, note_ids[], grid_ticks, strength, swing }
op:  track.create       { kind, name, parent_id?, index }
op:  device.insert      { chain_id, plugin_ref, ord }
op:  automation.write   { lane_id, points[], mode: replace|merge }
...
```

One vocabulary, three consumers: the UI, the scripting API, and the agent. This
is not just tidy — it means **the agent can never drift out of sync with what the
app can do**, and every op the agent can emit is one a human has already
exercised through the UI.

Consequence for SPEC §12.1: the op payload encoding must be something we can
project into a tool schema cleanly. That is another argument for CBOR or
MessagePack over a hand-rolled TLV.

---

## 4. How the agent sees the project

A real project does not fit in a context window, and dumping SQL rows at a model
is both expensive and bad. The agent gets a **projection**: a compact, structured,
*musically meaningful* view, with progressive disclosure.

**Level 0 — always present (~1–2 KB).** Tempo, signature, key, length, track list
with kind/name/colour/rough content density, device names per track, marker and
section list.

**Level 1 — on request, per track or range.** Clip list with positions and
lengths, automation lanes present, plugin parameter summaries.

**Level 2 — on request, per clip.** Actual notes, in a compact musical form, not
raw ticks:

```
Keys / "Riff" @ bar 5, 4 bars, Dm
  1.1.0  D3  q    v96
  1.2.0  F3  q    v88
  1.3.0  A3  h    v72   p=80%
```

Ticks are exact and unreadable; bars/beats plus note names are what a model
reasons well about. The projection layer converts, and converts back.

**Level 3 — analysis, on request.** See §5.

The projection is *read-only and derived*. It is never a second source of truth.

---

## 5. Analysis tools — and an honest limit

A text model cannot hear. This is the real constraint on an AI agent in a DAW,
and pretending otherwise produces a demo rather than a tool. The agent therefore
gets measurement tools whose outputs it *can* reason about:

| Tool | Returns |
|---|---|
| `analyze.loudness` | Integrated / short-term / momentary LUFS, true peak, LRA |
| `analyze.spectrum` | Banded energy over time; spectral centroid, flatness |
| `analyze.transients` | Onset map, density, timing deviation from grid |
| `analyze.key` | Detected key/scale with confidence |
| `analyze.tempo` | Detected tempo, beat grid, confidence |
| `analyze.harmony` | Chord sequence inferred from MIDI + key map |
| `analyze.groove` | Timing/velocity deviation profile of a part |
| `analyze.masking` | Band-overlap between two tracks over time |
| `analyze.headroom` | Per-channel peak/clip/inter-sample-peak census |

These run on the render path against a WAL snapshot (SPEC §3.6), so analysis
never blocks editing.

Where an audio-capable model is available, raw audio may be offered to it — but
only under §2's leaves-the-machine confirmation, and never by default.

**What the agent will be good at** are the things that are tedious, structural and
verifiable: gain staging, finding clipping, matching groove between parts,
building arrangement variations from existing material, consistent naming and
colouring, routing and bussing, converting a Session View idea into an
arrangement, fixing timing, generating variations of a MIDI part.

**What it will not be good at**, and we should not claim: judging whether
something sounds good.

---

## 6. Guardrails

1. **No op, no action.** There is no code path from the agent to the project model
   that bypasses the op log. This is enforced structurally: the agent process
   holds no writable handle to the database.
2. **One transaction per request.** However many ops an agent request produces,
   they share a `txn_id` and revert as one.
3. **Never on the audio thread.** Obvious, and worth writing down, because the
   pressure to "just peek at the buffer" will arrive.
4. **Never deletes media.** It can remove a *reference*; the file on disk is not
   its business.
5. **Dry-run first.** At Propose tier, the changeset renders as a visual diff:
   which clips move, which parameters change, before/after.
6. **Bounded.** Per-request caps on op count, wall-clock time and token spend,
   with the limits visible and editable.
7. **No silent network.** Model choice and destination are shown in the UI. If a
   cloud model is selected, the user is told *what* leaves the machine — project
   projection only, never audio, unless separately confirmed.
8. **Reproducible.** `actor_detail` records the model and version; the request
   text is stored alongside the txn. "What did I ask it, and what did it do" is
   answerable six months later.

## 7. Where the model runs

Pluggable, with no default that surprises anyone.

- **Local** (llama.cpp / ONNX Runtime, a small tool-calling model). Private, free,
  offline, weaker. Should be a genuinely supported path, not a token gesture —
  for a lot of users, "my unreleased album goes to a third party" is
  disqualifying, and for an open-source DAW that objection is the majority view.
- **Cloud API** (Anthropic / OpenAI / others, user-supplied key). Far more
  capable. Opt-in, per project, with the data boundary stated plainly.
- **Self-hosted endpoint** (an OpenAI-compatible URL). For anyone running their
  own.

The projection layer (§4) and the op vocabulary (§3) are identical across all
three. Only the transport differs.

## 8. What this is not

- **Not a music generator.** There are better tools for generating audio from
  nothing, and that is not what a DAW is for. The agent works on *your* material.
- **Not a mixing engineer.** It can measure, flag and suggest. It does not get an
  opinion about taste.
- **Not required.** The DAW must be complete and pleasant with the agent turned
  entirely off, and it must be possible to build it out.
- **Not a chatbot bolted to a sidebar.** If the only interface is a text box, it
  has failed. The op vocabulary means agent actions can be surfaced as ordinary
  UI affordances — a context-menu "match this groove", a "fix gain staging"
  button — that happen to be model-backed.

---

## 9. Open questions

1. **Op payload encoding** (SPEC §12.1) — gates the tool-schema projection.
2. **The full op vocabulary.** The largest single design document still owed, and
   it blocks both undo and the agent.
3. **Diff rendering.** What does "here is what I propose to change" look like for
   an arrangement edit? For automation? This is a hard UI problem and it is the
   difference between Propose tier being useful and being ignored.
4. **Projection fidelity.** How much of a project can be usefully summarised
   before the agent starts reasoning about a caricature?
5. **Evaluation.** How do we know a change to the projection or prompt made the
   agent better? Needs a fixture corpus of projects and tasks with checkable
   outcomes. Build this early — it is the difference between engineering and
   guessing.
