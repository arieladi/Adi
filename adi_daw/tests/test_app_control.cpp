// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/agent_runtime.hpp"
#include "adi/control/mcp.hpp"
#include "adi/control/proposal_freshness.hpp"
#include "adi/device_presets.hpp"
#include "adi/ui/plugin_scan.hpp"
#include "adi/ui/project_document.hpp"
#include "juce/app_control.hpp"
#include "temp_directory.hpp"

#ifndef CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH
#define CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH 245760
#endif
#ifndef CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND
#define CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND 1
#endif
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace adi;
using namespace adi::control;
using namespace std::chrono_literals;
using Client = httplib::ws::WebSocketClient;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void useTestHome(const std::filesystem::path& path) {
#if defined(_WIN32)
    require(_wputenv_s(L"ADI_HOME", path.wstring().c_str()) == 0, "set isolated ADI_HOME");
#else
    require(setenv("ADI_HOME", path.c_str(), 1) == 0, "set isolated ADI_HOME");
#endif
}

/// Runs the same owner-thread tasks as MessageManager::callAsync, without a window.
class OwnerTurns {
public:
    void post(Task task) {
        {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        changed_.notify_one();
    }

    Task takeFor(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [this] { return !tasks_.empty(); })) return {};
        Task task = std::move(tasks_.front());
        tasks_.pop_front();
        return task;
    }

    template <class Call>
    auto roundTrip(Call call) {
        auto answer = std::async(std::launch::async, std::move(call));
        while (answer.wait_for(0ms) != std::future_status::ready)
            if (auto task = takeFor(10ms)) task();
        return answer.get();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Task> tasks_;
};

Json request(int id, const char* method, Json params) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

Json callTool(int id, const char* name, const Json& arguments, const char* key) {
    return request(id, "tools/call", {{"name", name}, {"arguments", arguments},
        {"_meta", {{std::string(McpEndpoint::kMetaPrefix) + "idempotencyKey", key}}}});
}

Json exchange(Client& client, const Json& requestFrame) {
    std::string frame;
    require(client.send(dumpJson(requestFrame)), "send a WebSocket request");
    require(client.read(frame) == httplib::ws::Text, "receive its WebSocket reply");
    auto reply = Json::parse(frame);
    require(reply.at("id") == requestFrame.at("id"), "the reply matches the request id");
    return reply.at("result");
}

int toolError(const Json& reply) {
    return reply.value("isError", false)
        ? reply.at("structuredContent").at("error").at("code").get<int>() : 0;
}

bool needsConsent(const Json& reply) {
    return toolError(reply) == errc::ConsentRequired;
}

void approveDisclosure(AppControl& app, const std::string& uuid, ConsentData wanted) {
    auto questions = app.takeConsentQuestions();
    require(questions.size() == 1, "one consent question reached the owner");
    const auto& question = questions.front();
    require(question.projectUuid == uuid && question.destination.externalMcp &&
            question.capability == ConsentCapability::ProjectRead, "consent is bound to this project and external client");
    require(std::any_of(question.data.begin(), question.data.end(), [wanted](const ConsentItem& item) {
        return item.kind == wanted && item.bytes > 0;
    }), "the consent question lists the expected kind and size");
    require(app.answerConsent(question, ConsentTerm::ThisTime), "the owner approves the disclosure once");
}

Json helperReceive() {
    std::string line;
    if (!std::getline(std::cin, line)) return nullptr;
    return Json::parse(line, nullptr, false);
}

void helperSend(const Json& frame) {
    std::cout << dumpJson(frame) << std::endl;
}

