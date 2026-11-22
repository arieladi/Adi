// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Spectral Resonator, the core (DEV-22; lands after 3 December):
// the input's spectrum rings at the partials of a pitch.
//
//   Frequency   20 Hz to 5 kHz: the first partial.
//   Harmonics   1 to 64: how many partials ring.
//   Stretch     -100 to +100 %: partial n sits at Frequency x n^(1 + 0.1
//               Stretch / 100) (0: harmonic; up, a stiff string's spread).
//   Decay       20 ms to 20 s: a partial's ring falls 60 dB in this time.
//   Brightness  -100 to +100 %: partial n's level times n^(Brightness/100).
//   Dry/Wet     0 to 100 %.
//
// On a periodic Hann STFT at a hop of a quarter window (rebuilt exactly, as
// Spectral Time's), its window 2,048 points at 44.1 and 48 kHz and scaled with
// the rate (4,096 at 96, 8,192 at 192 kHz, so a bin spans 23 Hz at any rate), each partial owns the bins within one bin of it. In
// them the output's magnitude is the larger of the input's and the last
// frame's times the Decay's fall per hop, so a struck partial rings on and
// fades; while it rings every one of its bins advances at the partial's own
// frequency, so they stay together and the ring is a tone at the partial.
// The other bins are silent.
// The latency is one window, reported, and the dry signal is delayed with
// it. prepare() allocates and never throws: when memory cannot be had the
// device is inert, passing its input with no latency reported. Processing
// does not allocate. An input sample is bounded at 1e6, so no window's
// transform can overflow into a ring. Dry/Wet glides over about 10 ms once
// audio runs (dsp/glide.hpp).
//
// WHAT IS NOT LIVE'S YET, and says so: Spectral Resonator's MIDI and
// internal modes, Shift, Unison, the modulation and polyphony, the laws of
// Stretch, Brightness and the ring, and every range and default wait on a
// Live 12.4 reading of Spectral Resonator (the devices log's ONE list). It
// claims no parity.

#pragma once

#include "adi/dsp/glide.hpp"
#include "adi/dsp/spectrum.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace adi::dsp {

class SpectralResonator {
public:
    enum Param { Frequency, Harmonics, Stretch, Decay, Brightness, DryWet, Count };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{20, 5000, 220}, {1, 64, 16}, {-100, 100, 0}, {20, 20000, 2000}, {-100, 100, 0}, {0, 100, 50}};
    /// The window at 44.1 and 48 kHz; at the rate set, size().
    static constexpr std::size_t kSize = 2048;

    SpectralResonator();
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return ready_ ? static_cast<int>(size_) : 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// Bin k's weight as set (0: silent), for tests.
    [[nodiscard]] float weight(std::size_t k) const noexcept { return weights_[k]; }
    /// Partial n's frequency (from 1), for tests.
    [[nodiscard]] double partial(int n) const noexcept;

private:
    struct Lane {
        std::vector<float> in, acc, dry;
        std::vector<float> magnitude, phase, lastPhase;
    };
    void update() noexcept;
    void frame(Lane&) noexcept;
    std::array<double, Count> p_{};
    std::unique_ptr<RealFft> fft_;
    std::array<Lane, 2> lanes_;
    std::vector<float> window_, scratch_, weights_, advance_;   ///< advance_: a ringing bin's phase per hop
    std::vector<std::complex<float>> bins_, out_;
    std::size_t written_ = 0, size_ = 0, hop_ = 1, binCount_ = 1;
    double rate_ = 48000, fall_ = 0;
    Glide wet_;
    bool ready_ = false;
};

}  // namespace adi::dsp
