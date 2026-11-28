// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/media/peaks.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace adi::ui {
inline constexpr std::size_t kWaveformOverviewBins = 4096;
inline constexpr std::size_t kWaveformPeakGroup = 4;

inline void mergePeakBins(media::PeakBin &into, const media::PeakBin &bin) noexcept {
    into.minimum = std::min(into.minimum, bin.minimum);
    into.maximum = std::max(into.maximum, bin.maximum);
}

// Every coarser bin summarizes up to four bins from the previous level. The
// finest level is the worker's source analysis or the best cached APKS fallback.
class WaveformPeaks {
  public:
    explicit WaveformPeaks(media::MediaPeaks finest, bool buildCoarserLevels = true) {
        levels_.push_back(std::move(finest));
        if (!buildCoarserLevels)
            return;
        while (levels_.back().channels > 0 && levels_.back().bins.size() > levels_.back().channels) {
            const auto &previous = levels_.back();
            if (previous.framesPerBin > std::numeric_limits<std::uint64_t>::max() / kWaveformPeakGroup)
                break;
            const auto channels = previous.channels;
            const auto bins = previous.bins.size() / channels;
            const auto count = (bins + kWaveformPeakGroup - 1) / kWaveformPeakGroup;
            media::MediaPeaks next{previous.contentHash, previous.sampleRate, channels, previous.frames,
                                  previous.framesPerBin * kWaveformPeakGroup,
                                  std::vector<media::PeakBin>(count * channels)};
            for (std::size_t i = 0; i < bins; ++i)
                for (std::uint32_t channel = 0; channel < channels; ++channel) {
                    const auto &from = previous.bins[i * channels + channel];
                    auto &into = next.bins[(i / kWaveformPeakGroup) * channels + channel];
                    if (i % kWaveformPeakGroup == 0)
                        into = from;
                    else
                        mergePeakBins(into, from);
                }
            levels_.push_back(std::move(next));
        }
    }

    std::span<const media::MediaPeaks> levels() const noexcept { return levels_; }
    const media::MediaPeaks &finest() const noexcept { return levels_.front(); }

    std::size_t detailValues() const noexcept {
        std::size_t values = 0;
        for (const auto &level : levels_) {
            if (level.bins.size() <= kWaveformOverviewBins * level.channels)
                break;
            values += level.bins.size();
        }
        return values;
    }

    // The overview remains available even when the document has used its
    // budget for finer waveforms. Called by the worker before publication.
    std::size_t limitDetail(std::size_t budget) {
        auto remaining = detailValues();
        auto keep = levels_.begin();
        while (remaining > budget) {
            remaining -= keep->bins.size();
            ++keep;
        }
        levels_.erase(levels_.begin(), keep);
        return remaining;
    }

  private:
    std::vector<media::MediaPeaks> levels_;
};

struct PunchedPreviewRange {
    std::int64_t startTicks, endTicks;
    double startSeconds;
    std::uint64_t sourceStartFrame, sourceEndFrame;
};

inline std::optional<PunchedPreviewRange> punchedPreviewRange(
    const engine::AudioRecordingPreview::PunchedPlacement &punch, std::uint64_t frames,
    std::uint32_t sampleRate, const engine::TempoMap &tempo,
    std::optional<std::int64_t> trackOffset = {}) noexcept {
    if (sampleRate == 0)
        return std::nullopt;
    const auto offset = trackOffset.value_or(punch.offsetSamples);
    const auto range = engine::recordingPlacementRange(punch.rawStartSample, frames, offset, punch.window);
    if (!range)
        return std::nullopt;
    const auto rate = static_cast<double>(sampleRate);
    const double firstSeconds = static_cast<double>(range->timelineStartSample) / rate;
    auto first = tempo.secondsToTicks(firstSeconds);
    auto last = tempo.secondsToTicks(static_cast<double>(range->timelineEndSample) / rate);
    // Several ticks can project to a single device sample. At a cut, draw the
    // authored brace tick that landing uses instead of the inverse clock's tick.
    if (punch.window.in)
        first = range->timelineStartSample == punch.window.in->sample ? punch.window.in->tick
                                                                     : std::max(first, punch.window.in->tick);
    if (punch.window.out)
        last = range->timelineEndSample == punch.window.out->sample ? punch.window.out->tick
                                                                   : std::min(last, punch.window.out->tick);
    if (last <= first)
        return std::nullopt;
    return PunchedPreviewRange{first, last, firstSeconds, range->sourceStartFrame, range->sourceStartFrame + range->frames};
}

