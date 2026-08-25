# Phase 3 — Python and the Ableton remote script

**One agent, Python only. Nothing deleted, nothing modified.** Everything from Phases 1a, 1b
and 2 remains recorded and unapplied.

The agent built its own harness that stubs Live's API, models the real listener registry and
Live's "out of date object" raise, and **instantiates the real `LiveBridge.__init__` /
`setup()` / `teardown()`**. That is the whole reason this phase found what it did — see §6.
I independently re-verified the five highest-stakes claims. **All five held.**

**Fix categories are marked throughout**, because the standing rule is that the remote
script's *existing* code paths must not be edited while purely **additive** verbs are the
established exception. A fix inside an existing method is a bigger decision here than a new
verb, so each finding says which it would be.

---

# §1 — Stale listeners and lifecycle

## 1.1 A new Live Set may wedge the bridge while the hub still reads "online"

`live_bridge.py:66` caches `self.song = c_surface.song()` **once**. `_Framework`'s
`ControlSurface` exposes `song` as a *method*, i.e. the framework expects it to be re-read.
Harness: swap in a new Song, mark the old one out-of-date, send `subscribe` →

```
resend_all() RAISED RuntimeError: Accessing out of date Live object (selected_track)
  live_bridge.py:1136  self._on_track_changed()
  live_bridge.py:120   self._track = self.song.view.selected_track
```

`{"t":"hello"}` goes out at `:1135` *before* the raise, and `ws.onopen` sets
`state.online = true` unconditionally — **so the hub reads as connected while track, device,
params and mix all stay at their defaults.** Every later `subscribe`/`ping` throws the same
way, producing one `AdiVST dispatch error` line in Live's log and nothing on the surface.

**Unsure, and it decides the severity:** whether Live 11 re-instantiates the control surface
on Set load (harmless) or keeps the instance and swaps the Song (hub dead until Live
restarts). `_Framework` ships as Python 3.7 `.pyc` only and Live was not running.
**One-action field test: load a different Set with the plugin connected. If the hub goes
blank-but-online, it is the second case.** *(Fix: MODIFICATION — ~30 call sites.)*

## 1.2 One `hasattr` can abort a whole track change

`live_bridge.py:925` — `if not hasattr(track, name): continue`. `hasattr` swallows only
`AttributeError`. The same file knows better 110 lines earlier: `_emit_mix:809-813` reads
those three attributes through `getattr` inside `try/except Exception`, and
`cmd_track_toggle:849` uses Live's own `can_be_armed`. `_mix_listen` uses neither.

| Live raises | Outcome |
|---|---|
| `AttributeError` | correct — `_mixtoggles = [mute, solo]`, `mix.arm = None` |
| anything else | **`_on_track_changed` aborts at `:131`** having emitted only `{"t":"track"}` — no `device_pos`, no `mix`, no `device` |

**What you would see:** click a Return or Master track in Live → the hub shows the new
track's *name* but keeps the previous track's device page, params and dials, indefinitely.
`cmd_select_track` skips returns/master, so this is mouse-only — which is exactly how it
would escape testing. **Unsure** what Live raises for `Track.arm` there; no shipped Ableton
script uses `hasattr` on `arm`, which is supporting evidence, not proof.
*(Fix: MODIFICATION — one line.)*

## 1.3 ⚠️ My V61 transport keys never light on connect, and never follow Live

Verified myself. `_emit_transport` has **exactly one caller** — `cmd_transport:889`.
`resend_all` sends `hello` then `_on_track_changed()` and never touches transport. And
there is **no listener on `song.is_playing` or `song.loop`** anywhere.

```
msgs on subscribe: ['hello','track','device_pos','mix','device','params','eq8_state']
transport present: False
listeners on the song object: ['selected_track']
```

So `state.transport` stays `undefined`, `lit` evaluates false, and **Play and Loop render
unlit on every connect and every reconnect even when Live is playing** — and pressing
spacebar in Live never updates them. They only change when you press the Stream Deck key
itself.

**This is precisely the bug V62 fixed for mute/solo/arm** — where I *did* add listeners and
*did* emit on connect. I added the transport one version earlier and did neither. Same
author, same file, one version apart. *(Fix: MODIFICATION of `resend_all` + `setup()`.)*

