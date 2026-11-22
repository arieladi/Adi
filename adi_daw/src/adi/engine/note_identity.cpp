// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/note_identity.hpp"

#include <atomic>
#include <limits>
#include <stdexcept>

namespace adi::engine {

std::uint32_t allocateEventSourcePrefix() {
    static std::atomic<std::uint64_t> next{1};
    const auto value = next.fetch_add(1, std::memory_order_relaxed);
    if (value > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("event source identity exhausted");
    return static_cast<std::uint32_t>(value);
}

std::optional<std::int32_t> PluginNoteIds::assign(std::uint64_t engineId) noexcept {
    if (!engineId) return std::nullopt;
    Voice* available = nullptr;
    for (auto& voice : voices_) {
        if (voice.engineId == engineId) return voice.wire;
        if (!voice.engineId && !available) available = &voice;
    }
    if (!available || next_ > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        return std::nullopt;
    const auto wire = static_cast<std::int32_t>(next_++);
    *available = {engineId, wire};
    return wire;
}

std::optional<std::int32_t> PluginNoteIds::lookup(std::uint64_t engineId) const noexcept {
    if (!engineId) return std::nullopt;
    for (const auto& voice : voices_) {
        if (voice.engineId == engineId) return voice.wire;
    }
    return std::nullopt;
}

bool PluginNoteIds::release(std::uint64_t engineId) noexcept {
    if (!engineId) return false;
    for (auto& voice : voices_) {
        if (voice.engineId == engineId) {
            voice = {};
            return true;
        }
    }
    return false;
}

bool PluginNoteIds::noteEnd(std::int32_t wire) noexcept {
    if (wire < 1) return false;
    for (auto& voice : voices_) {
        if (voice.wire == wire) {
            voice = {};
            return true;
        }
    }
    return false;
}

}  // namespace adi::engine