// An audio clip's waveform (Live 12 manual 6.1, chapter 9; parity-7.3 row 35; N40, #404).
//
// The peaks come from `media::buildMediaPeaks` (APKS v1), on a worker of their own, never on the
// message thread: a file's first request queues it and draws nothing; the worker builds its peaks
// (from the pool's cache where one is stored, else by decoding the file) and the next frame draws
// them. Results are kept by the content hash the snapshot's pool row carries, so two clips of one
// file, or one file in two places, share them.
//
// What is kept follows the document: each new generation drops the files no clip of it uses any
// more, and a result that arrives for a file the document has let go is dropped as it lands. A file
// still in use keeps its peaks across generations, since its content hash names the audio itself;
// nothing on main writes the pool's peaks cache yet (open #449), so dropping them on every edit
// would decode each file again after every edit.
class WaveformCache {
  public:
    using Build = std::function<media::PeakResult(std::int64_t mediaId, std::stop_token)>;
    // A missing media ID selects an embedded sample state. Both builders run on the worker only.
    using BuildSource = std::function<media::PeakResult(std::optional<std::int64_t> mediaId,
                                                       const std::string &stateHash, std::stop_token)>;
    using TransientBuild = std::function<std::vector<std::uint64_t>(std::int64_t mediaId,
                                                                   const std::string &contentHash, std::stop_token)>;
    explicit WaveformCache(Build build)
        : WaveformCache([build = std::move(build)](std::optional<std::int64_t> mediaId, const std::string &,
                                                   std::stop_token stop) {
              return mediaId ? build(*mediaId, stop) : media::PeakResult{media::PeakProblem::invalidSource, {}, {}};
          }) {}
    explicit WaveformCache(BuildSource build, TransientBuild transientBuild = {})
        : build_(std::move(build)), transientBuild_(std::move(transientBuild)) {}
    WaveformCache(const WaveformCache &) = delete;
    WaveformCache &operator=(const WaveformCache &) = delete;
    ~WaveformCache() {
        if (worker_.joinable()) {
            worker_.request_stop();
            wake_.notify_all();
            worker_.join();
        }
    }
    // The peaks of a clip's file, from memory; else none, and the file queued once. A clip whose pool
    // row is missing, or marks the file missing, asks for nothing.
    std::shared_ptr<const WaveformPeaks> peaks(const engine::AudioClipSource &audio) {
        if (!audio.mediaInfo || audio.mediaInfo->missing || audio.mediaInfo->contentHash.empty())
            return nullptr;
        return peaks(audio.mediaId, audio.mediaInfo->contentHash);
    }
    // A verified relink preserves valid peaks; a failed build may try the new path.
    void retry(const std::string &hash) {
        std::lock_guard lock(mutex_);
        if (kept_.contains(hash))
            return;
        asked_.erase(hash);
        std::erase_if(queue_, [&hash](const Request &request) { return request.hash == hash; });
    }
    std::shared_ptr<const media::MediaPeaks> peaks(const engine::SampleSlotNode &slot) {
        if (slot.empty || slot.missing || sampleKey(slot).empty())
            return nullptr;
        const auto waveform = peaks(slot.filePresent ? slot.mediaId : std::nullopt, sampleKey(slot));
        return waveform ? std::shared_ptr<const media::MediaPeaks>(waveform, &waveform->finest()) : nullptr;
    }
    // Carry the finalized recording preview into its newly landed clips while the worker reads
    // the file's own peaks. A recording can share media with an earlier clip that already has them.
    void seedRecording(const engine::Snapshot &snapshot, std::span<const std::int64_t> clips,
                       const engine::AudioRecordingPreview &preview) {
        const auto &source = preview.peaks;
        if (clips.empty() || source.channels == 0 || source.frames == 0 || source.framesPerBin == 0 ||
            source.sampleRate == 0 || source.bins.size() > media::kMaxPeakValues)
            return;
        const auto binCount = source.bins.size() / source.channels;
        if (source.bins.size() % source.channels != 0 ||
            binCount != source.frames / source.framesPerBin + (source.frames % source.framesPerBin != 0))
            return;
        const std::set<std::int64_t> landed(clips.begin(), clips.end());
        for (const auto &mapping : preview.tracks) {
            const auto *track = snapshot.findTrack(mapping.trackId);
            if (!track || mapping.channels.empty() || mapping.channels.size() > source.channels ||
                std::any_of(mapping.channels.begin(), mapping.channels.end(),
                            [&](std::uint32_t channel) { return channel >= source.channels; }))
                continue;
            const auto channels = static_cast<std::uint32_t>(mapping.channels.size());
            for (const auto &clip : track->clips) {
                if (!landed.contains(clip->id) || !clip->audio || !clip->audio->mediaInfo)
                    continue;
                const auto &audio = *clip->audio;
                const auto &identity = *audio.mediaInfo;
                if (identity.missing || identity.contentHash.empty() ||
                    (identity.sampleRate && *identity.sampleRate != source.sampleRate) ||
                    (identity.channels && *identity.channels != channels) ||
                    (identity.frames && static_cast<std::uint64_t>(*identity.frames) != source.frames))
                    continue;
                media::MediaPeaks peaks{identity.contentHash, source.sampleRate, channels, source.frames,
                                      source.framesPerBin, {}};
                peaks.bins.reserve(binCount * channels);
                for (std::size_t bin = 0; bin < binCount; ++bin)
                    for (const auto channel : mapping.channels)
                        peaks.bins.push_back(source.bins[bin * source.channels + channel]);
                std::lock_guard lock(mutex_);
                if (kept_.contains(identity.contentHash) || provisional_.contains(identity.contentHash))
                    continue;
                requestLocked(audio.mediaId, identity.contentHash);
                // The recording's own preview is ready now. Build the hierarchy on the worker.
                provisional_.emplace(identity.contentHash, std::make_shared<WaveformPeaks>(std::move(peaks), false));
                delivered_.fetch_add(1, std::memory_order_acq_rel);
            }
        }
    }
    // Only the Sample Editor requests onsets; Arrangement overview builds
    // still need just peaks.
    std::shared_ptr<const std::vector<std::uint64_t>> transients(const engine::AudioClipSource &audio) {
        if (!transientBuild_ || !audio.mediaInfo || audio.mediaInfo->missing || audio.mediaInfo->contentHash.empty())
            return {};
        const auto &hash = audio.mediaInfo->contentHash;
        std::lock_guard lock(mutex_);
        if (const auto found = transients_.find(hash); found != transients_.end())
            return found->second;
        wanted_.insert(hash);
        if (transientAsked_.insert(hash).second) {
            queue_.push_back({audio.mediaId, hash, true});
            start();
            wake_.notify_one();
        }
        return {};
    }
    // A new document generation drops requests and results for sources neither a clip nor a sample slot uses.
    void follow(const engine::Snapshot &snapshot, std::uint64_t generation) {
        if (generation == generation_)
            return;
        generation_ = generation;
        std::set<std::string> used;
        for (const auto &t : snapshot.tracks)
            for (const auto *clips : {&t->clips, &t->takeClips})
                for (const auto &c : *clips)
                    if (c->audio && c->audio->mediaInfo && !c->audio->mediaInfo->contentHash.empty())
                        used.insert(c->audio->mediaInfo->contentHash);
        for (const auto &slot : snapshot.sampleSlots)
            if (slot && !slot->empty && !slot->missing && !sampleKey(*slot).empty())
                used.insert(sampleKey(*slot));
        std::lock_guard lock(mutex_);
        std::erase_if(kept_, [&](const auto &entry) {
            if (used.contains(entry.first))
                return false;
            detailValues_ -= entry.second->detailValues();
            return true;
        });
        std::erase_if(provisional_, [&used](const auto &entry) { return !used.contains(entry.first); });
        std::erase_if(asked_, [&used](const std::string &hash) { return !used.contains(hash); });
        std::erase_if(transients_, [&used](const auto &entry) { return !used.contains(entry.first); });
        std::erase_if(transientAsked_, [&used](const std::string &hash) { return !used.contains(hash); });
        std::erase_if(queue_, [&used](const Request &r) { return !used.contains(r.hash); });
        wanted_ = std::move(used);
    }
    // Counts the results that have landed: a frame that sees it move repaints.
    std::uint64_t delivered() const noexcept { return delivered_.load(std::memory_order_acquire); }
    // How many builds the worker has run, and whether it has nothing left to do (for the tests).
    int builds() const noexcept { return builds_.load(std::memory_order_acquire); }
    bool idle() const {
        std::lock_guard lock(mutex_);
        return queue_.empty() && !busy_;
    }
    bool holds(const std::string &hash) const {
        std::lock_guard lock(mutex_);
        return kept_.contains(hash);
    }
    // Why the last build failed, if it did (for the Status Bar and the tests).
    std::string problem() const {
        std::lock_guard lock(mutex_);
        return problem_;
    }

