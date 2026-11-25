# Mix suite intake: four outside proposals, read against ADI

**A working paper, second edition, 25 November 2026. The director ruled on it the same day: ADR-0254 approves every
recommendation as written, with six rulings of his own (section 8). Nothing in it is built, and nothing starts before
3 December.** The decisions live in `docs/DECISIONS.md` and the rows in `docs/PLAN.md`; this paper is kept as the record
of what was asked and why. Sections 1 to 7 stand as he read them.

The director brought four proposals drafted with an outside model:

1. a mixing workflow suite with a personal mix assistant;
2. more mixing, clip and stem tools;
3. capture, scope, sample and controller tools;
4. a smarter Browser, complementary sample matching, guardrails for the agent, clip variations, clip to instrument,
   and key presets (new in this edition: R46 to R58).

Win read each request against main, the ADR log and the plan, with eight separate audits. Two rules frame everything
below:

- ADI does what Live does; an addition needs the director's yes and a priority (ADR-0203 d1).
- Nothing new starts before 3 December (ADR-0248 d1, d2).

## 1. The short answer

The four proposals hold 58 requests.

- **21 are already ADI's**, built or decided under other names. They need finishing, not deciding
  (R1, R2, R4, R6, R10, R12, R14, R18, R20, R22, R23, R26, R30, R31, R40, R41, R42, R46, R52, R55, R57).
- **22 are new and worth having**, most of them small because they sit on ground that is already built
  (R5, R7, R8, R11, R13, R16, R17, R19, R21, R24, R25, R27, R28, R36, R37, R39, R43, R44, R47, R49, R51, R54).
- **7 are better deferred** until the thing they depend on exists (R3, R9, R29, R38, R50, R53, R58).
- **4 are not recommended**, because they repeat something ADI has, or go against a ruling (R15, R34, R35, R45).
- **4 are the director's call alone** (R32, R33, R48, R56).

The proposals assume three things ADI does not have and has ruled against: insert slots with a wrapper on each
(ADR-0212), a model running inside the DAW (ADR-0233 d4, ADR-0064 d5), and a strip that owns processing (ADR-0243 d2).
Where a request leaned on one of those, the recommendation gives the same result through devices, racks, the Mix
View's functions or the agent's changesets.

## 2. Every request

Status words: **have** (on main), **decided** (an ADR or a plan row says so, not built), **new**, **against** (a
ruling says otherwise). "Wave" is the order of section 3.

### A. Mixing and the Mix View

