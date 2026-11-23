# SPDX-License-Identifier: AGPL-3.0-or-later
"""Regenerate the director's research inputs and verify the checked-in contract."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent


def verify_manifest():
    manifest = json.loads((ROOT / "MANIFEST.json").read_text(encoding="utf-8"))
    for entry in manifest["files"]:
        asset = ROOT / entry["path"]
        data = asset.read_bytes()
        if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError(f"Asset manifest mismatch for {entry['path']}")


def run(script, *args):
    subprocess.run([sys.executable, str(ROOT / script), *map(str, args)],
                   cwd=ROOT, check=True)


def main():
    verify_manifest()
    with tempfile.TemporaryDirectory(prefix="adi-research-fixtures-") as temporary:
        output = Path(temporary)
        audio66 = output / "audio66"
        edges = output / "edges"
        midi = output / "midi"

        run("signals/generate66.py", "make", "--out", audio66)
        run("signals/generate_edges_latency.py", "--out", edges)
        run("midi/generate.py", "--out", midi)
        run("verify_assets.py", "--audio66", audio66, "--edges", edges, "--midi", midi)


if __name__ == "__main__":
    main()
