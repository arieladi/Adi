// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/jobs.hpp"

#include <cstdio>
#include <future>
#include <limits>
#include <string>
#include <utility>

namespace {

using namespace adi::control;

int failures = 0;
int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", message);
}

struct Fixture {
    std::int64_t now = 100;
    unsigned next = 0;
    JobBook book{[this] { return now; }, [this] { return "job-" + std::to_string(++next); }};
};

void ownershipAndTransitions() {
    Fixture fixture;
    auto& book = fixture.book;
    const auto render = book.enqueue("alice", {"render", "project"});
    const auto queued = book.enqueue("bob", {"render", "project"});
    check(render && queued, "both clients can own a pending job");
    if (!render || !queued) return;

    check(!book.get("bob", render->id) && !book.cancel("bob", render->id),
          "another owner's id is indistinguishable from an absent id");
    check(!book.progress(render->id, 0.25), "a pending job cannot report running progress");
    const auto stop = book.start(render->id);
    check(stop && !book.start(render->id), "a pending job starts exactly once");
    if (!stop) return;
    check(book.progress(render->id, 0.25) && book.progress(render->id, 0.75) &&
              !book.progress(render->id, 0.5) && !book.progress(render->id, 2.0) &&
              !book.progress(render->id, std::numeric_limits<double>::quiet_NaN()),
          "running progress is finite, bounded and monotone");

    const auto events = book.poll("alice", 2);
    check(events.size() == 2 && events[0].state == JobState::Pending && events[1].state == JobState::Running,
          "poll returns the owner's state transitions in order and honors its limit");
    const auto progress = book.poll("alice");
    check(progress.size() == 1 && progress[0].progress == 0.75 && book.poll("alice").empty(),
          "poll consumes one coalesced progress update");
    const auto bobEvents = book.poll("bob");
    check(bobEvents.size() == 1 && bobEvents[0].id == queued->id,
          "poll by one owner leaves the other owner's events intact");

    const auto queuedCancel = book.cancel("bob", queued->id);
    check(queuedCancel && queuedCancel->state == JobState::Cancelled && !book.start(queued->id),
          "cancelling before start is terminal and prevents the worker from starting");
    bool callbackRan = false;
    std::stop_callback callback(*stop, [&] {
        const auto running = book.get("alice", render->id);
        callbackRan = running && running->cancelRequested;
    });
    const auto runningCancel = book.cancel("alice", render->id);
    check(runningCancel && runningCancel->state == JobState::Running &&
              runningCancel->cancelRequested && callbackRan && stop->stop_requested(),
          "running cancellation posts a cooperative stop outside the book lock");
    bool calledPublisher = false;
    const auto stopped = book.complete(render->id, {}, [&] { calledPublisher = true; return true; });
    check(stopped && stopped->state == JobState::Cancelled && !calledPublisher &&
              !book.progress(render->id, 1.0) && !book.fail(render->id, "too late"),
          "the worker settles a cancelled job without publishing and its terminal status stands");
}

void revisionPolicies() {
    Fixture fixture;
    auto& book = fixture.book;
    std::string input = "revision-a";
    int unrelated = 0;
    const auto stale = book.enqueue("owner", {"stem", "project", JobRevisionPolicy::StableUntilCompletion});
    const auto current = book.enqueue("owner", {"stem", "project", JobRevisionPolicy::StableUntilCompletion});
    const auto render = book.enqueue("owner", {"render", "project"});
    check(stale && current && render, "the three revision-policy fixtures are queued");
    if (!stale || !current || !render) return;
    // Named read sets: GCC's hardened build takes a braced list written inside the call for a
    // temporary that may dangle (-Wdangling-pointer).
    const JobReadSet staleReads{{"source-clip", input}};
    check(!book.start(stale->id) && book.start(stale->id, staleReads),
          "a committing job declares its read set at start");
    check(!book.workerFinished(stale->id, JobState::Completed),
          "an already-published success cannot bypass the committing job's input guard");
    input = "revision-b";
    const JobReadSet currentReads{{"source-clip", input}};
    check(book.start(current->id, currentReads).has_value(),
          "a different job captures the newer source at its actual start");
    check(book.start(render->id).has_value(), "a render can use the owner's start-time snapshot");
    ++unrelated;

    const auto matches = [&](const JobReadSet& reads) {
        return reads.size() == 1 && reads[0].key == "source-clip" && reads[0].version == input;
    };
    int publications = 0;
    const auto publish = [&] { ++publications; return true; };
    const auto late = book.complete(stale->id, matches, publish);
    check(late && late->state == JobState::Failed && late->conflict && publications == 0,
          "a changed declared input fails with conflict before publication");
    const auto ready = book.complete(current->id, matches, publish);
    check(ready && ready->state == JobState::Completed && ready->progress == 1.0 &&
              unrelated == 1 && publications == 1,
          "an unrelated edit leaves the current input set eligible to publish");
    const auto rendered = book.complete(render->id, [](const JobReadSet&) { return false; }, publish);
    check(rendered && rendered->state == JobState::Completed && publications == 2,
          "a render uses CheckAtStartOnly and skips the completion read check");
}

