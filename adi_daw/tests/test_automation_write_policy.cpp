// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/automation_recording.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <stdexcept>
namespace {
using namespace adi;
using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
bool plays(Session& s) {const auto* b=s.deviceAutomation();return b && b->byDevice.contains(1);}
double emitted(Session& s) {
    if(!plays(s))return -1;
    std::array<Event,8> storage{};EventList events(storage.data(),static_cast<std::int32_t>(storage.size()));
    s.deviceAutomation()->byDevice.at(1)->emit(events,16);return events.empty()?-1:events.at(0).value;
}
void run() {
    test::TempDirectory temp("automation","write_policy");StoreError error=StoreError::Ok;
    auto store=Store::createEmpty(temp.path()/"a.adi",error);if(!store)throw std::runtime_error("store");
    store->db().exec("INSERT INTO project(id,name) VALUES(1,'Write');"
        "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Track'),(9,'master','Master');"
        "INSERT INTO mixer_strip(track_id) VALUES(1);"
        "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.write','Write','effect');"
        "INSERT INTO device_chains(id,track_id) VALUES(1,1);INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Write');"
        "INSERT INTO plugin_params(device_id,param_id,name,normalized_value) VALUES(1,'7','P',0.2);"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain) VALUES(2,'device',1,'7',1,'normalized'),(4,'track',1,'volume',1,'real')");
    AutomationPoint p{};p.point_id=1;p.curve=1;p.value=0.2;
    auto blob=writeStream<AutomationPoint>(FourCC::Automation,std::span<const AutomationPoint>(&p,1),1,StreamFlags::SortedByTime|StreamFlags::TimeIsNanos);
    SQLite::Statement q(store->db(),"INSERT INTO automation_data(id,lane_id,data) VALUES(3,2,?)");q.bind(1,blob.data(),static_cast<int>(blob.size()));q.exec();
    p.value=-12;
    check(store->putAutomationData(4,std::nullopt,writeStream<AutomationPoint>(FourCC::Automation,std::span<const AutomationPoint>(&p,1),1,StreamFlags::SortedByTime|StreamFlags::TimeIsNanos)),"track automation fixture");
    Session session;
    check(session.automationArmed() && !session.automationRecording(),"Session Arm defaults on, transport recording off");
    check(!session.beginAutomationWrite(2),"unloaded Session refuses writes");
    check(session.load(*store,{},SessionSpec{}),"load engine Session with automation");
    check(plays(session) && emitted(session)==0.2,"baseline parameter events follow the stored curve");
    check(!session.beginAutomationWrite(2),"record-off gate refuses a new write");
    session.setAutomationWriteState(false,true);
    check(!session.beginAutomationWrite(2),"Arm-off gate refuses a new write");
    session.setAutomationWriteState(true,true);
    check(!session.beginAutomationWrite(999),"unknown lane refused");
    check(session.beginAutomationWrite(2) && session.beginAutomationWrite(2),"begin write is idempotent");
    check(!plays(session) && session.writingAutomationLanes().size()==1 && !session.automationOverridden(),"active write suspends events without ordinary override");
    OpRequest manual;manual.opType="device.setParam";manual.payload={{"dev",1},{"param","7"},{"norm",0.8}};
    check(OpJournal(*store).commit(manual).ok && session.refresh(*store),"manual parameter edit during armed recording");
    check(!session.automationOverridden() && !plays(session),"recorded manual movement does not permanently override the lane");
    check(session.reenableAutomation() && !plays(session),"Re-Enable does not cancel active recording suspension");
    AutomationRecorder recorder({3,2,std::nullopt},8);recorder.setRecording(true,0);
    recorder.push(AutomationGesture::Begin,0,0.8);recorder.push(AutomationGesture::End,1000000000,0.9);
    std::vector<RecordedAutomation> gestures;recorder.drain(gestures);
    check(gestures.size()==1,"timestamped capture completed");if(gestures.empty())return;
    session.setAutomationWriteState(true,false);
    check(session.writingAutomationLanes().contains(2) && !plays(session),"record stop waits for captured-data commit before resuming old automation");
    std::string why;const auto prepared=prepareRecordedAutomation(*store,gestures.front(),why);
    check(prepared && automation::commitShapeInsertion(*store,*prepared).ok && session.refresh(*store),"captured data committed and refreshed while suspended");
    check(session.finishAutomationWrite(2) && !session.automationOverridden() && plays(session),"finish follows newly recorded automation");
    check(emitted(session)==0.8,"resumed event source reads the committed recording");
    check(!session.finishAutomationWrite(2),"finishing a nonexistent write is refused");
    manual.payload["norm"]=0.3;
    check(OpJournal(*store).commit(manual).ok && session.refresh(*store) && session.overriddenLanes().contains(2),"ordinary record-off manual override behavior survives");
    session.setAutomationWriteState(true,true);
    check(session.beginAutomationWrite(2) && !session.automationOverridden(),"recording takes over a previously overridden lane");
    check(session.finishAutomationWrite(2,false) && session.overriddenLanes().contains(2) && !plays(session),"failed/abandoned recording can explicitly retain manual override");
    check(session.reenableAutomation(2) && plays(session),"retained manual override can be re-enabled");
    check(session.beginAutomationWrite(4),"strip automation write begins");
    OpRequest fader;fader.opType="mixer.setVolume";fader.payload={{"id",1},{"db",-6.0}};
    check(OpJournal(*store).commit(fader).ok && session.refresh(*store) && !session.overriddenLanes().contains(4),"strip movement during write does not light manual override");
    check(session.finishAutomationWrite(4,false) && session.overriddenLanes().contains(4),"strip write abandonment has explicit manual fallback");
    check(session.reenableAutomation(4),"strip manual fallback re-enables");
    check(session.beginAutomationWrite(2),"begin before lane deletion");
    check(OpJournal(*store).commit(automation::deleteLane(2)).ok && session.refresh(*store),"delete active lane and refresh");
    check(session.writingAutomationLanes().empty() && !session.finishAutomationWrite(2),"departed lane drops recording suspension");
    check(History(*store).undo().ok && session.refresh(*store) && plays(session),"restored lane resumes without stale writing state");
}
} // namespace
int main(){try{run();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
