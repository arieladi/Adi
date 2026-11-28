// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/device_copy.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
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
void deviceRolePlacement() {
    Fixture fixture;
    auto &store = *fixture.store;
    auto &db = store.db();
    db.exec("UPDATE tracks SET kind='midi';"
            "INSERT INTO tracks(id,kind,name) VALUES(3,'audio','Audio'),(4,'midi','MIDI');"
            "INSERT INTO device_chains(id,track_id,name) VALUES(21,3,'Audio chain');"
            "INSERT INTO plugin_refs(id,format,uid,subtype) VALUES"
            "(31,'clap','test.instrument','instrument'),(32,'clap','test.midi','midi_effect');"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES"
            "(41,10,0,31,'Instrument'),(42,10,-1,32,'MIDI effect');"
            "UPDATE devices SET plugin_ref_id=32 WHERE id=51;"
            "UPDATE devices SET plugin_ref_id=31 WHERE id=52;");
    const auto ids = [&] {
        return db.execAndGet("SELECT group_concat(id,',') FROM "
                             "(SELECT id FROM devices WHERE chain_id=20 ORDER BY ord,id)").getString();
    };
    const auto audioEffect = readDeviceCopy(store, 40);
    const auto midiEffect = readDeviceCopy(store, 42);
    const auto instrument = readDeviceCopy(store, 41);
    const auto audioPaste = prepareDevicePaste(store, audioEffect, 70, 20, 52);
    check(audioPaste.ok && commitDevicePaste(store, audioPaste).ok && ids() == "51,52,70,53",
          "SC-284: pasting an audio effect at an instrument lands after it");
    const auto midiPaste = prepareDevicePaste(store, midiEffect, 71, 20, 53);
    check(midiPaste.ok && commitDevicePaste(store, midiPaste).ok && ids() == "51,71,52,70,53",
          "SC-284: pasting a MIDI effect at an audio effect lands before the instrument");
    const auto head = OpJournal(store).headSeq();
    const auto duplicate = prepareDevicePaste(store, readDeviceCopy(store, 52), 72, 20, 70);
    check(!duplicate.ok && duplicate.error.find("already has an instrument") != std::string::npos &&
              !prepareDevicePaste(store, instrument, 72, 21).ok &&
              !prepareDevicePasteForTrack(store, midiEffect, 72, 3).ok && OpJournal(store).headSeq() == head,
          "SC-284: an instrument cannot be duplicated in its chain or pasted onto audio; MIDI effects also need MIDI tracks");
    const auto synthPaste = prepareDevicePasteForTrack(store, instrument, 72, 4);
    check(synthPaste.ok && commitDevicePaste(store, synthPaste).ok &&
              db.execAndGet("SELECT track_id FROM device_chains JOIN devices ON devices.chain_id=device_chains.id "
                           "WHERE devices.id=72").getInt64() == 4 &&
              History(store).undo().ok && OpJournal(store).headSeq() == head &&
              db.execAndGet("SELECT count(*) FROM device_chains WHERE track_id=4").getInt() == 0,
          "SC-284: a copied instrument can still be pasted onto an empty MIDI track and undone in one step");
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

}
void reusedReferenceIds() {
    Fixture f;auto& db=f.store->db();const auto copy=readDeviceCopy(*f.store,40);
    check(copy.remoteHost && copy.remoteHost->id==50 && copy.remoteHost->host=="localhost" &&
          copy.remoteHost->port==55000 && copy.remoteHost->protocol=="audiogridder","clipboard captures remote endpoint identity without opening it");
    // Simulates ID reuse after undoing the original reference creation.
    db.exec("UPDATE plugin_refs SET uid='different.plugin' WHERE id=30");
    const auto wrong=prepareDevicePaste(*f.store,copy,70,20);
    check(!wrong.ok && wrong.error.find("identity changed")!=std::string::npos,"same project and row ID cannot select a different plug-in from an old clipboard");
    db.exec("UPDATE plugin_refs SET uid='test.uid',name='Updated label',path_hint='moved.clap' WHERE id=30");
    check(prepareDevicePaste(*f.store,copy,70,20).ok,"plug-in label and binary relocation keep the same identity");
    db.exec("UPDATE remote_hosts SET host='other-machine' WHERE id=50");
    const auto remote=prepareDevicePaste(*f.store,copy,70,20);
    check(!remote.ok && remote.error.find("identity changed")!=std::string::npos,"reused remote endpoint ID cannot redirect a copied device");
    db.exec("UPDATE remote_hosts SET host='localhost',name='New label' WHERE id=50");
    check(prepareDevicePaste(*f.store,copy,70,20).ok,"remote endpoint label does not change its identity");
}

