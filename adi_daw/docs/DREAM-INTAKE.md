# The Dream intake

**Status:** win's reading for the director, 26 November 2026, as he read it. **Ruled the same day by
ADR-0257:** the five choices of §5 as recommended (its d7 to d11). The plan rows are owed (DF-R2). It
processes one day's discussion between the director and a panel of three outside models, who read a pack
of ADI's public documents. It says what ADI already has for each idea and what win recommended. Nothing
of it starts before the basics of `docs/SANITY-CHECK.md` (ADR-0257 d1).

## 1. The short answer

The director's wish: leave ADI working overnight in his style, and wake to a project he can open, play
and change. Hundreds of parts are welcome if each is editable. A part may be a WAV, as long as it is one
identifiable part and not a finished master.

Win's reading:

- **It fits ADI's rules in one form.** The agent works in a copy of the project and never in the
  original. In the morning the user opens the copy, plays it, edits it, and pastes what he likes back.
- **Most of the ground is already decided and unbuilt.** The agent process, background jobs, the consent
  ledger, paste between projects, render recipes and templates all have plan rows. The Dream moves them
  up; it does not replace them.
- **The first step needs no AI model at all.** A session that explores a synthesizer's parameters by
  seeded rules and renders one-shots with their recipes proves the rendering, the recipes, the storage
  and the Browser. Win calls it the Foundry.
- **The larger visions are a direction, not a plan.** A shared latent canvas, constellation views and
  evolution across nights each need their own ruling after the first results are heard.

## 2. What ADI already has

| For | ADI has | State |
|---|---|---|
| A sandbox for the run | Save a Copy: the whole project file with its history (PD-01) | built |
| Bringing chosen parts back | paste between two open projects as one transaction (PD-15, ADR-0068) | decided; core in review |
| Driving a synthesizer | `device.insert`, `device.setParam`, `device.loadState`, `device.setPreset`, the expression route | built |
| The suite's own synthesizers | Surge XT and Vital builds beside the DAW; VST3 and CLAP hosting | built |
| Keeping the instrument under the audio | whole-track freeze: dry and wet files, clips and state intact (TM-FC-04) | first form built |
| A render kept with its recipe | the recipe key, reuse, an explicit Unused Files command (TM-FC-03) | decided |
| Rendering without real time | the offline render path and the export queue; up to 16 graph workers | built; workers off by default |
| Who did what | every op carries its actor and model; the agent's request beside its transaction (SPEC 8.7) | built |
| The agent | the changeset (preview, diff, apply), the control core, a headless `adi-mcp` | built |
| The agent's process, budgets and consent | AGT-06, AGT-03 | decided, unbuilt |
| Background jobs | AGT-04 | decided, unbuilt |
| Templates | a template is a project copy in the Templates folder (PD-14) | decided, unbuilt |
| Finding similar sounds | descriptors in the library index, Show Similar (TM-MD-07) | decided, unbuilt |
| Learning that stays local | the mix profile: plain statistics in one readable file (AGT-11) | decided, unbuilt |
| A script that drives the real window | WTS-05, WTS-06, CTL-09 (Track C) | decided, unbuilt |

## 3. Every idea, and where it goes

"D1" to "D4" are the phases of section 4.

