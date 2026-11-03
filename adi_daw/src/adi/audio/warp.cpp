// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/warp.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/transients.hpp"
#include <numbers>
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include <rubberband/RubberBandStretcher.h>
#include <samplerate.h>
#include <exception>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace adi::audio {
struct WarpPreparation {
    static std::shared_ptr<PreparedWarp> make(const std::filesystem::path& directory,std::uint32_t rate,std::uint16_t channels,std::uint64_t frames) {
        return std::shared_ptr<PreparedWarp>(new PreparedWarp(directory,rate,channels,frames));
    }
};
namespace {
namespace fs=std::filesystem;
constexpr std::uint32_t blockFrames=4096;
constexpr std::uint64_t maxFrames=(std::uint64_t{1}<<31)-1;
struct Failure {WarpError code;const char* text;};
void cancelled(std::stop_token stop){if(stop.stop_requested())throw Failure{WarpError::Cancelled,"warp cancelled"};}
struct Decoded {
    fs::path file;bool pinned=false;
    explicit Decoded(const fs::path& source):file(playableFile(source)),pinned(file!=source){}
    ~Decoded(){if(pinned)try{defaultDecodeCache().release(file);}catch(...){}}
};
std::shared_ptr<PreparedWarp> stage(const WarpRenderPlan& plan,std::uint16_t channels) {
    if(plan.workFolder.empty())throw Failure{WarpError::InvalidPlan,"warp needs a private work folder"};
    fs::create_directories(plan.workFolder);
    static std::atomic<std::uint64_t> serial{0};
    for(unsigned attempt=0;attempt<128;++attempt) {
        const auto folder=plan.workFolder/("warp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial.fetch_add(1)));
        std::error_code error;
        if(fs::create_directory(folder,error)) {
            try{return WarpPreparation::make(folder,plan.outputRate,channels,plan.anchors.back().outputFrame);}
            catch(...){std::error_code cleanup;fs::remove(folder,cleanup);throw;}
        }
        if(error && error!=std::errc::file_exists)throw fs::filesystem_error("warp staging",folder,error);
    }
    throw Failure{WarpError::Io,"cannot reserve warp output"};
}
void validate(const WarpRenderPlan& plan,const WavReader& reader) {
    if(plan.sourceFrames==0 || plan.sourceFrames>maxFrames || plan.sourceStart>reader.frames() ||
       plan.sourceFrames>reader.frames()-plan.sourceStart || reader.channels()==0 || reader.channels()>64 ||
       reader.sampleRate()<8000 || reader.sampleRate()>768000 || plan.outputRate<44100 || plan.outputRate>768000 ||
       plan.anchors.size()<2 || plan.anchors.size()>1000000 || !std::isfinite(plan.transposeSemis) ||
       plan.transposeSemis<-96 || plan.transposeSemis>96 || !std::isfinite(plan.formantScale) ||
       plan.formantScale<=0 || plan.formantScale>8)
        throw Failure{WarpError::InvalidPlan,"invalid warp source, rate, pitch or anchor count"};
    if(plan.anchors.front()!=WarpAnchor{0,0} || plan.anchors.back().sourceFrame!=plan.sourceFrames ||
       plan.anchors.back().outputFrame==0 || plan.anchors.back().outputFrame>maxFrames)
        throw Failure{WarpError::InvalidPlan,"warp anchors must cover the complete source and output"};
    for(std::size_t index=1;index<plan.anchors.size();++index)
        if(plan.anchors[index].sourceFrame<=plan.anchors[index-1].sourceFrame ||
           plan.anchors[index].outputFrame<=plan.anchors[index-1].outputFrame)
            throw Failure{WarpError::InvalidPlan,"warp anchors must increase in both clocks"};
    if(plan.transients.size()>1000000)throw Failure{WarpError::InvalidPlan,"too many warp onsets"};
    for(const auto frame:plan.transients)if(frame>=plan.sourceFrames)throw Failure{WarpError::InvalidPlan,"warp onset lies outside its source"};
}
struct RePitchInput {
    WavReader& reader;
    const WarpRenderPlan& plan;
    std::stop_token stop;
    std::vector<float> buffer;
    std::uint64_t position=0,padding=0;
    std::exception_ptr error;
    RePitchInput(WavReader& source,const WarpRenderPlan& spec,std::stop_token token)
        :reader(source),plan(spec),stop(token),buffer(static_cast<std::size_t>(blockFrames)*source.channels()) {
        double largest=1;
        for(std::size_t index=1;index<plan.anchors.size();++index)
            largest=std::max(largest,static_cast<double>(plan.anchors[index].sourceFrame-plan.anchors[index-1].sourceFrame)/
                static_cast<double>(plan.anchors[index].outputFrame-plan.anchors[index-1].outputFrame));
        padding=static_cast<std::uint64_t>(std::ceil(largest))*2048;
    }
    static long supply(void* opaque,float** samples)noexcept {
        auto& state=*static_cast<RePitchInput*>(opaque);
        *samples=state.buffer.data();
        if(state.stop.stop_requested())return 0;
        try {
            if(state.position<state.plan.sourceFrames) {
                const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,state.plan.sourceFrames-state.position));
                if(state.reader.read(state.buffer.data(),count)!=count)throw Failure{WarpError::Io,"short Re-Pitch source read"};
                for(std::size_t index=0;index<static_cast<std::size_t>(count)*state.reader.channels();++index)
                    if(!std::isfinite(state.buffer[index]))state.buffer[index]=0;
                state.position+=count;return static_cast<long>(count);
            }
            const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,state.padding));
            std::fill(state.buffer.begin(),state.buffer.end(),0);state.padding-=count;return static_cast<long>(count);
        } catch(...){state.error=std::current_exception();return 0;}
    }
};
void renderRePitch(const WarpRenderPlan& plan,WavReader& reader,WavWriter& writer,std::stop_token stop) {
    if(plan.transposeSemis!=0)throw Failure{WarpError::InvalidPlan,"Re-Pitch derives pitch from the warp rate"};
    for(std::size_t index=1;index<plan.anchors.size();++index) {
        const double ratio=static_cast<double>(plan.anchors[index].outputFrame-plan.anchors[index-1].outputFrame)/
            static_cast<double>(plan.anchors[index].sourceFrame-plan.anchors[index-1].sourceFrame);
        if(!src_is_valid_ratio(ratio))throw Failure{WarpError::InvalidPlan,"Re-Pitch segment ratio is outside the resampler range"};
    }
    reader.seek(plan.sourceStart);RePitchInput input(reader,plan,stop);int error=0;
    auto* raw=src_callback_new(&RePitchInput::supply,SRC_SINC_BEST_QUALITY,reader.channels(),&error,&input);
    if(!raw)throw Failure{WarpError::Process,"cannot prepare Re-Pitch resampler"};
    const std::unique_ptr<SRC_STATE,decltype(&src_delete)> state(raw,&src_delete);
    std::vector<float> output(static_cast<std::size_t>(blockFrames)*reader.channels());
    for(std::size_t index=1;index<plan.anchors.size();++index) {
        const auto frames=plan.anchors[index].outputFrame-plan.anchors[index-1].outputFrame;
        const double ratio=static_cast<double>(frames)/
            static_cast<double>(plan.anchors[index].sourceFrame-plan.anchors[index-1].sourceFrame);
        // No ratio ramp: each explicit anchor is a change of tape speed.
        if(src_set_ratio(state.get(),ratio)!=0)throw Failure{WarpError::Process,"cannot set Re-Pitch ratio"};
        for(std::uint64_t position=0;position<frames;) {
            cancelled(stop);const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,frames-position));
            const auto received=src_callback_read(state.get(),ratio,static_cast<long>(count),output.data());
            cancelled(stop);if(input.error)std::rethrow_exception(input.error);
            if(received!=static_cast<long>(count))throw Failure{WarpError::Process,"Re-Pitch output ended before its declared duration"};
            writer.write(output.data(),count);position+=count;
        }
    }
}

