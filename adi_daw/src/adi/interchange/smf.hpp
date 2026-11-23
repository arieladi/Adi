// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace adi::interchange::smf {

using Bytes = std::vector<std::uint8_t>;

struct Ppq {
    std::uint16_t ticksPerQuarter;
};

struct Smpte {
    // The SMF frame-rate code, including -29 for 29.97 drop-frame.
    int rateCode;
    std::uint8_t ticksPerFrame;
};

using Division = std::variant<Ppq, Smpte>;

struct Event {
    std::uint64_t tick = 0;
    std::uint8_t status = 0;
    std::uint8_t metaType = 0;
    Bytes data;
    bool runningStatus = false;
    // The observed wire widths. Zero requests the shortest form. The writer
    // grows a preserved width if an edited value needs more bytes.
    std::uint8_t deltaWidth = 0;
    std::uint8_t lengthWidth = 0;
};

struct Track {
    std::vector<Event> events;
};

struct UnknownChunk {
    std::array<std::uint8_t, 4> id;
    Bytes data;
};

using Chunk = std::variant<Track, UnknownChunk>;

struct File {
    std::uint16_t format = 0;
    Division division = Ppq{480};
    Bytes headerExtra;
    std::vector<Chunk> chunks;
};

enum class Issue {
    MissingHeader,
    ChunkHeaderTruncated,
    ChunkDataTruncated,
    HeaderTooShort,
    RepeatedHeader,
    ReservedChunkId,
    InvalidFormat,
    InvalidDivision,
    TrackCountMismatch,
    VlqTruncated,
    VlqTooLong,
    TickOverflow,
    InvalidTickOrder,
    MissingStatus,
    InvalidStatus,
    EventDataTruncated,
    InvalidChannelData,
    InvalidMetaType,
    InvalidEndOfTrack,
    MissingEndOfTrack,
    DataAfterEndOfTrack,
    ChunkSizeOverflow
};

struct Error {
    Issue issue;
    std::size_t offset; // Absolute byte offset in the input or encoded prefix.
};

[[nodiscard]] std::string_view issueName(Issue issue) noexcept;
[[nodiscard]] std::variant<File, Error> read(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::variant<Bytes, Error> write(const File &file);

} // namespace adi::interchange::smf
