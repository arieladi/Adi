// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/automation_edits.hpp"
#include "adi/blob.hpp"
#include "adi/digest.hpp"
#include "adi/engine/automation.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using namespace adi;
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks; if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}
std::unique_ptr<Store> project(const std::filesystem::path& path) {
    StoreError error = StoreError::Ok;
    auto s = Store::createEmpty(path, error);
    if (!s) throw std::runtime_error("create store");
    s->db().exec("INSERT INTO project(id,name) VALUES(1,'Automation');"
        "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Track');"
        "INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(1,1,'audio',0,0,9600);"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(2,'track',1,'volume');"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base) VALUES(3,'track',1,'pan',1)");
    return s;
}
std::vector<std::byte> bytes(Store& s, std::int64_t id = 4) {
    SQLite::Statement q(s.db(), "SELECT data FROM automation_data WHERE id=?"); q.bind(1,id);
    if (!q.executeStep()) return {};
    const auto col = q.getColumn(0); const auto* b = static_cast<const std::byte*>(col.getBlob());
    return {b, b + col.getBytes()};
}
std::vector<AutomationPoint> points(Store& s) {
    const auto b = bytes(s); StreamReader<AutomationPoint> r(b, FourCC::Automation);
    const auto result = r.all();
    if (!result) throw std::runtime_error("read points");
    return *result;
}
Payload point(std::uint64_t id, std::int64_t time, double value, int curve = 1, double tension = 0) {
    return {{"id",id},{"time",time},{"value",value},{"curve",curve},{"tension",tension}};
}
void run() {
    test::TempDirectory temp("automation", "edits");
    auto s = project(temp.path()/"a.adi");
    auto replay = project(temp.path()/"b.adi");
    OpJournal journal(*s), replayJournal(*replay); History history(*s);
    const auto initial = digestProject(*s).text;
    constexpr std::uint64_t wide = UINT64_MAX - 4;
    const double precise = 0.12345678901234567;
    const auto write = automation::writePoints(4,2,nullptr,Payload::array({point(wide,960,precise),point(7,0,0.0)}));
    check(journal.commit(write).ok,"native point insertion");
    check(replayJournal.commit(write).ok && bytes(*s)==bytes(*replay),"explicit IDs replay deterministically");
    const auto inserted = bytes(*s);
    const engine::AutomationProgram program(rows::readModel(*s), 48000);
    check(program.lane(2) && program.problems().empty(),"native edits compile through row model");
    check(program.lane(2) && engine::valueAt(*program.lane(2),24000)==precise,"committed double reaches engine evaluator");
    const auto p = points(*s);
    check(p.size()==2 && p[1].point_id==wide && p[1].value==precise,"uint64 identity and double value preserved");
    check(history.undo().ok && bytes(*s).empty(),"undo first creation restores absence");
    check(digestProject(*s).text==initial,"first insertion exact project undo");
    check(history.redo().ok && bytes(*s)==inserted,"redo exact bytes");
    check(journal.commit(automation::setCurve(4,2,nullptr,Payload::array({7}),5,0.75)).ok,"curve edit commits");
    check(points(*s)[0].curve==5 && points(*s)[0].tension==0.75f,"curve and tension changed");
    check(history.undo().ok && bytes(*s)==inserted,"curve exact undo");
    check(history.redo().ok,"curve redo");
    const auto shaped = bytes(*s);
    check(journal.commit(automation::movePoints(4,2,nullptr,Payload::array({point(wide,0,0.9)}))).ok,"absolute movement commits");
    check(points(*s)[1].point_id==wide && points(*s)[1].time==0,"coincident order is stable");
    check(history.undo().ok && bytes(*s)==shaped,"move restores exact order");
    check(journal.commit(automation::deletePoints(4,2,nullptr,Payload::array({wide}))).ok,"selected deletion commits");
    check(points(*s).size()==1 && points(*s)[0].point_id==7,"deletion preserves unrelated point");
    check(history.undo().ok && bytes(*s)==shaped,"delete exact undo");
    const auto stableDigest = digestProject(*s).text;
    auto refuse = [&](const OpRequest& r, const char* label) {
        check(!journal.commit(r).ok && digestProject(*s).text==stableDigest && bytes(*s)==shaped,label);
    };
    refuse(automation::deletePoints(4,2,nullptr,Payload::array({7,8})),"partial missing selection rolls back");
    refuse(automation::deletePoints(4,2,nullptr,Payload::array({7,7})),"duplicate selection refused");
    refuse(automation::deletePoints(4,2,nullptr,Payload::array()),"empty selection refused");
    refuse(automation::movePoints(4,2,nullptr,Payload::array({point(8,0,0)})),"move never inserts missing point");
    refuse(automation::writePoints(4,2,nullptr,Payload::array({point(0,0,0)})),"zero point ID refused");
    refuse(automation::writePoints(4,2,nullptr,Payload::array({point(8,-1,0)})),"negative time refused");
    refuse(automation::writePoints(4,2,nullptr,Payload::array({point(8,0,std::numeric_limits<double>::infinity())})),"nonfinite value refused");
    refuse(automation::setCurve(4,2,nullptr,Payload::array({7}),6,0),"unknown curve refused");
    refuse(automation::setCurve(4,2,nullptr,Payload::array({7}),1,1.01),"invalid tension refused");
    refuse(automation::writePoints(4,3,nullptr,Payload::array({point(8,0,0)})),"lane/data mismatch refused");
    refuse(automation::writePoints(4,2,1,Payload::array({point(8,0,0)})),"clip/data mismatch refused");
    refuse(automation::writePoints(5,2,nullptr,Payload::array({point(8,0,0)})),"duplicate addressed slot refused");
    auto overflow = automation::writePoints(4,2,nullptr,Payload::array({point(8,0,0)}));
    overflow.payload["points"][0]["time"] = UINT64_MAX;
    refuse(overflow,"int64 overflow refused before narrowing");
    check(journal.commit(automation::writePoints(5,3,1,Payload::array({point(8,1000000,0.5)}))).ok,"clip-relative nanos insertion");
    const auto nanos = bytes(*s,5); StreamReader<AutomationPoint> nr(nanos,FourCC::Automation);
    check(nr.ok() && (nr.header().flags & StreamFlags::TimeIsNanos)!=0,"new stream uses explicit lane time base");
    check(history.undo().ok && bytes(*s,5).empty(),"clip insertion undo restores slot absence");

    // Existing opaque record suffixes and trailer remain attached across sort,
    // updates and deletion. Undo must preserve the entire original blob.
    StreamHeader h{}; std::memcpy(&h,shaped.data(),sizeof h); h.rec_size=40; h.version=9; h.flags|=0x100;
    std::vector<std::byte> future(sizeof h); std::memcpy(future.data(),&h,sizeof h);
    for (std::size_t i=0;i<2;++i) {
        const auto begin=shaped.begin()+static_cast<std::ptrdiff_t>(sizeof h+i*sizeof(AutomationPoint));
        future.insert(future.end(),begin,begin+sizeof(AutomationPoint));
        future.insert(future.end(),8,static_cast<std::byte>(0xa0+i));
    }
    future.push_back(std::byte{0xcc});
    SQLite::Statement update(s->db(),"UPDATE automation_data SET data=? WHERE id=4");
    update.bind(1,future.data(),static_cast<int>(future.size())); update.exec();
    check(journal.commit(automation::movePoints(4,2,nullptr,Payload::array({point(wide,0,0.4),point(7,1920,0.6)}))).ok,"future prefix edit accepted");
    const auto moved=bytes(*s);
    check(moved.back()==std::byte{0xcc} && moved[sizeof h+32]==std::byte{0xa1} &&
        moved[sizeof h+40+32]==std::byte{0xa0},"opaque tails travel with sorted point identity");
    StreamHeader movedHeader{}; std::memcpy(&movedHeader,moved.data(),sizeof movedHeader);
    check(movedHeader.flags==h.flags && movedHeader.version==h.version && movedHeader.rec_size==40,"future header retained");
    check(history.undo().ok && bytes(*s)==future,"future stream exact undo");
    check(!journal.commit(automation::writePoints(4,2,nullptr,Payload::array({point(999,0,0)}))).ok && bytes(*s)==future,
        "future insertion refuses invented fields");
    check(journal.commit(automation::deletePoints(4,2,nullptr,Payload::array({wide}))).ok,"future record deletion");
    const auto removed=bytes(*s);
    check(removed.size()==sizeof h+40+1 && removed[sizeof h+32]==std::byte{0xa0} && removed.back()==std::byte{0xcc},"future deletion keeps survivor suffix and trailer");
    check(history.undo().ok && bytes(*s)==future,"future deletion undo");
    check(journal.commit(automation::deletePoints(4,2,nullptr,Payload::array({7,wide}))).ok,"delete all points");
    check(points(*s).empty() && !bytes(*s).empty(),"last deletion retains addressed empty stream");
    check(history.undo().ok && bytes(*s)==future,"empty stream exact undo");
    // A caller's rejected final hook rolls back both stream mutation and journal.
    CommitOptions options; options.beforeCommit=[](SQLite::Database&, std::int64_t, std::string& error) {
        error="test rejection"; return false;
    };
    const auto rejected = automation::setCurve(4,2,nullptr,Payload::array({7}),2,0);
    check(!journal.commit(std::span<const OpRequest>(&rejected,1),options).ok && bytes(*s)==future,"caller hook rolls back data");
}
} // namespace
int main() {
    try { run(); } catch(const std::exception& e) { ++failures; std::printf("EXCEPTION: %s\n",e.what()); }
    std::printf("%d checks, %d failures\n",checks,failures); return failures ? 1 : 0;
}
