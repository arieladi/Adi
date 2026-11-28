// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/cue_monitor.hpp"
#include "adi/dsp/stereo_envelope_meter.hpp"
#include "adi/engine/preview_through.hpp"
#include "adi/engine/publisher.hpp"
#include "adi/engine/sample_preview.hpp"
#include "adi/mix/filters/FlushDenormals.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
        apply(publication.valid()?publication->parameters.get():nullptr);
        gain_.process(io);
    }
    void silenceTaps(const NodeIo& io) noexcept override {
        SnapshotPublisher<Publication>::AudioRead publication(parameters_);
        apply(publication.valid()?publication->parameters.get():nullptr);
        gain_.silenceTaps(io);
    }
    bool processesDouble() const noexcept override{return true;}
    std::int32_t latencySamples() const noexcept override{return gain_.latencySamples();}
    std::int64_t tailSamples() const noexcept override{return 0;}
    EventFlow eventFlow() const noexcept override{return EventFlow::Stop;}
private:
    void apply(const Parameters* parameters) noexcept {
        if (const auto* value=published_.load(std::memory_order_acquire)) {
            gain_.applyPublishedControls(*value);
            gain_.usePublishedLanes(value->config.lanes);
        } else {
            gain_.usePublishedLanes(parameters && parameters->bound?&parameters->lanes:nullptr);
        }
    }
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
using ListenFilter = dsp::StereoMeterWeighting;
using ListenHistory = std::array<ListenFilter::History, 2>;
struct Buffers {
    int frames,channels;
    std::vector<double> cue,reference,scratch,difference;
    double sampleRate;
    std::array<ListenFilter, 4> weighting;
    ListenHistory narrowHistory{}, wideHistory{}; // audio only
    MainAudition activeFilter = MainAudition::Stereo;
    Buffers(int n,int ch,double rate)
        :frames(n),channels(ch),cue(static_cast<std::size_t>(2)*static_cast<std::size_t>(n)),
         reference(cue.size()),scratch(cue.size()),difference(cue.size()),sampleRate(rate) {
        using Weight = ListenFilter::Weighting;
        weighting[1].prepare(rate, Weight::A);
        weighting[2].prepare(rate, Weight::C);
        weighting[3].prepare(rate, Weight::K);
    }
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
    std::atomic<MainAudition> audition{MainAudition::Stereo};
    SamplePreview* preview=nullptr; // attached while the driver is stopped
    std::atomic<PreviewThroughTrack*> through{nullptr}; // MX-40
    std::map<std::int64_t,std::unique_ptr<Feed>> feeds;
    std::set<std::int64_t> present,selected;
    std::shared_ptr<Buffers> buffers;
    SnapshotPublisher<Scene> scenes;
    CueSink* lastReference=nullptr; // audio only; feeds live for the session
    Ramp referenceMix,volume{1,1,0,0};
    Ramp differenceMix; // MX-38, audio only
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
    if(!impl_->buffers || impl_->buffers->frames!=maxFrames || impl_->buffers->channels!=channels ||
       impl_->buffers->sampleRate!=rate)
        impl_->buffers=std::make_shared<Buffers>(maxFrames,channels,rate);
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
void CueControlBatch::captureLatencies(std::map<const Node*, std::int32_t>& captured, double renderRate) const {
    for (const auto& entry : entries_) {
        const auto& config = entry.image.config;
        captured.emplace(entry.target, StripNode::delayLatency(config.delaySamples, config.delayRate, renderRate));
    }
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
            [](Node* target,const StripControlImage& image) noexcept {static_cast<CueGain*>(target)->published(image);},std::move(value),&found->second->gain.node()});
    }
    return result;
}
void CueMonitor::collect(){impl_->scenes.collect();for(auto& [id,feed]:impl_->feeds){(void)id;feed->gain.collect();}}
void CueMonitor::release() noexcept {
    // A same-format restart gets fresh monitor history. Published scenes keep
    // their own buffers alive through retirement.
    impl_->buffers.reset();
}
void CueMonitor::setMainAudition(MainAudition mode) noexcept {
    impl_->audition.store(mode, std::memory_order_relaxed);
}
void CueMonitor::setSamplePreview(SamplePreview* player) noexcept {
    impl_->preview=player;
}
void CueMonitor::setPreviewThroughTrack(PreviewThroughTrack* node) noexcept {
    impl_->through.store(node,std::memory_order_release);
}
namespace {
template<class T> void auditionStereo(T* const* output, int frames, MainAudition mode) noexcept {
    if (!output || !output[0] || !output[1] || output[0] == output[1]) return;
    auto* left = output[0];
    auto* right = output[1];
    switch (mode) {
    case MainAudition::Left:
        std::copy_n(left, frames, right);
        break;
    case MainAudition::Right:
        std::copy_n(right, frames, left);
        break;
    case MainAudition::Mid:
    case MainAudition::Side: {
        const T sign = mode == MainAudition::Mid ? T(0.5) : T(-0.5);
        for (int i = 0; i < frames; ++i) {
            const auto sample = left[i] * T(0.5) + right[i] * sign;
            left[i] = right[i] = sample;
        }
        break;
    }
    default:
        break;
    }
}
template<class T>
void filterStereo(T* const* output, int frames, const ListenFilter& filter, ListenHistory& history) noexcept {
    if (!output || !output[0] || !output[1] || output[0] == output[1]) return;
    constexpr double listenGain = 0.45;
    for (std::size_t lane = 0; lane < history.size(); ++lane) {
        auto* samples = output[lane];
        if (filter.flat()) {
            for (int i = 0; i < frames; ++i)
                samples[i] = static_cast<T>(samples[i] * listenGain);
        } else {
            auto& state = history[lane];
            for (int i = 0; i < frames; ++i) {
                const double sample = samples[i];
                const auto filtered = filter.process(sample, state);
                if (std::isfinite(filtered)) {
                    samples[i] = static_cast<T>(filtered * listenGain);
                } else {
                    samples[i] = T{};
                    state = {};
                }
            }
        }
    }
}
template<class T>
void moveMainOutput(T* const* output, int frames, int first) noexcept {
    if (!output)
        return;
    auto* left = output[0];
    auto* right = output[1];
    for (int i = 0; i < frames; ++i) {
        const auto l = left ? left[i] : T{};
        const auto r = right ? right[i] : T{};
        if (left) left[i] = T{};
        if (right) right[i] = T{};
        if (output[first]) output[first][i] = l;
        if (output[first + 1]) output[first + 1][i] = r;
    }
}
int packedOutput(const OutputChannelMap& map, int physical) noexcept {
    if (physical < 0 || physical >= OutputChannelMap::maximumChannels)
        return -1;
    const auto bit = std::uint64_t{1} << physical;
    return (map.enabled & bit) != 0 ? std::popcount(map.enabled & (bit - 1)) : -1;
}
template<class T>
void silenceOutput(T* const* output, int frames, int channel) noexcept {
    if (output && output[channel])
        std::fill_n(output[channel], frames, T{});
}
}
void CueMonitor::process(const GraphHost& graph,const AudioIo& io,Click click) noexcept {
    SnapshotPublisher<Scene>::AudioRead scene(impl_->scenes);
    if(!scene.valid() || !scene->enabled || io.frames<=0)return;
    // Graph outputs and Main measurements have already been copied. Only the
    // physical monitor pair changes here, before adding cue/reference/click.
    const auto audition = impl_->audition.load(std::memory_order_relaxed);
    auto& buffers=*scene->buffers;
    // MX-38 (ADR-0254 d23): Hear Difference takes the physical Main pair, with
    // the reference's five-ms complementary swap, before the Main auditions so
    // they apply to it too. Main's meters, Cue Out and every render keep Main.
    const bool differenceOn = graph.differenceArmed() && !io.exporting;
    if ((differenceOn || impl_->differenceMix.current != 0) && io.frames <= buffers.frames) {
        const auto stride = static_cast<std::size_t>(buffers.frames);
        double* difference[]{buffers.difference.data(), buffers.difference.data() + stride};
        AudioIo copy;copy.out64=difference;copy.numOut=2;copy.frames=io.frames;
        if (!graph.copyRenderedDifference(copy))
            for (auto* lane : difference) std::fill_n(lane, static_cast<std::size_t>(io.frames), 0.0);
        else if (buffers.channels == 1)
            std::copy_n(difference[0], static_cast<std::size_t>(io.frames), difference[1]);
        impl_->differenceMix.set(differenceOn ? 1.0 : 0.0, scene->ramp);
        const int pair = std::min(io.numOut, 2);
        for (int i = 0; i < io.frames; ++i) {
            const double amount = impl_->differenceMix.next();
            const auto at = static_cast<std::size_t>(i);
            for (int c = 0; c < pair; ++c) {
                const double d = difference[c][at];
                if (io.out && io.out[c])
                    io.out[c][i] = static_cast<float>(static_cast<double>(io.out[c][i]) * (1 - amount) + d * amount);
                if (io.out64 && io.out64[c]) io.out64[c][i] = io.out64[c][i] * (1 - amount) + d * amount;
            }
        }
    }
    if (audition != MainAudition::Stereo && !io.exporting && io.numOut >= 2 && buffers.channels >= 2) {
        if (isFilterListen(audition)) {
            if (buffers.activeFilter != audition) {
                buffers.narrowHistory = {};
                buffers.wideHistory = {};
                buffers.activeFilter = audition;
            }
            const auto index = static_cast<std::size_t>(static_cast<int>(audition) - static_cast<int>(MainAudition::FilterFlat));
            const auto& filter = buffers.weighting[index];
            [[maybe_unused]] mix::filters::FlushDenormals flush;
            filterStereo(io.out, io.frames, filter, buffers.narrowHistory);
            filterStereo(io.out64, io.frames, filter, buffers.wideHistory);
        } else {
            buffers.activeFilter = MainAudition::Stereo;
            auditionStereo(io.out, io.frames, audition);
            auditionStereo(io.out64, io.frames, audition);
        }
    } else if (!io.exporting && buffers.activeFilter != MainAudition::Stereo) {
        buffers.activeFilter = MainAudition::Stereo;
    }
    int mainOut = 0;
    bool mainHeard[2]{true, true};
    if (!io.exporting && buffers.channels == 2) {
        const int physicalMain = scene->settings.firstMainOutputChannel;
        const int packedMain = io.packedOutputPair(physicalMain);
        if (packedMain > 0) {
            mainOut = packedMain;
            moveMainOutput(io.out, io.frames, mainOut);
            moveMainOutput(io.out64, io.frames, mainOut);
        } else if (packedMain < 0) {
            // The graph wrote Main into the first two packed buffers. With an incomplete
            // hardware pair, keep only channels that already carry their own Main output.
            for (int c = 0; c < 2; ++c) {
                mainHeard[c] = c < io.numOut && packedOutput(io.outputChannels, physicalMain + c) == c;
                if (!mainHeard[c] && c < io.numOut) {
                    silenceOutput(io.out, io.frames, c);
                    silenceOutput(io.out64, io.frames, c);
                }
            }
        }
    }
    const auto physicalFirst=scene->settings.firstOutputChannel;
    const auto first=io.packedOutputPair(physicalFirst);
    auto* preview=impl_->preview;
    // MX-40: a callback in which a track played the preview leaves it off Cue Out.
    auto* through=impl_->through.load(std::memory_order_acquire);
    const bool routed=through!=nullptr && through->takeMixed();
    const bool sampleActive=!routed && preview && preview->active() && SamplePreview::canOutput(io,physicalFirst);
    // Checked vector iterators can take a lock on the audio thread in MSVC Debug.
    const auto* voices=scene->voices.data();
    const auto voiceCount=scene->voices.size();
    bool cueActive=false;
    for(std::size_t voiceIndex=0;voiceIndex<voiceCount && !cueActive;++voiceIndex)
        cueActive=voices[voiceIndex].selected || voices[voiceIndex].gate->current!=0;
    if(!cueActive && !scene->settings.referenceOn && impl_->referenceMix.current==0 && !click.render && !sampleActive) {
        impl_->volume={scene->gain,scene->gain,0,0};
        return;
    }
    if(io.frames>buffers.frames)return;
    const auto frames=static_cast<std::size_t>(io.frames),stride=static_cast<std::size_t>(buffers.frames);
    double* cue[]{buffers.cue.data(),buffers.cue.data()+stride};
    double* reference[]{buffers.reference.data(),buffers.reference.data()+stride};
    double* scratch[]{buffers.scratch.data(),buffers.scratch.data()+stride};
    for(int c=0;c<2;++c){std::fill_n(cue[c],frames,0.0);std::fill_n(reference[c],frames,0.0);}
    AudioIo copy;copy.out64=scratch;copy.numOut=2;copy.frames=io.frames;
    for(std::size_t voiceIndex=0;voiceIndex<voiceCount;++voiceIndex) {
        const auto& voice=voices[voiceIndex];
        voice.gate->set(voice.selected?1.0:0.0,scene->ramp);
        if(voice.gate->current==0 && !voice.selected)continue;
        const bool copied=graph.copyRenderedOutput(*voice.node,copy);
        for(std::size_t i=0;i<frames;++i) {
            const double gate=voice.gate->next();
            if(copied)for(int c=0;c<2;++c)cue[c][i]+=scratch[buffers.channels==1?0:c][i]*gate;
        }
    }
    if(click.render || sampleActive) {
        AudioIo cueIo=io;cueIo.out=nullptr;cueIo.out64=cue;cueIo.numOut=2;
        cueIo.outputChannels={}; // local stereo bus, not the physical device
        if(click.render)click.render(click.context,cueIo);
        if(sampleActive)preview->mix(cueIo,buffers.sampleRate,0,1.);
    }
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
    auto add=[&](int channel,int i,double value){
        if(channel>=io.numOut || value==0)return;
        if(io.out && io.out[channel])io.out[channel][i]+=static_cast<float>(value);
        if(io.out64 && io.out64[channel])io.out64[channel][i]+=value;
    };
    for(int i=0;i<io.frames;++i) {
        if(wantReference && i==readyAt)impl_->referenceMix.set(1.0,scene->ramp);
        const double ref=impl_->referenceMix.next(),main=1-ref,gain=impl_->volume.next();
        for(int c=mainOut;main!=1 && c<std::min(mainOut + buffers.channels,io.numOut);++c) {
            if(c-mainOut<2 && !mainHeard[c-mainOut])continue;
            if(io.out && io.out[c])io.out[c][i]*=static_cast<float>(main);
            if(io.out64 && io.out64[c])io.out64[c][i]*=main;
        }
        for(int c=0;c<2;++c) {
            const auto index=static_cast<std::size_t>(i);
            const double r=reference[c][index]*ref*gain;
            if(mainHeard[c])add(mainOut + c, i, r);
            // An output shared with Main already has the reference. Cue Out
            // also receives it on its separate outputs, past Main's chain.
            if (first >= 0) {
                const int cueChannel = first + c;
                const bool onMain = cueChannel >= mainOut && cueChannel < mainOut + 2 && mainHeard[cueChannel - mainOut];
                add(cueChannel, i, cue[c][index] * main * gain + (onMain ? 0 : r));
            }
        }
    }
}
}
