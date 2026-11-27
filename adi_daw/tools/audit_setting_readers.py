# SPDX-License-Identifier: GPL-3.0-or-later
"""List the Settings rows dimmed by the window and find candidate source references.

Run after fetching the JSON headers, using a C++20 compiler on Linux:
    python tools/audit_setting_readers.py [--evidence]

Literal occurrences are candidates for review, regardless of whether they appear
in a read, a write, or an unused function. The window's reviewed list determines
which rows are available. Some working consumers use indirect names or fields.
"""

import argparse
from collections import defaultdict
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''
#include "adi/ui/settings_availability.hpp"
#include "adi/ui/settings_tabs.hpp"
#include <iostream>

int main() {
    for (const auto &tab : adi::ui::settingsTabs())
        for (const auto &group : tab.groups)
            for (const auto *row : group.settings)
                std::cout << row->key << '\t' << tab.name << '\t'
                          << adi::ui::settingAvailable(row->key) << '\n';
}
'''


def window_rows(compiler, json_include):
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "settings_audit.cpp"
        binary = Path(directory) / "settings_audit"
        source.write_text(PROBE, encoding="utf-8")
        subprocess.run([
            *shlex.split(compiler), "-std=c++20", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", f"-I{ROOT / 'src'}", f"-I{json_include}",
            str(source), str(ROOT / "src/adi/settings/registry.cpp"),
            str(ROOT / "src/adi/settings/channel_config.cpp"),
            str(ROOT / "src/adi/settings/catalogue.cpp"), "-o", str(binary),
        ], check=True)
        output = subprocess.check_output([binary], text=True, encoding="utf-8")
    return [tuple(row.split("\t")) for row in output.splitlines()]


def source_references(keys):
    tokens = re.compile(r'"(?:[^"\\]|\\.)*"|//[^\n]*|/\*.*?\*/', re.DOTALL)
    definitions = {
        "src/adi/settings/registry.cpp", "src/adi/settings/registry.hpp",
        "src/adi/settings/catalogue.cpp", "src/adi/ui/settings_tabs.hpp",
        "src/adi/ui/settings_availability.hpp",
    }
    hits = defaultdict(list)
    for path in sorted((ROOT / "src").rglob("*")):
        filename = path.relative_to(ROOT).as_posix()
        if path.suffix not in {".cpp", ".hpp", ".c", ".h", ".inc"} or filename in definitions:
            continue
        content = path.read_text(encoding="utf-8", errors="replace")
        for token in tokens.finditer(content):
            text = token.group()
            if text.startswith('"') and text[1:-1] in keys:
                line = content.count("\n", 0, token.start()) + 1
                hits[text[1:-1]].append(f"{filename}:{line}")
    return hits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--json-include", type=Path, default=ROOT / "third_party/json/include")
    parser.add_argument("--evidence", action="store_true", help="print candidate C++ references per key")
    args = parser.parse_args()
    rows = window_rows(args.cxx, args.json_include.resolve())
    hits = source_references({key for key, _, _ in rows})
    dimmed = defaultdict(list)
    for key, tab, available in rows:
        if available == "0":
            dimmed[tab].append(key)
    count = sum(len(keys) for keys in dimmed.values())
    print(f"{count} of {len(rows)} Settings rows are marked unavailable. "
          f"{len(hits)} keys have literal references outside the registry and layout tables.")
    print()
    for tab, keys in dimmed.items():
        print(f"- **{tab} ({len(keys)})** " + ", ".join(f"`{key}`" for key in sorted(keys)) + ".")
    if args.evidence:
        print("\nCandidate references by key\n")
        for key in sorted(hits):
            print(f"{key}\t" + " ".join(hits[key]))


if __name__ == "__main__":
    main()
