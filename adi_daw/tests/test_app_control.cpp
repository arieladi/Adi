// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/mcp.hpp"
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

bool needsConsent(const Json& reply) {
    return reply.value("isError", false) &&
           reply.at("structuredContent").at("error").at("code") == errc::ConsentRequired;
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

void testAppControl() {
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
    args["ops"][0]["payload"]["name"] = "Must not run";
    const auto pendingFrame = callTool(7, "changeset_propose", args, "pending");
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
            OpJournal(document->store()).headSeq() == currentHead, "the abandoned owner task did not run");
}

}  // namespace

int main() {
    try {
        testAppControl();
        std::puts("PASS -- loopback app control, consent, review, refresh and shutdown");
        return 0;
    } catch (const std::exception& exception) {
        std::printf("FAIL -- %s\n", exception.what());
        return 1;
    }
}
