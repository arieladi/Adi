// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/blob.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/note_diff.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace adi;
using notes::Record;
int checks = 0, failures = 0;
constexpr std::uint64_t wideId = (std::uint64_t{1} << 63) + 17;
constexpr auto maximumId = std::numeric_limits<std::uint64_t>::max();
void check(bool value, const std::string& message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message.c_str()); }
}
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

// Independent fixtures/oracle: no production diff/record encoder is used here.
NoteRecord native(std::uint64_t id, std::int64_t start = 0, std::int64_t duration = 7) {
    NoteRecord n{};
    n.note_id = id; n.start_ticks = start; n.dur_ticks = duration;
    n.key = 61; n.vel_on = 87; n.vel_off = 43; n.channel = 5;
    n.flags = 0x8007; n.probability = 7321; n.tuning_cents = 17.25f;
    return n;
}
Record rawRecord(const NoteRecord& note, std::size_t stride = sizeof(NoteRecord)) {
    require(stride >= sizeof(NoteRecord), "fixture record stride");
    Record result;
    result.bytes.resize(stride);
    std::memcpy(result.bytes.data(), &note, sizeof note);
    for (std::size_t i = sizeof note; i < stride; ++i)
        result.bytes[i] = static_cast<std::byte>((note.note_id + i * 29u) & 255u);
    return result;
}
NoteRecord readRecord(const Record& record) {
    require(record.bytes.size() >= sizeof(NoteRecord), "oracle record bounds");
    NoteRecord result{};
    std::memcpy(&result, record.bytes.data(), sizeof result);
    return result;
}
Record changeRecord(Record record, const std::function<void(NoteRecord&)>& change) {
    auto n = readRecord(record);
    change(n);
    std::memcpy(record.bytes.data(), &n, sizeof n);
    return record;
}
std::vector<std::byte> envelope(const std::vector<Record>& records, std::uint16_t stride = 40,
                                bool sorted = true, std::uint16_t version = 1,
                                const std::vector<std::byte>& trailer = {}) {
    require(records.size() <= std::numeric_limits<std::uint32_t>::max(), "fixture count");
    const StreamHeader header{static_cast<std::uint32_t>(FourCC::Notes), version, stride,
        static_cast<std::uint32_t>(records.size()), sorted ? StreamFlags::SortedByTime : 0u};
    std::vector<std::byte> result(sizeof header + records.size() * stride + trailer.size());
    std::memcpy(result.data(), &header, sizeof header);
    for (std::size_t i = 0; i < records.size(); ++i) {
        require(records[i].bytes.size() == stride, "fixture full record extent");
        std::memcpy(result.data() + sizeof header + i * stride, records[i].bytes.data(), stride);
    }
    if (!trailer.empty())
        std::memcpy(result.data() + sizeof header + records.size() * stride, trailer.data(), trailer.size());
    return result;
}
Payload binary(const std::vector<std::byte>& bytes) {
    std::vector<std::uint8_t> values;
    values.reserve(bytes.size());
    for (const auto value : bytes) values.push_back(std::to_integer<std::uint8_t>(value));
    return Payload::binary(std::move(values));
}
std::vector<std::byte> blobColumn(const SQLite::Column& value) {
    const auto* data = static_cast<const std::byte*>(value.getBlob());
    if (value.getBytes() == 0) return {};
    return {data, data + value.getBytes()};
}
struct Fixture {
    test::TempDirectory folder;
    std::unique_ptr<Store> store;
    explicit Fixture(const char* token) : folder("note_diff", token) {
        StoreError error{};
        store = Store::create(folder.path() / "song.adi", error);
        require(static_cast<bool>(store), std::string("create fixture: ") + toString(error));
        store->db().exec("INSERT INTO tracks(id,kind) VALUES(1,'audio'),(2,'midi'),(3,'midi')");
        store->db().exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) "
                        "VALUES(1,1,'midi',0,1000000),(2,2,'midi',0,1000000),(3,3,'midi',0,1000000)");
    }
    void seed(std::int64_t clip, const std::vector<std::byte>& bytes, std::int64_t row = 101) {
        SQLite::Statement st(store->db(), "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(?,?,'notes',?)");
        st.bind(1, row); st.bind(2, clip);
        st.bind(3, bytes.data(), static_cast<int>(bytes.size())); st.exec();
    }
    std::optional<std::vector<std::byte>> maybeBlob(std::int64_t clip = 1) {
        SQLite::Statement st(store->db(), "SELECT data FROM event_streams WHERE clip_id=? "
            "AND stream_kind='notes' AND channel IS NULL AND controller IS NULL");
        st.bind(1, clip);
        if (!st.executeStep()) return std::nullopt;
        return blobColumn(st.getColumn(0));
    }
    std::vector<std::byte> blob(std::int64_t clip = 1) {
        auto value = maybeBlob(clip); require(value.has_value(), "fixture canonical stream present");
        return std::move(*value);
    }
    void replaceBlob(const std::vector<std::byte>& bytes, std::int64_t row = 101) {
        SQLite::Statement st(store->db(), "UPDATE event_streams SET data=? WHERE id=?");
        st.bind(1, bytes.data(), static_cast<int>(bytes.size())); st.bind(2, row); st.exec();
    }
    void expression(std::int64_t clip, std::uint64_t note, int dimension) {
        // Deliberately opaque: delete/carry/undo must not interpret future AEXP.
        const std::array bytes{std::byte{0x41}, std::byte{0x45}, std::byte{0x58},
                               std::byte{0x50}, std::byte{0xfe}, std::byte{0x00}};
        require(store->putNoteExpression(clip, note, dimension, bytes), "expression fixture");
    }
    std::int64_t expressionCount(std::int64_t clip, std::uint64_t note) {
        SQLite::Statement st(store->db(), "SELECT count(*) FROM note_expression WHERE clip_id=? AND note_id=?");
        st.bind(1, clip); st.bind(2, std::bit_cast<std::int64_t>(note));
        require(st.executeStep(), "expression count query"); return st.getColumn(0).getInt64();
    }
    void reopen(bool readonly = false) {
        const auto path = store->path(); store.reset(); StoreError error{};
        store = Store::open(path, error, readonly);
        require(static_cast<bool>(store), std::string("reopen fixture: ") + toString(error));
    }
};
struct WriteCounter {
    sqlite3* db;
    std::int64_t target;
    int targetWrites = 0, targetDeletes = 0, allWrites = 0;
    WriteCounter(Store& store, std::int64_t row) : db(store.db().getHandle()), target(row) {
        sqlite3_update_hook(db, [](void* context, int operation, const char*, const char* table, sqlite3_int64 id) {
            auto& self = *static_cast<WriteCounter*>(context); ++self.allWrites;
            if (std::strcmp(table, "event_streams") == 0 && id == self.target) {
                if (operation == SQLITE_INSERT || operation == SQLITE_UPDATE) ++self.targetWrites;
                if (operation == SQLITE_DELETE) ++self.targetDeletes;
            }
        }, this);
    }
    ~WriteCounter() { sqlite3_update_hook(db, nullptr, nullptr); }
    void reset() { targetWrites = 0; targetDeletes = 0; allWrites = 0; }
};
struct SqlCounter {
    sqlite3* db;
    unsigned statements = 0;
    explicit SqlCounter(Store& store) : db(store.db().getHandle()) {
        sqlite3_trace_v2(db, SQLITE_TRACE_STMT, [](unsigned, void* context, void*, void*) -> int {
            ++static_cast<SqlCounter*>(context)->statements; return 0;
        }, this);
    }
    ~SqlCounter() { sqlite3_trace_v2(db, 0, nullptr, nullptr); }
};
notes::PreparedDiff changedVelocity(Fixture& f, std::int64_t clip = 1) {
    const auto snapshot = notes::captureNotes(*f.store, clip);
    require(snapshot.ok && !snapshot.records().empty(), "capture change fixture: " + snapshot.error);
    const auto old = snapshot.records().front();
    notes::Diff diff;
    diff.changed.push_back({old, changeRecord(old, [](NoteRecord& n) { n.vel_on = 99; })});
    return notes::prepareDiff(snapshot, diff);
}
void rejectedUnchanged(Fixture& f, const OpRequest& request, const std::string& label) {
    const auto before = digestProject(*f.store).text;
    const auto count = OpJournal(*f.store).count();
    const auto head = OpJournal(*f.store).headSeq();
    const auto result = OpJournal(*f.store).commit(request);
    check(!result.ok && digestProject(*f.store).text == before &&
              OpJournal(*f.store).count() == count && OpJournal(*f.store).headSeq() == head, label);
}

