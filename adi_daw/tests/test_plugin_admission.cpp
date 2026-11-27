// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/engine/offline_render_scope.hpp"
#include "adi/store.hpp"
#include "juce/net_plugin_admission.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {
using namespace adi;
int checks = 0, failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

struct Processor final : device::RemoteProcessor {
    std::atomic<int>& preparations;
    std::atomic<int>& packets;

    Processor(std::atomic<int>& setups, std::atomic<int>& blocks) : preparations(setups), packets(blocks) {}
    bool prepare(double, std::int32_t, std::int32_t, bool, std::int32_t& latency, std::string&) override {
        ++preparations;
        latency = 0;
        return true;
    }
    bool exchange(const device::RemoteBlock&, std::span<float> output, std::string&) override {
        ++packets;
        std::fill(output.begin(), output.end(), 0.25f);
        return true;
    }
};

std::unique_ptr<Store> project(const std::filesystem::path& path, const std::string& uuid) {
    StoreError error{};
    auto store = Store::createEmpty(path, error);
    if (!store) throw std::runtime_error("cannot create admission test project");
    store->db().exec("INSERT INTO remote_hosts(id,name,host,port,enabled)"
                    " VALUES(7,'Room','127.0.0.1',55056,1);");
    if (!uuid.empty()) {
        SQLite::Statement identity(store->db(), "UPDATE adi_meta SET value=? WHERE key='project_uuid'");
        identity.bind(1, uuid);
        identity.exec();
    }
    return store;
}

void run() {
    test::TempDirectory dir("plugin_admission", "scopes");
    auto original = project(dir.path() / "one.adi", "project-one");
    auto other = project(dir.path() / "two.adi", "project-two");
    auto old = project(dir.path() / "old.adi", "");
    rows::Device row;
    row.remoteHostId = 7;
    rows::PluginRef ref;
    ref.format = "vst3";
    ref.name = "Remote test";
    ref.uid = "157c0f3844664a65b03c829f28b96435";
    engine::DeviceRequest request;
    request.device = &row;
    request.ref = &ref;
    request.project = original.get();
    request.maxFrames = 64;
    request.offline = true;

    std::atomic<bool> valid{false};
    std::atomic<int> preparations{0}, packets{0};
    int factories = 0, authorizations = 0;
    bool matchId = false;
    const net::PluginServerAddress expected{"127.0.0.1", 55056};
    const auto loader = net::makePluginServerLoader(
        "adi-daw",
        [&](const net::PluginServerLocation& location, const device::DeviceIdentity& identity)
            -> std::optional<net::PluginServerBinding> {
            matchId = location.address == expected && location.protocol == "audiogridder" &&
                      identity.format == "vst3" && identity.uid == ref.uid;
            if (!matchId) return {};
            return net::PluginServerBinding{"private-test", "paired-room", [&](const net::PluginServerAddress& address) {
                check(address == expected, "the bound factory receives the authorized address");
                ++factories;
                return std::make_unique<Processor>(preparations, packets);
            }};
        },
        [&](const net::PluginHostingScope& scope) -> net::PluginHostingLease {
            ++authorizations;
            if (scope.app != "adi-daw" || scope.projectUuid != "project-one" ||
                scope.connectionId != "private-test" || scope.serverFingerprint != "paired-room" ||
                scope.address != expected || scope.audioBytesPerBlock != 512) return {};
            return [&] { return valid.load(); };
        });
    std::string error;
    auto refused = loader(request, error);
    check(!refused && matchId && factories == 0 && authorizations == 1 && error.find("Room") != std::string::npos,
          "a project-enabled server row never grants a connection");

    request.project = old.get();
    refused = loader(request, error);
    check(!refused && authorizations == 1 && factories == 0,
          "projects awaiting a durable identity ask no ledger and make no connection");

    valid.store(true);
    request.project = other.get();
    refused = loader(request, error);
    check(!refused && factories == 0, "the other project cannot use the first project's grant");

    request.project = original.get();
    auto remote = loader(request, error);
    check(remote && remote->loaded() && preparations.load() == 1 && factories == 1,
          "a valid local grant admits the exact destination and starts the worker");
    if (!remote) return;
    std::array<float,64> left{}, right{};
    std::array<float*,2> output{left.data(), right.data()};
    engine::NodeIo io;
    io.channels = 2;
    io.frames = 64;
    io.out = output.data();
    {
        const engine::OfflineRenderScope scope;
        remote->process(io);
        check(packets.load() == 1, "the worker sent a block while the grant was current");
        valid.store(false);
        remote->process(io);
    }
    check(packets.load() == 1 && remote->processingIssue().find("permission") != std::string_view::npos,
          "revocation stops audio at the worker before further bytes leave");

    valid.store(true);
    const auto id = ref.uid;
    ref.uid = "VST3-12ab";
    refused = loader(request, error);
    check(!refused && factories == 1 && error.find("full class") != std::string::npos,
          "a stock lookup token cannot be a project or grant identity");
    ref.uid = id;
    original->db().exec("UPDATE remote_hosts SET port=55057 WHERE id=7;");
    refused = loader(request, error);
    check(!refused && !matchId && factories == 1, "remapping the row requires a new destination binding");

    const auto attempts = authorizations;
    {
        const audio::CallbackScope callback;
        refused = loader(request, error);
    }
    check(!refused && authorizations == attempts && factories == 1,
          "audio callbacks cannot consult the ledger or create a worker");
}
} // namespace

int main() {
    try {
        run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
