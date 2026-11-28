// SPDX-License-Identifier: AGPL-3.0-or-later
// DEV-27: the consent ledger issues the plug-in server's hosting lease. A
// device streams only while its lease holds; the lease ends at its term's
// expiry, on revocation by the owner, and when another process removes the
// grant, also in the middle of a stream.
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/control/consent.hpp"
#include "adi/control/consent_scope.hpp"
#include "adi/engine/offline_render_scope.hpp"
#include "adi/store.hpp"
#include "juce/net_plugin_hosting.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace adi;
namespace fs = std::filesystem;

constexpr std::int64_t kStart = 1795780000;
constexpr std::int64_t kHour = 3600;
int checks = 0, failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

struct Processor final : device::RemoteProcessor {
    std::atomic<int>& packets;
    explicit Processor(std::atomic<int>& blocks) : packets(blocks) {}
    bool prepare(double, std::int32_t, std::int32_t, bool, std::int32_t& latency, std::string&) override {
        latency = 0;
        return true;
    }
    bool exchange(const device::RemoteBlock&, std::span<float> output, std::string&) override {
        ++packets;
        std::fill(output.begin(), output.end(), 0.25f);
        return true;
    }
};

struct Fixture {
    test::TempDirectory dir{"plugin_hosting", "leases"};
    fs::path ledgerFile = dir.path() / "control" / "consent.json";
    std::unique_ptr<Store> store;
    std::atomic<std::int64_t> clock{kStart};
    std::atomic<int> packets{0};
    int factories = 0;
    net::PluginHostingLeases leases{control::ConsentLedger(ledgerFile), [this] { return clock.load(); }};
    engine::DeviceLoader loader;
    rows::Device row;
    rows::PluginRef ref;
    engine::DeviceRequest request;
    const net::PluginServerAddress expected{"127.0.0.1", 55056};

    Fixture() {
        StoreError error{};
        store = Store::create(dir.path() / "song.adi", error);
        if (!store) throw std::runtime_error("cannot create the hosting test project");
        store->db().exec("INSERT INTO remote_hosts(id,name,host,port,enabled) VALUES(7,'Room','127.0.0.1',55056,1);");
        row.remoteHostId = 7;
        ref.format = "vst3";
        ref.name = "Remote test";
        ref.uid = "157c0f3844664a65b03c829f28b96435";
        request.device = &row;
        request.ref = &ref;
        request.project = store.get();
        request.maxFrames = 64;
        request.offline = true;
        loader = net::makePluginServerLoader(
            "adi-daw",
            [this](const net::PluginServerLocation& location, const device::DeviceIdentity& identity)
                -> std::optional<net::PluginServerBinding> {
                if (location.address != expected || identity.uid != ref.uid) return {};
                return net::PluginServerBinding{"private-test", "paired-room", [this](const net::PluginServerAddress&) {
                    ++factories;
                    return std::make_unique<Processor>(packets);
                }};
            },
            leases.authorizer());
    }

    std::unique_ptr<device::DeviceInstance> load(std::string& error) { return loader(request, error); }

    // One offline block: the worker exchanges it synchronously. True when it reached the server.
    bool stream(device::DeviceInstance& remote) {
        std::array<float, 64> left{}, right{};
        std::array<float*, 2> output{left.data(), right.data()};
        engine::NodeIo io;
        io.channels = 2;
        io.frames = 64;
        io.out = output.data();
        const auto before = packets.load();
        {
            const engine::OfflineRenderScope scope;
            remote.process(io);
        }
        return packets.load() == before + 1;
    }

    // Refuse once and return the question the refusal asked.
    std::optional<net::PluginHostingQuestion> ask() {
        std::string error;
        const auto made = factories;
        auto refused = load(error);
        auto questions = leases.takeQuestions();
        if (refused || factories != made || questions.size() != 1 || error.find("consent") == std::string::npos)
            return {};
        return questions.front();
    }
};

