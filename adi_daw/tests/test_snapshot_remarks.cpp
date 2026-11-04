// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
const RemarkNode* remark(const Snapshot& frame,std::int64_t id) {
    for(const auto& node:frame.remarks)if(node->id==id)return node.get();
    return nullptr;
}
struct Project {
    test::TempDirectory directory{"snapshot_remarks","project"};std::unique_ptr<Store> store;
    Project() {
        StoreError error{};store=Store::create(directory.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','One'),(2,'audio','Two'),(100,'group','Group');"
            "INSERT INTO clips(id,track_id,kind,name,time_base,pos_ticks,length_ticks) VALUES(100,2,'midi','Clip',0,0,5765760);"
            "INSERT INTO device_chains(id,track_id,name) VALUES(11,1,'Root');"
            "INSERT INTO devices(id,chain_id,name,is_rack) VALUES(100,11,'Rack',1);"
            "INSERT INTO device_chains(id,parent_device_id,name) VALUES(12,100,'Nested');"
            "INSERT INTO devices(id,chain_id,name) VALUES(200,12,'Effect');"
            "INSERT INTO remarks(id,target_kind,target_id,author,text,created_utc) VALUES"
            "(1,'device',200,'user','Original',10),(7,'clip',100,'agent','Clip note',20),(99,'track',2,'user','Keep',30)");
    }
};
void fieldsAndSharing() {
    Project project;auto& store=*project.store;auto& db=store.db();
    const auto original=SnapshotBuilder::fromStore(store);auto frame=SnapshotBuilder::fromStore(store,*original);
    check(frame->remarks.size()==3 && remark(*frame,1)->trackId==1 && remark(*frame,7)->trackId==2 && remark(*frame,99)->trackId==2,
        "track, clip and nested-device anchors resolve to their own tracks");
    check(frame->remarks==original->remarks && SnapshotBuilder::sharedNodeCount(*frame,*original)==SnapshotBuilder::sharedNodeCount(*original,*original),
        "unchanged remarks share nodes and count alongside shared tracks/clips");
    const std::array<const char*,11> updates={
        "UPDATE remarks SET author='agent' WHERE id=1", "UPDATE remarks SET actor_detail='model' WHERE id=1",
        "UPDATE remarks SET text='Changed' WHERE id=1", "UPDATE remarks SET created_utc=11 WHERE id=1",
        "UPDATE remarks SET resolved=1 WHERE id=1", "UPDATE remarks SET param_id='' WHERE id=1",
        "UPDATE remarks SET param_id='gain' WHERE id=1", "UPDATE remarks SET param_id=NULL WHERE id=1",
        "UPDATE remarks SET target_id=100 WHERE id=1", "UPDATE remarks SET target_kind='clip' WHERE id=1",
        "UPDATE remarks SET target_kind='track' WHERE id=1"};
    for(const char* update:updates) {
        auto previous=std::move(frame);db.exec(update);frame=SnapshotBuilder::fromStore(store,*previous);
        check(remark(*frame,1)!=remark(*previous,1) && remark(*frame,99)==remark(*previous,99),"every raw field participates in sharing, unchanged remarks stay shared");
        if(std::string_view(update).find("param_id='gain'")!=std::string_view::npos)
            check(remark(*frame,1)->trackId==1 && !remark(*frame,1)->anchorMissing && db.execAndGet("SELECT count(*) FROM plugin_params").getInt()==0,
                "a parameter anchor needs no mirror row before its first touch");
    }
    const auto* old=remark(*original,1);
    check(old->author=="user" && old->actorDetail.empty() && old->text=="Original" && old->createdUtc==10 && !old->resolved && !old->paramId && old->targetKind=="device" && old->targetId==200 && old->trackId==1,
        "old published frame remains immutable across all remark edits");
    auto previous=std::move(frame);db.exec("UPDATE remarks SET id=3 WHERE id=1");frame=SnapshotBuilder::fromStore(store,*previous);
    check(!remark(*frame,1) && remark(*frame,3) && remark(*frame,3)->id==3 && remark(*previous,1),"persistent remark identity changes rebuild its node");
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(1099511627776,'audio','Big'); INSERT INTO remarks(id,target_kind,target_id,author,text,created_utc) VALUES(8796093022208,'track',1099511627776,'user','Full width',9223372036854775806)");
    auto wide=SnapshotBuilder::fromStore(store,*frame);const auto* large=remark(*wide,8796093022208LL);
    check(large && large->targetId==1099511627776LL && large->trackId==1099511627776LL && large->createdUtc==9223372036854775806LL,
        "anchor identities and creation times retain full signed 64-bit width");
    db.exec("DELETE FROM remarks");auto empty=SnapshotBuilder::fromStore(store,*wide);
    check(empty->remarks.empty() && !wide->remarks.empty(),"removal clears current snapshot only");
}
void ownershipAndOrphans() {
    Project project;auto& store=*project.store;auto first=SnapshotBuilder::fromStore(store);
    store.db().exec("UPDATE device_chains SET track_id=2 WHERE id=11");auto moved=SnapshotBuilder::fromStore(store,*first);
    check(remark(*moved,1)->trackId==2 && remark(*first,1)->trackId==1 && remark(*moved,1)!=remark(*first,1),"device reparenting updates derived ownership even with unchanged remark bytes");
    store.db().exec("UPDATE device_chains SET track_id=NULL,parent_device_id=200 WHERE id=11");auto cycle=SnapshotBuilder::fromStore(store,*moved);
    check(remark(*cycle,1)->anchorMissing && !remark(*cycle,1)->trackId && remark(*cycle,1)->text=="Original", "cyclic rack ownership remains an unresolved remark without recursion or data loss");
    store.db().exec("UPDATE device_chains SET track_id=2,parent_device_id=NULL WHERE id=11");auto repaired=SnapshotBuilder::fromStore(store,*cycle);
    check(!remark(*repaired,1)->anchorMissing && remark(*repaired,1)->trackId==2 && remark(*cycle,1)->anchorMissing,"repairing ownership resolves only the new frame");
    store.db().exec("DELETE FROM devices WHERE id=200");auto orphan=SnapshotBuilder::fromStore(store,*repaired);
    check(remark(*orphan,1)->anchorMissing && !remark(*orphan,1)->trackId && orphan->remarks.size()==3,"deleted device anchors remain visible with full metadata");
    OpRequest add;add.opType="remark.add";add.payload={{"id",5},{"kind","track"},{"target",999},{"author","agent"},{"detail","model"},{"text","Orphan"},{"created",40}};
    check(OpJournal(store).commit(add).ok,"existing remark op permits weak orphan anchors");
    auto added=SnapshotBuilder::fromStore(store,*orphan);
    check(remark(*added,5)->anchorMissing && remark(*added,5)->author=="agent" && remark(*added,5)->actorDetail=="model","snapshot exposes native add and author provenance without reading the op actor");
    check(History(store).undo().ok && !remark(*SnapshotBuilder::fromStore(store,*added),5) && History(store).redo().ok && remark(*SnapshotBuilder::fromStore(store),5),"snapshot follows native remark undo and redo");
}
struct TraceMutation {SQLite::Database* writer=nullptr;bool fired=false,failed=false;};
int mutateDuringRead(unsigned kind,void* opaque,void* statement,void*) {
    auto& mutation=*static_cast<TraceMutation*>(opaque);
    if(kind!=SQLITE_TRACE_STMT || mutation.fired)return 0;
    const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
    if(!sql || std::string_view(sql).find("created_utc,resolved FROM remarks ORDER BY id")==std::string_view::npos)return 0;
    mutation.fired=true;
    try {SQLite::Transaction write(*mutation.writer);mutation.writer->exec("UPDATE device_chains SET track_id=2 WHERE id=11;UPDATE remarks SET text='After' WHERE id=1");write.commit();}
    catch(const std::exception&){mutation.failed=true;}
    return 0;
}
void coherentRead() {
    Project project;auto& store=*project.store;
    SQLite::Database writer(store.path().string(),SQLite::OPEN_READWRITE);TraceMutation mutation{&writer};
    sqlite3_trace_v2(store.db().getHandle(),SQLITE_TRACE_STMT,mutateDuringRead,&mutation);
    auto before=SnapshotBuilder::fromStore(store);
    sqlite3_trace_v2(store.db().getHandle(),0,nullptr,nullptr);
    check(mutation.fired && !mutation.failed && remark(*before,1)->text=="Original" && remark(*before,1)->trackId==1,
        "one snapshot sees a coherent WAL state when another writer commits between ownership and remark reads");
    auto after=SnapshotBuilder::fromStore(store,*before);
    check(remark(*after,1)->text=="After" && remark(*after,1)->trackId==2,"next rebuild sees the writer's complete new state");
    {
        SQLite::Transaction outer(store.db());store.db().exec("UPDATE tracks SET name='Uncommitted' WHERE id=1");
        const auto borrowed=SnapshotBuilder::fromStore(store);
        check(sqlite3_get_autocommit(store.db().getHandle())==0 && borrowed->findTrack(1)->name=="Uncommitted","snapshot borrows an outer transaction without committing it");
    }
    check(SnapshotBuilder::fromStore(store)->findTrack(1)->name=="One","caller rollback still owns the transaction's lifetime");
}
}
int main(){try{fieldsAndSharing();ownershipAndOrphans();coherentRead();}catch(const std::exception& exception){check(false,exception.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
