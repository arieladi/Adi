// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/device_copy.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace {
using namespace adi;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
struct Fixture {
    test::TempDirectory temporary{"device_copy","stored"};
    std::unique_ptr<Store> store;
    std::string hash;
    Fixture() {
        StoreError error{};store=Store::create(temporary.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
        auto& db=store->db();
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Source'),(2,'audio','Destination')");
        db.exec("INSERT INTO device_chains(id,track_id,name) VALUES(10,1,'Source chain'),(20,2,'Destination')");
        db.exec("INSERT INTO plugin_refs(id,format,uid,vendor,name,version,subtype,path_hint,is_shell,shell_id) VALUES(30,'clap','test.uid','Vendor','Original','1.2','effect','not-loaded.clap',1,0)");
        db.exec("INSERT INTO remote_hosts(id,name,host,port) VALUES(50,'LAN','localhost',55000)");
        db.exec("INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name,enabled,is_rack,rack_kind,preset_name,latency_samples,remote_host_id,always_process,missing) VALUES(40,10,9223372036854775806,30,'Renamed',0,0,NULL,'Preset',5000000000,50,1,1),"
            "(51,20,5,30,'Before',1,0,NULL,'',0,NULL,0,0),(52,20,5,30,'Target',1,0,NULL,'',0,NULL,0,0),(53,20,9,30,'After',1,0,NULL,'',0,NULL,0,0)");
        db.exec("INSERT INTO plugin_params(device_id,param_id,name,normalized_value,real_value,display,unit,flags) VALUES(40,'tone','Tone',0,NULL,'Zero','Hz',9223372036854775806)");
        const std::vector<std::byte> bytes={std::byte{0},std::byte{255},std::byte{1}};
        hash=media::blake3Bytes(bytes).hex;
        SQLite::Statement blob(db,"INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
        blob.bind(1,hash);blob.bind(2,bytes.data(),static_cast<int>(bytes.size()));blob.bind(3,static_cast<std::int64_t>(bytes.size()));blob.exec();
        SQLite::Statement state(db,"INSERT INTO plugin_state(device_id,stream_role,state_hash,format_hint) VALUES(40,'state',?,'opaque')");
        state.bind(1,hash);state.exec();
        db.exec("INSERT INTO device_panels(device_id) VALUES(40);INSERT INTO device_panel_params(device_id,ord,param_id) VALUES(40,1,'unknown'),(40,0,'tone')");
        db.exec("INSERT INTO device_expression_routes(device_id,route) VALUES(40,'mpe_midi')");
    }
};
void reads() {
    Fixture f;auto& db=f.store->db();const auto before=db.execAndGet("SELECT count(*) FROM ops").getInt();
    const auto copy=readDeviceCopy(*f.store,40);
    check(copy.ok && !copy.hasNestedChains && copy.insert.at("id")==40 && copy.insert.at("chain")==10,"read captures native device identity without a live host");
    check(copy.insert.at("ord")==std::numeric_limits<std::int64_t>::max()-1 && copy.insert.at("latency")==5000000000LL,"read retains full 64-bit native order and latency");
    check(copy.insert.at("name")=="Renamed" && copy.insert.at("enabled")==false && copy.insert.at("always")==true && copy.insert.at("missing")==true && copy.insert.at("preset")=="Preset","device state flags and user name are distinct from plug-in metadata");
    check(copy.reference && copy.reference->id==30 && copy.reference->uid=="test.uid" && copy.reference->format=="clap" &&
          copy.reference->name=="Original" && copy.reference->pathHint=="not-loaded.clap" && copy.reference->isShell && copy.reference->shellId==0,"read includes the referenced plug-in metadata and explicit shell zero");
    check(copy.insert.at("rackKind").is_null() && copy.insert.at("remoteHost")==50,"native nullable rack and remote host metadata are retained");
    check(copy.insert.at("params")[0].at("norm")==0 && copy.insert.at("params")[0].at("real").is_null() &&
          copy.insert.at("params")[0].at("flags")==std::numeric_limits<std::int64_t>::max()-1,"stored params preserve zero, absent real value and full-width flags");
    check(copy.insert.at("state")[0].at("hash")==f.hash && copy.insert.at("state")[0].at("hint")=="opaque" &&
          f.store->getStateBlob(f.hash)->size()==3,"copy carries state hashes without inlining opaque pool bytes");
    check(copy.insert.at("panel")==Payload::array({"tone","unknown"}) && copy.insert.at("route")=="mpe_midi","configured panel order and expression route are readable");
    check(db.execAndGet("SELECT count(*) FROM ops").getInt()==before,"copy read appends no op");
    check(!readDeviceCopy(*f.store,999).ok,"missing device is refused");
    StoreError error{};auto readonly=Store::open(f.temporary.path()/"song.adi",error,true);
    check(readonly && readDeviceCopy(*readonly,40).ok,"read-only project supports device inspection");
    auto refused=prepareDevicePaste(*readonly,copy,70,20,52);
    check(!refused.ok,"read-only project refuses paste preparation");
    OpRequest edit;edit.opType="device.setParam";edit.payload={{"dev",40},{"param","tone"},{"norm",0.75}};
    check(OpJournal(*f.store).commit(edit).ok && readDeviceCopy(*f.store,40).insert.at("params")[0].at("norm")==0.75 &&
          copy.insert.at("params")[0].at("norm")==0,"new read reflects journal edit while retained clipboard remains immutable");
}
void pasteAndUndo() {
    Fixture f;auto& db=f.store->db();const auto copy=readDeviceCopy(*f.store,40);
    const auto prepared=prepareDevicePaste(*f.store,copy,70,20,52);
    check(prepared.ok && prepared.options.expectHead && !prepared.options.expectHead->has_value(),"paste captures an expected empty undo root");
    const auto result=commitDevicePaste(*f.store,prepared);
    if(!result.ok)std::printf("paste: %s\n",result.error.c_str());
    check(result.ok,"ordered paste commits its moves and insertion as one batch");
    check(db.execAndGet("SELECT group_concat(id,',') FROM (SELECT id FROM devices WHERE chain_id=20 ORDER BY ord,id)").getString()=="51,70,52,53","paste is immediately before the selected device, including legacy order ties");
    const auto pasted=readDeviceCopy(*f.store,70);
    check(pasted.ok && pasted.insert.at("params")==copy.insert.at("params") && pasted.insert.at("state")==copy.insert.at("state") &&
          pasted.insert.at("panel")==copy.insert.at("panel") && pasted.insert.at("route")==copy.insert.at("route"),"pasted device has exact stored params, state hashes, panel and route");
    check(pasted.insert.at("latency")==5000000000LL && pasted.insert.at("remoteHost")==50,"paste preserves full-width fields and remote host reference");
    check(History(*f.store).undo().ok && db.execAndGet("SELECT count(*) FROM devices WHERE id=70").getInt()==0 &&
          db.execAndGet("SELECT ord FROM devices WHERE id=51").getInt64()==5 &&
          db.execAndGet("SELECT ord FROM devices WHERE id=52").getInt64()==5 &&
          db.execAndGet("SELECT ord FROM devices WHERE id=53").getInt64()==9,"one undo removes the copy and restores every exact original order value");
    check(History(*f.store).redo().ok && readDeviceCopy(*f.store,70).insert.at("params")==copy.insert.at("params"),"redo uses captured identity and stored values");
    const auto append=prepareDevicePaste(*f.store,copy,71,20);
    check(append.ok && commitDevicePaste(*f.store,append).ok &&
          db.execAndGet("SELECT id FROM devices WHERE chain_id=20 ORDER BY ord DESC,id DESC LIMIT 1").getInt64()==71,"null selection appends to the chosen chain");
}
void refusals() {
    Fixture f;auto& db=f.store->db();const auto copy=readDeviceCopy(*f.store,40);
    check(!prepareDevicePaste(*f.store,copy,40,20).ok && !prepareDevicePaste(*f.store,copy,51,20).ok,"paste refuses source identity and occupied IDs");
    check(!prepareDevicePaste(*f.store,copy,70,999).ok && !prepareDevicePaste(*f.store,copy,70,20,40).ok,"paste refuses missing chain and selection in another chain");
    for(const char* field:{"remoteHost","flags"}) {
        auto invalid=copy;
        if(std::string(field)=="flags")invalid.insert["params"][0]["flags"]=std::numeric_limits<std::uint64_t>::max();
        else invalid.insert[field]=std::numeric_limits<std::uint64_t>::max();
        const auto invalidPaste=prepareDevicePaste(*f.store,invalid,70,20,52);
        const auto refusedInteger=commitDevicePaste(*f.store,invalidPaste);
        check(!refusedInteger.ok && db.execAndGet("SELECT count(*) FROM devices WHERE id=70").getInt()==0 &&
              db.execAndGet("SELECT ord FROM devices WHERE id=51").getInt64()==5,"out-of-range integers roll back insertion and all earlier order moves");
    }
    const auto prepared=prepareDevicePaste(*f.store,copy,70,20,52);
    OpRequest rename;rename.opType="device.rename";rename.payload={{"id",40},{"name","Newer"}};
    check(OpJournal(*f.store).commit(rename).ok,"intervening edit fixture");
    const auto stale=commitDevicePaste(*f.store,prepared);
    check(!stale.ok && stale.stale && db.execAndGet("SELECT count(*) FROM devices WHERE id=70").getInt()==0 &&
          db.execAndGet("SELECT ord FROM devices WHERE id=51").getInt64()==5,"stale paste refuses all order changes and insertion");
    Fixture other;check(!prepareDevicePaste(*other.store,copy,70,20).ok,"clipboard cannot confuse state pools or plug-in refs from another project");
    db.exec("UPDATE devices SET is_rack=1,rack_kind='audio_effect',remote_host_id=NULL WHERE id=40");
    db.exec("INSERT INTO device_chains(id,parent_device_id,name) VALUES(60,40,'Nested')");
    const auto rack=readDeviceCopy(*f.store,40);
    check(rack.ok && rack.hasNestedChains && rack.insert.at("rackKind")=="audio_effect" && rack.insert.at("remoteHost").is_null(),"reader exposes rack metadata and nested-chain limitation");
    const auto refused=prepareDevicePaste(*f.store,rack,70,20);
    check(!refused.ok && refused.error.find("nested chain image")!=std::string::npos,"nested rack copy refuses rather than dropping its contents");
}
}
int main(){try{reads();pasteAndUndo();refusals();}catch(const std::exception& e){check(false,e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