struct NativeSource {
    WavReader& reader;const WarpRenderPlan& plan;
    std::vector<float> page;
    std::uint64_t first=std::numeric_limits<std::uint64_t>::max();std::uint32_t count=0;
    NativeSource(WavReader& source,const WarpRenderPlan& spec):reader(source),plan(spec),
        page(static_cast<std::size_t>(blockFrames+1)*source.channels()){}
    float sample(double position,std::uint16_t channel) {
        if(!(position>=0) || position>=static_cast<double>(plan.sourceFrames))return 0;
        const auto frame=static_cast<std::uint64_t>(position),start=frame/blockFrames*blockFrames;
        if(first!=start) {
            count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames+1,plan.sourceFrames-start));
            reader.seek(plan.sourceStart+start);
            if(reader.read(page.data(),count)!=count)throw Failure{WarpError::Io,"short native warp source read"};
            first=start;
        }
        const auto local=static_cast<std::size_t>(frame-first);const auto channels=reader.channels();
        const double a=page[local*channels+channel],b=local+1<count?page[(local+1)*channels+channel]:0;
        const double fraction=position-static_cast<double>(frame);
        return static_cast<float>((std::isfinite(a)?a:0)*(1-fraction)+(std::isfinite(b)?b:0)*fraction);
    }
};
double mapped(const std::vector<WarpAnchor>& anchors,double position,bool sourceToOutput) {
    auto from=[&](const WarpAnchor& point){return sourceToOutput?point.sourceFrame:point.outputFrame;};
    auto to=[&](const WarpAnchor& point){return sourceToOutput?point.outputFrame:point.sourceFrame;};
    const auto next=std::upper_bound(anchors.begin(),anchors.end(),position,[&](double at,const WarpAnchor& point){return at<static_cast<double>(from(point));});
    const auto index=next==anchors.begin()?std::size_t{0}:next==anchors.end()?anchors.size()-2:static_cast<std::size_t>(next-anchors.begin()-1);
    const auto& a=anchors[index];const auto& b=anchors[index+1];
    return static_cast<double>(to(a))+(position-static_cast<double>(from(a)))*
        static_cast<double>(to(b)-to(a))/static_cast<double>(from(b)-from(a));
}
void renderBeats(const WarpRenderPlan& plan,WavReader& reader,WavWriter& writer,std::stop_token stop) {
    if(!std::isfinite(plan.transientEnvelope) || plan.transientEnvelope<0 || plan.transientEnvelope>1 ||
       (plan.transientLoop!=TransientLoop::Off && plan.transientLoop!=TransientLoop::Forward && plan.transientLoop!=TransientLoop::PingPong))
        throw Failure{WarpError::InvalidPlan,"invalid Beats envelope or transient loop"};
    auto onsets=plan.transients;
    if(onsets.empty()) {
        const auto detected=detectTransients(reader,plan.sourceStart,plan.sourceFrames,50,stop);
        cancelled(stop);if(!detected)throw Failure{WarpError::Process,"cannot read the Beats onset map"};onsets=detected.frames;
    }
    for(const auto onset:onsets)if(onset>=plan.sourceFrames)throw Failure{WarpError::InvalidPlan,"Beats onset lies outside its source"};
    for(const auto& anchor:plan.anchors)onsets.push_back(anchor.sourceFrame);
    std::sort(onsets.begin(),onsets.end());onsets.erase(std::unique(onsets.begin(),onsets.end()),onsets.end());
    std::vector<WarpAnchor> slices;
    for(const auto onset:onsets) {
        const auto out=static_cast<std::uint64_t>(std::llround(mapped(plan.anchors,static_cast<double>(onset),true)));
        if(slices.empty() || out>slices.back().outputFrame)slices.push_back({onset,out});
    }
    if(slices.size()<2 || slices.back()!=plan.anchors.back())throw Failure{WarpError::InvalidPlan,"Beats boundaries are below output sample precision"};
    NativeSource source(reader,plan);std::vector<float> output(static_cast<std::size_t>(blockFrames)*reader.channels());
    const double pitch=std::exp2(plan.transposeSemis/12);
    std::size_t slice=0;
    for(std::uint64_t first=0;first<plan.anchors.back().outputFrame;) {
        cancelled(stop);const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,plan.anchors.back().outputFrame-first));
        for(std::uint32_t frame=0;frame<count;++frame) {
            const auto position=first+frame;
            while(slice+2<slices.size() && slices[slice+1].outputFrame<=position)++slice;
            const auto& a=slices[slice];const auto& b=slices[slice+1];
            const double local=static_cast<double>(position-a.outputFrame)*pitch,span=static_cast<double>(b.sourceFrame-a.sourceFrame);
            double readAt=local;
            if(readAt>=span) {
                if(plan.transientLoop==TransientLoop::Off)readAt=-1;
                else {
                    const double begin=std::min(span-1,std::max(1.0,plan.outputRate*0.01)),body=span-begin;
                    if(plan.transientLoop==TransientLoop::Forward)readAt=begin+std::fmod(readAt-begin,body);
                    else if(body<=1)readAt=begin;
                    else {const double phase=std::fmod(readAt-begin,2*(body-1));readAt=begin+(phase<body?phase:2*(body-1)-phase);}
                }
            }
            const double gain=std::exp(-12*(1-plan.transientEnvelope)*local/std::max(1.0,span));
            for(std::uint16_t channel=0;channel<reader.channels();++channel)
                output[static_cast<std::size_t>(frame)*reader.channels()+channel]=readAt<0?0:static_cast<float>(source.sample(static_cast<double>(a.sourceFrame)+readAt,channel)*gain);
        }
        writer.write(output.data(),count);first+=count;
    }
}
std::uint64_t grainRandom(std::uint64_t seed,std::int64_t index)noexcept {
    std::uint64_t value=seed+static_cast<std::uint64_t>(index)*0x9e3779b97f4a7c15ULL;
    value=(value^(value>>30))*0xbf58476d1ce4e5b9ULL;value=(value^(value>>27))*0x94d049bb133111ebULL;return value^(value>>31);
}
void renderTexture(const WarpRenderPlan& plan,WavReader& reader,WavWriter& writer,std::stop_token stop) {
    if(!std::isfinite(plan.grainSeconds) || plan.grainSeconds<0.001 || plan.grainSeconds>1 ||
       !std::isfinite(plan.fluctuation) || plan.fluctuation<0 || plan.fluctuation>1)
        throw Failure{WarpError::InvalidPlan,"invalid Texture grain size or fluctuation"};
    const auto grain=std::max<std::int64_t>(4,std::llround(plan.grainSeconds*plan.outputRate));
    const auto hop=std::max<std::int64_t>(1,grain/4),half=grain/2;
    const double pitch=std::exp2(plan.transposeSemis/12);NativeSource source(reader,plan);
    std::vector<double> mixed(static_cast<std::size_t>(blockFrames)*reader.channels()),weights(blockFrames);
    std::vector<float> output(mixed.size());
    for(std::uint64_t first=0;first<plan.anchors.back().outputFrame;) {
        cancelled(stop);const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,plan.anchors.back().outputFrame-first));
        std::fill(mixed.begin(),mixed.end(),0);std::fill(weights.begin(),weights.end(),0);
        const auto start=static_cast<std::int64_t>(first),end=start+count;
        const auto firstGrain=static_cast<std::int64_t>(std::floor(static_cast<double>(start-half)/static_cast<double>(hop)));
        const auto lastGrain=(end+half)/hop;
        for(auto index=firstGrain;index<=lastGrain;++index) {
            cancelled(stop);const auto center=index*hop,left=center-half,right=left+grain;
            const auto begin=std::max(start,left),finish=std::min(end,right);
            if(finish<=begin)continue;
            const double jitter=(static_cast<double>(grainRandom(plan.seed,index)>>11)/0x1p53-0.5)*2*plan.fluctuation*static_cast<double>(grain);
            const double sourceCenter=mapped(plan.anchors,static_cast<double>(center),false)+jitter;
            for(auto position=begin;position<finish;++position) {
                const double phase=static_cast<double>(position-left)/static_cast<double>(grain);
                const double weight=0.5-0.5*std::cos(2*std::numbers::pi*phase);
                const auto local=static_cast<std::size_t>(position-start);weights[local]+=weight;
                const double readAt=sourceCenter+static_cast<double>(position-center)*pitch;
                for(std::uint16_t channel=0;channel<reader.channels();++channel)
                    mixed[local*reader.channels()+channel]+=source.sample(readAt,channel)*weight;
            }
        }
        for(std::uint32_t frame=0;frame<count;++frame)for(std::uint16_t channel=0;channel<reader.channels();++channel) {
            const auto local=static_cast<std::size_t>(frame)*reader.channels()+channel;
            output[local]=weights[frame]>0?static_cast<float>(mixed[local]/weights[frame]):0;
        }
        writer.write(output.data(),count);first+=count;
    }
}

