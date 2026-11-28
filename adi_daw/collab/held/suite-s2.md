# Track S, wave S2: the mix suite's first additions (held chain)

**This chain lands only when the director says so.** It is built ahead of the wave's opening (ADR-0254
d1, d25: the wave was to open after 3 December); the director asked to advance his phases now. No pull
request is open for it, and none is opened until he gives the word.

- Chain: `held/suite-s2-chain`, from the newest landing stack (`origin/land/basics-1128c` on 28
  November; origin/main once that stack is an ancestor of main). Merge the newest of them in before
  each push.
- This log: `adi_daw/collab/held/suite-s2.md` on `held/suite-s2-log`.
- Mission: PLAN Track S, wave S2, the engine and device parts, in this order: DEV-33, MX-38, MX-39,
  MX-40, AU-10's engine part. DEV-34 is another session's (OneShot). The window parts are written
  down per row below, for the window work that follows.
- Build and test: the build PC's slot 21, through `slot.sh` (core = MSVC Debug, warnings as errors,
  `ctest -LE performance`, the acceptance suite included).

## Rows

| Row | Chain head | What changed | How it was proved | State |
|---|---|---|---|---|
| DEV-33 | `held/suite-s2-chain` (first commit) | Echo's ducker is one kernel (`src/adi/dsp/ducker.hpp`); Echo runs on it unchanged. Reverb (ids 32 to 34) and Delay (ids 28 to 30) gain Duck On, Duck Threshold, Duck Release after their own parameters, off by default, Echo's ranges and defaults; the canvases declare them. Delay's feedback is not ducked | `adi_reverb_tests`, `adi_delay_tests`: off nulls bit for bit (set then switched off; a threshold never crossed), the law against Echo's written out, the tail returns; `adi_pd_native_devices_tests`: two fixtures with Duck on, canvas against core, bit-exact | slot 21 core check running |

## What each row's window part will need

### DEV-33, the Duck inside Reverb and Delay
- **Controls:** three, on each device's panel: Duck (a switch), Threshold (dB, -80 to 0, default -24),
  Release (ms, 1 to 5000, default 200, log). They are ordinary declared parameters
  (`adi.param` in `pd/devices/Reverb.pd` and `Delay.pd`), so today's generic device panel already shows
  them as it shows Echo's, and they automate and map as any parameter does. No command and no sheet.
- **For the director (look):** where the three sit on Reverb's and Delay's panels once those panels get
  their agreed look (ADR-0243). Live has no Duck on either device; Echo's own Ducking section is the
  model. Recommendation: the same three controls in the same order as Echo's, in a small "Duck"
  group at the right edge, after Dry/Wet.

## For the director

1. **DEV-33's place on the panels** (above): recommendation, Echo's three controls in Echo's order,
   at the right edge after Dry/Wet.

## Reserve

(none yet)

## Notes for whoever continues

- The slot's first build is cold (up to an hour). `bash /d/adibuild-tools/slot.sh wait 21` again while
  it says STILL RUNNING.
- `collab/devices.md` (the devices seat's log) still lists Reverb's and Delay's parameters without the
  Duck; it is that seat's file, so it is not edited here.
