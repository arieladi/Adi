// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/agent_runtime.hpp"
#include "adi/control/agent_log.hpp"
#include "temp_directory.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace adi::control;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr std::string_view kProject = "project-uuid";
constexpr std::string_view kHandle = "project-handle";
constexpr std::string_view kOwner = "owner";
int checks = 0;
int failures = 0;

void check(bool ok, std::string_view detail) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("  FAIL  %.*s\n", static_cast<int>(detail.size()), detail.data());
    }
}

Json receive() {
    std::string line;
    if (!std::getline(std::cin, line)) return nullptr;
    return Json::parse(line, nullptr, false);
}

void emit(const Json& frame) {
    std::cout << dumpJson(frame) << std::endl;
}

int fixtureHelper(std::string_view mode, const std::optional<fs::path>& marker) {
    if (marker) {
        std::ofstream trace(*marker);
        trace << "a helper process started\n";
        if (!trace) return 2;
    }

    const auto request = receive();
    if (!request.is_object()) return 3;
    const auto id = request.value("id", std::string{});
    const auto type = request.value("type", std::string{});
    if (type == "check") {
        if (mode == "silent") return 0;
        emit({{"type", "ready"}, {"id", mode == "wrong-id" ? "other-check" : id}, {"ok", true}});
        return 0;
    }
    if (type != "run" || id.empty() || request.value("project", std::string{}) != kHandle) return 4;
    const auto tool = [&](std::string_view call, std::string_view name, Json args) {
        emit({{"type", "tool"}, {"id", id}, {"call", call}, {"name", name}, {"args", std::move(args)}});
    };
    const auto result = [](std::string_view call) {
        const auto reply = receive();
        if (!reply.is_object() || reply.value("call", std::string{}) != call ||
            reply.value("type", std::string{}) != "tool_result")
            return Json(nullptr);
        return reply.value("result", Json(nullptr));
    };

    if (mode == "trusted") {
        tool("read", "project_read", {{"project", "untrusted-project"}, {"level", 1}});
        if (const auto reply = result("read"); !reply.is_object() || reply.value("head", 0) != 17) return 5;
        tool("proposal", "changeset_propose", {{"project", "untrusted-project"}, {"baseHead", 17},
             {"request", "a fixture change"}, {"ops", Json::array()}});
        if (const auto reply = result("proposal"); !reply.is_object() || !reply.value("queued", false)) return 6;
        emit({{"type", "answer"}, {"id", id}, {"text", "fixture answer"}});
        emit({{"type", "ended"}, {"id", id}, {"reason", "completed"}, {"steps", 2}, {"tokens", 19}});
        return 0;
    }
    if (mode == "repeat" || mode == "repeat-read") {
        const bool reading = mode == "repeat-read";
        const auto args = reading ? Json{{"level", 0}} : Json{{"names", Json::array({"clip.setName"})}};
        for (const std::string_view call : {"first", "second", "third"}) {
            tool(call, reading ? "project_read" : "ops_describe", args);
            if (call != "third" && !result(call).is_object()) return 7;
        }
        return 0;
    }
    if (mode == "unauthorized") {
        tool("other", "outside_registry", Json::object());
        return 0;
    }
    if (mode == "malformed") {
        tool("wrong-shape", "ops_describe", Json::array());
        return 0;
    }
    if (mode == "two-proposals" || mode == "tool-after-answer") {
        if (mode == "tool-after-answer") emit({{"type", "answer"}, {"id", id}, {"text", "finished"}});
        const auto args = Json{{"baseHead", 17}, {"ops", Json::array()}};
        tool("first-proposal", "changeset_propose", args);
        if (mode == "two-proposals" && result("first-proposal").is_object())
            tool("second-proposal", "changeset_propose", args);
        return 0;
    }
    if (mode == "hold") {
        tool("first", "ops_describe", Json::object());
        if (result("first") != Json::object()) return 8;
        // The parent interrupts its pipe read and reaps the child. A bounded
        // fallback gives a finite failure if Stop does not reach the runtime.
        std::this_thread::sleep_for(7s);
        return 0;
    }
    return 9;
}

