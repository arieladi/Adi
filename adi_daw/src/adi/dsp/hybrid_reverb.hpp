// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "live_reverb.hpp"
#include "spectrum.hpp"
#include <complex>
#include <memory>
#include <span>
namespace adi::dsp {
// The impulse in NON-UNIFORM partitions (item 10 of the devices queue): its
// head in 256-sample partitions, then two longer stages. A stage of block B
// starting at offset S has a full block period to compute in because
// S + 256 (the latency) >= 2B, so its work is spread evenly and no callback
// carries it. About 71 complex multiply-adds a sample for a 5 s impulse, where
// uniform 256-sample partitions took about 940.
struct HybridImpulse {
    static constexpr std::size_t partition = 256, fftSize = 512, maxFrames = 262144;
    /// The head: the first headParts partitions of 256 (2,048 samples).
    static constexpr std::size_t headParts = 8;
    // Real input has only N/2+1 independent bins. PFFFT's packed layout never
    // escapes RealFft; DC and Nyquist each have their own complex entry here.
    using Spectrum = std::array<std::complex<float>, fftSize / 2 + 1>;
    std::array<std::vector<Spectrum>, 2> spectra;
    /// The longer stages: blocks of 1,024 from 2,048, blocks of 4,096 from
    /// 8,192 to the end. Each holds `parts` spectra of block + 1 bins per
    /// channel, one after another, from an FFT of twice the block.
    struct Stage {
        std::size_t parts = 0;
        std::array<std::vector<std::complex<float>>, 2> spectra;
    };
    static constexpr std::size_t stages = 2;
    static constexpr std::array<std::size_t, stages> stageBlock{1024, 4096}, stageOffset{2048, 8192},
        stageEnd{8192, maxFrames};
    std::array<Stage, stages> stage;
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
    /// One later stage's input, spectra and pending sum: the stage's block of
    /// input is transformed when it is complete, its parts are summed a share
    /// at a time over the next block (by sample position, so any blocking
    /// renders the same bits), and its inverse is overlap-added into tail_ at
    /// the convolution times it covers.
    struct Stage {
        std::unique_ptr<RealFft> fft;
        std::array<std::vector<float>, 2> input;                     // the block being collected
        std::array<std::vector<std::complex<float>>, 2> history;     // capacity x (block + 1) bins
        std::array<std::vector<std::complex<double>>, 2> sum;        // block + 1
        std::vector<float> frame;                                    // 2 x block
        std::vector<std::complex<float>> bins;                       // block + 1
        std::size_t capacity = 0, head = 0, used = 0, done = 0, total = 0;
        std::int64_t start = 0;     // the convolution time the pending sum's inverse begins at
        const HybridImpulse *ir = nullptr;
        bool live = false;
    };
    void stageSample(std::size_t s, const HybridImpulse *ir) noexcept;
    void stageAccumulate(std::size_t s, std::size_t target) noexcept;
    void stageFinish(std::size_t s, const HybridImpulse *ir) noexcept;
    void stageStart(std::size_t s, const HybridImpulse *ir) noexcept;
    std::array<double, Count> p_{};
    RealFft fft_;
    std::array<Stage, HybridImpulse::stages> stages_;
    // The later stages' output by convolution time (a ring: 16,384 > the
    // furthest a stage writes ahead of the sample being heard, S + B + 255 =
    // 12,543 for the last stage).
    static constexpr std::size_t tailSize = 16384;
    std::array<std::vector<double>, 2> tail_;
    std::int64_t samples_ = 0;   // input samples taken since prepare
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
