// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/blob.hpp"
#include "adi/engine/project_view.hpp"
#include "adi/engine/session.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::engine;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what.c_str()); }
}
AutomationPoint point(std::int64_t time, double value, std::uint64_t id) {
    AutomationPoint result{};
    result.time = time; result.value = value; result.point_id = id; result.curve = 1;
    return result;
}
std::vector<std::byte> bytes(const std::vector<AutomationPoint>& points, bool nanos = false) {
    return writeStream<AutomationPoint>(FourCC::Automation, points, 1,
        StreamFlags::SortedByTime | (nanos ? StreamFlags::TimeIsNanos : 0u));
}
struct Fixture {
    test::TempDirectory temp;
    std::unique_ptr<Store> store;
    explicit Fixture(const char* name) : temp("automation_snapshot", name) {
        StoreError error = StoreError::Ok;
        store = Store::create(temp.path() / "song.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        auto& db = store->db();
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Keys'),(9,'master','Main')");
        db.exec("INSERT INTO mixer_strip(track_id) VALUES(1),(9)");
        db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(5,1,'midi',0,23063040)");
        db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,param_name,value_domain,unit,default_value,min_value,max_value,enabled,visible,height) "
                "VALUES(1,'track',1,'volume','Volume','real','dB',-6,-150,6,1,1,73)");
        db.exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base) VALUES(2,'device',91,'17',1)");
        check(store->putAutomationData(1, {}, bytes({point(0,-3,4), point(5765760,-12,9)})), "arrangement stream seed");
        check(store->putAutomationData(1, 5, bytes({point(3,-9,12)})), "clip stream seed");
        check(store->putAutomationData(2, {}, bytes({point(1000000001,0.75,std::numeric_limits<std::uint64_t>::max())}, true)), "linear stream seed");
    }
};

void valuesAndSharing() {
    Fixture f("values");
    auto prior = SnapshotBuilder::fromStore(*f.store);
    const auto* lane = prior->findAutomationLane(1);
    check(prior->automationLanes.size() == 2 && lane && !prior->findAutomationLane(90), "lanes by id and missing lookup");
    if (!lane) return;
    check(lane->ownerKind == "track" && lane->ownerId == 1 && lane->paramRef == "volume" &&
              lane->paramName == "Volume" && lane->valueDomain == "real" && lane->unit == "dB" &&
              lane->timeBase == 0 && lane->defaultValue == -6 && lane->minValue == -150 && lane->maxValue == 6 &&
              lane->enabled && lane->visible && lane->height == 73 && !lane->overridden, "all lane metadata exposed");
    check(lane->envelopes.size() == 2 && !lane->envelopes[0]->clipId && lane->envelopes[1]->clipId == 5,
          "arrangement and clip envelopes retain independent data identities");
    check(lane->envelopes[0]->points.size() == 2 && lane->envelopes[0]->points[1].time == 5765760 &&
              lane->envelopes[0]->points[1].value == -12 && lane->envelopes[0]->points[1].id == 9 &&
              lane->envelopes[1]->points[0].time == 3, "ticks and clip-relative point identity");
    const auto* linear = prior->findAutomationLane(2);
    check(linear && linear->timeBase == 1 && linear->envelopes[0]->points[0].time == 1000000001 &&
              linear->envelopes[0]->points[0].id == std::numeric_limits<std::uint64_t>::max() &&
              !linear->minValue && !linear->maxValue, "nanoseconds, full unsigned point id and absent bounds");
    const auto same = SnapshotBuilder::fromStore(*f.store, *prior);
    check(same->automationLanes == prior->automationLanes, "unchanged lanes and envelopes shared");
    check(SnapshotBuilder::sharedNodeCount(*same, *prior) == 9, "sharing count includes tempo, tracks, clip, lanes and envelopes");
    for (const char* change : {"owner_kind='project'", "owner_id=8", "param_ref='pan'", "param_name='Pan'",
             "time_base=1", "value_domain='enum'", "unit='steps'", "default_value=2", "min_value=-151",
             "max_value=7", "enabled=0", "visible=0", "height=92"}) {
        f.store->db().exec(std::string("UPDATE automation_lanes SET ") + change + " WHERE id=1");
        auto next = SnapshotBuilder::fromStore(*f.store, *prior);
        check(next->automationLanes[0] != prior->automationLanes[0] &&
                  next->automationLanes[1] == prior->automationLanes[1] && next->tracks == prior->tracks,
              std::string("sharing sees lane ") + change);
        prior = std::move(next);
    }
    f.store->db().exec("UPDATE automation_lanes SET time_base=0 WHERE id=1");
    prior = SnapshotBuilder::fromStore(*f.store, *prior);
    const auto oldEnvelope = prior->automationLanes[0]->envelopes[0];
    auto p = point(17, -5, 99); p.tension = 0.5f; p.curve = 5; p.flags = 3;
    check(f.store->putAutomationData(1, {}, bytes({p})), "point replacement");
    auto next = SnapshotBuilder::fromStore(*f.store, *prior);
    const auto& changed = *next->automationLanes[0]->envelopes[0];
    check(changed.points.size() == 1 && changed.points[0].id == 99 && changed.points[0].time == 17 &&
              changed.points[0].value == -5 && changed.points[0].tension == 0.5f && changed.points[0].curve == 5 &&
              changed.points[0].flags == 3, "all point properties exposed");
    check(next->automationLanes[0] != prior->automationLanes[0] && changed.id == oldEnvelope->id &&
              next->automationLanes[0]->envelopes[1] == prior->automationLanes[0]->envelopes[1] &&
              next->automationLanes[1] == prior->automationLanes[1], "one envelope change preserves sibling envelope and lane");
    prior = std::move(next);
}