## 1.4 `_on_device_changed` forgets `device_pos` — and it has a sibling I missed

Confirmed by harness: a mouse click on a different device emits
`['device','params','eq8_state']`, no `device_pos`. `_on_track_changed` and
`_on_devices_changed` both emit it; this one does not.

**New sibling:** `cmd_device_key` (`:613-643`) has the same gap — pressing any plugin
shortcut key focuses a device and leaves the Prev/Next caption stale. `cmd_device_step`
*does* emit it, which is why the arrows look right and the plugin keys do not.

Six other handlers were checked for this shape and are balanced.
*(Fix: one line appended to `cmd_device_key` ≈ ADDITIVE; `_on_device_changed` = MODIFICATION.)*

## 1.5 No listener exists on any rack chain — and a comment claims one does

`live_bridge.py:138` says `_emit_device_pos()  # V53 — a rack gaining a device moves the
count`. The only `devices` listener is on the **top-level** track. Harness: drop a device
into a rack chain → zero messages emitted, and a forced `device_pos` then reports
`count: 3` where the surface still believes 2. *(Fix: MODIFICATION.)*

## 1.6 `_unlisten`'s bookkeeping line sits outside the `try` that protects it

`live_bridge.py:104-109`: the `remove_*_listener` call is guarded; the list-comprehension
rebuild on `:109` is not — and it compares tuples, which can call `__eq__` on dead Live
handles. Harness with a handle that raises on `__eq__`:

```
UNCAUGHT RuntimeError: Accessing out of date Live object (__eq__)
  live_bridge.py:109
```

Note the asymmetry: `_on_track_changed:116-119` wraps *its* unlisten in an outer try; `:115`
has none. **Unsure** whether Live's objects raise on `==` — but the shape is wrong
regardless: `:109` belongs inside the try that exists to tolerate dead subjects.
*(Fix: MODIFICATION — move one line.)*

## 1.7 `_mixtoggles` is never initialised in `__init__` — and the default is load-bearing

Confirmed: `hasattr(_mixtoggles)` is False after `__init__`; `_unmix_listen:943`'s
`getattr(self, "_mixtoggles", [])` genuinely saves a `teardown()`-before-`setup()` path. The
same defensive `getattr` applied to `_mixed` at `:934` is redundant — `_mixed` *is*
initialised. No other instance attribute has this shape.

## 1.8 The socket half of teardown, and a booby trap

**The Python teardown is clean** — see §8. But:

* **`WSServer._stop = threading.Event()` shadows `threading.Thread._stop`**, a real internal
  method. Verified: after the thread exits, `is_alive()` and `join()` both raise
  `TypeError: 'Event' object is not callable`. Nothing calls them today — but that is
  exactly where the fix for the next bullet belongs.
* **If Live reloads scripts without a clean disconnect, the plugin dies silently and leaks.**
  Verified: `SO_REUSEADDR` does not permit rebinding a live listening port on macOS, so
  `run()` logs `AdiVST WS bind failed` and returns — and `broadcast()` never checks whether
  the thread is alive:
  ```
  _outbox after 20000 broadcasts: 20000 entries, ~1.0 MB of strings
  ```
  Every parameter move in Live appends forever. *(Fix: ADDITIVE — an alive check in
  `broadcast`, plus `join(timeout)` in `stop()` once `_stop` is renamed.)*

## 1.9 Every mixer touch emits `mix` twice

`cmd_track_volume_delta:780` sets `vol.value`, which fires the registered `_emit_mix`
listener, and `:781` then calls `_emit_mix()` again. Same at `:788/789` and `:854/858`.
Harness: `volume tick → ['mix','mix']`. The frontend's `case 'mix'` also fires
`emit('state')`, so **a fast dial spin costs two full surface re-renders per detent.**
*(Fix: MODIFICATION.)*

---

# §2 — Unhandled MIDI bindings and the ControlSurface contract

The agent extracted Live 11's real `ControlSurface` method inventory from the app bundle's
`.pyc` and diffed it against `AdiVST.py`'s AST. **`AdiVST` defines seven methods; only two
(`update_display`, `disconnect`) are ControlSurface hooks.**

