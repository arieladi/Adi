// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace adi::mix::key_eq {
enum class Shape : std::uint8_t {
    Bell = 0,
    LowShelf = 1,
    LowCut = 2,
    HighShelf = 3,
    HighCut = 4,
    Notch = 5,
    BandPass = 6,
    Tilt = 7,
    FlatTilt = 8,
    AllPass = 9
};
enum class Placement : std::uint8_t { Stereo, Mid, Side };
struct Band {
    bool enabled = false;
    float frequencyHz = 1000;
    float gainDb = 0;
    float q = 1;
    float slope = 1;
    Shape shape = Shape::Bell;
    Placement placement = Placement::Stereo;
    bool operator==(const Band &) const = default;
};
struct Settings {
    std::array<Band, 6> bands{};
    bool audition = false;
    bool operator==(const Settings &) const = default;
};
enum class MovingLane : std::uint8_t { None, FrequencyLog2, GainDb, Q };
enum class Status {
    Ok,
    InvalidArgument,
    UnsupportedConfiguration,
    NotPrepared,
    AllocationFailed,
    InvalidBlock
};
// A normalized target maps to the lane's exact float DSP coordinate. Values
// outside [0,1], including nonfinite values, leave output unchanged.
bool mapTarget(MovingLane, float normalized, float &output) noexcept;
bool validBand(const Band &) noexcept;
} // namespace adi::mix::key_eq
