// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/sample_slots.hpp"
#include "adi/sampler_declarations.hpp"
#include "adi/dsp/sampler.hpp"
#include "adi/pd_builtins/oneshot_midi.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/pd_builtins/native_devices.hpp"
#include "juce/builtin_samples.hpp"
#include "adi/blob.hpp"
#include "adi/media/blake3.hpp"
#include <cstring>
#include "temp_directory.hpp"
#include "replace_operator_new.hpp"
#include "adi/builtin_catalog.hpp"
#include "juce/builtin_host.hpp"
#include "juce/device_model.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace {
std::atomic<int> allocations{0};thread_local bool counting=false;
}
void adi::test::countReplacedNew() noexcept{if(counting)allocations.fetch_add(1,std::memory_order_relaxed);}
namespace {
using namespace adi;using namespace adi::device;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
rows::PluginRef reference(std::string uid) {
    rows::PluginRef result;result.id=1;result.format="internal";result.uid=std::move(uid);result.name="Built-in";result.vendor="ADI";result.version="1";return result;
}
#if ADI_WITH_PD
std::unique_ptr<DeviceInstance> load(const char* uid) {
    auto ref=reference(uid);std::string error;DeviceRequest request;request.ref=&ref;
    auto instance=makeBuiltinDevice(request,error);
    if(!instance)throw std::runtime_error(error);
    return instance;
}
#endif
struct Project {
    test::TempDirectory temporary{"builtin_devices","project"};std::unique_ptr<Store> store;
    Project() {
        StoreError error{};store=Store::create(temporary.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio'),(9,'master','Main')");
    }
};
void catalogueAndHost() {
    check(builtinCatalog().size()==21 && !findBuiltin("unregistered"),"catalogue has stable known entries and rejects unknown UID");
    std::set<std::string_view> identities;int available=0;
    for(const auto& info:builtinCatalog()) {
        check(identities.insert(info.uid).second && !info.name.empty() && !info.category.empty() && !info.group.empty(),"catalogue identities are unique and labels/category/folder are present");
        std::string error;auto ref=reference(std::string(info.uid));DeviceRequest request;request.ref=&ref;
        auto instance=makeBuiltinDevice(request,error);
        if(!info.available){check(!instance && !error.empty(),"unavailable backend refuses by name");continue;}
        ++available;check(instance && instance->loaded() && instance->identity().uid==info.uid && !builtinDeclarations(info.uid).empty(),"canonical factory creates supported device without a binary scan or file path");
        if(!instance)continue;
        check(instance->wantsDeviceTransport()==info.usesTransportPosition,
              "host transport demand follows the catalog capability");
        check(instance->paramCount()>0 && instance->paramAt(0) && instance->findParam(instance->paramAt(0)->id),"declared parameter identities and descriptors are readable");
        instance->prepare(48000,64);
        std::array<std::array<float,64>,2> input{},output{};float* in[]={input[0].data(),input[1].data()};float* out[]={output[0].data(),output[1].data()};
        NodeIo io;io.in=in;io.out=out;io.channels=2;io.frames=64;
        instance->process(io);
        allocations.store(0,std::memory_order_relaxed);counting=true;
        {audio::CallbackScope callback;for(int block=0;block<4;++block)instance->process(io);}
        counting=false;
        check(allocations.load(std::memory_order_relaxed)==0,"canonical built-in processing allocates nothing");
    }
#if ADI_WITH_PD
    check(available==21,"all registered native canonical factories are available");
#else
    check(available==0,"PD-disabled build advertises no unavailable backend as loadable");
#endif
    auto fake=reference("not.a.builtin");DeviceRequest request;request.ref=&fake;std::string error;
    check(!makeBuiltinDevice(request,error) && !error.empty(),"unknown internal reference stays a named placeholder failure");
    int delegated=0;
    auto composed=withBuiltinDevices([&](const DeviceRequest&,std::string& message){++delegated;message="external";return std::unique_ptr<DeviceInstance>{};});
    fake.format="clap";(void)composed(request,error);
    check(delegated==1 && error=="external","composition retains external-format loader");
}
void paramsAndState() {
#if ADI_WITH_PD
    auto instance=load("org.adidaw.builtin.utility");
    check(instance->findParam("1")->shape==ParamShape::Switch && instance->findParam("3")->shape==ParamShape::Menu &&
          instance->findParam("11")->shape==ParamShape::Continuous && instance->findParam("11")->hasRealRange,"control shapes and real units follow declarations");
    check(instance->paramText("3",1)=="Stereo" && instance->paramText("9",0).find("Hz")!=std::string::npos,"host renders menu items and declared units");
    check(instance->setParam("11",ParamValue::fromNormalized(0.8)) && std::abs(instance->getParam("11").real-4)<1e-4,"normalized setter exposes its real value before the next process block");
    const auto saved=instance->saveState("state");auto payload=Payload::from_cbor(saved);payload["future"]={{"kept",7}};payload["params"]["future-param"]=0.3;
    check(instance->loadState("state",Payload::to_cbor(payload)),"known state accepts and retains future fields and unknown parameter IDs");
    const auto kept=Payload::from_cbor(instance->saveState("state"));
    check(kept.at("future")==payload.at("future") && kept.at("params").at("future-param")==0.3,"state round trip preserves future opaque fields");
    payload["params"]["1"]=1.0;payload["params"]["11"]=-1.0;
    const auto before=instance->getParam("1").normalized;
    check(!instance->loadState("state",Payload::to_cbor(payload)) && instance->getParam("1").normalized==before,"bad later state value refuses before any parameter changes");
    check(!instance->setParam("missing",ParamValue{}) && !instance->setParam("11",ParamValue::fromNormalized(2)) &&
          !instance->loadState("state",{1,2,3}) && !instance->loadState("component",saved),"missing parameters, bad values and unsupported/malformed state refuse");
    auto restored=load("org.adidaw.builtin.utility");
    check(restored->loadState("state",saved) && restored->getParam("11").normalized==instance->getParam("11").normalized,"stored state restores parameter intent exactly");
    check(!restored->setExpressionRoute(RouteChoice::NoteExpression) && restored->setExpressionRoute(RouteChoice::MpeMidi),"format route supports its declared MIDI boundary");
    // DF-E57: a partial state (a newer version's added parameters, a partial
    // preset) resets what it omits to the default, not to the value before.
    {
        auto device=load("org.adidaw.builtin.utility");
        check(device->setParam("11",ParamValue::fromNormalized(0.8)) && device->setParam("12",ParamValue::fromNormalized(0.9)),
              "two parameters moved from their defaults");
        const auto held=device->getParam("12").normalized;
        auto partial=Payload::from_cbor(device->saveState("state"));
        // Omitted everywhere the state can carry it: its position, its real value, its unit.
        for(const char* part:{"params","real","units"}) {
            if(partial.contains(part)) {
                partial[part].erase("11");
            }
        }
        check(device->loadState("state",Payload::to_cbor(partial)),"a state without one parameter loads");
        check(std::abs(device->getParam("11").normalized-device->findParam("11")->defaultValue.normalized)<1e-6 &&
              device->getParam("12").normalized==held,
              "the omitted parameter returns to its default; the one the state holds keeps its value (DF-E57)");
    }
#endif
}
void insertionAndHistory() {
    {
        Project project;auto& store=*project.store;auto& db=store.db();
        rows::PluginRef ref;
        ref.uid="test.shell";ref.name="Third-party effect";ref.vendor="Test vendor";ref.version="2.1";
        ref.subtype="effect";ref.pathHint="fixture/shell.vst3";ref.format="vst3";ref.isShell=true;ref.shellId=71;
        const auto added=preparePluginInsert(store,ref,1);
        check(added.ok && commitBuiltinInsert(store,added).ok &&
              db.execAndGet("SELECT shell_id FROM plugin_refs").getInt64()==71 &&
              db.execAndGet("SELECT path_hint FROM plugin_refs").getString()==ref.pathHint,
              "third-party insertion commits the exact resolved plugin identity, path and channel");
        check(History(store).undo().ok && db.execAndGet("SELECT count(*) FROM devices").getInt()==0 &&
              db.execAndGet("SELECT count(*) FROM plugin_refs").getInt()==0 &&
              db.execAndGet("SELECT count(*) FROM device_chains").getInt()==0,
              "one Undo removes only the external insertion and its new ref/chain");
        check(History(store).redo().ok && preparePluginInsert(store,ref,1).referenceId==added.referenceId,
              "redo and another insertion reuse the resolved third-party identity");
        db.exec("INSERT INTO tracks(id,kind,name) VALUES (8,'vca','VCA')");
        const auto vca = preparePluginInsert(store, ref, 8);
        check(!vca.ok && vca.error.find("VCA") != std::string::npos && vca.requests.empty(),
              "hosted insertion refuses a VCA before preparing any edits");
        ref.format="au";check(!preparePluginInsert(store,ref,1).ok,"unsupported external formats refuse before mutation");
    }
#if ADI_WITH_PD
    Project project;auto& store=*project.store;auto& db=store.db();
    const auto prepared=prepareBuiltinInsert(store,"org.adidaw.builtin.utility",1);
    check(prepared.ok && prepared.options.expectHead && !prepared.options.expectHead->has_value() &&
          db.execAndGet("SELECT count(*) FROM plugin_refs").getInt()==0 && db.execAndGet("SELECT count(*) FROM devices").getInt()==0,"browser preparation captures root/head/ref/chain/IDs without project writes");
    const auto result=commitBuiltinInsert(store,prepared);
    if(!result.ok)std::printf("insert: %s\n",result.error.c_str());
    check(result.ok && result.seqs.size()==3 && db.execAndGet("SELECT count(*) FROM devices").getInt()==1,"missing plug-in ref, chain and device are one native undo step");
    check(History(store).undo().ok && db.execAndGet("SELECT count(*) FROM devices").getInt()==0 &&
          db.execAndGet("SELECT count(*) FROM plugin_refs").getInt()==0 && db.execAndGet("SELECT count(*) FROM device_chains").getInt()==0,"undo removes exactly the rows the insertion created");
    check(History(store).redo().ok && db.execAndGet("SELECT plugin_ref_id FROM devices").getInt64()==prepared.referenceId,"redo restores captured identities and links");
    const auto second=prepareBuiltinInsert(store,"org.adidaw.builtin.utility",1,prepared.deviceId);
    check(second.ok && second.referenceId==prepared.referenceId && second.chainId==prepared.chainId &&
          commitBuiltinInsert(store,second).ok,"reuse existing ref/chain and insert before selected device");
    check(db.execAndGet("SELECT id FROM devices ORDER BY ord,id LIMIT 1").getInt64()==second.deviceId &&
          db.execAndGet("SELECT count(*) FROM plugin_refs").getInt()==1,"browser ordering and reference deduplication are deterministic");
    db.exec("UPDATE devices SET ord=9223372036854775807");
    const auto tied=prepareBuiltinInsert(store,"org.adidaw.builtin.delay",1,prepared.deviceId);
    check(tied.ok && commitBuiltinInsert(store,tied).ok && History(store).undo().ok &&
        db.execAndGet("SELECT count(*) FROM devices WHERE ord=9223372036854775807").getInt()==2,
        "insertion normalizes tied maximum ordinals and undo restores exact original values");
    db.exec("ALTER TABLE plugin_refs ADD COLUMN future_data BLOB");
    db.exec("UPDATE plugin_refs SET future_data=x'00ff00'");
    OpRequest remove;remove.opType="pluginRef.unregister";remove.payload={{"id",prepared.referenceId}};
    check(!OpJournal(store).commit(remove).ok && db.execAndGet("SELECT count(*) FROM plugin_refs").getInt()==1,
        "used reference removal refuses atomically through foreign keys");
    const auto future=prepareBuiltinInsert(store,"org.adidaw.builtin.reverb",1);
    check(future.ok && future.requests.front().payload.at("row").at("future_data").is_null(),
        "new reference captures defaults of future typed columns");
    const auto stale=prepareBuiltinInsert(store,"org.adidaw.builtin.reverb",1);
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",1},{"name","Newer"}};
    check(OpJournal(store).commit(rename).ok,"intervening browser insertion fixture edit");
    const auto refused=commitBuiltinInsert(store,stale);
    check(!refused.ok && refused.stale && db.execAndGet("SELECT count(*) FROM devices").getInt()==2,"stale browser result refuses all new rows");
    check(!prepareBuiltinInsert(store,"unknown",1).ok && !prepareBuiltinInsert(store,"org.adidaw.builtin.utility",999).ok &&
          !prepareBuiltinInsert(store,"org.adidaw.builtin.utility",9,prepared.deviceId).ok,"unknown devices, a missing track and a selection on another track refuse (every catalogue entry is available with Pd; the PD-disabled build refuses them all)");
    db.exec("INSERT INTO tracks(id,kind,name) VALUES (8,'vca','VCA')");
    const auto vca = prepareBuiltinInsert(store, "org.adidaw.builtin.utility", 8);
    check(!vca.ok && vca.error.find("VCA") != std::string::npos && vca.requests.empty(),
          "built-in insertion refuses a VCA before preparing any edits");
#endif
}
void thirdPartyPlacement() {
    Project project;
    auto& store = *project.store;
    auto& db = store.db();
    const auto plugin = [](const char* uid, const char* name, const char* kind) {
        rows::PluginRef ref;
        ref.format = "clap";
        ref.uid = uid;
        ref.name = name;
        ref.subtype = kind;
        return ref;
    };
    const auto order = [&] {
        std::vector<std::int64_t> result;
        SQLite::Statement rows(db,"SELECT devices.id FROM devices JOIN device_chains ON device_chains.id=devices.chain_id "
                                 "WHERE device_chains.track_id=1 ORDER BY devices.ord,devices.id");
        while (rows.executeStep()) result.push_back(rows.getColumn(0).getInt64());
        return result;
    };
    const auto effect = plugin("test.audio","Audio effect","effect");
    const auto midi = plugin("test.midi","MIDI effect","midi_effect");
    const auto instrument = plugin("test.instrument","Existing synth","instrument");
    const auto replacement = plugin("test.replacement","Replacement synth","instrument");
    check(!preparePluginInsert(store, instrument, 1).ok && !preparePluginInsert(store, midi, 1).ok,
          "the insertion entry point refuses instruments and MIDI effects on audio tracks");
    db.exec("UPDATE tracks SET kind='midi' WHERE id=1");
    const auto add = [&](const rows::PluginRef& ref,std::optional<std::int64_t> before = {},
                         const PluginInsertOptions& options = {}) {
        const auto prepared = preparePluginInsert(store,ref,1,before,options);
        const auto committed = commitBuiltinInsert(store,prepared);
        if (!committed.ok) throw std::runtime_error(committed.error);
        return prepared.deviceId;
    };
    const auto e1 = add(effect);
    const auto m1 = add(midi);
    const auto i1 = add(instrument,{}, {false,RouteChoice::Plain});
    const auto i2 = add(instrument);
    const auto e2 = add(effect,m1);
    const auto m2 = add(midi,e1);
    const auto expected = std::vector<std::int64_t>{m1,m2,i1,i2,e2,e1};
    check(order() == expected, "third-party MIDI effects stay before the first instrument and audio effects after the last");
    const auto head = OpJournal(store).headSeq();
    const PluginInsertOptions replace{true,RouteChoice::MpeMidi};
    check(!preparePluginInsert(store,replacement,1,{},replace).ok &&
          !preparePluginInsert(store,effect,1,{},replace).ok && OpJournal(store).headSeq()==head,
          "replacement requires an instrument and an explicit target when several are present");
    db.exec("INSERT INTO plugin_params(device_id,param_id,normalized_value) VALUES(" +
            std::to_string(i1) + ",'shape',0.375)");
    const auto prepared = preparePluginInsert(store,replacement,1,i1,replace);
    check(prepared.ok && order()==expected && OpJournal(store).headSeq()==head,
          "preparing an anchored replacement leaves the project and selection order intact");
    if (!prepared.ok) return;
    db.exec("CREATE TRIGGER reject_replacement BEFORE INSERT ON devices WHEN NEW.name='Replacement synth' "
            "BEGIN SELECT RAISE(ABORT,'injected insert failure'); END");
    check(!commitBuiltinInsert(store,prepared).ok && order()==expected &&
          db.execAndGet("SELECT normalized_value FROM plugin_params WHERE param_id='shape'").getDouble()==0.375 &&
          OpJournal(store).headSeq()==head, "a failed insert rolls back removal, reference and instance state");
    db.exec("DROP TRIGGER reject_replacement");
    check(commitBuiltinInsert(store,prepared).ok &&
          order()==std::vector<std::int64_t>{m1,m2,prepared.deviceId,i2,e2,e1} &&
          db.execAndGet("SELECT route FROM device_expression_routes WHERE device_id=" +
                       std::to_string(prepared.deviceId)).getString()=="mpe_midi",
          "anchored replacement preserves the slot and commits the chosen expression route");
    History history(store);
    const auto step = history.nextUndo();
    check(step && step->label=="Insert Replacement synth" && history.undo().ok && order()==expected &&
          db.execAndGet("SELECT normalized_value FROM plugin_params WHERE param_id='shape'").getDouble()==0.375 &&
          db.execAndGet("SELECT route FROM device_expression_routes WHERE device_id="+std::to_string(i1)).getString()=="plain" &&
          db.execAndGet("SELECT COUNT(*) FROM plugin_refs WHERE uid='test.replacement'").getInt()==0,
          "one Undo restores the previous instrument, its parameter and route, and removes the new reference");
    check(history.redo().ok && order()==std::vector<std::int64_t>{m1,m2,prepared.deviceId,i2,e2,e1},
          "Redo restores the replacement with the same identity and placement");
    const auto rack = i2 + 100;
    db.exec("INSERT INTO devices(id,chain_id,ord,is_rack,rack_kind) VALUES(" + std::to_string(rack) +
            "," + std::to_string(prepared.chainId) + ",8,1,'instrument')");
    const auto afterRack = add(effect,rack);
    const auto withRack = order();
    check(withRack.size()==8 && withRack[6]==rack && withRack[7]==afterRack &&
          !preparePluginInsert(store,replacement,1,rack,replace).ok,
          "instrument racks bound audio placement while a replacement protects their nested devices");
}
void hotSwapPlacement() {
    Project project;
    auto& store = *project.store;
    auto& db = store.db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(20,'group','Group');"
            "UPDATE tracks SET parent_id=20 WHERE id=1;"
            "INSERT INTO device_chains(id,track_id,ord) VALUES(10,1,0),(11,1,1);"
            "INSERT INTO plugin_refs(id,format,uid,name,subtype,is_shell,shell_id) VALUES"
            "(1,'clap','test.effect','Existing effect','effect',0,NULL),"
            "(2,'vst3','test.replacement','Replacement effect','effect',1,71),"
            "(3,'internal','org.adidaw.mix.eq','EQ','effect',0,NULL);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name,enabled) VALUES"
            "(30,11,-6,1,'First',1),(40,11,11,1,'Target',0),(50,11,25,1,'Last',1),(70,10,1,3,'Mix EQ',1);"
            "INSERT INTO devices(id,chain_id,ord,is_rack,rack_kind) VALUES(60,10,0,1,'effect');"
            "INSERT INTO plugin_params(device_id,param_id,normalized_value) VALUES(40,'shape',0.375);"
            "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(7,'device',40,'shape');");

