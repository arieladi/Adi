// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The MCP projection of the control methods (CTL-08; ADR-0168 d3; ADR-0233;
// docs/CONTROL-MCP.md §5.2, §6, §7).
//
// Protocol only, no sockets. Both eras the plan requires, as the published
// specification fixes them:
//
//   legacy (2025-11-25, also 2025-06-18 and 2024-11-05): `initialize`
//     negotiates a version for the session (the stdio process); `ping`. Not
//     2025-03-26, which requires receiving JSON-RPC batches: such a client is
//     answered 2025-11-25. A request whose _meta names a legacy version is
//     served in the legacy era, after initialize, not refused −32022.
//   modern (2026-07-28): stateless. Each request carries
//     _meta["io.modelcontextprotocol/protocolVersion"] and
//     _meta["io.modelcontextprotocol/clientCapabilities"]; every result
//     carries resultType; discover and the list/read results carry ttlMs and
//     cacheScope; `server/discover` is mandatory; `ping` is gone; an
//     unsupported version is −32022 with {supported, requested}.
//
// resources/read of something that is not there: −32002 with {uri} in a
// legacy session (2025-11-25's SHOULD), −32602 for a modern request (which
// must not emit −32002).
//
// Every refusal made here is recorded in the service's audit by its code and
// code name (CONTROL-MCP §7); one that came back from Service::call was
// recorded there and is not recorded twice.
//
// Four tools, never one per op (ADR-0168 d3, ruled by the director):
// project_read, ops_describe, changeset_propose, changeset_status. Resources
// are read-only views on the same methods: adi://projects/{project}/manifest,
// .../graph, .../telemetry, adi://ops and adi://ops/{name}.
//
// An unknown tool is −32602. A refusal -- bad arguments, an op not allowed,
// a stale head, over a cap, not granted -- is a tool execution error
// (isError: true) carrying ADI's refusal, so the model can correct itself.

#pragma once

#include "adi/control/jsonrpc.hpp"
#include "adi/control/project_access.hpp"
#include "adi/control/service.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi::control {

struct McpServerInfo {
    std::string name = "adi-mcp";
    std::string title = "ADI";
    std::string version;
};

class McpEndpoint {
public:
    static constexpr std::string_view kModernVersion = "2026-07-28";
    /// Newest first; the answer when a client asks for one we do not know.
    [[nodiscard]] static const std::vector<std::string>& legacyVersions();
    /// ADR-0233: what the instructions say (CTL-08).
    [[nodiscard]] static std::string defaultInstructions();
    /// ADI's own `_meta` namespace: reverse DNS of the project's GitHub
    /// Pages domain, which ADI controls.
    static constexpr std::string_view kMetaPrefix = "io.github.arieladi.adi/";

    /// One client connection's state. For stdio, the process.
    struct Session {
        /// The transport's credential for this client -- for adi-mcp a
        /// per-process secret. Never read from the wire.
        std::string credential;
        bool initialized = false;   ///< legacy era: after `initialize`
        std::string version;        ///< legacy era: the negotiated version
        std::string clientLabel;    ///< from clientInfo, for actor_detail only
    };

    McpEndpoint(Service& service, ProjectSet& projects, McpServerInfo info,
                std::string instructions = defaultInstructions());

    /// One message in; at most one line out (none for a notification or a
    /// client's response). The service's executor must run calls before
    /// `call` returns, as adi-mcp's inline executor does.
    [[nodiscard]] std::optional<std::string> handle(const Incoming& message, Session& session);

    /// Records a connection event ("connect", "disconnect") in the audit.
    void noteConnection(const Session& session, const char* kind);

private:
    struct Era {
        bool modern = false;
        std::string label;   ///< the caller's label for this request
    };

    [[nodiscard]] Json initializeResult(const Json& params, Session& session) const;
    [[nodiscard]] Json discoverResult() const;
    [[nodiscard]] Json toolsList(Tier tier) const;
    [[nodiscard]] Json resourcesList() const;
    [[nodiscard]] Json resourceTemplates() const;
    [[nodiscard]] Json capabilities(bool modern) const;
    [[nodiscard]] Json serverInfoJson() const;

    /// tools/call: a result, or a JSON-RPC error.
    [[nodiscard]] std::optional<Json> callTool(const Json& params, const Principal& who, RpcError& error);
    /// `modern`: the request's era, which decides the not-found code.
    [[nodiscard]] std::optional<Json> readResource(const Json& params, const Principal& who, bool modern,
                                                   RpcError& error);
    /// Records a refusal made at this layer: {now, "refused", label, method,
    /// code, the code's name}; never the message, never the credential.
    void auditRefusal(const std::string& label, const char* method, const RpcError& error);
    /// auditRefusal, then the encoded error.
    [[nodiscard]] std::string refuse(const Json& id, const std::string& label, const char* method,
                                     RpcError error);
    /// Runs one control method synchronously.
    [[nodiscard]] Outcome run(const Principal& who, std::string_view method, const Json& params,
                              const RequestMeta& meta);

    Service& service_;
    ProjectSet& projects_;
    McpServerInfo info_;
    std::string instructions_;
};

}  // namespace adi::control
