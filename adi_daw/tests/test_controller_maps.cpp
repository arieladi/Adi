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
    auto s=Store::createEmpty(path,error);
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
    check(detectControllerMode(std::array{63,64,65})==ControllerMode::BinaryOffset,"offset cluster retains its wire encoding");
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
void directTargets() {
    test::TempDirectory temporary("controller","direct");auto store=project(temporary.path()/"direct.adi");auto replay=project(temporary.path()/"replay.adi");
    ControllerPolicy policy;policy.remoteEnabled=true;const ControllerCc source{"Remote knobs",3,12};const std::array observations{0,20,70,127};std::string error;
    const auto before=digestProject(*store).text;
    auto pan=learnControllerTarget(10,"track",1,"pan",source,observations,policy,error);
    check(pan && readControllerBindings(*store).empty() && digestProject(*store).text==before && store->db().execAndGet("SELECT count(*) FROM automation_lanes WHERE param_ref='pan'").getInt()==0,
        "control target is prepared without an automation lane or project writes");
    if(!pan)return;
    check(pan->payload.at("binding").at("target_kind")=="track" && pan->payload.at("binding").at("target_id")==1 && pan->payload.at("binding").at("target_param")=="pan", "direct Learn captures the exact target address");
    auto lane=learnController(*store,11,2,source,observations,policy,error);auto direct=learnControllerTarget(11,"track",1,"volume",source,observations,policy,error);
    check(lane && direct && lane->payload==direct->payload,"lane and direct Learn share one detection and policy implementation");
    policy.takeover=ControllerTakeover::Scale;policy.remoteEnabled=false;policy.focusDial=source;
    check(OpJournal(*store).commit(*pan).ok && OpJournal(*replay).commit(*pan).ok && readControllerBindings(*store).front().binding==readControllerBindings(*replay).front().binding && digestProject(*store).text==digestProject(*replay).text,
        "captured direct binding replays independently of later machine policy");
    check(readControllerBindings(*store).front().binding.at("takeover")==1 && controllerBindingShadowed(readControllerBindings(*store).front(),policy),
        "captured absolute takeover stays intact and current Focus reservation still shadows dispatch");
    check(History(*store).undo().ok && readControllerBindings(*store).empty() && digestProject(*store).text==before && History(*store).redo().ok,"direct binding undo restores absence and redo restores the target");
    policy.remoteEnabled=true;policy.focusDial.reset();
    const auto relative=learnControllerTarget(12,"track",1,"volume",source,std::array{127,1},policy,error);
    check(relative && relative->payload.at("binding").at("mode")==1 && relative->payload.at("binding").at("takeover")==0,"relative direct Learn bypasses absolute takeover");
    policy.focusDial=source;check(!learnControllerTarget(13,"track",1,"pan",source,observations,policy,error),"direct target refuses reserved Focus Dial");
    policy.focusDial.reset();policy.remoteEnabled=false;check(!learnControllerTarget(13,"track",1,"pan",source,observations,policy,error),"direct target requires a Remote-enabled port");
    policy.remoteEnabled=true;
    check(!learnControllerTarget(0,"track",1,"pan",source,observations,policy,error) &&
        !learnControllerTarget(13,"unknown",1,"pan",source,observations,policy,error) &&
        !learnControllerTarget(13,"track",0,"pan",source,observations,policy,error) &&
        !learnControllerTarget(13,"track",1,"",source,observations,policy,error) &&
        !learnControllerTarget(13,"track",1,"pan",{"Remote knobs",0,12},observations,policy,error) &&
        !learnControllerTarget(13,"track",1,"pan",source,std::array{128},policy,error) &&
        !learnControllerTarget(13,"track",1,"pan",source,std::span<const int>{},policy,error),"invalid binding/address/CC/observations refuse during preparation");
    policy.takeover=static_cast<ControllerTakeover>(99);check(!learnControllerTarget(13,"track",1,"pan",source,observations,policy,error),"unknown takeover policy refuses");
    policy.takeover=ControllerTakeover::Pickup;
    const auto missing=learnControllerTarget(13,"track",999,"pan",source,observations,policy,error);const auto current=digestProject(*store).text;
    check(missing && !OpJournal(*store).commit(*missing).ok && digestProject(*store).text==current,"commit validates current target existence without creating a lane");
}

