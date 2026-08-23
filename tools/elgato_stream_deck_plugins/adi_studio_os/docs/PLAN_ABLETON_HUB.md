# Draft plan — the Ableton Hub as a control centre

**Status: DRAFT. No code written.** Batch 32 asked for the audit and the Calculator
first, then *"draft the plan for restructuring the Ableton Hub folders and transport
controls."* This is that draft. **Eight conflicts need your ruling before any of it is
implemented** — §0 is the list, and three of them are blocking.

---

# 0. Conflicts, blocking first

## ⛔ B1 — I cannot tell which keys are "above the touch screen"

This is the whole foundation of the request and **the codebase does not contain the
answer.** `surface.js` models the hardware as a flat 9×4 keypad *plus a separate 6×1
encoder strip*, and `make_profile.py:100-108` writes exactly that — two independent
`Controllers` blocks. **Neither carries any physical geometry**; the Stream Deck app
owns where things physically sit, and F1 only records the grid sizes.

The one piece of indirect evidence is `states.js`'s L3b comment: *"a window borrowing
N dials takes the LAST N — with N=2 that is physical dials 5 and 6, directly beneath
the dock"*, where the dock is cols 5-8. For dials 5-6 to sit beneath cols 5-8, the six
200px zones must span the **full nine-column width** — which puts the strip below the
whole keypad and makes **row 3 (the bottom row, buttons 28-36)** the row physically
nearest it.

**So my assumption is: the mode row is row 3, and the five modes take (0,3)…(4,3).**
Everything below is written on that assumption. **If the + module physically sits
beside or below the XL and its own 8 keys are what you mean, the entire key plan
changes** — tell me which cells and I will re-draw it.

## ⛔ B2 — row 3 is fully occupied in every module, and taking it costs band artwork

If the mode row is row 3, here is what it displaces:

| Module | What row 3 holds today |
|---|---|
| **Ableton hub** | the bottom cell of all four plugin bands, plus `NEXT` at (8,3) |
| **Root Hub** | five macOS window-state pictograms, plus the green traffic light at (4,3) |
| **Rekordbox** | nudge · CUE · PLAY per deck |
| **MIDI Control** | drum pads / CC bank |

And a specific, concrete consequence for the Ableton hub: **the band artwork is sliced
8 tiles per band, row-major, and the tile index IS the cell's slot index** (V55). Cut
a band from 8 cells to 6 and **`slice_backgrounds.py` must be re-run and
`backgrounds.js` regenerated** — the pictures do not simply reflow. That is not a
blocker, it is a cost, and you should know it before you rule.

## ⛔ B3 — global mode keys, or Ableton-only?

You framed this inside the Ableton restructure, but "use the physical keys as
dedicated Mode/Folder toggles" reads as a **board-wide reservation** — a second
Button-1-class global rule. The two readings differ enormously:

* **Ableton-only** — B2 shrinks to just the Ableton hub. Cheap, contained, and the
  other three modules keep row 3. But then `OS` mode is odd, because from inside
  Ableton it can only mean "go to the Root Hub".
* **Global** — five keys reserved in every module, in every state. That is a
  footprint decision across four modules and every Compact layout, and it is exactly
  the kind of change your standing rule says I must not make unilaterally.

## The five that are not blocking

**N1 — `OS` mode is undefined.** VST, MIDI, Device and Delay Calc all map onto
something that exists. `OS` does not. Does it mean *navigate to the Root Hub*, or
*leave the board where it is and put the OS controls (Scroll Y / Scroll X / Zoom /
Apps / Tabs) on the dials*? The second reading is the one consistent with the other
four being **strip-focus** switches, and it is what I have assumed.

**N2 — Delay Calc is a docked STATE, not a screen you navigate to.** You wrote *"route
the user to the existing Delay screen in the NAV"*. After V59 the Divisions window is
**State 1 of the carousel**, and a carousel state **docks over cols 5-8** rather than
being a nav destination. So the key must call `States.setState(States.DELAY)`, not
`Nav.enter()`. Consequence worth seeing before you rule: **the dock lands on cols 5-8,
which under B1's assumption covers mode keys at (5,3)…(8,3)** — harmless for the five
modes at cols 0-4, but it means the Delay Calc key cannot itself live in cols 5-8 or
it would dock a window on top of itself.

