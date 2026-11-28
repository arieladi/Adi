// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/net_plugin_server.hpp"

#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void number(Bytes& into, unsigned value, unsigned width) {
    for (auto remaining = width; remaining > 0; --remaining)
        into.push_back(static_cast<std::uint8_t>(value >> ((remaining - 1) * 8)));
}

void name(Bytes& into, std::string_view value) {
    while (!value.empty()) {
        const auto separator = value.find('.');
        const auto label = value.substr(0, separator);
        into.push_back(static_cast<std::uint8_t>(label.size()));
        for (const auto byte : label) into.push_back(static_cast<std::uint8_t>(byte));
        if (separator == std::string_view::npos) break;
        value.remove_prefix(separator + 1);
    }
    into.push_back(0);
}

void record(Bytes& into, const Bytes& owner, unsigned type, unsigned ttl, const Bytes& payload) {
    into.insert(into.end(), owner.begin(), owner.end());
    number(into, type, 2);
    number(into, 0x8001, 2);
    number(into, ttl, 4);
    number(into, static_cast<unsigned>(payload.size()), 2);
    into.insert(into.end(), payload.begin(), payload.end());
}

Bytes announcement(std::string_view service = "_audiogridder._tcp.local") {
    Bytes packet{0, 0, 0x84, 0, 0, 1, 0, 1, 0, 0, 0, 2};
    name(packet, service);
    const auto localOffset = static_cast<std::uint8_t>(packet.size() - 7);
    packet.insert(packet.end(), {0, 12, 0x80, 1});
    Bytes instance{7, 'S', 't', 'u', 'd', 'i', 'o', 'A', 0xc0, 12};
    const auto instanceOffset = packet.size() + 12;
    require(instanceOffset < 256, "test DNS compression offset");
    record(packet, {0xc0, 12}, 12, 60, instance);
    const Bytes reference{0xc0, static_cast<std::uint8_t>(instanceOffset)};
    Bytes srv{0, 0, 0, 0};
    number(srv, 55056, 2);
    srv.insert(srv.end(), {7, 'S', 't', 'u', 'd', 'i', 'o', 'A', 0xc0, localOffset});
    record(packet, reference, 33, 35, srv);
    constexpr std::string_view info =
        R"(INFO={"UUID":"00112233-4455-6677-8899-AABBCCDDEEFF","V":"1.2.0","ID":0,"LM":false})";
    Bytes txt{static_cast<std::uint8_t>(info.size())};
    for (const auto byte : info) txt.push_back(static_cast<std::uint8_t>(byte));
    record(packet, reference, 16, 30, txt);
    return packet;
}

void discovery() {
    adi::net::PluginServerDiscovery discovery;
    const auto packet = announcement();
    require(discovery.query().empty(), "discovery must start without a query");
    require(discovery.receive(packet, "192.168.1.12").empty(), "default receiver must ignore packets");
    discovery.setEnabled(true);
    const auto query = discovery.query();
    require(query.size() == 42 && query[5] == 1 && query.back() == 1, "one stock DNS-SD PTR query");
    const auto found = discovery.receive(packet, "192.168.1.12");
    require(found.size() == 1, "one stock advertisement");
    const auto& reply = found.front();
    require(reply.advertisedUuid == "00112233445566778899aabbccddeeff" && reply.name == "StudioA" &&
            reply.version == "1.2.0" && reply.address.host == "192.168.1.12" &&
            reply.address.port == 55056 && reply.ttlSeconds == 30, "compressed stock announcement fields");
    require(discovery.receive(announcement("_other._tcp.local"), "192.168.1.12").empty(),
            "unrelated service must never surface");
    for (std::size_t size = 0; size < packet.size(); ++size)
        require(discovery.receive(std::span(packet).first(size), "192.168.1.12").empty(),
                "every truncated packet must fail closed");
    auto compressionLoop = packet;
    const auto ownerOffset = query.size();
    compressionLoop[ownerOffset] = 0xc0;
    compressionLoop[ownerOffset + 1] = static_cast<std::uint8_t>(ownerOffset);
    require(discovery.receive(compressionLoop, "192.168.1.12").empty(), "cyclic compression must fail");
    {
        const adi::audio::CallbackScope callback;
        require(discovery.query().empty() && discovery.receive(packet, "192.168.1.12").empty(),
                "callback must have no discovery I/O or allocations");
    }
    discovery.setEnabled(false);
    require(discovery.query().empty() && discovery.receive(packet, "192.168.1.12").empty(),
            "turning discovery off stops queries and responses");
}