    using DeviceOrder = std::vector<std::pair<std::int64_t, std::int64_t>>;
    const auto order = [&] {
        DeviceOrder result;
        SQLite::Statement rows(db, "SELECT id,ord FROM devices WHERE chain_id=11 ORDER BY ord,id");
        while (rows.executeStep()) result.emplace_back(rows.getColumn(0).getInt64(), rows.getColumn(1).getInt64());
        return result;
    };
    const DeviceOrder original{{30, -6}, {40, 11}, {50, 25}};
    rows::PluginRef candidate;
    candidate.format = "vst3";
    candidate.uid = "test.replacement";
    candidate.name = "Replacement effect";
    // Existing references carry the stored role regardless of scan guesses.
    candidate.subtype = "instrument";
    candidate.isShell = true;
    candidate.shellId = 71;
    const auto replacement = preparePluginReplacement(store, candidate, 40, 1000);
    check(replacement.ok && replacement.referenceId == 2 && replacement.chainId == 11 && replacement.deviceId > 1000 &&
              order() == original,
          "SC-285 preparation anchors to the exact chain, installed shell reference, and a fresh runtime ID");
    if (!replacement.ok) return;
    auto otherShell = candidate;
    otherShell.shellId = 72;
    check(!preparePluginReplacement(store, otherShell, 40).ok && !preparePluginReplacement(store, candidate, 60).ok &&
              !preparePluginReplacement(store, candidate, 70).ok && !preparePluginReplacement(store, candidate, 400).ok,
          "SC-285 a different device role, a rack, a native Mix function, and a missing target refuse");
    db.exec("CREATE TRIGGER reject_hot_swap BEFORE INSERT ON devices WHEN NEW.name='Replacement effect' "
            "BEGIN SELECT RAISE(ABORT,'injected insert failure'); END");
    check(!commitBuiltinInsert(store, replacement).ok && order() == original &&
              db.execAndGet("SELECT COUNT(*) FROM automation_lanes WHERE owner_id=40").getInt() == 1,
          "SC-285 a failed candidate insert rolls the removed device and its automation back");
    db.exec("DROP TRIGGER reject_hot_swap");
    History history(store);
    const DeviceOrder swapped{{30, -6}, {replacement.deviceId, 11}, {50, 25}};
    check(commitBuiltinInsert(store, replacement).ok && order() == swapped &&
              db.execAndGet("SELECT enabled FROM devices WHERE ord=11").getInt() == 0 &&
              db.execAndGet("SELECT COUNT(*) FROM automation_lanes WHERE owner_id=40").getInt() == 0,
          "SC-285 a single replacement preserves exact order and bypass with no extra instance");
    const auto undo = history.nextUndo();
    check(undo && undo->label == "Hot-Swap Device" && history.undo().ok && order() == original &&
              db.execAndGet("SELECT normalized_value FROM plugin_params WHERE device_id=40").getDouble() == 0.375 &&
              db.execAndGet("SELECT owner_id FROM automation_lanes WHERE id=7").getInt64() == 40,
          "SC-285 one Undo restores the original device, parameter, and automation");
    check(history.redo().ok && order() == swapped && history.undo().ok, "SC-285 Redo reuses the same replacement slot");