void renderR3(const WarpRenderPlan& plan,WavReader& reader,WavWriter& writer,std::stop_token stop) {
    using Stretcher=RubberBand::RubberBandStretcher;
    auto options=Stretcher::OptionProcessOffline|Stretcher::OptionEngineFiner|Stretcher::OptionThreadingNever;
    if(plan.mode==WarpMode::Tones) {
        if(!std::isfinite(plan.grainSeconds) || plan.grainSeconds<0.001 || plan.grainSeconds>1)
            throw Failure{WarpError::InvalidPlan,"invalid Tones grain size"};
        if(plan.grainSeconds<=0.05)options|=Stretcher::OptionWindowShort;
    }
    if(plan.mode==WarpMode::ComplexPro)options|=Stretcher::OptionFormantPreserved;
    const double rateScale=static_cast<double>(reader.sampleRate())/plan.outputRate;
    const auto outputFrames=plan.anchors.back().outputFrame;
    Stretcher stretcher(reader.sampleRate(),reader.channels(),options,
        static_cast<double>(outputFrames)/static_cast<double>(plan.sourceFrames),std::exp2(plan.transposeSemis/12)*rateScale);
    if(stretcher.getEngineVersion()!=3)throw Failure{WarpError::Process,"R3 engine unavailable"};
    stretcher.setDebugLevel(0);
    // R3 applies this envelope correction before its pitch resampler.
    // Correct only the musical transpose: file-rate conversion is already
    // countered by pitchScale, so multiplying by rateScale here would move
    // the formants when the same file is played at a different session rate.
    if(plan.mode==WarpMode::ComplexPro)stretcher.setFormantScale(plan.formantScale/std::exp2(plan.transposeSemis/12));
    stretcher.setExpectedInputDuration(static_cast<std::size_t>(plan.sourceFrames));
    stretcher.setMaxProcessSize(blockFrames);
    std::map<std::size_t,std::size_t> anchors;
    // R3 derives its first ratio from its first key-frame pair. The origin
    // 0->0 would divide by zero; overall duration supplies both endpoints.
    for(std::size_t index=1;index+1<plan.anchors.size();++index) {
        const auto& point=plan.anchors[index];anchors.emplace(static_cast<std::size_t>(point.sourceFrame),static_cast<std::size_t>(point.outputFrame));
    }
    stretcher.setKeyFrameMap(anchors);
    const auto channels=reader.channels();
    std::vector<float> interleaved(static_cast<std::size_t>(blockFrames)*channels);
    std::vector<std::vector<float>> planes(channels,std::vector<float>(blockFrames));
    std::vector<const float*> input(channels);std::vector<float*> output(channels);
    for(std::size_t channel=0;channel<channels;++channel){input[channel]=planes[channel].data();output[channel]=planes[channel].data();}
    auto read=[&](std::uint32_t frames) {
        cancelled(stop);
        if(reader.read(interleaved.data(),frames)!=frames)throw Failure{WarpError::Io,"short warp source read"};
        for(std::uint32_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<channels;++channel)
            planes[channel][frame]=std::isfinite(interleaved[static_cast<std::size_t>(frame)*channels+channel])?interleaved[static_cast<std::size_t>(frame)*channels+channel]:0;
    };
    reader.seek(plan.sourceStart);
    for(std::uint64_t position=0;position<plan.sourceFrames;) {
        const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,plan.sourceFrames-position));read(count);position+=count;
        stretcher.study(input.data(),count,position==plan.sourceFrames);
    }
    reader.seek(plan.sourceStart);std::uint64_t written=0;
    auto drain=[&]() {
        for(int available=stretcher.available();available>0;available=stretcher.available()) {
            cancelled(stop);const auto requested=std::min<std::size_t>(blockFrames,static_cast<std::size_t>(available));
            const auto count=stretcher.retrieve(output.data(),requested);
            if(count==0 || count>requested)throw Failure{WarpError::Process,"R3 output stalled"};
            if(count>outputFrames-written)throw Failure{WarpError::Process,"R3 output exceeds its declared duration"};
            for(std::size_t frame=0;frame<count;++frame)for(std::size_t channel=0;channel<channels;++channel)
                interleaved[frame*channels+channel]=planes[channel][frame];
            writer.write(interleaved.data(),static_cast<std::uint32_t>(count));written+=count;
        }
    };
    for(std::uint64_t position=0;position<plan.sourceFrames;) {
        const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames,plan.sourceFrames-position));read(count);position+=count;
        stretcher.process(input.data(),count,position==plan.sourceFrames);drain();
    }
    drain();
    if(written!=outputFrames || stretcher.available()!=-1)throw Failure{WarpError::Process,"R3 did not produce its declared duration"};
}
}
const char* warpModeName(WarpMode mode)noexcept {
    switch(mode) {
        case WarpMode::Beats:return "beats";
        case WarpMode::Tones:return "tones";
        case WarpMode::Texture:return "texture";
        case WarpMode::RePitch:return "re-pitch";
        case WarpMode::Complex:return "complex";
        case WarpMode::ComplexPro:return "complexPro";
        default:return nullptr;
    }
}
std::optional<WarpMode> parseWarpMode(std::string_view name)noexcept {
    for(const auto mode:{WarpMode::Beats,WarpMode::Tones,WarpMode::Texture,WarpMode::RePitch,WarpMode::Complex,WarpMode::ComplexPro})
        if(name==warpModeName(mode))return mode;
    return std::nullopt;
}

