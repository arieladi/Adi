# Waiting on the director

Tests and checks that only Adi's own hardware or hands can do. Each says what
to run, what it proves, and where the result is written. When one is done, its
result goes into the log it names, and the row here is struck through with the
date, not deleted.

| # | What | Needs | How | Result goes to |
|---|---|---|---|---|
| 1 | **BLAKE3 on AVX-512** (ADR-0153) | the Lenovo laptop, Intel Core i9 11th gen (AVX-512), with Claude Code installed, later in the project | `tools\bench_blake3.bat` on the laptop. It prints portable, SSE4.1, AVX2 and AVX-512 medians; the last line is the AVX-512 number. The four digests must agree. | a row beside the Ryzen's in `collab/win.md`, and a short ADR if AVX-512 is slower than AVX2 there (it can be, from clock-down) |
| 2 | **ASIO heard** (ADR-0137) | Adi's audio interface and its ASIO driver (FlexASIO if it has none) | `adi_play <project.adi> --type ASIO --tone`, then listen for the 220 Hz tone | `collab/win.md`, and the ASIO row of `docs/FEATURES.md` |

## Measured so far, for comparison

BLAKE3, 1 GiB in memory, MSVC `/O2`, median of five runs (ADR-0153):

| Machine | portable | SSE4.1 | AVX2 | AVX-512 |
|---|---|---|---|---|
| win's desktop, Ryzen 7 5700X3D | 648 | 1,700 | 3,537 | no AVX-512 on this CPU |
| Adi's laptop, i9 11th gen | | | | **waiting (row 1)** |

linux's i5-3550S measured portable against SSE4.1 only, with GCC, in ADR-0147 d8.
