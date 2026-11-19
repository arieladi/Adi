// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/blob.hpp"
#include "adi/history.hpp"
#include "adi/note_diff.hpp"
#include "adi/note_transforms.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace adi;
using Bytes = std::vector<std::byte>;
constexpr std::uint64_t wideId = std::numeric_limits<std::uint64_t>::max();
int checks = 0, failures = 0;
bool check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", message.c_str()); }
    return condition;
}
Payload binary(const Bytes& bytes) {
    std::vector<std::uint8_t> result;
    result.reserve(bytes.size());
    for (const auto byte : bytes) result.push_back(std::to_integer<std::uint8_t>(byte));
    return Payload::binary(std::move(result));
}
Payload byteArray(const Bytes& bytes) {
    Payload result = Payload::array();
    for (const auto byte : bytes) result.push_back(std::to_integer<unsigned>(byte));
    return result;
}
NoteRecord note(std::uint64_t id, std::int64_t start, std::int64_t duration, int key = 60,
                std::uint16_t flags = 0x8007) {
    NoteRecord result{};
    result.note_id = id; result.start_ticks = start; result.dur_ticks = duration;
    result.key = static_cast<std::uint8_t>(key); result.vel_on = 93; result.vel_off = 37;
    result.channel = 7; result.probability = 4321; result.tuning_cents = -117.25f; result.flags = flags;
    return result;
}
std::vector<NoteRecord> seed(bool sorted = true) {
    std::vector<NoteRecord> result{note(wideId, 100, 60, 72), note(17, 100, 80, 32, 0x400e),
                                   note(3, 220, 25, 64, 0x2010)};
    if (!sorted) std::rotate(result.begin(), result.end() - 1, result.end());
    return result;
}
Bytes stream(const std::vector<NoteRecord>& records, std::uint16_t stride = 48,
             std::uint32_t flags = StreamFlags::SortedByTime) {
    const StreamHeader header{static_cast<std::uint32_t>(FourCC::Notes), 7, stride,
                               static_cast<std::uint32_t>(records.size()), flags};
    Bytes result(sizeof header + records.size() * stride + 7, std::byte{0x5a});
    std::memcpy(result.data(), &header, sizeof header);
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto offset = sizeof header + index * stride;
        std::memcpy(result.data() + offset, &records[index], sizeof(NoteRecord));
        for (std::size_t tail = sizeof(NoteRecord); tail < stride; ++tail)
            result[offset + tail] = static_cast<std::byte>((index * 29 + tail) & 255u);
    }
    return result;
}
StreamHeader headerOf(const Bytes& bytes) {
    if (bytes.size() < sizeof(StreamHeader)) throw std::runtime_error("fixture stream header");
    StreamHeader header{}; std::memcpy(&header, bytes.data(), sizeof header);
    if (header.rec_size < sizeof(NoteRecord) ||
        header.count > (bytes.size() - sizeof header) / header.rec_size)
        throw std::runtime_error("fixture stream extent");
    return header;
}
std::size_t offsetOf(const Bytes& bytes, std::uint64_t id) {
    const auto header = headerOf(bytes);
    for (std::uint32_t index = 0; index < header.count; ++index) {
        const auto offset = sizeof header + static_cast<std::size_t>(index) * header.rec_size;
        NoteRecord value{}; std::memcpy(&value, bytes.data() + offset, sizeof value);
        if (value.note_id == id) return offset;
    }
    throw std::runtime_error("fixture note identity absent");
}
template<class Function> Bytes patch(Bytes bytes, std::uint64_t id, Function function) {
    const auto offset = offsetOf(bytes, id);
    NoteRecord value{}; std::memcpy(&value, bytes.data() + offset, sizeof value);
    function(value); std::memcpy(bytes.data() + offset, &value, sizeof value); return bytes;
}
// Independent raw-byte oracle: only the header count and selected prefix are
// changed. It never calls the production note-diff codec or row writer.
Bytes sortedByStart(Bytes bytes) {
    const auto header = headerOf(bytes);
    std::vector<Bytes> records;
    for (std::uint32_t index = 0; index < header.count; ++index) {
        const auto offset = sizeof header + static_cast<std::size_t>(index) * header.rec_size;
        records.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                             bytes.begin() + static_cast<std::ptrdiff_t>(offset + header.rec_size));
    }
    std::stable_sort(records.begin(), records.end(), [](const Bytes& a, const Bytes& b) {
        std::int64_t first = 0, second = 0;
        std::memcpy(&first, a.data(), sizeof first); std::memcpy(&second, b.data(), sizeof second);
        return first < second;
    });
    for (std::size_t index = 0; index < records.size(); ++index)
        std::memcpy(bytes.data() + sizeof header + index * header.rec_size, records[index].data(), header.rec_size);
    return bytes;
}
Bytes erase(Bytes bytes, std::uint64_t id) {
    auto header = headerOf(bytes); const auto offset = offsetOf(bytes, id);
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + header.rec_size));
    --header.count; std::memcpy(bytes.data(), &header, sizeof header); return bytes;
}
Bytes appendNative(Bytes bytes, const NoteRecord& value) {
    auto header = headerOf(bytes);
    if (header.rec_size != sizeof value) throw std::runtime_error("fixture needs native stride");
    const auto at = sizeof header + static_cast<std::size_t>(header.count) * header.rec_size;
    const auto* begin = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), begin, begin + sizeof value);
    ++header.count; std::memcpy(bytes.data(), &header, sizeof header);
    return (header.flags & StreamFlags::SortedByTime) ? sortedByStart(std::move(bytes)) : bytes;
}
std::vector<Payload> rows(SQLite::Database& db, const char* table) {
    SQLite::Statement query(db, std::string("SELECT * FROM ") + table + " ORDER BY id");
    std::vector<Payload> result;
    while (query.executeStep()) {
        Payload row = Payload::object();
        for (int column = 0; column < query.getColumnCount(); ++column) {
            const auto cell = query.getColumn(column); const std::string name = query.getColumnName(column);
            switch (cell.getType()) {
                case SQLITE_INTEGER: row[name] = cell.getInt64(); break;
                case SQLITE_FLOAT: row[name] = cell.getDouble(); break;
                case SQLITE_TEXT: row[name] = cell.getString(); break;
                case SQLITE_NULL: row[name] = nullptr; break;
                case SQLITE_BLOB: {
                    std::vector<std::uint8_t> bytes;
                    if (cell.getBytes() > 0) {
                        const auto* first = static_cast<const std::uint8_t*>(cell.getBlob());
                        bytes.assign(first, first + cell.getBytes());
                    }
                    row[name] = Payload::binary(std::move(bytes)); break;
                }
                default: throw std::runtime_error("fixture SQL storage class");
            }
        }
        result.push_back(std::move(row));
    }
    return result;
}
struct State {
    std::vector<Payload> streams, expressions, clips;
    bool operator==(const State&) const = default;
};
struct Project {
    test::TempDirectory folder{"note_diff_compat", "project"};
    std::unique_ptr<Store> store;
    explicit Project(std::optional<Bytes> bytes = stream(seed()), bool extensions = true, bool auxiliary = true) {
        StoreError error{}; store = Store::createEmpty(folder.path() / "song.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        auto& db = store->db();
        db.exec("INSERT INTO tracks(id,kind) VALUES(1,'midi');"
                "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES"
                "(1,1,'midi',0,1000),(2,1,'midi',0,1000),(-1,1,'midi',0,1000)");
        if (extensions) db.exec("ALTER TABLE event_streams ADD COLUMN future_text TEXT DEFAULT 'extension';"
                                "ALTER TABLE event_streams ADD COLUMN future_blob BLOB DEFAULT X'005AFF';"
                                "ALTER TABLE note_expression ADD COLUMN future_int INTEGER DEFAULT 73;"
                                "ALTER TABLE note_expression ADD COLUMN future_blob BLOB DEFAULT X''");
        if (bytes && !store->putEventStream(1, "notes", *bytes)) throw std::runtime_error(store->lastError());
        // Lower signed row ID, same kind and clip, but an auxiliary address.
        // A canonical handler must not select this row by kind alone.
        if (auxiliary) {
            SQLite::Statement addressed(db, "INSERT INTO event_streams(id,clip_id,stream_kind,channel,controller,data) VALUES(-17,1,'notes',-2,NULL,?)");
            const auto other = stream({note(wideId, 9, 12)}, 40);
            addressed.bind(1, other.data(), static_cast<int>(other.size())); addressed.exec();
        }
        const auto foreign = stream({note(wideId, 8, 16)}, 40);
        if (!store->putEventStream(2, "notes", foreign)) throw std::runtime_error(store->lastError());
    }
    State state() { return {rows(store->db(), "event_streams"), rows(store->db(), "note_expression"), rows(store->db(), "clips")}; }
    std::optional<Bytes> blob() {
        SQLite::Statement query(store->db(), "SELECT data FROM event_streams WHERE clip_id=1 AND stream_kind='notes' AND channel IS NULL AND controller IS NULL");
        if (!query.executeStep()) return std::nullopt;
        const auto cell = query.getColumn(0); Bytes result;
        if (cell.getBytes() > 0) {
            const auto* first = static_cast<const std::byte*>(cell.getBlob());
            result.assign(first, first + cell.getBytes());
        }
        return result;
    }
    std::int64_t streamId() {
        return store->db().execAndGet("SELECT id FROM event_streams WHERE clip_id=1 AND stream_kind='notes' AND channel IS NULL AND controller IS NULL").getInt64();
    }
    void expression(std::int64_t rowId, std::int64_t clip, std::uint64_t noteId, int dimension) {
        ExpressionPoint point{}; point.time_ticks = 5; point.value = 0.3125f;
        auto bytes = writeStream<ExpressionPoint>(FourCC::Expression, std::vector<ExpressionPoint>{point});
        bytes.push_back(std::byte{0x37});
        SQLite::Statement insert(store->db(), "INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(?,?,?,?,?)");
        insert.bind(1, rowId); insert.bind(2, clip); insert.bind(3, std::bit_cast<std::int64_t>(noteId));
        insert.bind(4, dimension); insert.bind(5, bytes.data(), static_cast<int>(bytes.size())); insert.exec();
    }
    bool commit(const OpRequest& request, const char* message) {
        const auto result = OpJournal(*store).commit(request);
        return check(result.ok, std::string(message) + (result.error.empty() ? "" : ": " + result.error));
    }
    bool reopen() {
        const auto path = store->path(); store.reset(); StoreError error{}; store = Store::open(path, error);
        return check(bool(store), "reopen compatibility history");
    }
};
OpRequest request(const char* name, Payload payload) {
    OpRequest result; result.opType = name; result.payload = std::move(payload); return result;
}
Payload full(std::uint64_t id, std::int64_t start = 100, std::int64_t duration = 60, int key = 72) {
    return {{"clip", 1}, {"note", id}, {"start", start}, {"dur", duration}, {"key", key},
            {"vel", 93}, {"velOff", 37}, {"chan", 7}, {"prob", 4321}, {"cents", -117.25}};
}
void replaceCanonical(State& state, std::int64_t id, const Bytes& bytes) {
    const auto found = std::find_if(state.streams.begin(), state.streams.end(),
        [id](const Payload& row) { return row.at("id") == id; });
    if (found == state.streams.end()) throw std::runtime_error("oracle canonical row absent");
    (*found)["data"] = binary(bytes);
}
void exactHistory(Project& project, const State& before, const State& after, bool reopen = false) {
    const auto undone = History(*project.store).undo();
    if (!check(undone.ok && undone.opsApplied == 1 && project.state() == before,
               "one Undo restores exact canonical, auxiliary, foreign, expression and future SQL rows")) return;
    if (reopen && !project.reopen()) return;
    const auto redone = History(*project.store).redo();
    check(redone.ok && redone.opsApplied == 1 && project.state() == after,
          "one Redo reproduces exact bytes and full typed rows");
}
std::optional<Payload> inverse(Project& project) {
    const auto logged = OpJournal(*project.store).recent(1);
    if (!check(logged.size() == 1 && logged.front().inverse.has_value(), "operation has one stored inverse")) return {};
    return logged.front().inverse;
}
void replaceLog(Project& project, bool inverseColumn, const Payload& payload) {
    const auto encoded = encodePayload(payload);
    SQLite::Statement update(project.store->db(), inverseColumn ?
        "UPDATE ops SET inverse=? WHERE seq=(SELECT MAX(seq) FROM ops)" :
        "UPDATE ops SET payload=? WHERE seq=(SELECT MAX(seq) FROM ops)");
    update.bind(1, encoded.data(), static_cast<int>(encoded.size()));
    if (update.exec() != 1) throw std::runtime_error("historical journal fixture update");
}
void descriptors() {
    const std::array names{"note.insert", "note.delete", "note.move", "note.resize", "note.setVelocity"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto* descriptor = OpRegistry::instance().find(names[index]);
        if (!check(descriptor != nullptr, "P0 note descriptor remains registered")) continue;
        const auto paired = index == 0 ? "note.delete" : index == 1 ? "note.insert" : "";
        check(descriptor->scope == Scope::Edit && descriptor->engineImpact == EngineImpact::Snapshot &&
                  !descriptor->ephemeral && descriptor->coalescable == (index == 4) && descriptor->inverseOp == paired,
              "P0 descriptor scope, impact, pairing and velocity coalescing remain compatible");
    }
}
void wrapperBytes() {
    for (const bool sorted : {true, false}) {
        for (const int operation : {0, 1, 2, 3, 4}) {
            const auto original = stream(seed(sorted), operation == 0 ? 40 : 48,
                                         sorted ? StreamFlags::SortedByTime : 0);
            Project project(original);
            project.expression(41, 1, wideId, 1); project.expression(42, 1, wideId, 77);
            project.expression(43, 1, 17, 1); project.expression(44, 2, wideId, 1);
            const auto before = project.state(); const auto rowId = project.streamId();
            auto expected = before; auto bytes = original; OpRequest edit;
            if (operation == 0) {
                edit = request("note.insert", full(99, 100, 45, 16));
                bytes = appendNative(std::move(bytes), note(99, 100, 45, 16, 0));
            } else if (operation == 1) {
                edit = request("note.delete", {{"clip", 1}, {"note", wideId}});
                bytes = erase(std::move(bytes), wideId);
                std::erase_if(expected.expressions, [](const Payload& row) {
                    return row.at("clip_id") == 1 && row.at("note_id") == -1;
                });
            } else if (operation == 2) {
                edit = request("note.move", {{"clip", 1}, {"note", wideId}, {"start", -20}, {"key", 67}});
                bytes = patch(std::move(bytes), wideId, [](NoteRecord& value) { value.start_ticks = -20; value.key = 67; });
                if (sorted) bytes = sortedByStart(std::move(bytes));
            } else if (operation == 3) {
                edit = request("note.resize", {{"clip", 1}, {"note", wideId}, {"dur", 190}});
                bytes = patch(std::move(bytes), wideId, [](NoteRecord& value) { value.dur_ticks = 190; });
            } else {
                edit = request("note.setVelocity", {{"clip", 1}, {"note", wideId}, {"vel", 33}});
                bytes = patch(std::move(bytes), wideId, [](NoteRecord& value) { value.vel_on = 33; });
            }
            replaceCanonical(expected, rowId, bytes);
            if (!project.commit(edit, "legacy wrapper commits through its unchanged opcode")) continue;
            check(project.state() == expected, "wrapper exactly preserves inactive/unknown flags, tails, trailer, expression and other rows");
            const auto capture = inverse(project);
            if (capture) {
                if (operation == 4) {
                    check(*capture == Payload{{"clip", 1}, {"note", wideId}, {"vel", 93}},
                          "velocity inverse remains scalar and independent of intermediate-state hashes");
                } else {
                    check(capture->contains("note_capture") && capture->size() == 2,
                          "insert/delete/move/resize write the exact inverse-only note capture");
                    const auto* descriptor = OpRegistry::instance().find(edit.opType);
                    if (descriptor) {
                        const std::string target = descriptor->inverseOp.empty() ? edit.opType : std::string(descriptor->inverseOp);
                        const auto count = OpJournal(*project.store).count();
                        check(!OpJournal(*project.store).commit(request(target.c_str(), *capture)).ok &&
                                  project.state() == expected && OpJournal(*project.store).count() == count,
                              "new caller submission cannot smuggle a captured inverse through a paired wrapper");
                    }
                }
            }
            exactHistory(project, before, expected, operation == 1);
        }
    }
}
void legacyCreation() {
    Project project(std::nullopt, false);
    const auto before = project.state();
    if (!project.commit(request("note.insert", full(wideId, -12, 30)), "legacy insert creates its first native stream")) return;
    const auto after = project.state();
    check(project.blob().has_value() && project.streamId() == 1,
          "legacy first creation uses the checked positive allocation policy after negative auxiliary/foreign rows");
    exactHistory(project, before, after, true);
    Project future(std::nullopt);
    const auto unchanged = future.state();
    const auto refused = OpJournal(*future.store).commit(request("note.insert", full(wideId)));
    check(!refused.ok && !refused.error.empty() && future.state() == unchanged && OpJournal(*future.store).count() == 0,
          "implicit scalar creation refuses future SQL columns instead of evaluating defaults twice");
}
void existingTransforms() {
    for (const int operation : {0, 1, 2, 3}) {
        const auto flags = operation == 0 ? StreamFlags::SortedByTime : StreamFlags::SortedByTime | 0x400u;
        const auto original = stream(seed(), 48, flags);
        Project project(original, true, false); project.expression(51, 1, wideId, 2);
        const auto before = project.state(); auto expected = before; auto bytes = original;
        OpRequest edit;
        const std::array ids{wideId};
        if (operation == 0) {
            edit = request("note.setMute", {{"clip", 1}, {"note", wideId}, {"muted", false}});
            bytes = patch(std::move(bytes), wideId, [](NoteRecord& value) { value.flags = static_cast<std::uint16_t>(value.flags & static_cast<std::uint16_t>(~NoteFlags::Muted)); });
        } else if (operation == 1) {
            edit = notes::quantizeRequest(1, ids, {64});
            bytes = patch(std::move(bytes), wideId, [](NoteRecord& value) { value.start_ticks = 128; });
            bytes = sortedByStart(std::move(bytes));
        } else if (operation == 2) {
            edit = notes::transposeRequest(1, ids, -12);
            bytes = patch(std::move(bytes), wideId, [](NoteRecord& value) { value.key = 60; });
        } else {
            bytes = patch(std::move(bytes), wideId, [](NoteRecord& value) { value.vel_off = 91; });
            edit = request("note.restoreNotes", {{"clip", 1}, {"stream", {{"bytes", byteArray(bytes)}}}});
        }
        replaceCanonical(expected, project.streamId(), bytes);
        if (!project.commit(edit, "existing mute/quantize/transpose/restore handler remains compatible")) continue;
        check(project.state() == expected, "existing handler preserves original future bytes, header admission and SQL columns");
        exactHistory(project, before, expected, operation == 3);
    }
}
void historicalScalars() {
    for (const int operation : {0, 1, 2, 3, 4, 5}) {
        const auto first = note(wideId, 100, 60, 72, 0);
        Project project(stream({first, note(17, 220, 80, 32, 0)}, 40), true, operation != 5);
        const auto before = project.state(); OpRequest edit; Payload oldInverse;
        if (operation == 0) {
            edit = request("note.insert", full(99, 170, 45, 16)); oldInverse = {{"clip", 1}, {"note", 99}};
        } else if (operation == 1) {
            edit = request("note.delete", {{"clip", 1}, {"note", wideId}}); oldInverse = full(wideId);
        } else if (operation == 2) {
            edit = request("note.move", {{"clip", 1}, {"note", wideId}, {"start", -50}, {"key", 67}});
            oldInverse = {{"clip", 1}, {"note", wideId}, {"start", 100}, {"key", 72}};
        } else if (operation == 3) {
            edit = request("note.resize", {{"clip", 1}, {"note", wideId}, {"dur", 190}});
            oldInverse = {{"clip", 1}, {"note", wideId}, {"dur", 60}};
        } else if (operation == 4) {
            edit = request("note.setVelocity", {{"clip", 1}, {"note", wideId}, {"vel", 33}});
            oldInverse = {{"clip", 1}, {"note", wideId}, {"vel", 93}};
        } else {
            const auto original = project.blob();
            if (!check(original.has_value(), "historical restore fixture has a canonical stream")) continue;
            const auto changed = patch(*original, wideId, [](NoteRecord& value) { value.vel_on = 51; });
            edit = request("note.restoreNotes", {{"clip", 1}, {"stream", {{"bytes", byteArray(changed)}}}});
            oldInverse = {{"clip", 1}, {"stream", {{"bytes", binary(*original)}}}};
        }
        if (!project.commit(edit, "old valid scalar forward payload still submits")) continue;
        const auto after = project.state();
        // Literal pre-MI-01 bytes are installed in the actual log. History
        // dispatches today's inverseOp, bypassing new forward-only validation.
        replaceLog(project, true, oldInverse);
        exactHistory(project, before, after, true);
    }
    Project absent(std::nullopt, false);
    if (!absent.commit(request("note.insert", full(99)), "historical first insertion fixture")) return;
    const auto after = absent.state(); replaceLog(absent, true, {{"clip", 1}, {"note", 99}});
    check(History(*absent.store).undo().ok && absent.blob().has_value(),
          "old scalar insert inverse remains valid and retains its historically empty stream");
    const auto empty = absent.blob();
    check(empty && headerOf(*empty).count == 0, "historical scalar inverse deletes its note without inventing absence metadata");
    check(History(*absent.store).redo().ok && absent.state() == after, "old first-insertion forward replays after scalar undo");
}
// Build literal historical rows without running the modern note mutation.
// The harmless seed provides a valid transaction/head; only its stored opcode
// and CBOR are replaced. Material is established independently below.
bool historicalRow(Project& project, const char* opcode, const Payload& forward, const Payload& reverse) {
    const auto name = project.store->db().execAndGet("SELECT name FROM tracks WHERE id=1").getString();
    if (!project.commit(request("track.rename", {{"id", 1}, {"name", name}}), "seed historical journal transaction")) return false;
    const auto payload = encodePayload(forward), inverseBytes = encodePayload(reverse);
    SQLite::Statement update(project.store->db(), "UPDATE ops SET op_type=?,payload=?,inverse=? WHERE seq=(SELECT MAX(seq) FROM ops)");
    update.bind(1, opcode); update.bind(2, payload.data(), static_cast<int>(payload.size()));
    update.bind(3, inverseBytes.data(), static_cast<int>(inverseBytes.size()));
    return check(update.exec() == 1, "install literal old forward and inverse bytes in the journal");
}
void setCanonical(Project& project, const Bytes& bytes) {
    SQLite::Statement update(project.store->db(), "UPDATE event_streams SET data=? WHERE clip_id=1 AND stream_kind='notes' AND channel IS NULL AND controller IS NULL");
    update.bind(1, bytes.data(), static_cast<int>(bytes.size()));
    if (update.exec() != 1) throw std::runtime_error("historical canonical material fixture");
}
bool repeatedHistoricalRoundTrip(Project& project, const State& before, const State& after) {
    if (!check(History(*project.store).undo().ok && project.state() == before,
               "historical scalar Undo preserves the complete surviving/orphan expression rows")) return false;
    if (!check(History(*project.store).redo().ok && project.state() == after,
               "historical scalar Redo preserves the same complete expression rows")) return false;
    if (!check(History(*project.store).undo().ok && project.state() == before,
               "a second historical Undo remains valid after expression-preserving Redo")) return false;
    if (!project.reopen()) return false;
    if (!check(History(*project.store).redo().ok && project.state() == after,
               "historical scalar expression semantics survive reopen and Redo")) return false;
    if (!check(History(*project.store).undo().ok && project.state() == before,
               "historical scalar Undo remains exact after reopening")) return false;
    if (!project.reopen()) return false;
    return check(History(*project.store).redo().ok && project.state() == after,
                 "reopened old scalar history returns to its exact post-operation state");
}
void historicalExpressions() {
    const auto target = note(wideId, 100, 60, 72, 0);
    const auto other = note(17, 220, 80, 32, 0);
    for (const bool insertion : {false, true}) {
        const auto original = writeStream<NoteRecord>(FourCC::Notes,
            insertion ? std::vector<NoteRecord>{other} : std::vector<NoteRecord>{target, other});
        Project project(original, true, false);
        project.expression(81, 1, wideId, 2); project.expression(82, 1, wideId, 77);
        project.expression(83, 1, 17, 2); project.expression(84, 2, wideId, 2);
        // Preserve both empty-BLOB/defaulted cells and distinct opaque future
        // values, so reassociation cannot get away with recreating rows.
        project.store->db().exec("UPDATE note_expression SET future_int=-91,future_blob=X'00FF7E' WHERE id=82");
        const auto before = project.state();
        if (insertion) {
            const auto rejected = OpJournal(*project.store).commit(request("note.insert", full(wideId)));
            check(!rejected.ok && project.state() == before && OpJournal(*project.store).count() == 0,
                  "new scalar insertion still refuses preexisting orphan expression ownership");
        }
        const auto forward = insertion ? full(wideId) : Payload{{"clip", 1}, {"note", wideId}};
        const auto reverse = insertion ? Payload{{"clip", 1}, {"note", wideId}} : full(wideId);
        if (!historicalRow(project, insertion ? "note.insert" : "note.delete", forward, reverse)) continue;
        const auto resultingBytes = insertion ? appendNative(original, target) : erase(original, wideId);
        setCanonical(project, resultingBytes);
        auto expected = before; replaceCanonical(expected, project.streamId(), resultingBytes);
        if (!check(project.state() == expected, "literal old operation changes only ANOT and retains all expression rows")) continue;
        if (!repeatedHistoricalRoundTrip(project, before, expected)) continue;
        if (insertion) {
            // Compatibility is attached to the old recorded operation. A new
            // ordinary delete after that replay must use the modern capture.
            if (!project.commit(request("note.delete", {{"clip", 1}, {"note", wideId}}),
                                "new delete after historical replay uses current semantics")) continue;
            auto modern = expected; replaceCanonical(modern, project.streamId(), original);
            std::erase_if(modern.expressions, [](const Payload& row) {
                return row.at("clip_id") == 1 && row.at("note_id") == -1;
            });
            check(project.state() == modern, "new submission removes target expressions despite prior old scalar replay");
            exactHistory(project, expected, modern, true);
        }
    }
}
void historicalNegativeStreamIds() {
    for (const std::int64_t greatest : {-7, -2, -1}) {
        Project project(std::nullopt, false, false);
        auto& db = project.store->db(); db.exec("DELETE FROM event_streams");
        const auto foreign = writeStream<NoteRecord>(FourCC::Notes, std::vector<NoteRecord>{note(17, 0, 10, 32, 0)});
        {
            SQLite::Statement insert(db, "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(?,2,'notes',?)");
            insert.bind(1, greatest); insert.bind(2, foreign.data(), static_cast<int>(foreign.size())); insert.exec();
        }
        const auto before = project.state();
        if (!historicalRow(project, "note.insert", full(99), {{"clip", 1}, {"note", 99}})) continue;
        if (!check(OpJournal(*project.store).setHeadSeq(std::nullopt), "historical first insertion starts from its pre-operation root")) continue;
        if (!project.reopen()) continue;
        if (!check(History(*project.store).redo().ok && project.blob().has_value(),
                   "old first insertion replays when all previous stream keys are negative")) continue;
        const auto actualId = project.streamId();
        check(actualId == greatest + 1, "historical replay repeats SQLite MAX+1 including negative and zero IDs");
        auto expected = before;
        const auto createdBytes = writeStream<NoteRecord>(FourCC::Notes, std::vector<NoteRecord>{note(99, 100, 60, 72, 0)});
        expected.streams.push_back({{"id", greatest + 1}, {"clip_id", 1}, {"stream_kind", "notes"},
                                   {"channel", nullptr}, {"controller", nullptr}, {"data", binary(createdBytes)}});
        check(project.state() == expected, "old first-insertion Redo retains exact native row identity and bytes");
        auto empty = expected;
        replaceCanonical(empty, greatest + 1, writeStream<NoteRecord>(FourCC::Notes, std::vector<NoteRecord>{}));
        if (!check(History(*project.store).undo().ok && project.state() == empty,
                   "old scalar Undo keeps its exact negative/zero empty stream row")) continue;
        if (!project.reopen()) continue;
        check(History(*project.store).redo().ok && project.state() == expected,
              "old scalar Redo reuses the historical negative/zero stream identity after reopen");
    }
}
void capturedScalarRedo() {
    Project project(std::nullopt, false);
    const auto before = project.state(); const auto forward = request("note.insert", full(99));
    if (!project.commit(forward, "new scalar first insertion captures exact row identity")) return;
    const auto after = project.state(); const auto originalId = project.streamId();
    const auto captured = inverse(project);
    if (!check(captured && captured->contains("note_capture"), "new scalar insertion records an exact counterpart capture")) return;
    if (!check(History(*project.store).undo().ok && project.state() == before,
               "new first-insertion Undo restores absence before allocator pressure")) return;
    project.store->db().exec("INSERT INTO event_streams(id,clip_id,stream_kind,channel,controller,data) VALUES(900,2,'cc',4,7,X'0055FF')");
    const auto baselineWithForeign = project.state();
    auto expected = after;
    const auto extra = std::find_if(baselineWithForeign.streams.begin(), baselineWithForeign.streams.end(),
                                   [](const Payload& row) { return row.at("id") == 900; });
    if (!check(extra != baselineWithForeign.streams.end(), "unrelated stream was inserted without changing the undo head")) return;
    expected.streams.push_back(*extra);
    if (!check(History(*project.store).redo().ok && project.state() == expected && project.streamId() == originalId,
               "new captured scalar Redo reuses its recorded creation row despite a higher unrelated ID")) return;
    if (!check(History(*project.store).undo().ok && project.state() == baselineWithForeign,
               "next Undo still matches the exact recorded stream identity and retains unrelated rows")) return;
    if (!project.reopen()) return;
    check(History(*project.store).redo().ok && project.state() == expected,
          "captured scalar Redo retains original native defaults/identity after reopen");

    Project tampered(std::nullopt, false); const auto unchanged = tampered.state();
    if (!tampered.commit(forward, "captured forward-payload mismatch fixture")) return;
    const auto intended = tampered.state(); const auto savedCapture = inverse(tampered);
    if (!check(History(*tampered.store).undo().ok && tampered.state() == unchanged,
               "captured mismatch fixture returns to its empty original stream state")) return;
    auto forged = forward.payload; forged["key"] = 73; replaceLog(tampered, false, forged);
    const auto count = OpJournal(*tampered.store).count(); const auto head = OpJournal(*tampered.store).headSeq();
    const auto refused = History(*tampered.store).redo();
    check(!refused.ok && !refused.error.empty() && tampered.state() == unchanged &&
              OpJournal(*tampered.store).count() == count && OpJournal(*tampered.store).headSeq() == head &&
              inverse(tampered) == savedCapture,
          "valid but changed recorded forward fields refuse Redo atomically against the exact counterpart capture");
    replaceLog(tampered, false, forward.payload);
    check(History(*tampered.store).redo().ok && tampered.state() == intended,
          "restoring the recorded forward payload permits the original exact Redo");
}
void velocityCoalescing() {
    Project project(stream(seed(), 48)); const auto before = project.state();
    const auto first = request("note.setVelocity", {{"clip", 1}, {"note", wideId}, {"vel", 60}});
    if (!project.commit(first, "first velocity value commits")) return;
    const auto saved = inverse(project); if (!saved) return;
    const auto latest = request("note.setVelocity", {{"clip", 1}, {"note", wideId}, {"vel", 109}});
    const auto* descriptor = OpRegistry::instance().find(latest.opType);
    if (!check(descriptor && descriptor->apply && descriptor->coalescable && descriptor->inverseOp.empty(),
               "velocity keeps its coalescable symmetric descriptor")) return;
    {
        SQLite::Transaction transaction(project.store->db()); OpContext context{*project.store, project.store->db()};
        std::string error;
        if (!check(descriptor->apply(context, latest.payload, error), "later coalesced velocity value applies")) return;
        replaceLog(project, false, latest.payload); transaction.commit();
    }
    auto expected = before;
    const auto bytes = project.blob();
    if (!check(bytes.has_value(), "coalesced velocity retains its stream")) return;
    replaceCanonical(expected, project.streamId(), patch(stream(seed(), 48), wideId, [](NoteRecord& value) { value.vel_on = 109; }));
    check(project.state() == expected && OpJournal(*project.store).count() == 1 && inverse(project) == saved,
          "coalesced journal keeps the first scalar inverse, latest payload and unchanged tie ordering/tails");
    exactHistory(project, before, expected, true);
}
void malformedWrapperNumbers() {
    Project project(stream(seed(), 40)); const auto before = project.state();
    std::vector<OpRequest> invalid;
    auto insertion = [&](const char* field, Payload value) {
        auto payload = full(99); payload[field] = std::move(value); invalid.push_back(request("note.insert", std::move(payload)));
    };
    for (const auto& value : {Payload(-196), Payload(316), Payload(std::uint64_t{4294967356}), Payload(60.5)}) insertion("key", value);
    insertion("vel", 349); insertion("vel", std::uint64_t{4294967389}); insertion("velOff", 293);
    insertion("chan", 263); insertion("prob", 69857); insertion("cents", 1200.00001); insertion("cents", -1200.00001);
    insertion("start", std::uint64_t{1} << 63); insertion("dur", std::numeric_limits<std::uint64_t>::max());
    insertion("start", std::numeric_limits<std::int64_t>::max()); insertion("note", -1); insertion("clip", wideId);
    for (const auto& value : {Payload(316), Payload(std::uint64_t{4294967356}), Payload(60.5)})
        invalid.push_back(request("note.move", {{"clip", 1}, {"note", wideId}, {"start", 100}, {"key", value}}));
    invalid.push_back(request("note.move", {{"clip", 1}, {"note", wideId}, {"start", wideId}, {"key", 60}}));
    for (const auto& value : {Payload(0), Payload(-1), Payload(20.5), Payload(wideId)})
        invalid.push_back(request("note.resize", {{"clip", 1}, {"note", wideId}, {"dur", value}}));
    for (const auto& value : {Payload(349), Payload(std::uint64_t{4294967389}), Payload(93.5), Payload(-163)})
        invalid.push_back(request("note.setVelocity", {{"clip", 1}, {"note", wideId}, {"vel", value}}));
    invalid.push_back(request("note.delete", {{"clip", 1}, {"note", -1}}));
    invalid.push_back(request("note.move", {{"clip", 1}, {"note", -1}, {"start", -20}, {"key", 60}}));
    invalid.push_back(request("note.resize", {{"clip", 1}, {"note", -1}, {"dur", 20}}));
    invalid.push_back(request("note.setVelocity", {{"clip", 1}, {"note", -1}, {"vel", 20}}));
    for (const auto& edit : invalid) {
        const auto result = OpJournal(*project.store).commit(edit);
        check(!result.ok && !result.error.empty() && project.state() == before && OpJournal(*project.store).count() == 0,
              edit.opType + " rejects malformed wide/numeric input before narrowing or mutation");
    }
}
void futureAdditionAndInactive() {
    Project future; const auto original = future.state();
    const auto scalar = OpJournal(*future.store).commit(request("note.insert", full(99)));
    check(!scalar.ok && !scalar.error.empty() && future.state() == original && OpJournal(*future.store).count() == 0,
          "scalar insertion explicitly refuses an unknown wider record layout");
    const auto snapshot = notes::captureNotes(*future.store, 1);
    if (!check(snapshot.ok, "capture future-width stream for explicit-tail addition")) return;
    notes::NoteValues value; value.id = 99; value.start = 170; value.duration = 40;
    value.key = 51; value.velocity = 90; value.releaseVelocity = 30; value.flags = 0x8001;
    std::string error; const auto narrow = notes::makeRecord(value, {}, error);
    if (!check(narrow.has_value(), "native record construction succeeds")) return;
    notes::Diff rejected; rejected.added.push_back(*narrow);
    check(!notes::prepareDiff(snapshot, rejected).ok && future.state() == original,
          "pure preparation never pads a native record into unknown future tail bytes");
    const std::array tail{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
                           std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88}};
    const auto wide = notes::makeRecord(value, tail, error);
    if (!check(wide.has_value(), "explicit complete future record construction succeeds")) return;
    notes::Diff added; added.added.push_back(*wide);
    const auto prepared = notes::prepareDiff(snapshot, added);
    if (!check(prepared.ok && notes::commitDiff(*future.store, prepared).ok, "explicit full-tail addition commits")) return;
    const auto addedCapture = notes::captureNotes(*future.store, 1);
    check(addedCapture.ok && std::find(addedCapture.records().begin(), addedCapture.records().end(), *wide) != addedCapture.records().end(),
          "explicit added note keeps every future-tail byte and inactive flag");
    const auto changed = future.state(); exactHistory(future, original, changed);

