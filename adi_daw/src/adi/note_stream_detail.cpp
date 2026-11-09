// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/note_stream_detail.hpp"
#include "adi/blob.hpp"
#include "adi/media/blake3.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace adi::notes {
namespace {

void checkValues(const NoteValues& value) {
    if (value.id == 0) {
        throw std::runtime_error("note ID is unassigned");
    }
    if (value.duration <= 0) {
        throw std::runtime_error("note duration must be positive");
    }
    if (value.start > std::numeric_limits<std::int64_t>::max() - value.duration) {
        throw std::runtime_error("note endpoint overflows signed 64-bit ticks");
    }
    if (value.key < 0 || value.key > 127 || value.velocity < 1 || value.velocity > 127
        || value.releaseVelocity < 0 || value.releaseVelocity > 127
        || value.channel < 0 || value.channel > 15
        || value.probability < 0 || value.probability > 10000) {
        throw std::runtime_error("note key, velocity, channel or probability is outside its range");
    }
    if (value.flags > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("note flags exceed the stored uint16 range");
    }
    if (!std::isfinite(value.cents) || value.cents < -1200.0 || value.cents > 1200.0) {
        throw std::runtime_error("note tuning cents must be finite and within +/-1200");
    }
}

NoteRecord prefix(const NoteValues& value) {
    checkValues(value);
    NoteRecord result{};
    result.start_ticks = value.start;
    result.dur_ticks = value.duration;
    result.note_id = value.id;
    result.key = static_cast<std::uint8_t>(value.key);
    result.vel_on = static_cast<std::uint8_t>(value.velocity);
    result.vel_off = static_cast<std::uint8_t>(value.releaseVelocity);
    result.channel = static_cast<std::uint8_t>(value.channel);
    result.flags = static_cast<std::uint16_t>(value.flags);
    result.probability = static_cast<std::uint16_t>(value.probability);
    result.tuning_cents = static_cast<float>(value.cents);
    return result;
}

NoteValues readValues(const Record& record) {
    if (record.bytes.size() < detail::notePrefixBytes
        || record.bytes.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("note record has an unsupported stride");
    }
    NoteRecord known{};
    std::memcpy(&known, record.bytes.data(), sizeof known);
    if (known.reserved != 0) {
        throw std::runtime_error("note reserved word must be zero");
    }
    NoteValues result;
    result.start = known.start_ticks;
    result.duration = known.dur_ticks;
    result.id = known.note_id;
    result.key = known.key;
    result.velocity = known.vel_on;
    result.releaseVelocity = known.vel_off;
    result.channel = known.channel;
    result.probability = known.probability;
    result.flags = known.flags;
    result.cents = static_cast<double>(known.tuning_cents);
    checkValues(result);
    return result;
}

StreamHeader header(const detail::Stream& stream) noexcept {
    StreamHeader result{};
    std::memcpy(&result, stream.header.data(), sizeof result);
    return result;
}

void checkHeader(const StreamHeader& value) {
    if (value.fourcc != static_cast<std::uint32_t>(FourCC::Notes)
        || value.version < 1 || value.rec_size < detail::notePrefixBytes) {
        throw std::runtime_error("invalid or unsupported ANOT header");
    }
    if ((value.flags & ~StreamFlags::SortedByTime) != 0) {
        throw std::runtime_error("notes stream must be musical and use only supported header flags");
    }
}

std::size_t encodedSize(std::size_t count, std::uint16_t stride, std::size_t trailer) {
    if (count > StreamReader<NoteRecord>::kMaxRecordsAtOnce
        || count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("notes stream record count exceeds the supported limit");
    }
    // Both bounds are enforced before multiplication, reserve or narrowing.
    // The binding API takes a signed int even on a 64-bit process.
    const auto maximum = std::min<std::uint64_t>(
        std::numeric_limits<std::size_t>::max(),
        static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
    if (trailer > maximum - detail::streamHeaderBytes) {
        throw std::runtime_error("notes stream trailer exceeds the SQLite BLOB binding limit");
    }
    const auto available = maximum - detail::streamHeaderBytes - trailer;
    if (stride == 0 || static_cast<std::uint64_t>(count) > available / stride) {
        throw std::runtime_error("notes stream size exceeds the SQLite BLOB binding limit");
    }
    return detail::streamHeaderBytes + count * static_cast<std::size_t>(stride) + trailer;
}

std::vector<NoteValues> checkedRecords(const detail::Stream& stream) {
    const auto info = header(stream);
    checkHeader(info);
    (void)encodedSize(stream.records.size(), info.rec_size, stream.trailer.size());
    std::set<std::uint64_t> ids;
    std::vector<NoteValues> result;
    result.reserve(stream.records.size());
    std::int64_t previous = std::numeric_limits<std::int64_t>::min();
    for (const auto& record : stream.records) {
        if (record.bytes.size() != info.rec_size) {
            throw std::runtime_error("note record does not match its stream stride");
        }
        const auto value = readValues(record);
        if (!ids.insert(value.id).second) {
            throw std::runtime_error("notes stream has duplicate note IDs");
        }
        if ((info.flags & StreamFlags::SortedByTime) != 0 && value.start < previous) {
            throw std::runtime_error("notes stream has a false SortedByTime flag");
        }
        previous = value.start;
        result.push_back(value);
    }
    return result;
}

std::vector<std::byte> encodeChecked(const detail::Stream& stream) {
    auto info = header(stream);
    const auto size = encodedSize(stream.records.size(), info.rec_size, stream.trailer.size());
    info.count = static_cast<std::uint32_t>(stream.records.size());
    std::vector<std::byte> result(size);
    std::memcpy(result.data(), &info, sizeof info);
    auto offset = detail::streamHeaderBytes;
    for (const auto& record : stream.records) {
        std::memcpy(result.data() + offset, record.bytes.data(), record.bytes.size());
        offset += record.bytes.size();
    }
    if (!stream.trailer.empty()) {
        std::memcpy(result.data() + offset, stream.trailer.data(), stream.trailer.size());
    }
    return result;
}

std::int64_t startOf(const Record& record) noexcept {
    std::int64_t value = 0;
    std::memcpy(&value, record.bytes.data(), sizeof value);
    return value;
}

} // namespace

std::optional<Record> makeRecord(const NoteValues& value, std::span<const std::byte> tail,
                                 std::string& error) {
    error.clear();
    try {
        const auto known = prefix(value);
        if (tail.size() > std::numeric_limits<std::uint16_t>::max() - detail::notePrefixBytes) {
            throw std::runtime_error("note tail exceeds the stored stride range");
        }
        Record result;
        result.bytes.resize(detail::notePrefixBytes + tail.size());
        std::memcpy(result.bytes.data(), &known, sizeof known);
        if (!tail.empty()) {
            std::memcpy(result.bytes.data() + detail::notePrefixBytes, tail.data(), tail.size());
        }
        return result;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::optional<Record> withValues(const Record& record, const NoteValues& value, std::string& error) {
    error.clear();
    try {
        (void)readValues(record);
        const auto known = prefix(value);
        auto result = record;
        std::memcpy(result.bytes.data(), &known, sizeof known);
        return result;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::optional<NoteValues> values(const Record& record, std::string& error) {
    error.clear();
    try {
        return readValues(record);
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

namespace detail {

std::uint16_t recordStride(const Stream& stream) noexcept { return header(stream).rec_size; }
std::uint16_t streamVersion(const Stream& stream) noexcept { return header(stream).version; }
std::uint32_t streamFlags(const Stream& stream) noexcept { return header(stream).flags; }

std::optional<Stream> decodeNotes(std::span<const std::byte> bytes, std::string& error) {
    error.clear();
    try {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("notes stream exceeds the SQLite BLOB binding limit");
        }
        StreamReader<NoteRecord> reader(bytes, FourCC::Notes);
        if (!reader.ok()) {
            throw std::runtime_error("invalid, truncated or unsupported ANOT stream");
        }
        checkHeader(reader.header());
        // encodedSize proves the product fits size_t before using it below.
        const auto bodyEnd = encodedSize(reader.count(), reader.recSize(), 0);
        if (bodyEnd > bytes.size()) {
            throw std::runtime_error("truncated notes stream records");
        }
        (void)encodedSize(reader.count(), reader.recSize(), bytes.size() - bodyEnd);
        Stream result;
        std::memcpy(result.header.data(), bytes.data(), streamHeaderBytes);
        result.records.reserve(reader.count());
        std::size_t offset = streamHeaderBytes;
        for (std::uint32_t index = 0; index < reader.count(); ++index) {
            Record record;
            record.bytes.resize(reader.recSize());
            std::memcpy(record.bytes.data(), bytes.data() + offset, reader.recSize());
            result.records.push_back(std::move(record));
            offset += reader.recSize();
        }
        result.trailer.assign(bytes.begin() + static_cast<std::ptrdiff_t>(bodyEnd), bytes.end());
        (void)checkedRecords(result);
        return result;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::optional<std::vector<std::byte>> encodeNotes(const Stream& stream, std::string& error) {
    error.clear();
    try {
        (void)checkedRecords(stream);
        return encodeChecked(stream);
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

Stream emptyNotes() {
    Stream result;
    const StreamHeader info{static_cast<std::uint32_t>(FourCC::Notes), 1,
                            static_cast<std::uint16_t>(notePrefixBytes), 0,
                            StreamFlags::SortedByTime};
    std::memcpy(result.header.data(), &info, sizeof info);
    return result;
}

std::optional<std::string> hashNotes(std::span<const std::byte> bytes, std::string& error) {
    error.clear();
    auto result = media::blake3Bytes(bytes);
    if (!result || result.hex.size() != 64
        || !std::all_of(result.hex.begin(), result.hex.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) {
        error = "cannot compute the complete notes stream hash";
        return std::nullopt;
    }
    return std::move(result.hex);
}

StreamPlan planRecords(const Stream& stream, const Diff& diff) {
    try {
        const auto known = checkedRecords(stream);
        const auto stride = recordStride(stream);
        std::map<std::uint64_t, std::size_t> original;
        for (std::size_t index = 0; index < known.size(); ++index) {
            original.emplace(known[index].id, index);
        }
        std::set<std::uint64_t> touched;
        std::set<std::uint64_t> removed;
        std::map<std::uint64_t, const Record*> changed;
        StreamPlan result;
        bool timeChanged = false;
        const auto read = [stride](const Record& record) {
            if (record.bytes.size() != stride) {
                throw std::runtime_error("diff record does not match target stream stride; future records need full bytes");
            }
            return readValues(record);
        };
        const auto touch = [&](std::uint64_t id) {
            if (!touched.insert(id).second) {
                throw std::runtime_error("note ID is repeated within or between diff sections");
            }
        };
        const auto existing = [&](const Record& record, std::uint64_t id) {
            const auto found = original.find(id);
            if (found == original.end()) {
                throw std::runtime_error("diff old note identity does not exist");
            }
            if (stream.records[found->second].bytes != record.bytes) {
                throw std::runtime_error("diff old note bytes do not match the captured stream");
            }
        };
        for (const auto& record : diff.removed) {
            const auto value = read(record);
            touch(value.id);
            existing(record, value.id);
            removed.insert(value.id);
            result.effects.removed.push_back(record);
        }
        for (const auto& change : diff.changed) {
            const auto before = read(change.oldRecord);
            const auto after = read(change.newRecord);
            if (before.id != after.id) {
                throw std::runtime_error("a changed note must retain its nonzero identity");
            }
            touch(before.id);
            existing(change.oldRecord, before.id);
            if (!std::equal(change.oldRecord.bytes.begin() + static_cast<std::ptrdiff_t>(notePrefixBytes),
                            change.oldRecord.bytes.end(),
                            change.newRecord.bytes.begin() + static_cast<std::ptrdiff_t>(notePrefixBytes))) {
                throw std::runtime_error("a changed note must preserve its opaque future tail");
            }
            if (change.oldRecord != change.newRecord) {
                changed.emplace(before.id, &change.newRecord);
                result.effects.changed.push_back(change);
                timeChanged = timeChanged || before.start != after.start;
            }
        }
        for (const auto& record : diff.added) {
            const auto value = read(record);
            touch(value.id);
            if (original.contains(value.id)) {
                throw std::runtime_error("added note ID already exists; use changed for an existing identity");
            }
            result.effects.added.push_back(record);
        }
        result.changed = !result.effects.added.empty() || !result.effects.removed.empty()
                         || !result.effects.changed.empty();
        if (!result.changed) {
            result.bytes = encodeChecked(stream);
            result.ok = true;
            return result;
        }
        const auto retainedCount = stream.records.size() - removed.size();
        if (diff.added.size() > StreamReader<NoteRecord>::kMaxRecordsAtOnce
            || retainedCount > StreamReader<NoteRecord>::kMaxRecordsAtOnce - diff.added.size()) {
            throw std::runtime_error("diff result exceeds the supported note count");
        }
        const auto count = retainedCount + diff.added.size();
        (void)encodedSize(count, stride, stream.trailer.size());
        Stream after;
        after.header = stream.header;
        after.trailer = stream.trailer;
        after.records.reserve(count);
        for (std::size_t index = 0; index < stream.records.size(); ++index) {
            const auto id = known[index].id;
            if (removed.contains(id)) {
                continue;
            }
            const auto replacement = changed.find(id);
            after.records.push_back(replacement == changed.end() ? stream.records[index] : *replacement->second);
        }
        after.records.insert(after.records.end(), diff.added.begin(), diff.added.end());
        if ((streamFlags(stream) & StreamFlags::SortedByTime) != 0
            && (timeChanged || !diff.added.empty())) {
            std::stable_sort(after.records.begin(), after.records.end(), [](const Record& left, const Record& right) {
                return startOf(left) < startOf(right);
            });
        }
        // All records were checked before staging; validate the final order
        // too, then encode the complete envelope once for the result.
        (void)checkedRecords(after);
        result.bytes = encodeChecked(after);
        result.ok = true;
        return result;
    } catch (const std::exception& exception) {
        StreamPlan failure;
        failure.error = exception.what();
        return failure;
    }
}

Payload recordPacket(const Record& record) {
    (void)readValues(record);
    std::vector<std::uint8_t> bytes(record.bytes.size());
    std::memcpy(bytes.data(), record.bytes.data(), bytes.size());
    return {{"bytes", Payload::binary(std::move(bytes))}};
}

std::optional<Record> parseRecordPacket(const Payload& packet, std::string& error) {
    error.clear();
    try {
        if (!packet.is_object() || packet.size() != 1 || !packet.contains("bytes")) {
            throw std::runtime_error("note record packet must contain only bytes");
        }
        const auto& encoded = packet.at("bytes");
        const auto size = encoded.is_binary() ? encoded.get_binary().size() : encoded.size();
        if ((!encoded.is_binary() && !encoded.is_array()) || size < notePrefixBytes
            || size > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("note record bytes have an unsupported type or stride");
        }
        Record result;
        result.bytes.resize(size);
        if (encoded.is_binary()) {
            const auto& bytes = encoded.get_binary();
            std::memcpy(result.bytes.data(), bytes.data(), bytes.size());
        } else {
            for (std::size_t index = 0; index < size; ++index) {
                const auto value = signedValue(encoded[index], error);
                if (!value || *value < 0 || *value > 255) {
                    throw std::runtime_error("note record byte array needs integers in [0,255]");
                }
                result.bytes[index] = static_cast<std::byte>(*value);
            }
        }
        (void)readValues(result);
        return result;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::optional<std::int64_t> signedValue(const Payload& value, std::string& error) {
    error.clear();
    if (!value.is_number_integer()
        || (value.is_number_unsigned() && value.get<std::uint64_t>()
                > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
        error = "note integer is outside the signed 64-bit range";
        return std::nullopt;
    }
    return value.get<std::int64_t>();
}

std::optional<std::uint64_t> noteIdValue(const Payload& value, std::string& error) {
    error.clear();
    if (!value.is_number_integer()
        || (!value.is_number_unsigned() && value.get<std::int64_t>() <= 0)) {
        error = "note ID must be a nonzero uint64 integer";
        return std::nullopt;
    }
    const auto result = value.get<std::uint64_t>();
    if (result == 0) {
        error = "note ID is unassigned";
        return std::nullopt;
    }
    return result;
}

} // namespace detail
} // namespace adi::notes