void emptyTrack(Fixture &f) {
    f.store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(3,'audio','Empty')");
}
void pasteToEmptyTrack() {
    Fixture f;
    emptyTrack(f);
    auto &db = f.store->db();
    const auto copy = readDeviceCopy(*f.store, 40);
    const auto before = digestProject(*f.store).text;
    const auto prepared = prepareDevicePasteForTrack(*f.store, copy, 70, 3);
    check(prepared.ok && prepared.chainId == 21 && prepared.requests.size() == 2 &&
              prepared.requests.front().opType == "chain.create" &&
              prepared.requests.back().opType == "device.insert",
          "track paste stages the missing chain before the device");
    check(digestProject(*f.store).text == before &&
              db.execAndGet("SELECT count(*) FROM ops").getInt() == 0,
          "track paste preparation changes neither project nor journal");
    const auto result = commitDevicePaste(*f.store, prepared);
    check(result.ok &&
              db.execAndGet("SELECT count(*) FROM device_chains WHERE id=21 AND track_id=3 AND "
                            "ord=0 AND name='Devices'")
                      .getInt() == 1 &&
              db.execAndGet("SELECT count(*) FROM devices WHERE id=70 AND chain_id=21 AND ord=0")
                      .getInt() == 1,
          "one commit creates a real destination chain and its pasted device");
    check(db.execAndGet("SELECT count(*) FROM ops").getInt() == 2 &&
              db.execAndGet("SELECT count(DISTINCT txn_id) FROM ops").getInt() == 1,
          "chain creation and device insertion share one undo transaction");
    const auto pasted = readDeviceCopy(*f.store, 70);
    check(pasted.ok && copy.ok && pasted.insert.at("params") == copy.insert.at("params") &&
              pasted.insert.at("state") == copy.insert.at("state") &&
              pasted.insert.at("panel") == copy.insert.at("panel") &&
              pasted.insert.at("route") == copy.insert.at("route"),
          "empty-track paste retains exact parameter/state/panel/route bytes");
    check(History(*f.store).undo().ok && digestProject(*f.store).text == before &&
              db.execAndGet("SELECT count(*) FROM device_chains WHERE track_id=3").getInt() == 0,
          "one undo restores the exact project with no destination chain");
    const auto redone = History(*f.store).redo();
    const auto again = readDeviceCopy(*f.store, 70);
    check(redone.ok && again.ok && pasted.ok && again.insert == pasted.insert,
          "redo restores the same chain identity and stored device image");
    check(!commitDevicePaste(*f.store, prepared).ok &&
              db.execAndGet("SELECT count(*) FROM device_chains WHERE track_id=3").getInt() == 1,
          "a completed track packet cannot create a second chain");
}
void appendToTrackAndSubmitPacket() {
    Fixture f;
    const auto copy = readDeviceCopy(*f.store, 40);
    OpRequest rename;
    rename.opType = "device.rename";
    rename.payload = {{"id", 40}, {"name", "Changed after copy"}};
    check(OpJournal(*f.store).commit(rename).ok, "track append captures a non-root undo head");
    const auto before = digestProject(*f.store).text;
    const auto prepared = prepareDevicePasteForTrack(*f.store, copy, 70, 2);
    check(prepared.ok && prepared.chainId == 20 &&
              std::none_of(prepared.requests.begin(), prepared.requests.end(),
                           [](const auto &op) { return op.opType == "chain.create"; }),
          "track paste reuses the existing top-level chain");
    check(OpJournal(*f.store).commit(prepared.requests).ok &&
              f.store->db()
                      .execAndGet("SELECT group_concat(id,',') FROM (SELECT id FROM devices WHERE "
                                  "chain_id=20 ORDER BY ord,id)")
                      .getString() == "51,52,53,70",
          "ordinary outside-engine submission carries the complete prepared append");
    check(History(*f.store).undo().ok && digestProject(*f.store).text == before,
          "undo of a track append retains its original chain and exact tied orders");
}
void topLevelTrackChainChoice() {
    Fixture f;
    f.store->db().exec("INSERT INTO device_chains(id,track_id,ord,name) VALUES(25,2,-1,'First')");
    const auto before = digestProject(*f.store).text;
    const auto prepared = prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 2);
    check(
        prepared.ok && prepared.chainId == 25 && commitDevicePaste(*f.store, prepared).ok &&
            f.store->db()
                    .execAndGet("SELECT count(*) FROM devices WHERE id=70 AND chain_id=25")
                    .getInt() == 1 &&
            f.store->db()
                    .execAndGet("SELECT count(*) FROM devices WHERE chain_id=20 AND ord=5")
                    .getInt() == 2,
        "track append chooses the first top-level chain by order and leaves its sibling untouched");
    check(History(*f.store).undo().ok && digestProject(*f.store).text == before,
          "undo retains both pre-existing top-level chains");
}
void fullWidthTrackChainIdentity() {
    Fixture f;
    emptyTrack(f);
    SQLite::Statement last(f.store->db(), "INSERT INTO device_chains(id,track_id) VALUES(?,1)");
    last.bind(1, std::numeric_limits<std::int64_t>::max() - 1);
    last.exec();
    const auto before = digestProject(*f.store).text;
    const auto prepared = prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 3);
    check(prepared.ok && prepared.chainId == std::numeric_limits<std::int64_t>::max() &&
              commitDevicePaste(*f.store, prepared).ok,
          "track paste keeps a chain identity at the full int64 boundary");
    check(History(*f.store).undo().ok && digestProject(*f.store).text == before,
          "undo of a full-width chain identity restores the exact project");
}
void staleTrackPacket(Fixture &f, const PreparedDevicePaste &prepared, const char *label) {
    const auto before = digestProject(*f.store).text;
    const auto ops = f.store->db().execAndGet("SELECT count(*) FROM ops").getInt64();
    const auto result = commitDevicePaste(*f.store, prepared);
    check(!result.ok && result.stale && digestProject(*f.store).text == before &&
              f.store->db().execAndGet("SELECT count(*) FROM ops").getInt64() == ops,
          label);
}
void trackPasteGuards() {
    {
        Fixture f;
        emptyTrack(f);
        const auto prepared =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 3);
        f.store->db().exec("INSERT INTO device_chains(id,track_id,name) VALUES(99,3,'External')");
        staleTrackPacket(f, prepared,
                         "same-head added destination chain refuses the staged empty-chain packet");
    }
    {
        Fixture f;
        emptyTrack(f);
        const auto prepared =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 3);
        f.store->db().exec("UPDATE tracks SET name='Changed' WHERE id=3");
        staleTrackPacket(f, prepared,
                         "same-head changed destination track refuses before chain creation");
    }
    {
        Fixture f;
        const auto prepared =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 2);
        f.store->db().exec("UPDATE devices SET ord=17 WHERE id=53");
        staleTrackPacket(f, prepared,
                         "same-head destination order edit is not overwritten by captured moves");
    }
    {
        Fixture f;
        const auto prepared =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 2);
        f.store->db().exec("INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) "
                           "VALUES(100,20,7,30,'External')");
        staleTrackPacket(f, prepared,
                         "same-head inserted destination device is covered by the row-set guard");
    }
    {
        Fixture f;
        emptyTrack(f);
        const auto prepared =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 3);
        f.store->db().exec(
            "INSERT OR REPLACE INTO device_chains(id,track_id,name) VALUES(21,1,'Occupied')");
        staleTrackPacket(
            f, prepared,
            "reserved chain identity reuse is a stale-target refusal without partial rows");
    }
    {
        Fixture f;
        emptyTrack(f);
        const auto prepared =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 3);
        f.store->db().exec(
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(70,10,0,30,'Reused')");
        staleTrackPacket(f, prepared,
                         "same-head new device identity reuse refuses before any chain write");
    }
    {
        Fixture f;
        emptyTrack(f);
        const auto prepared =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 3);
        f.store->db().exec("UPDATE plugin_refs SET uid='different.plugin' WHERE id=30");
        staleTrackPacket(
            f, prepared,
            "same-head plug-in reference reuse cannot redirect a prepared track paste");
    }
    {
        Fixture f;
        emptyTrack(f);
        const auto prepared =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 3);
        OpRequest change;
        change.opType = "device.rename";
        change.payload = {{"id", 40}, {"name", "Newer"}};
        check(OpJournal(*f.store).commit(change).ok,
              "intervening edit before ordinary track-packet submission");
        const auto before = digestProject(*f.store).text;
        const auto result = OpJournal(*f.store).commit(prepared.requests);
        check(!result.ok && result.stale && digestProject(*f.store).text == before,
              "ordinary submission cannot drop the captured expected undo head");
    }
}
void trackPasteAtomicRefusals() {
    {
        Fixture f;
        emptyTrack(f);
        const auto prepared =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 3);
        auto partial = prepared;
        if (!partial.requests.empty())
            partial.requests.erase(partial.requests.begin());
        check(!commitDevicePaste(*f.store, partial).ok &&
                  f.store->db()
                          .execAndGet("SELECT count(*) FROM device_chains WHERE track_id=3")
                          .getInt() == 0,
              "an incomplete outside-engine packet refuses without a chain");
        check(commitDevicePaste(*f.store, prepared).ok,
              "invalid partial offer does not poison the authentic complete packet");
    }
    {
        Fixture f;
        emptyTrack(f);
        auto invalid = readDeviceCopy(*f.store, 40);
        const auto before = digestProject(*f.store).text;
        if (invalid.ok && invalid.insert.contains("params") &&
            invalid.insert.at("params").is_array() && !invalid.insert.at("params").empty())
            invalid.insert["params"][0]["flags"] = std::numeric_limits<std::uint64_t>::max();
        const auto prepared = prepareDevicePasteForTrack(*f.store, invalid, 70, 3);
        const auto result = commitDevicePaste(*f.store, prepared);
        check(
            !result.ok && digestProject(*f.store).text == before &&
                f.store->db().execAndGet("SELECT count(*) FROM ops").getInt() == 0,
            "failed device insertion rolls back the earlier real chain.create and all audit rows");
        const auto retry =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 3);
        check(retry.ok && retry.chainId == 21 && commitDevicePaste(*f.store, retry).ok,
              "a fresh packet can reuse identities after atomic rollback");
    }
    {
        Fixture f;
        emptyTrack(f);
        SQLite::Statement max(f.store->db(), "INSERT INTO device_chains(id,track_id) VALUES(?,1)");
        max.bind(1, std::numeric_limits<std::int64_t>::max());
        max.exec();
        const auto prepared =
            prepareDevicePasteForTrack(*f.store, readDeviceCopy(*f.store, 40), 70, 3);
        check(!prepared.ok && prepared.error.find("exhausted") != std::string::npos &&
                  f.store->db()
                          .execAndGet("SELECT count(*) FROM device_chains WHERE track_id=3")
                          .getInt() == 0,
              "chain identity exhaustion refuses without signed overflow or writes");
    }
    {
        Fixture f;
        const auto copy = readDeviceCopy(*f.store, 40);
        check(!prepareDevicePasteForTrack(*f.store, copy, 70, 999).ok,
              "track paste refuses a missing destination");
        Fixture other;
        check(!prepareDevicePasteForTrack(*other.store, copy, 70, 2).ok,
              "track paste keeps the same-project clipboard identity check");
    }
}

