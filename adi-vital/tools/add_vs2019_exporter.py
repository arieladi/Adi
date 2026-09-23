#!/usr/bin/env python3
"""
Add a VS2019 exporter to plugin/vital.jucer and fix the Windows plugin build.

Projucer 6.0.5 has no VS2022 exporter (JUCE 6.0.5 predates VS2022), so we add
VS2019 and override the toolset to v143 on the MSBuild command line -- the same
approach that already works for the standalone target.

Run this once, then `Projucer.exe --resave plugin/vital.jucer`.
"""

import copy
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

JUCER = Path(__file__).resolve().parent.parent / "vital" / "plugin" / "vital.jucer"

# Defines to drop from the Windows exporters, and what to add.
DROP_DEFINES = {"INTEL_IPP=1", "REQUIRE_AUTH=1"}
ADD_DEFINES = ["NO_AUTH=1"]


def fix_defines(exporter):
    raw = exporter.get("extraDefs", "")
    kept = [d for d in raw.split("\n") if d.strip() and d.strip() not in DROP_DEFINES]
    for d in ADD_DEFINES:
        if d not in kept:
            kept.insert(1 if len(kept) > 1 else len(kept), d)
    exporter.set("extraDefs", "\n".join(kept))
    return raw, exporter.get("extraDefs")


def fix_sdk_paths(exporter):
    notes = []
    # JUCE 6 bundles the VST3 SDK inside juce_audio_processors; the vendored
    # path in this .jucer points at a directory that does not exist.
    if exporter.get("vst3Folder"):
        notes.append(f"  vst3Folder: '{exporter.get('vst3Folder')}' -> '' (use JUCE's bundled SDK)")
        exporter.set("vst3Folder", "")
    # No VST2 SDK ships in this repo and Steinberg no longer licenses VST2.
    if exporter.get("vstLegacyFolder"):
        notes.append(f"  vstLegacyFolder: '{exporter.get('vstLegacyFolder')}' -> removed")
        del exporter.attrib["vstLegacyFolder"]
    if exporter.get("IPPLibrary"):
        notes.append(f"  IPPLibrary: '{exporter.get('IPPLibrary')}' -> removed")
        del exporter.attrib["IPPLibrary"]
    return notes


def main():
    if not JUCER.exists():
        raise SystemExit(f"missing {JUCER}")

    backup = JUCER.with_suffix(".jucer.bak")
    shutil.copy2(JUCER, backup)
    print(f"backed up -> {backup.name}\n")

    tree = ET.parse(JUCER)
    root = tree.getroot()

    # --- project level: VST2 cannot build, so stop asking for it ------------
    print("project-level:")
    if root.get("buildVST") == "1":
        root.set("buildVST", "0")
        print("  buildVST: 1 -> 0 (no VST2 SDK present, and VST2 is no longer licensed)")
    formats = root.get("pluginFormats", "")
    if "buildVST," in formats or formats.endswith("buildVST"):
        new_formats = ",".join(f for f in formats.split(",") if f != "buildVST")
        root.set("pluginFormats", new_formats)
        print(f"  pluginFormats: dropped buildVST -> {new_formats}")

    exportformats = root.find("EXPORTFORMATS")
    if exportformats is None:
        raise SystemExit("no EXPORTFORMATS element")

    vs2017 = exportformats.find("VS2017")
    if vs2017 is None:
        raise SystemExit("no VS2017 exporter to clone")

    if exportformats.find("VS2019") is not None:
        print("\nVS2019 exporter already present -- not cloning again")
        vs2019 = exportformats.find("VS2019")
    else:
        vs2019 = copy.deepcopy(vs2017)
        vs2019.tag = "VS2019"
        vs2019.set("targetFolder", "builds/vs19")
        # Insert directly after VS2017 so the file stays readable.
        idx = list(exportformats).index(vs2017)
        exportformats.insert(idx + 1, vs2019)
        print("\ncloned VS2017 -> VS2019 (targetFolder=builds/vs19)")

    for name, exporter in (("VS2017", vs2017), ("VS2019", vs2019)):
        print(f"\n{name}:")
        before, after = fix_defines(exporter)
        print(f"  extraDefs: {before.split(chr(10))} ")
        print(f"          -> {after.split(chr(10))}")
        for note in fix_sdk_paths(exporter):
            print(note)

    tree.write(JUCER, encoding="UTF-8", xml_declaration=True)
    print(f"\nwrote {JUCER}")
    print("next: Projucer.exe --resave plugin/vital.jucer")


if __name__ == "__main__":
    main()
