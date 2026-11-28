// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/engine/graph.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adi::engine::clap_wire {

// Version 1 of ADI's block payload for native plug-in servers. The caller
// negotiates this protocol, authenticates the peer and orders replies.
inline constexpr std::size_t kBlockHeaderBytes = 32;
inline constexpr std::uint32_t kMaxChannels = 64;
inline constexpr std::uint32_t kMaxFrames = 8192;
inline constexpr std::uint32_t kMaxEvents = 4096;
inline constexpr std::size_t kMaxBlockPacketBytes =
    kBlockHeaderBytes + 96 + 4 * kMaxChannels * kMaxFrames + 56 * kMaxEvents;

/// These wire values are independent of the CLAP SDK's event header type.
enum class Kind : std::uint16_t {
    NoteOn = 1, NoteOff = 2, NoteChoke = 3, NoteEnd = 4,
    NoteExpression = 5, ParamValue = 6, ParamMod = 7,
    ParamGestureBegin = 8, ParamGestureEnd = 9,
    Controller = 10, Midi1 = 11, Midi2 = 12,
    // ADI's normalized automation and modulation delta convert to CLAP plain
    // units using the hosted instance's parameter range at the receiving end.
    NormalizedParamValue = 13, NormalizedParamMod = 14,
};

struct Event {
    Kind kind = Kind::NoteOn;
    std::uint32_t frame = 0; ///< offset within this packet
    std::uint32_t flags = 0; ///< CLAP_EVENT_IS_LIVE and CLAP_EVENT_DONT_RECORD
    std::int32_t noteId = -1;
    // Signed note port or unsigned raw MIDI port. -1 denotes a wildcard where
    // allowed by CLAP. Addresses are scoped to this process/plug-in instance.
    std::int32_t port = -1;
    std::int16_t channel = -1;
    std::int16_t key = -1;
    // CLAP expression ID, CLAP parameter ID, or controller number, by kind.
    std::uint32_t selector = 0;
    // CLAP velocity, plain expression/value/modulation, or normalized
    // controller. The codec preserves the double bits across the wire.
    double value = 0;
    // MIDI1 packs the three bytes from least to most significant in word 0.
    // MIDI2 carries the four UMP words as integers. Other kinds leave this zero.
    std::array<std::uint32_t, 4> midi{};
    bool operator==(const Event&) const = default;
};

struct Block {
    std::uint64_t sequence = 0;
    std::uint32_t channels = 0;
    std::uint32_t frames = 0;
    std::optional<DeviceTransport> transport;
    /// Packets are ordered by frame. Equal-frame insertion order is significant.
    std::vector<Event> events;
    /// Planar float32 PCM. Zero channels is valid for instrument input and
    /// event-only paths. The connection carries the sample rate separately.
    std::vector<float> audio;
};

/// Read only the fixed header and validate the declared shape and length
/// before allocating a packet buffer or reading a network payload.
[[nodiscard]] bool blockPacketSize(std::span<const std::byte> header, std::size_t& size, std::string& error);

/// Worker-side only. Failure leaves the caller's output unchanged.
[[nodiscard]] bool encodeBlock(const Block& block, std::vector<std::byte>& bytes, std::string& error);
[[nodiscard]] bool decodeBlock(std::span<const std::byte> bytes, Block& block, std::string& error);

} // namespace adi::engine::clap_wire