    db.exec("UPDATE devices SET ord=11 WHERE id=50");
    const DeviceOrder tied{{30, -6}, {40, 11}, {50, 11}};
    const auto tiedReplacement = preparePluginReplacement(store, candidate, 40);
    check(tiedReplacement.ok && commitBuiltinInsert(store, tiedReplacement).ok &&
              order() == DeviceOrder{{30, 0}, {tiedReplacement.deviceId, 1}, {50, 2}} &&
              history.undo().ok && order() == tied,
          "SC-285 a tied order retains its audible order through the replacement and Undo");

    const auto stale = preparePluginReplacement(store, candidate, 40);
    OpRequest lock;
    lock.opType = "track.setLocked";
    lock.payload = {{"id", 20}, {"locked", true}};
    const auto locked = OpJournal(store).commit(std::span(&lock, 1));
    check(stale.ok && locked.ok && commitBuiltinInsert(store, stale).stale &&
              !preparePluginReplacement(store, candidate, 40).ok && order() == tied,
          "SC-285 a locked ancestor refuses the swap and a moved head invalidates a prepared swap");
    check(history.undo().ok, "SC-285 test unlocks the group");
    db.exec("UPDATE tracks SET frozen=1 WHERE id=1");
    check(!preparePluginReplacement(store, candidate, 40).ok && order() == tied,
          "SC-285 a frozen track refuses device replacement before mutation");
    db.exec("UPDATE tracks SET frozen=0 WHERE id=1");
#if ADI_WITH_PD
    const auto builtin = prepareBuiltinReplacement(store, "org.adidaw.builtin.utility", 40);
    check(builtin.ok && commitBuiltinInsert(store, builtin).ok &&
              order() == DeviceOrder{{30, 0}, {builtin.deviceId, 1}, {50, 2}} && history.undo().ok && order() == tied,
          "SC-285 the built-in wrapper replaces an effect at the same position");
#endif
}
void moveDeviceRange() {
    Project project;
    auto& store = *project.store;
    auto& db = store.db();
    db.exec("UPDATE tracks SET kind='midi' WHERE id=1;"
            "INSERT INTO tracks(id,kind,name) VALUES(2,'midi','Keys'),(3,'audio','Audio 2'),(4,'midi','Synth');"
            "INSERT INTO device_chains(id,track_id) VALUES(10,1),(20,2),(40,4);"
            "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES"
            "(1,'clap','test.midi','MIDI effect','midi_effect'),(2,'clap','test.synth','Synth','instrument'),"
            "(3,'clap','test.effect','Audio effect','effect');"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id) VALUES"
            "(10,10,0,1),(11,10,1,2),(12,10,2,3),(13,10,3,3),(14,10,4,3),"
            "(20,20,0,1),(21,20,1,3),(40,40,0,2);"
            "INSERT INTO plugin_params(device_id,param_id,normalized_value) VALUES(12,'shape',0.375);"
            "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(7,'device',11,'level');");
    const auto order = [&](std::int64_t track) {
        std::vector<std::int64_t> result;
        SQLite::Statement rows(db, "SELECT d.id FROM devices d JOIN device_chains c ON c.id=d.chain_id "
                                   "WHERE c.track_id=? ORDER BY c.ord,c.id,d.ord,d.id");
        rows.bind(1, track);
        while (rows.executeStep()) result.push_back(rows.getColumn(0).getInt64());
        return result;
    };
    const std::vector<std::int64_t> original{10, 11, 12, 13, 14};
    const std::array effects{DeviceMoveSource{13, 1, 10}, DeviceMoveSource{12, 1, 10}};
    const auto self = prepareDeviceMove(store, effects, 1, 13);
    const auto adjacent = prepareDeviceMove(store, effects, 1, 14);
    check(self.ok && self.requests.empty() && adjacent.ok && adjacent.requests.empty() &&
              commitDeviceMove(store, self).ok && !OpJournal(store).headSeq(),
          "SC-288 dropping a range on itself or in its existing position leaves history alone");
    const std::array separated{DeviceMoveSource{12, 1, 10}, DeviceMoveSource{14, 1, 10}};
    const std::array wrongOwner{DeviceMoveSource{12, 1, 10}, DeviceMoveSource{13, 2, 10}};
    check(!prepareDeviceMove(store, separated, 1, {}).ok && !prepareDeviceMove(store, wrongOwner, 1, {}).ok &&
              !prepareDeviceMove(store, effects, 1, 21).ok && order(1) == original,
          "SC-288 separated or stale source devices and a destination anchor on the wrong track refuse");

    History history(store);
    const auto tail = prepareDeviceMove(store, effects, 1, {});
    check(tail.ok && commitDeviceMove(store, tail).ok && order(1) == std::vector<std::int64_t>{10, 11, 14, 12, 13} &&
              history.nextUndo() && history.nextUndo()->label == "Move Devices" && history.undo().ok && order(1) == original,
          "SC-288 a same-chain range drag keeps visual order and is one Undo step");
    const std::array mixed{DeviceMoveSource{12, 1, 10}, DeviceMoveSource{10, 1, 10}, DeviceMoveSource{11, 1, 10}};
    check(!prepareDeviceMove(store, mixed, 3, {}).ok && !prepareDeviceMove(store, mixed, 4, {}).ok,
          "SC-288 a range with an instrument cannot move to audio or into a second instrument");
    const auto cross = prepareDeviceMove(store, mixed, 2, 20);
    StoreError error{};
    const auto otherConnection = Store::open(store.path(), error, true);
    check(otherConnection && commitDeviceMove(*otherConnection, cross).stale && order(1) == original,
          "SC-288 a prepared move belongs to the exact connection, even for the same file");
    check(cross.ok && commitDeviceMove(store, cross).ok && order(1) == std::vector<std::int64_t>{13, 14} &&
              order(2) == std::vector<std::int64_t>{20, 10, 11, 12, 21} &&
              db.execAndGet("SELECT normalized_value FROM plugin_params WHERE device_id=12").getDouble() == 0.375 &&
              db.execAndGet("SELECT owner_id FROM automation_lanes WHERE id=7").getInt64() == 11 &&
              history.undo().ok && order(1) == original && order(2) == std::vector<std::int64_t>{20, 21},
          "SC-288 a mixed-role range moves to its one legal slot with the same state and returns in one Undo");

