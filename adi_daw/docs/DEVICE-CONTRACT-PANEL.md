# The device/parameter contract — design panel findings

**Status:** research input to an open decision. **Not** a decision. ADR-0035 and
ADR-0040 both leave the device/parameter contract open, and ADR-0052 decision 4
and ADR-0053 decision 1 both say the same thing about it from the other side: it
must be **format-agnostic**, because CLAP and a remote AudioGridder device go
behind the same model and a second chain means hybrid tracks, modulation and
suspension get implemented twice.

This file exists so the work behind that decision is not lost. Four independent
designs were produced and scored by four judges on separate lenses; what follows
is what they concluded and, more usefully, what they each got wrong.

---

## 1. The question

**Where does parameter identity live, and what happens when it changes?**

It is hard because one contract has to serve three device kinds at once:

| kind | parameter list | state |
|---|---|---|
| **VST3 / CLAP plugin** | fixed, indexed, discovered at instantiation, owned by code we did not write | opaque bytes |
| **Pure Data patch** (ADR-0035) | declared *inside* the patch, **mutable by the user at runtime** — including while the project is closed | the patch, which is text |
| **native device** | fixed at compile time | ours |

The second row is the whole difficulty. A VST3's parameter set cannot change
under you; a `.pd` file's can, because it is a text file a user can edit.

## 2. The four designs and how they scored

| design | schema fit | realtime | change | agent/determinism | total |
|---|---|---|---|---|---|
| **open** (no assigned thesis) | 8 | 8 | **9** | **4** | **29** |
| **plugin-shaped** | 7 | 8 | 7 | 7 | **29** |
| author-symbol | 6 | 5 | 6 | **9** | 26 |
| op-log-native | 4 | 4 | 8 | 8 | 24 |

**A tie, and the split is more useful than a winner.**

- **plugin-shaped** — *the source declares, the project owns the identity, the
  machine merely binds.* Three lifetimes kept apart: declaration (re-read from
  the source every load), identity (`param_key`, minted once, never re-derived),
  binding (rebuilt per instantiation, never persisted). Its observation that the
  schema is already this shape is correct: `plugin_params.param_id` is `TEXT`
  with `UNIQUE(device_id, param_id)`, and `automation_lanes.param_ref`,
  `macro_mappings.target_param_ref` and `controller_maps.target_param` are all
  `TEXT` columns already waiting for it.
- **author-symbol** — the patch author's symbol *is* the identity; nobody
  allocates anything, so there is nothing to reconcile.
- **op-log-native** — the parameter set is project state maintained by typed
  ops; a patch only ever makes a *proposal*.
- **open** — two-level identity: an immutable opaque `pid` that travels inside
  the artefact plus a mutable `symbol` that is only ever a label, and
  **parameters are never deleted, only tombstoned**.

## 3. What each one gets wrong

The self-criticisms were sharper than the scores.

**open came last on the lens that matters most to this project**, and the reason
is precise: its distinguishing claim is that *reconciliation emits no op at
all*. That breaks ADR-0003 — the parameter set would change outside the op log,
so it is neither undoable nor replayable nor visible to the agent.

**plugin-shaped is "named after the format that conforms to it worst".** Its own
words. Every property it is proudest of — real-valued automation surviving a
range change — is available to Pd, CLAP and native devices and **not to VST3**,
whose API exposes real values only as strings (`getParamStringByValue`). This
matters directly to the VST3 hosting mission: a VST3 lane is `normalized` by
necessity, not by choice.

**op-log-native has a dual-truth window.** Between a patch declaring something
new and the ops being applied, the project and the running patch disagree — and
the running patch is the one making sound.

**author-symbol imposes a grammar on a file it does not own.** `[adi.param
Cutoff Freq]`, `filter/cutoff`, `частота` — all refused by its symbol rule.

**And the one that cuts across all four:** every design requires an
**ADI-specific object inside the patch**. That sits badly against ADR-0035's
promise of "the same Pd that Miller Puckette wrote" — a patch authored for this
DAW would not open meaningfully in vanilla Pd. No design was told to respect
that constraint and none of them did.

