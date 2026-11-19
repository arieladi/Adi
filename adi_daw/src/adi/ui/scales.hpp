// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The scales the Clip View's Scale chooser offers (Live 12 manual 8.2.5), headless: each a name, the
// id a clip stores, and its pitches as semitones above the root. The names and their order follow
// Live 12's chooser as far as the manual shows them; they are to be read again on Live (ADR-0108).
//
// A clip keeps the scale as a root (0 to 11), the id, and a 12-bit mask relative to the root, bit n
// set when the pitch n semitones above the root belongs (`clip.setScale`, N33).
#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace adi::ui {
struct ScaleName {
    const char *id;
    const char *name;
    std::uint16_t mask;
};
constexpr std::uint16_t scaleMask(std::initializer_list<int> semitones) noexcept {
    std::uint16_t mask = 0;
    for (const int s : semitones)
        mask = static_cast<std::uint16_t>(mask | (1u << (((s % 12) + 12) % 12)));
    return mask;
}
inline constexpr std::array<ScaleName, 35> kScales{{
    {"major", "Major", scaleMask({0, 2, 4, 5, 7, 9, 11})},
    {"minor", "Minor", scaleMask({0, 2, 3, 5, 7, 8, 10})},
    {"dorian", "Dorian", scaleMask({0, 2, 3, 5, 7, 9, 10})},
    {"mixolydian", "Mixolydian", scaleMask({0, 2, 4, 5, 7, 9, 10})},
    {"lydian", "Lydian", scaleMask({0, 2, 4, 6, 7, 9, 11})},
    {"phrygian", "Phrygian", scaleMask({0, 1, 3, 5, 7, 8, 10})},
    {"locrian", "Locrian", scaleMask({0, 1, 3, 5, 6, 8, 10})},
    {"whole-tone", "Whole Tone", scaleMask({0, 2, 4, 6, 8, 10})},
    {"half-whole-dim", "Half-whole Dim.", scaleMask({0, 1, 3, 4, 6, 7, 9, 10})},
    {"whole-half-dim", "Whole-half Dim.", scaleMask({0, 2, 3, 5, 6, 8, 9, 11})},
    {"minor-blues", "Minor Blues", scaleMask({0, 3, 5, 6, 7, 10})},
    {"minor-pentatonic", "Minor Pentatonic", scaleMask({0, 3, 5, 7, 10})},
    {"major-pentatonic", "Major Pentatonic", scaleMask({0, 2, 4, 7, 9})},
    {"harmonic-minor", "Harmonic Minor", scaleMask({0, 2, 3, 5, 7, 8, 11})},
    {"harmonic-major", "Harmonic Major", scaleMask({0, 2, 4, 5, 7, 8, 11})},
    {"dorian-4", "Dorian #4", scaleMask({0, 2, 3, 6, 7, 9, 10})},
    {"phrygian-dominant", "Phrygian Dominant", scaleMask({0, 1, 4, 5, 7, 8, 10})},
    {"melodic-minor", "Melodic Minor", scaleMask({0, 2, 3, 5, 7, 9, 11})},
    {"lydian-augmented", "Lydian Augmented", scaleMask({0, 2, 4, 6, 8, 9, 11})},
    {"lydian-dominant", "Lydian Dominant", scaleMask({0, 2, 4, 6, 7, 9, 10})},
    {"super-locrian", "Super Locrian", scaleMask({0, 1, 3, 4, 6, 8, 10})},
    {"8-tone-spanish", "8-Tone Spanish", scaleMask({0, 1, 3, 4, 5, 6, 8, 10})},
    {"bhairav", "Bhairav", scaleMask({0, 1, 4, 5, 7, 8, 11})},
    {"hungarian-minor", "Hungarian Minor", scaleMask({0, 2, 3, 6, 7, 8, 11})},
    {"hirajoshi", "Hirajoshi", scaleMask({0, 2, 3, 7, 8})},
    {"in-sen", "In-Sen", scaleMask({0, 1, 5, 7, 10})},
    {"iwato", "Iwato", scaleMask({0, 1, 5, 6, 10})},
    {"kumoi", "Kumoi", scaleMask({0, 2, 3, 7, 9})},
    {"pelog-selisir", "Pelog Selisir", scaleMask({0, 1, 3, 7, 8})},
    {"pelog-tembung", "Pelog Tembung", scaleMask({0, 1, 5, 7, 8})},
    {"messiaen-3", "Messiaen 3", scaleMask({0, 2, 3, 4, 6, 7, 8, 10, 11})},
    {"messiaen-4", "Messiaen 4", scaleMask({0, 1, 2, 5, 6, 7, 8, 11})},
    {"messiaen-5", "Messiaen 5", scaleMask({0, 1, 5, 6, 7, 11})},
    {"messiaen-6", "Messiaen 6", scaleMask({0, 2, 4, 5, 6, 8, 10, 11})},
    {"messiaen-7", "Messiaen 7", scaleMask({0, 1, 2, 3, 5, 6, 7, 8, 9, 11})},
}};
// The catalogue's scale with this id; none for an id it does not hold (a file from elsewhere).
constexpr const ScaleName *findScale(std::string_view id) noexcept {
    for (const auto &s : kScales)
        if (id == s.id)
            return &s;
    return nullptr;
}
// Whether `key` belongs to the scale `mask` on `root`.
constexpr bool inScale(int key, int root, std::int64_t mask) noexcept {
    const int degree = (((key - root) % 12) + 12) % 12;
    return ((mask >> degree) & 1) != 0;
}
// The key of the scale nearest to `key`: itself when it belongs, else the nearer neighbour, the lower
// one when both are as near; within the 128 keys.
constexpr int nearestInScale(int key, int root, std::int64_t mask) noexcept {
    if ((mask & 0xfff) == 0)
        return key;
    for (int distance = 0; distance < 12; ++distance) {
        if (key - distance >= 0 && inScale(key - distance, root, mask))
            return key - distance;
        if (key + distance <= 127 && inScale(key + distance, root, mask))
            return key + distance;
    }
    return key;
}
} // namespace adi::ui