    Project absent(std::nullopt); const auto before = absent.state();
    value.id = wideId; value.start = -12; value.duration = 30; value.flags = 0x8005;
    const auto inactive = notes::makeRecord(value, {}, error);
    if (!check(inactive.has_value(), "inactive expressive note is a complete native record")) return;
    const auto creation = notes::captureNotes(*absent.store, 1, -9);
    notes::Diff create; create.added.push_back(*inactive);
    const auto insertion = notes::prepareDiff(creation, create);
    if (!check(creation.ok && insertion.ok && notes::commitDiff(*absent.store, insertion).ok,
               "prepared absent-stream creation captures future SQL defaults and literal signed row ID")) return;
    const auto inserted = absent.state();
    const auto insertedCapture = notes::captureNotes(*absent.store, 1);
    check(insertedCapture.ok && insertedCapture.records().size() == 1 && insertedCapture.records().front() == *inactive,
          "newly created inactive expressive record is byte-identical to the full input record");
    check(absent.streamId() == -9, "prepared creation uses the caller-owned signed stream ID exactly");
    exactHistory(absent, before, inserted, true);
    absent.expression(61, 1, wideId, 2); absent.expression(62, 1, wideId, 77); absent.expression(63, 2, wideId, 2);
    const auto expressive = absent.state(); const auto captured = notes::captureNotes(*absent.store, 1);
    if (!check(captured.ok && captured.records().size() == 1, "capture inactive expressive note for removal")) return;
    notes::Diff removal; removal.removed.push_back(captured.records().front());
    const auto deletion = notes::prepareDiff(captured, removal);
    if (!check(deletion.ok && notes::commitDiff(*absent.store, deletion).ok, "remove inactive note and its exact expression set")) return;
    const auto removed = absent.state();
    check(removed.expressions.size() == 1 && removed.expressions.front().at("clip_id") == 2,
          "uint64 maximum removes signed SQL -1 expressions only from the selected clip");
    exactHistory(absent, expressive, removed, true);
    const auto capturedInverse = inverse(absent);
    if (capturedInverse) check(!OpJournal(*absent.store).commit(request("note.applyDiff", *capturedInverse)).ok && absent.state() == removed,
                              "new note.applyDiff forward submission rejects its inverse-only capture grammar");
}
} // namespace
int main() {
    try {
        descriptors(); wrapperBytes(); legacyCreation(); existingTransforms(); historicalScalars();
        historicalExpressions(); historicalNegativeStreamIds(); capturedScalarRedo(); velocityCoalescing(); malformedWrapperNumbers(); futureAdditionAndInactive();
    } catch (const std::exception& error) { check(false, std::string("exception: ") + error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