void rangePaste() {
    Fixture f;
    auto &store = *f.store;
    auto &db = store.db();
    const std::array copies{readDeviceCopy(store, 40), readDeviceCopy(store, 51)};
    // IDs need not be adjacent or ascending. The clipboard decides the order.
    const std::array<std::int64_t, 2> newIds{90, 70};
    const auto before = digestProject(store).text;
    const auto paste = prepareDevicePaste(store, copies, newIds, 20, 52);
    check(paste.ok && digestProject(store).text == before, "SC-288: preparing an anchored range writes nothing");
    if (!paste.ok)
        return;
    auto incomplete = paste;
    incomplete.requests.pop_back();
    check(!OpJournal(store).commit(incomplete.requests).ok && digestProject(store).text == before,
          "an incomplete range cannot reach the ordinary journal");
    const auto applied = commitDevicePaste(store, paste);
    check(applied.ok && db.execAndGet("SELECT group_concat(id,',') FROM "
                                    "(SELECT id FROM devices WHERE chain_id=20 ORDER BY ord,id)").getString() ==
                            "51,90,70,52,53" &&
              db.execAndGet("SELECT count(DISTINCT txn_id) FROM ops").getInt() == 1,
          "the range lands in its original order before the anchor as one transaction");
    const auto pasted = readDeviceCopy(store, 90);
    check(pasted.ok && pasted.insert.at("params") == copies[0].insert.at("params") &&
              pasted.insert.at("state") == copies[0].insert.at("state"),
          "the pasted range keeps stored parameter and state images");
    const auto landed = digestProject(store).text;
    check(History(store).undo().ok && digestProject(store).text == before,
          "one undo removes the whole range and restores old ordinal ties");
    check(History(store).redo().ok && digestProject(store).text == landed,
          "one redo restores both pasted devices and their identities");
}