std::string pathUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

AgentLaunch fixtureLaunch(const fs::path& testExecutable, std::string mode,
                          const std::optional<fs::path>& marker = std::nullopt) {
    AgentLaunch launch;
    launch.command.executable = testExecutable;
    launch.command.arguments = {"--fixture-helper", std::move(mode)};
    if (marker) launch.command.arguments.push_back(pathUtf8(*marker));
    launch.provider.kind = AgentProviderKind::Faux;
    launch.provider.model = "fixture-model";
    return launch;
}

struct Fixture {
    adi::test::TempDirectory temp;
    ConsentLedger ledger;
    JobBook jobs;
    AgentLog log;
    std::vector<ConsentRequest> questions;
    AgentRuntime runtime;

    explicit Fixture(const char* name)
        : temp("agent_runtime", name), ledger(temp.path() / "control" / "consent.json"),
          log(temp.path() / "agent-log"),
          runtime(ledger, jobs, [this](ConsentRequest request) { questions.push_back(std::move(request)); }, &log) {}
};

bool ready(Fixture& fixture, const AgentLaunch& launch) {
    const auto response = fixture.runtime.check(launch, std::string(kProject));
    check(response.canAnswer(), "the helper answers its check for this launch");
    return response.canAnswer();
}

AgentRunResult run(Fixture& fixture, const AgentLaunch& launch, RunOrigin origin, const AgentTool& tool) {
    return fixture.runtime.run(launch, std::string(kOwner), std::string(kProject), std::string(kHandle),
                               "a synthetic request", origin, tool);
}

void testReadiness(const fs::path& executable) {
    std::puts("[readiness requires a matching reply for this launch]");
    Fixture fixture("readiness");
    check(fixture.runtime.readiness(kProject).state == AgentReadiness::Unconfigured,
          "an unchecked launch is not ready");
    auto missing = fixtureLaunch(executable, "trusted");
    missing.command.executable = fixture.temp.path() / "no-such-helper";
    check(fixture.runtime.check(missing, std::string(kProject)).state == AgentReadiness::Unavailable,
          "a configured but missing helper fails its check");
    check(fixture.runtime.check(fixtureLaunch(executable, "silent"), std::string(kProject)).state ==
              AgentReadiness::Unavailable,
          "a helper that exits without answering fails its check");
    check(fixture.runtime.check(fixtureLaunch(executable, "wrong-id"), std::string(kProject)).state ==
              AgentReadiness::Unavailable,
          "a ready message with a different check id is ignored");
    const auto launch = fixtureLaunch(executable, "trusted");
    if (!ready(fixture, launch)) return;
    check(fixture.runtime.readiness(kProject).canAnswer() && !fixture.runtime.readiness("other-project").canAnswer(),
          "the successful check belongs to its project");
    AgentRuntime nextLaunch(fixture.ledger, fixture.jobs);
    check(!nextLaunch.readiness(kProject).canAnswer(), "a new runtime needs its own check");

    auto changed = launch;
    changed.provider.model = "different-model";
    int calls = 0;
    const auto result = run(fixture, changed, RunOrigin::Direct,
        [&](std::string_view, const Json&, const RequestMeta&, std::stop_token) {
            ++calls;
            return Outcome::ok(Json::object());
        });
    check(result.reason == "unavailable" && calls == 0, "a different launch configuration cannot reuse the check");
}

