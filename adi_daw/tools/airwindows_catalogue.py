#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Airwindows: what we keep, what we strip, and where auto gain goes (ADR-0166).

Reads an airwin2rack checkout (github.com/baconpaul/airwin2rack, MIT; the
registry Airwindows Consolidated is built from) and writes three files:

  plugins/airwindows/CATALOGUE.md        every effect: what it does, kept or why not
  plugins/airwindows/Source/aw_catalogue.inc   the kept effects, as a C++ table
  plugins/airwindows/effects.cmake       the kept effects' names, for the build

    python tools/airwindows_catalogue.py <airwin2rack checkout>

Run it again after moving the airwin2rack pin (tools/fetch_plugins.sh); the
three outputs are committed so a build needs only the checkout, not Python.
"""
import argparse
import collections
import pathlib
import re
import subprocess
import sys

# The keep list is Chris Johnson's own: the effects he files under Recommended
# (his Airwindopedia picks, one current version per job) and Basic (his
# starter set). Ours adds back only what those leave a gap for.
KEEP_COLLECTIONS = {"Recommended", "Basic"}
ADD_BACK = {
    "Density3": "the general saturator; the kept Saturation set is only tube flavours",
    "Pressure5": "the flagship compressor, vari-mu to peak, with output and a built-in clipper",
    "Channel9": "console colour in one insert; the kept console systems need a channel/buss pair",
}

# Auto gain (ADR-0166) is a parameter on every effect. It starts ON only where
# the effect changes level as a side effect of its tone job AND has no output
# control of its own. Where changing level IS the job -- EQ, filters,
# compressors, clippers, reverbs, utilities, consoles -- it starts OFF.
AUTO_GAIN_CATEGORIES = {"Saturation", "Distortion", "Tape", "Amp Sims", "Tone Color",
                        "Lo-Fi", "Subtlety"}
AUTO_GAIN_OFF = {
    "Mastering2": "a mastering chain: its Drive is there to make the mix louder",
}
OUTPUT_CONTROL = re.compile(r"^(out\b.*|output.*|outpt|vol|volume|fader|master|makeup|trim)$",
                            re.IGNORECASE)

REGISTER = re.compile(
    r'registerAirwindow\(\{"([^"]+)", "([^"]+)", (-?\d+), (true|false), "((?:[^"\\]|\\.)*)", '
    r'[^,]+, "([^"]*)", \[\]\(\) \{[^}]*\}, -?\d+, \{([^}]*)\}\}\);')
PARAM_NAMES = re.compile(r'getParameterName\(.*?\{(.*?)\n\}', re.S)
PARAM_NAME = re.compile(r'vst_strncpy\s*\(\s*text\s*,\s*"([^"]*)"')


def load(source):
    src = source / "src"
    text = (src / "ModuleAdd.h").read_text(encoding="utf-8")
    effects = []
    for m in REGISTER.finditer(text):
        name, cat, order, _mono, desc, date, colls = m.groups()
        body = (src / "autogen_airwin" / f"{name}.cpp").read_text(encoding="utf-8")
        pm = PARAM_NAMES.search(body)
        params = [p.strip() for p in PARAM_NAME.findall(pm.group(1))] if pm else []
        effects.append({
            "name": name, "category": cat, "order": int(order), "date": date,
            "desc": desc.replace('\\"', '"'), "params": params,
            "collections": [c.strip().strip('"') for c in colls.split(",") if c.strip()],
        })
    found = text.count("registerAirwindow(")
    if len(effects) != found:
        sys.exit(f"parsed {len(effects)} of {found} registry entries: the ModuleAdd.h format moved")
    return effects


def family(name):
    """Density3 -> Density; kCathedral5 -> kCathedral. Versions share a family."""
    return re.sub(r"\d+$", "", name)


def version(name):
    """Density3 -> 3; Density -> 1."""
    m = re.search(r"(\d+)$", name)
    return int(m.group(1)) if m else 1


def verdicts(effects):
    kept = {e["name"] for e in effects
            if set(e["collections"]) & KEEP_COLLECTIONS or e["name"] in ADD_BACK}
    missing = set(ADD_BACK) - {e["name"] for e in effects}
    if missing:
        sys.exit(f"ADD_BACK names not in the registry: {sorted(missing)}")
    kept_families = collections.defaultdict(list)
    for e in effects:
        if e["name"] in kept:
            kept_families[family(e["name"])].append(e["name"])
    for e in effects:
        e["has_output"] = any(OUTPUT_CONTROL.match(p) for p in e["params"])
        e["auto_gain"] = (e["category"] in AUTO_GAIN_CATEGORIES and not e["has_output"]
                          and e["name"] not in AUTO_GAIN_OFF)
        if e["name"] in ADD_BACK:
            e["keep"], e["why"] = True, "ADI add-back: " + ADD_BACK[e["name"]]
        elif e["name"] in kept:
            e["keep"], e["why"] = True, "/".join(c for c in e["collections"] if c in KEEP_COLLECTIONS)
        else:
            e["keep"] = False
            siblings = [n for n in kept_families.get(family(e["name"]), []) if n != e["name"]]
            newer = [n for n in siblings if version(n) > version(e["name"])]
            if not e["desc"]:
                e["why"] = "unfinished upstream: no description, and Consolidated does not register it"
            elif newer:
                e["why"] = "superseded by " + ", ".join(newer)
            elif siblings:
                who = "ADI keeps" if all(n in ADD_BACK for n in siblings) else "Chris recommends"
                e["why"] = f"a later version; {who} " + ", ".join(siblings) + " over it"
            elif e["category"] == "Unclassified":
                e["why"] = "unclassified experiment"
            elif e["category"] == "Dithers":
                e["why"] = "dither: export dithers once, at the end (TPDFDither, PaulDither, PaulWide kept)"
            elif e["category"] == "Consoles":
                e["why"] = "console system not kept (Console9, Console0, LA, MC and Purest are)"
            else:
                e["why"] = "not among Chris's Recommended or Basic picks"
    return effects


def ascii(s):
    table = {"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-",
             "—": "-", "…": "..."}
    s = "".join(table.get(ch, ch) for ch in s)
    return s.encode("ascii", "ignore").decode("ascii")


def c_string(s):
    return '"' + ascii(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_catalogue(effects, out, pin):
    kept = [e for e in effects if e["keep"]]
    by_cat = collections.defaultdict(list)
    for e in effects:
        by_cat[e["category"]].append(e)
    lines = [
        "# Airwindows: the catalogue",
        "",
        "<!-- Generated by tools/airwindows_catalogue.py; edit that, not this. -->",
        "",
        f"All {len(effects)} effects in airwin2rack {pin}, the registry Airwindows "
        f"Consolidated is built from. **{len(kept)} are kept** and ship as separate CLAP "
        "plug-ins in `ADI Airwindows.clap` (ADR-0166); the rest are listed with the reason "
        "they are not. Descriptions are Chris Johnson's own (MIT, airwindows.com).",
        "",
        "The keep list is Chris's **Recommended** and **Basic** collections, plus three "
        "add-backs. **Auto gain** is a parameter on every kept effect; *on* marks where it "
        "starts on: a tone effect whose level moves with its tone and which has no output "
        "control of its own.",
        "",
        "| Category | Kept | Of |",
        "|---|---:|---:|",
    ]
    for cat in sorted(by_cat):
        n = sum(1 for e in by_cat[cat] if e["keep"])
        lines.append(f"| {cat} | {n} | {len(by_cat[cat])} |")
    lines.append(f"| **All** | **{len(kept)}** | **{len(effects)}** |")
    for cat in sorted(by_cat):
        rows = sorted(by_cat[cat], key=lambda e: (not e["keep"], e["order"]))
        lines += ["", f"## {cat}", "", "| Effect | What it does | Controls | Kept | Auto gain |",
                  "|---|---|---|---|---|"]
        for e in rows:
            desc = e["desc"].replace("|", "\\|")
            params = ", ".join(e["params"]) if e["params"] else "(none)"
            keep = "**yes** (" + e["why"] + ")" if e["keep"] else "no: " + e["why"]
            ag = ("on" if e["auto_gain"] else "off") if e["keep"] else ""
            lines.append(f"| {e['name']} | {desc} | {params} | {keep} | {ag} |")
    (out / "CATALOGUE.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def write_table(effects, out, pin):
    kept = sorted((e for e in effects if e["keep"]), key=lambda e: (e["category"], e["order"]))
    inc = [
        f"// Generated by tools/airwindows_catalogue.py from airwin2rack {pin}. Do not edit.",
        "// The kept effects (ADR-0166): the includes, then one row each.",
        "// Row: name, category, description, auto gain on by default, factory.",
        "#ifndef ADI_AW_ROWS",
    ]
    inc += [f'#include "autogen_airwin/{e["name"]}.h"' for e in kept]
    inc += ["#else"]
    for e in kept:
        n = e["name"]
        inc.append(f"ADI_AW_ROW({n}, {c_string(n)}, {c_string(e['category'])}, "
                   f"{c_string(e['desc'])}, {'true' if e['auto_gain'] else 'false'})")
    inc += ["#endif", ""]
    (out / "Source" / "aw_catalogue.inc").write_text("\n".join(inc), encoding="ascii", newline="\n")
    cm = [f"# Generated by tools/airwindows_catalogue.py from airwin2rack {pin}. Do not edit.",
          "set(ADI_AW_EFFECTS"]
    cm += [f"    {e['name']}" for e in kept]
    cm += [")", ""]
    (out / "effects.cmake").write_text("\n".join(cm), encoding="ascii", newline="\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source", type=pathlib.Path, help="an airwin2rack checkout")
    ap.add_argument("--out", type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parent.parent / "plugins" / "airwindows")
    ap.add_argument("--pin", default=None, help="the commit, for the headers (default: read it)")
    args = ap.parse_args()
    pin = args.pin
    if pin is None:
        r = subprocess.run(["git", "-C", str(args.source), "rev-parse", "--short=10", "HEAD"],
                           capture_output=True, text=True)
        pin = r.stdout.strip() if r.returncode == 0 else "(unknown commit)"
    effects = verdicts(load(args.source))
    (args.out / "Source").mkdir(parents=True, exist_ok=True)
    write_catalogue(effects, args.out, pin)
    write_table(effects, args.out, pin)
    kept = [e for e in effects if e["keep"]]
    print(f"{len(effects)} effects, {len(kept)} kept, "
          f"{sum(e['auto_gain'] for e in kept)} with auto gain on by default")


if __name__ == "__main__":
    main()
