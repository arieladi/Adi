// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/interchange/smf.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace adi::interchange::smf {
namespace {
using View = std::span<const std::uint8_t>;
using ChunkId = std::array<std::uint8_t, 4>;
constexpr ChunkId headerId{'M', 'T', 'h', 'd'};
constexpr ChunkId trackId{'M', 'T', 'r', 'k'};
constexpr std::uint32_t maxVlq = 0x0fffffff;
constexpr std::uint8_t endOfTrack = 0x2f;

struct Cursor {
    View bytes;
    std::size_t base = 0, pos = 0;

    [[nodiscard]] bool empty() const { return pos == bytes.size(); }
    [[nodiscard]] std::size_t offset() const { return base + pos; }
    [[nodiscard]] std::optional<View> take(std::size_t count) {
        if (count > bytes.size() - pos)
            return {};
        const auto slice = bytes.subspan(pos, count);
        pos += count;
        return slice;
    }
};

std::uint32_t readBigEndian(View bytes) {
    std::uint32_t value = 0;
    for (const auto byte : bytes)
        value = (value << 8) | byte;
    return value;
}

void writeBigEndian(Bytes &bytes, std::uint32_t value, unsigned width) {
    for (unsigned i = width; i > 0; --i)
        bytes.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
}

struct Vlq {
    std::uint32_t value = 0;
    std::uint8_t width = 0;
};

std::variant<Vlq, Error> readVlq(Cursor &input) {
    Vlq result;
    const auto offset = input.offset();
    while (result.width < 4) {
        const auto next = input.take(1);
        if (!next)
            return Error{Issue::VlqTruncated, input.offset()};
        const auto byte = next->front();
        result.value = (result.value << 7) | (byte & 0x7fU);
        ++result.width;
        if ((byte & 0x80U) == 0)
            return result;
    }
    return Error{Issue::VlqTooLong, offset};
}

bool writeVlq(Bytes &bytes, std::uint64_t value, unsigned preferredWidth) {
    if (value > maxVlq || preferredWidth > 4)
        return false;
    unsigned width = 1;
    while ((value >> (width * 7)) != 0)
        ++width;
    width = std::max(width, preferredWidth);
    for (unsigned i = width; i > 0; --i) {
        const auto byte = (value >> ((i - 1) * 7)) & 0x7fU;
        bytes.push_back(static_cast<std::uint8_t>(byte | (i > 1 ? 0x80U : 0U)));
    }
    return true;
}

bool validRate(int code) {
    return code == -24 || code == -25 || code == -29 || code == -30;
}

std::optional<Division> readDivision(std::uint16_t raw) {
    if (raw == 0)
        return {};
    if (raw < 0x8000)
        return Ppq{raw};
    const int code = static_cast<int>(raw >> 8) - 256;
    const auto ticks = static_cast<std::uint8_t>(raw);
    if (!validRate(code) || ticks == 0)
        return {};
    return Smpte{code, ticks};
}

std::optional<std::uint16_t> encodeDivision(const Division &division) {
    if (const auto *ppq = std::get_if<Ppq>(&division)) {
        if (ppq->ticksPerQuarter > 0 && ppq->ticksPerQuarter < 0x8000)
            return ppq->ticksPerQuarter;
        return {};
    }
    const auto &smpte = std::get<Smpte>(division);
    if (!validRate(smpte.rateCode) || smpte.ticksPerFrame == 0)
        return {};
    return static_cast<std::uint16_t>(((256 + smpte.rateCode) << 8) | smpte.ticksPerFrame);
}

unsigned channelDataCount(std::uint8_t status) {
    if (status < 0x80 || status >= 0xf0)
        return 0;
    const auto group = status & 0xf0;
    return group == 0xc0 || group == 0xd0 ? 1 : 2;
}

bool isVariableEvent(std::uint8_t status) {
    return status == 0xf0 || status == 0xf7 || status == 0xff;
}

bool validChannelData(View data) {
    return std::all_of(data.begin(), data.end(), [](std::uint8_t b) { return b < 0x80; });
}

std::variant<Track, Error> readTrack(Cursor input) {
    Track track;
    std::uint64_t tick = 0;
    std::uint8_t running = 0;
    while (!input.empty()) {
        const auto delta = readVlq(input);
        if (const auto *error = std::get_if<Error>(&delta))
            return *error;
        const auto [advance, deltaWidth] = std::get<Vlq>(delta);
        if (advance > std::numeric_limits<std::uint64_t>::max() - tick)
            return Error{Issue::TickOverflow, input.offset()};
        tick += advance;

        const auto statusOffset = input.offset();
        if (input.empty())
            return Error{Issue::EventDataTruncated, statusOffset};
        const auto next = input.bytes[input.pos];
        const bool usesRunning = next < 0x80;
        if (!usesRunning)
            ++input.pos;
        const auto status = usesRunning ? running : next;
        if (status == 0)
            return Error{Issue::MissingStatus, statusOffset};

        auto count = channelDataCount(status);
        std::uint8_t meta = 0, lengthWidth = 0;
        if (count != 0) {
            running = status;
        } else {
            if (!isVariableEvent(status))
                return Error{Issue::InvalidStatus, statusOffset};
            running = 0;
            if (status == 0xff) {
                const auto type = input.take(1);
                if (!type)
                    return Error{Issue::EventDataTruncated, input.offset()};
                meta = type->front();
                if (meta >= 0x80)
                    return Error{Issue::InvalidMetaType, input.offset() - 1};
            }
            const auto length = readVlq(input);
            if (const auto *error = std::get_if<Error>(&length))
                return *error;
            count = std::get<Vlq>(length).value;
            lengthWidth = std::get<Vlq>(length).width;
        }
        const auto payloadOffset = input.offset();
        const auto payload = input.take(count);
        if (!payload)
            return Error{Issue::EventDataTruncated, payloadOffset};
        if (running != 0 && !validChannelData(*payload))
            return Error{Issue::InvalidChannelData, payloadOffset};
        track.events.push_back(Event{tick, status, meta, Bytes(payload->begin(), payload->end()),
                                     usesRunning, deltaWidth, lengthWidth});
        if (status == 0xff && meta == endOfTrack) {
            if (count != 0)
                return Error{Issue::InvalidEndOfTrack, payloadOffset};
            if (!input.empty())
                return Error{Issue::DataAfterEndOfTrack, input.offset()};
            return track;
        }
    }
    return Error{Issue::MissingEndOfTrack, input.offset()};
}

std::optional<Error> writeTrack(Bytes &bytes, const Track &track) {
    std::uint64_t tick = 0;
    std::uint8_t running = 0;
    for (const auto &event : track.events) {
        const auto offset = bytes.size();
        if (event.tick < tick)
            return Error{Issue::InvalidTickOrder, offset};
        if (!writeVlq(bytes, event.tick - tick, event.deltaWidth))
            return Error{Issue::VlqTooLong, offset};
        tick = event.tick;
        const auto count = channelDataCount(event.status);
        if (count == 0 && !isVariableEvent(event.status))
            return Error{Issue::InvalidStatus, bytes.size()};
        if (event.runningStatus && (count == 0 || event.status != running))
            return Error{Issue::MissingStatus, bytes.size()};
        if (!event.runningStatus)
            bytes.push_back(event.status);
        if (count != 0) {
            if (event.data.size() != count || !validChannelData(event.data))
                return Error{Issue::InvalidChannelData, bytes.size()};
            running = event.status;
        } else {
            running = 0;
            if (event.status == 0xff) {
                if (event.metaType >= 0x80)
                    return Error{Issue::InvalidMetaType, bytes.size()};
                bytes.push_back(event.metaType);
            }
            const auto lengthOffset = bytes.size();
            if (!writeVlq(bytes, event.data.size(), event.lengthWidth))
                return Error{Issue::VlqTooLong, lengthOffset};
        }
        bytes.insert(bytes.end(), event.data.begin(), event.data.end());
        if (event.status == 0xff && event.metaType == endOfTrack) {
            if (!event.data.empty())
                return Error{Issue::InvalidEndOfTrack, offset};
            if (&event != &track.events.back())
                return Error{Issue::DataAfterEndOfTrack, bytes.size()};
            return {};
        }
    }
    return Error{Issue::MissingEndOfTrack, bytes.size()};
}

bool validTrackCount(std::uint16_t format, std::size_t count) {
    return count > 0 && count <= 0xffff && (format != 0 || count == 1);
}
} // namespace