## 4. The synthesis the panel points at

Not a decision — the shape a decision should probably take:

1. **open's structure**: two-level identity (opaque stable `pid` for binding,
   mutable `symbol` for display), tombstones rather than deletion, and a
   Layer-1 surface that does not change.
2. **with reconciliation emitting ops**, which is op-log-native's contribution
   and what fixes open's one fatal flaw.
3. **and author-symbol's insight about determinism by subtraction**: every id
   that nobody has to allocate is an ADR-0021 problem that does not exist.

The deletion case is what any candidate must be judged on: *a user deletes a
parameter that has ten thousand automation points, a macro mapping and a
controller binding.* A design that loses those is wrong however elegant it is
elsewhere. Tombstoning is why **open** scored 9 on that lens.

## 5. Why this matters to VST3 hosting specifically

ADR-0038 already decides the *state* half — parameters are the undo unit via
`beginEdit`/`performEdit`/`endEdit`, opaque chunks captured at boundaries and
content-addressed. What it does not decide is **identity**, and that is what
this panel is about.

The trap the panel makes visible: it is natural to give a VST3 device a
parameter model shaped by VST3, because that is the format in hand. Doing so
bakes in the one format that conforms worst — `normalized`-only values,
host-assigned `ParamID`s, no author symbols — and then Pd, CLAP and a remote
device each need an exception. ADR-0052 decision 4 exists to prevent exactly
that.

---

## 6. Blueprint: Porting Max for Live to ADI Pd

**Status:** a porting guide at the director's request (2026-09-25), written
against ADR-0035 (the Pd tier), ADR-0076 (Tier 1 devices are headless and the
DAW draws their knobs), ADR-0095 (the `$0-` message convention) and ADR-0096
(patches are generated, not hand-edited). The device contract is still open
(§4), so where a rule needs it, the rule says what the contract must carry,
not the syntax it will use. No porting tool exists. An M4L device (`.amxd`) is
a Max patcher, not Pd text, so a port is a rewrite, object by object, checked
against the original by ear and by rendering both.

**Before anything: may it be ported?** A Max for Live device is a copyrighted
work, and translating its patch object by object makes a derivative of it.
`OPEN_SOURCE_POLICY.md` decides:
- **May be ported, with its notice kept:** a device under a licence the
  policy pre-authorises (MIT, BSD, GPL, LGPL, and the rest of its §3 table).
- **Behaviour only, nothing copied** (policy §3 and §5):
  - Ableton's own devices, and every commercial device;
  - devices with no licence at all;
  - devices under Creative Commons *NonCommercial* terms, which are common on
    maxforlive.com and cannot go into a GPLv3 project.

  Behaviour means what the device does to the sound, measured, rebuilt from
  our own patch.
- **Ask the director:** any other licence, CC BY-SA included, which the policy
  does not cover.

### Rule 1 — Core DSP: a direct translation, object by object

The signal processing ports closely, because Pd and Max share an ancestor
(ADR-0035) and most MSP objects have a Pd equivalent. Most, not all: every
translation is checked, and the port's DSP maths lives in `src/adi/dsp/`,
tested, as the limiter's does (ADR-0096).

| Max / MSP | ADI Pd (vanilla) |
|---|---|
| `[buffer~]` | `[array define]` or `[table]`, loaded with `[soundfiler]` |
| `[groove~]` (looping, variable-speed playback of a buffer) | `[tabread4~]` driven by `[phasor~]` or `[vline~]`; `[tabplay~]` for a one-shot |
| `[tapin~]` / `[tapout~]` (a delay line) | `[delwrite~]` / `[delread4~]` |
| `[cycle~]` | `[osc~]`, or `[tabosc4~]` for a wavetable |
| `[poly~]` | `[clone]` |
| `[pfft~]` | a subpatch with `[block~ N 4]` and `[rfft~]` / `[rifft~]` |

