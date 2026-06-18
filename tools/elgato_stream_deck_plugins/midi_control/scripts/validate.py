#!/usr/bin/env python3
"""
Validate the MIDI Control .sdPlugin before installing.

Checks manifest.json is well-formed and that every referenced file exists
(CodePath, PropertyInspectorPath, icons + @2x, action images, encoder layouts).
NOTE: this validates the Stream Deck plugin only — the native MIDI helper
(main.cpp / CMake) must be built separately per OS; see README.md.

Run:  python3 scripts/validate.py   (exit 0 = OK)
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.join(HERE, "..", "com.adiariel.midicontrol.sdPlugin")
problems = []


def need(rel, why):
    if not os.path.exists(os.path.join(PLUGIN, rel)):
        problems.append("missing %-40s (%s)" % (rel, why))


def need_icon(ref, why):
    need(ref + ".png", why)
    need(ref + "@2x.png", why + " @2x")


def main():
    try:
        m = json.load(open(os.path.join(PLUGIN, "manifest.json"), encoding="utf-8"))
    except Exception as e:
        print("FATAL: manifest.json invalid:", e)
        return 1

    need(m.get("CodePath", ""), "CodePath")
    if m.get("PropertyInspectorPath"):
        need(m["PropertyInspectorPath"], "PI")
    if m.get("Icon"):
        need_icon(m["Icon"], "plugin Icon")
    if m.get("CategoryIcon"):
        need_icon(m["CategoryIcon"], "CategoryIcon")

    plats = sorted(o.get("Platform") for o in m.get("OS", []))
    if plats != ["mac", "windows"]:
        problems.append("OS should list mac + windows; found %s" % plats)

    for a in m.get("Actions", []):
        nm = a.get("UUID", "?")
        if a.get("Icon"):
            need_icon(a["Icon"], "action %s icon" % nm)
        if a.get("PropertyInspectorPath"):
            need(a["PropertyInspectorPath"], "action %s PI" % nm)
        for i, st in enumerate(a.get("States", [])):
            if st.get("Image"):
                need_icon(st["Image"], "action %s state %d" % (nm, i))
        enc = a.get("Encoder")
        if enc and enc.get("layout") and not enc["layout"].startswith("$"):
            need(enc["layout"], "action %s encoder layout" % nm)

    if problems:
        print("VALIDATION FAILED (%d):" % len(problems))
        for p in problems:
            print("  -", p)
        return 1
    print("OK — manifest valid and all referenced plugin assets present.")
    print("   (Build the native helper separately — see README.md.)")
    print("   name: %s v%s | actions: %s" % (
        m.get("Name"), m.get("Version"),
        ", ".join(a.get("UUID", "?").split(".")[-1] for a in m.get("Actions", []))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
