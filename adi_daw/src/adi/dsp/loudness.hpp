// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one loudness meter (DEV-28; ADR-0203 d14, XC-03): ITU-R BS.1770 loudness
// with EBU Mode's readings (EBU Tech 3341: momentary, short-term, integrated,
// maximum momentary and short-term; Tech 3342: loudness range), and true peak
// through ADI's TruePeakMeter, on every channel, from float or double input. Every reading ADI shows or uses is meant to come
// from this class: Main's meter, export normalization, Auto Gain Stage's LUFS
// mode, `analyze.loudness`, the Reference's matched loudness.
//
// How it counts:
//   - K-weighting per channel (BS.1770's two stages, at any rate).
//   - 10 ms sub-blocks of weighted energy. Momentary is the last 40 (400 ms),
//     short-term the last 300 (3 s); both, and their maxima, update every
//     sub-block. Each reads -inf until its window has been filled once.
//   - Every 10 sub-blocks (100 ms) a 400 ms gating block goes into the
//     integrated histogram and a short-term value into the loudness-range
//     histogram (10 Hz, Tech 3342 v4's minimum), each above -70 LUFS.
//   - The histograms are Fenwick trees over 0.01 LU bins from -70 to +20 LUFS,
//     holding counts and exact energy sums, allocated in prepare. Integrated
//     and range are recomputed every 100 ms in O(log n): the relative gates
//     and percentiles never walk the bins on the audio thread.
//   - Run and pause: paused, the integrated, range and maximum readings stop
//     accumulating; momentary, short-term and true peak go on. reset() clears
//     all of it. The end of a programme needs nothing: no padding is ever fed
//     to the integrated reading.
//
// What it claims: it passes ADI's synthesised versions of Tech 3341's minimum
// requirement signals (cases 1-6, 9-23) and Tech 3342's (1-4); see
// tests/test_loudness.cpp. It does not claim "EBU Mode compliant".
//
// PROVENANCE (DEV-28; LD1). Written here from ITU-R BS.1770-4 and EBU Tech
// 3341 and 3342. The plan's LD1 named FilmCraft's meter as the base; its
// source was not on the machine this was written on and none of it is here,
// so no notice is carried for it. The K-weighting is AutoGain's, whose
// prototypes are libebur128's (credited in auto_gain.cpp); the histograms,
// sub-blocks and gating are ADI's own.
//
// prepare() allocates; process(), reset() and setRunning() do not.

#pragma once

#include "adi/dsp/biquad.hpp"
#include "adi/dsp/true_peak.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace adi::dsp {

class LoudnessMeter {
public:
    static constexpr double kInfinity = std::numeric_limits<double>::infinity();
    /// BS.1770's channel weights for the usual layouts: 1.0 for L, R and C,
    /// 1.41 (+1.5 dB) for the surrounds, 0 for LFE (never guessed: the caller
    /// passes one weight per channel).
    static constexpr double kSurroundWeight = 1.41;

    struct Readings {
        double momentary = -kInfinity;      ///< LUFS, -inf until 400 ms have run
        double shortTerm = -kInfinity;      ///< LUFS, -inf until 3 s have run
        double integrated = -kInfinity;     ///< LUFS, -inf until a block passes the gates
        double range = 0.0;                 ///< LU (Tech 3342)
        double maxMomentary = -kInfinity;   ///< LUFS since reset, while running
        double maxShortTerm = -kInfinity;   ///< LUFS since reset, while running
        double truePeak = -kInfinity;       ///< dBTP since reset, every channel
        bool rangeValid = false;            ///< at least one short-term value gated in
        bool running = true;
    };

    /// Message thread. One weight per channel; allocates every buffer.
    void prepare(double sampleRate, std::span<const double> channelWeights);

    /// Audio or analysis thread. `channels` pointers, one per prepared channel.
    void process(const float* const* channels, int frames) noexcept;
    void process(const double* const* channels, int frames) noexcept;

    /// Clears every reading and all history: a new programme.
    void reset() noexcept;
    /// Run (true) or pause (false) the integrated, range and maximum readings.
    /// A window that holds any paused audio stays out of them after resuming.
    void setRunning(bool running) noexcept;

    [[nodiscard]] Readings readings() const noexcept;
    [[nodiscard]] int channels() const noexcept { return static_cast<int>(weights_.size()); }

    /// -0.691 + 10 log10(energy): BS.1770's loudness of a mean weighted energy.
    [[nodiscard]] static double loudness(double energy) noexcept;

private:
    // A Fenwick tree over the 0.01 LU bins: counts and energy sums.
    struct Histogram {
        std::vector<std::int64_t> count;
        std::vector<double> energy;
        std::int64_t total = 0;
        double totalEnergy = 0.0;
        void prepare(int bins);
        void clear() noexcept;
        void add(int bin, double e) noexcept;
        [[nodiscard]] std::int64_t countBelow(int bin) const noexcept;   // bins [0, bin)
        [[nodiscard]] double energyBelow(int bin) const noexcept;
        /// The bin holding the k-th smallest value, k from 1.
        [[nodiscard]] int find(std::int64_t k) const noexcept;
    };

    template <class Sample>
    void run(const Sample* const* channels, int frames) noexcept;
    void endSubBlock() noexcept;
    void gatingStep() noexcept;
    /// A window of `window` sub-blocks may feed I, LRA and the maxima.
    [[nodiscard]] bool counts(int window) const noexcept;
    [[nodiscard]] static int binOf(double lufs) noexcept;
    [[nodiscard]] static double valueOf(int bin) noexcept;

    double rate_ = 48000.0;
    std::vector<double> weights_;
    std::vector<Biquad> shelf_, highPass_;
    struct State { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    std::vector<State> shelfState_, highPassState_;

    // Sub-blocks: their boundaries fall on round(k * rate / 100), so a rate
    // that does not divide by 100 still gets 100 sub-blocks a second.
    std::int64_t subBlocks_ = 0;         ///< completed since reset
    std::int64_t samplesInSubBlock_ = 0;
    std::int64_t subBlockLength_ = 480;
    double subBlockEnergy_ = 0.0;        ///< weighted sum of squares, this sub-block
    static constexpr int kRing = 300;    ///< the short-term window, in sub-blocks
    double ringEnergy_[kRing] = {};
    std::int64_t ringSamples_[kRing] = {};

    Histogram integrated_, range_;
    double momentary_ = -kInfinity, shortTerm_ = -kInfinity;
    double maxMomentary_ = -kInfinity, maxShortTerm_ = -kInfinity;
    double integratedValue_ = -kInfinity, rangeValue_ = 0.0;
    bool rangeValid_ = false;
    bool running_ = true;
    std::int64_t runningSince_ = 0;      ///< first sub-block wholly measured while running

    TruePeakMeter truePeak_;
    bool truePeakOn_ = false;
};

}  // namespace adi::dsp
