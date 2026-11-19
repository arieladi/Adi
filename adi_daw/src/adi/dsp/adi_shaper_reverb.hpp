// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <vector>

namespace adi::dsp {

// Convolution with the built-in ADI room, a finite, factored stereo impulse.
// The two short FIRs supply diffusion, followed by sparse wall reflections.
class AdiShaperReverb {
  public:
    bool prepare(double rate) noexcept;
    void reset() noexcept;
    void setShape(float size, float decay, bool gate) noexcept;
    void setWidth(float width) noexcept;
    void setPredelay(float position, double bpm) noexcept;
    void setClearTails(bool clear) noexcept { clearTails_ = clear; }
    // Wet only. The band mixer applies the measured overlapping dry/wet laws.
    void processStereo(float& l, float& r, float send, float ret) noexcept;

  private:
    using Frame = std::array<float, 2>;
    struct Tap { std::size_t lag; float left, right; };
    struct Diffuser {
        std::vector<Frame> ring;
        std::array<Tap, 5> taps{};
        std::size_t write = 0, valid = 0;
        Frame advance(Frame input) noexcept;
    };
    void shape() noexcept;
    std::array<Diffuser, 2> diffusion_;
    std::array<Tap, 64> taps_{};
    std::vector<Frame> history_;
    std::size_t write_ = 0, valid_ = 0, predelayFrames_ = 0, tailLength_ = 0, remaining_ = 0;
    double rate_ = 0, bpm_ = 120;
    float size_ = .5f, decay_ = 1, side_ = 1, delay_ = 0, previousReturn_ = 1;
    bool gate_ = false, clearTails_ = false;
};

} // namespace adi::dsp
