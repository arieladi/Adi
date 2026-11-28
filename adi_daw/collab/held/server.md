# Held stream: plug-ins on another computer (DEV-27)

**This chain lands only when the director says so.** Chain `held/server-chain`; this log on
`held/server-log`. No pull request is opened for either.

The director named the mission on 28 November: "audiogridder in adi and server". PLAN row DEV-27,
ADR-0261 d20 (it must stay carried). It continues the engine seat `server` (its log is
`collab/seats/server.md` on `origin/seats/server-log`; its code is `origin/seats/server-chain`,
checked on Windows by `origin/wincheck/seats/server-chain` and with libc++ by
`origin/libcxx/seats/server-chain`). The study is `docs/ENGINE-PLUGIN-SERVER.md` on the chain.

## The gate

1. A plug-in hosted by a server plays as a device through ADI's player.
2. A project with such a device, opened without the server, loads the plug-in locally.
3. Never a socket on the audio thread.
4. An offline render waits for the server and fails by name.

Everything testable on one machine with a server on loopback is in reach. What needs two machines is
said plainly in its row.

## How this stream works

- Base: `origin/land/basics-1128c` until it is an ancestor of `origin/main`, then `origin/main`. Merge
  the newest base into the chain before each push.
- Builds and tests run on the project's build PC, slot 19, through `slot.sh` (MSVC Debug, warnings as
  errors). Linux GCC/Clang and macOS run later in CI.
- The five validators run before each push.

## Rounds

| Round | Item | Chain head | What changed | How it was proved | State |
|---|---|---|---|---|---|
| 1 | Chain made | `2df6b6502` | `held/server-chain` = base `land/basics-1128c` at `16064c0a6` + `seats/server-chain` at `bc7877193` (which already holds its wincheck `321ea3fb2` and libc++ `5cce04174` branches). No conflicts. The base already carries the control seat's `control/agent-chain` (AGT-02 project UUID writer, AGT-03 consent ledger), which the server seat was waiting for. | Five validators pass. MSVC Debug /WX on slot 19, mode `all`: core built; 300 of 302 core suites passed, 1 skipped (ffmpeg loudness, no ffmpeg); the 4 window suites and the app passed. The two failures are the next two rows. This is the native MSVC result the server seat's log said was still owed. | done |
| 1 | Admission fixture after AGT-02 | `bc90e67dd` | `adi_plugin_admission_tests` failed on MSVC: "projects awaiting a durable identity ask no ledger". Cause: the base's AGT-02 Store now writes a project UUID when it creates a file, so the fixture's "legacy file with no UUID" had one. The fixture now clears it explicitly. Test-only. | Failing check seen in the slot run of `2df6b6502`; fixed run below. | checking |
| 1 | `adi_agent_log_tests` on Windows | none | Fails on MSVC in the base itself (the control seat's code, not this chain's): "all three child writers append successfully", "competing processes lose no entries", "every appended entry has a distinct chronological position". Its log says the Windows path was not run natively. Not mine to fix (control seat's files); listed under "For win". | slot run of `2df6b6502` | reported |
| 1 | Hosting lease from the consent ledger | `703192574` | The server seat's first unbuilt row: "a host lease that the control owner can revoke during an active stream". New `src/juce/net_plugin_hosting.{hpp,cpp}` (in `adi_control`): `PluginHostingLeases`, the app's one owner. Its `authorizer()` is what `makePluginServerLoader` takes. It asks the AGT-03 ledger for the exact project file binding, connection, server endpoint and disclosure (audio bytes per block, events, parameters, state, each as an upper bound). A refusal becomes one pending `PluginHostingQuestion` for the window's prompt (`takeQuestions`, `answer`). This time: one load, the lease lasts for the device's life. One hour, one day, until revoked: the lease ends at the expiry, checked by the device's I/O worker (two atomic loads and the clock). `revoke(project, connection)` ends live streams and removes their ledger rows; `refresh()` re-reads the ledger, so a grant removed by another process ends its stream and a renewed one moves the expiry. Shared file touched: `src/adi/control/consent.{hpp,cpp}` (control seat) gains `ConsentLedger::standing()`, a read that consumes nothing; `CMakeLists.txt` one source and one test. | New `adi_plugin_hosting_tests` drives the real ledger file, the real loader and a counting server: refusal asks once and opens nothing; a block one second before the hour reaches the server, the block at the hour does not; a renewal mid-stream keeps it going; another process's revoke plus `refresh()` stops it; the owner's revoke stops it and empties the ledger; This time admits one load only and outlives 30 days; an audio-thread request gets nothing. | checking |
| 1 | Never a socket on the audio thread | `30bb6968c` | `PluginSocket::open/write/read` refuse on a callback thread (`audio::onAudioThread`) and count in `plugin_socket::callbackRefusals`; the refusal's error is short so it does not allocate. Before this the rule held by construction only, proved on Linux by a syscall trace that is not in the tree. | `adi_remote_tcp_playback_tests` now runs every live block under `audio::CallbackScope`, proves the guard refuses a probe, and asserts that the whole stall, recovery and disconnect run made no other TCP call from the callback. | checking |

## For the director

(none yet)

## For win

- `adi_agent_log_tests` fails on MSVC in the base (`land/basics-1128c`, from `control/agent-chain`): its
  three-process timeline check. The control seat owns it.

## Reserve

(none yet)
