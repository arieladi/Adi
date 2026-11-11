// SPDX-License-Identifier: AGPL-3.0-or-later
//
// JSON-RPC 2.0, as the control stack speaks it (ADR-0232 d1; docs/CONTROL-MCP.md §5).
//
// One message per line, strict: `jsonrpc` is "2.0"; an id is a string or a
// number and never null (MCP forbids a null id); params are an object; batches
// are refused (MCP removed them in 2025-06-18). A line is bounded in bytes and
// in nesting depth BEFORE it is parsed, so a hostile line costs nothing to
// refuse and cannot exhaust the stack of anything that walks it afterwards.
//
// JUCE-free and transport-free: stdio (adi-mcp) and the loopback WebSocket
// (CTL-04) both frame lines and hand them here.

#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace adi::control {

using Json = nlohmann::json;

/// The error codes, in one place. JSON-RPC's own, then ADR-0232 d5's, then MCP's.
namespace errc {
inline constexpr int ParseError = -32700;
inline constexpr int InvalidRequest = -32600;
inline constexpr int MethodNotFound = -32601;
inline constexpr int InvalidParams = -32602;
inline constexpr int Internal = -32603;
inline constexpr int Stale = -32002;           ///< ADR-0232 d5; data.headFound
inline constexpr int DeadlinePassed = -32003;  ///< ADR-0233: not applied
inline constexpr int OverLimit = -32005;       ///< ADR-0232 d5
inline constexpr int NotGranted = -32006;      ///< ADR-0232 d5; one text, whatever was wrong
inline constexpr int UnsupportedProtocolVersion = -32022;  ///< MCP 2026-07-28
}  // namespace errc

struct RpcError {
    int code = errc::Internal;
    std::string message;
    Json data = nullptr;   ///< null: no `data` member is sent
};

struct WireLimits {
    std::size_t maxMessageBytes = std::size_t{4} << 20;   ///< 4 MiB
    int maxDepth = 64;
};

struct Incoming {
    enum class Kind {
        Request,        ///< has an id: exactly one reply
        Notification,   ///< no id: never a reply
        Response,       ///< a client's reply to a request; ADI sends none, so it is dropped
        Invalid,        ///< reply with `error` to `id`; with no id member when none could be read
    };
    Kind kind = Kind::Invalid;
    Json id = nullptr;
    std::string method;
    Json params = nullptr;   ///< an object, or null when absent
    RpcError error;          ///< Invalid only
};

/// The deepest nesting of `[` and `{` outside strings. Stops counting, and
/// returns limit + 1, once deeper than `limit`. Never parses.
[[nodiscard]] int nestingDepth(std::string_view text, int limit) noexcept;

/// One line in. Never throws.
[[nodiscard]] Incoming parseMessage(std::string_view line, const WireLimits& = {});

/// Compact JSON. Text that is not valid UTF-8 (a name read from somebody's
/// project) is replaced, never thrown on.
[[nodiscard]] std::string dumpJson(const Json&);

[[nodiscard]] std::string encodeResult(const Json& id, const Json& result);
[[nodiscard]] std::string encodeError(const Json& id, const RpcError&);

}  // namespace adi::control
