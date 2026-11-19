// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/blob.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/note_overlap.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace adi;
using notes::Record;
using Bytes = std::vector<std::byte>;
constexpr std::uint64_t wideId = (std::uint64_t{1} << 63) + 17;
constexpr auto maximumId = std::numeric_limits<std::uint64_t>::max();
int checks = 0, failures = 0;
bool check(bool value, const std::string& message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message.c_str()); }
    return value;
}
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
// Fixture records and expected streams are encoded independently of the note
// codec, resolver, preparation effects and production record constructors.
Record record(std::uint64_t id, std::int64_t start, std::int64_t duration,
              std::uint8_t key = 60, std::size_t stride = sizeof(NoteRecord)) {
    require(stride >= sizeof(NoteRecord), "fixture stride");
    NoteRecord value{};
    value.note_id = id; value.start_ticks = start; value.dur_ticks = duration;
    value.key = key; value.vel_on = 83; value.vel_off = 31; value.channel = 7;
    value.flags = 0x8007; value.probability = 7300; value.tuning_cents = 17.25f;
    Record result; result.bytes.resize(stride);
    std::memcpy(result.bytes.data(), &value, sizeof value);
    for (std::size_t i = sizeof value; i < stride; ++i)
        result.bytes[i] = static_cast<std::byte>((id + i * 29u) & 255u);
    return result;
}
NoteRecord prefix(const Record& value) {
    require(value.bytes.size() >= sizeof(NoteRecord), "fixture record bounds");
    NoteRecord result{}; std::memcpy(&result, value.bytes.data(), sizeof result); return result;
}
template<class Function> Record changed(Record value, Function function) {
    auto fields = prefix(value); function(fields);
    std::memcpy(value.bytes.data(), &fields, sizeof fields); return value;
}
Bytes stream(const std::vector<Record>& records, std::uint16_t stride = 40,
             bool sorted = true, std::uint16_t version = 1, const Bytes& trailer = {}) {
    require(records.size() <= std::numeric_limits<std::uint32_t>::max(), "fixture stream count");
    const StreamHeader header{static_cast<std::uint32_t>(FourCC::Notes), version, stride,
        static_cast<std::uint32_t>(records.size()), sorted ? StreamFlags::SortedByTime : 0u};
    Bytes result(sizeof header + records.size() * stride + trailer.size());
    std::memcpy(result.data(), &header, sizeof header);
    for (std::size_t i = 0; i < records.size(); ++i) {
        require(records[i].bytes.size() == stride, "fixture full record extent");
        std::memcpy(result.data() + sizeof header + i * stride, records[i].bytes.data(), stride);
    }
    if (!trailer.empty())
        std::memcpy(result.data() + sizeof header + records.size() * stride, trailer.data(), trailer.size());
    return result;
}
void order(std::vector<Record>& records) {
    std::stable_sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
        return prefix(a).start_ticks < prefix(b).start_ticks;
    });
}
Payload binary(const Bytes& bytes) {
    std::vector<std::uint8_t> values; values.reserve(bytes.size());
    for (auto value : bytes) values.push_back(std::to_integer<std::uint8_t>(value));
    return Payload::binary(std::move(values));
}
Bytes blob(const SQLite::Column& column) {
    if (column.getBytes() == 0) return {};
    const auto* first = static_cast<const std::byte*>(column.getBlob());
    return {first, first + column.getBytes()};
}
Payload tagged(int type, Payload value) { return {{"type", type}, {"value", std::move(value)}}; }
// Every SQL cell carries its actual storage class. REAL values use their exact
// double bits, so JSON's INTEGER(1)==REAL(1.0) comparison cannot hide drift.
std::vector<Payload> typedRows(SQLite::Database& db, const char* table, const char* orderColumn = "id") {
    // Both identifiers are trusted test literals, never packet or project data.
    SQLite::Statement query(db, std::string("SELECT * FROM ") + table + " ORDER BY " + orderColumn);
    std::vector<Payload> result;
    while (query.executeStep()) {
        Payload row = Payload::object();
        for (int i = 0; i < query.getColumnCount(); ++i) {
            const auto cell = query.getColumn(i); Payload value;
            switch (cell.getType()) {
                case SQLITE_INTEGER: value = cell.getInt64(); break;
                case SQLITE_FLOAT: value = std::bit_cast<std::uint64_t>(cell.getDouble()); break;
                case SQLITE_TEXT: value = cell.getString(); break;
                case SQLITE_BLOB: value = binary(blob(cell)); break;
                case SQLITE_NULL: value = nullptr; break;
                default: throw std::runtime_error("unknown SQLite storage class");
            }
            row[query.getColumnName(i)] = tagged(cell.getType(), std::move(value));
        }
        result.push_back(std::move(row));
    }
    return result;
}
struct State {
    std::vector<Payload> streams, expressions, clips, tracks;
    bool operator==(const State&) const = default;
};
struct JournalState {
    std::vector<Payload> operations, clocks, clients, branches;
    bool operator==(const JournalState&) const = default;
};
std::int64_t integer(const Payload& row, const char* name) {
    require(row.at(name).at("type") == SQLITE_INTEGER, "oracle expected SQL integer");
    return row.at(name).at("value").get<std::int64_t>();
}
void replaceStream(State& state, std::int64_t rowId, const Bytes& bytes) {
    const auto found = std::find_if(state.streams.begin(), state.streams.end(),
        [rowId](const Payload& row) { return integer(row, "id") == rowId; });
    require(found != state.streams.end(), "expected canonical row");
    (*found)["data"] = tagged(SQLITE_BLOB, binary(bytes));
}
void eraseExpressions(State& state, std::int64_t clip, const std::set<std::uint64_t>& removed) {
    std::erase_if(state.expressions, [&](const Payload& row) {
        return integer(row, "clip_id") == clip && removed.contains(std::bit_cast<std::uint64_t>(integer(row, "note_id")));
    });
}
struct Project {
    test::TempDirectory folder;
    std::unique_ptr<Store> store;
    explicit Project(const char* token) : folder("note_overlap_ops", token) {
        StoreError error{}; store = Store::createEmpty(folder.path() / "song.adi", error);
        require(bool(store), std::string("fixture create: ") + toString(error));
        store->db().exec("INSERT INTO tracks(id,kind) VALUES(1,'midi');"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES"
            "(1,1,'midi',0,1000000),(2,1,'midi',0,1000000),(3,1,'midi',0,1000000);"
            "ALTER TABLE event_streams ADD COLUMN future_text TEXT DEFAULT 'opaque stream';"
            "ALTER TABLE event_streams ADD COLUMN future_blob BLOB DEFAULT X'005AFF';"
            "ALTER TABLE note_expression ADD COLUMN future_any ANY;"
            "ALTER TABLE note_expression ADD COLUMN future_real REAL DEFAULT 1.25;"
            "ALTER TABLE note_expression ADD COLUMN future_blob BLOB DEFAULT X'';"
            "ALTER TABLE note_expression ADD COLUMN future_null TEXT");
    }
    void seed(std::int64_t clip, const Bytes& bytes, std::int64_t row = 101) {
        SQLite::Statement insert(store->db(), "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(?,?,'notes',?)");
        insert.bind(1, row); insert.bind(2, clip);
        insert.bind(3, bytes.data(), static_cast<int>(bytes.size())); insert.exec();
    }
    void expression(std::int64_t row, std::int64_t clip, std::uint64_t note, int dimension) {
        SQLite::Statement insert(store->db(), "INSERT INTO note_expression(id,clip_id,note_id,dimension,data,future_any) VALUES(?,?,?,?,X'41455850FE00',1)");
        insert.bind(1, row); insert.bind(2, clip); insert.bind(3, std::bit_cast<std::int64_t>(note));
        insert.bind(4, dimension); insert.exec();
    }
    State state() { return {typedRows(store->db(), "event_streams"), typedRows(store->db(), "note_expression"),
                           typedRows(store->db(), "clips"), typedRows(store->db(), "tracks")}; }
    JournalState journal() { return {typedRows(store->db(), "ops", "seq"), typedRows(store->db(), "op_clocks", "seq"),
        typedRows(store->db(), "op_clients", "client_id"), typedRows(store->db(), "op_branches", "id")}; }
    std::optional<Bytes> noteBlob(std::int64_t clip = 1) {
        SQLite::Statement query(store->db(), "SELECT data FROM event_streams WHERE clip_id=? AND stream_kind='notes' AND channel IS NULL AND controller IS NULL");
        query.bind(1, clip); if (!query.executeStep()) return {};
        return blob(query.getColumn(0));
    }
    void reopen() {
        const auto path = store->path(); store.reset(); StoreError error{};
        store = Store::open(path, error); require(bool(store), std::string("fixture reopen: ") + toString(error));
    }
};
struct Writes {
    sqlite3* db;
    std::int64_t target;
    int notes = 0, deletes = 0, material = 0, all = 0;
    Writes(Store& store, std::int64_t row) : db(store.db().getHandle()), target(row) {
        sqlite3_update_hook(db, [](void* context, int operation, const char*, const char* table, sqlite3_int64 id) {
            auto& self = *static_cast<Writes*>(context); ++self.all;
            if (std::strcmp(table, "event_streams") == 0 || std::strcmp(table, "note_expression") == 0 ||
                std::strcmp(table, "clips") == 0 || std::strcmp(table, "tracks") == 0) ++self.material;
            if (std::strcmp(table, "event_streams") == 0 && id == self.target) {
                if (operation == SQLITE_INSERT || operation == SQLITE_UPDATE) ++self.notes;
                if (operation == SQLITE_DELETE) ++self.deletes;
            }
        }, this);
    }
    ~Writes() { sqlite3_update_hook(db, nullptr, nullptr); }
    void reset() { notes = 0; deletes = 0; material = 0; all = 0; }
};
OpRequest raw(const notes::PreparedOverlap& prepared) {
    require(prepared.ok() && prepared.requests().size() == 1, "one overlap request: " + prepared.error());
    return prepared.requests().front();
}
OpRequest request(const char* name, Payload payload) {
    OpRequest result; result.opType = name; result.payload = std::move(payload); return result;
}
void rejected(Project& project, const OpRequest& edit, const std::string& label, bool zeroMaterialWrites = true) {
    const auto before = project.state(); const auto journal = project.journal();
    const auto digest = digestProject(*project.store).text;
    const auto count = OpJournal(*project.store).count(); const auto head = OpJournal(*project.store).headSeq();
    Writes writes(*project.store, 101); const auto result = OpJournal(*project.store).commit(edit);
    const bool sameMaterial = project.state() == before, sameJournal = project.journal() == journal;
    const bool sameDigest = digestProject(*project.store).text == digest;
    const bool sameCount = OpJournal(*project.store).count() == count, sameHead = OpJournal(*project.store).headSeq() == head;
    // The hook sees rolled-back op_clients/fork bookkeeping before inverse
    // validation. Pre-apply refusals must issue no musical-table writes; all
    // journal/control rows must still roll back to their exact typed contents.
    if (!check(!result.ok && sameMaterial && sameJournal && sameDigest && sameCount && sameHead &&
        (!zeroMaterialWrites || writes.material == 0), label)) {
        std::printf("  refusal: ok=%d error=%s writes(material=%d,ANOT=%d,deletes=%d,all=%d) unchanged(material=%d,journal=%d,digest=%d,count=%d,head=%d)\n",
            result.ok ? 1 : 0, result.error.c_str(), writes.material, writes.notes, writes.deletes, writes.all,
            sameMaterial ? 1 : 0, sameJournal ? 1 : 0, sameDigest ? 1 : 0, sameCount ? 1 : 0, sameHead ? 1 : 0);
    }
}
void replaceLog(Project& project, bool inverse, const Payload& payload) {
    const auto bytes = encodePayload(payload);
    SQLite::Statement update(project.store->db(), inverse ?
        "UPDATE ops SET inverse=? WHERE seq=(SELECT MAX(seq) FROM ops)" :
        "UPDATE ops SET payload=? WHERE seq=(SELECT MAX(seq) FROM ops)");
    update.bind(1, bytes.data(), static_cast<int>(bytes.size()));
    require(update.exec() == 1, "replace recorded test payload");
}
notes::PreparedOverlap mixed(Project& project, bool expressions = true) {
    const std::vector<Record> baseline{record(wideId, 0, 10, 60, 48), record(maximumId, 20, 10, 60, 48),
        record(31, 40, 10, 60, 48), record(42, 60, 10, 61, 48)};
    project.seed(1, stream(baseline, 48, true, 7, {std::byte{0x9b}, std::byte{0x00}}));
    if (expressions) {
        project.expression(-11, 1, wideId, 903); project.expression(19, 1, wideId, -777);
        project.expression(-7, 1, maximumId, 88); project.expression(45, 1, 31, 903);
        project.expression(46, 1, 42, 903);
        project.store->db().exec("UPDATE note_expression SET future_any=CAST(1 AS REAL) WHERE id=19;"
            "UPDATE note_expression SET future_any='future text',data=X'',future_blob=X'00FF' WHERE id=-7");
    }
    notes::Diff intent;
    intent.changed.push_back({baseline[2], changed(baseline[2], [](NoteRecord& value) {
        value.start_ticks = 8; value.dur_ticks = 15; value.channel = 1; value.tuning_cents = -51.0f;
    })});
    intent.added.push_back(record(100, 80, 5, 62, 48));
    return notes::prepareOverlapDiff(notes::captureNotes(*project.store, 1), intent);
}

