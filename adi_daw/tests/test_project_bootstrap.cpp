// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/check.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
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
    check(db.execAndGet("SELECT group_concat(name,',') FROM (SELECT name FROM tracks "
                        "WHERE kind!='master' ORDER BY index_in_parent,id)").getString()=="1 MIDI,2 MIDI,3 Audio,4 Audio",
          "default track names and positions are deterministic");
    check(count(*store,"mixer_strip")==5 && db.execAndGet("SELECT count(*) FROM mixer_strip "
        "WHERE volume_db=0 AND pan=0 AND input_gain_db=0 AND delay_samples=0").getInt()==5,"every root track has its default mixer strip");
    check(db.execAndGet("SELECT count(*) FROM tempo_map WHERE pos_ticks=0 AND bpm=120").getInt()==1 &&
          db.execAndGet("SELECT count(*) FROM time_signature_map WHERE pos_ticks=0 AND numerator=4 AND denominator=4").getInt()==1,
          "the root tempo and time signature are initialized");
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
        check(session.load(*store,{},spec),"a New project loads into the playback engine without more writes");
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
    OpRequest track;track.opType="track.create";track.payload={{"id",6},{"kind","audio"},{"name","Added"}};
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
}
int main() {
    try{newProject();}catch(const std::exception& e){check(false,e.what());}
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
