"""Create two disposable ADI projects with an equal number of hosted probes."""

import argparse
import hashlib
import json
import os
import shutil
import sqlite3
import subprocess
from pathlib import Path


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def build_project(tool, project, uid, bundle, count, iterations, topology, env):
    subprocess.run([str(tool), "create", str(project)], env=env, check=True)
    with sqlite3.connect(project) as db:
        db.execute("PRAGMA foreign_keys = ON")
        master, = db.execute("SELECT id FROM tracks WHERE kind='master'").fetchone()
        db.execute("DELETE FROM tracks WHERE id<>?", (master,))  # only the new empty fixture
        db.execute("UPDATE project SET name=?,sample_rate=48000", (topology,))
        plugin_ref, = db.execute("SELECT COALESCE(MAX(id),0)+1 FROM plugin_refs").fetchone()
        db.execute(
            "INSERT INTO plugin_refs(id,format,uid,vendor,name,version,subtype,path_hint) "
            "VALUES(?,'vst3',?,'ADI Research','ADI Engine Probe','1.0.0','effect',?)",
            (plugin_ref, uid, str(bundle)),
        )
        sources = count if topology == "independent" else 1
        for index in range(sources):
            track = 1000 + index
            db.execute(
                "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(?,'audio',?,?)",
                (track, f"Audio {index + 1}", index),
            )
            db.execute("INSERT INTO device_chains(id,track_id) VALUES(?,?)", (track, track))
        for index in range(count):
            chain = 1000 + (index if topology == "independent" else 0)
            order = index if topology == "serial" else 0
            device = 2000 + index
            db.execute(
                "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name,always_process) "
                "VALUES(?,?,?,?,'ADI Engine Probe',1)",
                (device, chain, order, plugin_ref),
            )
            db.executemany(
                "INSERT INTO plugin_params(device_id,param_id,name,normalized_value) VALUES(?,?,?,?)",
                [(device, "0", "Probe ID", (index + 1) / 4095),
                 (device, "1", "Gain", 0.01),
                 (device, "2", "Work iterations/sample", iterations / 20000),
                 (device, "3", "Generate 997Hz", 1.0)],
            )
        if db.execute("PRAGMA foreign_key_check").fetchone() is not None:
            raise RuntimeError("fixture foreign-key validation failed")
    with sqlite3.connect(project, isolation_level=None) as db:
        outcome = db.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        if outcome[0] != 0:
            raise RuntimeError("fixture's WAL could not be folded")


def build(args):
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    module_root = root / "modules"
    bundle = module_root / "ADIEngineProbe.vst3"
    module = bundle / "Contents" / "x86_64-win" / bundle.name
    module.parent.mkdir(parents=True)
    shutil.copyfile(args.module.resolve(strict=True), module)
    home = root / "home"
    home.mkdir()
    env = {**os.environ, "ADI_HOME": str(home)}
    # The workload keeps the probe's optional native trace/PCM sinks disabled.
    env.pop("ADI_ENGINE_MAP", None)
    for key in tuple(env):
        if key.startswith(("ADI_PROBE_", "ADI_TRACE_", "ADI_PCM_")):
            env.pop(key)
    listing = subprocess.run(
        [str(args.player.resolve()), "--list", "--search-only", str(module_root)],
        env=env, check=True, capture_output=True, text=True, errors="replace",
    )
    (root / "discovery.log").write_text(listing.stdout + listing.stderr, encoding="utf-8")
    rows = [line.strip().split("  ", 2) for line in listing.stdout.splitlines()
            if line.strip().startswith("vst3  ")]
    matched = [(uid, description) for kind, uid, description in rows
               if kind == "vst3" and description.startswith("ADI Engine Probe -- ADI Research ")]
    if len(matched) != 1 or len(rows) != 3:
        raise RuntimeError("confined discovery did not find exactly the three owned variants")
    uid, _ = matched[0]
    projects = {}
    for topology in ("independent", "serial"):
        path = root / f"{topology}.adi"
        build_project(args.tool.resolve(), path, uid, bundle, args.count, args.iterations, topology, env)
        projects[topology] = {"path": str(path), "sha256": sha256(path)}
    write_receipt = {
        "schema_version": 1,
        "projects": projects,
        "module_root": str(module_root),
        "module_sha256": sha256(module),
        "module": str(module),
        "uid": uid,
        "instances": args.count,
        "work_iterations_per_sample": args.iterations,
        "sample_rate": 48000,
        "home": str(home),
        "output": "997 Hz from each probe, normalized gain 0.01, stereo",
    }
    (root / "fixtures.json").write_text(json.dumps(write_receipt, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--player", type=Path, required=True)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--count", type=int, default=64)
    parser.add_argument("--iterations", type=int, default=500)
    args = parser.parse_args()
    if not 1 <= args.count <= 64 or not 1 <= args.iterations <= 20000:
        parser.error("count or work iterations outside the fixture contract")
    build(args)


if __name__ == "__main__":
    main()