void completionSafePoint() {
    Fixture fixture;
    auto& book = fixture.book;
    const auto checking = book.enqueue("owner", {"stem", "project", JobRevisionPolicy::StableUntilCompletion});
    const auto publishing = book.enqueue("owner", {"stem", "project", JobRevisionPolicy::StableUntilCompletion});
    const auto publishedRender = book.enqueue("owner", {"render", "project"});
    check(checking && publishing && publishedRender, "completion fixtures are queued");
    if (!checking || !publishing || !publishedRender) return;
    const auto checkingStop = book.start(checking->id, {{"clip", "1"}});
    const auto publishingStop = book.start(publishing->id, {{"clip", "1"}});
    const auto renderStop = book.start(publishedRender->id);
    check(checkingStop && publishingStop && renderStop, "completion fixtures are running");
    if (!checkingStop || !publishingStop || !renderStop) return;

    bool wrote = false;
    const auto cancelled = book.complete(checking->id, [&](const JobReadSet&) {
        check(book.cancel("owner", checking->id).has_value(), "cancel can reach a job during its input check");
        return true;
    }, [&] { wrote = true; return true; });
    check(cancelled && cancelled->state == JobState::Cancelled && !wrote && checkingStop->stop_requested(),
          "cancel wins until the publication safe point");
    const auto finished = book.complete(publishing->id, [](const JobReadSet&) { return true; }, [&] {
        const auto late = book.cancel("owner", publishing->id);
        return late && !late->cancelRequested && !publishingStop->stop_requested();
    });
    check(finished && finished->state == JobState::Completed,
          "the publisher runs without the book lock and a later cancel cannot stop its commit");
    check(book.cancel("owner", publishedRender->id).has_value() && renderStop->stop_requested(),
          "the book can ask a running render to stop");
    const auto actual = book.workerFinished(publishedRender->id, JobState::Completed);
    check(actual && actual->state == JobState::Completed,
          "a render already published before observing stop keeps its successful outcome");
    auto pollLeft = std::async(std::launch::async, [&] { return book.poll("owner"); });
    auto pollRight = std::async(std::launch::async, [&] { return book.poll("owner"); });
    const auto left = pollLeft.get();
    const auto right = pollRight.get();
    check(left.size() + right.size() == 11 && (left.empty() || right.empty()),
          "concurrent polls hand each queued event to exactly one poll");
}

