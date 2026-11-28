// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace adi::library {
struct AnalysisJob {
    std::filesystem::path source;
    std::string hash; // May be empty while the file index hashes the source.
    std::string key{}; // Stable listing identity. Defaults to the content hash.
};

struct AnalysisWorkerStats {
    std::size_t pending = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    bool active = false;
    bool paused = true;
    bool lowPriority = false;
};

// Own and call off the audio thread. The callback receives the shutdown token
// and runs without the queue lock. A true result includes an existing cache hit.
class AnalysisWorker {
public:
    using Analyze = std::function<bool(const AnalysisJob&, std::stop_token)>;

    explicit AnalysisWorker(Analyze);
    ~AnalysisWorker();
    AnalysisWorker(const AnalysisWorker&) = delete;
    AnalysisWorker& operator=(const AnalysisWorker&) = delete;

    // Replace pending work. Known equal hashes form one job with alternate paths.
    // The in-flight callback is allowed to finish.
    void replaceJobs(std::vector<AnalysisJob>&&, std::span<const std::string> shownKeys = {});
    // Set the order for displayed jobs. Unlisted jobs follow in submission order.
    void prioritize(std::span<const std::string> shownKeys);
    // The worker starts paused. Pausing lets an in-flight file finish.
    void pause();
    void resume();

    [[nodiscard]] AnalysisWorkerStats stats() const;
    // Pending paused jobs are not idle. A stopped wait leaves the queue intact.
    [[nodiscard]] bool waitIdle(std::stop_token = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
