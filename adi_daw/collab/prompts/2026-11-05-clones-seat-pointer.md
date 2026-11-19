# The clones seat — a pointer to its brief (5 November 2026)

For the director's Codex plug-in seat, the **clones** lane of ADI (`docs/LANES.md` §4; log
`adi_daw/collab/clones.md`, newest first; branches `clones/<topic>`; win reviews and merges; you
never merge, never touch main, never `git add -A`).

**Where your brief is.** Your ADI-View Lab brief and your prompt are not in this repository. They
are in the director's private round folder
`NowWeReverse/UI-Testing/20261105T1420-adi-view-combined-reference`, in the file
`HANDOFF-TO-CODEX.md`, **section 3**. Read it there and work from it; nothing of it is copied
here, because the plug-ins' internals stay private and this repository is public. The standing
prompt of 25 October (`2026-10-25-clones-lane.md`) and the reference-reading prompt of 4 November
still hold for everything the handoff does not change.

**What this repository says that binds you**, in the director's words of 5 November: no reserved
plug-in slots; no separate built-in ADI-Q or ADI-L; his VST3 and CLAP builds stay ordinary
plug-ins; when all his plug-ins are finished and his list arrives, their source becomes one
unified built-in feature with a device-like window, from the track and the Mix View; it never
hosts the VST3; its design waits for the list. So ADI loads your builds like any plug-in (the
plan's DEV-26), nothing in ADI waits for them, and the DJ Filter has moved to the devices lane on
Auto Filter's cores (`docs/PLAN.md` §8.1 §4). Your clones stay built from measurements, under his
names (ADI-Q, ADI-L, ADI-Clip, which LANES calls ADI-C, ADI Still, ADI Prism, ADI Spacer, ADI
Fuser), and his list is a gate of LANES §5: he sends it when he declares them all finished.

**Two rules that cost nothing now and everything later**, because their source becomes the
unified feature and because every project that saved a state must reopen:

1. **Never rename or reorder a parameter id.** An id, once shipped, is the identity, as CLAP's
   `clap_id` and VST3's `ParamID` are and as `[adi.param]`'s id is in ADI (ADR-0177 d2); the
   name is a label. A new parameter gets a new id; a removed one keeps its id reserved and is
   never reused; the order parameters are declared in never changes what an id means. Every
   automation lane, macro mapping and controller binding in every saved project keys on it.
2. **Keep every shipped state readable.** A newer build reads every state an older shipped build
   wrote: a version in the state blob, a migration per version kept forever, and a fixture of
   each shipped version's state in the tests, loaded on every run, so no released version is
   ever orphaned. A state a build cannot read is a defect, never "upgrade and resave".

**The lab app's links.** The ADI-View Lab links **ADI-Q 0.3.18, ADI-L 0.1.6 and ADI-Clip 0.1.3**
from source in his tree, which are the versions the lab is built against and measured with. When
a plug-in's version moves, the lab's link moves with it in the same change, the round folder's
name says which round measured it, and the two rules above hold across the move.

When blocked, log why and take the next item; a usage limit is not a blocker, you resume from
your log on "continue". Ask the director nothing that LANES §5 does not list.
