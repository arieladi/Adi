// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
struct Mutation {SQLite::Database* writer=nullptr;bool fired=false,failed=false;};
int mutateDuringRead(unsigned kind,void* opaque,void* statement,void*) {
    auto& mutation=*static_cast<Mutation*>(opaque);if(kind!=SQLITE_TRACE_STMT || mutation.fired)return 0;
    const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
    if(!sql || std::string_view(sql).find("FROM routing WHERE kind='send' ORDER BY id")==std::string_view::npos)return 0;
    mutation.fired=true;
    try {SQLite::Transaction transaction(*mutation.writer);mutation.writer->exec("INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) VALUES(1000,'track',1,'track',2,'main'),(900,'track',1,'track',2,'send')");transaction.commit();}
    catch(const std::exception&){mutation.failed=true;}
    return 0;
}
void run() {
    test::TempDirectory directory{"routing_ids","project"};StoreError error{};auto store=Store::create(directory.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Track'),(2,'return','Return')");
    auto frame=SnapshotBuilder::fromStore(*store);check(frame->maxRoutingId==0,"no routing has highest id zero");
    const std::array<const char*,5> kinds={"main","send","sidechain","vca","cue"};
    std::int64_t id=100;
    for(const char* kind:kinds) {
        SQLite::Statement row(store->db(),"INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,enabled) VALUES(?,'track',1,'hw_out',9223372036854775807,?,0)");row.bind(1,id);row.bind(2,kind);row.exec();
        auto next=SnapshotBuilder::fromStore(*store,*frame);
        check(next->maxRoutingId==id && next->findTrack(1)==frame->findTrack(1),"every routing kind, including disabled/external, updates max without replacing track nodes");
        frame=std::move(next);id+=100;
    }
    check(frame->sends.size()==1 && frame->sends.front()->id==200 && frame->maxRoutingId==500,"highest id includes non-send rows and excludes endpoint numbers");
    store->db().exec("DELETE FROM routing WHERE id=500");auto lower=SnapshotBuilder::fromStore(*store,*frame);
    check(lower->maxRoutingId==400 && frame->maxRoutingId==500 && lower->sends==frame->sends,"deleting highest non-send changes root metadata only; old frame and send sharing remain");
    store->db().exec("INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) VALUES(1099511627776,'hw_in',0,'track',1,'main')");
    auto wide=SnapshotBuilder::fromStore(*store,*lower);check(wide->maxRoutingId==1099511627776LL,"highest routing id retains all 64-bit identity bits");
    store->db().exec("INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) VALUES(9223372036854775807,'hw_in',0,'track',1,'cue')");
    auto full=SnapshotBuilder::fromStore(*store,*wide);check(full->maxRoutingId==std::numeric_limits<std::int64_t>::max(),"INT64_MAX remains exact so caller can refuse allocation overflow");
    store->db().exec("DELETE FROM routing;INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) VALUES(-7,'track',1,'track',2,'send')");
    auto negative=SnapshotBuilder::fromStore(*store,*full);check(negative->maxRoutingId==0,"legacy negative ids cannot become positive-id allocation candidates");
    store->db().exec("DELETE FROM routing");
    OpRequest request;request.opType="routing.connect";request.payload={{"id",800},{"srcKind","track"},{"src",1},{"dstKind","track"},{"dst",2},{"kind","sidechain"}};
    check(OpJournal(*store).commit(request).ok && SnapshotBuilder::fromStore(*store)->maxRoutingId==800,"native routing op updates highest id even when snapshot sends are empty");
    check(History(*store).undo().ok && SnapshotBuilder::fromStore(*store)->maxRoutingId==0,"routing undo resets highest id");
    check(History(*store).redo().ok && SnapshotBuilder::fromStore(*store)->maxRoutingId==800,"routing redo restores highest id");
    SQLite::Database writer(store->path().string(),SQLite::OPEN_READWRITE);Mutation mutation{&writer};
    sqlite3_trace_v2(store->db().getHandle(),SQLITE_TRACE_STMT,mutateDuringRead,&mutation);
    auto coherent=SnapshotBuilder::fromStore(*store);
    sqlite3_trace_v2(store->db().getHandle(),0,nullptr,nullptr);
    check(mutation.fired && !mutation.failed && coherent->maxRoutingId==800 && coherent->sends.empty(),"WAL writer cannot mix old maximum with newer send rows in one snapshot");
    auto next=SnapshotBuilder::fromStore(*store,*coherent);
    check(next->maxRoutingId==1000 && next->sends.size()==1 && next->sends.front()->id==900,"next snapshot sees the complete new routing state");

}
}
int main(){try{run();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