| # | Request | ADI today | Recommendation | Wave |
|---|---|---|---|---|
| R1 | Unmask one track from another, picked from a list, no manual routing | **have, not reachable:** the Carve core (32 keyed bands) and the Focus core are built and tested; no user can turn them on (DF-M11, DF-M54). A key is a stored row picked from a menu, in the Device View only | Finish as decided. One addition: picking the priority track in the Mix View writes the key | 1 |
| R2 | Another track's spectrum over the EQ, masked zones marked | **decided** (ADR-0195 d5; MX-21, P2). The Focus page already hatches masked bands | As decided | 3 |
| R3 | Three priority tiers on a group: front carves middle and back | **new.** Focus has one main channel (ADR-0244 d17) | Defer until Focus with one main is in use | later |
| R4 | One zoomable overview of every track's EQ, spectrum and gain reduction | **have:** this is the Stack, by the director's ruling (ADR-0212 d2, ADR-0237 d5, ADR-0244 d11 to d13), in its pre-sheet form | Nothing new | none |
| R5 | Drag an EQ band onto another track, as a copy or an inverted copy | **decided, then stranded:** accepted in ADR-0209 (§3, Q9); its carrier went with the slots | Re-home it on the Stack | 2 |
| R6 | Dynamic EQ bands; a spectral mode per band | **have** | Nothing new | none |
| R7 | EQ sketch: draw a curve, bands are fitted | **decided as a target** (ADR-0195 d2), not built, not queued | Add to the EQ's queue | 5 |
| R8 | EQ bands that follow pitch: MIDI notes, or analysed audio pitch | **new.** No pitch detector is chosen (DEV-22, TM-WT-09) | MIDI-follow: yes, it is exact and cheap. Audio-follow waits for the pitch detector | 5, later |
| R9 | An EQ band on transients only, or sustain only | **new.** A transient splitter is only a P2 note | Defer | later |
| R10 | Align two or more tracks in time and polarity, one action, undoable | **have** the measurement (preview only). **Decided:** Align Apply as one undo step (ADR-0247 d8), Fix Delay (MX-13, M4), a sub-sample lag with polarity | Finish as decided. Addition: several tracks in one action | 1, 2 |
| R11 | A low-end phase scope for two channels, a rotation dial, "optimise the low-end sum" | **have** the Focus Scope (Sum, Difference, Mid, Side) and a rotation mode in the Focus core. **Decided:** the Kick and Bass Lab (ADR-0209 §5, P2) | A low-band switch on the Focus Scope; the optimiser with the Lab | 2, P2 |
| R12 | A beat-locked oscilloscope for several tracks | **have:** the Focus Scope locks to one bar, in the tracks' colours. **Decided:** up to 8 layers, windows from 1/16 to 4 bars (ADR-0167) | Finish ADR-0167: only the division chooser is missing | 1 |
| R13 | Mix health signs: DC and rumble, low-end mono loss, which track caused a peak, a latency flag, each with a one-click fix | **decided in part:** the Mix Radar (MX-21, P2) flags masking, correlation, sub build-up, overs and crushed dynamics, with Propose Fix. Utility has DC and Bass Mono, as Live's. Latency shows on hover | Add the four measures to the Radar's list. A fix is a one-undo proposal that uses Live's own tools (Utility, Freeze, Reduced Latency). Signs on the strips only after a sheet | 3, 5 |
| R14 | Loudness-matched bypass on any plug-in; "Match Output Gain" | **decided:** auto gain-match for any plug-in, P2 (ADR-0203 d8). The matcher is built and used by one device family | As decided, plus the commit action | 2 |
| R15 | Linked input and output drive on every insert | **against:** there is no generic gain around a device (no slots) | Not as a wrapper. Finish the Clip stage's own Gain Link | none |
| R16 | Delta listen on any plug-in | **decided for native stages** (Hear What Is Removed, ADR-0247 d8), not built. The graph already keeps an aligned dry copy per device | Extend it to any device, as a device command | 2 |
| R17 | Mid/Side or Transient/Sustain processing around any plug-in | **against** as a wrapper (ADR-0212). Racks give aligned parallel chains (ADR-0253) | Mid/Side as a rack kind after racks play; Transient/Sustain later | 5 |
| R18 | A band filter on the wet path only | **arrives with racks:** a dry leg beside a chain of EQ then the plug-in | Nothing new | 1 |
| R19 | Reverb and delay tails that duck under the dry signal | **have** in Echo, as Live's. Reverb and Delay do not | A helper first: a keyed compressor on the return. A new switch inside Reverb or Delay is the director's call | 2 |
| R20 | Beat-synced or MIDI-triggered volume shaping in bands | **have:** ADI-Shaper's Volume stage, three bands, beat contours. MIDI and audio retrigger are queued for after 3 December. Its panel waits for a sheet | Nothing new. A strip-level version is not recommended | none |
| R21 | A 2D soundstage: pan across, level up, width as a handle | **new.** The strip has no working width (ADR-0203 d9 keeps width in Utility) | A new Mix View page, pan and level first, after a sheet | 5 |
| R22 | Reference files: a pool, loudness-matched A/B, analysis | **have** the Reference route and the REF button. **Decided:** role storage and matched loudness (MX-29, M4). One Reference per project | Finish MX-29. The pool is a list of analysed targets; the audible Reference stays one | 1, 3 |
| R23 | Solo one stem of the reference | **decided** (ADR-0209 d11, d12; P2) | As decided, after stems | 4 |

### B. Clips and the timeline

