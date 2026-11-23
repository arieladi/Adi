// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/note_transforms.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/blob.hpp"
#include "adi/engine/scale_context.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
namespace adi::notes {
namespace {
std::int64_t signedValue(const Payload &value) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::runtime_error("note transform integer is outside int64");
    return value.get<std::int64_t>();
}
std::uint64_t noteId(const Payload &value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 1))
        throw std::runtime_error("note ID must be a positive uint64");
    const auto id = value.get<std::uint64_t>();
    if (id == 0)
        throw std::runtime_error("note ID is unassigned");
    return id;
}
std::set<std::uint64_t> selection(const Payload &payload) {
    const auto &ids = payload.at("note_ids");
    if (!ids.is_array() || ids.empty())
        throw std::runtime_error("note transform requires explicit note IDs");
    std::set<std::uint64_t> result;
    for (const auto &value : ids)
        if (!result.insert(noteId(value)).second)
            throw std::runtime_error("duplicate selected note ID");
    return result;
}
struct Record {
    NoteRecord note{};
    std::vector<std::byte> bytes;
};
struct Stream {
    std::vector<std::byte> bytes;
    std::vector<Record> records;
    StreamHeader header{};
    std::set<std::uint64_t> ids;
    explicit Stream(std::vector<std::byte> blob) : bytes(std::move(blob)) {
        StreamReader<NoteRecord> reader(bytes, FourCC::Notes);
        if (!reader.ok() || reader.version() < 1 ||
            reader.count() > StreamReader<NoteRecord>::kMaxRecordsAtOnce ||
            (reader.header().flags & StreamFlags::TimeIsNanos))
            throw std::runtime_error("invalid or unsupported musical notes stream");
        header = reader.header();
        records.reserve(reader.count());
        std::int64_t previous = std::numeric_limits<std::int64_t>::min();
        for (std::uint32_t i = 0; i < reader.count(); ++i) {
            const auto note = *reader.at(i);
            if (note.note_id == 0 || !ids.insert(note.note_id).second || note.dur_ticks <= 0 ||
                note.key > 127 || note.vel_on < 1 || note.vel_on > 127 || note.vel_off > 127 ||
                note.channel > 15 || note.probability > 10000 || !std::isfinite(note.tuning_cents) ||
                note.reserved != 0)
                throw std::runtime_error("invalid note record");
            if ((header.flags & StreamFlags::SortedByTime) && note.start_ticks < previous)
                throw std::runtime_error("notes stream has a false time-order flag");
            previous = note.start_ticks;
            const auto offset = sizeof(StreamHeader) + static_cast<std::size_t>(i) * reader.recSize();
            const auto first = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
            records.push_back({note, {first, first + reader.recSize()}});
        }
    }
    std::vector<std::byte> encode(bool sorted) {
        if (sorted)
            std::stable_sort(records.begin(), records.end(), [](const Record &a, const Record &b) {
                return a.note.start_ticks < b.note.start_ticks;
            });
        auto result = bytes;
        for (std::size_t i = 0; i < records.size(); ++i) {
            auto &record = records[i];
            std::memcpy(record.bytes.data(), &record.note, sizeof(NoteRecord));
            std::memcpy(result.data() + sizeof(StreamHeader) + i * header.rec_size, record.bytes.data(),
                        record.bytes.size());
        }
        // Header flags/version/stride and any complete-stream trailer are retained.
        return result;
    }
};
Stream read(OpContext &context, const Payload &payload) {
    const auto clip = signedValue(payload.at("clip"));
    SQLite::Statement row(context.db, "SELECT 1 FROM clips WHERE id=?");
    row.bind(1, clip);
    if (!row.executeStep())
        throw std::runtime_error("no such clip");
    auto bytes = context.store.getEventStream(clip, "notes");
    if (!bytes)
        throw std::runtime_error("clip has no notes stream");
    return Stream(std::move(*bytes));
}
void selectedExist(const Stream &stream, const std::set<std::uint64_t> &ids) {
    for (auto id : ids)
        if (!stream.ids.contains(id))
            throw std::runtime_error("selected note does not exist");
}
bool save(OpContext &context, const Payload &payload, const std::vector<std::byte> &bytes,
          std::string &error) {
    if (context.store.putEventStream(signedValue(payload.at("clip")), "notes", bytes))
        return true;
    error = context.store.lastError();
    return false;
}
std::int64_t add(std::int64_t left, std::int64_t right) {
    const auto maximum = std::numeric_limits<std::int64_t>::max(),
               minimum = std::numeric_limits<std::int64_t>::min();
    if ((right > 0 && left > maximum - right) || (right < 0 && left < minimum - right))
        throw std::runtime_error("quantized note time overflows int64");
    return left + right;
}
// Exact rounded fraction without an overflowing product or long double.
std::int64_t scale(std::int64_t magnitude, std::int64_t numerator, std::int64_t denominator) {
    return (magnitude / denominator) * numerator +
           ((magnitude % denominator) * numerator + denominator / 2) / denominator;
}
std::int64_t phase(std::int64_t value, std::int64_t period) {
    const auto remainder = value % period;
    return remainder < 0 ? remainder + period : remainder;
}
QuantizeSettings parseSettings(const Payload &payload) {
    QuantizeSettings result;
    result.gridTicks = signedValue(payload.at("grid_ticks"));
    result.originTicks = signedValue(payload.at("origin_ticks"));
    const auto amount = signedValue(payload.at("strength")), swing = signedValue(payload.at("swing"));
    if (result.gridTicks < 1 || result.gridTicks > std::numeric_limits<std::int64_t>::max() / 2 ||
        amount < 0 || amount > 10000 || swing < 0 || swing > 10000)
        throw std::runtime_error("invalid quantize grid, strength or swing");
    result.strength = static_cast<int>(amount);
    result.swing = static_cast<int>(swing);
    result.starts = payload.at("starts").get<bool>();
    result.ends = payload.at("ends").get<bool>();
    if (!result.starts && !result.ends)
        throw std::runtime_error("choose note starts, ends, or both");
    return result;
}
std::int64_t quantized(std::int64_t time, const QuantizeSettings &options) {
    const auto period = options.gridTicks * 2;
    auto position = phase(time, period) - phase(options.originTicks, period);
    if (position < 0)
        position += period;
    const auto odd = options.gridTicks + scale(options.gridTicks, options.swing, 20000);
    std::int64_t delta = -position;
    for (const auto point : {odd, period}) {
        const auto candidate = point - position;
        // Exact half-way positions prefer the later grid line.
        if (std::abs(candidate) <= std::abs(delta))
            delta = candidate;
    }
    const auto movement = scale(std::abs(delta), options.strength, 10000);
    return add(time, delta < 0 ? -movement : movement);
}
bool quantizeApply(OpContext &context, const Payload &payload, std::string &error) {
    try {
        auto stream = read(context, payload);
        const auto ids = selection(payload);
        selectedExist(stream, ids);
        const auto options = parseSettings(payload);
        for (auto &record : stream.records)
            if (ids.contains(record.note.note_id)) {
                auto &note = record.note;
                const auto oldEnd = add(note.start_ticks, note.dur_ticks);
                const auto start = options.starts ? quantized(note.start_ticks, options) : note.start_ticks;
                const auto end = options.ends ? quantized(oldEnd, options) : add(start, note.dur_ticks);
                // Minimum one tick when independent start/end snaps collapse a note.
                const auto positiveEnd = end > start ? end : add(start, 1);
                if (start < 0 && positiveEnd > std::numeric_limits<std::int64_t>::max() + start)
                    throw std::runtime_error("quantized duration overflows int64");
                note.start_ticks = start;
                note.dur_ticks = positiveEnd - start;
            }
        return save(context, payload, stream.encode(true), error);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool captureStream(OpContext &context, const Payload &payload, Payload &inverse, std::string &error) {
    try {
        const auto stream = read(context, payload);
        std::vector<std::uint8_t> bytes;
        bytes.reserve(stream.bytes.size());
        for (auto value : stream.bytes)
            bytes.push_back(std::to_integer<std::uint8_t>(value));
        inverse = {{"clip", payload.at("clip")}, {"stream", {{"bytes", Payload::binary(bytes)}}}};
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool restoreApply(OpContext &context, const Payload &payload, std::string &error) {
    try {
        const auto current = read(context, payload);
        std::vector<std::byte> bytes;
        const auto &packet = payload.at("stream");
        if (!packet.is_object() || packet.size() != 1 || !packet.contains("bytes"))
            throw std::runtime_error("invalid notes stream packet");
        const auto &data = packet.at("bytes");
        if (data.is_binary()) {
            for (auto value : data.get_binary())
                bytes.push_back(static_cast<std::byte>(value));
        } else if (data.is_array()) {
            for (const auto &value : data) {
                const auto byte = signedValue(value);
                if (byte < 0 || byte > 255)
                    throw std::runtime_error("invalid notes stream byte");
                bytes.push_back(static_cast<std::byte>(byte));
            }
        } else
            throw std::runtime_error("invalid notes stream bytes");
        const Stream restored(std::move(bytes));
        if (restored.ids != current.ids)
            throw std::runtime_error("timing restore cannot replace note identities");
        return save(context, payload, restored.bytes, error);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
int parseSemitones(const Payload &payload) {
    const auto value = signedValue(payload.at("semitones"));
    if (value < -127 || value > 127)
        throw std::runtime_error("transpose must be within -127..127 semitones");
    return static_cast<int>(value);
}
bool transposeApply(OpContext &context, const Payload &payload, std::string &error) {
    try {
        auto stream = read(context, payload);
        const auto ids = selection(payload);
        selectedExist(stream, ids);
        const auto distance = parseSemitones(payload);
        for (auto &record : stream.records)
            if (ids.contains(record.note.note_id)) {
                const auto key = static_cast<int>(record.note.key) + distance;
                if (key < 0 || key > 127)
                    throw std::runtime_error("transpose would exceed the MIDI key range");
                record.note.key = static_cast<std::uint8_t>(key);
            }
        return save(context, payload, stream.encode(false), error);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool transposeInverse(OpContext &context, const Payload &payload, Payload &inverse, std::string &error) {
    try {
        const auto stream = read(context, payload);
        const auto ids = selection(payload);
        selectedExist(stream, ids);
        inverse = payload;
        inverse["semitones"] = -parseSemitones(payload);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
constexpr Field quantizeFields[] = {{"clip", FieldType::Int},       {"note_ids", FieldType::Array},
                                    {"grid_ticks", FieldType::Int}, {"origin_ticks", FieldType::Int},
                                    {"strength", FieldType::Int},   {"swing", FieldType::Int},
                                    {"starts", FieldType::Bool},    {"ends", FieldType::Bool}};
constexpr Field transposeFields[] = {
    {"clip", FieldType::Int}, {"note_ids", FieldType::Array}, {"semitones", FieldType::Int}};
constexpr Field restoreFields[] = {{"clip", FieldType::Int}, {"stream", FieldType::Object}};
const OpDescriptor descriptors[] = {
    {"note.quantize", "Quantize explicit note starts and/or ends", Scope::Edit, EngineImpact::Snapshot,
     quantizeFields, false, false, quantizeApply, captureStream, "note.restoreNotes"},
    {"note.transpose", "Transpose explicit notes by semitones", Scope::Edit, EngineImpact::Snapshot,
     transposeFields, false, false, transposeApply, transposeInverse, ""},
    {"note.restoreNotes", "Restore an exact notes stream with the same note identities", Scope::Edit,
     EngineImpact::Snapshot, restoreFields, false, false, restoreApply, captureStream, ""}};

double scaleTonic(const engine::ScaleContext &context, std::optional<double> tonic) {
    const auto *scale = context.scale();
    if (!scale || (!tonic && scale->tuningId))
        throw std::runtime_error("pitch utility needs a scale and an absolute tonic for a custom tuning");
    const auto root = tonic.value_or(static_cast<double>(scale->root) * 100.0);
    if (!std::isfinite(root))
        throw std::runtime_error("scale tonic must be finite");
    return root;
}

template <class Destination>
PreparedDiff preparePitches(const Snapshot &snapshot, std::span<const std::uint64_t> ids,
                             std::string_view label, Destination destination) {
    std::set<std::uint64_t> remaining(ids.begin(), ids.end());
    if (remaining.contains(0) || remaining.size() != ids.size())
        throw std::runtime_error("pitch utility needs unique assigned note IDs");
    Diff diff;
    std::string error;
    for (const auto &record : snapshot.records()) {
        if (remaining.empty())
            break;
        auto note = values(record, error);
        if (!note)
            throw std::runtime_error(error);
        if (!remaining.erase(note->id))
            continue;
        const auto cents = static_cast<double>(note->key) * 100.0 + note->cents;
        const auto encoded = engine::encodeMidiPitch(destination(cents));
        if (!encoded)
            throw std::runtime_error("target is outside the native pitch range or precision");
        if (cents == static_cast<double>(encoded->key) * 100.0 + encoded->tuningCents)
            continue;
        note->key = encoded->key;
        note->cents = encoded->tuningCents;
        auto changed = withValues(record, *note, error);
        if (!changed)
            throw std::runtime_error(error);
        diff.changed.push_back({record, std::move(*changed)});
    }
    if (!remaining.empty())
        throw std::runtime_error("pitch utility selection contains an unknown note ID");
    auto result = prepareDiff(snapshot, diff);
    for (auto &request : result.requests)
        request.label = label;
    return result;
}
} // namespace
OpRequest quantizeRequest(std::int64_t clip, std::span<const std::uint64_t> ids,
                          const QuantizeSettings &settings) {
    OpRequest result;
    result.opType = "note.quantize";
    result.label = "Quantize Notes";
    result.payload = {{"clip", clip},
                      {"note_ids", std::vector<std::uint64_t>(ids.begin(), ids.end())},
                      {"grid_ticks", settings.gridTicks},
                      {"origin_ticks", settings.originTicks},
                      {"strength", settings.strength},
                      {"swing", settings.swing},
                      {"starts", settings.starts},
                      {"ends", settings.ends}};
    return result;
}
OpRequest transposeRequest(std::int64_t clip, std::span<const std::uint64_t> ids, int semitones) {
    OpRequest result;
    result.opType = "note.transpose";
    result.label = "Transpose Notes";
    result.payload = {{"clip", clip},
                      {"note_ids", std::vector<std::uint64_t>(ids.begin(), ids.end())},
                      {"semitones", semitones}};
    return result;
}

PreparedDiff prepareFitToScale(const Snapshot &snapshot, std::span<const std::uint64_t> ids,
                                const engine::ScaleContext &context, std::optional<double> tonic) {
    if (!snapshot.ok || ids.empty())
        return prepareDiff(snapshot, {});
    try {
        const auto rootCents = scaleTonic(context, tonic);
        return preparePitches(snapshot, ids, "Fit to Scale", [&](double cents) {
            const auto target = context.nearest(cents, rootCents, true);
            if (!target)
                throw std::runtime_error("Fit to Scale cannot resolve a selected pitch");
            return target->cents;
        });
    } catch (const std::exception &error) {
        PreparedDiff result;
        result.error = error.what();
        return result;
    }
}

PreparedDiff prepareClipFitToScale(Store &store, std::int64_t clip,
                                    std::span<const std::uint64_t> ids, std::optional<double> tonic) {
    try {
        if (audio::onAudioThread)
            throw std::runtime_error("Fit to Scale belongs off the audio callback");
        auto &db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        const auto snapshot = captureNotes(store, clip);
        if (!snapshot.ok || ids.empty())
            return prepareDiff(snapshot, {});
        const auto scale = engine::readClipScaleContext(store, clip);
        if (!scale.context)
            throw std::runtime_error(scale.error);
        return prepareFitToScale(snapshot, ids, *scale.context, tonic);
    } catch (const std::exception &error) {
        PreparedDiff result;
        result.error = error.what();
        return result;
    }
}

PreparedDiff prepareInvertPitches(const Snapshot &snapshot, std::span<const std::uint64_t> ids,
                                  double axisCents) {
    if (!snapshot.ok || ids.empty())
        return prepareDiff(snapshot, {});
    try {
        if (!std::isfinite(axisCents))
            throw std::runtime_error("pitch inversion axis must be finite");
        return preparePitches(snapshot, ids, "Invert Pitches",
                              [=](double cents) { return axisCents + (axisCents - cents); });
    } catch (const std::exception &error) {
        PreparedDiff result;
        result.error = error.what();
        return result;
    }
}

PreparedDiff prepareTransposeDegrees(const Snapshot &snapshot, std::span<const std::uint64_t> ids,
                                      const engine::ScaleContext &context, std::int64_t steps,
                                      std::optional<double> tonic) {
    if (!snapshot.ok || ids.empty() || steps == 0)
        return prepareDiff(snapshot, {});
    try {
        const auto root = scaleTonic(context, tonic);
        return preparePitches(snapshot, ids, "Transpose Scale Degrees", [&](double cents) {
            const auto source = context.nearest(cents, root, true);
            const auto native = source ? engine::encodeMidiPitch(source->cents) : std::nullopt;
            if (!native || cents != static_cast<double>(native->key) * 100.0 + native->tuningCents)
                throw std::runtime_error("degree transposition needs an in-scale pitch");
            const auto destination = context.stepDegree(source->degree, source->period, steps, root);
            if (!destination)
                throw std::runtime_error("scale degree transposition exceeds the supported pitch range");
            return destination->cents;
        });
    } catch (const std::exception &error) {
        PreparedDiff result;
        result.error = error.what();
        return result;
    }
}

std::span<const OpDescriptor> transformOps() {
    return descriptors;
}
} // namespace adi::notes