std::string_view issueName(Issue issue) noexcept {
    switch (issue) {
    case Issue::MissingHeader: return "missing header";
    case Issue::ChunkHeaderTruncated: return "truncated chunk header";
    case Issue::ChunkDataTruncated: return "truncated chunk data";
    case Issue::HeaderTooShort: return "short header";
    case Issue::RepeatedHeader: return "repeated header";
    case Issue::ReservedChunkId: return "reserved chunk identifier";
    case Issue::InvalidFormat: return "invalid file format";
    case Issue::InvalidDivision: return "invalid time division";
    case Issue::TrackCountMismatch: return "track count mismatch";
    case Issue::VlqTruncated: return "truncated variable-length quantity";
    case Issue::VlqTooLong: return "variable-length quantity exceeds four bytes";
    case Issue::TickOverflow: return "absolute tick overflow";
    case Issue::InvalidTickOrder: return "events out of tick order";
    case Issue::MissingStatus: return "missing running status";
    case Issue::InvalidStatus: return "invalid event status";
    case Issue::EventDataTruncated: return "truncated event data";
    case Issue::InvalidChannelData: return "invalid channel event data";
    case Issue::InvalidMetaType: return "invalid meta event type";
    case Issue::InvalidEndOfTrack: return "invalid end-of-track event";
    case Issue::MissingEndOfTrack: return "missing end-of-track event";
    case Issue::DataAfterEndOfTrack: return "data after end-of-track event";
    case Issue::ChunkSizeOverflow: return "chunk size exceeds 32 bits";
    }
    return "unknown MIDI file issue";
}

