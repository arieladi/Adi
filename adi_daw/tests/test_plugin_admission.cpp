// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/engine/offline_render_scope.hpp"
#include "adi/engine/remote_devices.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/store.hpp"
#include "juce/net_plugin_admission.hpp"
#include "juce/net_plugin_server.hpp"

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
    std::atomic<int>& parameterWrites;

    Processor(std::atomic<int>& setups, std::atomic<int>& blocks, std::atomic<int>& writes)
        : preparations(setups), packets(blocks), parameterWrites(writes) {}
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
    bool readParameters(std::vector<device::RemoteParameter>& parameters, StateDeadline, std::string&) override {
        device::RemoteParameter gain;
        gain.selector = 3;
        gain.descriptor.id = "17";
        gain.descriptor.name = "Gain";
        parameters = {gain};
        return true;
    }
    bool setParameter(std::uint32_t selector, double normalized, double& applied, StateDeadline,
                      std::string&) override {
        if (selector != 3) return false;
        ++parameterWrites;
        applied = normalized;
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
    const auto legacyPath = dir.path() / "old.adi";
    {
        auto legacyFile = project(legacyPath, "");
        legacyFile->db().exec("DELETE FROM adi_meta WHERE key='project_uuid'");
        if (!legacyFile->close()) throw std::runtime_error("cannot close the legacy admission fixture");
    }
    // A writable open assigns an identity. A legacy file opened read-only
    // still has none and must never seek a hosting grant.
    StoreError legacyError{};
    const auto legacy = Store::open(legacyPath, legacyError, /*readOnly=*/true);
    if (!legacy || !legacy->projectUuid().empty())
        throw std::runtime_error("cannot open a legacy admission fixture without a project identity");
    std::error_code identityError;
    const auto originalFile = media::readFileIdentity(original->path(), identityError);
    if (!originalFile) throw std::runtime_error("cannot identify the original admission test project");
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
    std::atomic<int> preparations{0}, packets{0}, parameterWrites{0};
    int resolutions = 0, authorizations = 0, factories = 0;
    bool matchId = false;
    const net::PluginServerAddress expected{"127.0.0.1", 55056};
    const auto loader = net::makePluginServerLoader(
        "adi-daw",
        [&](const net::PluginServerLocation& location, const device::DeviceIdentity& identity)
            -> std::optional<net::PluginServerBinding> {
            ++resolutions;
            matchId = location.address == expected && location.protocol == "audiogridder" &&
                      identity.format == "vst3" && identity.uid == ref.uid;
            if (!matchId) return {};
            return net::PluginServerBinding{"private-test", "paired-room", [&](const net::PluginServerAddress& address) {
                check(address == expected, "the bound factory receives the authorized address");
                ++factories;
                return std::make_unique<Processor>(preparations, packets, parameterWrites);
            }};
        },
        [&](const Store& source, const net::PluginHostingScope& scope) -> net::PluginHostingLease {
            ++authorizations;
            if (scope.app != "adi-daw" || scope.projectUuid != "project-one" ||
                scope.connectionId != "private-test" || scope.serverFingerprint != "paired-room" ||
                scope.address != expected || scope.audioBytesPerBlock != 512) return {};
            std::error_code fileError;
            if (media::readFileIdentity(source.path(), fileError) != originalFile) return {};
            return [&valid] { return valid.load(); };
        });
    std::string error;
    auto refused = loader(request, error);
    check(!refused && matchId && factories == 0 && authorizations == 1 && error.find("Room") != std::string::npos,
          "a project-enabled server row never grants a connection");

    const auto resolutionsBeforeLegacy = resolutions;
    request.project = legacy.get();
    refused = loader(request, error);
    check(!refused && resolutions == resolutionsBeforeLegacy && authorizations == 1 && factories == 0 &&
          error.find("project identity") != std::string::npos,
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
    check(remote->paramCount() == 1 && remote->setParam("17", device::ParamValue::fromNormalized(.5)) &&
          parameterWrites.load() == 1, "a granted owner can set a bound remote parameter");
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
        check(!remote->setParam("17", device::ParamValue::fromNormalized(.75)) && parameterWrites.load() == 1 &&
              remote->getParam("17").normalized == .5, "revocation stops a parameter write before sending it");
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

    remote.reset();
    original->db().exec(
        "UPDATE remote_hosts SET port=55056 WHERE id=7;"
        "INSERT INTO project(id,name,sample_rate) VALUES(1,'Remote saved control',48000);"
        "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Track',0),(2,'master','Master',1);"
        "INSERT INTO plugin_refs(id,format,uid,name,subtype)"
        " VALUES(1,'vst3','157c0f3844664a65b03c829f28b96435','Remote test','effect');"
        "INSERT INTO device_chains(id,track_id) VALUES(1,1);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,remote_host_id) VALUES(1,1,0,1,7);"
        "INSERT INTO plugin_params(device_id,param_id,name,normalized_value) VALUES(1,'17','Gain',0.625);");
    const auto copyPath = dir.path() / "copy.adi";
    const auto copied = original->saveCopy(copyPath, StoreFileCollision::Fail, StoreCopyPolicy::preserve);
    check(copied.ok, "copy the granted project to another native file");
    if (copied.ok) {
        StoreError copyError{};
        const auto copy = Store::open(copyPath, copyError, /*readOnly=*/true);
        check(copy && copy->db().execAndGet("SELECT value FROM adi_meta WHERE key='project_uuid'").getString() ==
              "project-one", "the copy retains the original project UUID");
        if (copy) {
            const auto attemptsBeforeCopy = authorizations;
            request.project = copy.get();
            refused = loader(request, error);
            request.project = original.get();
            check(!refused && authorizations == attemptsBeforeCopy + 1 && factories == 1 &&
                  error.find("consent") != std::string::npos,
                  "the native copy cannot use the original file's grant despite the shared UUID");
        }
    }
    engine::Session session;
    engine::SessionSpec spec{2, 48000, 64};
    spec.offline = true;
    const auto previousWrites = parameterWrites.load();
    const bool loaded = session.load(*original, engine::withRemoteDevices({}, loader), spec);
    const auto* saved = loaded ? session.instanceFor(1) : nullptr;
    check(loaded && saved && session.stats().paramsApplied == 1 &&
          parameterWrites.load() == previousWrites + 1 && saved->getParam("17").normalized == .625,
          "project open restores the saved normalized value to the admitted remote device");

    const auto sharePath = dir.path() / "shared.adi";
    const auto shared = original->saveCopy(sharePath);
    check(shared.ok, "share the project with the default copy policy");
    if (shared.ok) {
        StoreError shareError{};
        const auto share = Store::open(sharePath, shareError, /*readOnly=*/true);
        check(share != nullptr, "reopen the shared project");
        if (share) {
            const auto uuid = share->db().execAndGet("SELECT value FROM adi_meta WHERE key='project_uuid'").getString();
            check(!uuid.empty() && uuid != "project-one" &&
                  share->db().execAndGet("SELECT enabled FROM remote_hosts WHERE id=7").getInt() == 0,
                  "a shared copy has a new UUID and retains the server with hosting disabled");
            const auto resolutionsBeforeShare = resolutions;
            const auto attemptsBeforeShare = authorizations;
            const auto factoriesBeforeShare = factories;
            request.project = share.get();
            refused = loader(request, error);
            request.project = original.get();
            check(!refused && resolutions == resolutionsBeforeShare && authorizations == attemptsBeforeShare &&
                  factories == factoriesBeforeShare && error.find("disabled") != std::string::npos,
                  "a shared copy cannot resolve or authorize its disabled server");
        }
    }
    original->db().exec("UPDATE remote_hosts SET enabled=0 WHERE id=7;");
    const auto resolutionsBeforeDisable = resolutions;
    const auto attemptsBeforeDisable = authorizations;
    const auto factoriesBeforeDisable = factories;
    refused = loader(request, error);
    check(!refused && resolutions == resolutionsBeforeDisable && authorizations == attemptsBeforeDisable &&
          factories == factoriesBeforeDisable && error.find("disabled") != std::string::npos,
          "a disabled server is rejected before resolving the host or consulting a valid grant");
}

void cataloguedBindings() {
    test::TempDirectory dir("plugin_admission", "catalogue");
    net::PluginServerCatalogue catalogue(dir.path() / "servers.sqlite");
    const net::PluginServerAddress endpoint{"127.0.0.1", 55056};
    const std::string fullId = "157C0F3844664A65B03C829F28B96435";
    const net::PluginCatalogueScope scope{"project-one", "paired-room", endpoint};
    std::string error;
    check(catalogue.refresh(scope, [](const auto&) { return true; },
              [&](const auto&, std::string&) -> std::optional<std::vector<net::ServerPlugin>> {
                  return std::vector<net::ServerPlugin>{{"vst3", fullId, "Named plug-in", "VST3-12ab", true},
                                                       {"clap", "org.adi.test", "Named plug-in", "org.adi.test", false}};
              }, error), "the full identity and stock token can share a cache row");
    int trustedMappings = 0;
    auto resolve = net::cataloguedPluginServerResolver(catalogue,
        [&](const net::PluginServerAddress& address, std::string_view)
            -> std::optional<net::PluginServerPairing> {
            if (address != endpoint) return {};
            return net::PluginServerPairing{"fixture-connection", "paired-room", [&](std::string_view id) {
                if (id != fullId) return std::vector<device::AudioGridderParameterBinding>{};
                ++trustedMappings;
                return std::vector<device::AudioGridderParameterBinding>{{3, "17"}};
            }};
        });
    net::PluginServerLocation server{7, "Room", endpoint, "audiogridder"};
    const device::DeviceIdentity vst3{"vst3", "157c0f3844664a65b03c829f28b96435", "Other name", {}, {}};
    const device::DeviceIdentity clap{"clap", "org.adi.test", "Named plug-in", {}, {}};
    auto stock = resolve(server, vst3);
    check(stock && stock->eventFlow == engine::EventFlow::Consume && stock->createProcessor(endpoint) && trustedMappings == 1,
          "stock backend uses full VST3 identity independent of display spelling");
    check(!resolve(server, clap), "stock catalogue cannot route a CLAP to a VST3 command");
    server.protocol = "adps-v1";
    auto native = resolve(server, clap);
    check(native && native->eventFlow == engine::EventFlow::Through && native->createProcessor(endpoint) &&
          trustedMappings == 1,
          "native backend uses the exact CLAP descriptor ID");
    server.address.port = 55057;
    check(!resolve(server, clap), "the cached server fingerprint grants no access to a remapped endpoint");
}
} // namespace

int main() {
    try {
        run();
        cataloguedBindings();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
