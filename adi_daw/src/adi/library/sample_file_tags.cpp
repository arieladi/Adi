// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_tonality.hpp"

#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <ios>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>

#include <apetag.h>
#include <fileref.h>
#include <id3v1tag.h>
#include <mp4file.h>
#include <mp4tag.h>
#include <mpegfile.h>
#include <riff/aiff/aifffile.h>
#include <tbytevector.h>
#include <tiostream.h>
#include <tpropertymap.h>
#include <utf8proc.h>
#include <wavfile.h>

namespace adi::library {
namespace {

using Bytes = std::span<const unsigned char>;
constexpr std::size_t maxMetadataBlock = 1024 * 1024;
constexpr std::size_t maxMetadataBytes = 8 * maxMetadataBlock;
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
    if (value.size() > maxTagBytes) return {};
    if (value.size() >= 3 && lower(value.substr(value.size() - 3)) == "bpm")
        value = trim(value.substr(0, value.size() - 3));
    // Keep decimal/exponent notation and parse in the classic locale on
    // standard libraries with only the integral from_chars overloads.
    if (value.empty() || value.front() == '+' || value.find_first_not_of("-+eE.0123456789") != value.npos)
        return {};
    double number = 0;
    std::istringstream input{std::string(value)};
    input.imbue(std::locale::classic());
    input >> std::noskipws >> number;
    if (input.fail() || !input.eof()) return {};
    return bpmInRange(number);
}

std::optional<SampleKey> parseKey(std::string_view value) {
    value = trim(value);
    const auto folded = lower(value);
    std::string_view key = folded;
    if (key.empty()) return {};
    if (key.front() >= '0' && key.front() <= '9') {
        int number = 0;
        const auto last = key.data() + key.size() - 1;
        const auto parsed = std::from_chars(key.data(), last, number);
        if (parsed.ec != std::errc{} || parsed.ptr != last || number < 1 || number > 12) return {};
        const char mode = key.back();
        constexpr std::array<int, 12> camelotMajors{11, 6, 1, 8, 3, 10, 5, 0, 7, 2, 9, 4};
        const bool camelot = mode == 'a' || mode == 'b';
        if (!camelot && mode != 'm' && mode != 'd') return {};
        const auto major = camelot ? camelotMajors[static_cast<std::size_t>(number - 1)] : (number - 1) * 7 % 12;
        const bool minor = mode == 'a' || mode == 'm';
        return SampleKey{static_cast<std::uint8_t>((major + (minor ? 9 : 0)) % 12), minor};
    }
    if (key.front() < 'a' || key.front() > 'g') return {};
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
    const bool abbreviatedMajor = key == "m" && value.back() == 'M';
    const bool minor = !abbreviatedMajor && (key == "m" || key == "min" || key == "minor");
    if (!minor && !abbreviatedMajor && !key.empty() && key != "maj" && key != "major") return {};
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
               || field == "tag" || field == "tags" || field == "category" || field == "keywords") {
        addTag(tags, value);
    }
}

class MetadataFile final : public TagLib::IOStream {
public:
    MetadataFile(const std::filesystem::path& path, std::stop_token stop)
        : stream_(path, std::ios::binary | std::ios::ate), stop_(stop) {
        const auto position = stream_.tellg();
        if (position > 0) size = static_cast<std::uint64_t>(position);
    }

    bool read(std::uint64_t offset, std::span<unsigned char> buffer) {
        if (stop_.stop_requested() || offset > size || buffer.size() > size - offset) return false;
        stream_.seekg(static_cast<std::streamoff>(offset));
        stream_.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        return static_cast<bool>(stream_);
    }

    // The underlying stream is opened read-only. There is no mutation path
    // and no extension hint, so the dependency selects formats by content.
    TagLib::FileName name() const override {
#ifdef _WIN32
        return L"";
#else
        return "";
#endif
    }

    bool readOnly() const override { return true; }
    bool isOpen() const override { return stream_.is_open(); }
    void writeBlock(const TagLib::ByteVector&) override {}
    void insert(const TagLib::ByteVector&, TagLib::offset_t, std::size_t) override {}
    void removeBlock(TagLib::offset_t, std::size_t) override {}
    void truncate(TagLib::offset_t) override {}
    void clear() override { stream_.clear(); }
    TagLib::offset_t tell() const override { return position_; }
    TagLib::offset_t length() override { return static_cast<TagLib::offset_t>(size); }

    void seek(TagLib::offset_t delta, Position from) override {
        const auto base = from == Beginning ? 0 : (from == Current ? position_ : length());
        if (delta < -base) position_ = 0;
        else if (delta > std::numeric_limits<TagLib::offset_t>::max() - base)
            position_ = std::numeric_limits<TagLib::offset_t>::max();
        else position_ = base + delta;
    }

