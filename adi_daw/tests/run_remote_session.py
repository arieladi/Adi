#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the export player against a TCP fixture hosting a real CLAP."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from audiogridder_fixture import Fixture


def main():
    executable, plugin = sys.argv[1:]
    subprocess.run([executable, "automation"], check=True, timeout=15)
    with tempfile.TemporaryDirectory(prefix="adi-remote-session-") as directory:
        copy = Path(directory) / Path(plugin).name
        binary = copy
        if sys.platform == "darwin":
            binary = copy / "Contents" / "MacOS" / copy.stem
            binary.parent.mkdir(parents=True)
        shutil.copyfile(plugin, binary)
        with subprocess.Popen([executable, "host", str(copy)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE) as host:
            try:
                def process(samples):
                    host.stdin.write(samples)
                    host.stdin.flush()
                    rendered = host.stdout.read(len(samples))
                    if len(rendered) != len(samples):
                        raise RuntimeError("the fixture CLAP process returned a short block")
                    return rendered

                for mode, fail_after in (("render", None), ("disconnect", 7)):
                    with Fixture(process_pcm=process, expect_note=False, latency=0,
                                 fail_after=fail_after) as server:
                        subprocess.run([executable, mode, str(copy), str(server.port)],
                                       check=True, timeout=15)
                    if fail_after is None and server.blocks < 10:
                        raise RuntimeError("the fixture played too few blocks for the comparison")
                    if fail_after is not None and server.blocks != fail_after:
                        raise RuntimeError("the fixture did not produce the requested disconnect")
                host.stdin.close()
                if host.wait(timeout=5) != 0:
                    raise RuntimeError("the fixture CLAP process did not exit successfully")
            finally:
                if host.poll() is None:
                    host.kill()
                    host.wait(timeout=5)


if __name__ == "__main__":
    main()