void tenThousandOneWrite() {
    Project project("ten_thousand");
    std::vector<Record> baseline, expected; notes::Diff intent;
    baseline.reserve(10000); expected.reserve(15000); intent.added.reserve(10000);
    for (std::uint64_t i = 0; i < 10000; ++i) {
        const auto start = static_cast<std::int64_t>(i * 8);
        baseline.push_back(record(i + 1, start, 4));
        if (i % 2 == 0) expected.push_back(record(i + 1, start, 2));
        const auto incoming = record(100001 + i, start + (i % 2 == 0 ? 2 : -1), i % 2 == 0 ? 1 : 2);
        intent.added.push_back(incoming); expected.push_back(incoming);
    }
    order(expected); project.seed(1, stream(baseline));
    project.expression(-1, 1, 1, 900); project.expression(2, 1, 2, 901); project.expression(3, 1, 10000, 902);
    const auto before = project.state(); auto after = before;
    replaceStream(after, 101, stream(expected)); eraseExpressions(after, 1, {2, 10000});
    const auto prepared = notes::prepareOverlapDiff(notes::captureNotes(*project.store, 1), intent);
    require(prepared.ok(), "large overlap prepare: " + prepared.error());
    check(prepared.collisions().size() == 10000 && prepared.sideEffects().size() == 10000 &&
        prepared.effects().removed.size() == 5000 && prepared.effects().changed.size() == 5000,
        "10,000 real collisions resolve to 5,000 removals and 5,000 shortenings");
    Writes writes(*project.store, 101);
    const auto result = notes::commitOverlapDiff(*project.store, prepared);
    if (!check(result.ok && result.seqs.size() == 1 && OpJournal(*project.store).count() == 1 &&
        project.state() == after && writes.notes == 1 && writes.deletes == 0,
        "10,000-note overlap commits one transaction/op and exactly one actual ANOT update")) return;
    check(project.store->db().execAndGet("SELECT count(DISTINCT txn_id) FROM ops").getInt64() == 1,
        "large overlap is one journal transaction");
    writes.reset(); const auto undo = History(*project.store).undo();
    check(undo.ok && project.state() == before && writes.notes == 1 && writes.deletes == 0,
        "large overlap Undo writes ANOT once and restores exact expression rows");
    writes.reset(); const auto redo = History(*project.store).redo();
    check(redo.ok && project.state() == after && writes.notes == 1 && writes.deletes == 0,
        "large overlap Redo writes ANOT once and reproduces independent expected bytes");
}

