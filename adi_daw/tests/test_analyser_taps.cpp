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
#include <limits>
#include <stdexcept>
#include <sstream>
#include <thread>

using namespace adi;
using namespace adi::engine;
namespace { std::atomic<unsigned> allocations{0}, deallocations{0}, callbackLatencyQueries{0}; }
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
void operator delete(void* p) noexcept { if (p && audio::onAudioThread) ++deallocations; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", label); }
}
struct Source final : Node {
    bool pulse = false, silent = false;
    std::atomic<bool> stall{false}, entered{false}, resume{false};
    void process(const NodeIo& io) noexcept override {
        if (stall.load()) {
            entered.store(true, std::memory_order_release);
            while (!resume.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        for (int c=0;c<io.channels;++c) for (int i=0;i<io.frames;++i) {
            const auto at = io.transport->timelineSample + io.blockOffset + i;
            const float value = silent ? 0.0f : pulse ? (at==256 ? 1.0f : 0.0f) : 0.25f;
            if (io.wide()) io.out64[c][io.blockOffset+i] = value;
            else io.out[c][io.blockOffset+i] = value;
        }
    }
    bool processesDouble() const noexcept override { return true; }
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
    std::uint64_t block = 0;
    std::int32_t configured = 0;
    std::int32_t latencySamples() const noexcept override {
        if (audio::onAudioThread) ++callbackLatencyQueries;
        return delay;
    }
    bool processesDouble() const noexcept override { return true; }
    std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
    EventFlow eventFlow() const noexcept override { return instrument ? EventFlow::Consume : EventFlow::Through; }
    void process(const NodeIo& io) noexcept override {
        block = io.renderBlock; configured = io.configuredLatency;
        for (int i=0;i<io.frames;++i) {
            for (int c=0;c<io.channels;++c) {
                const float value = instrument ? 0.375f : io.wide() ?
                    (io.in64 ? static_cast<float>(io.in64[c][io.blockOffset+i]) : 0) :
                    (io.in ? io.in[c][io.blockOffset+i] : 0);
                float delayed = value;
                if (delay) {
                    auto& ring = history[static_cast<std::size_t>(c)];
                    delayed = ring[static_cast<std::size_t>(cursor)];
                    ring[static_cast<std::size_t>(cursor)] = value;
                }
                if (io.wide()) io.out64[c][io.blockOffset+i] = static_cast<double>(delayed) * gain;
                else io.out[c][io.blockOffset+i] = delayed * gain;
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
        store = Store::createEmpty(dir.path()/"taps.adi",error);
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
        check(pre && post && scope && (inserts ? pre!=post : pre==post) && post!=scope,"physical aliases share capture; distinct endpoints have separate rings");
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
    auto a=synth.session.openScope(1,1,ScopePoint::ChainInput);
    const auto b=synth.session.openScope(1,1,ScopePoint::PreFader);
    synth.render();
    check(Window(a).l[0]==0.375f && Window(b).l[0]==0.75f,"instrument track taps after instrument and before first effect");
    // Removing the effect must move ChainInput onto the strip, and closing it
    // must detach the newly selected node as well as the previous generation.
    synth.store->db().exec("DELETE FROM devices WHERE id=1");
    check(synth.session.refresh(*synth.store),"chain edit rebuilds");
    a=synth.session.openScope(1,1,ScopePoint::ChainInput); // a new physical endpoint is a generation
    synth.render();
    check(Window(a).l[0]==0.375f && Window(b).l[0]==0.375f,"instrument-only chain falls back to PreFader audio");
    synth.session.closeScope(1,ScopePoint::ChainInput);
    const auto before=a->written(); synth.render();
    check(a->written()==before+512,"closing a retargeted alias preserves the remaining PreFader consumer");
    synth.session.closeScope(1,ScopePoint::PreFader);
    const auto closed=a->written(); synth.render();
    check(a->written()==closed,"last alias consumer detaches the retargeted physical endpoint");
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
    f.session.closeScope(1,ScopePoint::PreFader);
    f.session.closeScope(1,ScopePoint::ChainInput);
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
void cursorsAndBoundaries() {
    ScopeTap tap(64, 1);
    auto first=tap.cursorAtEnd(), second=tap.cursorAtEnd();
    float value=0;
    for(int i=0;i<64;++i) { value=static_cast<float>(i); tap.write(&value,nullptr,1,100+i,true); }
    std::array<float,16> l{},r{};
    auto result=tap.read(first,l,r);
    check(result.status==ScopeTap::ReadStatus::Ready && result.frames==16 && result.firstStamp==100 && l[15]==15 && r==l,
          "one-sample segments coalesce with exact stamps and mono replication");
    auto again=tap.read(second,l,r);
    check(again.firstSequence==0 && again.frames==16 && l[0]==0,"independent cursor does not steal another consumer's history");
    for(int i=0;i<3;++i) result=tap.read(first,l,r);
    check(result.firstStamp==148 && first.next==64 && tap.read(first,l,r).status==ScopeTap::ReadStatus::NotReady,
          "cursor drains each sample once and reports NotReady after complete drain");
    value=64; tap.write(&value,nullptr,1,164,true);
    second.next=0;
    check(tap.read(second,l,r).status==ScopeTap::ReadStatus::Overrun && second.next==1,"overrun resets to oldest retained sample");
    result=tap.read(first,l,r);
    check(result.status==ScopeTap::ReadStatus::Ready && result.firstStamp==164,"continuous wrap keeps heard time");
    value=65; tap.write(&value,nullptr,1,1000,true);
    result=tap.read(first,l,r);
    check(result.status==ScopeTap::ReadStatus::Discontinuity && result.frames==1 && result.firstStamp==1000,
          "seek returns the first new span with explicit discontinuity");
    tap.write(&value,nullptr,1,1001,false);
    result=tap.read(first,l,r);
    check(result.status==ScopeTap::ReadStatus::Discontinuity && !result.advancing,"parked transport transition is explicit");
    tap.write(&value,nullptr,1,1001,false);
    result=tap.read(first,l,r);
    check(result.status==ScopeTap::ReadStatus::Ready && result.firstStamp==1001,"parked callback stamps do not advance falsely");
    ScopeTap generation(64,1);
    check(generation.read(first,l,r).status==ScopeTap::ReadStatus::GenerationChanged && first.generation==generation.generation(),
          "a cursor cannot silently cross rate/history/source generations");
    ScopeTap jumps(4096,1);
    auto lag=jumps.cursorAtEnd();
    for(int i=0;i<256;++i) jumps.write(&value,nullptr,1,i*100,true);
    check(jumps.oldest()>0 && jumps.read(lag,l,r).status==ScopeTap::ReadStatus::Overrun,
          "bounded timestamp history reports an overrun, never fabricated gapless metadata");
    ScopeTap split(4096,1);
    auto splitCursor=split.cursorAtEnd();
    for(int i=0;i<4096;++i) split.write(&value,nullptr,1,i,true);
    std::array<float,4096> wholeL{},wholeR{};
    result=split.read(splitCursor,wholeL,wholeR);
    check(split.oldest()==0 && result.frames==4096 && result.status==ScopeTap::ReadStatus::Ready,
          "4096 single-sample event segments preserve the complete prepared history");
}

void scopedDemandAndGeneration() {
    Fixture f; f.load();
    auto a=f.session.requestScope({{1,ScopePoint::DeviceInput,1},0.5});
    auto b=f.session.requestScope({{1,ScopePoint::DeviceInput,1},0.25});
    check(a && b && a.tap()==b.tap() && f.session.scopeStats().sources==1,"scoped consumers share one physical device input capture");
    auto old=a.tap();
    auto c=f.session.requestScope({{1,ScopePoint::DeviceInput,1},1.5});
    check(a.tap()==b.tap() && b.tap()==c.tap() && c.tap()!=old && c.tap()->capacity()==72000,
          "maximum history demand prepares a shared replacement generation");
    auto cursor=a.tap()->cursorAtEnd(); f.render();
    std::array<float,512> l{},r{};
    auto read=a.tap()->read(cursor,l,r);
    check(read.frames==512 && l[0]==0.25f,"device input captures summed input before processing");
    a.reset(); b.reset(); f.render();
    check(c.tap()->written()==1024 && f.session.scopeStats().consumers==1,"one consumer closing cannot disable another");
    f.session.prepare(96000,512);
    check(c.tap()->sampleRate()==96000 && c.tap()->capacity()==144000 &&
          c.tap()->read(cursor,l,r).status==ScopeTap::ReadStatus::GenerationChanged,
          "rate preparation replaces capture storage and invalidates old analysis windows");
    auto output=f.session.requestScope({{1,ScopePoint::DeviceOutput,1},0.5});
    f.render();
    check(Window(output.tap()).l[0]==0.5f,"device output captures processed audio independently of input");
    f.store->db().exec("DELETE FROM devices WHERE id=1");
    check(f.session.refresh(*f.store) && !c.tap() && !output.tap(),"removed endpoint revokes availability while leases remain valid");
    auto unavailable=f.session.requestScope({{1,ScopePoint::DeviceInput,999},0.5});
    check(unavailable && !unavailable.tap(),"missing device keeps a demand handle without accessing an unrelated device");
    check(!f.session.requestScope({{1,ScopePoint::DeviceInput,0},1}) &&
          !f.session.requestScope({{1,ScopePoint::PostFader,1},1}) &&
          !f.session.requestScope({{1,ScopePoint::PostFader,0},std::numeric_limits<double>::infinity()}),
          "invalid endpoint and unbounded history requests are rejected");
    ScopeLease survivor;
    { Fixture temporary; temporary.load(); survivor=temporary.session.requestScope({{1,ScopePoint::DeviceInput,1},0.5}); }
    check(!survivor && !survivor.tap(),"a lease outliving its Session becomes inert");
}

void stalledRetirement() {
    Fixture f; f.load();
    auto lease=f.session.requestScope({{1,ScopePoint::DeviceOutput,1},0.5});
    f.render();
    auto current=lease.tap();
    std::weak_ptr<const ScopeTap> retired=current; current.reset();
    f.a.stall.store(true);
    std::thread callback([&] { f.render(); });
    while(!f.a.entered.load(std::memory_order_acquire)) std::this_thread::yield();
    lease.reset();
    f.session.collectScopeTaps();
    check(!retired.expired(),"closing and owner collection cannot free a stalled callback's capture storage");
    f.a.resume.store(true,std::memory_order_release); callback.join();
    f.session.collectScopeTaps();
    check(!retired.expired(),"retirement waits for port detachment at the next physical block");
    f.a.stall.store(false); f.render(); f.session.collectScopeTaps();
    check(retired.expired(),"owner frees retired storage after a newer callback detaches and acknowledges it");
}

void deviceStampsPrecisionAndSound() {
    for(const auto precision:{Precision::Float32,Precision::Float64}) {
        Fixture f; f.delays=true; f.a.pulse=true; f.load();
        f.session.setPrecision(precision);
        auto input=f.session.requestScope({{1,ScopePoint::DeviceInput,1},1});
        auto output=f.session.requestScope({{1,ScopePoint::DeviceOutput,1},1});
        f.render();
        const Window before(input.tap()), after(output.tap());
        check(before.stamp==0 && after.stamp==-32 && before.peakStamp()==after.peakStamp(),
              "input/output heard stamps use cached arrival and own latency at either precision");
        auto* effect=dynamic_cast<Effect*>(f.session.instanceFor(1));
        check(effect && effect->block!=0 && effect->configured==32,"device receives physical callback identity and cached configured latency");
        f.a.pulse=false; f.render(); f.render();
        const auto* entry=f.session.entryFor(1);
        f.session.devices().nodeAt(entry->hostIndex).setBypassed(true);
        f.render();
        check(Window(output.tap()).l[100]==0.25f,"device output follows actual dry bypass rather than dormant wet buffers");
        f.a.silent=true;
        f.render(); f.render();
        const auto silent=Window(output.tap());
        check(std::all_of(silent.l.begin(),silent.l.end(),[](float v){return v==0;}) &&
              output.tap()->written()==3072 && f.session.graph().currentGraph()->stats().nodesSuspended>0,
              "suspended bypass keeps capture current with exact silence at both precisions");
    }
    Fixture clean, watched; clean.load(); watched.load();
    auto input=watched.session.requestScope({{1,ScopePoint::DeviceInput,1},1});
    auto output=watched.session.requestScope({{1,ScopePoint::DeviceOutput,1},1});
    std::array<float,1024> a{},b{};
    float* ap[]={a.data(),a.data()+512}; float* bp[]={b.data(),b.data()+512};
    for(int i=0;i<8;++i) {
        clean.session.process({ap,nullptr,2,0,512,0}); watched.session.process({bp,nullptr,2,0,512,0});
        check(a==b,"capture demand leaves rendered audio sample-identical");
    }
}

void physicalCallbackMetadata() {
    Fixture f; f.load();
    auto lease=f.session.requestScope({{1,ScopePoint::DeviceInput,1},1});
    auto* effect=dynamic_cast<Effect*>(f.session.instanceFor(1));
    std::array<float,8> l{},r{}; float* channels[]={l.data(),r.data()};
    std::uint64_t first=0;
    bool stable=true;
    {
        auto block=f.session.beginRender();
        for(int i=0;i<8;++i) {
            if(i==3) f.session.transport().locate(12345);
            if(i==6) f.session.transport().play(false);
            stable=block.process({channels,nullptr,2,0,1,i}) && stable;
            if(i==0) first=effect->block;
            stable=stable && effect->block==first;
        }
    }
    check(stable && first!=0 && lease.tap()->written()==8,
          "one physical callback keeps its identity through one-frame slices, seeks and stop");
    f.render();
    check(effect->block!=first,"next physical callback gets a new ID even while transport is parked");
}

} // namespace
int main() {
    try {
        gainAndFallback(); stamps(); silenceAndConcurrentClose();
        cursorsAndBoundaries(); scopedDemandAndGeneration(); stalledRetirement(); deviceStampsPrecisionAndSound(); physicalCallbackMetadata();
        check(callbackLatencyQueries.load()==0,"no SDK latency queries in callback capture");
        check(allocations.load()==0,"zero allocations in audio process");
        check(deallocations.load()==0,"zero retirement destructors or deallocations in audio process");
        check(audio::callbackFileIo.load()==0,"zero callback file I/O");
    } catch(const std::exception& e) { ++failures; std::printf("FAIL exception: %s\n",e.what()); }
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
