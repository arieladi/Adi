// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/blob.hpp"
#include "adi/history.hpp"
#include "adi/note_transforms.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace {
using namespace adi;
int checks = 0, failures = 0;
constexpr std::uint64_t wideId = (std::uint64_t(1) << 63) + 17;
void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
NoteRecord note(std::uint64_t id, std::int64_t start, std::int64_t duration, int key = 60) {
    NoteRecord result{};
    result.note_id = id;
    result.start_ticks = start;
    result.dur_ticks = duration;
    result.key = static_cast<std::uint8_t>(key);
    result.vel_on = 93;
    result.vel_off = 45;
    result.channel = 7;
    result.probability = 4321;
    result.tuning_cents = 17.25f;
    result.flags = NoteFlags::HasExpression;
    return result;
}
struct Project {
    test::TempDirectory folder{"note_transforms", "project"};
    std::unique_ptr<Store> store;
    explicit Project(const std::vector<NoteRecord> &notes = {note(1, 75, 60), note(wideId, 75, 20),
                                                             note(3, 176, 25, 62)},
                     bool future = false) {
        StoreError error{};
        store = Store::create(folder.path() / "song.adi", error);
        if (!store)
            throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind) VALUES(1,'midi'); INSERT INTO "
                         "clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(1,1,'midi',0,1000)");
        auto bytes = writeStream<NoteRecord>(FourCC::Notes, notes);
        if (future) {
            StreamHeader header{};
            std::memcpy(&header, bytes.data(), sizeof header);
            header.version = 7;
            header.rec_size = 44;
            std::vector<std::byte> extended(sizeof header + notes.size() * 44 + 3, std::byte{0x5a});
            std::memcpy(extended.data(), &header, sizeof header);
            for (std::size_t i = 0; i < notes.size(); ++i) {
                std::memcpy(extended.data() + sizeof header + i * 44, &notes[i], sizeof(NoteRecord));
                extended[sizeof header + i * 44 + 40] = static_cast<std::byte>(i + 1);
            }
            bytes = std::move(extended);
        }
        if (!store->putEventStream(1, "notes", bytes))
            throw std::runtime_error("notes fixture");
    }
    std::vector<std::byte> blob() {
        const auto result = store->getEventStream(1, "notes");
        if (!result)
            throw std::runtime_error("missing notes");
        return *result;
    }
    NoteRecord get(std::uint64_t id) {
        const auto bytes = blob();
        StreamReader<NoteRecord> reader(bytes, FourCC::Notes);
        for (std::uint32_t i = 0; i < reader.count(); ++i)
            if (reader.at(i)->note_id == id)
                return *reader.at(i);
        throw std::runtime_error("missing note");
    }
    CommitResult quant(std::vector<std::uint64_t> ids, notes::QuantizeSettings settings = {100}) {
        return OpJournal(*store).commit(notes::quantizeRequest(1, ids, settings));
    }
};
void basic() {
    auto unselected = note(wideId, 75, 20);
    unselected.flags |= NoteFlags::Selected;
    Project p({note(1, 75, 60), unselected, note(3, 176, 25, 62)}, true);
    ExpressionPoint point{};
    point.time_ticks = 5;
    point.value = 0.25f;
    const std::vector<ExpressionPoint> points{point};
    const auto expression = writeStream<ExpressionPoint>(FourCC::Expression, points);
    if (!p.store->putNoteExpression(1, 1, 0, expression))
        throw std::runtime_error("expression fixture");
    const auto before = p.blob();
    check(p.quant({1, 3}).ok, "quantize native op commits explicit IDs");
    check(p.get(1).start_ticks == 100 && p.get(1).dur_ticks == 60 && p.get(3).start_ticks == 200,
          "start quantization snaps selected notes and keeps durations");
    check(p.get(wideId).start_ticks == 75 && p.get(wideId).vel_on == 93 && p.get(1).tuning_cents == 17.25f,
          "unselected notes, velocity, channel and tuning are unchanged");
    check(p.store->getNoteExpression(1, 1, 0) == std::optional<std::vector<std::byte>>(expression),
          "quantize retains per-note expression bytes and stable identities");
    const auto logged = OpJournal(*p.store).recent(1);
    check(!logged.empty() && logged.front().inverse &&
              logged.front().inverse->at("stream").at("bytes").is_binary(),
          "captured inverse uses compact CBOR binary rather than one JSON value per byte");
    const auto after = p.blob();
    StreamHeader header{};
    std::memcpy(&header, after.data(), sizeof header);
    check(header.version == 7 && header.rec_size == 44 && after.size() == before.size() &&
              after.back() == std::byte{0x5a},
          "future record stride, version and stream trailer survive edits");
    check(after[sizeof header + 40] == std::byte{2} && after[sizeof header + 44 + 40] == std::byte{1},
          "record tails move with their note during stable time sorting");
    History history(*p.store);
    check(history.undo().ok && p.blob() == before,
          "quantize undo restores exact original bytes, including original coincident-note ordering");
    check(history.redo().ok && p.blob() == after, "quantize redo reproduces the edited stream exactly");
    check(OpJournal(*p.store).count() == 1, "quantize is one undo step rather than one step per note");
}
void modes() {
    Project end({note(1, 45, 60)});
    notes::QuantizeSettings options{100};
    options.starts = false;
    options.ends = true;
    check(end.quant({1}, options).ok && end.get(1).start_ticks == 45 && end.get(1).dur_ticks == 55,
          "end-only quantize preserves the note start");
    Project both({note(1, 45, 60)});
    options.starts = true;
    check(both.quant({1}, options).ok && both.get(1).start_ticks == 0 && both.get(1).dur_ticks == 100,
          "start and end snap independently");
    Project collapse({note(1, 55, 2)});
    check(collapse.quant({1}, options).ok && collapse.get(1).start_ticks == 100 &&
              collapse.get(1).dur_ticks == 1,
          "collapsed endpoints retain a positive one-tick note");
    Project amount({note(1, 45, 60)});
    options.ends = false;
    options.strength = 5000;
    check(amount.quant({1}, options).ok && amount.get(1).start_ticks == 22,
          "fractional strength rounds exact integer movement toward the target");
    Project swing({note(1, 135, 10)});
    options.strength = 10000;
    options.swing = 10000;
    check(swing.quant({1}, options).ok && swing.get(1).start_ticks == 150,
          "swing shifts the odd grid line by half a division");
    Project negative({note(1, -150, 20)});
    options.swing = 0;
    check(negative.quant({1}, options).ok && negative.get(1).start_ticks == -100,
          "negative half-grid positions prefer the later grid line");
    Project origin({note(1, 75, 20)});
    options.originTicks = 25;
    check(origin.quant({1}, options).ok && origin.get(1).start_ticks == 125,
          "explicit grid origin controls phase without a playhead read");
    Project large({note(1, 9007199254740993LL, 17)});
    options.gridTicks = 10;
    options.originTicks = 0;
    check(large.quant({1}, options).ok && large.get(1).start_ticks == 9007199254740990LL,
          "quantize keeps int64 tick precision above the double integer range");
}
void extremeAndMalformed() {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    Project wide({note(1, maximum - 1000, 20)});
    notes::QuantizeSettings options{100};
    options.originTicks = std::numeric_limits<std::int64_t>::min();
    check(wide.quant({1}, options).ok && wide.get(1).start_ticks == maximum - 1015,
          "grid phasing remains exact when start minus origin would overflow int64");
    Project hugeGrid({note(1, maximum / 4, 1)});
    options = {maximum / 2};
    check(hugeGrid.quant({1}, options).ok && hugeGrid.get(1).start_ticks == 0,
          "maximum grid uses overflow-free exact fractional multiplication");
    Project zero;
    const auto original = zero.blob();
    options = {100};
    options.strength = 0;
    check(zero.quant({1}, options).ok && zero.blob() == original,
          "zero-strength quantize preserves an already ordered stream exactly");
    Project duplicate({note(1, 0, 10), note(1, 20, 10)});
    const auto duplicated = duplicate.blob();
    check(!duplicate.quant({1}).ok && duplicate.blob() == duplicated,
          "duplicate note identities in a stored stream refuse rewriting");
    Project nanos;
    auto bytes = nanos.blob();
    StreamHeader header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    header.flags |= StreamFlags::TimeIsNanos;
    std::memcpy(bytes.data(), &header, sizeof header);
    nanos.store->putEventStream(1, "notes", bytes);
    check(!nanos.quant({1}).ok && nanos.blob() == bytes,
          "a nonmusical notes clock refuses a tick-grid rewrite");
    Project late;
    const auto before = late.blob();
    late.store->db().exec("CREATE TRIGGER refuse_notes BEFORE UPDATE ON event_streams BEGIN SELECT "
                          "RAISE(ABORT,'planted notes update failure'); END");
    check(!late.quant({1}).ok && late.blob() == before && OpJournal(*late.store).count() == 0,
          "late stream write failure leaves notes and journal unchanged");
}
void transposeAndBatch() {
    Project p;
    const auto before = p.blob();
    const std::vector<std::uint64_t> selected{1, wideId};
    check(OpJournal(*p.store).commit(notes::transposeRequest(1, selected, 12)).ok && p.get(1).key == 72 &&
              p.get(wideId).key == 72 && p.get(3).key == 62,
          "transpose uses explicit full-width note IDs");
    check(History(*p.store).undo().ok && p.blob() == before,
          "transpose symmetric inverse restores exact bytes");
    std::vector<OpRequest> batch{notes::quantizeRequest(1, selected, {100}),
                                 notes::transposeRequest(1, selected, -12)};
    const auto result = OpJournal(*p.store).commit(batch);
    check(result.ok && History(*p.store).undo().opsApplied == 2 && p.blob() == before,
          "multiple clip/editor actions can share one native undo transaction");
    Project range({note(1, 0, 10, 60), note(2, 10, 10, 127)});
    const auto original = range.blob();
    const std::vector<std::uint64_t> all{1, 2};
    check(!OpJournal(*range.store).commit(notes::transposeRequest(1, all, 1)).ok &&
              range.blob() == original && OpJournal(*range.store).count() == 0,
          "out-of-range transpose refuses the whole selection without clipping intervals or partial writes");
}
void refusalAndReplay() {
    Project p;
    const auto before = p.blob();
    check(!p.quant({}).ok && !p.quant({1, 1}).ok && !p.quant({999}).ok && p.blob() == before,
          "empty, duplicate and nonexistent selections refuse without changing notes");
    check(!p.quant({1}, {0}).ok && !p.quant({1}, {100, 0, 10001}).ok &&
              !p.quant({1}, {100, 0, 10000, 10001}).ok,
          "invalid numeric quantize settings are refused");
    notes::QuantizeSettings disabled{100};
    disabled.starts = false;
    disabled.ends = false;
    check(!p.quant({1}, disabled).ok, "a quantize request must choose an endpoint");
    Project overflow({note(1, std::numeric_limits<std::int64_t>::max() - 1, 5)});
    const auto original = overflow.blob();
    check(!overflow.quant({1}).ok && overflow.blob() == original && OpJournal(*overflow.store).count() == 0,
          "overflowing note endpoints refuse rather than wrapping");
    OpRequest restore;
    restore.opType = "note.restoreNotes";
    restore.payload = {{"clip", 1}, {"stream", {{"bytes", Payload::array()}}}};
    check(!OpJournal(*p.store).commit(restore).ok && p.blob() == before,
          "malformed restore data cannot destroy an existing stream");
    const auto emptyStream = writeStream<NoteRecord>(FourCC::Notes, {});
    std::vector<std::uint8_t> noNotes;
    for (auto value : emptyStream)
        noNotes.push_back(std::to_integer<std::uint8_t>(value));
    restore.payload["stream"] = {{"bytes", Payload::binary(noNotes)}};
    check(!OpJournal(*p.store).commit(restore).ok && p.blob() == before,
          "well-formed restore cannot drop or replace existing note identities");
    Project target;
    p.store->db().exec(
        "INSERT INTO session_state(key,value) VALUES('selection','different source selection')");
    target.store->db().exec(
        "INSERT INTO session_state(key,value) VALUES('selection','different replay selection')");
    const std::vector<std::uint64_t> selected{wideId};
    const auto request = notes::quantizeRequest(1, selected, {100});
    check(OpJournal(*p.store).commit(request).ok && OpJournal(*target.store).commit(request).ok &&
              p.blob() == target.blob(),
          "explicit payload replay ignores unrelated UI selection state");
    const auto path = p.store->path();
    p.store.reset();
    StoreError error{};
    p.store = Store::open(path, error, true);
    check(p.store && !p.quant({1}).ok, "readonly project refuses note transforms");
}
} // namespace
int main() {
    try {
        basic();
        modes();
        extremeAndMalformed();
        transposeAndBatch();
        refusalAndReplay();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("note transforms: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