Everything else is inherited with nothing registered: `build_midi_map` iterates
`self.controls` (**empty**), `receive_midi` consults an empty `_forwarding_registry` and a
`None` `_pad_translations`, `suggest_input_port`/`suggest_output_port` suggest nothing,
`can_lock_to_devices` is default with **no `DeviceComponent`** (so Live's blue-hand lock
does nothing), and `_send_midi` is never called — **no MIDI is ever sent.**

**The finding that matters:** the base class carries the literals `"Got unknown message: "`
and `"Got unknown sysex message: "`. With an empty forwarding registry, **every MIDI byte
Live routes to this surface produces a line in Live's `Log.txt`.** So if an Input port is
ever assigned to the AdiVST slot in Live → Settings → Link/MIDI and that port carries
traffic, **Live's log floods.** The correct configuration is Input = None, Output = None —
and `AdiVST.py`'s install docstring does not say so.

The surface also occupies one of Live's six Control Surface slots, and any port assigned to
that slot leaves Live's generic Track/Sync/Remote lists. Since the script neither sends nor
receives MIDI, any such assignment is pure loss.

**`update_display` is cheap and correct** — base call plus draining at most 128 deque
entries; the idle path is one falsy `while` test, nothing allocates. `send()` measured at
**0.011 ms** including `json.dumps`, and `broadcast` is a bare `deque.append`, never a
socket write. Recorded so nobody "optimises" it.

**Verb table:** all 32 verbs the frontend sends are dispatched; `ping` is handled and never
sent. `_dispatch` has **no `else` branch**, so an unrecognised verb is dropped with no log
at all — on a protocol that is fire-and-forget in both directions.

---

# §3 — Dead Python, new only

## 3.1 Dead protocol fields — Python computes them, JS stores them, nothing reads them

| Field | Cost per emission | Read? |
|---|---|---|
| `params[].pidx` (`:211`) | **O(n) linear scan per slot × 6 slots, on every device change** | **zero frontend references** |
| whole `eq8_state` message (`_emit_eq8_state:415`) | an `_eq8_instances()` walk, **called 2× per devices change** | no `on('eq8_state')` subscriber exists |
| `eq8.scale` + `scale_disp` (`:307-308`) **plus its listener at `:258-260`** | one extra `_listen` per EQ8 | stored twice, never read |
| `hello.version` / `hello.live` (`:1132-1135`) | a `get_major_minor_version()` LOM call per subscribe | no subscriber |
| `device.param_count`, `track.color`, `eq8.page`, `params.page`/`pages` | small | all stored, none read |

The `scale` row **extends** the known dead "Scale arm": the listener registration and both
emitted fields are dead end to end. The `eq8_state` row means an already-known-dead function
is still *paid for twice on every device change*.

## 3.2 `_mix_listen`'s argument is half-ignored

`track` is used for the `None` guard and the toggle loop; the volume/pan half calls
`self._mixer()`, which reads `selected_track` instead. Harness: *"asked to watch T1 → vol/pan
listeners on T0: 2, on T1: 0."* Latent — the single call site sets `_track` immediately
before — but the two halves attach to different tracks.

## 3.3 `_on_ws_connect(client)` discards its client — and that is *why* `broadcast`'s targeting is dead

It pushes `{"c":"subscribe"}` onto the shared inbox, so `resend_all` **broadcasts the full
snapshot to every connected client**. So `broadcast(text, client=…)`'s per-client arm —
already on the known-dead list — is unreachable *because of this*. With two clients
connected, one reconnect re-floods both.

## 3.4 Two items in `adi_studio_os/scripts/gen_icons.py`

* `:100` — the `"plugin/icon"` TARGETS row writes `imgs/plugin/icon.png` + `@2x`, which
  **`manifest.json` references neither** (its plugin `Icon` is `imgs/plugin/marketplace`).
  The two orphaned *files* were already known; **the row that keeps regenerating them** was
  not.
* `:110-111` — `made += 1 and bool(make(...))` evaluates to `made += bool(...)`. Correct
  today only because `make` always returns a truthy path.

## 3.5 Smaller, hand-vetted

