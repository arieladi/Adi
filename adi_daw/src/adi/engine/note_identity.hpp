// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace adi::engine {

/// Allocate on the owner before publishing the event producer.
std::uint32_t allocateEventSourcePrefix();

[[nodiscard]] constexpr std::uint64_t eventIdentity(std::uint32_t prefix, std::uint32_t serial) noexcept {
    return (std::uint64_t{prefix} << 32) | serial;
}

[[nodiscard]] constexpr std::uint32_t eventSourcePrefix(std::uint64_t identity) noexcept {
    return static_cast<std::uint32_t>(identity >> 32);
}

/// One map per native instance. Slots can be reused, but a late release or
/// NOTE_END must never name a different voice, so the positive wire IDs
/// remain monotone even when an instance becomes idle.
class PluginNoteIds {
public:
    static constexpr std::size_t capacity = 256;

    [[nodiscard]] std::optional<std::int32_t> assign(std::uint64_t engineId) noexcept;
    [[nodiscard]] std::optional<std::int32_t> lookup(std::uint64_t engineId) const noexcept;
    bool release(std::uint64_t engineId) noexcept;
    bool noteEnd(std::int32_t wire) noexcept;

private:
    struct Voice {
        std::uint64_t engineId = 0;
        std::int32_t wire = -1;
    };
    std::array<Voice, capacity> voices_{};
    std::uint32_t next_ = 1;
};

}  // namespace adi::engine
