// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace adi::ui::filled_spectrum {
inline constexpr double lowHz = 10., highHz = 20000.;
inline constexpr float topDb = -13.f, bottomDb = -80.f;
inline constexpr double framesPerSecond = 40.;
inline double logWidth() noexcept { return std::log(highHz / lowHz); }

inline float columnMagnitude(std::span<const float> magnitudes, double binHz, double fromHz, double toHz) noexcept {
    if (magnitudes.size() < 2 || !(binHz > 0.) || !(fromHz > 0.) || !(toHz >= fromHz)) return 0.f;
    const auto last = static_cast<double>(magnitudes.size() - 1);
    const auto firstBin = std::max(1., std::ceil(fromHz / binHz)); // skip DC on the logarithmic axis
    const auto lastBin = std::min(last, std::floor(toHz / binHz));
    if (firstBin <= lastBin) {
        const auto bins = magnitudes.subspan(static_cast<std::size_t>(firstBin),
                                             static_cast<std::size_t>(lastBin - firstBin + 1.));
        return *std::max_element(bins.begin(), bins.end());
    }
    if (fromHz / binHz > last) return 0.f; // the band is wholly above Nyquist
    const auto centreBin = std::clamp(std::sqrt(fromHz * toHz) / binHz, 1., last);
    const auto below = static_cast<std::size_t>(centreBin);
    return std::lerp(magnitudes[below], magnitudes[std::min(below + 1, magnitudes.size() - 1)],
                     static_cast<float>(centreBin - static_cast<double>(below)));
}

struct Cursor {
    double hz, levelDb;
    const char *note;
    int octave, cents;
};

struct Note { const char *name; int octave, cents; };

inline Note noteAt(double hz) noexcept {
    constexpr std::array notes{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const auto pitch = 69. + 12. * std::log2(hz / 440.);
    const auto midi = static_cast<int>(std::floor(pitch + .5));
    const auto pitchClass = static_cast<std::size_t>((midi % 12 + 12) % 12);
    // Live's numbering assigns C3 to MIDI 60.
    const auto octave = static_cast<int>(std::floor(static_cast<double>(midi) / 12.)) - 2;
    const auto cents = static_cast<int>(std::floor((pitch - midi) * 100. + .5));
    return {notes[pitchClass], octave, cents};
}

inline Cursor cursorAt(double horizontal, double vertical) noexcept {
    const auto hz = lowHz * std::exp(logWidth() * std::clamp(horizontal, 0., 1.));
    const auto note = noteAt(hz);
    const auto db = std::lerp(static_cast<double>(topDb), static_cast<double>(bottomDb), std::clamp(vertical, 0., 1.));
    return {hz, db, note.name, note.octave, note.cents};
}
} // namespace adi::ui::filled_spectrum
