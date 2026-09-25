#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Put a chain of CLAP plug-ins on track 1 of a demo project, for an offline render.

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
"""
import argparse
import sqlite3


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("adi", help="a demo project from make_demo_project.py")
    ap.add_argument("chain", nargs="+", metavar="ID=NAME", help="CLAP plug-ins, in chain order")
    args = ap.parse_args()
    db = sqlite3.connect(args.adi)
    db.execute("INSERT INTO device_chains(id, track_id, ord, name) VALUES (1, 1, 0, '')")
    for i, spec in enumerate(args.chain, start=1):
        uid, _, name = spec.partition("=")
        name = name or uid
        subtype = "instrument" if uid.endswith("onepingonly") else "effect"
        db.execute("INSERT INTO plugin_refs(id, format, uid, vendor, name, version, subtype)"
                   " VALUES (?, 'clap', ?, '', ?, '', ?)", (i, uid, name, subtype))
        db.execute("INSERT INTO devices(id, chain_id, ord, plugin_ref_id, name, enabled)"
                   " VALUES (?, 1, ?, ?, ?, 1)", (i, i - 1, i, name))
    db.commit()
    print("chain on track 1:", ", ".join(s.partition("=")[2] or s for s in args.chain))


if __name__ == "__main__":
    main()