void typedExpressionsAndHistory() {
    Project project("typed_expression"); const auto prepared = mixed(project);
    const auto before = project.state(); auto after = before;
    std::vector<Record> expected{record(wideId, 0, 8, 60, 48),
        changed(record(31, 40, 10, 60, 48), [](NoteRecord& value) {
            value.start_ticks = 8; value.dur_ticks = 15; value.channel = 1; value.tuning_cents = -51.0f;
        }), record(42, 60, 10, 61, 48), record(100, 80, 5, 62, 48)};
    replaceStream(after, 101, stream(expected, 48, true, 7, {std::byte{0x9b}, std::byte{0x00}}));
    eraseExpressions(after, 1, {maximumId});
    if (!check(notes::commitOverlapDiff(*project.store, prepared).ok && project.state() == after,
        "full-width identities preserve future tails/header/trailer and retained opaque typed expression sets")) return;
    const auto digest = digestProject(*project.store).text;
    check(History(*project.store).undo().ok && project.state() == before,
        "Undo restores signed row IDs, unknown dimensions, SQL INTEGER/REAL/NULL/text and empty BLOBs");
    project.reopen();
    check(History(*project.store).redo().ok && project.state() == after && digestProject(*project.store).text == digest,
        "reopened captured replay preserves exact typed rows and complete note bytes");
}

void staleExpressionSets() {
    const std::array<const char*, 6> mutations{
        "INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(900,1,-9223372036854775791,777,X'')",
        "DELETE FROM note_expression WHERE id=-11",
        "UPDATE note_expression SET data=X'ABCDEF' WHERE id=-11",
        "UPDATE note_expression SET id=-12 WHERE id=-11",
        "UPDATE note_expression SET future_any=CAST(1 AS REAL) WHERE id=-11",
        "DELETE FROM note_expression WHERE id=-7"};
    for (std::size_t i = 0; i < mutations.size(); ++i) {
        Project project("stale_set"); const auto prepared = mixed(project, i != 0);
        const auto edit = raw(prepared); const auto head = OpJournal(*project.store).headSeq();
        const auto bytes = project.noteBlob(); project.store->db().exec(mutations[i]);
        check(project.noteBlob() == bytes && OpJournal(*project.store).headSeq() == head,
            "stale-expression fixture leaves ANOT hash and journal head untouched");
        if (i == 4) check(project.store->db().execAndGet("SELECT typeof(future_any) FROM note_expression WHERE id=-11").getString() == "real",
            "equal numeric value really changes SQL INTEGER to REAL");
        rejected(project, edit, "complete expression guard refuses direct writer mutation " + std::to_string(i));
    }
    Project unrelated("unrelated_expression"); const auto prepared = mixed(unrelated);
    unrelated.store->db().exec("UPDATE note_expression SET future_any=X'FD00',data=X'' WHERE id=46");
    const auto unrelatedRows = typedRows(unrelated.store->db(), "note_expression");
    check(notes::commitOverlapDiff(*unrelated.store, prepared).ok,
        "expression changes for an unaffected note do not stale a valid overlap edit");
    const auto currentRows = typedRows(unrelated.store->db(), "note_expression");
    const auto find46 = [](const std::vector<Payload>& rows) {
        return std::find_if(rows.begin(), rows.end(), [](const Payload& row) { return integer(row, "id") == 46; });
    };
    const auto previous = find46(unrelatedRows), current = find46(currentRows);
    check(previous != unrelatedRows.end() && current != currentRows.end() && *previous == *current,
        "unrelated expression rows remain byte and SQL-type exact");
    Project orphan("added_orphan"); const auto orphanPlan = mixed(orphan);
    orphan.expression(999, 1, 100, 66);
    rejected(orphan, raw(orphanPlan), "added note retains MI-01 orphan-expression refusal");
}

