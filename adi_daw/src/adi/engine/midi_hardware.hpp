// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/events.hpp"
#include "adi/engine/mpe_output.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace adi::engine {

// The live driver supplies this only during its callback. The engine never
// opens a port or calls its backend. Words are MIDI 1.0 channel-voice UMPs.
struct MidiHardwareSink {
    void* context = nullptr;
    void (*send)(void*, std::uint64_t port, std::int64_t track, std::int64_t sample, std::uint32_t word) noexcept = nullptr;
    void (*reset)(void*) noexcept = nullptr;
};

// An internal release request has no UMP message type.
inline constexpr std::uint32_t kMidiHardwareRelease = 0x10000000u;

inline std::uint64_t midiPortKey(std::string_view port) noexcept {
    std::uint64_t key = 14695981039346656037ull;
    for (const char letter : port) {
        key ^= static_cast<unsigned char>(letter);
        key *= 1099511628211ull;
    }
    return key;
}

inline std::uint32_t midiHardwareWord(const Event& event, int channel) noexcept {
    if (channel < 1 || channel > 16 || !std::isfinite(event.value)) return 0;
    const auto ch = static_cast<std::uint32_t>(channel - 1);
    const auto value = static_cast<std::uint32_t>(std::lround(127.0 * std::clamp(event.value, 0.0, 1.0)));
    std::uint32_t status = 0, a = 0, b = 0;
    switch (event.type) {
        case EventType::NoteOn:
        case EventType::NoteOff:
            if (event.dim > 127) return 0;
            status = event.type == EventType::NoteOn ? 0x90u : 0x80u;
            a = event.dim;
            b = event.type == EventType::NoteOn ? std::max(1u, value) : value;
            break;
        case EventType::Controller:
            if (event.paramId > 127) return 0;
            status = 0xb0u;
            a = event.paramId;
            b = value;
            break;
        case EventType::NoteExpression:
            if (event.noteId != 0) return 0; // one conventional channel
            if (event.dim == static_cast<std::uint16_t>(ExpressionDim::Pitch)) {
                const auto bend = semitonesToBendWord(event.value, 2.0);
                status = 0xe0u;
                a = bend & 0x7fu;
                b = bend >> 7;
            } else if (event.dim == static_cast<std::uint16_t>(ExpressionDim::Pressure)) {
                status = 0xd0u;
                a = value;
            } else {
                return 0;
            }
            break;
        default:
            return 0;
    }
    return 0x20000000u | ((status | ch) << 16) | (a << 8) | b;
}
} // namespace adi::engine