    const auto emptyTrack = prepareDeviceMove(store, effects, 3, {});
    check(emptyTrack.ok && commitDeviceMove(store, emptyTrack).ok && order(3) == std::vector<std::int64_t>{12, 13} &&
              history.undo().ok && order(1) == original &&
              db.execAndGet("SELECT COUNT(*) FROM device_chains WHERE track_id=3").getInt() == 0,
          "SC-288 moving a range to an empty track makes the chain in the same Undo step");
    OpRequest lock;
    lock.opType = "track.setLocked";
    lock.payload = {{"id", 3}, {"locked", true}};
    check(OpJournal(store).commit(lock).ok && commitDeviceMove(store, emptyTrack).stale &&
              !prepareDeviceMove(store, effects, 3, {}).ok && order(1) == original && order(3).empty(),
          "SC-288 a changed head or a locked destination refuses the entire move");
}
// DF-E50: automation on a built-in device is applied, at its frame. Utility's
// Gain is "11", -120..35 dB on a linear curve, so 0 dB is 120/155 and +6 dB
// 126/155; its core ramps a gain change over 5 ms.
void automationEvents() {
#if ADI_WITH_PD
    auto instance=load("org.adidaw.builtin.utility");
    check(instance->setParam("11",ParamValue::fromNormalized(120.0/155)),"stored gain 0 dB");
    instance->prepare(48000,64);
    std::array<std::array<float,64>,2> input{},output{};
    for(auto& channel:input)channel.fill(0.25f);
    float* in[]={input[0].data(),input[1].data()};float* out[]={output[0].data(),output[1].data()};
    NodeIo io;io.in=in;io.out=out;io.channels=2;io.frames=64;
    const auto run=[&](int blocks){for(int block=0;block<blocks;++block)instance->process(io);};
    run(1);
    const float before=output[0][63];
    check(std::abs(before-0.25f)<1e-6f,"the stored 0 dB plays");
    Event events[2]{};
    events[0].frame=32;events[0].type=EventType::ParamValue;events[0].paramId=11;events[0].value=126.0/155;
    events[1].frame=40;events[1].type=EventType::ParamValue;events[1].paramId=999;events[1].value=0;
    io.events=EventSpan{events,2};
    allocations.store(0,std::memory_order_relaxed);counting=true;
    {audio::CallbackScope callback;instance->process(io);}
    counting=false;
    check(output[0][31]==before && output[0][32]>before+1e-6f,"a ParamValue applies at its frame: untouched before 32, rising from 32");
    check(allocations.load(std::memory_order_relaxed)==0,"applying automation allocates nothing");
    check(std::abs(instance->getParam("11").real)<1e-4,"automation leaves the stored value as it was");
    io.events={};run(60);
    const float up=0.25f*std::pow(10.0f,6.0f/20);
    check(std::abs(output[0][63]-up)<2e-5f,"the automated +6 dB holds into later blocks (an unknown id did nothing)");
    // ParamMod: an offset on the value in force, held until the next one.
    events[0].frame=0;events[0].type=EventType::ParamMod;events[0].value=-6.0/155;
    io.events=EventSpan{events,1};run(1);io.events={};run(60);
    check(std::abs(output[0][63]-0.25f)<2e-5f,"a ParamMod of -6 dB on the automated +6 dB plays 0 dB, and holds");
    events[0].value=0;io.events=EventSpan{events,1};run(1);io.events={};run(60);
    check(std::abs(output[0][63]-up)<2e-5f,"a ParamMod of 0 takes the offset away");
    // A setParam (the user's knob) is heard again: it overrides the lane until its next value.
    check(instance->setParam("11",ParamValue::fromNormalized(123.0/155)),"the knob moves to +3 dB");
    run(60);
    check(std::abs(output[0][63]-0.25f*std::pow(10.0f,3.0f/20))<2e-5f,"a knob change after automation is heard");
    // DF-E61: the stored value goes A to B and back to A with no audio
    // between (two edits in one period: two clicks, an Undo and a Redo). The
    // value in force is the lane's +6 dB; the knob's A must be heard again.
    const float knob=0.25f*std::pow(10.0f,3.0f/20);
    const auto stateAtKnob=instance->saveState("state");
    const auto automateUp=[&] {
        events[0].frame=0;events[0].type=EventType::ParamValue;events[0].value=126.0/155;
        io.events=EventSpan{events,1};run(1);io.events={};run(60);
        return std::abs(output[0][63]-up)<2e-5f;
    };
    check(automateUp(),"the lane moves the gain to +6 dB again");
    check(instance->setParam("11",ParamValue::fromNormalized(120.0/155)) &&
              instance->setParam("11",ParamValue::fromNormalized(123.0/155)),"the knob goes to 0 dB and back to +3 dB in one period");
    run(60);
    check(std::abs(output[0][63]-knob)<2e-5f,"the knob's +3 dB is heard, not the lane's +6 dB (DF-E61)");
    check(automateUp(),"the lane moves the gain to +6 dB once more");
    check(instance->setParam("11",ParamValue::fromNormalized(120.0/155)) && instance->loadState("state",stateAtKnob),
          "the knob goes to 0 dB, then a state holding +3 dB loads, in one period");
    run(60);
    check(std::abs(output[0][63]-knob)<2e-5f,"the state's +3 dB is heard (DF-E61, through loadState)");
    // Both edits by states alone: 0 dB, then +3 dB, in one period.
    check(instance->setParam("11",ParamValue::fromNormalized(120.0/155)),"the knob goes to 0 dB, to save a state there");
    const auto stateAtZero=instance->saveState("state");
    check(instance->setParam("11",ParamValue::fromNormalized(123.0/155)),"and back to +3 dB");
    check(automateUp(),"the lane moves the gain to +6 dB again, over the stored +3 dB");
    check(instance->loadState("state",stateAtZero) && instance->loadState("state",stateAtKnob),
          "a state at 0 dB, then one at +3 dB, load in one period");
    run(60);
    check(std::abs(output[0][63]-knob)<2e-5f,"the second state's +3 dB is heard (DF-E61, states alone)");
    // A write that leaves the stored value as it was is no edit: the lane's
    // value stays in force (a preset reload, a drag that ends where it began).
    check(automateUp(),"the lane moves the gain to +6 dB a third time");
    check(instance->setParam("11",ParamValue::fromNormalized(123.0/155)),"the knob is set to the +3 dB it already holds");
    run(60);
    check(std::abs(output[0][63]-up)<2e-5f,"an unchanged stored value leaves the lane's +6 dB in force");
    // A new format restarts the core from the stored values: the knob's +3 dB
    // is in force again until the lane's next value, as before DF-E61.
    instance->prepare(44100,64);run(60);
    check(std::abs(output[0][63]-knob)<2e-5f,"after a new sample rate the stored +3 dB is in force again");
#endif
}
#if ADI_WITH_PD
std::vector<std::byte> constantLane(double normalized) {
    AutomationPoint point{};point.time=0;point.value=normalized;point.curve=1;point.point_id=1;
    return writeStream<AutomationPoint>(FourCC::Automation,std::vector<AutomationPoint>{point},1,
                                        StreamFlags::SortedByTime|StreamFlags::TimeIsNanos);
}
#endif
// DF-E50 through the product: a lane on Utility's Gain at 0 dB, over a stored
// +6 dB, in an ordinary Session (live) and in an offline export (a render).
void automatedSession() {
#if ADI_WITH_PD
    for(const bool render:{false,true}) {
        Project project;auto& store=*project.store;auto& db=store.db();
        const auto prepared=prepareBuiltinInsert(store,"org.adidaw.builtin.utility",1);check(commitBuiltinInsert(store,prepared).ok,"automation fixture inserts Utility");
        std::vector<float> samples(8192,0.25f);audio::WavWriter writer(project.temporary.path()/"audio.wav",48000,2,audio::WavFormat::Float32);writer.write(samples.data(),4096);writer.close();
        db.exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','audio.wav','wav')");
        db.exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,1000000000)");
        db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,4096)");
        OpRequest gain;gain.opType="device.setParam";gain.payload={{"dev",prepared.deviceId},{"param","11"},{"norm",126.0/155}};
        check(OpJournal(store).commit(gain).ok,"stored gain +6 dB");
        {
            SQLite::Statement lane(db,"INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain) VALUES(1,'device',?,'11',1,'normalized')");
            lane.bind(1,prepared.deviceId);lane.exec();
        }
        check(store.putAutomationData(1,std::nullopt,constantLane(120.0/155)),"a lane on Gain at 0 dB");
        SessionSpec spec{2,48000,render?512:64};spec.offline=render;spec.exporting=render;
        Session session;session.graph().setFadeFrames(0);
        check(session.load(store,withBuiltinDevices(),spec),render?"the render loads":"the live session loads");
        if(!session.error().empty())std::printf("session: %s\n",session.error().c_str());
        session.transport().play();
        std::vector<float> left(static_cast<std::size_t>(spec.maxFrames)),right(left.size());float* out[]={left.data(),right.data()};
        bool primed=true;
        for(int block=0;block<(render?6:40);++block){primed=primed && session.clips()->prime();AudioIo io;io.out=out;io.numOut=2;io.frames=spec.maxFrames;session.process(io);}
        std::printf("METRIC Utility automated to 0 dB over a stored +6 dB (%s): %.6f\n",render?"render":"live",left.back());
        check(primed && std::abs(left.back()-0.25f)<2e-5f,render?"the render plays the lane's 0 dB, not the stored +6 dB":"live, the lane's 0 dB plays, not the stored +6 dB");
    }
