# Phase 1b — the Node backend (`service/`)

**One agent, `service/` only. Nothing deleted, nothing modified.** The six DECISIONS.md
repairs from Phase 1a remain recorded in `docs/AUDIT_DECISIONS.md` and unapplied.

This pass is different in kind from the earlier ones: the agent **measured** rather than
inferred, running read-only probes against the Stream Deck app's own bundled
`node v20.20.0`. I then independently reproduced the headline finding and the two
assertions that indict my own work.

---

# §1 — The leak chain: a phantom client can leave a MIDI note sounding

Three defects that compose. **I reproduced link 1 myself on the bundled runtime**, and
read links 2–4 in the code.

## L1 — `'close'` never fires on an abrupt disconnect, so nothing reaps the client

`ws-server.js:92` — `socket.on("close", () => client._gone())` — is the primary reaper.
**Node's `http.Server` creates its sockets with `allowHalfOpen: true`.** When the peer's
fd vanishes, the server socket gets `'end'` and then sits there writable **forever**;
`'close'` is never emitted.

Measured on `~/Library/Application Support/com.elgato.StreamDeck/NodeJS/20.20.0/node`:

```
http.Server allowHalfOpen = true
+300ms   events = [end]
+1000ms  events = [end]
+2000ms  events = [end]
'end'   fired: true
'close' fired: false      <- so _gone() never runs
```

## L2 — the heartbeat therefore reaps in **30 seconds, not 15**

`ws-server.js:56-62`: cycle 1 only *arms* (`awaitingPong = true; c.ping()`); only cycle 2
terminates. So a dead client sits in `this.clients` for two full intervals.

**The original F5 symptom is still reproducible.** F5 records the service reporting 3
clients when 1 existed. Restart the Stream Deck app more than once per 30 s and phantoms
accumulate again — **and `scripts/deploy-mac.sh` restarts the app, so a double deploy does
exactly this.** (I deployed twice inside one turn earlier today.)

## L3 — and the phantom suppresses the stuck-note guarantee

`index.js:108-113`:

```js
server.onDisconnect = () => {
  if (server.clients.size === 0) { midi.panicAll(); }
};
```

With a phantom still in the set, **the real client leaving never reaches zero, so
`panicAll()` never runs.** A note held when the app died stays sounding for up to 30 s —
and the service's own header (`index.js:8-10`) states silencing on last disconnect as its
guarantee.

## L4 — `_gone()` leaves the socket open, and the buffer keeps growing

`ws-server.js:151-156` drops the client from the set and fires `onDisconnect`, but never
destroys the socket or its three listeners. Since the heartbeat only iterates
`this.clients`, **once `_gone()` has run nothing can ever reap that socket again.** The
agent measured `_feed` continuing to buffer on an already-disconnected client at **1 MiB
per continuation frame with no ceiling** — `fragments` is not cleared on the `close(1009)`
path at `:186`.

## L5 — verbs still dispatch after disconnect

`_feed` (`:158-197`) has no liveness check. Measured: two verbs executed *after*
`onDisconnect` ran. If one is a `midi.noteOn`, the note is armed **after** the final
panic, and since the client is already out of the set nothing will ever panic again —
a permanently stuck note.

## L6 — a held Command modifier survives every service exit path

`os.js:256` sends `key down command` and deliberately never releases it. The only releases
are `appSwitchCommit`, `appSwitchCancel`, and a 25 s guard — **all inside this process.**
Neither `shutdown()` (`index.js:121`) nor the `uncaughtException` handler (`:132`) calls
`appSwitchCancel()`. So if the service is SIGTERMed between a dial-5 turn and its press —
**which is what a deploy does** — Command is left logically down with no timer left to
release it. The code's own comment names this as the failure mode to avoid.

## L7 — `noteOff` forgets the note before it knows the send worked

`midi.js:136-139` deletes from `sounding` unconditionally, then sends. `noteOn` correctly
adds only on success. So a failed send leaves the note sounding in the DAW and
**invisible to `panic()` forever.** Same hole inside `panic()` itself.

## L8 — `tap()`'s note-off timer is untracked

`midi.js:143` — `setTimeout(() => this.noteOff(...), ms)` — the handle is never stored, so
`close()` cannot cancel it. The frontend sends `ms: 40`, so back-to-back taps of the same
note have timer A cutting the note timer B just re-armed.

## L9 — any string on the wire becomes a real CoreMIDI port

`midi.js:184-189`: `get(id)` creates a `Port` for **any** `String(id)` and opens a real
virtual CoreMIDI source named after it. Nothing prunes the map, nothing validates against
`DEFAULT_PORTS`. Not leaking today (the frontend only ever sends two ids) but every
`midi.*` verb is an unauthenticated entry point for any local process, and `m.port` as an
object yields a port named `[object Object]`.

## L10 — `Port.close()` re-enters itself through `panic()`