void envelopeIdentityAndRules() {
    Fixture f("identity");
    auto prior = SnapshotBuilder::fromStore(*f.store);
    f.store->db().exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(6,1,'midi',0,4)");
    f.store->db().exec("UPDATE automation_data SET clip_id=6 WHERE lane_id=1 AND clip_id=5");
    auto next = SnapshotBuilder::fromStore(*f.store, *prior);
    check(next->automationLanes[0]->envelopes[1] != prior->automationLanes[0]->envelopes[1] &&
              next->automationLanes[0]->envelopes[1]->clipId == 6, "envelope clip identity participates in sharing");
    prior = std::move(next);
    f.store->db().exec("UPDATE automation_data SET id=77 WHERE lane_id=1 AND clip_id=6");
    next = SnapshotBuilder::fromStore(*f.store, *prior);
    check(next->automationLanes[0]->envelopes[1]->id == 77 &&
              next->automationLanes[0]->envelopes[1] != prior->automationLanes[0]->envelopes[1], "data identity participates in sharing");
    prior = std::move(next);
    f.store->db().exec("UPDATE automation_lanes SET min_value=-5 WHERE id=1");
    next = SnapshotBuilder::fromStore(*f.store, *prior);
    check(next->automationLanes[0]->envelopes[0]->points.empty() && !next->automationLanes[0]->envelopes[0]->problem.empty(),
          "same bytes are revalidated after bound change");
    prior = std::move(next);
    f.store->db().exec("UPDATE automation_lanes SET min_value=-150 WHERE id=1");
    next = SnapshotBuilder::fromStore(*f.store, *prior);
    check(next->automationLanes[0]->envelopes[0]->problem.empty() && next->automationLanes[0]->envelopes[0]->points.size() == 2,
          "bound correction restores readable points");
    f.store->db().exec("DELETE FROM automation_data WHERE lane_id=1");
    prior = SnapshotBuilder::fromStore(*f.store, *next);
    check(prior->automationLanes[0]->envelopes.empty() && prior->automationLanes[0]->defaultValue == -6,
          "empty lane keeps default and metadata");
    f.store->db().exec("DELETE FROM automation_lanes WHERE id=1");
    next = SnapshotBuilder::fromStore(*f.store, *prior);
    check(!next->findAutomationLane(1) && next->automationLanes[0] == prior->automationLanes[1], "deleted lane disappears without copying sibling");
}