int bindingHelper() {
    const auto start = helperReceive();
    if (!start.is_object()) return 2;
    const auto id = start.value("id", std::string{});
    if (start.value("type", std::string{}) == "check") {
        helperSend({{"type", "ready"}, {"id", id}, {"ok", true}});
        return 0;
    }
    if (start.value("type", std::string{}) != "run") return 3;
    const auto head = Json::parse(start.at("request").get_ref<const std::string&>(), nullptr, false);
    const auto refuses = [&](const char* name, const Json& args) {
        helperSend({{"type", "tool"}, {"id", id}, {"call", name}, {"name", name}, {"args", args}});
        const auto reply = helperReceive();
        return reply.is_object() && reply.value("call", std::string{}) == name &&
               reply.contains("error") && reply.at("error").value("code", 0) == errc::InvalidParams;
    };
    const Json edit = {{"baseHead", head}, {"request", "This proposal belongs to the original file"},
                       {"ops", Json::array({{{"op", "track.rename"},
                                             {"payload", {{"id", 101}, {"name", "Wrong helper file"}}}}})}};
    if (!refuses("changeset_propose", edit) || !refuses("project_read", Json::object()) ||
        !refuses("browser_catalog", Json::object())) {
        helperSend({{"type", "ended"}, {"id", id}, {"reason", "fixture_scope_failed"}});
        return 0;
    }
    helperSend({{"type", "answer"}, {"id", id}, {"text", "The original project closed"}});
    helperSend({{"type", "ended"}, {"id", id}, {"reason", "completed"}});
    return 0;
}

/// A faux helper that does real work: it reads the project and proposes one
/// rename on the head it read. Run twice, it proposes on whatever head is current.
int proposingHelper() {
    const auto start = helperReceive();
    if (!start.is_object()) return 2;
    const auto id = start.value("id", std::string{});
    if (start.value("type", std::string{}) == "check") {
        helperSend({{"type", "ready"}, {"id", id}, {"ok", true}});
        return 0;
    }
    if (start.value("type", std::string{}) != "run") return 3;
    const auto tool = [&](const char* call, const char* name, const Json& args) {
        helperSend({{"type", "tool"}, {"id", id}, {"call", call}, {"name", name}, {"args", args}});
        const auto reply = helperReceive();
        return reply.is_object() && reply.value("call", std::string{}) == call && reply.contains("result")
            ? reply.at("result") : Json(nullptr);
    };
    const auto read = tool("read", "project_read", Json::object());
    if (!read.is_object() || !read.contains("head")) {
        helperSend({{"type", "ended"}, {"id", id}, {"reason", "fixture_read_failed"}});
        return 0;
    }
    const Json edit = {{"baseHead", read.at("head")}, {"request", "replaced by the host"},
                       {"ops", Json::array({{{"op", "track.rename"},
                                             {"payload", {{"id", 101}, {"name", "Agent name"}}}}})}};
    const auto proposed = tool("propose", "changeset_propose", edit);
    if (!proposed.is_object() || proposed.value("status", std::string{}) != "awaiting_review") {
        helperSend({{"type", "ended"}, {"id", id}, {"reason", "fixture_propose_failed"}});
        return 0;
    }
    helperSend({{"type", "answer"}, {"id", id}, {"text", "One change waits for your review"}});
    helperSend({{"type", "ended"}, {"id", id}, {"reason", "completed"}, {"steps", 2}});
    return 0;
}

/// Freshness on a hand-built history tree: the branches and the compacted root
/// that a live document makes slowly.
void testFreshnessOnTheTree() {
    const auto step = [](std::int64_t seq, std::optional<std::int64_t> parent, bool applied, bool line) {
        History::TimelineStep s;
        s.step.txnId = seq;
        s.step.firstSeq = seq;
        s.step.lastSeq = seq;
        s.parentSeq = parent;
        s.applied = applied;
        s.onCurrentLine = line;
        return s;
    };
    // root -> 1 -> 2 (the head); 3 branches from 1 and is not on the current line.
    History::Timeline tree;
    tree.headSeq = 2;
    tree.steps = {step(3, 1, false, false), step(2, 1, true, true), step(1, std::nullopt, true, true)};
    const auto is = [&](std::optional<std::int64_t> base, Freshness state, std::size_t steps) {
        const auto found = proposalFreshness(tree, base);
        return found.state == state && found.steps == steps;
    };
    require(is(2, Freshness::Current, 0) && is(1, Freshness::Behind, 1) && is(std::nullopt, Freshness::Behind, 2),
            "a base on the undo path is behind by the steps after it");
    require(is(3, Freshness::Elsewhere, 0) && is(99, Freshness::Elsewhere, 0),
            "a base on another branch, or unknown to the tree, is elsewhere");
    tree.headSeq = 1;
    tree.steps = {step(3, 1, false, false), step(2, 1, false, true), step(1, std::nullopt, true, true)};
    require(is(2, Freshness::Ahead, 1) && is(3, Freshness::Elsewhere, 0),
            "a base on the redo line is ahead; the other branch still elsewhere");
    tree.headSeq = 2;
    tree.baseSeq = 1;
    tree.steps = {step(2, 1, true, true)};
    require(is(1, Freshness::Behind, 1) && is(std::nullopt, Freshness::Elsewhere, 0),
            "past a compacted root, Undo reaches the root it kept and not the empty song");
    tree.error = "cannot read";
    require(is(2, Freshness::Unknown, 0), "a history that cannot be read says nothing");
}

