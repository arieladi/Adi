#!/usr/bin/env python3
"""
Validate the RekordBox .sdPlugin before installing.

Checks: manifest well-formed + every referenced file exists (CodePath, PI,
icons + @2x, encoder layouts), layouts parse as JSON, the vendored MIDI stack
is present with the right prebuilds, the bundle exists, runtime key images
referenced by src/plugin.js exist, and manifest action UUIDs match src.

Run:  python3 scripts/validate.py   (exit 0 = OK)
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.normpath(os.path.join(HERE, ".."))
problems = []


def need(rel, why):
    if not os.path.exists(os.path.join(PLUGIN, rel)):
        problems.append("missing %-52s (%s)" % (rel, why))


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
    if m.get("Icon"):
        need_icon(m["Icon"], "plugin Icon")
    if m.get("CategoryIcon"):
        need_icon(m["CategoryIcon"], "CategoryIcon")

    plats = sorted(o.get("Platform") for o in m.get("OS", []))
    if plats != ["mac", "windows"]:
        problems.append("OS should list mac + windows; found %s" % plats)
    if not m.get("Nodejs", {}).get("Version"):
        problems.append("Nodejs.Version missing (this is a Node plugin)")

    manifest_uuids = set()
    for a in m.get("Actions", []):
        nm = a.get("UUID", "?")
        manifest_uuids.add(nm)
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
            lp = os.path.join(PLUGIN, enc["layout"])
            if os.path.exists(lp):
                try:
                    json.load(open(lp, encoding="utf-8"))
                except Exception as e:
                    problems.append("layout %s invalid JSON: %s" % (enc["layout"], e))

    # src <-> manifest UUID consistency
    src = open(os.path.join(PLUGIN, "src", "plugin.js"), encoding="utf-8").read()
    src_uuids = set(re.findall(r'"(com\.adiariel\.rekordbox\.[a-z]+)"', src))
    for u in sorted(src_uuids - manifest_uuids):
        problems.append("src uses UUID %s not in manifest" % u)
    for u in sorted(manifest_uuids - src_uuids):
        problems.append("manifest action %s never referenced in src" % u)

    # runtime key images referenced by src
    for img in sorted(set(re.findall(r'"(imgs/keys/[a-z_]+\.png)"', src))):
        need(img, "runtime setImage")

    # vendored MIDI stack (committed; end users must not need npm)
    for rel in (
        "vendor/_resolve_.cjs",
        "vendor/node_modules/easymidi/index.js",
        "vendor/node_modules/easymidi/package.json",
        "vendor/node_modules/@julusian/midi/midi.js",
        "vendor/node_modules/@julusian/midi/binding-options.js",
        "vendor/node_modules/pkg-prebuilds/bindings.js",
    ):
        need(rel, "vendored MIDI stack")
    pb = os.path.join(PLUGIN, "vendor", "node_modules", "@julusian", "midi", "prebuilds")
    for plat in ("midi-darwin-arm64", "midi-darwin-x64", "midi-win32-x64", "midi-win32-arm64"):
        if not os.path.isdir(os.path.join(pb, plat)):
            problems.append("missing prebuild %s (mac/Windows out-of-box rule)" % plat)

    # Declared profiles: a .streamDeckProfile is binary and (per repo
    # convention, same as the console plugin) is NOT committed — the user
    # exports it once per README. Warn loudly, but don't fail.
    warnings = []
    for p in m.get("Profiles", []):
        rel = p.get("Name", "") + ".streamDeckProfile"
        if not os.path.exists(os.path.join(PLUGIN, rel)):
            warnings.append(
                "%s not present — the launcher key will log an error until you "
                "export the profile (README: 'One-time profile export')" % rel)

    if problems:
        print("VALIDATION FAILED (%d):" % len(problems))
        for p in problems:
            print("  -", p)
        return 1
    for w in warnings:
        print("WARN:", w)
    print("OK — manifest valid, assets + vendored MIDI stack present.")
    print("   name: %s v%s | actions: %s" % (
        m.get("Name"), m.get("Version"),
        ", ".join(a.get("UUID", "?").split(".")[-1] for a in m.get("Actions", []))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
