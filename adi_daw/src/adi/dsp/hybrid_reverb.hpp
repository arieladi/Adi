// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "live_reverb.hpp"
#include "spectrum.hpp"
#include <complex>
#include <span>
namespace adi::dsp {
struct HybridImpulse {
    static constexpr std::size_t partition = 256, fftSize = 512, maxFrames = 262144;
    // Real input has only N/2+1 independent bins. PFFFT's packed layout never
    // escapes RealFft; DC and Nyquist each have their own complex entry here.
    using Spectrum = std::array<std::complex<float>, fftSize / 2 + 1>;
    std::array<std::vector<Spectrum>, 2> spectra;
    double sampleRate = 48000;
    std::size_t frames = 0;
};
struct HybridImpulseOptions {
    double attackSeconds = 0, decaySeconds = 0, size = 1;
};
// Worker/message thread only. Original time-domain IR, not a Color Cab magnitude profile.
HybridImpulse prepareHybridImpulse(std::span<const float>, int channels, double sampleRate,
                                   HybridImpulseOptions = {});
class HybridReverb {
  public:
    enum Param {
        Route,
        Blend,
        Send,
        Predelay,
        Sync,
        Beats,
        Feedback,
        Decay,
        Size,
        AlgoDelay,
        Freeze,
        FreezeIn,
        Width,
        Mix,
        Count
    };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {
        {0, 3, 0},         {0, 100, 50}, {-60, 12, 0},  {0, 2000, 0},   {0, 1, 0},
        {.015625, 4, .25}, {0, .95, 0},  {.1, 20, 1.5}, {10, 400, 100}, {0, 250, 0},
        {0, 1, 0},         {0, 1, 0},    {0, 200, 100}, {0, 100, 50}};
    void prepare(double);
    void set(Param, double) noexcept;
    void tempo(double bpm) noexcept;
    void process(const float *, const float *, float *, float *, std::size_t,
                 const HybridImpulse *) noexcept;
    static constexpr int latency = static_cast<int>(HybridImpulse::partition);
    HybridReverb();

  private:
    void partition(const HybridImpulse *) noexcept;
    /// The older partitions' share of the NEXT block's convolution, spread
    /// over the 256 samples before it: by sample `position` (1 to 256) that
    /// share is position/256 done. It is the bulk of the work for a long IR,
    /// and the boundary is left with the newest partition and the FFTs, so no
    /// callback carries a whole block's accumulation (the performance pass).
    void accumulate(std::size_t position, const HybridImpulse *ir) noexcept;
    void accumulatePartition(std::size_t p) noexcept;
    std::array<double, Count> p_{};
    RealFft fft_;
    LiveReverb algorithm_;
    std::array<std::vector<HybridImpulse::Spectrum>, 2> history_;
    std::array<std::array<double, 256>, 2> input_{}, output_{}, overlap_{}, dry_{}, aligned_{};
    std::array<std::vector<float>, 2> predelay_;
    std::size_t cursor_ = 0, head_ = 0, used_ = 0, delayAt_ = 0;
    // The next block's older partitions: summed so far, how many are done of
    // how many, and the impulse they were taken from.
    std::array<std::array<std::complex<double>, HybridImpulse::fftSize / 2 + 1>, 2> pending_{};
    std::size_t pendingDone_ = 0, pendingParts_ = 0;
    const HybridImpulse *pendingIr_ = nullptr;
    double rate_ = 48000, bpm_ = 120, mix_ = 0.5, blend_ = 0.5;
};
} // namespace adi::dsp
