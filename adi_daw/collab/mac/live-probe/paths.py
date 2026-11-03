# SPDX-License-Identifier: GPL-3.0-or-later
"""Where the probe kit keeps its bulky files (signals, sets, Live's renders): never in the repo."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get("ADI_LIVE_PROBE_WORK", os.path.expanduser("~/adi-live-probe"))
for d in ("sig", "sets", "out", "cache", "bin"):
    os.makedirs(os.path.join(WORK, d), exist_ok=True)
LIVECTL = os.path.join(WORK, "bin", "livectl")
MBD_RENDER = os.path.join(WORK, "bin", "mbd_render")