void testConsentBeforeSpawn(const fs::path& executable) {
    std::puts("[the provider consent gate precedes child startup]");
    Fixture fixture("consent");
    const auto marker = fixture.temp.path() / "spawned";
    auto launch = fixtureLaunch(executable, "trusted", marker);
    launch.provider.kind = AgentProviderKind::Lan;
    launch.provider.endpoint = "http://192.168.1.2:123";
    const auto refused = fixture.runtime.check(launch, std::string(kProject));
    check(refused.state == AgentReadiness::ConsentRequired && !fs::exists(marker),
          "the LAN check needs consent before spawning anything");
    const bool asked = fixture.questions.size() == 1;
    check(asked, "the host receives a consent question");
    if (!asked) return;
    const auto& question = fixture.questions.front();
    check(question.projectUuid == kProject && question.destination.endpoint == launch.provider.endpoint &&
              question.capability == ConsentCapability::ModelProvider && question.data.size() == 1 &&
              question.data.front().kind == ConsentData::Prompt && question.data.front().bytes != 0,
          "the question identifies the project, recipient, and prompt kind and size");
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const bool granted = fixture.ledger.grant(question, ConsentTerm::ThisTime, now);
    check(granted, "grant only the one-time check");
    if (!granted || !ready(fixture, launch)) return;
    check(fs::remove(marker), "the approved check launched the child");

    int calls = 0;
    const auto deniedRun = run(fixture, launch, RunOrigin::Direct,
        [&](std::string_view, const Json&, const RequestMeta&, std::stop_token) {
            ++calls;
            return Outcome::ok(Json::object());
        });
    check(deniedRun.reason == "consent_required" && calls == 0 && !fs::exists(marker) &&
              fixture.questions.size() == 2,
          "approval of the check does not authorize launching a new prompt");
}

void testTrustedDispatch(const fs::path& executable) {
    std::puts("[the host supplies project identity and marker provenance]");
    Fixture fixture("dispatch");
    const auto launch = fixtureLaunch(executable, "trusted");
    if (!ready(fixture, launch)) return;
    std::shared_ptr<RunPolicy> policy;
    std::vector<std::string> methods;
    std::string key;
    const auto result = run(fixture, launch, RunOrigin::MarkerPrompt,
        [&](std::string_view method, const Json& arguments, const RequestMeta& meta, std::stop_token stop) {
            methods.emplace_back(method);
            check(arguments.value("project", std::string{}) == kHandle &&
                      meta.runOrigin == RunOrigin::MarkerPrompt && !stop.stop_requested(),
                  "each callback uses the owner's project binding and marker origin");
            check(meta.readDestination && meta.readDestination->endpoint == "http://127.0.0.1" &&
                      meta.runPolicy && !meta.runPolicy->egress,
                  "the host owns the provider destination and run policy");
            if (method == "project.read") {
                check(!meta.idempotencyKey, "a read consumes no changeset key");
                policy = meta.runPolicy;
                if (policy) policy->readRemarks.store(true);
                return Outcome::ok({{"head", 17}});
            }
            check(meta.runPolicy == policy && policy && policy->readRemarks.load(),
                  "the proposal sees the same taint state as the earlier read");
            check(arguments.value("request", std::string{}) == "a synthetic request",
                  "the proposal records the actual request instead of the helper's wording");
            key = meta.idempotencyKey.value_or(std::string{});
            return Outcome::ok({{"queued", true}});
        });
    check(methods == std::vector<std::string>{"project.read", "changeset.propose"},
          "only the corresponding control methods are dispatched");
    check(result.reason == "completed" && result.answer == "fixture answer" &&
              result.steps == 2 && result.tokens == 19 && key == result.job + ":proposal",
          "the reply, usage, and tool idempotency key cross the real process boundary");
    const auto job = fixture.jobs.get(kOwner, result.job);
    check(job && job->state == JobState::Completed, "the run settles its observable job");
    const auto log = fixture.log.read(kProject);
    check(log && log->size() == 2 && log->at(0).event.kind == AgentLogKind::Request &&
              log->at(1).event.kind == AgentLogKind::Answer && log->at(0).event.id == result.job &&
              log->at(1).event.id == result.job && log->at(1).event.text == result.answer,
          "a marker request and its answer are recorded in order with the same run id");
}

