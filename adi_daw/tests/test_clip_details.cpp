// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/blob.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::engine;
constexpr std::int64_t quarter = 5765760;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what.c_str()); }
}
struct Fixture {
    test::TempDirectory temp;
    std::unique_ptr<Store> store;
    explicit Fixture(const char* name) : temp("clip_details", name) {
        StoreError error = StoreError::Ok;
        store = Store::create(temp.path() / "song.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Keys')");
        store->db().exec("INSERT INTO clips(id,track_id,kind,name,color,pos_ticks,length_ticks,content_offset_ticks,loop_enabled,loop_start_ticks,loop_len_ticks,muted,gain_db,fade_in_ticks,fade_out_ticks,fade_in_curve,fade_out_curve) "
                        "VALUES(10,1,'midi','Phrase',0,11531520,23063040,23,1,31,5765760,1,-6,17,19,2,4)");
    }
    ClipDetails read(std::int64_t id = 10) {
        auto result = readClipDetails(*store, id);
        if (!result.clip) throw std::runtime_error(result.error);
        return std::move(*result.clip);
    }
};
NoteRecord note(std::uint64_t id, std::int64_t start, std::uint16_t flags = 0) {
    NoteRecord value{};
    value.note_id = id; value.start_ticks = start; value.dur_ticks = quarter;
    value.key = 64; value.vel_on = 99; value.vel_off = 77; value.channel = 3;
    value.flags = flags; value.probability = 8100; value.tuning_cents = -23.5f;
    return value;
}
std::vector<std::byte> stream(const std::vector<NoteRecord>& notes) {
    return writeStream<NoteRecord>(FourCC::Notes, notes);
}

void propertiesAndIdentity() {
    Fixture f("properties");
    check(f.store->putEventStream(10, "notes", stream({note(777,0,NoteFlags::Muted | NoteFlags::Selected),
              note(std::numeric_limits<std::uint64_t>::max(),quarter)})), "note fixture");
    f.store->db().exec("INSERT INTO time_signature_map(pos_ticks,numerator,denominator) VALUES(0,4,4),(5765760,7,8)");
    f.store->db().exec("INSERT INTO key_map(id,pos_ticks,root,scale_id,scale_mask) VALUES(2,0,0,'major',2741),(3,5765760,2,'minor',1453)");
    auto value = f.read();
    check(value.id == 10 && value.trackId == 1 && !value.laneId && !value.aliasOf && value.kind == "midi" &&
              value.name == "Phrase" && value.color == 0 && value.timeBase == 0 && value.posTicks == 2*quarter &&
              value.lengthTicks == 4*quarter && !value.posNs && !value.lengthNs, "clip identity, palette zero and original placement units");
    check(value.loopEnabled && value.loopStartTicks == 31 && value.loopLengthTicks == quarter &&
              value.contentOffsetTicks == 23 && value.startMarkerTicks == 23 && value.endMarkerTicks == 4*quarter+23 &&
              !value.endMarkerLocal && value.muted && value.gainDb == -6 && value.fadeInTicks == 17 &&
              value.fadeOutTicks == 19 && value.fadeInCurve == 2 && value.fadeOutCurve == 4, "loop window, default markers and common clip fields");
    check(value.signature.numerator == 7 && value.signature.denominator == 8 && !value.signature.local &&
              value.scale.root == 2 && value.scale.id == "minor" && value.scale.mask == 1453 &&
              !value.scale.local && !value.scale.tuningId, "signature and scale inherited at clip start");
    check(value.notes.size() == 2 && value.notes[0].id == 777 && value.notes[1].id == std::numeric_limits<std::uint64_t>::max(), "exact note ids include full uint64");
    check(value.notes[0].startTicks == 0 && value.notes[0].durationTicks == quarter && value.notes[0].key == 64 &&
              value.notes[0].velocity == 99 && value.notes[0].releaseVelocity == 77 && value.notes[0].channel == 3 &&
              value.notes[0].flags == (NoteFlags::Muted | NoteFlags::Selected) && value.notes[0].probability == 8100 &&
              value.notes[0].tuningCents == -23.5f && !value.notes[0].active && value.notes[1].active &&
              value.notesProblem.empty() && value.notesRecordSize == 40 && !value.encodedNotes.empty(), "all notes, including inactive, with original properties");
    OpRequest op; op.opType = "note.move";
    op.payload = {{"clip",10},{"note",std::numeric_limits<std::uint64_t>::max()},{"start",0},{"key",60}};
    check(OpJournal(*f.store).commit(op).ok, "move addresses id from read model");
    const auto moved = f.read();
    check(moved.notes[0].id == std::numeric_limits<std::uint64_t>::max() && moved.notes[0].key == 60 &&
              value.notes[0].id == 777 && value.notes[1].startTicks == quarter, "sorting after edit changes display order without changing identity or retained values");
    check(History(*f.store).undo().ok && f.read().notes[1].startTicks == quarter, "refresh after undo reads original note");
    f.store->db().exec("UPDATE clips SET color=NULL,loop_enabled=0,loop_len_ticks=NULL WHERE id=10");
    value = f.read();
    check(!value.color && !value.loopEnabled && !value.loopLengthTicks, "NULLs are distinct from explicit zero/defaults");
    check(!readClipDetails(*f.store,999).clip && !readClipDetails(*f.store,999).error.empty(), "missing clip is explicit");
}

