// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace adi::mix::gate {
enum class Style { Classic, Clean, Vocal, Guitar, Upward, Ducking };
enum class KeySource { Internal, External };

// Physical controls, fixed for a prepared stream. These convenience defaults
// are not a controller factory preset or a normalized parameter-state format.
struct Settings {
    Style style = Style::Classic;
    KeySource keySource = KeySource::Internal;
    float thresholdDb = -30;
    float ratio = 4;
    float rangeDb = 60;
    float attackSeconds = .01f;
    float releaseSeconds = .1f;
    float holdMs = 0;
    float kneeDb = 0;
    // -1 selects left, 0 takes the maximum, +1 selects right. Each output has
    // its own mix of the two independently followed and held key envelopes.
    float leftKeyMix = 0;
    float rightKeyMix = 0;
    bool operator==(const Settings &) const = default;
};
} // namespace adi::mix::gate