*A correction to the rule as proposed:* `[delwrite~]` and `[vd~]` are a delay
line, the translation of `[tapin~]`/`[tapout~]`, not of `[groove~]`, which
plays a buffer and maps to `[tabread4~]`. `[vd~]` is also the old name of
`[delread4~]`, kept for compatibility; a port writes `[delread4~]`.

**Vanilla only.** `cyclone`, a Pd library of Max clones, would shortcut the
work, but shipping any Pd external is a per-library decision (ADR-0035) and
none has been made.

### Rule 2 — The UI: headless, and the DAW draws the knobs

libpd is headless, and Tier 1 devices never draw their own interface
(ADR-0076). So every GUI object is deleted: `[live.dial]`, `[live.slider]`,
`[live.numbox]`, `[live.menu]`, `[live.toggle]`, `[live.text]`, panels,
`[jsui]`.

**But in M4L those objects *are* the parameters.** A `live.dial` whose
parameter visibility is "Automated and Stored" is a host parameter, and its
Inspector holds the parameter's name, range, unit, initial value, exponent and
steps. Deleting the dial without carrying those over loses the parameter. For
each parameter object, the port declares:
- the name;
- the minimum, maximum and default;
- the unit, and the curve (M4L's exponent);
- for a menu, the enumerated items.

The DAW draws the knobs from that declaration, as Live draws M4L's.

- **The declaration's syntax is the open contract's to decide** (§4). The rule
  as proposed wrote it `[adi.param name min max default]`. That is one of the
  panel's candidates (author-symbol, §2), not a decision, and §3 records
  against every candidate that an ADI-specific object inside the patch breaks
  ADR-0035's promise that a patch stays vanilla Pd. Until the contract is
  decided, a port lists its declarations in the generator (ADR-0096), which is
  where the patch comes from anyway.
- **Values arrive at `$0-` receives: `[r $0-cutoff]`, never a bare
  `[r cutoff]`.** Pd's send and receive names are global within an instance,
  so two copies of one device on two tracks would share a bare name. This is
  ADR-0095's rule, and the shipped patches follow it (`[r $0-release]`).
- **UI objects with DSP inside do port.** `[live.gain~]` becomes `[*~]` with
  `[line~]` smoothing, driven by its parameter. A meter the device drew is the
  DAW's to draw.

### Rule 3 — The Live API (LOM) does not port

`[live.path]`, `[live.object]`, `[live.observer]`, `[live.remote~]` and
`[live.thisdevice]` query and drive the Live Object Model, and there is no Live
Object Model in ADI. Two consequences:
- **Transport comes from the host as messages.** Tempo, play state and the
  beat position arrive at `$0-` receives like any other host message. The
  names and the rate are the contract's to fix.
  - They arrive once per Pd block (64 samples), not sample-accurately.
  - A tempo-synced device therefore derives its phase from the beat position
    it is sent, never by counting its own blocks.
  - This replaces `[plugsync~]`, `[transport]` and `[live.observer]` on tempo.
- **A device whose purpose is the LOM is not a Pd device in ADI.** Clip
  generators, track and device remote controls, and MIDI-clip tools change
  the project, and in ADI the project changes only through ops (ADR-0003). So
  they port as scripts over the op API, the agent's vocabulary, not as
  patches.

### Rule 4 — Compiled code: `[gen~]` does not port

Pd has no `[gen~]`, and ADI will not ship one. A `gen~` box is rebuilt in one
of two ways:
1. **In vanilla Pd objects,** which suits block-rate maths. `gen~` is often
   used for what Pd's block-based signal objects cannot do: feedback within one
   sample, as in a one-pole filter or a short feedback delay. For that,
   `[fexpr~]` evaluates a per-sample expression with access to its own past
   outputs. A `[block~ 1]` subpatch also works, but it is costly.
2. **As a native C++ node** (Tier 1, ADR-0062). This suits a `gen~` that is
   hot, reused or subtle. Its maths lives in `src/adi/dsp/`, tested against a
   reference, and the Pd patch or the device uses it.

RNBO, Cycling '74's exporter, is not a route: it needs Max and RNBO to author
(ADR-0035).
