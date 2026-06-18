#!/usr/bin/env python3
"""
Validate the plugin + Remote Script before installing/packaging.

Checks manifest.json well-formedness, that every referenced asset exists
(CodePath, PI, icons + @2x, encoder layout, action images), that the JS modules
and the AdiVST Remote Script files are present, and compiles the Python.

Run:  python3 scripts/validate.py   (exit 0 = OK)
"""
import json
import os
import py_compile
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
PLUGIN = os.path.join(ROOT, "com.adiariel.ableton-vst.sdPlugin")
RS = os.path.join(ROOT, "ableton", "remote_script", "AdiVST")

problems = []


def need(base, rel, why):
    if not os.path.exists(os.path.join(base, rel)):
        problems.append("missing %-44s (%s)" % (rel, why))


def need_icon(rel, why):
    need(PLUGIN, rel + ".png", why)
    need(PLUGIN, rel + "@2x.png", why + " @2x")


def main():
    mpath = os.path.join(PLUGIN, "manifest.json")
    try:
        m = json.load(open(mpath, encoding="utf-8"))
    except Exception as e:
        print("FATAL: manifest.json invalid:", e)
        return 1

    for k in ("Name", "Version", "CodePath", "SDKVersion", "OS", "Actions"):
        if k not in m:
            problems.append("manifest missing key: " + k)
    need(PLUGIN, m.get("CodePath", ""), "CodePath")
    if m.get("PropertyInspectorPath"):
        need(PLUGIN, m["PropertyInspectorPath"], "PI")
    for key in ("Icon", "CategoryIcon"):
        if m.get(key):
            need_icon(m[key], key)

    plats = sorted(o.get("Platform") for o in m.get("OS", []))
    if plats != ["mac", "windows"]:
        problems.append("OS should be mac+windows; got %s" % plats)

    for a in m.get("Actions", []):
        nm = a.get("UUID", "?")
        if a.get("Icon"):
            need_icon(a["Icon"], "action %s icon" % nm)
        for i, st in enumerate(a.get("States", [])):
            if st.get("Image"):
                need_icon(st["Image"], "action %s state %d" % (nm, i))
        enc = a.get("Encoder")
        if enc and enc.get("layout") and not enc["layout"].startswith("$"):
            need(PLUGIN, enc["layout"], "encoder layout")

    js = ["js/sd-client.js", "js/bridge.js", "js/touchscreen.js", "js/keys.js", "js/plugin.js",
          "js/controllers/DeviceController.js", "js/controllers/GenericController.js",
          "js/controllers/EQ8Controller.js", "js/controllers/PulsarMassiveController.js",
          "js/controllers/registry.js", "pi/inspector.js", "pi/sdpi.css"]
    for f in js:
        need(PLUGIN, f, "js/pi source")

    # Remote Script present + compiles
    for f in ("__init__.py", "AdiVST.py", "ws_server.py", "live_bridge.py"):
        need(RS, f, "remote script")
        p = os.path.join(RS, f)
        if os.path.exists(p):
            try:
                py_compile.compile(p, doraise=True)
            except py_compile.PyCompileError as e:
                problems.append("python compile failed %s: %s" % (f, e))

    if problems:
        print("VALIDATION FAILED (%d):" % len(problems))
        for p in problems:
            print("  -", p)
        return 1
    print("OK — manifest valid, assets present, JS modules present, Remote Script compiles.")
    print("   name:    %s v%s" % (m.get("Name"), m.get("Version")))
    print("   actions: %s" % ", ".join(a.get("UUID") for a in m.get("Actions", [])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