void run() {
    Fixture f;
    if (control::projectFileConsentBinding(*f.store).empty()) {
#if defined(_WIN32)
        // NTFS always gives a file a durable identity: the skip below would hide the whole test.
        check(false, "a Windows project file has a durable consent binding");
#endif
        std::string error;
        check(!f.load(error) && f.factories == 0 && f.leases.takeQuestions().empty(),
              "a file with no durable binding is refused and asks nothing it could not grant");
        return;
    }

    std::puts("[a refusal asks one question and opens no connection]");
    auto question = f.ask();
    check(question.has_value(), "the first load is refused for consent and asks once");
    if (!question) return;
    check(question->scope.projectUuid == f.store->projectUuid() && question->scope.connectionId == "private-test" &&
          question->scope.serverFingerprint == "paired-room" && question->scope.address == f.expected &&
          question->scope.audioBytesPerBlock == 512 &&
          question->request.capability == control::ConsentCapability::PluginHosting &&
          question->request.data.size() == 4,
          "the question names the project, connection, server, address and per-block audio");
    check(f.leases.takeQuestions().empty(), "a question is handed over once");
    {
        std::string error;
        (void)f.load(error);
        (void)f.load(error);
        check(f.leases.takeQuestions().size() == 1, "repeated refusals of one scope wait as one question");
    }

    std::puts("[a one-hour grant streams until its hour ends, mid-stream]");
    check(f.leases.answer(*question, control::ConsentTerm::OneHour), "the answer is written to the ledger");
    {
        std::string error;
        auto remote = f.load(error);
        check(remote && remote->loaded() && f.factories == 1 && f.leases.live() == 1,
              "the granted load admits the server and publishes one lease");
        if (!remote) return;
        check(f.stream(*remote), "a block reaches the server while the hour holds");
        f.clock += kHour - 1;
        check(f.stream(*remote), "a block one second before the expiry still reaches it");
        f.clock += 1;
        check(!f.stream(*remote) && remote->processingIssue().find("permission") != std::string_view::npos,
              "the expired lease stops the stream at the worker before a byte leaves");
        check(f.leases.live() == 0, "an expired lease is no longer live");
    }
    check(f.leases.live() == 0, "a released device holds no lease");
    question = f.ask();
    check(question.has_value(), "after the hour the ledger asks again");
    if (!question) return;

    std::puts("[a standing grant renewed while streaming moves the expiry]");
    check(f.leases.answer(*question, control::ConsentTerm::OneHour), "grant another hour");
    {
        std::string error;
        auto remote = f.load(error);
        check(remote && remote->loaded(), "the renewed grant admits the device");
        if (!remote) return;
        f.clock += kHour / 2;
        check(f.leases.answer(*question, control::ConsentTerm::OneDay), "extend to a day while streaming");
        check(f.leases.refresh() == 0, "a renewed grant ends nothing");
        f.clock += kHour;
        check(f.stream(*remote), "the stream outlives the first hour once the day was granted");
    }

    std::puts("[another process removes the grant: refresh ends the live stream]");
    check(f.leases.answer(*question, control::ConsentTerm::UntilRevoked), "grant until revoked");
    {
        std::string error;
        auto remote = f.load(error);
        check(remote && remote->loaded(), "the standing grant admits the device");
        if (!remote) return;
        check(f.stream(*remote), "the standing grant streams");
        control::ConsentLedger elsewhere(f.ledgerFile);
        check(elsewhere.revoke(question->request), "another process revokes the grant in the ledger");
        check(f.leases.refresh() == 1, "refresh ends the one lease whose grant is gone");
        check(!f.stream(*remote) && remote->processingIssue().find("permission") != std::string_view::npos,
              "the revoked lease stops the stream before a byte leaves");
    }

    std::puts("[the owner's revoke ends the stream and removes the grant]");
    question = f.ask();
    check(question.has_value() && f.leases.answer(*question, control::ConsentTerm::UntilRevoked),
          "grant again until revoked");
    if (!question) return;
    {
        std::string error;
        auto remote = f.load(error);
        check(remote && remote->loaded() && f.stream(*remote), "the new grant streams");
        if (!remote) return;
        check(f.leases.revoke(f.store->projectUuid(), "other-connection") == 0 && f.stream(*remote),
              "revoking another connection leaves this stream alone");
        check(f.leases.revoke(f.store->projectUuid(), "private-test") == 1, "revoke ends the one live lease");
        check(!f.stream(*remote), "the revoked stream sends nothing more");
        check(control::ConsentLedger(f.ledgerFile).standing(question->request, f.clock.load()).decision ==
                  control::ConsentDecision::ConsentRequired,
              "the owner's revoke also removed the standing grant from the ledger");
    }

    std::puts("[a This-time grant admits one load, for that device's life]");
    question = f.ask();
    check(question.has_value() && f.leases.answer(*question, control::ConsentTerm::ThisTime), "grant this time");
    if (!question) return;
    {
        std::string error;
        auto remote = f.load(error);
        check(remote && remote->loaded(), "this time admits the load");
        if (!remote) return;
        const auto second = f.ask();
        check(second.has_value(), "a second load asks again: the one-time grant was spent");
        check(f.leases.refresh() == 0, "refresh does not end a This-time lease");
        f.clock += 30 * 24 * kHour;
        check(f.stream(*remote), "a This-time lease lasts as long as its device");
        check(f.leases.revoke(f.store->projectUuid(), "private-test") == 1 && !f.stream(*remote),
              "the owner can still end a This-time stream");
    }

    std::puts("[the audio thread never consults the ledger]");
    {
        const auto authorize = f.leases.authorizer();
        net::PluginHostingScope scope = question->scope;
        net::PluginHostingLease lease;
        {
            const audio::CallbackScope callback;
            lease = authorize(*f.store, scope);
        }
        check(!lease && f.leases.takeQuestions().empty(), "an audio-thread request gets no lease and asks nothing");
    }
}
} // namespace

int main() {
    try {
        run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::printf("%s -- %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures == 0 ? 0 : 1;
}
