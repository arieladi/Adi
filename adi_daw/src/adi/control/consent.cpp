// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/consent.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/control/consent_file.hpp"
#include "adi/control/consent_scope.hpp"
#include "adi/control/jsonrpc.hpp"
#include "adi/media/blake3.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace adi::control {
namespace {

constexpr int kLastKind = static_cast<int>(ConsentData::BrowserCatalogue);

bool local(const ConsentRequest& request) {
    return request.capability != ConsentCapability::PluginHosting &&
           !request.destination.externalMcp && isLoopbackDestination(request.destination.endpoint);
}

std::string fingerprint(const Json& value) {
    try {
        const auto bytes = value.dump();
        const auto result = media::blake3Bytes(std::as_bytes(std::span(bytes)));
        return result ? result.hex : std::string{};
    } catch (const Json::exception&) {
        return {};
    }
}

std::string scopeKey(const ConsentRequest& request) {
    const auto& destination = request.destination;
    if (request.projectUuid.empty() || request.projectUuid.size() > 128 ||
        destination.connection.empty() || destination.connection.size() > 512 ||
        destination.endpoint.empty() || destination.endpoint.size() > 2048 ||
        request.fileBinding.size() > 128 ||
        (request.capability == ConsentCapability::PluginHosting && request.fileBinding.empty()))
        return {};
    return fingerprint(Json::array({
        "ADI DAW", request.projectUuid, destination.connection, destination.endpoint, destination.externalMcp,
        static_cast<int>(request.capability),
        request.capability == ConsentCapability::PluginHosting ? request.fileBinding : std::string{}}));
}

std::string onceKey(const ConsentRequest& request) {
    if (request.disclosureFingerprint.size() > 128) return {};
    auto sorted = request.data;
    std::sort(sorted.begin(), sorted.end(), [](const ConsentItem& a, const ConsentItem& b) {
        return a.kind < b.kind;
    });
    Json data = Json::array();
    for (const auto& item : sorted) data.push_back({static_cast<int>(item.kind), item.bytes});
    return fingerprint(Json::array({request.disclosureFingerprint, std::move(data)}));
}

bool isKey(const Json& value) {
    if (!value.is_string()) return false;
    const auto& key = value.get_ref<const std::string&>();
    return key.size() == 64 && key.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool validItems(const std::vector<ConsentItem>& data) {
    unsigned kinds = 0;
    for (const auto& item : data) {
        const auto kind = static_cast<int>(item.kind);
        if (kind < 0 || kind > kLastKind || item.bytes == 0) return false;
        const auto bit = 1U << kind;
        if (kinds & bit) return false;
        kinds |= bit;
    }
    return kinds != 0;
}

std::optional<Json> load(const std::filesystem::path& file) {
    const auto bytes = detail::readConsentFile(file, ConsentLedger::kMaxFileBytes);
    if (!bytes) return std::nullopt;
    if (bytes->empty()) return Json::array();
    // This document has a fixed shape. A corrupt file cannot supply
    // arbitrarily nested objects even within the byte cap.
    std::array<std::set<std::string>, 6> keys;
    const auto doc = Json::parse(*bytes, [&keys](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 4) throw std::runtime_error("invalid consent ledger depth");
        const auto level = static_cast<std::size_t>(depth);
        if (event == Json::parse_event_t::object_start) keys.at(level + 1).clear();
        if (event == Json::parse_event_t::key && !keys.at(level).insert(value.get<std::string>()).second)
            throw std::runtime_error("duplicate consent ledger key");
        return true;
    }, false);
    if (!doc.is_object() || doc.size() != 2 || !doc.contains("version") ||
        !doc.at("version").is_number_integer() || doc.at("version") != 1 ||
        !doc.contains("grants") || !doc.at("grants").is_array())
        return std::nullopt;
    std::set<std::pair<std::string, int>> scopes;
    for (const auto& row : doc.at("grants")) {
        if (!row.is_object() || row.size() != 4 ||
            !row.contains("scope") || !isKey(row.at("scope")) ||
            !row.contains("kind") || !row.at("kind").is_number_integer() ||
            row.at("kind") < 0 || row.at("kind") > kLastKind ||
            !row.contains("expires") ||
            (!row.at("expires").is_null() &&
             (!row.at("expires").is_number_integer() || row.at("expires") < 0 ||
              row.at("expires") > std::numeric_limits<std::int64_t>::max())) ||
            !row.contains("once") ||
            (!row.at("once").is_null() && (!isKey(row.at("once")) || !row.at("expires").is_null())))
            return std::nullopt;
        const auto& scope = row.at("scope").get_ref<const std::string&>();
        if (!scopes.emplace(scope, row.at("kind").get<int>()).second) return std::nullopt;
    }
    return doc.at("grants");
}

bool save(const std::filesystem::path& file, Json rows) {
    const auto bytes = dumpJson(Json{{"version", 1}, {"grants", std::move(rows)}});
    return bytes.size() <= ConsentLedger::kMaxFileBytes && detail::writeConsentFile(file, bytes);
}

bool matches(const Json& row, std::string_view scope, ConsentData kind) {
    // Compared as a string: a json beside a string_view is ambiguous to MSVC under C++20.
    const auto& stored = row.at("scope");
    return stored.is_string() && stored.get_ref<const std::string&>() == scope &&
           row.at("kind") == static_cast<int>(kind);
}

bool expired(const Json& row, std::int64_t now) {
    return !row.at("expires").is_null() && row.at("expires").get<std::int64_t>() <= now;
}

}  // namespace

