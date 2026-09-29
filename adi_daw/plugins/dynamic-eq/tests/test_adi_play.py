# SPDX-License-Identifier: AGPL-3.0-only
"""Exercise the delivered Dynamic EQ through ADI's real Session/CLAP loader.

Only --render is used: no audio driver is opened. Float WAVs, projects and logs stay in
an isolated run directory. Pass --output to keep the evidence after the test.
"""
import argparse
import array
import struct
import sys
import math
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile


def run(command, folder, label, expected=0):
    result = subprocess.run([str(x) for x in command], capture_output=True,
                            text=True, encoding="utf-8", errors="replace", timeout=120)
    text = result.stdout + result.stderr
    (folder / (label + ".log")).write_text(text, encoding="utf-8")
    if result.returncode != expected:
        raise AssertionError(f"{label}: exit {result.returncode}, expected {expected}\n{text}")
    return text


def fixture(tool, folder, name, plugin, bindings, enabled=True, uid="com.adi.dynamic-eq"):
    project = folder / (name + ".adi")
    run([tool, "create", project], folder, name + "-create")
    with sqlite3.connect(project) as db:
        db.execute("INSERT OR REPLACE INTO project(id,name,sample_rate) VALUES(1,'Dynamic EQ host proof',48000)")
        db.execute("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Tone'),(2,'master','Master')")
        if enabled:
            db.execute("INSERT INTO plugin_refs(id,format,uid,name,subtype,path_hint) "
                       "VALUES(1,'clap',?,'ADI Dynamic EQ','effect',?)", (uid, str(plugin)))
            db.execute("INSERT INTO device_chains(id,track_id,ord) VALUES(1,1,0)")
            db.execute("INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name,enabled) "
                       "VALUES(1,1,0,1,'ADI Dynamic EQ',1)")
            db.executemany("INSERT INTO plugin_params(device_id,param_id,normalized_value) VALUES(1,?,?)",
                           [(f"{key:08x}", value) for key, value in bindings.items()])
    return project



def read_float_wav(path, rate):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AssertionError("not a RIFF WAV")
    chunks = {}
    offset = 12
    while offset + 8 <= len(data):
        key, size = struct.unpack_from("<4sI", data, offset)
        chunks[key] = data[offset+8:offset+8+size]
        offset += 8 + size + (size & 1)
    fmt = chunks[b"fmt "]
    encoding, channels, sample_rate, _, _, bits = struct.unpack_from("<HHIIHH", fmt)
    if (encoding, channels, sample_rate, bits) != (3, 2, rate, 32):
        raise AssertionError("output must be stereo 32-bit IEEE float at the session rate")
    samples = array.array("f")
    samples.frombytes(chunks[b"data"])
    if sys.byteorder != "little":
        samples.byteswap()
    if len(samples) != rate * 3 * 2 or not all(math.isfinite(v) for v in samples):
        raise AssertionError("incomplete or nonfinite render")
    if samples[0::2] != samples[1::2]:
        raise AssertionError("stereo fixture channels differ")
    return samples[rate*4::2]  # final settled second, one channel


def bin_amplitude(samples, rate, hz):
    real = math.fsum(v*math.cos(2*math.pi*hz*i/rate) for i, v in enumerate(samples))
    imag = math.fsum(v*math.sin(2*math.pi*hz*i/rate) for i, v in enumerate(samples))
    return 2*math.hypot(real, imag)/len(samples)


