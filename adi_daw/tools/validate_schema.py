#!/usr/bin/env python3
"""Validate docs/format/schema.sql against a real SQLite engine.

Run from anywhere:  python adi_daw/tools/validate_schema.py

Checks, in order:
  1. The DDL executes cleanly into a fresh in-memory database.
  2. application_id and user_version survive as the spec says they must.
  3. Every declared foreign key resolves to a real table and column
     (SQLite resolves FKs lazily, so a typo stays invisible until runtime).
  4. No UNIQUE index has a nullable column in it -- in SQLite, NULLs are
     distinct, so such an index does not actually enforce uniqueness.
  5. foreign_keys=ON plus a smoke-test insert of a minimal project.
  5b. Every table is STRICT -- without it a TEXT value can sit in a REAL
     column and the C API coerces it silently on read (ADR-0029).
  5c. Every polymorphic reference kind in `routing` names a real table or a
     known-external endpoint; a FOREIGN KEY cannot constrain (kind, id).
  5d. Content addressing is enforced: a duplicate media hash is rejected
     and un-hashed rows still do not collide (ADR-0032).
  5e. Every clip is placed, on a track, in the domain it declares (ADR-0037).
  5f. Opaque device state references resolve (ADR-0038).
  5g. One grouping concept, and a track may hold audio and MIDI clips at
     once (ADR-0044, ADR-0045).
  5h. routing.origin defaults to 'user' so grouping cannot rewrite a
     hand-made connection (ADR-0044).
  6. The tick base really has the arithmetic properties SPEC 4.2 claims,
     because a specification should not assert what it can check.
  7. The counts README states are the counts that exist, and no ADR
     number is used twice (ADR-0028).
"""

from __future__ import annotations

import pathlib
import re
import sqlite3
import sys

SPEC_APPLICATION_ID = 1094994225  # 0x41444931 == 'ADI1'
SPEC_USER_VERSION = 1000
ADI_PPQ = 5765760  # SPEC 4.2

HERE = pathlib.Path(__file__).resolve().parent
SCHEMA = HERE.parent / "docs" / "format" / "schema.sql"


def fail(msg: str) -> None:
    print(f"  FAIL  {msg}")
    fail.count += 1  # type: ignore[attr-defined]


fail.count = 0  # type: ignore[attr-defined]


def ok(msg: str) -> None:
    print(f"  ok    {msg}")