`make_profile.py:57`'s `next((m for m in [key] if False), None)` is unconditionally `None`
and never read. `_find_item`'s depth cap of `6` (`:435`) is a bare magic number next to a
named, commented `MAX_RACK_DEPTH = 12` — it silently floors `cmd_load_device` at 7 levels in
`user_library`/`packs`. `slice_backgrounds.py` has **no `if __name__ == "__main__"` guard**,
so `import slice_backgrounds` opens `~/Downloads` and overwrites `backgrounds.js`.
`AdiVST.py:21-24`'s `ControlSurface = object` fallback makes the module importable but **not
instantiable** (verified `TypeError`), so it buys nothing testable. `_fn_cache` never clears
— bounded and tiny, but only grows.

**Unreachable `except`/`try` blocks: none found.** Every `try` has a reachable failure mode,
and `ws_server.py:148/150`'s `BlockingIOError` before `OSError` is the required ordering.

---

# §4 — Orphaned configuration and paths

**8.1 MB of abandoned backups sitting inside Elgato's own directories**, verified:

```
ProfilesV3.studioos-backup-20260809-020214    4.1M
ProfilesV3.studioos-backup-20260809-020548    4.1M
com.elgato.StreamDeck.plist.studioos-backup-20260809-020214   3173 bytes
com.elgato.StreamDeck.plist.studioos-backup-20260809-020548   2509 bytes
```

`make_profile.py`'s `backup()` never prunes and `restore()` only ever consumes the newest, so
this grows by ~4 MB per run — in a directory the Stream Deck app scans.

Also: **stale `__pycache__` in the *installed* remote script** (`.pyc` from Aug 21 against
`.py` from Aug 24) which survives every deploy **because** `rsync --delete --exclude
__pycache__` protects excluded paths from deletion. Harmless, permanently orphaned.
`ableton/scripts/validate.py:80` calls `py_compile` with no `cfile`, which would create a
`__pycache__` inside the live remote-script folder. Ten `.DS_Store` files. No
`requirements.txt`/`setup.py`/`pyproject.toml` anywhere.

**A guard that warns and then does the wrong thing anyway:** `slice_backgrounds.py:168-170`
prints *"the measured bands below are no longer valid"* and then **falls straight through** to
patch the stale coordinates. Its own docstring claims it "says so loudly rather than quietly
patching the wrong stripe." It does both.

**Clean:** all four source images present and exactly `EXPECT_SIZE`; **the installed remote
script is byte-identical to the repo** (`diff -q` on all four files); exactly one AdiVST on
the machine, none in the Live app bundle.

**What is load-bearing in the legacy ableton-vst plugin:** exactly one path —
`com.adiariel.ableton-vst.sdPlugin/js/controllers/` (16 files), read by
`test_ableton.mjs:12` as a **negative** baseline. Everything else in that folder has no live
consumer.

---

# §5 — Bugs found in passing

1. **`ws_server.py` accepts any local connection, with no origin, method or version check.**
   Verified against a live instance: a `POST /anything HTTP/1.0` with
   `Origin: https://evil.example` got `101 Switching Protocols`, and `{"c":"ping"}` was
   delivered. WebSocket handshakes are **not** subject to CORS, so **any web page open in any
   browser on this Mac can reach the full verb table** — including `eq8_load_preset`, which
   calls `track.delete_device`. Low likelihood, high blast radius. *(Fix: ADDITIVE.)*
2. Unmasked client frames are accepted (RFC 6455 §5.1 requires failing the connection).
   Benign on loopback.
3. `make_profile.py --activate-only` calls `find_profile()` **before** its own
   `isdir(PROFILES)` check → raw traceback instead of the friendly exit.
4. `make_profile.py restore()` does `rmtree` then `copytree`; a failure partway leaves the
   profile store gone with nothing in flight to restore it.
5. **`test_bridge.py:236-238`** computes `db` and then unconditionally overwrites it on the
   next line — and the docstring's claim that "0.85 is 0 dB" is false for the curve actually
   used (it gives **+1.83 dB**). Dead code plus a false comment in the harness that certifies
   the volume feature.
6. `verify_band`'s `below`/`aboveY` are semantically swapped relative to their names (output
   still correct — cosmetic, but misleading to the next editor).
7. **`validate.py` compiles the remote script with the host Python — 3.9.6 — while Live 11
   embeds 3.7** (confirmed from the installed `.pyc` magic `420d0d0a`). Syntax valid in 3.9
   but not 3.7 would pass validation and fail at Live launch.
