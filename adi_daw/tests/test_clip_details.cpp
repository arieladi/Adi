// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/blob.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
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
    ClipDetails read(std::int64_t id = 10,bool includeExpression=false) {
        auto result = readClipDetails(*store,id,true,includeExpression);
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
    check(value.notes.size() == 2 && value.notes[0].startTicks == 0 && value.notes[0].durationTicks == quarter && value.notes[0].key == 64 &&
              value.notes[0].velocity == 99 && value.notes[0].releaseVelocity == 77 && value.notes[0].channel == 3 &&
              value.notes[0].flags == (NoteFlags::Muted | NoteFlags::Selected) && value.notes[0].probability == 8100 &&
              value.notes[0].tuningCents == -23.5f && !value.notes[0].active && value.notes[1].active &&
              value.notesProblem.empty() && value.notesRecordSize == 40 && !value.encodedNotes.empty(), "all notes, including inactive, with original properties");
    OpRequest op; op.opType = "note.move";
    // A genuinely earlier start tests display reordering. Equal-time records
    // retain their order; pitch is not a secondary sorting contract (MI-01).
    op.payload = {{"clip",10},{"note",std::numeric_limits<std::uint64_t>::max()},{"start",-1},{"key",60}};
    check(OpJournal(*f.store).commit(op).ok, "move addresses id from read model");
    const auto moved = f.read();
    check(moved.notesProblem.empty() && moved.notes.size() == 2 && moved.notes[0].startTicks == -1,
          "negative note origins remain valid signed relative ticks in the Clip View read model");
    check(moved.notes.size() == 2 && value.notes.size() == 2 &&
              moved.notes[0].id == std::numeric_limits<std::uint64_t>::max() && moved.notes[0].key == 60 && moved.notes[0].startTicks == -1 &&
              value.notes[0].id == 777 && value.notes[1].startTicks == quarter, "sorting after edit changes display order without changing identity or retained values");
    const bool undone = History(*f.store).undo().ok;
    const auto restored = f.read();
    check(undone && restored.notes.size() == 2 && restored.notes[1].startTicks == quarter, "refresh after undo reads original note");
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
    value = f.read();
    check(value.propertiesProblem.empty() && value.endMarkerTicks == quarter + 23,
          "a linear clip after the ramp keeps its musical duration and needs no fallback");
    f.store->db().exec("UPDATE clips SET content_offset_ticks=9223372036854775807 WHERE id=10");
    value = f.read();
    check(!value.endMarkerTicks && !value.propertiesProblem.empty() && value.posNs == 750000000,
          "out-of-range derived end marker does not overflow or lose stored placement");
}