void strictPackets() {
    Project project("strict_packets"); const auto prepared = mixed(project); const auto valid = raw(prepared);
    using Mutation = std::function<void(Payload&)>;
    const std::vector<std::pair<std::string, Mutation>> mutations{
        {"stale full-stream hash", [](Payload& p) { p["from"] = std::string(64, '0'); }},
        {"wrong canonical row identity", [](Payload& p) { p["stream_id"] = 102; }},
        {"wrong clip owner", [](Payload& p) { p["clip"] = 2; }},
        {"unknown version", [](Payload& p) { p["overlap"]["version"] = 2; }},
        {"float version", [](Payload& p) { p["overlap"]["version"] = 1.0; }},
        {"missing version", [](Payload& p) { p["overlap"].erase("version"); }},
        {"nonobject envelope", [](Payload& p) { p["overlap"] = Payload::array(); }},
        {"unknown envelope field", [](Payload& p) { p["overlap"]["future"] = true; }},
        {"unknown inner intent field", [](Payload& p) { p["overlap"]["intent"]["future"] = 1; }},
        {"missing inner array", [](Payload& p) { p["overlap"]["intent"].erase("removed"); }},
        {"wrong inner array type", [](Payload& p) { p["overlap"]["intent"]["added"] = nullptr; }},
        {"missing effective removal", [](Payload& p) { p["removed"] = Payload::array(); }},
        {"missing effective shortening", [](Payload& p) { p["changed"].erase(p["changed"].begin()); }},
        {"duplicate effective change", [](Payload& p) { p["changed"].push_back(p["changed"].front()); }},
        {"unknown record field", [](Payload& p) { p["overlap"]["intent"]["added"].front()["future"] = 0; }},
        {"unknown change field", [](Payload& p) { p["overlap"]["intent"]["changed"].front()["future"] = 0; }},
        {"missing retained guard", [](Payload& p) { p["overlap"]["expression_guards"].erase(p["overlap"]["expression_guards"].begin()); }},
        {"duplicate retained guard", [](Payload& p) { p["overlap"]["expression_guards"].push_back(p["overlap"]["expression_guards"].front()); }},
        {"extra retained guard", [](Payload& p) { p["overlap"]["expression_guards"].push_back({{"note_id", 42}, {"rows", Payload::array()}}); }},
        {"unknown retained guard field", [](Payload& p) { p["overlap"]["expression_guards"].front()["future"] = 1; }},
        {"wrong retained guard ID type", [](Payload& p) { p["overlap"]["expression_guards"].front()["note_id"] = 31.0; }},
        {"wrong retained guard rows type", [](Payload& p) { p["overlap"]["expression_guards"].front()["rows"] = nullptr; }},
        {"missing guard list", [](Payload& p) { p["overlap"].erase("expression_guards"); }}
    };
    require(valid.payload.at("changed").size() == 2 && valid.payload.at("removed").size() == 1 &&
        valid.payload.at("overlap").at("expression_guards").size() == 2,
        "strict mutation fixture has complete effective changes and guards");
    for (const auto& [name, mutate] : mutations) {
        auto forged = valid; mutate(forged.payload); rejected(project, forged, "closed versioned packet rejects " + name);
    }
    auto noOp = valid;
    const auto untouched = record(42, 60, 10, 61, 48);
    noOp.payload["changed"].push_back({{"old", {{"bytes", binary(untouched.bytes)}}},
                                     {"new", {{"bytes", binary(untouched.bytes)}}}});
    rejected(project, noOp, "extra outer byte-identical change cannot disappear during canonical comparison");
    auto contradictory = valid;
    const auto movedAway = changed(record(31, 40, 10, 60, 48), [](NoteRecord& value) { value.start_ticks = 100; });
    contradictory.payload["overlap"]["intent"]["changed"].front()["new"]["bytes"] = binary(movedAway.bytes);
    rejected(project, contradictory, "valid inner intent with contradictory recomputed outer effects refuses");

    const auto victim = record(maximumId, 20, 10, 60, 48);
    const auto velocity = changed(victim, [](NoteRecord& value) { value.vel_on = 29; });
    for (int variation = 0; variation < 4; ++variation) {
        auto forged = valid; auto old = victim, next = velocity;
        if (variation == 0) old.bytes.back() ^= std::byte{1};
        if (variation == 1) next.bytes.back() ^= std::byte{1};
        if (variation == 2) next.bytes.pop_back();
        if (variation == 3) next = changed(next, [](NoteRecord& value) { value.dur_ticks = 0; });
        forged.payload["overlap"]["intent"]["changed"].push_back({
            {"old", {{"bytes", binary(old.bytes)}}}, {"new", {{"bytes", binary(next.bytes)}}}});
        rejected(project, forged, "superseded victim edit still validates old bytes/tail/stride/prefix " + std::to_string(variation));
    }
    auto equivalent = valid;
    equivalent.payload["overlap"]["intent"]["changed"].push_back({
        {"old", {{"bytes", binary(victim.bytes)}}}, {"new", {{"bytes", binary(velocity.bytes)}}}});
    if (!check(OpJournal(*project.store).commit(equivalent).ok,
        "valid superseded non-geometric intent with the same exact material delta is admissible")) return;
    const auto expected = project.state();
    require(History(*project.store).undo().ok, "undo equivalent intent fixture");
    auto permuted = valid;
    for (const char* field : {"added", "removed", "changed"})
        std::reverse(permuted.payload[field].begin(), permuted.payload[field].end());
    auto& guards = permuted.payload["overlap"]["expression_guards"];
    std::reverse(guards.begin(), guards.end());
    for (auto& guard : guards) std::reverse(guard["rows"].begin(), guard["rows"].end());
    check(OpJournal(*project.store).commit(permuted).ok && project.state() == expected,
        "outer arrays and complete guard rows accept harmless permutations with identical results");
    require(History(*project.store).undo().ok, "undo permutation fixture");
    auto literal = valid; literal.payload.erase("overlap");
    check(OpJournal(*project.store).commit(literal).ok && project.state() == expected,
        "removing the entire optional envelope remains a legal MI-01 literal diff");
}