void immutableAndPure() {
    Fixture f("pure");
    const auto a = rawRecord(native(1, 50));
    const auto b = rawRecord(native(wideId, 50));
    f.seed(1, envelope({b, a}));
    const auto before = digestProject(*f.store).text;
    auto snapshot = notes::captureNotes(*f.store, 1);
    require(snapshot.ok && snapshot.records().size() == 2, "capture immutable: " + snapshot.error);
    check(snapshot.streamId() == std::optional<std::int64_t>(101) && snapshot.from().size() == 64,
          "capture identifies canonical stream and whole-blob hash");
    notes::Diff diff;
    diff.changed.push_back({a, changeRecord(a, [](NoteRecord& n) { n.vel_on = 66; })});
    notes::PreparedDiff prepared;
    {
        SqlCounter sql(*f.store); WriteCounter writes(*f.store, 101);
        prepared = notes::prepareDiff(snapshot, diff);
        check(prepared.ok && sql.statements == 0 && writes.allWrites == 0,
              "pure prepare performs no SQL and no project writes");
    }
    check(digestProject(*f.store).text == before && OpJournal(*f.store).count() == 0,
          "capture and preparation leave state and history unchanged");
    {
        SQLite::Transaction outer(f.store->db(), SQLite::TransactionBehavior::DEFERRED);
        const auto nested = notes::captureNotes(*f.store, 1);
        check(nested.ok && nested.from() == snapshot.from() &&
                  sqlite3_get_autocommit(f.store->db().getHandle()) == 0,
              "owner capture reuses a caller read transaction without ending it");
        outer.commit();
    }
    check(prepared.options.expectHead.has_value() && !*prepared.options.expectHead,
          "empty journal head is an engaged null-head precondition");
    require(prepared.ok && prepared.requests.size() == 1, "one prepared request");
    const auto originalPacket = prepared.requests.front().payload;
    diff.changed.front().newRecord.bytes[25] = std::byte{12};
    check(prepared.requests.front().payload == originalPacket && snapshot.records()[1] == a,
          "prepared request owns values and immutable snapshot retains original record");
    const auto copied = snapshot;
    f.store.reset();
    const auto detached = notes::prepareDiff(copied, notes::Diff{{}, {}, {{a, a}}});
    check(detached.ok && detached.requests.empty() && detached.effects.changed.empty(),
          "snapshot remains usable without Store and identical changes are no-ops");
    StoreError error{}; f.store = Store::open(f.folder.path() / "song.adi", error);
    require(static_cast<bool>(f.store), "reopen detached snapshot fixture");
    check(notes::commitDiff(*f.store, prepared).ok, "detached prepared value commits on its original project");
    auto expected = envelope({b, changeRecord(a, [](NoteRecord& n) { n.vel_on = 66; })});
    check(f.blob() == expected, "non-time edit preserves equal-time record order exactly");
    check(History(*f.store).undo().ok && digestProject(*f.store).text == before,
          "exact inverse restores original coincident ordering");
    notes::Snapshot fabricated; fabricated.ok = true;
    check(!notes::prepareDiff(fabricated, {}).ok, "public ok flag cannot manufacture snapshot backing");
    Fixture other("other_source"); other.seed(1, envelope({b, a}));
    const auto otherBefore = digestProject(*other.store).text;
    check(!notes::commitDiff(*other.store, prepared).ok && digestProject(*other.store).text == otherBefore,
          "commit refuses a different project with identical note content");
}

void tenThousandOneWrite() {
    Fixture f("ten_thousand");
    std::vector<Record> originals;
    originals.reserve(10000);
    for (std::uint64_t i = 1; i <= 10000; ++i)
        originals.push_back(rawRecord(native(i, static_cast<std::int64_t>(i * 8u), 4)));
    const auto initial = envelope(originals);
    f.seed(1, initial);
    const auto before = digestProject(*f.store).text;
    const auto snapshot = notes::captureNotes(*f.store, 1);
    require(snapshot.ok, "large capture: " + snapshot.error);
    notes::Diff diff;
    auto expectedRecords = originals;
    for (std::size_t i = 0; i < originals.size(); ++i) {
        expectedRecords[i].bytes[25] = std::byte{71};
        diff.changed.push_back({originals[i], expectedRecords[i]});
    }
    const auto prepared = notes::prepareDiff(snapshot, diff);
    require(prepared.ok && prepared.requests.size() == 1, "large prepare: " + prepared.error);
    WriteCounter writes(*f.store, 101);
    const auto committed = notes::commitDiff(*f.store, prepared);
    check(committed.ok, "10,000-note diff commits: " + committed.error);
    check(writes.targetWrites == 1 && writes.targetDeletes == 0,
          "10,000-note diff performs exactly one actual SQLite note BLOB write");
    check(f.blob() == envelope(expectedRecords), "all 10,000 note changes match independent byte oracle");
    check(OpJournal(*f.store).count() == 1 && committed.seqs.size() == 1,
          "10,000-note diff is one op and one undo action");
    writes.reset();
    const auto undone = History(*f.store).undo();
    check(undone.ok && undone.opsApplied == 1 && writes.targetWrites == 1 &&
              digestProject(*f.store).text == before,
          "10,000-note undo writes the blob once and restores exact project state");
    writes.reset();
    check(History(*f.store).redo().ok && writes.targetWrites == 1 && f.blob() == envelope(expectedRecords),
          "10,000-note redo writes once and reproduces exact bytes");
}