void stopAgentJobs() {
    Fixture fixture;
    auto& book = fixture.book;
    const auto enqueue = [&](JobOwnership ownership, std::string owner) {
        return book.enqueue(std::move(owner), {"render", "project", JobRevisionPolicy::CheckAtStartOnly, ownership});
    };
    const auto queued = enqueue(JobOwnership::Agent, "agent");
    const auto agent = enqueue(JobOwnership::Agent, "agent");
    const auto foundry = enqueue(JobOwnership::Foundry, "foundry");
    const auto ordinary = book.enqueue("ordinary", {"render", "project"});
    const auto publishing = enqueue(JobOwnership::Agent, "agent");
    check(queued && agent && foundry && ordinary && publishing, "the assorted job owners are registered");
    if (!queued || !agent || !foundry || !ordinary || !publishing) return;

    const auto agentStop = book.start(agent->id);
    const auto foundryStop = book.start(foundry->id);
    const auto ordinaryStop = book.start(ordinary->id);
    const auto publishingStop = book.start(publishing->id);
    check(agentStop && foundryStop && ordinaryStop && publishingStop, "the workers have their own stop tokens");
    if (!agentStop || !foundryStop || !ordinaryStop || !publishingStop) return;

    bool callbackRan = false;
    std::stop_callback callback(*agentStop, [&] {
        const auto status = book.get("agent", agent->id);
        callbackRan = status && status->cancelRequested;
    });
    const auto admitted = book.complete(publishing->id, {}, [&] {
        check(book.stopAgentJobs() == 3, "Stop counts the one queued and two stoppable running jobs");
        return !publishingStop->stop_requested();
    });
    check(admitted && admitted->state == JobState::Completed,
          "Stop respects an already-admitted publication");
    const auto queuedStatus = book.get("agent", queued->id);
    check(queuedStatus && queuedStatus->state == JobState::Cancelled && !book.start(queued->id),
          "the queued agent job is cancelled immediately and cannot start");
    check(agentStop->stop_requested() && foundryStop->stop_requested() && callbackRan,
          "running Agent and Foundry jobs get cooperative stop callbacks outside the book lock");
    check(!ordinaryStop->stop_requested() && book.enqueue("ordinary", {"render", "project"}),
          "ordinary jobs can run and enqueue while the agent is stopped");
    check(!enqueue(JobOwnership::Agent, "agent") && !enqueue(JobOwnership::Foundry, "foundry") &&
              book.stopAgentJobs() == 0,
          "Stop blocks new agent and Foundry work and does not count cancelled work twice");

    book.rearmAgentJobs();
    const auto fresh = enqueue(JobOwnership::Foundry, "foundry");
    check(fresh && book.start(fresh->id) && enqueue(JobOwnership::Agent, "agent"),
          "re-arming admits new jobs from both stopped owners");
    check(!book.start(queued->id) && agentStop->stop_requested() && foundryStop->stop_requested(),
          "re-arming leaves the old queue and stop tokens cancelled");
    const auto settled = book.complete(agent->id);
    check(settled && settled->state == JobState::Cancelled,
          "an old cooperative job settles cancelled once its worker reaches its safe point");
}

void retentionAndCapacity() {
    Fixture fixture;
    auto& book = fixture.book;
    std::string first;
    std::string second;
    for (std::size_t index = 0; index < JobBook::kCapacity; ++index) {
        const auto job = book.enqueue("owner", {"render", "project"});
        check(job.has_value(), "a job fits under the capacity");
        if (!job) return;
        if (index == 0) first = job->id;
        if (index == 1) second = job->id;
    }
    check(!book.enqueue("owner", {"render", "project"}), "a full book does not discard active jobs");
    check(book.cancel("owner", first).has_value(), "one job reaches its terminal state");
    const auto replacement = book.enqueue("owner", {"render", "project"});
    check(replacement && !book.get("owner", first) && book.get("owner", second),
          "capacity evicts a terminal job and preserves active work");
    if (!replacement) return;
    check(book.cancel("owner", replacement->id).has_value(), "the replacement reaches a terminal state");
    fixture.now += JobBook::kRetentionMs - 1;
    check(book.get("owner", replacement->id).has_value(), "the terminal job remains just before its expiry");
    ++fixture.now;
    check(!book.get("owner", replacement->id) && book.get("owner", second),
          "the terminal job ages out but active work has no terminal expiry");
    const auto events = book.poll("owner");
    check(events.size() == JobBook::kCapacity - 1,
          "unpolled events age out alongside their terminal job");
}

}  // namespace

int main() {
    ownershipAndTransitions();
    revisionPolicies();
    completionSafePoint();
    retentionAndCapacity();
    stopAgentJobs();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures == 0 ? 0 : 1;
}