  private:
    static constexpr std::size_t fileDetailValues = 2 * 1024 * 1024 / sizeof(media::PeakBin);
    static constexpr std::size_t documentDetailValues = 32 * 1024 * 1024 / sizeof(media::PeakBin);
    static const std::string &sampleKey(const engine::SampleSlotNode &slot) noexcept {
        return slot.filePresent && slot.mediaId ? slot.mediaHash : slot.stateHash;
    }
    std::shared_ptr<const WaveformPeaks> peaks(std::optional<std::int64_t> mediaId, const std::string &hash) {
        std::lock_guard lock(mutex_);
        if (const auto found = kept_.find(hash); found != kept_.end())
            return found->second;
        if (const auto found = provisional_.find(hash); found != provisional_.end())
            return found->second;
        requestLocked(mediaId, hash);
        return nullptr;
    }
    struct Request {
        std::optional<std::int64_t> mediaId;
        std::string hash;
        bool transient = false;
    };
    void requestLocked(std::optional<std::int64_t> mediaId, const std::string &hash) {
        wanted_.insert(hash);
        if (asked_.insert(hash).second) {
            queue_.push_back({mediaId, hash});
            start();
            wake_.notify_one();
        }
    }
    void start() {
        if (!worker_.joinable())
            worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Request next;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop.stop_requested() || !queue_.empty(); });
                if (stop.stop_requested())
                    return;
                next = std::move(queue_.front());
                queue_.pop_front();
                busy_ = true;
            }
            if (next.transient) {
                std::shared_ptr<const std::vector<std::uint64_t>> built;
                try {
                    if (next.mediaId)
                        built = std::make_shared<const std::vector<std::uint64_t>>(
                            transientBuild_(*next.mediaId, next.hash, stop));
                } catch (const std::bad_alloc &) {}
                {
                    std::lock_guard lock(mutex_);
                    busy_ = false;
                    if (built && !stop.stop_requested() && wanted_.contains(next.hash))
                        transients_.emplace(std::move(next.hash), std::move(built));
                    delivered_.fetch_add(1, std::memory_order_acq_rel);
                }
                continue;
            }
            static const std::string noState;
            auto result = build_(next.mediaId, next.mediaId ? noState : next.hash, stop);
            std::shared_ptr<WaveformPeaks> built;
            if (result && !stop.stop_requested()) {
                try {
                    built = std::make_shared<WaveformPeaks>(std::move(*result.data));
                } catch (const std::bad_alloc &) {
                    result.problem = media::PeakProblem::resource;
                }
            }
            builds_.fetch_add(1, std::memory_order_acq_rel);
            {
                std::lock_guard lock(mutex_);
                busy_ = false;
                if (!built) {
                    problem_ = result.detail.empty() ? "the file's peaks could not be built" : result.detail;
                    continue; // asked_ keeps it: a file that fails is not tried again this session
                }
                // Kept under the hash the worker read; dropped when the document let the file go.
                if (wanted_.contains(next.hash) && !kept_.contains(next.hash)) {
                    const auto budget = std::min(fileDetailValues, documentDetailValues - detailValues_);
                    const auto values = built->limitDetail(budget);
                    provisional_.erase(next.hash);
                    kept_.emplace(std::move(next.hash), std::move(built));
                    detailValues_ += values;
                }
                delivered_.fetch_add(1, std::memory_order_acq_rel);
            }
        }
    }
    BuildSource build_;
    TransientBuild transientBuild_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Request> queue_;
    std::map<std::string, std::shared_ptr<const WaveformPeaks>> kept_;
    std::map<std::string, std::shared_ptr<const WaveformPeaks>> provisional_;
    std::map<std::string, std::shared_ptr<const std::vector<std::uint64_t>>> transients_;
    std::set<std::string> asked_, transientAsked_, wanted_;
    std::string problem_;
    bool busy_ = false;
    std::size_t detailValues_ = 0;
    std::uint64_t generation_ = 0;
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<int> builds_{0};
    std::jthread worker_; // last: stopped and joined before the rest goes
};
// The file-relative boundary on a canonical warp map, in the clip's direction.
inline double warpedWaveformFrame(std::span<const WarpMarker> markers, double tick, bool reverse) noexcept {
    if (markers.empty())
        return 0.;
    const auto end = static_cast<double>(markers.back().tick);
    if (reverse)
        tick = end - std::clamp(tick, 0., end);
    const auto right = std::upper_bound(markers.begin(), markers.end(), tick,
                                       [](double at, const WarpMarker &m) { return at < static_cast<double>(m.tick); });
    if (right == markers.begin())
        return 0.;
    const auto &a = *(right - 1);
    if (right == markers.end())
        return static_cast<double>(a.sourceFrame);
    return static_cast<double>(a.sourceFrame) + (tick - static_cast<double>(a.tick)) *
               static_cast<double>(right->sourceFrame - a.sourceFrame) / static_cast<double>(right->tick - a.tick);
}
// Convert Arrangement ticks into file frames using the clip's source clock.
// Loop bounds repeat in elapsed render time, as in ClipPlayback, including
// when later repeats play across a change of project tempo.
class ClipSourceMap {
  public:
    struct FrameSpan {
        double from = 0., to = 0.;
    };
    struct Column {
        // A pixel at a wrap has one range on either side of the loop boundary.
        FrameSpan first{}, second{};
        bool repeats = false;
    };
    struct LoopPosition {
        double seconds;
        std::optional<double> warpTicks;
    };
    struct RepeatedRange {
        // An empty start keeps the original selection start on the first pass.
        std::optional<LoopPosition> from;
        LoopPosition to;
    };

