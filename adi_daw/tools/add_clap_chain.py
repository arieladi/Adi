#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Put CLAP plug-ins on a track of a demo project, for an offline render.

How ADR-0166 to ADR-0171 checked every plug-in in the DAW itself:

    ./build/adi_tool create demo.adi
    python tools/make_demo_project.py demo.adi --clip-rates 48000
    python tools/add_clap_chain.py demo.adi com.adi.airwindows.distortion=Distortion \\
        michaelwillis.dragonfly.hall=Hall
    ./build-juce/adi_play_artefacts/Debug/adi_play demo.adi --search <dir of .clap> --render 4

Each argument is `<CLAP id>=<device name>`, in chain order. adi_play prints
each device as loaded, placeholder or skipped, and the master's peak per
second: a reverb's tail shows as level after the clip ends. A plug-in id whose
last part is "onepingonly" is written as an instrument; everything else as an
effect.

It adds to what is there, so it works on any demo project, including one made
with --vst3-uid, which already has a chain:
- the plug-ins go on `--track` (default 1), which must exist;
- the track's chain is reused, or a new one is made with a free id;
- a plug-in already on that chain is left alone, so running the same command
  twice changes nothing;
- plug-in rows are shared by `(format, uid)`, as the schema's unique index
  requires, and foreign keys are enforced.
"""
import argparse
import sqlite3
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("adi", help="a demo project from make_demo_project.py")
    ap.add_argument("chain", nargs="+", metavar="ID=NAME", help="CLAP plug-ins, in chain order")
    ap.add_argument("--track", type=int, default=1, help="the track to add them to (default 1)")
    args = ap.parse_args()

    db = sqlite3.connect(args.adi)
    db.execute("PRAGMA foreign_keys = ON")
    if db.execute("SELECT 1 FROM tracks WHERE id = ?", (args.track,)).fetchone() is None:
        sys.exit(f"{args.adi}: there is no track {args.track}")

    row = db.execute("SELECT id FROM device_chains WHERE track_id = ? ORDER BY ord, id LIMIT 1",
                     (args.track,)).fetchone()
    if row:
        chain = row[0]
    else:
        chain = db.execute("INSERT INTO device_chains(track_id, ord, name) VALUES (?, 0, '')",
                           (args.track,)).lastrowid
    next_ord = db.execute("SELECT COALESCE(MAX(ord) + 1, 0) FROM devices WHERE chain_id = ?",
                          (chain,)).fetchone()[0]

    added, kept = [], []
    for spec in args.chain:
        uid, _, name = spec.partition("=")
        name = name or uid
        ref = db.execute("SELECT id FROM plugin_refs WHERE format = 'clap' AND uid = ? AND shell_id IS NULL",
                         (uid,)).fetchone()
        if ref:
            ref_id = ref[0]
        else:
            subtype = "instrument" if uid.endswith("onepingonly") else "effect"
            ref_id = db.execute("INSERT INTO plugin_refs(format, uid, vendor, name, version, subtype)"
                                " VALUES ('clap', ?, '', ?, '', ?)", (uid, name, subtype)).lastrowid
        if db.execute("SELECT 1 FROM devices WHERE chain_id = ? AND plugin_ref_id = ?",
                      (chain, ref_id)).fetchone():
            kept.append(name)
            continue
        db.execute("INSERT INTO devices(chain_id, ord, plugin_ref_id, name, enabled) VALUES (?, ?, ?, ?, 1)",
                   (chain, next_ord, ref_id, name))
        next_ord += 1
        added.append(name)
    db.commit()

    names = [r[0] for r in db.execute("SELECT name FROM devices WHERE chain_id = ? ORDER BY ord", (chain,))]
    print(f"track {args.track}, chain {chain}: {', '.join(names)}")
    if added:
        print("added:", ", ".join(added))
    if kept:
        print("already there, left alone:", ", ".join(kept))


if __name__ == "__main__":
    main()
