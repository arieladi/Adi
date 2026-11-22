// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Roar, the core (DEV-22; lands after 3 December): three shaper
// stages in one of five routings, with a feedback path around them.
//
//   Routing     Single (stage 1), Serial (1, 2, 3 in turn), Parallel (the
//               three summed, each a third), Mid Side (stage 1 on the mid,
//               stage 2 on the side), Multiband (Linkwitz-Riley LR4 bands
//               split at Low Split and High Split: stage 1 on the low, 2
//               the mid, 3 the high; the low passes the high split's
//               allpass, so the bands sum flat).
//   Input       -24 to +24 dB into the routing.
//   For each stage (s = 1, 2, 3):
//     Shape     Soft Sine (sin(pi/2 x), clipped), Diode (tanh, steeper for
//               negatives), Tube (tanh), Polynomial (x - x^3/3, clipped),
//               Bit Crusher (steps of 2^-(16 - 14 Amount)), Fold (sin(x)),
//               each with slope 1 at 0 except the crusher.
//     Amount    0 to 100 %: the drive into the shape, 0 to +36 dB, the
//               shape's output divided by it again (a quiet signal passes the
//               stage at the shape's slope at its bias: unity with none).
//     Bias      -100 to +100 %: an offset into the shape (0.5 at the ends),
//               removed after it, the stage's DC then blocked at 5 Hz: a
//               loud signal gains even harmonics.
//     Filter    Off, Low Pass, High Pass, Band Pass (a biquad after the
//               shape) at Freq (20 Hz to 20 kHz) and Q (0.5 to 10).
//     Level     -36 to +12 dB.
//   Feedback    0 to 95 %, Feedback Time 1 to 1,000 ms, Feedback Freq: the
//               routing's output, low-passed at Feedback Freq and delayed
//               by Feedback Time, added to its input.
//   Output      -36 to +12 dB. Dry/Wet 0 to 100 %.
//
// With every shape quiet, no Bias, no crusher and no feedback, the device is
// linear: its response is the routing of the stages' filters, levels and DC
// blockers, which the test checks per routing, and with feedback the loop's
// own response. A change of Routing restarts every stage from silence, so a
// stage that sat idle does not replay its old state.
//
// WHAT IS NOT LIVE'S YET, and says so: Roar's shape list, its compressor,
// its modulation matrix (LFOs, envelopes, noise), the feedback's modes and
// tone, every range and default, and these curves wait on a Live 12.4
// reading of Roar (the devices log's ONE list). It is not oversampled. It
// claims no parity. prepare() allocates the feedback line and never throws;
// processing does not. No latency.

#pragma once

#include "adi/dsp/biquad.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace adi::dsp {

class Roar {
public:
    static constexpr std::size_t kStages = 3, kStageFields = 7;
    enum Routing { Single, Serial, Parallel, MidSide, Multiband };
    enum Shape { SoftSine, Diode, Tube, Polynomial, BitCrusher, Fold, Shapes };
    enum Filter { Off, LowPass, HighPass, BandPass };
    enum StageField { StageShape, StageAmount, StageBias, StageFilter, StageFreq, StageQ, StageLevel };
    enum Param : int {
        RoutingParam,
        Input,
        LowSplit,
        HighSplit,
        FirstStage,   ///< then kStages x kStageFields stage controls
        Feedback = FirstStage + static_cast<int>(kStages * kStageFields),
        FeedbackTime,
        FeedbackFreq,
        Output,
        DryWet,
        Count
    };
    struct Range {
        double low, high, initial;
    };
    [[nodiscard]] static constexpr Param stage(std::size_t s, StageField f) noexcept {
        return static_cast<Param>(FirstStage + static_cast<int>(s * kStageFields) + static_cast<int>(f));
    }
    [[nodiscard]] static Range range(Param) noexcept;
    /// A shape at a drive's input, before its bias is removed: for tests.
    [[nodiscard]] static double shape(int shape, double x, double amount) noexcept;

    Roar();
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// Stage s's filter as set (a pass-through when Off), for tests.
    [[nodiscard]] const Biquad& filter(std::size_t s) const noexcept { return stages_[s].filter; }

private:
    struct State {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    struct Stage {
        Biquad filter{1, 0, 0, 0, 0};
        double drive = 1, bias = 0, offset = 0, level = 1, amount = 0;
        int shape = 0;
        std::array<State, 2> state{};
        std::array<double, 2> dcIn{}, dcOut{};
    };
    struct Chain {   ///< an LR4 low or high pass: two second-order Butterworths
        std::array<Biquad, 2> sections{};
        std::array<std::array<State, 2>, 2> state{};
        double run(std::size_t channel, double x) noexcept;
    };
    void update() noexcept;
    double runStage(std::size_t s, std::size_t channel, double x) noexcept;
    std::array<double, Count> p_{};
    std::array<Stage, kStages> stages_{};
    Chain lowLp_, lowHp_, highLp_, highHp_, allLp_, allHp_;
    std::array<std::vector<float>, 2> line_;
    std::array<State, 2> feedbackState_{};
    Biquad feedbackFilter_{};
    std::size_t size_ = 0, write_ = 0, feedbackDelay_ = 1;
    double rate_ = 48000, input_ = 1, output_ = 1, wet_ = 1, feedback_ = 0, dcPole_ = 0;
};

}  // namespace adi::dsp
