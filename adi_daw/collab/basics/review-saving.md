# Review of saving: log

A review, with proof, of saving: creating, saving, opening and closing a project, the folder per song,
backups on a timer, snapshots, session copies, quitting without saving, and opening a project written by
an older version. A finding counts only when a headless test reproduces it.

- Code: branch `basics/review-saving-chain` (one chain, no pull request).
- This log: branch `basics/review-saving-log`, file `adi_daw/collab/basics/review-saving.md`.
- Checks run on the build PC (MSVC Debug, warnings as errors). Linux and macOS run later in CI.
- The findings carry no register id: none was taken. They are numbered here only (finding 1, 2, 3).

## Round 1 (28 November 2026)

Started from `origin/land/basics-1128c`; `origin/main` took that stack in during the round and was merged
into the chain (`8e68fa122`).

| Finding | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| 1. A project file the system protects from writing cannot be opened: "Cannot open project: sql error" | `basics/review-saving-chain`: test `6b4ec213c`, fix `da8dfc619` | `src/adi/store.cpp` (engine seat's file): SQLite opens such a file for reading even when asked to write, and entering WAL mode then failed the whole open. The store now opens it read-only, as it does a project of an unsupported major; an exclusive open for writing still refuses. `src/adi/ui/project_document.cpp` (window seat's file): the open notice says "This project's file is write-protected, so it opened read-only. Use Save As to keep your changes." Save As from it gives an editable project. | Tests `writeProtectedSource` in `tests/test_project_files.cpp` (core) and `writeProtectedProject` in `tests/test_ui_document.cpp` (window suites). **Slot check: see "State of the proof" below.** | see below |
| 2. Save As and the first save fail when anything is written to the project while the worker copies it: a click that moves the selection, Play or Stop, an edit. The message was "Save As failed: copy saved but the original project changed before switching", a stale copy stayed at the destination, and trying again met "already exists". The window stays in use during a save (the progress window is not modal), so on a large project this is easy to do. | `basics/review-saving-chain`: test `6b4ec213c`, fix `e0fb0f7d6` | `src/adi/ui/project_document.cpp`: when the adoption is refused for that one reason, the document makes the copy again over exactly the stale copy (by its file identity), at most twice more (`SaveJob::repeatsAllowed`), so the save carries what was done while it ran. Past the bound it fails as before. `src/adi/project_files.cpp`, `src/adi/store.hpp` (engine seat's files): `StoreFileSave::takeStaleCopy()` says whether the refusal was that reason, hands over the copy's identity, and takes back the media a first save published, so the song's folder holds each file once. The store's own refusal is unchanged (`stagedFileSaves` still proves it). | Test `saveWhileTheWindowIsUsed` in `tests/test_ui_document.cpp`: a first save with a selection change, then a Save As with Play and a rename, each before the owner can adopt. Also changed: the last part of `largeProjectSaveResponse` (it expected the failure); that function runs only by hand (`--large-project-save-only`), no check runs it. **Slot check: see below.** | see below |

## Round 2 (28 November 2026, a second session from about 17:30)

The chain is checked out under a local name in this session's own worktree (the first session's
worktree still holds `basics/review-saving-chain`); pushes go to `origin/basics/review-saving-chain`.

| Finding | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|
| 3. A clock set back sends the newest backup to the Recycle Bin. The ten kept are chosen by the UTC time stored in each copy (ADR-0213 d4), so a backup written after the clock went back carries the earliest time, is the eleventh, and was offered for the bin the moment it was written. A dual-boot PC whose two systems disagree about the hardware clock does this at every boot. | `basics/review-saving-chain`: test `0cbf0d5ae`, fix `ff10e56e1` | `src/adi/project_files.cpp`, `src/adi/project_backup.hpp` (engine seat's files): `backup::expiredBackups(project, writtenUtc)` takes the stamp just written; that backup counts as one of the ten and is never offered. `backup::write` passes it; `src/juce/ui_main.cpp` (window seat's file) passes it on the synced-project path. The order (d4) is unchanged. Limit: while the clock stays behind, the backup written one turn before is the one binned next; the real cure is under "For the director" 3. | Test `backupsAfterTheClockGoesBack` in `tests/test_project_files.cpp` (core): ten backups an hour apart, then one an hour before the first; then one more through `Store::writeProjectBackup`. **Slot check: owed, see "State of the proof".** | not proved yet |