void immutableCommitAndMetadata() {
    Project project("metadata"); auto prepared = mixed(project);
    const auto original = raw(prepared); auto inspected = original;
    inspected.payload["overlap"]["version"] = 999;
    const auto copy = prepared;
    check(raw(copy).payload == original.payload && raw(prepared).payload == original.payload,
        "mutating an inspected request copy cannot alter the immutable original or its copied preparation");
    const auto empty = notes::prepareOverlapDiff(prepared.source(), {});
    require(empty.ok() && empty.requests().empty(), "successful overlap no-op");
    {
        Writes writes(*project.store, 101);
        check(!empty.expectedHead() && notes::commitOverlapDiff(*project.store, empty).ok && writes.all == 0 &&
            OpJournal(*project.store).count() == 0, "captured null-head no-op succeeds with no write or empty history transaction");
    }
    Project other("other_source"); const auto otherPlan = mixed(other);
    static_cast<void>(otherPlan);
    const auto otherBefore = other.state();
    check(!notes::commitOverlapDiff(*other.store, prepared).ok && !notes::commitOverlapDiff(*other.store, empty).ok &&
        other.state() == otherBefore && OpJournal(*other.store).count() == 0,
        "both mutation and no-op refuse another source filename with identical material");
    notes::OverlapCommitMetadata metadata;
    metadata.actor = Actor::Agent; metadata.actorDetail = "independent MI-02 owner gate";
    metadata.label = "Resolve note overlap"; metadata.selBefore = Payload{{"notes", Payload::array({wideId, maximumId})}};
    if (!check(notes::commitOverlapDiff(*project.store, copy, metadata).ok,
        "immutable committer accepts only submission metadata overrides")) return;
    SQLite::Statement logged(project.store->db(), "SELECT actor,actor_detail,label,op_type,target_kind,target_id,payload FROM ops");
    require(logged.executeStep(), "logged metadata row");
    std::string error; const auto decoded = decodeFromLog(blob(logged.getColumn(6)), error);
    check(logged.getColumn(0).getString() == "agent" && logged.getColumn(1).getString() == metadata.actorDetail &&
        logged.getColumn(2).getString() == *metadata.label && logged.getColumn(3).getString() == original.opType &&
        logged.getColumn(4).getString() == original.targetKind && original.targetId.has_value() &&
        logged.getColumn(5).getInt64() == *original.targetId && decoded && *decoded == original.payload,
        "journal metadata changes leave exact payload/type/target bound to prepared preview");
    logged.reset();
    const auto after = project.state(); const auto staleEmpty = notes::commitOverlapDiff(*project.store, empty);
    const auto staleFull = notes::commitOverlapDiff(*project.store, prepared);
    check(!staleEmpty.ok && staleEmpty.stale && !staleFull.ok && staleFull.stale && project.state() == after &&
        OpJournal(*project.store).count() == 1, "both captured null-head guards reject an intervening commit");
    const auto nonnull = notes::prepareOverlapDiff(notes::captureNotes(*project.store, 1), {});
    require(nonnull.ok() && nonnull.expectedHead().has_value(), "non-null head capture");
    require(OpJournal(*project.store).commit(request("track.rename", {{"id", 1}, {"name", "new name"}})).ok,
        "intervening non-null head edit");
    const auto staleNonnull = notes::commitOverlapDiff(*project.store, nonnull);
    check(!staleNonnull.ok && staleNonnull.stale, "captured non-null-head no-op refuses an intervening operation");
    require(History(*project.store).undo().ok, "undo intervening track metadata");
    const auto undone = History(*project.store).undo();
    check(undone.ok && undone.selBefore == metadata.selBefore,
        "overlap metadata selection is advisory first-request history data restored on Undo");
    notes::PreparedOverlap notPrepared;
    const auto refusedDefault = notes::commitOverlapDiff(*project.store, notPrepared);
    check(!refusedDefault.ok && !refusedDefault.error.empty(),
        "default preparation cannot authorize an empty-source no-op");
    auto movable = prepared; const auto moved = std::move(movable);
    const auto refusedMoved = notes::commitOverlapDiff(*project.store, movable);
    check(moved.ok() && !refusedMoved.ok && !refusedMoved.error.empty(),
        "moved-from preparation refuses with a concrete diagnostic");
}

void nonemptyHeadGuard() {
    Project project("nonempty_head_guard"); const auto prepared = mixed(project);
    require(prepared.ok() && prepared.requests().size() == 1 && !prepared.expectedHead(),
        "nonempty edit captures an explicit null-head guard");
    const auto captured = project.state();
    require(OpJournal(*project.store).commit(request("track.rename", {{"id", 1}, {"name", "unrelated head advance"}})).ok,
        "unrelated rename advances history without changing note material");
    const auto before = project.state(); const auto journal = project.journal();
    const auto head = OpJournal(*project.store).headSeq();
    const auto count = OpJournal(*project.store).count();
    require(head.has_value() && before.streams == captured.streams && before.expressions == captured.expressions,
        "stale-head fixture preserves exact ANOT and typed expression rows");
    Writes writes(*project.store, 101);
    const auto refused = notes::commitOverlapDiff(*project.store, prepared);
    check(!refused.ok && refused.stale && writes.notes == 0 && writes.material == 0 && writes.all == 0 &&
        project.state() == before && project.journal() == journal &&
        OpJournal(*project.store).headSeq() == head && OpJournal(*project.store).count() == count,
        "nonempty null-head preparation refuses unrelated head advance before any material or journal write");
}

