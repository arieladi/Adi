# Reservations: the numbers in flight

Win keeps this file (ADR-0248 d5). A seat reserves here before it numbers a schema minor, an ADR or a
register row: it asks in its log, win writes the row on main, and only then does the number go into a
branch. On 19 November two seats that numbered alone both took schema 1.22, and four pull requests
wrote the same "212 ops" headline.

**The rule.**
- Ask in your log ("Reserve: schema minor for …", "Reserve: two DF-E rows for …"). Win writes the row
  here on main within the day and answers in `collab/win.md`.
- Use the number only after its row is on main. A number given up is marked `burned` and never reused.
- When the work lands, win marks the row `landed` with the pull request.
- Whoever lands second recounts what is counted (OPS.md's headline, the README's entry count) against
  current main.

## Schema minors (`src/adi/store.hpp`, `kSchemaMinor`)

Main is at 1.21. PLAN PD-10's register of pending minors is this table.

| Minor | Seat | What | Pull request | Status |
|---|---|---|---|---|
| 1.22 | mixview | the device key taps, `routing_key_taps` (DEV-05, MX-24, DF-M11) | #651 (#674 carries the same) | in flight; lands first, once DF-M60 (c) is fixed on its branch (ADR-0248 d10) |
| 1.23 | engine | the send taps, `routing_send_taps` (DF-E22, ADR-0245 d10) | #677 (#665 carries the same) | in flight; renumbered from 1.22 once #651 lands, with its line in `store.hpp`'s comment |
| 1.24 | — | the next minor, by reservation here | — | free |

Known to want a minor and not yet reserved: DF-W6, the `tracks.role` column (win).

## ADR numbers

The reservation table stays in `collab/README.md` ("Reserved ADR numbers"), where check 8 of
`tools/validate_schema.py` reads it. #210, win's earlier fix that makes check 8 catch stray and missing
rows, is kept open and folded into this work.

Next free: **ADR-0252**.

## Register ids (`docs/DEFERRED.md`)

| Prefix | Section | Who numbers in it | Last used | Next free |
|---|---|---|---|---|
| DF-C | Control | the control seat, through win | DF-C5 | DF-C6 |
| DF-M | Mix View | the Mix View seat, through win | DF-M62 | DF-M63 |
| DF-W | Window | the window lane and win | DF-W38 | DF-W39 |
| DF-E | Engine | the engine seat, engine-win and the devices lane; the row's owner column says which | DF-E64 | DF-E65 |
| DF-R | Repository | win | DF-R1 | DF-R2 |

Three seats share DF-E, so each DF-E id is reserved here first.

## Op names (`docs/OPS.md`)

Claimed by open pull requests. #674 carries #651's op and #665 carries #677's: one op each.

| Op | Pull request(s) | Seat |
|---|---|---|
| `routing.setKeyTap` | #651, #674 | mixview |
| `routing.setSendTap` | #677, #665 | engine |
| `lane.restore` | #662 | engine-win |

## The window's needs-table numbers (`collab/window.md`)

| N | What | Owner |
|---|---|---|
| N57 | the insert of a third-party plug-in (`preparePluginInsert`, `insertPlugin`), from the Mix View's 668e05c9 core with Live's placement rules | engine |
| N58 | one catalogue API off the message thread: a startup scan service, Rescan, a default worker count, each VST3's kind with the instrument flag | engine-win |
| N59 | a distinct scan state for "the scanner helper could not start" (`HelperCouldNotStart`) | engine-win; **met on main** (#694) |
| N60 | live MIDI input to the armed track's instrument, taking the window's per-port rings (`MidiPortEvent`) on the audio thread | engine's seam on main (#685, `Session::enqueueMidiInput`); the drain from the window's rings into it is the window's (window-fn) |
| N61 | a `midi.port.name` settings row, so a port's record heals by name when its identifier changes | engine-win; **met on main** (`settings/registry.cpp`); the healing by name is the window's |
| N62 | the render worker's contract for plug-ins (instances made and given state on the message thread, processing on the worker, `serviceHost` marshalled), so Export renders a project with plug-ins | engine-win (EX-11's engine half) |
| N63 | `notes::prepareBatch(snapshot, ops)`: one prepared `note.applyDiff` whose effects are the legacy note ops applied in turn, one undo step | engine (`note_diff*`, MI-01) |
| N64 | the snapshot's recording fields `TrackNode::recordArmed`, `monitorMode`, `inputRef` | engine (`engine/snapshot.*`) |
| N65 | the input-monitor node of MX-09, heard per `monitor_mode` | engine; **met on main** (#685, `Session::setAudioMonitorInputs`); wiring it is the window's |
| N66 | a failed capture's audio kept up to the failure (a locate or an overflow after minutes of good audio is lost whole today) | engine-win (recording, ADR-0248 d7) |
| N67 | Live's monitoring default for a new track: Auto, where New and `track.create` store 0 (Off) | **ruled by win (ADR-0203 d1): Auto, as Live**; the engine's `track.create` default and New's tracks |
| N68 | after a discovery job, a project open still probes a plug-in's saved path when the catalogue lacks it, as before any job (`managedDiscovery_`, `juce_device_loader.cpp` 450) | engine-win (N58) |

Next free: **N69**. The project key and scale take it if they need an engine field.
