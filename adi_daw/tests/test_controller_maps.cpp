// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/controller_maps.hpp"
#include "adi/history.hpp"
#include "adi/digest.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace adi;
int checks=0, failures=0;
void check(bool ok,const char* label) { ++checks; if(!ok) { ++failures; std::printf("FAIL: %s\n",label); } }
std::unique_ptr<Store> project(const std::filesystem::path& path) {
    StoreError error=StoreError::Ok;
    auto s=Store::create(path,error);
    if(!s) throw std::runtime_error("create store");
    s->db().exec("INSERT INTO project(id,name) VALUES(1,'Learn'); INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Track');"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,param_name) VALUES(2,'track',1,'volume','Display only')");
    return s;
}
void run() {
    test::TempDirectory temp("controller", "learn");
    auto s=project(temp.path()/"a.adi");
    auto other=project(temp.path()/"b.adi");
    const ControllerCc cc{"Keyboard",1,7};
    ControllerPolicy policy{true,cc,ControllerTakeover::Pickup};
    std::string error;
    const std::array absolute{0,16,48,100,127};
    check(!learnController(*s,5,2,cc,absolute,policy,error),"Learn refuses Focus Dial");
    policy.focusDial.reset(); policy.remoteEnabled=false;
    check(!learnController(*s,5,2,cc,absolute,policy,error),"Learn requires Remote role");
    policy.remoteEnabled=true;
    check(!learnController(*s,5,999,cc,absolute,policy,error),"Learn requires real lane");
    check(!learnController(*s,5,2,{"Keyboard",0,7},absolute,policy,error),"one-based channel required");
    check(!learnController(*s,5,2,cc,std::array{128},policy,error),"invalid CC rejected");
    check(!learnController(*s,5,2,cc,std::span<const int>{},policy,error),"empty observation rejected");
    check(detectControllerMode(std::array{1,127,0,1})==ControllerMode::Relative,"twos cluster");
    check(detectControllerMode(std::array{63,64,65})==ControllerMode::Relative,"offset cluster");
    check(detectControllerMode(std::array{64,64})==ControllerMode::Absolute,"neutral alone ambiguous");
    check(detectControllerMode(absolute)==ControllerMode::Absolute,"absolute sweep");
    check(detectControllerMode(std::array{1,65,127})==ControllerMode::Absolute,"mixed clusters not relative");
    auto learned=learnController(*s,5,2,cc,absolute,policy,error);
    check(learned.has_value(),"resolved op built"); if(!learned) return;
    check(learned->payload.at("binding").at("target_param")=="volume","target is param_ref not display name");
    check(learned->payload.at("binding").at("takeover")==1,"absolute uses requested takeover");
    check(learned->payload.at("binding").size()==14 && learned->payload.size()==2,"only resolved binding enters payload");
    auto relative=learnController(*s,6,2,cc,std::array{127,1},policy,error);
    check(relative && relative->payload.at("binding").at("mode")==1 && relative->payload.at("binding").at("takeover")==0,"relative skips absolute takeover");
    OpJournal journal(*s), replay(*other); History history(*s);
    const auto empty=digestProject(*s).text;
    check(journal.commit(*learned).ok,"bind commits");
    const auto first=readControllerBindings(*s);
    check(first.size()==1 && first[0].binding==learned->payload.at("binding"),"all columns stored");
    check(history.undo().ok && readControllerBindings(*s).empty(),"one undo removes new binding");
    check(digestProject(*s).text==empty,"undo restores project exactly");
    check(history.redo().ok,"redo binding");
    OpRequest replacement=*learned; replacement.payload["binding"]["target_param"]="pan";
    replacement.payload["binding"]["range_min"]=-1.0;
    check(journal.commit(replacement).ok,"relearn same id");
    check(history.undo().ok && readControllerBindings(*s)[0].binding==first[0].binding,"one undo restores entire replaced row");
    check(history.redo().ok,"redo replacement");
    OpRequest remove; remove.opType="controller.unbind"; remove.payload={{"id",5}};
    check(journal.commit(remove).ok && readControllerBindings(*s).empty(),"unbind removes row");
    check(history.undo().ok && readControllerBindings(*s)[0].binding==replacement.payload.at("binding"),"unbind inverse restores every column");
    // Replay needs neither settings nor a Learn session. Different live machine
    // policy may shadow the result but cannot alter the stored project.
    policy.focusDial=cc; policy.remoteEnabled=false;
    check(replay.commit(*learned).ok && replay.commit(replacement).ok,"resolved operations replay without settings");
    check(digestProject(*s).text==digestProject(*other).text,"replay project identical despite live reservation");
    const auto bindings=readControllerBindings(*other);
    check(controllerBindingShadowed(bindings[0],policy),"imported collision shown as shadowed");
    int focus=0,bound=0;
    const auto onFocus=[&](int v){ if(v==37) ++focus; };
    const auto onBinding=[&](const ControllerBinding&,int){++bound;};
    check(dispatchController(cc,37,policy,bindings,onFocus,onBinding)==ControllerDispatch::FocusDial && focus==1 && bound==0,"Focus wins even when Remote is off");
    policy.remoteEnabled=true;
    check(dispatchController(cc,37,policy,bindings,onFocus,onBinding)==ControllerDispatch::FocusDial && focus==2 && bound==0,"shadowed binding never fires with Remote on");
    policy.focusDial=ControllerCc{"Other",1,7};
    check(!controllerBindingShadowed(bindings[0],policy),"moving Focus unshadows binding");
    check(dispatchController(cc,37,policy,bindings,onFocus,onBinding)==ControllerDispatch::Binding && bound==1,"unshadowed binding fires");
    policy.remoteEnabled=false;
    check(dispatchController(cc,37,policy,bindings,onFocus,onBinding)==ControllerDispatch::Ignored && bound==1,"Remote off suppresses project binding");
    policy.remoteEnabled=true;
    check(dispatchController({"Keyboard",2,7},37,policy,bindings,onFocus,onBinding)==ControllerDispatch::Ignored,"channel identity matters");
    auto disabled=bindings; disabled[0].binding["enabled"]=false;
    check(dispatchController(cc,37,policy,disabled,onFocus,onBinding)==ControllerDispatch::Ignored,"disabled binding ignored");
    const auto before=digestProject(*s).text;
    for(const char* key:{"mode","channel","target_id"}) {
        auto bad=*learned; bad.payload["binding"][key]=999;
        check(!journal.commit(bad).ok && digestProject(*s).text==before,"invalid value rolls back atomically");
    }
    auto bad=*learned; bad.payload["binding"]["focusDial"]="ambient";
    check(!journal.commit(bad).ok,"unknown binding field rejected");
    bad=*learned; bad.payload.erase("binding"); check(!journal.commit(bad).ok,"missing binding rejected");
    remove.payload["id"]=999; check(!journal.commit(remove).ok,"missing unbind rejected");
    // Nullable protocol columns survive a delete/undo too.
    auto osc=*learned; auto& b=osc.payload["binding"];
    b["protocol"]="osc"; b["msg_type"]="osc_path"; b["osc_path"]="/gain"; b["channel"]=nullptr; b["msg_num"]=nullptr;
    check(journal.commit(osc).ok,"OSC row accepted for full-row inverse preservation");
    remove.payload["id"]=5;
    check(journal.commit(remove).ok && history.undo().ok && readControllerBindings(*s)[0].binding==b,"null protocol fields restored exactly");
}
}
int main() {
    try { run(); } catch(const std::exception& e) { check(false,e.what()); }
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
