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
| 1 | Admission fixture after AGT-02 | `bc90e67dd` | `adi_plugin_admission_tests` failed on MSVC: "projects awaiting a durable identity ask no ledger". Cause: the base's AGT-02 Store now writes a project UUID when it creates a file, so the fixture's "legacy file with no UUID" had one. The fixture now clears it explicitly. Test-only. | Failing check seen in the slot run of `2df6b6502`; passes in both later runs. | done |
| 1 | `adi_agent_log_tests` on Windows | none | Failed on MSVC in the base itself (the control seat's code, not this chain's), in the full parallel run: "all three child writers append successfully", "competing processes lose no entries", "every appended entry has a distinct chronological position". It passed in the filtered run and in the second full run, so it is load-sensitive, not broken. Not mine to fix; listed under "For win". | slot runs of `2df6b6502` (fail), `703192574` and `1f017567` (pass) | reported |
| 1 | Hosting lease from the consent ledger | `703192574` | The server seat's first unbuilt row: "a host lease that the control owner can revoke during an active stream". New `src/juce/net_plugin_hosting.{hpp,cpp}` (in `adi_control`): `PluginHostingLeases`, the app's one owner. Its `authorizer()` is what `makePluginServerLoader` takes. It asks the AGT-03 ledger for the exact project file binding, connection, server endpoint and disclosure (audio bytes per block, events, parameters, state, each as an upper bound). A refusal becomes one pending `PluginHostingQuestion` for the window's prompt (`takeQuestions`, `answer`). This time: one load, the lease lasts for the device's life. One hour, one day, until revoked: the lease ends at the expiry, checked by the device's I/O worker (two atomic loads and the clock). `revoke(project, connection)` ends live streams and removes their ledger rows; `refresh()` re-reads the ledger, so a grant removed by another process ends its stream and a renewed one moves the expiry. Shared file touched: `src/adi/control/consent.{hpp,cpp}` (control seat) gains `ConsentLedger::standing()`, a read that consumes nothing; `CMakeLists.txt` one source and one test. | New `adi_plugin_hosting_tests` drives the real ledger file, the real loader and a counting server: refusal asks once and opens nothing; a block one second before the hour reaches the server, the block at the hour does not; a renewal mid-stream keeps it going; another process's revoke plus `refresh()` stops it; the owner's revoke stops it and empties the ledger; This time admits one load only and outlives 30 days; an audio-thread request gets nothing. On Windows the test fails rather than skip when the file has no durable binding (`1f017567`). | MSVC slot runs: filtered 11/11 at `703192574`; full core 302/303 at `1f017567` (the one failure is the next-but-one row). | done |
| 1 | Never a socket on the audio thread | `30bb6968c` | `PluginSocket::open/write/read` refuse on a callback thread (`audio::onAudioThread`) and count in `plugin_socket::callbackRefusals`; the refusal's error is short so it does not allocate. Before this the rule held by construction only, proved on Linux by a syscall trace that is not in the tree. | `adi_remote_tcp_playback_tests` now runs every live block under `audio::CallbackScope`, proves the guard refuses a probe, and asserts that the whole stall, recovery and disconnect run made no other TCP call from the callback. MSVC full core at `1f017567`: passes. | done |
| 1 | `adi_link_playback_tests` | none | Failed once in the full run at `1f017567` (62 s, five tempo-history checks of Link restart). It passed in the full run at `2df6b6502`; this chain touches nothing of Link or Warp between the two (only the hosting lease, the socket guard and their tests). Load-sensitive under twenty sessions on one PC. | slot runs of `2df6b6502` (pass), `1f017567` (fail) | reported |

**Chain head at the end of round 1: `1f017567`.** Last full check: MSVC Debug /WX core, 302 of 303 passed
(`adi_link_playback_tests` failed, unrelated, above); window suites last run at `2df6b6502`, 4/4.

## The gate, where it stands

1. Plays as a device through ADI's player: proved at the engine (Session, the render queue, WAV export)
   against a loopback server in a second process: `adi_remote_session_tests` (actual CLAP, all compensated
   samples equal to local), `adi_native_plugin_host_tests`, `adi_remote_tcp_playback_tests`, all green on
   MSVC. **Not yet in the application or `adi_play`**: both call `JuceDeviceLoader::fn()` with no remote
   loader (`ui_main.cpp` lines near 897, 1392, 1752; `play.cpp` near 609), so in the shipped program a
   remote device always takes the local fallback. That is the next step.
2. Opened without the server, loads the plug-in locally: proved (`withRemoteDevices`, the live fallback
   and the named placeholder in `adi_remote_session_tests`). It is also what the shipped app does today.
3. Never a socket on the audio thread: guarded and counted in the socket layer this round.
4. Offline render waits and fails by name: proved (`adi_remote_session_tests` render and disconnect,
   leading-silence preflight).

What needs two machines: a physical-link latency and jitter reading and PTP between two clocks
(ADR-0107 d5); a stock server across a real LAN with a real interface; mDNS discovery across a real
switch. Everything else above runs on one machine over loopback.

## Next, in order (for whoever continues)

1. **The app's hosting owner, engine half.** One object in the app that composes `PluginHostingLeases`
   (default `ConsentLedger()`), `PluginServerCatalogue(pluginServerCataloguePath(...))`,
   `cataloguedPluginServerResolver(...)` and `makePluginServerLoader("adi-daw", ...)`, and hands the result
   to `loader_.fn(grantedRemote, localVst3)` at the three `ui_main.cpp` sites and in `play.cpp`; calls
   `refresh()` on the message thread on a timer. It needs a **pairing store** (app-local: address and
   protocol to connection id and server fingerprint, the `PluginServerPairLookup`); none exists. Stock v13
   cannot prove a fingerprint, so what pairing means depends on director's question A below.
2. `adi_play` options to pair a loopback server and grant This time from the command line, with a test
   that plays the native CLAP fixture server through `adi_play --render` and compares it to the local
   render. That is gate 1 through ADI's own player without the window.
3. The window's half (consent prompt from `takeQuestions`/`answer`, the server list, Revoke in the
   Privacy list) needs pictures agreed first (ADR-0243): not to be built before the director's sheet.
4. DEV-25's full-class-ID lookup for the VST3 fallback and the trusted stock parameter map (call sites
   exist: `Vst3ClassLookup`, `PluginServerPairing::stockParameters`).

## For the director

A. (Carried from the server seat, still open.) Which servers does the release talk to? (a) Only the
   ADI server, with an authenticated, encrypted connection; (b) that, plus the stock AudioGridder server
   as an option you switch on per server for a trusted home network. **Recommendation: (b)**, off by
   default, because the stock server is what exists today and it already matched local Surge XT bit for
   bit. Reply: "A b" or "A a".

B. May `adi_play` (the command-line player, not the app) record a This-time hosting grant from a flag you
   type, so a server can be tested without the window's prompt? **Recommendation: yes**, This time only,
   never a standing grant from the command line. Reply: "B yes" or "B no". Until answered, step 2 of
   "Next" is held and step 1 goes on.

## For win

- `adi_agent_log_tests` failed once on MSVC in the base (`land/basics-1128c`, from `control/agent-chain`):
  its three-process timeline check, in a full parallel run; it passed in two later runs. The control
  seat owns it.
- `adi_link_playback_tests` failed once in a full parallel run at `1f017567` and passed at `2df6b6502`.
- This chain changes the control seat's `src/adi/control/consent.{hpp,cpp}` by one added method
  (`ConsentLedger::standing`), and `src/juce/net_plugin_socket.hpp` (the server seat's).

## Reserve

(none yet)
