// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/clip_playback.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/engine/timeline_clips.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/engine/clip_fade.hpp"
#include "adi/engine/clip_sinc_441_to_480.hpp"
#include "adi/engine/launch_audio_runtime.hpp"
#include "adi/engine/launch_follow_playback.hpp"
#include "adi/engine/launch_overlay_delay.hpp"
#include "adi/engine/launch_paged_audio.hpp"
#include "adi/engine/launch_store.hpp"
#include "adi/engine/launch_warp_source.hpp"
#include "adi/engine/warp_clip_player.hpp"
#include "adi/engine/warp_projection.hpp"
#include "adi/engine/warp_r3_pages.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/progressive_decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_compare.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/edit_time.hpp"
#include "adi/media/clip_pitch_render.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <samplerate.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <thread>
#include <type_traits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace adi::engine {
namespace fs = std::filesystem;
namespace {
fs::path utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(),s.end())); }
struct MissingClipMedia : std::runtime_error { using std::runtime_error::runtime_error; };
struct DecodeProblem {
    std::string detail;
    bool mediaOffline;
};
}
struct ClipProject {
    rows::Model rows;
    fs::path folder;
    std::unordered_map<std::int64_t, fs::path> hints;
    NoteExpressionsRead noteExpressions;
    std::optional<launch::LaunchData> session;
    std::optional<std::map<std::int64_t,std::optional<media::FileRevision>>> launchFiles;
    clip_edit::ProjectPitchEditsRead pitchEdits;
};

namespace {
struct RelocatedSource { fs::path path; std::string error; };

fs::path clipMediaPath(const ClipProject& project, const rows::Media& source,
                       std::unordered_map<std::int64_t,RelocatedSource>& relocated) {
    const auto hint = project.hints.find(source.id);
    if (source.relPath) {
        const auto relative = utf8Path(*source.relPath);
        const auto path = project.folder / relative;
        if (hint != project.hints.end() && !relative.empty() && *relative.begin() == ".." &&
            path.lexically_normal() != hint->second.lexically_normal()) {
            auto [found, inserted] = relocated.try_emplace(source.id);
            auto& resolved = found->second;
            if (inserted) resolved.path = media::resolvePlaybackMedia(project.folder,relative,hint->second,
                source.origName,source.hashBlake3,resolved.error);
            if (resolved.path.empty()) {
                if (resolved.error.empty()) throw MissingClipMedia("missing media: " + source.origName);
                throw std::runtime_error(resolved.error);
            }
            return resolved.path;
        }
        return path;
    }
    if (hint != project.hints.end()) return hint->second;
    throw MissingClipMedia("no media path");
}

decltype(ClipProject::launchFiles) readLaunchFiles(const ClipProject& project) {
    std::map<std::int64_t,std::optional<media::FileRevision>> files;
    std::unordered_map<std::int64_t,RelocatedSource> relocated;
    for (const auto& clip : project.rows.slotClips) {
        if (clip.kind != "audio" || clip.muted) continue;
        const auto audio = std::ranges::find(project.rows.audioClips,clip.id,&rows::AudioClip::clipId);
        if (audio == project.rows.audioClips.end()) continue;
        const auto source = std::ranges::find(project.rows.media,audio->mediaId,&rows::Media::id);
        if (source == project.rows.media.end() || source->embedded) continue;
        auto [entry, inserted] = files.try_emplace(source->id);
        if (!inserted) continue;
        try {
            std::error_code error;
            // Playback follows symlinks. Read the resolved regular file's stamp.
            const auto path = fs::canonical(clipMediaPath(project,*source,relocated),error);
            if (!error) entry->second = media::readFileRevision(path,error);
            if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
                return std::nullopt;
        } catch (const MissingClipMedia&) {
            // Playback also treats this source as unavailable.
        } catch (const std::runtime_error&) { return std::nullopt; }
    }
    return files;
}
} // namespace

std::shared_ptr<const ClipProject> readClipProject(const Store& store, const rows::Model& rows, bool readSession) {
    auto p = std::make_shared<ClipProject>(); p->rows = rows;
    // SQLite resolves its filename when opening. Store::path() can remain
    // relative, and a subsequent working-directory change must not rebase media.
    SQLite::Statement location(store.db(), "PRAGMA database_list");
    while(location.executeStep()) if(location.getColumn(1).getString()=="main")
        p->folder=utf8Path(location.getColumn(2).getString()).parent_path();
    SQLite::Statement q(store.db(), "SELECT id,abs_path_hint FROM media_files WHERE abs_path_hint IS NOT NULL");
    while (q.executeStep()) p->hints.emplace(q.getColumn(0).getInt64(), utf8Path(q.getColumn(1).getString()));
    p->noteExpressions=readNoteExpressions(store);
    if (readSession) {
        p->session = launch::readLaunchData(store, rows);
        if (p->session) p->launchFiles = readLaunchFiles(*p);
    }
    p->pitchEdits=clip_edit::readProjectPitchEdits(store);
    return p;
}
const NoteExpressionsRead& clipNoteExpressions(const ClipProject& project) noexcept {
    return project.noteExpressions;
}

bool sameStoredLaunchSources(const ClipProject& a, const ClipProject& b) {
    return a.session && b.session && a.folder == b.folder && a.hints == b.hints &&
        a.rows.slotClips == b.rows.slotClips && a.rows.audioClips == b.rows.audioClips &&
        a.rows.media == b.rows.media &&
        std::ranges::equal(a.session->tracks(), b.session->tracks()) &&
        std::ranges::equal(a.session->scenes(), b.session->scenes()) &&
        std::ranges::equal(a.session->slots(), b.session->slots()) &&
        std::ranges::equal(a.session->settingsRows(), b.session->settingsRows());
}

static bool sameStoredAudioBacking(const ClipProject& a, const ClipProject& b, std::int64_t id) {
    const auto leftAudio = std::ranges::find(a.rows.audioClips,id,&rows::AudioClip::clipId);
    const auto rightAudio = std::ranges::find(b.rows.audioClips,id,&rows::AudioClip::clipId);
    if (leftAudio == a.rows.audioClips.end() || rightAudio == b.rows.audioClips.end() || *leftAudio != *rightAudio)
        return false;
    const auto mediaId = leftAudio->mediaId;
    const auto leftMedia = std::ranges::find(a.rows.media,mediaId,&rows::Media::id);
    const auto rightMedia = std::ranges::find(b.rows.media,mediaId,&rows::Media::id);
    if (leftMedia == a.rows.media.end() || rightMedia == b.rows.media.end() || *leftMedia != *rightMedia)
        return false;
    if (leftMedia->relPath) return a.folder == b.folder;
    const auto leftHint = a.hints.find(mediaId);
    const auto rightHint = b.hints.find(mediaId);
    return leftHint != a.hints.end() && rightHint != b.hints.end() && leftHint->second == rightHint->second;
}

bool sameStoredWarpSource(const ClipProject& a, const ClipProject& b, std::int64_t id) {
    const auto left = std::ranges::find(a.rows.slotClips,id,&rows::Clip::id);
    const auto right = std::ranges::find(b.rows.slotClips,id,&rows::Clip::id);
    return left != a.rows.slotClips.end() && right != b.rows.slotClips.end() && left->gainDb == right->gainDb &&
        sameStoredAudioBacking(a,b,id);
}

bool sameLaunchSource(const ClipProject& a, const ClipProject& b) noexcept {
    if (!a.launchFiles || !b.launchFiles || a.launchFiles != b.launchFiles) return false;
    if (&a == &b) return true;
    if (!a.session || !b.session || a.folder != b.folder || a.hints != b.hints ||
        a.rows.slotClips != b.rows.slotClips || a.rows.audioClips != b.rows.audioClips || a.rows.media != b.rows.media)
        return false;
    if (!std::ranges::equal(a.rows.tracks,b.rows.tracks,[](const auto& left, const auto& right) {
            return left.id == right.id && left.kind == right.kind;
        }) || !std::ranges::equal(a.rows.clips,b.rows.clips,[](const auto& left, const auto& right) {
            return left.trackId == right.trackId && left.kind == right.kind;
        })) return false;
    return std::ranges::equal(a.session->tracks(),b.session->tracks()) &&
        std::ranges::equal(a.session->scenes(),b.session->scenes()) &&
        std::ranges::equal(a.session->slots(),b.session->slots()) &&
        std::ranges::equal(a.session->settingsRows(),b.session->settingsRows());
}

