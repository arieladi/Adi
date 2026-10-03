# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive Ableton Live 11 to export every track of a generated set (All Individual Tracks,
48 kHz, 32-bit float -- the dialog remembers those settings)."""
import glob, os, shutil, subprocess, sys, time

from paths import LIVECTL, WORK
DL = os.path.expanduser("~/Downloads")
HERE = WORK

def sh(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def live_pid():
    out = sh("pgrep", "-f", "Ableton Live 11 Suite.app/Contents/MacOS/Live").split()
    return out[0] if out else None

def windows(pid):
    res = []
    for line in sh(LIVECTL, "windows", pid).splitlines():
        p = line.split("\t")
        if len(p) < 5 or p[1] != "on=true":
            continue
        xy, wh = p[3].split(" ")
        x, y = map(float, xy.split(","))
        w, h = map(float, wh.split("x"))
        res.append(dict(id=p[0], layer=int(p[2].split("=")[1]), x=x, y=y, w=w, h=h, name=p[4]))
    return res

def wait_for(pred, timeout=60, step=0.3, what="condition"):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = pred()
        if r:
            return r
        time.sleep(step)
    raise TimeoutError(what)

def capture(wid, path):
    subprocess.run(["screencapture", "-x", "-o", f"-l{wid}", path])

def ensure_front(pid):
    for _ in range(20):
        if sh(LIVECTL, "frontname").strip() == "Live":
            return
        sh(LIVECTL, "front", pid)
        time.sleep(0.3)
    raise RuntimeError("Live is not frontmost; refusing to send HID events")

def open_set(path):
    name = os.path.splitext(os.path.basename(path))[0]
    subprocess.run(["open", "-a", "Ableton Live 11 Suite", path])
    def ready():
        pid = live_pid()
        if not pid:
            return None
        ws = windows(pid)
        if any(w["name"] == name and w["layer"] == 0 for w in ws) and not any(
                w["layer"] == 0 and w["w"] < 1000 and w["name"] != name for w in ws):
            return pid
        # unexpected dialog?
        for w in ws:
            if w["layer"] == 0 and 200 < w["w"] < 900 and w["name"] == "":
                capture(w["id"], os.path.join(HERE, "out", "_dialog.png"))
        return None
    pid = wait_for(ready, 120, what=f"set {name} to load")
    time.sleep(1.0)
    return pid

def export(path):
    name = os.path.splitext(os.path.basename(path))[0]
    for f in glob.glob(os.path.join(DL, name + " *.wav")):
        os.remove(f)
    pid = open_set(path)
    ensure_front(pid)
    sh(LIVECTL, "key", pid, "15", "cmd,shift")
    dlg = wait_for(lambda: next((w for w in windows(pid) if w["name"] == "Export Audio/Video"), None), 20,
                   what="export dialog")
    time.sleep(0.5)
    ensure_front(pid)
    r = subprocess.run([LIVECTL, "hclickpid", pid, str(dlg["x"] + 95), str(dlg["y"] + 614.5)])
    if r.returncode:
        raise RuntimeError("Live lost focus before Export could be clicked")
    save = wait_for(lambda: next((w for w in windows(pid) if w["name"] == "Save"), None), 20, what="save panel")
    time.sleep(0.8)
    ensure_front(pid)
    r = subprocess.run([LIVECTL, "hkeypid", pid, "36"])
    if r.returncode:
        raise RuntimeError("Live lost focus before the save panel could be confirmed")
    def done():
        ws = windows(pid)
        if any(w["name"] in ("Save", "Export Audio/Video") for w in ws):
            return False
        if any(w["layer"] == 0 and w["w"] < 1000 for w in ws):   # progress window
            return False
        return bool(glob.glob(os.path.join(DL, name + " *.wav")))
    wait_for(done, 600, 0.5, what="export to finish")
    time.sleep(1.0)
    dst = os.path.join(HERE, "out", name)
    os.makedirs(dst, exist_ok=True)
    files = sorted(glob.glob(os.path.join(DL, name + " *.wav")))
    for f in files:
        shutil.move(f, os.path.join(dst, os.path.basename(f)[len(name) + 1:]))
    # Live also writes the master mixdown as <name>.wav: never leave it in Downloads
    mix = os.path.join(DL, name + ".wav")
    if os.path.exists(mix):
        os.makedirs(os.path.join(HERE, "out", "_mixdowns"), exist_ok=True)
        shutil.move(mix, os.path.join(HERE, "out", "_mixdowns", name + ".wav"))
    return dst

if __name__ == "__main__":
    for p in sys.argv[1:]:
        print(export(p))
