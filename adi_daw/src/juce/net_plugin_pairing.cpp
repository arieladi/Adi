// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/net_plugin_pairing.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/control/consent_file.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace adi::net {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxFileBytes = 64 * 1024;
constexpr std::size_t kMaxHost = 253;

bool printable(std::string_view text, std::size_t maximum) {
    return !text.empty() && text.size() <= maximum &&
           std::ranges::all_of(text, [](char c) { return c > ' ' && c < '\x7f'; });
}

bool validHost(std::string_view host) {
    return !host.empty() && host.size() <= kMaxHost && std::ranges::all_of(host, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
               c == '-' || c == '_' || c == ':';
    });
}

bool knownProtocol(std::string_view protocol) { return protocol == "adps-v1" || protocol == "audiogridder"; }

bool valid(const PluginServerPair& pair) {
    return validHost(pair.address.host) && pair.address.port != 0 && knownProtocol(pair.protocol) &&
           printable(pair.connectionId, PluginServerPairings::maximumIdentifier) &&
           printable(pair.serverFingerprint, PluginServerPairings::maximumIdentifier);
}

bool sameEndpoint(const PluginServerPair& pair, const PluginServerAddress& address, std::string_view protocol) {
    return pair.address == address && pair.protocol == protocol;
}

std::string newConnectionId() {
    std::random_device entropy;
    constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string id = "pair-";
    for (int word = 0; word < 4; ++word) {
        auto bits = static_cast<std::uint32_t>(entropy());
        for (int nibble = 0; nibble < 8; ++nibble) {
            id.push_back(digits[bits & 0xfU]);
            bits >>= 4U;
        }
    }
    return id;
}

std::string text(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) throw std::runtime_error("invalid pairing record");
    return found->get_ref<const std::string&>();
}

/// The file as it is now. A missing or empty file has no pairings; anything
/// that is not exactly the stored shape is nullopt.
std::optional<std::vector<PluginServerPair>> load(const std::filesystem::path& file) {
    const auto bytes = control::detail::readConsentFile(file, kMaxFileBytes);
    if (!bytes) return std::nullopt;
    std::vector<PluginServerPair> pairs;
    if (bytes->empty()) return pairs;
    try {
        const auto doc = Json::parse(*bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 3) throw std::runtime_error("invalid pairing file depth");
            return true;
        });
        if (!doc.is_object() || doc.size() != 2 || !doc.contains("version") || !doc.contains("pairings") ||
            !doc.at("version").is_number_integer() || doc.at("version").get<std::int64_t>() != 1 ||
            !doc.at("pairings").is_array() || doc.at("pairings").size() > PluginServerPairings::maximumPairs)
            return std::nullopt;
        for (const auto& row : doc.at("pairings")) {
            if (!row.is_object() || row.size() != 5) return std::nullopt;
            const auto port = row.find("port");
            if (port == row.end() || !port->is_number_unsigned() || port->get<std::uint64_t>() < 1 ||
                port->get<std::uint64_t>() > 65535)
                return std::nullopt;
            PluginServerPair pair{{text(row, "host"), static_cast<std::uint16_t>(port->get<std::uint64_t>())},
                                  text(row, "protocol"), text(row, "connection"), text(row, "fingerprint")};
            if (!valid(pair)) return std::nullopt;
            if (std::ranges::any_of(pairs, [&](const auto& seen) {
                    return sameEndpoint(seen, pair.address, pair.protocol);
                }))
                return std::nullopt;
            pairs.push_back(std::move(pair));
        }
        return pairs;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool store(const std::filesystem::path& file, std::vector<PluginServerPair> pairs) {
    std::ranges::sort(pairs, [](const PluginServerPair& a, const PluginServerPair& b) {
        return std::tie(a.address.host, a.address.port, a.protocol) <
               std::tie(b.address.host, b.address.port, b.protocol);
    });
    Json records = Json::array();
    for (const auto& pair : pairs) {
        records.push_back({{"host", pair.address.host},
                        {"port", pair.address.port},
                        {"protocol", pair.protocol},
                        {"connection", pair.connectionId},
                        {"fingerprint", pair.serverFingerprint}});
    }
    const Json doc = {{"version", 1}, {"pairings", std::move(records)}};
    const auto bytes = doc.dump(2) + "\n";
    return bytes.size() <= kMaxFileBytes && control::detail::writeConsentFile(file, bytes);
}

} // namespace