| # | The idea | Win's reading |
|---|---|---|
| | **Reliability** | |
| 1 | Make the real window reliable before anything else | It is the hold (ADR-0255 d1). No change |
| 2 | A repeatable real-window test through the app's own handlers | Decided as WTS-05, WTS-06 and CTL-09. Start it when the hold ends (decision H) |
| 3 | Scenarios the headless suite lacked | Add them now: record after Save As, an edit during playback, twenty Plays in a row. A fix, not a feature |
| | **The agent's rules** | |
| 4 | Rebase a stale changeset: silently, with a preview, or by degrees | Keep ADR-0148 d4: stale is refused. In a Dream copy the agent is the only writer, so nothing goes stale |
| 5 | Budgets, consent, uploads and growth, beyond the op boundary | AGT-03, AGT-06 and the unread per-minute cap (DF-C6) come before any unattended run |
| 6 | Plug-ins stay in process; improve crash recovery | Stands as ruled |
| 7 | Leave the agent's own edits out of any learning | Already possible: the actor is on every op |
| | **The overnight Dream** | |
| 8 | Wake to a project to open, play and change; never replace the user's work | Yes, in a copy (D2) |
| 9 | A branch, ghost lanes, a merge | No new branch model. A copy, and paste between projects. Ghost lanes are not planned |
| 10 | Several directions in one night, combined afterwards | Several copies; combine by paste (D3) |
| 11 | A shared musical plan that the parts follow | Yes, small first: tempo, key, form and a role per track, written before any part, readable by the user (D2). A relationship graph is later (D4) |
| 12 | Symbolic first: notes, instruments, settings, automation | Yes, the rule of D2 |
| 13 | One WAV per part is editable enough | The director's position. Freeze keeps the notes and patch under the audio |
| 14 | Three states: live instrument, frozen with its recipe, audio only; say when an edit breaks the link | Yes, with TM-FC-03 and TM-FC-04 (D3) |
| 15 | Hundreds of parts, folded into groups by role | Yes (D3). Groups exist |
| 16 | Rank parts by what the mix loses when each is muted | Yes, as offline jobs, never as edits in the journal (D3) |
| 17 | Chunks under the 6,000-op cap, a snapshot per chunk | Yes: each chunk is one changeset, with a named snapshot between (D2) |
| 18 | A budget for the whole run, and a stop | Yes: a long-run budget beside AGT-06's short one (D2) |
| 19 | A night journal of what the agent tried | Yes: a readable file beside the copy (D2) |
| 20 | Recipes: seed, parameters, engine and model version, source; carried through paste | Yes: the recipe key for renders (D1), the request row for edits (D2), carried by paste (D3) |
| 21 | Regeneration may not be bit exact | Say so. The recipe names the engine build |
| 22 | Keeping or pruning generated audio | A session's files live in its own folder and go with it (decision M) |
| 23 | Media on disk always asks first | Amend for a session the user started: one consent at the start (decision M) |
| | **Making sounds** | |
| 24 | Morph a patch through its exposed parameters, not its opaque state | Yes (D1) |
| 25 | Seeded search and offline rendering, with no neural audio | Yes: this is the Foundry (D1) |
| 26 | Sessions that make samples; a rough sound turned into playable material | One-shots with recipes first (D1). Sample to instrument is DEV-34. A hummed idea is later |
| 27 | Templates as a replayable list of ops with inputs | A template is a project copy today (PD-14). A template with inputs is later (D3) |
| 28 | A starter kit: samples and a template made to fit each other | The Foundry's first output (D1) |
| 29 | The Browser shows what a session made | Yes (D1), checked on a real desktop |
| 30 | Neural audio generation | Not in D1 to D3 (decision N). The cloud kind stays out, as already agreed |
| 31 | A shared latent canvas, cross-coupled diffusion | Not planned |
| 32 | "Original" or "royalty-free" claims | Never claimed. ADI shows how each sound was made |
| | **Taste** | |
| 33 | A local profile of the user's habits | Decided as AGT-11 |
| 34 | Explicit keep, reject and revisit on Dream results | Yes, recorded in the same local file (D3). Never inferred from mute or delete |
| 35 | Choosing which projects feed the profile | Yes, on the profile's page (D3) |
| 36 | Evolution across nights; a gallery of broken sounds | A direction (D4) |
| | **Views** | |
| 37 | Constellation and lineage views, a density control, targeted mutations | A direction (D4). Each needs an agreed sheet (ADR-0243) |
| 38 | Explanations in musical language; comparing through the history | The agent already reads and writes bars, beats and note names. The journal of D2 uses the same language |
| 39 | Searching past projects for reusable sounds | TM-MD-07 and the library index; the agent's access is DF-C7 |
| | **Other** | |
| 40 | A whole-song mode beside the co-pilot | The Dream is that mode, and it works only in a copy. In the user's own project the tiers stand |
| 41 | The Live 12 readings | Owed, about fifty, on one list. Not scheduled |

