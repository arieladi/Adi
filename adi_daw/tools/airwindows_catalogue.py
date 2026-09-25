#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Airwindows: what we keep, what we strip, and where auto gain goes (ADR-0170).

Reads an airwin2rack checkout (github.com/baconpaul/airwin2rack, MIT; the
registry Airwindows Consolidated is built from) and writes three files:

  plugins/airwindows/CATALOGUE.md        every effect: what it does, kept or why not
  plugins/airwindows/Source/aw_catalogue.inc   the kept effects, as a C++ table
  plugins/airwindows/effects.cmake       the kept effects' names, for the build

    python tools/airwindows_catalogue.py <airwin2rack checkout>

Run it again after moving the airwin2rack pin (tools/fetch_plugins.sh); the
three outputs are committed so a build needs only the checkout, not Python.

THE RULES are the director's (2026-09-25, ADR-0170), in this order; the first
that decides an effect wins, and CATALOGUE.md names it for every row:

  0. Unfinished upstream (no description) is out: Consolidated skips it too.
  5. The six "secret weapons" are in, whatever else says.
  1. EQs and filters, dithers, utilities and tests, and compressors are out.
  3. Noise reduction and de-essing: the named set, at most ten.
  4. Stereo and imaging: the named set, at most three.
  2. Saturation, distortion, tape and consoles -- and the colour categories
     that are those under other names -- are ALL in, newest of each family.
  -. Anything the rules do not mention keeps Chris Johnson's own picks
     (his Recommended and Basic collections).