void exactLinearEnd() {
    Fixture fixture("exact-end");
    fixture.store->db().exec(
        "DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
        "UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,"
        "pos_ns=342123,length_ns=5126627 WHERE id=10");
    check(fixture.read().endMarkerTicks == 59141,
          "the Clip View resolves a linear end on the exact tick");
    OpRequest start;
    start.opType = "clip.setStartMarker";
    start.payload = {{"id",10},{"ticks",101}};
    check(OpJournal(*fixture.store).commit(start).ok && fixture.read().endMarkerTicks == 59141,
          "the native Start operation preserves the same exact linear End tick");
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
    if (f.store) {
        const auto reopened = f.read();
        check(reopened.notes.size() == 1 && reopened.notes[0].id == 123, "read-only reopened clip view");
    } else {
        check(false, "read-only reopened clip view");
    }
}
void storedExpression() {
    Fixture f("expression");
    const auto wideId=std::numeric_limits<std::uint64_t>::max();
    check(f.store->putEventStream(10,"notes",stream({note(wideId,0),note(77,quarter),note(77,2*quarter),note(333,3*quarter)})),
          "expression fixture includes a full-width note and an ambiguous pair");
    auto& db=f.store->db();
    db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(11,1,'midi',0,4608)");
    const auto point=[](std::int64_t tick,float v,std::uint8_t shape=1,float tension=0) {
        ExpressionPoint p{};p.time_ticks=tick;p.value=v;p.curve=shape;p.tension=tension;return p;
    };
    const auto encode=[](const std::vector<ExpressionPoint>& points,std::uint32_t flags=StreamFlags::SortedByTime) {
        return writeStream<ExpressionPoint>(FourCC::Expression,points,1,flags);
    };
    const auto insert=[&](int id,int clip,std::int64_t noteId,int dimension,const std::vector<std::byte>& bytes) {
        SQLite::Statement row(db,"INSERT INTO note_expression(id,clip_id,note_id,dimension,data) VALUES(?,?,?,?,?)");
        row.bind(1,id);row.bind(2,clip);row.bind(3,noteId);row.bind(4,dimension);
        row.bind(5,bytes.data(),static_cast<int>(bytes.size()));row.exec();
    };
    auto future=encode({point(-7,12,5,std::numeric_limits<float>::quiet_NaN())});
    StreamHeader header{};std::memcpy(&header,future.data(),sizeof header);
    header.rec_size=28;header.version=2;std::memcpy(future.data(),&header,sizeof header);
    future.resize(future.size()+4,std::byte{0xa5});
    insert(11,10,-1,0,future);
    insert(12,10,-1,1,encode({point(40,0.5f,1,-2),point(0,0.25f)},0));
    insert(13,10,-1,2,encode({point(0,0.875f)}));
    insert(14,10,-1,3,encode({point(0,-18.0f)}));
    insert(15,10,-1,4,encode({point(0,-0.375f)}));
    insert(16,10,-1,72,encode({point(0,17.0f)}));
    insert(17,10,333,0,encode({point(0,1.0f,6)}));
    insert(18,10,333,1,encode({point(0,0.25f),point(0,0.875f)}));
    insert(19,10,333,2,{std::byte{13}});
    insert(20,10,333,4,encode({point(0,1.5f)}));
    insert(21,10,77,4,encode({point(0,0.25f)}));
    insert(22,10,999,4,encode({point(0,0.25f)}));
    insert(23,11,500,3,{std::byte{97}});

    const auto value=f.read(10,true);
    const auto& curves=value.noteExpressions.curves;
    check(curves.size()==6 && std::all_of(curves.begin(),curves.end(),[&](const auto& c){return c.noteId==wideId && c.clipId==10;}),
          "Clip Details binds six curves to the exact note ID and excludes ambiguous and orphaned rows");
    if(curves.size()==6) {
        check(curves[0].dimension==0 && curves[0].recordSize==28 && curves[0].points.size()==1 &&
              curves[0].points[0].time_ticks==-7 && curves[0].points[0].value==12 &&
              curves[0].points[0].curve==5 && curves[0].points[0].tension==0,
              "future AEXP stride preserves interpreted prefix and signed time with neutral NaN tension");
        check(curves[1].points.size()==2 && curves[1].points[0].time_ticks==0 && curves[1].points[1].time_ticks==40 &&
              curves[1].points[1].tension==-1.0f,"unflagged expression sorts by time and clamps tension for display");
        check(curves[2].dimension==2 && curves[2].points[0].value==0.875f && curves[3].dimension==3 &&
              curves[3].points[0].value==-18.0f && curves[4].dimension==4 && curves[4].points[0].value==-0.375f &&
              curves[5].dimension==72 && curves[5].points[0].value==17.0f,
              "slide, decibel gain, signed pan and plug-in dimensions keep their native values");
    }
    const auto& problems=value.noteExpressions.problems;
    const auto mentions=[&](std::string_view needle){return std::any_of(problems.begin(),problems.end(),
        [&](const auto& problem){return problem.find(needle)!=std::string::npos;});};
    check(problems.size()==6 && mentions("note_expression#17") && mentions("note_expression#18") &&
          mentions("note_expression#19") && mentions("note_expression#20") && mentions("note 77") && mentions("note 999") &&
          !mentions("note_expression#23"),"scoped read names only the damaged and unbound expression rows of that clip");
    const auto all=readNoteExpressions(*f.store);
    check(all.curves.size()==8 && all.problems.size()==5,"project projection returns each decodable row for later playback admission");
    const auto noNotes=readClipDetails(*f.store,10,false,true);
    check(noNotes.clip && noNotes.clip->noteExpressions.curves.empty() && noNotes.clip->noteExpressions.problems.empty() &&
          f.read().noteExpressions.curves.empty(),"metadata and ordinary Clip View reads skip expression decoding");
    check(db.execAndGet("SELECT hex(substr(data,-4)) FROM note_expression WHERE id=11").getString()=="A5A5A5A5",
          "read projection leaves the original future AEXP bytes on disk");
}
} // namespace
int main() {
    try {
        propertiesAndIdentity();
        linearAndTuningContext();
        exactLinearEnd();
        optionalLocalProperties();
        corruptAndIndependentStreams();
        storedExpression();
    }
    catch (const std::exception& error) { check(false,std::string("exception: ")+error.what()); }
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
