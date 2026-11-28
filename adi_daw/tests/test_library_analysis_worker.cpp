// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/library/analysis_worker.hpp"
#include "adi/audio/io_audit.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using namespace adi::library;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int failures = 0;
void check(bool condition, const char* description) {
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

void shownOrderAndAlternateSources() {
    std::vector<AnalysisJob> tried;
    AnalysisWorker worker([&](const AnalysisJob& job, std::stop_token) {
        tried.push_back(job);
        if (job.source == "missing.wav") throw std::runtime_error("file disappeared");
        return job.source != "unreadable.wav";
    });
    worker.replaceJobs({{"first.wav", "first"}, {"second.wav", "second"}, {"missing.wav", "shown"},
                        {"replacement.wav", "shown"}, {"alias.wav", "shown", "other-place/alias"},
                        {"unreadable.wav", "bad"}, {"first.wav", "first"}, {"", "invalid"},
                        {"unidentified.wav", ""}, {"cold.wav", "", "one-place/cold"}});
    const std::array<std::string, 5> shown{"other-place/alias", "unknown", "one-place/cold", "second", "shown"};
    worker.prioritize(shown);
    check(worker.stats().paused && worker.stats().pending == 5 && !worker.stats().active,
          "new worker queues each valid content hash once and starts paused");
    worker.resume();
    check(worker.waitIdle(), "worker reaches idle");
    const auto state = worker.stats();
    check(state.pending == 0 && !state.active && state.completed == 4 && state.failed == 1,
          "worker reports completed and failed content, counting the alternate path only once");
    check(tried.size() == 6 && tried[0].source == "missing.wav" && tried[1].source == "replacement.wav" &&
              tried[2].hash.empty() && tried[2].key == "one-place/cold" && tried[3].hash == "second" &&
              tried[4].hash == "first" && tried[5].hash == "bad",
          "shown jobs run in list order, missing paths try their duplicate, then unshown jobs run in queue order");
}

void pauseAndReplaceSnapshot() {
    std::binary_semaphore firstStarted{0}, finishFirst{0};
    std::vector<std::string> tried;
    AnalysisWorker worker([&](const AnalysisJob& job, std::stop_token) {
        tried.push_back(job.hash);
        if (tried.size() == 1) {
            firstStarted.release();
            finishFirst.acquire();
        }
        return true;
    });
    worker.replaceJobs({{"active.wav", "active"}, {"removed.wav", "removed"}});
    worker.resume();
    const bool started = firstStarted.try_acquire_for(5s);
    check(started, "first callback starts");
    worker.pause();
    const std::array<std::string, 1> shown{"shown"};
    worker.replaceJobs({{"new.wav", "new"}, {"duplicate.wav", "active"}, {"shown.wav", "shown"}}, shown);
    check(worker.stats().paused && worker.stats().pending == 3 && worker.stats().active == started,
          "pause retains work while a new published snapshot replaces queued work");
    finishFirst.release();
    std::stop_source stop;
    stop.request_stop();
    check(!worker.waitIdle(stop.get_token()), "waiting for paused jobs can be cancelled");
    worker.resume();
    check(worker.waitIdle(), "resuming drains the latest snapshot");
    check(tried == std::vector<std::string>{"active", "shown", "new", "active"},
          "the replacement publishes shown files first and rechecks an in-flight file in the new snapshot");
}

void stopAndRefuseCallback() {
    std::binary_semaphore started{0};
    bool stopped = false;
    auto worker = std::make_unique<AnalysisWorker>([&](const AnalysisJob&, std::stop_token stop) {
        std::mutex mutex;
        std::unique_lock lock(mutex);
        std::condition_variable_any cv;
        started.release();
        cv.wait(lock, stop, [] { return false; });
        stopped = stop.stop_requested();
        return false;
    });
    const std::array<std::string, 1> shown{"wanted"};
    std::vector<AnalysisJob> unwanted{{"unwanted.wav", "unwanted"}};
    worker->replaceJobs({{"wanted.wav", "wanted"}});
    {
        const adi::audio::CallbackScope audio;
        worker->replaceJobs(std::move(unwanted));
        worker->prioritize(shown);
        worker->resume();
        worker->pause();
        check(!worker->waitIdle(), "the marked audio callback refuses to wait");
    }
    check(worker->stats().paused && worker->stats().pending == 1, "the callback cannot edit or start the queue");
    worker->resume();
    check(started.try_acquire_for(5s), "worker starts the retained job");
    worker.reset();
    check(stopped, "destruction stops the callback and joins it");
}

void benchmark() {
    constexpr std::size_t files = 200'000, displayed = 40;
    std::vector<AnalysisJob> jobs;
    jobs.reserve(files);
    std::array<std::string, displayed> shown;
    for (std::size_t i = 0; i < files; ++i) {
        auto number = std::to_string(i);
        auto hash = std::string(64 - number.size(), '0') + number;
        jobs.push_back({"library/sample" + number + ".wav", std::move(hash)});
    }
    for (std::size_t i = 0; i < displayed; ++i) shown[i] = jobs[files - 1 - i].hash;

    Clock::time_point shownFinished;
    std::size_t dispatched = 0;
    bool order = true;
    AnalysisWorker worker([&](const AnalysisJob& job, std::stop_token) {
        if (dispatched < displayed) order = order && job.hash == shown[dispatched];
        if (++dispatched == displayed) shownFinished = Clock::now();
        return true;
    });
    const auto buildStart = Clock::now();
    worker.replaceJobs(std::move(jobs));
    const auto buildEnd = Clock::now();
    worker.prioritize(shown);
    const auto dispatchStart = Clock::now();
    worker.resume();
    check(worker.waitIdle(), "benchmark dispatch finishes");
    const auto dispatchEnd = Clock::now();
    check(order && dispatched == files, "benchmark dispatches every file, shown files first");

    const auto ms = [](Clock::duration elapsed) { return std::chrono::duration<double, std::milli>(elapsed).count(); };
    std::printf("200,000 synthetic queue entries, no decoding: build %.3f ms, priority %.3f ms, "
                "first 40 callbacks %.3f ms, all callbacks %.3f ms\n",
                ms(buildEnd - buildStart), ms(dispatchStart - buildEnd),
                ms(shownFinished - dispatchStart), ms(dispatchEnd - dispatchStart));
}
}

int main(int argc, char** argv) {
    shownOrderAndAlternateSources();
    pauseAndReplaceSnapshot();
    stopAndRefuseCallback();
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") benchmark();
    return failures == 0 ? 0 : 1;
}
