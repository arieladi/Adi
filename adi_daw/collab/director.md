# The director's remarks on the window

The first-window gate (`docs/LANES.md` §5): the director opens the Windows build win hands him,
says what is wrong, and each remark becomes a window-lane item that comes before new items
(LANES §4 window item 4). Win writes them here the same day, in the director's words first, then
win's reading of the cause. A remark is struck when the PR that answers it merges. Newest build at
the top. Only win writes to this file. Since 5 November it also holds his answers to the plan's choices (`docs/PLAN.md` §6), in his words where he gave them, so LANES §5 can point here.

---

## 2026-11-05 — the plan's first choices, answered in chat (binding; recorded 8 November)

The plan (`docs/PLAN.md`, #462) asked six choices now (Q-A1, Q-A2, Q-B, Q-C, Q-D, Q-E), the gate
G-1, and in §6.3 the Pd editor's three (Q-K1 to Q-K3) and Q-GRID. He answered them all in chat on
5 November. The letters are the plan's options; the ADR that records each is named.

| Choice | His answer | Where it is recorded |
|---|---|---|
| Q-A1, what stays reachable at the undo cap | **A**: every named snapshot and every dead end kept as deduplicated stored states; the cap always holds; compaction best-effort (P-10 stands) | ADR-0213 |
| Q-A2, Live's Backup folder | **(a)**: ten timestamped copies in the project's Backup folder from the second Take Snapshot, as Live | ADR-0213 |
| Q-B, what STACK shows before the unified feature | **(a)**, in his words: "each track's own devices, Live's and VST3s loaded as plug-ins, like in the Ableton device area, only with the FabFilter Pro-Q instance view where what I am standing on and tinkering with gets a zoomed-in vibe, bigger than the rest, and I can move to the rest and they get bigger instead; VST3s as live tiles with the fisheye; when the processor lands, it joins the row" | ADR-0212 (its Q-B paragraph); LANES §3b item 2 |
| Q-C, the control API | **(a)**: it starts now, on a seat of its own | ADR-0232; LANES §1 and §3c; `collab/prompts/2026-11-05-control-api-seat.md` |
| Q-D, one beat, tempo and key analyser | **(a)**: benchmark first, then one analyser for ADI and ADiJ; qm-dsp in the DAW meanwhile behind one interface | ADR-0226 |
| Q-E, short samples on drop | **(a)**: every drop arrives clean; Live's short-sample result on Warp or from the import prompt; the setting offers Live's four choices | ADR-0214 |
| G-1, the Live 12 measurement seat | he runs the Live 12 readings himself, with a Claude session on the second Windows machine, from Sunday 9 November; his brief is in his private `Live12-readings` folder | LANES §1 (measurement seat) and §5; ADR-0220 |
| Q-K1, the Pd editor's route | **A**: vanilla Pd's editor attached to the running device, edits heard at once | ADR-0215 |
| Q-K2, a user patch's delay | **1**: no added delay when the buffer is a multiple of 64, events on Pd's tick; one reported block otherwise | ADR-0215 |
| Q-K3, editing a built-in | **1**: editing makes a copy "<Device> (edited)" on the Pd path | ADR-0215 |
| Q-GRID, the Grid-style tier | **(a)**: the hybrid on Pd with host-owned timing (DEV-15) | no ADR yet: the plan places that ADR before any Grid-style work (M7); LANES §3 item 6 and §4b item 3 carry the answer meanwhile |

His ruling of 5 November on the slots (no reserved plug-in slots; no separate built-in ADI-Q or
ADI-L; his VST3 and CLAP builds stay ordinary plug-ins; one unified built-in feature from their
source when his list arrives; it never hosts the VST3) is ADR-0212. The placements told with the
questions (`docs/PLAN.md` §6.2, P-1 to P-10) stand unless he strikes one.

---

## 2026-11-02 evening — build `add5102` (ADI.exe of 18:23; the chain #331 to #352 not yet in it)

The director's words:

> channels do not expand or get smaller when clicking on the triangle, the manual expand or make
> smaller with the mouse works but the borders are not visible
> transition to mix view and detach and get it docked again is terrible
> i can't click and drag channels or groups to move them
> select multiple channels holding shift works but grouping them not: it only groups one channel
> and i can't add any other channels into it
> clicking on automation and lock buttons do nothing
> i stopped here because there must be much more. the mix view looks terrible but we can get to
> it later

| # | Remark | Win's reading | Window item |
|---|---|---|---|
| R1 | The triangle on a track does not expand or shrink it | On a plain track Live's triangle unfolds the automation lanes (manual 19.1), which wait on N26; ADI folds groups only (`foldedAway`). A click on a plain track's triangle did nothing and said nothing | A click on the triangle of a track with nothing to unfold says so in the Status Bar until N26 lands; the group fold stays. When N26 lands, the triangle unfolds the lanes as Live's |
| R2 | Resizing a track with the mouse works, but the borders are not visible | The lane edge the mouse grabs is not drawn as Live draws it (the thin divider under each track header, and the cursor change over it) | Draw Live's divider under each track header and the resize cursor over it (parity 7.3); keep the drag |
| R3 | The transition to the Mix View, and detaching it and docking it again, is terrible | ADR-0209 P1 built the panel, F4 and the top-bar button, docked or detached (#320 to #323), on the first pass | The window lane takes the Mix View's switch, detach and re-dock as one item: what "terrible" is in detail, the director says on the next build; until then, no flash on switch, the detached window keeps its place and size, docking returns it where it was, the button and F4 toggle both ways |
| R4 | Tracks and groups cannot be dragged to move them | `window/track-move` (#347) and `window/return-move` (#350) are in the chain, not in this build | None new: #347 and #350 merge today; the next build has them |
| R5 | Shift-click selects several tracks, but Group Tracks groups only one and nothing can be added to the group | `window/group-selected` (#339) is in the chain, not in this build; dragging a track into a group is #347 | None new: #339 and #347 merge today |
| R6 | The automation and lock buttons do nothing | They are drawn and wait on N26 (automation data in the snapshot); a click neither acts nor says so | A click on either says what it waits for in the Status Bar (the window's own rule for every dead control) |
| R7 | The Mix View looks terrible; later | Noted for after the Arrangement passes | The Mix View's look is a later round, after the director's next remarks on it |

**For the director's next round** (the build after the chain merges tonight): the things in this
build that take a click and are worth a look, so the remarks reach everything: New (Live's default
set), Insert Audio/MIDI Track, Rename, Delete, Group and Ungroup with undo, the colour chooser on a
track's menu, track height and the Optimize commands, zoom (Ctrl + wheel) and the grid menu, the
Mixer (Ctrl+Alt+M) with its In/Out, Sends and Returns sections, the Settings window (Ctrl+,) on
Live's ten tabs, the range selection on the lanes (drag, Delete, 0), Select All, dragging an audio
file onto a track, the Channel EQ window, the Browser's sidebar, the Info View and the Status Bar's
messages. Nothing plays, and the clips are drawn as their colour and name until the engine's N40.