    // A caller already holding the clip's parsed Warp map can borrow it for a frame.
    ClipSourceMap(const engine::ClipNode &clip, const engine::TempoMap *tempo, double sampleRate,
                  std::optional<std::span<const WarpMarker>> parsed = std::nullopt)
        : pos_(clip.posTicks), offsetTicks_(clip.contentOffsetTicks),
          rate_(clip.audio->warpEnabled ? sampleRate : audio::pitchedSampleRate(sampleRate, clip.audio->transposeSemis)), tempo_(tempo),
          audio_(*clip.audio), parsedMarkers_(parsed.value_or(std::span<const WarpMarker>{})) {
        if (!parsed && audio_.warpEnabled && audio_.warpMarkers)
            if (auto read = audio::readWarpMap(*audio_.warpMarkers, static_cast<std::uint64_t>(audio_.sourceLengthFrames)))
                ownedMarkers_.emplace(std::move(read.markers));
        if (pos_ < 0 || !(rate_ > 0))
            return;
        startSeconds_ = timelineSeconds(pos_);
        if (markers().empty()) {
            if (clip.contentOffsetNs)
                offsetSeconds_ = static_cast<double>(*clip.contentOffsetNs) * 1e-9;
            else
                offsetSeconds_ = secondsFrom(pos_, offsetTicks_);
        }
        if (clip.loopEnabled) {
            setLoop(clip);
            if (!audio_.warpEnabled && loopLength_ == 0. && std::isfinite(offsetSeconds_) &&
                offsetSeconds_ * rate_ >= static_cast<double>(audio_.sourceLengthFrames))
                offsetSeconds_ = 0.; // ClipPlayback's fallback for an invalid legacy loop.
        }
    }