void badAndFutureStreams() {
    Fixture f("streams");
    auto prior = SnapshotBuilder::fromStore(*f.store);
    const auto badPoint = [&](AutomationPoint p, bool nanos, const char* name) {
        check(f.store->putAutomationData(1, {}, bytes({point(0,-3,1), p}, nanos)), "write invalid fixture");
        auto next = SnapshotBuilder::fromStore(*f.store, *prior);
        const auto& envelope = *next->findAutomationLane(1)->envelopes[0];
        check(envelope.points.empty() && !envelope.problem.empty(), name);
        prior = std::move(next);
    };
    auto p = point(2,-4,2); p.value = std::numeric_limits<double>::quiet_NaN(); badPoint(p, false, "nonfinite value refuses entire envelope");
    p = point(2,-4,2); p.tension = std::numeric_limits<float>::infinity(); badPoint(p, false, "nonfinite tension refused");
    p = point(2,-4,2); p.curve = 6; badPoint(p, false, "unknown curve refused");
    p = point(2,-4,2); p.reserved = 1; badPoint(p, false, "reserved bytes refused");
    p = point(-1,-4,2); badPoint(p, false, "negative or unsorted point refused");
    p = point(2,-4,2); badPoint(p, true, "mismatched time units refused");
    check(f.store->putAutomationData(1, {}, std::vector<std::byte>{std::byte{1}}), "corrupt stream fixture");
    prior = SnapshotBuilder::fromStore(*f.store, *prior);
    const auto bad = SnapshotBuilder::fromStore(*f.store, *prior);
    check(!bad->findAutomationLane(1)->envelopes[0]->problem.empty() && bad->automationLanes == prior->automationLanes,
          "corrupt stream remains named and shares when unchanged");
    auto encoded = bytes({point(3,-2,4)});
    StreamHeader header{}; std::memcpy(&header, encoded.data(), sizeof header);
    header.rec_size = static_cast<std::uint16_t>(sizeof(AutomationPoint) + 4);
    std::memcpy(encoded.data(), &header, sizeof header); encoded.resize(encoded.size() + 4, std::byte{5});
    check(f.store->putAutomationData(1, {}, encoded), "future tail fixture");
    prior = SnapshotBuilder::fromStore(*f.store, *bad);
    check(prior->findAutomationLane(1)->envelopes[0]->points.size() == 1 &&
              prior->findAutomationLane(1)->envelopes[0]->problem.empty(), "wider future AAUT record retains known prefix");
    encoded.back() = std::byte{6};
    check(f.store->putAutomationData(1, {}, encoded), "future tail edit");
    const auto tail = SnapshotBuilder::fromStore(*f.store, *prior);
    check(tail->automationLanes[0] != prior->automationLanes[0] &&
              tail->automationLanes[0]->envelopes[0]->points == prior->automationLanes[0]->envelopes[0]->points,
          "unknown tail change invalidates sharing even when known points match");
}

