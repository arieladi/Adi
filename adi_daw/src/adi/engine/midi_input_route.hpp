// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace adi::engine {

struct MidiInputChoice {
    std::string_view port;
    int channel = 0; // 0 is all channels, 1 to 16 are MIDI channel numbers
};

inline MidiInputChoice midiInputChoice(std::string_view ref) noexcept {
    constexpr std::string_view prefix = "midi-input:";
    if (!ref.starts_with(prefix))
        return {};
    ref.remove_prefix(prefix.size());
    const auto separator = ref.find(':');
    if (separator == std::string_view::npos)
        return {};
    int channel = 0;
    const auto end = ref.data() + separator;
    const auto parsed = std::from_chars(ref.data(), end, channel);
    if (parsed.ec != std::errc{} || parsed.ptr != end || channel < 0 || channel > 16)
        return {};
    return {ref.substr(separator + 1), channel};
}

inline std::string midiInputRef(MidiInputChoice choice) {
    return "midi-input:" + std::to_string(choice.channel) + ":" + std::string(choice.port);
}

// Keep the hardware endpoint identifier off the audio callback. A zero key
// denotes an event that did not arrive through a named physical endpoint.
inline constexpr std::uint64_t midiInputKey(std::string_view port) noexcept {
    if (port.empty())
        return 0;
    std::uint64_t key = 14695981039346656037ull;
    for (const auto byte : port) {
        key ^= static_cast<unsigned char>(byte);
        key *= 1099511628211ull;
    }
    return key == 0 ? 1 : key;
}

struct MidiInputFilter {
    std::uint64_t port = 0;
    int channel = 0;

    [[nodiscard]] bool matches(std::uint64_t source, int sourceChannel) const noexcept {
        return (port == 0 || port == source) && (channel == 0 || channel == sourceChannel + 1);
    }
    bool operator==(const MidiInputFilter&) const = default;
};

inline MidiInputFilter midiInputFilter(const std::optional<std::string>& ref) noexcept {
    const auto choice = midiInputChoice(ref ? std::string_view(*ref) : std::string_view{});
    return {midiInputKey(choice.port), choice.channel};
}

} // namespace adi::engine
