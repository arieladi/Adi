// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <sstream>
#include <thread>

using namespace adi;
using namespace adi::engine;
namespace { std::atomic<unsigned> allocations{0}; }
void* operator new(std::size_t n) {
    if (audio::onAudioThread) ++allocations;
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", label); }
}
struct Source final : Node {
    bool pulse = false, silent = false;
    void process(const NodeIo& io) noexcept override {
        for (int c=0;c<io.channels;++c) for (int i=0;i<io.frames;++i) {
            const auto at = io.transport->timelineSample + io.blockOffset + i;
            io.out[c][io.blockOffset+i] = silent ? 0.0f : pulse ? (at==256 ? 1.0f : 0.0f) : 0.25f;
        }
    }
};
struct Effect final : device::DeviceInstance {
    device::DeviceIdentity id;
    float gain = 2;
    int delay = 0;
    bool instrument = false;
    std::array<std::array<float,256>,2> history{};
    int cursor = 0;
    const device::DeviceIdentity& identity() const noexcept override { return id; }
    bool loaded() const noexcept override { return true; }
    std::int32_t latencySamples() const noexcept override { return delay; }
    std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
    EventFlow eventFlow() const noexcept override { return instrument ? EventFlow::Consume : EventFlow::Through; }
    void process(const NodeIo& io) noexcept override {
        for (int i=0;i<io.frames;++i) {
            for (int c=0;c<io.channels;++c) {
                const float value = instrument ? 0.375f : io.in ? io.in[c][io.blockOffset+i] : 0;
                float delayed = value;
                if (delay) {
                    auto& ring = history[static_cast<std::size_t>(c)];
                    delayed = ring[static_cast<std::size_t>(cursor)];
                    ring[static_cast<std::size_t>(cursor)] = value;
                }
                io.out[c][io.blockOffset+i] = delayed * gain;
            }
            if (delay) cursor = (cursor+1)%delay;
        }
    }
};
struct Fixture {
    test::TempDirectory dir{"analyser_taps","session"};
    std::unique_ptr<Store> store;
    Source a,b;
    Session session;
    bool delays = false;
    Fixture(bool inserts=true, bool instrument=false) {
        StoreError error{};
        store = Store::create(dir.path()/"taps.adi",error);
        if (!store) throw std::runtime_error("fixture store");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
                        "(1,'audio','A',0),(2,'audio','B',1),(9,'master','Master',2);");
        if (inserts) store->db().exec(
            "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','gain','Gain','effect');"
            "INSERT INTO device_chains(id,track_id) VALUES(1,1),(2,2);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,1,1,'Gain A'),(2,2,0,1,'Gain B');");
        if (instrument) store->db().exec(
            "UPDATE tracks SET kind='midi' WHERE id=1;"
            "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(2,'vst3','synth','Synth','instrument');"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(3,1,0,2,'Synth');");
        session.setSourcesFor([this](std::int64_t id) {
            return id==1 ? std::vector<Node*>{&a} : id==2 ? std::vector<Node*>{&b} : std::vector<Node*>{};
        });
        session.graph().setFadeFrames(0);
    }
    void load() {
        if (!session.load(*store,[this](const DeviceRequest& rq,std::string&) {
            auto effect = std::make_unique<Effect>();
            effect->instrument = rq.device->id==3;
            effect->gain = effect->instrument ? 1.0f : rq.device->id==1 ? 2.0f : 0.5f;
            effect->delay = delays ? (rq.device->id==1 ? 32 : 96) : 0;
            return effect;
        },{2,48000,512})) throw std::runtime_error(session.error());
        session.transport().play();
    }
    void render(int frames=512) {
        std::array<float,1024> out{};
        float* channels[]={out.data(),out.data()+512};
        session.process({channels,nullptr,2,0,frames,0});
    }
};
struct Window {
    std::array<float,512> l{},r{};
    std::int64_t stamp=0;
    explicit Window(const std::shared_ptr<const ScopeTap>& tap) {
        if (!tap || !tap->read(l,r,stamp)) throw std::runtime_error("tap read");
    }
    std::int64_t peakStamp() const {
        return stamp + std::distance(l.begin(),std::max_element(l.begin(),l.end()));
    }
};
constexpr std::array<ScopePoint,3> points{ScopePoint::PostFader,ScopePoint::PreFader,ScopePoint::ChainInput};
void gainAndFallback() {
    for (bool inserts : {false,true}) {
        Fixture f(inserts);
        // -6.020599913 dB is a half-amplitude fader, independent of inserts.
        f.store->db().exec("INSERT INTO mixer_strip(track_id,volume_db) VALUES(1,-6.020599913);");
        f.load();
        const auto pre=f.session.openScope(1,1,ScopePoint::ChainInput);
        const auto post=f.session.openScope(1,1,ScopePoint::PreFader);
        const auto scope=f.session.openScope(1,1);
        check(pre && post && scope && pre!=post && post!=scope,"points have separate per-track rings");
        check(pre->capacity()==48000 && f.session.openScope(1,1,ScopePoint::ChainInput)==pre,"caller selects one-second ring; reopening reuses it");
        f.render();
        const Window a(pre),b(post),c(scope);
        bool exact=true;
        for(std::size_t i=0;i<a.l.size();++i) exact=exact && a.l[i]==0.25f && b.l[i]==a.l[i]*(inserts?2.0f:1.0f);
        check(exact,"ChainInput to PreFader is exactly insert gain, unity with no inserts");
        check(std::abs(c.l[0]-b.l[0]*0.5f)<1e-6f,"PreFader excludes the fader; legacy PostFader includes it");
        check(a.stamp==b.stamp && c.stamp==b.stamp,"zero-latency points share heard stamps");
        for(auto point:points) f.session.closeScope(1,point);
        const auto n=pre->written(),m=post->written(),k=scope->written();
        f.render();
        check(pre->written()==n && post->written()==m && scope->written()==k,"closed taps copy zero frames");
    }
    Fixture synth(true,true); synth.load();
    const auto a=synth.session.openScope(1,1,ScopePoint::ChainInput);
    const auto b=synth.session.openScope(1,1,ScopePoint::PreFader);
    synth.render();
    check(Window(a).l[0]==0.375f && Window(b).l[0]==0.75f,"instrument track taps after instrument and before first effect");
    // Removing the effect must move ChainInput onto the strip, and closing it
    // must detach the newly selected node as well as the previous generation.
    synth.store->db().exec("DELETE FROM devices WHERE id=1");
    check(synth.session.refresh(*synth.store),"chain edit rebuilds");
    synth.render();
    check(Window(a).l[0]==0.375f && Window(b).l[0]==0.375f,"instrument-only chain falls back to PreFader audio");
    synth.session.closeScope(1,ScopePoint::ChainInput);
    const auto before=a->written(); synth.render();
    check(a->written()==before,"close detaches retargeted ChainInput");
}
void stamps() {
    Fixture f; f.delays=true; f.a.pulse=true; f.b.pulse=true; f.load();
    const auto a=f.session.openScope(1,1,ScopePoint::PreFader);
    const auto b=f.session.openScope(2,1,ScopePoint::PreFader);
    const auto ca=f.session.openScope(1,1,ScopePoint::ChainInput);
    const auto cb=f.session.openScope(2,1,ScopePoint::ChainInput);
    f.render();
    const Window wa(a),wb(b);
    check(a->latency()==32 && b->latency()==96,"PreFader has each insert's arrival latency");
    check(wa.stamp==-32 && wb.stamp==-96,"window stamps subtract different arrivals");
    check(wa.peakStamp()==256 && wb.peakStamp()==256,"two delayed tracks align their actual impulse by heard stamp");
    check(Window(ca).peakStamp()==256 && Window(cb).peakStamp()==256 && ca->latency()==0 && cb->latency()==0,"ChainInput excludes downstream insert latency");
    Fixture shifted(false);
    shifted.store->db().exec("INSERT INTO mixer_strip(track_id,delay_samples) VALUES(1,100);");
    shifted.load();
    const auto pre=shifted.session.openScope(1,1,ScopePoint::PreFader);
    const auto post=shifted.session.openScope(1,1);
    check(pre->latency()==0 && post->latency()==-100,"strip's own latency belongs only to PostFader");
    Fixture instrument(true,true); instrument.delays=true; instrument.load();
    const auto input=instrument.session.openScope(1,1,ScopePoint::ChainInput);
    const auto output=instrument.session.openScope(1,1,ScopePoint::PreFader);
    instrument.render();
    check(input->latency()==96 && output->latency()==128,"instrument arrival is included before the effect's own latency");
    check(Window(input).stamp==-96 && Window(output).stamp==-128,"nonzero ChainInput arrival is used in actual write stamps");
    check(Window(input).l[96]==0.375f && Window(output).l[128]==0.75f,"delayed instrument audio reaches both expected points");
}
void silenceAndConcurrentClose() {
    Fixture f(false); f.load();
    const auto pre=f.session.openScope(1,1,ScopePoint::PreFader);
    const auto chain=f.session.openScope(1,1,ScopePoint::ChainInput);
    f.render(); f.a.silent=true; f.render();
    check(pre->written()==1024 && chain->written()==1024,"suspended silent strip keeps open rings current");
    check(Window(pre).l[0]==0 && Window(chain).l[0]==0,"silent taps replace previously audible samples");
    std::atomic<bool> stop{false};
    std::atomic<unsigned> blocks{0};
    std::thread renderer([&] { while(!stop.load(std::memory_order_acquire)) { f.render(32); ++blocks; } });
    bool reused=true,quiet=true;
    try {
        // First-time publication on track 2 races rendering as well as reuse
        // on track 1. Readers may run concurrently, but never the writer.
        for(int i=0;i<40;++i) for(auto point:points) {
            const auto tap=f.session.openScope(i%2+1,1,point);
            reused=reused && tap==f.session.openScope(i%2+1,1,point);
            f.session.closeScope(i%2+1,point);
            auto settled=blocks.load();
            while(blocks.load()-settled<2) std::this_thread::yield();
            const auto written=tap->written(); settled=blocks.load();
            while(blocks.load()-settled<2) std::this_thread::yield();
            quiet=quiet && tap->written()==written;
        }
    } catch(...) { stop.store(true); renderer.join(); throw; }
    stop.store(true); renderer.join();
    check(reused && blocks.load()>0,"opening and closing all points while rendering preserves tap lifetime");
    check(quiet,"closed taps stop writing after the in-flight callback");
}
} // namespace
int main() {
    try {
        gainAndFallback(); stamps(); silenceAndConcurrentClose();
        check(allocations.load()==0,"zero allocations in audio process");
        check(audio::callbackFileIo.load()==0,"zero callback file I/O");
    } catch(const std::exception& e) { ++failures; std::printf("FAIL exception: %s\n",e.what()); }
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}