def verify(a, folder):
    description = run([a.probe, a.plugin, "--host-fixture"], folder, "binary-abi")
    bindings = {}
    names = set()
    for key, value, name in re.findall(r"^HOST_PARAM\t(\d+)\t([^\t]+)\t([^\r\n]+)$", description, re.M):
        bindings[int(key)] = float(value)
        names.add(name)
    if names != {"Filter Status0", "Freq0", "Gain0", "Q0"} or len(bindings) != 4:
        raise AssertionError("The delivered CLAP did not expose the four fixture parameters")
    match = re.search(r"^HOST_BYPASS\t(\d+)\t([^\r\n]+)$", description, re.M)
    if not match:
        raise AssertionError("Missing bypass value from CLAP text conversion")
    bypass = dict(bindings)
    bypass[int(match[1])] = float(match[2])
    dry = fixture(a.tool, folder, "dry", a.plugin, {}, enabled=False)
    cut = fixture(a.tool, folder, "cut", a.plugin, bindings)
    off = fixture(a.tool, folder, "bypass", a.plugin, bypass)

    def render(project, label, rate, block):
        wav = folder / (label + ".wav")
        wav.write_bytes(b"previous output")  # success must replace, not append
        command = [a.play, project, "--search", a.plugin.parent, "--tone", "1",
                   "--render", "3", "--rate", rate, "--block", block,
                   "--require-peak", "-50", "--expect-no-edits", "--render-output", wav]
        if project != dry:
            command.append("--require-devices")
        text = run(command, folder, label)
        if project != dry and not re.search(r"devices\s+1 loaded,\s+0 placeholders,\s+0 skipped", text):
            raise AssertionError(f"{label}: real device did not load")
        if project != dry and "parameter rows 4 applied on top" not in text:
            raise AssertionError(f"{label}: stored parameters were not applied")
        if project != dry and not re.search(r"loader:\s+0 vst3,\s+1 clap", text):
            raise AssertionError(f"{label}: wrong device format")
        peaks = re.findall(r"render\s+[\d.]+ s\s+peak (-?[\d.]+) dBFS", text)
        if len(peaks) != 3:
            raise AssertionError(f"{label}: expected three non-silent offline windows")
        peak = float(peaks[-1])  # settled final second, not startup smoothing
        if not math.isfinite(peak):
            raise AssertionError("nonfinite output")
        samples = read_float_wav(wav, rate)
        # Check the noise floor before trusting the wanted bin; then require
        # energy, so a valid-looking but all-zero float WAV cannot pass.
        if bin_amplitude(samples, rate, 7011) > 1e-7:
            raise AssertionError(f"{label}: far bin is not at the floor")
        if bin_amplitude(samples, rate, 220) < 0.01:
            raise AssertionError(f"{label}: missing tone in actual samples")
        return peak, samples

    for rate, block in [(48000, 32), (48000, 512), (96000, 4096)]:
        suffix = f"-{rate}-{block}"
        dry_db, dry_samples = render(dry, "dry" + suffix, rate, block)
        cut_db, cut_samples = render(cut, "cut" + suffix, rate, block)
        off_db, off_samples = render(off, "bypass" + suffix, rate, block)
        if not -25 < dry_db < -10:
            raise AssertionError(f"source missing: {dry_db} dBFS")
        if abs(off_db - dry_db) > 0.2:
            raise AssertionError(f"bypass changed signal: {off_db-dry_db} dB")
        if abs((cut_db-dry_db) - (-12)) > 0.3:
            raise AssertionError(f"filter not applied: {cut_db-dry_db} dB (expected -12)")
        factor = 10**(-12/20)
        bypass_error = max(abs(x-y) for x,y in zip(dry_samples, off_samples))
        response_error = max(abs(y-factor*x) for x,y in zip(dry_samples, cut_samples))
        if bypass_error > 2e-6 or response_error > 2e-5:
            raise AssertionError(f"sample response mismatch: bypass={bypass_error}, bell={response_error}")
        print(f"sample errors: bypass {bypass_error:.3g}, bell {response_error:.3g}")
        print(f"{rate} Hz / {block}: dry {dry_db}, bypass {off_db}, bell {cut_db} dBFS")
    missing = fixture(a.tool, folder, "missing", a.plugin, {}, uid="com.adi.test.missing-dynamic-eq")
    run([a.play, missing, "--search", a.plugin.parent, "--tone", "1", "--render", "1",
         "--require-devices"], folder, "missing-rejected", expected=2)
    output = folder / "must-not-exist.wav"
    run([a.play, dry, "--render-output", output], folder, "output-without-render", expected=2)
    if output.exists():
        raise AssertionError("usage error wrote a file")
    for existing in (False, True):
        if existing:
            output.write_bytes(b"previous output")
        run([a.play, dry, "--tone", "1", "--render", "0.1", "--require-peak", "0",
             "--render-output", output], folder, f"failed-render-{existing}", expected=3)
        changed = output.read_bytes() != b"previous output" if existing else output.exists()
        if changed:
            raise AssertionError("failed render changed the output")
    run([a.play, dry, "--tone", "1", "--render", "0.1", "--expect-no-edits"],
        folder, "legacy-render")
    invalid = folder / "missing-directory" / "output.wav"
    run([a.play, dry, "--tone", "1", "--render", "0.1", "--render-output", invalid],
        folder, "output-open-failure", expected=1)
    if invalid.exists():
        raise AssertionError("failed writer published output")
    print("PASS: real ADI host loads CLAP, applies bell, bypasses, and rejects a missing plug-in")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("play", "tool", "probe", "plugin"):
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--output", type=Path)
    a = parser.parse_args()
    for name in ("play", "tool", "probe", "plugin"):
        path = getattr(a, name).resolve()
        if not path.exists():
            parser.error(f"{name} does not exist: {path}")
        setattr(a, name, path)
    if a.output:
        a.output.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(prefix="run-", dir=a.output))
        print(f"Render evidence: {folder}")
        verify(a, folder)
    else:
        with tempfile.TemporaryDirectory(prefix="adi-eq-host-") as temp:
            verify(a, Path(temp))


if __name__ == "__main__":
    main()
