// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/agent_runtime.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/control/agent_log.hpp"
#include "adi/control/consent_scope.hpp"
#include "adi/control/session_discovery.hpp"
#include "adi/media/blake3.hpp"

#include <algorithm>
#include <chrono>
#include <span>
#include <utility>

namespace adi::control {
namespace {

using namespace std::chrono_literals;
using SteadyClock = std::chrono::steady_clock;
constexpr auto kWallBudget = 2min;
constexpr auto kProbeBudget = 5s;
constexpr int kToolBudget = 8;
constexpr int kTokenBudget = 32'000;
constexpr int kSameCallBudget = 2;

std::int64_t wallSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string_view nameOf(AgentProviderKind kind) {
    switch (kind) {
        case AgentProviderKind::Local: return "local";
        case AgentProviderKind::Lan: return "lan";
        case AgentProviderKind::Cloud: return "cloud";
        case AgentProviderKind::Faux: return "faux";
    }
    return {};
}

bool configured(const AgentLaunch& launch) {
    if (!launch.command.executable.is_absolute() || launch.provider.model.empty()) return false;
    switch (launch.provider.kind) {
        case AgentProviderKind::Local: return isLoopbackDestination(launch.provider.endpoint);
        case AgentProviderKind::Lan: return !launch.provider.endpoint.empty();
        case AgentProviderKind::Cloud: return !launch.provider.vendor.empty();
        case AgentProviderKind::Faux: return true;
    }
    return false;
}

Json providerJson(const AgentProvider& provider) {
    Json out = {{"kind", nameOf(provider.kind)}, {"model", provider.model}};
    if (!provider.endpoint.empty()) out["endpoint"] = provider.endpoint;
    if (!provider.vendor.empty()) out["vendor"] = provider.vendor;
    if (!provider.credentialAccount.empty()) out["credentialAccount"] = provider.credentialAccount;
    return out;
}

ConsentDestination destinationOf(const AgentProvider& provider) {
    auto endpoint = provider.endpoint;
    if (provider.kind == AgentProviderKind::Faux) endpoint = "http://127.0.0.1";
    if (provider.kind == AgentProviderKind::Cloud) endpoint = "cloud:" + provider.vendor;
    return {"adi-agent:" + std::string(nameOf(provider.kind)) + ":" + provider.model +
                ":" + provider.credentialAccount, std::move(endpoint), false};
}

ConsentRequest disclosure(std::string project, const AgentProvider& provider, std::string_view text) {
    const auto hash = media::blake3Bytes(std::as_bytes(std::span(text)));
    return {std::move(project), destinationOf(provider), ConsentCapability::ModelProvider,
            {{ConsentData::Prompt, text.size()}}, {}, hash.hex};
}

std::string_view controlMethod(std::string_view tool) {
    if (tool == "project_read") return "project.read";
    if (tool == "ops_describe") return "ops.describe";
    if (tool == "browser_catalog") return "browser.catalog";
    if (tool == "changeset_propose") return "changeset.propose";
    return {};
}

std::optional<Json> eventFrom(std::optional<std::string> line) {
    if (!line || nestingDepth(*line, 64) > 64) return std::nullopt;
    auto event = Json::parse(*line, nullptr, false);
    if (!event.is_object()) return std::nullopt;
    return event;
}

int reportedCount(const Json& event, const char* key, int limit) {
    const auto value = event.find(key);
    if (value == event.end()) return 0;
    if (!value->is_number_integer() || *value < 0 || *value > limit) return limit + 1;
    return value->get<int>();
}

std::string field(const Json& event, const char* key) {
    const auto value = event.find(key);
    return value != event.end() && value->is_string() ? value->get<std::string>() : std::string{};
}

}  // namespace

AgentRuntime::AgentRuntime(ConsentLedger& ledger, JobBook& jobs, std::function<void(ConsentRequest)> ask,
                           AgentLog* log)
    : ledger_(ledger), jobs_(jobs), ask_(std::move(ask)), log_(log) {}

AgentCheck AgentRuntime::check(const AgentLaunch& launch, std::string projectUuid) {
    if (audio::onAudioThread) return {};
    AgentCheck result;
    if (!configured(launch) || projectUuid.empty()) {
        result.detail = "configure the helper and its model";
    } else {
        auto request = disclosure(projectUuid, launch.provider, "helper check");
        const auto consent = ledger_.authorize(request, wallSeconds());
        if (consent != ConsentDecision::Allowed) {
            if (consent == ConsentDecision::ConsentRequired && ask_) ask_(std::move(request));
            result = {AgentReadiness::ConsentRequired, "the provider needs consent"};
        } else {
            result = {AgentReadiness::Unavailable, "the helper did not answer this launch's check"};
            const auto id = makeSessionToken();
            AgentProcess child;
            std::string error;
            if (!id.empty() && child.start(launch.command, error) &&
                child.send(dumpJson({{"type", "check"}, {"id", id}, {"provider", providerJson(launch.provider)}}),
                           kProbeBudget, error)) {
                const auto reply = eventFrom(child.read(kProbeBudget, error));
                if (reply && field(*reply, "type") == "ready" && field(*reply, "id") == id &&
                    reply->contains("ok") && reply->at("ok") == true) {
                    result = {AgentReadiness::Ready, {}};
                }
            }
        }
    }
    std::lock_guard lock(mutex_);
    checked_.insert_or_assign(std::move(projectUuid), Checked{launch, result});
    return result;
}

AgentCheck AgentRuntime::readiness(std::string_view projectUuid) const {
    if (audio::onAudioThread) return {};
    std::lock_guard lock(mutex_);
    const auto found = checked_.find(std::string(projectUuid));
    return found == checked_.end() ? AgentCheck{} : found->second.result;
}

void AgentRuntime::releaseLane(const std::string& project) {
    {
        std::lock_guard lock(mutex_);
        auto lane = lanes_.find(project);
        if (lane != lanes_.end()) {
            lane->second.pop_front();
            if (lane->second.empty()) lanes_.erase(lane);
        }
    }
    laneReady_.notify_all();
}

AgentRunResult AgentRuntime::run(const AgentLaunch& launch, std::string owner, std::string projectUuid,
                                  std::string projectHandle, std::string request, RunOrigin origin,
                                  const AgentTool& tool) {
    if (audio::onAudioThread) return {};
    AgentRunResult result;
    result.reason = "unavailable";
    if (!tool || request.empty() || request.size() > 64 * 1024 || projectHandle.empty()) return result;
    const auto queued = jobs_.enqueue(std::move(owner), {"agent", projectUuid,
        JobRevisionPolicy::CheckAtStartOnly, JobOwnership::Agent});
    const auto historyId = queued ? queued->id : (log_ ? makeSessionToken() : std::string{});
    const auto source = origin == RunOrigin::MarkerPrompt ? "marker" : "agent";
    if (log_) (void)log_->append(projectUuid, {AgentLogKind::Request, historyId, request, {}, "user", source});
    const auto finish = [&] {
        if (log_) (void)log_->append(projectUuid,
            {AgentLogKind::Answer, historyId, result.answer, result.reason, "agent", source});
        return std::move(result);
    };
    if (!queued) {
        result.reason = "agent_stopped_or_busy";
        return finish();
    }
    result.job = queued->id;
    {
        std::unique_lock lock(mutex_);
        lanes_[projectUuid].push_back(result.job);
        laneReady_.wait(lock, [&] { return lanes_.at(projectUuid).front() == result.job; });
    }
    struct ReleaseLane {
        AgentRuntime& runtime;
        const std::string& project;
        ~ReleaseLane() { runtime.releaseLane(project); }
    } lane{*this, projectUuid};
    {
        std::lock_guard lock(mutex_);
        const auto entry = checked_.find(projectUuid);
        if (entry == checked_.end() || !entry->second.result.canAnswer() ||
            entry->second.launch.command.executable != launch.command.executable ||
            entry->second.launch.command.arguments != launch.command.arguments ||
            entry->second.launch.provider != launch.provider) {
            (void)jobs_.fail(result.job, "check the helper at this launch before asking");
            return finish();
        }
    }
    const auto stop = jobs_.start(result.job);
    if (!stop) {
        result.reason = "cancelled";
        return finish();
    }
    auto question = disclosure(projectUuid, launch.provider, request);
    const auto consent = ledger_.authorize(question, wallSeconds());
    if (consent != ConsentDecision::Allowed) {
        if (consent == ConsentDecision::ConsentRequired && ask_) ask_(std::move(question));
        result.reason = "consent_required";
        (void)jobs_.fail(result.job, "the provider needs consent");
        return finish();
    }
    const auto destination = std::make_shared<const ConsentDestination>(destinationOf(launch.provider));
    RequestMeta meta;
    meta.readDestination = destination;
    meta.runOrigin = origin;
    meta.runPolicy = std::make_shared<RunPolicy>(!isLoopbackDestination(destination->endpoint));

    AgentProcess child;
    std::string error;
    const auto end = SteadyClock::now() + kWallBudget;
    const auto remaining = [&] {
        return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(end - SteadyClock::now()), 0ms);
    };
    const auto send = [&](const Json& frame) {
        return child.send(dumpJson(frame), remaining(), error, *stop);
    };
    const auto settle = [&] {
        if (stop->stop_requested()) {
            result.reason = "cancelled";
            (void)jobs_.workerFinished(result.job, JobState::Cancelled);
        } else if (result.reason == "completed") {
            const auto completed = jobs_.complete(result.job);
            if (!completed || completed->state != JobState::Completed) {
                result.reason = completed && completed->state == JobState::Cancelled ? "cancelled" : "unavailable";
            }
        } else {
            const auto failed = jobs_.fail(result.job, result.reason);
            if (failed && failed->state == JobState::Cancelled) result.reason = "cancelled";
        }
    };
    if (!stop->stop_requested() && child.start(launch.command, error) &&
        send({{"type", "run"}, {"id", result.job}, {"request", request}, {"project", projectHandle},
              {"provider", providerJson(launch.provider)}})) {
        int calls = 0;
        bool answered = false;
        bool proposed = false;
        Json head = nullptr;
        std::unordered_map<std::string, int> repeats;
        while (!stop->stop_requested() && remaining() > 0ms) {
            const auto event = eventFrom(child.read(remaining(), error, *stop));
            if (!event || field(*event, "id") != result.job) break;
            const auto type = field(*event, "type");
            if (type == "answer") {
                if (answered || !event->contains("text") || !event->at("text").is_string()) {
                    result.reason = "unexpected_helper_event";
                    break;
                }
                answered = true;
                result.answer = field(*event, "text");
                continue;
            }
            if (type == "ended") {
                result.steps = reportedCount(*event, "steps", kToolBudget);
                result.tokens = reportedCount(*event, "tokens", kTokenBudget);
                result.reason = field(*event, "reason");
                if (result.steps > kToolBudget || result.tokens > kTokenBudget) result.reason = "budget_exceeded";
                if (result.reason == "completed" && !answered) result.reason = "unexpected_helper_event";
                if (result.reason.empty()) result.reason = "unavailable";
                break;
            }
            const auto callId = field(*event, "call");
            const auto method = controlMethod(field(*event, "name"));
            const auto supplied = event->find("args");
            auto args = supplied == event->end() ? Json::object() : *supplied;
            if (answered || type != "tool" || method.empty() || callId.empty() || !args.is_object()) {
                result.reason = "unexpected_helper_event";
                break;
            }
            if (++calls > kToolBudget) {
                result.reason = "budget_exceeded";
                break;
            }
            if (method != "ops.describe") args["project"] = projectHandle;
            const bool proposes = method == "changeset.propose";
            if (proposes) {
                if (proposed) {
                    result.reason = "changeset_limit";
                    break;
                }
                args["request"] = request;
            }
            const auto callKey = dumpJson(Json::array({method, args, head}));
            if (++repeats[callKey] > kSameCallBudget) {
                result.reason = "repeated_tool_call";
                break;
            }
            if (stop->stop_requested()) break;
            if (proposes) meta.idempotencyKey = result.job + ":" + callId;
            else meta.idempotencyKey.reset();
            const auto outcome = tool(method, args, meta, *stop);
            Json response = {{"type", "tool_result"}, {"call", callId}};
            if (outcome.error) {
                response["error"] = {{"code", outcome.error->code}, {"message", outcome.error->message}};
            } else {
                if (proposes) proposed = true;
                response["result"] = outcome.result;
                if (method == "project.read" && outcome.result.contains("head") && outcome.result.at("head") != head) {
                    // Charge the first read to the head it actually returned.
                    const auto prior = repeats.find(callKey);
                    if (--prior->second == 0) repeats.erase(prior);
                    head = outcome.result.at("head");
                    ++repeats[dumpJson(Json::array({method, args, head}))];
                }
            }
            if (!send(response)) break;
            (void)jobs_.progress(result.job, static_cast<double>(calls) / kToolBudget);
        }
    }
    if (remaining() == 0ms && result.reason == "unavailable") result.reason = "wall_budget_exceeded";
    settle();
    if (result.reason != "completed") result.answer.clear();
    if (result.reason == "unavailable" || result.reason == "provider_error") {
        std::lock_guard lock(mutex_);
        const auto found = checked_.find(projectUuid);
        if (found != checked_.end() && found->second.launch.provider == launch.provider)
            found->second.result = {AgentReadiness::Unavailable, "the configured helper stopped answering"};
    }
    return finish();
}

}  // namespace adi::control
