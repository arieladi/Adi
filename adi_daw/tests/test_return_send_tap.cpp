// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/blob.hpp"
#include "adi/row_capture.hpp"
#include "adi/check.hpp"
#include "adi/digest.hpp"
#include "adi/engine/plan.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/return_send_tap.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "adi/textproj_store.hpp"
#include "adi/ui/track_edits.hpp"
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
void mixedClickAndHistory() {
    Project project;
    auto& store=*project.store;
    store.db().exec("ALTER TABLE routing_send_taps ADD COLUMN future BLOB DEFAULT X'';"
                    "INSERT INTO routing_send_taps(routing_id,tap,future) "
                    "VALUES(1,'pre_fx',X'00FF'),(2,'post_mixer',X'01FF'),(4,'post_fx',X'FF');");
    const auto original=digestProject(store).text;
    auto journal=OpJournal(store);
    check(journal.commit(clickReturnSendTap(10)).ok &&
          journal.count()==1 && readReturnSendTaps(store).at(10)==ReturnSendTap{false,true,false} &&
          project.scalar("SELECT count(*) FROM routing WHERE id IN(1,2) AND pre_fader=0")==2 &&
          project.scalar("SELECT count(*) FROM routing_send_taps WHERE routing_id IN(1,2)")==0 &&
          project.scalar("SELECT count(*) FROM routing_send_taps WHERE routing_id=4")==1,
          "one mixed switch click makes every incoming send Post");
    const auto uniformPost=digestProject(store).text;
    check(History(store).undo().ok && digestProject(store).text==original &&
          project.scalar("SELECT count(*) FROM routing_send_taps WHERE routing_id=1 AND future=X'00FF'")==1 &&
          project.scalar("SELECT count(*) FROM routing_send_taps WHERE routing_id=2 AND future=X'01FF'")==1,
          "one Undo restores the return's exact individual rows and mixed flags");
    check(History(store).redo().ok && digestProject(store).text==uniformPost,
          "one Redo repeats the original mixed-to-Post decision");
    check(journal.commit(clickReturnSendTap(10)).ok &&
          readReturnSendTaps(store).at(10)==ReturnSendTap{true,true,false} &&
          project.scalar("SELECT count(*) FROM routing WHERE id IN(1,2) AND pre_fader=1")==2,
          "the next switch click makes every send Pre");
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

void routingDependants() {
    for (const bool moveOut : {false, true}) {
        Project project;
        auto& store = *project.store;
        auto& db = store.db();
        db.exec("DELETE FROM routing;"
                "INSERT INTO tracks(id,kind,name) VALUES(30,'group','Group');"
                "UPDATE tracks SET parent_id=30 WHERE id IN(1,10);"
                "ALTER TABLE automation_lanes ADD COLUMN future BLOB;"
                "ALTER TABLE controller_maps ADD COLUMN future BLOB;"
                "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain,future)"
                " VALUES(101,'routing',41,'gain_db','real',X'0001FF');"
                "INSERT INTO controller_maps(id,target_kind,target_id,target_param,future)"
                " VALUES(102,'routing',41,'gain_db',X'AB00CD')");
        auto journal = OpJournal(store);
        const auto connected = journal.commit(connect(41));
        const auto tapped = journal.commit(op("routing.setSendTap", {{"id", 41}, {"tap", "pre_fx"}}));
        std::array<AutomationPoint, 2> points{};
        points[0].value = -4;
        points[0].point_id = 1;
        points[1].time = 5765760;
        points[1].value = -12;
        points[1].point_id = 2;
        const auto blob = writeStream<AutomationPoint>(FourCC::Automation, points, 1, StreamFlags::SortedByTime);
        if (!connected.ok || !tapped.ok || !store.putAutomationData(101, std::nullopt, blob)) {
            throw std::runtime_error("seed routing dependants");
        }
        const auto before = digestProject(store).text;
        const auto request = moveOut ? op("track.setParent", {{"id", 1}})
                                     : op("routing.disconnect", {{"id", 41}});
        const auto removed = journal.commit(request);
        const auto absent = [&] {
            return project.scalar("SELECT COUNT(*) FROM routing WHERE id=41") == 0 &&
                project.scalar("SELECT COUNT(*) FROM routing_send_taps WHERE routing_id=41") == 0 &&
                project.scalar("SELECT COUNT(*) FROM automation_lanes WHERE id=101") == 0 &&
                project.scalar("SELECT COUNT(*) FROM automation_data WHERE lane_id=101") == 0 &&
                project.scalar("SELECT COUNT(*) FROM controller_maps WHERE id=102") == 0;
        };
        check(removed.ok && absent() && checkProject(store).clean(),
              moveOut ? "a move that loses a send deletes its lanes, streams and maps"
                      : "disconnect deletes a route's lanes, streams and maps");
        if (!removed.ok) return;

        const auto after = digestProject(store).text;
        check(History(store).undo().ok && digestProject(store).text == before &&
              project.scalar("SELECT future=X'0001FF' FROM automation_lanes WHERE id=101") == 1 &&
              project.scalar("SELECT future=X'AB00CD' FROM controller_maps WHERE id=102") == 1,
              "Undo restores all routing owners and their extension cells with exact IDs");
        check(History(store).redo().ok && absent() && digestProject(store).text == after,
              "Redo removes the same complete routing image");
        check(journal.commit(connect(41, moveOut ? 11 : 10)).ok &&
              project.scalar("SELECT COUNT(*) FROM automation_lanes WHERE owner_kind='routing' AND owner_id=41") == 0 &&
              project.scalar("SELECT COUNT(*) FROM controller_maps WHERE target_kind='routing' AND target_id=41") == 0,
              "reused routing ID inherits no lane or controller map");
    }
}

void legacyDisconnectReplay() {
    Project project;
    auto& store = *project.store;
    auto& db = store.db();
    db.exec("DELETE FROM routing;"
            "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain)"
            " VALUES(101,'routing',41,'gain_db','real')");
    auto journal = OpJournal(store);
    if (!journal.commit(connect(41)).ok) throw std::runtime_error("seed old route");
    const auto row = adi::detail::captureRow(db, "routing", "id", 41);
    const auto removed = journal.commit(op("routing.disconnect", {{"id", 41}}));
    if (!row || !removed.ok || !History(store).undo().ok) throw std::runtime_error("seed old replay");
    const Payload legacy{{"id", 41}, {"srcKind", "track"}, {"src", 1}, {"dstKind", "track"},
                         {"dst", 10}, {"kind", "send"}, {"row", *row}};
    const auto bytes = encodePayload(legacy);
    SQLite::Statement old(db, "UPDATE ops SET inverse=? WHERE seq=?");
    old.bind(1, bytes.data(), static_cast<int>(bytes.size()));
    old.bind(2, removed.seqs.front());
    old.exec();
    check(History(store).redo().ok && project.scalar("SELECT COUNT(*) FROM automation_lanes WHERE id=101") == 1 &&
          History(store).undo().ok && project.scalar("SELECT COUNT(*) FROM routing WHERE id=41") == 1,
          "historic row-only disconnect replays its original effects and inverse");
}

void inaudibleSendSources() {
    Project project;
    auto& store = *project.store;
    auto& db = store.db();
    db.exec("INSERT INTO tracks(id,kind,parent_id) VALUES (30,'group',NULL),(32,'vca',30);"
            "UPDATE tracks SET parent_id=30 WHERE id=10;"
            "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind)"
            " VALUES (43,'track',32,'track',10,'send');");
    const auto captured = adi::detail::captureRow(db, "routing", "id", 43);
    if (!captured) throw std::runtime_error("seed captured VCA send");
    db.exec("DELETE FROM routing WHERE id=43");

    auto main = connect(41, 11);
    main.payload["src"] = 20;
    auto vca = connect(42);
    vca.payload["src"] = 32;
    auto capturedVca = vca;
    capturedVca.payload["id"] = 43;
    capturedVca.payload["row"] = *captured;

    const auto before = digestProject(store).text;
    OpJournal journal(store);
    const auto head = journal.headSeq();
    const auto preparedMain = prepareSendToReturn(store, 41, 20, 11);
    const auto preparedVca = prepareSendToReturn(store, 42, 32, 10);
    check(!preparedMain.ok && preparedMain.error.find("Main") != std::string::npos &&
              preparedMain.requests.empty(),
          "send preparation refuses Main for a top-level return");
    check(!preparedVca.ok && preparedVca.error.find("VCA") != std::string::npos &&
              preparedVca.requests.empty(),
          "send preparation refuses a VCA inside the return's group");
    const auto rawMain = journal.commit(main);
    const auto rawVca = journal.commit(vca);
    const auto rowVca = journal.commit(capturedVca);
    check(!rawMain.ok && rawMain.error.find("Main") != std::string::npos,
          "the raw routing op refuses Main as a send source");
    check(!rawVca.ok && rawVca.error.find("VCA") != std::string::npos,
          "the raw routing op refuses VCA as a send source");
    check(!rowVca.ok && rowVca.error.find("VCA") != std::string::npos,
          "a fresh captured send row is subject to the same VCA source check");
    check(journal.headSeq() == head && digestProject(store).text == before,
          "refused sends leave routing and the journal unchanged");

    Project legacy;
    auto& older = *legacy.store;
    auto historical = connect(44, 11);
    historical.payload["src"] = 2;
    if (!OpJournal(older).commit(historical).ok || !History(older).undo().ok)
        throw std::runtime_error("seed a previously accepted send");
    // Model a stored send that the older journal accepted from a VCA.
    older.db().exec("UPDATE tracks SET kind='vca' WHERE id=2");
    const auto path = older.path();
    legacy.store.reset();
    StoreError error{};
    legacy.store = Store::open(path, error);
    if (!legacy.store) throw std::runtime_error(toString(error));
    auto& reopened = *legacy.store;
    check(History(reopened).redo().ok &&
              legacy.scalar("SELECT src_id FROM routing WHERE id=44") == 2,
          "a saved historical VCA send still replays its forward connect");
    const auto row = adi::detail::captureRow(reopened.db(), "routing", "id", 44);
    check(row && OpJournal(reopened).commit(op("routing.disconnect", {{"id",44}})).ok &&
              History(reopened).undo().ok &&
              adi::detail::captureRow(reopened.db(), "routing", "id", 44) == row,
          "undoing a disconnect still restores the exact historical VCA send row");
}

void groupedReturns() {
    Project project;
    auto& store = *project.store;
    store.db().exec(
        "DELETE FROM routing;"
        "INSERT INTO tracks(id,kind,name,parent_id) VALUES"
        "(30,'group','Outer',NULL),(31,'group','Inner',30),(32,'audio','Outside',NULL),"
        "(33,'return','Inner FX',31),(34,'return','Outer FX 2',30);"
        "UPDATE tracks SET parent_id=30 WHERE id IN(1,10);"
        "UPDATE tracks SET parent_id=31 WHERE id=2;"
        "INSERT OR IGNORE INTO mixer_strip(track_id) SELECT id FROM tracks");
    const auto send = [](std::int64_t id, std::int64_t source, std::int64_t destination) {
        return op("routing.connect", {{"id", id}, {"srcKind", "track"}, {"src", source},
                                       {"dstKind", "track"}, {"dst", destination}, {"kind", "send"}});
    };
    auto journal = OpJournal(store);
    check(journal.commit(send(41, 1, 10)).ok && journal.commit(send(42, 2, 10)).ok,
          "both the direct and nested child can send to the outer return");
    check(!journal.commit(send(43, 32, 10)).ok && !journal.commit(send(43, 30, 10)).ok &&
          !prepareSendToReturn(store, 43, 32, 10).ok,
          "an outside track and the group itself cannot create a send to its grouped return");
    check(journal.commit(send(44, 10, 34)).ok && journal.commit(send(45, 33, 10)).ok &&
          journal.commit(send(46, 33, 11)).ok,
          "grouped returns can send to their siblings, an outer return, and a top-level return");
    check(journal.commit(tap(10, true)).ok &&
          journal.commit(op("routing.setSendTap", {{"id", 41}, {"tap", "pre_fx"}})).ok &&
          journal.commit(op("routing.setSendTap", {{"id", 42}, {"tap", "post_mixer"}})).ok,
          "an individual send can override the return's Post FX default");
    const auto snapshot = SnapshotBuilder::fromStore(store);
    check(snapshot->findTrack(10)->returnSendTap->legacyMixed,
          "the snapshot exposes a return with unequal effective taps as mixed");
    const auto plan = planGraph(rows::readModel(store));
    const auto edgeTap = [&](std::int64_t id) -> std::optional<Tap> {
        const auto edge = std::find_if(plan.edges.begin(), plan.edges.end(),
            [id](const PlannedEdge& candidate) { return candidate.routingId == id; });
        return edge == plan.edges.end() ? std::nullopt : std::optional(edge->tap);
    };
    check(edgeTap(41) == Tap::PreFx && edgeTap(42) == Tap::PostFader && edgeTap(45) == Tap::PostFx,
          "the audio plan selects each of the three physical taps");
    check(textproj::projectStore(store).text.find("sendTap pre_fx") != std::string::npos,
          "text projection stores the individual tap");
    const auto before = digestProject(store).text;
    check(journal.commit(op("track.setParent", {{"id", 31}})).ok &&
          project.scalar("SELECT count(*) FROM routing WHERE id IN(42,45)") == 0 &&
          project.scalar("SELECT count(*) FROM routing WHERE id=46") == 1,
          "moving a nested group removes scoped sends and keeps its top-level send");
    check(History(store).undo().ok && digestProject(store).text == before &&
          project.scalar("SELECT count(*) FROM routing_send_taps WHERE routing_id=42 AND tap='post_mixer'") == 1,
          "Undo restores the exact routes and their individual taps");
    check(journal.commit(op("track.delete", {{"id", 30}})).ok &&
          project.scalar("SELECT count(*) FROM routing") == 0,
          "deleting the group removes every attached send");
    check(History(store).undo().ok && digestProject(store).text == before,
          "Undo restores the group and its sends in one step");
    auto duplicate = std::vector<OpRequest>{
        op("track.create", {{"id", 60}, {"kind", "group"}}),
        op("track.create", {{"id", 61}, {"kind", "audio"}, {"parent", 60}}),
        op("track.create", {{"id", 62}, {"kind", "return"}, {"parent", 60}}),
    };
    const auto copies = copyGroupSendRequests(store, {{30, 60}, {1, 61}, {10, 62}}, {{41, 71}});
    duplicate.insert(duplicate.end(), copies.begin(), copies.end());
    check(journal.commit(duplicate).ok &&
          project.scalar("SELECT count(*) FROM routing WHERE id=71 AND src_id=61 AND dst_id=62") == 1 &&
          project.scalar("SELECT count(*) FROM routing_send_taps WHERE routing_id=71 AND tap='pre_fx'") == 1,
          "a copied send points to the copied return and retains its tap");
    check(History(store).undo().ok && digestProject(store).text == before,
          "the copied send and its tracks undo as one group");
    store.db().exec("INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind)"
                    " VALUES(55,'track',32,'track',10,'send')");
    const auto report = checkProject(store);
    check(std::any_of(report.findings.begin(), report.findings.end(),
        [](const auto& finding) { return finding.code == "route.groupReturnScope"; }),
        "integrity checking identifies a grouped send inserted from outside the group");
}

void ungroupKeepsSendsAgainstFinalForest() {
    Project project;
    auto& store = *project.store;
    store.db().exec("DELETE FROM routing;"
        "INSERT INTO tracks(id,kind,name,parent_id,index_in_parent) VALUES"
        "(30,'group','Group',NULL,3),(31,'group','Nested',30,2),"
        "(33,'audio','Nested audio',31,0),(34,'return','Nested return',31,1);"
        "UPDATE tracks SET parent_id=30,index_in_parent=0 WHERE id=1;"
        "UPDATE tracks SET parent_id=30,index_in_parent=1 WHERE id=2;"
        "UPDATE tracks SET parent_id=30,index_in_parent=3 WHERE id=10;"
        "UPDATE tracks SET parent_id=30,index_in_parent=4 WHERE id=11;"
        "ALTER TABLE routing_send_taps ADD COLUMN future BLOB;");
    auto journal = OpJournal(store);
    const auto send = [&](int id, int source, int target, const char* mode) {
        auto request = connect(id, target);
        request.payload["src"] = source;
        if (!journal.commit(request).ok ||
            !journal.commit(op("routing.setSendTap", {{"id", id}, {"tap", mode}})).ok)
            throw std::runtime_error("write Ungroup send fixture");
    };
    send(41, 1, 10, "pre_fx");
    send(42, 2, 11, "post_fx");
    send(43, 33, 10, "post_mixer");
    send(44, 10, 11, "post_fx");
    send(45, 11, 10, "pre_fx");
    send(46, 33, 34, "pre_fx");
    store.db().exec("UPDATE routing_send_taps SET future=X'00FEFF' WHERE routing_id IN(41,45)");
    const auto before = digestProject(store).text;
    const auto snapshot = SnapshotBuilder::fromStore(store);
    const auto command = adi::ui::ungroupTrack(snapshot->tracks, 30);
    const auto result = journal.commit(command.ops);
    check(command.refusal.empty() && result.ok && result.seqs.size() == command.ops.size() &&
          project.scalar("SELECT COUNT(*) FROM routing WHERE id BETWEEN 41 AND 46") == 6 &&
          project.scalar("SELECT COUNT(*) FROM routing_send_taps WHERE future=X'00FEFF'") == 2,
          "normal Ungroup keeps both return directions, nested sends and their exact tap rows");
    check(project.scalar("SELECT COUNT(*) FROM tracks WHERE id IN(1,2,10,11,31) AND parent_id IS NULL") == 5 &&
          project.scalar("SELECT COUNT(*) FROM tracks WHERE id IN(33,34) AND parent_id=31") == 2 &&
          checkProject(store).clean(), "Ungroup moves every peer without invalidating the send scope");
    const auto after = digestProject(store).text;
    check(History(store).undo().ok && digestProject(store).text == before,
          "Undo of Ungroup restores the exact tree, sends and tap bytes");
    check(History(store).redo().ok && digestProject(store).text == after,
          "recorded peer moves replay the same Ungroup through Redo");
}

class Tone final:public Node {
public:void process(const NodeIo& io)noexcept override {for(std::int32_t channel=0;channel<io.channels;++channel)if(io.out[channel])std::fill_n(io.out[channel]+io.blockOffset,io.frames,0.25f);}
};
void compensatedGroupNull() {
    class Pattern final : public Node {
    public:
        bool quiet = false;
        void process(const NodeIo& io) noexcept override {
            for (int i = 0; i < io.frames; ++i) {
                const float signal = quiet ? 0.0f
                    : (static_cast<float>((sample_++) % 257) - 128.0f) / 256.0f;
                for (int c = 0; c < io.channels; ++c) io.out[c][io.blockOffset + i] = signal;
            }
        }
    private:
        int sample_ = 0;
    } source;
    class FixtureDevice final : public device::DeviceInstance {
    public:
        explicit FixtureDevice(int delay, bool inverse) : delay_(delay), inverse_(inverse) {}
        const device::DeviceIdentity& identity() const noexcept override { return identity_; }
        bool loaded() const noexcept override { return true; }
        std::int32_t latencySamples() const noexcept override { return delay_; }
        std::int64_t tailSamples() const noexcept override { return delay_; }
        void process(const NodeIo& io) noexcept override {
            for (int i = 0; i < io.frames; ++i) {
                const auto offset = io.blockOffset + i;
                for (int c = 0; c < io.channels; ++c) {
                    const float input = io.in ? io.in[c][offset] : 0.0f;
                    if (delay_ == 0) {
                        peak = std::max(peak, std::abs(input));
                        io.out[c][offset] = std::clamp(input, -0.0625f, 0.0625f);
                    } else {
                        auto& retained = memory_[static_cast<std::size_t>(c)][static_cast<std::size_t>(head_)];
                        io.out[c][offset] = inverse_ ? -retained : retained;
                        retained = input;
                    }
                }
                if (delay_ != 0) head_ = (head_ + 1) % delay_;
            }
        }
        float peak = 0;
    private:
        device::DeviceIdentity identity_;
        std::array<std::array<float, 1024>, 2> memory_{};
        int delay_;
        bool inverse_;
        int head_ = 0;
    };
    Project project;
    auto& store = *project.store;
    store.db().exec(
        "DELETE FROM routing;"
        "INSERT INTO tracks(id,kind,name) VALUES(30,'group','Group');"
        "UPDATE tracks SET parent_id=30 WHERE id IN(1,10);"
        "INSERT INTO mixer_strip(track_id) VALUES(30);"
        "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'clap','test.group','Fixture','effect');"
        "INSERT INTO device_chains(id,track_id) VALUES(1,1),(2,10),(3,30);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name,always_process) VALUES"
        "(1,1,0,1,'Dry delay',1),(2,2,0,1,'Inverted return',1),(3,3,0,1,'Group clipper',1)");
    check(OpJournal(store).commit(connect(1)).ok &&
          OpJournal(store).commit(op("routing.setSendTap", {{"id", 1}, {"tap", "post_fx"}})).ok,
          "group null fixture connects the wet path at Post FX");
    FixtureDevice* clipper = nullptr;
    Session session;
    session.graph().setFadeFrames(0);
    session.setSourcesFor([&](std::int64_t id) {
        return id == 1 ? std::vector<Node*>{&source} : std::vector<Node*>{};
    });
    SessionSpec spec;
    spec.maxFrames = 64;
    const bool loaded = session.load(store, [&](const DeviceRequest& request, std::string&) {
        const auto id = request.device->id;
        auto device = std::make_unique<FixtureDevice>(id == 1 ? 1024 : id == 2 ? 512 : 0, id == 2);
        if (id == 3) clipper = device.get();
        return device;
    }, spec);
    check(loaded && clipper, "load the actual group, return and delay graph");
    if (!loaded || !clipper) return;
    session.transport().play();
    std::array<float,64> left{}, right{};
    float* channels[]{left.data(), right.data()};
    int streamFrame = 0;
    const auto renderPeak = [&](int blocks) {
        float peak = 0;
        for (int block = 0; block < blocks; ++block) {
            AudioIo io;
            io.out = channels;
            io.numOut = 2;
            io.frames = 64;
            io.streamTimeSamples = streamFrame;
            session.process(io);
            streamFrame += io.frames;
            for (const float value : left) peak = std::max(peak, std::abs(value));
        }
        return peak;
    };
    const float peak = renderPeak(64);
    check(clipper->peak < 1e-6f && peak < 1e-6f,
          "a 1024-sample dry delay and 512-sample inverted return cancel before the group device");
    const auto attenuated = OpJournal(store).commit(op("mixer.setVolume", {{"id", 1}, {"db", -120.0}}));
    check(attenuated.ok && session.refresh(store), "attenuate the dry fader while leaving Post FX wet");
    renderPeak(64);
    clipper->peak = 0;
    source.quiet = true;
    const float tail = renderPeak(16);
    check(clipper->peak > 0.2f && tail == 0.0625f,
          "the group clipper limits the remaining return tail after the source goes silent");
}

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
void switchingPreFxClearsSource() {
    Project project;
    auto& store = *project.store;
    store.db().exec("DELETE FROM routing;"
                    "UPDATE project SET sample_rate=48000 WHERE id=1;"
                    "UPDATE mixer_strip SET volume_db=-12,pan=1,delay_samples=480 WHERE track_id=1;"
                    "UPDATE tracks SET muted=1 WHERE id=1;"
                    "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain,default_value)"
                    " VALUES(201,'track',1,'volume','real',-18)");
    auto journal = OpJournal(store);
    if (!journal.commit(connect(41)).ok) throw std::runtime_error("seed switching send");
    const auto choose = [&](const char* value) {
        if (!journal.commit(op("routing.setSendTap", {{"id", 41}, {"tap", value}})).ok)
            throw std::runtime_error("write switching send tap");
    };

    Tone reusedTone;
    Session reused;
    reused.graph().setFadeFrames(0);
    reused.setSourcesFor([&](std::int64_t id) {
        return id == 1 ? std::vector<Node*>{&reusedTone} : std::vector<Node*>{};
    });
    SessionSpec spec;
    spec.sampleRate = 48000;
    spec.maxFrames = 64;
    if (!reused.load(store, {}, spec)) throw std::runtime_error("load switching send");
    reused.transport().play();
    const auto render = [](Session& session, int& frame) {
        std::array<float, 64> left{}, right{};
        float* output[]{left.data(), right.data()};
        for (int block = 0; block < 32; ++block) {
            AudioIo io;
            io.out = output;
            io.numOut = 2;
            io.frames = 64;
            io.streamTimeSamples = frame;
            session.process(io);
            frame += io.frames;
        }
        return std::array<float, 2>{left.back(), right.back()};
    };
    int reusedFrame = 0;
    render(reused, reusedFrame);

    MixerStrips liveControls, stagedControls;
    Transport transport;
    const auto post = rows::readModel(store);
    const auto bound = bindStripAutomation(compileAutomation(post, 48000.0), {});
    liveControls.sync(post, &transport, bound);
    stagedControls.sync(post, &transport, bound);
    auto* send = liveControls.sendFor(41);
    auto* stagedSend = stagedControls.sendFor(41);
    check(send && send->latencySamples() == -480 && send->captureControls().source.config.lanes,
          "the original post send follows Track Delay and strip automation");
    if (!send || !stagedSend) return;

    for (const bool pre : {true, false, true}) {
        choose(pre ? "pre_fx" : "post_mixer");
        const auto next = rows::readModel(store);
        liveControls.sync(next, &transport, bound);
        const auto source = send->captureControls().source;
        check(send == liveControls.sendFor(41) && send->latencySamples() == (pre ? 0 : -480) &&
              (source.config.lanes != nullptr) == !pre && source.audible == pre &&
              source.config.pan == (pre ? 0.0 : 1.0) && source.config.volumeDb == (pre ? 0.0 : -12.0),
              "the same send node changes source controls and compensation with its tap");
        if (!reused.refresh(store)) throw std::runtime_error("refresh switching send");
        const auto hot = render(reused, reusedFrame);

        Tone freshTone;
        Session fresh;
        fresh.graph().setFadeFrames(0);
        fresh.setSourcesFor([&](std::int64_t id) {
            return id == 1 ? std::vector<Node*>{&freshTone} : std::vector<Node*>{};
        });
        if (!fresh.load(store, {}, spec)) throw std::runtime_error("reopen switching send");
        fresh.transport().play();
        int freshFrame = 0;
        const auto cold = render(fresh, freshFrame);
        const float expected = pre ? 0.25f : 0.0f;
        check(hot == cold && hot == std::array{expected, expected},
              "Post to Pre FX and back matches a fresh playback session");
    }
    const auto pre = rows::readModel(store);
    stagedControls.prepareControls(pre, &transport, bound, true, {})->apply();
    const auto staged = stagedSend->captureControls().source;
    check(staged.audible && staged.config.volumeDb == 0 && staged.config.pan == 0 && !staged.config.lanes,
          "staged controls use the neutral Pre FX source");

    auto solo = pre;
    const auto other = std::find_if(solo.tracks.begin(), solo.tracks.end(),
                                    [](const auto& track) { return track.id == 2; });
    if (other == solo.tracks.end()) throw std::runtime_error("seed independent solo");
    other->soloed = true;
    HearingFlags flags;
    flags.preFollowsSolo = false;
    check(hearing(solo, planGraph(solo), flags).send.at(41),
          "Pre FX respects the setting that lets Pre sends ignore another track's solo");
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
int main(){try{choiceAndHistory();mixedClickAndHistory();emptyAndProducer();metadataAndGuards();disconnectAndProjection();routingDependants();legacyDisconnectReplay();inaudibleSendSources();groupedReturns();ungroupKeepsSendsAgainstFinalForest();compensatedGroupNull();audible();switchingPreFxClearsSource();replay();legacyMigration();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