void rangePasteRoles() {
    Fixture f;
    auto &store = *f.store;
    auto &db = store.db();
    db.exec("UPDATE tracks SET kind='midi';"
            "INSERT INTO tracks(id,kind,name) VALUES(3,'audio','Empty Audio'),(4,'midi','Empty MIDI');"
            "INSERT INTO plugin_refs(id,format,uid,subtype) VALUES"
            "(31,'clap','test.instrument','instrument'),(32,'clap','test.midi','midi_effect');"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES"
            "(41,10,0,31,'Instrument'),(42,10,-1,32,'MIDI effect');"
            "UPDATE devices SET plugin_ref_id=32 WHERE id=51;"
            "UPDATE devices SET plugin_ref_id=31 WHERE id=52;");
    const std::array copies{readDeviceCopy(store, 42), readDeviceCopy(store, 41), readDeviceCopy(store, 40)};
    const std::array<std::int64_t, 3> newIds{73, 90, 70};
    const std::array<std::int64_t, 2> twoIds{73, 90};
    const auto before = digestProject(store).text;
    const auto duplicateInstrument = prepareDevicePaste(store, copies, newIds, 20, 52);
    const std::array twoInstruments{copies[1], readDeviceCopy(store, 52)};
    const std::array separatedByInstrument{copies[0], copies[2]};
    const std::array reversed{copies[2], copies[0]};
    check(!duplicateInstrument.ok && duplicateInstrument.error.find("already has an instrument") != std::string::npos &&
              !prepareDevicePasteForTrack(store, twoInstruments, twoIds, 4).ok &&
              !prepareDevicePasteForTrack(store, copies, newIds, 3).ok &&
              !prepareDevicePaste(store, separatedByInstrument, twoIds, 20, 52).ok &&
              !prepareDevicePasteForTrack(store, reversed, twoIds, 4).ok && digestProject(store).text == before,
          "incompatible roles or source order refuse the whole range before creating anything");
    const auto paste = prepareDevicePasteForTrack(store, copies, newIds, 4);
    check(paste.ok && paste.requests.size() == 4 && paste.requests.front().opType == "chain.create" &&
              commitDevicePaste(store, paste).ok &&
              db.execAndGet("SELECT group_concat(id,',') FROM (SELECT d.id FROM devices d JOIN device_chains c "
                            "ON c.id=d.chain_id WHERE c.track_id=4 ORDER BY d.ord,d.id)").getString() == "73,90,70" &&
              db.execAndGet("SELECT count(DISTINCT txn_id) FROM ops").getInt() == 1,
          "one paste stages a new MIDI chain and keeps MIDI effect, instrument and audio effect together");
    check(History(store).undo().ok && digestProject(store).text == before,
          "one undo also removes the chain staged for a pasted range");
}