`midi.js:121-127` catches a send failure and calls `close()` → `panic()` → `noteOff` →
`send` → throws → `close()` again. Bounded by the number of sounding notes and swallowed
by a bare catch, so harmless in practice; worth a re-entrancy flag.

---

# §2 — Unhandled IPC and orphaned routes

The good news first: **every one of the 25 handlers resolves to a real export.** No
handler references a missing function.

| Finding | Detail |
|---|---|
| **There is no server→client push channel at all** | `broadcast` is dead *and* nothing could consume it: the only `IPC.on(...)` in the frontend is `plugin.js:237`'s internal `'online'` event. `{t:"ready"}` is consumed by an inline `if`, not a listener. |
| **`os.window` silently no-ops for 5 of the 9 layouts it is sent** | `GEOM_TILES` (`os.js:489`) covers only `left/right/fill`. If the Window menu is absent, disabled or non-English, `top`, `bottom`, `leftright`, `leftquarters`, `quarters` return `false` — fire-and-forget, so the key just does nothing. |
| **3 live keys are silent no-ops on Windows** | `WIN_TILES` has no entry for `leftright`, `leftquarters`, `quarters`. It also maps `topleft`/`bottomleft` → `win+left`, so the quarter keys do halves. |
| **The red traffic light has no failure channel** | When `guarded()` refuses, the only trace is the service log. The key looks broken — the same silence the V36 unknown-verb log was added to fight, one level in. |
| `os.launch` / `os.hotkey` are dead **but not for the obvious reason** | `root.js:271-272` *does* call them. They are unreachable because no `SLOTS` entry declares `app:` or `hotkey:` (all 17 checked). Deleting the verbs means deleting those two branches too. |
| `appCache` has no live invalidator | `rescanApps` is its only one and it is dead, so **an app installed after login never appears until the service restarts.** A consequence of the known `os.rescan` finding, not a new symbol. |

---

# §3 — New dead code, and one important non-deletion

| Symbol | Verdict |
|---|---|
| `midi.ports` + `MidiPorts.status()` | **Test-only but LOAD-BEARING — do not purge.** It is the *only* observation window for the virtual-port assertion and for "panic on last disconnect left nothing sounding" (`test_service.mjs:151`). Removing it removes the only behavioural test of the stuck-note guarantee. |
| `hello` verb | Test-only + the documented manual health probe. Its 400-byte variant is the only exercise of `decode`'s 16-bit length path. Keep. |
| `MidiPorts.panic(id)` | Reachable only via the dead `midi.panic` verb; dies with it. `panicAll` is the live one. |
| `frontAppScript(body)` | Live, but parameterised for a second caller that never arrived — `body` is always the same string. Inline-able, not dead. |
| `psOut` | Dormant, not dead — Windows half of the V63 guard. Keep. |

Nothing else new.

---

# §4 — Robustness gaps that are not leaks

**R1 is the one I would act on.** `ws-server.js:42`'s `server.on("error")` **logs and keeps
running**. Measured: after `EADDRINUSE` the process stays alive with no listener, forever.
`KeepAlive` cannot rescue it because it never exits — so **the entire surface is silently
dead, every key a no-op, until someone reads the log.** It should exit and let launchd
respawn it.

Then, in descending order:

* **R5 — a residual guard hole.** `quitFront` falls back to `hotkey("cmd+q")` if the Apple
  Event fails — a blind keystroke aimed at whatever is focused *now*. If focus moved to
  Ableton in between, the guard is bypassed. Low probability; exactly the accident the
  list exists for.
* **R6 — the V63 guard is probably ineffective on Windows.** `winFrontApp` returns
  `ProcessName`, which for Elgato's Windows build is `StreamDeck` (no space) — neither
  guard entry is a prefix of `streamdeck`, so **the red key could kill its own host.**
  Also missing: `explorer`, `dwm`, `csrss`. *Unverified — no Windows machine.*
* **R7 — a PowerShell injection in `launch()`.** `JSON.stringify` does not escape `$` or
  backtick and PowerShell interpolates inside double quotes, so `app: "$(calc)"` executes.
  Windows-only, and the verb is dead, but reachable from any local process on 9011.
* **R3** — `onMessage`'s promise floats with no `unhandledRejection` handler; Node 20
  routes that to `uncaughtException` → `exit(1)`. No live trigger found.
* **R9** — no in-flight coalescing on `os.scroll`: every `dialRotate` spawns an
  `osascript`. Bounded by the 5 s timeout, so not a leak, but it contradicts the
  "six ticks must not be six process spawns" intent stated at `os.js:356`.
* **R11** — the MIDI re-open retry has no backoff and no cap: a Windows box with no
  loopMIDI port re-scans every 3 s forever (logged once, so quiet).
* **R10** — the LaunchAgent points stdout *and* stderr at one `service.log` with no
  rotation, for an agent that runs for days.

**Corrected: backpressure is a non-issue.** The agent went looking for it, could not make
it matter, and said so — the "15 fps multi-kilobyte" traffic is on the *Stream Deck*
socket and the Ableton bridge, **not on 9011**. The service never pushes, so its writes
are tiny replies and pings. The unchecked `socket.write()` return is worth a comment so a
future push channel does not inherit it, nothing more. I am recording the negative result
because it stops the question being re-asked.

