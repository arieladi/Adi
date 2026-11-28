// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace adi::rows {
struct MidiOutput {
    std::string port; // the direction-qualified identifier in MIDI Settings
    int channel = 1;
    bool operator==(const MidiOutput&) const = default;
};

// A reserved hardware-out row stores its port and channel as tagged CBOR in
// channel_map. Existing audio channel maps have no MIDI keys.
std::optional<MidiOutput> decodeMidiOutput(std::span<const std::byte>);
} // namespace adi::rows
