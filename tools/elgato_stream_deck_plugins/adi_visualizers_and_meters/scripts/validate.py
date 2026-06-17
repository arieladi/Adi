#!/usr/bin/env python3
"""
Validate the .sdPlugin folder before packaging/installing.

Checks that manifest.json is well-formed and that every file it references
(CodePath, PropertyInspectorPath, icons with @2x, encoder layout, action images)
actually exists on disk. Stream Deck silently ignores a plugin whose manifest
points at missing files, so this catches the common mistakes early.

Run:  python3 scripts/validate.py   (exit code 0 = OK, 1 = problems found)
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
PLUGIN = os.path.join(ROOT, "com.adi.visualizers-and-meters.sdPlugin")

problems = []


def require(relpath, why):
    p = os.path.join(PLUGIN, relpath)
    if not os.path.exists(p):
        problems.append("missing %-40s (%s)" % (relpath, why))


def require_icon(ref, why):
    # Stream Deck icon refs omit the extension and use name.png + name@2x.png
    require(ref + ".png", why)
    require(ref + "@2x.png", why + " @2x")


def main():
    mpath = os.path.join(PLUGIN, "manifest.json")
    if not os.path.exists(mpath):
        print("FATAL: manifest.json not found at", mpath)
        return 1
    try:
        with open(mpath, "r", encoding="utf-8") as f:
            m = json.load(f)
    except Exception as e:
        print("FATAL: manifest.json is not valid JSON:", e)
        return 1

    for key in ("Name", "Version", "Author", "Category", "CodePath", "SDKVersion", "OS", "Actions"):
        if key not in m:
            problems.append("manifest missing required key: " + key)

    require(m.get("CodePath", ""), "CodePath")
    if m.get("PropertyInspectorPath"):
        require(m["PropertyInspectorPath"], "PropertyInspectorPath")
    if m.get("Icon"):
        require_icon(m["Icon"], "plugin Icon")
    if m.get("CategoryIcon"):
        require_icon(m["CategoryIcon"], "CategoryIcon")

    plats = sorted([o.get("Platform") for o in m.get("OS", [])])
    if plats != ["mac", "windows"]:
        problems.append("OS should list both mac and windows; found: %s" % plats)

    for a in m.get("Actions", []):
        name = a.get("UUID", a.get("Name", "?"))
        if a.get("Icon"):
            require_icon(a["Icon"], "action %s Icon" % name)
        if a.get("PropertyInspectorPath"):
            require(a["PropertyInspectorPath"], "action %s PI" % name)
        for i, st in enumerate(a.get("States", [])):
            if st.get("Image"):
                require_icon(st["Image"], "action %s state %d image" % (name, i))
        enc = a.get("Encoder")
        if enc and enc.get("layout") and not enc["layout"].startswith("$"):
            require(enc["layout"], "action %s encoder layout" % name)

    # JS/HTML the host + PI load
    for f in ("js/engine.js", "js/plugin.js", "pi/inspector.js", "pi/sdpi.css"):
        require(f, "source file")

    if problems:
        print("VALIDATION FAILED (%d problem(s)):" % len(problems))
        for p in problems:
            print("  -", p)
        return 1
    print("OK — manifest valid and all referenced assets present.")
    print("   plugin:  %s" % os.path.relpath(PLUGIN, ROOT))
    print("   name:    %s  v%s" % (m.get("Name"), m.get("Version")))
    print("   actions: %s" % ", ".join(a.get("UUID", "?") for a in m.get("Actions", [])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
