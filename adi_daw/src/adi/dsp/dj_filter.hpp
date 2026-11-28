// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DJ Filter (DEV-22; lands after 3 December): one knob that low-passes
// to its left and high-passes to its right, on Auto Filter's own cores. It is
// an AutoFilter held at its DJ type, so its response is Auto Filter's at the
// matched settings, sample for sample, and a fix to one is a fix to both.
//
//   Filter    -100 (low pass, its cutoff falling from 20 kHz to 20 Hz) to
//             +100 (high pass, rising from 20 Hz to 20 kHz); 0 passes the
//             signal untouched. Auto Filter's Control at Type DJ.
//   Output    -24 to +24 dB: Auto Filter's Output.
//   Dry/Wet   0 to 100 %: Auto Filter's Mix.
//
// The resonance follows the knob, as Auto Filter's DJ type has it (the
// filter's Q rises with |Filter|). Every other Auto Filter control stays at
// its default: no LFO, no envelope, no sidechain, Clean.
//
// WHAT IS NOT LIVE'S YET, and says so: the controls, their ranges, and
// whether Live's DJ Filter has a resonance of its own wait on a Live 12.4
// reading (the devices log's ONE list); Auto Filter's DJ type itself waits on
// AWAITING 10. It claims no parity. prepare() allocates Auto Filter's comb
// line, unused here; processing does not allocate. No latency.

#pragma once

#include "adi/dsp/auto_filter.hpp"

#include <array>
#include <cstddef>

namespace adi::dsp {

class DjFilter {
public:
    enum Param { Filter, Output, DryWet, Count };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{-100, 100, 0}, {-24, 24, 0}, {0, 100, 100}};
    /// The Auto Filter control each one drives.
    static constexpr AutoFilter::Param kTarget[Count] = {AutoFilter::Control, AutoFilter::Output, AutoFilter::Mix};

    DjFilter();
    void prepare(double sampleRate);
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return core_.latency(); }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

private:
    AutoFilter core_;
    std::array<double, Count> p_{};
};

}  // namespace adi::dsp
