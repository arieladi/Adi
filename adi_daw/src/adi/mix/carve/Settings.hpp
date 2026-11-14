// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>

namespace adi::mix::carve {
enum class Mode : std::uint8_t { Stereo, MidSide };
struct Settings {
    bool enabled = true;
    // Normalized amount/pan in [0,1]; pan weights left/right or mid/side.
    float amount = .15f, pan = .5f;
    // Attack [.01,100] ms; release [1,700] ms.
    float attackMs = 5.f, releaseMs = 30.f;
    // Physical Hz in [20,20000]. Crossed/equal edges use a 1 Hz minimum span.
    double lowHz = 20., highHz = 20000.;
    Mode mode = Mode::Stereo;
    bool freeze = false, audition = false;
    bool operator==(const Settings &) const = default;
};
bool valid(const Settings &) noexcept;
} // namespace adi::mix::carve
