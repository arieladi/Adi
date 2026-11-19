// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "../src/juce/ui_shell.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/ui/shared_analysis.hpp"
#include "adi/textproj.hpp"
#include "juce/device_model.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace adi::uitest {
namespace mix_seek_detail {
// A deterministic test barrier inside a headless device, after the document
// drained its transport mailbox. This deliberately blocks only the synthetic
// callback thread; there is no audio device, native peer, capture or host.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> blockNext{false}, oldSignal{true};
    bool entered=false, released=false;
    std::uint64_t revision=0;
    std::int64_t sample=0;
    std::int32_t frames=0;
    bool wide=false;
    std::atomic<std::uint64_t> targetCalls{0};

    void arm() {
        std::lock_guard lock(mutex);
        entered=released=false;
        blockNext.store(true,std::memory_order_release);
    }
    bool awaitEntry() {
        std::unique_lock lock(mutex);
        // Watchdog only: no wall-clock measurement contributes to acceptance.
        return changed.wait_for(lock,std::chrono::seconds(10),[this]{return entered;});
    }
    void release() noexcept {
        {
            std::lock_guard lock(mutex);
            released=true;
        }
        changed.notify_all();
    }
};

class Source final : public device::DeviceInstance {
  public:
    explicit Source(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {
        identity_.format="clap";identity_.uid="test.mix-seek-source";identity_.name="Seek source";
    }
    const device::DeviceIdentity &identity() const noexcept override {return identity_;}
    bool loaded() const noexcept override {return true;}
    bool processesDouble() const noexcept override {return true;}
    engine::EventFlow eventFlow() const noexcept override {return engine::EventFlow::Consume;}
    void process(const engine::NodeIo &io) noexcept override {
        // Capture the old signal before acknowledging the barrier. UI intent
        // cannot change the completed contents of this in-flight callback.
        const double value=gate_->oldSignal.load(std::memory_order_acquire)?.5:0.;
        if(gate_->blockNext.exchange(false,std::memory_order_acq_rel)) {
            std::unique_lock lock(gate_->mutex);
            gate_->revision=io.transport?io.transport->transportRevision:0;
            gate_->sample=io.transport?io.transport->timelineSample:0;
            gate_->frames=io.frames;gate_->wide=io.wide();gate_->entered=true;
            gate_->changed.notify_all();
            gate_->changed.wait(lock,[this]{return gate_->released;});
        }
        for(std::int32_t c=0;c<io.channels;++c)for(std::int32_t f=0;f<io.frames;++f) {
            const auto i=io.blockOffset+f;
            if(io.wide())io.out64[c][i]=value;
            else io.out[c][i]=static_cast<float>(value);
        }
    }
  private:
    std::shared_ptr<Gate> gate_;
    device::DeviceIdentity identity_;
};

class Target final : public device::DeviceInstance {
  public:
    Target(std::shared_ptr<Gate> gate,bool suspend) : gate_(std::move(gate)),suspend_(suspend) {
        identity_.format="clap";identity_.uid="test.mix-seek-target";identity_.name="Seek target";
    }
    const device::DeviceIdentity &identity() const noexcept override {return identity_;}
    bool loaded() const noexcept override {return true;}
    bool processesDouble() const noexcept override {return true;}
    std::int64_t tailSamples() const noexcept override {return suspend_?0:engine::kInfiniteTail;}
    void process(const engine::NodeIo &io) noexcept override {
        gate_->targetCalls.fetch_add(1,std::memory_order_relaxed);
        device::passThrough(io);
    }
  private:
    std::shared_ptr<Gate> gate_;
    bool suspend_;
    device::DeviceIdentity identity_;
};

struct Driver {
    explicit Driver(ui::ProjectDocument &sourceDocument) : document(sourceDocument) {
        for(std::size_t c=0;c<2;++c){out[c]=samples[c].data();out64[c]=wide[c].data();}
    }
    engine::AudioIo io(std::int32_t frames) noexcept {
        engine::AudioIo result;
        result.out=out.data();result.out64=out64.data();result.numOut=2;
        result.frames=frames;result.streamTimeSamples=stream;
        return result;
    }
    void render(std::int32_t frames) noexcept {
        document.process(io(frames));stream+=frames;
    }
    ui::ProjectDocument &document;
    std::array<std::array<float,2048>,2> samples{};
    std::array<std::array<double,2048>,2> wide{};
    std::array<float*,2> out{};
    std::array<double*,2> out64{};
    std::int64_t stream=0;
};

// Unwind safely even if a setup/check callback throws after the synthetic
// callback entered its barrier. Destruction never leaves a blocked thread.
class BlockedCallback final {
  public:
    BlockedCallback(Driver &driver,Gate &gate) : driver_(driver),gate_(gate),
        thread_([this]{driver_.document.process(driver_.io(2048));}) {}
    ~BlockedCallback(){finish();}
    void finish() noexcept {
        gate_.release();
        if(thread_.joinable()){thread_.join();driver_.stream+=2048;}
    }
  private:
    Driver &driver_;
    Gate &gate_;
    std::thread thread_;
};

enum class Seek { Distant, Contiguous, Parked };
inline const char *seekName(Seek seek) noexcept {
    return seek==Seek::Distant?"advancing-far":seek==Seek::Contiguous?"advancing-contiguous":"parked-same";
}

inline void run(ui::AppCommands &commands,const std::filesystem::path &directory,
                const std::function<void(bool,const char*)> &check,engine::Precision precision,
                Seek seek,engine::ScopePoint point,bool suspend) {
    const std::string name=std::string("mix-seek-")+
        (precision==engine::Precision::Float64?"f64-":"f32-")+seekName(seek)+
        (point==engine::ScopePoint::DeviceInput?"-input":"-output")+(suspend?"-suspended":"-active");
    const auto expect=[&](bool okay,const char *text) {
        const auto label=name+": "+text;check(okay,label.c_str());
    };
    const auto gate=std::make_shared<Gate>();
    const engine::DeviceLoader loader=[gate,suspend](const engine::DeviceRequest &request,std::string &error)
        ->std::unique_ptr<device::DeviceInstance> {
        if(request.ref&&request.ref->uid=="test.mix-seek-source")return std::make_unique<Source>(gate);
        if(request.ref&&request.ref->uid=="test.mix-seek-target")return std::make_unique<Target>(gate,suspend);
        error="not a seek fixture device";return {};
    };
    std::string error;
    auto document=ui::ProjectDocument::open(directory/(name+".adi"),true,loader,error);
    expect(document!=nullptr,"open an isolated software seek project");
    if(!document)return;
    document->store().db().exec(
        "INSERT INTO plugin_refs(id,format,uid,name) VALUES"
        "(901,'clap','test.mix-seek-source','Seek source'),"
        "(902,'clap','test.mix-seek-target','Seek target')");
    std::vector<OpRequest> requests(3);
    requests[0].opType="chain.create";requests[0].payload={{"id",900},{"track",2},{"ord",0},{"name","Seek chain"}};
    requests[1].opType="device.insert";requests[1].payload={{"id",901},{"chain",900},{"ord",0},{"ref",901},{"name","Seek source"},{"latency",0}};
    requests[2].opType="device.insert";requests[2].payload={{"id",902},{"chain",900},{"ord",1},{"ref",902},{"name","Seek target"},{"latency",0}};
    const bool ready=document->ops().submit(std::move(requests)).ok&&document->synchronise()&&
        document->session().setPrecision(precision);
    expect(ready,"prepare actual source and downstream tapped target");
    if(!ready)return;
    document->prepare(48000.,2048);
    Driver driver(*document);
    const engine::ScopeSource source{2,point,902};
    auto engineLease=document->session().requestScope({source,.25});
    expect(static_cast<bool>(engineLease),"request the actual graph capture port");
    if(!engineLease){document->release();return;}
    // Settle publication/acknowledgement/deferred binding before constructing
    // the broker. The broker then starts at the stable tap's current end.
    bool settled=false;
    for(int i=0;i<8;++i) {
        driver.render(256);document->session().collectScopeTaps();
        const auto scope=document->session().scopeStats();
        const auto active=engineLease.tap();
        if(!scope.awaitingAudio&&scope.deferredPlans==0&&active&&active->written()>0){settled=true;break;}
    }
    const auto tap=engineLease.tap();
    expect(settled&&tap&&tap->written()>0,"settle the chosen tap before the in-flight race");
    if(!settled||!tap||tap->written()==0){engineLease.reset();document->release();return;}

    auto broker=std::make_shared<ui::SharedAnalysis>(
        [&,source](engine::ScopeSource requested,double)->std::shared_ptr<const engine::ScopeTap>{
            return requested==source?engineLease.tap():nullptr;
        },[](engine::ScopeSource){},ui::SharedAnalysis::Mode::Manual);
    ui::SharedAnalysis::Request request;
    request.track=2;request.point=point;request.device=902;request.fftSize=1024;request.hopSize=1024;
    request.outputs=ui::SharedAnalysis::Spectrum|ui::SharedAnalysis::Scope|ui::SharedAnalysis::Meters|
                    ui::SharedAnalysis::Spectrogram;
    auto analysis=broker->acquire(request);
    expect(static_cast<bool>(analysis)&&analysis.available(),"acquire an available Manual analysis domain");
    if(!analysis){broker->shutdown();engineLease.reset();document->release();return;}

    {
        ui::AdiRootComponent root(document->view(),document->ops(),document->mailbox(),commands,
                                  document->views(),document->sessionState(),name);
        root.sharedAnalysis=broker; // The real Locate path must not force a UI-intent epoch.
        auto &transport=document->session().transport();
        transport.play(seek!=Seek::Parked);
        // Default tempo120 and PPQ divisible by3/4 represent8000/30000
        // samples exactly. After8*256 the advancing old block starts5952.
        transport.locate(seek==Seek::Parked?8000:3904);
        std::shared_ptr<const ui::SharedAnalysis::Result> first;
        for(int i=0;i<8;++i) {
            driver.render(256);document->session().collectScopeTaps();broker->pump();
            if(i==3)first=analysis.read();
        }
        const auto baseline=analysis.read();
        expect(first&&baseline&&first->sourceEpoch==baseline->sourceEpoch&&
               baseline->revision>first->revision&&baseline->peak[0]>.25f&&
               baseline->advancing==(seek!=Seek::Parked),
               "no-command contiguous/parked callbacks retain their source epoch");
        if(!baseline){broker->shutdown();document->release();return;}
        const auto oldEpoch=baseline->sourceEpoch,oldGeneration=baseline->generation;
        const auto oldWritten=tap->written();
        auto rawCursor=tap->cursorAtEnd();
        gate->arm();
        {
            BlockedCallback blocked(driver,*gate);
            const bool entered=gate->awaitEntry();
            expect(entered,"2048-frame callback entered the post-mailbox device barrier");
            if(!entered){blocked.finish();broker->shutdown();document->release();return;}
            const auto oldRevision=gate->revision;
            expect(gate->frames==2048&&gate->wide==(precision==engine::Precision::Float64)&&
                   gate->sample==(seek==Seek::Parked?8000:5952)&&oldRevision!=0,
                   "the blocked callback retains its prepared precision/sample/revision");
            const std::int64_t targetTick=seek==Seek::Distant?textproj::kPPQ*5/4:textproj::kPPQ/3;
            const auto current=document->view().current();
            const auto target=static_cast<std::int64_t>(current->tempo->ticksToSeconds(targetTick)*48000.);
            expect(target==(seek==Seek::Distant?30000:8000),"seek fixture's authored tick maps to its exact heard sample");
            expect(root.locate(targetTick),"actual root Locate posts while the old callback is blocked");
            gate->oldSignal.store(false,std::memory_order_release);
            blocked.finish();
            broker->pump();
            const auto old=analysis.read();
            expect(old&&old->sourceEpoch==oldEpoch&&old->generation==oldGeneration&&old->peak[0]>.25f&&
                   old->firstStamp==(seek==Seek::Parked?8000:6976),
                   "completed old audio is never relabelled as a UI-requested seek epoch");
            std::array<float,2048> left{},right{};
            const auto oldRead=tap->read(rawCursor,left,right);
            expect(oldRead.frames==2048&&oldRead.transportRevision==oldRevision&&
                   oldRead.firstStamp==(seek==Seek::Parked?8000:5952),
                   "the completed tap span carries the callback's old applied revision");
            expect(tap->written()==oldWritten+2048,"old completion publishes exactly its own captured samples");
            const auto newSequence=tap->written();
            const auto targetCalls=gate->targetCalls.load(std::memory_order_relaxed);
            for(int block=0;block<4;++block) {
                driver.render(256);document->session().collectScopeTaps();
                std::array<float,256> zeroLeft{},zeroRight{};
                const auto read=tap->read(rawCursor,zeroLeft,zeroRight);
                const auto applied=document->session().transport().revision();
                expect(read.frames==256&&read.transportRevision==applied&&applied!=oldRevision&&
                       read.advancing==(seek!=Seek::Parked)&&
                       read.firstStamp==target+(seek==Seek::Parked?0:block*256)&&
                       read.status==(block==0?engine::ScopeTap::ReadStatus::Discontinuity:engine::ScopeTap::ReadStatus::Ready),
                       "actual callback revision forms a boundary even at equal/contiguous heard stamps");
                broker->pump();
                if(block<3)expect(!analysis.read(),"fewer than1024 post-seek frames cannot publish a fresh window");
            }
            const auto fresh=analysis.read();
            expect(fresh&&fresh->sourceEpoch==oldEpoch+1&&fresh->generation==oldGeneration&&
                   fresh->firstStamp==target&&fresh->firstSequence==newSequence&&
                   fresh->scopeLeft.size()==1024&&fresh->scopeRight.size()==1024&&
                   fresh->peak[0]==0.f&&fresh->peak[1]==0.f&&fresh->rms[0]==0.f&&fresh->rms[1]==0.f&&
                   std::all_of(fresh->scopeLeft.begin(),fresh->scopeLeft.end(),[](float v){return v==0.f;})&&
                   std::all_of(fresh->scopeRight.begin(),fresh->scopeRight.end(),[](float v){return v==0.f;})&&
                   std::all_of(fresh->binsDbfs.begin(),fresh->binsDbfs.end(),[](float v){return v<=-100.f;})&&
                   fresh->columns.size()==1&&fresh->columns.front().firstStamp==target,
                   "fourth256-frame callback publishes one new1024 window without old peak/overlap/columns");
            const auto after=gate->targetCalls.load(std::memory_order_relaxed);
            expect(suspend?after==targetCalls:after>targetCalls,
                   "the selected active/suspended graph tap path is actually exercised");
        }
        analysis.reset();broker->shutdown();root.sharedAnalysis.reset();
    }
    engineLease.reset();document->session().collectScopeTaps();document->release();
}
} // namespace mix_seek_detail

// Caller provides the canonical noninteractive CRT/exception test harness.
// Every project, device and callback here is software-only.
inline void mixSeekChecks(ui::AppCommands &commands,const std::filesystem::path &directory,
                          std::function<void(bool,const char*)> check) {
    for(const auto precision:{engine::Precision::Float32,engine::Precision::Float64})
    for(const auto seek:{mix_seek_detail::Seek::Distant,mix_seek_detail::Seek::Contiguous,mix_seek_detail::Seek::Parked})
    for(const auto point:{engine::ScopePoint::DeviceInput,engine::ScopePoint::DeviceOutput})
    for(const bool suspend:{false,true}) {
        try {mix_seek_detail::run(commands,directory,check,precision,seek,point,suspend);}
        catch(const std::exception &error){const auto text=std::string("software seek fixture exception: ")+error.what();check(false,text.c_str());}
        catch(...){check(false,"software seek fixture unknown exception");}
    }
}
} // namespace adi::uitest
