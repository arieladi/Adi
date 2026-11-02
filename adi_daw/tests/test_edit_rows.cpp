// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/edit_rows.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <array>
#include <stdexcept>

namespace {
using namespace adi;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
struct Project {
    test::TempDirectory temporary{"edit_rows","changes"};std::unique_ptr<Store> store;
    Project(){StoreError error{};store=Store::create(temporary.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','Edits')");
        store->db().exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(7,21,'audio',0,100),(8,21,'audio',0,100)");
    }
    bool apply(const Payload& changes,std::string& error) {
        SQLite::Transaction transaction(store->db());OpContext context{*store,store->db()};
        if(!edit::applyRows(context,{{"changes",changes}},error))return false;
        transaction.commit();return true;
    }
    Payload inverse(const Payload& changes) {
        OpContext context{*store,store->db()};Payload result;std::string error;
        if(!edit::inverseRows(context,{{"changes",changes}},result,error))throw std::runtime_error(error);
        return result.at("changes");
    }
};
void updatesAndConflicts() {
    Project p;auto& db=p.store->db();edit::RowEdits plan(db);
    auto clip=plan.current("clips",7);(*clip)["name"]="Edited";plan.set("clips",7,clip);
    auto again=plan.current("clips",7);(*again)["length_ticks"]=200;plan.set("clips",7,again);
    const auto changes=plan.changes();std::string error;
    check(changes.size()==1 && changes[0].at("before").at("name")=="" && changes[0].at("after").at("length_ticks")==200,"multiple planned fields retain one original row image");
    check(p.apply(changes,error) && db.execAndGet("SELECT name FROM clips WHERE id=7").getString()=="Edited","captured edit applies");
    check(p.apply(p.inverse(changes),error) && db.execAndGet("SELECT length_ticks FROM clips WHERE id=7").getInt64()==100,"swapped packet restores exact original state");
    db.exec("UPDATE clips SET color=12 WHERE id=7");
    check(!p.apply(changes,error) && error.find("changed after")!=std::string::npos,"stale preparation refuses to overwrite an intervening edit");
    check(db.execAndGet("SELECT name FROM clips WHERE id=7").getString()=="" && db.execAndGet("SELECT color FROM clips WHERE id=7").getInt()==12,"stale refusal leaves all newer values");
    edit::RowEdits changed(db);auto first=changed.current("clips",7);(*first)["name"]="Would roll back";changed.set("clips",7,first);
    auto second=changed.current("clips",8);(*second)["track_id"]=999;changed.set("clips",8,second);
    check(!p.apply(changed.changes(),error) && db.execAndGet("SELECT name FROM clips WHERE id=7").getString()=="","late SQL constraint failure rolls back earlier row updates");
    edit::RowEdits fresh(db);auto value=fresh.current("clips",7);(*value)["name"]="Fresh";fresh.set("clips",7,value);auto duplicate=fresh.changes();duplicate.push_back(duplicate[0]);check(!p.apply(duplicate,error) && error.find("duplicate")!=std::string::npos,"duplicate row targets are refused");
    edit::RowEdits root(db);auto project=root.current("project",1);(*project)["sample_rate"]=96000;root.set("project",1,project);
    check(!p.apply(root.changes(),error) && error.find("only change project length")!=std::string::npos,"time edit cannot change sample rate under a snapshot-only impact");
}
void mapOrder() {
    Project p;auto& db=p.store->db();
    db.exec("INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(9,100,110),(2,200,100),(4,300,90)");
    edit::RowEdits plan(db);
    for(std::int64_t id:{std::int64_t{2},std::int64_t{4},std::int64_t{9}}) {
        auto point=plan.current("tempo_map",id);(*point)["pos_ticks"]=point->at("pos_ticks").get<std::int64_t>()+100;
        plan.set("tempo_map",id,point);
    }
    std::string error;const auto changes=plan.changes();
    check(p.apply(changes,error),"positive map move orders by old position despite shuffled ids");
    check(db.execAndGet("SELECT pos_ticks FROM tempo_map WHERE id=9").getInt64()==200 && db.execAndGet("SELECT pos_ticks FROM tempo_map WHERE id=4").getInt64()==400,"all unique positions shift exactly");
    check(p.apply(p.inverse(changes),error) && db.execAndGet("SELECT pos_ticks FROM tempo_map WHERE id=9").getInt64()==100,"negative inverse uses opposite safe update order");
}
void linksAndChildren() {
    Project p;auto& db=p.store->db();
    db.exec("INSERT INTO clip_playback_details(clip_id,in_tension,content_offset_ns) VALUES(7,0,123)");
    db.exec("UPDATE clips SET alias_of=7 WHERE id=8");
    edit::RowEdits unsafe(db);unsafe.set("clips",7,std::nullopt);std::string error;
    check(!p.apply(unsafe.changes(),error) && error.find("uncaptured")!=std::string::npos,"deleting a root cannot silently cascade a native child or clear an alias");
    edit::RowEdits plan(db);plan.set("clips",7,std::nullopt);plan.set("clip_playback_details",7,std::nullopt);
    auto alias=plan.current("clips",8);(*alias)["alias_of"]=nullptr;plan.set("clips",8,alias);
    const auto changes=plan.changes();
    check(p.apply(changes,error) && db.execAndGet("SELECT count(*) FROM clips WHERE id=7").getInt()==0,"captured root/child deletion and explicit alias detachment apply together");
    check(p.apply(p.inverse(changes),error) && db.execAndGet("SELECT alias_of FROM clips WHERE id=8").getInt64()==7 && db.execAndGet("SELECT content_offset_ns FROM clip_playback_details WHERE clip_id=7").getInt64()==123,"inverse creates root before children and restores alias link");
    db.exec("CREATE TABLE future_child(id INTEGER PRIMARY KEY,clip_id INTEGER REFERENCES clips(id) ON DELETE CASCADE,payload BLOB) STRICT");
    db.exec("INSERT INTO future_child VALUES(1,7,X'0102')");
    check(!p.apply(changes,error) && error.find("future_child")!=std::string::npos,"unknown future cascade refuses by name");
    check(db.execAndGet("SELECT hex(payload) FROM future_child").getString()=="0102" && db.execAndGet("SELECT count(*) FROM clips WHERE id=7").getInt()==1,"refused cascade preserves extension and root data");
}
void aliasForest() {
    Project p;auto& db=p.store->db();edit::RowEdits plan(db);
    auto a=*plan.current("clips",7),b=*plan.current("clips",8);a["id"]=20;a["alias_of"]=19;b["id"]=19;b["alias_of"]=20;
    plan.set("clips",20,a);plan.set("clips",19,b);std::string error;const auto changes=plan.changes();
    check(p.apply(changes,error) && db.execAndGet("SELECT alias_of FROM clips WHERE id=20").getInt64()==19,"captured aliases need no insertion-order dependency, even for legacy cycles");
    check(p.apply(p.inverse(changes),error) && db.execAndGet("SELECT count(*) FROM clips WHERE id IN(19,20)").getInt()==0,"inverse removes the complete captured alias cycle");
}
void defaultsAndRanges() {
    Project p;auto& db=p.store->db();std::string error;
    db.exec("ALTER TABLE clips ADD COLUMN future_value BLOB NOT NULL DEFAULT X'00FF'");
    const auto clip=edit::defaultRow(db,"clips",{{"id",30},{"track_id",21},{"kind","midi"},{"pos_ticks",3}});
    check(clip.at("future_value").is_binary() && clip.at("name")=="" &&
          db.execAndGet("SELECT count(*) FROM clips WHERE id=30").getInt()==0,"default capture preserves future typed defaults without project writes");
    edit::RowEdits plan(db);plan.set("clips",30,clip);check(p.apply(plan.changes(),error),"captured future defaults apply as exact values");
    db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(1,'track',21,'pan')");
    db.exec("INSERT INTO automation_data(id,lane_id,data) VALUES(3,1,X'00')");
    db.exec("INSERT INTO automation_segment_ranges(id,data_id,record_index,progress_begin,progress_end,source_first,source_last) VALUES(9,3,1,0,1,0,1),(2,3,2,0,1,0,1)");
    edit::RowEdits move(db);
    for(const auto id:std::array<std::int64_t,2>{2,9}) {
        auto row=*move.current("automation_segment_ranges",id);
        row["record_index"]=row.at("record_index").get<std::int64_t>()+1;move.set("automation_segment_ranges",id,row);
    }
    const auto changes=move.changes();
    check(p.apply(changes,error) && p.apply(p.inverse(changes),error),"curve-range ordinal moves and inverse avoid UNIQUE collisions despite shuffled ids");
    edit::RowEdits created(db);
    created.set("automation_data",4,edit::defaultRow(db,"automation_data",{{"id",4},{"lane_id",1},{"clip_id",30},{"data",Payload::binary({1,2})}}));
    created.set("automation_segment_ranges",12,edit::defaultRow(db,"automation_segment_ranges",
        {{"id",12},{"data_id",4},{"clip_id",30},{"record_index",0},{"progress_begin",0.0},{"progress_end",0.5},{"source_first",0.0},{"source_last",1.0}}));
    const auto inserted=created.changes();
    check(p.apply(inserted,error) && p.apply(p.inverse(inserted),error) && p.apply(inserted,error),"parent data restores before curve ranges in both captured orders");
}

}
int main(){try{updatesAndConflicts();mapOrder();linksAndChildren();aliasForest();defaultsAndRanges();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