void rangePasteAtomicRefusals() {
    Fixture f;
    emptyTrack(f);
    auto &store = *f.store;
    auto &db = store.db();
    db.exec("INSERT INTO plugin_refs(id,format,uid,subtype) VALUES(31,'internal','org.adidaw.mix.eq','effect');"
            "INSERT INTO device_chains(id,track_id,name) VALUES(25,2,'Sibling chain');"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES"
            "(41,10,0,31,'EQ'),(55,25,0,31,'Already on destination');");
    const auto effect = readDeviceCopy(store, 40);
    const auto function = readDeviceCopy(store, 41);
    const std::array<std::int64_t, 2> newIds{90, 70};
    const std::array nativeConflict{effect, function};
    const std::array nativeTwice{function, readDeviceCopy(store, 55)};
    check(!prepareDevicePaste(store, nativeConflict, newIds, 20, 52).ok &&
              !prepareDevicePasteForTrack(store, nativeTwice, newIds, 3).ok,
          "a native mix function cannot be pasted twice or duplicated elsewhere on the destination channel");

    const std::array copies{effect, readDeviceCopy(store, 51)};
    const std::array<std::int64_t, 2> repeatedId{90, 90};
    check(!prepareDevicePasteForTrack(store, copies, repeatedId, 3).ok,
          "every pasted member needs its own fresh identity");
    const auto prepared = prepareDevicePaste(store, copies, newIds, 20, 52);
    check(prepared.ok, "range anchor packet prepared before destination changes");
    db.exec("UPDATE devices SET ord=19 WHERE id=52");
    staleTrackPacket(f, prepared, "same-head destination reorder refuses the whole anchored range");

    auto invalid = std::array{effect, effect};
    invalid[1].insert["params"][0]["flags"] = std::numeric_limits<std::uint64_t>::max();
    const auto before = digestProject(store).text;
    const auto failed = prepareDevicePasteForTrack(store, invalid, newIds, 3);
    check(!commitDevicePaste(store, failed).ok && digestProject(store).text == before &&
              db.execAndGet("SELECT count(*) FROM ops").getInt() == 0,
          "failure of a later insertion rolls back the earlier device, chain, and journal rows");
}

void seedRack(Fixture& f) {
    auto& db=f.store->db();
    db.exec("UPDATE devices SET is_rack=1,rack_kind='audio_effect',plugin_ref_id=NULL,remote_host_id=NULL WHERE id=40;"
            "INSERT INTO device_chains(id,parent_device_id,ord,name,key_zone,muted) VALUES(21,40,2,'Nested',X'000102ff',1);"
            "INSERT INTO devices(id,chain_id,name,is_rack,rack_kind) VALUES(2,21,'Inner',1,'audio_effect');"
            "INSERT INTO device_chains(id,parent_device_id,ord,name,chain_zone,soloed) VALUES(22,2,3,'Leaf chain',X'',1);"
            "INSERT INTO plugin_refs(id,format,uid,name) VALUES(31,'vst3','test.nested','Nested plugin');"
            "INSERT INTO devices(id,chain_id,name,plugin_ref_id,remote_host_id) VALUES(42,22,'Leaf',31,50);"
            "INSERT INTO plugin_params(device_id,param_id,name,normalized_value,display) VALUES(42,'gain','Gain',0.75,'3 dB');"
            "INSERT INTO plugin_state SELECT 42,stream_role,state_hash,format_hint FROM plugin_state WHERE device_id=40;"
            "INSERT INTO device_expression_routes(device_id,route) VALUES(42,'mpe_midi');"
            "INSERT INTO device_panels(device_id) VALUES(42);"
            "INSERT INTO device_panel_params(device_id,ord,param_id) VALUES(42,4,'gain');"
            "INSERT INTO macros(id,device_id,ord,name) VALUES(9,40,0,'Outer'),(12,51,0,'Outside'),(99,2,0,'Inner');"
            "INSERT INTO macro_mappings(id,macro_id,target_device_id,target_param_ref,inverted) VALUES"
            "(65,9,42,'gain',1),(66,9,51,'gain',0),(67,12,42,'gain',0),(68,99,42,'gain',0);"
            "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,channel_map) VALUES"
            "(71,'track',2,'device',42,'sidechain',X''),(72,'device',2,'device',42,'sidechain',NULL);"
            "INSERT INTO routing_key_taps(routing_id,tap) VALUES(71,'post_fx'),(72,'pre_fx');"
            "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(81,'device',42,'gain');"
            "INSERT INTO controller_maps(id,target_kind,target_id,target_param) VALUES(92,'device',42,'gain');");
}

std::int64_t scalar(Fixture& f,const char* sql) {
    return f.store->db().execAndGet(sql).getInt64();
}