std::variant<File, Error> read(View bytes) {
    Cursor input{bytes};
    File file;
    std::size_t declaredTracks = 0, actualTracks = 0;
    bool seenHeader = false;
    while (!input.empty()) {
        const auto chunkOffset = input.offset();
        const auto prefix = input.take(8);
        if (!prefix)
            return Error{Issue::ChunkHeaderTruncated, chunkOffset};
        ChunkId id{};
        std::copy_n(prefix->begin(), id.size(), id.begin());
        const auto length = readBigEndian(prefix->subspan(4));
        const auto bodyOffset = input.offset();
        const auto body = input.take(length);
        if (!body)
            return Error{Issue::ChunkDataTruncated, bodyOffset};
        if (id == headerId) {
            if (seenHeader)
                return Error{Issue::RepeatedHeader, chunkOffset};
            if (length < 6)
                return Error{Issue::HeaderTooShort, bodyOffset};
            file.format = static_cast<std::uint16_t>(readBigEndian(body->first(2)));
            if (file.format > 2)
                return Error{Issue::InvalidFormat, bodyOffset};
            declaredTracks = readBigEndian(body->subspan(2, 2));
            const auto division = readDivision(static_cast<std::uint16_t>(readBigEndian(body->subspan(4, 2))));
            if (!division)
                return Error{Issue::InvalidDivision, bodyOffset + 4};
            file.division = *division;
            const auto extra = body->subspan(6);
            file.headerExtra.assign(extra.begin(), extra.end());
            seenHeader = true;
            continue;
        }
        if (!seenHeader)
            return Error{Issue::MissingHeader, chunkOffset};
        if (id == trackId) {
            auto parsed = readTrack(Cursor{*body, bodyOffset});
            if (const auto *error = std::get_if<Error>(&parsed))
                return *error;
            file.chunks.emplace_back(std::get<Track>(std::move(parsed)));
            ++actualTracks;
        } else {
            file.chunks.emplace_back(UnknownChunk{id, Bytes(body->begin(), body->end())});
        }
    }
    if (!seenHeader)
        return Error{Issue::MissingHeader, 0};
    if (actualTracks != declaredTracks || !validTrackCount(file.format, actualTracks))
        return Error{Issue::TrackCountMismatch, 10};
    return file;
}

std::variant<Bytes, Error> write(const File &file) {
    if (file.format > 2)
        return Error{Issue::InvalidFormat, 8};
    const auto tracks = std::count_if(file.chunks.begin(), file.chunks.end(), [](const Chunk &chunk) {
        return std::holds_alternative<Track>(chunk);
    });
    if (!validTrackCount(file.format, static_cast<std::size_t>(tracks)))
        return Error{Issue::TrackCountMismatch, 10};
    const auto division = encodeDivision(file.division);
    if (!division)
        return Error{Issue::InvalidDivision, 12};
    constexpr auto maxChunk = std::numeric_limits<std::uint32_t>::max();
    if (file.headerExtra.size() > maxChunk - 6)
        return Error{Issue::ChunkSizeOverflow, 4};
    Bytes bytes(headerId.begin(), headerId.end());
    writeBigEndian(bytes, static_cast<std::uint32_t>(file.headerExtra.size() + 6), 4);
    writeBigEndian(bytes, file.format, 2);
    writeBigEndian(bytes, static_cast<std::uint32_t>(tracks), 2);
    writeBigEndian(bytes, *division, 2);
    bytes.insert(bytes.end(), file.headerExtra.begin(), file.headerExtra.end());
    for (const auto &chunk : file.chunks) {
        if (const auto *unknown = std::get_if<UnknownChunk>(&chunk)) {
            if (unknown->id == headerId)
                return Error{Issue::RepeatedHeader, bytes.size()};
            if (unknown->id == trackId)
                return Error{Issue::ReservedChunkId, bytes.size()};
            if (unknown->data.size() > maxChunk)
                return Error{Issue::ChunkSizeOverflow, bytes.size() + 4};
            bytes.insert(bytes.end(), unknown->id.begin(), unknown->id.end());
            writeBigEndian(bytes, static_cast<std::uint32_t>(unknown->data.size()), 4);
            bytes.insert(bytes.end(), unknown->data.begin(), unknown->data.end());
            continue;
        }
        bytes.insert(bytes.end(), trackId.begin(), trackId.end());
        const auto lengthOffset = bytes.size();
        bytes.resize(lengthOffset + 4);
        const auto bodyOffset = bytes.size();
        if (const auto error = writeTrack(bytes, std::get<Track>(chunk)))
            return *error;
        const auto length = bytes.size() - bodyOffset;
        if (length > maxChunk)
            return Error{Issue::ChunkSizeOverflow, lengthOffset};
        for (unsigned i = 0; i < 4; ++i)
            bytes[lengthOffset + i] = static_cast<std::uint8_t>(length >> ((3 - i) * 8));
    }
    return bytes;
}

} // namespace adi::interchange::smf
