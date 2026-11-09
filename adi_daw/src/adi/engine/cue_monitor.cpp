// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/cue_monitor.hpp"
#include "adi/engine/publisher.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace adi::engine {
namespace {
struct Ramp {
    double current=0,target=0,step=0;int remaining=0;
    void set(double to,int frames) noexcept {if(to==target)return;target=to;remaining=frames;step=(to-current)/frames;}
    double next() noexcept {if(remaining>0){current+=step;if(--remaining==0)current=target;}return current;}
};
class CueGain final:public Node {
    struct Parameters {StripLanes lanes;bool bound=false;std::shared_ptr<void> owner;};
    struct Publication final:Sequenced {std::shared_ptr<Parameters> parameters;};
    StripNode gain_;
    SnapshotPublisher<Publication> parameters_;
    std::shared_ptr<Parameters> configured_; // owner only; generations retain their own value
    std::atomic<const StripControlImage*> published_{nullptr};
public:
    explicit CueGain(const Transport* transport){gain_.setTransport(transport);}
    void configure(const StripConfig& config) {
        published_.store(nullptr,std::memory_order_release);
        gain_.setStatic(config.volumeDb,config.pan,config.law,config.unmuted);
        gain_.setDelay(config.delaySamples,config.delayRate);
        auto parameters=std::make_shared<Parameters>();
        if(config.lanes){parameters->lanes=*config.lanes;parameters->lanes.mute=nullptr;parameters->bound=true;parameters->owner=config.keepAlive;}
        configured_=parameters;
        auto publication=std::make_unique<Publication>();publication->parameters=std::move(parameters);
        parameters_.publish(std::move(publication));
    }
    void captureLanes(StripLaneBindings& captured) const {
        captured.add(gain_,configured_ && configured_->bound?&configured_->lanes:nullptr,configured_);
    }
    void published(const StripControlImage& value) noexcept { published_.store(&value,std::memory_order_release); }
    const StripNode& node() const noexcept { return gain_; }
    void collect(){parameters_.collect();}
    void prepare(double rate,std::int32_t frames) override{gain_.prepare(rate,frames);}
    void release() override{gain_.release();}
    void process(const NodeIo& io) noexcept override {
        SnapshotPublisher<Publication>::AudioRead publication(parameters_);
        const auto* parameters=publication.valid()?publication->parameters.get():nullptr;
        if(const auto* value=published_.load(std::memory_order_acquire))gain_.applyPublishedControls(*value);
        else gain_.usePublishedLanes(parameters && parameters->bound?&parameters->lanes:nullptr);
        gain_.process(io);
    }
    void silenceTaps(const NodeIo& io) noexcept override {
        SnapshotPublisher<Publication>::AudioRead publication(parameters_);
        const auto* parameters=publication.valid()?publication->parameters.get():nullptr;
        if(const auto* value=published_.load(std::memory_order_acquire))gain_.applyPublishedControls(*value);
        else gain_.usePublishedLanes(parameters && parameters->bound?&parameters->lanes:nullptr);
        gain_.silenceTaps(io);
    }
    bool processesDouble() const noexcept override{return true;}
    std::int32_t latencySamples() const noexcept override{return gain_.latencySamples();}
    std::int64_t tailSamples() const noexcept override{return 0;}
    EventFlow eventFlow() const noexcept override{return EventFlow::Stop;}
};
class CueSink final:public Node {
    MixNode copy_;
public:
    std::uint64_t frames=0; // audio owner only, including silent blocks
    void process(const NodeIo& io) noexcept override{copy_.process(io);frames+=static_cast<std::uint64_t>(io.frames);}
    void silenceTaps(const NodeIo& io) noexcept override{frames+=static_cast<std::uint64_t>(io.frames);}
    bool processesDouble() const noexcept override{return true;}
    std::int64_t tailSamples() const noexcept override{return 0;}
    EventFlow eventFlow() const noexcept override{return EventFlow::Stop;}
};
struct Feed {
    CueGain gain;CueSink aligned;Ramp cueMix;
    explicit Feed(const Transport* transport):gain(transport){}
};
bool audioTrack(const rows::Track& t) {
    return t.kind=="audio" || t.kind=="midi" || t.kind=="instrument" || t.kind=="group" || t.kind=="return";
}
struct Buffers {
    int frames,channels;
    std::vector<double> cue,reference,scratch;
    Buffers(int n,int ch):frames(n),channels(ch),cue(static_cast<std::size_t>(2)*static_cast<std::size_t>(n)),reference(cue.size()),scratch(cue.size()){}
};
struct Scene final:Sequenced {
    CueSettings settings;
    struct Voice {Node* node;Ramp* gate;bool selected;};
    std::vector<Voice> voices;
    CueSink* reference=nullptr;
    std::int32_t referenceWarmup=0;
    std::shared_ptr<Buffers> buffers;
    bool enabled=false;
    double gain=1;
    int ramp=240;
};

}
struct CueMonitor::Impl {
    std::map<std::int64_t,std::unique_ptr<Feed>> feeds;
    std::set<std::int64_t> present,selected;
    std::shared_ptr<Buffers> buffers;
    SnapshotPublisher<Scene> scenes;
    CueSink* lastReference=nullptr; // audio only; feeds live for the session
    Ramp referenceMix,volume{1,1,0,0};
};
CueMonitor::CueMonitor():impl_(std::make_unique<Impl>()){}
CueMonitor::~CueMonitor()=default;
void CueMonitor::configure(const rows::Model& model,const CueSettings& settings,const Transport* transport,
                           const std::shared_ptr<StripAutomation>& automation,bool enabled) {
    impl_->present.clear();impl_->selected.clear();
    if(!enabled)return;
    std::map<std::int64_t,const rows::MixerStrip*> strips;
    for(const auto& strip:model.strips)strips[strip.trackId]=&strip;
    for(const auto& track:model.tracks) {
        if(!audioTrack(track))continue;
        const bool reference=track.id==settings.referenceTrack;
        if(settings.cueMode && std::binary_search(settings.selectedTracks.begin(),settings.selectedTracks.end(),track.id))impl_->selected.insert(track.id);
        if(reference || impl_->selected.contains(track.id)) {
            auto& feed=impl_->feeds[track.id];if(!feed)feed=std::make_unique<Feed>(transport);
        }
        const auto found=impl_->feeds.find(track.id);if(found==impl_->feeds.end())continue;
        impl_->present.insert(track.id);
        StripConfig config;config.delayRate=model.project.sampleRate;
        if(const auto it=strips.find(track.id);it!=strips.end()) {
            config.delaySamples=it->second->delaySamples;
            if(reference){config.volumeDb=it->second->volumeDb;config.pan=it->second->pan;config.law=static_cast<PanLaw>(std::clamp<std::int64_t>(it->second->panLaw,0,3));}
        }
        if(reference) {
            config.unmuted=true; // PFL audition ignores the track's Main mute
            if(automation)if(const auto it=automation->byTrack.find(track.id);it!=automation->byTrack.end()){
                config.lanes=&it->second;config.keepAlive=automation;
            }
        }
        found->second->gain.configure(config);
    }
}
std::vector<MonitorPoint> CueMonitor::points(const TapFn& source) const {
    std::vector<MonitorPoint> out;out.reserve(impl_->present.size());
    for(auto id:impl_->present) {
        auto& feed=*impl_->feeds.at(id);
        out.push_back({id,source?source(id,Tap::PostFx):nullptr,&feed.gain,&feed.aligned});
    }
    return out;
}
void CueMonitor::captureLanes(StripLaneBindings& captured) const {
    for(const auto& [id,feed]:impl_->feeds){(void)id;feed->gain.captureLanes(captured);}
}
bool CueMonitor::publish(const CueSettings& settings,double rate,int maxFrames,int channels,bool enabled,const Graph* prepared) noexcept {
    try {
    auto scene=std::make_unique<Scene>();scene->settings=settings;scene->enabled=enabled;
    scene->ramp=std::max(1,static_cast<int>(std::lround(rate*0.005)));scene->gain=faderGain(settings.volumeDb);
    if(!impl_->buffers || impl_->buffers->frames!=maxFrames || impl_->buffers->channels!=channels)
        impl_->buffers=std::make_shared<Buffers>(maxFrames,channels);
    scene->buffers=impl_->buffers;
    if(enabled) {
        for(auto id:impl_->present){auto& feed=*impl_->feeds.at(id);scene->voices.push_back({&feed.aligned,&feed.cueMix,impl_->selected.contains(id)});}
        if(impl_->present.contains(settings.referenceTrack)) {
            auto& feed=*impl_->feeds.at(settings.referenceTrack);scene->reference=&feed.aligned;
            if(prepared)if(const auto* line=prepared->edgeLine(prepared->find(feed.gain),prepared->find(feed.aligned),Bus::Main))scene->referenceWarmup=line->delay();
        }
    }
    impl_->scenes.publish(std::move(scene));collect();return true;
    } catch(...) {return false;}
}
void CueControlBatch::captureLanes(StripLaneBindings& captured) const {
    for(const auto& entry:entries_)captured.add(*entry.node,entry.image.config.lanes,entry.image.config.keepAlive);
}
void CueControlBatch::apply() const noexcept {
    for(const auto& entry:entries_)entry.apply(entry.target,entry.image);
}
std::shared_ptr<const CueControlBatch> CueMonitor::prepareControls(const rows::Model& model,const CueSettings& settings,
    const std::shared_ptr<StripAutomation>& automation,bool enabled) const {
    auto result=std::make_shared<CueControlBatch>();
    if(!enabled)return result;
    for(const auto id:impl_->present) {
        const auto found=impl_->feeds.find(id);if(found==impl_->feeds.end())continue;
        StripControlImage value;value.config.delayRate=model.project.sampleRate;
        const bool reference=id==settings.referenceTrack;
        for(const auto& strip:model.strips)if(strip.trackId==id) {
            value.config.delaySamples=strip.delaySamples;
            if(reference) {
                value.config.volumeDb=strip.volumeDb;value.config.pan=strip.pan;
                value.config.law=static_cast<PanLaw>(std::clamp<std::int64_t>(strip.panLaw,0,3));
            }
        }
        if(reference && automation)if(const auto lane=automation->byTrack.find(id);lane!=automation->byTrack.end()) {
            struct Held {StripLanes lanes;std::shared_ptr<StripAutomation> owner;};
            auto held=std::make_shared<Held>();held->lanes=lane->second;held->lanes.mute=nullptr;held->owner=automation;
            value.config.lanes=&held->lanes;value.config.keepAlive=std::move(held);
        }
        result->entries_.push_back({&found->second->gain,
            [](void* target,const StripControlImage& image) noexcept {static_cast<CueGain*>(target)->published(image);},std::move(value),&found->second->gain.node()});
    }
    return result;
}
void CueMonitor::collect(){impl_->scenes.collect();for(auto& [id,feed]:impl_->feeds){(void)id;feed->gain.collect();}}
void CueMonitor::process(const GraphHost& graph,const AudioIo& io,Click click) noexcept {
    SnapshotPublisher<Scene>::AudioRead scene(impl_->scenes);
    if(!scene.valid() || !scene->enabled || io.frames<=0)return;
    const bool cueActive=std::any_of(scene->voices.begin(),scene->voices.end(),[](const Scene::Voice& voice){return voice.selected || voice.gate->current!=0;});
    if(!cueActive && !scene->settings.referenceOn && impl_->referenceMix.current==0 && !click.render) {
        impl_->volume={scene->gain,scene->gain,0,0};
        return;
    }
    auto& buffers=*scene->buffers;if(io.frames>buffers.frames)return;
    const auto frames=static_cast<std::size_t>(io.frames),stride=static_cast<std::size_t>(buffers.frames);
    double* cue[]{buffers.cue.data(),buffers.cue.data()+stride};
    double* reference[]{buffers.reference.data(),buffers.reference.data()+stride};
    double* scratch[]{buffers.scratch.data(),buffers.scratch.data()+stride};
    for(int c=0;c<2;++c){std::fill_n(cue[c],frames,0.0);std::fill_n(reference[c],frames,0.0);}
    AudioIo copy;copy.out64=scratch;copy.numOut=2;copy.frames=io.frames;
    for(const auto& voice:scene->voices) {
        voice.gate->set(voice.selected?1.0:0.0,scene->ramp);
        if(voice.gate->current==0 && !voice.selected)continue;
        const bool copied=graph.copyRenderedOutput(*voice.node,copy);
        for(std::size_t i=0;i<frames;++i) {
            const double gate=voice.gate->next();
            if(copied)for(int c=0;c<2;++c)cue[c][i]+=scratch[buffers.channels==1?0:c][i]*gate;
        }
    }
    if(click.render){AudioIo cueIo=io;cueIo.out=nullptr;cueIo.out64=cue;cueIo.numOut=2;click.render(click.context,cueIo);}
    if(scene->reference)impl_->lastReference=scene->reference;
    bool haveReference=false;
    if(impl_->lastReference) {
        copy.out64=reference;haveReference=graph.copyRenderedOutput(*impl_->lastReference,copy);
        if(buffers.channels==1)std::copy_n(reference[0],frames,reference[1]);
    }
    const bool wantReference=scene->settings.referenceOn && scene->reference && haveReference;
    int readyAt=0;
    if(wantReference) {
        const auto count=scene->reference->frames;
        const auto before=count>=frames?count-frames:0;
        const auto warmup=static_cast<std::uint64_t>(std::max(0,scene->referenceWarmup));
        if(before<warmup)readyAt=static_cast<int>(std::min<std::uint64_t>(warmup-before,frames));
    }
    if(!wantReference || readyAt==0)impl_->referenceMix.set(wantReference?1.0:0.0,scene->ramp);
    impl_->volume.set(scene->gain,scene->ramp);
    const auto first=scene->settings.firstOutputChannel;
    auto add=[&](int channel,int i,double value){
        if(channel>=io.numOut || value==0)return;
        if(io.out && io.out[channel])io.out[channel][i]+=static_cast<float>(value);
        if(io.out64 && io.out64[channel])io.out64[channel][i]+=value;
    };
    for(int i=0;i<io.frames;++i) {
        if(wantReference && i==readyAt)impl_->referenceMix.set(1.0,scene->ramp);
        const double ref=impl_->referenceMix.next(),main=1-ref,gain=impl_->volume.next();
        for(int c=0;main!=1 && c<std::min(buffers.channels,io.numOut);++c) {
            if(io.out && io.out[c])io.out[c][i]*=static_cast<float>(main);
            if(io.out64 && io.out64[c])io.out64[c][i]*=main;
        }
        for(int c=0;c<2;++c) {
            const auto index=static_cast<std::size_t>(i);
            const double r=reference[c][index]*ref*gain;
            add(c,i,r);
            // Shared physical pair gets each signal once. Separate Cue Out
            // also receives the exclusive reference, past Main's chain.
            add(first+c,i,cue[c][index]*main*gain+(first==0?0:r));
        }
    }
}
}