void rackPasteRoundTrip() {
    Fixture f;
    seedRack(f);
    const auto origin=digestProject(*f.store).text;
    const auto copy=readDeviceCopy(*f.store,40);
    check(copy.ok && copy.hasNestedChains && copy.hasOriginalDeviceId(40) &&
          copy.hasOriginalDeviceId(2) && copy.hasOriginalDeviceId(42) && !copy.hasOriginalDeviceId(51) &&
          copy.insert.at("rackKind")=="audio_effect" && copy.insert.at("remoteHost").is_null() &&
          digestProject(*f.store).text==origin && scalar(f,"SELECT count(*) FROM ops")==0,
          "rack capture holds nested origin IDs and makes no writes");
    if (!copy.ok) return;
    f.store->db().exec("UPDATE plugin_params SET normalized_value=0.25 WHERE device_id=42");
    const auto before=digestProject(*f.store).text;
    const auto prepared=prepareDevicePaste(*f.store,copy,100,20,52);
    check(prepared.ok && prepared.chainId==20 && prepared.requests.size()==4 &&
          digestProject(*f.store).text==before,"rack paste prepares sealed placement without writes");
    if (!prepared.ok) return;
    auto altered=prepared;
    altered.requests.back().payload["chain"]=10;
    check(!commitDevicePaste(*f.store,altered).ok && digestProject(*f.store).text==before,
          "tampering with a prepared rack destination refuses before touching existing order");
    auto raw = prepared.requests.back();
    raw.capturedTransaction.reset();
    raw.payload["chain"] = 10;
    check(!OpJournal(*f.store).commit(raw).ok && digestProject(*f.store).text == before,
          "a direct typed insert also validates the rack image's destination");
    const auto result=OpJournal(*f.store).commit(prepared.requests);
    check(result.ok,"submit the intact sealed rack paste through the ordinary journal");
    if (!result.ok) throw std::runtime_error(result.error);
    const auto after=digestProject(*f.store).text;
    check(scalar(f,"SELECT id FROM devices WHERE chain_id=20 AND ord=1")==100 &&
          scalar(f,"SELECT count(*) FROM device_chains WHERE parent_device_id=100 AND id>22 AND muted "
                   "AND key_zone=X'000102ff' AND ord=2")==1 &&
          scalar(f,"SELECT count(*) FROM device_chains WHERE parent_device_id=54 AND id>22 AND soloed "
                   "AND chain_zone=X'' AND ord=3")==1 &&
          scalar(f,"SELECT count(*) FROM devices WHERE id=55 AND chain_id>22 AND plugin_ref_id=31 "
                   "AND remote_host_id=50")==1,"clone rebinds both nested chain owners and device IDs exactly");
    check(scalar(f,"SELECT count(*) FROM plugin_params WHERE device_id=55 AND normalized_value=0.75 "
                   "AND display='3 dB'")==1 &&
          scalar(f,"SELECT count(*) FROM plugin_state WHERE device_id=55 AND format_hint='opaque'")==1 &&
          scalar(f,"SELECT count(*) FROM device_expression_routes WHERE device_id=55 AND route='mpe_midi'")==1 &&
          scalar(f,"SELECT count(*) FROM device_panel_params WHERE device_id=55 AND ord=4 AND param_id='gain'")==1 &&
          scalar(f,"SELECT count(*) FROM state_blobs")==1 && scalar(f,"SELECT count(*) FROM plugin_refs")==2,
          "nested data is copied from the captured state and pooled references are shared");
    check(scalar(f,"SELECT count(*) FROM macro_mappings WHERE id>68 AND target_device_id=55 "
                   "AND macro_id IN (SELECT id FROM macros WHERE id>99 AND device_id IN (54,100))")==2 &&
          scalar(f,"SELECT count(*) FROM macro_mappings WHERE id<=68")==4 &&
          scalar(f,"SELECT count(*) FROM routing r JOIN routing_key_taps t ON r.id=t.routing_id "
                   "WHERE r.id>72 AND r.src_kind='device' AND r.src_id=54 AND r.dst_id=55 "
                   "AND r.channel_map IS NULL AND t.tap='pre_fx'")==1 &&
          scalar(f,"SELECT count(*) FROM routing")==3 &&
          scalar(f,"SELECT count(*) FROM automation_lanes")==1 && scalar(f,"SELECT count(*) FROM controller_maps")==1,
          "internal mappings and taps get fresh IDs and external relationships stay on the source");
    auto history=History(*f.store);
    check(history.undo().ok && digestProject(*f.store).text==before &&
          history.redo().ok && digestProject(*f.store).text==after &&
          history.undo().ok && digestProject(*f.store).text==before &&
          history.redo().ok && digestProject(*f.store).text==after,
          "one Undo and Redo reproduce both exact nested rack placements repeatedly");
}

void rackPasteAfterCut() {
    Fixture f;
    seedRack(f);
    emptyTrack(f);
    f.store->db().exec("INSERT INTO devices(id,chain_id,name) VALUES(54,22,'Last descendant before Cut')");
    const auto copy=readDeviceCopy(*f.store,40);
    OpRequest remove;
    remove.opType="device.remove";
    remove.payload={{"id",40}};
    check(OpJournal(*f.store).commit(remove).ok,"remove the original captured rack before pasting");
    const auto before=digestProject(*f.store).text;
    check(!prepareDevicePasteForTrack(*f.store,copy,40,3).ok &&
          !prepareDevicePasteForTrack(*f.store,copy,42,3).ok &&
          !prepareDevicePasteForTrack(*f.store,copy,2,3).ok,
          "root paste cannot reuse any original device ID released by Cut");
    const auto automatic = prepareDevicePasteForTrackWithFreshId(*f.store, copy, 3);
    check(automatic.ok && automatic.requests.back().payload.at("id") == 55,
          "engine root allocation skips the next live ID when it is a released descendant");
    const auto prepared=prepareDevicePasteForTrack(*f.store,copy,std::numeric_limits<std::int64_t>::max(),3);
    check(prepared.ok && prepared.chainId==23 && prepared.requests.size()==2,
          "empty-track paste skips the cut rack's chain IDs, including the staged chain");
    if (!prepared.ok) throw std::runtime_error(prepared.error);
    const auto applied=commitDevicePaste(*f.store,prepared);
    if (!applied.ok) throw std::runtime_error(applied.error);
    const auto after=digestProject(*f.store).text;
    check(scalar(f,"SELECT count(*) FROM devices WHERE id=9223372036854775807 AND chain_id=23")==1 &&
          scalar(f,"SELECT count(*) FROM devices WHERE id IN (40,2,42,54)")==0 &&
          scalar(f,"SELECT count(*) FROM device_chains WHERE id IN (21,22)")==0 &&
          scalar(f,"SELECT count(*) FROM macros WHERE id IN (9,99)")==0 &&
          scalar(f,"SELECT count(*) FROM macro_mappings WHERE id IN (65,68)")==0 &&
          scalar(f,"SELECT count(*) FROM routing WHERE id IN (71,72)")==0,
          "the full-width explicit root does not consume fresh IDs for the Cut descendants");
    check(History(*f.store).undo().ok && digestProject(*f.store).text==before &&
          History(*f.store).redo().ok && digestProject(*f.store).text==after,
          "staged chain and nested paste are one exact Undo step after Cut");
}