"""
import argparse
import collections
import pathlib
import re
import subprocess
import sys

# --- the director's rules (ADR-0170) ------------------------------------------

# Rule 5: in regardless. StarChild is named; StarChild2 (2023) is the same
# effect adapted to high sample rates, and is NOT substituted without a ruling.
SECRET_WEAPONS = ["Melt", "TapeDust", "GrooveWear", "StarChild2", "Vibrato", "NonlinearSpace"]


# Rule 1: out.
DROP_CATEGORIES = {
    "Filter": "an EQ or filter",
    "XYZ Filters": "an EQ or filter",
    "Biquads": "an EQ or filter",
    "Dithers": "a dither",
    "Utility": "a utility",
    "Unclassified": "a test or experiment",
    "Dynamics": "a compressor",
}
# EQs and filters filed under other categories, by what they do.
DROP_NAMES = {
    "Air4": "an EQ (air band)",
    "Energy2": "an EQ (fixed treble boosts)",
    "Elliptical": "a filter (side-channel high-pass)",
    "PhaseNudge": "a filter (all-pass phase rotator)",
    "SlewSonic": "a utility (solos the brightness, like SlewOnly)",
}

# Rule 3: noise reduction and de-essing, at most ten. VoiceTrick is the
# director's example although Airwindows files it as a utility (it cancels
# speaker bleed while recording vocals); DeCrackle is also filed as a utility.
# The three gates are here rather than under rule 1: a gate is not a
# compressor, and two of them are sold as hiss and tail cleaners. DeEss is
# left out because DeBess is its improved version.
NOISE_REDUCTION = ["DeBess", "DeHiss", "DeNoise", "DeCrackle", "VoiceTrick", "Slew2",
                   "SoftGate", "Gatelope", "DigitalBlack", "AQuickVoiceClip"]
NOISE_REDUCTION_CAP = 10

# Rule 4: stereo and imaging, at most three. The director's examples were
# Wider and Srsly2; Srsly3 is Srsly2 with a Nonlin control. ToVinyl4 is the
# third of Chris's own Stereo picks.
STEREO = ["Wider", "Srsly3", "ToVinyl4"]
STEREO_CAP = 3

# The suites (ADR-0171, ADR-0173): the kept algorithms ship as eleven CLAP plug-ins, one
# per category group, the algorithm chosen inside, each named
# "ADI Airwindows - <group>" (director). A secret weapon is also in its suite.
SUITES = [
    ("distortion", "ADI Airwindows - Distortion", {"Distortion", "Saturation", "Subtlety", "Clipping"}),
    ("tape", "ADI Airwindows - Tape", {"Tape"}),
    ("ampsims", "ADI Airwindows - Amp Sims", {"Amp Sims"}),
    ("reverb", "ADI Airwindows - Reverb", {"Reverb"}),
    ("lofimod", "ADI Airwindows - Lo-Fi & Mod", {"Lo-Fi", "Effects"}),
    ("noisedyn", "ADI Airwindows - Noise & Dynamics", {"Brightness", "Noise", "Dynamics", "Utility"}),
    ("secret", "ADI Airwindows - Secret Weapons", SECRET_WEAPONS),
    ("delay", "ADI Airwindows - Delay", ["ClearCoat", "TapeDelay2", "PitchDelay", "TripleSpread"]),
    ("stereo", "ADI Airwindows - Stereo", STEREO),
    ("sub", "ADI Airwindows - Sub", {"Bass"}),
    ("color", "ADI Airwindows - Color", {"Tone Color"}),
]

# Rule 2: every family in these categories, newest version only. Saturation,
# Distortion, Tape and Consoles are named; Tone Color (console and channel
# colour), Subtlety (subtle saturation), Amp Sims (amp distortion), Clipping
# (clippers) and Lo-Fi (crushers and wear) are the same job under other names.
FLAVOUR_CATEGORIES = {"Saturation", "Distortion", "Tape", "Consoles", "Tone Color",
                      "Subtlety", "Amp Sims", "Clipping", "Lo-Fi"}
# Variants that are a version of another family, not a family of their own.
FAMILY_OF = {
    "FinalClip": "ADClip",           # one stage of ADClip8, set up for Final Cut Pro
}
# A console is a SYSTEM -- a channel, a buss and sometimes a submix -- and a
# version is the whole system. Console0 to Console9 are one family and
# PurestConsole to PurestConsole3 another, and their numbers are not their
# age: Console6 (2024) is newer than Console7 (2022). So "newest" is the date.
CONSOLE_SYSTEM = re.compile(
    r"^(Console|PurestConsole)(\d*)(Lite)?(Channel|Buss|Sub|Cascade|Crunch|DarkCh)(In|Out|Hype)?$")

# Everything else: Chris Johnson's own picks.
KEEP_COLLECTIONS = {"Recommended", "Basic"}

# --- auto gain (ADR-0166) -----------------------------------------------------

# A parameter on every effect. It starts ON only where the effect changes level
# as a side effect of its tone job AND has no output control of its own. Where
# changing level IS the job -- clippers, consoles, reverbs, utilities -- it
# starts OFF.
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


def family_and_version(name):
    """Density3 -> (Density, 3); Console9Channel -> (Console system, 9);
    Console8LiteBuss -> (Console system, 8)."""
    m = CONSOLE_SYSTEM.match(name)
    if m:
        return m.group(1) + " system", int(m.group(2)) if m.group(2) else 1
    if name in FAMILY_OF:
        return FAMILY_OF[name], 0
    digits = re.findall(r"\d+", name)
    return re.sub(r"\d+", "", name), int(digits[-1]) if digits else 1


def newest_versions(effects):
    """For each family in the flavour categories, the version that is newest by
    date (ties: the higher number), and every member of that version."""
    families = collections.defaultdict(list)
    for e in effects:
        if e["category"] in FLAVOUR_CATEGORIES and e["desc"]:
            families[family_and_version(e["name"])[0]].append(e)
    keep, beaten_by = set(), {}
    for fam, members in families.items():
        by_version = collections.defaultdict(list)
        for e in members:
            by_version[family_and_version(e["name"])[1]].append(e)
        best = max(by_version, key=lambda v: (max(e["date"] for e in by_version[v]), v))
        winners = sorted(e["name"] for e in by_version[best])
        for e in members:
            if e["name"] in winners:
                keep.add(e["name"])
            else:
                beaten_by[e["name"]] = winners
    return keep, beaten_by


def verdicts(effects):
    names = {e["name"] for e in effects}
    for listed in (SECRET_WEAPONS, NOISE_REDUCTION, STEREO, list(DROP_NAMES), list(FAMILY_OF)):
        missing = set(listed) - names
        if missing:
            sys.exit(f"named in the rules but not in the registry: {sorted(missing)}")
    if len(NOISE_REDUCTION) > NOISE_REDUCTION_CAP or len(STEREO) > STEREO_CAP:
        sys.exit("a capped rule lists more effects than its cap")
    flavour_keep, beaten_by = newest_versions(effects)
    for e in effects:
        e["has_output"] = any(OUTPUT_CONTROL.match(p) for p in e["params"])
        e["auto_gain"] = (e["category"] in AUTO_GAIN_CATEGORIES and not e["has_output"]
                          and e["name"] not in AUTO_GAIN_OFF)
        n, cat = e["name"], e["category"]
        if not e["desc"]:
            e["keep"], e["why"] = False, "unfinished upstream: no description, and Consolidated does not register it"
        elif n in SECRET_WEAPONS:
            e["keep"], e["why"] = True, "rule 5, a secret weapon"
        elif n in NOISE_REDUCTION:
            e["keep"], e["why"] = True, "rule 3, noise reduction and de-essing"
        elif n in STEREO:
            e["keep"], e["why"] = True, "rule 4, stereo and imaging"
        elif n in DROP_NAMES:
            e["keep"], e["why"] = False, "rule 1, " + DROP_NAMES[n]
        elif cat in DROP_CATEGORIES:
            e["keep"], e["why"] = False, "rule 1, " + DROP_CATEGORIES[cat]
        elif cat == "Stereo":
            e["keep"], e["why"] = False, "rule 4, past the three kept (" + ", ".join(STEREO) + ")"
        elif cat in FLAVOUR_CATEGORIES:
            if n in flavour_keep:
                e["keep"], e["why"] = True, "rule 2, the newest of its family"
            else:
                e["keep"], e["why"] = False, "rule 2, superseded by " + ", ".join(beaten_by[n])
        elif set(e["collections"]) & KEEP_COLLECTIONS:
            e["keep"], e["why"] = True, "Chris's pick (" + "/".join(
                c for c in e["collections"] if c in KEEP_COLLECTIONS) + ")"
        else:
            e["keep"], e["why"] = False, "not among Chris's picks, and no rule keeps it"
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
    in_suite = collections.defaultdict(list)
    for i, e in suite_rows(effects):
        in_suite[e["name"]].append(SUITES[i][1].replace("ADI Airwindows - ", ""))
    for e in kept:
        if e["category"] in SUMMING_CATEGORIES:
            in_suite[e["name"]].append("the mixer's group summing")
    by_cat = collections.defaultdict(list)
    for e in effects:
        by_cat[e["category"]].append(e)
    lines = [
        "# Airwindows: the catalogue",
        "",
        "<!-- Generated by tools/airwindows_catalogue.py; edit that, not this. -->",
        "",
        f"All {len(effects)} effects in airwin2rack {pin}, the registry Airwindows "
        f"Consolidated is built from. **{len(kept)} are kept**, as the algorithms of eleven "
        "suite CLAP plug-ins, \"ADI Airwindows - <group>\" (ADR-0171), and of the mixer's group "
        "summing (ADR-0173); the rest are listed "
        "with the reason they are not. Descriptions are Chris Johnson's own (MIT, airwindows.com).",
        "",
        "The selection is the director's five rules (ADR-0170): no EQs, filters, dithers, "
        "utilities or compressors; every saturation, distortion, tape and console family, "
        "newest version only; at most ten noise-reduction and de-essing tools and three "
        "stereo tools; six named secret weapons. What the rules do not mention keeps "
        "Chris Johnson's own picks. Every row names the rule that decided it.",
        "",
        "**Auto gain** is a parameter on every kept effect; *on* marks where it starts on: "
        "a tone effect whose level moves with its tone and which has no output control of "
        "its own.",
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
        lines += ["", f"## {cat}", "", "| Effect | What it does | Controls | Kept | Suite | Auto gain |",
                  "|---|---|---|---|---|---|"]
        for e in rows:
            desc = e["desc"].replace("|", "\\|")
            params = ", ".join(e["params"]) if e["params"] else "(none)"
            keep = "**yes** (" + e["why"] + ")" if e["keep"] else "no: " + e["why"]
            ag = ("on" if e["auto_gain"] else "off") if e["keep"] else ""
            suite = ", ".join(in_suite.get(e["name"], []))
            lines.append(f"| {e['name']} | {desc} | {params} | {keep} | {suite} | {ag} |")
    (out / "CATALOGUE.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


# ADR-0173: the console systems (a channel half and a buss half) are not a
# plug-in. They are the mixer's native group summing, compiled into the
# engine. The single-insert colours are the Color suite.
SUMMING_CATEGORIES = {"Consoles"}


def suite_rows(effects):
    """(suite index, effect) for every algorithm in every suite."""
    kept = sorted((e for e in effects if e["keep"]), key=lambda e: (e["category"], e["order"]))
    rows = []
    for i, (_, _, cats) in enumerate(SUITES):
        for e in kept:
            # A set names categories; a list names algorithms.
            if e["name"] in cats if isinstance(cats, list) else e["category"] in cats:
                rows.append((i, e))
    return rows


def write_table(effects, out, pin):
    rows = suite_rows(effects)
    seen = set()
    kept = [e for _, e in rows if not (e["name"] in seen or seen.add(e["name"]))]
    inc = [
        f"// Generated by tools/airwindows_catalogue.py from airwin2rack {pin}. Do not edit.",
        "// The kept effects (ADR-0166, ADR-0170): the includes, then one row each.",
        "// Row: suite, symbol, name, category, description, auto gain on by default.",
        "#ifndef ADI_AW_ROWS",
    ]
    inc += [f'#include "autogen_airwin/{e["name"]}.h"' for e in kept]
    inc += ["#else"]
    # One module per suite (-DADI_AW_ONLY_SUITE=i) links only its own
    # algorithms; without the define every suite is present (the tests).
    for i in range(len(SUITES)):
        inc.append(f"#if !defined(ADI_AW_ONLY_SUITE) || ADI_AW_ONLY_SUITE == {i}")
        for j, e in rows:
            if j == i:
                n = e["name"]
                inc.append(f"ADI_AW_ROW({i}, {n}, {c_string(n)}, {c_string(e['category'])}, "
                           f"{c_string(e['desc'])}, {'true' if e['auto_gain'] else 'false'})")
        inc.append("#endif")
    inc += ["#endif", "#ifdef ADI_AW_SUITE"]
    for i, (key, name, _) in enumerate(SUITES):
        inc.append(f"ADI_AW_SUITE({i}, {c_string(key)}, {c_string(name)})")
    inc += ["#endif", ""]
    (out / "Source" / "aw_catalogue.inc").write_text("\n".join(inc), encoding="ascii", newline="\n")
    cm = [f"# Generated by tools/airwindows_catalogue.py from airwin2rack {pin}. Do not edit.",
          "set(ADI_AW_EFFECTS"]
    cm += [f"    {e['name']}" for e in kept]
    cm += [")", "", "# The suites, one CLAP module each: index, key, and the name a host shows."]
    cm += ["set(ADI_AW_SUITE_KEYS " + " ".join(k for k, _, _ in SUITES) + ")"]
    cm += [f'set(ADI_AW_SUITE_NAME_{k} "{name}")' for k, name, _ in SUITES]
    cm += [""]
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
    orphans = [e["name"] for e in effects if e["keep"] and e["category"] not in SUMMING_CATEGORIES
               and e["name"] not in {x["name"] for _, x in suite_rows(effects)}]
    if orphans:
        print("kept but in no suite (not built):", ", ".join(orphans))
    (args.out / "Source").mkdir(parents=True, exist_ok=True)
    write_catalogue(effects, args.out, pin)
    write_table(effects, args.out, pin)
    kept = [e for e in effects if e["keep"]]
    rules = collections.Counter(e["why"].split(",")[0] for e in kept)
    print(f"{len(effects)} effects, {len(kept)} kept, "
          f"{sum(e['auto_gain'] for e in kept)} with auto gain on by default")
    for why, n in sorted(rules.items()):
        print(f"  {n:4}  {why}")


if __name__ == "__main__":
    main()
