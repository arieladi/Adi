// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_tonality.hpp"

#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <string_view>

#include <utf8proc.h>

namespace adi::library {
namespace {

using Bytes = std::span<const unsigned char>;
constexpr std::size_t maxMetadataBytes = 1024 * 1024;
constexpr std::size_t maxTags = 32;
constexpr std::size_t maxTagBytes = 512;
constexpr std::size_t maxChunks = 4096;

std::uint64_t integer(Bytes bytes, bool littleEndian) {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        result = (result << 8) | bytes[littleEndian ? bytes.size() - 1 - i : i];
    return result;
}

std::string_view text(Bytes bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string_view trim(std::string_view value) {
    constexpr std::string_view spaces = " \t\r\n";
    const auto first = value.find_first_not_of(spaces);
    if (first == value.npos) return {};
    return value.substr(first, value.find_last_not_of(spaces) - first + 1);
}

std::string lower(std::string_view value) {
    std::string result(value);
    for (char& ch : result)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}

std::optional<double> bpmInRange(double value) {
    return std::isfinite(value) && value >= 10 && value <= 999 ? std::optional<double>{value} : std::nullopt;
}

std::optional<double> parseBpm(std::string_view value) {
    value = trim(value);
    if (value.empty()) return {};
    double number = 0;
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, number);
    if (parsed.ec != std::errc{}) return {};
    const std::string_view suffix{parsed.ptr, static_cast<std::size_t>(end - parsed.ptr)};
    if (!trim(suffix).empty() && lower(trim(suffix)) != "bpm") return {};
    return bpmInRange(number);
}

std::optional<SampleKey> parseKey(std::string_view value) {
    const auto folded = lower(trim(value));
    std::string_view key = folded;
    if (key.empty() || key.front() < 'a' || key.front() > 'g') return {};
    constexpr std::array<int, 7> noteByLetter{9, 11, 0, 2, 4, 5, 7};
    int note = noteByLetter[static_cast<std::size_t>(key.front() - 'a')];
    key.remove_prefix(1);
    if (key.starts_with('#') || key.starts_with("♯")) {
        ++note;
        key.remove_prefix(key.starts_with('#') ? 1u : std::string_view("♯").size());
    } else if (key.starts_with('b') || key.starts_with("♭")) {
        --note;
        key.remove_prefix(key.starts_with('b') ? 1u : std::string_view("♭").size());
    }
    key = trim(key);
    const bool minor = key == "m" || key == "min" || key == "minor";
    if (!minor && !key.empty() && key != "maj" && key != "major") return {};
    return SampleKey{static_cast<std::uint8_t>((note + 12) % 12), minor};
}

std::optional<bool> parseLoop(std::string_view value) {
    const auto folded = lower(trim(value));
    if (folded == "loop" || folded == "1" || folded == "true" || folded == "yes") return true;
    if (folded == "one shot" || folded == "one-shot" || folded == "oneshot" || folded == "0"
        || folded == "false" || folded == "no") return false;
    return {};
}

bool validUtf8(std::string_view value) {
    while (!value.empty()) {
        utf8proc_int32_t code = 0;
        const auto count = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(value.data()),
                                           static_cast<utf8proc_ssize_t>(value.size()), &code);
        if (count <= 0 || code <= 0) return false;
        value.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

void addTag(SampleFileTags& tags, std::string_view value) {
    value = trim(value);
    if (value.empty() || value.size() > maxTagBytes || tags.tags.size() >= maxTags || !validUtf8(value)) return;
    if (std::find(tags.tags.begin(), tags.tags.end(), value) == tags.tags.end()) tags.tags.emplace_back(value);
}

void assignTag(SampleFileTags& tags, std::string_view name, std::string_view value) {
    if (name.size() > maxTagBytes || value.size() > maxTagBytes) return;
    const auto field = lower(name);
    if (field == "bpm" || field == "tempo" || field == "tbpm") {
        if (!tags.tagBpm) tags.tagBpm = parseBpm(value);
    } else if (field == "key" || field == "initialkey" || field == "tkey") {
        if (!tags.tagKey) tags.tagKey = parseKey(value);
    } else if (field == "loop" || field == "loop_type" || field == "looptype") {
        if (!tags.tagLoop) tags.tagLoop = parseLoop(value);
    } else if (field == "genre" || field == "title" || field == "artist" || field == "album"
               || field == "tag" || field == "tags" || field == "category" || field == "keywords"
               || field == "tcon" || field == "tit2" || field == "tpe1" || field == "talb") {
        addTag(tags, value);
    }
}

std::optional<std::uint32_t> syncsafe(Bytes four) {
    std::uint32_t value = 0;
    for (const auto byte : four) {
        if ((byte & 0x80u) != 0) return {};
        value = (value << 7) | byte;
    }
    return value;
}

void encodeCodepoint(std::string& result, std::uint32_t code) {
    std::array<utf8proc_uint8_t, 4> bytes{};
    const auto count = utf8proc_encode_char(static_cast<utf8proc_int32_t>(code), bytes.data());
    if (count > 0) result.append(reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(count));
}

std::optional<std::string> decodeId3Text(Bytes bytes) {
    if (bytes.empty() || bytes.size() > 4 * maxTagBytes) return {};
    const auto encoding = bytes.front();
    bytes = bytes.subspan(1);
    std::string result;
    if (encoding == 3) return std::string(text(bytes));
    if (encoding == 0) {
        for (const auto byte : bytes) encodeCodepoint(result, byte);
        return result;
    }
    if (encoding != 1 && encoding != 2) return {};
    bool little = false;
    if (encoding == 1) {
        if (bytes.size() < 2) return {};
        const auto bom = integer(bytes.first(2), false);
        if (bom != 0xfeff && bom != 0xfffe) return {};
        little = bom == 0xfffe;
        bytes = bytes.subspan(2);
    }
    if (bytes.size() % 2 != 0) return {};
    while (!bytes.empty()) {
        auto code = integer(bytes.first(2), little);
        bytes = bytes.subspan(2);
        if (code >= 0xd800 && code <= 0xdbff) {
            if (bytes.size() < 2) return {};
            const auto tail = integer(bytes.first(2), little);
            if (tail < 0xdc00 || tail > 0xdfff) return {};
            bytes = bytes.subspan(2);
            code = 0x10000 + (code - 0xd800) * 1024 + tail - 0xdc00;
        } else if (code >= 0xdc00 && code <= 0xdfff) {
            return {};
        }
        if (code != 0xfeff) encodeCodepoint(result, static_cast<std::uint32_t>(code));
    }
    return result;
}

void parseId3(Bytes bytes, SampleFileTags& tags) {
    if (bytes.size() < 10 || text(bytes.first(3)) != "ID3" || (bytes[3] != 3 && bytes[3] != 4)) return;
    const bool version4 = bytes[3] == 4;
    const auto flags = bytes[5];
    const auto size = syncsafe(bytes.subspan(6, 4));
    if (!size || *size > bytes.size() - 10 || (flags & 0x80u) != 0) return;
    bytes = bytes.subspan(10, *size);
    if ((flags & 0x40u) != 0) {
        if (bytes.size() < 4) return;
        const auto extended = version4 ? syncsafe(bytes.first(4))
                                       : std::optional<std::uint32_t>{static_cast<std::uint32_t>(integer(bytes.first(4), false))};
        if (!extended || *extended < 4 || *extended > bytes.size() - (version4 ? 0u : 4u)) return;
        bytes = bytes.subspan(static_cast<std::size_t>(*extended) + (version4 ? 0u : 4u));
    }
    while (bytes.size() >= 10 && bytes.front() != 0) {
        const auto name = text(bytes.first(4));
        const auto fieldSize = version4 ? syncsafe(bytes.subspan(4, 4))
                                       : std::optional<std::uint32_t>{static_cast<std::uint32_t>(integer(bytes.subspan(4, 4), false))};
        if (!fieldSize || *fieldSize > bytes.size() - 10) return;
        if (name.starts_with('T') && bytes[9] == 0) {
            const auto decoded = decodeId3Text(bytes.subspan(10, *fieldSize));
            if (decoded) {
                const std::string_view value = *decoded;
                const auto split = value.find('\0');
                if (name == "TXXX" && split != value.npos) {
                    const auto tail = value.substr(split + 1);
                    assignTag(tags, value.substr(0, split), tail.substr(0, tail.find('\0')));
                } else {
                    assignTag(tags, name, value.substr(0, split));
                }
            }
        }
        bytes = bytes.subspan(10 + static_cast<std::size_t>(*fieldSize));
    }
}

void parseVorbisComments(Bytes bytes, SampleFileTags& tags) {
    if (bytes.size() < 8) return;
    const auto vendorSize = integer(bytes.first(4), true);
    if (vendorSize > bytes.size() - 8) return;
    bytes = bytes.subspan(4 + static_cast<std::size_t>(vendorSize));
    const auto count = std::min<std::uint64_t>(integer(bytes.first(4), true), maxChunks);
    bytes = bytes.subspan(4);
    for (std::uint64_t i = 0; i < count && bytes.size() >= 4; ++i) {
        const auto fieldSize = integer(bytes.first(4), true);
        bytes = bytes.subspan(4);
        if (fieldSize > bytes.size()) return;
        const auto field = text(bytes.first(static_cast<std::size_t>(fieldSize)));
        const auto equal = field.find('=');
        if (equal != field.npos) assignTag(tags, field.substr(0, equal), field.substr(equal + 1));
        bytes = bytes.subspan(static_cast<std::size_t>(fieldSize));
    }
}

class MetadataFile {
public:
    explicit MetadataFile(const std::filesystem::path& path) : stream_(path, std::ios::binary | std::ios::ate) {
        const auto position = stream_.tellg();
        if (position > 0) size = static_cast<std::uint64_t>(position);
    }

    bool read(std::uint64_t offset, std::span<unsigned char> buffer) {
        if (offset > size || buffer.size() > size - offset) return false;
        stream_.seekg(static_cast<std::streamoff>(offset));
        stream_.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        return static_cast<bool>(stream_);
    }

    std::uint64_t size = 0;

private:
    std::ifstream stream_;
};

void readId3(MetadataFile& file, std::uint64_t offset, std::uint64_t available, SampleFileTags& tags) {
    std::array<unsigned char, 10> header{};
    if (available < header.size() || !file.read(offset, header)) return;
    const auto bodySize = syncsafe(Bytes(header).subspan(6, 4));
    if (!bodySize || *bodySize > maxMetadataBytes || *bodySize > available - header.size()) return;
    std::vector<unsigned char> bytes(header.size() + *bodySize);
    if (file.read(offset, bytes)) parseId3(bytes, tags);
}

void readAcid(Bytes bytes, SampleFileTags& tags) {
    const auto flags = integer(bytes.first(4), true);
    const auto root = integer(bytes.subspan(4, 2), true);
    tags.acidLoop = (flags & 1u) == 0;
    tags.acidBpm = bpmInRange(std::bit_cast<float>(static_cast<std::uint32_t>(integer(bytes.subspan(20, 4), true))));
    if ((flags & 2u) != 0 && root < 128) tags.rootNote = static_cast<std::uint8_t>(root);
}

void readRiff(MetadataFile& file, std::uint32_t declaredSize, bool extended, SampleFileTags& tags,
              std::stop_token stop) {
    std::uint64_t boundary = extended ? file.size : static_cast<std::uint64_t>(declaredSize) + 8;
    if (boundary > file.size) return;
    std::optional<std::uint64_t> extendedDataBytes;
    std::optional<std::uint8_t> samplerRoot;
    std::array<unsigned char, 8> header{};
    for (std::uint64_t offset = 12, count = 0;
         offset <= boundary && boundary - offset >= 8 && count < maxChunks && !stop.stop_requested(); ++count) {
        if (!file.read(offset, header)) break;
        const auto name = text(Bytes(header).first(4));
        auto size = integer(Bytes(header).subspan(4, 4), true);
        offset += header.size();
        if (extended && name == "data" && size == 0xffffffffu && extendedDataBytes) size = *extendedDataBytes;
        if (size > boundary - offset) break;
        if (name == "ds64" && extended && size >= 28) {
            std::array<unsigned char, 16> payload{};
            if (!file.read(offset, payload)) break;
            const auto riffSize = integer(Bytes(payload).first(8), true);
            if (riffSize > file.size - 8 || riffSize + 8 < offset + size) break;
            boundary = riffSize + 8;
            extendedDataBytes = integer(Bytes(payload).subspan(8, 8), true);
        } else if (name == "acid" && size >= 24 && !tags.acidLoop) {
            std::array<unsigned char, 24> payload{};
            if (file.read(offset, payload)) readAcid(payload, tags);
        } else if (name == "id3 " || name == "ID3 ") {
            readId3(file, offset, size, tags);
        } else if (name == "smpl" && size >= 36) {
            std::array<unsigned char, 4> root{};
            if (file.read(offset + 12, root) && integer(root, true) < 128)
                samplerRoot = static_cast<std::uint8_t>(integer(root, true));
        } else if (name == "inst" && size >= 7 && !samplerRoot) {
            std::array<unsigned char, 1> root{};
            if (file.read(offset, root) && root.front() < 128) samplerRoot = root.front();
        }
        offset += size + (size & 1u);
    }
    if (!tags.rootNote) tags.rootNote = samplerRoot;
}

void readAiff(MetadataFile& file, std::uint32_t declaredSize, SampleFileTags& tags, std::stop_token stop) {
    const auto boundary = static_cast<std::uint64_t>(declaredSize) + 8;
    if (boundary > file.size) return;
    std::optional<double> duration;
    std::optional<double> beats;
    SampleFileTags apple;
    std::array<unsigned char, 8> header{};
    for (std::uint64_t offset = 12, count = 0;
         offset <= boundary && boundary - offset >= 8 && count < maxChunks && !stop.stop_requested(); ++count) {
        if (!file.read(offset, header)) break;
        const auto name = text(Bytes(header).first(4));
        const auto size = integer(Bytes(header).subspan(4, 4), false);
        offset += header.size();
        if (size > boundary - offset) break;
        if (name == "ID3 ") {
            readId3(file, offset, size, tags);
        } else if (name == "COMM" && size >= 18) {
            std::array<unsigned char, 18> payload{};
            if (file.read(offset, payload)) {
                const Bytes chunk(payload);
                const auto frames = integer(chunk.subspan(2, 4), false);
                const auto exponent = static_cast<int>(integer(chunk.subspan(8, 2), false));
                if (frames > 0 && exponent >= 16383 && exponent <= 16402) {
                    const auto rate = std::ldexp(static_cast<double>(integer(chunk.subspan(10, 8), false)), exponent - 16446);
                    if (rate > 0) duration = static_cast<double>(frames) / rate;
                }
            }
        } else if (name == "basc" && size >= 18) {
            std::array<unsigned char, 18> payload{};
            if (file.read(offset, payload)) {
                const Bytes chunk(payload);
                const auto root = integer(chunk.subspan(8, 2), false);
                const auto mode = integer(chunk.subspan(10, 2), false);
                const auto denominator = integer(chunk.subspan(14, 2), false);
                const auto type = integer(chunk.subspan(16, 2), false);
                if (root < 128) {
                    apple.rootNote = static_cast<std::uint8_t>(root);
                    if (mode == 1 || mode == 2) apple.tagKey = SampleKey{static_cast<std::uint8_t>(root % 12), mode == 1};
                }
                if (type <= 1) apple.tagLoop = type == 0;
                if (denominator > 0)
                    beats = 4. * static_cast<double>(integer(chunk.subspan(4, 4), false)) / static_cast<double>(denominator);
            }
        }
        offset += size + (size & 1u);
    }
    if (!tags.tagKey) tags.tagKey = apple.tagKey;
    if (!tags.tagLoop) tags.tagLoop = apple.tagLoop;
    if (!tags.rootNote) tags.rootNote = apple.rootNote;
    if (!tags.tagBpm && beats && duration) tags.tagBpm = bpmInRange(60. * *beats / *duration);
}

void readFlac(MetadataFile& file, SampleFileTags& tags, std::stop_token stop) {
    std::array<unsigned char, 4> header{};
    for (std::uint64_t offset = 4, count = 0; count < maxChunks && !stop.stop_requested(); ++count) {
        if (!file.read(offset, header)) return;
        const auto size = static_cast<std::size_t>(integer(Bytes(header).subspan(1, 3), false));
        offset += header.size();
        if (size > file.size - offset) return;
        if ((header.front() & 0x7fu) == 4u && size <= maxMetadataBytes) {
            std::vector<unsigned char> bytes(size);
            if (file.read(offset, bytes)) parseVorbisComments(bytes, tags);
            return;
        }
        if ((header.front() & 0x80u) != 0) return;
        offset += size;
    }
}

void readOgg(MetadataFile& file, SampleFileTags& tags, std::stop_token stop) {
    std::vector<unsigned char> packet;
    std::optional<std::uint64_t> serial;
    std::array<unsigned char, 27> header{};
    std::array<unsigned char, 255> segments{};
    bool firstPacket = true;
    for (std::uint64_t offset = 0; offset < 2 * maxMetadataBytes && !stop.stop_requested();) {
        if (!file.read(offset, header) || text(Bytes(header).first(4)) != "OggS" || header[4] != 0) return;
        const auto pageSerial = integer(Bytes(header).subspan(14, 4), true);
        if (!serial) serial = pageSerial;
        if (serial != pageSerial || ((header[5] & 1u) != 0) != !packet.empty()) return;
        const std::size_t segmentCount = header.back();
        offset += header.size();
        if (!file.read(offset, std::span(segments).first(segmentCount))) return;
        offset += segmentCount;
        for (const auto length : std::span(segments).first(segmentCount)) {
            const auto size = packet.size();
            if (size + length > maxMetadataBytes) return;
            packet.resize(size + length);
            if (length > 0 && !file.read(offset, std::span(packet).subspan(size, length))) return;
            offset += length;
            if (length == 255) continue;
            if (packet.size() < 7 || text(Bytes(packet).subspan(1, 6)) != "vorbis") return;
            if (!firstPacket) {
                if (packet.front() == 3) parseVorbisComments(Bytes(packet).subspan(7), tags);
                return;
            }
            if (packet.front() != 1) return;
            firstPacket = false;
            packet.clear();
        }
    }
}

} // namespace

SampleFileTags readSampleFileTags(const std::filesystem::path& path, std::stop_token stop) {
    if (audio::onAudioThread || stop.stop_requested()) return {};
    MetadataFile file(path);
    std::array<unsigned char, 12> header{};
    if (!file.read(0, header)) return {};

    SampleFileTags tags;
    const Bytes prefix(header);
    const auto format = text(prefix.first(4));
    const auto type = text(prefix.subspan(8, 4));
    if ((format == "RIFF" || format == "RF64" || format == "BW64") && type == "WAVE") {
        readRiff(file, static_cast<std::uint32_t>(integer(prefix.subspan(4, 4), true)), format != "RIFF", tags, stop);
    } else if (format == "FORM" && (type == "AIFF" || type == "AIFC")) {
        readAiff(file, static_cast<std::uint32_t>(integer(prefix.subspan(4, 4), false)), tags, stop);
    } else if (format == "fLaC") {
        readFlac(file, tags, stop);
    } else if (format == "OggS") {
        readOgg(file, tags, stop);
    } else if (text(prefix.first(3)) == "ID3") {
        readId3(file, 0, file.size, tags);
    }
    if (stop.stop_requested()) return {};
    return tags;
}

} // namespace adi::library