    TagLib::ByteVector readBlock(std::size_t requested) override {
        const auto offset = static_cast<std::uint64_t>(position_);
        if (refused_ || stop_.stop_requested() || offset >= size || requested == 0) return {};
        const auto amount = std::min<std::uint64_t>(requested, size - offset);
        if (amount > maxMetadataBlock || amount > bytesLeft_ || blocksLeft_ == 0) {
            refused_ = true;
            return {};
        }
        --blocksLeft_;
        bytesLeft_ -= static_cast<std::size_t>(amount);
        TagLib::ByteVector result(static_cast<unsigned int>(amount), 0);
        auto buffer = std::span(reinterpret_cast<unsigned char*>(result.data()), static_cast<std::size_t>(amount));
        if (!read(offset, buffer)) {
            refused_ = true;
            return {};
        }
        position_ += static_cast<TagLib::offset_t>(amount);
        return result;
    }

    [[nodiscard]] bool refused() const { return refused_; }
    std::uint64_t size = 0;

private:
    std::ifstream stream_;
    std::stop_token stop_;
    TagLib::offset_t position_ = 0;
    std::size_t bytesLeft_ = maxMetadataBytes;
    std::size_t blocksLeft_ = maxChunks;
    bool refused_ = false;
};

void applyProperties(const TagLib::PropertyMap& properties, SampleFileTags& tags) {
    for (const auto& [name, values] : properties) {
        if (name.size() > maxTagBytes) continue;
        const auto field = name.to8Bit(true);
        for (const auto& value : values) {
            if (value.size() <= maxTagBytes) assignTag(tags, field, value.to8Bit(true));
        }
    }
}

void collectEmbeddedTags(TagLib::File& parsed, SampleFileTags& tags) {
    // The MP4 integer tempo and an authored fractional freeform tempo map to
    // the same generic property. Preserve the fractional value when present.
    if (const auto* mp4 = dynamic_cast<const TagLib::MP4::File*>(&parsed); mp4 && mp4->tag()) {
        for (const auto& [name, item] : mp4->tag()->itemMap()) {
            if (name.size() > maxTagBytes || lower(name.to8Bit(true)) != "----:com.apple.itunes:bpm") continue;
            const auto values = item.toStringList();
            for (const auto& value : values)
                if (value.size() <= maxTagBytes) assignTag(tags, "bpm", value.to8Bit(true));
        }
    }
    applyProperties(parsed.properties(), tags);
    // A format can contain several tag families. The preferred one supplies
    // tempo and key first; supplementary tags still contribute missing fields.
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(&parsed)) {
        if (const auto* ape = mpeg->APETag(false)) applyProperties(ape->properties(), tags);
        if (const auto* id3v1 = mpeg->ID3v1Tag(false)) applyProperties(id3v1->properties(), tags);
    } else if (const auto* wav = dynamic_cast<const TagLib::RIFF::WAV::File*>(&parsed)) {
        if (const auto* info = wav->InfoTag()) applyProperties(info->properties(), tags);
    }
}

enum class EmbeddedFormat { Wave, Aiff, Mp4, Generic };

void readEmbeddedTags(MetadataFile& file, SampleFileTags& tags, EmbeddedFormat format) {
    if (format == EmbeddedFormat::Wave) {
        TagLib::RIFF::WAV::File parsed(&file, false);
        if (parsed.isValid() && !file.refused()) collectEmbeddedTags(parsed, tags);
    } else if (format == EmbeddedFormat::Aiff) {
        TagLib::RIFF::AIFF::File parsed(&file, false);
        if (parsed.isValid() && !file.refused()) collectEmbeddedTags(parsed, tags);
    } else if (format == EmbeddedFormat::Mp4) {
        TagLib::MP4::File parsed(&file, false);
        if (parsed.isValid() && !file.refused()) collectEmbeddedTags(parsed, tags);
    } else {
        const TagLib::FileRef parsed(&file, false);
        if (!parsed.isNull() && !file.refused()) collectEmbeddedTags(*parsed.file(), tags);
    }
}

void readAcid(Bytes bytes, SampleFileTags& tags) {
    const auto flags = integer(bytes.first(4), true);
    const auto root = integer(bytes.subspan(4, 2), true);
    tags.acidLoop = (flags & 1u) == 0;
    tags.acidBpm = bpmInRange(std::bit_cast<float>(static_cast<std::uint32_t>(integer(bytes.subspan(20, 4), true))));
    if ((flags & 2u) != 0 && root < 128) tags.rootNote = static_cast<std::uint8_t>(root);
}

bool readRiff(MetadataFile& file, std::uint32_t declaredSize, bool extended, SampleFileTags& tags,
              std::stop_token stop) {
    std::uint64_t boundary = extended ? file.size : static_cast<std::uint64_t>(declaredSize) + 8;
    if (boundary > file.size) return false;
    std::optional<std::uint64_t> extendedDataBytes;
    std::optional<std::uint8_t> samplerRoot;
    std::optional<std::uint8_t> instrumentRoot;
    bool embeddedTags = false;
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
        } else if (name == "smpl" && size >= 36 && !samplerRoot) {
            std::array<unsigned char, 4> root{};
            if (file.read(offset + 12, root) && integer(root, true) < 128)
                samplerRoot = static_cast<std::uint8_t>(integer(root, true));
        } else if (name == "inst" && size >= 7 && !instrumentRoot) {
            std::array<unsigned char, 1> root{};
            if (file.read(offset, root) && root.front() < 128) instrumentRoot = root.front();
        } else if (name == "id3 " || name == "ID3 " || name == "LIST") {
            embeddedTags = true;
        }
        offset += size + (size & 1u);
    }
    if (!tags.rootNote) tags.rootNote = samplerRoot ? samplerRoot : instrumentRoot;
    return embeddedTags;
}

