// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/clip_playback.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <samplerate.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>
#include <thread>
#include <stdexcept>
#include <unordered_map>

namespace adi::engine {
namespace fs = std::filesystem;
namespace { fs::path utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(),s.end())); } }
struct ClipProject {
    rows::Model rows;
    fs::path folder;
    std::unordered_map<std::int64_t, fs::path> hints;
};
std::shared_ptr<const ClipProject> readClipProject(const Store& store, const rows::Model& rows) {
    auto p = std::make_shared<ClipProject>(); p->rows = rows; p->folder = store.path().parent_path();
    SQLite::Statement q(store.db(), "SELECT id,abs_path_hint FROM media_files WHERE abs_path_hint IS NOT NULL");
    while (q.executeStep()) p->hints.emplace(q.getColumn(0).getInt64(), utf8Path(q.getColumn(1).getString()));
    return p;
}
namespace {
constexpr std::int64_t pageFrames = 8192;
constexpr std::size_t slots = 8;
static_assert(std::atomic<std::int64_t>::is_always_lock_free);
// Clip and sample arithmetic is checked off-thread; leave generous headroom
// for callback offsets, loop arithmetic and conversion guards.
constexpr std::int64_t limit = std::int64_t{1} << 50;
std::int64_t samples(double seconds, double rate) {
    const double n = seconds * rate;
    if (!std::isfinite(n) || n < 0 || n > static_cast<double>(limit)) throw std::runtime_error("time outside supported sample range");
    return static_cast<std::int64_t>(std::llround(n));
}
float fade(float x, std::int64_t curve) noexcept {
    x = std::clamp(x, 0.0f, 1.0f);
    switch (curve) {
    case 0: return x >= 1 ? 1.0f : 0.0f;
    case 2: return x*x;
    case 3: return std::sqrt(x);
    case 4: return x*x*(3.0f-2.0f*x);
    default: return x;
    }
}
struct Page {
    // 0 empty, 1 worker-owned, 2 ready, 3 reader-owned. Both sides must
    // acquire ownership BEFORE reading even the page number; no seqlock over
    // non-atomic samples, no concurrent overwrite and no audio-side retry loop.
    std::atomic<int> state{0};
    std::atomic<std::int64_t> number{-1};
    std::vector<float> data;
};
struct Clip {
    std::int64_t track = 0, start = 0, end = 0, offset = 0, length = 0;
    std::int64_t loopBegin = 0, loopLength = 0, fadeIn = 0, fadeOut = 0;
    std::int64_t inCurve = 1, outCurve = 1;
    std::uint64_t sourceStart = 0, sourceLength = 0;
    std::uint32_t sourceRate = 0, rate = 0;
    std::uint16_t channels = 0;
    int mode = 0;
    float gain = 1;
    fs::path path;
    std::unique_ptr<audio::WavReader> reader; // touched ONLY by worker after construction
    std::array<Page, slots> pages;
    std::array<std::atomic<std::int64_t>, slots> requests;
    std::atomic<std::uint32_t> requestSeq{0}, underruns{0}, errors{0};
    Clip() { for (auto& r : requests) r.store(-1); }
    std::int64_t sourceAt(std::int64_t time) const noexcept {
        if (time < start || time >= end) return -1;
        auto n = time-start+offset;
        if (loopLength > 0 && n >= loopBegin) n = loopBegin+(n-loopBegin)%loopLength;
        return n < length ? n : -1;
    }
    float envelope(std::int64_t time) const noexcept {
        float g = gain;
        if (fadeIn > 0) g *= fade(static_cast<float>(time-start)/static_cast<float>(fadeIn),inCurve);
        if (fadeOut > 0) g *= fade(static_cast<float>(end-1-time)/static_cast<float>(fadeOut),outCurve);
        return g;
    }
    std::array<std::int64_t,slots> wanted(const Transport& t, std::int32_t frames) const noexcept {
        std::array<std::int64_t,slots> out; out.fill(-1);
        if (!t.playing()) return out;
        std::size_t used = 0;
        for (std::int32_t i=0; i<frames*2; ++i) {
            const auto n = sourceAt(t.sampleAt(i));
            if (n < 0) continue;
            const auto p = n/pageFrames;
            if (std::find(out.begin(),out.end(),p)==out.end() && used<slots) out[used++]=p;
        }
        // Read ahead beyond the two requested callbacks while preserving all
        // discontinuity pages (locate, transport loop AND clip loop).
        const auto mandatory = used;
        for (std::size_t i=0;i<mandatory && used<slots;++i) {
            const auto next=out[i]+1;
            if (next*pageFrames<length && std::find(out.begin(),out.end(),next)==out.end()) out[used++]=next;
        }
        return out;
    }
    void request(const std::array<std::int64_t,slots>& w) noexcept {
        requestSeq.fetch_add(1,std::memory_order_seq_cst);
        for(std::size_t i=0;i<slots;++i) requests[i].store(w[i],std::memory_order_seq_cst);
        requestSeq.fetch_add(1,std::memory_order_seq_cst);
    }
    Page* acquire(std::int64_t number) noexcept {
        for (auto& p : pages) {
            int ready=2;
            if (!p.state.compare_exchange_strong(ready,3,std::memory_order_acquire)) continue;
            if(p.number==number) return &p;
            p.state.store(2,std::memory_order_release);
        }
        return nullptr;
    }
    void fill(Page& p, std::int64_t number) {
        const auto first=number*pageFrames;
        std::fill(p.data.begin(),p.data.end(),0.0f);
        const auto count=std::min(pageFrames,length-first);
        if (rate==sourceRate) {
            reader->seek(sourceStart+static_cast<std::uint64_t>(first));
            if (reader->read(p.data.data(),static_cast<std::uint32_t>(count))!=count) throw std::runtime_error("short WAV read");
        } else {
            // Independent rationally aligned pages with real context either
            // side. Alignment makes the resampler phase identical at a page
            // seam and after random seeks, regardless of callback block size.
            const std::int64_t divisor=std::gcd(rate,sourceRate);
            const std::int64_t inPeriod=sourceRate/divisor, outPeriod=rate/divisor;
            const std::int64_t guard=1024*std::max<std::int64_t>(1,(sourceRate+rate-1)/rate);
            const auto anchor=(first/outPeriod)*inPeriod;
            const auto begin=anchor-((guard+inPeriod-1)/inPeriod)*inPeriod;
            const auto outBegin=(begin/inPeriod)*outPeriod;
            const auto inputCount=static_cast<std::int64_t>(std::ceil(static_cast<double>(first+count-outBegin)*sourceRate/rate))+guard;
            const auto outputCount=static_cast<std::int64_t>(std::ceil(static_cast<double>(inputCount)*rate/sourceRate))+32;
            std::vector<float> input(static_cast<std::size_t>(inputCount)*channels,0.0f);
            std::vector<float> output(static_cast<std::size_t>(outputCount)*channels,0.0f);
            const auto readBegin=std::max<std::int64_t>(0,begin);
            const auto readEnd=std::min<std::int64_t>(static_cast<std::int64_t>(sourceLength),begin+inputCount);
            if (readEnd>readBegin) {
                reader->seek(sourceStart+static_cast<std::uint64_t>(readBegin));
                auto* dst=input.data()+static_cast<std::size_t>(readBegin-begin)*channels;
                if(reader->read(dst,static_cast<std::uint32_t>(readEnd-readBegin))!=readEnd-readBegin) throw std::runtime_error("short conversion read");
            }
            SRC_DATA data{};
            data.data_in=input.data(); data.data_out=output.data();
            data.input_frames=static_cast<long>(inputCount); data.output_frames=static_cast<long>(outputCount);
            data.src_ratio=static_cast<double>(rate)/sourceRate; data.end_of_input=1;
            const int error=src_simple(&data,SRC_SINC_BEST_QUALITY,channels);
            if(error!=0 || data.output_frames_gen<first-outBegin+count) throw std::runtime_error("resampler did not fill page");
            std::copy_n(output.data()+static_cast<std::size_t>(first-outBegin)*channels,
                        static_cast<std::size_t>(count)*channels,p.data.data());
        }
        p.number=number;
    }
    bool service() {
        std::array<std::int64_t,slots> w;
        const auto seq=requestSeq.load(std::memory_order_seq_cst);
        if ((seq&1u)!=0) return false;
        for(std::size_t i=0;i<slots;++i) w[i]=requests[i].load(std::memory_order_seq_cst);
        if(requestSeq.load(std::memory_order_seq_cst)!=seq) return false;
        bool worked=false;
        for(const auto number:w) {
            if(number<0) continue;
            bool found=false;
            for(auto& p:pages) {
                const int state=p.state.load(std::memory_order_acquire);
                if((state==2 || state==3) && p.number.load(std::memory_order_relaxed)==number) { found=true; break; }
            }
            if(found) continue;
            for(auto& p:pages) {
                int state=p.state.load(std::memory_order_relaxed);
                if(state!=0 && state!=2) continue;
                if(!p.state.compare_exchange_strong(state,1,std::memory_order_acquire)) continue;
                if(state==2 && std::find(w.begin(),w.end(),p.number.load(std::memory_order_relaxed))!=w.end()) {
                    p.state.store(2,std::memory_order_release); continue;
                }
                try { fill(p,number); }
                catch(...) { errors.fetch_add(1,std::memory_order_relaxed); p.number=-1; }
                p.state.store(p.number>=0?2:0,std::memory_order_release);
                worked=true; break;
            }
        }
        return worked;
    }
};
class TrackSource final : public Node {
public:
    Transport& transport;
    std::int32_t maxFrames;
    std::int64_t track;
    std::vector<Clip*> clips;
    std::weak_ptr<void> owner;
    std::shared_ptr<void> sourceLifetime() const override { return owner.lock(); }
    TrackSource(Transport& t,std::int32_t f,std::int64_t id):transport(t),maxFrames(f),track(id){}
    void process(const NodeIo& io) noexcept override {
        for(std::int32_t c=0;c<io.channels;++c) std::fill_n(io.out[c]+io.blockOffset,io.frames,0.0f);
        if(!transport.playing()) return;
        for(auto* clip:clips) {
            if(io.blockOffset==0) clip->request(clip->wanted(transport,maxFrames));
            Page* page=nullptr; std::int64_t number=-1; std::uint32_t missing=0;
            for(std::int32_t i=0;i<io.frames;++i) {
                const auto time=transport.sampleAt(io.blockOffset+i);
                const auto n=clip->sourceAt(time);
                if(n<0) continue;
                if(n/pageFrames!=number) {
                    if(page) page->state.store(2,std::memory_order_release);
                    number=n/pageFrames; page=clip->acquire(number);
                }
                if(!page) { ++missing; continue; }
                const auto* sample=page->data.data()+static_cast<std::size_t>(n%pageFrames)*clip->channels;
                const float gain=clip->envelope(time);
                for(std::int32_t c=0;c<io.channels;++c) {
                    float value=0;
                    if(clip->mode==1) value=sample[0];
                    else if(clip->mode==2) value=sample[clip->channels>1?1:0];
                    else if(clip->mode==3) {
                        for(std::uint16_t j=0;j<clip->channels;++j) value+=sample[j];
                        value/=clip->channels;
                    } else if(clip->channels==1) value=sample[0];
                    else if(c<clip->channels) value=sample[c];
                    io.out[c][io.blockOffset+i]+=gain*value;
                }
            }
            if(page) page->state.store(2,std::memory_order_release);
            if(missing) clip->underruns.fetch_add(missing,std::memory_order_relaxed);
        }
    }
    const char* name() const noexcept override { return "disk clips"; }
};
}
struct ClipPlayback::Impl {
    Transport& transport;
    std::int32_t maxFrames;
    std::vector<std::unique_ptr<Clip>> clips;
    std::vector<std::unique_ptr<TrackSource>> tracks;
    std::vector<std::string> problems;
    std::atomic<bool> stalled{false};
    std::jthread worker;
    Impl(Transport& t,std::int32_t f):transport(t),maxFrames(f){}
    ~Impl() { worker.request_stop(); if(worker.joinable()) worker.join(); }
};
ClipPlayback::ClipPlayback(std::shared_ptr<const ClipProject> project, Transport& transport,
                           double rate,std::int32_t channels,std::int32_t maxFrames)
    :impl_(std::make_shared<Impl>(transport,maxFrames)) {
    if(!project) return;
    if(!std::isfinite(rate) || rate<8000 || rate>192000 || rate!=std::floor(rate) || channels<1 || channels>64 || maxFrames<1 || maxFrames>4096)
        throw std::runtime_error("clip playback format outside 8-192 kHz integer rate / 1-64 channels / 1-4096 frames");
    TempoMap tempo;
    for(const auto& e:project->rows.tempo) tempo.events.push_back({e.posTicks,e.bpm,static_cast<int>(e.curve)});
    if(tempo.events.empty()) tempo.events.push_back({0,120,0});
    if(!project->rows.audioClips.empty() && std::any_of(tempo.events.begin(),tempo.events.end(),[](const auto& e){return e.curve!=0;}))
        impl_->problems.push_back("tempo_map: TempoMap does not implement ramps; clip timing uses step tempos");
    for(const auto& row:project->rows.clips) {
        if(row.kind!="audio") continue;
        const auto label="clips#"+std::to_string(row.id)+": ";
        try {
            const auto a=std::find_if(project->rows.audioClips.begin(),project->rows.audioClips.end(),[&](const auto& x){return x.clipId==row.id;});
            if(a==project->rows.audioClips.end()) throw std::runtime_error("no audio row; silent");
            if(a->warpEnabled) throw std::runtime_error("warp unsupported; silent");
            if(a->reverse) throw std::runtime_error("reverse unsupported; silent");
            if(a->transposeSemis!=0 || a->formantShift!=0) throw std::runtime_error("pitch/formant shift unsupported; silent");
            if(row.aliasOf) throw std::runtime_error("alias resolution unsupported; silent");
            if(row.fadeInCurve<0 || row.fadeInCurve>5 || row.fadeOutCurve<0 || row.fadeOutCurve>5) throw std::runtime_error("fade curve unsupported; silent");
            if(a->channelMode<0 || a->channelMode>3) throw std::runtime_error("unknown channel mode; silent");
            const auto media=std::find_if(project->rows.media.begin(),project->rows.media.end(),[&](const auto& m){return m.id==a->mediaId;});
            if(media==project->rows.media.end() || media->embedded) throw std::runtime_error("missing or embedded media; extract first; silent");
            auto clip=std::make_unique<Clip>();
            if(media->relPath) clip->path=project->folder/utf8Path(*media->relPath);
            else if(auto h=project->hints.find(media->id);h!=project->hints.end()) clip->path=h->second;
            else throw std::runtime_error("no media path; silent");
            // WavReader sniffs RIFF/RF64/BW64; no decoder on the callback.
            clip->reader=std::make_unique<audio::WavReader>(clip->path);
            clip->channels=clip->reader->channels(); clip->sourceRate=clip->reader->sampleRate(); clip->rate=static_cast<std::uint32_t>(rate);
            if(clip->channels>64 || clip->sourceRate<8000 || clip->sourceRate>192000) throw std::runtime_error("source format outside supported bounds; silent");
            if(a->srcStartFrames<0 || a->srcLenFrames<=0 || static_cast<std::uint64_t>(a->srcStartFrames)>clip->reader->frames() || static_cast<std::uint64_t>(a->srcLenFrames)>clip->reader->frames()-static_cast<std::uint64_t>(a->srcStartFrames)) throw std::runtime_error("source window outside WAV; silent");
            clip->sourceStart=static_cast<std::uint64_t>(a->srcStartFrames); clip->sourceLength=static_cast<std::uint64_t>(a->srcLenFrames);
            clip->length=samples(static_cast<double>(clip->sourceLength)/clip->sourceRate,rate);
            const auto origin=row.timeBase==0?row.posTicks.value_or(0):tempo.secondsToTicks(static_cast<double>(row.posNs.value_or(0))*1e-9);
            if(origin<0) throw std::runtime_error("negative musical time unsupported by TempoMap");
            const double startSeconds=row.timeBase==0?tempo.ticksToSeconds(origin):static_cast<double>(row.posNs.value_or(0))*1e-9;
            auto duration=[&](std::int64_t ticks){
                if(ticks<0 || origin>std::numeric_limits<std::int64_t>::max()-ticks) throw std::runtime_error("invalid tick duration");
                return samples(tempo.ticksToSeconds(origin+ticks)-tempo.ticksToSeconds(origin),rate);
            };
            clip->start=samples(startSeconds,rate);
            const auto len=row.timeBase==0?duration(row.lengthTicks.value_or(0)):samples(static_cast<double>(row.lengthNs.value_or(0))*1e-9,rate);
            clip->end=clip->start+len; clip->offset=duration(row.contentOffsetTicks);
            clip->fadeIn=duration(row.fadeInTicks);
            const auto endTicks=row.timeBase==0?origin+row.lengthTicks.value_or(0):tempo.secondsToTicks(startSeconds+static_cast<double>(row.lengthNs.value_or(0))*1e-9);
            if(row.fadeOutTicks<0 || endTicks<row.fadeOutTicks) throw std::runtime_error("invalid fade-out duration");
            clip->fadeOut=samples(tempo.ticksToSeconds(endTicks)-tempo.ticksToSeconds(endTicks-row.fadeOutTicks),rate);
            clip->inCurve=row.fadeInCurve;clip->outCurve=row.fadeOutCurve;
            if(row.loopEnabled) {
                const auto loopTicks=row.loopLenTicks.value_or(0);
                if(loopTicks<0 || row.loopStartTicks<0 || row.loopStartTicks>std::numeric_limits<std::int64_t>::max()-loopTicks) throw std::runtime_error("invalid loop ticks");
                clip->loopBegin=duration(row.loopStartTicks);clip->loopLength=duration(row.loopStartTicks+loopTicks)-clip->loopBegin;
                if(clip->loopLength<=0 || clip->loopBegin+clip->loopLength>clip->length) throw std::runtime_error("clip loop outside source window; silent");
            }
            if(!std::isfinite(row.gainDb) || std::abs(row.gainDb)>120) throw std::runtime_error("invalid clip gain; silent");
            clip->gain=static_cast<float>(std::pow(10.0,row.gainDb/20)); clip->mode=static_cast<int>(a->channelMode); clip->track=row.trackId;
            if(row.muted) continue;
            for(auto& p:clip->pages) p.data.resize(static_cast<std::size_t>(pageFrames)*clip->channels);
            auto track=std::find_if(impl_->tracks.begin(),impl_->tracks.end(),[&](const auto& x){return x->track==row.trackId;});
            if(track==impl_->tracks.end()) { impl_->tracks.push_back(std::make_unique<TrackSource>(transport,maxFrames,row.trackId)); track=impl_->tracks.end()-1; }
            (*track)->clips.push_back(clip.get()); impl_->clips.push_back(std::move(clip));
        } catch(const std::exception& e) { impl_->problems.push_back(label+e.what()+"; silent"); }
    }
    for(auto& t:impl_->tracks) t->owner=impl_;
    if(impl_->clips.empty()) return;
    impl_->worker=std::jthread([p=impl_.get()](std::stop_token stop){
        while(!stop.stop_requested()) {
            bool work=false;
            if(!p->stalled.load(std::memory_order_acquire)) for(auto& c:p->clips) { if(stop.stop_requested()) break; work=c->service()||work; }
            if(!work) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}
ClipPlayback::~ClipPlayback()=default;
std::vector<Node*> ClipPlayback::sourcesFor(std::int64_t id) {
    for(auto& t:impl_->tracks) if(t->track==id) return {t.get()};
    return {};
}
const std::vector<std::string>& ClipPlayback::problems() const noexcept { return impl_->problems; }
void ClipPlayback::stallForTest(bool v) noexcept { impl_->stalled.store(v,std::memory_order_release); }
std::uint32_t ClipPlayback::underrunSamples() const noexcept { std::uint32_t n=0;for(const auto& c:impl_->clips)n+=c->underruns.load();return n; }
std::uint32_t ClipPlayback::readErrors() const noexcept { std::uint32_t n=0;for(const auto& c:impl_->clips)n+=c->errors.load();return n; }
bool ClipPlayback::prime(std::chrono::milliseconds timeout) {
    const auto until=std::chrono::steady_clock::now()+timeout;
    for(auto& c:impl_->clips)c->request(c->wanted(impl_->transport,impl_->maxFrames));
    do {
        bool ready=true;
        for(auto& c:impl_->clips) for(const auto n:c->wanted(impl_->transport,impl_->maxFrames)) {
            if(n<0)continue;
            if(auto* p=c->acquire(n))p->state.store(2,std::memory_order_release);else ready=false;
        }
        if(ready)return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<until);
    return false;
}
}
