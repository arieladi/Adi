#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Seed corpus for adi_fuzz_blob.
#
#   python3 adi_daw/tests/fuzz_seeds.py <out-dir>
#
# A coverage-guided fuzzer starting from nothing spends most of its budget
# discovering that the first four bytes have to spell 'ANOT'. These seeds hand it
# that for free, and every one of them is a shape that has already mattered:
# the four defects found by hand in the first audit (ADR-0023), plus valid blobs
# of each record type for the mutator to work outwards from.
#
# Generated rather than committed as binaries, so a reviewer can read what each
# one is for. The names are the finding, not a hash.

import pathlib
import struct
import sys

HEADER = "<IHHII"  # fourcc, version, rec_size, count, flags  -- SPEC 6.3, 16 bytes
assert struct.calcsize(HEADER) == 16

ANOT, AAUT, AEXP = b"ANOT", b"AAUT", b"AEXP"
SORTED = 1


def header(fourcc, version, rec_size, count, flags=SORTED):
    return struct.pack(HEADER, struct.unpack("<I", fourcc)[0],
                       version, rec_size, count, flags)


def note(start=0, dur=1441440, nid=1, key=60, vel=100, flags=0, prob=10000, cents=0.0):
    # SPEC 6.3.1, 40 bytes.
    return struct.pack("<qqQBBBBHHfI", start, dur, nid, key, vel, 64, 0,
                       flags, prob, cents, 0)


def autopt(t=0, v=0.0, tension=0.0, curve=1, flags=0, pid=1):
    # SPEC 6.3.3, 32 bytes.
    return struct.pack("<qdfBBHQ", t, v, tension, curve, flags, 0, pid)


def exprpt(t=0, v=0.0, tension=0.0, curve=1, flags=0):
    # SPEC 6.3.2, 24 bytes.
    return struct.pack("<qffBB6s", t, v, tension, curve, flags, b"\0" * 6)


def seeds():
    out = {}

    # --- valid, so the mutator has somewhere to stand -----------------------
    out["valid-anot-3notes"] = header(ANOT, 1, 40, 3) + b"".join(
        note(start=i * 1441440, nid=i + 1, key=60 + i) for i in range(3))
    out["valid-aaut-4points"] = header(AAUT, 1, 32, 4) + b"".join(
        autopt(t=i * 720720, v=i / 3.0, pid=i + 1) for i in range(4))
    out["valid-aexp-4points"] = header(AEXP, 1, 24, 4) + b"".join(
        exprpt(t=i * 1000, v=i * 0.25) for i in range(4))
    out["valid-anot-empty"] = header(ANOT, 1, 40, 0)

    # --- the four found by hand, which is why this file exists --------------

    # 1. The old operator[] segfault: a header that claims records, and no body.
    #    count() folded to 0 while recSize()/header() still reported the claim,
    #    so a caller that skipped ok() memcpy'd from a null body_.
    out["finding-truncated-claims-1000"] = header(ANOT, 1, 40, 1000)

    # 2. The ILP32 length overflow. count * rec_size == 2^32 exactly, so
    #    `16 + count*rec_size` wrapped to 16 in a 32-bit size_t and a
    #    header-only blob passed validation while count() reported 131072.
    out["finding-ilp32-overflow"] = header(ANOT, 1, 32768, 131072)

    # 3. The allocation amplification: rec_size 1 is far smaller than
    #    sizeof(NoteRecord), so a small blob decodes into a large vector.
    #    (rec_size 1 is now RecSizeUnknown; kept because it is the shape, and
    #    because the rule that rejects it is itself worth fuzzing.)
    out["finding-amplification-recsize1"] = header(ANOT, 1, 1, 4096) + b"\x41" * 4096

    # 4. The mid-field tear: rec_size 29 copied one byte of the 2-byte `flags`
    #    and zeroed the other, so the field was neither present nor absent.
    #    ADR-0023 made this RecSizeUnknown.
    out["finding-midfield-tear-recsize29"] = header(ANOT, 1, 29, 2) + note()[:29] * 2

    # --- boundaries around the rules ADR-0023 introduced --------------------
    out["edge-recsize-zero"] = header(ANOT, 1, 0, 1)
    out["edge-recsize-max"] = header(ANOT, 1, 0xFFFF, 1)
    out["edge-count-max"] = header(ANOT, 1, 40, 0xFFFFFFFF)
    out["edge-wider-record-v2"] = header(ANOT, 2, 48, 2) + (note() + b"\xde\xad\xbe\xef" * 2) * 2
    out["edge-one-below-released"] = header(ANOT, 1, 39, 1) + note()[:39]
    out["edge-one-above-released"] = header(ANOT, 1, 41, 1) + note() + b"\x00"
    out["edge-flags-all-reserved-set"] = header(ANOT, 1, 40, 1, flags=0xFFFFFFFF) + note()
    out["edge-header-only-15-bytes"] = header(ANOT, 1, 40, 1)[:15]
    out["edge-empty"] = b""
    out["edge-fourcc-wrong"] = header(b"BAAD", 1, 40, 1) + note()

    return out


def main(argv):
    if len(argv) != 2:
        print("usage: fuzz_seeds.py <out-dir>", file=sys.stderr)
        return 2
    out = pathlib.Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    written = seeds()
    for name, data in sorted(written.items()):
        (out / name).write_bytes(data)
    print("wrote %d seeds to %s" % (len(written), out))
    for name, data in sorted(written.items()):
        print("  %-34s %5d bytes" % (name, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