void futureAndExpressions() {
    Fixture f("future_expression");
    const auto a = rawRecord(native(maximumId, 10), 48);
    const auto b = rawRecord(native(wideId, 20), 48);
    const auto c = rawRecord(native(17, 30), 48);
    const std::vector<std::byte> trailer{std::byte{0xde}, std::byte{0xad}, std::byte{0x00}, std::byte{0xfe}};
    f.seed(1, envelope({a, b, c}, 48, true, 7, trailer));
    f.store->db().exec("ALTER TABLE event_streams ADD COLUMN future BLOB");
    f.store->db().exec("UPDATE event_streams SET future=X'0055FF' WHERE id=101");
    f.store->db().exec("ALTER TABLE note_expression ADD COLUMN future TEXT");
    f.store->db().exec("ALTER TABLE note_expression ADD COLUMN future_integer INTEGER");
    f.store->db().exec("ALTER TABLE note_expression ADD COLUMN future_real REAL");
    f.store->db().exec("ALTER TABLE note_expression ADD COLUMN future_empty BLOB");
    f.store->db().exec("ALTER TABLE note_expression ADD COLUMN future_null TEXT");
    for (const int dimension : {0, 4, 64, 999}) f.expression(1, maximumId, dimension);
    f.expression(1, wideId, 1);
    f.store->db().exec("UPDATE note_expression SET future='opaque extension',future_integer=-17,"
                     "future_real=1.25,future_empty=X'',future_null=NULL");
    f.store->db().exec("INSERT INTO event_streams(id,clip_id,stream_kind,channel,controller,data) "
                     "VALUES(77,1,'notes',2,9,X'0102'),(78,1,'cc',2,7,X'FF00')");
    const auto before = digestProject(*f.store).text;
    const auto snapshot = notes::captureNotes(*f.store, 1);
    require(snapshot.ok && snapshot.records().size() == 3, "canonical future capture: " + snapshot.error);
    notes::Diff diff;
    diff.removed.push_back(a);
    const auto moved = changeRecord(b, [](NoteRecord& n) { n.start_ticks = 40; n.key = 72; });
    diff.changed.push_back({b, moved});
    const auto added = rawRecord(native(555, 15), 48);
    diff.added.push_back(added);
    const auto prepared = notes::prepareDiff(snapshot, diff);
    require(prepared.ok && prepared.requests.size() == 1, "future prepare: " + prepared.error);
    const auto& removed = prepared.requests.front().payload.at("removed");
    check(removed.size() == 1 && removed.at(0).at("expressions").size() == 4,
          "removed packet captures every expression dimension and full-width note identity");
    {
        WriteCounter writes(*f.store, 101);
        check(notes::commitDiff(*f.store, prepared).ok, "mixed future-stride diff commits");
        check(writes.targetWrites == 1 && writes.targetDeletes == 0,
              "mixed addition/removal/change serializes its future note stream exactly once");
    }
    check(f.blob() == envelope({added, c, moved}, 48, true, 7, trailer),
          "addition/removal/move retain original version, stride, tails and trailer");
    check(f.expressionCount(1, maximumId) == 0 && f.expressionCount(1, wideId) == 1,
          "deleted uint64-max note removes all expressions without touching moved note expression");
    check(f.store->db().execAndGet("SELECT hex(future) FROM event_streams WHERE id=101").getString() == "0055FF" &&
              f.store->db().execAndGet("SELECT hex(data) FROM event_streams WHERE id=77").getString() == "0102",
          "canonical row extension and auxiliary addressed notes stream remain untouched");
    const auto after = digestProject(*f.store).text;
    check(History(*f.store).undo().ok && digestProject(*f.store).text == before,
          "inactive note undo restores every flag, expression row ID/type/blob and future column exactly");
    f.reopen();
    check(History(*f.store).redo().ok && digestProject(*f.store).text == after,
          "future-expression inverse survives reopen and redo");
    const auto current = notes::captureNotes(*f.store, 1);
    notes::Diff unknownSynthesis; unknownSynthesis.added.push_back(rawRecord(native(556, 100)));
    check(!notes::prepareDiff(current, unknownSynthesis).ok && digestProject(*f.store).text == after,
          "native-only addition cannot invent an unknown future tail");
}

void creationAndSignedRows() {
    Fixture f("creation_defaults");
    // Fixture-only future schema; random default proves capture evaluates once.
    f.store->db().exec("DROP TABLE event_streams");
    f.store->db().exec("CREATE TABLE event_streams(id INTEGER PRIMARY KEY,clip_id INTEGER NOT NULL "
        "REFERENCES clips(id) ON DELETE CASCADE,stream_kind TEXT NOT NULL,channel INTEGER,controller INTEGER,"
        "data BLOB NOT NULL,future_nonce BLOB NOT NULL DEFAULT(randomblob(8)),"
        "future_text TEXT NOT NULL DEFAULT 'captured default') STRICT");
    f.store->db().exec("CREATE UNIQUE INDEX idx_stream_clip ON event_streams"
        "(clip_id,stream_kind,IFNULL(channel,-1),IFNULL(controller,-1))");
    const auto absent = notes::captureNotes(*f.store, 1);
    require(absent.ok, "read absent snapshot: " + absent.error);
    check(!absent.hasStream() && !absent.streamId() && absent.from() == "absent" && absent.records().empty(),
          "absent stream is distinct from a present empty stream");
    check(notes::prepareDiff(absent, {}).ok && notes::prepareDiff(absent, {}).requests.empty(),
          "empty absent diff is a no-op without allocating an identity");
    notes::Diff diff; diff.added.push_back(rawRecord(native(maximumId, -12, 8)));
    check(!notes::prepareDiff(absent, diff).ok, "nonempty absent creation requires caller stream identity");
    const auto before = digestProject(*f.store).text;
    notes::Snapshot snapshot;
    {
        WriteCounter writes(*f.store, 0);
        snapshot = notes::captureNotes(*f.store, 1, 0);
        check(snapshot.ok && writes.allWrites == 0,
              "full future default row is captured without a project write");
    }
    require(snapshot.ok, "creation capture: " + snapshot.error);
    const auto prepared = notes::prepareDiff(snapshot, diff);
    const auto again = notes::prepareDiff(snapshot, diff);
    require(prepared.ok && prepared.requests.size() == 1 && again.ok && again.requests.size() == 1,
            "creation prepares: " + prepared.error);
    const auto& payload = prepared.requests.front().payload;
    check(snapshot.creationId() == std::optional<std::int64_t>(0) && payload.at("stream_id") == 0 &&
              payload.at("from") == "absent" && payload == again.requests.front().payload,
          "zero SQL identity and random defaults are captured literals reused by pure preparation");
    const auto capturedNonce = payload.at("create").at("future_nonce");
    check(notes::commitDiff(*f.store, prepared).ok, "prepared first insertion with future defaults commits");
    SQLite::Statement nonce(f.store->db(), "SELECT future_nonce,future_text FROM event_streams WHERE id=0");
    require(nonce.executeStep(), "created default row");
    check(binary(blobColumn(nonce.getColumn(0))) == capturedNonce &&
              nonce.getColumn(1).getString() == "captured default",
          "apply inserts the captured random/default values without reevaluation");
    nonce.reset();
    const auto after = digestProject(*f.store).text;
    check(History(*f.store).undo().ok && !f.maybeBlob() && digestProject(*f.store).text == before,
          "first-insert undo restores exact row absence");
    check(History(*f.store).redo().ok && digestProject(*f.store).text == after,
          "first-insert redo preserves literal identity/default bytes");
    const auto emptyPresent = notes::captureNotes(*f.store, 2, -7);
    notes::Diff collision; collision.added.push_back(rawRecord(native(1)));
    const auto collisionPlan = notes::prepareDiff(emptyPresent, collision);
    require(collisionPlan.ok && collisionPlan.requests.size() == 1, "negative stream creation prepare");
    f.store->db().exec("INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(-7,3,'cc',X'01')");
    rejectedUnchanged(f, collisionPlan.requests.front(), "captured absent stream ID collision refuses atomically");

    Fixture signedRows("signed_rows");
    signedRows.store->db().exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(-5,1,'midi',0,100)");
    signedRows.seed(-5, envelope({rawRecord(native(wideId, -20, 5))}), -11);
    const auto plan = changedVelocity(signedRows, -5);
    check(plan.ok && plan.requests.size() == 1 && plan.requests.front().payload.at("stream_id") == -11 &&
              notes::commitDiff(*signedRows.store, plan).ok,
          "signed SQL clip and stream identities stay valid while note IDs remain full uint64");
}

