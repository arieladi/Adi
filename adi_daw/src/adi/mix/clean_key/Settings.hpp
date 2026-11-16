// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/mix/compressor/Settings.hpp"
#include "adi/mix/key_eq/Settings.hpp"

namespace adi::mix::clean_key {
// Authored static controls, not a native controller or factory preset. The
// outer source is Internal and output is Programme. Existing child domains
// apply; routing, Auto controls and active Hold are excluded in this tier.
struct Settings {
    compressor::Settings dynamics{};
    key_eq::Settings equalizer{};
    key_eq::MovingLane movingLane = key_eq::MovingLane::None;
    bool operator==(const Settings &) const = default;
};
struct Target {
    key_eq::MovingLane lane = key_eq::MovingLane::None;
    float mappedValue = 0;
    bool operator==(const Target &) const = default;
};
} // namespace adi::mix::clean_key
