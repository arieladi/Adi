// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/render_jobs.hpp"

#include <algorithm>
#include <utility>

namespace adi::control {

TrackedRenderQueue::~TrackedRenderQueue() {
    for (const auto& [unused, member] : members_) {
        (void)unused;
        try { (void)book_.cancel(member.owner, member.id); }
        catch (...) {} // The renderer and its owner are already going away.
    }
}

std::optional<JobInfo> TrackedRenderQueue::enqueue(std::string owner, std::string project, render::Job job,
                                                    JobOwnership ownership) {
    const auto id = queue_.add(std::move(job));
    try {
        auto member = members_.emplace(id, Member{{}, std::move(owner)}).first;
        auto info = book_.enqueue(member->second.owner,
                                  {"render", std::move(project), JobRevisionPolicy::CheckAtStartOnly, ownership});
        if (info) {
            member->second.id = info->id;
            return info;
        }
    } catch (...) {
        members_.erase(id);
        (void)queue_.remove(id);
        throw;
    }
    members_.erase(id);
    (void)queue_.remove(id);
    return std::nullopt;
}

void TrackedRenderQueue::run(engine::DeviceLoader loader, render::Control control) {
    const render::QueueObserver observe{
        [this](render::JobId id) -> std::optional<std::stop_token> {
            const auto member = members_.find(id);
            if (member == members_.end()) return std::nullopt;
            auto stop = book_.start(member->second.id);
            if (!stop) members_.erase(member); // cancelled while pending
            return stop;
        },
        [this](render::JobId id, const render::Result& result) {
            const auto member = members_.find(id);
            if (member == members_.end()) return;
            auto terminal = JobState::Failed;
            if (result.state == render::JobState::Completed) terminal = JobState::Completed;
            if (result.state == render::JobState::Cancelled) terminal = JobState::Cancelled;
            // Full render errors remain in the trusted queue. They may name
            // local files, so the client job receipt carries a fixed error.
            (void)book_.workerFinished(member->second.id, terminal,
                                       terminal == JobState::Failed ? "render failed" : "");
            members_.erase(member);
        }};
    control.progress = [this, progress = std::move(control.progress)](
        render::JobId id, std::uint64_t written, std::uint64_t total) {
        if (const auto member = members_.find(id); member != members_.end() && total != 0) {
            const auto fraction = static_cast<double>(written) / static_cast<double>(total);
            (void)book_.progress(member->second.id, std::min(fraction, 1.0));
        }
        if (progress) progress(id, written, total);
    };
    queue_.run(std::move(loader), std::move(control), observe);
}

}  // namespace adi::control
