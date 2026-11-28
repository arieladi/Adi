// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace adi::control {

enum class JobState { Pending, Running, Completed, Failed, Cancelled };
enum class JobRevisionPolicy { CheckAtStartOnly, StableUntilCompletion };
enum class JobOwnership { Ordinary, Agent, Foundry };

[[nodiscard]] std::string_view toString(JobState);
[[nodiscard]] std::string_view toString(JobRevisionPolicy);

struct JobSpec {
    std::string kind;
    std::string project;
    JobRevisionPolicy revision = JobRevisionPolicy::CheckAtStartOnly;
    JobOwnership ownership = JobOwnership::Ordinary;
};

struct JobInfo {
    std::string id;
    JobSpec spec;
    JobState state = JobState::Pending;
    double progress = 0.0;
    bool cancelRequested = false;
    bool conflict = false;
    std::string error;
};

/// The producer chooses the inputs that its result depends on and captures
/// their versions when it starts. Unrelated edits need not affect this set.
struct JobRead {
    std::string key;
    std::string version;
};
using JobReadSet = std::vector<JobRead>;

/// AGT-04, ADR-0222. Application-scoped observation and cancellation for
/// existing workers. A producer retains scheduling and execution of its work.
class JobBook {
public:
    static constexpr std::size_t kCapacity = 128;
    static constexpr std::int64_t kRetentionMs = 10 * 60 * 1000;
    static constexpr std::size_t kPollLimit = 128;

    explicit JobBook(std::function<std::int64_t()> nowMs = {}, std::function<std::string()> newId = {});

    /// Trusted producer seams. The caller supplies the authenticated owner at
    /// enqueue and retains the returned id for later worker notifications.
    [[nodiscard]] std::optional<JobInfo> enqueue(std::string owner, JobSpec);
    [[nodiscard]] std::optional<std::stop_token> start(std::string_view id, JobReadSet = {});
    [[nodiscard]] bool progress(std::string_view id, double fraction);

    /// Call from the project owner when publishing into a project. The guard
    /// reads the declared inputs, and the publisher runs synchronously on that
    /// same owner after admission. Both run without the book lock. Cancel can
    /// win during the guard, until the job enters the publication safe point.
    [[nodiscard]] std::optional<JobInfo> complete(
        std::string_view id, const std::function<bool(const JobReadSet&)>& matchesReadSet = {},
        const std::function<bool()>& publish = {});

    [[nodiscard]] std::optional<JobInfo> fail(std::string_view id, std::string error);

    /// External workers such as render::Queue can report an already settled
    /// result. Only CheckAtStartOnly may report an already-published success.
    [[nodiscard]] std::optional<JobInfo> workerFinished(std::string_view id, JobState terminal,
                                                       std::string error = {});

    /// A wrong owner gets the same answer as an unknown job. Poll consumes
    /// only this owner's events. Rapid progress updates coalesce until polled.
    [[nodiscard]] std::optional<JobInfo> get(std::string_view owner, std::string_view id);
    [[nodiscard]] std::vector<JobInfo> poll(std::string_view owner, std::size_t maximum = kPollLimit);
    [[nodiscard]] std::optional<JobInfo> cancel(std::string_view owner, std::string_view id);

    /// ADR-0259 d1. Stop accepts no new Agent or Foundry jobs and requests
    /// cancellation of the old ones. Return the count newly cancelled or
    /// signalled. Jobs already admitted to publish finish at their safe point.
    [[nodiscard]] std::size_t stopAgentJobs();
    void rearmAgentJobs();

private:
    enum class Completion { Open, Checking, Publishing };

    struct Entry {
        std::string owner;
        JobInfo info;
        std::stop_source stop;
        JobReadSet readSet;
        Completion completion = Completion::Open;
        std::optional<std::int64_t> terminalMs;
    };
    struct Event {
        std::string owner;
        JobInfo info;
        bool progressOnly;
    };

    [[nodiscard]] Entry* find(std::string_view id);
    void prune();
    void forgetEvents(std::string_view id);
    void emit(const Entry&, bool progressOnly = false);
    [[nodiscard]] JobInfo settle(Entry&, JobState, std::string error = {}, bool conflict = false);
    [[nodiscard]] bool requestCancellation(Entry&, std::stop_source&);

    std::function<std::int64_t()> nowMs_;
    std::function<std::string()> newId_;
    std::mutex mutex_;
    bool agentJobsArmed_ = true;
    std::vector<Entry> entries_;
    // Each retained job contributes at most five events: Pending, Running,
    // latest progress, a cancel request, and its terminal state.
    std::deque<Event> events_;
};

}  // namespace adi::control