---

# §5 — ⚠️ My own test fix from last turn is vacuous, in the same way

Last turn I removed the `axFullScreenToggle()` conjunct because it matched a function that
does not exist — only a comment. **What I left behind has the identical defect**, and the
agent caught it. Verified myself:

```
after deleting the AX code path (os.js:650-680):
  /AXFullScreen/ still matches : True     <- the assertion would still PASS
  surviving hits are comment lines: [551, 567]
```

So `test_service.mjs:244` cannot distinguish "the feature works" from "only the prose
survives". I fixed the symptom and left the class. Two more of the same shape:

| Line | Defect |
|---|---|
| `test_service.mjs:344` | `/contents of pr/.test(src)` — `os.js:494` is a comment containing exactly that string. |
| `test_service.mjs:301` | `/GetForegroundWindow/` — `os.js:592` is a comment containing it. Saved only by the `&& /GetWindowThreadProcessId/` conjunct, which is code-only. |
| `test_service.mjs:293-295` | My Windows-guard assertions are **absence** assertions on source text — the exact fragility the comment I wrote three lines below warns against. And close to vacuous: `if (isWin) return hotkey("alt+f4");` bypasses the guard *and* passes both regexes. |
| `test_service.mjs:325` | Asserts **textual order** (`indexOf` < `indexOf`), not control flow. |
| Header vs content | Line 2 claims the suite "checks the loopback guard". **It does not** — nothing tests the `!local` rejection at `ws-server.js:71`. |

**The real gap is coverage, not phrasing:** nothing tests the F5 heartbeat — *the bug the
file was written for* — nor half-open reaping, `MAX_MESSAGE`/1009, continuation frames, or
client-count integrity across reconnect cycles. All four probes in §1 are ~30 lines each
and **would have caught L1–L5.**

The two strongest assertions in the file, for contrast, are the `guarded()` block (which
calls the real export, including the inverse) and `/Stop-Process -Id \$\{w\.pid\} -Force/`
(code-only). That is the shape the others should copy.

---

# §6 — Checked and HEALTHY (do not "fix")

* **The heartbeat timer itself cannot leak** — one `setInterval`, created once, `unref()`'d
  so it never holds the process open, cleared in `close()`. The *policy* is wrong (L2);
  the timer is right.
* **Double-free is already safe.** `_gone()`, `close()` and `terminate()` are all
  idempotent through the `alive` flag; the client count never double-decrements.
* **`decode()` is sound** — rejects RSV bits and unmasked client frames per RFC 6455 §5.1,
  caps the 64-bit length at `MAX_MESSAGE`, returns `null` on partial frames.
* **The macOS quit guard fails CLOSED.** `osaOut` resolves `""` on error or on its 8 s
  timeout → `frontApp()` returns `null` → both verbs log and return false. Neither new
  V63 helper introduces a bypass on the path it covers.
* **`Port.sounding` is bounded** at 16×128 by construction; `appCache` is built once.
* **Process-level listeners** are three, attached once at load, never re-attached.
* **`switchTimer`** is always cleared before replacement and by both commit and cancel.
* **`home.js`** is dormant as documented — no timers, no listeners, errors caught.
* **`MAC_ANSI` / `MAC_KEY` / `WIN_VK` / `MAC_SPECIAL` / `WIN_SPECIAL`** — complete tables,
  correct by design.

---

# §7 — What could not be determined

1. **What the real CEF client does on teardown.** The abrupt-FIN case is proven mishandled;
   what fraction of real disconnects are clean Close frames vs FINs was not observed. F5's
   history is strong evidence that FINs happen — and once one does, L4 and L5 follow.
2. **All Windows findings are static reading.** `StreamDeck` as the Windows process name is
   inferred from Elgato's binary naming, **not measured.** Someone with a Windows box
   should print `winFrontApp()` for the Stream Deck app and for Explorer before trusting
   R6 either way.
3. **L7** (a lost note-off) is a code-reading finding; confirming it end to end needs a
   forced port failure with Live or rekordbox running, and creating virtual CoreMIDI ports
   is a visible side effect the brief forbade.
4. **R4** — an unhandled `'error'` on a rejected upgrade socket. Three attempts to force it
   failed; unproven either way, reported as cheap hardening rather than a bug.
5. **`ThrottleInterval`** on the LaunchAgent is unset; with R1 that becomes relevant, and
   launchd's respawn behaviour against a permanently occupied port was not modelled.

---

# Still unapplied, carried forward

* The **six DECISIONS.md repairs** from Phase 1a.
* Everything in this report and in `docs/AUDIT.md` / `docs/AUDIT_PHASE1.md`.

If any single thing here deserves to jump the queue it is **L1–L3**: it is verified, it
silently defeats the one guarantee the service documents, and a double deploy triggers it.
