// SPDX-License-Identifier: AGPL-3.0-or-later
// DEV-27: the consent ledger issues the plug-in server's hosting lease. A
// device streams only while its lease holds; the lease ends at its term's
// expiry, on revocation by the owner, and when another process removes the
// grant, also in the middle of a stream.
#include "juce/net_plugin_socket.hpp" // first: winsock2 before anything that may bring windows.h
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/control/consent.hpp"
#include "adi/control/consent_file.hpp"
#include "adi/control/consent_scope.hpp"
#include "adi/engine/offline_render_scope.hpp"
#include "adi/engine/remote_devices.hpp"
#include "adi/store.hpp"
#include "juce/net_plugin_hosting.hpp"
#include "juce/net_plugin_owner.hpp"
#include "juce/net_plugin_pairing.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

// The pairing store: the local user's records, owner-only, failing closed.
void pairings() {
    std::puts("[the pairing store records, replaces, removes and fails closed]");
    test::TempDirectory dir{"plugin_hosting", "pairings"};
    const net::PluginServerPairings store(fs::absolute(dir.path()) / "plugin-servers" / "pairings.json");
    const net::PluginServerAddress room{"127.0.0.1", 55056};
    std::string error;
    const auto none = store.list(error);
    check(none && none->empty() && !store.find(room, "adps-v1"), "a missing file pairs nothing");

    const auto made = store.pair({room, "adps-v1", {}, "room-fp"}, error);
    check(made && made->connectionId.size() == 37 && made->connectionId.starts_with("pair-"),
          "an empty connection id is made when the server is paired");
    const auto lookup = store.lookup();
    const auto paired = lookup(room, "adps-v1");
    check(paired && made && paired->connectionId == made->connectionId && paired->serverFingerprint == "room-fp",
          "the lookup returns the pairing of that exact address and protocol");
    check(!lookup(room, "audiogridder") && !lookup({"127.0.0.1", 55057}, "adps-v1"),
          "another protocol or port at the same host is not paired");

    check(store.pair({room, "adps-v1", "second", "room-fp-2"}, error).has_value() &&
              store.pair({room, "audiogridder", "stock", "room-stock"}, error).has_value(),
          "a second pairing of the same endpoint and a second protocol are recorded");
    const auto two = store.list(error);
    const auto replaced = store.find(room, "adps-v1");
    check(two && two->size() == 2 && replaced && replaced->connectionId == "second",
          "pairing an endpoint again replaces its record");
    check(!store.pair({room, "http", "c", "f"}, error) && !error.empty(), "an unknown protocol is refused by name");
    const auto unchanged = store.list(error);
    check(unchanged && unchanged->size() == 2, "a refused pairing changes nothing");
    check(!store.pair({{"bad host", 1}, "adps-v1", "c", "f"}, error) && !store.pair({room, "adps-v1", "c", ""}, error),
          "a malformed host or an empty fingerprint is refused");

    const net::PluginServerPairings elsewhere(store.file());
    check(elsewhere.unpair(room, "audiogridder", error) && !elsewhere.unpair(room, "audiogridder", error) &&
              !lookup(room, "audiogridder") && lookup(room, "adps-v1"),
          "a pairing removed by another owner is gone at the next lookup");
    {
        const audio::CallbackScope callback;
        check(!lookup(room, "adps-v1") && !store.pair({room, "adps-v1", "c", "f"}, error),
              "the audio thread neither reads nor writes pairings");
    }

    check(control::detail::writeConsentFile(store.file(), "{\"version\":1,\"pairings\":[{\"host\":7}]}"),
          "write an invalid pairing file");
    check(!store.list(error) && !error.empty() && !lookup(room, "adps-v1"), "an invalid file pairs nothing");
    check(!store.pair({room, "adps-v1", "c", "f"}, error) && error.find("cannot be read") != std::string::npos &&
              control::detail::readConsentFile(store.file(), 4096) ==
                  std::optional<std::string>("{\"version\":1,\"pairings\":[{\"host\":7}]}"),
          "an invalid file is never overwritten by a new pairing");
}

