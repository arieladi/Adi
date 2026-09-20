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