    std::optional<double> frameAt(std::int64_t tick) const {
        auto frame = boundaryAt(tick);
        if (!frame)
            return std::nullopt;
        const auto start = static_cast<double>(audio_.sourceStartFrames);
        if (audio_.reverse)
            return std::max(start, *frame - 1.);
        return *frame < start + static_cast<double>(audio_.sourceLengthFrames) ? frame : std::nullopt;
    }

    // The Sample Editor's source ruler is musical for Warp and physical otherwise.
    std::optional<LoopPosition> repeatedPositionAt(std::int64_t tick) const {
        if (const auto seconds = repeatedClockAt(tick))
            return loopPosition(*seconds);
        return {};
    }

    // The continuous source-ruler interval covering the audible selection [first, last).
    std::optional<RepeatedRange> repeatedRangeAcross(std::int64_t first, std::int64_t last) const {
        const auto lastRepeat = repeatAt(last);
        if (lastRepeat < 1.)
            return {};
        const auto firstRepeat = repeatAt(first);
        if (firstRepeat == lastRepeat)
            return RepeatedRange{repeatedPositionAt(first), loopPosition(clockInRepeat(last, lastRepeat))};

        const auto loopEnd = loopPosition(loopStart_ + loopLength_);
        const auto lastClock = clockAt(last);
        const auto wrapClock = loopStart_ + lastRepeat * loopLength_;
        const auto error = std::numeric_limits<double>::epsilon() * std::max(lastClock, wrapClock) * 8.;
        if (lastRepeat == firstRepeat + 1. && std::abs(lastClock - wrapClock) <= error)
            return RepeatedRange{repeatedPositionAt(first), loopEnd};

        // A wrap includes the tail and head of the loop. Keep any lead-in before it too.
        RepeatedRange range{std::nullopt, loopEnd};
        if (clockAt(first) >= loopStart_)
            range.from = loopPosition(loopStart_);
        return range;
    }

