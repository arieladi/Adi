// SPDX-License-Identifier: GPL-3.0-or-later
//
// Second-order filter design, and the conversion Pd's `biquad~` needs — ADR-0096.
//
// The designs are Robert Bristow-Johnson's Audio EQ Cookbook: bell, low and high
// shelf, low and high pass. The cuts steeper than 12 dB/oct are Butterworth
// cascades. The reason this file exists is not the cookbook, which is
// everywhere; it is two conversions that go wrong silently:
//
//   1. **Pd's `biquad~` feedback coefficients have the OPPOSITE sign to the
//      cookbook's.** The cookbook writes the recursion as
//          y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
//      and Pd's help file writes it as
//          w = x + fb1 w1 + fb2 w2,   y = ff1 w + ff2 w1 + ff3 w2
//      so `fb1 = -a1` and `fb2 = -a2`. Copying `a1, a2` straight in puts the
//      poles outside the unit circle for most useful settings and the filter
//      runs away. `toPd()` does the conversion, and the test feeds the
//      un-negated coefficients through a model of `biquad~` to show the blow-up
//      is real, not theoretical.
//
//   2. **A steep cut is not N identical sections.** Four Q = 0.707 biquads in
//      series droop to -12 dB at the cutoff, not -3 dB. An 8th-order Butterworth
//      -- 48 dB/oct, EQ Eight's steepest -- needs four sections with STAGGERED
//      Q, 1 / (2 sin((2k-1) pi / 16)) for k = 1..4: 2.563, 0.900, 0.601, 0.510.
//
// Double precision throughout. A coefficient is computed once per parameter
// change, not per sample, and the extra digits are what keep a low-frequency
// bell at 48 kHz from rounding into a slightly different filter.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adi::dsp {

/// Normalised so a0 = 1:  y = b0 x + b1 x[-1] + b2 x[-2] - a1 y[-1] - a2 y[-2]
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
};

/// The five numbers Pd's `biquad~` takes, in its order and with its signs.
struct PdBiquad {
    double fb1 = 0.0, fb2 = 0.0;
    double ff1 = 1.0, ff2 = 0.0, ff3 = 0.0;
};

// --- the cookbook ----------------------------------------------------------

/// A bell. `gainDb` at `f0`; `q` sets the width.
[[nodiscard]] Biquad peaking(double fs, double f0, double q, double gainDb) noexcept;
/// Shelves, with the cookbook's shelf slope S = 1 (the steepest monotonic one).
[[nodiscard]] Biquad lowShelf(double fs, double f0, double gainDb) noexcept;
[[nodiscard]] Biquad highShelf(double fs, double f0, double gainDb) noexcept;
/// Second-order low and high pass. q = 1/sqrt(2) is Butterworth, -3 dB at f0.
[[nodiscard]] Biquad lowPass(double fs, double f0, double q) noexcept;
[[nodiscard]] Biquad highPass(double fs, double f0, double q) noexcept;

/// Section k (1-based) of an order-`order` Butterworth, for even orders.
[[nodiscard]] double butterworthQ(int order, int k) noexcept;

enum class CutKind : std::uint8_t { LowCut, HighCut };

/// A cut of 12, 24, 36 or 48 dB/oct as that many Butterworth sections, -3 dB at
/// `f0` whatever the slope. A slope that is not a multiple of 12 is rounded
/// down to one that is, and never below 12.
[[nodiscard]] std::vector<Biquad> cut(CutKind kind, double fs, double f0,
                                      int slopeDbPerOct);

// --- analysis --------------------------------------------------------------

/// True when both poles lie strictly inside the unit circle -- the stability
/// triangle, |a2| < 1 and |a1| < 1 + a2.
[[nodiscard]] bool isStable(const Biquad& c) noexcept;

/// |H| in dB at `f`.
[[nodiscard]] double magnitudeDb(const Biquad& c, double fs, double f) noexcept;
[[nodiscard]] double magnitudeDb(const std::vector<Biquad>& cascade, double fs,
                                 double f) noexcept;

// --- Pd --------------------------------------------------------------------

/// The cookbook's coefficients in `biquad~`'s convention: the feedback terms
/// NEGATED. See the header comment.
[[nodiscard]] PdBiquad toPd(const Biquad& c) noexcept;

/// "fb1 fb2 ff1 ff2 ff3", the list message `biquad~` accepts on its left inlet.
/// What the host sends to `$0-bandN` in the EQ patch (ADR-0096).
[[nodiscard]] std::string pdMessage(const PdBiquad& p);

}  // namespace adi::dsp