#endif
}
void playingSession() {
#if ADI_WITH_PD
    Project project;auto& store=*project.store;auto& db=store.db();
    const auto prepared=prepareBuiltinInsert(store,"org.adidaw.builtin.utility",1);check(commitBuiltinInsert(store,prepared).ok,"session fixture inserts builtin");
    std::vector<float> samples(8192,0.25f);audio::WavWriter writer(project.temporary.path()/"audio.wav",48000,2,audio::WavFormat::Float32);writer.write(samples.data(),4096);writer.close();
    db.exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','audio.wav','wav')");
    db.exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,1000000000)");
    db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,4096)");
    OpRequest gain;gain.opType="device.setParam";gain.payload={{"dev",prepared.deviceId},{"param","11"},{"norm",126.0/155}};
    check(OpJournal(store).commit(gain).ok,"persist builtin control as ordinary parameter op");
    Session session;session.graph().setFadeFrames(0);
    check(session.load(store,withBuiltinDevices(),{2,48000,64}) && session.stats().loaded==1,"project internal reference becomes loaded device in ordinary Session");
    if(!session.error().empty())std::printf("session: %s\n",session.error().c_str());
    session.transport().play();std::array<std::array<float,64>,2> output{};float* out[]={output[0].data(),output[1].data()};
    bool primed=true;
    for(int block=0;block<20;++block) {primed=primed && session.clips()->prime();AudioIo io;io.out=out;io.numOut=2;io.frames=64;session.process(io);}
    check(primed && std::abs(output[0][63]-0.25f*std::pow(10.0f,6.0f/20))<2e-5f,"stored built-in gain processes real clip audio through the complete graph");
#endif
    automationEvents();automatedSession();  // DF-E50
}
// DF-M36: session rebuilds prepare every node. A prepare that changes nothing
// never takes the gate (a block in flight would pass unprocessed) and keeps
// the device's state: here a delay's line, against an uninterrupted twin.
void duplicatePrepareKeepsBuiltinState() {
#if ADI_WITH_PD
    constexpr double rate=48000;constexpr std::int32_t frames=64;
    auto live=load("org.adidaw.builtin.delay"),reference=load("org.adidaw.builtin.delay"),
         restarted=load("org.adidaw.builtin.delay");
    auto* gate=adi::device::detail::builtinGateForTesting(*live);
    if(!gate){check(false,"the built-in exposes its gate to the prepare probe");return;}
    for(auto* device:{live.get(),reference.get(),restarted.get()})device->prepare(rate,frames);
    std::uint64_t block=0;
    const auto run=[&](DeviceInstance& device,std::array<std::array<float,64>,2>& audio) {
        float* planes[]={audio[0].data(),audio[1].data()};
        const float* inputs[]={planes[0],planes[1]};
        NodeIo io;io.in=inputs;io.out=planes;io.channels=2;io.frames=frames;io.renderBlock=block;
        device.process(io);
    };
    const auto programme=[&] {
        std::array<std::array<float,64>,2> audio{};
        for(std::size_t c=0;c<audio.size();++c)for(std::size_t i=0;i<audio[c].size();++i) {
            const double t=static_cast<double>(block)*64.+static_cast<double>(i);
            audio[c][i]=static_cast<float>(.5*std::sin(t*(c?.013:.031)));
        }
        return audio;
    };
    // One block through all three: whether live, then restarted, match the reference.
    const auto step=[&] {
        ++block;auto a=programme(),b=a,r=a;
        run(*live,a);run(*restarted,b);run(*reference,r);
        return std::pair{a==r,b==r};
    };
    bool same=true;
    for(int i=0;i<400;++i)same=step().first&&same;
    check(same,"three delays start with the same audio");

    std::atomic<bool> finished{false};
    const auto done=[&]{return finished.load(std::memory_order_acquire);};
    const auto waitFor=[](auto predicate) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(!predicate()) {
            if(std::chrono::steady_clock::now()>=deadline)return false;
            std::this_thread::yield();
        }
        return true;
    };
    ++block;auto during=programme(),expected=during,restartedBlock=during;
    run(*reference,expected);run(*restarted,restartedBlock);
    std::exception_ptr prepareFailure;std::jthread owner;bool admitted=false;
    {
        // With this Pass held, a Hold taken by the prepare stays closed until
        // the Pass leaves: the block below then passes unprocessed.
        const ReactivationGate::Pass inFlight(*gate);
        owner=std::jthread([&] {
            try{live->prepare(rate,frames);}
            catch(...){prepareFailure=std::current_exception();}
            finished.store(true,std::memory_order_release);
        });
        if(waitFor([&]{return done()||gate->closed();})) {
            admitted=static_cast<bool>(inFlight)&&!gate->closed();
            run(*live,during);
        }
    }
    if(!waitFor(done)) {
        std::fputs("FAIL same-format built-in prepare did not unwind after the test released its gate pass\n",stderr);
        std::abort();
    }
    owner.join();
    check(!prepareFailure && admitted && during==expected,
          "a same-format built-in prepare with a block in flight never takes the gate; the block is processed");
    // The control: a real re-preparation loses the line. The native core keeps
    // its line over a same-rate prepare by itself, so the control changes rate.
    restarted->prepare(44100,frames);restarted->prepare(rate,frames);
    bool kept=true,lost=false;
    for(int i=0;i<400;++i){const auto [liveSame,restartedSame]=step();kept=kept&&liveSame;lost=lost||!restartedSame;}
    check(kept,"a repeated identical prepare keeps the delay line: output equals an uninterrupted delay");
    check(lost,"control: a delay re-prepared at another rate and back sounds different, so the comparison can see a reset");
#endif
}
}
// DEV-21: OneShot is a loadable instrument, so a MIDI clip sounds. Through
// the product's own calls: the browser insert (registered as an instrument),
// the sample slot assignment (#414), an ordinary Session with a MIDI clip.
// DEV-22: Hybrid Reverb is loadable. Through the product's calls: the browser
// insert, an impulse response assigned to its slot (#414), an ordinary Session
// at 48 kHz, the Convolution route alone, fully wet, so a constant 0.25 comes
// out at 0.25 times the IR's DC gain once the 256-sample partition has passed.
// Two IRs, each one impulse of 0.5: at the session's rate on its first sample
// (nothing resampled: exact), and at 44.1 kHz 64 samples in (resampled with its
// DC gain kept). An impulse on the very first sample of an IR at another rate
// loses the half of its resampled pulse that falls before time zero.
void hybridSession() {
#if ADI_WITH_PD
    const auto* info=findBuiltin("org.adidaw.builtin.hybrid-reverb");
    check(info && info->available,"Hybrid Reverb is available in the catalogue");
    if(!info || !info->available)return;
    struct Case{std::uint32_t rate;std::size_t at;float tolerance;const char* what;};
    for(const Case& c:{Case{48000,0,1e-6f,"an IR at the session's rate convolves exactly"},
                       Case{44100,64,1e-3f,"an IR at 44.1 kHz is resampled to 48 kHz with its DC gain kept"}}) {
        Project project;auto& store=*project.store;auto& db=store.db();
        const auto inserted=prepareBuiltinInsert(store,"org.adidaw.builtin.hybrid-reverb",1);
        check(inserted.ok && commitBuiltinInsert(store,inserted).ok,"the browser inserts Hybrid Reverb on an audio track");
        const auto ir=project.temporary.path()/"impulse.wav";
        {
            std::vector<float> impulse(512,0.f);impulse[c.at]=.5f;
            audio::WavWriter writer(ir,c.rate,1,audio::WavFormat::Float32);writer.write(impulse.data(),512);writer.close();
        }
        const auto assigned=prepareSampleAssignment(store,inserted.deviceId,1,ir,48000,7);
        if(!assigned.ok)std::printf("assign: %s\n",assigned.error.c_str());
        check(assigned.ok && commitSampleAssignment(store,assigned).ok,"an impulse response is assigned to Hybrid Reverb's slot as one undo step");
        for(const auto& [param,norm]:{std::pair<const char*,double>{"1",1.0},{"14",1.0}}) {  // Route: Convolution; Dry/Wet 100 %
            OpRequest set;set.opType="device.setParam";set.payload={{"dev",inserted.deviceId},{"param",param},{"norm",norm}};
            check(OpJournal(store).commit(set).ok,"Hybrid Reverb parameter set");
        }
        std::vector<float> samples(96000,0.25f);
        audio::WavWriter writer(project.temporary.path()/"audio.wav",48000,2,audio::WavFormat::Float32);writer.write(samples.data(),48000);writer.close();
        db.exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(2,'test','audio.wav','wav')");
        db.exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,1000000000)");
        db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,2,0,48000)");
        Session session;session.graph().setFadeFrames(0);
        const bool loaded=session.load(store,withBuiltinDevices(),{2,48000,256});
        if(!session.error().empty())std::printf("session: %s\n",session.error().c_str());
        check(loaded && session.stats().loaded==1,"the project with Hybrid Reverb loads in an ordinary Session");
        session.transport().play();
        std::vector<float> left(256),right(256);float* out[]={left.data(),right.data()};
        for(int block=0;block<40;++block){(void)session.clips()->prime();AudioIo io;io.out=out;io.numOut=2;io.frames=256;session.process(io);}
        std::printf("METRIC Hybrid Reverb, a 0.5 impulse at %u Hz (sample %zu) convolving 0.25 at 48 kHz: %.6f\n",c.rate,c.at,left.back());
        check(std::abs(left.back()-0.125f)<c.tolerance && std::abs(right.back()-0.125f)<c.tolerance,c.what);
    }