void literalCreation() {
    Project project("literal_creation");
    project.store->db().exec("DROP TABLE event_streams;"
        "CREATE TABLE event_streams(id INTEGER PRIMARY KEY,clip_id INTEGER NOT NULL REFERENCES clips(id),"
        "stream_kind TEXT NOT NULL,channel INTEGER,controller INTEGER,data BLOB NOT NULL,"
        "future_nonce BLOB NOT NULL DEFAULT(randomblob(8)),future_real REAL DEFAULT 1.0) STRICT;"
        "CREATE UNIQUE INDEX idx_stream_clip ON event_streams(clip_id,stream_kind,IFNULL(channel,-1),IFNULL(controller,-1))");
    const auto absent = notes::captureNotes(*project.store, 1); notes::Diff intent;
    intent.added.push_back(record(maximumId, -9, 4));
    check(notes::prepareOverlapDiff(absent, {}).ok() && !notes::prepareOverlapDiff(absent, intent).ok(),
        "absent no-op needs no identity but nonempty creation needs a captured literal stream ID");
    const auto source = notes::captureNotes(*project.store, 1, -7);
    const auto prepared = notes::prepareOverlapDiff(source, intent), again = notes::prepareOverlapDiff(source, intent);
    const auto edit = raw(prepared);
    check(edit.payload == raw(again).payload && edit.payload.at("stream_id") == -7 && edit.payload.at("from") == "absent",
        "pure creation reuses captured signed row ID and random future defaults");
    const auto before = project.state();
    if (!check(notes::commitOverlapDiff(*project.store, prepared).ok,
        "overlap first insertion accepts its literal creation row")) return;
    const auto after = project.state();
    {
        SQLite::Statement nonce(project.store->db(), "SELECT future_nonce,typeof(future_real) FROM event_streams WHERE id=-7");
        require(nonce.executeStep(), "created literal default row");
        check(binary(blob(nonce.getColumn(0))) == edit.payload.at("create").at("future_nonce") &&
            nonce.getColumn(1).getString() == "real", "creation stores the captured random bytes and SQL REAL class");
    }
    check(History(*project.store).undo().ok && project.state() == before && !project.noteBlob(),
        "captured creation Undo restores exact canonical absence");
    project.seed(2, stream({record(9, 1, 2)}), 9001);
    const auto withForeign = project.state(); auto expected = after;
    const auto foreign = std::find_if(withForeign.streams.begin(), withForeign.streams.end(),
        [](const Payload& row) { return integer(row, "id") == 9001; });
    require(foreign != withForeign.streams.end(), "unrelated ID pressure row");
    expected.streams.push_back(*foreign);
    project.reopen();
    check(History(*project.store).redo().ok && project.state() == expected,
        "creation Redo keeps original ID/defaults despite higher unrelated row-ID pressure and reopen");
    check(History(*project.store).undo().ok && project.state() == withForeign,
        "stable captured creation remains undoable without touching unrelated streams");
    project.seed(3, stream({record(10, 2, 3)}), -7);
    rejected(project, edit, "literal creation ID occupied by a different clip refuses without overwriting its owner");
}

