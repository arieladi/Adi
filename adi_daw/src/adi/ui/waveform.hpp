// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/warp_map.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/media/peaks.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
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
    // Builds one media row's peaks; run on the worker only.
    using Build = std::function<media::PeakResult(std::int64_t mediaId, std::stop_token)>;
    explicit WaveformCache(Build build) : build_(std::move(build)) {}
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
        const auto &hash = audio.mediaInfo->contentHash;
        std::lock_guard lock(mutex_);
        if (const auto found = kept_.find(hash); found != kept_.end())
            return found->second;
        wanted_.insert(hash);
        if (asked_.insert(hash).second) {
            queue_.push_back({audio.mediaId, hash});
            start();
            wake_.notify_one();
        }
        return nullptr;
    }
    // A new generation of the document: the files none of its clips use are dropped, and any
    // request or result for them with it.
    void follow(const engine::Snapshot &snapshot, std::uint64_t generation) {
        if (generation == generation_)
            return;
        generation_ = generation;
        std::set<std::string> used;
        for (const auto &t : snapshot.tracks)
            for (const auto &c : t->clips)
                if (c->audio && c->audio->mediaInfo && !c->audio->mediaInfo->contentHash.empty())
                    used.insert(c->audio->mediaInfo->contentHash);
        std::lock_guard lock(mutex_);
        std::erase_if(kept_, [&used](const auto &entry) { return !used.contains(entry.first); });
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
    struct Request {
        std::int64_t mediaId = 0;
        std::string hash;
    };
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
            auto result = build_(next.mediaId, stop);
            builds_.fetch_add(1, std::memory_order_acq_rel);
            {
                std::lock_guard lock(mutex_);
                busy_ = false;
                if (!result) {
                    problem_ = result.detail.empty() ? "the file's peaks could not be built" : result.detail;
                    continue; // asked_ keeps it: a file that fails is not tried again this session
                }
                // Kept under the hash the worker read; dropped when the document let the file go.
                if (wanted_.contains(next.hash))
                    kept_[next.hash] = std::make_shared<const media::MediaPeaks>(std::move(*result.data));
            }
            delivered_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    Build build_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Request> queue_;
    std::map<std::string, std::shared_ptr<const media::MediaPeaks>> kept_;
    std::set<std::string> asked_, wanted_;
    std::string problem_;
    bool busy_ = false;
    std::uint64_t generation_ = 0;
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<int> builds_{0};
    std::jthread worker_; // last: stopped and joined before the rest goes
};
// Where a clip's timeline tick falls in its file: the source frame, or none outside its source
// window (before the clip, or past `sourceLengthFrames`). Unwarped, the tempo map's seconds from the
// clip's start at the file's rate; warped, the clip's warp markers (AWRP v1: a source-window frame
// and the source's musical tick, from the clip's start), between them straight, and none past the
// last. A map that does not read is taken as unwarped.
class ClipSourceMap {
  public:
    ClipSourceMap(const engine::ClipNode &clip, const engine::TempoMap *tempo, double sampleRate)
        : pos_(clip.posTicks), rate_(sampleRate), tempo_(tempo), audio_(*clip.audio) {
        if (audio_.warpEnabled && audio_.warpMarkers)
            if (auto read = audio::readWarpMap(*audio_.warpMarkers, static_cast<std::uint64_t>(audio_.sourceLengthFrames)))
                markers_ = std::move(read.markers);
        if (tempo_ != nullptr)
            startSeconds_ = tempo_->ticksToSeconds(pos_);
    }
    std::optional<double> frameAt(std::int64_t tick) const {
        const auto into = tick - pos_;
        if (into < 0)
            return std::nullopt;
        double frame = 0;
        if (markers_.size() >= 2) {
            const auto after = std::upper_bound(markers_.begin(), markers_.end(), into,
                                                [](std::int64_t t, const WarpMarker &m) { return t < m.tick; });
            if (after == markers_.begin() || after == markers_.end())
                return std::nullopt;
            const auto &b = *after, &a = *(after - 1);
            frame = static_cast<double>(a.sourceFrame) + static_cast<double>(into - a.tick) *
                                                             static_cast<double>(b.sourceFrame - a.sourceFrame) /
                                                             static_cast<double>(std::max<std::int64_t>(1, b.tick - a.tick));
        } else {
            const double seconds = tempo_ != nullptr ? tempo_->ticksToSeconds(tick) - startSeconds_
                                                     : static_cast<double>(into) / textproj::kPPQ / 2.; // 120 BPM
            frame = seconds * rate_;
        }
        if (frame < 0 || frame >= static_cast<double>(audio_.sourceLengthFrames))
            return std::nullopt;
        return static_cast<double>(audio_.sourceStartFrames) + frame;
    }

  private:
    std::int64_t pos_;
    double rate_, startSeconds_ = 0;
    const engine::TempoMap *tempo_;
    const engine::AudioClipSource &audio_;
    std::vector<WarpMarker> markers_;
};
// The lowest and highest sample of `channel` over the file's frames [from, to), from its peaks;
// none where the span holds no bin.
inline std::optional<media::PeakBin> peakSpan(const media::MediaPeaks &p, double from, double to, std::uint32_t channel) {
    if (p.channels == 0 || p.framesPerBin == 0 || channel >= p.channels || to <= from)
        return std::nullopt;
    const auto binCount = p.bins.size() / p.channels;
    const auto first = static_cast<std::size_t>(std::max(0., from) / static_cast<double>(p.framesPerBin));
    auto last = static_cast<std::size_t>(std::max(0., to - 1.) / static_cast<double>(p.framesPerBin));
    if (first >= binCount)
        return std::nullopt;
    last = std::min(last, binCount - 1);
    media::PeakBin out{1.f, -1.f};
    for (auto b = first; b <= last; ++b) {
        const auto &bin = p.bins[b * p.channels + channel];
        out.minimum = std::min(out.minimum, bin.minimum);
        out.maximum = std::max(out.maximum, bin.maximum);
    }
    return out;
}
} // namespace adi::ui
