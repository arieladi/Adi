// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/check.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "juce/builtin_host.hpp"
#include "temp_directory.hpp"
#include "return_project_fixture.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace adi;
int checks=0,failures=0;
void check(bool good,const char* name) {
    ++checks;
    if(!good){++failures;std::printf("FAIL: %s\n",name);}
}
int count(Store& store,const char* table) {
    return store.db().execAndGet(std::string("SELECT count(*) FROM ")+table).getInt();
}
void newProject() {
    test::TempDirectory temp("bootstrap","new");
    const auto path=temp.path()/"project.adi";
    StoreError error{};
    auto store=Store::createNewProject(path,error);
    check(store && error==StoreError::Ok,"New creates the complete project");
    if(!store)return;
    auto& db=store->db();
    check(count(*store,"project")==1 && db.execAndGet("SELECT sample_rate FROM project").getInt()==44100,
          "the project has one row at 44.1 kHz");
    check(db.execAndGet("SELECT count(*) FROM tracks WHERE id=1 AND kind='master' AND name='Main' "
                        "AND parent_id IS NULL").getInt()==1,"Main is present at the root");
    check(count(*store,"tracks")==5 && db.execAndGet("SELECT count(*) FROM tracks WHERE kind='midi'").getInt()==2 &&
          db.execAndGet("SELECT count(*) FROM tracks WHERE kind='audio'").getInt()==2,
          "the default Set has two MIDI and two audio tracks");
    check(db.execAndGet("SELECT count(*) FROM tracks WHERE kind='audio' AND monitor_mode=0").getInt()==2 &&
          db.execAndGet("SELECT count(*) FROM tracks WHERE kind='midi' AND monitor_mode=2").getInt()==2 &&
          db.execAndGet("SELECT count(*) FROM tracks WHERE id=1 AND monitor_mode=0").getInt()==1,
          "SC-249: New seeds audio tracks Off and MIDI tracks Auto");
    check(db.execAndGet("SELECT group_concat(name,',') FROM (SELECT name FROM tracks "
                        "WHERE kind!='master' ORDER BY index_in_parent,id)").getString()=="# MIDI,# MIDI,# Audio,# Audio",
          "default track names and positions are deterministic: numbered names, as Live's (walk-B B13)");
    check(count(*store,"mixer_strip")==5 && db.execAndGet("SELECT count(*) FROM mixer_strip "
        "WHERE volume_db=0 AND pan=0 AND input_gain_db=0 AND delay_samples=0").getInt()==5,"every root track has its default mixer strip");
    check(db.execAndGet("SELECT count(*) FROM tempo_map WHERE pos_ticks=0 AND bpm=120").getInt()==1 &&
          db.execAndGet("SELECT count(*) FROM time_signature_map WHERE pos_ticks=0 AND numerator=4 AND denominator=4").getInt()==1,
          "the root tempo and time signature are initialized");
    check(db.execAndGet("SELECT count(*) FROM tracks WHERE kind='return'").getInt()==0 &&
          db.execAndGet("SELECT index_in_parent FROM tracks WHERE id=1").getInt()==4,
          "SC-437: Main follows the four source tracks with no returns");
    check(count(*store,"routing")==0 && count(*store,"devices")==0 && count(*store,"plugin_refs")==0,
          "SC-437: New has no sends or effects belonging to former default returns");
    check(checkProject(*store).clean(),"the initialized project passes the actual project checker");
    check(count(*store,"ops")==0,"bootstrap creates no ops");
    History history(*store);
    check(!history.head() && !history.canUndo() && !history.canRedo(),"New is the history root without UI undo protection");
    const auto root=history.takeSnapshot("New",1,0);
    check(root.ok && !history.listSnapshots().front().opSeq,"a snapshot can name New before any edit");
    const auto snapshot=engine::SnapshotBuilder::fromStore(*store);
    check(snapshot->tracks.size()==5 && snapshot->findTrack(1) && snapshot->findTrack(1)->kind=="master" &&
          snapshot->findTrack(1)->name=="Main" && snapshot->sampleRate==44100,
          "the window can read Main and all root tracks directly from the snapshot");
    {
        engine::Session session;
        engine::SessionSpec spec;spec.sampleRate=snapshot->sampleRate;spec.maxFrames=64;
        check(session.load(*store,device::withBuiltinDevices(),spec),"a New project loads into the playback engine without more writes");
        check(session.devices().deviceCount()==0,"New has no implicit return effects to load");
        std::array<float,128> l,r;l.fill(7);r.fill(7);float* out[]{l.data(),r.data()};
        engine::AudioIo io;io.out=out;io.numOut=2;io.frames=64;session.process(io);
        check(std::all_of(l.begin(),l.begin()+64,[](float v){return v==0;}) &&
              std::all_of(r.begin(),r.begin()+64,[](float v){return v==0;}),"the initialized engine renders silence");
        check(count(*store,"ops")==0,"loading and rendering do not turn New into an undo step");
    }
    OpJournal journal(*store);
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",1},{"name","Changed"}};
    check(journal.commit(rename).ok && history.canUndo(),"the first real edit becomes undoable");
    check(history.undo().ok && count(*store,"tracks")==5 &&
          db.execAndGet("SELECT name FROM tracks WHERE id=1").getString()=="Main","Undo restores Main and leaves the root tracks");
    check(!history.undo().ok,"a second Undo cannot undo New");
    check(history.redo().ok && db.execAndGet("SELECT name FROM tracks WHERE id=1").getString()=="Changed",
          "Redo reapplies the first edit");
    check(history.undo().ok,"return to New before forking");
    OpRequest track;track.opType="track.create";track.payload={{"id",8},{"kind","audio"},{"name","Added"}};
    check(journal.commit(track).ok && history.branches().size()==2,"an edit from New preserves the old branch");
    check(history.revertToSnapshot(root.id,2).ok && count(*store,"tracks")==5 &&
          db.execAndGet("SELECT name FROM tracks WHERE id=1").getString()=="Main",
          "reverting to New removes later edits while preserving all initialized data");
    store.reset();store=Store::open(path,error);
    check(store && count(*store,"tracks")==5 && count(*store,"mixer_strip")==5 && !History(*store).canUndo(),
          "the history root and its strips survive reopening");
    auto refused=Store::createNewProject(path,error);
    check(!refused && error==StoreError::AlreadyExists && count(*store,"tracks")==5,
          "New refuses to overwrite an existing project");
}
void explicitReturnsSurviveOpen() {
    test::TempDirectory temp("bootstrap","explicit-returns");
    const auto path=temp.path()/"explicit.adi";
    StoreError error{};
    auto authored=test::createProjectWithReturns(path,error);
    check(authored!=nullptr,"write a project with an authored pair of returns");
    if(!authored)return;
    authored.reset();
    auto store=Store::open(path,error);
    check(store && count(*store,"tracks")==7 && count(*store,"routing")==8 &&
          store->db().execAndGet("SELECT count(*) FROM tracks WHERE kind='return'").getInt()==2,
          "SC-437: opening an explicit project keeps both returns and all eight sends");
    if(!store)return;
    engine::Session session;
    engine::SessionSpec spec;
    check(session.load(*store,device::withBuiltinDevices(),spec),"a saved project with returns loads into the engine");
#if ADI_WITH_PD
    check(session.stats().loaded==2 && session.devices().deviceCount()==2,"the authored return effects load in the actual engine");
    if(session.devices().deviceCount()==2) {
        check(session.entryFor(1) && session.entryFor(2) && session.entryFor(1)->trackId==6 && session.entryFor(2)->trackId==7 &&
              session.chainFor(6).size()==1 && session.chainFor(7).size()==1 &&
              session.devices().deviceAt(0).identity().uid=="org.adidaw.builtin.reverb" &&
              session.devices().deviceAt(1).identity().uid=="org.adidaw.builtin.delay" &&
              session.devices().deviceAt(0).getParam("31").real==100 && session.devices().deviceAt(1).getParam("25").real==100,
              "stored return controls reach the native hosts on the correct tracks");
    }
#endif
}
void monitorDefaultAndLegacyRedo() {
    test::TempDirectory temp("bootstrap","track-monitor");
    StoreError error{};
    auto store=Store::createEmpty(temp.path()/"monitor.adi",error);
    if (!store) throw std::runtime_error("create monitor fixture");
    auto& db=store->db();
    OpJournal journal(*store);
    OpRequest create;
    create.opType="track.create";
    create.payload={{"id",1},{"kind","instrument"}};
    const auto created=journal.commit(create);
    if (!created.ok) throw std::runtime_error(created.error);
    const auto monitor=[&] { return db.execAndGet("SELECT monitor_mode FROM tracks WHERE id=1").getInt(); };
    check(monitor()==2,"ordinary track.create monitors a new instrument on Auto");
    auto history=History(*store);
    check(history.undo().ok && history.redo().ok && monitor()==2,
          "Redo retains the recorded Auto default of the new track");
    if(!history.undo().ok) throw std::runtime_error("prepare replay of an older create");
    const auto legacy=encodePayload({{"id",1}});
    SQLite::Statement prior(db,"UPDATE ops SET inverse=? WHERE seq=?");
    prior.bind(1,legacy.data(),static_cast<int>(legacy.size()));
    prior.bind(2,created.seqs.front());
    prior.exec();
    check(history.redo().ok && monitor()==0,
          "redoing an older track.create retains its original Off default");

    create.payload={{"id",2},{"kind","audio"}};
    const auto audio=journal.commit(create);
    if (!audio.ok) throw std::runtime_error(audio.error);
    const auto audioMonitor=[&] { return db.execAndGet("SELECT monitor_mode FROM tracks WHERE id=2").getInt(); };
    check(audioMonitor()==0, "SC-249: ordinary track.create monitors a new audio track Off");
    check(history.undo().ok && history.redo().ok && audioMonitor()==0,
          "SC-249: an audio track created Off is still Off after Undo and Redo");
    if (!history.undo().ok) throw std::runtime_error("prepare replay of an N67 audio create");
    const auto oldAuto=encodePayload({{"id",2},{"monitorMode",2}});
    prior.reset();
    prior.bind(1,oldAuto.data(),static_cast<int>(oldAuto.size()));
    prior.bind(2,audio.seqs.front());
    prior.exec();
    check(history.redo().ok && audioMonitor()==2,
          "SC-249: Redo restores an older audio track whose create recorded Auto");
}

}
int main() {
    try{newProject();explicitReturnsSurviveOpen();monitorDefaultAndLegacyRedo();}catch(const std::exception& e){check(false,e.what());}
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