struct Model {
    std::vector<Record> records;
    std::uint16_t stride = 40;
    bool sorted = true;
    std::uint16_t version = 1;
    Bytes trailer;
    Bytes bytes() const { return stream(records, stride, sorted, version, trailer); }
};
// Independent bounded integer-cell oracle. It walks the unoccupied prefix of
// each original victim; it does not reproduce the resolver's interval branches
// or consume its effects/diagnostics. Corpus coordinates are deliberately small.
Model applyCells(const Model& before, const notes::Diff& intent) {
    std::set<std::uint64_t> removed, drivers;
    std::map<std::uint64_t, Record> replacements;
    std::map<int, std::set<std::int64_t>> occupied;
    const auto occupy = [&](const Record& item) {
        const auto value = prefix(item);
        require(value.start_ticks >= -32 && value.start_ticks + value.dur_ticks <= 160,
            "bounded independent occupancy fixture");
        for (auto tick = value.start_ticks; tick < value.start_ticks + value.dur_ticks; ++tick)
            occupied[static_cast<int>(value.key)].insert(tick);
    };
    for (const auto& item : intent.removed) removed.insert(prefix(item).note_id);
    for (const auto& item : intent.changed) {
        const auto old = prefix(item.oldRecord), next = prefix(item.newRecord);
        replacements.emplace(old.note_id, item.newRecord);
        if (old.start_ticks != next.start_ticks || old.dur_ticks != next.dur_ticks || old.key != next.key) {
            drivers.insert(old.note_id); occupy(item.newRecord);
        }
    }
    for (const auto& item : intent.added) { drivers.insert(prefix(item).note_id); occupy(item); }
    Model result = before; result.records.clear();
    for (const auto& item : before.records) {
        const auto old = prefix(item);
        if (removed.contains(old.note_id)) continue;
        const auto replacement = replacements.find(old.note_id);
        auto final = replacement == replacements.end() ? item : replacement->second;
        if (!drivers.contains(old.note_id)) {
            std::int64_t prefixLength = 0;
            const auto cells = occupied.find(static_cast<int>(old.key));
            while (prefixLength < old.dur_ticks && (cells == occupied.end() ||
                   !cells->second.contains(old.start_ticks + prefixLength))) ++prefixLength;
            if (prefixLength == 0) continue;
            if (prefixLength != old.dur_ticks)
                final = changed(std::move(final), [prefixLength](NoteRecord& value) { value.dur_ticks = prefixLength; });
        }
        result.records.push_back(std::move(final));
    }
    auto additions = intent.added;
    std::sort(additions.begin(), additions.end(), [](const Record& a, const Record& b) {
        return prefix(a).note_id < prefix(b).note_id;
    });
    result.records.insert(result.records.end(), additions.begin(), additions.end());
    if (result.sorted) order(result.records);
    return result;
}
struct Random {
    std::uint64_t state = 0x390a14d2739b25e1ULL;
    std::uint64_t next() {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state;
    }
};
void independentHistoryCorpus() {
    Project project("independent_history"); std::array<Model, 3> models;
    for (std::size_t clip = 0; clip < models.size(); ++clip) {
        auto& model = models[clip]; model.stride = clip == 0 ? 40 : 48;
        model.sorted = clip != 2; model.version = clip == 0 ? 1 : 9;
        if (clip != 0) model.trailer = {std::byte{0xfa}, std::byte{0x00}, std::byte{0x9d}};
        for (std::uint64_t i = 0; i < 8; ++i) {
            const auto id = (clip == 1 ? wideId : 100u * static_cast<std::uint64_t>(clip + 1)) + i;
            model.records.push_back(record(id, static_cast<std::int64_t>(i * 9), 20,
                static_cast<std::uint8_t>(60 + i % 2), model.stride));
            project.expression(static_cast<std::int64_t>(clip * 100 + static_cast<std::size_t>(i) + 1),
                static_cast<std::int64_t>(clip + 1), id, 700 + static_cast<int>(i));
        }
        if (!model.sorted) std::reverse(model.records.begin(), model.records.end());
        project.seed(static_cast<std::int64_t>(clip + 1), model.bytes(), static_cast<std::int64_t>(101 + clip));
    }
    project.store->db().exec("UPDATE note_expression SET future_any=CAST(1 AS REAL) WHERE id%2=0;"
        "UPDATE note_expression SET future_blob=X'00FF99' WHERE id%3=0");
    std::vector<State> states{project.state()};
    std::vector<std::string> digests{digestProject(*project.store).text};
    Random random; std::size_t successes = 0;
    for (std::size_t attempt = 0; attempt < 180 && successes < 48; ++attempt) {
        const auto clipIndex = successes % models.size(); const auto clip = static_cast<std::int64_t>(clipIndex + 1);
        auto& model = models[clipIndex]; notes::Diff intent;
        const auto mode = attempt % 5;
        if (mode == 0 || model.records.empty()) {
            intent.added.push_back(record(100000 + static_cast<std::uint64_t>(attempt),
                static_cast<std::int64_t>(random.next() % 97) - 8,
                static_cast<std::int64_t>(random.next() % 18) + 1,
                static_cast<std::uint8_t>(60 + random.next() % 3), model.stride));
        } else {
            const auto index = static_cast<std::size_t>(random.next() % model.records.size());
            const auto old = model.records[index];
            if (mode == 4) intent.removed.push_back(old);
            else {
                const auto next = changed(old, [&](NoteRecord& value) {
                    if (mode == 1) {
                        value.start_ticks = static_cast<std::int64_t>(random.next() % 97) - 8;
                        value.key = static_cast<std::uint8_t>(60 + random.next() % 3);
                    } else if (mode == 2) value.dur_ticks = static_cast<std::int64_t>(random.next() % 18) + 1;
                    else value.vel_on = value.vel_on == 83 ? 39 : 83;
                });
                intent.changed.push_back({old, next});
            }
        }
        auto expectedModel = applyCells(model, intent);
        if (expectedModel.bytes() == model.bytes()) continue;
        auto expected = states.back(); replaceStream(expected, static_cast<std::int64_t>(101 + clipIndex), expectedModel.bytes());
        std::set<std::uint64_t> gone, retained;
        for (const auto& item : model.records) gone.insert(prefix(item).note_id);
        for (const auto& item : expectedModel.records) retained.insert(prefix(item).note_id);
        for (const auto id : retained) gone.erase(id);
        eraseExpressions(expected, clip, gone);
        const auto prepared = notes::prepareOverlapDiff(notes::captureNotes(*project.store, clip), intent);
        if (!check(prepared.ok() && prepared.requests().size() == 1,
            "independent corpus actual nonempty preparation " + std::to_string(successes))) return;
        const auto committed = notes::commitOverlapDiff(*project.store, prepared);
        if (!check(committed.ok && project.state() == expected,
            "independent occupancy model agrees with all typed material after successful edit " + std::to_string(successes))) return;
        model = std::move(expectedModel); states.push_back(std::move(expected));
        digests.push_back(digestProject(*project.store).text); ++successes;
    }
    require(successes == 48 && states.size() == 49 && digests.size() == states.size(),
        "corpus must execute 48 actual nonempty mutations, not refusals/no-ops");
    check(OpJournal(*project.store).count() == 48, "independent three-clip corpus records exactly 48 successful edits");
    for (std::size_t count = successes; count > 0; --count) {
        if (!check(History(*project.store).undo().ok && project.state() == states[count - 1] &&
            digestProject(*project.store).text == digests[count - 1],
            "Undo-all exact typed state and digest at step " + std::to_string(count))) return;
    }
    check(!History(*project.store).head() && project.state() == states.front(),
        "Undo-all restores historical overlapping baseline, future sorted and unsorted streams exactly");
    project.reopen();
    for (std::size_t count = 1; count < states.size(); ++count) {
        if (!check(History(*project.store).redo().ok && project.state() == states[count] &&
            digestProject(*project.store).text == digests[count],
            "reopened Redo exact typed state and digest at step " + std::to_string(count))) return;
    }
    const auto oldTip = History(*project.store).head();
    require(History(*project.store).undo().ok, "fork rewind");
    const auto snapshot = notes::captureNotes(*project.store, 1);
    require(snapshot.ok, "fork source capture");
    notes::Diff forkIntent; forkIntent.added.push_back(record(500000, 140, 1, 127));
    const auto fork = notes::prepareOverlapDiff(snapshot, forkIntent);
    require(notes::commitOverlapDiff(*project.store, fork).ok, "commit real overlap-history fork");
    const auto forkState = project.state(); const auto forkDigest = digestProject(*project.store).text;
    const auto forkBranch = OpJournal(*project.store).currentBranchId();
    const auto branches = History(*project.store).branches();
    const auto oldBranch = std::find_if(branches.begin(), branches.end(), [&](const History::Branch& branch) {
        return !branch.isCurrent && branch.headSeq == oldTip;
    });
    require(oldBranch != branches.end(), "fork retains former tip as reachable branch");
    check(History(*project.store).switchToBranch(oldBranch->id).ok && project.state() == states.back() &&
        digestProject(*project.store).text == digests.back(), "branch switch replays exact original versioned tip");
    check(History(*project.store).switchToBranch(forkBranch).ok && project.state() == forkState &&
        digestProject(*project.store).text == forkDigest, "branch switch back restores exact independent fork state");
}

void literalAndHistoricalReplay() {
    for (int mode = 0; mode < 3; ++mode) {
        Project project("literal_history");
        const auto a = record(wideId, 0, 20, 60, 48), b = record(17, 3, 20, 60, 48);
        project.seed(1, stream({a, b}, 48, true, 4, {std::byte{0x67}}));
        project.expression(-9, 1, wideId, 999); const auto before = project.state();
        auto expected = before; OpRequest edit;
        if (mode == 0) {
            const auto added = record(maximumId, 2, 8, 60, 48); notes::Diff literal;
            literal.added.push_back(added);
            const auto prepared = notes::prepareDiff(notes::captureNotes(*project.store, 1), literal);
            require(prepared.ok && prepared.requests.size() == 1, "literal MI-01 overlap preparation");
            edit = prepared.requests.front();
            require(!edit.payload.contains("overlap"), "historical literal request has no opt-in envelope");
            replaceStream(expected, 101, stream({a, added, b}, 48, true, 4, {std::byte{0x67}}));
        } else {
            edit = request("note.move", {{"clip", 1}, {"note", wideId}, {"start", 5}, {"key", 60}});
            const auto moved = changed(a, [](NoteRecord& value) { value.start_ticks = 5; });
            replaceStream(expected, 101, stream({b, moved}, 48, true, 4, {std::byte{0x67}}));
        }
        if (!check(OpJournal(*project.store).commit(edit).ok && project.state() == expected,
            "literal MI-01 and scalar edits retain overlapping material without implicit resolution " + std::to_string(mode))) continue;
        if (mode == 1) replaceLog(project, true, {{"clip", 1}, {"note", wideId}, {"start", 0}, {"key", 60}});
        check(History(*project.store).undo().ok && project.state() == before,
            "literal historical scalar/captured inverse restores all historical overlaps " + std::to_string(mode));
        project.reopen();
        check(History(*project.store).redo().ok && project.state() == expected,
            "reopened literal/scalar/captured forward replay does not opt into overlap normalization " + std::to_string(mode));
    }
}