bool launchFilesCurrent(const ClipProject& project) {
    return project.launchFiles && project.launchFiles == readLaunchFiles(project);
}
namespace {
constexpr std::size_t slots = 8;
static_assert(std::atomic<std::int64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
// Clip and sample arithmetic is checked off-thread; leave generous headroom
// for callback offsets, loop arithmetic and conversion guards.
constexpr std::int64_t limit = std::int64_t{1} << 50;
std::int32_t delayLeadIn(const Transport& transport, std::int32_t delay) noexcept {
    return static_cast<std::int32_t>(std::max<std::int64_t>(0,delay-transport.streamedFrames()));
}
std::int64_t playbackSample(const Transport& transport, std::int32_t offset, std::int32_t delay) noexcept {
    return delay ? transport.delayedSample(offset,delay) : transport.sampleAt(offset);
}
std::int32_t contiguousPlayback(const Transport& transport,std::int32_t offset,std::int32_t frames,
                                std::int32_t delay) noexcept {
    return transport.inputContiguousFrames(playbackSample(transport,offset,delay),frames);
}
bool supportsR3Ratios(std::span<const WarpMarker> markers,const WarpArrangementClock& clock,
                      double sourceOrigin,double endTick) noexcept {
    // Include source history before a trimmed placement, and the tempo of
    // each audible loop lap. Tempo changes after the clip cannot affect it.
    const auto [minimum,maximum]=clock.framesPerTickRange(sourceOrigin,endTick);
    for (std::size_t i=1; i<markers.size(); ++i) {
        const double ticksPerSourceFrame = static_cast<double>(markers[i].tick-markers[i-1].tick) /
            static_cast<double>(markers[i].sourceFrame-markers[i-1].sourceFrame);
        if (!audio::RealtimeWarpR3::supportsRatio(ticksPerSourceFrame*minimum) ||
            !audio::RealtimeWarpR3::supportsRatio(ticksPerSourceFrame*maximum)) return false;
    }
    return true;
}
bool supportsLaunchRePitchRates(std::span<const WarpMarker> markers, const WarpArrangementClock& clock) noexcept {
    constexpr double minimumStride = 1.0/256.0, maximumStride = 256.0;
    const auto slowest = clock.minTicksPerFrame(), fastest = clock.maxTicksPerFrame();
    for (std::size_t i=1; i<markers.size(); ++i) {
        const auto& a = markers[i-1];
        const auto& b = markers[i];
        const auto sourcePerTick = static_cast<double>(b.sourceFrame-a.sourceFrame)/static_cast<double>(b.tick-a.tick);
        if (sourcePerTick*slowest < minimumStride || sourcePerTick*fastest > maximumStride) return false;
    }
    return true;
}
std::int64_t samples(double seconds, double rate) {
    const double n = seconds * rate;
    if (!std::isfinite(n) || n < 0 || n > static_cast<double>(limit)) throw std::runtime_error("time outside supported sample range");
    return static_cast<std::int64_t>(std::llround(n));
}
std::int64_t samplesAtNs(std::int64_t ns, std::int64_t rate) {
    if (ns < 0) throw std::runtime_error("negative nanosecond clock");
    // ClipPlayback admits only bounded integer sample rates. Separate whole
    // seconds from the fraction so the products fit and half-sample ties are exact.
    constexpr std::int64_t second = 1000000000;
    const auto value = (ns / second) * rate + ((ns % second) * rate + second / 2) / second;
    if (value > limit) throw std::runtime_error("time outside supported sample range");
    return value;
}
struct Page {
    // 0 empty, 1 worker-owned, 2 ready, 3 reader-owned. Both sides must
    // acquire ownership BEFORE trusting the page number (an unowned peek is
    // only a filter); no seqlock over non-atomic samples, no concurrent
    // overwrite and no audio-side retry loop.
    std::atomic<int> state{0};
    std::atomic<std::int64_t> number{-1};
    std::unique_ptr<float[]> data;
};
struct PrefixPending {};
struct WavSource {
    struct File {
        fs::path path;
        bool pinned;
        File(const fs::path& input, bool decode)
            : path(decode ? audio::playableFile(input) : input), pinned(path != input) {}
        ~File() {
            if (pinned) try { audio::defaultDecodeCache().release(path); } catch (...) {}
        }
    };
    // Destroy the stream before releasing the decode cache pin. Windows
    // cannot evict the file while the stream is open.
    File file;
    audio::WavReader wav;
    std::mutex mutex;

    explicit WavSource(const fs::path& path, bool decode = false) : file(path, decode), wav(file.path) {}
    void read(std::uint64_t first, float* out, std::uint32_t count) {
        // Page workers can read different offsets from this file. Seek and
        // read share a cursor and must run together, off the audio thread.
        const std::lock_guard lock(mutex);
        wav.seek(first);
        if (wav.read(out, count) != count) throw std::runtime_error("short WAV source read");
    }
};
struct SourceReaders {
    std::weak_ptr<WavSource> native;
    std::weak_ptr<WavSource> decoded;
    std::optional<audio::PcmStreamInfo> compressedInfo;
    bool refusedNative = false;
};
std::string_view preparationDetail(const audio::WarpResult& result) { return result.detail; }
std::string_view preparationDetail(const media::ClipPitchRenderResult& result) {
    return result.detail ? std::string_view(*result.detail) : std::string_view{};
}
struct PreparedSource {
    using Task = std::variant<std::shared_ptr<const audio::WarpTask>,
                              std::shared_ptr<const media::ClipPitchRenderTask>>;
    struct State {
        bool ready = false;
        const fs::path* file = nullptr;
        std::string_view detail;
    };
    // The task retains the prepared file. Close the shared WAV reader before
    // dropping this lease so the file can be removed on Windows.
    const Task task;
    std::atomic<bool> pending;
    std::mutex mutex;
    std::shared_ptr<WavSource> reader;

    explicit PreparedSource(Task value) : task(std::move(value)), pending(!state().ready) {}
    State state() const {
        return std::visit([](const auto& job) -> State {
            const auto result = job->result();
            if (!result) return {};
            if (*result) return {true, &result->prepared->file, {}};
            return {true, nullptr, preparationDetail(*result)};
        }, task);
    }
    const char* failure() const noexcept {
        return std::holds_alternative<std::shared_ptr<const audio::WarpTask>>(task)
            ? "cannot prepare Warp source" : "cannot prepare pitch edits";
    }
    std::shared_ptr<WavSource> open() {
        const auto result = state();
        if (!result.ready) return {};
        pending.store(false, std::memory_order_release);
        if (!result.file) throw std::runtime_error(failure());
        const std::lock_guard lock(mutex);
        if (!reader) reader = std::make_shared<WavSource>(*result.file);
        return reader;
    }
};
struct Clip {
    std::int64_t clipId = 0, mediaId = 0;
    std::string storedBlake3;
    std::int64_t track = 0, start = 0, end = 0, offset = 0, length = 0;
    std::int64_t loopBegin = 0, loopLength = 0, fadeIn = 0, fadeOut = 0;
    std::int64_t inCurve = 1, outCurve = 1;
    std::optional<double> inTension,outTension;
    std::array<std::optional<std::int64_t>,4> fadeClock;
    std::uint64_t sourceStart = 0, sourceLength = 0;
    std::uint32_t sourceRate = 0, rate = 0, preparedRate = 0;
    std::int64_t pageFrames = 8192;
    std::int64_t warmStartFrame = 0;
    std::int32_t warmStartFrames = 0;
    std::uint16_t channels = 0;
    int mode = 0;
    float gain = 1;
    double gainWide = 1;   ///< the same gain unrounded, for a graph at 64
    fs::path path;
    std::unique_ptr<WarpClipPlayer> streamingWarp;
    std::unique_ptr<WarpR3Pages> r3Warp;
    std::int32_t playbackDelay = 0;
    std::shared_ptr<PreparedSource> prepared;
    std::optional<media::ClipPitchRenderPlan> pitchComparisonPlan;
    std::shared_ptr<audio::ProgressiveFile> progressive;
    bool reverse = false;
    bool monitorInputOnly = false;
    bool failed = false; // page worker only
    // Open streams and cache pins die with the generation off the callback.
    // Keep this after prepared so the rendered stream closes before its file.
    std::shared_ptr<WavSource> reader;
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;
    std::array<Page, slots> pages;
    // Only the page writer allocates or releases storage while it owns a slot.
    std::size_t allocatedPages = 0;
    std::chrono::steady_clock::time_point releaseAfter{};
    // Single request producer/renderer only. Admission is a real lease: a
    // worker must not even temporarily claim a page until rendering releases
    // or supersedes it. No callback allocation or shared ownership operation.
    std::array<Page*,slots> admitted{};
    // Established before a private Session source publishes. The worker
    // reads only requests, so replacing callback requests with this set keeps
    // every reachable phase available without adding cache slots.
    std::optional<std::array<std::int64_t,slots>> retainedPages;
    std::array<std::atomic<std::int64_t>, slots> requests;
    std::atomic<std::uint32_t> requestSeq{0}, underruns{0}, errors{0};
    std::atomic<bool> invalidWarpRate{false};
    Clip() { for (auto& r : requests) r.store(-1); }
    bool preparationPending() const noexcept {
        return prepared && prepared->pending.load(std::memory_order_acquire);
    }
    bool sourcePreparationPending() const noexcept {
        return preparationPending() || (streamingWarp && !streamingWarp->ready());
    }
    std::int64_t pageSourceEnd() const noexcept {
        if (r3Warp) return end;
        return streamingWarp ? static_cast<std::int64_t>(sourceLength) : length;
    }
    std::int64_t sourceAt(std::int64_t time) const noexcept {
        if (time < start || time >= end) return -1;
        if (r3Warp) return time;
        auto n = time-start+offset;
        if (loopLength > 0 && n >= loopBegin) n = loopBegin+(n-loopBegin)%loopLength;
        return n < length ? n : -1;
    }
    template<typename V> V shaped(std::int64_t time,V gainValue)const noexcept {
        const auto local=time-start+offset;
        const auto shape=[](std::int64_t progressed,std::int64_t width,std::int64_t curve,std::optional<double> tension) {
            if(width==0)return progressed>=0?V{1}:V{0};
            return clipFadeShape(static_cast<V>(progressed)/static_cast<V>(width),curve,tension);
        };
        if(fadeClock[0])gainValue*=shape(local-*fadeClock[0],*fadeClock[1]-*fadeClock[0],inCurve,inTension);
        else if(fadeIn>0)gainValue*=clipFadeShape(static_cast<V>(time-start)/static_cast<V>(fadeIn),inCurve,inTension);
        if(fadeClock[2])gainValue*=shape(*fadeClock[3]-1-local,*fadeClock[3]-*fadeClock[2],outCurve,outTension);
        else if(fadeOut>0)gainValue*=clipFadeShape(static_cast<V>(end-1-time)/static_cast<V>(fadeOut),outCurve,outTension);
        return gainValue;
    }
    float envelope(std::int64_t time)const noexcept {return shaped(time,gain);}
    double envelopeWide(std::int64_t time)const noexcept {return shaped(time,gainWide);}
    /// One output channel's sample of this clip at `sample` (a frame of the
    /// file), by the clip's channel mode. A fold of the channels is a sum, in
    /// double (ADR-0207 d2), rounded to `V` once.
    template <typename V>
    V valueOf(const float* sample, std::int32_t c) const noexcept {
        if(mode==1) return sample[0];
        if(mode==2) return sample[channels>1?1:0];
        if(mode==3) {
            double folded=0;
            for(std::uint16_t j=0;j<channels;++j) folded+=sample[j];
            return static_cast<V>(folded/channels);
        }
        if(channels==1) return sample[0];
        if(c<channels) return sample[c];
        return V{0};
    }
    std::array<std::int64_t,slots> wanted(const Transport& t, std::int32_t frames,bool lookAhead=true) const noexcept {
        std::array<std::int64_t,slots> out; out.fill(-1);
        if (monitorInputOnly || !t.playing()) return out;
        std::size_t used = 0;
        const auto sourceEnd = pageSourceEnd();
        auto sourceRange = [&](std::int64_t from, std::int64_t count) {
            // sourceAt never reads at or past the effective source End.
            if(from<0 || from>=sourceEnd || count<=0) return;
            const auto last=std::min(sourceEnd,from+count)-1;
            for(auto p=from/pageFrames;p<=last/pageFrames && used<slots;++p)
                if(std::find(out.begin(),out.end(),p)==out.end()) out[used++]=p;
        };
        auto timelineRange = [&](std::int64_t from, std::int64_t count) {
            if(from>=end || count<=0) return;
            const auto to=from+std::min(count,end-from);
            from=std::max(from,start);
            if(to<=from) return;
            if (r3Warp) { sourceRange(from,to-from); return; }
            if (streamingWarp) {
                streamingWarp->sourceRanges(from, to-1, [&](audio::WarpSourceRange range) {
                    sourceRange(range.first, range.last-range.first);
                });
                return;
            }
            auto n=from-start+offset;
            count=to-from;
            if(loopLength==0) { sourceRange(n,count); return; }
            if(n<loopBegin) {
                const auto before=std::min(count,loopBegin-n);
                sourceRange(n,before); n+=before; count-=before;
            }
            if(count<=0) return;
            n=loopBegin+(n-loopBegin)%loopLength;
            const auto tail=std::min(count,loopBegin+loopLength-n);
            sourceRange(n,tail); count-=tail;
            // Further wraps repeat these same pages; never walk every sample
            // or every repetition of a one-sample loop on the callback.
            if(count>0) sourceRange(loopBegin,std::min(count,loopLength));
        };
        auto horizon = [&](std::int32_t framesAhead) {
            const auto begin = delayLeadIn(t,playbackDelay);
            if (begin >= framesAhead) return;
            const auto first=contiguousPlayback(t,begin,framesAhead-begin,playbackDelay);
            timelineRange(playbackSample(t,begin,playbackDelay),first);
            const auto wrap = begin+first;
            if (wrap < framesAhead)
                timelineRange(playbackSample(t,wrap,playbackDelay),
                              contiguousPlayback(t,wrap,framesAhead-wrap,playbackDelay));
        };
        // The current block always has priority. A private physical slot
        // can restart at Start inside a callback, including after a long
        // run whose ordinary read-ahead no longer covers those pages.
        horizon(frames);
        if (lookAhead) {
            timelineRange(warmStartFrame, warmStartFrames);
            horizon(ClipPlayback::readAheadFrames(rate));
        }
        return out;
    }
    void retainUnrolledPages(std::int64_t firstFrame) noexcept {
        const auto frames = std::min(end,length);
        if (start != 0 || offset != 0 || loopLength != 0 || firstFrame < 0 || firstFrame >= frames) return;
        const auto firstPage = firstFrame / pageFrames;
        const auto lastPage = (frames - 1) / pageFrames;
        if (lastPage - firstPage >= static_cast<std::int64_t>(slots)) return;
        auto& retained = retainedPages.emplace();
        retained.fill(-1);
        for (auto page = firstPage; page <= lastPage; ++page)
            retained[static_cast<std::size_t>(page - firstPage)] = page;
    }
    void request(const std::array<std::int64_t,slots>& requestedPages) noexcept {
        const auto& w = retainedPages ? *retainedPages : requestedPages;
        // The same horizon lasts across many callbacks. Keep an in-flight
        // worker snapshot valid until the single producer changes its pages.
        if(std::equal(w.begin(),w.end(),requests.begin(),[](auto page,const auto& requested) {
            return page==requested.load(std::memory_order_relaxed);
        }))return;
        requestSeq.fetch_add(1,std::memory_order_seq_cst);
        for(std::size_t i=0;i<slots;++i) requests[i].store(w[i],std::memory_order_seq_cst);
        requestSeq.fetch_add(1,std::memory_order_seq_cst);
    }
    void releaseAdmission() noexcept {
        for(auto& page:admitted)if(page){auto* held=page;page=nullptr;held->state.store(2,std::memory_order_release);}
    }
    void trimAdmission(const std::array<std::int64_t,slots>& wantedPages) noexcept {
        for(auto& page:admitted)if(page && std::find(wantedPages.begin(),wantedPages.end(),page->number.load(std::memory_order_relaxed))==wantedPages.end()){
            auto* held=page;page=nullptr;held->state.store(2,std::memory_order_release);
        }
    }
    void release(Page* page) noexcept {
        if(std::find(admitted.begin(),admitted.end(),page)==admitted.end())page->state.store(2,std::memory_order_release);
    }
    Page* acquire(std::int64_t number) noexcept {
        for(auto* page:admitted)if(page && page->number.load(std::memory_order_relaxed)==number)return page;
        for (auto& p : pages) {
            // Unowned peek as a filter only, so the worker never sees a slot
            // this reader does not want as transiently taken; the number is
            // checked again once the slot is owned.
            if(p.state.load(std::memory_order_acquire)!=2 || p.number.load(std::memory_order_relaxed)!=number) continue;
            int ready=2;
            if (!p.state.compare_exchange_strong(ready,3,std::memory_order_acquire)) continue;
            if(p.number.load(std::memory_order_relaxed)==number) return &p;
            p.state.store(2,std::memory_order_release);
        }
        return nullptr;
    }
    struct StreamReader {
        Clip& clip;
        Page* page = nullptr;
        std::int64_t number = -1;
        bool missing = false;
        ~StreamReader() { if (page) clip.release(page); }
        audio::WarpStreamPcm pcm(std::int64_t frame) noexcept {
            if (frame < 0 || static_cast<std::uint64_t>(frame) >= clip.sourceLength) return {};
            const auto nextPage = frame / clip.pageFrames;
            if (nextPage != number) {
                if (page) clip.release(page);
                page = clip.acquire(nextPage);
                number = nextPage;
            }
            const auto pageFrame = frame % clip.pageFrames;
            const auto frames = static_cast<std::uint32_t>(std::min(clip.pageFrames-pageFrame,
                static_cast<std::int64_t>(clip.sourceLength)-frame));
            if (!page) { missing = true; return {nullptr,frames}; }
            return {page->data.get()+static_cast<std::size_t>(pageFrame)*clip.channels,frames};
        }
        audio::WarpStreamInput input() noexcept {
            return {this, [](void* context, std::int64_t frame, std::uint16_t channel) noexcept {
                const auto found = static_cast<StreamReader*>(context)->pcm(frame);
                return found.data ? found.data[channel] : 0.0f;
            }, [](void* context,std::int64_t frame) noexcept {
                return static_cast<StreamReader*>(context)->pcm(frame);
            }};
        }
    };
    bool reserve(std::int64_t number) noexcept {
        for(auto* page:admitted)if(page && page->number.load(std::memory_order_relaxed)==number)return true;
        for(auto& page:admitted)if(!page){page=acquire(number);return page!=nullptr;}
        return false;
    }
    void readSource(std::uint64_t first,float* out,std::uint32_t count) {
        // The worker reads each requested source span contiguously. In a reversed
        // window, that span lives at the reflected offset with the frames reversed.
        first=sourceStart+(reverse?sourceLength-first-count:first);
        if(progressive) {
            const auto result=progressive->read(first,std::span(out,static_cast<std::size_t>(count)*channels));
            if(result==audio::ProgressiveRead::Pending)throw PrefixPending{};
            if(result!=audio::ProgressiveRead::Frames)throw std::runtime_error("progressive source failed: "+progressive->error());
        } else reader->read(first, out, count);
        if(reverse)for(std::uint32_t i=0;i<count/2;++i) {
            auto* left=out+static_cast<std::size_t>(i)*channels;
            auto* right=out+static_cast<std::size_t>(count-1-i)*channels;
            std::swap_ranges(left,left+channels,right);
        }
    }
    std::vector<float> renderRange(std::int64_t first, std::int64_t count, std::uint32_t currentRate) {
        if (currentRate == sourceRate) {
            std::vector<float> buf(static_cast<std::size_t>(count)*channels, 0.0f);
            const auto readBegin = std::max<std::int64_t>(0, first);
            const auto readEnd = std::min<std::int64_t>(static_cast<std::int64_t>(sourceLength), first + count);
            if (readEnd > readBegin) {
                auto* dst = buf.data() + static_cast<std::size_t>(readBegin - first) * channels;
                readSource(static_cast<std::uint64_t>(readBegin),dst,static_cast<std::uint32_t>(readEnd-readBegin));
            }
            return buf;
        }

        std::uint32_t priorRate = sourceRate;
        while (static_cast<double>(currentRate) / priorRate > 256.0) {
            priorRate *= 128;
        }
        while (static_cast<double>(priorRate) / currentRate > 256.0) {
            priorRate /= 128;
        }

        const std::int64_t divisor=std::gcd(currentRate, priorRate);
        const std::int64_t inPeriod=priorRate/divisor, outPeriod=currentRate/divisor;
        const std::int64_t guard=1024*std::max<std::int64_t>(1,(priorRate+currentRate-1)/currentRate);
        const auto anchor=(first/outPeriod)*inPeriod;
        const auto begin=anchor-((guard+inPeriod-1)/inPeriod)*inPeriod;
        const auto outBegin=(begin/inPeriod)*outPeriod;
        const auto inputCount=static_cast<std::int64_t>(std::ceil(static_cast<double>(first+count-outBegin)*priorRate/currentRate))+guard;
        const auto outputCount=static_cast<std::int64_t>(std::ceil(static_cast<double>(inputCount)*currentRate/priorRate))+32;

        std::vector<float> input = renderRange(begin, inputCount, priorRate);
        std::vector<float> output(static_cast<std::size_t>(outputCount)*channels,0.0f);

        SRC_DATA data{};
        data.data_in=input.data(); data.data_out=output.data();
        data.input_frames=static_cast<long>(inputCount); data.output_frames=static_cast<long>(outputCount);
        data.src_ratio=static_cast<double>(currentRate)/priorRate; data.end_of_input=1;
        const int error=src_simple(&data,SRC_SINC_BEST_QUALITY,channels);
        if(error!=0 || data.output_frames_gen<first-outBegin+count) throw std::runtime_error("resampler did not fill page");

        std::vector<float> cropped(static_cast<std::size_t>(count)*channels);
        std::copy_n(output.data()+static_cast<std::size_t>(first-outBegin)*channels,
                    static_cast<std::size_t>(count)*channels, cropped.data());
        return cropped;
    }

    void fill(Page& p, std::int64_t number) {
        const auto first=number*pageFrames;
        const auto sampleCount=static_cast<std::size_t>(pageFrames)*channels;
        if(!p.data) {
            p.data=std::make_unique<float[]>(sampleCount);
            ++allocatedPages;
        } else {
            std::fill_n(p.data.get(),sampleCount,0.0f);
        }
        const auto count=std::min(pageFrames,pageSourceEnd()-first);
        if (r3Warp) {
            const WarpR3Source source{this, [](void* context,std::uint64_t frame,float* output,std::uint32_t frames) {
                auto& clip = *static_cast<Clip*>(context);
                // WarpR3Pages supplies file offsets. The clip reader applies
                // its authored source-window offset to relative frames.
                clip.readSource(frame-clip.sourceStart,output,frames);
            }};
            if (!r3Warp->render(first,static_cast<std::uint32_t>(count),p.data.get(),source))
                throw std::runtime_error("R3 Warp rate or source is outside supported bounds");
        } else if (streamingWarp || rate==sourceRate) {
            readSource(static_cast<std::uint64_t>(first),p.data.get(),static_cast<std::uint32_t>(count));
            if (streamingWarp) for (std::size_t i=0; i<static_cast<std::size_t>(count)*channels; ++i)
                if (!std::isfinite(p.data[i])) p.data[i] = 0;
        } else if (!src_is_valid_ratio(static_cast<double>(rate) / sourceRate)) {
            std::vector<float> res = renderRange(first, count, rate);
            std::copy_n(res.begin(), static_cast<std::size_t>(count)*channels, p.data.get());
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
            // The right guard supplies filter context. Its output is outside
            // this page and need not be computed.
            const auto outputCount=first-outBegin+count;
            std::vector<float> input(static_cast<std::size_t>(inputCount)*channels,0.0f);
            const auto readBegin=std::max<std::int64_t>(0,begin);
            const auto readEnd=std::min<std::int64_t>(static_cast<std::int64_t>(sourceLength),begin+inputCount);
            if (readEnd>readBegin) {
                auto* dst=input.data()+static_cast<std::size_t>(readBegin-begin)*channels;
                readSource(static_cast<std::uint64_t>(readBegin),dst,static_cast<std::uint32_t>(readEnd-readBegin));
            }
            if(sourceRate==44100 && rate==48000 && channels==2 &&
               detail::renderSinc441To480(input,first-outBegin,
                   std::span(p.data.get(),static_cast<std::size_t>(count)*channels))) {
                p.number=number;
                return;
            }
            std::vector<float> output(static_cast<std::size_t>(outputCount)*channels,0.0f);
            SRC_DATA data{};
            data.data_in=input.data(); data.data_out=output.data();
            data.input_frames=static_cast<long>(inputCount); data.output_frames=static_cast<long>(outputCount);
            data.src_ratio=static_cast<double>(rate)/sourceRate; data.end_of_input=1;
            const int error=src_simple(&data,SRC_SINC_BEST_QUALITY,channels);
            if(error!=0 || data.output_frames_gen!=outputCount) throw std::runtime_error("resampler did not fill page");
            std::copy_n(output.data()+static_cast<std::size_t>(first-outBegin)*channels,
                        static_cast<std::size_t>(count)*channels,p.data.get());
        }
        p.number=number;
    }
    bool reclaimPages(std::uint32_t seq) noexcept {
        if(!allocatedPages)return false;
        bool reclaimed=false;
        for(auto& page:pages) {
            if(!page.data)continue;
            int state=page.state.load(std::memory_order_relaxed);
            if(state!=0 && state!=2)continue;
            if(!page.state.compare_exchange_strong(state,1,std::memory_order_acquire))continue;
            // A seek may have posted fresh needs or admitted a page just as
            // the worker tried to retire it. Keep the new request intact.
            if(requestSeq.load(std::memory_order_seq_cst)!=seq) {
                page.state.store(state,std::memory_order_release);
                break;
            }
            page.data.reset();
            --allocatedPages;
            page.number=-1;
            page.state.store(0,std::memory_order_release);
            reclaimed=true;
        }
        return reclaimed;
    }

    bool service(const ClipPlayback::RequestSnapshotProbeForTest* probe,bool interleave) {
        if(failed)return false;
        if (streamingWarp && !streamingWarp->ready()) {
            try {
                if (!streamingWarp->prepare()) return false;
            } catch (...) {
                failed=true;
                errors.fetch_add(1,std::memory_order_relaxed);
                return false;
            }
        }
        if(prepared && !reader) {
            try {
                reader = prepared->open();
                if(!reader)return false;
                const auto& wav = reader->wav;
                if(wav.channels()!=channels || wav.sampleRate()!=preparedRate || wav.frames()!=sourceLength)
                    throw std::runtime_error("prepared clip source dimensions changed");
            } catch(...) {
                failed=true;
                errors.fetch_add(1,std::memory_order_relaxed);
                return false;
            }
        }
        if(progressive) {
            const auto status=progressive->snapshot().status;
            if(status==audio::ProgressiveStatus::Failed || status==audio::ProgressiveStatus::Cancelled) {
                failed=true;errors.fetch_add(1,std::memory_order_relaxed);return false;
            }
        }
        std::array<std::int64_t,slots> w;
        const auto seq=requestSeq.load(std::memory_order_seq_cst);
        if ((seq&1u)!=0) return false;
        for(std::size_t i=0;i<slots;++i) w[i]=requests[i].load(std::memory_order_seq_cst);
        if(requestSeq.load(std::memory_order_seq_cst)!=seq) return false;
        struct CompletedProbe {
            const ClipPlayback::RequestSnapshotProbeForTest* probe;
            ~CompletedProbe(){if(probe && probe->afterService)probe->afterService(probe->context);}
        } completed{probe};
        if(probe && probe->afterSnapshot)probe->afterSnapshot(probe->context,w.front());
        if(w.front()<0) {
            if(!allocatedPages)return false;
            // Hold a short window for nearby seeks before returning the
            // storage of clips outside the current block and read-ahead.
            const auto now=std::chrono::steady_clock::now();
            if(releaseAfter==std::chrono::steady_clock::time_point{})
                releaseAfter=now+std::chrono::milliseconds(250);
            const bool reclaimed=now>=releaseAfter && reclaimPages(seq);
            if(reclaimed && r3Warp && !allocatedPages) r3Warp->releaseIdle();
            return reclaimed;
        }
        releaseAfter={};
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
                const auto oldPage=p.number.load(std::memory_order_relaxed);
                // Never claim, even briefly, a ready page of this same request:
                // a reader inside that claim would see a cached page as missing.
                // Only this worker writes number, so the unowned read is exact.
                if(state==2 && std::find(w.begin(),w.end(),oldPage)!=w.end()) continue;
                if(probe && probe->beforePageClaim)probe->beforePageClaim(probe->context,oldPage,number);
                const bool claimed=p.state.compare_exchange_strong(state,1,std::memory_order_acquire);
                if(probe && probe->afterPageClaimAttempt)probe->afterPageClaimAttempt(probe->context,oldPage,number,claimed);
                if(!claimed)continue;
                // The audio driver can replace the request while this worker
                // is walking an older snapshot. Recheck AFTER owning the page:
                // a ready query that acquired/released this page after posting
                // its request synchronizes with this claim, so stale work cannot
                // evict a page it just admitted. If we claimed first, the query
                // sees worker-owned state and refuses instead.
                if(requestSeq.load(std::memory_order_seq_cst)!=seq) {
                    p.state.store(state,std::memory_order_release);
                    return worked;
                }
                try { fill(p,number); }
                catch(const PrefixPending&) { p.number=-1;p.state.store(0,std::memory_order_release);break; }
                catch(...) { failed=true;errors.fetch_add(1,std::memory_order_relaxed); p.number=-1; }
                p.state.store(p.number>=0?2:0,std::memory_order_release);
                if(interleave)return true;
                worked=true; break;
            }
        }
        return worked;
    }
};
struct PitchComparison : Sequenced {
    const Clip* clip = nullptr;
    mutable audio::PitchComparePlayback playback;

