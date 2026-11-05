// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/blob.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace {
using namespace adi;
int checks=0,failures=0;
void check(bool ok,const char* label) { ++checks; if(!ok) { ++failures; std::printf("FAIL: %s\n",label); } }
std::unique_ptr<Store> project(const std::filesystem::path& path) {
    StoreError error=StoreError::Ok; auto s=Store::createEmpty(path,error);
    if(!s) throw std::runtime_error("create store");
    s->db().exec("INSERT INTO project(id,name) VALUES(1,'Lanes'); INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Track');"
        "INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(1,1,'audio',0,0,960)");
    return s;
}
void stream(Store& s,std::int64_t id,bool clip,const std::vector<std::byte>& bytes) {
    SQLite::Statement q(s.db(),"INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(?,2,?,?)");
    q.bind(1,id); if(clip)q.bind(2,1);else q.bind(2);
    q.bind(3,bytes.data(),static_cast<int>(bytes.size())); q.exec();
}
std::vector<std::byte> bytesAt(Store& s,std::int64_t id) {
    SQLite::Statement q(s.db(),"SELECT data FROM automation_data WHERE id=?"); q.bind(1,id);
    if(!q.executeStep())return {};
    const auto col=q.getColumn(0); const auto* b=static_cast<const std::byte*>(col.getBlob()); return {b,b+col.getBytes()};
}
void run() {
    test::TempDirectory temp("automation","lanes"); auto s=project(temp.path()/"a.adi"), replay=project(temp.path()/"b.adi");
    OpJournal journal(*s),other(*replay); History history(*s);
    automation::LaneSpec spec; spec.id=2;spec.ownerId=1;spec.paramRef="volume";spec.paramName="Gain";
    spec.valueDomain="real";spec.unit="dB";spec.defaultValue=-6;spec.minValue=-60;spec.maxValue=12;spec.height=160;spec.visible=true;
    const auto empty=digestProject(*s).text; const auto create=automation::createLane(spec);
    check(journal.commit(create).ok,"create complete native lane");
    check(other.commit(create).ok && digestProject(*s).text==digestProject(*replay).text,"explicit row replay");
    const auto created=digestProject(*s).text;
    check(history.undo().ok && digestProject(*s).text==empty,"creation exact undo to absence");
    check(history.redo().ok && digestProject(*s).text==created,"creation redo");
    check(!journal.commit(create).ok && digestProject(*s).text==created,"duplicate lane ID refused atomically");
    auto bad=spec; bad.id=3;bad.ownerId=999;
    check(!journal.commit(automation::createLane(bad)).ok,"missing polymorphic owner refused");
    bad.ownerId=1;bad.ownerKind="mystery";
    check(!journal.commit(automation::createLane(bad)).ok,"unknown owner refused");
    bad=spec;bad.id=3;bad.timeBase=3;
    check(!journal.commit(automation::createLane(bad)).ok,"unknown time base refused");
    bad=spec;bad.id=3;bad.minValue=10;
    check(!journal.commit(automation::createLane(bad)).ok,"inconsistent default and bounds refused");
    bad=spec;bad.id=3;bad.defaultValue=std::numeric_limits<double>::quiet_NaN();
    check(!journal.commit(automation::createLane(bad)).ok,"nonfinite creation refused");
    bad=spec;bad.id=3;bad.paramRef="";
    check(!journal.commit(automation::createLane(bad)).ok,"empty parameter refused");
    auto wrong=create;wrong.payload["id"]=3;
    check(!journal.commit(wrong).ok,"payload and row ID mismatch refused");
    check(journal.commit(automation::setLaneEnabled(2,false)).ok,"disable native lane");
    check(s->db().execAndGet("SELECT enabled FROM automation_lanes WHERE id=2").getInt()==0,"enabled stored");
    check(history.undo().ok && digestProject(*s).text==created,"enable exact undo");
    check(journal.commit(automation::setLaneVisible(2,false)).ok,"hide lane");
    check(s->db().execAndGet("SELECT visible FROM automation_lanes WHERE id=2").getInt()==0,"visible stored");
    check(history.undo().ok && digestProject(*s).text==created,"visibility exact undo");
    const double precise=-6.1234567890123;
    check(journal.commit(automation::setDefaultValue(2,precise)).ok,"set double default");
    check(s->db().execAndGet("SELECT default_value FROM automation_lanes WHERE id=2").getDouble()==precise,"default precision retained");
    check(history.undo().ok && digestProject(*s).text==created,"default exact undo");
    check(!journal.commit(automation::setDefaultValue(2,13)).ok,"out-of-range default refused");
    check(!journal.commit(automation::setDefaultValue(999,0)).ok,"missing lane setter refused");
    check(!journal.commit(automation::deleteLane(999)).ok,"missing lane delete refused");
    check(!journal.commit(automation::clearLane(999)).ok,"missing lane clear refused");

    AutomationPoint point{};point.point_id=UINT64_MAX;point.time=12;point.value=-3;
    auto normal=writeStream<AutomationPoint>(FourCC::Automation,std::span<const AutomationPoint>(&point,1));
    StreamHeader h{};std::memcpy(&h,normal.data(),sizeof h);h.version=8;h.rec_size=40;h.flags|=0x100;
    auto future=normal;std::memcpy(future.data(),&h,sizeof h);future.insert(future.end(),8,std::byte{0xa5});future.push_back(std::byte{0xee});
    stream(*s,4,false,future);stream(*s,5,true,normal);
    const auto withData=digestProject(*s).text;
    check(journal.commit(automation::clearLane(2)).ok,"clear arrangement and clip streams together");
    const auto cleared=bytesAt(*s,4);StreamReader<AutomationPoint> reader(cleared,FourCC::Automation);
    check(reader.ok() && reader.count()==0 && reader.header().version==8 && reader.header().rec_size==40 &&
        reader.header().flags==h.flags && cleared.back()==std::byte{0xee},"clear retains future header and trailing bytes");
    const auto clearedClip=bytesAt(*s,5);StreamReader<AutomationPoint> clipReader(clearedClip,FourCC::Automation);
    check(clipReader.ok() && clipReader.count()==0,"clear retains empty clip slot");
    check(history.undo().ok && bytesAt(*s,4)==future && bytesAt(*s,5)==normal && digestProject(*s).text==withData,"clear exact full-stream undo");
    check(history.redo().ok && history.undo().ok,"clear redo and undo");
    check(journal.commit(automation::deleteLane(2)).ok,"delete populated lane");
    check(s->db().execAndGet("SELECT count(*) FROM automation_data").getInt()==0,"delete cascades streams");
    check(history.undo().ok && digestProject(*s).text==withData && bytesAt(*s,4)==future,"delete restores all metadata and opaque data");
    check(history.redo().ok && history.undo().ok,"populated lane delete redo");
    CommitOptions options; options.beforeCommit=[](SQLite::Database&,std::int64_t,std::string& err){err="reject";return false;};
    const auto del=automation::deleteLane(2);
    check(!journal.commit(std::span<const OpRequest>(&del,1),options).ok && digestProject(*s).text==withData,"caller rejection rolls back cascade and journal");
    s->db().exec("UPDATE automation_data SET data=x'00' WHERE id=5");
    check(!journal.commit(automation::clearLane(2)).ok && bytesAt(*s,4)==future,"malformed later stream rolls back earlier clear");
    // Undo must preserve legacy metadata without applying creation validation.
    s->db().exec("UPDATE automation_lanes SET value_domain='future',height=-3 WHERE id=2");
    const auto legacy=digestProject(*s).text;
    check(journal.commit(automation::deleteLane(2)).ok && history.undo().ok && digestProject(*s).text==legacy,"legacy metadata and malformed bytes survive delete undo");
    bad=spec;bad.id=3;bad.ownerKind="clip";bad.paramRef="pan";
    check(journal.commit(automation::createLane(bad)).ok,"clip owner supported");
    bad.id=4;bad.ownerKind="project";
    check(journal.commit(automation::createLane(bad)).ok,"project owner supported");
}
} // namespace
int main() {
    try {run();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