void contradictoryRecordedForward() {
    Project project("contradictory_replay"); const auto prepared = mixed(project); const auto edit = raw(prepared);
    const auto before = project.state();
    require(notes::commitOverlapDiff(*project.store, prepared).ok, "recorded overlap fixture commit");
    const auto after = project.state(); require(History(*project.store).undo().ok, "recorded overlap fixture undo");
    auto contradictory = edit.payload;
    const auto moved = changed(record(31, 40, 10, 60, 48), [](NoteRecord& value) { value.start_ticks = 100; });
    contradictory["overlap"]["intent"]["changed"].front()["new"]["bytes"] = binary(moved.bytes);
    replaceLog(project, false, contradictory);
    const auto head = History(*project.store).head(); const auto digest = digestProject(*project.store).text;
    const auto refused = History(*project.store).redo();
    check(!refused.ok && project.state() == before && History(*project.store).head() == head &&
        digestProject(*project.store).text == digest && OpJournal(*project.store).count() == 1,
        "Redo refuses contradictory recorded inner intent atomically against its outer effects and original capture");
    replaceLog(project, false, edit.payload);
    check(History(*project.store).redo().ok && project.state() == after,
        "restoring original recorded forward bytes permits the same exact Redo");
    require(History(*project.store).undo().ok, "metadata replay fixture undo");
    project.store->db().exec("UPDATE event_streams SET future_text='direct metadata writer' WHERE id=101");
    const auto altered = project.state(); const auto alteredHead = History(*project.store).head();
    check(!History(*project.store).redo().ok && project.state() == altered && History(*project.store).head() == alteredHead,
        "versioned Redo retains captured row-delta guards for unknown stream metadata, beyond ANOT hash");
    project.store->db().exec("UPDATE event_streams SET future_text='opaque stream' WHERE id=101");
    check(History(*project.store).redo().ok && project.state() == after,
        "restoring original future metadata restores replay admission");
}

void lateRollbackAndAffinity() {
    const std::array<const char*, 5> triggers{
        "CREATE TRIGGER fail_edit BEFORE UPDATE ON event_streams BEGIN SELECT RAISE(ABORT,'ANOT failure'); END",
        "CREATE TRIGGER fail_edit BEFORE DELETE ON note_expression BEGIN SELECT RAISE(ABORT,'AEXP failure'); END",
        "CREATE TRIGGER fail_edit BEFORE INSERT ON ops BEGIN SELECT RAISE(ABORT,'journal failure'); END",
        "CREATE TRIGGER fail_edit AFTER UPDATE ON event_streams BEGIN UPDATE event_streams SET data=NEW.data WHERE id=NEW.id; END",
        "CREATE TRIGGER fail_edit AFTER UPDATE ON event_streams BEGIN INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(999,1,42,777,X'00'); END"};
    for (std::size_t i = 0; i < triggers.size(); ++i) {
        Project project("late_rollback"); const auto prepared = mixed(project);
        project.store->db().exec(triggers[i]);
        rejected(project, raw(prepared), "late overlap/trigger failure rolls back full typed state, digest, head and log " + std::to_string(i), false);
        project.store->db().exec("DROP TRIGGER fail_edit");
        check(notes::commitOverlapDiff(*project.store, prepared).ok,
            "same prepared operation succeeds once the late failure is removed " + std::to_string(i));
    }
    Project undo("inverse_failure"); const auto prepared = mixed(undo);
    const auto before = undo.state(); require(notes::commitOverlapDiff(*undo.store, prepared).ok, "inverse failure fixture commit");
    const auto after = undo.state(); const auto head = History(*undo.store).head();
    undo.store->db().exec("CREATE TRIGGER fail_restore BEFORE INSERT ON note_expression BEGIN SELECT RAISE(ABORT,'inverse expression failure'); END");
    check(!History(*undo.store).undo().ok && undo.state() == after && History(*undo.store).head() == head &&
        OpJournal(*undo.store).count() == 1, "late inverse-expression insertion refusal leaves exact post-edit state intact");
    undo.store->db().exec("DROP TRIGGER fail_restore");
    check(History(*undo.store).undo().ok && undo.state() == before,
        "same captured Undo restores complete removed expression rows after trigger removal");

    Project affinity("affinity"); affinity.store->db().exec("ALTER TABLE event_streams ADD COLUMN future_real REAL DEFAULT 1.0");
    notes::Diff intent; intent.added.push_back(record(1, 0, 4));
    const auto creation = notes::prepareOverlapDiff(notes::captureNotes(*affinity.store, 1, 101), intent);
    auto coerced = raw(creation);
    require(coerced.payload.at("create").at("future_real").is_number_float(), "capture preserves SQL REAL default");
    coerced.payload["create"]["future_real"] = 1;
    rejected(affinity, coerced, "overlap creation refuses SQL affinity drift from declared INTEGER to stored REAL atomically", false);
    check(!affinity.noteBlob() && OpJournal(*affinity.store).count() == 0,
        "affinity mismatch leaves neither created ANOT row nor operation history");
    check(notes::commitOverlapDiff(*affinity.store, creation).ok,
        "original literal typed creation remains admissible after affinity rollback");
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::vector<std::pair<const char*, void(*)()>> suites{
        {"10,000 actual overlaps and one write", tenThousandOneWrite},
        {"typed expressions and history", typedExpressionsAndHistory},
        {"stale expression sets", staleExpressionSets},
        {"strict versioned packets", strictPackets},
        {"immutable commit and metadata", immutableCommitAndMetadata},
        {"nonempty head guard", nonemptyHeadGuard},
        {"literal creation", literalCreation},
        {"independent history corpus", independentHistoryCorpus},
        {"literal and historical replay", literalAndHistoricalReplay},
        {"contradictory recorded forward", contradictoryRecordedForward},
        {"late rollback and affinity", lateRollbackAndAffinity}};
    for (const auto& [name, run] : suites) {
        std::printf("note overlap owner: %s\n", name);
        try { run(); }
        catch (const std::exception& error) { check(false, std::string(name) + ": " + error.what()); }
    }
    std::printf("note overlap owner: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
