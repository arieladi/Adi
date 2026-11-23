// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-09: self-contained OSC 1.0 packet decoder (messages and bundles).
//
// Decodes untrusted bytes received over UDP into structured OSC messages and
// bundles without reading outside the input span, enforcing 4-byte alignment,
// zero-padded strings/blobs, bundle time tags, and explicit bounds on packet
// size, bundle nesting depth, total elements, and per-message arguments.
//
// Encoding and the UDP read loop belong to the network stack (PLAN SRF-09).
// This unit has no dependencies outside the C++ standard library so its fuzz
// harness (tests/fuzz_osc_codec.cpp) compiles against osc_codec.cpp alone.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace adi::osc {

/// Explicit resource caps protecting the decoder against hostile datagrams.
struct DecodeLimits {
    std::size_t maxPacketBytes = 65536;
    std::size_t maxBundleDepth = 8;
    std::size_t maxElements = 256;
    std::size_t maxArguments = 64;
};

enum class DecodeError {
    None = 0,
    EmptyPacket,
    PacketTooLarge,
    MisalignedPacket,
    Truncated,
    InvalidAddress,
    InvalidPadding,
    MissingTypeTag,
    UnsupportedTypeTag,
    TooManyArguments,
    InvalidBlobSize,
    TrailingBytes,
    InvalidBundleHeader,
    InvalidBundleElementSize,
    BundleDepthExceeded,
    TooManyElements,
};

[[nodiscard]] std::string_view toString(DecodeError) noexcept;

/// OSC 1.0 64-bit NTP time tag. Raw value 1 means "immediately".
struct TimeTag {
    std::uint64_t raw = 1;

    [[nodiscard]] constexpr std::uint32_t seconds() const noexcept {
        return static_cast<std::uint32_t>(raw >> 32U);
    }
    [[nodiscard]] constexpr std::uint32_t fraction() const noexcept {
        return static_cast<std::uint32_t>(raw & 0xFFFFFFFFULL);
    }
    [[nodiscard]] constexpr bool isImmediate() const noexcept {
        return raw == 1ULL;
    }

    friend bool operator==(TimeTag, TimeTag) = default;
};

using Blob = std::vector<std::byte>;
using Argument = std::variant<std::int32_t, float, std::string, Blob>;

struct Message {
    std::string address;
    std::vector<Argument> args;

    friend bool operator==(const Message&, const Message&) = default;
};

struct Packet;

struct Bundle {
    TimeTag timeTag;
    std::vector<Packet> elements;

    friend bool operator==(const Bundle&, const Bundle&) = default;
};

struct Packet {
    std::variant<Message, Bundle> content;

    [[nodiscard]] bool isMessage() const noexcept {
        return std::holds_alternative<Message>(content);
    }
    [[nodiscard]] bool isBundle() const noexcept {
        return std::holds_alternative<Bundle>(content);
    }
    [[nodiscard]] const Message* asMessage() const noexcept {
        return std::get_if<Message>(&content);
    }
    [[nodiscard]] const Bundle* asBundle() const noexcept {
        return std::get_if<Bundle>(&content);
    }

    friend bool operator==(const Packet&, const Packet&) = default;
};

struct DecodeResult {
    std::optional<Packet> packet;
    DecodeError error = DecodeError::None;
    std::string detail;

    [[nodiscard]] bool ok() const noexcept {
        return error == DecodeError::None && packet.has_value();
    }
};

/// Decode one OSC 1.0 message or bundle from `bytes` under `limits`.
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes,
                                  const DecodeLimits& limits = {});

/// Convenience overload for raw uint8_t buffers (e.g. fuzzers and sockets).
[[nodiscard]] DecodeResult decode(std::span<const std::uint8_t> bytes,
                                  const DecodeLimits& limits = {});

/// Flatten all messages in a decoded packet in depth-first bundle order.
[[nodiscard]] std::vector<const Message*> flattenMessages(const Packet& packet);

}  // namespace adi::osc