void numericAndMalformed() {
    notes::NoteValues good;
    good.start = -9; good.duration = 3; good.id = maximumId;
    good.key = 127; good.velocity = 127; good.releaseVelocity = 127; good.channel = 15;
    good.probability = 10000; good.flags = 65535; good.cents = -1200;
    std::string error;
    const std::array tail{std::byte{0x00}, std::byte{0xff}};
    const auto made = notes::makeRecord(good, tail, error);
    require(made.has_value(), "valid typed record: " + error);
    const auto decoded = notes::values(*made, error);
    check(decoded && *decoded == good && made->bytes.size() == 42 && made->bytes.back() == std::byte{0xff},
          "typed helpers retain full-width ID, signed ticks, bounds and explicit tail");
    auto replacement = good; replacement.cents = 1200; replacement.start = 9007199254740993LL;
    const auto patched = notes::withValues(*made, replacement, error);
    check(patched && patched->bytes.size() == 42 &&
              notes::values(*patched, error) == std::optional<notes::NoteValues>(replacement) &&
              patched->bytes[40] == std::byte{0x00} && patched->bytes[41] == std::byte{0xff},
          "typed patch keeps exact ticks above 2^53 and opaque tail");
    const std::vector<std::function<void(notes::NoteValues&)>> invalid = {
        [](auto& n) { n.id = 0; }, [](auto& n) { n.duration = 0; },
        [](auto& n) { n.duration = -1; }, [](auto& n) { n.key = -1; },
        [](auto& n) { n.key = 256; }, [](auto& n) { n.velocity = 0; },
        [](auto& n) { n.velocity = 128; }, [](auto& n) { n.releaseVelocity = -1; },
        [](auto& n) { n.releaseVelocity = 128; }, [](auto& n) { n.channel = 16; },
        [](auto& n) { n.channel = -1; }, [](auto& n) { n.probability = -1; },
        [](auto& n) { n.probability = 10001; }, [](auto& n) { n.flags = 65536; },
        [](auto& n) { n.cents = 1200.01; }, [](auto& n) { n.cents = -1200.01; },
        [](auto& n) { n.cents = std::numeric_limits<double>::infinity(); },
        [](auto& n) { n.cents = std::numeric_limits<double>::quiet_NaN(); },
        [](auto& n) { n.cents = std::numeric_limits<double>::max(); },
        [](auto& n) { n.start = std::numeric_limits<std::int64_t>::max(); n.duration = 1; }
    };
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        auto value = good; invalid[i](value);
        check(!notes::makeRecord(value, {}, error) && !notes::withValues(*made, value, error),
              "typed numeric admission rejects before narrowing case " + std::to_string(i));
    }
    Fixture f("malformed");
    const auto a = rawRecord(native(1)); const auto b = rawRecord(native(2, 20));
    const auto original = envelope({a, b}); f.seed(1, original);
    const auto snapshot = notes::captureNotes(*f.store, 1);
    require(snapshot.ok, "malformed base capture");
    std::vector<notes::Diff> bad;
    bad.push_back({{a}, {}, {}});
    bad.push_back({{rawRecord(native(7)), rawRecord(native(7))}, {}, {}});
    bad.push_back({{}, {rawRecord(native(999))}, {}});
    bad.push_back({{}, {a, a}, {}});
    bad.push_back({{}, {a}, {{a, changeRecord(a, [](NoteRecord& n) { ++n.dur_ticks; })}}});
    bad.push_back({{}, {}, {{a, changeRecord(a, [](NoteRecord& n) { n.note_id = 5; })}}});
    bad.push_back({{}, {}, {{changeRecord(a, [](NoteRecord& n) { ++n.vel_on; }), b}}});
    bad.push_back({{}, {}, {{changeRecord(a, [](NoteRecord& n) { ++n.vel_on; }),
                             changeRecord(a, [](NoteRecord& n) { ++n.dur_ticks; })}}});
    bad.push_back({{changeRecord(rawRecord(native(7)), [](NoteRecord& n) { n.reserved = 1; })}, {}, {}});
    bad.push_back({{changeRecord(rawRecord(native(7)), [](NoteRecord& n) { n.vel_on = 0; })}, {}, {}});
    bad.push_back({{Record{{std::byte{1}}}}, {}, {}});
    for (std::size_t i = 0; i < bad.size(); ++i)
        check(!notes::prepareDiff(snapshot, bad[i]).ok && f.blob() == original && OpJournal(*f.store).count() == 0,
              "bad diff rejects exact identities/old bytes/ranges without writes case " + std::to_string(i));
    std::vector<std::vector<std::byte>> malformed;
    auto corrupt = original; corrupt.resize(15); malformed.push_back(corrupt);
    corrupt = original; corrupt[0] = std::byte{0}; malformed.push_back(corrupt);
    corrupt = original; corrupt[4] = std::byte{0}; corrupt[5] = std::byte{0}; malformed.push_back(corrupt);
    corrupt = original; corrupt[6] = std::byte{39}; malformed.push_back(corrupt);
    corrupt = original; corrupt[12] = std::byte{2}; malformed.push_back(corrupt);
    corrupt = original; corrupt[12] = std::byte{8}; malformed.push_back(corrupt);
    malformed.push_back(envelope({a, a}));
    malformed.push_back(envelope({b, a}));
    malformed.push_back(envelope({rawRecord(native(0))}));
    malformed.push_back(envelope({changeRecord(a, [](NoteRecord& n) { n.key = 255; })}));
    corrupt = original;
    corrupt.resize(16);
    for (std::size_t i = 6; i < 12; ++i) corrupt[i] = std::byte{0xff};
    malformed.push_back(corrupt);
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        f.replaceBlob(malformed[i]);
        check(!notes::captureNotes(*f.store, 1).ok && f.blob() == malformed[i] && OpJournal(*f.store).count() == 0,
              "unreadable/unsupported stored stream refuses without empty rewrite case " + std::to_string(i));
    }
    f.replaceBlob(original);
    const auto prepared = changedVelocity(f);
    require(prepared.ok && prepared.requests.size() == 1, "wire validation prepare");
    const auto goodRequest = prepared.requests.front();
    std::vector<OpRequest> invalidWire;
    auto request = goodRequest; request.payload["stream_id"] = maximumId; invalidWire.push_back(request);
    request = goodRequest; request.payload["clip"] = 1.5; invalidWire.push_back(request);
    request = goodRequest; request.payload["from"] = "not-a-hash"; invalidWire.push_back(request);
    request = goodRequest; request.payload["changed"][0]["new"]["unknown"] = 1; invalidWire.push_back(request);
    request = goodRequest; request.payload["changed"][0]["new"]["bytes"] = Payload::array({-1}); invalidWire.push_back(request);
    request = goodRequest; request.payload["changed"][0]["new"]["bytes"] = Payload::array({1.5}); invalidWire.push_back(request);
    request = goodRequest; request.payload["changed"][0]["new"]["bytes"] = Payload::array({256}); invalidWire.push_back(request);
    request = goodRequest;
    request.payload["changed"][0]["old"]["bytes"] = binary(changeRecord(a, [](NoteRecord& n) { ++n.vel_on; }).bytes);
    rejectedUnchanged(f, request, "old-record byte guard refuses a same-ID mismatch with correct from hash");
    request = goodRequest; request.payload["note_capture"] = Payload::object(); invalidWire.push_back(request);
    request = goodRequest; request.payload["create"] = Payload::object(); invalidWire.push_back(request);
    for (std::size_t i = 0; i < invalidWire.size(); ++i)
        rejectedUnchanged(f, invalidWire[i], "closed forward grammar rejects malformed wire case " + std::to_string(i));
}

