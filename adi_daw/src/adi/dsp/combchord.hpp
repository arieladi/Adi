// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <vector>

namespace adi::dsp {
// ADR-0192: six voices and eight stored chords. One mono core per channel.
// All setters and process are serialized by the caller on the audio thread;
// prepare is the only allocating operation. State changes reset the resonators
// (a discrete chord selection, not a pitch glide).
class CombChord {
public:
    static constexpr std::size_t voices = 6, states = 8;
    using Chord = std::array<double, voices>;
    enum class Mode { Saw, Square };
    CombChord() noexcept;
    // Supported rates follow the engine, 44100..768000 Hz (ADR-0157).
    void prepare(double sampleRate);
    void reset() noexcept;
    void setChord(std::size_t state, const Chord& midiNotes) noexcept;
    [[nodiscard]] const Chord& chord(std::size_t state) const noexcept;
    void setState(std::size_t state) noexcept;
    void setMode(Mode mode) noexcept;
    // 0.05..20 seconds; Color adds frequency-dependent damping to this
    // unfiltered T60. It cannot preserve T60 at every partial while low-passing.
    void setDecay(double seconds) noexcept;
    // 0..1 maps the loop pole from 0 to 1/2: unity DC, <=1 magnitude.
    void setColor(double color) noexcept;
    void setMix(double mix) noexcept;
    void setOutputDb(double db) noexcept;
    void process(const float* input, float* output, std::size_t frames) noexcept;
private:
    struct Voice {
        std::vector<double> line;
        std::size_t delay = 1;
        double allpass = 0, previousInput = 0, previousOutput = 0, lowpass = 0, gain = 0;
        bool fractional = false;
    };
    void tune() noexcept;
    std::array<Chord, states> chords_{};
    std::array<Voice, voices> bank_{};
    std::size_t state_ = 0, write_ = 0;
    double rate_ = 48000, decay_ = 1, color_ = 0, mix_ = 1, output_ = 1;
    Mode mode_ = Mode::Saw;
};
} // namespace adi::dsp
