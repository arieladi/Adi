// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/jobs.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/control/session_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace adi::control {
namespace {

std::int64_t steadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string newJobId() {
    const auto token = makeSessionToken();
    return token.empty() ? std::string{} : "job-" + token.substr(0, 32);
}

bool agentJob(JobOwnership ownership) {
    return ownership == JobOwnership::Agent || ownership == JobOwnership::Foundry;
}

bool terminal(JobState state) {
    return state == JobState::Completed || state == JobState::Failed || state == JobState::Cancelled;
}

}  // namespace

std::string_view toString(JobState state) {
    switch (state) {
        case JobState::Pending: return "pending";
        case JobState::Running: return "running";
        case JobState::Completed: return "completed";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return {};
}

std::string_view toString(JobRevisionPolicy policy) {
    switch (policy) {
        case JobRevisionPolicy::CheckAtStartOnly: return "check_at_start_only";
        case JobRevisionPolicy::StableUntilCompletion: return "stable_until_completion";
    }
    return {};
}

JobBook::JobBook(std::function<std::int64_t()> nowMs, std::function<std::string()> newId)
    : nowMs_(nowMs ? std::move(nowMs) : steadyMs), newId_(newId ? std::move(newId) : newJobId) {}

JobBook::Entry* JobBook::find(std::string_view id) {
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& candidate) {
        return candidate.info.id == id;
    });
    return entry == entries_.end() ? nullptr : &*entry;
}

void JobBook::forgetEvents(std::string_view id) {
    std::erase_if(events_, [&](const Event& event) { return event.info.id == id; });
}

void JobBook::prune() {
    const auto now = nowMs_();
    if (now < std::numeric_limits<std::int64_t>::min() + kRetentionMs) return;
    const auto cutoff = now - kRetentionMs;
    std::erase_if(entries_, [&](const Entry& entry) {
        if (!entry.terminalMs || *entry.terminalMs > cutoff) return false;
        forgetEvents(entry.info.id);
        return true;
    });
}

void JobBook::emit(const Entry& entry, bool progressOnly) {
    if (progressOnly) {
        std::erase_if(events_, [&](const Event& event) {
            return event.progressOnly && event.info.id == entry.info.id;
        });
    }
    events_.push_back({entry.owner, entry.info, progressOnly});
}

JobInfo JobBook::settle(Entry& entry, JobState state, std::string error, bool conflict) {
    entry.info.state = state;
    if (state == JobState::Completed) entry.info.progress = 1.0;
    entry.info.error = std::move(error);
    entry.info.conflict = conflict;
    entry.readSet.clear();
    entry.terminalMs = nowMs_();
    emit(entry);
    return entry.info;
}

std::optional<JobInfo> JobBook::enqueue(std::string owner, JobSpec spec) {
    if (audio::onAudioThread || owner.empty() || spec.kind.empty() || toString(spec.revision).empty())
        return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!agentJobsArmed_ && agentJob(spec.ownership)) return std::nullopt;
    prune();
    const auto id = newId_();
    if (id.empty() || find(id)) return std::nullopt;
    if (entries_.size() == kCapacity) {
        auto oldest = entries_.end();
        for (auto entry = entries_.begin(); entry != entries_.end(); ++entry) {
            if (entry->terminalMs && (oldest == entries_.end() || *entry->terminalMs < *oldest->terminalMs))
                oldest = entry;
        }
        if (oldest == entries_.end()) return std::nullopt;
        forgetEvents(oldest->info.id);
        entries_.erase(oldest);
    }
    Entry entry;
    entry.owner = std::move(owner);
    entry.info.id = id;
    entry.info.spec = std::move(spec);
    entries_.push_back(std::move(entry));
    emit(entries_.back());
    return entries_.back().info;
}

std::optional<std::stop_token> JobBook::start(std::string_view id, JobReadSet readSet) {
    if (audio::onAudioThread) return std::nullopt;
    std::lock_guard lock(mutex_);
    auto* entry = find(id);
    if (!entry || entry->info.state != JobState::Pending ||
        (!agentJobsArmed_ && agentJob(entry->info.spec.ownership))) return std::nullopt;
    if (entry->info.spec.revision == JobRevisionPolicy::StableUntilCompletion && readSet.empty())
        return std::nullopt;
    entry->readSet = std::move(readSet);
    entry->info.state = JobState::Running;
    emit(*entry);
    return entry->stop.get_token();
}

bool JobBook::progress(std::string_view id, double fraction) {
    if (audio::onAudioThread || !std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) return false;
    std::lock_guard lock(mutex_);
    auto* entry = find(id);
    if (!entry || entry->info.state != JobState::Running || entry->completion != Completion::Open ||
        entry->info.cancelRequested || fraction < entry->info.progress) return false;
    if (fraction > entry->info.progress) {
        entry->info.progress = fraction;
        emit(*entry, true);
    }
    return true;
}