void staleGuards() {
    // Diff touches note 1 only; changed note 2/tail/header/trailer preserve its
    // old-record precondition. Direct commit intentionally omits expected head.
    for (int mode = 0; mode < 5; ++mode) {
        Fixture f("stale_hash");
        const auto a = rawRecord(native(1), 48); const auto b = rawRecord(native(2, 20), 48);
        const auto original = envelope({a, b}, 48, true, 7, {std::byte{0x51}});
        f.seed(1, original);
        const auto prepared = changedVelocity(f);
        require(prepared.ok && prepared.requests.size() == 1, "stale guard prepares");
        auto changed = original;
        if (mode == 0) changed[16 + 48 + 25] = std::byte{88};
        if (mode == 1) changed[16 + 48 + 43] ^= std::byte{1};
        if (mode == 2) changed[4] = std::byte{8};
        if (mode == 3) changed.back() ^= std::byte{1};
        if (mode == 4) f.store->db().exec("UPDATE event_streams SET id=777 WHERE id=101");
        else f.replaceBlob(changed);
        const std::array labels = {
            "from hash refuses unrelated note mutation while requested old bytes still match",
            "from hash refuses unrelated opaque tail mutation while requested old bytes still match",
            "from hash refuses header-only mutation while requested old bytes still match",
            "from hash refuses trailer-only mutation while requested old bytes still match",
            "stream identity guard refuses equal bytes under a replacement row ID"};
        rejectedUnchanged(f, prepared.requests.front(), labels[static_cast<std::size_t>(mode)]);
    }
    Fixture expression("stale_empty_expression");
    const auto a = rawRecord(native(maximumId)); expression.seed(1, envelope({a}));
    const auto snapshot = notes::captureNotes(*expression.store, 1);
    notes::Diff diff; diff.removed.push_back(a);
    const auto prepared = notes::prepareDiff(snapshot, diff);
    require(prepared.ok && prepared.requests.size() == 1, "empty expression stale prepare");
    check(prepared.requests.front().payload.at("removed").at(0).at("expressions").empty(),
          "removed note explicitly guards an empty expression set");
    expression.expression(1, maximumId, 64);
    rejectedUnchanged(expression, prepared.requests.front(),
        "expression complete-set guard refuses a new row in a formerly empty set");
    const auto withExpression = notes::captureNotes(*expression.store, 1);
    const auto second = notes::prepareDiff(withExpression, diff);
    require(second.ok && second.requests.size() == 1, "nonempty expression stale prepare");
    expression.store->db().exec("UPDATE note_expression SET data=X'00FF'");
    rejectedUnchanged(expression, second.requests.front(), "expression row-byte guard catches expression-only mutation");

    Fixture wal("wal_stale"); wal.seed(1, envelope({rawRecord(native(1)), rawRecord(native(2, 20))}));
    const auto walPlan = changedVelocity(wal);
    require(walPlan.ok && walPlan.requests.size() == 1, "WAL prepare");
    {
        SQLite::Database other(wal.store->path().string(), SQLite::OPEN_READWRITE);
        auto bytes = wal.blob(); bytes[16 + 40 + 25] = std::byte{92};
        SQLite::Statement update(other, "UPDATE event_streams SET data=? WHERE id=101");
        update.bind(1, bytes.data(), static_cast<int>(bytes.size())); update.exec();
    }
    rejectedUnchanged(wal, walPlan.requests.front(), "second WAL writer cannot bypass content guard without a head change");
    Fixture head("head_stale"); head.seed(1, envelope({rawRecord(native(1))}));
    const auto headPlan = changedVelocity(head);
    OpRequest rename; rename.opType = "project.setName"; rename.payload = {{"name", "Unrelated edit"}};
    require(OpJournal(*head.store).commit(rename).ok, "move journal head");
    const auto beforeHead = digestProject(*head.store).text;
    const auto stale = notes::commitDiff(*head.store, headPlan);
    check(!stale.ok && stale.stale && digestProject(*head.store).text == beforeHead,
          "supplemental expected head refuses unchanged stream after an unrelated edit");
}

void coherentOwnerCapture() {
    Fixture f("coherent_capture");
    const auto a = rawRecord(native(1));
    const auto original = envelope({a});
    const auto changed = envelope({changeRecord(a, [](NoteRecord& n) { n.vel_on = 91; })});
    f.seed(1, original);
    const auto originalSnapshot = notes::captureNotes(*f.store, 1);
    require(originalSnapshot.ok, "original coherent snapshot");
    SQLite::Database writer(f.store->path().string(), SQLite::OPEN_READWRITE);
    struct DuringCapture {
        SQLite::Database* writer;
        const std::vector<std::byte>* changed;
        bool fired = false, failed = false;
    } mutation{&writer, &changed};
    sqlite3_trace_v2(f.store->db().getHandle(), SQLITE_TRACE_STMT,
        [](unsigned, void* context, void* statement, void*) -> int {
            auto& state = *static_cast<DuringCapture*>(context);
            const auto* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
            if (state.fired || !sql || std::string_view(sql).find("note_expression") == std::string_view::npos)
                return 0;
            state.fired = true;
            try {
                SQLite::Transaction transaction(*state.writer);
                SQLite::Statement update(*state.writer, "UPDATE event_streams SET data=? WHERE id=101");
                update.bind(1, state.changed->data(), static_cast<int>(state.changed->size()));
                update.exec();
                state.writer->exec("INSERT INTO note_expression(clip_id,note_id,dimension,data) VALUES(1,1,64,X'0102')");
                transaction.commit();
            } catch (const std::exception&) { state.failed = true; }
            return 0;
        }, &mutation);
    const auto captured = notes::captureNotes(*f.store, 1);
    sqlite3_trace_v2(f.store->db().getHandle(), 0, nullptr, nullptr);
    notes::Diff remove; remove.removed.push_back(a);
    const auto oldPlan = notes::prepareDiff(captured, remove);
    check(mutation.fired && !mutation.failed && captured.ok && captured.from() == originalSnapshot.from() &&
              oldPlan.ok && oldPlan.requests.size() == 1 &&
              oldPlan.requests.front().payload.at("removed").at(0).at("expressions").empty(),
          "one read transaction cannot mix old note bytes with a newer expression row from a WAL writer");
    const auto next = notes::captureNotes(*f.store, 1);
    require(next.ok && next.records().size() == 1, "next coherent snapshot");
    notes::Diff removeNew; removeNew.removed.push_back(next.records().front());
    const auto newPlan = notes::prepareDiff(next, removeNew);
    check(next.from() != originalSnapshot.from() && newPlan.ok && newPlan.requests.size() == 1 &&
              newPlan.requests.front().payload.at("removed").at(0).at("expressions").size() == 1,
          "next owner snapshot sees complete new note and expression state");
}