| # | Request | ADI today | Recommendation | Wave |
|---|---|---|---|---|
| R24 | FX throws on a time range, tails ringing on, no duplicate track | **new.** Clip effects are decided as offline (ADR-0069; TM-FC-03, M5). The engine sums a track's clips before any tap | A Throw command: send automation over the selection to a return, one undo step. No engine work | 2 |
| R25 | Preverb: a reverse-reverb swell into a selection | **new.** Reverse is stored and not played. Recipe-keyed offline renders are specified (TM-FC-03) | An offline render that lands as an ordinary clip | 4 |
| R26 | Snap a selection to words or phrases | **decided:** Detect Silence (DF-W15), better onsets (TM-WT-01, M6), word chopping behind the service boundary (P2) | As planned | none |
| R27 | Clip prep: a gain rider drawn on the clip | **decided ground:** the clip volume curve (AU-07, M6), clip gain drawn in the editor (ADR-0242 d1). "No real-time rider" (ADR-0203 d14) stands: this one is offline | An offline analyser that writes the clip volume curve, one undo step, editable like any curve | 4 |
| R28 | Clip prep: fade the gaps, trim the breaths | **decided ground:** Detect Silence. Breath detection is new | The same curve. Breath detection as plain signal analysis | 4 |
| R29 | Clip prep: sibilance | **have** a de-esser core no user can load. A region in time and frequency is taken conditionally (ADR-0242 d4, DF-W16) | With that region, or with clip offline processing | later |
| R30 | Retrospective audio capture | **have, not wired:** a RAM memory per armed or monitored input, 30 seconds, tested. The Capture button, for audio and for MIDI, says it is not built. Decided as P1 (ADR-0132 d9, ADR-0203 d7) | Wire it. Let the ring follow its setting (up to 600 s). Add the range strip. A ring on Main's output is a separate P2 item (EX-12) | 1, 2 |
| R53 | Five variations of an audio clip by plain signal processing (reshuffled slices, pitch per slice, reverse, stutter), on alternate lanes, with a stored seed | **new.** Ground: take-lane ops are built, and a take lane is heard only while auditioned (ADR-0225 d1), which suits A/B. Taken on condition: slicing with a seed per clip (ADR-0242 d4), pitch and gain per audio event (ADR-0242 d1). Missing: reverse playback, slicing as an edit (TM-WT-08, P2), transpose on an unwarped clip | Defer until those land. Then build a variation as edits only: slices as clips on a take lane, no render, the source untouched. Whether ADI wants a generator at all is the director's call | later |
| R54 | Turn a clip into a playable instrument in one action | **decided in parts:** OneShot (Classic, 1-Shot, Slice) and Sampler are on main; a sample is assigned in one undo step; "a sample drop makes a OneShot chain" (DEV-04, P2). Today an audio file on a MIDI track is refused. Missing: the command, root-pitch detection (no tracker chosen, TM-WT-09), OneShot's Warp (decided parity, ADR-0169 d4), macros (DEV-06) | Yes. The command, and the drop onto a MIDI track, first: existing ops, and it is Live's own gesture. Root pitch comes with the pitch detector; the stretched mode is OneShot's Warp; macros come with racks | 2, then with their rows |

### C. Stems, samples and the Browser