void testBounds(const fs::path& executable) {
    std::puts("[invalid tools and repeated calls are decided by the host]");
    for (const std::string_view mode : {"repeat", "repeat-read", "unauthorized", "malformed", "two-proposals",
                                        "tool-after-answer"}) {
        Fixture fixture(mode.data());
        const auto launch = fixtureLaunch(executable, std::string(mode));
        if (!ready(fixture, launch)) continue;
        int calls = 0;
        const auto result = run(fixture, launch, RunOrigin::Direct,
            [&](std::string_view method, const Json&, const RequestMeta&, std::stop_token) {
                ++calls;
                if (method == "project.read") return Outcome::ok({{"head", 17}});
                return Outcome::ok(Json::object());
            });
        const auto job = fixture.jobs.get(kOwner, result.job);
        if (mode == "repeat" || mode == "repeat-read") {
            check(result.reason == "repeated_tool_call" && calls == 2,
                  "the third identical call is refused before dispatch");
        } else if (mode == "two-proposals") {
            check(result.reason == "changeset_limit" && calls == 1,
                  "a run cannot publish a second proposal");
        } else {
            check(result.reason == "unexpected_helper_event" && calls == 0,
                  "malformed or unlisted tool calls never reach the dispatcher");
        }
        check(result.answer.empty() && job && job->state == JobState::Failed,
              "a refused run has no answer and settles as failed");
    }
}

void testStop(const fs::path& executable) {
    std::puts("[Stop cancels a run blocked on its helper]");
    Fixture fixture("stop");
    const auto launch = fixtureLaunch(executable, "hold");
    if (!ready(fixture, launch)) return;

    std::promise<void> entered;
    auto observed = entered.get_future();
    auto running = std::async(std::launch::async, [&] {
        return run(fixture, launch, RunOrigin::Direct,
            [&](std::string_view, const Json&, const RequestMeta&, std::stop_token) {
                entered.set_value();
                return Outcome::ok(Json::object());
            });
    });
    check(observed.wait_for(4s) == std::future_status::ready, "the fixture was dispatched and is waiting");
    check(fixture.jobs.stopAgentJobs() == 1, "Stop signals the active agent job");
    check(running.wait_for(3s) == std::future_status::ready, "Stop interrupts the pipe wait before the fixture exits");
    const auto result = running.get();
    const auto job = fixture.jobs.get(kOwner, result.job);
    check(result.reason == "cancelled" && result.answer.empty() && job && job->state == JobState::Cancelled,
          "the run returns cancelled and leaves no answer");
}

void testNodeHelper(const fs::path& node, const fs::path& helper) {
    std::puts("[the native runtime can check and run the Node helper]");
    Fixture fixture("node_helper");
    AgentLaunch launch{{fs::absolute(node), {pathUtf8(fs::absolute(helper))}},
                       {AgentProviderKind::Faux, "faux:reads", {}, {}, {}}};
    if (!ready(fixture, launch)) return;
    std::vector<std::string> methods;
    const auto result = run(fixture, launch, RunOrigin::Direct,
        [&](std::string_view method, const Json&, const RequestMeta&, std::stop_token) {
            methods.emplace_back(method);
            return Outcome::ok(Json::object());
        });
    check(methods == std::vector<std::string>{"project.read", "ops.describe"},
          "the Node helper calls the two read tools through the native host");
    check(result.reason == "completed" && !result.answer.empty(), "the Node helper sends a completed answer");
}

int entry(int argc, const std::vector<fs::path>& args) {
    if (argc >= 3 && args[1] == "--fixture-helper") {
        return fixtureHelper(pathUtf8(args[2]), argc == 4 ? std::optional<fs::path>{args[3]} : std::nullopt);
    }
    try {
        const auto executable = fs::absolute(args[0]);
        testReadiness(executable);
        testConsentBeforeSpawn(executable);
        testTrustedDispatch(executable);
        testBounds(executable);
        testStop(executable);
        if (argc == 4 && args[1] == "--node-helper") testNodeHelper(args[2], args[3]);
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<fs::path> args;
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    return entry(argc, args);
}