def main() -> int:
    if not SCHEMA.exists():
        print(f"schema not found: {SCHEMA}")
        return 2

    ddl = SCHEMA.read_text(encoding="utf-8")
    db = sqlite3.connect(":memory:")

    # --- 1. it executes -----------------------------------------------------
    print("[1] execute DDL")
    try:
        db.executescript(ddl)
    except sqlite3.Error as exc:
        print(f"  FAIL  {type(exc).__name__}: {exc}")
        return 1
    ok(f"{SCHEMA.name} executed")

    tables = [
        r[0]
        for r in db.execute(
            "SELECT name FROM sqlite_master WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%' ORDER BY name"
        )
    ]
    indexes = [
        r[0]
        for r in db.execute(
            "SELECT name FROM sqlite_master WHERE type='index' "
            "AND name NOT LIKE 'sqlite_%'"
        )
    ]
    ok(f"{len(tables)} tables, {len(indexes)} explicit indexes")

    # --- 2. container identity ---------------------------------------------
    print("[2] container identity")
    app_id = db.execute("PRAGMA application_id").fetchone()[0]
    user_v = db.execute("PRAGMA user_version").fetchone()[0]
    if app_id != SPEC_APPLICATION_ID:
        fail(f"application_id is {app_id}, spec says {SPEC_APPLICATION_ID}")
    else:
        ok(f"application_id = {app_id} (0x{app_id:08X} = 'ADI1')")
    if user_v != SPEC_USER_VERSION:
        fail(f"user_version is {user_v}, spec says {SPEC_USER_VERSION}")
    else:
        ok(f"user_version = {user_v}")

    # --- 3. foreign keys resolve -------------------------------------------
    print("[3] foreign keys resolve")
    bad = 0
    for t in tables:
        cols = {r[1].lower() for r in db.execute(f'PRAGMA table_info("{t}")')}
        del cols  # only needed per-target below
        for row in db.execute(f'PRAGMA foreign_key_list("{t}")'):
            target, from_col, to_col = row[2], row[3], row[4]
            if target not in tables:
                fail(f"{t}.{from_col} -> missing table {target}")
                bad += 1
                continue
            tcols = {r[1].lower() for r in db.execute(f'PRAGMA table_info("{target}")')}
            # to_col NULL means "the primary key"
            if to_col is not None and to_col.lower() not in tcols:
                fail(f"{t}.{from_col} -> {target}.{to_col} (no such column)")
                bad += 1
    if not bad:
        ok("every foreign key resolves to a real table and column")

    # --- 4. UNIQUE indexes must not contain nullable columns ---------------
    # In SQLite two NULLs are distinct, so a UNIQUE index over a nullable
    # column silently fails to enforce what it looks like it enforces.
    print("[4] UNIQUE indexes over nullable columns")
    leaky = 0
    for t in tables:
        nullable = {
            r[1].lower()
            for r in db.execute(f'PRAGMA table_info("{t}")')
            if r[3] == 0 and r[5] == 0  # notnull == 0 and not pk
        }
        for idx in db.execute(f'PRAGMA index_list("{t}")'):
            name, is_unique = idx[1], idx[2]
            if not is_unique or name.startswith("sqlite_"):
                continue
            for c in db.execute(f'PRAGMA index_info("{name}")'):
                col = c[2]
                if col is None:
                    continue  # expression column, e.g. IFNULL(x,-1) -- fine
                if col.lower() in nullable:
                    fail(
                        f"UNIQUE {name} on {t} includes nullable {col} "
                        f"-- NULLs are distinct, so this does not enforce uniqueness"
                    )
                    leaky += 1
    if not leaky:
        ok("no UNIQUE index relies on a nullable column")

    # --- 5. smoke test ------------------------------------------------------
    print("[5] smoke test: minimal project")
    try:
        db.execute("PRAGMA foreign_keys = ON")
        db.executescript(
            """
            INSERT INTO project(id, name) VALUES (1, 'Smoke Test');
            INSERT INTO tempo_map(pos_ticks, bpm) VALUES (0, 120.0);
            INSERT INTO time_signature_map(pos_ticks, numerator, denominator)
                 VALUES (0, 4, 4);
            INSERT INTO tracks(id, kind, name) VALUES (1, 'midi', 'Keys');
            INSERT INTO mixer_strip(track_id) VALUES (1);
            INSERT INTO clips(id, track_id, kind, name, time_base,
                              pos_ticks, length_ticks)
                 VALUES (1, 1, 'midi', 'Riff', 0, 0, 23063040);
            INSERT INTO event_streams(clip_id, stream_kind, data)
                 -- A real 16-byte ANOT header: 'ANOT', v1, rec_size 40,
                 -- count 0, flags SortedByTime. Previous fixtures here were 15
                 -- and 14 bytes -- too short for the header they claimed to be,
                 -- in a file whose job is catching exactly that.
                 VALUES (1, 'notes', X'414E4F54010028000000000001000000');
            INSERT INTO note_expression(clip_id, note_id, dimension, data)
                 VALUES (1, 1, 1, X'41455850010018000000000001000000');
            INSERT INTO ops(txn_id, ts_utc, actor, op_type, label)
                 VALUES (1, 0, 'user', 'clip.create', 'Create clip');
            INSERT INTO ops(txn_id, ts_utc, actor, actor_detail, op_type, label)
                 VALUES (2, 1, 'agent', 'adi-agent/claude-opus-5',
                         'note.quantize', 'Quantize Keys');
            """
        )
        db.commit()
    except sqlite3.Error as exc:
        fail(f"{type(exc).__name__}: {exc}")
    else:
        ok("minimal project inserted with foreign_keys=ON")

    # Confirm the accountability query actually works.
    agent_ops = db.execute(
        "SELECT COUNT(*) FROM ops WHERE actor = 'agent'"
    ).fetchone()[0]
    if agent_ops != 1:
        fail(f"agent-op accountability query returned {agent_ops}, expected 1")
    else:
        ok("agent-attributed ops are queryable (SPEC 8.1)")

    fk_violations = db.execute("PRAGMA foreign_key_check").fetchall()
    if fk_violations:
        fail(f"foreign_key_check reported {len(fk_violations)} violations")
    else:
        ok("foreign_key_check clean")

    # --- 5b. every table is STRICT -------------------------------------------
    # Without STRICT, SQLite's flexible typing lets a TEXT value live in a REAL
    # column and sqlite3_column_double() coerces it silently, so a reader
    # returns a number that is not what is stored. There are 23 REAL columns
    # here. Checked per table rather than by counting the keyword, because a
    # comment mentioning STRICT would satisfy a count. See ADR-0029.
    print("[5b] every table is STRICT")
    not_strict = []
    for t in tables:
        row = db.execute(
            "SELECT sql FROM sqlite_master WHERE type='table' AND name = ?", (t,)
        ).fetchone()
        table_sql = (row[0] or "") if row else ""
        tail = table_sql[table_sql.rfind(")") :].upper() if ")" in table_sql else ""
        if "STRICT" not in tail:
            not_strict.append(t)
    if not_strict:
        fail(f"tables without STRICT: {not_strict}")
    else:
        ok(f"all {len(tables)} tables are STRICT")

    # --- 5c. polymorphic reference kinds resolve -----------------------------
    # `routing` carries (kind, id) pairs, which a FOREIGN KEY cannot constrain.
    # That is the price of one routing table instead of six -- but it means a
    # CHECK can permit a kind whose target table does not exist, which is how
    # 'bus' became a dangling reference by construction (ADR-0029). Internal
    # kinds must name a real table; external ones are an explicit allow-list.
    print("[5c] polymorphic reference kinds resolve")
    EXTERNAL_KINDS = {"hw_in", "hw_out"}   # hardware port indices, not rows
    import re as _re

    dangling = []
    for col in ("src_kind", "dst_kind"):
        row = db.execute(
            "SELECT sql FROM sqlite_master WHERE type='table' AND name='routing'"
        ).fetchone()
        m = _re.search(col + r"[^,]*?CHECK\s*\(\s*" + col + r"\s+IN\s*\(([^)]*)\)",
                       row[0], _re.S | _re.I)
        if not m:
            fail(f"could not find the {col} CHECK in routing")
            continue
        kinds = [k.strip().strip("'\"") for k in m.group(1).split(",")]
        for k in kinds:
            if k in EXTERNAL_KINDS:
                continue
            target = k + "s" if not k.endswith("s") else k
            if target not in tables and k not in tables:
                dangling.append(f"routing.{col} permits '{k}' but no table '{target}' exists")
    if dangling:
        for d in dangling:
            fail(d)
    else:
        ok("every routing endpoint kind names a real table or a known external")

    # --- 5d. content addressing is actually enforced -------------------------
    # ADR-0005 says dedupe comes from hash_blake3. A non-unique index makes that
    # a comment: twenty rows could share one hash and relink-by-content would
    # have no single answer. ADR-0031.
    print("[5d] content addressing is enforced")
    try:
        db.execute("INSERT INTO media_files(id, hash_blake3) VALUES (100, 'deadbeef')")
        db.execute("INSERT INTO media_files(id, hash_blake3) VALUES (101, 'deadbeef')")
        fail("two media rows can share a hash -- dedupe is not enforced")
    except sqlite3.IntegrityError:
        ok("a duplicate media hash is rejected")
    # ...but rows with no hash yet must not collide with each other.
    try:
        db.execute("INSERT INTO media_files(id, hash_blake3) VALUES (102, '')")
        db.execute("INSERT INTO media_files(id, hash_blake3) VALUES (103, '')")
        ok("un-hashed rows do not collide (the index is partial)")
    except sqlite3.IntegrityError as exc:
        fail(f"un-hashed media rows collide: {exc}")

    # --- 5e. ADR-0037: every clip is placed, on a track ----------------------
    # These columns were nullable only because a clip owned by a clip_slot had
    # no timeline position. ADR-0037 removed clip_slots, so the nullability had
    # to go with it -- otherwise the schema still permits the state that the
    # removed feature was the only reason for.
    print("[5e] every clip is placed (ADR-0037)")
    db.execute("INSERT INTO tracks(id, kind, name) VALUES (900, 'audio', 'placement')")
    for sql, why in (
        ("INSERT INTO clips(id,track_id,kind,time_base,pos_ticks) "
         "VALUES (900,900,'midi',0,NULL)", "a musical clip with no pos_ticks"),
        ("INSERT INTO clips(id,track_id,kind,time_base,pos_ns) "
         "VALUES (901,900,'midi',1,NULL)", "a linear clip with no pos_ns"),
        ("INSERT INTO clips(id,track_id,kind,time_base,pos_ticks) "
         "VALUES (902,NULL,'midi',0,0)", "a clip on no track"),
    ):
        try:
            db.execute(sql)
            fail(f"{why} was accepted")
        except sqlite3.IntegrityError:
            ok(f"{why} is rejected")
    try:
        db.execute("INSERT INTO clips(id,track_id,kind,time_base,pos_ticks) "
                   "VALUES (903,900,'midi',0,0)")
        db.execute("INSERT INTO clips(id,track_id,kind,time_base,pos_ns) "
                   "VALUES (904,900,'midi',1,0)")
        ok("a placed clip in either domain is accepted")
    except sqlite3.IntegrityError as exc:
        fail(f"a correctly placed clip was rejected: {exc}")

    # --- 5f. ADR-0038: opaque device state is content-addressed --------------
    # plugin_state holds a hash, not bytes, so an undo history that returns a
    # plugin to a state it already held costs nothing. That only holds if the
    # reference is real: a dangling state_hash is a device whose state is gone.
    print("[5f] device state references resolve (ADR-0038)")
    db.execute("INSERT INTO plugin_refs(id, format, uid) VALUES (900, 'vst3', 'x')")
    db.execute("INSERT INTO device_chains(id, track_id) VALUES (900, 900)")
    db.execute("INSERT INTO devices(id, chain_id, plugin_ref_id) VALUES (900, 900, 900)")
    try:
        db.execute("INSERT INTO plugin_state(id, device_id, stream_role, state_hash) "
                   "VALUES (900, 900, 'component', 'nosuchhash')")
        fail("plugin_state accepted a hash with no state_blobs row")
    except sqlite3.IntegrityError:
        ok("a dangling state_hash is rejected")
    db.execute("INSERT INTO state_blobs(hash_blake3, data, size_bytes) "
               "VALUES ('abc123', X'00', 1)")
    db.execute("INSERT INTO plugin_state(id, device_id, stream_role, state_hash) "
               "VALUES (901, 900, 'component', 'abc123')")
    db.execute("INSERT INTO plugin_state(id, device_id, stream_role, state_hash) "
               "VALUES (902, 900, 'controller', 'abc123')")
    ok("two streams may share one blob -- that is the point of ADR-0038")

    # --- 5g. ADR-0044/0045: one grouping concept, and hybrid tracks ---------
    print("[5g] grouping and hybrid tracks (ADR-0044, ADR-0045)")
    try:
        db.execute("INSERT INTO tracks(id, kind, name) VALUES (910, 'folder', 'f')")
        fail("'folder' is still a track kind -- ADR-0044 merged it into 'group'")
    except sqlite3.IntegrityError:
        ok("'folder' is no longer a track kind")

    # The hybrid rule is an ABSENCE of a constraint, which is exactly the kind
    # of rule that gets re-added by someone tidying up. Asserting it holds is
    # how that gets caught.
    db.execute("INSERT INTO tracks(id, kind, name) VALUES (911, 'audio', 'hybrid')")
    db.execute("INSERT INTO clips(id,track_id,kind,time_base,pos_ticks) "
               "VALUES (910, 911, 'audio', 0, 0)")
    db.execute("INSERT INTO clips(id,track_id,kind,time_base,pos_ticks) "
               "VALUES (911, 911, 'midi', 0, 0)")
    ok("an 'audio' track holds an audio clip and a midi clip at once")

    # --- 5h. ADR-0044: routing.origin protects a hand-made connection -------
    print("[5h] routing.origin and device suspension opt-out")
    db.execute("INSERT INTO routing(id, src_kind, src_id, dst_kind, dst_id, kind) "
               "VALUES (910, 'track', 911, 'track', 911, 'send')")
    row = db.execute("SELECT origin FROM routing WHERE id = 910").fetchone()
    if row and row[0] == "user":
        ok("a routing row defaults to origin='user' -- grouping leaves it alone")
    else:
        fail(f"routing.origin defaulted to {row and row[0]!r}, not 'user'. "
             "An 'auto' default means automatic grouping may rewrite a "
             "connection nothing asked it to touch (ADR-0044)")
    try:
        db.execute("UPDATE routing SET origin = 'managed' WHERE id = 910")
        fail("routing.origin accepted a value outside ('auto','user')")
    except sqlite3.IntegrityError:
        ok("routing.origin admits only 'auto' and 'user'")

    try:
        db.execute("UPDATE devices SET always_process = 2 WHERE id = 900")
        fail("devices.always_process accepted a non-boolean")
    except sqlite3.IntegrityError:
        ok("devices.always_process is 0 or 1 (ADR-0043)")

    # --- 6. the tick base actually has the properties SPEC 4.2 claims --------
    # These are load-bearing claims in a specification, so they get checked
    # rather than asserted. An earlier draft claimed 960 PPQ could not express a
    # quintuplet, which is false -- 960/5 = 192.
    print("[6] ADI_PPQ properties (SPEC 4.2)")
    # fetchone() is None when check [5] failed to insert, and None[0] is a
    # TypeError that kills the run -- truncating this check and the summary, so
    # the tool stops reporting at exactly the moment it has something to report.
    row = db.execute("SELECT ppq FROM project WHERE id = 1").fetchone()
    ppq = row[0] if row else None
    if ppq is None:
        fail("no project row to read ppq from (check [5] did not insert)")
    elif ppq != ADI_PPQ:
        fail(f"project.ppq is {ppq}, spec says {ADI_PPQ}")
    if ADI_PPQ != 2**7 * 3**2 * 5 * 7 * 11 * 13:
        fail("ADI_PPQ factorisation in the spec is wrong")
    else:
        ok("ADI_PPQ = 2^7 x 3^2 x 5 x 7 x 11 x 13")

    inexact = [n for n in range(1, 17) if ADI_PPQ % n]
    if inexact:
        fail(f"tuplets not exactly representable: {inexact}")
    else:
        ok("every tuplet 1..16 is exactly representable")

    if ADI_PPQ % 32:
        fail("128th notes are not exactly representable")
    else:
        ok("128th notes and 128th triplets exact")

    unrepresentable = [p for p in (24, 48, 96, 192, 384, 480, 960) if ADI_PPQ % p]
    if unrepresentable:
        fail(f"interchange PPQs lost: {unrepresentable}")
    else:
        ok("MIDI interchange PPQs 24/48/96/192/384/480/960 all divide exactly")

    # The comparison the spec draws against Logic (960) and Cubase (480).
    for other in (960, 480):
        if other % 7 == 0:
            fail(f"spec claims {other} PPQ cannot express a septuplet, but it can")
    ok("the septuplet comparison against 960/480 PPQ holds")

    years = (2**63 / ADI_PPQ) / 2 / 60 / 60 / 24 / 365
    if years < 1000:
        fail(f"i64 tick range is only {years:.0f} years at 120 BPM")
    else:
        ok(f"i64 tick range = {years:,.0f} years at 120 BPM")

    # --- 7. the counts README states are the counts that exist ---------------
    # `validate_ops.py` has checked the op count since the prose said 152 and
    # the tables held 174. The same class of drift had gone unnoticed here: the
    # README said 38 tables through two schema changes, and 37 ADRs through
    # eleven. A number in prose that nothing checks is a number that is wrong.
    print("[7] README's counts match reality")
    readme = (HERE.parent / "README.md").read_text(encoding="utf-8")
    decisions = (HERE.parent / "docs" / "DECISIONS.md").read_text(encoding="utf-8")

    real_tables = len(re.findall(r"^CREATE TABLE ", SCHEMA.read_text(encoding="utf-8"), re.M))
    m = re.search(r"verified on every change\. (\d+) tables", readme)
    if not m:
        fail("README no longer states a table count where this check looks")
    elif int(m.group(1)) != real_tables:
        fail(f"README says {m.group(1)} tables, schema.sql has {real_tables}")
    else:
        ok(f"README and schema.sql agree: {real_tables} tables")

    real_adrs = len(re.findall(r"^## ADR-", decisions, re.M))
    m = re.search(r"Decision log\. Append-only\. (\d+) entries", readme)
    if not m:
        fail("README no longer states an ADR count where this check looks")
    elif int(m.group(1)) != real_adrs:
        fail(f"README says {m.group(1)} ADRs, DECISIONS.md has {real_adrs}")
    else:
        ok(f"README and DECISIONS.md agree: {real_adrs} ADRs")

    # Numbers are never reused (ADR-0028), so a duplicate heading means two
    # branches landed the same number -- which has happened twice.
    nums = re.findall(r"^## (ADR-\d+R?) ", decisions, re.M)
    dupes = sorted({n for n in nums if nums.count(n) > 1})
    if dupes:
        fail(f"duplicate ADR numbers, and ADR-0028 says numbers are never reused: {dupes}")
    else:
        ok("no ADR number is used twice")

    n = fail.count  # type: ignore[attr-defined]
    print()
    print(f"{'FAILED' if n else 'PASS'} -- {n} problem(s)")
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
