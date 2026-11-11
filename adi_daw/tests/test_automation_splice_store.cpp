// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/automation_splice_store.hpp"
#include "adi/digest.hpp"
#include "adi/edit_time.hpp"
#include "adi/engine/automation.hpp"
#include "adi/history.hpp"
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::automation;
int checks = 0, failures = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}
std::unique_ptr<Store> project(const std::filesystem::path &path, int base = 0) {
    StoreError error = StoreError::Ok;
    auto store = Store::createEmpty(path, error);
    if (!store)
        throw std::runtime_error("create splice project");
    store->db().exec(
        "INSERT INTO project(id,name) VALUES(1,'Splice');"
        "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Track');"
        "INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) "
        "VALUES(1,1,'audio',0,0,960);"
        "INSERT INTO "
        "automation_lanes(id,owner_kind,owner_id,param_ref,default_value,min_value,max_value) "
        "VALUES(2,'track',1,'volume',0.25,0.0,1.0)");
    store->db().exec("UPDATE automation_lanes SET time_base=" + std::to_string(base));
    return store;
}
NativeSpliceKnot point(std::int64_t time, double value, std::uint8_t curve = 1,
                       std::uint64_t id = 0) {
    NativeSpliceKnot result;
    result.point.time = time;
    result.point.value = value;
    result.point.curve = curve;
    result.point.tension = 0.6f;
    result.point.point_id = id;
    return result;
}
void stream(Store &store, std::span<const NativeSpliceKnot> points, int base = 0,
            std::int64_t id = 4, bool clip = false, bool trailer = false) {
    std::vector<AutomationPoint> raw;
    for (const auto &knot : points)
        raw.push_back(knot.point);
    auto bytes = writeStream<AutomationPoint>(FourCC::Automation, raw, 1,
                                              StreamFlags::SortedByTime |
                                                  (base ? StreamFlags::TimeIsNanos : 0));
    if (trailer) {
        bytes.push_back(std::byte{0xaa});
        bytes.push_back(std::byte{0});
    }
    SQLite::Statement insert(
        store.db(), "INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(?,2,?,?)");
    insert.bind(1, id);
    if (clip)
        insert.bind(2, 1);
    else
        insert.bind(2);
    insert.bind(3, bytes.data(), static_cast<int>(bytes.size()));
    insert.exec();
}
Payload rows(Store &store) {
    Payload result = Payload::array();
    for (const char *table :
         {"automation_lanes", "automation_data", "automation_segment_ranges", "tempo_map"})
        result.push_back(detail::captureGuardRows(store, table, Payload::object()));
    return result;
}
std::vector<SpliceKnot> decoded(Store &store, int base) {
    const auto row = detail::captureRow(store.db(), "automation_data", "id", 4);
    if (!row)
        throw std::runtime_error("splice data missing");
    const auto &binary = row->at("data").get_binary();
    StreamReader<AutomationPoint> reader(
        {reinterpret_cast<const std::byte *>(binary.data()), binary.size()}, FourCC::Automation);
    if (!reader.ok())
        throw std::runtime_error("bad stored stream");
    const edit::TimeMap time(store.db());
    std::vector<SpliceKnot> result;
    for (std::uint32_t i = 0; i < reader.count(); ++i) {
        const auto knot = *reader.at(i);
        result.push_back(
            {knot,
             static_cast<std::int64_t>(std::llround(
                 (base ? static_cast<double>(knot.time) * 1e-9 : time.seconds(knot.time)) * 48000)),
             {}});
    }
    SQLite::Statement ranges(
        store.db(), "SELECT record_index,progress_begin,progress_end,source_first,source_last FROM "
                    "automation_segment_ranges WHERE data_id=4 ORDER BY record_index");
    while (ranges.executeStep()) {
        const auto index = ranges.getColumn(0).getInt64();
        if (index < 0 || static_cast<std::uint64_t>(index) >= result.size())
            throw std::runtime_error("bad range index");
        result.at(static_cast<std::size_t>(index)).slice =
            engine::CurveSlice{ranges.getColumn(1).getDouble(), ranges.getColumn(2).getDouble(),
                               ranges.getColumn(3).getDouble(), ranges.getColumn(4).getDouble()};
    }
    return result;
}
engine::AutomationLaneProgram program(std::span<const SpliceKnot> knots) {
    engine::AutomationLaneProgram result;
    result.defaultValue = 0.25;
    for (const auto &knot : knots)
        result.points.push_back({knot.sample, knot.point.value, knot.point.tension,
                                 knot.point.curve, knot.slice ? &*knot.slice : nullptr});
    return result;
}
void curves(const std::filesystem::path &directory) {
    for (int base = 0; base < 2; ++base)
        for (std::uint8_t curve = 0; curve <= 5; ++curve) {
            auto store = project(
                directory / ("curve" + std::to_string(base) + "-" + std::to_string(curve) + ".adi"),
                base);
            auto &db = store->db();
            db.exec("ALTER TABLE automation_data ADD COLUMN future_blob BLOB NOT NULL DEFAULT "
                    "X'0001ff';"
                    "ALTER TABLE automation_segment_ranges ADD COLUMN future_text TEXT NOT NULL "
                    "DEFAULT 'retained';"
                    "ALTER TABLE automation_segment_ranges ADD COLUMN future_real REAL NOT NULL "
                    "DEFAULT 1.25");
            const std::int64_t unit = base ? 1000000 : 10000;
            if (!base)
                db.exec("INSERT INTO tempo_map(id,pos_ticks,bpm,curve) "
                        "VALUES(20,1000000,90,0),(21,2000000,150,0)");
            auto old = std::vector{point(0, 0.1, curve, UINT64_MAX), point(300 * unit, 0.9, 1, 0)};
            stream(*store, std::vector{point(0, 0.3)}, base, 8, true);
            // Use the real retained-domain endpoints, rather than an inconsistent fixture.
            old.at(0).point.value = engine::curveValue(0, 1, 0.1, curve, 0.6f);
            old.at(1).point.value = engine::curveValue(0, 1, 0.9, curve, 0.6f);
            stream(*store, old, base, 4, false, true);
            db.exec("INSERT INTO "
                    "automation_segment_ranges(id,data_id,record_index,progress_begin,progress_end,"
                    "source_first,source_last) VALUES(11,4,0,0.1,0.9,0.0,1.0)");
            db.exec("UPDATE automation_data SET future_blob=X'abc000' WHERE id=4;"
                    "UPDATE automation_segment_ranges SET future_text='native old "
                    "row',future_real=2.125 WHERE id=11");
            const auto before = rows(*store);
            const auto digest = digestProject(*store).text;
            const auto original = decoded(*store, base);
            std::vector<NativeSpliceKnot> incoming{point(75 * unit, 0.7, 4, UINT64_MAX),
                                                   point(225 * unit, 0.2, 1, 0)};
            const auto prepared =
                prepareEnvelopeSplice(*store, 2, 75 * unit, 225 * unit, incoming, 48000);
            if (!prepared.ok)
                std::printf("prepare diagnostic %s\n", prepared.error.c_str());
            check(prepared.ok && prepared.requests.size() == 1 &&
                      prepared.requests.front().capturedTransaction,
                  "prepare one sealed envelope splice");
            check(detail::sameSqlValue(before, rows(*store)) &&
                      digestProject(*store).text == digest,
                  "preparation leaves all project and journal material untouched");
            const auto commit = OpJournal(*store).commit(prepared.requests);
            if (!commit.ok)
                std::printf("commit diagnostic %s\n", commit.error.c_str());
            check(commit.ok && commit.seqs.size() == 1,
                  "ordinary journal commits one atomic splice");
            const auto after = rows(*store);
            const auto output = decoded(*store, base);
            std::vector<SpliceKnot> replacement;
            const edit::TimeMap clock(db);
            for (const auto &knot : incoming)
                replacement.push_back({knot.point,
                                       static_cast<std::int64_t>(std::llround(
                                           (base ? static_cast<double>(knot.point.time) * 1e-9
                                                 : clock.seconds(knot.point.time)) *
                                           48000)),
                                       knot.slice});
            const auto a = program(original), b = program(output), c = program(replacement);
            bool rendered = true;
            if (replacement.size() == 2 && !original.empty()) {
                const auto begin = replacement.front().sample, end = replacement.back().sample;
                for (std::int64_t sample = 0; sample < original.back().sample + 100; ++sample) {
                    const auto expected = sample >= begin && sample < end
                                              ? engine::valueAt(c, sample)
                                              : engine::valueAt(a, sample);
                    rendered = rendered && std::abs(engine::valueAt(b, sample) - expected) < 3e-12;
                }
            } else
                rendered = false;
            check(rendered, "persisted output renders old curve outside and replacement inside at "
                            "every sample");
            bool ids = output.size() == 6;
            std::set<std::uint64_t> fresh;
            for (const auto &knot : output)
                if (knot.point.time != 0 && knot.point.time != 300 * unit)
                    ids = ids && knot.point.point_id != 0 && knot.point.point_id != UINT64_MAX &&
                          fresh.insert(knot.point.point_id).second;
            check(ids && output.front().point.point_id == UINT64_MAX &&
                      output.back().point.point_id == 0 && fresh.size() == 4,
                  "outside full-width and zero IDs survive; guards and colliding incoming IDs "
                  "become fresh");
            check(
                db.execAndGet("SELECT count(*) FROM automation_segment_ranges WHERE data_id=4 AND "
                              "future_text='native old row' AND typeof(future_real)='real' AND "
                              "future_real=2.125")
                            .getInt() == 2 &&
                    db.execAndGet("SELECT record_index FROM automation_segment_ranges WHERE id=11")
                            .getInt() == 0,
                "split original range keeps one identity and complete typed extension columns on "
                "both pieces");
            const auto current = detail::captureRow(db, "automation_data", "id", 4);
            const auto &bytes = current->at("data").get_binary();
            check(bytes.size() >= 2 && bytes[bytes.size() - 2] == 0xaa && bytes.back() == 0 &&
                      current->at("future_blob") ==
                          Payload::binary(std::vector<std::uint8_t>{0xab, 0xc0, 0}),
                  "header trailer and opaque data columns survive serialization");
            check(detail::sameSqlValue(before.at(1).at(1), after.at(1).at(1)),
                  "clip-scoped stream remains exact");
            History history(*store);
            check(history.undo().ok && detail::sameSqlValue(rows(*store), before) &&
                      digestProject(*store).text == digest,
                  "one Undo restores exact typed rows stream trailer and original range identity");
            check(history.redo().ok && detail::sameSqlValue(rows(*store), after),
                  "Redo restores exact authored point and range IDs");
            check(history.undo().ok && detail::sameSqlValue(rows(*store), before),
                  "second Undo is exact");
            check(!OpJournal(*store).commit(prepared.requests).ok,
                  "durable packet cannot execute a second time");
        }
}
void guards(const std::filesystem::path &directory) {
    const std::vector<std::string> mutations{
        "UPDATE automation_lanes SET default_value=0.3 WHERE id=2",
        "UPDATE automation_lanes SET time_base=1 WHERE id=2",
        "UPDATE automation_lanes SET owner_id=7 WHERE id=2",
        "UPDATE automation_data SET data=CAST(data || X'00' AS BLOB) WHERE id=4",
        "INSERT INTO automation_data(id,lane_id,clip_id,data) SELECT 9,2,1,data FROM "
        "automation_data WHERE id=4",
        "UPDATE automation_segment_ranges SET source_last=0.8 WHERE id=11",
        "DELETE FROM automation_segment_ranges WHERE id=11",
        "INSERT INTO "
        "automation_segment_ranges(id,data_id,record_index,progress_begin,progress_end,source_"
        "first,source_last) VALUES(12,4,1,0,1,0,1)",
        "INSERT INTO tempo_map(id,pos_ticks,bpm,curve) VALUES(40,50,90,0)"};
    std::size_t index = 0;
    for (const auto &mutation : mutations) {
        auto store = project(directory / ("guard" + std::to_string(index++) + ".adi"));
        stream(*store, std::vector{point(0, 0.1, 3, 9), point(300, 0.9, 1, 10)});
        store->db().exec("INSERT INTO "
                         "automation_segment_ranges(id,data_id,record_index,progress_begin,"
                         "progress_end,source_first,source_last) VALUES(11,4,0,0,1,0.1,0.9)");
        const auto prepared = prepareEnvelopeSplice(
            *store, 2, 75, 225, std::vector{point(75, 0.7), point(225, 0.3)}, 48000);
        check(prepared.ok, "prepare exact read-set race witness");
        store->db().exec(mutation);
        const auto before = rows(*store);
        const auto head = OpJournal(*store).headSeq();
        const auto result = OpJournal(*store).commit(prepared.requests);
        check(!result.ok && result.stale && !result.error.empty() &&
                  OpJournal(*store).headSeq() == head && detail::sameSqlValue(rows(*store), before),
              "same-head external row or set change refuses without prefix or journal write");
    }
}
void failuresAndAbsence(const std::filesystem::path &directory) {
    auto store = project(directory / "absence.adi");
    const std::vector incoming{point(75, 0.7, 4, 1), point(225, 0.2, 1, 2)};
    const auto before = rows(*store);
    auto prepare = [&]() { return prepareEnvelopeSplice(*store, 2, 75, 225, incoming, 48000); };
    auto prepared = prepare();
    check(prepared.ok, "default-only lane prepares global stream creation");
    auto other = project(directory / "other.adi");
    check(!OpJournal(*other).commit(prepared.requests).ok &&
              detail::sameSqlValue(rows(*other), before),
          "same-shaped other live connection refuses captured packet");
    check(OpJournal(*store).commit(prepared.requests).ok,
          "wrong-source offer does not poison authentic group");
    const auto after = rows(*store);
    check(store->db()
                  .execAndGet(
                      "SELECT count(*) FROM automation_data WHERE lane_id=2 AND clip_id IS NULL")
                  .getInt() == 1,
          "new envelope has one global data row");
    check(History(*store).undo().ok && detail::sameSqlValue(rows(*store), before),
          "new data and generated ranges Undo to absence");
    check(History(*store).redo().ok && detail::sameSqlValue(rows(*store), after),
          "new stream Redo retains allocated identities");
    prepared = prepare();
    auto changed = prepared.requests;
    if (!changed.empty())
        changed.front().payload["id"] = 123;
    check(!OpJournal(*store).commit(changed).ok, "tampered packet refuses");
    check(OpJournal(*store).commit(prepared.requests).ok,
          "tamper refusal does not poison exact authentic packet");
    prepared = prepare();
    store->db().exec("CREATE TEMP TRIGGER deny_splice BEFORE UPDATE ON automation_data BEGIN "
                     "SELECT RAISE(ABORT,'splice rollback'); END");
    const auto rollback = rows(*store);
    const auto head = OpJournal(*store).headSeq();
    check(!OpJournal(*store).commit(prepared.requests).ok &&
              detail::sameSqlValue(rows(*store), rollback) && OpJournal(*store).headSeq() == head,
          "real SQL failure rolls all point range and journal changes back");
    store->db().exec("DROP TRIGGER deny_splice");
    check(!OpJournal(*store).commit(prepared.requests).ok,
          "failed durable submission needs fresh preparation");
    check(OpJournal(*store).commit(prepare().requests).ok,
          "fresh preparation commits after real rollback");
    auto empty = project(directory / "reserved.adi");
    const auto reserved = prepareEnvelopeSplice(*empty, 2, 75, 225, incoming, 48000);
    stream(*empty, std::vector{point(0, 0.4)}, 0, 1, true);
    const auto occupied = rows(*empty);
    check(!OpJournal(*empty).commit(reserved.requests).ok &&
              detail::sameSqlValue(rows(*empty), occupied),
          "new data ID reuse refuses including absent global set");
    const auto malformed = [&](std::vector<NativeSpliceKnot> input, std::int64_t begin = 75,
                               std::int64_t end = 225, double rate = 48000,
                               std::size_t budget = kSplicePointBudget) {
        const auto capture = rows(*store);
        const auto result = prepareEnvelopeSplice(*store, 2, begin, end, input, rate, budget);
        check(!result.ok && !result.error.empty() && result.requests.empty() &&
                  detail::sameSqlValue(rows(*store), capture),
              "invalid native input refuses without writes or partial packet");
    };
    auto bad = incoming;
    bad.at(0).point.value = 2;
    malformed(bad);
    bad = incoming;
    bad.at(0).point.flags = 1;
    malformed(bad);
    bad = incoming;
    bad.at(0).point.curve = 6;
    malformed(bad);
    bad = incoming;
    bad.at(0).point.value = std::numeric_limits<double>::quiet_NaN();
    malformed(bad);
    bad = incoming;
    bad.at(1).slice = engine::CurveSlice{0, 1, 0, 1};
    malformed(bad);
    bad = incoming;
    bad.at(0).point.time = 76;
    malformed(bad);
    malformed(incoming, 75, 75);
    malformed(incoming, -1, 225);
    malformed(incoming, 75, 225, 0);
    malformed(incoming, 75, 225, std::numeric_limits<double>::infinity());
    malformed(incoming, 75, 225, 48000, 2);
    malformed(incoming, 75, 225, 48000, (1u << 23) + 1);
    const bool wasAudio = audio::onAudioThread;
    audio::onAudioThread = true;
    const auto audioResult = prepare();
    audio::onAudioThread = wasAudio;
    check(!audioResult.ok && audioResult.requests.empty(),
          "callback thread refuses before touching SQLite");
}
void additionalAdmission(const std::filesystem::path &directory) {
    auto store = project(directory / "additional.adi", 1);
    std::vector incoming{point(75000000, 0.3, 4, UINT64_MAX), point(225000000, 0.6, 1, UINT64_MAX)};
    incoming.at(0).slice = engine::CurveSlice{0.2, 0.7, 0.1, 0.9};
    const auto empty = rows(*store);
    auto prepared = prepareEnvelopeSplice(*store, 2, 75000000, 225000000, incoming, 48000);
    const auto copy = incoming;
    incoming.at(0).point.value = 0.9;
    incoming.at(0).slice.reset();
    check(prepared.ok && OpJournal(*store).commit(prepared.requests).ok,
          "owned incoming domain survives caller mutation and creates new stream plus range "
          "atomically");
    check(store->db()
                  .execAndGet(
                      "SELECT count(*) FROM automation_segment_ranges WHERE progress_begin=0.2 AND "
                      "progress_end=0.7 AND source_first=0.1 AND source_last=0.9")
                  .getInt() == 1,
          "incoming retained domain persists without reinterpretation");
    check(History(*store).undo().ok && detail::sameSqlValue(rows(*store), empty),
          "new range and stream Undo together to absence");
    check(History(*store).redo().ok, "new stream and range Redo together");
    prepared = prepareEnvelopeSplice(*store, 2, 75000000, 225000000, copy, 48000);
    check(OpJournal(*store).commit(automation::setDefaultValue(2, 0.3)).ok,
          "advance actual journal head after capture");
    const auto advanced = rows(*store);
    const auto head = OpJournal(*store).headSeq();
    check(prepared.ok && !OpJournal(*store).commit(prepared.requests).ok &&
              OpJournal(*store).headSeq() == head && detail::sameSqlValue(rows(*store), advanced),
          "stale journal head refuses exact old packet");
    const auto noLane = prepareEnvelopeSplice(*store, 999, 75000000, 225000000, copy, 48000);
    check(!noLane.ok && noLane.requests.empty(), "missing lane refuses");
    auto reserved = project(directory / "range-reserved.adi", 1);
    stream(*reserved, std::vector{point(0, 0.1, 3, 1), point(300000000, 0.9, 1, 2)}, 1);
    stream(*reserved, std::vector{point(0, 0.4), point(300000000, 0.5)}, 1, 8, true);
    reserved->db().exec("INSERT INTO "
                        "automation_segment_ranges(id,data_id,record_index,progress_begin,progress_"
                        "end,source_first,source_last) VALUES(11,4,0,0,1,0.1,0.9)");
    auto packet = prepareEnvelopeSplice(*reserved, 2, 75000000, 225000000, copy, 48000);
    reserved->db().exec("INSERT INTO "
                        "automation_segment_ranges(id,data_id,clip_id,record_index,progress_begin,"
                        "progress_end,source_first,source_last) VALUES(12,8,1,0,0,1,0.4,0.5)");
    const auto occupied = rows(*reserved);
    check(packet.ok && !OpJournal(*reserved).commit(packet.requests).ok &&
              detail::sameSqlValue(rows(*reserved), occupied),
          "new split range ID reused on another stream refuses by captured absence");
    auto unrelated = project(directory / "unrelated.adi", 1);
    stream(*unrelated, std::vector{point(0, 0.1), point(300000000, 0.9)}, 1);
    packet = prepareEnvelopeSplice(*unrelated, 2, 75000000, 225000000, copy, 48000);
    unrelated->db().exec("UPDATE tracks SET name='changed independently' WHERE id=1");
    check(packet.ok && OpJournal(*unrelated).commit(packet.requests).ok,
          "unrelated same-head display change does not falsely stale the lane");
    unrelated->db().exec("UPDATE automation_lanes SET max_value=0.05 WHERE id=2");
    const auto invalid =
        prepareEnvelopeSplice(*unrelated, 2, 75000000, 225000000,
                              std::vector{point(75000000, 0.02), point(225000000, 0.03)}, 48000);
    check(!invalid.ok && invalid.requests.empty(),
          "invalid existing lane values refuse rather than silently repairing outside material");
}
void identities(const std::filesystem::path &directory) {
    auto store = project(directory / "point-identities.adi", 1);
    stream(
        *store,
        std::vector{point(0, 0.1, 3, 1), point(150000000, 0.5, 2, 2), point(300000000, 0.9, 1, 3)},
        1);
    const auto before = rows(*store);
    const std::vector incoming{point(75000000, 0.3, 1, 1), point(225000000, 0.6, 1, 3)};
    const auto packet = prepareEnvelopeSplice(*store, 2, 75000000, 225000000, incoming, 48000);
    check(packet.ok && OpJournal(*store).commit(packet.requests).ok,
          "low-ID and removed-point collision witness commits");
    const auto output = decoded(*store, 1);
    bool distinct = output.size() == 6;
    std::set<std::uint64_t> used;
    for (const auto &knot : output) {
        distinct = distinct && used.insert(knot.point.point_id).second;
        if (knot.point.time > 0 && knot.point.time < 300000000)
            distinct = distinct && knot.point.point_id > 3;
    }
    check(distinct && used.contains(1) && used.contains(3) && !used.contains(2),
          "fresh IDs skip every original ID including removed points and never reuse an outside "
          "identity");
    check(History(*store).undo().ok && detail::sameSqlValue(rows(*store), before),
          "low-ID collision witness Undo restores every literal identity");
}
void layouts(const std::filesystem::path &directory) {
    for (int variant = 0; variant < 6; ++variant) {
        auto store = project(directory / ("layout" + std::to_string(variant) + ".adi"));
        std::vector<AutomationPoint> points{point(0, 0.1).point, point(300, 0.9).point};
        auto bytes =
            writeStream<AutomationPoint>(FourCC::Automation, points, StreamFlags::SortedByTime);
        StreamHeader header{};
        std::memcpy(&header, bytes.data(), sizeof header);
        if (variant == 0)
            header.version = 2;
        if (variant == 1) {
            header.rec_size = 40;
            bytes.insert(bytes.end(), 16, std::byte{0});
        }
        if (variant == 2)
            header.flags |= 0x100;
        if (variant == 3)
            header.flags |= StreamFlags::TimeIsNanos;
        if (variant == 4)
            header.count = 3;
        if (variant == 5)
            header.fourcc = 0;
        std::memcpy(bytes.data(), &header, sizeof header);
        SQLite::Statement q(store->db(),
                            "INSERT INTO automation_data(id,lane_id,data) VALUES(4,2,?)");
        q.bind(1, bytes.data(), static_cast<int>(bytes.size()));
        q.exec();
        const auto before = rows(*store);
        const auto prepared = prepareEnvelopeSplice(
            *store, 2, 75, 225, std::vector{point(75, 0.7), point(225, 0.3)}, 48000);
        check(!prepared.ok && prepared.requests.empty() &&
                  detail::sameSqlValue(rows(*store), before),
              "future malformed or conflicting AAUT layout refuses unchanged");
    }
}
} // namespace
int main() {
    try {
        adi::test::TempDirectory temp("automation", "splice-store");
        curves(temp.path());
        guards(temp.path());
        failuresAndAbsence(temp.path());
        layouts(temp.path());
        additionalAdmission(temp.path());
        identities(temp.path());
    } catch (const std::exception &error) {
        ++failures;
        std::printf("EXCEPTION %s\n", error.what());
    }
    std::printf("automation_splice_store: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
