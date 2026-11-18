# The engine lane — plug-in transport, returns inside groups, a send's tap (18 November 2026)

You are the **engine** lane of ADI (`adi_daw` in the `arieladi/Adi` monorepo), a Codex work seat.
Everything in `2026-11-18-engine-lane-codex.md` and the three prompts it names still holds. This
note adds three pieces of work the director ruled on 18 November (ADR-0245 d8, d9, d10), and it
comes **first** in your queue, before what that prompt lists: he asked for the pull requests that
fill these gaps.

Use everything you have: build, run, measure, stress, install what helps, write harnesses and test
plug-ins.

## Why

The director finds FX channels as other DAWs build them close to unusable for the way he mixes: a
tempo-synced plug-in behind a latent one drifts, and a return's tails go straight to Main, past the
clipper on the group they belong to. He brought a four-part proposal. Win read it against the
engine: one part is already yours and built (every path aligned at every summing point, key inputs
included, the taps crossed over when a latency changes); three are real gaps. Read ADR-0245 whole,
and `docs/RESEARCH-INTAKE.md` §6.2 and §6.4 for what Live and REAPER were measured doing.

## 1. A hosted plug-in is told the transport, at the time of the audio it receives (DF-E21)

On main a VST3 or CLAP plug-in is told nothing: `vst3_host.cpp` sets `processContext` to null and
always says real time, `clap_host.cpp` sets `transport` to null. A tempo-synced plug-in cannot
follow ADI at all. ADR-0245 d8 is the contract. In short:

- Every block, each hosted plug-in gets: playing, tempo, time signature, the loop and its bounds,
  the position in samples and in musical time, the start of the bar. VST3's `ProcessContext` and
  CLAP's `clap_event_transport`, with the validity flags set only for what is really given.
- With Delay Compensation on, the position is that of the first sample at the plug-in's own input:
  the block's timeline position less the arrival latency the graph already holds for that device's
  node. With it off, the heard time, as the scope's stamps already are.
- Musical time comes from the tempo map at that sample time. Before the song's start it is
  negative and continuous, the bar start included.
- In a loop the position wraps into the loop for audio that left before the wrap, by the loop's
  length as many times as needed (a loop may be shorter than the latency). A block that crosses
  the loop point is split there; Live was measured handing a plug-in blocks of 16 to 1,536 frames
  at seeks and wraps on a 2,048 buffer.
- The device's parameter automation and its note events run on the same clock. Where that already
  holds, a test proves it; where it does not, say so and fix it.
- A render, a freeze and a bounce use the same clock and say they are not real time (VST3's
  offline process mode, CLAP's render extension).
- Nothing new on the audio thread allocates, locks or waits.

Acceptance, at the least: a test plug-in that records what it is told (the research left one for
VST3 and one for CLAP under a permissive licence; win can pass them over, or write your own). On a
track with a 1,024-sample device before it, its position is the timeline's less 1,024, sample for
sample, through a seek, a loop wrap inside a block, a tempo change, a start before bar 1, and a
latency change while playing; with Delay Compensation off it is the heard time; in a render it is
told offline and the positions are contiguous.

While you are in those two files, DF-E20 holds three small faults beside this one: CLAP's unbounded
tail is every value from `INT32_MAX` up, not `UINT32_MAX` alone; the VST3 path hands a plug-in only
its main input, so a key never arrives; an instance load scans its path hint again every time.

## 2. A return may live inside a group (DF-E22, with the window's DF-W33)

ADR-0245 d9. A return whose parent is a group: its output is that group, summed with the group's
other channels before the group's devices; the group goes on to its own parent as today. Its send
exists only on the tracks inside that group, at any depth. The group has no send to its own
return. Its own sends to sibling returns exist and start disabled, as a top-level return's do. A
top-level return is unchanged. Solo follows Live's returns.

You decide the schema and the ops (a return with a parent, the send's scope, what moving a track
out of the group does to its send to that return, what deleting the group does), in one undo step
each, with the text projection and `check` kept whole. Tell the window lane the snapshot fields it
needs through your log.

Acceptance, at the least: the null test of ADR-0245 d11 (a dry track and an inverted unity send
through a return inside the same group, the track carrying 1,024 samples of latency and the return
512, cancelling to silence at the group's output, with a device on the group after the sum); a
clipper on the group changes the return's tail; a track outside the group cannot be given the
send; move-out, delete and Undo of each restore every row.

## 3. A send's tap (in DF-E22)

ADR-0245 d10. The return's Pre/Post switch stays exactly as you built it (N42). The addition: one
send may be set to Pre FX, Post FX or Post Mixer, and a return whose sends differ reports mixed,
which your model already has a state for. Compensation uses the latency at the tap. No tap by
device number, and no output chooser on a return.

## Report

As always: your log, "PR requested" with the branch and the title, one piece per pull request,
stacked where they depend on each other. Win opens and lands them.