    PitchComparison(const Clip* target, std::shared_ptr<const audio::PitchComparePcm> pcm,
                    std::uint32_t fadeFrames)
        : clip(target), playback(std::move(pcm), fadeFrames) {}

    bool renderRePitch(std::int64_t time, std::span<float> output) const noexcept {
        using Side = audio::PitchCompareSide;
        const auto render = [&](Side side, std::span<float> frame) {
            struct Input {
                std::span<const float> pcm;
                std::uint16_t channels;

                audio::WarpStreamPcm run(std::int64_t position) const noexcept {
                    if (position < 0 || static_cast<std::uint64_t>(position) >= pcm.size() / channels)
                        return {};
                    const auto index = static_cast<std::size_t>(position) * channels;
                    return {pcm.data() + index, static_cast<std::uint32_t>((pcm.size() - index) / channels)};
                }
            } input{playback.source(side), clip->channels};
            const audio::WarpStreamInput reader{
                &input,
                [](void* context, std::int64_t position, std::uint16_t channel) noexcept {
                    const auto run = static_cast<const Input*>(context)->run(position);
                    return run.data ? run.data[channel] : 0.0f;
                },
                [](void* context, std::int64_t position) noexcept {
                    return static_cast<const Input*>(context)->run(position);
                }};
            return clip->streamingWarp->renderFrame(time, reader, frame);
        };
        const auto mix = playback.nextMix();
        if (mix == 0.0f) return render(Side::Original, output);
        if (mix == 1.0f) return render(Side::Corrected, output);
        std::array<float, 64> corrected;
        if (!render(Side::Original, output) ||
            !render(Side::Corrected, std::span(corrected.data(), output.size()))) return false;
        for (std::size_t channel = 0; channel < output.size(); ++channel)
            output[channel] = std::lerp(output[channel], corrected[channel], mix);
        return true;
    }
};

