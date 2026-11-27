// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/warp_map.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/media/peaks.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>
namespace adi::ui {
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
    explicit WaveformCache(Build build)
        : WaveformCache([build = std::move(build)](std::optional<std::int64_t> mediaId, const std::string &,
                                                   std::stop_token stop) {
              return mediaId ? build(*mediaId, stop) : media::PeakResult{media::PeakProblem::invalidSource, {}, {}};
          }) {}
    explicit WaveformCache(BuildSource build) : build_(std::move(build)) {}
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
    std::shared_ptr<const media::MediaPeaks> peaks(const engine::AudioClipSource &audio) {
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
        return peaks(slot.filePresent ? slot.mediaId : std::nullopt, sampleKey(slot));
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
                auto peaks = std::make_shared<media::MediaPeaks>();
                peaks->contentHash = identity.contentHash;
                peaks->sampleRate = source.sampleRate;
                peaks->channels = channels;
                peaks->frames = source.frames;
                peaks->framesPerBin = source.framesPerBin;
                peaks->bins.reserve(binCount * channels);
                for (std::size_t bin = 0; bin < binCount; ++bin)
                    for (const auto channel : mapping.channels)
                        peaks->bins.push_back(source.bins[bin * source.channels + channel]);
                std::lock_guard lock(mutex_);
                if (kept_.contains(identity.contentHash) || provisional_.contains(identity.contentHash))
                    continue;
                requestLocked(audio.mediaId, identity.contentHash);
                provisional_.emplace(identity.contentHash, std::move(peaks));
                delivered_.fetch_add(1, std::memory_order_acq_rel);
            }
        }
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
        std::erase_if(kept_, [&used](const auto &entry) { return !used.contains(entry.first); });
        std::erase_if(provisional_, [&used](const auto &entry) { return !used.contains(entry.first); });
        std::erase_if(asked_, [&used](const std::string &hash) { return !used.contains(hash); });
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
    static const std::string &sampleKey(const engine::SampleSlotNode &slot) noexcept {
        return slot.filePresent && slot.mediaId ? slot.mediaHash : slot.stateHash;
    }
    std::shared_ptr<const media::MediaPeaks> peaks(std::optional<std::int64_t> mediaId, const std::string &hash) {
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
            static const std::string noState;
            auto result = build_(next.mediaId, next.mediaId ? noState : next.hash, stop);
            builds_.fetch_add(1, std::memory_order_acq_rel);
            {
                std::lock_guard lock(mutex_);
                busy_ = false;
                if (!result) {
                    problem_ = result.detail.empty() ? "the file's peaks could not be built" : result.detail;
                    continue; // asked_ keeps it: a file that fails is not tried again this session
                }
                // Kept under the hash the worker read; dropped when the document let the file go.
                if (wanted_.contains(next.hash)) {
                    kept_[next.hash] = std::make_shared<const media::MediaPeaks>(std::move(*result.data));
                    provisional_.erase(next.hash);
                }
            }
            delivered_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    BuildSource build_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Request> queue_;
    std::map<std::string, std::shared_ptr<const media::MediaPeaks>> kept_;
    std::map<std::string, std::shared_ptr<const media::MediaPeaks>> provisional_;
    std::set<std::string> asked_, wanted_;
    std::string problem_;
    bool busy_ = false;
    std::uint64_t generation_ = 0;
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<int> builds_{0};
    std::jthread worker_; // last: stopped and joined before the rest goes
};
// Where a clip's timeline tick falls in its file: the source frame, or none outside its source
// window. Unwarped, seconds from the clip's placement are added to its source offset and converted
// at the file's rate. Warped, ticks from the placement are added to the source offset, then mapped
// between warp markers (AWRP v1: a frame and a musical tick, both relative to the source window).
// A map that does not read is taken as unwarped.
class ClipSourceMap {
  public:
    ClipSourceMap(const engine::ClipNode &clip, const engine::TempoMap *tempo, double sampleRate)
        : pos_(clip.posTicks), offsetTicks_(clip.contentOffsetTicks), rate_(sampleRate), tempo_(tempo),
          audio_(*clip.audio) {
        if (audio_.warpEnabled && audio_.warpMarkers)
            if (auto read = audio::readWarpMap(*audio_.warpMarkers, static_cast<std::uint64_t>(audio_.sourceLengthFrames)))
                markers_ = std::move(read.markers);
        if (tempo_ != nullptr)
            startSeconds_ = tempo_->ticksToSeconds(pos_);
        if (markers_.empty()) {
            if (clip.contentOffsetNs)
                offsetSeconds_ = static_cast<double>(*clip.contentOffsetNs) * 1e-9;
            else if (pos_ < 0 || offsetTicks_ < 0 || offsetTicks_ > std::numeric_limits<std::int64_t>::max() - pos_)
                offsetSeconds_ = std::numeric_limits<double>::infinity();
            else
                offsetSeconds_ = tempo_ != nullptr ? tempo_->ticksToSeconds(pos_ + offsetTicks_) - startSeconds_
                                                   : static_cast<double>(offsetTicks_) * fallbackSecondsPerTick;
        }
    }
    std::optional<double> frameAt(std::int64_t tick) const {
        if (pos_ < 0 || tick < pos_)
            return std::nullopt;
        const auto into = tick - pos_;
        double frame = 0;
        if (markers_.size() >= 2) {
            if (offsetTicks_ < 0 || offsetTicks_ >= markers_.back().tick - into)
                return std::nullopt;
            const auto sourceTick = into + offsetTicks_;
            const auto after = std::upper_bound(markers_.begin(), markers_.end(), sourceTick,
                                                [](std::int64_t t, const WarpMarker &m) { return t < m.tick; });
            const auto &b = *after, &a = *(after - 1);
            frame = static_cast<double>(a.sourceFrame) + static_cast<double>(sourceTick - a.tick) *
                                                             static_cast<double>(b.sourceFrame - a.sourceFrame) /
                                                             static_cast<double>(b.tick - a.tick);
        } else {
            const double seconds = tempo_ != nullptr ? tempo_->ticksToSeconds(tick) - startSeconds_
                                                     : static_cast<double>(into) * fallbackSecondsPerTick;
            frame = (offsetSeconds_ + seconds) * rate_;
        }
        if (frame < 0 || frame >= static_cast<double>(audio_.sourceLengthFrames))
            return std::nullopt;
        return static_cast<double>(audio_.sourceStartFrames) + frame;
    }

  private:
    static constexpr double fallbackSecondsPerTick = .5 / textproj::kPPQ; // 120 BPM
    std::int64_t pos_, offsetTicks_;
    double rate_, startSeconds_ = 0, offsetSeconds_ = 0;
    const engine::TempoMap *tempo_;
    const engine::AudioClipSource &audio_;
    std::vector<WarpMarker> markers_;
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
inline std::optional<media::PeakBin> peakSpan(const media::MediaPeaks &p, double from, double to, std::uint32_t channel) {
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
    auto out = p.bins[first * p.channels + channel];
    for (auto b = first + 1; b <= last; ++b) {
        const auto &bin = p.bins[b * p.channels + channel];
        out.minimum = std::min(out.minimum, bin.minimum);
        out.maximum = std::max(out.maximum, bin.maximum);
    }
    return out;
}
} // namespace adi::ui