| # | Request | ADI today | Recommendation | Wave |
|---|---|---|---|---|
| R31 | Local four-stem separation in its own process | **decided, P1** (ADR-0203 d8). The result gateway is built and tested. There is no model runner, and no plan row owns the model, the process or the weights | Give that gap its row | 4 |
| R32 | GPU acceleration for separation | **not ruled.** ADR-0086 is against shipping a GPU runtime; no Python | CPU first. GPU only through what the system already provides | his call |
| R33 | Drum-kit stems (kick, snare, hat, toms, cymbals); voice isolation | **against, in part:** more or better models are optional downloads or a service; other AI jobs stay service-only (ADR-0203 d8) | The director's call: do optional downloads extend to these two models | his call |
| R34 | Level and EQ per stem inside one clip | **new.** The decided result is a group and one track per stem | Not recommended: a folded group does this | none |
| R35 | A transient "unlimiter" | **against:** restoration is out of scope (the feature catalogue: plug-ins do this). A transient shaper is a P2 note | Not recommended | none |
| R36 | Find similar samples; swap a sample for a similar one while playing | **Live 12 has both.** ADI deferred them "to a later row" because of the model they need (TM-MD-04). The library index is built: paths, tags, tempo, name search | Do it with measured descriptors, no model. It is Live parity. An embedding can come later as an option | 3 |
| R37 | A 2D map of the samples | **new.** The Browser's sheet is drawn, not agreed | On top of R36, with the Browser's sheet | 5 |
| R38 | The agent can query the sample index | **not ruled** | After R36, through the same index | later |
| R46 | Browser preview in time with the project (half, normal, double) and in its key | **decided, not built:** the Browser draws a preview row with no player. A preview path into Cue Out is decided (ADR-0104 d2; MX-08, M1), and no row or queue reserves the engine's half. ADI's documents record Live's browser as transposing to key and syncing to scale. There is no real-time stretcher (TM-WT-04, P2) and no key detector (TM-MD-04, M3) | Build the decided preview first. Tempo sync follows the real-time stretcher, key follows the key analysis. The half and double switches are a small addition on top | 1, then 4 |
| R47 | Preview through the selected track's devices | **new.** The decided preview needs no project (ADR-0104 d2) and goes to Cue Out, as Live's | Yes, as a switch that is off by default, after the preview exists. The seam is built (the per-track input-monitor node) | 2 |
| R48 | A dragged sample lands already warped and transposed as previewed | **against:** "Every drop arrives clean" (ADR-0214 d1), kept as a standing deviation (ADR-0203 d12). Sample settings saved in the library are already recalled on import | The director's call. Win's view: keep the rule, with one narrow exception: while the preview's Sync switch is on, the drop carries what was heard, because the switch is a choice the user made | his call |
| R49 | "Fits": find samples that complement a loop (compatible key, energy where the loop is empty, hits in its gaps) | **new.** The masking measure is built. Key and onset analysis are planned (TM-MD-04, M3). Similarity itself is not built (R36) | Yes, after R36: the same index, one more scorer, no model | 3 |
| R50 | The agent can ask for samples that fit | **not ruled** | With R38 | later |

### D. Control, keys and the Stream Deck

