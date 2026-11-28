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
| `window-fn/loop-record` | `d1e692f80` | itself | 34% | to take | CT-02 window half, a take per pass |
| `window-fn/send-pan` | `1a9e2dab0` | itself | 29% | to take | MX-06, a pan on a send (menu) |
| `window-fn/split-pan` | `5c5833410` | itself | 18% | to take | MX-02 Split Stereo Pan |
| `window-fn/automation-curves` | `f170fbe3a` | itself | 2% | to take | AU-01, curved segments drawn as played |
| `window-fn/n64-snapshot-arm` | `d7ee13349` | itself | 66% | to take | N64 window half; its engine half is on main |
| `window-fn/note-batch` | `52a803f96` | itself | 87% | to take | MI-01 window half; N63 is on main |
| `window-fn/open-saved-path` | `2dac35671` | itself | 92% | to take | N68 window half; its engine half is on main |
| `window-fn/param-touched` | `7ebeac6bb` | itself | 0% | to take | SRF-01, the last-moved parameter |
| `window-fn/object-snap` | `8fa8a9baf` | itself | 3% | to take | ED-09 object snapping |
| `window-fn/left-trim` | `bd594ef79` | itself | 18% | to take | ED-04, a clip's left edge |
| `window-fn/insert-shape` | `f1da17a0d` | `window-fn/simplify-automation` | 16% | contained in | AU-02 |
| `window-fn/simplify-automation` | `7e8629197` | itself | 15% | to take | AU-08 on AU-02 |
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
key-conflicts (with shortcut-sets). Sixteen branch heads, eighteen branches.
