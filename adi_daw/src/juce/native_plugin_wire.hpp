// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/engine/clap_block_wire.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace adi::net::native_plugin_wire {

// Plain TCP is used only by the loopback proof. This framing has no
// authentication properties and the release transport must authenticate the
// endpoint before sending a Hello or plug-in state over a network.
inline constexpr std::size_t kHeaderBytes = 16;
inline constexpr std::size_t kHelloPrefixBytes = 24;
inline constexpr std::size_t kMaxIdBytes = 256;
inline constexpr std::size_t kMaxStateBytes = 40 * 1024 * 1024;
inline constexpr std::size_t kMaxFailureBytes = 1024;
inline constexpr std::size_t kMaxParameterBytes = 2 * 1024 * 1024;
inline constexpr std::size_t kParameterValueBytes = 12;

enum class Message : std::uint16_t {
    Hello = 1, Ready = 2, Process = 3, Result = 4,
    FetchState = 5, State = 6, RestoreState = 7, Ack = 8, Failure = 9,
    ReadParameters = 10, Parameters = 11, SetParameter = 12, ParameterValue = 13,
};

struct Header {
    Message message = Message::Hello;
    std::size_t bodyBytes = 0;
};

struct Hello {
    double sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t frames = 0;
    bool offline = false;
    bool instrument = false;
    std::string pluginId;
};

struct Ready {
    std::int32_t pluginLatency = 0;
    bool state = false;
    bool parameters = false;
};

// Fixed layout integer fields. Callers check the frame length before reading.
template<std::unsigned_integral Unsigned>
inline void put(std::span<std::byte> bytes, std::size_t offset, Unsigned value) noexcept {
    const auto bits = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i)
        bytes[offset + i] = static_cast<std::byte>((bits >> (8 * i)) & 0xffu);
}

template<std::unsigned_integral Unsigned>
[[nodiscard]] inline Unsigned get(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i)
        value |= std::to_integer<std::uint64_t>(bytes[offset + i]) << (8 * i);
    return static_cast<Unsigned>(value);
}

[[nodiscard]] inline bool validLength(Message message, std::size_t length) noexcept {
    using engine::clap_wire::kBlockHeaderBytes;
    using engine::clap_wire::kMaxBlockPacketBytes;
    switch (message) {
        case Message::Hello:
            return length > kHelloPrefixBytes && length <= kHelloPrefixBytes + kMaxIdBytes;
        case Message::Ready: return length == 8;
        case Message::Process: return length >= kBlockHeaderBytes && length <= kMaxBlockPacketBytes;
        case Message::Result: return length >= 4 + kBlockHeaderBytes && length <= 4 + kMaxBlockPacketBytes;
        case Message::FetchState:
        case Message::ReadParameters:
        case Message::Ack: return length == 0;
        case Message::State:
        case Message::RestoreState: return length <= kMaxStateBytes;
        case Message::Failure: return length <= kMaxFailureBytes;
        case Message::Parameters: return length >= 4 && length <= kMaxParameterBytes;
        case Message::SetParameter:
        case Message::ParameterValue: return length == kParameterValueBytes;
    }
    return false;
}

[[nodiscard]] inline std::array<std::byte, kHeaderBytes> encodeHeader(Message type, std::size_t bodyBytes) noexcept {
    std::array<std::byte, kHeaderBytes> bytes{};
    put(bytes, 0, std::uint32_t{0x53504441}); // ADPS
    put(bytes, 4, std::uint16_t{1});
    put(bytes, 6, static_cast<std::uint16_t>(type));
    put(bytes, 8, static_cast<std::uint32_t>(bodyBytes));
    return bytes;
}

[[nodiscard]] inline bool decodeHeader(std::span<const std::byte> bytes, Header& header) noexcept {
    if (bytes.size() != kHeaderBytes || get<std::uint32_t>(bytes, 0) != 0x53504441 ||
        get<std::uint16_t>(bytes, 4) != 1 || get<std::uint32_t>(bytes, 12) != 0)
        return false;
    const auto message = static_cast<Message>(get<std::uint16_t>(bytes, 6));
    const auto bodyBytes = get<std::uint32_t>(bytes, 8);
    if (!validLength(message, bodyBytes)) return false;
    header = {message, bodyBytes};
    return true;
}