| # | Request | ADI today | Recommendation | Wave |
|---|---|---|---|---|
| R39 | A table of parameter roles (gain, pan, EQ band, threshold); a controller bound once that follows the focus | **decided in another shape:** a surface follows the selection, with a Pin key and pick-up (ADR-0188 d4). One page model for every surface, with the director's 13 controllers first (SRF-14, P2). His controllers already carry a role table per plug-in. There is no shared role vocabulary | Role tags as an attribute of the one page model. His 13 tables are the first profiles. A profile is keyed on the plug-in's identity, never its name | with SRF-14 |
| R40 | A local WebSocket and OSC bridge | **decided:** one listener, loopback, a token (CTL-04 to CTL-09); OSC and OSCQuery (SRF-09, SRF-10; the cores are on the gemini seat's branches) | Nothing new. "There is no second server" (ADR-0181 d2) | as planned |
| R41 | The Stream Deck as a native ADI controller | **decided:** ADR-0181 names the Stream Deck + XL as the first surface. SRF-13 (P2) is ADI's own plug-in: it follows the selection, or the hovered Mix View row, and his 13 controllers are re-pointed at ADI. Nothing of it is built. It waits on the listener and pairing, parameter subscriptions, and SRF-04, SRF-07, SRF-14 | The director's call on priority: surfaces are third in the P2 order today | his call |
| R55 | Built-in key presets, with the user's own keys kept apart | **have:** presets `live` (the default), `cubase` and `bitwig`; the user's keys are stored by stable command id and survive every reload (WIN-04, on main). Today the Cubase and Bitwig presets differ from Live's only in Redo; their real tables are WIN-05's (M3) | Finish WIN-05 | as planned |
| R56 | A REAPER preset; a preset named "ADI Default" | **new.** The default is Live's by the standing rule, with ADI's ruled deviations | REAPER: the director's call, cheap once WIN-05's tables exist. A separate "ADI Default" is not needed | his call |
| R57 | A conflict detector; import and export of a key set; search | **decided, on branches:** import and export (`window/shortcut-sets`), clashes and reserved chords (`window/key-conflicts`). Still to build: Steal, Keep or Refuse; foreign key files; search by pressed key. All of WIN-05 lands after 3 December | As planned | as planned |
| R58 | A first-run chooser for the key preset | **new.** ADI has no first-run flow | Defer: the preset is one setting | later |

### E. The mix assistant and its guardrails

| # | Request | ADI today | Recommendation | Wave |
|---|---|---|---|---|
| R42 | Suggestions as a reviewable list; apply one, some or all; undo | **decided, engine built:** an agent's edits are a changeset the user applies as one undo step (ADR-0145 to ADR-0148). The review panel is P2 (AGT-05) | As decided | 6 |
| R43 | Learn how the user mixes, opt-in, local, inspectable | **noted, not ruled:** ADR-0205 d7 carries the director's own words | Rule it: learning is plain statistics over the finished project, not a model; a readable local file; off by default | 6 |
| R44 | Suggestions that cite the user's habits or a reference | **decided in part:** Propose Fix on the Radar; the analysis tools (AGT-07, P2) | The analysers of wave 3, then the agent proposes with a reason per change | 3, 6 |
| R45 | Model routing and inference inside the DAW | **against** (ADR-0233 d4; ADR-0064 d5) | Not recommended. The agent stays outside and proposes through changesets | none |
| R51 | Lock what the agent may change to chosen tracks, a group or a time range, enforced by the engine | **new.** The allow-list is by kind of edit; nothing limits a request to a target. An edit does not always name its track, so the check needs the project, which the changeset's validation already holds | Yes. The scope is a list of ids and a range fixed when the request is made, never "whatever is selected" (the ops rule, OPS §7.1). An edit with no single target, such as tempo or a route across the scope's edge, is refused under a narrow scope. It belongs with the agent's tier rules (AGT-09) | 6 |
| R52 | One labelled undo step for a whole agent task | **have:** one changeset is one transaction and one undo step, always (ADR-0145 d9, ADR-0233 d3). Only the label is missing: the step shows its first edit's name | A small addition: the step is named from the request | 2 |

## 3. The order win recommends

The outside drafts order by "phases" that each assume the one before exists. This order follows what ADI has.

1. **Finish what is decided.** Carve and Focus reachable; Align Apply and Fix Delay; the Reference with matched
   loudness; racks; the scope's divisions; Capture wired; the Browser's preview. Rows exist for all of it; most are
   M1 to M4. The key presets' real tables (WIN-05) are on their own row.
2. **Small additions on built ground.** The Throw command; Hear Difference on any device; the band drag on the
   Stack; several tracks aligned at once; auto gain-match with its commit; the duck helper; the capture strip; clip
   to instrument; preview through the selected track; the agent's step label.
3. **Analysis, no model.** The reference analyser and the mix snapshot analyser; the Radar's measures with the four
   new ones; sample descriptors, Swap Similar, then Fits.
4. **Offline clip work, stems and the stretcher.** Clip offline processing (TM-FC-03) with Preverb as its first
   user; the clip volume curve (AU-07) with clip prep; the stem runner; the Reference's stems; the real-time
   stretcher that gives the preview its tempo sync and OneShot its Warp.
5. **New surfaces, each after its sheet.** The soundstage; the health signs; the sample map; Mid/Side racks; EQ
   sketch and the MIDI-following band.
6. **The assistant.** The scope lock; the profile and the learning switch; proposals with reasons.

The Stream Deck (R41) runs beside these once the director sets its place.

## 4. Decisions for the director

Each has win's recommendation. None is needed before 3 December.

| # | Decision | Recommendation |
|---|---|---|
| D1 | Timing | Nothing of this starts before 3 December |
| D2 | The insert wrapper (R15 to R18) | Drop it. Devices and racks give the same results |
| D3 | Throws (R24) | The send-automation command first; clip-owned effects wait for TM-FC-03 |
| D4 | Preverb (R25) | Yes, as a rendered clip. Its swell lives on a new lane kind under the track that plays along with it (a format addition) |
| D5 | The soundstage page (R21) | Yes, pan and level first; its priority is yours |
| D6 | Ducked tails (R19) | The helper first; a switch inside Reverb and Delay only if you want it |
| D7 | The learning companion (R43; ADR-0205 d7) | Yes, in the shape of R43 |
| D8 | Mix health (R13) | The four measures join the Radar; fixes are proposals; strip signs after a sheet |
| D9 | EQ additions (R7 to R9) | Sketch: yes. MIDI-following band: yes. Transient-only bands: defer |
| D10 | Clip prep (R27 to R29) | Rider, gaps and breaths through the clip volume curve; sibilance later |
| D11 | Stems (R31 to R33) | A row for the runner. CPU first. Drum-kit and voice models: your call |
| D12 | Samples (R36, R37, R49) | Similarity by measured descriptors, then Fits; the map with the Browser's sheet |
| D13 | The Stream Deck (R41) and role tags (R39) | Role tags inside the page model. Where the Stream Deck sits in the order: your call |
| D14 | Three tiers (R3) | Defer |
| D15 | Capture (R30) | Wire it as decided; the longer ring and the strip: yes |
| D16 | Preview through the selected track (R47) | Yes, as a switch that is off by default |
| D17 | The conformed drop (R48) | Keep "every drop arrives clean"; one exception while the preview's Sync is on. Your call |
| D18 | The agent's scope lock and step label (R51, R52) | Yes to both; the lock joins the tier rules |
| D19 | Clip variations (R53) | Defer; say later whether ADI wants a generator |
| D20 | Clip to instrument (R54) | Yes: the command and the drop first, the rest with their rows |
| D21 | Key presets (R55 to R58) | Finish WIN-05. REAPER: your call. No "ADI Default", no first-run chooser now |

## 5. Not recommended

| Request | Why |
|---|---|
| A wrapper on every insert (R15, and R17 as a wrapper) | ADI has no insert slots, by ruling (ADR-0212). A rack is Live's own way to wrap a device |
| A strip-level shaper (under R20) | The same ruling. ADI-Shaper is the device |
| Level and EQ per stem inside one clip (R34) | A folded group of stem tracks is the same thing with nothing new to store |
| A transient unlimiter (R35) | Restoration is out of scope by the feature catalogue |
| A model inside the DAW (R45) | Ruled out twice. The agent proposes from outside |
| Three priority tiers now (R3) | It multiplies a function no user has reached yet |
| Transient-only EQ bands now (R9) | Needs a transient splitter that does not exist |
| Every drop conformed by default (R48) | Against ADR-0214 d1. The narrow exception is D17 |
| A preset named "ADI Default"; a first-run chooser (R56, R58) | The default is Live's, by the standing rule; the preset is one setting |
| The drafts' "do not build" lists | One names mix snapshots, which ADI plans (MX-19, P2). ADI's plan stands. The rest (cloud library, accounts, generative cloud audio, tutorial overlays) agree with ADI |

## 6. Found on the way

Gaps in what is already decided, to register when this intake lands:

- No plan row owns the stem separator's model, process or weights, though separation is P1 (ADR-0203 d8).
- Audio Capture has no row for its wiring. Its ring refuses more than 30 seconds while its setting allows 600.
- The Capture MIDI button answers "not built" though its engine parts are built and tested.
- The Browser's preview has no player, and no row or queue reserves the engine's preview path that MX-08 (M1) needs.
- The Cubase and Bitwig key presets differ from Live's only in Redo.
- A clip's transpose is stored, but no edit or control sets it, and it plays only with Warp on.
- `agent.maxOpsPerMinute` is a setting nothing reads.
- The plan's "On main" lines are stale for MX-13, MX-21, MX-24, MX-36, DEV-21, DEV-31, CT-01 and TM-MD-04.
- The feature catalogue lists stem splitting under remote services, against ADR-0203 d8.
- The agent's tier is per project in one document and per application in the settings registry.
- The EQ core has a loudness-match switch that no parameter sets; the Clip stage's Gain Link is named and unavailable.

## 7. Research

None is needed to decide. Later, and only for these, each read in Live 12:

- Similarity Search and Similar Sample Swapping, before R36 (parity).
- Stem separation, before R31's command is placed.
- The Browser's preview: tempo sync, the raw switch, key and loudness, before R46.
- A sample or clip dropped on a MIDI track, and Simpler's Warp, before R54.

The outside products the drafts were inspired by are not needed. The unmasking law already has its measured
references from the earlier round.

## 8. The ruling, and where each request went

The director's words, 25 November: "Approve all recommendations in MIX-SUITE-INTAKE.md as written and register all 11
Section 6 gaps into the single ADR and docs/PLAN.md", with these rulings of his own:

| Decision | His ruling | In ADR-0254 |
|---|---|---|
| D3, Throws | The target is a send to a return, or an inline device's or rack's own wet or mix parameter on the track | d3 |
| D6, ducked tails | A native Duck control inside Reverb and Delay, matching Echo | d6 |
| D11, stems | CPU only is fine. Yes to optional user downloads for a drum-kit model and for voice isolation | d11 |
| D13, the Stream Deck | The priority order stays as it is | d13 |
| D17, the conformed drop | While the preview's Sync is on, the drop lands warped and pitched; a setting switches that behaviour | d17 |
| D21, REAPER | Yes, at low priority after WIN-05 | d21 |

Where each request went. "Track S" is the plan's parallel track for the additions; S2 to S6 are its waves.

| Request | Where |
|---|---|
| R1 | finish DF-M11 and DF-M54; the key picked in the Mix View is DF-M75 (a) |
| R2, R13 | MX-21 and its note |
| R3 | DF-M70 (deferred) |
| R4, R6, R20 | nothing new |
| R5 | MX-37 (S2) |
| R7, R8 | MX-42 (S5); following the pitch of audio is DF-M72 |
| R9 | DF-M71 (deferred) |
| R10 | MX-13; several channels in one action is DF-M75 (b) |
| R11 | DF-M75 (c); the optimiser stays with the Kick and Bass Lab (P2) |
| R12 | DF-M74 |
| R14 | MX-39 (S2) |
| R15 | not taken; the Clip stage's Gain Link is DF-M73 |
| R16 | MX-38 (S2) |
| R17 | DEV-35 (S5); Transient and Sustain is DF-E83 |
| R18 | arrives with racks (DEV-01) |
| R19 | DEV-33 (S2) |
| R21 | MX-41 (S5) |
| R22, R23 | MX-29 and its note |
| R24 | AU-10 (S2) |
| R25 | TM-FC-05 (S4) |
| R26 | as planned (DF-W15, TM-WT-01) |
| R27, R28 | AU-11 (S4) |
| R29 | DF-W42 (deferred) |
| R30 | CT-06 (M3); the strip is CT-07 (S2) |
| R31, R32, R33 | AGT-10 (M5) |
| R34, R35, R45 | not taken |
| R36 | TM-MD-07 (S3) |
| R37 | TM-MD-09 (S5) |
| R38, R50 | DF-C7 (deferred) |
| R39 | SRF-14's note |
| R40 | as planned (CTL-04 to CTL-09, SRF-09, SRF-10) |
| R41 | SRF-13, its place unchanged |
| R42, R44 | AGT-05 and AGT-07, with their notes |
| R43 | AGT-11 (S6) |
| R46 | MX-08 with N76 and DF-E82; tempo and key are TM-MD-06 (S4) |
| R47 | MX-40 (S2) |
| R48 | TM-MD-06 (S4) |
| R49 | TM-MD-08 (S3) |
| R51, R52 | AGT-12 (S6; the step's name may land with S2) |
| R53 | DF-W44 (deferred) |
| R54 | DEV-34 (S2) |
| R55, R57 | WIN-05 |
| R56 | WIN-11 (P2); no preset named "ADI Default" |
| R58 | DF-W43 (deferred) |