std::optional<JobInfo> JobBook::complete(std::string_view id,
                                          const std::function<bool(const JobReadSet&)>& matchesReadSet,
                                          const std::function<bool()>& publish) {
    if (audio::onAudioThread) return std::nullopt;
    JobReadSet readSet;
    {
        std::lock_guard lock(mutex_);
        auto* entry = find(id);
        if (!entry || entry->info.state != JobState::Running || entry->completion != Completion::Open)
            return std::nullopt;
        if (entry->info.cancelRequested) return settle(*entry, JobState::Cancelled);
        if (entry->info.spec.revision == JobRevisionPolicy::StableUntilCompletion) {
            if (!matchesReadSet) return settle(*entry, JobState::Failed, "the job has no input check");
            readSet = entry->readSet;
        }
        entry->completion = Completion::Checking;
    }

    bool matches = true;
    bool checked = true;
    try {
        if (!readSet.empty()) matches = matchesReadSet(readSet);
    } catch (...) {
        checked = false;
    }
    {
        std::lock_guard lock(mutex_);
        auto& entry = *find(id);
        if (entry.info.cancelRequested) return settle(entry, JobState::Cancelled);
        if (!checked) return settle(entry, JobState::Failed, "could not check the job's inputs");
        if (!matches) return settle(entry, JobState::Failed, "the job's declared inputs changed", true);
        entry.completion = Completion::Publishing;
    }

    bool published = true;
    try {
        if (publish) published = publish();
    } catch (...) {
        published = false;
    }
    std::lock_guard lock(mutex_);
    auto& entry = *find(id);
    return published ? settle(entry, JobState::Completed)
                     : settle(entry, JobState::Failed, "could not publish the job result");
}

std::optional<JobInfo> JobBook::fail(std::string_view id, std::string error) {
    return workerFinished(id, JobState::Failed, std::move(error));
}

std::optional<JobInfo> JobBook::workerFinished(std::string_view id, JobState state, std::string error) {
    if (audio::onAudioThread || !terminal(state)) return std::nullopt;
    std::lock_guard lock(mutex_);
    auto* entry = find(id);
    if (!entry || terminal(entry->info.state) || entry->completion != Completion::Open)
        return std::nullopt;
    if (state == JobState::Completed &&
        (entry->info.state != JobState::Running ||
         entry->info.spec.revision != JobRevisionPolicy::CheckAtStartOnly))
        return std::nullopt;
    if (state == JobState::Cancelled || (state == JobState::Failed && entry->info.cancelRequested))
        return settle(*entry, JobState::Cancelled);
    return settle(*entry, state, std::move(error));
}

std::optional<JobInfo> JobBook::get(std::string_view owner, std::string_view id) {
    if (audio::onAudioThread) return std::nullopt;
    std::lock_guard lock(mutex_);
    prune();
    const auto* entry = find(id);
    if (!entry || entry->owner != owner) return std::nullopt;
    return entry->info;
}

std::vector<JobInfo> JobBook::poll(std::string_view owner, std::size_t maximum) {
    if (audio::onAudioThread) return {};
    std::lock_guard lock(mutex_);
    prune();
    std::vector<JobInfo> result;
    const auto limit = std::min(maximum, kPollLimit);
    for (auto event = events_.begin(); event != events_.end() && result.size() < limit;) {
        if (event->owner != owner) {
            ++event;
            continue;
        }
        result.push_back(std::move(event->info));
        event = events_.erase(event);
    }
    return result;
}

bool JobBook::requestCancellation(Entry& entry, std::stop_source& toStop) {
    if (terminal(entry.info.state) || entry.info.cancelRequested ||
        entry.completion == Completion::Publishing) return false;
    entry.info.cancelRequested = true;
    if (entry.info.state == JobState::Pending) {
        (void)settle(entry, JobState::Cancelled);
    } else {
        emit(entry);
        toStop = entry.stop;
    }
    return true;
}

std::optional<JobInfo> JobBook::cancel(std::string_view owner, std::string_view id) {
    if (audio::onAudioThread) return std::nullopt;
    std::stop_source toStop(std::nostopstate);
    std::optional<JobInfo> result;
    {
        std::lock_guard lock(mutex_);
        prune();
        auto* entry = find(id);
        if (!entry || entry->owner != owner) return std::nullopt;
        (void)requestCancellation(*entry, toStop);
        result = entry->info;
    }
    toStop.request_stop();  // stop callbacks may call the book
    return result;
}

std::size_t JobBook::stopAgentJobs() {
    if (audio::onAudioThread) return 0;
    std::vector<std::stop_source> signals;
    signals.reserve(kCapacity);
    std::size_t cancelled = 0;
    {
        std::lock_guard lock(mutex_);
        agentJobsArmed_ = false;
        prune();
        for (auto& entry : entries_) {
            if (!agentJob(entry.info.spec.ownership)) continue;
            std::stop_source toStop(std::nostopstate);
            if (!requestCancellation(entry, toStop)) continue;
            ++cancelled;
            if (toStop.stop_possible()) signals.push_back(std::move(toStop));
        }
    }
    for (auto& signal : signals) signal.request_stop();
    return cancelled;
}

void JobBook::rearmAgentJobs() {
    if (audio::onAudioThread) return;
    std::lock_guard lock(mutex_);
    agentJobsArmed_ = true;
}

}  // namespace adi::control
