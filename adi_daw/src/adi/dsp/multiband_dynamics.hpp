// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <vector>
namespace adi::dsp {
// Ableton Live's Multiband Dynamics, rebuilt from measurements of Live 11.2.7
// (collab/mac/2026-10-03-multiband-dynamics-pd.md). Everything runs at twice the
// host rate between a polyphase IIR half-band upsampler and downsampler.
class MultibandDynamics {
  public:
    // Live's own parameter order (its LOM list without Device On), then the four
    // switches Live keeps in the set but does not automate.
    enum Param {
        LowMidCrossover,
        MidHighCrossover,
        SoftKnee,
        PeakMode,
        MasterOutput,
        Amount,
        TimeScaling,
        OutputGainLow,
        OutputGainMid,
        OutputGainHigh,
        InputGainLow,
        InputGainMid,
        InputGainHigh,
        ActiveLow,
        ActiveMid,
        ActiveHigh,
        AboveThresholdLow,
        AboveThresholdMid,
        AboveThresholdHigh,
        BelowThresholdLow,
        BelowThresholdMid,
        BelowThresholdHigh,
        AboveRatioLow,
        AboveRatioMid,
        AboveRatioHigh,
        BelowRatioLow,
        BelowRatioMid,
        BelowRatioHigh,
        AttackLow,
        AttackMid,
        AttackHigh,
        ReleaseLow,
        ReleaseMid,
        ReleaseHigh,
        SidechainOn,
        SidechainGain,
        SidechainMix,
        LowBandOn,
        HighBandOn,
        SoloLow,
        SoloMid,
        SoloHigh,
        SidechainListen,
        Count
    };
    // Units: Hz, 0/1, 0 = RMS 1 = Peak, dB, percent, percent of the set times,
    // dB, dB, 0/1, dB, dB, Live's ratio value, Live's ratio value, ms, ms,
    // 0/1, dB, percent, 0/1, 0/1, 0/1 x3, 0/1. A fresh Live device's values.
    static constexpr std::array<double, Count> kDefaults{
        120, 2500, 1, 0,   0,   100, 100, 0,   0,   0,   0,   0,   0, 1, 1,
        1,   -20,  -20, -20, -60, -60, -60, 0, 0,   0,   0,   0,   0, 50, 10,
        5,   300,  200, 100, 0,   0,   100, 1, 1,   0,   0,   0,   0};

    MultibandDynamics();
    void prepare(double sampleRate);
    void reset() noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double get(Param p) const noexcept { return value_[p]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float *inL, const float *inR, float *outL, float *outR, std::size_t n,
                 const float *scL = nullptr, const float *scR = nullptr) noexcept;

    // The pieces, public so the tests can hold each one against Live's numbers.
    // HIIR-style first-order allpass chain: y = (x - y1) * a + x1, in float.
    struct AllpassChain {
        std::array<float, 4> c{}, x{}, y{};
        float step(float v) noexcept;
    };
    struct HalfbandUp {
        AllpassChain even, odd;
        void step(float in, float &first, float &second) noexcept;
    };
    struct HalfbandDown {
        AllpassChain even, odd;
        float held = 0;
        float step(float first, float second) noexcept;
    };
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, s1 = 0, s2 = 0;
        float step(float v) noexcept;
    };
    struct Lr4 {
        std::array<Biquad, 2> stage;
        void design(double frequency, double rate, bool high) noexcept;
        float step(float v) noexcept { return stage[1].step(stage[0].step(v)); }
        void clear() noexcept;
    };
    struct BandSplit {
        Lr4 lowL, highL, lowOfLow, highOfLow, lowOfHigh, highOfHigh, lowH, highH;
        void design(double low, double high, double rate) noexcept;
        void clear() noexcept;
        // low/mid/high of one sample, for the current switch state
        void step(float v, bool lowOn, bool highOn, bool crossed, float &low, float &mid,
                  float &high) noexcept;
    };
    static const std::array<float, 8> kHalfbandCoefficients;
    // Gain in dB for a detector level in dB, before the +36.12 dB cap.
    static double staticGain(double level, double aboveThreshold, double aboveRatio,
                             double belowThreshold, double belowRatio, bool knee) noexcept;
    static double envelopeCoefficient(double ms, double rate) noexcept;

  private:
    // Live softens every parameter jump with a short S-curve (a triangle kernel,
    // two boxcars of ~2 ms); this is its causal twin.
    struct Smoother {
        double target = 0, out = 0, sum1 = 0, sum2 = 0;
        std::vector<double> ring1, ring2;
        std::size_t pos = 0, quiet = 0;
        bool active = false;
        void resize(std::size_t length);
        void jump(double v) noexcept;
        void retarget(double v) noexcept;
        double step() noexcept;
    };
    struct Detector {
        float env = 0;
    };
    void derive() noexcept;
    void designFilters() noexcept;
    [[nodiscard]] bool smoothed(Param) const noexcept;

    std::array<double, Count> value_{};
    std::array<Smoother, Count> smooth_{};
    std::array<HalfbandUp, 2> up_{}, upSc_{};
    std::array<HalfbandDown, 2> down_{}, downListen_{};
    std::array<BandSplit, 2> split_{}, splitSc_{};
    std::array<Detector, 3> detector_{};
    double rate_ = 48000, designedLow_ = -1, designedHigh_ = -1;
    std::size_t smoothing_ = 94, activeSmoothers_ = 0;
    int redesignCountdown_ = 0;
    // derived, refreshed when a parameter moves
    std::array<float, 3> inGain_{}, outGain_{};
    std::array<double, 3> aboveT_{}, aboveR_{}, belowT_{}, belowR_{};
    std::array<float, 3> attack_{}, release_{};
    float master_ = 1, scGain_ = 1, scDry_ = 0, scWet_ = 1;
};
} // namespace adi::dsp
