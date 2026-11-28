// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "adi/control/jobs.hpp"
#include "adi/render/queue.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace adi::control {

/// One render::Queue with its members visible in the application's JobBook.
/// The renderer still owns intake, run membership, snapshots and files.
/// Use this on the render queue's owner thread. Clients use JobBook to watch
/// and cancel concurrently.
class TrackedRenderQueue final {
public:
    explicit TrackedRenderQueue(JobBook& book) : book_(book) {}
    ~TrackedRenderQueue();
    TrackedRenderQueue(const TrackedRenderQueue&) = delete;
    TrackedRenderQueue& operator=(const TrackedRenderQueue&) = delete;

    [[nodiscard]] std::optional<JobInfo> enqueue(std::string owner, std::string project,
                                                  render::Job job,
                                                  JobOwnership ownership = JobOwnership::Ordinary);
    void run(engine::DeviceLoader = {}, render::Control = {});
    [[nodiscard]] std::vector<render::Entry> snapshot() const { return queue_.snapshot(); }
    [[nodiscard]] const render::RunReport& lastRun() const noexcept { return queue_.lastRun(); }

private:
    struct Member {
        std::string id;
        std::string owner;
    };

    JobBook& book_;
    render::Queue queue_;
    std::unordered_map<render::JobId, Member> members_;
};

}  // namespace adi::control