    Column framesAcross(std::int64_t first, std::int64_t last) const {
        if (last <= pos_ || last < first)
            return {};
        first = std::max(first, pos_);
        const auto begin = boundaryAt(first);
        if (!begin)
            return {};
        const auto end = boundaryAt(last);
        const auto wraps = repeatAt(last) - repeatAt(first);
        const auto span = [](double a, double b) { return FrameSpan{std::min(a, b), std::max(a, b)}; };
        if (wraps >= 1.) {
            const double afterWrap = wraps > 1. ? loopLastFrame_ : end.value_or(loopFirstFrame_);
            return {span(*begin, loopLastFrame_), span(loopFirstFrame_, afterWrap), true};
        }
        const auto sourceStart = static_cast<double>(audio_.sourceStartFrames);
        const auto sourceEnd = sourceStart + static_cast<double>(audio_.sourceLengthFrames);
        const auto limit = loopLength_ > 0 ? loopLastFrame_ : audio_.reverse ? sourceStart : sourceEnd;
        const auto endFrame = audio_.reverse ? std::max(limit, std::min(*begin - 1., end.value_or(limit)))
                                            : std::min(limit, std::max(*begin + 1., end.value_or(limit)));
        return {span(*begin, endFrame), {}, false};
    }

  private:
    std::span<const WarpMarker> markers() const noexcept {
        return ownedMarkers_ ? std::span<const WarpMarker>(*ownedMarkers_) : parsedMarkers_;
    }
    static constexpr double fallbackSecondsPerTick = .5 / textproj::kPPQ; // 120 BPM
    double earlySecondsPerTick() const {
        return tempo_ != nullptr ? 60. / tempo_->bpmAt(0) / textproj::kPPQ : fallbackSecondsPerTick;
    }
    double timelineSeconds(std::int64_t tick) const {
        // Playback extrapolates a Warp source origin before song zero at the first tempo.
        return tempo_ != nullptr && tick >= 0 ? tempo_->ticksToSeconds(tick)
                                             : static_cast<double>(tick) * earlySecondsPerTick();
    }
    double secondsFrom(std::int64_t origin, std::int64_t ticks) const {
        if (ticks < 0 || ticks > std::numeric_limits<std::int64_t>::max() - std::max(std::int64_t{0}, origin))
            return std::numeric_limits<double>::infinity();
        return timelineSeconds(origin + ticks) - timelineSeconds(origin);
    }
    double clockAt(std::int64_t tick) const { return offsetSeconds_ + (timelineSeconds(tick) - startSeconds_); }
    std::optional<double> repeatedClockAt(std::int64_t tick) const {
        const auto repeat = repeatAt(tick);
        if (repeat < 1.)
            return {};
        return clockInRepeat(tick, repeat);
    }
    double clockInRepeat(std::int64_t tick, double repeat) const {
        return std::clamp(clockAt(tick) - repeat * loopLength_, loopStart_, loopStart_ + loopLength_);
    }

