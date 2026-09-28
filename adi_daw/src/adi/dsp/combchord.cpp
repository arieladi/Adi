// SPDX-License-Identifier: GPL-3.0-or-later
// Reference read: Surge XT CombulatorEffect (GPL-3.0-or-later), parallel
// bipolar combs. This implementation is original: no Surge code is copied.
// Allpass transfer function: Julius O. Smith, Physical Audio Signal Processing,
// https://www.dsprelated.com/freebooks/pasp/First_Order_Allpass_Interpolation.html
#include "adi/dsp/combchord.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
double bounded(double x, double lo, double hi, double fallback) noexcept {
    return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
}
double clean(double x) noexcept {
    // 1e-30 is -600 dB amplitude, far below float audio; kill recursive tails
    // before double denormals, independent of the host's floating-point mode.
    return std::abs(x) < 1e-30 ? 0 : x;
}
}
CombChord::CombChord() noexcept {
    // MIDI A2 minor, doubled across octaves. All slots start with the same
    // neutral preset until device state supplies its eight actual chords.
    for (auto& c : chords_) c = {45, 48, 52, 57, 60, 64};
}
void CombChord::prepare(double sampleRate) {
    rate_ = bounded(sampleRate, 44100, 768000, 48000);
    // Lowest supported note is 40 Hz; two guard samples cover rounding.
    const auto size = static_cast<std::size_t>(std::ceil(rate_ / 40)) + 2;
    for (auto& b : banks_) for (auto& v : b.resonators) v.line.assign(size, 0);
    fadeSamples_ = static_cast<std::size_t>(std::llround(rate_ * 0.010));
    reset();
}
void CombChord::clear(Bank& bank) noexcept {
    for (auto& v : bank.resonators) {
        std::fill(v.line.begin(), v.line.end(), 0);
        v.previousInput = v.previousOutput = v.lowpass = 0;
    }
}
void CombChord::reset() noexcept {
    write_ = 0;
    remaining_ = 0;
    pending_ = processed_ = resetRequested_ = false;
    for (auto& bank : banks_) { tune(bank); clear(bank); }
}
void CombChord::changed(bool resetBank) noexcept {
    if (!processed_) tune(banks_[active_]);
    else { pending_ = true; resetRequested_ |= resetBank; }
}
void CombChord::beginFade() noexcept {
    auto& next = banks_[1 - active_];
    tune(next);
    if (resetRequested_) clear(next);
    for (std::size_t i = 0; i < voices; ++i) {
        const auto& old = banks_[active_].resonators[i];
        auto& voice = next.resonators[i];
        if (!resetRequested_) {
            // Color/Decay preserve the ringing history. Restarting it every
            // control update would inject a new delayed step once per period.
            std::copy(old.line.begin(), old.line.end(), voice.line.begin());
            voice.previousInput = old.previousInput;
            voice.previousOutput = old.previousOutput;
            voice.lowpass = old.lowpass;
        }
        if (!resetRequested_ || voice.delay == old.delay) voice.allpass = old.allpass;
    }
    resetRequested_ = false;
    remaining_ = fadeSamples_;
    pending_ = false;
}
void CombChord::setChord(std::size_t state, const Chord& notes) noexcept {
    if (state >= states) return;
    // MIDI frequency definition: A4=440 Hz, note 69, twelve semitones/octave.
    const double lo = 69 + 12 * std::log2(40.0 / 440);
    const double hi = 69 + 12 * std::log2(1500.0 / 440);
    for (std::size_t i = 0; i < voices; ++i)
        chords_[state][i] = bounded(notes[i], lo, hi, 69);
    if (state == state_) changed(true);
}
const CombChord::Chord& CombChord::chord(std::size_t state) const noexcept {
    return chords_[std::min(state, states - 1)];
}
void CombChord::setState(std::size_t state) noexcept {
    state = std::min(state, states - 1);
    if (state != state_) { state_ = state; changed(true); }
}
void CombChord::setMode(Mode mode) noexcept {
    if (mode != mode_) { mode_ = mode; changed(true); }
}
void CombChord::setDecay(double seconds) noexcept {
    decay_ = bounded(seconds, 0.05, 20, 1); // measured domain in test_combchord
    changed();
}
void CombChord::setColor(double color) noexcept {
    color_ = bounded(color, 0, 1, 0);
    changed();
}
void CombChord::setMix(double mix) noexcept { mix_ = bounded(mix, 0, 1, 1); }
void CombChord::setOutputDb(double db) noexcept {
    // Amplitude definition of dB; bounded to a conventional -60..+12 dB trim.
    output_ = std::pow(10.0, bounded(db, -60, 12, 0) / 20);
}
void CombChord::tune(Bank& bank) noexcept {
    const double pole = color_ / 2;
    bank.pole = pole;
    for (std::size_t i = 0; i < voices; ++i) {
        auto& v = bank.resonators[i];
        const double hz = 440 * std::exp2((chords_[state_][i] - 69) / 12);
        const double w = 2 * std::numbers::pi * hz / rate_;
        const double period = rate_ / hz / (mode_ == Mode::Square ? 2 : 1);
        // H(z)=(1-p)/(1-p z^-1). Subtract -arg(H)/w from the delay.
        const double phaseDelay = std::atan2(pole * std::sin(w), 1 - pole * std::cos(w)) / w;
        const double delay = period - phaseDelay;
        v.delay = static_cast<std::size_t>(std::floor(delay - 0.5));
        const double fraction = delay - static_cast<double>(v.delay);
        // Keep the fractional delay in [0.5, 1.5): no near-cancelled pole.
        // First-order allpass interpolation: unity magnitude preserves decay,
        // unlike linear interpolation's frequency-dependent loop loss. Solve
        // arg((a+z^-1)/(1+a z^-1))=-w*fraction exactly at the note.
        v.allpass = std::sin(w * (1 - fraction) / 2)
                  / std::sin(w * (1 + fraction) / 2);
        v.targetAllpass = v.allpass;
        // A -60 dB amplitude ratio is 10^-3. period includes filter phase;
        // the unity-DC Color filter can only REDUCE loop magnitude, never
        // destabilize it. Finite 20 s maximum guarantees |gain|<1.
        v.gain = std::pow(10.0, -3 * period / (rate_ * decay_));
        if (mode_ == Mode::Square) v.gain = -v.gain;
    }
}
double CombChord::tick(Bank& bank, double x, bool smooth) noexcept {
    const auto size = bank.resonators[0].line.size();
    double sum = 0;
    for (auto& v : bank.resonators) {
        if (smooth) v.allpass += (v.targetAllpass - v.allpass) / static_cast<double>(remaining_);
        const auto index = (write_ + size - v.delay) % size;
        const double d = v.line[index];
        const double delayed = clean(v.allpass * d + v.previousInput - v.allpass * v.previousOutput);
        v.previousInput = d;
        v.previousOutput = delayed;
        v.lowpass = clean((1 - bank.pole) * delayed + bank.pole * v.lowpass);
        const double y = clean((1 - std::abs(v.gain)) * x + v.gain * v.lowpass);
        v.line[write_] = y;
        sum += y;
    }
    return sum / voices;
}
void CombChord::process(const float* input, float* output, std::size_t frames) noexcept {
    if (banks_[active_].resonators[0].line.empty()) {
        std::copy_n(input, frames, output);
        return;
    }
    const auto size = banks_[active_].resonators[0].line.size();
    for (std::size_t n = 0; n < frames; ++n) {
        if (pending_ && !remaining_) beginFade();
        const double x = std::isfinite(input[n]) ? input[n] : 0;
        double wet = tick(banks_[active_], x, false);
        if (remaining_) {
            const double next = tick(banks_[1 - active_], x, true);
            const double alpha = static_cast<double>(fadeSamples_ - remaining_ + 1)
                               / static_cast<double>(fadeSamples_);
            wet += alpha * (next - wet);
            if (--remaining_ == 0) active_ = 1 - active_;
        }
        output[n] = static_cast<float>(output_ * ((1 - mix_) * x + mix_ * wet));
        if (++write_ == size) write_ = 0;
    }
    if (frames) processed_ = true;
}
} // namespace adi::dsp
