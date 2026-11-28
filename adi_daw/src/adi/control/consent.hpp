// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace adi::control {

enum class ConsentCapability { ProjectRead, ModelProvider, PluginHosting };
enum class ConsentTerm { ThisTime, OneHour, OneDay, UntilRevoked };
enum class ConsentData {
    ProjectMetadata, TrackNames, Routing, Devices, Parameters, Markers, Clips,
    Automation, Notes, Remarks, Tunings, Diagnostics, Prompt, Audio, PluginState,
    ProjectGraph, Telemetry, ChangesetPreview, BrowserCatalogue
};

struct ConsentItem {
    ConsentData kind;
    std::size_t bytes;
};

/// Created by the host that will send the data. Connection identifies its
/// pairing or provider configuration. Destination identifies the actual
/// recipient, or its endpoint for a directly connected provider.
struct ConsentDestination {
    std::string connection;
    std::string endpoint;
    bool externalMcp = false;
};

struct ConsentRequest {
    std::string projectUuid;
    ConsentDestination destination;
    ConsentCapability capability = ConsentCapability::ProjectRead;
    std::vector<ConsentItem> data;
    /// Required for server plug-ins. Save a Copy preserves the project UUID,
    /// but a different native file cannot inherit its server grant.
    std::string fileBinding = {};
    /// The trusted caller may bind a one-time approval to the exact payload.
    /// Standing approvals remain scoped by data kind.
    std::string disclosureFingerprint = {};
};

enum class ConsentDecision { Allowed, ConsentRequired, Unavailable };

/// One ledger for ADI, outside the project. All times are Unix seconds.
/// Only the host that asks the local user should call grant or revoke.
class ConsentLedger {
public:
    static constexpr std::size_t kMaxFileBytes = 256 * 1024;

    ConsentLedger();
    explicit ConsentLedger(std::filesystem::path file);

    /// A successful read consumes any matching This-time grants atomically.
    [[nodiscard]] ConsentDecision authorize(const ConsentRequest&, std::int64_t now);
    [[nodiscard]] bool grant(const ConsentRequest&, ConsentTerm, std::int64_t now);
    [[nodiscard]] bool revoke(const ConsentRequest&);

private:
    std::filesystem::path file_;
};

}  // namespace adi::control