/// AGT-05's engine side for the AI mode's cards (ADR-0261 d7, ADR-0262 d2):
/// what waits and why, its full list, how far the song moved since, that Undo
/// makes it current again, and Ask again on a fresh read. Then the applied
/// step's name in the Edit menu (AGT-12). Headless, through the owner seams.
void testReviewCards(const std::filesystem::path& executable) {
    test::TempDirectory temp("app_control", "cards");
    useTestHome(temp.path() / "home");
    std::string error;
    auto document = ui::ProjectDocument::open(temp.path() / "Cards.adi", true, {}, error);
    if (!document) throw std::runtime_error("open test document: " + error);
    const auto submit = [&](const char* type, Json payload) {
        OpRequest edit;
        edit.opType = type;
        edit.payload = std::move(payload);
        return document->ops().submit(std::move(edit)).ok;
    };
    require(submit("track.create", {{"id", 101}, {"kind", "midi"}, {"name", "Keys"}, {"index", 0}}),
            "give the cards' document one track");
    const auto uuid = document->store().projectUuid();

    OwnerTurns owner;
    AppControlOptions options;
    options.tier = Tier::Propose;
    AppControl app([&owner](Task task) { owner.post(std::move(task)); }, std::move(options));
    app.attach(document.get());
    require(app.liveProject() == uuid, "the window names the open project by the handle the cards take");
    AgentLaunch launch;
    launch.command.executable = executable;
    launch.command.arguments = {"--proposing-helper"};
    launch.provider.kind = AgentProviderKind::Faux;
    launch.provider.model = "fixture-model";
    require(app.checkAgent(launch, uuid).canAnswer(), "the proposing faux helper answers its check");
    require(app.agentReadiness(app.liveProject()).canAnswer(),
            "after its check, the readiness the AI label follows says the agent can answer");
    const auto run = owner.roundTrip([&] { return app.runAgent(launch, uuid, "Name the track"); });
    require(run.reason == "completed" && run.answer == "One change waits for your review",
            "the agent read the project and proposed one change");

    const auto cardsNow = [&] { return app.reviewCards(uuid); };
    auto cards = cardsNow();
    require(cards.size() == 1, "one card waits in the AI mode");
    const auto first = cards.front();
    require(first.request == "Name the track" && first.fromAdiAgent && first.client == "ADI agent" &&
                first.origin == RunOrigin::Direct && first.whyWaits == std::vector<std::string>{"tier"},
            "the card says what was asked, by whom, and that it waits because the agent may only propose");
    require(first.changeCount == 1 && first.changes.size() == 1 && !first.changesTruncated &&
                first.changes[0].at("op") == "track.rename" && first.changes[0].at("before").dump().find("Keys") !=
                std::string::npos && first.changes[0].at("after").dump().find("Agent name") != std::string::npos &&
                first.diff.find("Agent name") != std::string::npos,
            "the full list carries each change before and after, for the floating window");
    require(first.freshness.state == Freshness::Current && first.freshness.steps == 0,
            "a card made on the head is current");

    require(submit("track.rename", {{"id", 101}, {"name", "User one"}}) &&
                submit("track.rename", {{"id", 101}, {"name", "User two"}}),
            "the user goes on editing");
    cards = cardsNow();
    require(cards.size() == 1 && cards[0].freshness.state == Freshness::Behind && cards[0].freshness.steps == 2,
            "the card counts the two edits made after it");
    require(app.review(first.id, true).refusal == agent::Refusal::Stale &&
                document->view().committed()->findTrack(101)->name == "User two",
            "Apply refuses the moved head and writes nothing");
    require(document->ops().undo().ok && cardsNow().at(0).freshness.steps == 1 && document->ops().undo().ok,
            "each Undo brings the card one step closer");
    require(cardsNow().at(0).freshness.state == Freshness::Current, "undoing back makes the card current");
    require(document->ops().undo().ok && cardsNow().at(0).freshness.state == Freshness::Ahead &&
                cardsNow().at(0).freshness.steps == 1,
            "undoing past it says one Redo makes it current");
    require(document->ops().redo().ok && cardsNow().at(0).freshness.state == Freshness::Current,
            "and the Redo does");

    require(submit("track.rename", {{"id", 101}, {"name", "User three"}}) &&
                cardsNow().at(0).freshness.state == Freshness::Behind,
            "a new edit on a fresh branch moves the song on again");
    const auto again = owner.roundTrip([&] { return app.askAgain(launch, first.id); });
    require(again.reason == "completed", "Ask again runs the same request on a fresh read");
    cards = cardsNow();
    require(cards.size() == 1 && cards[0].id != first.id && cards[0].request == "Name the track" &&
                cards[0].freshness.state == Freshness::Current &&
                cards[0].changes[0].at("before").dump().find("User three") != std::string::npos,
            "the old card is gone and the new one is built on the song as it is now");
    const auto log = app.agentHistory(uuid);
    require(log && std::any_of(log->begin(), log->end(), [&](const AgentLogEntry& entry) {
                return entry.event.kind == AgentLogKind::ProposalOutcome && entry.event.id == first.id &&
                       entry.event.state == "rejected" && entry.event.actor == "user" &&
                       entry.event.source == "ask again";
            }), "the agent's log records that the user asked again in place of the old proposal");

    const auto second = cards[0].id;
    require(owner.roundTrip([&] { return app.askAgain(launch, second); }).reason == "current" &&
                cardsNow().size() == 1,
            "a current card is applied, not asked again");
    const auto applied = app.review(second, true);
    require(applied.ok && document->view().committed()->findTrack(101)->name == "Agent name",
            "Apply on the current card writes it");
    const auto step = document->ops().nextUndo();
    require(step && step->label == "Name the track" && step->actor == "agent",
            "the Edit menu's undo step is named from the request and says the agent made it (AGT-12)");
    require(owner.roundTrip([&] { return app.askAgain(launch, second); }).reason == "not_waiting" &&
                app.reviewCards(uuid).empty(),
            "an applied card cannot be asked again");
}