void linearAndTuningContext() {
    Fixture f("linear");
    f.store->db().exec("UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=750000000,length_ns=1000000000 WHERE id=10");
    f.store->db().exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(5765760,60)");
    f.store->db().exec("INSERT INTO time_signature_map(pos_ticks,numerator,denominator) VALUES(0,4,4),(5765760,3,8)");
    f.store->db().exec("INSERT INTO key_map(id,pos_ticks,root,scale_id,scale_mask) VALUES(3,5765760,0,'rast',2741)");
    f.store->db().exec("INSERT INTO tuning_systems(id,name,source) VALUES(2,'24-TET','edo')");
    f.store->db().exec("INSERT INTO key_map_tunings(key_map_id,tuning_id) VALUES(3,2)");
    f.store->db().exec("INSERT INTO key_map_degrees(key_map_id,degree) VALUES(3,0),(3,4),(3,7)");
    auto value = f.read();
    check(value.timeBase == 1 && value.posNs == 750000000 && value.lengthNs == 1000000000 &&
              !value.posTicks && !value.lengthTicks && value.endMarkerTicks == quarter+23, "linear clip derives marker ticks through tempo change, preserving raw ns");
    check(value.signature.numerator == 3 && value.signature.denominator == 8 && value.scale.id == "rast" &&
              value.scale.tuningId == 2 && value.scale.degrees == std::vector<std::int64_t>{0,4,7}, "linear origin selects inherited meter and native tuned scale");
    f.store->db().exec("UPDATE tempo_map SET curve=1 WHERE pos_ticks=0");
    check(!f.read().propertiesProblem.empty(), "unsupported tempo ramp is named");
    f.store->db().exec("UPDATE clips SET content_offset_ticks=9223372036854775807 WHERE id=10");
    value = f.read();
    check(!value.endMarkerTicks && !value.propertiesProblem.empty() && value.posNs == 750000000,
          "out-of-range derived end marker does not overflow or lose stored placement");
}

void optionalLocalProperties() {
    Fixture f("local");
    // Read compatibility with N33's additive format, not a migration claim.
    f.store->db().exec("CREATE TABLE IF NOT EXISTS clip_properties(clip_id INTEGER PRIMARY KEY,end_marker_ticks INTEGER,signature_numerator INTEGER,signature_denominator INTEGER,scale_root INTEGER,scale_id TEXT,scale_mask INTEGER)");
    f.store->db().exec("INSERT INTO clip_properties VALUES(10,919,5,4,7,'major',2741)");
    const auto value = f.read();
    check(value.endMarkerTicks == 919 && value.endMarkerLocal && value.signature.local && value.signature.numerator == 5 &&
              value.signature.denominator == 4 && value.scale.local && value.scale.root == 7 && value.scale.id == "major" &&
              value.scale.mask == 2741 && !value.scale.tuningId, "optional per-clip overrides read when N33 format exists");
    f.store->db().exec("UPDATE clip_properties SET end_marker_ticks=NULL,signature_numerator=NULL,signature_denominator=NULL,scale_root=NULL,scale_id=NULL,scale_mask=NULL");
    const auto inherited = f.read();
    check(!inherited.endMarkerLocal && !inherited.signature.local && !inherited.scale.local &&
              inherited.signature.numerator == 4 && inherited.scale.root == 0, "NULL local fields inherit normal defaults");
}

void corruptAndIndependentStreams() {
    Fixture f("bad");
    f.store->db().exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(11,1,'midi',0,4),(12,1,'audio',0,4)");
    check(f.store->putEventStream(11,"notes",std::vector<std::byte>{std::byte{9}}), "other corrupt clip");
    auto value = f.read();
    check(value.notes.empty() && value.notesProblem.empty() && value.name == "Phrase", "empty selected clip does not decode unrelated corrupt stream");
    check(!f.read(11).notesProblem.empty() && f.read(11).notes.empty(), "corrupt selected stream is named, metadata retained");
    check(f.store->putEventStream(12,"notes",std::vector<std::byte>{std::byte{9}}), "audio stream fixture");
    check(f.read(12).notesProblem.empty() && f.read(12).kind == "audio", "audio properties do not parse a MIDI stream");
    auto bad = note(9,0); bad.dur_ticks = 0;
    check(f.store->putEventStream(10,"notes",stream({note(8,0),bad})), "bad record fixture");
    value = f.read();
    check(value.notes.empty() && !value.notesProblem.empty() && value.loopEnabled, "invalid note refuses whole curve without losing clip properties");
    check(f.store->putEventStream(10,"notes",stream({note(0,0),note(7,quarter),note(7,2*quarter)})), "ambiguous ids fixture");
    value = f.read();
    check(value.notes.size() == 3 && value.notes[0].id == 0 && value.notes[1].id == value.notes[2].id &&
              !value.notesProblem.empty(), "legacy ids stay exact and ambiguity is named rather than silently renumbered");
    auto encoded = stream({note(123,0)});
    StreamHeader header{}; std::memcpy(&header,encoded.data(),sizeof header);
    header.rec_size = 44; std::memcpy(encoded.data(),&header,sizeof header); encoded.resize(encoded.size()+4,std::byte{7});
    check(f.store->putEventStream(10,"notes",encoded), "future note fixture");
    value = f.read();
    check(value.notes.size() == 1 && value.notes[0].id == 123 && value.notesRecordSize == 44 &&
              value.encodedNotes == encoded, "future record prefix and original bytes exposed");
    const auto path = f.store->path(); f.store.reset();
    StoreError error = StoreError::Ok; f.store = Store::open(path,error,true);
    check(f.store && f.read().notes[0].id == 123, "read-only reopened clip view");
}
} // namespace
int main() {
    try { propertiesAndIdentity(); linearAndTuningContext(); optionalLocalProperties(); corruptAndIndependentStreams(); }
    catch (const std::exception& error) { check(false,std::string("exception: ")+error.what()); }
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