void rackPasteGuards() {
    {
        Fixture f;
        seedRack(f);
        f.store->db().exec("INSERT INTO device_chains(id,parent_device_id) VALUES(150,51)");
        const auto result = prepareDevicePasteWithFreshId(*f.store, readDeviceCopy(*f.store, 40), 150);
        check(!result.ok && result.error.find("not a rack") != std::string::npos,
              "a fresh rack cannot be pasted into an unreachable child chain under an ordinary device");
    }
    {
        Fixture f;
        seedRack(f);
        const auto copy=readDeviceCopy(*f.store,40);
        f.store->db().exec("UPDATE plugin_refs SET uid='different' WHERE id=31");
        check(!prepareDevicePaste(*f.store,copy,70,20).ok,
              "nested reference identity is checked against the original clipboard");
        f.store->db().exec("UPDATE plugin_refs SET uid='test.nested',name='Relabelled' WHERE id=31");
        const auto prepared=prepareDevicePaste(*f.store,copy,70,20);
        check(prepared.ok,"nested reference relabelling before preparation retains its identity");
        f.store->db().exec("UPDATE plugin_refs SET uid='same-head-change' WHERE id=31");
        staleTrackPacket(f,prepared,"nested reference reuse after preparation stales a direct paste");
    }
    {
        Fixture f;
        seedRack(f);
        const auto prepared=prepareDevicePaste(*f.store,readDeviceCopy(*f.store,40),70,20);
        f.store->db().exec("INSERT INTO devices(id,chain_id,name) VALUES(54,10,'Same-head occupied descendant')");
        staleTrackPacket(f,prepared,"same-head occupied descendant prevents any rack paste mutation");
    }
    {
        Fixture f;
        seedRack(f);
        const auto copy=readDeviceCopy(*f.store,40);
        f.store->db().exec("UPDATE remote_hosts SET host='changed-before-prepare' WHERE id=50");
        check(!prepareDevicePaste(*f.store,copy,70,20).ok,"nested remote identity protects preparation");
        f.store->db().exec("UPDATE remote_hosts SET host='localhost',name='Relabelled' WHERE id=50");
        const auto prepared=prepareDevicePaste(*f.store,copy,70,20);
        check(prepared.ok,"nested remote endpoint relabelling retains its captured identity");
        f.store->db().exec("UPDATE remote_hosts SET host='same-head-change' WHERE id=50");
        const auto before=digestProject(*f.store).text;
        const auto refused=OpJournal(*f.store).commit(prepared.requests);
        check(!refused.ok && refused.stale && before==digestProject(*f.store).text,
              "plain journal submission also rejects a same-head changed nested remote endpoint");
    }
    for (const char* statement : {
            "INSERT INTO macros(id,device_id,ord,name) VALUES(100,51,5,'Same-head occupied macro')",
            "INSERT INTO macro_mappings(id,macro_id,target_device_id,target_param_ref) VALUES(69,12,51,'outside')"}) {
        Fixture f;
        seedRack(f);
        const auto prepared=prepareDevicePaste(*f.store,readDeviceCopy(*f.store,40),70,20);
        f.store->db().exec(statement);
        staleTrackPacket(f,prepared,"same-head occupied macro or mapping rejects transaction admission");
    }
    {
        Fixture f;
        seedRack(f);
        f.store->db().exec("UPDATE plugin_refs SET format='internal',uid='org.adidaw.mix.eq' WHERE id=31");
        const auto copy=readDeviceCopy(*f.store,40);
        const auto prepared=prepareDevicePaste(*f.store,copy,70,20);
        check(prepared.ok,"a rack with one Mix View function prepares for an empty channel slot");
        f.store->db().exec("INSERT INTO devices(id,chain_id,name,plugin_ref_id) VALUES(58,20,'EQ',31)");
        staleTrackPacket(f,prepared,"a same-head Mix View function insertion invalidates rack duplication");
        check(!prepareDevicePaste(*f.store,copy,70,20).ok,
              "rack copy respects the channel's already occupied Mix View function");
    }
    {
        Fixture f;
        seedRack(f);
        f.store->db().exec("INSERT INTO devices(id,chain_id,name) VALUES(9223372036854775807,20,'Full')");
        const auto before=digestProject(*f.store).text;
        const auto result=prepareDevicePaste(*f.store,readDeviceCopy(*f.store,40),70,20);
        check(!result.ok && result.error.find("exhausted")!=std::string::npos && before==digestProject(*f.store).text,
              "exhausted rack descendant IDs refuse without overflow or a partial order edit");
    }
}

