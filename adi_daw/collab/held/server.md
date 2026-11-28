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
| 1 | Chain made | `2df6b6502` | `held/server-chain` = base `land/basics-1128c` at `16064c0a6` + `seats/server-chain` at `bc7877193` (which already holds its wincheck `321ea3fb2` and libc++ `5cce04174` branches). No conflicts. The base already carries the control seat's `control/agent-chain` (AGT-02 project UUID writer, AGT-03 consent ledger), which the server seat was waiting for. | Five validators pass. MSVC check `all` started on slot 19. | in progress |

## For the director

(none yet)

## Reserve

(none yet)
