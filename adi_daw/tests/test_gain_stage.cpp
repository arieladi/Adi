// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/session.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/store.hpp"
#include "adi/history.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>
using namespace adi;
using namespace adi::engine;
namespace {
int checks=0, failures=0;
void check(bool ok,const char* name) { ++checks; if(!ok) { ++failures; std::printf("FAIL %s\n",name); } }
struct Synth : device::DeviceInstance {
    device::DeviceIdentity id;
    bool active=false;
    const device::DeviceIdentity& identity() const noexcept override { return id; }
    bool loaded() const noexcept override { return true; }
    EventFlow eventFlow() const noexcept override { return EventFlow::Consume; }
    void process(const NodeIo& io) noexcept override {
        for(const auto& e : io.events) {
            if(e.type==EventType::NoteOn) active=true;
            if(e.type==EventType::NoteOff) active=false;
        }
        for(int i=0;i<io.frames;++i) for(int c=0;c<io.channels;++c)
            io.out[c][io.blockOffset+i]=static_cast<float>((active ? 0.05 : 0.0)*std::sin(2*std::numbers::pi*1000*
                static_cast<double>(io.transport->timelineSample+io.blockOffset+i)/48000.0));
    }
};
struct Fixture {
    test::TempDirectory dir{"gain_stage","project"};
    std::unique_ptr<Store> store;
    Session session;
    explicit Fixture(int block=512,double amplitude=0.1,bool synth=true,bool silence=false) {
        StoreError error{}; store=Store::create(dir.path()/"stage.adi",error);
        if(!store) throw std::runtime_error("store");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
            "(1,'audio','Audio',0),(2,'midi','Instrument',1),(3,'group','Group',2),(9,'master','Master',3);"
            "INSERT INTO mixer_strip(track_id,input_gain_db) VALUES(1,6),(2,-3),(3,2);"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','audio.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,2000000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,96000);");
        if(synth) store->db().exec("INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','synth','Synth','instrument');"
            "INSERT INTO device_chains(id,track_id) VALUES(1,2);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Synth');");
        if(synth) {
            store->db().exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(2,2,'midi',0,0,23063040);");
            NoteRecord note{}; note.start_ticks=0; note.dur_ticks=23063040; note.note_id=1;
            note.key=60; note.vel_on=127; note.probability=10000;
            if(!store->putEventStream(2,"notes",writeStream<NoteRecord>(FourCC::Notes,std::vector<NoteRecord>{note})))
                throw std::runtime_error("notes");
        }
        std::vector<float> samples(192000);
        for(std::size_t i=0;i<96000;++i) samples[2*i]=samples[2*i+1]=silence ? 0.0f :
            static_cast<float>(amplitude*std::sin(2*std::numbers::pi*1000*static_cast<double>(i)/48000));
        audio::WavWriter wav(dir.path()/"audio.wav",48000,2,audio::WavFormat::Float32);
        wav.write(samples.data(),96000); wav.close();
        session.graph().setFadeFrames(0);
        if(!session.load(*store,[](const DeviceRequest&,std::string&) { return std::make_unique<Synth>(); },{2,48000,block}))
            throw std::runtime_error(session.error());
    }
    double gain(int id) { SQLite::Statement q(store->db(),"SELECT input_gain_db FROM mixer_strip WHERE track_id=?");q.bind(1,id);q.executeStep();return q.getColumn(0).getDouble(); }
};
void transaction() {
    Fixture f;
    f.session.transport().locate(1234); f.session.transport().play();
    auto r=f.session.autoGainStage(*f.store);
    if(!r.commit.ok) std::printf("gain-stage error: %s\n",r.commit.error.c_str());
    check(r.commit.ok && r.commit.seqs.size()==2,"one batch stages audio and instrument");
    if(!r.commit.ok || r.tracks.size()!=2) return;
    check(std::abs(r.tracks[0].measuredDb-(-23.0102999566))<0.001,"clips measured before existing input gain");
    check(std::abs(r.tracks[1].measuredDb-(-29.0308998699))<0.001,"instrument is rendered offline");
    check(std::abs(f.gain(1)-5.0102999566)<0.001 && std::abs(f.gain(2)-11.0308998699)<0.001,"default -18 dBFS RMS absolute gains");
    check(f.gain(3)==2,"groups not staged");
    check(f.session.transport().position()==1234 && f.session.transport().playing(),"live transport untouched");
    const auto logs=OpJournal(*f.store).recent();
    check(logs.size()==2 && logs[0].txnId==logs[1].txnId && logs[0].opType=="mixer.setInputGain","only existing gain ops in one transaction");
    check(History(*f.store).undo().ok && f.gain(1)==6 && f.gain(2)==-3,"one undo restores both original gains");
    const auto again=f.session.autoGainStage(*f.store);
    check(again.commit.ok && again.tracks[0].gainDb==r.tracks[0].gainDb && again.tracks[1].gainDb==r.tracks[1].gainDb,"repeat is deterministic");
}
struct Square : Synth {
    EventFlow eventFlow() const noexcept override { return EventFlow::Through; }
    void process(const NodeIo& io) noexcept override {
        for(int c=0;c<io.channels;++c) for(int i=0;i<io.frames;++i) {
            const float v=io.in[c][io.blockOffset+i]; io.out[c][io.blockOffset+i]=v*v;
        }
    }
};
void placement() {
    Fixture f;
    f.store->db().exec("INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(2,'vst3','square','Square','effect');"
        "INSERT INTO device_chains(id,track_id) VALUES(2,1);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(2,2,0,2,'Square');");
    Session s; s.graph().setFadeFrames(0);
    check(s.load(*f.store,[](const DeviceRequest& rq,std::string&) -> std::unique_ptr<device::DeviceInstance> {
        if(rq.ref->subtype=="instrument") return std::make_unique<Synth>();
        return std::make_unique<Square>();
    },{2,48000,512}),"gain-placement project loads");
    auto tap=s.openScope(1,1,ScopePoint::PreFader);
    auto instrument=s.openScope(2,1,ScopePoint::PreFader); s.transport().play();
    std::array<float,512> l{},r{};float* output[]{l.data(),r.data()};
    check(s.clips()->prime(),"gain-placement media ready");
    s.process({output,nullptr,2,0,512,0});std::int64_t stamp=0;
    check(tap->read(l,r,stamp),"gain-placement tap readable");
    check(std::abs(l[12]-0.01*std::pow(10.0,6.0/10.0))<1e-6,"input gain precedes nonlinear audio insert");
    check(instrument->read(l,r,stamp) && std::abs(l[12]-0.05*std::pow(10.0,-3.0/20.0))<1e-6,
          "instrument input gain follows its audio generation");
}
void modes() {
    Fixture small(32),large(4096);
    auto a=small.session.autoGainStage(*small.store),b=large.session.autoGainStage(*large.store);
    check(a.commit.ok && b.commit.ok && a.tracks[0].gainDb==b.tracks[0].gainDb && a.tracks[1].gainDb==b.tracks[1].gainDb,"block-size independent measurements");
    GainStageOptions o; o.measure=GainStageMeasure::IntegratedLufs;
    auto lufs=large.session.autoGainStage(*large.store,o);
    // Stereo 1 kHz -20 dBFS peak: standard K weighting gives about -20 LUFS.
    check(lufs.commit.ok && std::abs(lufs.tracks[0].measuredDb+20)<0.1,"LUFS-I K weighting and stereo calibration");
    o.measure=GainStageMeasure::GatedRms; o.targetDb=0;
    auto limited=large.session.autoGainStage(*large.store,o);
    check(limited.commit.ok && limited.tracks[0].ceilingLimited &&
          limited.tracks[0].gainDb+limited.tracks[0].peakDbTP<=-1+1e-10,"true peak overrides loudness target");
    Fixture gated(4096,0.1,false);
    GainStageOptions range; range.endSample=192000;
    auto g=gated.session.autoGainStage(*gated.store,range);
    check(g.commit.ok && std::abs(g.tracks[0].measuredDb+23.0103)<0.4,"silence gated out rather than lowering RMS by 3 dB");
    gated.store->db().exec("UPDATE clips SET gain_db=-6");
    auto clipGain=gated.session.autoGainStage(*gated.store);
    check(clipGain.commit.ok && std::abs(clipGain.tracks[0].measuredDb+29.0103)<0.001,"clip gain included in analysis");
    Fixture muted(4096);
    muted.store->db().exec("UPDATE clips SET muted=1 WHERE id=2");
    auto noNotes=muted.session.autoGainStage(*muted.store);
    check(noNotes.commit.ok && noNotes.tracks[1].silent && muted.gain(2)==-3,"instrument sound comes from MIDI clip notes");
    Fixture silent(512,0.1,false,true);
    auto s=silent.session.autoGainStage(*silent.store);
    check(s.commit.ok && s.commit.seqs.empty() && silent.gain(1)==6,"silent tracks unchanged without empty undo entry");
    o.targetDb=std::numeric_limits<double>::quiet_NaN();
    check(!silent.session.autoGainStage(*silent.store,o).commit.ok,"nonfinite target rejected");
    Fixture missing;
    missing.store->db().exec("UPDATE media_files SET rel_path='missing.wav'");
    check(!missing.session.autoGainStage(*missing.store).commit.ok && missing.gain(1)==6 && missing.gain(2)==-3,
          "missing media fails atomically");
}
}
int main(int argc, char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==2 && std::string(argv[1])=="--placement") placement();
    else { transaction(); modes(); placement(); }
    std::printf("%s -- %d checks, %d failures\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