**N3 — Play / Stop / Loop do not exist in the remote script.** I checked: the verbs
are `cmd_watch`, `cmd_eq`, `cmd_eq8_key`, `cmd_get_all_params`, `cmd_param_set`,
`cmd_param_delta`, `cmd_set_index`, `cmd_step_index`, `cmd_toggle_index`,
`cmd_delta_index`, `cmd_delta_log_index`, `cmd_select_track`, `cmd_select_device`,
`cmd_load_device`, `cmd_device_key`, `cmd_device_step`, `cmd_get_mix`,
`cmd_track_volume_delta`, `cmd_track_pan_delta`, `cmd_list_presets`,
`cmd_load_preset`. **There is no transport verb of any kind.** Three new ones are
needed. They are purely *additive*, which is the established V30 exception — but it is
a **sibling-repo commit** and **Live must be restarted** after the deploy.

**N4 — Device/Mixer mode needs more verbs than the groundwork provides.** V50 gave us
`get_mix`, `track_volume_delta` and `track_pan_delta` — Volume and Pan. **Mute, Solo
and Record Arm are not there.** Three more additive verbs, same restart caveat.

**N5 — the green VST tint needs a genuinely new piece of state.** See §4; it is the
most interesting part of the request and the part that is new architecture rather
than rearrangement.

---

# 1. The shape, under B1's assumption

```
        col 0     col 1     col 2     col 3     col 4   | col 5   col 6   col 7   col 8
row 0   BACK      PLAY      STOP      LOOP      ·       | ·       ·       ·       ·
row 1   ·         ·         ·         ·         ·       | ·       ·       ·       ·
row 2   ·         ·         ·         ·         ·       | ·       ·       ·       ·
row 3  [ VST ]  [ MIDI ]  [Device]  [  OS  ]  [Delay]   | (free)  (free)  (free)  (free)
        ^^^^^^^^^^^^^ the mode row ^^^^^^^^^^^^^^^^^^
```

**Level 1 (`ableton.hub`)** becomes the Transport / Session surface. Play · Stop ·
Loop to start, per your brief, with rows 1-2 and cols 4-8 of row 0 left deliberately
empty — that is the room you get back by moving the VST grid down a level, and I am
not filling it without a ruling.

**Level 2 (`ableton.vst`)** is today's hub, moved wholesale: the four two-column
bands, `MIDI`/`◀Dev`/`Dev▶`/`NEXT` on col 8, pagination, artwork, all of it. Under
B1's assumption each band drops from 8 cells to 6 (rows 0-2), which changes capacity
and therefore pagination — see B2.

**Level 2 (`ableton.midi`)** is `midictl.hub`, which already exists and is already
reachable from (8,0). The MIDI folder key is a second door to the same screen, not a
new screen.

---

# 2. What each mode key does

The five keys are **not five folders.** Four of them switch *what the dials and strip
control* and only two of them also navigate. Keeping those two jobs separate is the
whole trick, and it is what makes §4's state retention possible.

| Key | Navigates to | Sets strip focus to |
|---|---|---|
| `VST` | `ableton.vst` (Level 2) | `vst` — device/macro focus, the mode that works today |
| `MIDI` | `ableton.midi` (Level 2) | unchanged — MIDI Control owns its own dials |
| `Device` | *nothing* — stays where you are | `mix` — the new Ableton mixer/track mode |
| `OS` | *nothing* (pending N1) | `os` — Scroll Y · Scroll X · Zoom · Apps · Tabs · clock |
| `Delay Calc` | *nothing* — docks a window | `States.setState(States.DELAY)` (see N2) |

`Device`, `OS` and `Delay Calc` **deliberately do not move you.** That is what makes
them usable mid-take: you are on the transport surface, you grab a fader, you are
still on the transport surface.

---

# 3. Device mode — the Ableton mixer

Six dials on the focused track, using V50's groundwork plus the three new verbs from
N4:

