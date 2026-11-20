"""Own/control/export the bounded ADI VST3 observer mapping (Windows, stdlib only).

Create before the plug-in is instantiated, keep this object alive, and use the
same ADI_ENGINE_MAP base name in the DAW process. Only Probe ID 100 writes PCM.
Reset requires the producer stopped; disabling is not a join/barrier for a
callback already copying a block. Snapshot/save also requires a quiescent
producer. PCM is a plug-in output observation, NOT a physical-device recording.
"""
from __future__ import annotations
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import struct

FRAMES = 1 << 20
CHUNKS = 4096
HEADER_BYTES = 4096

class Header(ct.Structure):
    _fields_ = [("magic", ct.c_char * 8), ("version", ct.c_uint32),
                ("chunk_size", ct.c_uint32), ("capacity_frames", ct.c_uint64),
                ("capacity_chunks", ct.c_uint64), ("next_frame", ct.c_int64),
                ("next_chunk", ct.c_int64), ("lost_frames", ct.c_int64),
                ("lost_chunks", ct.c_int64), ("qpc_frequency", ct.c_int64),
                ("enabled", ct.c_int32), ("reserved", ct.c_uint32)]

class Chunk(ct.Structure):
    _fields_ = [("committed", ct.c_int64), ("qpc0", ct.c_int64),
                ("qpc1", ct.c_int64), ("start_frame", ct.c_uint64),
                ("transport", ct.c_double), ("frames", ct.c_uint32),
                ("flags", ct.c_uint32), ("tid", ct.c_uint32),
                ("cpu", ct.c_uint32), ("mode", ct.c_int32), ("rt", ct.c_int32)]

assert ct.sizeof(Header) == 80 and Header.enabled.offset == 72
assert ct.sizeof(Chunk) == 64 and Chunk.frames.offset == 40
DATA_OFFSET = HEADER_BYTES + CHUNKS * ct.sizeof(Chunk)
MAP_BYTES = DATA_OFFSET + FRAMES * 8

def _win():
    k = ct.WinDLL("kernel32", use_last_error=True)
    k.CreateFileMappingW.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_uint32,
                                    ct.c_uint32, ct.c_uint32, ct.c_wchar_p]
    k.CreateFileMappingW.restype = ct.c_void_p
    k.MapViewOfFile.argtypes = [ct.c_void_p, ct.c_uint32, ct.c_uint32,
                               ct.c_uint32, ct.c_size_t]
    k.MapViewOfFile.restype = ct.c_void_p
    k.UnmapViewOfFile.argtypes = [ct.c_void_p]
    k.UnmapViewOfFile.restype = ct.c_int
    k.CloseHandle.argtypes = [ct.c_void_p]
    k.CloseHandle.restype = ct.c_int
    k.QueryPerformanceFrequency.argtypes = [ct.POINTER(ct.c_int64)]
    k.QueryPerformanceFrequency.restype = ct.c_int
    return k

