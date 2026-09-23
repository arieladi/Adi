#!/usr/bin/env python3
"""
Find "phantom" parameters: entries in Vital's ValueDetails table that the engine
never registers, so they can never be set.

SynthPlugin's constructor skips any ValueDetails entry with no matching key in
the engine's control_map:

    src/plugin/synth_plugin.cpp:28-30
        if (controls_.count(details->name) == 0)
          continue;

Those names are in our extracted schema but are not real parameters. Handing
them to a model is handing it names that silently do nothing.

Ground truth is the host's own parameter list, because ValueBridge::getName()
returns the entry's display_name:

    VitalValidator.exe <plugin>.vst3 --dump-params out/host_params.tsv
    python find_phantom_params.py

Everything after Vital's own parameters is JUCE's MIDI-CC emulation block
("MIDI CC <channel>|<cc>"), which we drop before comparing.

Writes out/phantom_params.json. Exits 1 if the extractor is missing something
the engine has, because that is a bug in us rather than a fact about Vital.
"""

import json
import re
import sys
from collections import Counter
from pathlib import Path

MIDI_CC_RE = re.compile(r"^MIDI CC \d+\|\d+$")


def main():
    here = Path(__file__).resolve().parent
    tsv = here / "out" / "host_params.tsv"
    if not tsv.exists():
        raise SystemExit(
            f"missing {tsv}\nrun: VitalValidator.exe <plugin>.vst3 --dump-params {tsv}"
        )

    schema = json.loads((here / "out" / "vital_schema_full.json").read_text(encoding="utf-8"))

    host_names, midi_cc = [], 0
    for line in tsv.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        _idx, _tab, name = line.partition("\t")
        if MIDI_CC_RE.match(name):
            midi_cc += 1
        else:
            host_names.append(name)

    print(f"host parameters total     : {len(host_names) + midi_cc}")
    print(f"  JUCE MIDI-CC emulation  : {midi_cc}  (16 channels x 130)")
    print(f"  Vital parameters        : {len(host_names)}")
    print(f"ValueDetails table        : {len(schema)}")
    print()

    # display_name is the join key. Verify it is actually unique first --
    # a collision would silently mis-attribute a phantom.
    by_display = {}
    dup_display = Counter()
    for internal, d in schema.items():
        dn = d.get("display_name", "")
        dup_display[dn] += 1
        by_display.setdefault(dn, internal)

    collisions = {k: v for k, v in dup_display.items() if v > 1}
    if collisions:
        print(f"WARNING: {len(collisions)} display_name collisions -- join is ambiguous:")
        for k, v in list(collisions.items())[:10]:
            print(f"    {k!r} x{v}")
        print()

    host_set = set(host_names)
    registered, unmapped = set(), []
    for name in host_names:
        internal = by_display.get(name)
        if internal is None:
            unmapped.append(name)
        else:
            registered.add(internal)

    phantom = sorted(set(schema) - registered)

    if unmapped:
        print(f"UNMAPPED: {len(unmapped)} host names absent from our schema (extractor bug):")
        for n in unmapped[:20]:
            print(f"    {n!r}")
        print()

    print(f"phantom parameters: {len(phantom)}")
    by_group = {}
    for name in phantom:
        by_group.setdefault(schema[name].get("group", "?"), []).append(name)
    for group, names in sorted(by_group.items()):
        print(f"\n  [{group}]  {len(names)}")
        for n in sorted(names):
            d = schema[n]
            print(f"      {n:34} {d.get('display_name','')}")

    llm = {k: v for k, v in schema.items() if v.get("group") != "modulation"}
    llm_phantom = [p for p in phantom if schema[p].get("group") != "modulation"]
    print()
    print(f"LLM-facing subset today     : {len(llm)}")
    print(f"  of which phantom          : {len(llm_phantom)}")
    print(f"  REAL LLM-facing parameters: {len(llm) - len(llm_phantom)}")

    out = here / "out" / "phantom_params.json"
    out.write_text(json.dumps({
        "phantom": phantom,
        "phantom_by_group": {g: sorted(n) for g, n in by_group.items()},
        "unmapped_host_names": unmapped,
        "registered_count": len(registered),
        "table_count": len(schema),
        "midi_cc_count": midi_cc,
        "llm_facing_real": len(llm) - len(llm_phantom),
    }, indent=2), encoding="utf-8")
    print(f"\nwrote {out}")

    return 1 if unmapped else 0


if __name__ == "__main__":
    sys.exit(main())