```
dial 1  Volume   (0.5 dB steps, exactly as Track Mode does today)   push = reset to 0.0
dial 2  Pan                                                          push = centre
dial 3  Send A                                                       push = 0
dial 4  Send B                                                       push = 0
dial 5  track select — rotate walks tracks                           push = fold/unfold
dial 6  the readout: track name, volume in dB, pan, and the M/S/R state
```

Mute · Solo · Record Arm are **buttons, not dials** — they are three toggles, and a
toggle on a dial press is a worse control than a lit key. Under B1's assumption they
have nowhere obvious to go on the transport surface without me choosing key positions
for you, so **that placement is a footprint decision I am leaving open.**

---

# 4. State retention — the green VST key

This is the part that is new architecture, and it is worth being explicit about why.

**Today the strip follows navigation.** `pickController()` resolves from the focused
Live device, `composite()` paints the strip, and both are driven by `Nav.current()`.
Press BACK and the Ableton module stops owning the strip. So *"the dials are still
actively controlling VSTs after I go back to Level 1"* is not a tint — **it is a real
decoupling of strip ownership from nav position.**

The proposal:

* Add **one** module-level variable, `stripFocus`, with values `vst` · `mix` · `os` ·
  `idle`. It lives in `ableton.js` beside `page`, it is set only by the mode keys, and
  **it survives `Nav.back()` because nothing in nav touches it.**
* `dials()` switches on `stripFocus` instead of inferring from the nav level.
* Each mode key paints `active: stripFocus === 'vst'` (and so on), which is how it
  gets the green tint **for free** — `active` is already a `keySpec()` field and
  `render.js` already draws it as a lit cap. **No new render path, no new whitelist
  field, no fourth list to keep in sync.** That matters: the three-whitelist trap in
  the field notes has bitten twice, and this design does not go near it.
* The tint colour: `active` uses the key's own `color`, so tinting green means giving
  the VST mode key `R.PALETTE.green` — the same green the Divisions readout already
  uses. One palette entry, already present.

**The interaction to watch, and the reason this is a design and not a patch:** the
Ableton hub is `fullScreenCapable`, so D15's `syncToScreen` / `autoFullFrom` already
moves the *carousel state* on entering and rewinds it on leaving. `stripFocus` is a
third, orthogonal axis on top of nav level and carousel state. **Three orthogonal
state machines is exactly where this project has been bitten before** (a hardcoded `4`
in `input.js`; eight literal `3`s in the tests last batch). So `stripFocus` gets
pinned by tests the way `States` now is: never compared to a literal outside
`ableton.js`, and asserted as a shape, not a value.

---

# 5. Implementation order, if you approve

Nothing here starts until B1, B2 and B3 are ruled on. Given a ruling:

1. **Sibling repo, one commit.** Six additive verbs: `transport_play`,
   `transport_stop`, `transport_loop`, `track_mute`, `track_solo`, `track_arm`. Plus
   `python3 scripts/test_bridge.py`. **Then restart Live** — nothing below can be
   verified on hardware until you do.
2. **`stripFocus` alone**, with no key moves at all. It is invisible until something
   sets it, so it lands green and proves the decoupling on its own.
3. **Split the hub**: `ableton.vst` gets today's `hubKeys`, `ableton.hub` becomes the
   transport surface. Re-run `slice_backgrounds.py` if B2's band capacity changed.
4. **The mode row**, in whichever cells you name.
5. **Device mode's six dials.**
6. **The Compact layouts.** The new `ableton.hub` and `ableton.vst` each need one, per
   the L6 dual-layout contract — and per L6 I owe you a **Discovery briefing** for
   each before I design either. I will not start those unprompted.

---

# 6. What I am NOT proposing

* **No change to `registry.js`, to any controller's parameter map, to `EQ8_MAPPING.md`,
  or to Pro-Q 3.** All frozen or verified-against-hardware.
* **No edit to any existing remote-script code path.** Additive verbs only.
* **Nothing from `docs/AUDIT.md` gets purged as part of this.** You said audit first,
  rule second, and these are separate turns.