#endif
}
// DEV-21 (win, 20 November: option 1): the Sampler is a loadable instrument.
// Its catalogue entry declares 32 sample slots in a declaration text that is
// not a canvas; the slot ops fill them; a filled slot is a zone over every key
// and velocity, so a sample dropped into any slot plays at once, and two
// dropped samples layer, as in Live's Sampler.
#if ADI_WITH_PD
// SC-483: `zones` is a zone table committed as the device's "zones" state by
// an ordinary op; `undone` takes that op back before the project is played.
double samplerPeak(const std::vector<std::int32_t>& slots,bool reload,double volumeDb=-12,
                   const std::vector<std::uint8_t>* zones=nullptr,bool undone=false) {
    Project project;auto& store=*project.store;auto& db=store.db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(2,'midi','Keys')");
    const auto inserted=prepareBuiltinInsert(store,std::string(device::samplerUid),2);
    check(inserted.ok && commitBuiltinInsert(store,inserted).ok,"the browser inserts the Sampler on a MIDI track");
    check(db.execAndGet("SELECT subtype FROM plugin_refs WHERE uid='org.adidaw.builtin.sampler'").getString()=="instrument",
          "the Sampler is registered with subtype instrument");
    const auto file=project.temporary.path()/"tone.wav";
    {
        std::vector<float> tone(96000);
        for(std::size_t i=0;i<48000;++i) {
            tone[2*i]=tone[2*i+1]=static_cast<float>(.5*std::sin(2*3.141592653589793*1000*static_cast<double>(i)/48000));
        }
        audio::WavWriter writer(file,48000,2,audio::WavFormat::Float32);writer.write(tone.data(),48000);writer.close();
    }
    for(const auto slot:slots) {
        const auto assigned=prepareSampleAssignment(store,inserted.deviceId,slot,file,48000,7);
        if(!assigned.ok) {
            std::printf("assign: %s\n",assigned.error.c_str());
        }
        check(assigned.ok && commitSampleAssignment(store,assigned).ok,"a sample is assigned to a Sampler slot as one undo step");
    }
    if(volumeDb!=-12) {
        // The hosted parameter path: an ordinary parameter op on Volume ("1",
        // -36..36 dB) reaches the core through SamplerNative's sendParameter.
        OpRequest volume;volume.opType="device.setParam";
        volume.payload={{"dev",inserted.deviceId},{"param","1"},{"norm",(volumeDb+36)/72}};
        check(OpJournal(store).commit(volume).ok,"the Sampler's Volume is set by an ordinary parameter op");
    }
    if(zones) {
        const auto hash=media::blake3Bytes(std::as_bytes(std::span(*zones)));
        SQLite::Statement blob(db,"INSERT OR IGNORE INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
        blob.bind(1,hash.hex);blob.bind(2,zones->data(),static_cast<int>(zones->size()));
        blob.bind(3,static_cast<std::int64_t>(zones->size()));blob.exec();
        OpRequest shaped;shaped.opType="device.loadState";shaped.label="Edit Sampler zones";shaped.targetKind="device";shaped.targetId=inserted.deviceId;
        shaped.payload={{"dev",inserted.deviceId},{"role",std::string(device::samplerZonesRole)},{"hash",hash.hex},{"hint","adi.sampler.zones.cbor.v1"}};
        check(hash && OpJournal(store).commit(shaped).ok,"the zone table is stored by an ordinary state op");
        if(undone) {
            check(History(store).undo().ok,"one Undo takes the zone edit back");
        }
    }
    db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(5,2,'midi',0,23063040)");
    {
        StreamHeader header{static_cast<std::uint32_t>(FourCC::Notes),1,40,1,0};
        NoteRecord note{};note.start_ticks=0;note.dur_ticks=5765760;note.note_id=1;note.key=60;note.vel_on=127;
        note.vel_off=64;note.probability=10000;
        std::vector<std::uint8_t> bytes(sizeof header+sizeof note);
        std::memcpy(bytes.data(),&header,sizeof header);std::memcpy(bytes.data()+sizeof header,&note,sizeof note);
        SQLite::Statement statement(db,"INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(50,5,'notes',?)");
        statement.bind(1,bytes.data(),static_cast<int>(bytes.size()));statement.exec();
    }
    // The project snapshot names each filled slot from the declarations.
    {
        const auto snapshot=SnapshotBuilder::fromStore(store);
        auto order=slots;std::sort(order.begin(),order.end());
        bool named=snapshot->sampleSlots.size()==order.size();
        for(std::size_t i=0;named && i<slots.size();++i) {
            const auto& node=*snapshot->sampleSlots[i];
            named=node.problem.empty() && node.slot==order[i] && node.name=="Sample "+std::to_string(order[i]) && !node.empty;
        }
        check(named,"the snapshot shows each filled Sampler slot by its number and name, in slot order");
    }
    double peak=0;
    for(int pass=0;pass<(reload?2:1);++pass) {
        // The second pass is a fresh Session on the same store: every slot is
        // restored from the project, as on reopening it.
        Session session;session.graph().setFadeFrames(0);
        const bool loaded=session.load(store,withBuiltinDevices(),{2,48000,64});
        if(!session.error().empty()) {
            std::printf("session: %s\n",session.error().c_str());
        }
        check(loaded,"the project with the Sampler loads in an ordinary Session");
        session.transport().play();
        std::array<std::array<float,64>,2> output{};float* out[]={output[0].data(),output[1].data()};
        peak=0;int firstSound=-1;
        for(int block=0;block<200;++block) {
            (void)session.clips()->prime();AudioIo io;io.out=out;io.numOut=2;io.frames=64;session.process(io);
            for(int i=0;i<64;++i) {
                const double v=std::abs(static_cast<double>(output[0][static_cast<std::size_t>(i)]));
                if(v>1e-4 && firstSound<0) {
                    firstSound=block*64+i;
                }
                peak=std::max(peak,v);
            }
        }
        check(slots.empty() || zones!=nullptr || (firstSound>=0 && firstSound<256),"the MIDI clip's note sounds at once");
    }
    return peak;
}
// SC-483: a Sampler slot's saved state, written by hand as the host writes it
// (a float backup of the source at its own rate).
std::vector<std::uint8_t> samplerSlotState(std::int32_t slot,const std::vector<float>& interleaved,int channels,double rate) {
    auto value=emptyBuiltinSample(samplerUid,slot);
    value["empty"]=false;value["mediaHash"]=std::string(64,static_cast<char>('a'+slot%6));
    value["sourceRate"]=rate;value["channels"]=channels;
    std::vector<std::uint8_t> bytes(interleaved.size()*4);
    for(std::size_t i=0;i<interleaved.size();++i) {
        std::uint32_t bits=0;std::memcpy(&bits,&interleaved[i],sizeof bits);
        for(unsigned byte=0;byte<4;++byte)bytes[i*4+byte]=static_cast<std::uint8_t>((bits>>(8*byte))&255u);
    }
    value["source_f32le"]=Payload::binary(std::move(bytes));
    return Payload::to_cbor(value);
}
// SC-483: the hosted Sampler over typed events, 64 frames a block: both channels.
std::array<std::vector<float>,2> samplerRender(DeviceInstance& sampler,std::span<const Event> input,int count) {
    std::array<std::vector<float>,2> heard{std::vector<float>(static_cast<std::size_t>(count)),std::vector<float>(static_cast<std::size_t>(count))};
    std::array<std::array<float,64>,2> output{};float* out[]={output[0].data(),output[1].data()};
    for(int at=0;at<count;at+=64) {
        std::array<Event,16> events{};std::int32_t n=0;
        for(auto event:input)if(event.frame>=at && event.frame<at+64 && n<16) {
            event.frame-=at;events[static_cast<std::size_t>(n++)]=event;
        }
        NodeIo io;io.out=out;io.channels=2;io.frames=std::min(64,count-at);io.events={events.data(),n};
        allocations.store(0,std::memory_order_relaxed);counting=true;
        {audio::CallbackScope callback;sampler.process(io);}
        counting=false;
        check(allocations.load(std::memory_order_relaxed)==0,"the hosted Sampler allocates nothing while it plays");
        for(std::size_t c=0;c<2;++c)std::copy_n(output[c].begin(),io.frames,heard[c].begin()+at);
    }
    return heard;
}
// SC-483: a project saved before the Sampler grew (the build of 28 November
// 2026: parameters 1 to 24, 32 sample slots and no other state) opens and
// sounds as it did. The state is written as that build wrote it, every
// parameter away from its default; the probes are that build's own output.
void samplerSavedBefore() {
    const std::array<double,24> real{-6,3,5,80,-9,40,4,1,1,0,1,2500,60,1,2,6,0,1,.5,30,40,1.5,60,20};
    auto author=load("org.adidaw.builtin.sampler");
    bool set=true;
    for(std::size_t i=0;i<real.size();++i)set=author->setParam(std::to_string(i+1),ParamValue::withReal(0,real[i])) && set;
    auto saved=Payload::from_cbor(author->saveState("state"));
    for(const char* part:{"params","real","units"}) {
        auto& object=saved.at(part);
        for(auto it=object.begin();it!=object.end();) {
            if(std::stoi(it.key())>24)it=object.erase(it);else ++it;
        }
    }
    check(set && saved.at("params").size()==24,"the state of 28 November: 24 parameters and nothing else");
    std::vector<float> mono(9600),stereo(2*8820);
    for(std::size_t i=0;i<9600;++i) {
        const double t=static_cast<double>(i)/48000;
        mono[i]=static_cast<float>(.35*std::sin(2*3.141592653589793*220*t)+.2*std::sin(2*3.141592653589793*1760*t+.3)+
                                   .1*static_cast<double>(i%97)/97-.05);
    }
    for(std::size_t i=0;i<8820;++i) {
        const double t=static_cast<double>(i)/44100;
        stereo[2*i]=static_cast<float>(.3*std::sin(2*3.141592653589793*330*t));
        stereo[2*i+1]=static_cast<float>(.25*std::sin(2*3.141592653589793*495*t+1));
    }
    auto sampler=load("org.adidaw.builtin.sampler");
    sampler->prepare(48000,64);
    check(sampler->loadState("state",Payload::to_cbor(saved)) && sampler->loadState("sample:3",samplerSlotState(3,mono,1,48000)) &&
          sampler->loadState("sample:7",samplerSlotState(7,stereo,2,44100)),"that project's state and its two samples load");
    const std::array<Event,5> played{{{0,EventType::NoteOn,0,60,1,0,.9},{300,EventType::NoteOn,0,67,2,0,.5},
        {1500,EventType::Controller,0,0,0,1,.5},{2000,EventType::NoteOff,0,60,1,0,0},{3500,EventType::NoteOff,0,67,2,0,0}}};
    const auto heard=samplerRender(*sampler,played,4800);
    constexpr std::array<std::size_t,12> frames{90,250,420,800,1400,1900,2050,2600,3300,3450,3700,4500};
    std::array<double,26> probes{};
    for(std::size_t i=0;i<frames.size();++i)for(std::size_t c=0;c<2;++c)probes[2*i+c]=static_cast<double>(heard[c][frames[i]]);
    for(std::size_t c=0;c<2;++c)for(const float v:heard[c])probes[24+c]+=std::abs(static_cast<double>(v));
    std::printf("METRIC Sampler, the state of 28 November:");
    for(const double probe:probes)std::printf(" %.9g",probe);
    std::printf("\n");
    bool finite=true;
    for(const double probe:probes)finite=finite && std::isfinite(probe);
    check(finite && probes[24]<0,"PROBE ONLY: print the probes of the build of 28 November");
}
// SC-483: the hosted Sampler takes its zone table as the state stream "zones",
// plays by it, saves it, and a project keeps it through reopening and Undo.
std::vector<std::uint8_t> samplerZoneTable(std::int32_t slot,Payload zone) {
    zone["slot"]=slot;
    const Payload table={{"version",1},{"zones",Payload::array({std::move(zone)})}};
    return Payload::to_cbor(table);
}
void samplerZones() {
    // A ramp: a frame of the output names the position the sample was read at.
    std::vector<float> ramp(8192);
    for(std::size_t i=0;i<ramp.size();++i)ramp[i]=static_cast<float>(i)/8192.f;
    const auto plain=[&] {
        auto sampler=load("org.adidaw.builtin.sampler");
        sampler->prepare(48000,64);
        // Volume 0 dB, the filter off, velocity at full depth: the sample's own values.
        check(sampler->setParam("1",ParamValue::withReal(0,0)) && sampler->setParam("9",ParamValue::withReal(0,0)) &&
              sampler->setParam("23",ParamValue::withReal(0,100)) && sampler->loadState("sample:3",samplerSlotState(3,ramp,1,48000)),
              "a hosted Sampler with a ramp in slot 3");
        return sampler;
    };
    const std::array<Event,1> c3{{{0,EventType::NoteOn,0,60,1,0,1}}};
    const auto first=plain();
    const auto fresh=first->saveState(std::string(device::samplerZonesRole));
    check(!fresh.empty() && fresh.size()<64 && Payload::from_cbor(fresh).at("zones").empty(),
          "a Sampler nobody shaped saves a small zone table that lists no slot");
    check(std::abs(static_cast<double>(samplerRender(*first,c3,256)[0][100])-100./8192)<1e-7,"with a fresh zone C3 plays the sample at its own pitch");
    const auto rooted=samplerZoneTable(3,{{"root",48},{"gain",-20.0},{"pan",-1.0}});
    const auto shaped=plain();
    check(shaped->loadState("zones",rooted),"the hosted Sampler takes a zone table");
    shaped->pumpMainThread();
    const auto heard=samplerRender(*shaped,c3,256);
    check(std::abs(static_cast<double>(heard[0][100])-.1*std::sqrt(2.)*200/8192)<1e-6 && heard[1][100]==0,
          "root key C2, -20 dB and hard left: C3 reads an octave up, a tenth as loud, on the left only");
    const auto saved=shaped->saveState("zones");
    const auto listed=Payload::from_cbor(saved);
    check(listed.at("zones").size()==1 && listed.at("zones")[0].at("slot")==3 && listed.at("zones")[0].at("root")==48,
          "it saves the table with the shaped slot");
    const auto copy=plain();
    check(copy->loadState("zones",saved) && copy->saveState("zones")==saved && samplerRender(*copy,c3,256)==heard,
          "a second Sampler given that stream plays the same, sample for sample");
    check(!copy->loadState("zones",{1,2,3}) && !copy->loadState("zones",samplerZoneTable(40,Payload::object())) &&
          !copy->loadState("zones",samplerZoneTable(3,{{"key",std::array{80.,20.,0.,0.}}})) && copy->saveState("zones")==saved,
          "a table that is not one, names slot 40 or holds an invalid range is refused, and the old table stays");
    // In a project: the note is C3 (key 60). A zone that ends at B2 leaves it
    // silent, through reopening; Undo brings the sound back.
    const double one=.5*std::pow(10.,-12./20);
    const auto below=samplerZoneTable(5,{{"key",std::array{0.,59.,0.,0.}}});
    const auto around=samplerZoneTable(5,{{"key",std::array{60.,60.,0.,0.}}});
    check(samplerPeak({5},true,-12,&below)==0,"a zone that ends below the note keeps the slot silent, and again after reopening");
    check(std::abs(samplerPeak({5},true,-12,&around)-one)<2e-3,"a zone around the note plays it, and again after reopening");
    check(std::abs(samplerPeak({5},true,-12,&below,true)-one)<2e-3,"after Undo of the zone edit the project plays the note again");
}
#endif
void samplerSession() {
#if ADI_WITH_PD
    const auto* info=findBuiltin(device::samplerUid);
    check(info && info->instrument && info->midi && info->inputs==0 && info->available && builtinCanvas(device::samplerUid).empty(),
          "the Sampler is in the catalogue as an instrument, with declarations that are not a canvas");
    check(!device::makeNativePdDevice(builtinDeclarations(device::samplerUid),0,2) &&
          !device::makeNativePdDevice(builtinDeclarations(device::samplerUid),2,2),"no Pd canvas path can become the Sampler");
    auto instance=load("org.adidaw.builtin.sampler");
    check(instance->eventFlow()==EventFlow::Consume && instance->paramCount()==24,"it consumes notes and has its 24 parameters");
    const auto roles=instance->stateRoles();
    check(roles.size()==34 && roles.front()=="state" && roles[1]=="sample:1" && roles[32]=="sample:32" && roles.back()==device::samplerZonesRole,
          "32 sample roles beside its state, and its zone table (SC-483)");
    check(instance->saveState("state").size()<4096,"its parameter state holds no audio");
    const double one=.5*std::pow(10.,-12./20);   // the 0.5 sine at the default -12 dB
    const double slot5=samplerPeak({5},true);
    std::printf("METRIC Sampler, slot 5: peak %.4f (expected %.4f)\n",slot5,one);
    check(std::abs(slot5-one)<2e-3,"a sample dropped into slot 5 plays at once over every key, and again after reopening");
    const double loud=samplerPeak({5},false,0);
    std::printf("METRIC Sampler, slot 5 at Volume 0 dB: peak %.4f (expected 0.5000)\n",loud);
    check(std::abs(loud-.5)<5e-3,"Volume set to 0 dB reaches the core: the 0.5 sine plays at 0.5");
    check(device::Sampler::kEventCapacity>=OneShotPdMidi::capacity,"the Sampler's event list holds every event its MIDI wiring can bring");
    const double layered=samplerPeak({10,2},false);
    std::printf("METRIC Sampler, slots 2 and 10: peak %.4f (expected %.4f)\n",layered,2*one);
    check(std::abs(layered-2*one)<4e-3,"two dropped samples layer (slots 10 and 2: the snapshot lists them 2, 10)");
    check(samplerPeak({},false)==0,"an empty Sampler is silent");
    Project project;auto& store=*project.store;
    // On a MIDI track: an instrument is refused on the helper's audio track 1 (walk-A A7).
    store.db().exec("INSERT INTO tracks(id,kind,name) VALUES(2,'midi','Keys')");
    const auto inserted=prepareBuiltinInsert(store,std::string(device::samplerUid),2);
    check(inserted.ok && commitBuiltinInsert(store,inserted).ok,"a Sampler for the slot checks");
    check(prepareSampleClear(store,inserted.deviceId,1).ok && prepareSampleClear(store,inserted.deviceId,32).ok &&
          !prepareSampleClear(store,inserted.deviceId,0).ok && !prepareSampleClear(store,inserted.deviceId,33).ok,
          "slots 1 to 32 are declared, 0 and 33 refused");
    // A source fits a built-in's state when its float backup does: 8M values.
    check(device::builtinSampleStateFits(8u*1024u*1024u,1) && device::builtinSampleStateFits(4u*1024u*1024u,2) &&
          !device::builtinSampleStateFits(4u*1024u*1024u+1u,2) && !device::builtinSampleStateFits(0,2),
          "a stereo sample holds at most 4M frames, a mono one 8M");
    samplerSavedBefore();  // SC-483
    samplerZones();        // SC-483
#endif
}
#if ADI_WITH_PD
void instrumentSustainWithAutomation(DeviceInstance& source) {
    auto instrument=load("org.adidaw.builtin.oneshot");
    const auto param=[](int index){return std::to_string(index+1);};
    const auto volume=param(OneShot::Volume);
    instrument->prepare(48000,64);
    check(instrument->loadState("sample:1",source.saveState("sample:1")) &&
          instrument->setParam(param(OneShot::Attack),ParamValue::withReal(0,0)) &&
          instrument->setParam(param(OneShot::Release),ParamValue::withReal(0,1)) &&
          instrument->setParam(volume,ParamValue::withReal(0,0)),"OneShot loads the sample for live MIDI");
    std::array<std::array<float,64>,2> output{};
    float* out[]{output[0].data(),output[1].data()};
    NodeIo io;io.out=out;io.channels=2;io.frames=64;
    const auto peak=[&](std::size_t from,std::size_t to) {
        double result=0;
        for(std::size_t i=from;i<to;++i)result=std::max(result,std::abs(static_cast<double>(output[0][i])));
        return result;
    };
    const std::array<Event,4> play{{{0,EventType::NoteOn,7,60,1,0,1},
        {12,EventType::Controller,7,0,0,64,1},
        {32,EventType::ParamValue,0,0,0,OneShot::Volume+1,0},
        {48,EventType::NoteOff,7,60,1,0,0}}};
    io.events={play.data(),static_cast<std::int32_t>(play.size())};
    allocations.store(0,std::memory_order_relaxed);counting=true;
    {audio::CallbackScope callback;instrument->process(io);}
    counting=false;
    check(peak(12,32)>.05 && peak(34,48)<.025 && allocations.load(std::memory_order_relaxed)==0,
          "typed MIDI and one-based automation share a split callback without allocating");
    const Event restore{0,EventType::ParamValue,0,0,0,OneShot::Volume+1,
                        instrument->getParam(volume).normalized};
    io.events={&restore,1};
    instrument->process(io);
    check(peak(0,64)>.05,"the pedal holds the released live note across automation");
    const Event pedalUp{8,EventType::Controller,7,0,0,64,0};
    io.events={&pedalUp,1};
    instrument->process(io);
    check(peak(0,8)>.05 && peak(57,64)==0,"the pedal starts the one-millisecond release at its frame");
}
#endif
void instrumentSession() {
#if ADI_WITH_PD
    const auto* info=findBuiltin("org.adidaw.builtin.oneshot");
    check(info && info->instrument && info->midi && info->inputs==0 && info->available,"OneShot is in the catalogue as an instrument");
    auto instance=load("org.adidaw.builtin.oneshot");
    check(instance->eventFlow()==EventFlow::Consume,"and consumes the notes it is given");
    Project project;auto& store=*project.store;auto& db=store.db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(2,'midi','Keys')");
    const auto inserted=prepareBuiltinInsert(store,"org.adidaw.builtin.oneshot",2);
    check(inserted.ok && commitBuiltinInsert(store,inserted).ok,"the browser inserts OneShot on a MIDI track");
    check(db.execAndGet("SELECT subtype FROM plugin_refs WHERE uid='org.adidaw.builtin.oneshot'").getString()=="instrument",
          "registered with subtype instrument");
    // A 1 kHz sine at 0.5, 1 s, stereo.
    const auto file=project.temporary.path()/"tone.wav";
    {
        std::vector<float> tone(96000);
        for(std::size_t i=0;i<48000;++i)tone[2*i]=tone[2*i+1]=static_cast<float>(.5*std::sin(2*3.141592653589793*1000*static_cast<double>(i)/48000));
        audio::WavWriter writer(file,48000,2,audio::WavFormat::Float32);writer.write(tone.data(),48000);writer.close();
    }
    const auto assigned=prepareSampleAssignment(store,inserted.deviceId,1,file,48000,7);
    if(!assigned.ok)std::printf("assign: %s\n",assigned.error.c_str());
    check(assigned.ok && commitSampleAssignment(store,assigned).ok,"the sample is assigned to OneShot's slot as one undo step");
    // One note: C3 (60, OneShot's root) from the start, one quarter, full velocity.
    db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(5,2,'midi',0,23063040)");
    {
        StreamHeader header{static_cast<std::uint32_t>(FourCC::Notes),1,40,1,0};
        NoteRecord note{};note.start_ticks=0;note.dur_ticks=5765760;note.note_id=1;note.key=60;note.vel_on=127;
        note.vel_off=64;note.probability=10000;
        std::vector<std::uint8_t> bytes(sizeof header+sizeof note);
        std::memcpy(bytes.data(),&header,sizeof header);std::memcpy(bytes.data()+sizeof header,&note,sizeof note);
        SQLite::Statement statement(db,"INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(50,5,'notes',?)");
        statement.bind(1,bytes.data(),static_cast<int>(bytes.size()));statement.exec();
    }
    Session session;session.graph().setFadeFrames(0);
    const bool loaded=session.load(store,withBuiltinDevices(),{2,48000,64});
    if(!session.error().empty())std::printf("session: %s\n",session.error().c_str());
    check(loaded,"the project with OneShot loads in an ordinary Session");
    if(auto* live=session.instanceFor(inserted.deviceId))instrumentSustainWithAutomation(*live);
    session.transport().play();
    std::array<std::array<float,64>,2> output{};float* out[]={output[0].data(),output[1].data()};
    double peak=0;int firstSound=-1;
    for(int block=0;block<200;++block) {
        (void)session.clips()->prime();AudioIo io;io.out=out;io.numOut=2;io.frames=64;session.process(io);
        for(int i=0;i<64;++i) {
            const double v=std::abs(static_cast<double>(output[0][static_cast<std::size_t>(i)]));
            if(v>1e-4 && firstSound<0)firstSound=block*64+i;
            peak=std::max(peak,v);
        }
    }
    std::printf("METRIC OneShot in a Session: first sound at frame %d, peak %.4f\n",firstSound,peak);
    // At full velocity, OneShot's default Volume (Live's -12 dB) on the 0.5 sine.
    check(firstSound>=0 && firstSound<256 && std::abs(peak-.5*std::pow(10.,-12./20))<2e-3,"the MIDI clip's note plays the sample through OneShot, at its default -12 dB");
#endif
    hybridSession();  // DEV-22
    samplerSession();  // DEV-21
}
// Walk-A A7 (DF-W48): a built-in instrument loaded from the Browser (or the Mix View's menu, or a
// drop) goes where Live puts an instrument: onto a MIDI track only, in front of the audio effects,
// and in place of the track's instrument, each one undo step.
void builtinInstrumentPlacement() {
#if ADI_WITH_PD
    Project project;auto& store=*project.store;auto& db=store.db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(2,'midi','Keys'),(4,'audio','Voice')");
    const auto onAudio=prepareBuiltinInsert(store,"org.adidaw.builtin.oneshot",4);
    check(!onAudio.ok && onAudio.error.find("load it onto a MIDI track")!=std::string::npos &&
              db.execAndGet("SELECT count(*) FROM devices").getInt()==0,
          "OneShot is refused on an audio track, which it would silence, and says to use a MIDI track");
    const auto reverb=prepareBuiltinInsert(store,"org.adidaw.builtin.reverb",2);
    check(reverb.ok && commitBuiltinInsert(store,reverb).ok,"Reverb on the MIDI track first");
    const auto oneShot=prepareBuiltinInsert(store,"org.adidaw.builtin.oneshot",2);
    check(oneShot.ok && commitBuiltinInsert(store,oneShot).ok,"then OneShot, with nothing chosen in the strip");
    const auto at=[&db](std::int64_t id){
        SQLite::Statement q(db,"SELECT ord FROM devices WHERE id=?");q.bind(1,id);
        return q.executeStep()?q.getColumn(0).getInt64():std::int64_t{-1};
    };
    check(at(oneShot.deviceId)==0 && at(reverb.deviceId)==1,
          "the instrument goes in front of the audio effect, as a plug-in instrument does, not after it");
    const auto sampler=prepareBuiltinInsert(store,"org.adidaw.builtin.sampler",2);
    check(sampler.ok && commitBuiltinInsert(store,sampler).ok && at(oneShot.deviceId)==-1 &&
              at(sampler.deviceId)==0 && at(reverb.deviceId)==1 &&
              db.execAndGet("SELECT count(*) FROM devices").getInt()==2,
          "a second instrument replaces the first in its place instead of stacking after it");
    check(History(store).undo().ok && at(oneShot.deviceId)==0 && at(sampler.deviceId)==-1 && at(reverb.deviceId)==1,
          "one Undo brings the replaced instrument back where it was");
    const auto utility = prepareBuiltinInsert(store, "org.adidaw.builtin.utility", 2, oneShot.deviceId);
    check(utility.ok && commitBuiltinInsert(store, utility).ok && at(oneShot.deviceId) == 0 &&
              at(utility.deviceId) == 1 && at(reverb.deviceId) == 2,
          "SC-282: a built-in effect loaded at the selected instrument is placed after it");
    check(History(store).undo().ok && at(oneShot.deviceId) == 0 && at(reverb.deviceId) == 1 && at(utility.deviceId) == -1,
          "SC-282: one Undo restores the original effect order");
#endif
}
int main(){try{catalogueAndHost();paramsAndState();insertionAndHistory();thirdPartyPlacement();hotSwapPlacement();moveDeviceRange();playingSession();instrumentSession();duplicatePrepareKeepsBuiltinState();builtinInstrumentPlacement();}catch(const std::exception& exception){counting=false;check(false,exception.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
