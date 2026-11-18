# M2 history-retention corpus

`adi_history_retention_fuzz_tests` runs the complete corpus by default. The
optional `long`, `seed1`, `seed2`, `seed3` selectors run complete named cases for
compiler review, diagnosis and compiled fault controls; they do not replace the
aggregate native gate.

The long case commits 3,000 two-operation editing steps under the default
2,000-transaction/1 GB policy, with two names on the day-one origin. Every step
checks admission, retained transaction count, exact typed material and the newest
complete group/head. Periodic full timelines and public Undo metadata supplement
those independent SQL counts. Count pressure must retain at least the documented
90% low-water depth, preserve both origin aliases as one material hash, restore
the original state and preserve the departing final tip. This case exercises
count pressure, not a 1 GB memory allocation or collaboration/embedded-media
protection.

Three 416-decision sequences use xorshift64 seeds `0x4849324300112026`,
`0x4849324300222026`, `0x4849324300332026` and a 31-transaction cap. They mix
one-to-three-operation commits, whole-step Undo/Redo, forks, named snapshots,
stored restores and switches to known branches. Coverage minima prevent a seed
silently missing those paths. Every named point is revisited at the end, branch
identities remain present, and each forest passes the ordinary integrity check.

The oracle starts with literal SQL rows and changes only the submitted project
name and volume in its independent model. It compares 21 explicitly listed native
material tables, tagging every SQLite storage class and comparing REAL bits and
BLOB bytes. Static rows include routing, a MIDI clip/ANOT with a full-width note
identity and an opaque future project BLOB column. It never calls the checkpoint
encoder/decoder or project digest to calculate expected material. Branch metadata
selects targets whose expected values were recorded from original inputs. A stored
restore starts a new local navigation epoch; retained-root refusal holds the head.
No renderer is attached, so the test caller is already stopped/reloadable for its
explicit `SessionReloadPrepared` restores. It does not claim runtime reload or
Live parity measurements.
