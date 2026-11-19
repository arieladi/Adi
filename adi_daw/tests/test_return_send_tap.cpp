// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/digest.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/return_send_tap.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "adi/textproj_store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const std::string& text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text.c_str());}}
OpRequest op(const char* type,Payload payload){OpRequest request;request.opType=type;request.payload=std::move(payload);return request;}
OpRequest tap(std::int64_t id,bool pre){return op("return.setSendTap",{{"id",id},{"pre",pre}});}
OpRequest connect(std::int64_t id,std::int64_t target=10){return op("routing.connect",{{"id",id},{"srcKind","track"},{"src",1},{"dstKind","track"},{"dst",target},{"kind","send"}});}
struct Project {
    test::TempDirectory directory{"return_tap","project"};std::unique_ptr<Store> store;
    Project(){StoreError error{};store=Store::create(directory.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));seed(store->db());}
    static void seed(SQLite::Database& db) {
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','One'),(2,'audio','Two'),(10,'return','Reverb'),(11,'return','Delay'),(20,'master','Main');"
            "INSERT INTO mixer_strip(track_id) SELECT id FROM tracks;"
            "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,ord,gain_db,pan,pre_fader,enabled,channel_map) VALUES"
            "(1,'track',1,'track',10,'send',3,-4,0.25,1,1,X'00ff'),"
            "(2,'track',2,'track',10,'send',7,-8,-0.25,0,0,NULL),"
            "(3,'track',1,'track',10,'cue',0,0,0,1,1,NULL),"
            "(4,'track',1,'hw_out',10,'send',0,0,0,0,1,NULL)");
    }
    std::int64_t scalar(const char* sql){return store->db().execAndGet(sql).getInt64();}
};
void choiceAndHistory() {
    Project project;auto& store=*project.store;auto initial=SnapshotBuilder::fromStore(store);const auto before=digestProject(store).text;
    check(initial->findTrack(10)->returnSendTap->legacyMixed && !initial->findTrack(10)->returnSendTap->explicitChoice && !initial->findTrack(11)->returnSendTap->preFader && !initial->findTrack(1)->returnSendTap,"legacy mixed and empty Post are explicit, other track kinds have no return control");
    check(SnapshotBuilder::fromStore(store,*initial)->findTrack(10)==initial->findTrack(10),"unchanged return control shares its track node");
    const auto changed=OpJournal(store).commit(tap(10,false));check(changed.ok,"one Post choice: "+changed.error);
    check(project.scalar("SELECT count(*) FROM return_send_taps WHERE track_id=10 AND pre_fader=0")==1 && project.scalar("SELECT count(*) FROM routing WHERE id IN(1,2) AND pre_fader=0")==2,"return choice persists and normalizes enabled and disabled incoming sends");
    check(project.scalar("SELECT pre_fader FROM routing WHERE id=3")==1 && project.scalar("SELECT pre_fader FROM routing WHERE id=4")==0,"cue and equal-number hardware destinations are not return sends");
    auto frame=SnapshotBuilder::fromStore(store,*initial);
    check(frame->findTrack(10)!=initial->findTrack(10) && frame->findTrack(1)==initial->findTrack(1) && frame->findTrack(10)->returnSendTap->explicitChoice && !frame->findTrack(10)->returnSendTap->legacyMixed,"choice presence and legacyMixed invalidate sharing even with the same displayed Post value");
    check(initial->findTrack(10)->returnSendTap->legacyMixed && !initial->findTrack(10)->returnSendTap->explicitChoice,"old published return controls stay immutable");
    const auto after=digestProject(store).text;
    check(History(store).undo().ok && digestProject(store).text==before,"undo restores preference absence and exact legacy mixed flags");
    check(History(store).redo().ok && digestProject(store).text==after,"redo repeats one return choice exactly");
    const auto path=store.path();project.store.reset();StoreError error{};project.store=Store::open(path,error);
    check(project.store && readReturnSendTaps(*project.store).at(10).explicitChoice && History(*project.store).undo().ok && digestProject(*project.store).text==before,"return inverse and absent preference survive CBOR and reopen");
}
void emptyAndProducer() {
    Project project;auto& store=*project.store;
    check(OpJournal(store).commit(tap(11,true)).ok && readReturnSendTaps(store).at(11).preFader,"a return with no send remembers Pre");
    check(OpJournal(store).commit(connect(55,11)).ok && project.scalar("SELECT pre_fader FROM routing WHERE id=55")==1,"ordinary new send inherits an explicit return choice without a per-edge edit");
    const auto before=digestProject(store).text;
    check(!OpJournal(store).commit(op("routing.setPreFader",{{"id",55},{"pre",false}})).ok && digestProject(store).text==before,"new conflicting per-edge edit refuses atomically");
    check(OpJournal(store).commit(op("routing.setPreFader",{{"id",55},{"pre",true}})).ok,"same-value legacy UI setter remains compatible");
    Project legacy;legacy.store->db().exec("UPDATE routing SET pre_fader=1 WHERE id IN(1,2)");
    const auto root=digestProject(*legacy.store).text;const auto prepared=prepareSendToReturn(*legacy.store,99,1,10);
    check(prepared.ok && prepared.requests.size()==2 && prepared.requests.back().payload.at("pre")==true,"new-send producer captures uniform legacy Pre as literal ops");
    const auto committed=commitSendToReturn(*legacy.store,prepared);
    check(committed.ok && legacy.scalar("SELECT pre_fader FROM routing WHERE id=99")==1 && !readReturnSendTaps(*legacy.store).at(10).explicitChoice,"producer inherits legacy Pre without inventing a preference row");
    check(History(*legacy.store).undo().ok && digestProject(*legacy.store).text==root,"new send and captured legacy tap undo as one step");
    const auto stale=prepareSendToReturn(*legacy.store,100,1,10);
    check(OpJournal(*legacy.store).commit(op("track.rename",{{"id",2},{"name","Other"}})).ok && commitSendToReturn(*legacy.store,stale).stale && legacy.scalar("SELECT count(*) FROM routing WHERE id=100")==0,"prepared send checks the captured head");
    check(commitSendToReturn(store,stale).stale,"prepared send refuses a different project");
    check(!prepareSendToReturn(store,100,999,11).ok && !prepareSendToReturn(store,100,1,1).ok,"producer validates source and return target");
}
void metadataAndGuards() {
    Project project;auto& store=*project.store;auto& db=store.db();
    db.exec("ALTER TABLE return_send_taps ADD COLUMN future BLOB;ALTER TABLE routing ADD COLUMN future BLOB;"
        "INSERT INTO return_send_taps(track_id,pre_fader,future) VALUES(10,0,X'00ff');UPDATE routing SET future=X'123400ff' WHERE id=1");
    const auto before=digestProject(store).text;
    check(OpJournal(store).commit(tap(10,true)).ok,"change an existing choice with future metadata");const auto after=digestProject(store).text;
    check(project.scalar("SELECT future=X'00ff' FROM return_send_taps WHERE track_id=10")==1 && project.scalar("SELECT future=X'123400ff' FROM routing WHERE id=1")==1,"narrow updates preserve future preference and routing BLOBs");
    check(History(store).undo().ok && digestProject(store).text==before && History(store).redo().ok && digestProject(store).text==after,"future metadata survives full choice undo/redo");
    db.exec("UPDATE routing SET pre_fader=0 WHERE id=1");const auto raw=digestProject(store).text;
    auto frame=SnapshotBuilder::fromStore(store);const auto model=rows::readModel(store);
    bool effective=false,stored=false;for(const auto& route:model.routing)if(route.id==1){effective=route.preFader;stored=route.storedPreFader==false;}
    check(frame->sends.front()->preFader && effective && stored && project.scalar("SELECT pre_fader FROM routing WHERE id=1")==0,"snapshot and engine use explicit policy while retaining the raw row for projection");
    check(!History(store).undo().ok && digestProject(store).text==raw,"changed current flags refuse restoration without touching data");
    db.exec("UPDATE routing SET pre_fader=1 WHERE id=1;INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,pre_fader) VALUES(99,'track',1,'track',10,'send',1)");const auto extra=digestProject(store).text;
    check(!History(store).undo().ok && digestProject(store).text==extra,"changed incoming edge set refuses restoration before writes");
    Project batch;const auto root=digestProject(*batch.store).text;
    const std::array<OpRequest,2> bad={tap(11,true),connect(1,11)};const auto count=batch.scalar("SELECT count(*) FROM ops");
    check(!OpJournal(*batch.store).commit(bad).ok && digestProject(*batch.store).text==root && batch.scalar("SELECT count(*) FROM ops")==count,"late batch collision rolls back choice, flags and journal together");
    check(!OpJournal(*batch.store).commit(tap(1,true)).ok && !OpJournal(*batch.store).commit(op("return.setSendTap",{{"id",11}})).ok,"wrong track kind or absent choice refuses");
    batch.store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1099511627776,'return','Wide')");
    check(OpJournal(*batch.store).commit(tap(1099511627776LL,true)).ok && readReturnSendTaps(*batch.store).at(1099511627776LL).preFader,"return ids retain 64-bit width");
}
void disconnectAndProjection() {
    Project project;auto& store=*project.store;auto& db=store.db();
    db.exec("ALTER TABLE routing ADD COLUMN future BLOB;UPDATE routing SET future=X'0001ff' WHERE id=1;INSERT INTO return_send_taps VALUES(10,0)");
    const auto before=digestProject(store).text;
    check(OpJournal(store).commit(op("routing.disconnect",{{"id",1}})).ok,"disconnect captures full typed routing row");
    check(History(store).undo().ok && digestProject(store).text==before,"captured reconnect preserves raw Pre, gain, pan, order, channel-map and future bytes instead of inheriting the current Post policy");
    check(History(store).redo().ok && project.scalar("SELECT count(*) FROM routing WHERE id=1")==0,"disconnect redo still deletes its literal id");
    const auto explicitPost=textproj::projectStore(store);
    check(explicitPost.text.find("sendTap post")!=std::string::npos,"text projection represents an explicit Post choice");
    db.exec("DELETE FROM return_send_taps");
    check(textproj::projectStore(store).text.find("sendTap")==std::string::npos,"absent default/legacy preference is distinct from stored Post in projection");
}
class Tone final:public Node {
public:void process(const NodeIo& io)noexcept override {for(std::int32_t channel=0;channel<io.channels;++channel)if(io.out[channel])std::fill_n(io.out[channel]+io.blockOffset,io.frames,0.25f);}
};
void audible() {
    for(const auto precision:{Precision::Float32,Precision::Float64})for(bool pre:{false,true}) {
        Project project;auto& store=*project.store;store.db().exec("DELETE FROM routing;UPDATE mixer_strip SET volume_db=-6.020599913279624 WHERE track_id=1");
        check(OpJournal(store).commit(tap(10,pre)).ok && OpJournal(store).commit(connect(1)).ok,"audible fixture chooses return tap and creates send");
        // A legacy/raw mismatch proves the reader policy, not just normalized SQL.
        store.db().exec(pre?"UPDATE routing SET pre_fader=0":"UPDATE routing SET pre_fader=1");
        Tone tone;Session session;session.graph().setFadeFrames(0);session.setSourcesFor([&](std::int64_t id){return id==1?std::vector<Node*>{&tone}:std::vector<Node*>{};});
        SessionSpec spec;spec.sampleRate=44100;spec.maxFrames=64;spec.precision=precision;
        check(session.load(store,{},spec),"return policy loads through actual engine model");session.transport().play();
        std::array<float,64> left{},right{};float* out[]={left.data(),right.data()};
        for(int block=0;block<12;++block){AudioIo io;io.out=out;io.numOut=2;io.frames=64;io.streamTimeSamples=block*64;session.process(io);}
        const float expected=pre?0.375f:0.25f;
        check(std::abs(left.back()-expected)<1e-5f && std::abs(right.back()-expected)<1e-5f,"actual Pre/Post return levels follow policy in both precisions");
    }
}
void replay() {
    Project first,second;first.store->db().exec("INSERT INTO session_state VALUES('selection','one')");second.store->db().exec("INSERT INTO session_state VALUES('selection','different')");
    const std::array<OpRequest,6> requests={tap(10,false),tap(11,true),connect(55,11),op("routing.setGain",{{"id",55},{"db",-12}}),op("routing.disconnect",{{"id",55}}),tap(10,true)};
    for(const auto& request:requests)check(OpJournal(*first.store).commit(request).ok && OpJournal(*second.store).commit(request).ok,"literal return/send log replays under different UI state");
    check(digestProject(*first.store).text==digestProject(*second.store).text,"literal return/send logs produce identical project rows");
    for(std::size_t step=0;step<requests.size();++step)check(History(*first.store).undo().ok && History(*second.store).undo().ok && digestProject(*first.store).text==digestProject(*second.store).text,"return/send replay undo captures the same flags and routing metadata");
    for(std::size_t step=0;step<requests.size();++step)check(History(*first.store).redo().ok && History(*second.store).redo().ok && digestProject(*first.store).text==digestProject(*second.store).text,"return/send replay redo remains deterministic");
}
void legacyMigration() {
    test::TempDirectory directory{"return_tap","migration"};const auto path=directory.path()/"old.adi";
    {std::ifstream file(std::filesystem::path(ADI_SCHEMA_HISTORY_DIR)/"schema-1.9.sql");std::ostringstream ddl;ddl<<file.rdbuf();SQLite::Database db(path.string(),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);db.exec(ddl.str());db.exec("INSERT INTO project(id,name) VALUES(1,'Old')");Project::seed(db);}
    StoreError error{};{auto readonly=Store::open(path,error,true);check(readonly && readonly->schemaMinor()==9 && readReturnSendTaps(*readonly).at(10).legacyMixed,"read-only old format gets an empty optional policy table without rewriting mixed audio");}
    auto upgraded=Store::open(path,error);check(upgraded && upgraded->schemaMinor()==adi::kSchemaMinor && readReturnSendTaps(*upgraded).at(10).legacyMixed && !readReturnSendTaps(*upgraded).at(10).explicitChoice,"native migration adds only an empty policy table and preserves legacy sound");
    check(OpJournal(*upgraded).commit(tap(10,true)).ok && History(*upgraded).undo().ok && readReturnSendTaps(*upgraded).at(10).legacyMixed,"first migrated choice undoes to original legacy mixed state");
}
}
int main(){try{choiceAndHistory();emptyAndProducer();metadataAndGuards();disconnectAndProjection();audible();replay();legacyMigration();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