### State of the proof

The build place (slot 12) was held by a warm-up of the base for an hour (it ended with one failure that
is not this chain's: `adi_decode_space_tests`, "cold progressive worker fully decodes the valid compact
constant stream"; the same suite passed in the next run, so it looks like a timing fault under load).

**Done at 16:47:** head `8e68fa122`, mode core: MSVC Debug build with warnings as errors, then
291 of 291 tests passed (`adi_project_files_tests` with `writeProtectedSource` among them).
**Done at 16:57:** the window build of the same head compiled. Its suites were still running when this
line was written. What is owed, in this order:

1. `bash /d/adibuild-tools/slot.sh run 12 origin/basics/review-saving-chain all` and wait: both fixes are
   in, everything must pass. The new tests are the four named above.
2. The proof that the tests fail without the fixes: the test commit alone is `6b4ec213c`. Run the same
   check on it (at head `8e68fa122` it is `origin/basics/review-saving-chain~3`; if the script refuses
   that form, push `6b4ec213c` under a throwaway branch name) and expect exactly these failures:
   `adi_project_files_tests`: "a write-protected project opens read-only instead of failing";
   `adi_ui_document_tests`: "save in use: a click that moves the selection while the first save copies
   does not fail it" and "write-protected: the project opens".

Until step 1 passes the two rows are **not proved**; they were written from a careful reading only.

**Round 2.** At 17:03 the first session started step 2 on `origin/basics/review-saving-proof` (=
`6b4ec213c`); at 17:47 it was still "waiting for a build place". Finding 3's test commit alone is pushed
as `origin/basics/review-saving-proof3` (= `0cbf0d5ae`): a core check filtered to `adi_project_files`
must fail exactly "clock set back: the backup just written is not offered to the Recycle Bin" and "clock
set back: the oldest of the other backups is offered instead". Step 1 now covers finding 3 too.

## Suspected, not proved

- (Round 2: this one now has a test and a fix, finding 3.) **A clock set back sends the newest backup to the Recycle Bin.** `backup::expiredBackups` keeps the ten
  with the latest stored UTC time (ADR-0213 d4: "ordered by a UTC time stored in each copy, so a clock
  that goes back an hour overwrites nothing"). Nothing is overwritten, but a backup written after the
  clock went back carries the earliest time, so it is the eleventh and is binned at once, and so is each
  one after it until the clock passes the old times. Read in `src/adi/project_files.cpp`
  (`projectBackups`, `expiredBackups`); no test written. A dual-boot PC whose two systems disagree about
  the hardware clock does this every boot.
- **A writable project file in a folder that refuses new files** (POSIX: a directory without write
  permission) still fails to open as "sql error": SQLite cannot create its journal. Finding 1 covers only
  the file SQLite itself reports as read-only. Not reproduced: needs a non-root POSIX run.
- **The first Ctrl+S cannot keep the name "Untitled".** `requestSaveAs` in `src/juce/ui_main.cpp` maps
  the chosen `Untitled.adi` in the projects folder to `Untitled/Untitled.adi`, which is the open project,
  and answers "The current project is already at this location"; the project stays unnamed. In the
  application's own file, which no headless test reaches.
- **When a save made again cannot start or is cancelled (finding 2's fix)**, the stale copy stays at the
  destination; for a first save its media were already taken back, so that leftover file would open with
  its samples missing. The open project is untouched. Not reproduced.

Added in round 2:

- **A song whose own folder is named "Backup" never gets a timed backup, and nothing says so.**
  `requestProjectBackup` in `src/juce/ui_main.cpp` turns the schedule off when
  `store().path().parent_path().filename() == "Backup"` (meant for a backup opened from its song's
  Backup folder). `backup::prepare` already refuses a real backup by its stamp (`backup::isBackup`), so
  the name test only catches songs kept in a folder of that name (an external drive's "Backup" folder,
  say). Suggested fix: test `backup::isBackup(store)` instead. In the application's own file, which no
  headless test reaches; a test needs the predicate moved into `src/adi/ui/project_backup_schedule.hpp`.
- **Timed backups follow any write, not only an edit.** The schedule's revision is
  `sqlite3_total_changes64` plus `PRAGMA data_version` (`queueProjectBackup`), and a selection change,
  Play or Stop also write to the project (finding 2 depends on that). So a session spent only listening
  and clicking writes a backup every ten minutes, and after 100 minutes the ten kept are copies of the
  same state, the earlier distinct ones gone to the Recycle Bin. Not reproduced: the revision is read in
  the application's file. If it holds, the revision should be the undo head and op count (as
  `DailyHistorySnapshots` uses), not the raw change counter.
- **macOS: a project path spelled in another Unicode normalization than its stamp.** `readOwnedBackup`
  compares the stored `backup_origin` byte for byte with `project.filename()`. A name with an accent,
  opened once by an NFC path and once by an NFD one (Recent list, Finder, command line), may stop
  matching its own backups. Needs a Mac to try.

## For the director

1. **Finding 1 adds one sentence a user sees**, in today's Status Bar notice at open: "This project's
   file is write-protected, so it opened read-only. Use Save As to keep your changes." Live opens a locked
   Set and asks for another name when saving. Recommendation: keep it; change the words if you wish.
2. **Finding 2: what a save does when you keep working while it copies.** Before: the save failed and
   said so. Now: the copy is made again (up to twice more) and the save succeeds with what you did.
   Choices: **(a)** as built; **(b)** as Live: the window takes no input while a save runs, so nothing can
   change under it; **(c)** as before: refuse and say so. Recommendation: (a). It loses nothing, and (b)
   would also stop Play during a long save.
3. **Backups and a clock set back** (first line under "Suspected"): ADR-0213 d4 rules the order by stored
   time. Choices: **(a)** leave it; **(b)** never bin the backup just written; **(c)** order backups by
   the project's own edit count stored in each copy, time second. Recommendation: (c), after a test
   proves the fault; it needs a ruling because d4 names the order.
   **Round 2:** (b) is built as finding 3, because it keeps d4's order and only stops the newest backup
   being binned. It leaves this: while the clock stays behind, each new backup is kept and the one written
   just before it is binned next, so the ten kept are the nine from the "future" plus the newest. (c)
   would fix that and still needs the ruling. Reply: "3c" to build it, "3b" to keep only what is built.

## For the next session

- First: the two slot checks under "State of the proof". If the full check fails to compile, the new
  code is in `store.cpp` (one block in `openWithLease`), `project_files.cpp` (`takeStaleCopy`, two fields
  on `CopyState`, the constant `kSourceChangedBeforeSwitch`), `project_document.cpp` (`startCopy` in
  `advanceSave`) and the two test files.
- Then write the test for the clock set back (core: `tests/test_project_files.cpp`, beside
  `automaticProjectBackups`: ten backups at T+1..T+10, one at T-5, expect it not in `expired`).
- Read and not faulted this round: `project_files.cpp` whole (copies, Save As, first save and its media,
  backups, restore), `store.cpp` open and migration, `project_document.cpp` open and save, the backup
  schedule, the untitled life cycle in `ui_main.cpp`.
- Not read yet: `history*.cpp` (snapshots, daily versions, compaction), `media/collect_export.cpp`,
  `media/publish_file.cpp` beyond `replaceFile`, recording recovery, `ui_backup_trash.cpp`.