class Scratch {
public:
    Scratch() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("adi-server-catalogue-" + std::to_string(suffix));
        require(std::filesystem::create_directory(path), "could not reserve cache fixture");
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

void catalogue() {
    const auto dawCache = adi::net::pluginServerCataloguePath(adi::appdata::App::Daw);
    const auto djCache = adi::net::pluginServerCataloguePath(adi::appdata::App::DJ);
    require(dawCache.empty() || dawCache != djCache, "application caches must stay separate");
    Scratch scratch;
    const auto cacheDirectory = scratch.path / std::filesystem::path(u8"cache-\u05d0\u05d3\u05d9");
    const auto database = cacheDirectory / "catalogue.sqlite";
    const adi::net::PluginServerCatalogue cache(database);
    const adi::net::PluginCatalogueScope scope{
        "project-uuid", "trusted-server-key", {"192.168.1.12", 55056}};
    std::string error;
    unsigned queries = 0;
    const auto grant = [&](const auto& request) {
        return request.projectUuid == scope.projectUuid &&
               request.serverFingerprint == scope.serverFingerprint &&
               request.address == scope.address;
    };
    const auto fetch = [&](const auto&, std::string&) -> std::optional<std::vector<adi::net::ServerPlugin>> {
        ++queries;
        return std::vector<adi::net::ServerPlugin>{
            {"vst3", "ad1e0001504c41499f2a0c1100000003", "Plain", "VST3-8a7afd7a", true},
            {"clap", "org.example.test", "Example", "org.example.test", false},
            {"clap", "org.example.Test", "Example Uppercase", "org.example.Test", false}};
    };
    const auto initial = cache.cached(scope.serverFingerprint, error);
    require(initial && initial->empty() && !std::filesystem::exists(cacheDirectory),
            "an offline read of a missing Unicode cache must not create it");
    require(!cache.refresh(scope, {}, fetch, error) && queries == 0 &&
            !std::filesystem::exists(cacheDirectory), "missing grant prevents any contact or write");
    require(!cache.refreshStock(scope, grant, {}, error) && !std::filesystem::exists(cacheDirectory),
            "missing full class ID resolver prevents any stock catalogue query");
    auto other = scope;
    other.projectUuid = "copied-project";
    require(!cache.refresh(other, grant, fetch, error) && queries == 0, "grant cannot follow project copy");
    other = scope;
    other.address.host = "192.168.1.13";
    require(!cache.refresh(other, grant, fetch, error) && queries == 0, "grant cannot follow remap");
    other = scope;
    other.serverFingerprint = "00112233445566778899aabbccddeeff";
    require(!cache.refresh(other, grant, fetch, error) && queries == 0, "advertised UUID cannot act as grant");
    require(cache.refresh(scope, grant, fetch, error) && queries == 1, "granted full catalogue refresh");
    const adi::net::PluginServerCatalogue reopened(database);
    auto rows = reopened.cached(scope.serverFingerprint, error);
    require(rows && rows->size() == 3 && queries == 1, "offline catalogue reopens without a remote call");
    require((*rows)[0].uid == "org.example.Test" && (*rows)[1].uid == "org.example.test" &&
            (*rows)[2].uid == "AD1E0001504C41499F2A0C1100000003" &&
            (*rows)[2].serverToken == "VST3-8a7afd7a", "case-preserved CLAP ID, full canonical VST3 ID");
    auto malformed = [](const auto&, std::string&) -> std::optional<std::vector<adi::net::ServerPlugin>> {
        return std::vector<adi::net::ServerPlugin>{{"vst3", "8a7afd7a", "Short UID", "VST3-8a7afd7a", true}};
    };
    require(!cache.refresh(scope, grant, malformed, error), "stock lookup token must never become persistent UID");
    auto duplicate = [](const auto&, std::string&) -> std::optional<std::vector<adi::net::ServerPlugin>> {
        return std::vector<adi::net::ServerPlugin>{
            {"vst3", "ad1e0001504c41499f2a0c1100000003", "Plain", "VST3-8a7afd7a", true},
            {"vst3", "AD1E0001504C41499F2A0C1100000003", "Plain", "VST3-8a7afd7a", true}};
    };
    require(!cache.refresh(scope, grant, duplicate, error), "same canonical ID cannot appear twice");
    rows = reopened.cached(scope.serverFingerprint, error);
    require(rows && rows->size() == 3, "failed refresh keeps the last complete catalogue");
    auto absent = reopened.cached("00112233445566778899aabbccddeeff", error);
    require(absent && absent->empty(), "no trusted snapshot under advertised UUID");
    auto empty = [](const auto&, std::string&) -> std::optional<std::vector<adi::net::ServerPlugin>> {
        return std::vector<adi::net::ServerPlugin>{};
    };
    require(cache.refresh(scope, grant, empty, error), "atomic refresh replaces the server snapshot");
    rows = reopened.cached(scope.serverFingerprint, error);
    require(rows && rows->empty(), "absent plug-ins are removed from the server snapshot");
}

std::uint16_t testPort(std::string_view portText) {
    std::uint16_t port = 0;
    const auto end = portText.data() + portText.size();
    const auto parsed = std::from_chars(portText.data(), end, port);
    require(parsed.ec == std::errc{} && parsed.ptr == end && port != 0, "invalid loopback fixture port");
    return port;
}

void browseUdp(std::string_view mode, std::uint16_t port) {
    require(mode == "--udp" || mode == "--udp-off", "invalid discovery test mode");
    adi::net::PluginServerDiscovery discovery;
    if (mode == "--udp") discovery.setEnabled(true);
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    const auto found = discovery.browse({std::chrono::milliseconds(250), port}, error);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(found.has_value() && error.empty(), "UDP discovery socket failed");
    if (mode == "--udp-off") {
        require(found->empty(), "disabled UDP browse produced a result");
        return;
    }
    require(found->size() == 1, "one bounded and deduplicated UDP discovery result");
    const auto& result = found->front();
    require(result.address.host == "127.0.0.1" && result.address.port == 55056 &&
            result.name == "Loopback" && result.ttlSeconds == 15 &&
            result.advertisedUuid == "00112233445566778899aabbccddeeff", "loopback advertisement fields");
    std::cout << "Loopback UDP browse found one server in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms\n";
}

void stockCatalogue(std::uint16_t port) {
    Scratch scratch;
    const adi::net::PluginServerCatalogue cache(scratch.path / "catalogue.sqlite");
    const adi::net::PluginCatalogueScope scope{"project", "locally-paired-server", {"127.0.0.1", port}};
    std::string error;
    unsigned resolved = 0;
    const auto identity = [&](const auto& request, std::string_view token) -> std::optional<std::string> {
        require(request.serverFingerprint == scope.serverFingerprint && request.address == scope.address,
                "the identity resolver received a different destination");
        ++resolved;
        if (token == "VST3-12ab") return "AD1E0001504C41499F2A0C1100000003";
        return std::nullopt;
    };
    require(!cache.refreshStock(scope, {}, identity, error) && resolved == 0,
            "stock catalogue must not query without a grant");
    const auto granted = [&](const auto& request) {
        return request.projectUuid == scope.projectUuid && request.serverFingerprint == scope.serverFingerprint &&
            request.address == scope.address;
    };
    require(cache.refreshStock(scope, granted, identity, error) && resolved == 2,
            "stock catalogue must hand valid VST3 tokens to the paired resolver");
    const auto rows = cache.cached(scope.serverFingerprint, error);
    require(rows && rows->size() == 1 && rows->front().serverToken == "VST3-12ab" &&
            rows->front().uid == "AD1E0001504C41499F2A0C1100000003",
            "stock catalogue must save only rows with a separate full class ID mapping");
    std::cout << "Stock v13 metadata admitted one mapped identity from two eligible entries\n";
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc == 3) {
            const auto port = testPort(argv[2]);
            if (std::string_view(argv[1]) == "--stock-catalogue") stockCatalogue(port);
            else browseUdp(argv[1], port);
            return 0;
        }
        require(argc == 1, "invalid catalogue test arguments");
        discovery();
        catalogue();
        std::cout << "mDNS discovery and per-app catalogue tests passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
