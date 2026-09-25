# Frozen schemas

`schema-1.N.sql` is `docs/format/schema.sql` exactly as schema 1.N shipped,
byte for byte, taken from git history:

| File | Commit | Minor added |
|---|---|---|
| `schema-1.0.sql` | `225c6fc^` | the 1.0 baseline |
| `schema-1.1.sql` | `225c6fc` | embedded media locked by triggers (ADR-0136) |
| `schema-1.2.sql` | `74ac501` | remarks (ADR-0131) |
| `schema-1.3.sql` | `f593fd7` | history snapshots (ADR-0128, ADR-0140) |
| `schema-1.4.sql` | `9805616` | expression routes, agent requests (ADR-0146) |
| `schema-1.5.sql` | `42d0000` | the plug-in panel (ADR-0154) |
| `schema-1.6.sql` | `35ebce8` | op clients and clocks (ADR-0161) |

They are test inputs, never edited. `tests/test_migrate.cpp` builds real 1.N
files from them and upgrades each one. `tools/validate_schema.py` check 9 proves
every minor was additive, and that the missing objects applied to each frozen file
give the current schema (ADR-0144).

**Bumping the minor:** first copy the current `schema.sql` here as
`schema-1.<old minor>.sql`, then add the new minor's objects to
`migrationSteps()` in `src/adi/store.cpp`. Check 9 fails until the copy exists.
