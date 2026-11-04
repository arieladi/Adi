// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/track_recording.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/textproj_store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
struct Project {
    test::TempDirectory directory{"track_recording","project"};std::unique_ptr<Store> store;
    Project(){StoreError error{};store=Store::create(directory.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Keys'),(2,'audio','Audio')");}
};
CommitResult set(Store& store,const char* type,Payload payload){OpRequest request;request.opType=type;request.payload=std::move(payload);return OpJournal(store).commit(request);}
void historyAndSharing() {
    Project project;auto& store=*project.store;auto& db=store.db();
    const auto first=SnapshotBuilder::fromStore(store);
    check(first->findTrack(1)->recording==TrackRecordingOptions{} && readTrackRecordingOptions(store).empty(),"old/missing rows expose 1/16 Rec-Q, free Play-Q and retained monitoring latency without writes");
    check(set(store,"track.setRecordQuantize",{{"id",1},{"on",false}}).ok && History(store).undo().ok &&
        db.execAndGet("SELECT count(*) FROM track_recording_options").getInt()==0,"undo of first policy touch restores absence, not a default row");
    check(History(store).redo().ok && !readTrackRecordingOptions(store).at(1).recordEnabled,"redo restores the captured per-track switch");
    check(History(store).undo().ok,"policy sharing fixture returns to absent defaults");
    std::shared_ptr<const TrackNode> oldTrack=first->tracks.front();auto frame=SnapshotBuilder::fromStore(store);
    const std::array<std::pair<const char*,Payload>,6> edits={{{"track.setRecordQuantize",{{"id",1},{"on",false}}},
        {"track.setRecordQuantize",{{"id",1},{"on",false},{"grid","1/8"}}},
        {"track.setPlayQuantize",{{"id",1},{"on",true}}},
        {"track.setPlayQuantize",{{"id",1},{"on",true},{"grid","1/32"}}},
        {"track.setPlayQuantize",{{"id",1},{"on",true},{"forgivenessMs",30.0}}},
        {"track.setKeepMonitoringLatency",{{"id",1},{"on",false}}}}};
    for(const auto& [type,payload]:edits) {
        const auto previous=std::move(frame);check(set(store,type,payload).ok,"policy op commits");frame=SnapshotBuilder::fromStore(store,*previous);
        check(frame->findTrack(1)!=previous->findTrack(1) && frame->findTrack(2)==previous->findTrack(2),"each recording field changes only its owning track's shared node");
    }
    const auto options=frame->findTrack(1)->recording;
    check(!options.recordEnabled && options.recordGrid=="1/8" && options.playEnabled && options.playGrid=="1/32" && options.playForgivenessMs==30 && !options.keepMonitoringLatency &&
        oldTrack->recording==TrackRecordingOptions{},"snapshot exposes every policy field and old frames stay immutable");
    const auto projection=textproj::projectStore(store).text;
    check(rows::readModel(store).tracks.front().recording==options && projection.find("recQuantizeOff")!=std::string::npos &&
        projection.find("recGrid")!=std::string::npos && projection.find("playQuantize")!=std::string::npos &&
        projection.find("playGrid")!=std::string::npos && projection.find("playForgivenessMs 30")!=std::string::npos &&
        projection.find("discardMonitoringLatency")!=std::string::npos,"row model and deterministic text projection expose all non-default recording policy");
    auto shared=SnapshotBuilder::fromStore(store,*frame);check(shared->tracks==frame->tracks,"unchanged recording policy shares nodes");
    db.exec("ALTER TABLE track_recording_options ADD COLUMN future_data BLOB");db.exec("UPDATE track_recording_options SET future_data=x'00ff00'");
    check(set(store,"track.setKeepMonitoringLatency",{{"id",1},{"on",true}}).ok,"capture complete future recording row");
    db.exec("UPDATE track_recording_options SET future_data=x'ab'");
    check(History(store).undo().ok && db.execAndGet("SELECT hex(future_data) FROM track_recording_options").getString()=="00FF00" && !readTrackRecordingOptions(store).at(1).keepMonitoringLatency,"inverse restores full typed future BLOB and original policy");
    OpContext context{store,db};bool futureIgnored=false;std::string replayError;
    for(const auto& op:trackRecordingOps())if(op.name=="track.setKeepMonitoringLatency") {
        futureIgnored=op.apply(context,{{"id",2},{"on",false},{"grid","future meaning"},{"forgivenessMs",999.0}},replayError);
    }
    check(futureIgnored && readTrackRecordingOptions(store).at(2).playGrid=="1/16" &&
        readTrackRecordingOptions(store).at(2).playForgivenessMs==0,"old log reader ignores future fields outside each op's declared contract");
    const auto count=OpJournal(store).count();
    check(!set(store,"track.setRecordQuantize",{{"id",999},{"on",true}}).ok &&
        !set(store,"track.setRecordQuantize",{{"id",1},{"on",true},{"grid","1/7"}}).ok &&
        !set(store,"track.setPlayQuantize",{{"id",1},{"on",true},{"forgivenessMs",-1.0}}).ok &&
        !set(store,"track.setPlayQuantize",{{"id",1},{"on",true},{"forgivenessMs",std::numeric_limits<double>::infinity()}}).ok &&
        !set(store,"track.setKeepMonitoringLatency",{{"id",std::numeric_limits<std::uint64_t>::max()},{"on",true}}).ok && OpJournal(store).count()==count,"missing IDs, bad grids/nonfinite forgiveness and overflowing IDs refuse atomically");
    const auto file=store.path();project.store.reset();StoreError error{};project.store=Store::open(file,error);
    check(project.store && readTrackRecordingOptions(*project.store).at(1)==options,"track policy survives close/reopen");
}
void rootDefaults() {
    test::TempDirectory directory{"track_recording","root"};StoreError error{};
    auto store=Store::createNewProject(directory.path()/"root.adi",error,"off",false);
    check(store && !History(*store).canUndo() && OpJournal(*store).count()==0 &&
        readTrackRecordingOptions(*store).size()==4 &&
        !SnapshotBuilder::fromStore(*store)->findTrack(2)->recording.recordEnabled &&
        !SnapshotBuilder::fromStore(*store)->findTrack(5)->recording.keepMonitoringLatency,
        "New captures inherited policy for all four initial tracks in the history root");
    const auto invalid=directory.path()/"invalid.adi";
    check(!Store::createNewProject(invalid,error,"bad choice",true) && !std::filesystem::exists(invalid),
        "invalid New recording defaults create no partial file");
}
void inheritedDefaultsAndLegacy() {
    Project project;auto& store=*project.store;
    auto requests=trackRecordingDefaults(3,"1/8",false);OpRequest create;create.opType="track.create";create.payload={{"id",3},{"kind","audio"},{"name","New"}};requests.insert(requests.begin(),create);
    check(OpJournal(store).commit(requests).ok && readTrackRecordingOptions(store).at(3).recordGrid=="1/8" && !readTrackRecordingOptions(store).at(3).keepMonitoringLatency,"new-track batch captures explicit app defaults rather than reading them on replay");
    check(History(store).undo().ok && store.db().execAndGet("SELECT count(*) FROM tracks WHERE id=3").getInt()==0 &&
        store.db().execAndGet("SELECT count(*) FROM track_recording_options").getInt()==0,"one undo removes inherited policy and new track together");
    check(History(store).redo().ok,"new-track defaults replay as captured");
    const auto free=trackRecordingDefaults(4,"off",true);check(!free.front().payload.at("on").get<bool>(),"off registry default creates a Free track explicitly");
    bool refused=false;try{(void)trackRecordingDefaults(4,"unexpected",true);}catch(const std::exception&){refused=true;}check(refused,"unknown registry defaults refuse instead of guessing");
    // This minor added only the optional table: removing it reconstructs the
    // frozen predecessor, with no other native DDL changes.
    {auto gone=adi::tablesAddedAfter(9);for(auto it=gone.rbegin();it!=gone.rend();++it) store.db().exec("DROP TABLE "+*it);} /* every table since 1.9 (chain) */store.db().exec("PRAGMA user_version=1009");store.db().exec("UPDATE adi_meta SET value='9' WHERE key='schema_minor'");
    const auto path=store.path();project.store.reset();StoreError error{};
    project.store=Store::open(path,error,true);
    check(project.store && project.store->schemaMinor()==9 && readTrackRecordingOptions(*project.store).empty() &&
        SnapshotBuilder::fromStore(*project.store)->findTrack(1)->recording==TrackRecordingOptions{},"read-only 1.9 file remains old and exposes policy defaults");
    project.store.reset();project.store=Store::open(path,error);
    check(project.store && project.store->schemaMinor()==adi::kSchemaMinor && project.store->upgradedFromMinor()==9 && readTrackRecordingOptions(*project.store).empty(),"writing upgrades 1.9 additively without creating fake per-track overrides");
}
}
int main(){try{historyAndSharing();rootDefaults();inheritedDefaultsAndLegacy();}catch(const std::exception& exception){check(false,exception.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
