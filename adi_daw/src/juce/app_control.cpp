// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/app_control.hpp"

#include "adi/control/agent_stop.hpp"
#include "adi/control/jobs.hpp"
#include "adi/control/live_project.hpp"
#include "adi/control/loopback_listener.hpp"
#include "adi/control/mcp.hpp"
#include "adi/version.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace adi::control {
namespace {

std::int64_t nowMicros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

using SessionId = LoopbackListener::SessionId;
using Reply = LoopbackListener::Reply;
using Cancel = LoopbackListener::Cancel;

struct McpWork {
    Incoming frame;
    Reply reply;
    std::uint64_t receivedAgentEpoch = 0;
    std::atomic<CallState> state{CallState::Pending};
};

struct Client {
    McpEndpoint::Session mcp;
    std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<std::shared_ptr<McpWork>> queue;
    // Joins before the other fields are destroyed.
    std::jthread worker;
};

}  // namespace

class AppControl::Impl {
public:
    Impl(Executor owner, AppControlOptions config)
        : options(std::move(config)),
          service([owner = std::move(owner), alive = std::weak_ptr<int>(lifetime)](Task task) {
              owner([alive, task = std::move(task)] { if (alive.lock()) task(); });
          }, grants),
          context{projects, queue, options.limits, nowMicros, {}, "adi", kVersion, "control"},
          mcp(service, projects, {"adi", "ADI", kVersion}) {
        context.appTier = [this] { return options.tier; };
        context.maxOpsPerMinute = [this] { return options.maxOpsPerMinute; };
        context.consentLedger = &ledger;
        context.jobs = &jobs;
        context.agentWrites = &agentWrites;
        context.askConsent = [this](ConsentRequest question) {
            std::lock_guard lock(questionMutex);
            if (questions.size() < 64) questions.push_back(std::move(question));
        };
        registerControlMethods(service, context);
    }

    ~Impl() {
        stop();
        attach(nullptr);
        lifetime.reset();
    }

    bool start(std::string& error) {
        if (listener) return true;
        auto token = makeSessionToken();
        if (token.empty()) {
            error = "could not generate the control session token";
            return false;
        }
        service.audit().registerSecret(token);
        auto next = std::make_unique<LoopbackListener>(
            LoopbackOptions{options.port, token, options.allowedOrigins},
            [this](LoopbackRoute route, SessionId id, std::string_view frame, Reply reply) {
                return dispatch(route, id, frame, std::move(reply));
            },
            [this](LoopbackRoute route, SessionId id, bool connected) { connection(route, id, connected); });
        if (!next->start(error)) return false;
        SessionDiscovery record{next->port(), std::move(token)};
        if (options.writeDiscoveryFile && !discovery.publish(record)) {
            next->stop();
            error = "could not publish the control session discovery file";
            return false;
        }
        activeSession = std::move(record);
        listener = std::move(next);
        return true;
    }

    void stop() {
        discovery.reset();
        if (listener) listener->stop();
        listener.reset();
        activeSession.reset();
        std::lock_guard lock(questionMutex);
        questions.clear();
    }

    void attach(ui::ProjectDocument* doc) {
        if (doc && live && live->matches(*doc)) return;
        if (live) live->detach();
        projects.clear();
        live.reset();
        if (doc) {
            std::string error;
            live = LiveProject::open(*doc, error);
            projects.add(live);
        }
    }

    void connection(LoopbackRoute route, SessionId id, bool connected) {
        if (connected) {
            auto client = std::make_shared<Client>();
            auto credential = makeSessionToken();
            if (credential.empty()) return;
            client->mcp.credential = std::move(credential);
            client->mcp.readDestination = std::make_shared<ConsentDestination>(
                ConsentDestination{client->mcp.credential, "external control client", true});
            grants.grant(client->mcp.credential, std::min(options.tier, Tier::Propose));
            service.audit().record({service.now(), "connect", "external control client", "", 0, "loopback"});
            if (route == LoopbackRoute::Mcp) {
                client->worker = std::jthread([this, entry = client.get()](std::stop_token stop) {
                    runMcp(*entry, stop);
                });
            }
            std::lock_guard lock(clientMutex);
            clients.emplace(id, std::move(client));
            return;
        }
        std::shared_ptr<Client> client;
        {
            std::lock_guard lock(clientMutex);
            auto found = clients.find(id);
            if (found == clients.end()) return;
            client = std::move(found->second);
            clients.erase(found);
        }
        grants.revoke(client->mcp.credential);
        client->worker.request_stop();
        service.audit().record({service.now(), "disconnect", "external control client", "", 0, "loopback"});
    }