class TrackSource final : public Node {
public:
    Transport& transport;
    std::int32_t maxFrames;
    std::int32_t channels;
    std::int64_t track;
    std::int32_t playbackDelay = 0;
    const std::span<const std::int64_t>* recordingTracks = nullptr;
    std::vector<Clip*> clips;
    std::weak_ptr<void> owner;
    std::shared_ptr<const LaunchAudioRuntime> launchAudio;
    std::shared_ptr<LaunchOverlayDelay> launchDelay;
    std::vector<float> launchNarrow;
    std::vector<double> launchWide;
    std::vector<std::uint8_t> launchReplaces;
    SnapshotPublisher<PitchComparison>& pitchComparisons;
    // ADR-0207 d2: the clips of one track are a mix point, summed in double
    // and converted once. Sized here, on the message thread, at the shape the
    // session builds this rebuild's graph at, and never resized after.
    std::vector<double> wide;
    std::shared_ptr<void> sourceLifetime() const override { return owner.lock(); }
    /// ADR-0092: new at every rebuild, the same track's clips all the same.
    std::uint64_t historyName() const noexcept override { return 1; }
    TrackSource(Transport& t,std::int32_t f,std::int32_t ch,std::int64_t id,
                SnapshotPublisher<PitchComparison>& comparisonPublisher)
        :transport(t),maxFrames(f),channels(ch),track(id),pitchComparisons(comparisonPublisher),
         wide(static_cast<std::size_t>(ch)*static_cast<std::size_t>(f),0.0){}
    bool processesDouble() const noexcept override { return true; }
    std::int32_t latencySamples() const noexcept override { return playbackDelay; }
    void attachLaunch(std::shared_ptr<const LaunchAudioRuntime> runtime, const TrackSource* before) {
        launchAudio = std::move(runtime);
        if (!launchAudio) return;
        playbackDelay = std::max(playbackDelay, launchAudio->latencyForTrack(track));
        if (playbackDelay == 0) return;
        for (auto* clip : clips) clip->playbackDelay = playbackDelay;

        if (before && before->launchDelay && before->playbackDelay == playbackDelay && before->channels == channels &&
            before->launchAudio->incarnation() == launchAudio->incarnation()) {
            // Only the graph selected by the driver writes the shared history.
            // The owner does not inspect or copy its live PCM.
            launchDelay = before->launchDelay;
        } else {
            launchDelay = std::make_shared<LaunchOverlayDelay>(playbackDelay, channels);
        }
        const auto samples = static_cast<std::size_t>(maxFrames) * static_cast<std::size_t>(channels);
        launchNarrow.resize(samples);
        launchWide.resize(samples);
        launchReplaces.resize(static_cast<std::size_t>(maxFrames));
    }
    void process(const NodeIo& io) noexcept override {
        if(io.wide()) play<double>(io,io.out64);
        else play<float>(io,io.out);
        if (!launchAudio) return;
        if (!launchDelay) {
            launchAudio->overlay(track, io);
        } else if (io.channels == channels && io.blockOffset >= 0 && io.frames >= 0 &&
                   io.blockOffset <= maxFrames - io.frames) {
            if (io.wide()) delayOverlay(io, io.out64, launchWide.data());
            else delayOverlay(io, io.out, launchNarrow.data());
        }
    }
    template<class T>
    void delayOverlay(const NodeIo& io, T* const* output, T* scratch) noexcept {
        std::array<T*, 64> channelsOut{};
        for (std::int32_t channel = 0; channel < channels; ++channel) {
            auto* plane = scratch + static_cast<std::size_t>(channel) * static_cast<std::size_t>(maxFrames);
            std::fill_n(plane + io.blockOffset, io.frames, T{0});
            channelsOut[static_cast<std::size_t>(channel)] = plane;
        }
        std::fill_n(launchReplaces.data() + io.blockOffset, io.frames, std::uint8_t{0});
        auto slot = io;
        slot.out = nullptr;
        slot.out64 = nullptr;
        if constexpr (std::is_same_v<T,double>) slot.out64 = channelsOut.data();
        else slot.out = channelsOut.data();
        launchAudio->overlay(track, slot, {launchReplaces.data(), launchReplaces.size()});
        launchDelay->mix(transport, io.blockOffset, io.frames, channelsOut.data(), launchReplaces.data(), output);
    }
    template <typename T>
    void play(const NodeIo& io, T* const* out) noexcept {
        SnapshotPublisher<PitchComparison>::AudioRead comparison(pitchComparisons);
        const auto* activeComparison = io.exporting ? nullptr : comparison.get();
        for(std::int32_t c=0;c<io.channels;++c) std::fill_n(out[c]+io.blockOffset,io.frames,T{0});
        const bool recording = recordingTracks && !recordingTracks->empty() &&
            std::binary_search(recordingTracks->data(), recordingTracks->data() + recordingTracks->size(), track);
        if(!transport.playing() || recording || (!clips.empty() && clips.front()->monitorInputOnly)){
            if(io.blockOffset==0) {
                // Keep looking ahead while output is suppressed so playback can resume.
                // Candidate preparation keeps the captured transport intact.
                auto nextPlay=transport;
                if(!nextPlay.playing())nextPlay.play();
                for(std::size_t clipIndex=0;clipIndex<clips.size();++clipIndex) {
                    auto* clip=clips[clipIndex];
                    clip->releaseAdmission();
                    clip->request(clip->wanted(nextPlay,maxFrames));
                }
            }
            return;
        }
        const auto offset = std::max(io.blockOffset, delayLeadIn(transport,playbackDelay));
        const auto audibleFrames=std::max(0,io.blockOffset+io.frames-offset);
        const auto firstSample = playbackSample(transport,offset,playbackDelay);
        const auto firstFrames = contiguousPlayback(transport,offset,audibleFrames,playbackDelay);
        std::int64_t wrappedSample = 0, wrappedFrames = 0;
        if (firstFrames < audibleFrames) {
            const auto wrapOffset = offset + firstFrames;
            wrappedSample = playbackSample(transport,wrapOffset,playbackDelay);
            // If the block wraps again, the second range already covers the
            // whole loop. Further laps visit only samples in that range.
            wrappedFrames = contiguousPlayback(transport,wrapOffset,audibleFrames-firstFrames,playbackDelay);
        }
        const auto intersects = [](const Clip& clip, std::int64_t start, std::int64_t frames) {
            // Use a distance to avoid overflowing an exclusive end near the
            // transport's saturated sample position.
            return frames > 0 && start < clip.end &&
                   (clip.start <= start || clip.start - start < frames);
        };
        // One clip is not a sum: its samples go straight out, bit for bit.
        // Two or more may overlap, and then every sample is summed in double:
        // at 32 into `wide`, converted once at the end; at 64 the output is
        // double already and is the sum. The guard is the shape the
        // accumulator was built for; the session builds this at the graph's
        // own channels and block size.
        bool sum=false;
        if constexpr (std::is_same_v<T,float>)
            sum=clips.size()>1 && io.channels<=channels && io.blockOffset+io.frames<=maxFrames;
        auto at=[&](std::int32_t c,std::int32_t i)->double& {
            return wide[static_cast<std::size_t>(c)*static_cast<std::size_t>(maxFrames)+static_cast<std::size_t>(io.blockOffset+i)];
        };
        if(sum) for(std::int32_t c=0;c<io.channels;++c) for(std::int32_t i=0;i<io.frames;++i) at(c,i)=0.0;
        // Raw indices avoid MSVC Debug vector-iterator ownership locks.
        for(std::size_t clipIndex=0;clipIndex<clips.size();++clipIndex) {
            auto* clip=clips[clipIndex];
            const auto* compare = activeComparison && activeComparison->clip == clip ? activeComparison : nullptr;
            if(io.blockOffset==0){
                const auto required=clip->wanted(transport,maxFrames,false);
                clip->trimAdmission(required);
                clip->request(clip->wanted(transport,maxFrames));
                for(const auto number:required)if(number>=0)(void)clip->reserve(number);
            }
            if((clip->pitchComparisonPlan && !compare && clip->preparationPending()) ||
               (!intersects(*clip, firstSample, firstFrames) && !intersects(*clip, wrappedSample, wrappedFrames)))
                continue;
            Page* page=nullptr; std::int64_t number=-1; std::uint32_t missing=0;
            Clip::StreamReader streamReader{*clip};
            const auto streamInput=streamReader.input();
            std::array<float,64> warpedFrame;
            WarpClipPlayer::RenderCursor warpCursor;
            std::array<float,64> comparisonFrame;
            const std::span<float> comparisonSamples{comparisonFrame.data(),clip->channels};
            for(std::int32_t i=offset-io.blockOffset;i<io.frames;++i) {
                const auto time=playbackSample(transport,io.blockOffset+i,playbackDelay);
                const float* sample = nullptr;
                if (clip->streamingWarp) {
                    if (time < clip->start || time >= clip->end) continue;
                    streamReader.missing = false;
                    const std::span warpedSamples(warpedFrame.data(), clip->channels);
                    const bool rendered = compare ? compare->renderRePitch(time, warpedSamples)
                        : clip->streamingWarp->renderFrame(warpCursor, time, streamInput, warpedSamples);
                    if (!rendered) {
                        if (!clip->invalidWarpRate.exchange(true,std::memory_order_relaxed))
                            clip->errors.fetch_add(1,std::memory_order_relaxed);
                        continue;
                    }
                    if (streamReader.missing) { ++missing; continue; }
                    sample = warpedFrame.data();
                } else {
                    const auto n=clip->sourceAt(time);
                    if(n<0) continue;
                    if(compare) {
                        const auto frame = clip->reverse && clip->pitchComparisonPlan->warp
                            ? clip->sourceLength - 1 - static_cast<std::uint64_t>(n)
                            : static_cast<std::uint64_t>(n);
                        if(!compare->playback.readAt(frame,comparisonSamples)) continue;
                        sample=comparisonFrame.data();
                    } else {
                        if(n/clip->pageFrames!=number) {
                            if(page) clip->release(page);
                            number=n/clip->pageFrames; page=clip->acquire(number);
                        }
                        if(!page) { ++missing; continue; }
                        sample=page->data.get()+static_cast<std::size_t>(n%clip->pageFrames)*clip->channels;
                    }
                }
                if constexpr (std::is_same_v<T,float>) {
                    const float gain=clip->envelope(time);
                    for(std::int32_t c=0;c<io.channels;++c) {
                        const float value=clip->valueOf<float>(sample,c);
                        if(sum) at(c,i)+=gain*value;
                        else out[c][io.blockOffset+i]+=gain*value;
                    }
                } else {
                    const double gain=clip->envelopeWide(time);
                    for(std::int32_t c=0;c<io.channels;++c)
                        out[c][io.blockOffset+i]+=gain*clip->valueOf<double>(sample,c);
                }
            }
            if(page) clip->release(page);
            if(missing) clip->underruns.fetch_add(missing,std::memory_order_relaxed);
        }
        if(sum) for(std::int32_t c=0;c<io.channels;++c) for(std::int32_t i=0;i<io.frames;++i)
            out[c][io.blockOffset+i]=static_cast<float>(at(c,i));
    }
    const char* name() const noexcept override { return "disk clips"; }
};
}
struct ClipPlayback::Impl {
    Transport& transport;
    std::int32_t maxFrames, channels;
    const bool synchronousReads;
    std::optional<WarpArrangementClock> warpClock;
    std::vector<std::unique_ptr<Clip>> clips;
    SnapshotPublisher<PitchComparison> pitchComparisons;
    std::vector<ClipMediaSource> externalSources;
    std::vector<std::unique_ptr<TrackSource>> tracks;
    std::vector<std::string> problems;
    std::size_t fatalProblemCount = 0;
    std::unordered_set<std::int64_t> unavailableClips;
    std::unordered_set<std::int64_t> unavailableMedia;
    std::vector<std::size_t> missingMediaProblems;
    std::map<std::int64_t, DecodeProblem> terminalProblems;
    std::chrono::steady_clock::time_point nextDecodePoll;
    std::atomic<bool> stalled{false};
    std::atomic<const ClipPlayback::RequestSnapshotProbeForTest*> requestProbe{nullptr};
    // Last member so all workers join before any of their clips are destroyed.
    std::vector<std::jthread> workers;
    Impl(Transport& t,std::int32_t f,std::int32_t ch,bool synchronous)
        :transport(t),maxFrames(f),channels(ch),synchronousReads(synchronous){}
    ~Impl() { for(auto& worker:workers) worker.request_stop(); }
    std::uint32_t sampleRate = 0;
};
ClipPlayback::ClipPlayback(std::shared_ptr<const ClipProject> project, Transport& transport,
                           double rate,std::int32_t channels,std::int32_t maxFrames, audio::ProgressiveDecoder* decoder,
                           bool synchronousReads, WarpPlayback warpPlayback)
    :impl_(std::make_shared<Impl>(transport,maxFrames,channels,synchronousReads)) {
    if(!project) return;
    if(!std::isfinite(rate) || rate<44100 || rate>768000 || rate!=std::floor(rate) || channels<1 || channels>64 || maxFrames<1 || maxFrames>4096)
        throw std::runtime_error("clip playback format outside 44.1-768 kHz integer rate / 1-64 channels / 1-4096 frames");
    impl_->sampleRate = static_cast<std::uint32_t>(rate);
    const auto tempo = TempoMap::fromRows(project->rows.tempo);
    impl_->warpClock.emplace(tempo,rate);
    const auto& warpClock = *impl_->warpClock;
    std::unordered_map<fs::path, SourceReaders> readers;
    if (!project->pitchEdits.error.empty()) throw std::runtime_error(project->pitchEdits.error);
    std::unordered_map<const audio::WarpTask*, std::weak_ptr<PreparedSource>> warpedReaders;
    std::unordered_map<std::int64_t, RelocatedSource> relocatedSources;
    const auto mediaRow = [&](std::int64_t id) -> const rows::Media* {
        const auto found = std::find_if(project->rows.media.begin(), project->rows.media.end(),
                                       [id](const auto& media) { return media.id == id; });
        return found == project->rows.media.end() ? nullptr : &*found;
    };
    const auto mediaPath = [&](const rows::Media& source) {
        return clipMediaPath(*project,source,relocatedSources);
    };
    std::unordered_map<std::int64_t, bool> skippedMediaAvailable;
    const auto checkSkippedMedia = [&](std::int64_t clipId, std::int64_t mediaId) {
        auto [known, inserted] = skippedMediaAvailable.try_emplace(mediaId, false);
        if (inserted) {
            try {
                const auto* source = mediaRow(mediaId);
                std::error_code error;
                known->second = source && !source->embedded && fs::is_regular_file(mediaPath(*source), error);
            } catch (const std::exception&) {} // No reader is opened for an unplayed clip.
        }
        if (!known->second) impl_->unavailableMedia.insert(clipId);
    };
    for(const auto& row:timelineClips(project->rows)) {
        if(row.kind!="audio") continue;
        const auto a=std::find_if(project->rows.audioClips.begin(),project->rows.audioClips.end(),[&](const auto& x){return x.clipId==row.id;});
        if (row.muted) {
            if (a != project->rows.audioClips.end()) checkSkippedMedia(row.id, a->mediaId);
            continue;
        }
        const auto label="clips#"+std::to_string(row.id)+": ";
        bool openingMedia = false;
        try {
            if(a==project->rows.audioClips.end()) throw std::runtime_error("no audio row");
            const auto* source = mediaRow(a->mediaId);
            const clip_edit::ClipPitchEdits* pitch = nullptr;
            if (source && !source->embedded && project->pitchEdits.clips) {
                const auto& pitches = *project->pitchEdits.clips;
                if (const auto found = pitches.find(row.id); found != pitches.end()) {
                    if (found->second.blocksSource(source->hashBlake3)) throw std::runtime_error(found->second.error);
                    const auto& authored = found->second.edits;
                    if (authored && authored->sourceHash == source->hashBlake3 && !authored->rows.empty())
                        pitch = &*authored;
                }
            }
            if(!a->warpEnabled && a->formantShift!=0) {
                checkSkippedMedia(row.id, a->mediaId);
                throw std::runtime_error("unwarped formant shift unsupported");
            }
            const auto warpMode = a->warpEnabled ? audio::parseWarpMode(a->warpMode) : std::nullopt;
            if (a->warpEnabled && !warpMode) throw std::runtime_error("warp mode is not available");
            const bool arrangementStreaming = warpPlayback == WarpPlayback::Streaming;
            // Re-Pitch reads the same native-rate window before and after
            // flattening. Other edited Warp modes still prepare the composite.
            const bool nativePitchRePitch = pitch && arrangementStreaming && warpMode == audio::WarpMode::RePitch;
            const bool nativeWarp = (!pitch || nativePitchRePitch) &&
                (arrangementStreaming || warpPlayback == WarpPlayback::NativePages) &&
                warpMode && a->formantShift == 0 && (!a->reverse || arrangementStreaming) &&
                (*warpMode == audio::WarpMode::Beats || *warpMode == audio::WarpMode::Texture ||
                 *warpMode == audio::WarpMode::RePitch);
            const bool r3Mode = !pitch && (arrangementStreaming || warpPlayback == WarpPlayback::AlignedR3) &&
                warpMode && !a->reverse &&
                (*warpMode==audio::WarpMode::Tones || *warpMode==audio::WarpMode::Complex ||
                 *warpMode==audio::WarpMode::ComplexPro);
            if(!a->warpEnabled && !(std::abs(a->transposeSemis)<=48.)) throw std::runtime_error("invalid unwarped pitch");
            if(row.aliasOf) throw std::runtime_error("alias resolution unsupported");
            if(row.fadeInCurve<0 || row.fadeInCurve>5 || row.fadeOutCurve<0 || row.fadeOutCurve>5) throw std::runtime_error("fade curve unsupported");
            if(a->channelMode<0 || a->channelMode>3) throw std::runtime_error("unknown channel mode");
            openingMedia = true;
            if (!source) throw MissingClipMedia("missing media");
            if (source->embedded) throw std::runtime_error("embedded media; extract first");
            auto clip=std::make_unique<Clip>();
            clip->clipId=row.id; clip->mediaId=source->id; clip->storedBlake3=source->hashBlake3;
            clip->reverse=a->reverse;
            clip->path = mediaPath(*source);
            // Reuse the open stream for each source within this generation.
            // Plain compressed clips consume progressive prefixes. For Warp,
            // gather only dimensions here and let its worker decode/render.
            auto& opened = readers[clip->path];
            std::optional<std::uint64_t> sourceFrames;
            clip->reader = opened.native.lock();
            if (!clip->reader && !opened.refusedNative) {
                try {
                    clip->reader = std::make_shared<WavSource>(clip->path);
                    opened.native = clip->reader;
                } catch (const std::exception&) { opened.refusedNative = true; }
            }
            if (!clip->reader) {
                std::error_code inspectError;
                if (!fs::exists(clip->path, inspectError) && !inspectError)
                    throw MissingClipMedia("missing audio file");
                if(!opened.compressedInfo) opened.compressedInfo=audio::inspectPcm(clip->path);
                const auto& info=*opened.compressedInfo;
                if(a->warpEnabled || pitch) {
                    if(info.channels<1 || info.channels>64)
                        throw std::runtime_error("source channel count outside supported bounds");
                    clip->channels=static_cast<std::uint16_t>(info.channels);
                    clip->sourceRate=info.sampleRate;
                    sourceFrames=info.frames;
                    if (!pitch && (nativeWarp || r3Mode) && info.frames && *info.frames) {
                        auto& service=decoder?*decoder:audio::defaultProgressiveDecoder();
                        clip->progressive=service.openSaved(clip->path,info,source->hashBlake3);
                    }
                } else if(info.frames && *info.frames) {
                    auto& service=decoder?*decoder:audio::defaultProgressiveDecoder();
                    clip->progressive=service.openSaved(clip->path,info,source->hashBlake3);
                    clip->channels=static_cast<std::uint16_t>(clip->progressive->info().channels);
                    clip->sourceRate=clip->progressive->info().sampleRate;
                    sourceFrames=*clip->progressive->info().frames;
                }
            }
            if(!clip->reader && !clip->progressive && !a->warpEnabled && !pitch) {
                clip->reader = opened.decoded.lock();
                if (!clip->reader) {
                    clip->reader = std::make_shared<WavSource>(clip->path, true);
                    opened.decoded = clip->reader;
                }
            }
            if(clip->reader) {
                const auto& wav = clip->reader->wav;
                clip->channels=wav.channels();clip->sourceRate=wav.sampleRate();sourceFrames=wav.frames();
            }
            clip->rate=static_cast<std::uint32_t>(rate);
            if(clip->channels>64 || clip->sourceRate<1 || clip->sourceRate>768000)throw std::runtime_error("source format outside supported bounds");
            if(a->srcStartFrames<0 || a->srcLenFrames<=0)
                throw std::runtime_error("source window outside audio");
            clip->pageFrames=static_cast<std::int64_t>(std::ceil(8192.0*rate/48000.0));
            clip->sourceStart=static_cast<std::uint64_t>(a->srcStartFrames); clip->sourceLength=static_cast<std::uint64_t>(a->srcLenFrames);
            if(sourceFrames && (clip->sourceStart>*sourceFrames || clip->sourceLength>*sourceFrames-clip->sourceStart))
                throw std::runtime_error("source window outside audio");
            openingMedia = false;
            clip->preparedRate=clip->sourceRate;
            if(!a->warpEnabled)
                clip->sourceRate=static_cast<std::uint32_t>(audio::pitchedSampleRate(clip->sourceRate,a->transposeSemis));
            clip->length=samples(static_cast<double>(clip->sourceLength)/clip->sourceRate,rate);
            audio::WarpRenderPlan spec;
            audio::WarpMapRead map;
            if (warpMode) {
                if(row.contentOffsetTicks<0 || !a->warpMarkers)
                    throw std::runtime_error("warp needs valid source offsets and a stored marker map");
                map=audio::readWarpMap(*a->warpMarkers,clip->sourceLength);
                if(!map)throw std::runtime_error("warp marker map is not readable or canonical");
                if(!std::isfinite(a->formantShift) || std::abs(a->formantShift)>96)
                    throw std::runtime_error("invalid warp formant shift");
                spec.source=clip->path;
                spec.sourceStart=clip->sourceStart;
                spec.sourceFrames=clip->sourceLength;
                spec.outputRate=clip->rate;
                spec.mode=*warpMode;
                if(spec.mode==audio::WarpMode::Tones)spec.grainSeconds=0.03;
                // Re-Pitch derives pitch from the rate and preserves the
                // authored transpose for selection of another mode.
                spec.transposeSemis=*warpMode==audio::WarpMode::RePitch?0.0:a->transposeSemis;
                spec.formantScale=std::exp2(a->formantShift/12);
            }
            const auto beginNs = row.posNs.value_or(0);
            // Project stored nanoseconds as the edit clock and Snapshot do: an
            // integer nanosecond rounded below its originating tick maps back to it.
            const auto origin=row.timeBase==0?row.posTicks.value_or(0):tempo.storedNanosecondsToTicks(beginNs);
            if(origin<0) throw std::runtime_error("negative musical time unsupported by TempoMap");
            auto tickSpanSeconds=[&](std::int64_t ticks){
                if(ticks<0 || origin>std::numeric_limits<std::int64_t>::max()-ticks) throw std::runtime_error("invalid tick duration");
                return tempo.ticksToSeconds(origin+ticks)-tempo.ticksToSeconds(origin);
            };
            auto duration=[&](std::int64_t ticks){return samples(tickSpanSeconds(ticks),rate);};
            if(row.contentOffsetTicks<0 && (a->warpEnabled || !row.contentOffsetNs))
                throw std::runtime_error("negative clip content offset needs an unwarped source clock");
            const auto sourceOrigin=a->warpEnabled?origin-row.contentOffsetTicks:origin;
            std::optional<audio::WarpRenderPlan> pitchWarp;
            std::int64_t endTicks=origin;
            if(row.timeBase==0) {
                const auto ticks=row.lengthTicks.value_or(0);
                if(ticks<0 || origin>std::numeric_limits<std::int64_t>::max()-ticks) throw std::runtime_error("invalid clip duration");
                endTicks+=ticks;
                clip->start=samples(tempo.ticksToSeconds(origin),rate);
                clip->end=samples(tempo.ticksToSeconds(endTicks),rate);
            } else {
                const auto nanos=row.lengthNs.value_or(0);
                if(beginNs<0 || nanos<0 || beginNs>std::numeric_limits<std::int64_t>::max()-nanos)
                    throw std::runtime_error("invalid clip duration");
                const auto endNs = beginNs + nanos;
                clip->start=samplesAtNs(beginNs,clip->rate);
                clip->end=samplesAtNs(endNs,clip->rate);
                endTicks=tempo.storedNanosecondsToTicks(endNs);
            }
            auto r3EndTick = static_cast<double>(endTicks);
            if (r3Mode && !row.loopEnabled) {
                const auto tail = map.markers.back().tick;
                const auto end = std::min(tail,row.endMarkerTicks.value_or(tail));
                r3EndTick = std::min(r3EndTick, static_cast<double>(sourceOrigin)+static_cast<double>(end));
            }
            const bool r3 = r3Mode && audio::RealtimeWarpR3::supports(spec,clip->sourceRate,clip->channels) &&
                supportsR3Ratios(map.markers,warpClock,static_cast<double>(sourceOrigin),r3EndTick);
            const bool stream = (nativeWarp || r3) && (clip->reader || clip->progressive || nativePitchRePitch);
            if (warpPlayback == WarpPlayback::NativePages && !stream)
                throw std::runtime_error("raw Session Warp pages need a native mode and readable source");
            if (warpPlayback == WarpPlayback::NativePages && warpMode == audio::WarpMode::RePitch &&
                !supportsLaunchRePitchRates(map.markers,warpClock))
                throw std::runtime_error("Session Re-Pitch Warp rate exceeds the streaming resampler range");
            const bool reverseSynthesis = stream && clip->reverse &&
                (spec.mode == audio::WarpMode::Beats || spec.mode == audio::WarpMode::Texture);
            // Traverse the forward Beats slices or Texture grains backward.
            // Their worker and onset analysis still see the original source.
            if (reverseSynthesis) clip->reverse = false;
            if (stream && !r3)
                clip->pageFrames=static_cast<std::int64_t>(std::ceil(8192.0*clip->sourceRate/48000.0));
            auto sourceDuration=[&](std::int64_t ticks) {
                return a->warpEnabled?warpSourceSamples(tempo,sourceOrigin,ticks,rate):duration(ticks);
            };
            if(a->warpEnabled) {
                if (clip->reverse) {
                    // Persisted markers describe the forward file. Native PCM
                    // pages and the clip's musical clocks use reversed order.
                    const auto last = map.markers.back();
                    std::reverse(map.markers.begin(), map.markers.end());
                    for (auto& marker : map.markers)
                        marker = {last.sourceFrame-marker.sourceFrame,last.tick-marker.tick};
                }
                if (stream) {
                    clip->length=sourceDuration(map.markers.back().tick);
                } else {
                    spec.workFolder=fs::temp_directory_path()/"adi-warp";
                    spec.anchors=projectWarpAnchors(map.markers,tempo,sourceOrigin,rate);
                    if (clip->reverse) {
                        // The prepared renderer still reads the forward file.
                        // Its result is reversed when read into playback pages.
                        const auto last = spec.anchors.back();
                        std::reverse(spec.anchors.begin(), spec.anchors.end());
                        for (auto& anchor : spec.anchors)
                            anchor = {last.sourceFrame-anchor.sourceFrame,last.outputFrame-anchor.outputFrame};
                    }
                    const bool nativeIdentity = clip->reader && spec.mode == audio::WarpMode::Beats &&
                        clip->sourceRate == clip->rate && spec.transposeSemis == 0 && spec.formantScale == 1 &&
                        std::all_of(spec.anchors.begin(), spec.anchors.end(), [](const auto& anchor) {
                            return anchor.sourceFrame == anchor.outputFrame;
                        });
                    if (!nativeIdentity) {
                        clip->sourceStart=0;clip->sourceLength=spec.anchors.back().outputFrame;clip->sourceRate=clip->rate;
                        clip->preparedRate=clip->rate;
                        clip->length=static_cast<std::int64_t>(clip->sourceLength);
                        if (pitch) {
                            pitchWarp = std::move(spec);
                        } else {
                            auto task = audio::requestWarp(std::move(spec));
                            if(!task)throw std::runtime_error("cannot request Warp preparation here");
                            auto& shared = warpedReaders[task.get()];
                            clip->prepared = shared.lock();
                            if(!clip->prepared) {
                                clip->prepared = std::make_shared<PreparedSource>(std::move(task));
                                shared = clip->prepared;
                            }
                            clip->reader.reset();
                            clip->progressive.reset();
                        }
                    }
                }
            }
            auto fixedTime=[&](std::int64_t nanos) {return samplesAtNs(nanos,clip->rate);};
            clip->offset=!a->warpEnabled && row.contentOffsetNs?fixedTime(*row.contentOffsetNs):sourceDuration(row.contentOffsetTicks);
            if (row.loopEnabled) {
                try {
                    const auto ticks = row.loopLenTicks.value_or(0);
                    if (ticks < 0 || row.loopStartTicks < 0 || row.loopStartTicks > std::numeric_limits<std::int64_t>::max() - ticks)
                        throw std::runtime_error("invalid loop ticks");
                    const auto loopBegin = !a->warpEnabled && row.loopBeginNs ? fixedTime(*row.loopBeginNs)
                                                                           : sourceDuration(row.loopStartTicks);
                    const auto loopEnd = !a->warpEnabled && row.loopEndNs ? fixedTime(*row.loopEndNs)
                                                                         : sourceDuration(row.loopStartTicks + ticks);
                    if (loopBegin < 0 || loopEnd <= loopBegin || loopEnd > clip->length)
                        throw std::runtime_error("clip loop outside source window");
                    clip->loopBegin = loopBegin;
                    clip->loopLength = loopEnd - loopBegin;
                } catch (const std::runtime_error&) {
                    if (a->warpEnabled)
                        throw;
                    // Older projects could enable Loop with Warp off. If a tempo change
                    // puts the loop outside the file, fall through to one pass.
                }
            }
            if (clip->loopLength == 0) {
                const bool legacyLoopFallback = row.loopEnabled;
                // A physical pin survives placement and tempo edits. Warp and
                // unpinned clips derive End from the same clock as their offset.
                if (!a->warpEnabled && row.sourceEndNs) {
                    clip->length = std::min(clip->length, fixedTime(*row.sourceEndNs));
                } else if (row.endMarkerTicks) {
                    const auto marker = *row.endMarkerTicks;
                    if (!legacyLoopFallback && (a->warpEnabled || !row.contentOffsetNs) && marker <= row.contentOffsetTicks)
                        throw std::runtime_error("invalid audio clip End marker");
                    std::int64_t end;
                    if (a->warpEnabled || row.timeBase == 0) {
                        end = sourceDuration(marker);
                    } else if (!row.contentOffsetNs && marker - row.contentOffsetTicks == endTicks - origin) {
                        // A legacy marker mirroring the visible End retains
                        // the stored absolute endpoint, including sub-tick time.
                        end = clip->offset + (clip->end - clip->start);
                    } else {
                        const auto absolute = edit::addTime(beginNs, edit::nanosFromSeconds(tickSpanSeconds(marker)));
                        end = samplesAtNs(absolute, clip->rate) - clip->start;
                    }
                    if (!legacyLoopFallback && !a->warpEnabled && row.contentOffsetNs && end <= clip->offset)
                        throw std::runtime_error("invalid audio clip End marker");
                    clip->length = std::min(clip->length, end);
                }
                // An unrolled repeat offset can be outside the playable interval
                // even when it still falls within the untrimmed source window.
                if (legacyLoopFallback && clip->offset >= clip->length)
                    clip->offset = 0;
            }
            const auto& fadeClock=a->warpEnabled?row.fadeClockTicks:row.fadeClockNs;
            for(std::size_t i=0;i<4;i+=2) {
                if(fadeClock[i].has_value()!=fadeClock[i+1].has_value())throw std::runtime_error("incomplete fade clock");
                if(fadeClock[i]) {
                    const auto first=a->warpEnabled?sourceDuration(*fadeClock[i]):fixedTime(*fadeClock[i]);
                    const auto last=a->warpEnabled?sourceDuration(*fadeClock[i+1]):fixedTime(*fadeClock[i+1]);
                    // A valid stored span can quantize to a single boundary
                    // at this device rate. Render it as a step at that sample.
                    if(last<first)throw std::runtime_error("fade clock is reversed");
                    clip->fadeClock[i]=first;clip->fadeClock[i+1]=last;
                }
            }
            clip->fadeIn = duration(row.fadeInTicks);
            if (row.fadeOutTicks < 0) throw std::runtime_error("invalid fade-out duration");
            // A fade-out covering the whole placement spans exactly the
            // rendered frames. A legacy resize can leave one wider, and
            // clip.resize clamps it to the span: both render the same.
            // Retained split domains are evaluated independently in shaped().
            const auto tickSpan = endTicks - origin;
            const auto frameSpan = clip->end - clip->start;
            clip->fadeOut = row.fadeOutTicks > 0 && row.fadeOutTicks >= tickSpan ? frameSpan : std::min(frameSpan,
                samples(tempo.ticksToSeconds(endTicks) - tempo.ticksToSeconds(endTicks - row.fadeOutTicks), rate));
            clip->inCurve=row.fadeInCurve;clip->outCurve=row.fadeOutCurve;
            if((row.fadeInTension && (!std::isfinite(*row.fadeInTension) || std::abs(*row.fadeInTension)>1)) ||
               (row.fadeOutTension && (!std::isfinite(*row.fadeOutTension) || std::abs(*row.fadeOutTension)>1)))
                throw std::runtime_error("invalid fade tension");
            clip->inTension=row.fadeInTension;clip->outTension=row.fadeOutTension;
            if (stream) {
                const auto tail = map.markers.back().tick;
                const auto end = row.loopEnabled ? tail : std::min(tail,row.endMarkerTicks.value_or(tail));
                const auto loopStart=static_cast<double>(row.loopEnabled ? row.loopStartTicks : 0);
                const auto loopLength=static_cast<double>(row.loopEnabled ? row.loopLenTicks.value_or(0) : 0);
                const auto warpSourceOrigin=static_cast<double>(sourceOrigin);
                if (r3) {
                    clip->r3Warp=std::make_unique<WarpR3Pages>(spec,clip->sourceRate,clip->channels,
                        map.markers,warpClock,warpSourceOrigin,static_cast<double>(end),loopStart,loopLength,clip->end);
                } else {
                    clip->streamingWarp=std::make_unique<WarpClipPlayer>(warpClock,std::move(spec),
                        std::move(map.markers),clip->sourceRate,clip->channels,warpSourceOrigin,
                        static_cast<double>(end),loopStart,loopLength,reverseSynthesis);
                }
            }
            if(!std::isfinite(row.gainDb) || std::abs(row.gainDb)>120) throw std::runtime_error("invalid clip gain");
            clip->gain=static_cast<float>(std::pow(10.0,row.gainDb/20)); clip->gainWide=std::pow(10.0,row.gainDb/20); clip->mode=static_cast<int>(a->channelMode); clip->track=row.trackId;
            if (pitch) {
                media::ClipPitchRenderPlan plan{clip->path, fs::temp_directory_path()/"adi-pitch",
                    std::make_shared<const clip_edit::ClipPitchEdits>(*pitch),
                    static_cast<std::uint64_t>(a->srcStartFrames), static_cast<std::uint64_t>(a->srcLenFrames),
                    false, std::move(pitchWarp), a->warpEnabled ? 0u : clip->sourceRate};
                auto task = media::requestClipPitchRender(plan);
                if (!task) throw std::runtime_error("cannot request pitch preparation");
                clip->pitchComparisonPlan = std::move(plan);
                clip->prepared = std::make_shared<PreparedSource>(std::move(task));
                clip->sourceStart = 0;
                clip->reader.reset();
            }
            auto track=std::find_if(impl_->tracks.begin(),impl_->tracks.end(),[&](const auto& x){return x->track==row.trackId;});
            if(track==impl_->tracks.end()) {
                impl_->tracks.push_back(std::make_unique<TrackSource>(transport,maxFrames,channels,row.trackId,impl_->pitchComparisons));
                track=impl_->tracks.end()-1;
            }
            if (clip->r3Warp && warpPlayback == WarpPlayback::Streaming)
                (*track)->playbackDelay=std::max((*track)->playbackDelay,clip->r3Warp->latencySamples());
            (*track)->clips.push_back(clip.get()); impl_->clips.push_back(std::move(clip));
        } catch(const MissingClipMedia& e) {
            impl_->unavailableClips.insert(row.id);
            impl_->unavailableMedia.insert(row.id);
            impl_->missingMediaProblems.push_back(impl_->problems.size());
            impl_->problems.push_back(label+e.what()+"; silent");
        } catch(const std::exception& e) {
            impl_->unavailableClips.insert(row.id);
            if (openingMedia) impl_->unavailableMedia.insert(row.id);
            impl_->problems.push_back(label+e.what()+"; silent");
        }
    }
    impl_->fatalProblemCount = impl_->problems.size();
    impl_->externalSources.reserve(impl_->clips.size());
    for(auto& clip:impl_->clips) {
        if (clip->prepared && (!clip->pitchComparisonPlan || clip->pitchComparisonPlan->warp))
            impl_->problems.push_back("clips#"+std::to_string(clip->clipId)+
                                      ": tempo changes need a new prepared Warp render");
        impl_->externalSources.push_back({clip->clipId,clip->mediaId,fs::absolute(clip->path),std::move(clip->storedBlake3)});
    }
    for(auto& t:impl_->tracks) {
        t->owner=impl_;
        for(auto* clip:t->clips) clip->playbackDelay=t->playbackDelay;
    }
    if(impl_->clips.empty() || impl_->synchronousReads) return;
    // A single worker handles native-rate playback and small conversion jobs.
    // Spread heavier conversion work without increasing the number of workers
    // with every added clip. Each clip still has exactly one page writer.
    constexpr std::size_t maxWorkers=4, conversionsPerWorker=4;
    const auto needsConversion=[](const auto& clip){return clip->r3Warp || (!clip->streamingWarp && clip->rate!=clip->sourceRate);};
    const auto converted=static_cast<std::size_t>(std::count_if(impl_->clips.begin(),impl_->clips.end(),needsConversion));
    const auto hardware=static_cast<std::size_t>(std::thread::hardware_concurrency());
    const auto workerCount=std::max(std::size_t{1},std::min({hardware,maxWorkers,
        (converted+conversionsPerWorker-1)/conversionsPerWorker}));
    std::vector<std::vector<Clip*>> shards(workerCount);
    std::size_t native=0, resampled=0;
    for(const auto& clip:impl_->clips) {
        auto& next=needsConversion(clip)?resampled:native;
        shards[next++%workerCount].push_back(clip.get());
    }
    impl_->workers.reserve(workerCount);
    for(auto& shard:shards) {
        impl_->workers.emplace_back([p=impl_.get(), clips=std::move(shard), interleave=workerCount>1](std::stop_token stop){
            while(!stop.stop_requested()) {
                bool work=false;
                if(!p->stalled.load(std::memory_order_acquire)) for(auto* clip:clips) {
                    if(stop.stop_requested()) break;
                    // On busy generations, give every clip its first missing
                    // page before filling a clip's remaining lookahead.
                    work=clip->service(p->requestProbe.load(std::memory_order_acquire),interleave)||work;
                }
                if(!work) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
}
ClipPlayback::~ClipPlayback()=default;

namespace {
class PagedLaunchWarpSource final : public LaunchWarpSource {
public:
    PagedLaunchWarpSource(std::shared_ptr<void> bank, Clip& clip, std::int32_t maxFrames)
        : bank_(std::move(bank)), clip_(clip), reader_{clip}, maxFrames_(maxFrames),
          lookAhead_(static_cast<std::int32_t>(std::ceil(16384.0 * clip.rate / 48000.0))) {
        warmed_.fill(-1);
        forecast_.fill(-1);
    }

    void prefetch(WarpClipPlayer::Placement placement) noexcept override {
        Pages pages;
        pages.fill(-1);
        (void)addRange(pages,placement,placement.firstFrame,maxFrames_);
        if (pages == forecast_) return;
        forecast_ = pages;
        for (const auto page : warmed_)
            if (page >= 0) (void)append(pages,page);
        (void)addRange(pages,placement,placement.firstFrame,lookAhead_);
        clip_.request(pages);
    }

    const WarpClipPlayer& player() const noexcept override { return *clip_.streamingWarp; }
    std::uint16_t channels() const noexcept override { return clip_.channels; }
    int channelMode() const noexcept override { return clip_.mode; }
    float gain() const noexcept override { return clip_.gain; }
    std::uint32_t underrunSamples() const noexcept override { return clip_.underruns.load(std::memory_order_relaxed); }
    std::uint32_t readErrors() const noexcept override { return clip_.errors.load(std::memory_order_relaxed); }
    bool allPhasesPrimed() const noexcept override { return allPhasesPrimed_; }

    bool prime(const LaunchSlot& slot, bool warmAllPhases) {
        const auto lastPage = (static_cast<std::int64_t>(clip_.sourceLength) - 1) / clip_.pageFrames;
        if (warmAllPhases && lastPage >= static_cast<std::int64_t>(warmed_.size())) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!player().ready()) {
            if (readErrors() || std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto warm = [&](std::int64_t tick) {
            const auto placement = player().launchAt(static_cast<double>(tick),static_cast<double>(tick));
            return placement && addRange(warmed_,placement,placement.firstFrame,maxFrames_);
        };
        if (!warm(slot.startTick) || (slot.loopEndTick > slot.loopStartTick && !warm(slot.loopStartTick)))
            return false;
        if (warmAllPhases) {
            for (std::int64_t page = 0; page <= lastPage; ++page)
                (void)append(warmed_,page);
        }
        clip_.request(warmed_);
        do {
            if (readErrors()) break;
            bool ready = true;
            for (const auto page : warmed_)
                if (page >= 0 && !clip_.reserve(page)) ready = false;
            if (ready) {
                clip_.releaseAdmission();
                allPhasesPrimed_ = warmAllPhases;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        clip_.releaseAdmission();
        return false;
    }

    void begin(WarpClipPlayer::Placement placement, std::int64_t first, std::int32_t frames) noexcept override {
        Pages current;
        current.fill(-1);
        (void)addRange(current,placement,first,frames);
        auto wanted = current;
        for (const auto page : forecast_)
            if (page >= 0) (void)append(wanted,page);
        for (const auto page : warmed_)
            if (page >= 0) (void)append(wanted,page);
        (void)addRange(wanted,placement,first,std::max(frames,lookAhead_));
        clip_.request(wanted);
        for (const auto page : current)
            if (page >= 0) (void)clip_.reserve(page);
    }

    void renderFrame(WarpClipPlayer::Placement placement, std::int64_t frame,
                     std::span<float> output) noexcept override {
        reader_.missing = false;
        if (!player().renderFrame(placement,frame,reader_.input(),output)) {
            if (!clip_.invalidWarpRate.exchange(true,std::memory_order_relaxed))
                clip_.errors.fetch_add(1,std::memory_order_relaxed);
            std::fill_n(output.data(),output.size(),0.0f);
        } else if (reader_.missing) {
            ++missing_;
            std::fill_n(output.data(),output.size(),0.0f);
        }
    }

    void finish() noexcept override {
        if (reader_.page) clip_.release(reader_.page);
        reader_.page = nullptr;
        reader_.number = -1;
        clip_.releaseAdmission();
        if (missing_) clip_.underruns.fetch_add(missing_,std::memory_order_relaxed);
        missing_ = 0;
    }

private:
    using Pages = std::array<std::int64_t,slots>;

    static bool append(Pages& pages, std::int64_t page) noexcept {
        if (std::find(pages.begin(),pages.end(),page) != pages.end()) return true;
        const auto open = std::find(pages.begin(),pages.end(),-1);
        if (open == pages.end()) return false;
        *open = page;
        return true;
    }

    bool addRange(Pages& pages, WarpClipPlayer::Placement placement, std::int64_t first,
                  std::int32_t frames) const noexcept {
        if (!placement || frames <= 0) return true;
        first = std::clamp(first,placement.firstFrame,placement.endFrame-1);
        const auto last = first+std::min<std::int64_t>(frames-1,placement.endFrame-1-first);
        bool complete = true;
        player().sourceRanges(placement,first,last,[&](audio::WarpSourceRange range) {
            const auto sourceEnd = static_cast<std::int64_t>(clip_.sourceLength);
            if (range.first >= sourceEnd || range.first >= range.last) return;
            const auto lastPage = (std::min(range.last,sourceEnd)-1)/clip_.pageFrames;
            for (auto page=range.first/clip_.pageFrames; page<=lastPage; ++page) {
                if (!append(pages,page)) { complete = false; break; }
            }
        });
        return complete;
    }

    std::shared_ptr<void> bank_;
    Clip& clip_;
    Clip::StreamReader reader_;
    std::int32_t maxFrames_, lookAhead_;
    Pages warmed_, forecast_;
    std::uint32_t missing_ = 0;
    bool allPhasesPrimed_ = false;
};
} // namespace

struct LaunchWarpPageBank : std::enable_shared_from_this<LaunchWarpPageBank> {
    Transport transport;
    ClipPlayback playback;

    LaunchWarpPageBank(std::shared_ptr<const ClipProject> project, std::uint32_t rate,
                       std::int32_t channels, std::int32_t maxFrames)
        : playback(std::move(project),transport,rate,channels,maxFrames,nullptr,false,
                   ClipPlayback::WarpPlayback::NativePages) {}

    std::shared_ptr<LaunchWarpSource> prepare(const LaunchSlot& slot, bool warmAllPhases) {
        const auto& media = playback.impl_->clips;
        const auto found = std::ranges::find_if(media,[&](const auto& clip) { return clip->clipId == slot.clip; });
        if (found == media.end()) return {};
        auto source = std::make_shared<PagedLaunchWarpSource>(shared_from_this(),**found,playback.impl_->maxFrames);
        return source->prime(slot,warmAllPhases) ? std::move(source) : nullptr;
    }
};

// The private readers share the same projection of the unrolled source.
static void appendLaunchSource(ClipProject& project, const ClipProject& original, const rows::Clip& authored,
                               std::int64_t sourceTrack, std::int64_t endTick,
                               std::optional<std::int64_t> endNs = {}) {
    auto& model = project.rows;
    const auto audioRow = std::ranges::find(original.rows.audioClips, authored.id, &rows::AudioClip::clipId);
    if (audioRow == original.rows.audioClips.end()) throw std::runtime_error("missing Session audio row");
    model.audioClips.push_back(*audioRow);
    const auto media = std::ranges::find(original.rows.media, audioRow->mediaId, &rows::Media::id);
    if (media == original.rows.media.end()) throw std::runtime_error("missing Session media row");
    if (std::ranges::find(model.media, media->id, &rows::Media::id) == model.media.end()) model.media.push_back(*media);
    if (const auto hint = original.hints.find(media->id); hint != original.hints.end())
        project.hints.emplace(hint->first, hint->second);

    auto source = authored;
    source.trackId = sourceTrack;
    source.timeBase = 0;
    source.posTicks = 0;
    source.lengthTicks = endTick;
    source.posNs.reset();
    source.lengthNs.reset();
    if (endNs) {
        source.timeBase = 1;
        source.posTicks.reset();
        source.lengthTicks.reset();
        source.posNs = 0;
        source.lengthNs = *endNs;
    }
    source.contentOffsetTicks = 0;
    source.contentOffsetNs.reset();
    source.loopEnabled = false;
    source.fadeInTicks = 0;
    source.fadeOutTicks = 0;
    source.fadeClockNs = {};
    source.fadeClockTicks = {};
    model.clips.push_back(std::move(source));
}

static std::optional<audio::WarpMode> nativeLaunchWarpMode(const rows::AudioClip& clip) noexcept {
    if (!clip.warpEnabled || clip.formantShift != 0) return {};
    const auto mode=audio::parseWarpMode(clip.warpMode);
    if (mode && (*mode == audio::WarpMode::Beats || *mode == audio::WarpMode::Texture ||
                 *mode == audio::WarpMode::RePitch)) return mode;
    return {};
}

static LaunchAudioClip prepareStreamingLaunchClip(const ClipProject& project, const rows::Clip& clip,
    const rows::AudioClip& audioRow, audio::WarpMode mode, const TempoMap& reference,
    const std::shared_ptr<const WarpArrangementClock>& clock, std::int64_t endTick, std::uint32_t rate,
    std::uint16_t channels, std::uint64_t byteBudget) {
    if (audioRow.reverse || clip.aliasOf || audioRow.srcStartFrames < 0 || audioRow.srcLenFrames <= 0 ||
        !audioRow.warpMarkers || audioRow.channelMode < 0 || audioRow.channelMode > 3 ||
        !std::isfinite(clip.gainDb) || std::abs(clip.gainDb) > 120)
        throw std::runtime_error("invalid native Session Warp source");
    const auto media = std::ranges::find(project.rows.media, audioRow.mediaId, &rows::Media::id);
    if (media == project.rows.media.end() || media->embedded)
        throw std::runtime_error("Session Warp source media is missing or embedded");
    fs::path source;
    if (media->relPath) source=project.folder/utf8Path(*media->relPath);
    else if (const auto hint=project.hints.find(media->id); hint!=project.hints.end()) source=hint->second;
    else throw std::runtime_error("Session Warp source has no media path");

    WavSource input(source,true);
    const auto count=static_cast<std::uint64_t>(audioRow.srcLenFrames);
    const auto start=static_cast<std::uint64_t>(audioRow.srcStartFrames);
    const auto nativeChannels=input.wav.channels();
    if (!nativeChannels || nativeChannels > 64 || start > input.wav.frames() || count > input.wav.frames()-start)
        throw std::runtime_error("Session Warp window is outside its source");
    if (count > byteBudget / sizeof(float) / nativeChannels)
        throw std::length_error("Session Warp source exceeds its resident PCM budget");
    auto markers=audio::readWarpMap(*audioRow.warpMarkers,count);
    if (!markers) throw std::runtime_error("Session Warp marker map is not readable or canonical");
    if (mode == audio::WarpMode::RePitch && !supportsLaunchRePitchRates(markers.markers,*clock))
        throw std::runtime_error("Session Re-Pitch Warp rate exceeds the streaming resampler range");
    const auto referenceFrames=samples(reference.ticksToSeconds(endTick),rate);
    if (referenceFrames <= 0) throw std::runtime_error("Session Warp source is shorter than one frame");

    std::vector<float> pcm(static_cast<std::size_t>(count)*nativeChannels);
    for (std::uint64_t first=0; first<count;) {
        const auto frames=static_cast<std::uint32_t>(std::min<std::uint64_t>(4096,count-first));
        input.read(start+first,pcm.data()+static_cast<std::size_t>(first)*nativeChannels,frames);
        first+=frames;
    }
    audio::WarpRenderPlan plan;
    plan.source=std::move(source);
    plan.sourceStart=start;plan.sourceFrames=count;plan.outputRate=rate;
    plan.mode=mode;
    plan.transposeSemis=mode==audio::WarpMode::RePitch?0.0:audioRow.transposeSemis;
    LaunchWarpMedia prepared{clock,std::move(plan),std::move(markers.markers),input.wav.sampleRate(),
        nativeChannels,static_cast<int>(audioRow.channelMode),static_cast<float>(std::pow(10.0,clip.gainDb/20.0)),endTick};
    return LaunchAudioClip::fromWarp(clip.id,channels,static_cast<std::uint64_t>(referenceFrames),
                                     std::move(prepared),std::move(pcm));
}

static LaunchAudioClip prepareLaunchClipAudio(const ClipProject& original, const rows::Clip& authored,
                                       std::int64_t endTick, double bpm, std::uint32_t rate,
                                       std::uint16_t channels, std::uint64_t byteBudget,
                                       std::optional<std::int64_t> endNs = {}) {
    auto project = std::make_shared<ClipProject>();
    project->folder = original.folder;
    project->rows.tempo.push_back({0, bpm, 0, 0.0});
    appendLaunchSource(*project, original, authored, authored.trackId, endTick, endNs);
    TempoMap tempo;
    tempo.events.push_back({0, bpm, 0});
    const auto frames = endNs ? samplesAtNs(*endNs,rate) : samples(tempo.ticksToSeconds(endTick),rate);
    if (channels == 0 || frames <= 0 || static_cast<std::uint64_t>(frames) > byteBudget / sizeof(float) / channels)
        throw std::length_error("Session clip exceeds the prepared audio budget");
    constexpr std::int32_t blockSize = 512;
    Transport transport;
    ClipPlayback playback(project, transport, rate, channels, blockSize, nullptr, true,
                          ClipPlayback::WarpPlayback::Prepared);
    const auto fatal = playback.fatalProblems();
    if (!fatal.empty()) throw std::runtime_error(fatal.front());
    const auto sources = playback.sourcesFor(authored.trackId);
    if (sources.size() != 1 || !playback.waitForWarp()) throw std::runtime_error("Session clip audio is unavailable");
    transport.play();
    std::vector<float> pcm(static_cast<std::size_t>(frames) * channels);
    std::vector<float> buffer(static_cast<std::size_t>(blockSize) * channels);
    std::vector<float*> output(channels);
    for (std::size_t channel = 0; channel < channels; ++channel)
        output[channel] = buffer.data() + channel * blockSize;
    NodeIo io;
    io.channels = channels;
    io.sampleRate = rate;
    io.out = output.data();
    for (std::int64_t first = 0; first < frames; first += io.frames) {
        io.frames = static_cast<std::int32_t>(std::min<std::int64_t>(blockSize, frames - first));
        if (!playback.prime()) throw std::runtime_error("Session clip page could not be read");
        sources.front()->process(io);
        for (std::int32_t frame = 0; frame < io.frames; ++frame)
            for (std::size_t channel = 0; channel < channels; ++channel)
                pcm[static_cast<std::size_t>(first + frame) * channels + channel] = output[channel][frame];
        transport.advance(io.frames);
    }
    if (playback.readErrors() || playback.underrunSamples())
        throw std::runtime_error("Session clip playback preparation could not read all pages");
    const auto& audio = project->rows.audioClips.front();
    const bool directPcm = !audio.warpEnabled && !audio.reverse && audio.transposeSemis == 0 && audio.formantShift == 0 &&
        playback.readsNativePcm(authored.id);
    return LaunchAudioClip::fromSamples(authored.id, rate, channels, std::move(pcm), directPcm);
}

static std::shared_ptr<const LaunchAudioProject> prepareStoredLaunchBank(const ClipProject& original,
    const TempoMap& clock, std::uint32_t rate, std::uint16_t channels, std::int32_t maxFrames, std::uint64_t incarnation,
    std::vector<std::string>& problems, std::uint64_t residentBudget, std::optional<double> preparedBpm,
    std::span<const std::int64_t> warmWarpClips, StoredLaunchPageReuse priorPinnedPages,
    bool allowColdPinnedPages, bool fixedProjection) {
    if (audio::onAudioThread) throw std::logic_error("prepare Session clips off the callback");
    if (!original.session || original.session->scenes().empty()) return {};
    const auto& data = *original.session;
    const auto initialBpm = clock.bpmAt(0);
    const auto bpm = preparedBpm.value_or(initialBpm);
    const auto streamingClock=std::make_shared<const WarpArrangementClock>(clock,rate);
    const bool steadyWarp = streamingClock->constantFramesPerTick() != 0;
    const bool hasPreparedClock = preparedBpm.has_value() || steadyWarp;
    const bool useNativeWarp = !fixedProjection && (!steadyWarp ||
        std::ranges::any_of(data.scenes(), [](const auto& scene) { return scene.tempoEnabled; }));
    TempoMap fixedClock;
    fixedClock.events.push_back({0, bpm, 0});
    TempoMap nativeReferenceClock;
    nativeReferenceClock.events.push_back({0, initialBpm, 0});
    std::vector<std::int64_t> tracks, midiTracks, scenes;
    for (const auto id : data.tracks()) {
        const auto track = std::ranges::find(original.rows.tracks, id, &rows::Track::id);
        if (track == original.rows.tracks.end()) continue;
        const bool midi = track->kind == "midi" || track->kind == "instrument";
        if (track->kind != "audio" && !midi) continue;
        tracks.push_back(id);
        const auto onTrack = [id](const rows::Clip& clip) { return clip.trackId == id && clip.kind == "midi"; };
        if (midi || std::ranges::any_of(original.rows.clips, onTrack) ||
            std::ranges::any_of(original.rows.slotClips, onTrack)) midiTracks.push_back(id);
    }
    std::vector<LaunchSceneClock> sceneClocks;
    for (const auto& scene : data.scenes()) {
        scenes.push_back(scene.id);
        if (scene.tempoEnabled || scene.signatureEnabled) {
            LaunchSceneClock change;
            change.scene = scene.id;
            if (scene.tempoEnabled) change.bpm = scene.tempoBpm;
            if (scene.signatureEnabled)
                change.signature = rows::TimeSignature{0, scene.signatureNumerator, scene.signatureDenominator};
            sceneClocks.push_back(change);
        }
    }
    std::ranges::sort(scenes);

    std::uint64_t remaining = residentBudget;
    std::vector<LaunchSlot> launchSlots;
    std::vector<LaunchAudioClip> pcm;
    auto pagedProject = std::make_shared<ClipProject>();
    pagedProject->folder = original.folder;
    pagedProject->rows.tempo.push_back({0, bpm, 0, 0.0});
    auto warpPageProject = std::make_shared<ClipProject>();
    warpPageProject->folder = original.folder;
    for (const auto& event : clock.events)
        warpPageProject->rows.tempo.push_back({event.posTicks,event.bpm,event.curve,event.tension,event.rampSpan});
    struct NativePageSlot {
        const rows::Clip& clip;
        std::size_t slotIndex;
        std::uint64_t frames;
    };
    std::vector<NativePageSlot> nativePages;
    std::vector<LaunchPagedSource> paged;
    std::vector<std::uint64_t> pagedExtents;
    const auto fixedFrame = [&](std::int64_t tick) { return samples(fixedClock.ticksToSeconds(tick), rate); };
    const auto addPreparedPages = [&](const rows::Clip& clip, const LaunchSlot& slot, std::int64_t end,
                                      bool warmPhysicalWindow = false) {
        const auto pinned = slot.physicalStartFrame >= 0;
        const auto extent = pinned ? slot.physicalStartFrame + slot.physicalFrames : fixedFrame(end);
        if (extent <= 0) throw std::runtime_error("Session clip source is shorter than one frame");
        appendLaunchSource(*pagedProject,original,clip,clip.id,end,pinned ? clip.sourceEndNs : std::nullopt);
        paged.push_back({clip.id,clip.id,pinned ? slot.physicalStartFrame : fixedFrame(slot.startTick),
            pinned ? slot.physicalLoopBeginFrame : fixedFrame(slot.loopStartTick),
            pinned ? slot.physicalLoopEndFrame : fixedFrame(slot.loopEndTick),extent,
            pinned && warmPhysicalWindow, pinned, slot.legato});
        pagedExtents.push_back(static_cast<std::uint64_t>(extent));
    };
    const auto add = [](std::int64_t a, std::int64_t b) {
        if (a < 0 || b < 0 || a > std::numeric_limits<std::int64_t>::max() - b)
            throw std::runtime_error("invalid Session clip extent");
        return a + b;
    };
    const auto usePreparedWarpEnd = [&](const rows::Clip& clip, LaunchSlot& slot) {
        // Native Warp keeps clip-local ticks on the original clock. An
        // absolute-duration fallback needs its end on the prepared clock.
        if (clip.timeBase != 1 || clip.endMarkerTicks) return;
        const auto end = add(slot.startTick,fixedClock.storedNanosecondsToTicks(clip.lengthNs.value_or(0)));
        if (end <= slot.startTick) throw std::runtime_error("invalid Session start or end marker");
        slot.endTick = end;
    };
    for (const auto& cell : data.slots()) {
        if (!std::ranges::binary_search(tracks, cell.key.trackId)) continue;
        LaunchSlot slot;
        slot.track = cell.key.trackId;
        slot.scene = cell.key.sceneId;
        slot.hasStop = cell.hasStop;
        if (!cell.clipId) {
            launchSlots.push_back(slot);
            continue;
        }
        const auto clip = std::ranges::find(original.rows.slotClips, *cell.clipId, &rows::Clip::id);
        if (clip == original.rows.slotClips.end()) continue; // the persisted reader has already validated this
        const auto audioRow = std::ranges::find(original.rows.audioClips, *cell.clipId, &rows::AudioClip::clipId);
        const bool warped = audioRow != original.rows.audioClips.end() && audioRow->warpEnabled;
        const auto nativeMode = warped ? nativeLaunchWarpMode(*audioRow) : std::nullopt;
        // Arrangement streams Re-Pitch. Use the same interpolation at a steady
        // Session tempo so an authored Warp capture replays the performed PCM.
        const auto mode = nativeMode && !fixedProjection && (useNativeWarp || *nativeMode == audio::WarpMode::RePitch)
            ? nativeMode : std::nullopt;
        const auto& slotClock = mode ? nativeReferenceClock : fixedClock;
        const auto setting = data.settings(clip->id).value_or(launch::ClipLaunchSettings{});
        slot.clip = clip->id;
        slot.mode = setting.mode;
        slot.legato = setting.legato;
        slot.fade = setting.clipFade;
        slot.nativePcmClock = clip->kind == "audio" && !warped;
        slot.velocityAmount = static_cast<float>(setting.velocityAmount);
        if (setting.quantization != 0) slot.quantization = launch::resolveClipQuantization(setting.quantization,
            LaunchQuantization::Bar);
        try {
            const bool usePins = clip->kind == "audio" && !warped;
            const bool physicalSource = usePins && clip->timeBase == 1 && clip->contentOffsetNs && clip->sourceEndNs &&
                *clip->contentOffsetNs >= 0;
            const bool physicalLoop = physicalSource && clip->loopEnabled && clip->loopBeginNs && clip->loopEndNs &&
                *clip->loopBeginNs >= 0 && *clip->loopEndNs <= *clip->sourceEndNs &&
                *clip->loopEndNs > std::max(*clip->loopBeginNs,*clip->contentOffsetNs) &&
                samplesAtNs(*clip->loopEndNs,rate) >
                    samplesAtNs(std::max(*clip->loopBeginNs,*clip->contentOffsetNs),rate);
            const bool boundedPhysicalWindow = physicalSource && samplesAtNs(*clip->sourceEndNs,rate) -
                samplesAtNs(physicalLoop ? std::min(*clip->contentOffsetNs,*clip->loopBeginNs) : *clip->contentOffsetNs,rate)
                    <= 3 * ClipPlayback::readAheadFrames(rate);
            const bool supportedPhysicalLaunch = !slot.legato || setting.ramMode || boundedPhysicalWindow ||
                (!clip->loopEnabled && slot.mode != LaunchMode::Repeat);
            const bool physicalPins = physicalSource && supportedPhysicalLaunch && (!clip->loopEnabled || physicalLoop);
            const bool warmPhysicalWindow = boundedPhysicalWindow && (!physicalLoop || slot.legato);
            // A paired tempo scene still observes the outgoing pin's logical
            // region when a different slot takes its relative Legato phase.
            const auto& pinClock = physicalPins ? nativeReferenceClock : fixedClock;
            slot.startTick = usePins && clip->contentOffsetNs
                ? pinClock.storedNanosecondsToTicks(*clip->contentOffsetNs) : clip->contentOffsetTicks;
            const auto length = clip->timeBase == 0 ? clip->lengthTicks.value_or(0)
                : slotClock.storedNanosecondsToTicks(clip->lengthNs.value_or(0));
            slot.endTick = usePins && clip->sourceEndNs
                ? pinClock.storedNanosecondsToTicks(*clip->sourceEndNs)
                : clip->endMarkerTicks.value_or(add(slot.startTick, length));
            if (clip->loopEnabled) {
                slot.loopStartTick = usePins && clip->loopBeginNs
                    ? pinClock.storedNanosecondsToTicks(*clip->loopBeginNs) : clip->loopStartTicks;
                slot.loopEndTick = usePins && clip->loopEndNs
                    ? pinClock.storedNanosecondsToTicks(*clip->loopEndNs)
                    : add(slot.loopStartTick, clip->loopLenTicks.value_or(0));
                if (slot.loopEndTick <= slot.loopStartTick) throw std::runtime_error("invalid Session loop bounds");
            }
            if (slot.startTick < 0 || slot.endTick <= slot.startTick)
                throw std::runtime_error("invalid Session start or end marker");
            launchSlots.push_back(slot);
            if (clip->kind == "audio" && !clip->muted) {
                const auto end = std::max(slot.endTick, slot.loopEndTick);
                if (setting.ramMode && mode) {
                    try {
                        auto audioClip=prepareStreamingLaunchClip(original,*clip,*audioRow,*mode,nativeReferenceClock,
                                                                  streamingClock,end,rate,channels,remaining);
                        remaining-=audioClip.bytes();
                        pcm.push_back(std::move(audioClip));
                        continue;
                    } catch (const std::exception&) {
                        // Keep fixed playback available if this source cannot
                        // fit the streaming backend's PCM or ratio budget.
                        if (!hasPreparedClock) throw;
                    }
                }
                if (!setting.ramMode && mode) {
                    const auto extent = samples(nativeReferenceClock.ticksToSeconds(end),rate);
                    if (extent <= 0) throw std::runtime_error("Session Warp source is shorter than one frame");
                    appendLaunchSource(*warpPageProject,original,*clip,clip->id,end);
                    nativePages.push_back({*clip,launchSlots.size()-1,static_cast<std::uint64_t>(extent)});
                    continue;
                }
                auto& prepared = launchSlots.back();
                if (physicalPins) {
                    const auto startFrame = samplesAtNs(*clip->contentOffsetNs,rate);
                    const auto endFrame = samplesAtNs(*clip->sourceEndNs,rate);
                    if (endFrame <= startFrame) throw std::runtime_error("Session clip source is shorter than one frame");
                    prepared.physicalStartFrame = startFrame;
                    prepared.physicalFrames = endFrame - startFrame;
                    if (physicalLoop) {
                        prepared.physicalLoopBeginFrame = samplesAtNs(*clip->loopBeginNs,rate);
                        prepared.physicalLoopEndFrame = samplesAtNs(*clip->loopEndNs,rate);
                    }
                    if (!setting.ramMode) {
                        if (priorPinnedPages.source && priorPinnedPages.bank) {
                            const auto* prior = priorPinnedPages.bank->clip(clip->id);
                            const auto priorSlots = priorPinnedPages.bank->view().slots;
                            const auto priorSlot = std::ranges::find(priorSlots,clip->id,&LaunchSlot::clip);
                            const auto& priorClips = priorPinnedPages.source->rows.slotClips;
                            const auto priorClip = std::ranges::find(priorClips,clip->id,&rows::Clip::id);
                            if (prior && prior->pageReader() && prior->latencySamples() == 0 &&
                                prior->pageReader()->matchesFormat(rate,channels,maxFrames) &&
                                prior->frames() == static_cast<std::uint64_t>(endFrame) &&
                                priorSlot != priorSlots.end() && priorSlot->physicalStartFrame == startFrame &&
                                (!physicalLoop || !warmPhysicalWindow ||
                                 prior->pageReader()->allPhysicalPhasesPrimed(clip->id)) &&
                                priorSlot->physicalFrames == prepared.physicalFrames &&
                                priorSlot->physicalLoopBeginFrame == prepared.physicalLoopBeginFrame &&
                                priorSlot->physicalLoopEndFrame == prepared.physicalLoopEndFrame &&
                                priorClip != priorClips.end() && *priorClip == *clip &&
                                sameStoredAudioBacking(*priorPinnedPages.source,original,clip->id)) {
                                // Share the warm reader only for this unchanged
                                // slot. Only the selected graph calls read().
                                pcm.push_back(*prior);
                                continue;
                            }
                        }
                        if (!hasPreparedClock && !allowColdPinnedPages && !warmPhysicalWindow)
                            throw std::runtime_error("Session unwarped source needs an identical running page bank");
                    }
                }
                const auto pinned = prepared.physicalStartFrame >= 0;
                if (!hasPreparedClock && !pinned)
                    throw std::runtime_error("Session audio requires a constant prepared tempo");
                if (mode) usePreparedWarpEnd(*clip,prepared);
                const auto preparedEnd = std::max(launchSlots.back().endTick,slot.loopEndTick);
                if (setting.ramMode) {
                    auto clipAudio = prepareLaunchClipAudio(original, *clip, preparedEnd, bpm, rate, channels, remaining,
                                                           pinned ? clip->sourceEndNs : std::nullopt);
                    remaining -= clipAudio.bytes();
                    pcm.push_back(std::move(clipAudio));
                } else {
                    addPreparedPages(*clip,launchSlots.back(),preparedEnd,warmPhysicalWindow);
                }
            }
        } catch (const std::exception& e) {
            problems.push_back("Session clips#" + std::to_string(clip->id) + ": " + e.what());
        }
    }
    if (!nativePages.empty()) {
        auto bank = std::make_shared<LaunchWarpPageBank>(std::move(warpPageProject),rate,channels,maxFrames);
        for (const auto& entry : nativePages) {
            auto& slot = launchSlots[entry.slotIndex];
            const bool warmAllPhases = std::ranges::binary_search(warmWarpClips,entry.clip.id);
            if (auto source = bank->prepare(slot,warmAllPhases)) {
                pcm.push_back(LaunchAudioClip::fromPagedWarp(entry.clip.id,rate,channels,entry.frames,std::move(source)));
            } else if (hasPreparedClock) {
                try {
                    usePreparedWarpEnd(entry.clip,slot);
                    addPreparedPages(entry.clip,slot,std::max(slot.endTick,slot.loopEndTick));
                } catch (const std::exception& e) {
                    problems.push_back("Session clips#" + std::to_string(entry.clip.id) + ": " + e.what());
                }
            } else {
                const auto prefix = "clips#" + std::to_string(entry.clip.id) + ": ";
                const auto& errors = bank->playback.problems();
                const auto detail = std::ranges::find_if(errors,[&](const auto& error) { return error.starts_with(prefix); });
                problems.push_back("Session " + (detail == errors.end() ?
                    prefix + "could not warm native Warp start and loop pages" : *detail));
            }
        }
    }
    if (!paged.empty()) {
        auto reader = std::make_shared<LaunchPagedAudio>(std::move(pagedProject), paged, rate, channels, maxFrames);
        if (reader->prime()) {
            for (std::size_t index = 0; index < paged.size(); ++index) {
                const auto id = paged[index].clip;
                if (reader->hasClip(id)) pcm.push_back(LaunchAudioClip::fromPages(id, rate, channels, pagedExtents[index], reader));
            }
        }
        problems.insert(problems.end(), reader->problems().begin(), reader->problems().end());
    }
    return std::make_shared<const LaunchAudioProject>(incarnation, bpm, std::move(tracks), std::move(launchSlots),
        std::move(scenes), std::move(pcm), LaunchQuantization::Bar,
        std::make_shared<const LaunchFollowPlayer>(data), incarnation, std::move(midiTracks), true, std::move(sceneClocks));
}

std::shared_ptr<const LaunchAudioProject> prepareStoredLaunchProject(const ClipProject& original,
    const TempoMap& clock, std::uint32_t rate, std::uint16_t channels, std::int32_t maxFrames, std::uint64_t incarnation,
    std::vector<std::string>& problems, std::uint64_t residentBudget, std::optional<double> preparedBpm,
    std::span<const std::int64_t> warmWarpClips, StoredLaunchPageReuse priorPinnedPages, bool allowColdPinnedPages) {
    return prepareStoredLaunchBank(original,clock,rate,channels,maxFrames,incarnation,problems,residentBudget,
                                   preparedBpm,warmWarpClips,priorPinnedPages,allowColdPinnedPages,false);
}

std::shared_ptr<const LaunchAudioProject> prepareStoredLaunchHistoryBank(const ClipProject& original,
    const TempoMap& clock, std::uint32_t rate, std::uint16_t channels, std::int32_t maxFrames, std::uint64_t incarnation,
    std::vector<std::string>& problems, std::uint64_t residentBudget) {
    return prepareStoredLaunchBank(original,clock,rate,channels,maxFrames,incarnation,problems,residentBudget,
                                   {},{},{},false,true);
}

bool ClipPlayback::preparePitchComparison(std::int64_t id, audio::PitchCompareSide initial,
                                          std::string& error, std::uint32_t crossfadeFrames,
                                          std::stop_token stop) {
    if(audio::onAudioThread) return false;
    error.clear();
    const auto clip = std::find_if(impl_->clips.begin(),impl_->clips.end(),
                                   [id](const auto& item) { return item->clipId == id; });
    if(clip==impl_->clips.end() || !(*clip)->pitchComparisonPlan) {
        error="no prepared recipe for an edited audio clip";
        return false;
    }
    try {
        impl_->pitchComparisons.collect();
        if(impl_->pitchComparisons.retainedCount()>=2) {
            error="audio driver has not released previous pitch comparisons";
            return false;
        }
        auto plan = *(*clip)->pitchComparisonPlan;
        // The ordinary Warp page reader reverses its prepared result. The
        // comparison uses mirrored output addresses for the same Warp result.
        // At native rate, reverse before resampling the comparison sides.
        if (!plan.warp) plan.reverse = (*clip)->reverse;
        const bool nativeRePitch = (*clip)->streamingWarp != nullptr;
        const auto comparisonRate = nativeRePitch ? (*clip)->sourceRate : (*clip)->rate;
        const auto comparisonFrames = nativeRePitch ? (*clip)->sourceLength
            : static_cast<std::uint64_t>((*clip)->length);
        auto ready=media::prepareClipPitchCompare(plan,stop,comparisonRate);
        if(!ready) {
            error=ready.detail.value_or("cannot prepare pitch comparison");
            return false;
        }
        if(ready.prepared->channels()!=(*clip)->channels || ready.prepared->rate()!=comparisonRate ||
           ready.prepared->frames()<comparisonFrames) {
            error="prepared pitch comparison disagrees with the clip clock";
            return false;
        }
        auto next=std::make_unique<PitchComparison>(clip->get(),std::move(ready.prepared),crossfadeFrames);
        next->playback.select(initial);
        impl_->pitchComparisons.publish(std::move(next));
        return true;
    } catch(const std::exception& failure) {
        error=failure.what();
        return false;
    }
}
bool ClipPlayback::selectPitchComparison(std::int64_t id, audio::PitchCompareSide side) noexcept {
    if(audio::onAudioThread) return false;
    const auto* comparison=impl_->pitchComparisons.peek();
    if(!comparison || !comparison->clip || comparison->clip->clipId!=id) return false;
    comparison->playback.select(side);
    return true;
}
void ClipPlayback::endPitchComparison() {
    if(audio::onAudioThread) return;
    const auto* current=impl_->pitchComparisons.peek();
    if(current && current->clip)
        impl_->pitchComparisons.publish(std::make_unique<PitchComparison>(nullptr,nullptr,0));
}
void ClipPlayback::collectPitchComparisons() {
    if(!audio::onAudioThread) impl_->pitchComparisons.collect();
}

std::vector<Node*> ClipPlayback::sourcesFor(std::int64_t id) {
    for(auto& t:impl_->tracks) if(t->track==id) return {t.get()};
    return {};
}

void ClipPlayback::prefetchSource(Node& node, std::span<const Transport> candidates) noexcept {
    auto* source = dynamic_cast<TrackSource*>(&node);
    if (!source || candidates.empty()) return;
    for (std::size_t index = 0; index < source->clips.size(); ++index) {
        auto& clip = *source->clips.data()[index];
        // Mandatory first-frame windows take priority over read-ahead. The
        // launch, loop head and far Legato cue share the existing page cap.
        std::array<std::int64_t, slots> requested;
        requested.fill(-1);
        auto count = std::size_t{0};
        for (const auto lookahead : {false, true}) {
            for (const auto& candidate : candidates) {
                for (const auto page : clip.wanted(candidate, source->maxFrames, lookahead)) {
                    if (page < 0 || count == requested.size() ||
                        std::find(requested.data(), requested.data() + count, page) != requested.data() + count)
                        continue;
                    requested.data()[count++] = page;
                }
            }
        }
        clip.trimAdmission(requested);
        clip.request(requested);
    }
}
bool ClipPlayback::reserveSource(Node& node, const Transport& transport, std::int32_t frames) noexcept {
    auto* source = dynamic_cast<TrackSource*>(&node);
    if (!source || source->clips.size() != 1 || frames < 1 || frames > source->maxFrames) return false;
    auto& clip = *source->clips.data()[0];
    const auto required = clip.wanted(transport,frames,false);
    clip.trimAdmission(required);
    clip.request(clip.wanted(transport,frames));
    if (required.front() < 0 || clip.errors.load(std::memory_order_relaxed)) {
        clip.releaseAdmission();
        return false;
    }
    for (const auto page : required) {
        if (page >= 0 && !clip.reserve(page)) {
            clip.releaseAdmission();
            return false;
        }
    }
    return true;
}

void ClipPlayback::releaseSource(Node& node) noexcept {
    auto* source = dynamic_cast<TrackSource*>(&node);
    if (source && source->clips.size() == 1) source->clips.data()[0]->releaseAdmission();
}

void ClipPlayback::keepSourceStartWarm(Node& node, std::int64_t firstFrame) noexcept {
    auto* source = dynamic_cast<TrackSource*>(&node);
    if (!source) return;
    for (auto* clip : source->clips) {
        clip->warmStartFrame = firstFrame;
        clip->warmStartFrames = source->maxFrames;
    }
}
std::int32_t ClipPlayback::warpLatencyFor(std::int64_t id) const noexcept {
    std::int32_t delay = 0;
    for (const auto& clip : impl_->clips)
        if (clip->track == id && clip->r3Warp) delay = std::max(delay,clip->r3Warp->latencySamples());
    return delay;
}
std::vector<ClipPlayback::SampleRange> ClipPlayback::arrangementRanges(std::int64_t id) const {
    std::vector<SampleRange> ranges;
    for (const auto& clip : impl_->clips)
        if (clip->track == id) ranges.emplace_back(clip->start, clip->end);
    return ranges;
}
void ClipPlayback::useMonitoredInput(std::int64_t id) noexcept {
    for (auto& clip : impl_->clips)
        if (clip->track == id) clip->monitorInputOnly = true;
}
void ClipPlayback::attachLaunchAudio(std::int64_t id, std::shared_ptr<const LaunchAudioRuntime> runtime,
                                     bool createIfMissing, const ClipPlayback* previous) {
    const TrackSource* before = nullptr;
    if (previous && &previous->impl_->transport == &impl_->transport && previous->impl_->sampleRate == impl_->sampleRate) {
        for (const auto& track : previous->impl_->tracks) {
            if (track->track == id) { before = track.get(); break; }
        }
    }
    for (auto& track : impl_->tracks) {
        if (track->track == id) {
            track->attachLaunch(std::move(runtime), before);
            return;
        }
    }
    if (!createIfMissing) return;
    auto track = std::make_unique<TrackSource>(impl_->transport, impl_->maxFrames, impl_->channels, id,
                                               impl_->pitchComparisons);
    track->owner = impl_;
    track->attachLaunch(std::move(runtime), before);
    impl_->tracks.push_back(std::move(track));
}
void ClipPlayback::muteWhileRecording(std::int64_t id, const std::span<const std::int64_t>& recordingTracks) noexcept {
    for (auto& track : impl_->tracks) {
        if (track->track == id) {
            track->recordingTracks = &recordingTracks;
            break;
        }
    }
}
const std::vector<std::string>& ClipPlayback::problems() const noexcept { return impl_->problems; }
std::span<const std::string> ClipPlayback::fatalProblems() const noexcept {
    return std::span<const std::string>(impl_->problems).first(impl_->fatalProblemCount);
}
bool ClipPlayback::clipUnavailable(std::int64_t clipId) const noexcept {
    return impl_->unavailableClips.contains(clipId) || impl_->terminalProblems.contains(clipId);
}
bool ClipPlayback::mediaUnavailable(std::int64_t clipId) const noexcept {
    const auto problem = impl_->terminalProblems.find(clipId);
    return impl_->unavailableMedia.contains(clipId) ||
        (problem != impl_->terminalProblems.end() && problem->second.mediaOffline);
}
void ClipPlayback::appendPlaybackProblems(std::vector<std::string>& out) const {
    const auto blocking = fatalProblems();
    out.insert(out.end(), blocking.begin(), blocking.end());
    for (const auto& [clipId, problem] : impl_->terminalProblems)
        out.push_back("clips#" + std::to_string(clipId) + ": " + problem.detail + "; silent");
}
void ClipPlayback::appendExportBlockingProblems(std::vector<std::string>& out) const {
    const auto& missing=impl_->missingMediaProblems;
    for(std::size_t i=0;i<impl_->fatalProblemCount;++i)
        if(!std::binary_search(missing.begin(),missing.end(),i)) out.push_back(impl_->problems[i]);
}
std::span<const ClipMediaSource> ClipPlayback::externalSources() const noexcept {
    if(audio::onAudioThread)return {};
    return impl_->externalSources;
}
bool ClipPlayback::readsNativePcm(std::int64_t clipId) const noexcept {
    if (audio::onAudioThread) return false;
    const auto found = std::ranges::find_if(impl_->clips, [&](const auto& clip) { return clip->clipId == clipId; });
    if (found == impl_->clips.end()) return false;
    const auto& clip = **found;
    return !clip.prepared && !clip.streamingWarp && !clip.r3Warp && !clip.progressive && clip.sourceRate == clip.rate && clip.reader && !clip.reader->file.pinned;
}
void ClipPlayback::stallForTest(bool v) noexcept { impl_->stalled.store(v,std::memory_order_release); }
void ClipPlayback::setRequestSnapshotProbeForTest(const RequestSnapshotProbeForTest* probe) noexcept {
    impl_->requestProbe.store(probe,std::memory_order_release);
}
std::uint32_t ClipPlayback::underrunSamples() const noexcept { std::uint32_t n=0;for(const auto& c:impl_->clips)n+=c->underruns.load();return n; }
std::uint32_t ClipPlayback::readErrors() const noexcept { std::uint32_t n=0;for(const auto& c:impl_->clips)n+=c->errors.load();return n; }
std::uint64_t ClipPlayback::pageBytesCeiling() const noexcept {
    std::uint64_t bytes = 0;
    for (const auto& clip : impl_->clips)
        bytes += slots * static_cast<std::uint64_t>(clip->pageFrames) * clip->channels * sizeof(float);
    return bytes;
}

namespace {
std::optional<DecodeProblem> decodeFailure(const Clip& clip) {
    if (clip.progressive) {
        const auto status = clip.progressive->snapshot().status;
        if (status == audio::ProgressiveStatus::Failed || status == audio::ProgressiveStatus::Cancelled) {
            auto detail = clip.progressive->error();
            if (detail.empty()) detail = "cannot decode audio source";
            return DecodeProblem{std::move(detail),true};
        }
    }
    if (clip.prepared) {
        const auto state = clip.prepared->state();
        if (state.ready && !state.file)
            return DecodeProblem{state.detail.empty() ? clip.prepared->failure() : std::string(state.detail),false};
    }
    if (clip.invalidWarpRate.load(std::memory_order_relaxed))
        return DecodeProblem{"Re-Pitch Warp rate exceeds the streaming resampler range",false};
    if (!clip.errors.load(std::memory_order_relaxed)) return std::nullopt;
    if (clip.r3Warp)
        return DecodeProblem{"cannot render a real-time Warp output page at this rate or source position",false};
    if (clip.streamingWarp)
        return DecodeProblem{"cannot prepare or read streaming Warp source",false};
    if (clip.prepared)
        return DecodeProblem{clip.pitchComparisonPlan ? "cannot read prepared clip source"
                                                      : "cannot read prepared Warp source",false};
    return DecodeProblem{"cannot read audio source",true};
}
}
bool ClipPlayback::pollDecodeProblems() {
    if (audio::onAudioThread || impl_->clips.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now < impl_->nextDecodePoll) return false;
    impl_->nextDecodePoll = now + std::chrono::milliseconds(250);
    bool changed = false;
    for (const auto& clip : impl_->clips) {
        if (impl_->terminalProblems.contains(clip->clipId)) continue;
        if (auto problem = decodeFailure(*clip)) {
            impl_->terminalProblems.emplace(clip->clipId, std::move(*problem));
            changed = true;
        }
    }
    return changed;
}
std::vector<std::string> ClipPlayback::decodeProblems() const {
    if(audio::onAudioThread)return {};
    std::vector<std::string> result;
    for(const auto& clip:impl_->clips) {
        if (const auto problem = decodeFailure(*clip))
            result.push_back("clips#" + std::to_string(clip->clipId) + ": " + problem->detail);
    }
    return result;
}
bool ClipPlayback::prime(std::chrono::milliseconds timeout,std::stop_token stop) {
    if(audio::onAudioThread)return false;
    return primeTargets(impl_->transport,nullptr,timeout,true,stop);
}
bool ClipPlayback::waitForWarp(std::stop_token stop) const {
    if(audio::onAudioThread)return false;
    while(!stop.stop_requested()) {
        bool ready=true;
        for(const auto& clip:impl_->clips) {
            if (!clip->prepared && !clip->streamingWarp && !clip->r3Warp) continue;
            if(clip->errors.load(std::memory_order_relaxed))return false;
            if (clip->prepared) {
                const auto state=clip->prepared->state();
                if(state.ready && !state.file)return false;
                if(!state.ready)ready=false;
            }
            if (clip->streamingWarp && !clip->streamingWarp->ready()) {
                if (impl_->synchronousReads) (void)clip->service(nullptr,false);
                if (clip->errors.load(std::memory_order_relaxed)) return false;
                ready=ready && clip->streamingWarp->ready();
            }
        }
        if(ready)return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
ClipPlayback::Readiness ClipPlayback::readiness(ReadinessScope scope) const {
    if(audio::onAudioThread)return {};
    const bool delayedOnly = scope == ReadinessScope::Delayed;
    if (delayedOnly && (impl_->synchronousReads || std::none_of(impl_->clips.begin(),impl_->clips.end(),
        [](const auto& clip) { return clip->playbackDelay > 0 && !clip->monitorInputOnly; }))) return {};
    return {[held=impl_,delayedOnly](const Transport& snapshot,std::int32_t frames,bool allowFailedSources) noexcept {
        if(frames<1 || frames>held->maxFrames)return false;
        // One producer, the audio driver. This fixed target is shared by the
        // admission check and the source immediately rendered after it.
        const Transport target=snapshot;
        // No checked vector iterators on the callback (MSVC Debug can lock
        // their shared proxy). Source generations have a fixed clip count.
        const auto included = [delayedOnly](const Clip& clip) { return !delayedOnly || clip.playbackDelay > 0; };
        const auto releaseAll=[&]()noexcept{
            for(std::size_t i=0;i<held->clips.size();++i)
                if(included(*held->clips[i])) held->clips[i]->releaseAdmission();
        };
        releaseAll();
        for(std::size_t i=0;i<held->clips.size();++i) {
            auto& clip=held->clips[i];
            if(included(*clip))clip->request(clip->wanted(target,frames));
        }
        for(std::size_t i=0;i<held->clips.size();++i) {
            auto& clip=held->clips[i];
            if(clip->monitorInputOnly || !included(*clip))continue;
            if(clip->errors.load(std::memory_order_relaxed)) {
                if(allowFailedSources)continue;
                releaseAll();
                return false;
            }
            if (clip->sourcePreparationPending()) {
                if (allowFailedSources) continue;
                if (clip->streamingWarp) { releaseAll(); return false; }
            }
            for(const auto number:clip->wanted(target,frames,false)) {
                if(number<0)continue;
                if(!clip->reserve(number)){releaseAll();return false;}
            }
        }
        return true;
    }};
}
bool ClipPlayback::primeAt(const Transport& snapshot,std::chrono::milliseconds timeout,bool waitForPendingWarp) {
    return primeTargets(snapshot,nullptr,timeout,waitForPendingWarp);
}
bool ClipPlayback::primeAt(const Transport& snapshot,const Transport& alternate,std::chrono::milliseconds timeout,
                           bool waitForPendingWarp) {
    return primeTargets(snapshot,&alternate,timeout,waitForPendingWarp);
}
bool ClipPlayback::primeTrackAt(std::int64_t track, const Transport& first, const Transport& loop,
                                std::chrono::milliseconds timeout, std::optional<std::int64_t> retainFromFrame) {
    if (audio::onAudioThread || timeout.count() < 0) return false;
    if (retainFromFrame)
        for (auto& clip : impl_->clips)
            if (clip->track == track) clip->retainUnrolledPages(*retainFromFrame);
    return primeTargets(first, &loop, timeout, true, {}, track);
}
bool ClipPlayback::primeTrackWindow(std::int64_t track, const Transport& first, std::int32_t frames,
                                    std::chrono::milliseconds timeout) {
    // A private unwarped source uses output-rate pages. This range can cover
    // at most seven of the existing eight pages, even with an unaligned start.
    if (audio::onAudioThread || timeout.count() < 0 || frames <= 0) return false;
    for (const auto& clip : impl_->clips)
        if (clip->track == track && frames > 3 * readAheadFrames(clip->rate)) return false;
    for (auto& clip : impl_->clips)
        if (clip->track == track) clip->retainUnrolledPages(first.sampleAt(0));
    return primeTargets(first, nullptr, timeout, true, {}, track, frames);
}
std::optional<ClipPlayback::SampleRange> ClipPlayback::retainedTrackRange(std::int64_t track) const noexcept {
    for (const auto& clip : impl_->clips) {
        if (clip->track == track && clip->retainedPages)
            return SampleRange{clip->retainedPages->front() * clip->pageFrames, std::min(clip->end, clip->length)};
    }
    return {};
}
bool ClipPlayback::primeTargets(const Transport& snapshot,const Transport* alternate,std::chrono::milliseconds timeout,
                                bool waitForPendingWarp,std::stop_token stop,std::optional<std::int64_t> track,
                                std::int32_t windowFrames) {
    if(audio::onAudioThread || timeout.count()<0)return false;
    // One stable request for the entire wait. In particular, a driver advancing
    // the live Transport cannot move a candidate's readiness target mid-wait.
    const Transport target=snapshot;
    const Transport other=alternate?*alternate:Transport{};
    const bool synchronous=impl_->synchronousReads;
    const auto selected = [track](const Clip& clip) { return !track || clip.track == *track; };
    auto wanted=[&](const Clip& clip,std::array<std::int64_t,slots>& pages) {
        if (clip.retainedPages) {
            pages = *clip.retainedPages;
            return true;
        }
        pages=windowFrames > 0 ? clip.wanted(target,windowFrames,false)
                               : clip.wanted(target,impl_->maxFrames,!synchronous);
        if(alternate)for(const auto number:clip.wanted(other,impl_->maxFrames,!synchronous)) {
            if(number<0 || std::find(pages.begin(),pages.end(),number)!=pages.end())continue;
            const auto empty=std::find(pages.begin(),pages.end(),-1);
            if(empty==pages.end())return false;
            *empty=number;
        }
        return true;
    };
    const auto started=std::chrono::steady_clock::now();
    std::array<std::int64_t,slots> pages;
    // Refuse the complete preparation before posting any partial union.
    if(alternate)for(const auto& clip:impl_->clips)if(selected(*clip) && !wanted(*clip,pages))return false;
    const auto releaseAll=[&]()noexcept{for(auto& clip:impl_->clips)if(selected(*clip))clip->releaseAdmission();};
    if(!synchronous)releaseAll();
    for(auto& clip:impl_->clips) {
        if(!selected(*clip))continue;
        (void)wanted(*clip,pages);
        if(synchronous)clip->trimAdmission(pages);
        clip->request(pages);
        // A queued export has no page workers and advances block by block.
        // Release pages for clips its caller has passed.
        if(synchronous && pages.front()<0 && clip->allocatedPages)
            (void)clip->reclaimPages(clip->requestSeq.load(std::memory_order_seq_cst));
    }
    do {
        if(stop.stop_requested()){releaseAll();return false;}
        bool ready=true;
        for(auto& clip:impl_->clips) {
            if(!selected(*clip) || clip->monitorInputOnly)continue;
            if(clip->errors.load(std::memory_order_relaxed)) {
                if(!waitForPendingWarp && (clip->prepared || clip->streamingWarp))continue;
                releaseAll();
                return false;
            }
            if(!waitForPendingWarp && clip->sourcePreparationPending())continue;
            if (clip->streamingWarp && !clip->streamingWarp->ready()) {
                if (synchronous && !impl_->stalled.load(std::memory_order_acquire))
                    (void)clip->service(impl_->requestProbe.load(std::memory_order_acquire),false);
                ready=false;
                continue;
            }
            if (!wanted(*clip,pages)) { releaseAll(); return false; }
            if (clip->streamingWarp) clip->request(pages);
            bool serviced=false;
            for(const auto number:pages) {
                if(number<0 || clip->reserve(number))continue;
                if(synchronous && !serviced && !impl_->stalled.load(std::memory_order_acquire)) {
                    if(stop.stop_requested()){releaseAll();return false;}
                    (void)clip->service(impl_->requestProbe.load(std::memory_order_acquire),false);
                    if(clip->errors.load(std::memory_order_relaxed)){releaseAll();return false;}
                    serviced=true;
                }
                if(!synchronous || !clip->reserve(number))ready=false;
            }
        }
        if(ready)return true;
        // Compare elapsed milliseconds rather than adding an arbitrary timeout
        // to a nanosecond time_point, which can overflow for a large duration.
        if(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now()-started)>=timeout){releaseAll();return false;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(true);
}
}