void encoderWires() {
    struct Encoding { ControllerMode mode, linear; int left, right, neutral, fastLeft, fastRight; };
    using Mode = ControllerMode;
    constexpr std::array encoders{
        Encoding{Mode::Relative, Mode::LinearTwosComplement, 127, 1, 0, 124, 4},
        Encoding{Mode::BinaryOffset, Mode::LinearBinaryOffset, 63, 65, 64, 60, 68},
        Encoding{Mode::SignedBit, Mode::LinearSignedBit, 65, 1, 0, 68, 4},
        Encoding{Mode::SignedBitInverted, Mode::LinearSignedBitInverted, 1, 65, 0, 4, 68}
    };
    for (const auto& encoder : encoders) {
        check(detectControllerMode(std::array{encoder.neutral, encoder.left, encoder.left},
                  ControllerLearnMotion::SlowLeftTurn) == encoder.mode,
              "a witnessed slow left turn identifies the encoding and polarity");
        for (const auto mode : {encoder.mode, encoder.linear}) {
            check(decodeRelativeControllerTicks(mode, encoder.left) == -1 &&
                      decodeRelativeControllerTicks(mode, encoder.right) == 1 &&
                      decodeRelativeControllerTicks(mode, encoder.neutral) == 0,
                  "relative encodings report one signed step and their neutral marker");
            check(decodeRelativeControllerTicks(mode, encoder.fastLeft) == -4 &&
                      decodeRelativeControllerTicks(mode, encoder.fastRight) == 4,
                  "both variants retain the exact tick magnitude from the wire");
        }
    }
    check(static_cast<int>(Mode::Relative) == 1 && decodeRelativeControllerTicks(Mode::Relative, 64) == -64,
          "old mode 1 remains two's complement including its signed minimum");
    check(!decodeRelativeControllerTicks(Mode::Absolute, 1) && !decodeRelativeControllerTicks(Mode::Toggle, 1) &&
              !decodeRelativeControllerTicks(Mode::BinaryOffset, -1) && !decodeRelativeControllerTicks(Mode::Relative, 128),
          "relative decoding refuses other controller kinds and invalid wire words");
    check(detectControllerMode(std::array{0,0}, ControllerLearnMotion::SlowLeftTurn) == Mode::Absolute &&
              detectControllerMode(std::array{1,65}, ControllerLearnMotion::SlowLeftTurn) == Mode::Absolute,
          "neutral or contradictory samples cannot witness an encoder's direction");
    using Value = ControllerAbsoluteFormat;
    check(decodeAbsoluteControllerValue(Value::SevenBit, 127) == 1 &&
              decodeAbsoluteControllerValue(Value::FourteenBit, 0) == 0 &&
              decodeAbsoluteControllerValue(Value::FourteenBit, 16383) == 1 &&
              !decodeAbsoluteControllerValue(Value::SevenBit, 128) &&
              !decodeAbsoluteControllerValue(Value::FourteenBit, 16384),
          "absolute endpoints use the declared MIDI width and refuse overflows");
    const auto belowCenter = decodeAbsoluteControllerValue(Value::PitchBend, 8191);
    check(decodeAbsoluteControllerValue(Value::PitchBend, 0) == 0 && belowCenter && *belowCenter < 0.5 &&
              decodeAbsoluteControllerValue(Value::PitchBend, 8192) == 0.5 &&
              decodeAbsoluteControllerValue(Value::PitchBend, 16383) == 1 &&
              !decodeAbsoluteControllerValue(Value::PitchBend, -1),
          "pitch bend uses an exact center with distinct low and high halves");
    check(!controllerCcIsDown(63) && controllerCcIsDown(64) && controllerCcIsDown(127) && !controllerCcIsDown(128),
          "note-like CC goes down at 64 and ends at 127");
    check(controllerRangeActive(0.25, 0.25, 0.75) && controllerRangeActive(0.75, 0.25, 0.75) &&
              !controllerRangeActive(0.25, 0.75, 0.25) && !controllerRangeActive(0.5, 0.75, 0.25) &&
              controllerRangeActive(0.1, 0.75, 0.25), "inverted activator bounds complement the inclusive interval");
}

void encoderBindings() {
    test::TempDirectory temp("controller", "encoder-rows");
    auto store = project(temp.path() / "encoders.adi");
    ControllerPolicy policy;
    policy.remoteEnabled = true;
    policy.learnMotion = ControllerLearnMotion::SlowLeftTurn;
    const ControllerCc source{"Encoder", 1, 10};
    std::string error;
    const auto learned = learnControllerTarget(3, "track", 1, "volume", source, std::array{0,65,65}, policy, error);
    if (!learned) throw std::runtime_error(error);
    check(learned->payload.at("binding").at("mode") == 4 && learned->payload.at("binding").at("takeover") == 0,
          "Learn stores the witnessed signed-bit mode and bypasses absolute takeover");
    OpJournal journal(*store);
    History history(*store);
    for (int mode = 0; mode <= 9; ++mode) {
        auto request = *learned;
        request.payload["binding"]["mode"] = mode;
        check(journal.commit(request).ok && readControllerBindings(*store).front().binding.at("mode") == mode,
              "every documented mode commits as its exact numeric code");
    }
    const auto encoder = readControllerBindings(*store).front().binding;
    auto keyboard = *learned;
    auto& key = keyboard.payload["binding"];
    key["device_name"] = "";
    key["protocol"] = "key";
    key["channel"] = nullptr;
    key["msg_type"] = "key";
    key["msg_num"] = 65560;
    key["mode"] = static_cast<int>(ControllerMode::Toggle);
    key["range_min"] = 1.0;
    key["range_max"] = 0.0;
    check(journal.commit(keyboard).ok && readControllerBindings(*store).front().binding == key,
          "a project Key Map stores a special key without a machine device and preserves inverted bounds");
    check(history.undo().ok && readControllerBindings(*store).front().binding == encoder &&
              history.redo().ok && readControllerBindings(*store).front().binding == key,
          "undo and redo preserve complete older encoder and new keyboard rows");
    auto invalid = keyboard;
    invalid.payload["binding"]["channel"] = 1;
    check(!journal.commit(invalid).ok, "a keyboard binding cannot name a MIDI channel");
    invalid = keyboard;
    invalid.payload["binding"]["protocol"] = "midi";
    check(!journal.commit(invalid).ok, "keyboard messages require the keyboard protocol");
    invalid = keyboard;
    invalid.payload["binding"]["msg_num"] = -1;
    check(!journal.commit(invalid).ok, "a project Key Map refuses an invalid key code");
}

}
int main() {
    try { run(); directTargets(); encoderWires(); encoderBindings(); } catch(const std::exception& e) { check(false,e.what()); }
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
