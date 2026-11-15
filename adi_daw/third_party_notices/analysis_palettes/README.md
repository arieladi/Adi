# Scientific display palettes

The analyzer includes 46 named public scientific palette families in addition to
ADI's original default colors. These are upstream datasets, not lookup tables or
artwork extracted from another application. No cross-application pixel identity
is claimed.

Sources are pinned by URL and SHA-256 in `PROVENANCE.json`:

- Matplotlib `b9ca6c2fd78b5331d9406e2994b5cb08609d0f18`: cividis,
  inferno, magma, plasma, turbo and viridis; default Cubehelix formula.
- Seaborn `c1cb287835852406691aed60ea0c62710f2c6b18`: mako and rocket.
- CMasher `819b230338fcffd9b961ee81688eabc20f5fe2ac`: the other 36 named
  color datasets. Gray is the analytic equal-channel ramp.

Changes for this derivative: all 256 positions from each source table are kept;
each RGB component is quantized to eight bits using nearest rounding (ties
upward), and the result is stored in an immutable C++ table. There is no runtime
interpolation, dependency import or file download. Reversal indexes the same
table backwards. Cubehelix and gray are sampled at `i/255`, for `i=0..255`.

Keep the complete `CMasher-LICENSE.txt`, `seaborn-LICENSE.txt` and
`matplotlib-LICENSE.txt` with source and binary distributions. The Matplotlib
license includes its required change summary above. These notices do not imply
endorsement by any upstream author.

To reproduce from a directory of the verified public source files:

```
python tools/generate_analysis_palettes.py --source-dir /path/to/pinned/sources --check
```

The generator only parses literal arrays and the verified `.npy` layout; it does
not execute the downloaded Python modules. It requires only Python's standard
library. See `src/adi/ui/palettes/data.hpp` for generated tables.