[[nodiscard]] inline bool validHello(const Hello& hello) {
    return std::isfinite(hello.sampleRate) && hello.sampleRate >= 8000 && hello.sampleRate <= 768000 &&
        hello.channels > 0 && hello.channels <= engine::clap_wire::kMaxChannels &&
        hello.frames > 0 && hello.frames <= engine::clap_wire::kMaxFrames &&
        !hello.pluginId.empty() && hello.pluginId.size() <= kMaxIdBytes &&
        hello.pluginId.find('\0') == std::string::npos;
}

[[nodiscard]] inline bool encodeHello(const Hello& hello, std::vector<std::byte>& bytes) {
    if (!validHello(hello)) return false;
    bytes.resize(kHelloPrefixBytes + hello.pluginId.size());
    put(bytes, 0, std::bit_cast<std::uint64_t>(hello.sampleRate));
    put(bytes, 8, hello.channels);
    put(bytes, 12, hello.frames);
    put(bytes, 16, (hello.offline ? 1u : 0u) | (hello.instrument ? 2u : 0u));
    put(bytes, 20, static_cast<std::uint32_t>(hello.pluginId.size()));
    const auto id = std::as_bytes(std::span(hello.pluginId));
    std::copy(id.begin(), id.end(), bytes.begin() + kHelloPrefixBytes);
    return true;
}

[[nodiscard]] inline bool decodeHello(std::span<const std::byte> bytes, Hello& hello) {
    if (!validLength(Message::Hello, bytes.size())) return false;
    const auto flags = get<std::uint32_t>(bytes, 16);
    const auto id = bytes.subspan(kHelloPrefixBytes);
    if ((flags & ~3u) != 0 || get<std::uint32_t>(bytes, 20) != id.size()) return false;
    Hello decoded{std::bit_cast<double>(get<std::uint64_t>(bytes, 0)),
                  get<std::uint32_t>(bytes, 8), get<std::uint32_t>(bytes, 12),
                  (flags & 1u) != 0, (flags & 2u) != 0,
                  std::string(reinterpret_cast<const char*>(id.data()), id.size())};
    if (!validHello(decoded)) return false;
    hello = std::move(decoded);
    return true;
}

[[nodiscard]] inline std::array<std::byte, 8> encodeReady(Ready ready) noexcept {
    std::array<std::byte, 8> bytes{};
    put(bytes, 0, static_cast<std::uint32_t>(ready.pluginLatency));
    put(bytes, 4, (ready.state ? 1u : 0u) | (ready.parameters ? 2u : 0u));
    return bytes;
}

[[nodiscard]] inline bool decodeReady(std::span<const std::byte> bytes, Ready& ready) noexcept {
    if (!validLength(Message::Ready, bytes.size())) return false;
    const auto latency = get<std::uint32_t>(bytes, 0);
    const auto caps = get<std::uint32_t>(bytes, 4);
    if (latency > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        (caps & ~3u) != 0) return false;
    ready = {static_cast<std::int32_t>(latency), (caps & 1u) != 0, (caps & 2u) != 0};
    return true;
}

[[nodiscard]] inline bool normalized(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

[[nodiscard]] inline std::array<std::byte, kParameterValueBytes> encodeParameterValue(
    std::uint32_t selector, double value) noexcept {
    std::array<std::byte, kParameterValueBytes> bytes{};
    put(bytes, 0, selector);
    put(bytes, 4, std::bit_cast<std::uint64_t>(value));
    return bytes;
}

[[nodiscard]] inline bool decodeParameterValue(std::span<const std::byte> bytes,
                                                std::uint32_t& selector, double& value) noexcept {
    if (bytes.size() != kParameterValueBytes) return false;
    const auto decoded = std::bit_cast<double>(get<std::uint64_t>(bytes, 4));
    if (!normalized(decoded)) return false;
    selector = get<std::uint32_t>(bytes, 0);
    value = decoded;
    return true;
}

} // namespace adi::net::native_plugin_wire