// One loopback listener that accepts every connection and closes it at once:
// a device that reaches it fails its activation and the test counts the attempt.
class Listener {
public:
    Listener() {
        using namespace net::plugin_socket;
        handle_ = createSocket();
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        auto length = static_cast<SocketLength>(sizeof(address));
        if (handle_ == invalidSocket ||
            ::bind(handle_, reinterpret_cast<const sockaddr*>(&address), length) != 0 || ::listen(handle_, 8) != 0 ||
            ::getsockname(handle_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            throw std::runtime_error("cannot listen on loopback");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    ~Listener() {
        stop_ = true;
        thread_.join();
        net::plugin_socket::closeSocket(handle_);
    }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] int accepted() const noexcept { return accepted_.load(); }
    // The worker connects before its load returns; the accept may trail by one poll.
    [[nodiscard]] bool awaitAccepted(int count) const {
        for (int wait = 0; wait < 200 && accepted_.load() < count; ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return accepted_.load() == count;
    }

private:
    void serve() {
        using namespace net::plugin_socket;
        while (!stop_) {
            SocketPollFd descriptor{handle_, POLLIN, 0};
            if (socketPoll(descriptor, 20) <= 0 || !(descriptor.revents & POLLIN)) continue;
            const auto peer = ::accept(handle_, nullptr, nullptr);
            if (peer == invalidSocket) continue;
            ++accepted_;
            closeSocket(peer);
        }
    }

    net::plugin_socket::SocketHandle handle_ = net::plugin_socket::invalidSocket;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> accepted_{0};
    std::thread thread_;
};

// The app's owner composes pairing, catalogue, ledger and loader: nothing
// connects until all four agree, and then only to the paired address.
void owner() {
    std::puts("[the app's hosting owner connects only through pairing, catalogue and consent]");
#if defined(_WIN32)
    static const net::plugin_socket::SocketRuntime runtime;
#endif
    test::TempDirectory dir{"plugin_hosting", "owner"};
    const auto root = fs::absolute(dir.path());
    const Listener server;
    const net::PluginServerAddress address{"127.0.0.1", server.port()};
    net::PluginServerHosting hosting("adi-daw", {root / "consent" / "ledger.json", root / "cache" / "servers.sqlite",
                                                root / "config" / "plugin-servers" / "pairings.json"},
                                     [] { return kStart; });
    StoreError storeError{};
    auto store = Store::create(root / "song.adi", storeError);
    if (!store) throw std::runtime_error("cannot create the owner test project");
    if (control::projectFileConsentBinding(*store).empty()) {
#if defined(_WIN32)
        check(false, "the owner test project has a durable consent binding");
#else
        std::puts("  (skipped: this file system gives the project no durable binding)");
#endif
        return;
    }
    store->db().exec("INSERT INTO remote_hosts(id,name,host,port,protocol,enabled) VALUES(7,'Room','127.0.0.1'," +
                     std::to_string(address.port) + ",'adps-v1',1);");
    rows::Device row;
    row.remoteHostId = 7;
    rows::PluginRef ref;
    ref.format = "clap";
    ref.uid = "org.adi.test.remote";
    ref.name = "Remote test";
    engine::DeviceRequest request;
    request.device = &row;
    request.ref = &ref;
    request.project = store.get();
    request.maxFrames = 64;
    request.offline = true;
    const auto remote = hosting.loader();
    int localLoads = 0;
    const auto composed = engine::withRemoteDevices(
        [&](const engine::DeviceRequest&, std::string& error) -> std::unique_ptr<device::DeviceInstance> {
            ++localLoads;
            error = "not installed here";
            return {};
        },
        hosting.loader());
    const auto refused = [&](std::string_view expected) {
        std::string error;
        const auto made = remote(request, error);
        return !made && error.find(expected) != std::string::npos && server.accepted() == 0;
    };

    check(refused("no paired endpoint") && hosting.leases().takeQuestions().empty(),
          "an unpaired server is refused by name before the ledger or a socket");
    std::string error;
    check(hosting.pairings().pair({address, "audiogridder", "stock", "room-stock"}, error).has_value() &&
              refused("no paired endpoint"),
          "a pairing for another protocol does not pair this one");
    const auto paired = hosting.pairings().pair({address, "adps-v1", {}, "room-fp"}, error);
    check(paired && refused("no paired endpoint"), "a paired server with no cached plug-in list is refused");
    if (!paired) return;
    check(hosting.catalogue().refresh({store->projectUuid(), "room-fp", address}, [](const auto&) { return true; },
              [&](const auto&, std::string&) -> std::optional<std::vector<net::ServerPlugin>> {
                  return std::vector<net::ServerPlugin>{{"clap", ref.uid, ref.name, ref.uid, false}};
              }, error),
          "cache the paired server's plug-in list");
    check(refused("consent is required"), "a paired, catalogued plug-in still waits for consent");
    const auto questions = hosting.leases().takeQuestions();
    check(questions.size() == 1 && questions.front().scope.connectionId == paired->connectionId &&
              questions.front().scope.serverFingerprint == "room-fp" && questions.front().scope.address == address &&
              questions.front().scope.app == "adi-daw",
          "the question names the paired connection, server fingerprint and address");
    if (questions.size() != 1) return;

    check(hosting.leases().answer(questions.front(), control::ConsentTerm::ThisTime), "grant this time");
    {
        const auto before = localLoads;
        const auto made = composed(request, error);
        check(!made && server.awaitAccepted(1) && localLoads == before && error.starts_with("server #7: Room: "),
              "the granted offline load connects to the paired address once and fails by name, never locally");
    }
    {
        request.offline = false;
        const auto before = localLoads;
        const auto made = composed(request, error);
        check(!made && localLoads == before + 1 && error.find("consent is required") != std::string::npos &&
                  error.find("local plug-in unavailable") != std::string::npos && server.accepted() == 1,
              "a live load whose one-time grant was spent tries the local plug-in instead");
        request.offline = true;
    }
    check(hosting.refresh() == 0 && hosting.leases().live() == 0, "refresh with no live lease ends nothing");

    const net::PluginServerPairings elsewhere(hosting.files().pairings);
    check(elsewhere.unpair(address, "adps-v1", error), "another owner removes the pairing");
    (void)hosting.leases().takeQuestions();
    {
        std::string unpairedError;
        check(!remote(request, unpairedError) && unpairedError.find("no paired endpoint") != std::string::npos &&
                  server.accepted() == 1 && hosting.leases().takeQuestions().empty(),
              "after unpairing nothing connects and nothing is asked");
    }
}
} // namespace

int main() {
    try {
        run();
        pairings();
        owner();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::printf("%s -- %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures == 0 ? 0 : 1;
}