    LoopPosition loopPosition(double seconds) const {
        LoopPosition position{seconds, std::nullopt};
        if (!markers().empty())
            position.warpTicks = sourceTickAt(seconds);
        return position;
    }
    double sourceTickAt(double seconds) const {
        const auto absolute = warpOriginSeconds_ + seconds;
        if (tempo_ == nullptr || absolute < 0)
            return absolute / earlySecondsPerTick() - static_cast<double>(warpOrigin_);
        const auto tick = tempo_->secondsToTicks(absolute);
        const auto at = tempo_->ticksToSeconds(tick);
        const auto ticksFromOrigin = static_cast<double>(tick) - static_cast<double>(warpOrigin_);
        if (tick == std::numeric_limits<std::int64_t>::max())
            return ticksFromOrigin;
        const auto interval = tempo_->ticksToSeconds(tick + 1) - at;
        return interval > 0 ? ticksFromOrigin + (absolute - at) / interval : ticksFromOrigin;
    }
    std::optional<double> boundaryAt(std::int64_t tick) const {
        if (pos_ < 0 || tick < pos_ || !(rate_ > 0))
            return std::nullopt;
        const auto into = tick - pos_;
        const auto sourceLength = static_cast<double>(audio_.sourceLengthFrames);
        const auto wrapped = repeatedClockAt(tick);
        const auto map = markers();
        double frame;
        if (!map.empty()) {
            if (!wrapped && (offsetTicks_ < 0 || offsetTicks_ >= map.back().tick - into))
                return std::nullopt;
            const auto ticks = wrapped ? sourceTickAt(*wrapped) : static_cast<double>(offsetTicks_ + into);
            frame = warpedWaveformFrame(map, ticks, audio_.reverse);
        } else {
            frame = (wrapped ? *wrapped : clockAt(tick)) * rate_;
            if (!(frame >= 0. && frame < sourceLength))
                return std::nullopt;
            if (audio_.reverse)
                frame = sourceLength - frame;
        }
        if (!(frame >= 0. && frame <= sourceLength))
            return std::nullopt;
        return static_cast<double>(audio_.sourceStartFrames) + frame;
    }
    double repeatAt(std::int64_t tick) const {
        if (loopLength_ <= 0 || tick < pos_)
            return 0.;
        const double cycles = (clockAt(tick) - loopStart_) / loopLength_;
        if (!std::isfinite(cycles) || cycles <= 0.)
            return 0.;
        // Keep exact musical boundaries on the next pass through roundoff in the tempo conversion.
        const double error = std::numeric_limits<double>::epsilon() * cycles * 8.;
        return std::floor(cycles + error);
    }
    void setLoop(const engine::ClipNode &clip) {
        const auto start = clip.loopStartTicks, length = clip.loopLengthTicks.value_or(0);
        if (start < 0 || length < 0 || length > std::numeric_limits<std::int64_t>::max() - start)
            return;
        const auto end = start + length;
        const auto map = markers();
        double beginSeconds, endSeconds, beginFrame, endFrame;
        if (map.empty()) {
            if (audio_.warpEnabled)
                return;
            beginSeconds = clip.loopBeginNs ? static_cast<double>(*clip.loopBeginNs) * 1e-9 : secondsFrom(pos_, start);
            endSeconds = clip.loopEndNs ? static_cast<double>(*clip.loopEndNs) * 1e-9 : secondsFrom(pos_, end);
            beginFrame = beginSeconds * rate_;
            endFrame = endSeconds * rate_;
            if (audio_.reverse) {
                const auto sourceLength = static_cast<double>(audio_.sourceLengthFrames);
                beginFrame = sourceLength - beginFrame;
                endFrame = sourceLength - endFrame;
            }
        } else {
            if (offsetTicks_ < 0 || end > map.back().tick)
                return;
            warpOrigin_ = pos_ - offsetTicks_;
            warpOriginSeconds_ = timelineSeconds(warpOrigin_);
            offsetSeconds_ = startSeconds_ - warpOriginSeconds_;
            beginSeconds = secondsFrom(warpOrigin_, start);
            endSeconds = secondsFrom(warpOrigin_, end);
            beginFrame = warpedWaveformFrame(map, static_cast<double>(start), audio_.reverse);
            endFrame = warpedWaveformFrame(map, static_cast<double>(end), audio_.reverse);
        }
        if (!(beginSeconds >= 0. && endSeconds > beginSeconds && beginFrame != endFrame &&
              std::min(beginFrame, endFrame) >= 0. && std::max(beginFrame, endFrame) <= static_cast<double>(audio_.sourceLengthFrames)))
            return;
        loopStart_ = beginSeconds;
        loopLength_ = endSeconds - beginSeconds;
        loopFirstFrame_ = static_cast<double>(audio_.sourceStartFrames) + beginFrame;
        loopLastFrame_ = static_cast<double>(audio_.sourceStartFrames) + endFrame;
    }

