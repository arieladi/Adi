// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"
#include "adi/automation_edits.hpp"
#include "adi/digest.hpp"
#include "adi/edit_time.hpp"
#include "adi/engine/automation_recording.hpp"
#include "adi/engine/automation.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
namespace {
thread_local bool watch=false;
std::atomic<int> allocations{0};
}
void adi::test::countReplacedNew() noexcept {if(watch)++allocations;}
namespace {
using namespace adi;
using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
std::unique_ptr<Store> project(const std::filesystem::path& path) {
    StoreError e=StoreError::Ok;auto s=Store::createEmpty(path,e);if(!s)throw std::runtime_error("store");
    s->db().exec("INSERT INTO project(id,name) VALUES(1,'Record');INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Track');"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base) VALUES(2,'track',1,'volume',1)");return s;
}
std::vector<std::byte> bytes(Store& s) {
    SQLite::Statement q(s.db(),"SELECT data FROM automation_data WHERE id=3");if(!q.executeStep())return {};
    const auto c=q.getColumn(0);const auto* b=static_cast<const std::byte*>(c.getBlob());return {b,b+c.getBytes()};
}
void run() {
    const AutomationTarget target{3,2,std::nullopt};AutomationRecorder recorder(target,16);
    std::vector<RecordedAutomation> out;
    check(recorder.armed() && !recorder.recording(),"Automation Arm defaults on and transport record off");
    check(!recorder.push(AutomationGesture::Begin,0,0.1) && recorder.drain(out)==0,"transport record gate");
    check(recorder.setRecording(true,0) && recorder.setArmed(false,0),"producer controls");
    check(!recorder.push(AutomationGesture::Value,1,0.2) && recorder.drain(out)==0,"disarmed capture ignores manual values");
    check(recorder.setArmed(true,1),"rearm while recording");
    constexpr double precise=0.12345678901234567;constexpr std::int64_t wide=(std::int64_t{1}<<54)+1;
    watch=true;
    const bool begin=recorder.push(AutomationGesture::Begin,wide,precise);
    const bool value=recorder.push(AutomationGesture::Value,wide+10,0.5);
    const bool end=recorder.push(AutomationGesture::End,wide+20,0.8);
    watch=false;
    check(begin && value && end && allocations==0,"producer capture allocates nothing");
    check(recorder.drain(out)==1 && out.size()==1,"completed gesture emitted once");
    if(out.empty())return;
    check(out[0].target.data==3 && out[0].target.lane==2 && out[0].points.size()==3,"explicit target and all points retained");
    check(out[0].points.size()>=2 && out[0].points[0].time==wide && out[0].points[1].time==wide+10 && out[0].points[0].value==precise,"timestamps and values are never narrowed or drain-timed");
    check(recorder.drain(out)==0,"second drain does not duplicate a gesture");
    check(recorder.push(AutomationGesture::Value,100,0.2),"implicit manual gesture starts");
    check(recorder.push(AutomationGesture::Value,100,0.3),"coincident update accepted");
    check(recorder.setArmed(false,120) && recorder.drain(out)==1,"disarm closes the prior gesture at supplied time");
    check(out.back().points.size()==2 && out.back().points[0].value==0.3 && out.back().points[1].time==120 && out.back().points[1].value==0.3,"coincident values coalesce and held endpoint is recorded");
    check(recorder.setArmed(true,130) && recorder.push(AutomationGesture::Value,140,0.4),"new recording after arm transition");
    check(recorder.setRecording(false,150) && recorder.drain(out)==1 && out.back().points.back().time==150,"stop closes capture at producer transport time");
    check(!recorder.setRecording(true,-1) && !recorder.recording(),"invalid gate time leaves control unchanged");
    recorder.setRecording(true,200);recorder.push(AutomationGesture::Begin,200,0.2);
    check(!recorder.push(AutomationGesture::Value,190,0.3),"unannounced backward seek invalidates gesture");
    recorder.push(AutomationGesture::End,210,0.3);
    check(recorder.drain(out)==0,"invalid timeline never yields a partial recording");
    recorder.push(AutomationGesture::Begin,200,0.2);recorder.push(AutomationGesture::Value,210,std::numeric_limits<double>::infinity());
    recorder.push(AutomationGesture::End,220,0.2);
    check(recorder.drain(out)==0,"nonfinite value invalidates whole gesture");
    recorder.push(AutomationGesture::Begin,300,0.1);recorder.push(AutomationGesture::Begin,400,0.2);recorder.push(AutomationGesture::End,450,0.3);
    check(recorder.drain(out)==2,"new begin closes old gesture without crossing a seek");
    AutomationRecorder small(target,1);std::vector<RecordedAutomation> recovered;small.setRecording(true,0);
    check(small.push(AutomationGesture::Begin,0,0.1) && !small.push(AutomationGesture::Value,1,0.2),"bounded ring reports overflow");
    check(small.drain(recovered)==0,"overflow discards a pending prefix");
    small.push(AutomationGesture::End,2,0.3);check(small.drain(recovered)==0,"overflow-tainted end cannot publish partial data");
    small.push(AutomationGesture::Begin,3,0.4);small.drain(recovered);small.push(AutomationGesture::End,4,0.5);small.drain(recovered);
    check(recovered.size()==1 && recovered[0].points.front().time==3 && small.stats().dropped>0,"next healthy gesture recovers after overflow");
    AutomationRecorder bounded(target,8,2);bounded.setRecording(true,0);bounded.push(AutomationGesture::Begin,0,0.1);
    bounded.push(AutomationGesture::Value,1,0.2);bounded.push(AutomationGesture::End,2,0.3);
    std::vector<RecordedAutomation> none;
    check(bounded.drain(none)==0 && bounded.stats().discardedGestures==1,"owner point budget rejects whole gesture");
    AutomationRecorder zero(target,0);zero.setRecording(true,0);
    check(!zero.push(AutomationGesture::Begin,0,0.1) && zero.stats().dropped==1,"zero capacity rejects safely");

    test::TempDirectory temp("automation","recording");auto store=project(temp.path()/"a.adi");std::string error;
    const auto prepared=prepareRecordedAutomation(*store,out.front(),error);
    check(prepared && automation::commitShapeInsertion(*store,*prepared).ok,"captured gesture commits through native atomic replacement");
    const auto persisted=bytes(*store);StreamReader<AutomationPoint> reader(persisted,FourCC::Automation);
    check(reader.ok() && reader.count()==3 && reader.at(0)->value==precise && reader.at(0)->time==wide &&
        (reader.header().flags&StreamFlags::TimeIsNanos)!=0,"recorded data preserves lane units and precision");
    History history(*store);
    check(history.undo().ok && bytes(*store).empty(),"one undo removes the whole recorded gesture");
    check(history.redo().ok && bytes(*store)==persisted,"recording redo exact bytes");
    auto invalid=out.front();invalid.points[0].point_id=8;
    check(!prepareRecordedAutomation(*store,invalid,error),"recorded IDs are assigned only by owner preparation");
    invalid=out.front();invalid.points[0].curve=99;
    check(!prepareRecordedAutomation(*store,invalid,error),"unknown recording curve rejected");
    invalid=out.front();invalid.gesture=0;
    check(!prepareRecordedAutomation(*store,invalid,error),"missing gesture identity rejected");

    // Exactly one producer and one consumer exchange 1000 complete gestures.
    AutomationRecorder concurrent(target,4);std::vector<RecordedAutomation> transferred;std::atomic<std::size_t> ack{0};
    std::atomic<bool> producerOk{true},abort{false};
    std::thread producer([&]{
        concurrent.setRecording(true,0);
        for(std::size_t i=0;i<1000;++i) {
            while(ack.load(std::memory_order_acquire)<i && !abort.load(std::memory_order_relaxed))std::this_thread::yield();
            if(abort.load(std::memory_order_relaxed))return;
            watch=true;
            const bool a=concurrent.push(AutomationGesture::Begin,static_cast<std::int64_t>(i*2),0.1);
            const bool b=concurrent.push(AutomationGesture::End,static_cast<std::int64_t>(i*2+1),0.2);
            watch=false;if(!a || !b)producerOk=false;
        }
    });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(transferred.size()<1000 && std::chrono::steady_clock::now()<deadline) {concurrent.drain(transferred);ack.store(transferred.size(),std::memory_order_release);std::this_thread::yield();}
    abort.store(true,std::memory_order_relaxed);producer.join();
    bool ordered=true;for(std::size_t i=0;i<transferred.size();++i)ordered=ordered && transferred[i].gesture==i+1 &&
        transferred[i].points.size()==2 && transferred[i].points[0].time==static_cast<std::int64_t>(i*2);
    check(producerOk && ordered && transferred.size()==1000 && concurrent.stats().dropped==0,"SPSC preserves every timestamped gesture across actual threads");
    check(allocations==0,"all guarded producer operations remain allocation-free");
}
void passBoundaries() {
    const AutomationTarget target{3, 2, std::nullopt};
    AutomationRecorder latch(target, 16, 64, AutomationCaptureMode::Latch);
    std::vector<RecordedAutomation> passes;
    watch = true;
    const bool released = latch.setRecording(true, 50) &&
                          latch.push(AutomationGesture::Begin, 100, 0.25) &&
                          latch.push(AutomationGesture::Value, 125, 0.5) &&
                          latch.push(AutomationGesture::End, 130, 0.5);
    watch = false;
    check(released && latch.drain(passes) == 0,
          "latch keeps the released value open for the rest of the pass");
    watch = true;
    const bool wrapped = latch.splitAtTransportBoundary(200, 50);
    watch = false;
    check(wrapped && latch.drain(passes) == 1, "loop wrap yields exactly one completed pass");
    if (passes.size() != 1)
        return;
    const auto &first = passes[0].points;
    check(first.size() == 4 && first.front().time == 100 && first[2].time == 130 &&
              first[2].curve == static_cast<std::uint8_t>(Curve::Hold) &&
              first.back().time == 200 && first.back().value == 0.5,
          "the old pass records the held value up to the old loop end");
    watch = true;
    const bool wrappedAgain = latch.splitAtTransportBoundary(200, 50);
    watch = false;
    check(wrappedAgain && latch.drain(passes) == 1 && passes.size() == 2,
          "an untouched later cycle still records its held value");
    if (passes.size() != 2)
        return;
    const auto &next = passes[1].points;
    check(next.size() == 2 && next.front().time == 50 && next.back().time == 200 &&
              next.front().value == 0.5 && next.back().value == 0.5 &&
              next.front().curve == static_cast<std::uint8_t>(Curve::Hold),
          "the next loop begins on its literal native clock with a held knot");
    watch = true;
    const bool retouched = latch.push(AutomationGesture::Begin, 75, 0.75) &&
                           latch.push(AutomationGesture::End, 80, 0.75) &&
                           latch.splitAtTransportBoundary(110, 300) &&
                           latch.setRecording(false, 350);
    watch = false;
    check(retouched && latch.drain(passes) == 2 && passes.size() == 4,
          "retouch stays in its pass while locate and Stop close separate passes");
    if (passes.size() != 4)
        return;
    const auto &beforeLocate = passes[2].points;
    const auto &afterLocate = passes[3].points;
    check(beforeLocate.size() == 4 && beforeLocate.front().time == 50 &&
              beforeLocate[1].time == 75 && beforeLocate[1].value == 0.75 &&
              beforeLocate.back().time == 110 && afterLocate.front().time == 300 &&
              afterLocate.back().time == 350 && afterLocate.front().value == 0.75,
          "separate exact locate clocks preserve new touch and continued value");

    AutomationRecorder touch(target, 8);
    std::vector<RecordedAutomation> touched;
    check(touch.setRecording(true, 0) && touch.push(AutomationGesture::Begin, 10, 0.125) &&
              touch.splitAtTransportBoundary(30, 100) &&
              touch.push(AutomationGesture::End, 110, 0.25) && touch.drain(touched) == 2 &&
              touched[0].points.back().time == 30 && touched[1].points.front().time == 100 &&
              touched[1].points.back().time == 110 && touch.splitAtTransportBoundary(120, 200) &&
              touch.drain(touched) == 0,
          "touch splits while held, closes on release and stays closed at a later boundary");

    AutomationRecorder small(target, 2, 64, AutomationCaptureMode::Latch);
    std::vector<RecordedAutomation> bounded;
    check(small.setRecording(true, 0) && small.push(AutomationGesture::Begin, 10, 0.5) &&
              !small.splitAtTransportBoundary(20, 0) && small.drain(bounded) == 1 &&
              small.setRecording(false, 1) && small.drain(bounded) == 0 &&
              bounded.front().points.back().time == 20,
          "overflow at the split keeps the completed pass and discards the missing start");
    check(small.setRecording(true, 10) && small.push(AutomationGesture::Begin, 11, 0.25) &&
              !small.splitAtTransportBoundary(9, 0) && small.setRecording(false, 15) &&
              small.drain(bounded) == 0,
          "a contradictory clock boundary discards the open pass without partial history");
    check(allocations == 0, "latch, retouch and transport splitting allocate nothing on the producer");

    const test::TempDirectory temp("automation", "write-pass");
    auto store = project(temp.path() / "pass.adi");
    std::string error;
    const auto prepared = prepareRecordedAutomation(*store, passes[2], error);
    check(prepared && automation::commitShapeInsertion(*store, *prepared).ok,
          "a closed pass commits through the existing owner-side writer");
    const auto persisted = bytes(*store);
    const StreamReader<AutomationPoint> replay(persisted, FourCC::Automation);
    check(replay.ok() && replay.count() == 4 && replay.at(0)->time == 50 &&
              replay.at(0)->curve == static_cast<std::uint8_t>(Curve::Hold) &&
              replay.at(1)->time == 75 && replay.at(1)->value == 0.75,
          "the native stream stores the held interval and the exact retouch");
    History history(*store);
    check(history.undo().ok && bytes(*store).empty() &&
              history.redo().ok && bytes(*store) == persisted,
          "the completed pass has one exact undo and redo");
}
void boundedThinning() {
    const test::TempDirectory temp("automation", "thinning");
    constexpr double rate = 48000.0, tolerance = 0.00025;
    for (int clock : {0, 1}) {
        auto exact = project(temp.path() / ("exact-" + std::to_string(clock) + ".adi"));
        auto thinned = project(temp.path() / ("thinned-" + std::to_string(clock) + ".adi"));
        if (clock == 0) {
            for (auto *store : {exact.get(), thinned.get()}) {
                store->db().exec("UPDATE automation_lanes SET time_base=0;"
                                 "INSERT INTO tempo_map(id,pos_ticks,bpm) "
                                 "VALUES(1,0,120),(2,5500000,73),(3,10550000,155);");
            }
        }
        const std::int64_t unit = clock == 0 ? 50000 : 1000000;
        RecordedAutomation pass{{3, 2, std::nullopt}, 1, {}};
        for (std::int64_t i = 0; i <= 400; ++i) {
            AutomationPoint p{};
            p.time = i * unit;
            p.value = 0.2 + static_cast<double>(i) * 0.001 + (i % 2 == 0 ? 0.0 : 0.00005);
            p.curve = static_cast<std::uint8_t>(i == 64 ? Curve::Hold : Curve::Linear);
            if (i == 144)
                p.time = 143 * unit;
            if (i == 250)
                p.value += 0.03;
            pass.points.push_back(p);
        }
        std::string error;
        const auto raw = prepareRecordedAutomation(*exact, pass, error);
        const auto reduced = prepareRecordedAutomation(
            *thinned, pass, error, automation::AutomationThinning{tolerance, rate});
        if (!raw || !reduced ||
            !automation::commitShapeInsertion(*exact, *raw).ok ||
            !automation::commitShapeInsertion(*thinned, *reduced).ok)
            throw std::runtime_error("cannot commit exact and thinned recording fixtures: " + error);

        const auto exactProgram = compileAutomation(rows::readModel(*exact), rate);
        const auto reducedProgram = compileAutomation(rows::readModel(*thinned), rate);
        const auto *a = exactProgram->lane(2), *b = reducedProgram->lane(2);
        if (!a || !b || a->points.empty() || b->points.empty())
            throw std::runtime_error("cannot compile the exact and thinned recording fixtures");
        double maxError = 0.0;
        for (std::int64_t sample = 0; sample <= a->points.back().sample; ++sample)
            maxError = std::max(maxError, std::abs(valueAt(*a, sample) - valueAt(*b, sample)));
        check(maxError <= tolerance && b->points.size() < a->points.size() / 4,
              "projected automation is substantially smaller and bounded at every playback sample");
        const edit::TimeMap time(exact->db());
        const auto sampleAt = [&](std::int64_t position) {
            const double seconds = clock == 0 ? time.seconds(position)
                                                : static_cast<double>(position) * 1e-9;
            return std::llround(seconds * rate);
        };
        const auto hold = sampleAt(64 * unit);
        const auto step = sampleAt(143 * unit);
        check(b->points.front().sample == a->points.front().sample &&
                  b->points.back().sample == a->points.back().sample &&
                  valueAt(*a, hold) == valueAt(*b, hold) && valueAt(*a, hold + 1) == valueAt(*b, hold + 1) &&
                  std::count_if(b->points.begin(), b->points.end(),
                                [step](const auto &p) { return p.sample == step; }) == 2 &&
                  valueAt(*a, step) == valueAt(*b, step),
              "first and last samples, the Hold segment, and a coincident step keep their anchors");
        const auto logs = OpJournal(*thinned).recent();
        check(!logs.empty() && logs.front().actorDetail.find("Automation thinning removed ") == 0 &&
                  OpJournal(*exact).recent().front().actorDetail.empty(),
              "the journal distinguishes a thinned pass from an original sparse capture");
        const auto stored = bytes(*thinned);
        History history(*thinned);
        check(history.undo().ok && bytes(*thinned).empty() && history.redo().ok &&
                  bytes(*thinned) == stored,
              "Undo and Redo restore the exact recorded reduction");
        check(!prepareRecordedAutomation(*thinned, pass, error, automation::AutomationThinning{-1, rate}) &&
                  bytes(*thinned) == stored,
              "invalid thinning policy leaves the active stream untouched");
    }
}
void guardsRecordedPass() {
    const test::TempDirectory temp("automation", "pass-guards");
    constexpr double rate = 48000.0;
    for (int clock : {0, 1}) {
        auto store = project(temp.path() / ("guard-" + std::to_string(clock) + ".adi"));
        auto &db = store->db();
        db.exec("UPDATE project SET sample_rate=48000;"
                "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120),(2,1000000,93);");
        if (clock == 0)
            db.exec("UPDATE automation_lanes SET time_base=0;");
        const std::int64_t unit = clock == 0 ? 10000 : 1000000;
        const auto makePoint = [](std::int64_t time, double value, Curve curve) {
            AutomationPoint p{};
            p.time = time;
            p.value = value;
            p.curve = static_cast<std::uint8_t>(curve);
            return p;
        };
        const auto initial = automation::writePoints(3, 2, nullptr, Payload::array({
            {{"id", 1}, {"time", 0}, {"value", 0.25}, {"curve", 5}, {"tension", 0.65}},
            {{"id", 2}, {"time", 300 * unit}, {"value", 0.75}, {"curve", 1}, {"tension", 0}}}));
        if (!OpJournal(*store).commit(initial).ok)
            throw std::runtime_error("cannot create the authored exterior curve");
        const auto before = compileAutomation(rows::readModel(*store), rate);
        const auto beforeDigest = digestProject(*store).text;
        const RecordedAutomation pass{{3, 2, std::nullopt}, 2,
                                      {makePoint(75 * unit, 0.125, Curve::Hold),
                                       makePoint(150 * unit, 0.625, Curve::Linear),
                                       makePoint(225 * unit, 0.5, Curve::Linear)}};
        std::string error;
        const auto prepared = prepareRecordedAutomation(*store, pass, error);
        check(prepared && prepared->requests.size() == 1 &&
                  prepared->requests.front().capturedTransaction &&
                  prepared->requests.front().label == "Record Automation" &&
                  digestProject(*store).text == beforeDigest,
              "a pass captures both guards and one Undo label before it writes");
        if (!prepared || !automation::commitShapeInsertion(*store, *prepared).ok)
            throw std::runtime_error("cannot commit the guarded write pass: " + error);
        const auto after = compileAutomation(rows::readModel(*store), rate);
        const auto *outside = before->lane(2), *recorded = after->lane(2);
        if (!outside || !recorded)
            throw std::runtime_error("cannot compile the guarded write pass");
        const edit::TimeMap time(db);
        const auto sampleAt = [&](std::int64_t pos) {
            const double seconds = clock == 0 ? time.seconds(pos) : static_cast<double>(pos) * 1e-9;
            return std::llround(seconds * rate);
        };
        const auto begin = sampleAt(75 * unit), end = sampleAt(225 * unit);
        double maxError = 0.0;
        for (std::int64_t sample = 0; sample <= outside->points.back().sample; ++sample) {
            if (sample < begin || sample >= end)
                maxError = std::max(maxError,
                                    std::abs(valueAt(*outside, sample) - valueAt(*recorded, sample)));
        }
        check(maxError < 1e-9 && valueAt(*recorded, end) == valueAt(*outside, end) &&
                  valueAt(*recorded, begin + 1) == 0.125 &&
                  valueAt(*recorded, sampleAt(150 * unit)) == 0.625,
              "the held recording plays inside while every exterior sample remains intact");
        History history(*store);
        const auto afterDigest = digestProject(*store).text;
        check(history.undo().ok && digestProject(*store).text == beforeDigest &&
                  history.redo().ok && digestProject(*store).text == afterDigest,
              "the guarded pass, curves and data replay as one exact undo");
        const auto stale = prepareRecordedAutomation(*store, pass, error);
        if (!stale)
            throw std::runtime_error(error);
        db.exec("UPDATE tempo_map SET bpm=100 WHERE id=2");
        const auto changed = digestProject(*store).text;
        check(!automation::commitShapeInsertion(*store, *stale).ok &&
                  digestProject(*store).text == changed,
              "changed captured tempo refuses without publishing any part of the pass");
    }
}
} // namespace
int main(){try{run();passBoundaries();boundedThinning();guardsRecordedPass();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