## 4. The plan

Each phase ends in something the director can open and judge. A phase opens only when the one before it
has been heard.

**D0. The ground.** Existing rows, moved up, no new idea.
- The clean pass in the real window, then the three features of ADR-0256.
- The real-window script: WTS-05, WTS-06, CTL-09.
- For the agent: the per-minute cap enforced (DF-C6), the consent ledger (AGT-03), jobs (AGT-04), the
  agent's process (AGT-06).
- Paste between projects (PD-15), and render recipes (TM-FC-03).

**D1. The Foundry.** No AI model.
- A session takes one of the suite's synthesizers and walks its exposed parameters by seeded rules.
- It renders each result offline as a one-shot and writes its recipe beside it: the patch, the seed, the
  notes played, the engine build.
- It makes a template project that holds the kit.
- The Browser lists the session's output.
- **First target:** twelve one-shots with recipes and one template.
- **What it proves:** offline rendering at scale, recipes, storage and retention, and the Browser's
  listing, before any model is involved.

**D2. The small Dream.**
- The agent works overnight in a copy of a project the user chose as the seed.
- It writes a plan first: tempo, key, form and a role per track.
- It builds parts as notes, instruments, settings and automation, using ADI's own devices and the
  Foundry's sounds.
- Each chunk of work is one changeset and one undo step, with a named snapshot after it.
- It has a budget for the whole run and a stop. It keeps a journal of what it tried.
- **First target:** an eight-bar loop with twelve parts. The director listens and decides whether to go
  on.

**D3. Scale and comfort.**
- Hundreds of parts, folded into groups by role.
- Parts frozen as the run goes; each shows whether it is live, frozen with its recipe, or audio only.
- The parts ranked by what the mix loses without them.
- Several directions in one night, each its own copy; chosen parts pasted back with their provenance.
- Keep, reject and revisit recorded in the local profile; the user chooses which projects feed it.
- Templates with inputs such as tempo and key.

**D4. Worlds.** A direction only. Constellation and lineage views, a relationship graph between parts,
targeted mutations, evolution across nights, and a neural audio model as one more instrument. Each needs
its own ruling.

## 5. Decisions for the director

| | The choice | Win recommends |
|---|---|---|
| H | Start the real-window script (WTS-05, WTS-06, CTL-09) on its own seat the day the hold ends | Yes. The walk found about thirty faults the tests missed, and the Dream's output must be checked in the real window too |
| J | The shape: the Dream works in a copy, symbolic first, one part per track, with ADI's own instruments; the Foundry comes first, then the small Dream | Yes |
| L | Move the ground up: paste between projects, the agent's process, the consent ledger and jobs come right after the three features of ADR-0256, and the Dream gets a parallel track of its own | Yes. Without them nothing of D2 can run |
| M | A session the user starts writes its files into its own folder after one consent at the start. A discarded session goes to the Recycle Bin with its files | Yes. It amends the rule that media on disk always asks |
| N | No neural audio generation in D1 to D3. Decide after hearing the twelve-part test | Yes. Synthesizers, samples and resampling keep every sound reproducible from its recipe |

## 6. Not recommended

- **A branch-and-merge model inside the project file.** A copy and paste between projects do the same
  job with what exists.
- **Rebasing a stale changeset,** silently or by degrees. The ruling stands; the Dream does not need it.
- **Hundreds of live instruments in real time.** The run renders offline and freezes.
- **Learning from mute and delete.** They are logged, but they do not say why.
- **A whole-song mode that writes into the user's own project.**
- **Views before results.** No constellation or lineage view until the small Dream has been heard.

## 7. To measure first

Nobody has measured these. D1 exists partly to find out.

- How long one one-shot takes to render offline, and a full part.
- How many parameters each of the suite's synthesizers exposes, and whether a seeded walk through them
  gives usable sounds or mostly noise.
- Whether the same recipe gives the same audio twice on one machine.
- How much disk a session of twelve, and of three hundred, parts takes.