PreparedWarp::PreparedWarp(std::filesystem::path directory,std::uint32_t sampleRate,std::uint16_t channelCount,std::uint64_t frameCount)
    :file(directory/"prepared.wav"),rate(sampleRate),channels(channelCount),frames(frameCount),directory_(std::move(directory)){}
PreparedWarp::~PreparedWarp() {
    std::error_code error;std::filesystem::remove_all(directory_,error);
}
WarpResult prepareWarp(const WarpRenderPlan& plan,std::stop_token stop) {
    WarpResult result;
    if(onAudioThread){result.error=WarpError::Realtime;return result;}
    try {
        cancelled(stop);

        Decoded decoded(plan.source);std::shared_ptr<PreparedWarp> converted;
        auto reader=std::make_unique<WavReader>(decoded.file);validate(plan,*reader);
        auto working=plan;
        if(working.mode==WarpMode::Beats && working.transients.empty()) {
            const auto detected=detectTransients(*reader,working.sourceStart,working.sourceFrames,50,stop);
            cancelled(stop);if(!detected)throw Failure{WarpError::Process,"cannot read the source onset map"};
            working.transients=detected.frames;
            // Detect on the original file grid. Resampling first would let
            // sinc pre-ringing move the detected attack ahead of its source.
            if(working.transients.empty() || working.transients.front()!=0)working.transients.insert(working.transients.begin(),0);
        }
        if((plan.mode==WarpMode::Beats || plan.mode==WarpMode::Texture) && reader->sampleRate()!=plan.outputRate) {
            const long double frames=static_cast<long double>(plan.sourceFrames)*plan.outputRate/reader->sampleRate();
            if(frames<0.5L || frames>maxFrames)throw Failure{WarpError::InvalidPlan,"native warp source conversion is outside supported duration"};
            const auto convertedFrames=static_cast<std::uint64_t>(std::llround(frames));auto conversion=plan;
            conversion.mode=WarpMode::RePitch;conversion.transposeSemis=0;conversion.anchors={{0,0},{plan.sourceFrames,convertedFrames}};
            converted=stage(conversion,reader->channels());
            {WavWriter writer(converted->file,converted->rate,converted->channels,WavFormat::Float32);renderRePitch(conversion,*reader,writer,stop);writer.close();}
            auto sourceFrame=[&](std::uint64_t frame){return static_cast<std::uint64_t>(std::llround(static_cast<long double>(frame)*plan.outputRate/reader->sampleRate()));};
            for(auto& anchor:working.anchors)anchor.sourceFrame=sourceFrame(anchor.sourceFrame);
            for(auto& onset:working.transients)onset=sourceFrame(onset);
            working.transients.erase(std::remove_if(working.transients.begin(),working.transients.end(),[&](auto frame){return frame>=convertedFrames;}),working.transients.end());
            working.sourceStart=0;working.sourceFrames=convertedFrames;
            reader=std::make_unique<WavReader>(converted->file);validate(working,*reader);
        }
        auto prepared=stage(working,reader->channels());
        {
            WavWriter writer(prepared->file,prepared->rate,prepared->channels,WavFormat::Float32);
            switch(working.mode) {
                case WarpMode::Beats:renderBeats(working,*reader,writer,stop);break;
                case WarpMode::Texture:renderTexture(working,*reader,writer,stop);break;
                case WarpMode::RePitch:renderRePitch(working,*reader,writer,stop);break;
                case WarpMode::Tones:
                case WarpMode::Complex:
                case WarpMode::ComplexPro:renderR3(working,*reader,writer,stop);break;
                default:throw Failure{WarpError::UnsupportedMode,"unknown warp mode"};
            }
            cancelled(stop);writer.close();
        }
        result.prepared=std::move(prepared);
    } catch(const Failure& failure){result.error=failure.code;result.detail=failure.text;}
      catch(const std::exception& failure){result.error=WarpError::Io;result.detail=failure.what();}
    return result;
}
} // namespace adi::audio
