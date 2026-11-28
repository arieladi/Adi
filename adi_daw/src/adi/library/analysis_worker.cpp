// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis_worker.hpp"
#include "adi/audio/io_audit.hpp"
#include "volume.hpp"
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace adi::library {
namespace {
const std::string& keyOf(const AnalysisJob& job) {
    return job.key.empty() ? job.hash : job.key;
}

struct Sources {
    AnalysisJob first;
    std::vector<AnalysisJob> alternatives;
};

struct Queue {
    using Jobs = std::list<Sources>;
    Jobs jobs;
    std::unordered_map<std::string, Jobs::iterator> byKey, byHash;

    void add(AnalysisJob&& job) {
        if (job.source.empty() || keyOf(job).empty()) return;
        const auto duplicate = byKey.find(keyOf(job));
        if (duplicate != byKey.end()) {
            // With the default key the other source may have the same key and hash.
            auto& group = *duplicate->second;
            if (!job.hash.empty() && job.hash == group.first.hash && job.source != group.first.source)
                group.alternatives.push_back(std::move(job));
            return;
        }
        const auto existing = job.hash.empty() ? byHash.end() : byHash.find(job.hash);
        if (existing != byHash.end()) {
            const auto group = existing->second;
            group->alternatives.push_back(std::move(job));
            byKey.emplace(keyOf(group->alternatives.back()), group);
            return;
        }
        const auto added = jobs.insert(jobs.end(), {std::move(job), {}});
        byKey.emplace(keyOf(added->first), added);
        if (!added->first.hash.empty()) byHash.emplace(added->first.hash, added);
    }

    Sources take(Jobs::iterator job) {
        byKey.erase(keyOf(job->first));
        if (!job->first.hash.empty()) byHash.erase(job->first.hash);
        for (const auto& alternate : job->alternatives) byKey.erase(keyOf(alternate));
        auto result = std::move(*job);
        jobs.erase(job);
        return result;
    }

    void replace(Queue& other) {
        jobs.swap(other.jobs);
        byKey.swap(other.byKey);
        byHash.swap(other.byHash);
    }
};
}

struct AnalysisWorker::Impl {
    explicit Impl(Analyze callback)
        : analyze(std::move(callback)), worker([this](std::stop_token stop) { run(stop); }) {}

    Analyze analyze;
    mutable std::mutex mutex;
    std::condition_variable_any cv;
    Queue queue;
    std::vector<std::string> shown;
    std::size_t nextShown = 0;
    AnalysisWorkerStats status;
    std::jthread worker;

    Sources takeNext() {
        while (nextShown < shown.size()) {
            const auto match = queue.byKey.find(shown[nextShown++]);
            if (match != queue.byKey.end()) return queue.take(match->second);
        }
        return queue.take(queue.jobs.begin());
    }

    bool tryAnalyze(const AnalysisJob& job, std::stop_token stop) const {
        try {
            return analyze(job, stop);
        } catch (...) {
            return false;
        }
    }

    bool analyzeSources(const Sources& sources, std::stop_token stop) const {
        if (tryAnalyze(sources.first, stop)) return true;
        if (sources.alternatives.empty() || stop.stop_requested()) return false;

        std::unordered_set<std::filesystem::path> tried;
        tried.insert(sources.first.source);
        for (const auto& job : sources.alternatives) {
            if (stop.stop_requested()) return false;
            if (tried.insert(job.source).second && tryAnalyze(job, stop)) return true;
        }
        return false;
    }

    void run(std::stop_token stop) {
        const auto lowPriority = lowerWorkerPriority();
        std::unique_lock lock(mutex);
        status.lowPriority = lowPriority;
        while (cv.wait(lock, stop, [&] { return !status.paused && !queue.jobs.empty(); }) &&
               !stop.stop_requested()) {
            const auto sources = takeNext();
            status.active = true;
            lock.unlock();
            bool succeeded = false;
            try {
                succeeded = analyzeSources(sources, stop);
            } catch (const std::bad_alloc&) {
                // An alternate-path lookup could not allocate memory.
            }
            lock.lock();
            status.active = false;
            if (succeeded) {
                ++status.completed;
            } else if (!stop.stop_requested()) {
                ++status.failed;
            }
            if (queue.jobs.empty()) cv.notify_all();
        }
    }
};

AnalysisWorker::AnalysisWorker(Analyze analyze) : impl_(std::make_unique<Impl>(std::move(analyze))) {}
AnalysisWorker::~AnalysisWorker() = default;

void AnalysisWorker::replaceJobs(std::vector<AnalysisJob>&& jobs, std::span<const std::string> shownKeys) {
    if (audio::onAudioThread) return;
    Queue replacement;
    replacement.byKey.reserve(jobs.size());
    replacement.byHash.reserve(jobs.size());
    for (auto& job : jobs) replacement.add(std::move(job));
    std::vector<std::string> shown(shownKeys.begin(), shownKeys.end());
    {
        std::lock_guard lock(impl_->mutex);
        impl_->queue.replace(replacement);
        impl_->shown.swap(shown);
        impl_->nextShown = 0;
    }
    impl_->cv.notify_all();
}

void AnalysisWorker::prioritize(std::span<const std::string> shownKeys) {
    if (audio::onAudioThread) return;
    std::vector<std::string> shown(shownKeys.begin(), shownKeys.end());
    std::lock_guard lock(impl_->mutex);
    impl_->shown.swap(shown);
    impl_->nextShown = 0;
}

void AnalysisWorker::pause() {
    if (audio::onAudioThread) return;
    std::lock_guard lock(impl_->mutex);
    impl_->status.paused = true;
}

void AnalysisWorker::resume() {
    if (audio::onAudioThread) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->status.paused = false;
    }
    impl_->cv.notify_all();
}

AnalysisWorkerStats AnalysisWorker::stats() const {
    if (audio::onAudioThread) return {};
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->status;
    result.pending = impl_->queue.jobs.size();
    return result;
}

bool AnalysisWorker::waitIdle(std::stop_token stop) {
    if (audio::onAudioThread || stop.stop_requested()) return false;
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait(lock, stop, [&] { return impl_->queue.jobs.empty() && !impl_->status.active; });
}
}
