// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/transients.hpp"
#include "adi/media/publish_file.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace adi::engine {

namespace detail { class WarpOnsetCache; }

// Owner-held lease for a source window's analysis. The renderer may check
// readiness through this stable object without copying its shared pointer.
class WarpOnsetJob {
public:
    [[nodiscard]] const audio::TransientResult* result() const noexcept {
        return ready_.load(std::memory_order_acquire) ? &result_ : nullptr;
    }

private:
    friend class detail::WarpOnsetCache;
    static_assert(std::atomic<bool>::is_always_lock_free);
    std::atomic<bool> ready_{false};
    audio::TransientResult result_;
};

namespace detail {

// Source-domain analysis is reusable across project tempo edits and marker
// changes. One worker owns analysis and publishes each result once.
class WarpOnsetCache {
public:
    explicit WarpOnsetCache(audio::DecodeCache& decode)
        : decode_(decode), worker_([this](std::stop_token stop) { work(stop); }) {}

    std::shared_ptr<const WarpOnsetJob> request(const std::filesystem::path& source,
                                               std::uint64_t first, std::uint64_t length) {
        // Playback accepts symlinked media. Resolve it before taking the native
        // file revision, which includes identity, size, mtime and change time.
        auto path = std::filesystem::canonical(source);
        const auto revision = fileRevision(path);
        Key key{std::move(path), first, length};

        const std::lock_guard lock(mutex_);
        if (++requests_ == 64) {
            requests_ = 0;
            std::erase_if(recent_, [](const auto& entry) { return entry.second.job.expired(); });
            std::erase_if(queue_, [](const auto& entry) { return entry.job.expired(); });
        }
        auto known = recent_.find(key);
        if (known != recent_.end() && known->second.revision == revision) {
            if (auto job = known->second.job.lock()) {
                const auto* result = job->result();
                if (!result) return job;
                if (*result) {
                    retain(job);
                    return job;
                }
            }
        }
        auto job = std::make_shared<WarpOnsetJob>();
        queue_.push_back({key, revision, job});
        recent_.insert_or_assign(std::move(key), Recent{revision, job});
        wake_.notify_one();
        return job;
    }

private:
    using Key = std::tuple<std::filesystem::path, std::uint64_t, std::uint64_t>;
    struct Recent {
        media::FileRevision revision;
        std::weak_ptr<WarpOnsetJob> job;
    };
    struct Work {
        Key key;
        media::FileRevision revision;
        std::weak_ptr<WarpOnsetJob> job;
    };
    struct Playable {
        audio::DecodeCache& decode;
        std::filesystem::path path;
        bool pinned;

        Playable(audio::DecodeCache& cache, const std::filesystem::path& source,
                 std::stop_token stop)
            : decode(cache), path(cache.playable(source, stop)), pinned(path != source) {}

        ~Playable() {
            if (pinned) try { decode.release(path); } catch (...) {}
        }
    };

    static media::FileRevision fileRevision(const std::filesystem::path& path) {
        std::error_code error;
        auto revision = media::readFileRevision(path, error);
        if (!revision)
            throw std::filesystem::filesystem_error("cannot identify the Beats source", path, error);
        return *revision;
    }

    audio::TransientResult analyze(const Work& entry, std::stop_token stop) {
        const auto& [source, first, length] = entry.key;
        if (fileRevision(source) != entry.revision) return {audio::AnalysisError::Io, {}};
        Playable playable(decode_, source, stop);
        audio::WavReader reader(playable.path);
        auto result = audio::detectTransients(reader, first, length, 50, stop);
        if (fileRevision(source) != entry.revision) return {audio::AnalysisError::Io, {}};
        if (!result) return {result.error, {}};
        return result;
    }

    void work(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Work entry;
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, stop, [&] { return !queue_.empty(); }) || stop.stop_requested())
                    return;
                entry = std::move(queue_.front());
                queue_.pop_front();
            }
            auto job = entry.job.lock();
            if (!job) continue;
            try {
                job->result_ = analyze(entry, stop);
            } catch (const audio::DecodeError& error) {
                job->result_.error = error.problem() == audio::DecodeProblem::Cancelled
                    ? audio::AnalysisError::Cancelled : audio::AnalysisError::Io;
            } catch (...) {
                job->result_.error = audio::AnalysisError::Io;
            }
            job->ready_.store(true, std::memory_order_release);
            if (job->result_) {
                const std::lock_guard lock(mutex_);
                retain(job);
            }
        }
    }

    // Caller holds mutex_. Only this small LRU retains completed work after a
    // graph releases it. Outstanding requests have weak queue/index entries.
    void retain(const std::shared_ptr<WarpOnsetJob>& job) noexcept {
        auto to = std::find(retained_.begin(), retained_.end(), job);
        if (to == retained_.end()) to = std::prev(to);
        std::move_backward(retained_.begin(), to, std::next(to));
        retained_.front() = job;
        std::size_t bytes = 0;
        for (auto current = retained_.begin(); current != retained_.end() && *current; ++current) {
            const auto used = (*current)->result_.frames.capacity() * sizeof(std::uint64_t);
            if (used > retainedBytes - bytes) {
                std::fill(current, retained_.end(), nullptr);
                break;
            }
            bytes += used;
        }
    }

    static constexpr std::size_t retainedBytes = 16u * 1024u * 1024u;
    audio::DecodeCache& decode_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Work> queue_;
    std::map<Key, Recent> recent_;
    std::array<std::shared_ptr<WarpOnsetJob>, 32> retained_;
    unsigned requests_ = 0;
    std::jthread worker_;
};

} // namespace detail

// Owner/worker only. Compressed files use the shared decode cache and its
// release rules. A callback request refuses before constructing either cache.
[[nodiscard]] inline std::shared_ptr<const WarpOnsetJob> requestWarpOnsets(
    const std::filesystem::path& source, std::uint64_t first, std::uint64_t length) {
    if (audio::onAudioThread) return {};
    static detail::WarpOnsetCache cache(audio::defaultDecodeCache());
    return cache.request(source, first, length);
}

} // namespace adi::engine