class PcmCapture:
    def __init__(self, base_name: str | None = None):
        base_name = base_name or os.environ.get("ADI_ENGINE_MAP")
        if not base_name:
            raise ValueError("Pass the same mapping base used by the DAW's ADI_ENGINE_MAP")
        self.name = base_name + "_PCM"
        self.k = _win()
        self.handle = self.k.CreateFileMappingW(ct.c_void_p(-1), None, 0x04,
                                               0, MAP_BYTES, self.name)
        existed = ct.get_last_error() == 183
        if not self.handle:
            raise ct.WinError(ct.get_last_error())
        self.address = self.k.MapViewOfFile(self.handle, 0xF001F, 0, 0, MAP_BYTES)
        if not self.address:
            self.k.CloseHandle(self.handle)
            self.handle = None
            raise ct.WinError(ct.get_last_error())
        self.header = Header.from_address(self.address)
        if existed:
            if (self.header.magic != b"ADIPCM01" or self.header.version != 1
                    or self.header.chunk_size != ct.sizeof(Chunk)
                    or self.header.capacity_frames != FRAMES
                    or self.header.capacity_chunks != CHUNKS):
                self.close()
                raise ValueError("Existing PCM mapping has an incompatible layout")
        else:
            ct.memset(self.address, 0, MAP_BYTES)
            self.header.magic = b"ADIPCM01"
            self.header.version = 1
            self.header.chunk_size = ct.sizeof(Chunk)
            self.header.capacity_frames = FRAMES
            self.header.capacity_chunks = CHUNKS
            freq = ct.c_int64()
            self.k.QueryPerformanceFrequency(ct.byref(freq))
            self.header.qpc_frequency = freq.value

    def reset(self):
        """Clear counters/chunks. Caller MUST have stopped all producer callbacks."""
        self.header.enabled = 0
        self.header.next_frame = self.header.next_chunk = 0
        self.header.lost_frames = self.header.lost_chunks = 0
        ct.memset(self.address + HEADER_BYTES, 0, CHUNKS * ct.sizeof(Chunk))

    def arm(self, clear=False):
        if clear:
            self.reset()
        self.header.enabled = 1

    def stop(self):
        """Disable future captures. Does not join a callback already in flight."""
        self.header.enabled = 0

    def snapshot(self):
        """Read committed chunks; producer must be stopped/quiescent."""
        rows, last = [], 0
        attempted = self.header.next_chunk
        for i in range(min(max(attempted, 0), CHUNKS)):
            chunk = Chunk.from_buffer_copy(ct.string_at(
                self.address + HEADER_BYTES + i * ct.sizeof(Chunk), ct.sizeof(Chunk)))
            if chunk.committed != i + 1:
                continue
            if chunk.start_frame + chunk.frames > FRAMES:
                raise ValueError("Corrupt PCM chunk bounds")
            rows.append({key: getattr(chunk, key) for key, _ in Chunk._fields_})
            last = max(last, chunk.start_frame + chunk.frames)
        pcm = ct.string_at(self.address + DATA_OFFSET, last * 8)
        info = {"schema": "ADIPCM01", "mapping": self.name,
                "sample_format": "little-endian float64 mono",
                "observation": "Probe ID100 main-output channel0; not physical output",
                "attempted_chunks": attempted, "committed_chunks": len(rows),
                "attempted_frames": self.header.next_frame, "stored_frames": last,
                "lost_frames": self.header.lost_frames,
                "lost_chunks": self.header.lost_chunks,
                "qpc_frequency": self.header.qpc_frequency,
                "capacity_frames": FRAMES, "capacity_chunks": CHUNKS,
                "chunks": rows}
        return pcm, info

    def save(self, directory: str | Path, prefix="observer", sample_rate=48000):
        pcm, info = self.snapshot()
        dest = Path(directory)
        dest.mkdir(parents=True, exist_ok=True)
        path = dest / (prefix + ".f64")
        path.write_bytes(pcm)
        info["sample_rate"] = sample_rate
        info["pcm_file"] = path.name
        info["pcm_sha256"] = hashlib.sha256(pcm).hexdigest()
        meta = dest / (prefix + ".json")
        meta.write_text(json.dumps(info, indent=2), encoding="utf-8")
        return path, meta, info

    def close(self):
        if getattr(self, "address", None):
            self.header = None
            self.k.UnmapViewOfFile(self.address)
            self.address = None
        if getattr(self, "handle", None):
            self.k.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

def selftest():
    base = "Local\\ADI_PCM_PY_SELFTEST_" + str(os.getpid())
    with PcmCapture(base) as capture:
        assert capture.header.enabled == 0
        capture.arm(clear=True)
        assert capture.header.enabled == 1
        capture.stop()
        values = struct.pack("<3d", .25, -.5, .75)
        ct.memmove(capture.address + DATA_OFFSET, values, len(values))
        chunk = Chunk.from_address(capture.address + HEADER_BYTES)
        chunk.qpc0 = 10
        chunk.qpc1 = 11
        chunk.start_frame = 0
        chunk.transport = 1.0
        chunk.frames = 3
        chunk.mode = 1
        chunk.rt = 0
        chunk.committed = 1
        capture.header.next_chunk = 1
        capture.header.next_frame = 3
        pcm, meta = capture.snapshot()
        assert pcm == values and meta["committed_chunks"] == 1
        with PcmCapture(base) as second:
            assert second.snapshot()[0] == values, "Opening existing mapping must preserve it"
        del chunk
        capture.reset()
        assert capture.snapshot()[0] == b""
    print("PASS: PCM Python layout, mapping ownership, control, snapshots, reopen preservation")

if __name__ == "__main__":
    selftest()
