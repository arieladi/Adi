// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/progressive_decode.hpp"
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/publish_file.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <thread>
namespace adi::audio {
namespace fs=std::filesystem;
namespace {
void owner() {if(onAudioThread)throw DecodeError(DecodeProblem::AudioThread,"progressive decode requires a worker");}
void cancelled(std::stop_token stop) {if(stop.stop_requested())throw DecodeError(DecodeProblem::Cancelled,"progressive decode cancelled");}
AacTrack aacTrack(const fs::path& source,SourceFormat format) {
    try {return format==SourceFormat::Adts?readAdts(source):readAacAudioMovie(source).tracks.at(0);}
    catch(const AacTransportError& e) {
        const auto p=e.problem()==AacTransportProblem::NoAudio?DecodeProblem::NoAudio:
            (e.problem()==AacTransportProblem::Unsupported?DecodeProblem::Unsupported:DecodeProblem::Corrupt);
        throw DecodeError(p,e.what());
    }
}
bool aac(SourceFormat f){return f==SourceFormat::Mp4 || f==SourceFormat::Adts;}
}
PcmStreamInfo inspectPcm(const fs::path& source,std::stop_token stop) {
    owner();cancelled(stop);const auto format=sniff(source);
    if(aac(format)) {
        const auto track=aacTrack(source,format);cancelled(stop);const auto config=aacConfig(track.config);
        return {format,track.sampleRate,config.channels,track.frames};
    }
    auto opened=openPcmStream(source,stop);
    if(!opened.ready())throw DecodeError(opened.problem.value_or(DecodeProblem::Corrupt),opened.error);
    return opened.stream->info();
}
struct ProgressiveFile::State {
    PcmStreamInfo info;
    fs::path source,path,directory;
    std::string hash,error;
    DecodeCache* cache=nullptr;
    bool pinned=false;
    mutable std::mutex mutex;
    std::stop_source stop;
    std::atomic<ProgressiveStatus> status{ProgressiveStatus::Pending};
    std::atomic<std::uint64_t> ready{0};
    ~State() {
        if(pinned)try {cache->release(path);}catch(...){}
        std::error_code ec;if(!directory.empty())fs::remove_all(directory,ec);
    }
};
ProgressiveFile::ProgressiveFile(std::shared_ptr<State> s):state_(std::move(s)){}
ProgressiveFile::~ProgressiveFile(){
    cancel();
    // Completed cache ownership belongs to the last public lease, not a
    // worker's brief bookkeeping reference after it publishes completion.
    std::lock_guard lock(state_->mutex);
    if(state_->pinned) {
        state_->pinned=false;
        try {state_->cache->release(state_->path);}catch(...){}
    }
}
const PcmStreamInfo& ProgressiveFile::info()const noexcept{return state_->info;}
ProgressiveSnapshot ProgressiveFile::snapshot()const noexcept {
    const auto status=state_->status.load(std::memory_order_acquire);
    return {status,state_->ready.load(std::memory_order_acquire)};
}
void ProgressiveFile::cancel()noexcept{state_->stop.request_stop();}
std::string ProgressiveFile::error()const {owner();std::lock_guard lock(state_->mutex);return state_->error;}
ProgressiveRead ProgressiveFile::read(std::uint64_t first,std::span<float> out) {
    if(onAudioThread)return ProgressiveRead::AudioThread;
    if(out.empty() || out.size()%state_->info.channels || out.size()/state_->info.channels>1048576 ||
       first>*state_->info.frames || out.size()/state_->info.channels>*state_->info.frames-first)return ProgressiveRead::InvalidBuffer;
    std::lock_guard lock(state_->mutex);
    const auto status=state_->status.load(std::memory_order_acquire);
    if(status==ProgressiveStatus::Failed)return ProgressiveRead::Failed;
    if(status==ProgressiveStatus::Cancelled)return ProgressiveRead::Cancelled;
    if(first+out.size()/state_->info.channels>state_->ready.load(std::memory_order_acquire))return ProgressiveRead::Pending;
    try {
        WavReader reader(state_->path);reader.seek(first);
        if(reader.read(out.data(),static_cast<std::uint32_t>(out.size()/state_->info.channels))!=out.size()/state_->info.channels)
            throw std::runtime_error("short progressive prefix read");
        return ProgressiveRead::Frames;
    }catch(const std::exception& e) {
        state_->error=e.what();state_->stop.request_stop();state_->status.store(ProgressiveStatus::Failed,std::memory_order_release);return ProgressiveRead::Failed;
    }
}
struct ProgressiveDecoder::Impl {
    DecodeCache& cache;
    ProgressiveHooks hooks;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::shared_ptr<ProgressiveFile::State>> queue;
    std::map<std::string,std::weak_ptr<ProgressiveFile>> files;
    std::vector<std::jthread> workers;
    bool stopping=false;
    std::size_t admitted=0;
    std::uint64_t reserved=0;
    explicit Impl(DecodeCache& c,ProgressiveHooks h,unsigned count):cache(c),hooks(std::move(h)) {
        if(count<1 || count>8)throw std::invalid_argument("progressive worker count outside 1..8");
        try {for(unsigned i=0;i<count;++i)workers.emplace_back([this]{work();});}
        catch(...) {shutdown();throw;}
    }
    void shutdown() {
        {std::lock_guard lock(mutex);stopping=true;for(auto& [key,weak]:files){(void)key;if(auto file=weak.lock())file->cancel();}}
        condition.notify_all();workers.clear();
    }
    ~Impl(){shutdown();}
    void work() {
        for(;;) {
            std::shared_ptr<ProgressiveFile::State> state;
            {std::unique_lock lock(mutex);condition.wait(lock,[&]{return stopping || !queue.empty();});
                if(queue.empty()){if(stopping)return;continue;}state=std::move(queue.front());queue.pop_front();}
            run(state);
            {std::lock_guard lock(mutex);--admitted;reserved-=*state->info.frames*state->info.channels*4+4096;}
        }
    }
    void finish(ProgressiveFile::State& state) {
        const auto stop=state.stop.get_token();cancelled(stop);
        std::lock_guard cacheLock(cache.mutex_);
        const auto target=cache.fileFor(state.hash);
        const auto published=media::publishFile(state.path,target);
        if(published.status==media::PublishStatus::exists) {
            if(!fs::is_regular_file(fs::symlink_status(target)))throw DecodeError(DecodeProblem::CacheWrite,"cache target is not a real regular file");
            WavReader actual(target);
            if(actual.frames()!=*state.info.frames || actual.channels()!=state.info.channels || actual.sampleRate()!=state.info.sampleRate)
                throw DecodeError(DecodeProblem::CacheWrite,"cache target has different progressive dimensions");
        } else if(published.status!=media::PublishStatus::published)throw DecodeError(DecodeProblem::CacheWrite,published.error);
        auto& item=cache.items_[state.hash];item.bytes=fs::file_size(target);item.lastUse=cache.stamp();++item.pins;
        // Keep the lease even if optional index maintenance fails: no dangling
        // pin, and a complete target remains independently discoverable.
        state.path=target;state.pinned=true;++cache.stats_.decodes;
        cache.evict(0,state.hash);cache.saveIndex();
    }
    void run(const std::shared_ptr<ProgressiveFile::State>& state) {
        const auto stop=state->stop.get_token();
        try {
            cancelled(stop);
            {std::lock_guard lock(state->mutex);state->path=state->directory/"prefix.wav";}
            std::unique_ptr<WavWriter> writer;
            struct CloseWriter {
                std::mutex& mutex;
                std::unique_ptr<WavWriter>& value;
                ~CloseWriter(){std::lock_guard lock(mutex);value.reset();}
            } closeWriter{state->mutex,writer};
            // Unwinding native errors/cancellation must also finalize under
            // the prefix-reader lock, including WavWriter's destructor.
            writer=std::make_unique<WavWriter>(state->path,state->info.sampleRate,static_cast<std::uint16_t>(state->info.channels),WavFormat::Float32);
            const auto sink=[&](std::span<const float> pcm) {
                cancelled(stop);const auto frames=pcm.size()/state->info.channels;
                if(pcm.empty() || pcm.size()%state->info.channels || frames>65536 || frames>*state->info.frames-state->ready.load())
                    throw DecodeError(DecodeProblem::Corrupt,"progressive decoder exceeded declared dimensions");
                {std::lock_guard lock(state->mutex);writer->write(pcm.data(),static_cast<std::uint32_t>(frames));state->ready.fetch_add(frames,std::memory_order_release);}
                if(hooks.checkpoint)hooks.checkpoint(state->ready.load(std::memory_order_acquire),stop);
            };
            if(aac(state->info.format)) {
                if(!aacCodecAvailable())throw DecodeError(DecodeProblem::CodecUnavailable,"AAC codec unavailable");
                const auto track=aacTrack(state->source,state->info.format);const auto config=aacConfig(track.config);
                if(track.frames!=state->info.frames || track.sampleRate!=state->info.sampleRate || config.channels!=state->info.channels)
                    throw DecodeError(DecodeProblem::Corrupt,"AAC metadata changed before decoding");
                decodeAacPcm(track,state->directory/"native",sink,[&]{cancelled(stop);});
            }else {
                auto opened=openPcmStream(state->source,stop);
                if(!opened.ready())throw DecodeError(opened.problem.value_or(DecodeProblem::Corrupt),opened.error);
                const auto& info=opened.stream->info();
                if(info.frames!=state->info.frames || info.sampleRate!=state->info.sampleRate || info.channels!=state->info.channels)
                    throw DecodeError(DecodeProblem::Corrupt,"PCM metadata changed before decoding");
                std::vector<float> pcm(static_cast<std::size_t>(4096)*info.channels);
                for(;;) {
                    const auto result=opened.stream->read(pcm,stop);
                    if(result.status==PcmReadStatus::End)break;
                    if(result.status==PcmReadStatus::Cancelled)cancelled(stop);
                    if(result.status!=PcmReadStatus::Frames)throw DecodeError(opened.stream->problem().value_or(DecodeProblem::Corrupt),opened.stream->error());
                    sink(std::span(pcm.data(),static_cast<std::size_t>(result.frames)*info.channels));
                }
            }
            cancelled(stop);
            {std::lock_guard lock(state->mutex);
                writer->close();
                if(state->ready.load()!=state->info.frames)throw DecodeError(DecodeProblem::Corrupt,"progressive decode ended before declared total");
                if(state->status.load()==ProgressiveStatus::Failed)throw DecodeError(DecodeProblem::Corrupt,"progressive reader failed");
            }
            // The final source pass may be long. Prefix readers keep serving
            // existing pages while it runs; only publication holds their lock.
            const auto hash=media::blake3File(state->source,stop);cancelled(stop);
            if(!hash || hash.hex!=state->hash)throw DecodeError(DecodeProblem::Unreadable,"source changed during progressive decoding");
            {std::lock_guard lock(state->mutex);finish(*state);state->status.store(ProgressiveStatus::Complete,std::memory_order_release);}
        }catch(const std::exception& e) {
            std::lock_guard lock(state->mutex);
            if(state->status.load()!=ProgressiveStatus::Failed) {
                state->error=e.what();state->status.store(stop.stop_requested()?ProgressiveStatus::Cancelled:ProgressiveStatus::Failed,std::memory_order_release);
            }
        }catch(...) {
            std::lock_guard lock(state->mutex);state->error="progressive decoder failed";state->status.store(ProgressiveStatus::Failed,std::memory_order_release);
        }
    }
};
ProgressiveDecoder::ProgressiveDecoder(DecodeCache& cache,ProgressiveHooks hooks,unsigned count) {
    owner();impl_=std::make_unique<Impl>(cache,std::move(hooks),count);
}
ProgressiveDecoder::~ProgressiveDecoder()=default;
std::shared_ptr<ProgressiveFile> ProgressiveDecoder::open(const fs::path& source,std::stop_token stop) {
    owner();cancelled(stop);const auto hash=media::blake3File(source,stop);cancelled(stop);
    if(!hash)throw DecodeError(DecodeProblem::Unreadable,"progressive source cannot be hashed");
    std::lock_guard lock(impl_->mutex);cancelled(stop);
    for(auto i=impl_->files.begin();i!=impl_->files.end();) {if(i->second.expired())i=impl_->files.erase(i);else ++i;}
    if(auto i=impl_->files.find(hash.hex);i!=impl_->files.end())if(auto file=i->second.lock()) {
        const auto status=file->snapshot().status;
        if(status==ProgressiveStatus::Pending || status==ProgressiveStatus::Complete)return file;
    }
    if(impl_->stopping || impl_->admitted>=32)throw DecodeError(DecodeProblem::Unsupported,"progressive job capacity reached");
    auto state=std::make_shared<ProgressiveFile::State>();state->info=inspectPcm(source,stop);cancelled(stop);
    if(!state->info.frames || !*state->info.frames)throw DecodeError(DecodeProblem::Unsupported,"progressive playback requires a nonempty exact header length");
    if(*state->info.frames>(std::numeric_limits<std::uint64_t>::max()-4096)/state->info.channels/4)throw DecodeError(DecodeProblem::Unsupported,"progressive dimensions overflow");
    state->source=source;state->hash=hash.hex;state->cache=&impl_->cache;
    auto file=std::shared_ptr<ProgressiveFile>(new ProgressiveFile(state));
    {std::lock_guard cacheLock(impl_->cache.mutex_);
        const auto target=impl_->cache.fileFor(hash.hex);
        if(fs::exists(target)) {
            if(!fs::is_regular_file(fs::symlink_status(target)))throw DecodeError(DecodeProblem::CacheWrite,"cache target is not a real regular file");
            WavReader reader(target);
            if(reader.frames()!=state->info.frames || reader.sampleRate()!=state->info.sampleRate || reader.channels()!=state->info.channels)
                throw DecodeError(DecodeProblem::CacheWrite,"cached progressive dimensions differ");
            auto& item=impl_->cache.items_[hash.hex];item.bytes=fs::file_size(target);item.lastUse=impl_->cache.stamp();++item.pins;++impl_->cache.stats_.hits;
            state->path=target;state->pinned=true;state->ready.store(*state->info.frames);state->status.store(ProgressiveStatus::Complete);
            impl_->cache.saveIndex();impl_->files[hash.hex]=file;return file;
        }
        const auto bytes=*state->info.frames*state->info.channels*4+4096;
        if(bytes>impl_->cache.limits_.maxBytes || impl_->reserved>impl_->cache.limits_.maxBytes-bytes)
            throw DecodeError(DecodeProblem::NoSpace,"progressive jobs exceed cache budget");
        impl_->cache.evict(bytes+impl_->reserved,hash.hex);
        const auto free=impl_->cache.freeBytes();
        if(free<impl_->cache.limits_.minFreeBytes || bytes+impl_->reserved>free-impl_->cache.limits_.minFreeBytes)
            throw DecodeError(DecodeProblem::NoSpace,"progressive jobs would consume minimum free space");
    }
    static std::atomic<std::uint64_t> serial{0};
    const auto root=impl_->cache.folder()/"progressive";fs::create_directories(root);
    for(unsigned attempt=0;attempt<128;++attempt) {
        const auto candidate=root/(hash.hex+"-"+std::to_string(serial.fetch_add(1)));
        std::error_code ec;if(fs::create_directory(candidate,ec)){state->directory=candidate;break;}
        if(ec && ec!=std::errc::file_exists)throw DecodeError(DecodeProblem::CacheWrite,ec.message());
    }
    if(state->directory.empty())throw DecodeError(DecodeProblem::CacheWrite,"cannot reserve progressive private directory");
    fs::permissions(state->directory,fs::perms::owner_all,fs::perm_options::replace);
    cancelled(stop);impl_->files[hash.hex]=file;impl_->queue.push_back(state);++impl_->admitted;impl_->reserved+=*state->info.frames*state->info.channels*4+4096;
    impl_->condition.notify_one();return file;
}
ProgressiveDecoder& defaultProgressiveDecoder(){static ProgressiveDecoder decoder(defaultDecodeCache());return decoder;}
} // namespace adi::audio