std::filesystem::path pluginServerPairingsPath(appdata::App app) {
    const auto config = appdata::pathsFor(app).config;
    if (config.empty()) return {};
    return config / "plugin-servers" / "pairings.json";
}

PluginServerPairings::PluginServerPairings(std::filesystem::path file) : file_(std::move(file)) {}

std::optional<PluginServerPair> PluginServerPairings::pair(PluginServerPair record, std::string& error) const {
    error.clear();
    if (audio::onAudioThread) return std::nullopt;
    if (record.connectionId.empty()) record.connectionId = newConnectionId();
    if (!valid(record)) {
        error = "a pairing needs a host, a port, a known protocol, a connection and a server fingerprint";
        return std::nullopt;
    }
    if (!file_.is_absolute()) {
        error = "plug-in server pairings need an absolute app config path";
        return std::nullopt;
    }
    bool stored = false;
    const bool locked = control::detail::withConsentFileLock(file_, [&] {
        auto pairs = load(file_);
        if (!pairs) {
            error = "the plug-in server pairings cannot be read; nothing was changed";
            return;
        }
        std::erase_if(*pairs, [&](const auto& held) { return sameEndpoint(held, record.address, record.protocol); });
        if (pairs->size() >= maximumPairs) {
            error = "too many plug-in servers are paired";
            return;
        }
        pairs->push_back(record);
        stored = store(file_, std::move(*pairs));
        if (!stored) error = "the plug-in server pairings could not be written";
    });
    if (!locked && error.empty()) error = "the plug-in server pairings are in use or unavailable";
    if (!stored) return std::nullopt;
    return record;
}

bool PluginServerPairings::unpair(const PluginServerAddress& address, std::string_view protocol,
                                  std::string& error) const {
    error.clear();
    if (audio::onAudioThread || !file_.is_absolute()) return false;
    bool removed = false;
    const bool locked = control::detail::withConsentFileLock(file_, [&] {
        auto pairs = load(file_);
        if (!pairs) {
            error = "the plug-in server pairings cannot be read; nothing was changed";
            return;
        }
        if (std::erase_if(*pairs, [&](const auto& held) { return sameEndpoint(held, address, protocol); }) == 0)
            return;
        removed = store(file_, std::move(*pairs));
        if (!removed) error = "the plug-in server pairings could not be written";
    });
    if (!locked && error.empty()) error = "the plug-in server pairings are in use or unavailable";
    return removed;
}

std::optional<std::vector<PluginServerPair>> PluginServerPairings::list(std::string& error) const {
    error.clear();
    if (audio::onAudioThread) return std::nullopt;
    if (file_.empty()) return std::vector<PluginServerPair>{};
    auto pairs = load(file_);
    if (!pairs) error = "the plug-in server pairings cannot be read";
    return pairs;
}

std::optional<PluginServerPair> PluginServerPairings::find(const PluginServerAddress& address,
                                                           std::string_view protocol) const {
    if (audio::onAudioThread || file_.empty()) return std::nullopt;
    const auto pairs = load(file_);
    if (!pairs) return std::nullopt;
    const auto found = std::ranges::find_if(*pairs, [&](const auto& held) {
        return sameEndpoint(held, address, protocol);
    });
    if (found == pairs->end()) return std::nullopt;
    return *found;
}

PluginServerPairLookup PluginServerPairings::lookup() const {
    return [pairings = *this](const PluginServerAddress& address, std::string_view protocol)
               -> std::optional<PluginServerPairing> {
        auto found = pairings.find(address, protocol);
        if (!found) return std::nullopt;
        PluginServerPairing pairing;
        pairing.connectionId = std::move(found->connectionId);
        pairing.serverFingerprint = std::move(found->serverFingerprint);
        return pairing;
    };
}

} // namespace adi::net