void sqlStorageClassGuards() {
    // STRICT ANY has no coercing affinity: INTEGER 1 and REAL 1.0 retain
    // different SQLite storage classes despite ordinary JSON numeric equality.
    {
        Fixture f("typed_expression_from");
        const auto note = rawRecord(native(maximumId));
        f.seed(1, envelope({note})); f.expression(1, maximumId, 64);
        f.store->db().exec("ALTER TABLE note_expression ADD COLUMN future_type ANY");
        f.store->db().exec("UPDATE note_expression SET future_type=1");
        notes::Diff diff; diff.removed.push_back(note);
        const auto prepared = notes::prepareDiff(notes::captureNotes(*f.store, 1), diff);
        require(prepared.ok && prepared.requests.size() == 1, "typed expression preparation");
        const auto& rows = prepared.requests.front().payload.at("removed").at(0).at("expressions");
        check(rows.size() == 1 && rows.at(0).at("future_type").is_number_integer(),
              "removed expression precondition retains INTEGER storage class");
        const auto head = OpJournal(*f.store).headSeq();
        f.store->db().exec("UPDATE note_expression SET future_type=CAST(1 AS REAL)");
        require(f.store->db().execAndGet("SELECT typeof(future_type) FROM note_expression").getString() == "real",
                "fixture preserves REAL in no-affinity column");
        rejectedUnchanged(f, prepared.requests.front(),
            "expression precondition refuses INTEGER-to-REAL change with equal numeric value and unchanged head");
        check(OpJournal(*f.store).headSeq() == head &&
                  f.store->db().execAndGet("SELECT typeof(future_type) FROM note_expression").getString() == "real",
              "refused expression precondition preserves current REAL storage and history head");
    }
    {
        Fixture f("typed_inverse_stream");
        f.seed(1, envelope({rawRecord(native(1))}));
        f.store->db().exec("ALTER TABLE event_streams ADD COLUMN future_type ANY");
        f.store->db().exec("UPDATE event_streams SET future_type=1");
        const auto prepared = changedVelocity(f);
        require(prepared.ok && notes::commitDiff(*f.store, prepared).ok, "typed inverse stream setup");
        const auto head = History(*f.store).head();
        f.store->db().exec("UPDATE event_streams SET future_type=CAST(1 AS REAL)");
        require(f.store->db().execAndGet("SELECT typeof(future_type) FROM event_streams").getString() == "real",
                "inverse stream fixture preserves REAL");
        const auto before = digestProject(*f.store).text;
        WriteCounter writes(*f.store, 101);
        const auto result = History(*f.store).undo();
        check(!result.ok && writes.allWrites == 0 && digestProject(*f.store).text == before &&
                  History(*f.store).head() == head &&
                  f.store->db().execAndGet("SELECT typeof(future_type) FROM event_streams").getString() == "real",
              "captured inverse current-stream guard distinguishes INTEGER from equal REAL without changing type");
    }
    {
        Fixture f("typed_inverse_expression");
        const auto note = rawRecord(native(maximumId));
        f.seed(1, envelope({note})); f.expression(1, maximumId, 64);
        f.store->db().exec("ALTER TABLE note_expression ADD COLUMN future_type ANY");
        f.store->db().exec("UPDATE note_expression SET future_type=1");
        notes::Diff diff; diff.removed.push_back(note);
        const auto prepared = notes::prepareDiff(notes::captureNotes(*f.store, 1), diff);
        require(prepared.ok && notes::commitDiff(*f.store, prepared).ok, "typed inverse expression setup");
        const auto log = OpJournal(*f.store).recent(1);
        require(log.size() == 1 && log.front().inverse.has_value(), "actual captured deletion inverse");
        auto capture = *log.front().inverse;
        require(History(*f.store).undo().ok, "restore genuine captured expression before guard test");
        // The opposite-direction capture is a valid deletion of the restored
        // state. It exercises a non-null expression before-image; normal note
        // deletion inverses have null before-images because they restore rows.
        auto& packet = capture.at("note_capture");
        auto& stream = packet.at("stream");
        std::swap(stream.at("before"), stream.at("after"));
        for (auto& row : packet.at("expressions")) std::swap(row.at("before"), row.at("after"));
        f.store->db().exec("UPDATE note_expression SET future_type=CAST(1 AS REAL)");
        require(f.store->db().execAndGet("SELECT typeof(future_type) FROM note_expression").getString() == "real",
                "inverse expression fixture preserves REAL");
        const auto before = digestProject(*f.store).text;
        const auto head = History(*f.store).head();
        const auto* descriptor = OpRegistry::instance().find("note.applyDiff");
        require(descriptor && descriptor->apply, "native diff descriptor exists");
        SQLite::Transaction transaction(f.store->db());
        WriteCounter writes(*f.store, 101);
        OpContext context{*f.store, f.store->db()};
        std::string error;
        const bool applied = descriptor->apply(context, capture, error);
        check(!applied && writes.allWrites == 0 && digestProject(*f.store).text == before &&
                  History(*f.store).head() == head &&
                  f.store->db().execAndGet("SELECT coalesce((SELECT typeof(future_type) FROM note_expression),'missing')").getString() == "real",
              "captured inverse current-expression guard refuses equal-number storage-class changes");
        // Never commit the direct handler probe, including on a planted fault.
    }
    {
        Fixture f("nonfinite_create_cell");
        f.store->db().exec("ALTER TABLE event_streams ADD COLUMN future_real REAL DEFAULT 1.0");
        notes::Diff diff; diff.added.push_back(rawRecord(native(1)));
        const auto prepared = notes::prepareDiff(notes::captureNotes(*f.store, 1, 101), diff);
        require(prepared.ok && prepared.requests.size() == 1, "finite native creation setup");
        check(prepared.requests.front().payload.at("create").at("future_real").is_number_float(),
              "owner captures REAL-affinity default as a REAL typed value");
        auto forged = prepared.requests.front();
        forged.payload.at("create")["future_real"] = std::numeric_limits<double>::quiet_NaN();
        WriteCounter writes(*f.store, 101);
        rejectedUnchanged(f, forged,
            "nested NaN in forged future REAL creation cell is refused instead of predicting a SQLite NULL");
        check(writes.targetWrites == 0 && !f.maybeBlob(),
              "nonfinite captured SQL cell refuses before a notes row is written");
        auto coerced = prepared.requests.front();
        coerced.payload.at("create")["future_real"] = 1;
        writes.reset();
        rejectedUnchanged(f, coerced,
            "forged INTEGER in future REAL column cannot commit a different SQL storage class");
        check(!f.maybeBlob() && OpJournal(*f.store).count() == 0,
              "REAL-affinity coercion mismatch leaves neither notes row nor history");
        f.store->db().exec("CREATE TRIGGER change_created_notes AFTER INSERT ON event_streams "
                         "BEGIN UPDATE event_streams SET future_real=2.0 WHERE id=NEW.id; END");
        writes.reset();
        rejectedUnchanged(f, prepared.requests.front(),
            "after-capture trigger changing predicted notes row rolls back the complete action");
        check(writes.targetWrites >= 2 && !f.maybeBlob() && OpJournal(*f.store).count() == 0,
              "post-write row guard observes insert and trigger update before rolling both back");
    }
    {
        Fixture f("trigger_extra_expression");
        notes::Diff diff; diff.added.push_back(rawRecord(native(1)));
        const auto prepared = notes::prepareDiff(notes::captureNotes(*f.store, 1, 101), diff);
        require(prepared.ok && prepared.requests.size() == 1, "extra-expression trigger preparation");
        f.store->db().exec("CREATE TRIGGER add_unpredicted_expression AFTER INSERT ON event_streams "
                         "WHEN NEW.stream_kind='notes' AND NEW.channel IS NULL AND NEW.controller IS NULL "
                         "BEGIN INSERT INTO note_expression(clip_id,note_id,dimension,data) "
                         "VALUES(NEW.clip_id,1,64,X'0102'); END");
        WriteCounter writes(*f.store, 101);
        rejectedUnchanged(f, prepared.requests.front(),
            "trigger-created extra expression row rolls back even when the canonical stream matches prediction");
        check(writes.targetWrites == 1 && !f.maybeBlob() && f.expressionCount(1, 1) == 0 &&
                  OpJournal(*f.store).count() == 0 && !History(*f.store).head(),
              "whole-clip expression postcondition rolls back the unchanged stream insertion and unexpected expression");
    }
    {
        Fixture f("trigger_replaced_canonical");
        notes::Diff diff; diff.added.push_back(rawRecord(native(1)));
        const auto prepared = notes::prepareDiff(notes::captureNotes(*f.store, 1, 101), diff);
        require(prepared.ok && notes::commitDiff(*f.store, prepared).ok, "canonical replacement undo setup");
        const auto before = digestProject(*f.store).text;
        const auto head = History(*f.store).head();
        const auto count = OpJournal(*f.store).count();
        f.store->db().exec("CREATE TRIGGER replace_deleted_canonical AFTER DELETE ON event_streams "
                         "WHEN OLD.id=101 BEGIN INSERT INTO event_streams"
                         "(id,clip_id,stream_kind,channel,controller,data) "
                         "VALUES(202,OLD.clip_id,OLD.stream_kind,OLD.channel,OLD.controller,OLD.data); END");
        WriteCounter writes(*f.store, 101);
        const auto undone = History(*f.store).undo();
        check(!undone.ok && writes.targetDeletes == 1 && writes.allWrites >= 2 &&
                  digestProject(*f.store).text == before && History(*f.store).head() == head &&
                  OpJournal(*f.store).count() == count,
              "undo rejects a trigger replacement under a new canonical row ID and rolls back rows and head");
        check(f.store->db().execAndGet("SELECT count(*) FROM event_streams WHERE id=101").getInt64() == 1 &&
                  f.store->db().execAndGet("SELECT count(*) FROM event_streams WHERE id=202").getInt64() == 0,
              "canonical absence postcondition restores original row and removes trigger replacement on rollback");
    }
    {
        Fixture f("redo_future_metadata");
        f.seed(1, envelope({rawRecord(native(1))}));
        f.store->db().exec("ALTER TABLE event_streams ADD COLUMN future_metadata TEXT");
        f.store->db().exec("UPDATE event_streams SET future_metadata='X'");
        const auto before = digestProject(*f.store).text;
        const auto prepared = changedVelocity(f);
        require(prepared.ok && notes::commitDiff(*f.store, prepared).ok, "new diff redo metadata setup");
        const auto after = digestProject(*f.store).text;
        require(History(*f.store).undo().ok && digestProject(*f.store).text == before,
                "new diff undo before metadata-only change");
        const auto originalBytes = f.blob();
        const auto head = History(*f.store).head();
        const auto count = OpJournal(*f.store).count();
        f.store->db().exec("UPDATE event_streams SET future_metadata='Y'");
        require(f.blob() == originalBytes && History(*f.store).head() == head &&
                    f.store->db().execAndGet("SELECT id FROM event_streams").getInt64() == 101,
                "redo stale fixture changes only future metadata, not stream bytes, ID or history head");
        const auto altered = digestProject(*f.store).text;
        const auto redo = History(*f.store).redo();
        check(!redo.ok && digestProject(*f.store).text == altered &&
                  History(*f.store).head() == head && OpJournal(*f.store).count() == count,
              "new applyDiff redo refuses metadata-only divergence from its recorded capture atomically");
        f.store->db().exec("UPDATE event_streams SET future_metadata='X'");
        check(History(*f.store).redo().ok && digestProject(*f.store).text == after,
              "restoring recorded future metadata makes the same redo succeed exactly");
        check(History(*f.store).undo().ok && digestProject(*f.store).text == before,
              "redo metadata refusal leaves a usable exact undo after correction");
    }
    {
        Fixture f("trigger_redundant_note_write");
        notes::Diff diff; diff.added.push_back(rawRecord(native(1)));
        const auto prepared = notes::prepareDiff(notes::captureNotes(*f.store, 1, 101), diff);
        require(prepared.ok && prepared.requests.size() == 1, "redundant stream-write trigger preparation");
        f.store->db().exec("CREATE TRIGGER rewrite_identical_notes AFTER INSERT ON event_streams "
                         "BEGIN UPDATE event_streams SET data=NEW.data WHERE id=NEW.id; END");
        WriteCounter writes(*f.store, 101);
        rejectedUnchanged(f, prepared.requests.front(),
            "redundant trigger UPDATE violates one-stream-write contract even with an identical after-image");
        check(writes.targetWrites == 2 && !f.maybeBlob() && OpJournal(*f.store).count() == 0 &&
                  !History(*f.store).head(),
              "observed insert plus same-byte update both roll back without a notes row or history");
    }
}

