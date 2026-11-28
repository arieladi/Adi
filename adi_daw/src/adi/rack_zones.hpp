// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace adi::rack {

struct Zone {
    std::uint8_t low = 0, high = 127, lowFade = 0, highFade = 127;
    bool operator==(const Zone &) const = default;
};
enum class FadeLaw : std::uint8_t { Gain, Power };
enum class RackKind : std::uint8_t { Instrument, MidiEffect, AudioEffect, Drum };

struct Zones {
    // A SQL NULL is a present full-range Zone. An empty optional denotes
    // malformed persisted bytes, which cannot admit an applicable event.
    std::optional<Zone> key = Zone{}, velocity = Zone{}, selector = Zone{};
};

[[nodiscard]] constexpr std::optional<Zone> decodeZone(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != 4)
        return {};
    const Zone zone{std::to_integer<std::uint8_t>(bytes[0]), std::to_integer<std::uint8_t>(bytes[1]),
                    std::to_integer<std::uint8_t>(bytes[2]), std::to_integer<std::uint8_t>(bytes[3])};
    if (zone.low > zone.lowFade || zone.lowFade > zone.highFade ||
        zone.highFade > zone.high || zone.high > 127)
        return {};
    return zone;
}

/// A missing return is outside the zone. An in-range fade endpoint can have
/// zero amplitude. The caller can retain routing ownership independently.
[[nodiscard]] inline std::optional<double> zoneWeight(std::optional<Zone> zone, int value,
                                                       FadeLaw law) noexcept {
    if (!zone || value < zone->low || value > zone->high)
        return {};
    double weight = 1.0;
    if (value < zone->lowFade)
        weight = static_cast<double>(value - zone->low) / (zone->lowFade - zone->low);
    else if (value > zone->highFade)
        weight = static_cast<double>(zone->high - value) / (zone->high - zone->highFade);
    return law == FadeLaw::Power ? std::sqrt(weight) : weight;
}

struct ZoneWeights {
    double key = 1, velocity = 1, selector = 1;
    [[nodiscard]] double combined() const noexcept { return key * velocity * selector; }
};

/// The three applicable factors are kept explicit for MIDI velocity and
/// output-gain consumers. Audio Effect Racks use only selector. Drum Rack's
/// separate note mapping decides admission for its pad chains.
[[nodiscard]] inline std::optional<ZoneWeights>
evaluateZones(const Zones &zones, RackKind kind, int key, int velocity,
              int selector, FadeLaw law) noexcept {
    if (kind == RackKind::Drum)
        return ZoneWeights{};
    const auto select = zoneWeight(zones.selector, selector, law);
    if (!select)
        return {};
    if (kind == RackKind::AudioEffect)
        return ZoneWeights{1, 1, *select};
    const auto byKey = zoneWeight(zones.key, key, law);
    const auto byVelocity = zoneWeight(zones.velocity, velocity, law);
    if (!byKey || !byVelocity)
        return {};
    return ZoneWeights{*byKey, *byVelocity, *select};
}

} // namespace adi::rack