    std::int64_t pos_, offsetTicks_, warpOrigin_ = 0;
    double rate_, startSeconds_ = 0., offsetSeconds_ = 0., warpOriginSeconds_ = 0.;
    double loopStart_ = 0., loopLength_ = 0., loopFirstFrame_ = 0., loopLastFrame_ = 0.;
    const engine::TempoMap *tempo_;
    const engine::AudioClipSource &audio_;
    std::span<const WarpMarker> parsedMarkers_;
    std::optional<std::vector<WarpMarker>> ownedMarkers_;
};
// The lowest and highest sample of `channel` over the file's frames [from, to), from its peaks;
// none where the span holds no bin: an empty or backwards span, one that is not a number, one that
// ends at or before the file's first frame, one that starts past its last bin.
//
// The answer is always the extremes of real bins, never a seed. A float file holds samples past
// full scale, so a bin can lie wholly above 1 or below -1; and a span shorter than one frame still
// lies in a bin (three faults a review of 18 November found: the seed was {1, -1}, which clipped
// such a bin's near edge to full scale and came back as it stood, minimum above maximum, for a
// span inside one frame; and a span before the file answered with the first bin).
inline std::optional<std::pair<std::size_t, std::size_t>> peakSpanBins(const media::MediaPeaks &p, double from,
                                                                    double to, std::uint32_t channel) {
    if (p.channels == 0 || p.framesPerBin == 0 || channel >= p.channels || !(to > from) || !(to > 0.))
        return std::nullopt;
    const auto binCount = p.bins.size() / p.channels;
    const auto perBin = static_cast<double>(p.framesPerBin);
    // Compared as numbers before any cast: a position past the bins (or past what a size_t holds)
    // is out of the file, and casting it first would be undefined.
    if (!(std::max(0., from) / perBin < static_cast<double>(binCount)))
        return std::nullopt;
    const auto first = static_cast<std::size_t>(std::max(0., from) / perBin);
    const auto lastAt = std::max(0., to - 1.) / perBin;
    auto last = lastAt < static_cast<double>(binCount) ? static_cast<std::size_t>(lastAt) : binCount - 1;
    last = std::max(last, first); // a span inside one frame: `to - 1` falls in the bin before
    return std::pair{first, last + 1};
}

inline std::optional<media::PeakBin> peakSpan(const media::MediaPeaks &p, double from, double to, std::uint32_t channel) {
    const auto range = peakSpanBins(p, from, to, channel);
    if (!range)
        return std::nullopt;
    const auto [first, end] = *range;
    auto out = p.bins[first * p.channels + channel];
    for (auto b = first + 1; b < end; ++b)
        mergePeakBins(out, p.bins[b * p.channels + channel]);
    return out;
}

inline std::optional<media::PeakBin> peakSpan(const WaveformPeaks &p, double from, double to, std::uint32_t channel) {
    const auto range = peakSpanBins(p.finest(), from, to, channel);
    if (!range)
        return std::nullopt;
    auto [first, end] = *range;
    std::optional<media::PeakBin> out;
    const auto levels = p.levels();
    for (const auto &level : levels) {
        const auto add = [&](std::size_t bin) {
            const auto &peak = level.bins[bin * level.channels + channel];
            if (out)
                mergePeakBins(*out, peak);
            else
                out = peak;
        };
        // Scan the edges at their own resolution, then use the next level for
        // complete groups. Wide spans cost O(log bins) without using a bin
        // that extends outside the pixel's range in the finest level.
        if (&level == &levels.back()) {
            while (first < end)
                add(first++);
            break;
        }
        while (first < end && first % kWaveformPeakGroup != 0)
            add(first++);
        while (first < end && end % kWaveformPeakGroup != 0)
            add(--end);
        if (first == end)
            break;
        first /= kWaveformPeakGroup;
        end /= kWaveformPeakGroup;
    }
    return out;
}
} // namespace adi::ui
