// SPDX-License-Identifier: MIT
// Original ADI implementation; no DeRez/Bespoke code is incorporated.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace adi::dsp {
// Parameters are owned by the audio thread (Pd delivers messages there).
// Provisional ADI ranges/curves: see the per-PR parity table.
class Redux {
  public:
    void prepare(double sampleRate) noexcept;
    void setRate(double hz) noexcept;
    void setJitter(double percent) noexcept;
    void setBits(double bits) noexcept;
    void setShape(double percent) noexcept;
    void setPre(double enabled) noexcept;
    void setPost(double enabled) noexcept;
    void setOctave(double octaves) noexcept;
    void setDcShift(double enabled) noexcept;
    void setMix(double percent) noexcept;
    void process(const float *left, const float *right, float *outLeft, float *outRight,
                 std::size_t frames) noexcept;

  private:
    struct Channel {
        double remaining = 0, held = 0, pre1 = 0, pre2 = 0, post1 = 0, post2 = 0;
        std::uint32_t random = 1;
    };
    std::array<Channel, 2> channels_{};
    double sampleRate_ = 48000, rate_ = 48000, jitter_ = 0, bits_ = 16, shape_ = 0;
    double octave_ = 0, mix_ = 1, preCoefficient_ = 0, postCoefficient_ = 0;
    bool pre_ = false, post_ = false, dc_ = false, processing_ = false;
    double bitsCurrent_ = 16, mixCurrent_ = 1;
    double bitsStep_ = 0, mixStep_ = 0;
    std::size_t rampSamples_ = 240, bitsRemaining_ = 0, mixRemaining_ = 0;
    void advanceRamps() noexcept;
    void coefficients() noexcept;
    double sample(double input, Channel &channel) noexcept;
};
} // namespace adi::dsp