bool readAiff(MetadataFile& file, std::uint32_t declaredSize, SampleFileTags& tags, std::stop_token stop) {
    const auto boundary = static_cast<std::uint64_t>(declaredSize) + 8;
    if (boundary > file.size) return false;
    std::optional<double> duration;
    std::optional<double> beats;
    bool embeddedTags = false;
    std::array<unsigned char, 8> header{};
    for (std::uint64_t offset = 12, count = 0;
         offset <= boundary && boundary - offset >= 8 && count < maxChunks && !stop.stop_requested(); ++count) {
        if (!file.read(offset, header)) break;
        const auto name = text(Bytes(header).first(4));
        const auto size = integer(Bytes(header).subspan(4, 4), false);
        const auto paddedSize = size + (size & 1u);
        offset += header.size();
        if (paddedSize > boundary - offset) break;
        if (name == "COMM" && size >= 18) {
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
                    tags.rootNote = static_cast<std::uint8_t>(root);
                    if (mode == 1 || mode == 2) tags.tagKey = SampleKey{static_cast<std::uint8_t>(root % 12), mode == 1};
                }
                if (type <= 1) tags.tagLoop = type == 0;
                if (denominator > 0)
                    beats = 4. * static_cast<double>(integer(chunk.subspan(4, 4), false)) / static_cast<double>(denominator);
            }
        } else if ((name == "NAME" || name == "AUTH") && size > 0 && size <= maxTagBytes) {
            std::array<unsigned char, maxTagBytes> payload{};
            const auto textBytes = std::span(payload).first(static_cast<std::size_t>(size));
            if (file.read(offset, textBytes)) addTag(tags, text(textBytes));
        } else if (name == "ID3 " || name == "id3 ") {
            embeddedTags = true;
        }
        offset += paddedSize;
    }
    if (beats && duration) tags.tagBpm = bpmInRange(60. * *beats / *duration);
    return embeddedTags;
}

} // namespace

SampleFileTags readSampleFileTags(const std::filesystem::path& path, std::stop_token stop) {
    if (audio::onAudioThread || stop.stop_requested()) return {};
    MetadataFile file(path, stop);
    std::array<unsigned char, 12> header{};
    if (!file.read(0, header)) return {};
    const Bytes prefix(header);
    const auto format = text(prefix.first(4));
    const auto type = text(prefix.subspan(8, 4));
    // Refuse a truncated declared envelope instead of publishing whatever
    // tags happened to precede the missing bytes.
    if (format == "RIFF" || format == "FORM") {
        const auto size = integer(prefix.subspan(4, 4), format == "RIFF");
        if (size < 4 || size > file.size - 8) return {};
    }

    SampleFileTags tags;
    const bool wave = (format == "RIFF" || format == "RF64" || format == "BW64") && type == "WAVE";
    const bool aiff = format == "FORM" && (type == "AIFF" || type == "AIFC");
    SampleFileTags nativeAiff;
    bool embedded = true;
    if (wave)
        embedded = readRiff(file, static_cast<std::uint32_t>(integer(prefix.subspan(4, 4), true)),
                            format != "RIFF", tags, stop);
    else if (aiff)
        embedded = readAiff(file, static_cast<std::uint32_t>(integer(prefix.subspan(4, 4), false)), nativeAiff, stop);
    if (embedded && !stop.stop_requested() &&
        file.size <= static_cast<std::uint64_t>(std::numeric_limits<TagLib::offset_t>::max())) {
        file.clear();
        // These containers already supplied a format signature. A generic
        // content probe is useful only for the other file families.
        const auto knownFormat = wave ? EmbeddedFormat::Wave : aiff ? EmbeddedFormat::Aiff :
            (text(prefix.subspan(4, 4)) == "ftyp" ? EmbeddedFormat::Mp4 : EmbeddedFormat::Generic);
        readEmbeddedTags(file, tags, knownFormat);
    }
    if (aiff) {
        if (!tags.tagKey) tags.tagKey = nativeAiff.tagKey;
        if (!tags.tagLoop) tags.tagLoop = nativeAiff.tagLoop;
        if (!tags.rootNote) tags.rootNote = nativeAiff.rootNote;
        if (!tags.tagBpm) tags.tagBpm = nativeAiff.tagBpm;
        for (const auto& tag : nativeAiff.tags) addTag(tags, tag);
    }
    if (stop.stop_requested()) return {};
    return tags;
}

} // namespace adi::library