void testAppControl(const std::filesystem::path& executable) {
    test::TempDirectory temp("app_control", "live");
    useTestHome(temp.path() / "home");
    std::string error;
    auto document = ui::ProjectDocument::open(temp.path() / "Private project title.adi", true, {}, error);
    if (!document) throw std::runtime_error("open test document: " + error);
    OpRequest create;
    create.opType = "track.create";
    create.payload = {{"id", 101}, {"kind", "audio"}, {"name", "Private session track"}, {"index", 0}};
    require(document->ops().submit(std::move(create)).ok, "give the document one named track");
    const auto uuid = document->store().projectUuid();
    require(!uuid.empty(), "the document has a persisted identity");
    const auto beforeHead = OpJournal(document->store()).headSeq();
    const auto beforeGeneration = document->view().generation();

    OwnerTurns owner;
    AppControlOptions options;
    options.tier = Tier::Apply;
    AppControl app([&owner](Task task) { owner.post(std::move(task)); }, std::move(options));
    app.attach(document.get());
    ui::PluginScan scan;
    scan.listed = 1;
    scan.listing.plugins.push_back({"clap", "fixture.eq", "Catalogue Fixture EQ", "Fixture Vendor", "1.0",
                                    "/private/fixture.clap", ui::PluginEntry::Kind::Effect});
    const std::vector<preset::Entry> presets = {{preset::deviceKey({"clap", "fixture.eq", std::nullopt}),
                                                "Catalogue Fixture Preset", "/private/fixture.adipreset"}};
    app.setCataloguePlugins(scan);
    app.setCataloguePresets(presets);
    if (!app.start(error)) throw std::runtime_error("start app control: " + error);
    const auto session = app.session();
    require(session.has_value() && session->port != 0 && !session->token.empty(), "the running app offers pairing");
    Client client("ws://127.0.0.1:" + std::to_string(session->port) + "/mcp");
    client.set_bearer_token_auth(session->token);
    client.set_connection_timeout(2s);
    client.set_read_timeout(3s);
    require(owner.roundTrip([&] { return static_cast<bool>(client.connect()); }), "pair over real loopback");
    const auto call = [&](const Json& frame) { return owner.roundTrip([&] { return exchange(client, frame); }); };
    call(request(1, "initialize", {{"protocolVersion", "2025-11-25"}}));
    require(call(request(2, "resources/list", Json::object())).dump().find("Private project title") ==
                std::string::npos, "listing resources exposes no project content before consent");
    require(call(request(22, "resources/list", Json::object())).dump().find("adi://browser/catalog") !=
                std::string::npos, "MCP lists the live Browser catalogue resource");

    const Json search = {{"project", uuid}, {"query", "catalogue fixture"}};
    const auto unapproved = call(callTool(23, "browser_catalog", search, "catalogue"));
    require(needsConsent(unapproved) && unapproved.dump().find("Fixture EQ") == std::string::npos,
            "the external client needs consent before receiving any catalogue row");
    approveDisclosure(app, uuid, ConsentData::BrowserCatalogue);
    const auto catalogue = call(callTool(24, "browser_catalog", search, "catalogue")).at("structuredContent");
    require(catalogue.at("total") == 2 && catalogue.at("entries")[0].at("name") == "Catalogue Fixture EQ" &&
            catalogue.at("entries")[1].at("name") == "Catalogue Fixture Preset" &&
            catalogue.dump().find("/private/") == std::string::npos,
            "the approved search returns the shared plug-in and preset without their paths");
    require(needsConsent(call(callTool(25, "browser_catalog", search, "catalogue"))),
            "a second catalogue disclosure needs its own approval");
    require(app.takeConsentQuestions().size() == 1, "the second consent question can be dismissed");

    const Json read = {{"project", uuid}};
    require(needsConsent(call(callTool(3, "project_read", read, "read"))), "the external client must ask before reading");
    approveDisclosure(app, uuid, ConsentData::TrackNames);
    const auto projection = call(callTool(4, "project_read", read, "read")).at("structuredContent");
    require(projection.at("tracks").dump().find("Private session track") != std::string::npos,
            "the approved retry sees the committed track");

    Json args = {{"project", uuid}, {"baseHead", projection.at("head")}, {"request", "Rename the track"},
                 {"ops", Json::array({{{"op", "track.rename"}, {"payload", {{"id", 101}, {"name", "Renamed"}}}}})}};
    require(needsConsent(call(callTool(5, "changeset_propose", args, "rename"))),
            "disclosing a preview asks separately");
    require(app.proposals().empty(), "the refused preview did not queue an edit");
    approveDisclosure(app, uuid, ConsentData::ChangesetPreview);
    const auto proposal = call(callTool(6, "changeset_propose", args, "rename")).at("structuredContent");
    require(proposal.at("status") == "awaiting_review" && app.proposals().size() == 1,
            "the approved preview is queued for local review");
    require(document->view().generation() == beforeGeneration && OpJournal(document->store()).headSeq() == beforeHead,
            "queuing a remote edit leaves the document alone");

    const auto applied = app.review(proposal.at("id").get<std::string>(), true);
    require(applied.ok && applied.seqs.size() == 1 && app.proposals().empty(), "local review commits one transaction");
    require(document->view().generation() > beforeGeneration &&
            document->view().committed()->findTrack(101)->name == "Renamed", "the open document's view refreshed");

    const auto currentHead = OpJournal(document->store()).headSeq();
    require(currentHead.has_value(), "the approved change has a committed head");
    args["baseHead"] = *currentHead;
    args["ops"][0]["payload"]["name"] = "Rejected by Stop Agent";
    require(needsConsent(call(callTool(7, "changeset_propose", args, "old"))),
            "ask before disclosing a proposal that will be stopped");
    approveDisclosure(app, uuid, ConsentData::ChangesetPreview);
    const auto oldProposal = call(callTool(8, "changeset_propose", args, "old")).at("structuredContent");
    const auto oldId = oldProposal.at("id").get<std::string>();
    require(oldProposal.at("status") == "awaiting_review" && app.proposals().size() == 1,
            "Stop Agent has a proposal waiting for review");

    args["ops"][0]["payload"]["name"] = "Pending before Stop";
    const auto interruptedFrame = callTool(9, "changeset_propose", args, "interrupted");
    auto interrupted = std::async(std::launch::async, [&] { return exchange(client, interruptedFrame); });
    auto interruptedTurn = owner.takeFor(3s);
    require(static_cast<bool>(interruptedTurn), "the second proposal is already waiting on the owner thread");
    const auto stopped = app.stopAgent("test surface");
    require(app.agentStopped() && stopped.rejectedProposals == 1 && app.proposals().empty(),
            "Stop Agent rejects the proposal that was awaiting review");
    const auto log = app.agentHistory(uuid);
    require(log && log->size() == 5, "the project log has one entry for each proposal, outcome and Stop press");
    const auto& first = log->at(0).event;
    const auto& accepted = log->at(1).event;
    const auto& rejected = log->at(3).event;
    const auto& press = log->at(4).event;
    require(first.kind == AgentLogKind::Proposal && first.id == proposal.at("id").get_ref<const std::string&>() &&
            first.state == "awaiting_review" && accepted.kind == AgentLogKind::ProposalOutcome &&
            accepted.id == first.id && accepted.state == "applied" && accepted.actor == "user",
            "a proposal and its explicit Apply are recorded in order");
    require(rejected.kind == AgentLogKind::ProposalOutcome && rejected.id == oldId &&
            rejected.state == "rejected" && press.kind == AgentLogKind::StopPress &&
            press.actor == "user" && press.source == "test surface" &&
            Json::parse(press.text).at("rejectedProposals") == Json::array({oldId}),
            "the Stop press records its surface and the proposal it rejected");
    app.rearmAgent();
    require(!app.agentStopped() && !app.review(oldId, true).ok,
            "re-arming cannot make the rejected proposal reviewable");
    interruptedTurn();
    require(toolError(interrupted.get()) == errc::AgentStopped && app.takeConsentQuestions().empty(),
            "an owner turn queued before Stop is refused even after re-arming");
    require(OpJournal(document->store()).headSeq() == currentHead,
            "stopping the agent and refusing its in-flight edit write nothing");

    (void)app.stopAgent("test surface");
    const Json statusArgs = {{"id", oldId}};
    require(needsConsent(call(callTool(10, "changeset_status", statusArgs, "stopped status"))),
            "the stopped agent can ask to read its proposal status");
    approveDisclosure(app, uuid, ConsentData::ChangesetPreview);
    require(call(callTool(11, "changeset_status", statusArgs, "stopped status"))
                .at("structuredContent").at("status") == "rejected",
            "the stopped agent can read the recorded rejection");
    require(needsConsent(call(callTool(12, "project_read", read, "stopped read"))),
            "the stopped agent can ask to read the project");
    approveDisclosure(app, uuid, ConsentData::TrackNames);
    require(call(callTool(13, "project_read", read, "stopped read"))
                .at("structuredContent").at("head") == *currentHead,
            "project reads still work while stopped");
    require(toolError(call(callTool(14, "changeset_propose", args, "while stopped"))) == errc::AgentStopped &&
            app.takeConsentQuestions().empty() && app.proposals().empty(),
            "new agent writes are also refused while stopped");

    const auto copyPath = temp.path() / "Copy with the same identity.adi";
    require(document->store().saveCopy(copyPath, StoreFileCollision::Fail, StoreCopyPolicy::preserve).ok,
            "copy the file with its original project UUID");
    std::string copyError;
    auto copy = ui::ProjectDocument::open(copyPath, false, {}, copyError);
    if (!copy) throw std::runtime_error("open preserved copy: " + copyError);
    require(copy->store().projectUuid() == uuid, "the preserved copy retains the UUID");
    OpRequest copyEdit;
    copyEdit.opType = "track.rename";
    copyEdit.payload = {{"id", 101}, {"name", "Independent copy"}};
    require(copy->ops().submit(std::move(copyEdit)).ok, "give the preserved copy a different edit");

    app.rearmAgent();
    args["ops"][0]["payload"]["name"] = "Fresh after rearm";
    require(needsConsent(call(callTool(15, "changeset_propose", args, "fresh"))),
            "a fresh request proceeds to the consent gate after re-arming");
    approveDisclosure(app, uuid, ConsentData::ChangesetPreview);
    const auto fresh = call(callTool(16, "changeset_propose", args, "fresh")).at("structuredContent");
    require(fresh.at("status") == "awaiting_review" && app.review(fresh.at("id").get<std::string>(), true).ok &&
            document->view().committed()->findTrack(101)->name == "Fresh after rearm",
            "a fresh proposal can be applied after re-arming");

    const auto beforeShutdown = OpJournal(document->store()).headSeq();
    require(beforeShutdown.has_value(), "the fresh change moved the committed head");
    require(OpJournal(copy->store()).headSeq() == beforeShutdown,
            "the preserved copy can diverge and still have the same numeric head");
    args["baseHead"] = *beforeShutdown;
    args["ops"][0]["payload"]["name"] = "Original file only";
    require(needsConsent(call(callTool(31, "changeset_propose", args, "original only"))),
            "the external caller asks to preview another edit in the original");
    approveDisclosure(app, uuid, ConsentData::ChangesetPreview);
    const auto originalOnly = call(callTool(32, "changeset_propose", args, "original only")).at("structuredContent");
    const auto originalId = originalOnly.at("id").get<std::string>();

    AgentLaunch launch;
    launch.command.executable = executable;
    launch.command.arguments = {"--binding-helper"};
    launch.provider.kind = AgentProviderKind::Faux;
    launch.provider.model = "fixture-model";
    require(app.checkAgent(launch, uuid).canAnswer(), "the self-launched faux helper answers its check");
    const auto headText = dumpJson(Json(*beforeShutdown));
    auto run = std::async(std::launch::async, [&] { return app.runAgent(launch, uuid, headText); });
    auto originalTurn = owner.takeFor(3s);
    require(static_cast<bool>(originalTurn), "the helper's proposal is waiting on the owner thread");
    app.attach(copy.get());

    require(app.review(originalId, true).refusal == agent::Refusal::ReadOnly &&
            OpJournal(copy->store()).headSeq() == beforeShutdown &&
            copy->view().committed()->findTrack(101)->name == "Independent copy",
            "a proposal can never be approved against another attachment at the same UUID and head");
    require(toolError(call(callTool(33, "changeset_status", {{"id", originalId}}, "original status"))) ==
                errc::InvalidParams &&
            toolError(call(callTool(34, "changeset_propose", args, "original only"))) == errc::InvalidParams &&
            app.takeConsentQuestions().empty(),
            "status and idempotent replay cannot substitute the preserved copy");
    originalTurn();
    const auto finished = run.get();
    require(finished.reason == "completed" && finished.answer == "The original project closed" &&
            OpJournal(copy->store()).headSeq() == beforeShutdown &&
            copy->view().committed()->findTrack(101)->name == "Independent copy",
            "the parked helper cannot propose, read the project or read its catalogue against the copy");
    require(app.review(originalId, false).ok, "local review can still discard the closed attachment's proposal");
    app.attach(document.get());

    args["ops"][0]["payload"]["name"] = "Must not run";
    const auto pendingFrame = callTool(17, "changeset_propose", args, "pending");
    auto remote = std::async(std::launch::async, [&] {
        require(client.send(dumpJson(pendingFrame)), "send another proposal before stopping");
        std::string frame;
        (void)client.read(frame);
    });
    auto pending = owner.takeFor(3s);
    require(static_cast<bool>(pending), "the last proposal is waiting for its owner turn");
    const auto stopping = std::chrono::steady_clock::now();
    app.stop();
    require(std::chrono::steady_clock::now() - stopping < 4s && !app.session(),
            "stop interrupts a client waiting for its owner turn");
    remote.get();
    pending();
    require(app.takeConsentQuestions().empty() && app.proposals().empty() &&
            OpJournal(document->store()).headSeq() == beforeShutdown, "the abandoned owner task did not run");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--binding-helper") return bindingHelper();
        if (argc == 2 && std::string_view(argv[1]) == "--proposing-helper") return proposingHelper();
        testAppControl(std::filesystem::absolute(argv[0]));
        testFreshnessOnTheTree();
        testReviewCards(std::filesystem::absolute(argv[0]));
        std::puts("PASS -- loopback app control, consent, review, Stop Agent and shutdown");
        return 0;
    } catch (const std::exception& exception) {
        std::printf("FAIL -- %s\n", exception.what());
        return 1;
    }
}