void rackPasteSkipsRemarks() {
    Fixture f;
    seedRack(f);
    auto& db = f.store->db();
    db.exec("INSERT INTO remarks(target_kind,target_id,author,text,created_utc) "
            "VALUES('device',54,'user','Orphaned device remark',0)");
    const auto copy = readDeviceCopy(*f.store, 40);
    check(!prepareDevicePaste(*f.store, copy, 54, 20).ok, "rack paste does not occupy an explicitly requested orphan remark ID");
    const auto stale = prepareDevicePaste(*f.store, copy, 70, 20);
    check(stale.ok, "rack paste skips an existing remark when allocating children");
    db.exec("INSERT INTO remarks(target_kind,target_id,author,text,created_utc) "
            "VALUES('device',55,'user','Same-head orphan',0)");
    const auto before = digestProject(*f.store).text;
    check(!commitDevicePaste(*f.store, stale).ok && digestProject(*f.store).text == before,
          "a same-head orphan claim on a staged descendant refuses the whole paste");
    const auto current = prepareDevicePaste(*f.store, copy, 70, 20);
    check(current.ok && commitDevicePaste(*f.store, current).ok &&
              scalar(f, "SELECT count(*) FROM devices WHERE id IN (54,55)") == 0,
          "fresh rack descendants use the shared allocator and leave orphan remarks detached");
}

void rackGroupAndUngroup() {
    Fixture f;
    const auto original = digestProject(*f.store).text;
    const std::array<std::int64_t,2> split{51,53};
    check(!groupDevicesInRack(*f.store, split, "audio_effect").ok && digestProject(*f.store).text == original,
          "grouping refuses a split serial selection without moving anything");
    const std::array<std::int64_t,2> selected{52,51};
    const auto grouped = groupDevicesInRack(*f.store, selected, "audio_effect");
    check(grouped.ok && scalar(f, "SELECT count(DISTINCT txn_id) FROM ops") == 1,
          "grouping writes a rack, chain and moves in one transaction");
    if (!grouped.ok) throw std::runtime_error(grouped.error);
    const auto rack = scalar(f, "SELECT id FROM devices WHERE is_rack=1 AND chain_id=20");
    check(scalar(f, "SELECT count(*) FROM devices WHERE id=51 AND ord=0 AND chain_id<>20") == 1 &&
              scalar(f, "SELECT count(*) FROM devices WHERE id=52 AND ord=1 AND chain_id<>20") == 1 &&
              scalar(f, "SELECT ord FROM devices WHERE id=53") == 1,
          "grouping uses stored order for ties and retains the following device");
    const auto groupImage = digestProject(*f.store).text;
    History history(*f.store);
    check(history.undo().ok && digestProject(*f.store).text == original && history.redo().ok &&
              digestProject(*f.store).text == groupImage, "one Group Undo restores the original tied order and Redo restores both IDs");

    auto& db = f.store->db();
    SQLite::Statement add(db, "INSERT INTO device_chains(id,parent_device_id) VALUES(200,?)");
    add.bind(1, rack);
    add.exec();
    const auto ambiguous = digestProject(*f.store).text;
    check(!ungroupDeviceRack(*f.store, rack).ok && digestProject(*f.store).text == ambiguous,
          "Ungroup refuses an ambiguous parallel rack without changing it");
    db.exec("DELETE FROM device_chains WHERE id=200");
    const auto ungrouped = ungroupDeviceRack(*f.store, rack);
    check(ungrouped.ok && scalar(f, "SELECT count(DISTINCT txn_id) FROM ops") == 2 &&
              db.execAndGet("SELECT group_concat(id,',') FROM (SELECT id FROM devices WHERE chain_id=20 ORDER BY ord,id)").getString() == "51,52,53",
          "one Ungroup restores the serial relationships in the parent chain");
    if (!ungrouped.ok) throw std::runtime_error(ungrouped.error);
    const auto ungroupImage = digestProject(*f.store).text;
    check(history.undo().ok && digestProject(*f.store).text == groupImage && history.redo().ok &&
              digestProject(*f.store).text == ungroupImage, "one Ungroup Undo and Redo restore both exact layouts");
}
}
int main() {
    try {
        reads();
        pasteAndUndo();
        deviceRolePlacement();
        refusals();
        reusedReferenceIds();
        pasteToEmptyTrack();
        appendToTrackAndSubmitPacket();
        topLevelTrackChainChoice();
        fullWidthTrackChainIdentity();
        trackPasteGuards();
        trackPasteAtomicRefusals();
        rangePaste();
        rangePasteRoles();
        rangePasteAtomicRefusals();
        rackPasteRoundTrip();
        rackPasteAfterCut();
        rackPasteGuards();
        rackPasteSkipsRemarks();
        rackGroupAndUngroup();
    } catch (const std::exception &e) {
        check(false, e.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