void runtimeOverride() {
    Fixture f("runtime");
    // Device lane 2 has no actual device and is irrelevant to the volume lane.
    Session session;
    check(session.load(*f.store, {}, SessionSpec{}), "session loads automation");
    ProjectView view;
    view.refresh(*f.store, session.overriddenLanes());
    const auto untouched = view.current();
    check(!untouched->automationOverridden && !untouched->findAutomationLane(1)->overridden, "no runtime override at load");
    OpRequest op; op.opType = "mixer.setVolume"; op.payload = {{"id", 1}, {"db", -9}};
    check(OpJournal(*f.store).commit(op).ok && session.refresh(*f.store), "manual fader edit refreshes runtime");
    view.refresh(*f.store, session.overriddenLanes());
    const auto overridden = view.current();
    check(overridden->automationOverridden && overridden->findAutomationLane(1)->overridden &&
              !overridden->findAutomationLane(2)->overridden && !untouched->automationOverridden,
          "view captures session override and retained frame stays consistent");
    check(overridden->automationLanes[0]->envelopes == untouched->automationLanes[0]->envelopes &&
              overridden->automationLanes[1] == untouched->automationLanes[1], "override changes lane metadata while envelopes and siblings share");
    check(session.reenableAutomation(1), "runtime reenable");
    view.refresh(*f.store, session.overriddenLanes());
    check(!view.current()->automationOverridden && !view.current()->findAutomationLane(1)->overridden &&
              overridden->automationOverridden, "reenable clears new frame, not the retained one");
    check(!SnapshotBuilder::fromStore(*f.store)->automationOverridden, "ordinary/offline rebuild does not persist overrides");
    view.refresh(*f.store, {999});
    check(view.current()->automationOverridden && !view.current()->findAutomationLane(1)->overridden,
          "global Re-Enable mirrors whole session set even before an unknown lane appears");
}
void retainedCurves() {
    Fixture f("retained");auto& db=f.store->db();
    // Optional N17 table; this reader remains independent of its schema PR.
    db.exec("CREATE TABLE IF NOT EXISTS automation_segment_ranges(id INTEGER PRIMARY KEY,data_id INTEGER NOT NULL,clip_id INTEGER,"
        "record_index INTEGER NOT NULL,progress_begin REAL NOT NULL,progress_end REAL NOT NULL,source_first REAL NOT NULL,source_last REAL NOT NULL) STRICT");
    db.exec("INSERT INTO automation_segment_ranges SELECT 12,id,NULL,0,0.2,0.8,-3,-12 FROM automation_data WHERE lane_id=1 AND clip_id IS NULL");
    auto prior=SnapshotBuilder::fromStore(*f.store);
    const auto envelope=prior->findAutomationLane(1)->envelopes[0];
    check(envelope->problem.empty() && envelope->points[0].curveSlice &&
          envelope->points[0].curveSlice->begin==0.2 && envelope->points[0].curveSlice->end==0.8 &&
          envelope->points[0].curveSlice->sourceFirst==-3 && envelope->points[0].curveSlice->sourceLast==-12,
          "window reads original source values and retained progress interval on outgoing point");
    check(envelope->curveRanges.size()==1 && envelope->curveRanges[0].id==12 &&
          envelope->curveRanges[0].recordIndex==0 && !envelope->curveRanges[0].clipId,"range identity, storage ordinal and owner are retained");
    const auto same=SnapshotBuilder::fromStore(*f.store,*prior);
    check(same->automationLanes==prior->automationLanes,"unchanged retained curves share");
    for(const char* change:{"progress_begin=0.1","progress_end=0.9","source_first=-6","source_last=0","id=13","record_index=1"}) {
        db.exec(std::string("UPDATE automation_segment_ranges SET ")+change);
        auto next=SnapshotBuilder::fromStore(*f.store,*prior);
        check(next->findAutomationLane(1)->envelopes[0]!=prior->findAutomationLane(1)->envelopes[0] &&
              next->findAutomationLane(1)->envelopes[1]==prior->findAutomationLane(1)->envelopes[1] &&
              next->findAutomationLane(2)==prior->findAutomationLane(2),"every range field participates in sharing without changing siblings");
        prior=std::move(next);
    }
    check(prior->findAutomationLane(1)->envelopes[0]->encoded==envelope->encoded &&
          !prior->findAutomationLane(1)->envelopes[0]->points[0].curveSlice &&
          prior->findAutomationLane(1)->envelopes[0]->points[1].curveSlice,"range-only edits update decoded geometry despite identical AAUT bytes");
    for(const char* change:{"clip_id=5","clip_id=NULL,record_index=99","record_index=0,progress_end=0.01",
                           "progress_end=0.9,source_last=1e999"}) {
        try { db.exec(std::string("UPDATE automation_segment_ranges SET ")+change); }
        catch(const std::exception&) { continue; } // the native table refuses it before the reader can (chain)
        const auto bad=SnapshotBuilder::fromStore(*f.store,*prior);
        check(bad->findAutomationLane(1)->envelopes[0]->points.empty() &&
              bad->findAutomationLane(1)->envelopes[0]->problem.find("retained")!=std::string::npos &&
              bad->findAutomationLane(1)->envelopes[0]->curveRanges.size()==1,"invalid owner, record, interval or value refuses whole envelope and retains diagnostic data");
    }
    db.exec("UPDATE automation_segment_ranges SET record_index=0,progress_begin=0.1,progress_end=0.9,source_last=0");
    try {
        db.exec("INSERT INTO automation_segment_ranges SELECT 14,data_id,clip_id,record_index,progress_begin,progress_end,source_first,source_last FROM automation_segment_ranges");
        const auto duplicate=SnapshotBuilder::fromStore(*f.store,*prior);
        check(duplicate->findAutomationLane(1)->envelopes[0]->points.empty() &&
              !duplicate->findAutomationLane(1)->envelopes[0]->problem.empty(),"duplicate storage ordinal refuses the whole envelope");
    } catch(const std::exception&) { check(true,"duplicate storage ordinal is refused by the native UNIQUE(data_id,record_index)"); } // chain
    db.exec("DELETE FROM automation_segment_ranges");
    const auto reset=SnapshotBuilder::fromStore(*f.store,*prior);
    check(reset->findAutomationLane(1)->envelopes[0]->problem.empty() &&
          !reset->findAutomationLane(1)->envelopes[0]->points[0].curveSlice &&
          envelope->points[0].curveSlice->begin==0.2,"removing the domain restores legacy geometry without mutating retained frames");
}

} // namespace
int main() {
    try { valuesAndSharing(); envelopeIdentityAndRules(); badAndFutureStreams(); runtimeOverride(); retainedCurves(); }
    catch (const std::exception& e) { check(false, std::string("exception: ") + e.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