8. `AdiVST.py`'s install docstring names the wrong folder (`MIDI Remote Scripts` vs the
   actual `~/Music/Ableton/User Library/Remote Scripts/`).
9. `_device_index` returning `-1` for anything inside a rack is now **proven** rather than
   inferred — which confirms the known `delete_device(-1)` path. Mitigating: `device.index`
   is stored by the frontend and never read, so the `-1` is inert except through
   `cmd_load_preset`, which is already dead.

---

# §6 — Why every §1 finding survived: `test_bridge.py` never runs the real constructor

**All four test bridges subclass `lb.LiveBridge` and override `__init__` without ever calling
it.** Verified: zero `super().__init__()` / `lb.LiveBridge.__init__` calls, and `setup()` /
`teardown()` appear **0 times** in the whole 471-line suite.

So the real constructor, `setup`, `teardown`, `_listen`/`_unlisten` and the entire listener
lifecycle are **never executed by any test** — which is exactly the surface §1 lives on.

**Untested entirely:** all eleven EQ8 commands, `_safe_set`, `resend_all`,
`_mix_listen`/`_unmix_listen`, all seven `*_index` verbs, `cmd_watch`, **`cmd_transport` and
`cmd_track_toggle` (V61 and V62 — the two newest features have zero Python tests)**, all 339
lines of `ws_server.py`, and all 171 lines of `AdiVST.py`.

**The stubs actively encode a known bug.** `VolParam`/`PanParam` have no `is_enabled`
attribute — so routing the two known `_safe_set`-bypassing setters through `_safe_set` would
**break blocks [10]–[13]**. The test cannot catch that bug and would resist the fix.

Four weak assertions: `ok(…, True)` at `:329` (the condition is the literal `True`);
`n > 0 and n <= 40` at `:468` where the answer is a determinate 13; a message *count* rather
than its content at `:110`; and `KeyBridge`/`TreeBridge` **overriding `_device_index`**, which
structurally hides the rack `-1` bug.

**Good news on the specific worry from earlier phases: no Python test asserts on source text
or comments.** Every assertion is behavioural. The suite's weakness is scope, not shape — and
what it does cover (name normalisation, the browser walk, the dB bisection, rack flattening,
step clamping) it covers well, with two regression tests carrying their failure mode written
down.

---

# §7 — Legacy Python triage

Nothing live references any of the six files — the only hits are prose in three legacy
READMEs/CHANGELOGs. Their output directories are committed and populated, so the generators
build nothing that is needed.

Duplication, measured (non-comment `SequenceMatcher`): rekordbox ↔ midi_control **41.1 %**
(`gen_icons`) and **62.5 %** (`validate`); Studio OS ↔ any legacy **4.5–5.5 %** — its
`gen_icons.py` is a from-scratch Pillow implementation sharing no code with the four
hand-rolled stdlib PNG writers.

**Two things were never ported and are worth having back:**

1. **Studio OS has no `validate.py` at all.** `HANDOFF.md:116` records the decision. The
   consequence is already visible in §3.4 — two orphaned generated assets nobody noticed. The
   **rekordbox** validator is the richest of the four and has three checks Studio OS performs
   nowhere: a *bidirectional* manifest-UUID ↔ source cross-check, encoder-layout JSON
   validation, and a vendored `@julusian/midi` prebuild presence check for mac **and**
   windows — the same dependency Studio OS ships.
2. **`adi_ableton_vst_controller/scripts/validate.py` is the only thing in the entire repo
   that syntax-checks the live remote script.** It is invoked by nobody, so **the live remote
   script currently has no automated syntax gate** — which matters more than usual, because a
   syntax error there fails silently at Live launch.

| Folder | Verdict |
|---|---|
| rekordbox `scripts/` (491 L) | archive the generator; **extract the three unique validator checks first** |
| midi_control `scripts/` (295 L) | archive both — duplicates with nothing unique |
| visualizers `scripts/` (261 L) | archive both |
| ableton `gen_icons.py` (114 L) | archive |
| **ableton `validate.py` (96 L)** | **KEEP** — its `PLUGIN` half is dead, but `:74-82` is the only syntax check on the live remote script. Ideally move that half into `adi_studio_os/scripts/` and wire it into `deploy-mac.sh` before step 2. |
| legacy `ableton-vst.sdPlugin/` | keep **`js/controllers/` only**; the rest is archivable |

