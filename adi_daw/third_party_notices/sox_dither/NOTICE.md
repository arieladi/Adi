# SoX dither coefficients and error-feedback algorithm

Source: SoX 14.4.2, commit `45b161d73ec087a8e003747b1aed07cd33589bca`.
The `sox-14.4.2` tag resolves to that same object in the official repository
`https://git.code.sf.net/p/sox/code` and the `https://github.com/chirlu/sox` mirror.

- [Tables and rate selection](https://github.com/chirlu/sox/blob/45b161d73ec087a8e003747b1aed07cd33589bca/src/dither.c).
- [FIR error-feedback implementation](https://github.com/chirlu/sox/blob/45b161d73ec087a8e003747b1aed07cd33589bca/src/dither.h).
- [Upstream licensing boundary](https://github.com/chirlu/sox/blob/45b161d73ec087a8e003747b1aed07cd33589bca/COPYING).

Copyright (c) 2008-9 robs@users.sourceforge.net.
The source is licensed under the GNU Lesser General Public License, version 2.1
or, at your option, any later version. The original licence text is retained in
`LICENSE.LGPL` (trailing whitespace normalized); the coefficient include retains the upstream copyright and
licence notice. ADI's original integration is AGPL-3.0-or-later under the project's
existing open-source policy. This reuse adds no SoX executable or linked library.

Pinned upstream SHA-256:

| File | SHA-256 |
|---|---|
| `src/dither.c` | `9742fb68416814dcf647e322f79126c300bf6d2704c2e016a43a2ef5250bfbb4` |
| `src/dither.h` | `c8012b2ba1a0f396f5d088bbe7efcb6608c323c73085b269ef84da97e689f562` |
| `LICENSE.LGPL` | `5df07007198989c622f5d41de8d703e7bef3d0e79d62e24332ee739a452af62a` |

ADI adaptation, 20 November 2026:

- `src/adi/audio/sox_dither_coefficients.inc` contains the unmodified numeric
  literals of `lip44`, `iew44` and `shi48/44/38/32/22/16/11/08`; declarations use
  `constexpr`. Other SoX filters and its application/effect framework are omitted.
- `src/adi/audio/quantizer.cpp` adapts the FIR recurrence to destination-LSB
  double arithmetic and independently maintained interleaved-channel history.
  It preserves ADI's existing SplitMix64 and unshaped rounding behavior.
- Shaped 1 selects Lipshitz, Shaped 2 improved E-weighted, and Shaped 3 Shibata.
  The pinned table order and inclusive five-percent rate tolerance are retained.
  Missing tables fall back to ordinary TPDF, exposed by `effectiveDither`.
  In particular, Shaped 1 at 48 kHz and all three at 96 kHz use TPDF.
- The SoX empirical `gain_cB` values are not copied: they do not guarantee no
  clipping. ADI reserves a conservative FIR bound in destination LSBs. With
  TPDF magnitude at most 1 and rounding error at most 0.5, each stored feedback
  error has magnitude at most 1.5. For coefficient absolute sum S, reserving
  `ceil(1.5 * (1 + S)) + 2` LSBs covers that noise/feedback bound, the positive
  endpoint and numerical margin for inputs in [-1,1]. `gain()` reports this
  attenuation; an actual pre-clamp saturation count makes it testable.
- Nonfinite input still consumes its normal random draws and advances the
  interleaved channel. Its output is zero and only that channel's feedback
  history is cleared. Source values outside [-1,1] retain saturating behavior.

This implements open noise-shaping filters, not POW-r and not a claim of
bit-for-bit compatibility with SoX or another DAW's output.
