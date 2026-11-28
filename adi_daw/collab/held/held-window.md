# held-window — log

**This chain lands only when the director says so.** Nobody opens a pull request for it before then.

The mission (28 November, round 1): gather the finished work of the window lane (`window/*`), the window's
function seat (`window-fn/*`) and the Codex window seats (`codex/*`) that never landed while landing was held
("lands after 3 December"), into one proved chain, `held/window-chain`. This log is on `held/held-window-log`;
only this stream writes it. Newest entry at the top, the survey below them. A session that continues this
stream reads the newest entry first.

- **BASE:** `origin/land/basics-1128c` while it is not an ancestor of `origin/main`; `origin/main` once it is.
  The chain merges the newest BASE before each push.
- **Where it is proved:** the project's build PC, slot 17 (`bash /d/adibuild-tools/slot.sh run 17
  origin/held/window-chain <core|ui|all>`), MSVC Debug, warnings as errors.

---

## For the director

(Each with this stream's recommendation. Nothing here holds up the chain's proof.)

1. **`window/command-palette` (WIN-06): left out of the chain.** It adds a surface Live does not have: a
   palette over the window on Ctrl/Cmd+P, drawn in today's look, and no agreed sheet shows it (its seat says
   "until the menus and dialogs sheet is agreed"). **Recommendation:** agree its place from a picture with the
   menus-and-dialogs sheet, then this stream merges it (it merged cleanly with main on 24 November).
2. **`window/held-peak` (DF-W32, N48's window half): left out.** It changes what the strip's meter shows (the
   engine's held peak); its own seat holds it "for the Mixer sheet", and no Mixer sheet is agreed.
   **Recommendation:** take it with the Mixer sheet.
3. **`window/shaper-panel-fixes`: left out.** Its four fixes sit on the ADI-Shaper panel stack, which is the
   clones seat's and waits in #716 (open). **Recommendation:** land it with #716 when you release that panel.

---

## 2026-11-28, round 1 — step 2, the chain (gathered; its proof is running)

`held/window-chain`, from BASE. BASE was `origin/land/basics-1128c` when the chain began; it landed on main as
#838 during the round, so BASE is `origin/main` now and the chain has merged it (head `b85ca765`).

**Merged** (one merge commit each; every collision kept both sides; the merge commits say how):

| Branch | Seat head | Row | Collisions |
|---|---|---|---|
| `window-fn/summing-drive` | `1e2915b9` | MX-23 drive test | none |
| `window-fn/tap-quiet` | `96ad25fc` | HI-01 | 5 files: BASE's threaded saves (`startSave`) and history restore also end a tap run |
| `window-fn/send-pan` | `1a9e2dab` | MX-06 | 1; plus the two MSVC/Clang fixes the 23 Nov landing made (`1012a2c2`, `6cd7b273`), which the seat's head lacks |
| `window-fn/split-pan` | `5c583341` | MX-02 | 2 files: BASE's `panMenuItems` seam now takes the clicked half; a split half offers no Delete Automation (no target of its own yet) |
| `window-fn/param-touched` | `7ebeac6b` | SRF-01 | none |
| `window-fn/object-snap` | `8fa8a9ba` | ED-09 | 1 (tests) |
| `window-fn/remarks` | `a1e78539` | RE-01 | 5 files: the header menu's remarks follow BASE's clicked track |
| `window/text-goldens` | `b88bd09a` | WTS-03 first half | 1 (an include) |
| `window/key-conflicts` (with `shortcut-sets`) | `cc7174a1` | WIN-05 part | 3 files: the Latin-key fallback after BASE's computer-MIDI key |

**Found on BASE by another route while merging, so not merged** (the survey's line share could not see these:
BASE rebuilt them in other code):

| Branch | Row | On BASE as |
|---|---|---|
| `window-fn/loop-record` | CT-02 | SC-27's loop recording (`prepareLoopCommit` in `landTake`), with its own `loopRecord` tests |
| `window-fn/automation-curves` | AU-01 curves | SC-347 (`835e403a`): `AutomationCurveProjection` draws every segment as played, through the tempo map, keeping a trimmed source's slice in a drag |
| `window-fn/note-batch` | MI-01 | SC-412 (`5efef2b7`): a MIDI gesture commits as one note diff; BASE's tests count one op per step |
| `window-fn/open-saved-path` | N68 window half | a comment and a test label on `openSettled`, which SC-330/SC-415 replaced; nothing left to carry |
| `window-fn/left-trim` | ED-04 | `551be89b` and `9e304065`: the left edge from mouse and keyboard, through `edit_plan::prepareTrim` |
| `window-fn/simplify-automation` (with `insert-shape`) | AU-02, AU-08 | Insert Shape, Simplify Envelope and Delete Automation on the lanes' menu, through the engine's splice |

**Not merged, for its seat:** `window-fn/n64-snapshot-arm` (N64). It makes Arm and monitoring read the snapshot's
`recordArmed`; BASE has since built selection arming on the session (`session_.recordArmed`,
`setSelectionArmedTrack`, `refreshSelectionArming`) on the very handoff N64 removes. Merging it would undo
that. It needs its seat to rebuild it on BASE (read the snapshot and keep selection arming), or to retire it.

**Proof:** slot 17's first check (cold, `all`) was started on the chain's second commit and was still waiting
for a build place after nine minutes; the next check runs on the head. Results go in the next entry.

---

## 2026-11-28, round 1 — step 1, the survey

Read: `collab/window.md` on `window/log` (its entry of 25 November, "where the lane stands"), `collab/window-fn.md`
on `window-fn/log`, and the landing pull requests #717, #724, #725 (closed), #730 and #733 (merged). #733's body
says why the window-fn link was taken out of the 23 November landing: every row of it is after 3 December
(ADR-0248 d2). So `window-fn/*` and the window lane's four after-3-December requests are the held work.

How each row was judged: `git branch -r --no-merged origin/main`; for each branch its head, whether another
surveyed branch contains it (`merge-base --is-ancestor`), `git cherry` against main, and the share of the lines it
adds that already stand in BASE's files (a carried pull request changes the patch id, so cherry alone misses it).
"In BASE" below is that share.

| Branch | Head | Top of stack | In BASE | State | Why |
|---|---|---|---|---|---|
| `window-fn/summing-drive` | `1e2915b9f` | itself | 4% | to take | MX-23 drive test; after 3 Dec (#733) |
| `window-fn/tap-quiet` | `96ad25fca` | itself | 24% | to take | HI-01 Tap Tempo through a quiet gesture |
| `window-fn/loop-record` | `d1e692f80` | itself | 34% | already on main | CT-02 window half; on BASE as SC-27 (step 2) |
| `window-fn/send-pan` | `1a9e2dab0` | itself | 29% | to take | MX-06, a pan on a send (menu) |
| `window-fn/split-pan` | `5c5833410` | itself | 18% | to take | MX-02 Split Stereo Pan |
| `window-fn/automation-curves` | `f170fbe3a` | itself | 2% | already on main | AU-01 curves; on BASE as SC-347 (step 2) |
| `window-fn/n64-snapshot-arm` | `d7ee13349` | itself | 66% | unclear | N64 window half; collides with BASE's selection arming, for its seat (step 2) |
| `window-fn/note-batch` | `52a803f96` | itself | 87% | already on main | MI-01 window half; on BASE as SC-412 (step 2) |
| `window-fn/open-saved-path` | `2dac35671` | itself | 92% | already on main | N68 window half; its code was replaced by SC-330/SC-415 (step 2) |
| `window-fn/param-touched` | `7ebeac6bb` | itself | 0% | to take | SRF-01, the last-moved parameter |
| `window-fn/object-snap` | `8fa8a9baf` | itself | 3% | to take | ED-09 object snapping |
| `window-fn/left-trim` | `bd594ef79` | itself | 18% | already on main | ED-04; on BASE through `prepareTrim` (step 2) |
| `window-fn/insert-shape` | `f1da17a0d` | `window-fn/simplify-automation` | 16% | contained in | AU-02 |
| `window-fn/simplify-automation` | `7e8629197` | itself | 15% | already on main | AU-08 on AU-02; on BASE through the engine's splice (step 2) |
| `window-fn/remarks` | `a1e785390` | itself | 14% | to take | RE-01, remarks from the window |
| `window/text-goldens` | `b88bd09a1` | itself | 1% | to take | WTS-03 first half (tests only) |
| `window/shortcut-sets` | `c08afd7ba` | `window/key-conflicts` | 17% | contained in | WIN-05 first half |
| `window/key-conflicts` | `cc7174a1e` | itself | 13% | to take | WIN-05 part, on shortcut-sets |
| `window/command-palette` | `051ec6c0a` | itself | 4% | for the director | WIN-06: a new surface Live lacks, no agreed sheet |
| `window/held-peak` | `8b527bbb3` | itself | 5% | for the director | DF-W32: its seat holds it for the Mixer sheet |
| `window/shaper-panel-fixes` | `c10003c49` | itself | 1% | left out | lands with #716 (the clones' panel stack, open) |
| `window-fn/shaped-dither` | `93f7541ba` | itself | 0% | left out | EX-06 sits on `engine-win/export-quantizer`, not landed |
| `window-fn/df-e78` | `8f44a2153` | itself | 5% | left out | on `engine-win/export-profile-validation`; lands with the export stack |
| `window-fn/function-keys` | `97036ee9e` | itself | 26% | abandoned by its seat | WIN-01 withdrawn; the window lane's landed in #739 |
| `window-fn/shortcut-overrides` | `49bbc68e9` | itself | 10% | abandoned by its seat | WIN-04 withdrawn; landed in #739 |
| `window-fn/lane-ref-canonical` | `996d5b1fe` | itself | 50% | abandoned by its seat | superseded by `window-fn/df-w38-param-ref` (landed) |
| `window-fn/play-first-block-main` | `848bf6b92` | itself | 97% | already on main | DF-E64 landed through engine-win (a7983164) |
| `window/launch-isolation` | `6d9834e61` | itself | 97% | already on main | superseded by `window/launch-isolation-main` (landed) |
| `window/main-row-readers` | `b93f43c57` | itself | 26% | abandoned by its seat | superseded by #744 |
| `window/browser-devices-test` | `886492930` | itself | 54% | already on main | `git cherry`: its one patch is on main |
| `window/plugins-browser` | `259a2b901` | itself | 46% | abandoned by its seat | #676 closed; superseded by window-fn's `8d46c9fa` on main |
| `window/clip-view` | `dd81946bf` | itself | 72% | abandoned by its seat | 29 Oct plan and parity text, no code; the Clip View is built on main since |
| `window/clip-view-timesel` | `cda57e510` | itself | 100% | already on main | #607 merged |
| `window/delete-track-parity` | `1b23701c8` | itself | 100% | already on main | #477 merged |
| `window/proposals` | `bebbcee99` | itself | 0% | left out | four proposal sheets, kept on their branch as link targets for the director |
| `window/log`, `window-fn/log` | `a44a7ad3a`, `d52bcdafe` | — | 0% | left out | the seats' own logs, on their own branches |
| `codex/step7-root-window` … `codex/step7-mixer` (6) | `8d5e02884` … `36c8de434` | `codex/hybrid-reverb` | 64-83% | abandoned by its seat | #196, #198, #199, #203, #204, #205 closed: rebuilt by the window lane as #246 to #296 |
| `codex/oneshot`, `codex/sampler` | `072c81371`, `b982fda8d` | `codex/hybrid-reverb` | 66%, 46% | abandoned by its seat | #206, #208 closed: carried by the devices lane (#278) |
| `codex/midi-effects`, `codex/hybrid-reverb` | `c130f0e70`, `7f421f4df` | `codex/hybrid-reverb` | 43%, 48% | abandoned by its seat | #207, #209 closed; the fourteen MIDI kernels are register row DF-E39 (devices) |
| `codex/video-production-handoff` | `87ec9f9a8` | itself | 0% | left out | not ADI: `tools/video-production`, private paths |
| `land/window-1123`, `land/window-fn-1123`, `land/window-fn-1122b` | `3413cb868`, `854e0ba5c`, `0d0376043` | — | 38-53% | left out | closed landing stacks (#724, #725, #717); they carry engine and clones links; their window-fn rows are taken from the seat branches above, at the seats' newer heads |

**To take, oldest first** (the window-fn seat's request order, then the window lane's): summing-drive, tap-quiet,
loop-record, send-pan, split-pan, automation-curves, n64-snapshot-arm, note-batch, open-saved-path,
param-touched, object-snap, left-trim, simplify-automation (with insert-shape), remarks; then text-goldens and
key-conflicts (with shortcut-sets). Sixteen branch heads, eighteen branches. (Step 2 found six of them on BASE by
another route and one that collides with BASE's design; its entry above says which.)