---

# §8 — Checked and HEALTHY (do not "fix")

* **`teardown()` is fully clean** — 14 → 0 registered listeners, `_listened`/`_param_map`/
  `_mixed`/`_mixtoggles` all emptied, zero log lines. `disconnect()` is present, correctly
  ordered, and calls up to `ControlSurface.disconnect`.
* **Deleting the watched track or device recovers cleanly** — both re-emit the full state and
  leave no stale entries. The `try`/`getattr` guards are doing real work; do not simplify them.
* **Re-entrancy could not be broken.** A listener triggering a device switch mid-rebuild, and
  `_select_device` called from inside `cmd_device_key`, both left consistent state.
* **Two different listener-identity strategies coexist and both are correct.** `_cache_fn`
  exists because a closure over `slot` cannot be recreated equal; `_emit_mix` is registered
  raw because *bound methods compare equal*. **Do not add caching to `_emit_mix`, and do not
  remove `_cache_fn`.**
* **`ws_server.py`'s backpressure works** — `MAX_OUT_BUF` drops a slow client, `_read` guards
  `MAX_FRAME + 4096`, the 64-bit high bit is rejected, and oversized/fragmented control
  frames get a proper 1002 close.
* **`broadcast()` never blocks Live's main thread**; `stop()` → `select()` unwind is correct.
* **Per-attribute mixer guarding is correct** in `_emit_mix` and `cmd_track_toggle` — the
  return/master asymmetry *is* handled there. The gap is only `_mix_listen` (§1.2).
* **The verb table is complete** — all 32 verbs the frontend sends are dispatched.
* **The installed script is byte-identical to the repo**, and there is exactly one AdiVST.

---

# §9 — What could not be determined

1. **Whether Live 11 re-instantiates the control surface on Set load** (decides §1.1's
   severity). Blocked: `_Framework` is 3.7 `.pyc`, local Python is 3.9.6, no decompiler,
   Live not running. **Field test: load a different Set with the plugin connected.**
2. **What Live raises for `Track.arm` on a return/master track** (decides §1.2). Same
   blocker. **Field test: click a Return track and see whether the device page updates.**
3. **Whether Live's LOM objects raise on `==`** (§1.6). Same blocker; the misplaced-`try`
   shape is a finding regardless.
4. **Live's actual `update_display` tick rate** — docstring says ~10 Hz. Nothing downstream
   depends on it, since the per-call cost is negligible either way.
5. **Whether AdiVST is currently selected as a Control Surface, and what ports are assigned**
   — so whether §2's log-flood condition is live. `Preferences.cfg` is compressed; no script
   name at all is extractable.
6. **An observation left unresolved:** `~/Library/Preferences/Ableton/` holds preference
   folders for **Live 12.1, 12.1.1, 12.1.5 and 12.3** alongside 11.2.7, but only
   `Ableton Live 11 Suite.app` is installed. 11.2.7's prefs are the recently-touched ones, so
   these look like uninstall leftovers — but a reinstalled Live 12 resolves remote-scripts
   paths differently and the deploy target was not verified against it.
7. **`make_profile.py` and `slice_backgrounds.py` were not run** — both write to live
   locations, so those findings are static analysis only.

---

# The audit is now complete. Carried forward, all unapplied:

| Phase | Report |
|---|---|
| Original | `docs/AUDIT.md` — the first purge menu (partly actioned by V60) |
| 1 | `docs/AUDIT_PHASE1.md` — backend, Python, installers, docs |
| 1a | `docs/AUDIT_DECISIONS.md` — DECISIONS.md, **6 repairs recommended** |
| 1b | `docs/AUDIT_PHASE1B.md` — the Node service, **the phantom-client leak chain** |
| 2 | `docs/AUDIT_PHASE2.md` — frontend, **4 regressions of mine** |
| 3 | this file — Python and the remote script, **1 more regression of mine** |

**Five of my own regressions are now on the list** (V59-V62): the V61 pump gate, the V62
`zone()` dim that never fires, V60's false RME premise, the two surviving `METER.corr`-class
peaks, and V61's transport with no listener and no emit-on-connect.

**Two one-action field tests would settle the two biggest unknowns**, and both need Ableton
open with the plugin connected: load a different Set, and click a Return track.