ConsentLedger::ConsentLedger() : ConsentLedger(detail::consentFilePath()) {}
ConsentLedger::ConsentLedger(std::filesystem::path file) : file_(std::move(file)) {}

ConsentDecision ConsentLedger::authorize(const ConsentRequest& request, std::int64_t now) {
    if (audio::onAudioThread || !validItems(request.data)) return ConsentDecision::Unavailable;
    if (local(request)) return ConsentDecision::Allowed;
    const auto scope = scopeKey(request);
    const auto once = onceKey(request);
    if (scope.empty() || once.empty() || now < 0) return ConsentDecision::Unavailable;
    auto decision = ConsentDecision::Unavailable;
    const auto checked = detail::withConsentFileLock(file_, [&] {
        auto rows = load(file_);
        if (!rows) return;
        bool consume = false;
        for (const auto& item : request.data) {
            const auto row = std::find_if(rows->begin(), rows->end(), [&](const Json& entry) {
                return matches(entry, scope, item.kind) && !expired(entry, now) &&
                       (entry.at("once").is_null() || entry.at("once") == once);
            });
            if (row == rows->end()) {
                decision = ConsentDecision::ConsentRequired;
                return;
            }
            consume |= !row->at("once").is_null();
        }
        if (consume) {
            for (auto row = rows->begin(); row != rows->end();) {
                const bool used = row->at("scope") == scope && row->at("once") == once;
                row = used ? rows->erase(row) : std::next(row);
            }
            if (!save(file_, std::move(*rows))) return;
        }
        decision = ConsentDecision::Allowed;
    });
    return checked ? decision : ConsentDecision::Unavailable;
}

bool ConsentLedger::grant(const ConsentRequest& request, ConsentTerm term, std::int64_t now) {
    if (audio::onAudioThread || !validItems(request.data)) return false;
    if (local(request)) return true;
    const auto scope = scopeKey(request);
    const auto once = term == ConsentTerm::ThisTime ? onceKey(request) : std::string{};
    if (scope.empty() || now < 0 || now > std::numeric_limits<std::int64_t>::max() - 86400) return false;
    if (term == ConsentTerm::ThisTime && once.empty()) return false;
    Json expires = nullptr;
    switch (term) {
        case ConsentTerm::ThisTime:
        case ConsentTerm::UntilRevoked: break;
        case ConsentTerm::OneHour: expires = now + 3600; break;
        case ConsentTerm::OneDay: expires = now + 86400; break;
        default: return false;
    }
    bool saved = false;
    const auto checked = detail::withConsentFileLock(file_, [&] {
        auto rows = load(file_);
        if (!rows) return;
        for (auto row = rows->begin(); row != rows->end();) {
            const bool preserveStanding = term == ConsentTerm::ThisTime && row->at("once").is_null();
            const bool replace = expired(*row, now) ||
                (!preserveStanding && std::any_of(request.data.begin(), request.data.end(), [&](const ConsentItem& item) {
                    return matches(*row, scope, item.kind);
                }));
            row = replace ? rows->erase(row) : std::next(row);
        }
        for (const auto& item : request.data) {
            if (term == ConsentTerm::ThisTime &&
                std::any_of(rows->begin(), rows->end(), [&](const Json& row) {
                    return matches(row, scope, item.kind);
                })) continue;
            rows->push_back({{"scope", scope}, {"kind", static_cast<int>(item.kind)}, {"expires", expires},
                             {"once", once.empty() ? Json(nullptr) : Json(once)}});
        }
        saved = save(file_, std::move(*rows));
    });
    return checked && saved;
}

bool ConsentLedger::revoke(const ConsentRequest& request) {
    if (audio::onAudioThread) return false;
    const auto scope = scopeKey(request);
    if (scope.empty()) return false;
    bool saved = false;
    const auto checked = detail::withConsentFileLock(file_, [&] {
        auto rows = load(file_);
        if (!rows) return;
        const auto before = rows->size();
        for (auto row = rows->begin(); row != rows->end();) {
            row = row->at("scope") == scope ? rows->erase(row) : std::next(row);
        }
        saved = rows->size() == before || save(file_, std::move(*rows));
    });
    return checked && saved;
}

}  // namespace adi::control
