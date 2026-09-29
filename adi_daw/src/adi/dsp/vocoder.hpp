/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2024, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace adi::dsp {
// Scalar lane of Surge's two-stage VectorizedSVFilter. Two substeps keep
// the broad/high Live-style bank stable without dependence on host blocks.
struct VocoderBand {
    double l1 = 0, b1 = 0, l2 = 0, b2 = 0, f1 = 0, f2 = 0, q = 1;
    void coefficients(double hz, double quality, double sampleRate) noexcept;
    double step(double input) noexcept;
};
class Vocoder {
  public:
    enum Param {
        Carrier,
        NoiseRate,
        NoiseDensity,
        PitchLow,
        PitchHigh,
        Waveform,
        Pitch,
        Enhance,
        Unvoiced,
        Sensitivity,
        Fast,
        Bands,
        Low,
        High,
        Bandwidth,
        Retro,
        Gate,
        Level,
        Depth,
        Attack,
        Release,
        Channels,
        Formant,
        Mix,
        Band1,
        Band2,
        Band3,
        Band4,
        Band5,
        Band6,
        Band7,
        Band8,
        Band9,
        Band10,
        Band11,
        Band12,
        Band13,
        Band14,
        Band15,
        Band16,
        Band17,
        Band18,
        Band19,
        Band20,
        Count
    };
    Vocoder() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    void process(const float *left, const float *right, float *outLeft, float *outRight,
                 std::size_t frames, const float *carrierLeft = nullptr,
                 const float *carrierRight = nullptr) noexcept;
    double trackedHz() const noexcept { return pitchHz_; }
    double envelope(std::size_t band, std::size_t channel = 0) const noexcept {
        return envelopes_[channel][band];
    }

  private:
    void update() noexcept;
    void track(double input) noexcept;
    double random() noexcept;
    std::array<double, Count> p_{};
    std::array<std::array<VocoderBand, 20>, 2> mod_{}, carrier_{};
    std::array<std::array<double, 20>, 2> envelopes_{}, carrierPower_{};
    std::array<double, 20> gains_{};
    std::array<double, 1024> pitchBuffer_{};
    std::size_t pitchWrite_ = 0, pitchCount_ = 0;
    unsigned decimation_ = 4, decimationPhase_ = 0, trackerClock_ = 0;
    std::uint32_t seed_ = 0x12345678u;
    double sr_ = 48000, pitchHz_ = 220, oscPhase_ = 0, noisePhase_ = 1, noise_ = 0, pitchLP_ = 0;
    double detectorLP_ = 0, detectorPower_ = 0, detectorHigh_ = 0, unvoiced_ = 0;
    double attack_ = 0, release_ = 0, gate_ = 0, level_ = 1, detectorRate_ = 0;
    int bands_ = 20;
    bool dirty_ = true;
};
} // namespace adi::dsp