struct Random {
    std::uint64_t state = 0x901f7e3ab25d48c6ULL;
    std::uint64_t next() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
};
struct Model {
    std::vector<Record> records;
    std::uint16_t stride = 40;
    bool sorted = true;
    std::uint16_t version = 1;
    std::vector<std::byte> trailer;
    std::vector<std::byte> bytes() const { return envelope(records, stride, sorted, version, trailer); }
};
void seededActualCommits() {
    Fixture f("seeded");
    std::array<Model, 3> models;
    models[1].stride = 48; models[1].version = 9; models[1].trailer = {std::byte{0x7f}, std::byte{0x00}};
    models[2].sorted = false;
    for (std::size_t c = 0; c < models.size(); ++c) {
        auto& model = models[c];
        for (std::uint64_t i = 1; i <= 8; ++i)
            model.records.push_back(rawRecord(native(i, static_cast<std::int64_t>(i * 23u)), model.stride));
        if (!model.sorted) std::reverse(model.records.begin(), model.records.end());
        const auto clip = static_cast<std::int64_t>(c + 1);
        f.seed(clip, model.bytes(), 101 + static_cast<std::int64_t>(c));
        for (const auto id : {std::uint64_t{1}, std::uint64_t{4}, std::uint64_t{7}}) f.expression(clip, id, 64);
    }
    f.store->db().exec("ALTER TABLE event_streams ADD COLUMN future TEXT DEFAULT 'kept'");
    f.store->db().exec("ALTER TABLE note_expression ADD COLUMN future BLOB");
    f.store->db().exec("UPDATE note_expression SET future=X'00FF33'");
    const auto before = digestProject(*f.store).text;
    std::vector<std::string> afterStates;
    Random random;
    std::uint64_t nextId = 1000;
    int successful = 0;
    for (int step = 0; step < 72; ++step) {
        const auto c = static_cast<std::size_t>(step % 3);
        auto& model = models[c];
        const auto clip = static_cast<std::int64_t>(c + 1);
        const auto index = static_cast<std::size_t>(random.next() % model.records.size());
        const auto old = model.records[index];
        notes::Diff diff;
        const int kind = (step / 3) % 6;
        bool reorder = false;
        if (kind == 0) {
            const auto start = static_cast<std::int64_t>(random.next() % 400u) - 50;
            const auto added = rawRecord(native(nextId++, start, 3), model.stride);
            diff.added.push_back(added); model.records.push_back(added); reorder = true;
        } else if (kind == 1) {
            diff.removed.push_back(old);
            model.records.erase(model.records.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            const auto shifted = static_cast<std::int64_t>(random.next() % 43u) + 1;
            const auto changed = changeRecord(old, [kind, shifted](NoteRecord& n) {
                if (kind == 2) n.start_ticks += shifted;
                if (kind == 3) n.dur_ticks += 2;
                if (kind == 4) n.vel_on = n.vel_on == 90 ? 91 : 90;
                if (kind == 5) n.flags ^= NoteFlags::Muted;
            });
            diff.changed.push_back({old, changed}); model.records[index] = changed; reorder = kind == 2;
        }
        // Reference ordering uses only semantic start values and stable input order.
        if (model.sorted && reorder)
            std::stable_sort(model.records.begin(), model.records.end(), [](const Record& a, const Record& b) {
                return readRecord(a).start_ticks < readRecord(b).start_ticks;
            });
        const auto snapshot = notes::captureNotes(*f.store, clip);
        const auto plan = notes::prepareDiff(snapshot, diff);
        require(plan.ok && plan.requests.size() == 1,
                "seeded actual nonempty prepare step " + std::to_string(step) + ": " + plan.error);
        const auto result = notes::commitDiff(*f.store, plan);
        check(result.ok, "seeded actual commit step " + std::to_string(step) + ": " + result.error);
        if (!result.ok) return;
        ++successful;
        bool same = true;
        for (std::size_t i = 0; i < models.size(); ++i)
            same = same && f.blob(static_cast<std::int64_t>(i + 1)) == models[i].bytes();
        check(same, "independent three-clip byte model after actual step " + std::to_string(step));
        if (!diff.removed.empty())
            check(f.expressionCount(clip, readRecord(old).note_id) == 0,
                  "seeded removed note has no surviving expression rows");
        afterStates.push_back(digestProject(*f.store).text);
    }
    check(successful == 72 && OpJournal(*f.store).count() == 72,
          "seeded gate contains 72 successful nonempty commits, not refused/no-op trials");
    for (int step = 0; step < successful; ++step)
        require(History(*f.store).undo().ok, "seeded undo step " + std::to_string(step));
    check(digestProject(*f.store).text == before && !History(*f.store).canUndo(),
          "seeded undo-all restores exact bytes, row identities, expression rows and unknown columns");
    f.reopen();
    for (std::size_t step = 0; step < afterStates.size(); ++step) {
        const auto result = History(*f.store).redo();
        check(result.ok && digestProject(*f.store).text == afterStates[step],
              "seeded reopen/redo matches saved independent state step " + std::to_string(step));
        if (!result.ok) return;
    }
}

void lateFailures() {
    const std::array triggers = {
        "CREATE TRIGGER fail_notes BEFORE UPDATE ON event_streams BEGIN SELECT RAISE(ABORT,'note write failure'); END",
        "CREATE TRIGGER fail_expression BEFORE DELETE ON note_expression BEGIN SELECT RAISE(ABORT,'expression delete failure'); END",
        "CREATE TRIGGER fail_log BEFORE INSERT ON ops BEGIN SELECT RAISE(ABORT,'late journal failure'); END"};
    for (std::size_t i = 0; i < triggers.size(); ++i) {
        Fixture f("late_failure"); const auto a = rawRecord(native(maximumId));
        f.seed(1, envelope({a, rawRecord(native(2, 20))})); f.expression(1, maximumId, 64);
        const auto snapshot = notes::captureNotes(*f.store, 1);
        notes::Diff diff; diff.removed.push_back(a);
        const auto prepared = notes::prepareDiff(snapshot, diff);
        require(prepared.ok && prepared.requests.size() == 1, "late-failure preparation");
        f.store->db().exec(triggers[i]);
        rejectedUnchanged(f, prepared.requests.front(), "late SQL failure rolls back notes/expressions/history case " + std::to_string(i));
    }
    Fixture undo("undo_late_failure"); const auto a = rawRecord(native(maximumId));
    undo.seed(1, envelope({a})); undo.expression(1, maximumId, 64);
    const auto before = digestProject(*undo.store).text;
    notes::Diff diff; diff.removed.push_back(a);
    const auto plan = notes::prepareDiff(notes::captureNotes(*undo.store, 1), diff);
    require(plan.ok && notes::commitDiff(*undo.store, plan).ok, "prepare expression undo failure");
    const auto after = digestProject(*undo.store).text;
    const auto head = History(*undo.store).head();
    undo.store->db().exec("CREATE TRIGGER fail_restore BEFORE INSERT ON note_expression BEGIN SELECT RAISE(ABORT,'restore failure'); END");
    check(!History(*undo.store).undo().ok && digestProject(*undo.store).text == after && History(*undo.store).head() == head,
          "late expression inverse failure restores neither partial notes nor moved history head");
    undo.store->db().exec("DROP TRIGGER fail_restore");
    check(History(*undo.store).undo().ok && digestProject(*undo.store).text == before,
          "same inverse succeeds exactly after failure cause is removed");
    Fixture batch("batch_atomic"); batch.seed(1, envelope({rawRecord(native(1))}));
    const auto prepared = changedVelocity(batch); require(prepared.ok && prepared.requests.size() == 1, "batch prepare");
    const auto batchBefore = digestProject(*batch.store).text;
    auto invalid = prepared.requests.front(); invalid.payload["from"] = std::string(64, '0');
    const std::array requests{prepared.requests.front(), invalid};
    check(!OpJournal(*batch.store).commit(requests).ok && digestProject(*batch.store).text == batchBefore &&
              OpJournal(*batch.store).count() == 0,
          "failed second request rolls back successful first diff and both log rows");
    batch.reopen(true);
    rejectedUnchanged(batch, prepared.requests.front(), "read-only project refuses a valid prepared diff");
}
} // namespace
int main() {
    const std::array<std::pair<const char*, void (*)()>, 10> cases = {{
        {"immutable and pure", immutableAndPure}, {"10,000 notes", tenThousandOneWrite},
        {"future and expressions", futureAndExpressions}, {"creation", creationAndSignedRows},
        {"numeric and malformed", numericAndMalformed}, {"stale guards", staleGuards},
        {"coherent capture", coherentOwnerCapture}, {"SQL storage classes", sqlStorageClassGuards},
        {"seeded commits", seededActualCommits},
        {"late failures", lateFailures}}};
    for (const auto& [name, run] : cases) {
        try { run(); }
        catch (const std::exception& error) {
            check(false, std::string(name) + " unexpected exception: " + error.what());
        }
    }
    std::printf("note diff: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
