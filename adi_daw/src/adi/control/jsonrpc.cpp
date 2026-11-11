// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/jsonrpc.hpp"

#include <exception>
#include <utility>

namespace adi::control {

int nestingDepth(std::string_view text, int limit) noexcept {
    int depth = 0;
    int deepest = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > deepest) {
                deepest = depth;
                if (deepest > limit) return limit + 1;
            }
        } else if (c == ']' || c == '}') {
            if (depth > 0) --depth;
        }
    }
    return deepest;
}

namespace {

Incoming invalid(int code, std::string message, Json id = nullptr) {
    Incoming m;
    m.kind = Incoming::Kind::Invalid;
    m.id = std::move(id);
    m.error = {code, std::move(message), nullptr};
    return m;
}

bool validId(const Json& id) { return id.is_string() || id.is_number(); }

}  // namespace

Incoming parseMessage(std::string_view line, const WireLimits& limits) {
    if (line.size() > limits.maxMessageBytes)
        return invalid(errc::InvalidRequest,
                       "message larger than " + std::to_string(limits.maxMessageBytes) + " bytes");
    if (nestingDepth(line, limits.maxDepth) > limits.maxDepth)
        return invalid(errc::InvalidRequest,
                       "message nested deeper than " + std::to_string(limits.maxDepth) + " levels");

    // No exceptions: a line that is not JSON is a parse error, never a throw.
    Json j = Json::parse(line.begin(), line.end(), nullptr, false);
    if (j.is_discarded()) return invalid(errc::ParseError, "parse error: not JSON");
    if (j.is_array())
        return invalid(errc::InvalidRequest, "batches are not supported (MCP removed them in 2025-06-18)");
    if (!j.is_object()) return invalid(errc::InvalidRequest, "a message is a JSON object");

    // The id first, so every later refusal can be addressed to it.
    Json id = nullptr;
    const auto idIt = j.find("id");
    const bool hasId = idIt != j.end();
    if (hasId) {
        if (!validId(*idIt))
            return invalid(errc::InvalidRequest, "an id is a string or a number, never null");
        id = *idIt;
    }

    const auto version = j.find("jsonrpc");
    if (version == j.end() || !version->is_string() || version->get_ref<const std::string&>() != "2.0")
        return invalid(errc::InvalidRequest, "\"jsonrpc\" must be \"2.0\"", id);

    const auto method = j.find("method");
    if (method == j.end()) {
        if (hasId && (j.contains("result") || j.contains("error"))) {
            Incoming m;
            m.kind = Incoming::Kind::Response;
            m.id = std::move(id);
            return m;
        }
        return invalid(errc::InvalidRequest, "\"method\" is required", id);
    }
    if (!method->is_string()) return invalid(errc::InvalidRequest, "\"method\" must be a string", id);

    Incoming m;
    m.kind = hasId ? Incoming::Kind::Request : Incoming::Kind::Notification;
    m.id = std::move(id);
    m.method = method->get<std::string>();
    if (const auto params = j.find("params"); params != j.end()) {
        if (!params->is_object()) {
            // A notification is never answered: it keeps no params and is
            // dropped as every notification is. A request gets the refusal.
            if (!hasId) return m;
            return invalid(errc::InvalidParams, "\"params\" must be an object", m.id);
        }
        m.params = std::move(*params);
    }
    return m;
}

std::string dumpJson(const Json& j) {
    try {
        return j.dump(-1, ' ', false, Json::error_handler_t::replace);
    } catch (const std::exception&) {
        // Only an allocation failure reaches here; say so in valid JSON.
        return "null";
    }
}

std::string encodeResult(const Json& id, const Json& result) {
    Json m = Json::object();
    m["jsonrpc"] = "2.0";
    m["id"] = id;
    m["result"] = result;
    return dumpJson(m);
}

std::string encodeError(const Json& id, const RpcError& e) {
    Json err = Json::object();
    err["code"] = e.code;
    err["message"] = e.message;
    if (!e.data.is_null()) err["data"] = e.data;
    Json m = Json::object();
    m["jsonrpc"] = "2.0";
    m["id"] = id;
    m["error"] = std::move(err);
    return dumpJson(m);
}

}  // namespace adi::control
