// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/digest.hpp"
#include "adi/freeze_fingerprint.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <functional>
#include <stdexcept>
namespace {
using namespace adi;
int checks = 0, failures = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
struct Project {
    test::TempDirectory directory{"freeze_fingerprint", "project"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};
        store = Store::create(directory.path() / "source.adi", error);
        if (!store)
            throw std::runtime_error(toString(error));
        store->db().exec(
            "INSERT INTO tracks(id,kind,name,parent_id,index_in_parent,color) "
            "VALUES(10,'group','Group',NULL,0,NULL),(1,'midi','Source',10,0,6),(2,'audio','Key',10,1,NULL),("
            "9,'master','Main',NULL,1,NULL);INSERT INTO mixer_strip(track_id,volume_db,pan,input_gain_db) "
            "VALUES(1,-3,0.25,6),(2,0,0,0),(10,-6,0,0);INSERT INTO tempo_map(id,pos_ticks,bpm) "
            "VALUES(1,0,120);INSERT INTO "
            "media_files(id,hash_blake3,rel_path,sample_rate,channels,frames,format,missing) "
            "VALUES(8,'source-hash','source.wav',48000,2,3000,'wav',0);INSERT INTO "
            "clips(id,track_id,kind,name,time_base,pos_ns,length_ns) VALUES(1,1,'audio','Source "
            "Clip',1,0,62500000),(2,2,'audio','Key Clip',1,0,62500000);INSERT INTO "
            "audio_clips(clip_id,media_id,src_start_frames,src_len_frames) "
            "VALUES(1,8,0,3000),(2,8,0,3000);INSERT INTO plugin_refs(id,format,uid,name,version,subtype) "
            "VALUES(1,'vst3','test.midi','MIDI "
            "FX','1','effect'),(2,'vst3','test.synth','Synth','1','instrument'),(3,'vst3','test.fx','FX','1',"
            "'effect');INSERT INTO device_chains(id,track_id) VALUES(1,1);INSERT INTO "
            "devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'MIDI "
            "FX'),(2,1,1,2,'Synth'),(3,1,2,3,'FX');INSERT INTO state_blobs(hash_blake3,data,size_bytes) "
            "VALUES('state',x'0102',2),('new-state',x'0304',2);INSERT INTO "
            "plugin_state(device_id,stream_role,state_hash) VALUES(2,'component','state');INSERT INTO "
            "plugin_params(device_id,param_id,name,normalized_value) "
            "VALUES(3,'gain','Gain',0.25);CREATE TABLE future_source(id INTEGER PRIMARY KEY,track_id INTEGER "
            "REFERENCES tracks(id) ON DELETE RESTRICT,tone REAL,shape BLOB) STRICT;INSERT INTO future_source "
            "VALUES(1,1,1.25,x'');");
    }
    freeze::FingerprintSpec spec() const {
        freeze::FingerprintSpec spec;
        spec.root = 1;
        return spec;
    }
    void mutation(const freeze::FingerprintSpec &spec, const std::string &sql, bool different,
                  const char *message) {
        const auto before = freeze::fingerprint(*store, spec).hash;
        store->db().exec("SAVEPOINT fingerprint_mutation");
        store->db().exec(sql);
        const auto after = freeze::fingerprint(*store, spec).hash;
        check((before != after) == different, message);
        store->db().exec("ROLLBACK TO fingerprint_mutation;RELEASE fingerprint_mutation");
    }
};
void fields() {
    Project p;
    const auto spec = p.spec();
    const auto state = digestProject(*p.store).text;
    const auto fingerprint = freeze::fingerprint(*p.store, spec);
    check(fingerprint.hash.size() == 64 && fingerprint.tracks == std::vector<std::int64_t>({1}) &&
              fingerprint.dependencies.empty(),
          "source scope has stable content hash and excludes downstream parent/Main");
    check(digestProject(*p.store).text == state,
          "fingerprint capture is read-only even with a restrictive future child");
    bool typed = false;
    for (const auto &row : fingerprint.source.at("rows"))
        if (row.at("table") == "future_source")
            typed =
                row.at("value").at("shape").is_binary() && row.at("value").at("shape").get_binary().empty();
    check(typed, "read-only causal rows preserve empty BLOB storage and future restrictive-FK data");
    p.mutation(spec,
               "UPDATE tracks SET name='Renamed',color=0,index_in_parent=3,muted=1,soloed=1 WHERE "
               "id=1;UPDATE project SET name='Renamed project',notes='A note',author='A person'",
               false, "names, colors, UI order and source live mute/solo do not invalidate cache");
    p.mutation(spec,
               "UPDATE mixer_strip SET volume_db=-12,pan=-0.5,width=0.5,phase_invert=1,delay_samples=19 "
               "WHERE track_id=1",
               false, "root post-effect fader/pan/width/phase/delay remain live");
    p.mutation(spec, "UPDATE mixer_strip SET input_gain_db=0 WHERE track_id=1", true,
               "baked input gain changes the source fingerprint");
    p.mutation(spec, "UPDATE clips SET gain_db=3 WHERE id=1", true, "clip gain is causal source state");
    p.mutation(spec, "UPDATE audio_clips SET src_start_frames=1 WHERE clip_id=1", true,
               "audio source-window edits invalidate cached material");
    p.mutation(spec, "UPDATE plugin_params SET normalized_value=0.75 WHERE device_id=3", true,
               "stored device parameter changes invalidate the frozen chain");
    p.mutation(spec, "UPDATE plugin_state SET state_hash='new-state' WHERE device_id=2", true,
               "opaque pooled state identity changes invalidate the source");
    p.mutation(spec, "UPDATE plugin_refs SET version='2' WHERE id=2", true,
               "stored plugin version identity participates");
    p.mutation(spec, "UPDATE devices SET ord=4 WHERE id=3", true, "device topology/order participates");
    p.mutation(spec, "UPDATE tempo_map SET bpm=97 WHERE id=1", true,
               "tempo context participates in source timing");
    p.mutation(spec, "UPDATE future_source SET tone=1.5 WHERE id=1", true,
               "future causal columns remain covered");
    p.mutation(spec, "UPDATE future_source SET shape=NULL WHERE id=1", true,
               "empty BLOB differs from NULL in canonical source state");
    p.mutation(spec, "UPDATE clips SET gain_db=30 WHERE id=2", false,
               "unrelated track material is outside the source scope");
    p.mutation(spec,
               "UPDATE media_files SET "
               "rel_path='relocated.wav',abs_path_hint='other.wav',missing=1,peaks=x'010203',orig_name='"
               "Renamed' WHERE id=8",
               false,
               "storage relocation/missing hints/peak display cache preserve immutable media identity");
    p.mutation(spec,
               "INSERT INTO frozen_renders VALUES(1,8,8,3000,48000,2,'arbitrary-source',x'1234',0);UPDATE "
               "tracks SET "
               "frozen=1,freeze_media_id=8,record_armed=1,locked=1 WHERE id=1",
               false, "own frozen metadata and parked flags do not recursively change the fingerprint");
}
void dependencies() {
    Project p;
    auto wet = p.spec();
    p.store->db().exec("INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) "
                       "VALUES(20,'track',2,'track',1,'sidechain')");
    const auto full = freeze::fingerprint(*p.store, wet);
    check(full.dependencies == std::vector<std::int64_t>({2}),
          "wet source fingerprints actual upstream sidechain context");
    p.mutation(wet, "UPDATE mixer_strip SET volume_db=-9 WHERE track_id=2", true,
               "baked key-track level participates in a wet fingerprint");
    auto prefix = wet;
    prefix.prefixOnly = true;
    // Track sidechains enter the chain head. Only an empty baked prefix
    // can retain that key entirely in its live suffix.
    prefix.prefixDevices = {};
    check(freeze::fingerprint(*p.store, prefix).dependencies.empty(),
          "source-prefix cache keeps external audio-FX sidechain inputs live");
    p.mutation(prefix,
               "UPDATE mixer_strip SET volume_db=-9 WHERE track_id=2;UPDATE clips SET gain_db=12 WHERE id=2",
               false, "live key signal can change without changing cached source prefix");
    p.mutation(prefix, "UPDATE plugin_state SET state_hash='new-state' WHERE device_id=2", true,
               "instrument prefix state remains protected");
    prefix.prefixDevices = {1, 2};
    bool refused = false;
    try {
        (void)freeze::fingerprint(*p.store, prefix);
    } catch (const std::exception &) {
        refused = true;
    }
    check(refused, "a sidechain that drives the cached instrument prefix refuses unsafe capture");
    p.store->db().exec("UPDATE routing SET dst_kind='device',dst_id=3 WHERE id=20");
    refused = false;
    try {
        (void)freeze::fingerprint(*p.store, wet);
    } catch (const std::exception &) {
        refused = true;
    }
    check(refused, "unsupported endpoint routing cannot silently lose a fingerprint dependency");
}
void groups() {
    Project p;
    freeze::FingerprintSpec spec;
    spec.root = 10;
    spec.subtree = true;
    const auto group = freeze::fingerprint(*p.store, spec);
    check(group.tracks == std::vector<std::int64_t>({1, 2, 10}),
          "whole-group scope includes every descendant");
    p.mutation(spec, "UPDATE mixer_strip SET volume_db=-12,pan=0.75 WHERE track_id=10", false,
               "whole group fader and pan stay live");
    p.mutation(spec, "UPDATE mixer_strip SET volume_db=-12 WHERE track_id=1", true,
               "a child's baked balance participates in whole-group cache");
    p.mutation(spec, "UPDATE tracks SET record_armed=1,frozen=1,freeze_media_id=8 WHERE id IN(1,2,10)", false,
               "parking/disarming group members does not alter their musical fingerprint");
    p.mutation(
        spec, "INSERT INTO tracks(id,kind,name,parent_id,index_in_parent) VALUES(3,'audio','New child',10,2)",
        true, "new group membership invalidates an old bus render");
}
} // namespace
int main() {
    try {
        fields();
        dependencies();
        groups();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("freeze fingerprint: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
