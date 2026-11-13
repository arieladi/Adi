// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace adi::mix::compressor {

enum class KeySource { Internal, External };
enum class LinkMode { Mid, Side, MidToSide, SideToMid };

// Clean compression at the input sample rate, with zero latency. This component
// has no other style, oversampling, lookahead, hold, key EQ, audition or wet/dry
// controls. Its neutral defaults are component defaults, not a factory preset.
struct Settings {
    // Physical units: threshold [-60, 0] dB, ratio [1, 100] (100 is infinity
    // in the dynamic transfer), knee [0, 72] dB, reduction range [0, 60] dB.
    float thresholdDb = 0.0f;
    float ratio = 1.0f;
    float kneeDb = 0.0f;
    float rangeDb = 60.0f;
    // Canonical unit controls, not milliseconds. ControlMapping describes the
    // coupled manual attack/release times; automatic release uses the same unit
    // release control to scale its evolving envelope-dependent duration.
    float attack = 0.5f;
    float release = 0.5f;
    bool autoRelease = false;
    bool autoGain = false;
    KeySource keySource = KeySource::Internal;
    // 0 = separate L/R, 1 = linked maximum, 2 = selected M/S detector.
    float stereoLink = 1.0f;
    LinkMode linkMode = LinkMode::Mid;
    bool operator==(const Settings &) const = default;
};

[[nodiscard]] bool validSettings(const Settings &) noexcept;

} // namespace adi::mix::compressor
