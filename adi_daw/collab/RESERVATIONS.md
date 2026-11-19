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
| DF-M | Mix View | the Mix View seat, through win | DF-M61 | DF-M62 |
| DF-W | Window | the window lane and win | DF-W35 | DF-W36 |
| DF-E | Engine | the engine seat, engine-win and the devices lane; the row's owner column says which | DF-E57 | DF-E58 |
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

Next free: **N59**. The project key and scale take it if they need an engine field.