    Cancel dispatch(LoopbackRoute route, SessionId id, std::string_view frame, Reply reply) {
        const auto receivedAgentEpoch = agentWrites.generation();
        std::shared_ptr<Client> client;
        {
            std::lock_guard lock(clientMutex);
            if (auto found = clients.find(id); found != clients.end()) client = found->second;
        }
        auto message = parseMessage(frame, {245760, 64});
        if (!client) {
            reply(encodeError(message.id, {errc::NotGranted, "not granted", nullptr}));
            return {};
        }
        if (route == LoopbackRoute::Mcp) {
            auto work = std::make_shared<McpWork>();
            work->frame = std::move(message);
            work->reply = std::move(reply);
            work->receivedAgentEpoch = receivedAgentEpoch;
            {
                std::lock_guard lock(client->mutex);
                client->queue.push_back(work);
            }
            client->ready.notify_one();
            return [work] {
                auto pending = CallState::Pending;
                return work->state.compare_exchange_strong(pending, CallState::Abandoned);
            };
        }
        if (message.kind == Incoming::Kind::Invalid) {
            reply(encodeError(message.id, message.error));
            return {};
        }
        if (message.kind != Incoming::Kind::Request) {
            reply(std::nullopt);
            return {};
        }
        RpcError error;
        auto meta = parseMeta(message.meta, error);
        if (!meta) {
            reply(encodeError(message.id, error));
            return {};
        }
        meta->deadlineMs = std::min(meta->deadlineMs.value_or(10000), std::int64_t{10000});
        meta->readDestination = client->mcp.readDestination;
        meta->receivedAgentEpoch = receivedAgentEpoch;
        auto pending = service.call({client->mcp.credential, "external control client"},
                                    message.method, message.params, *meta,
                                    [id = std::move(message.id), reply = std::move(reply)](Outcome out) {
            reply(out.error ? encodeError(id, *out.error) : encodeResult(id, out.result));
        });
        return [pending = std::move(pending)] { return pending->abandon(); };
    }

    void runMcp(Client& client, std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::shared_ptr<McpWork> work;
            {
                std::unique_lock lock(client.mutex);
                if (!client.ready.wait(lock, stop, [&] { return !client.queue.empty(); })) return;
                work = std::move(client.queue.front());
                client.queue.pop_front();
            }
            auto pending = CallState::Pending;
            if (!work->state.compare_exchange_strong(pending, CallState::Taken)) continue;
            client.mcp.receivedAgentEpoch = work->receivedAgentEpoch;
            work->reply(mcp.handle(work->frame, client.mcp, stop));
        }
    }

    AppControlOptions options;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    GrantTable grants;
    Service service;
    ProjectSet projects;
    ChangesetQueue queue;
    JobBook jobs;
    AgentWriteGate agentWrites;
    ConsentLedger ledger;
    ControlContext context;
    McpEndpoint mcp;
    SessionDiscoveryFile discovery;
    std::optional<SessionDiscovery> activeSession;
    std::mutex clientMutex;
    std::unordered_map<SessionId, std::shared_ptr<Client>> clients;
    std::mutex questionMutex;
    std::vector<ConsentRequest> questions;
    std::shared_ptr<LiveProject> live;
    std::unique_ptr<LoopbackListener> listener;
};

AppControl::AppControl(Executor owner, AppControlOptions options)
    : impl_(std::make_unique<Impl>(std::move(owner), std::move(options))) {}

AppControl::~AppControl() = default;
bool AppControl::start(std::string& error) { return impl_->start(error); }
void AppControl::stop() { impl_->stop(); }
void AppControl::configure(AppControlOptions options) {
    impl_->stop();
    impl_->options = std::move(options);
    impl_->context.limits = impl_->options.limits;
}
std::optional<SessionDiscovery> AppControl::session() const { return impl_->activeSession; }
void AppControl::attach(ui::ProjectDocument* doc) { impl_->attach(doc); }

void AppControl::tick(ui::ProjectDocument* doc) {
    if (!impl_->activeSession) return;
    impl_->attach(doc);
    if (impl_->live) impl_->live->publishTelemetry(impl_->service.now());
}

std::vector<ConsentRequest> AppControl::takeConsentQuestions() {
    std::lock_guard lock(impl_->questionMutex);
    return std::exchange(impl_->questions, {});
}

bool AppControl::answerConsent(const ConsentRequest& request, ConsentTerm term) {
    return impl_->ledger.grant(request, term, nowMicros() / 1'000'000);
}

std::vector<QueuedChangeset> AppControl::proposals() const {
    impl_->queue.expire(impl_->service.now());
    return impl_->queue.forReview();
}

JobBook& AppControl::jobs() { return impl_->jobs; }

AgentStopReport AppControl::stopAgent(std::string surface) {
    impl_->agentWrites.stop();
    AgentStopReport report;
    report.atUtcMicros = nowMicros();
    report.surface = std::move(surface);
    report.rejectedProposals = impl_->queue.rejectPending();
    report.cancelledJobs = impl_->jobs.stopAgentJobs();
    return report;
}

void AppControl::rearmAgent() {
    impl_->jobs.rearmAgentJobs();
    impl_->agentWrites.rearm();
}

bool AppControl::agentStopped() const { return impl_->agentWrites.stopped(); }

agent::ApplyResult AppControl::review(std::string_view id, bool approve) {
    if (approve && agentStopped()) return {false, agent::Refusal::Invalid, "agent writes are stopped"};
    auto entries = proposals();
    const auto entry = std::find_if(entries.begin(), entries.end(), [id](const QueuedChangeset& e) {
        return e.id == id;
    });
    if (entry == entries.end()) return {false, agent::Refusal::Invalid, "the proposal is no longer awaiting review"};
    if (!approve) {
        const bool changed = impl_->queue.setState(id, entry->credential, Review::Rejected);
        return {changed, changed ? agent::Refusal::None : agent::Refusal::Invalid, {}};
    }
    auto project = impl_->projects.find(entry->project);
    if (!project) return {false, agent::Refusal::ReadOnly, "the project is no longer open"};
    auto result = project->applyChangeset(entry->changeset, impl_->options.limits);
    if (result.ok) impl_->queue.setState(id, entry->credential, Review::Applied);
    return result;
}

}  // namespace adi::control
