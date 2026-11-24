// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-10: OSCQuery v1 core — namespace tree builder, ?HOST_INFO, and pure
// HTTP/1.1 GET-only request parser and responder (ADR-0185 d4; ADR-0188 d7).

#include "adi/oscquery.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace adi::oscquery {

namespace {

using Json = nlohmann::json;

constexpr int kOscQueryReadWriteAccess = 3;

/// Serialize JSON with replacing UTF-8 error handling so invalid UTF-8 bytes
/// in user/plugin names produce U+FFFD instead of throwing.
[[nodiscard]] std::string dumpJson(const Json& j) {
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

[[nodiscard]] double clampNormalized(float v) noexcept {
    if (!std::isfinite(v)) {
        return 0.0;
    }
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return clamped == 0.0f ? 0.0 : static_cast<double>(clamped);
}

[[nodiscard]] std::string formatDescription(std::string_view name, std::string_view text) {
    if (text.empty()) {
        return std::string(name);
    }
    std::string out;
    out.reserve(name.size() + 2U + text.size());
    out.append(name);
    out.append(": ");
    out.append(text);
    return out;
}

[[nodiscard]] Json makeLeafNode(const std::string& fullPath,
                                float value,
                                std::string_view name,
                                std::string_view valueText) {
    return Json{
        {"ACCESS", kOscQueryReadWriteAccess},
        {"DESCRIPTION", formatDescription(name, valueText)},
        {"FULL_PATH", fullPath},
        {"RANGE", Json::array({Json{{"MAX", 1.0}, {"MIN", 0.0}}})},
        {"TYPE", "f"},
        {"VALUE", Json::array({clampNormalized(value)})},
    };
}

[[nodiscard]] Json makeContainerNode(const std::string& fullPath,
                                     std::string_view description,
                                     Json contents) {
    return Json{
        {"CONTENTS", std::move(contents)},
        {"DESCRIPTION", std::string(description)},
        {"FULL_PATH", fullPath},
    };
}

void indexNodes(const Json& node, std::map<std::string, std::string, std::less<>>& byPath) {
    if (!node.is_object()) {
        return;
    }
    if (const auto fp = node.find("FULL_PATH"); fp != node.end() && fp->is_string()) {
        byPath[fp->get<std::string>()] = dumpJson(node);
    }
    if (const auto contents = node.find("CONTENTS");
        contents != node.end() && contents->is_object()) {
        for (auto it = contents->begin(); it != contents->end(); ++it) {
            indexNodes(it.value(), byPath);
        }
    }
}

[[nodiscard]] Json buildDevicesContainer(const std::string& basePath,
                                         const std::vector<DeviceDescriptor>& devices) {
    Json devicesContents = Json::object();
    std::set<std::uint64_t> seenDevices;

    for (const auto& dev : devices) {
        if (dev.stableId == 0U || !seenDevices.insert(dev.stableId).second) {
            continue;
        }
        const std::string devKey = std::to_string(dev.stableId);
        const std::string devPath = basePath + "/" + devKey;

        Json devContents = Json::object();
        devContents["bypass"] =
            makeLeafNode(devPath + "/bypass", dev.bypassed ? 1.0f : 0.0f, "Bypass", "");

        if (!dev.params.empty()) {
            Json paramsContents = Json::object();
            std::set<std::uint64_t> seenParams;
            const std::string paramsBase = devPath + "/param";
            for (const auto& param : dev.params) {
                if (param.stableId == 0U || !seenParams.insert(param.stableId).second) {
                    continue;
                }
                const std::string pKey = std::to_string(param.stableId);
                paramsContents[pKey] =
                    makeLeafNode(paramsBase + "/" + pKey, param.value, param.name, param.valueText);
            }
            if (!paramsContents.empty()) {
                devContents["param"] =
                    makeContainerNode(paramsBase, "Parameters", std::move(paramsContents));
            }
        }

        devicesContents[devKey] = makeContainerNode(
            devPath, dev.name.empty() ? ("Device " + devKey) : dev.name, std::move(devContents));
    }
    return devicesContents;
}

[[nodiscard]] bool isHttpTokenChar(unsigned char c) noexcept {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        return true;
    }
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool isValidToken(std::string_view s) noexcept {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (!isHttpTokenChar(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidHeaderValue(std::string_view s) noexcept {
    for (char c : s) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc != 0x09U && (uc < 0x20U || uc > 0x7EU)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] char asciiLower(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

[[nodiscard]] bool asciiEqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view trimOws(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1U);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1U);
    }
    return s;
}

struct ParsedHost {
    std::string_view hostPart;
    std::optional<std::uint16_t> port;
};

[[nodiscard]] std::optional<std::uint16_t> parsePortNumber(std::string_view s) noexcept {
    if (s.empty() || s.size() > 5U || s[0] < '1' || s[0] > '9') {
        return std::nullopt;
    }
    std::uint32_t acc = 0U;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        acc = (acc * 10U) + static_cast<std::uint32_t>(c - '0');
        if (acc > 65535U) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint16_t>(acc);
}

[[nodiscard]] bool isValidHostNameChar(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '-' || c == '.' || c == '_';
}

[[nodiscard]] bool isValidIpv6Char(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') ||
           c == ':' || c == '.';
}

/// Parse a trimmed Host header value (`host` or `host:port` or `[ipv6]` or `[ipv6]:port`).
[[nodiscard]] std::optional<ParsedHost> parseHostHeader(std::string_view raw) noexcept {
    const std::string_view trimmed = trimOws(raw);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    ParsedHost out;
    if (trimmed.front() == '[') {
        const std::size_t close = trimmed.find(']');
        if (close == std::string_view::npos || close <= 1U) {
            return std::nullopt;
        }
        const std::string_view inside = trimmed.substr(1U, close - 1U);
        if (inside.find(':') == std::string_view::npos) {
            return std::nullopt;
        }
        for (char c : inside) {
            if (!isValidIpv6Char(c)) {
                return std::nullopt;
            }
        }
        out.hostPart = trimmed.substr(0U, close + 1U);
        if (close + 1U < trimmed.size()) {
            if (trimmed[close + 1U] != ':') {
                return std::nullopt;
            }
            const auto port = parsePortNumber(trimmed.substr(close + 2U));
            if (!port.has_value()) {
                return std::nullopt;
            }
            out.port = *port;
        }
        return out;
    }

    const std::size_t colon = trimmed.find(':');
    std::string_view hostPart = trimmed;
    if (colon != std::string_view::npos) {
        if (trimmed.find(':', colon + 1U) != std::string_view::npos) {
            return std::nullopt;  // unbracketed IPv6 or extra colon
        }
        hostPart = trimmed.substr(0U, colon);
        const auto port = parsePortNumber(trimmed.substr(colon + 1U));
        if (!port.has_value()) {
            return std::nullopt;
        }
        out.port = *port;
    }

    if (hostPart.empty() || hostPart.front() == '.' || hostPart.back() == '.') {
        return std::nullopt;
    }
    for (char c : hostPart) {
        if (!isValidHostNameChar(c)) {
            return std::nullopt;
        }
    }
    out.hostPart = hostPart;
    return out;
}

[[nodiscard]] bool matchesAllowedHostEntry(std::string_view reqHostPart,
                                           std::string_view allowedEntry) noexcept {
    const std::string_view entry = trimOws(allowedEntry);
    if (entry.empty()) {
        return false;
    }
    if (asciiEqualsIgnoreCase(reqHostPart, entry)) {
        return true;
    }
    // If the configured entry is a bare IPv6 literal (e.g. "fe80::1"), match
    // the bracketed Host form ("[fe80::1]").
    if (entry.front() != '[' && entry.find(':') != std::string_view::npos &&
        reqHostPart.size() == entry.size() + 2U && reqHostPart.front() == '[' &&
        reqHostPart.back() == ']') {
        return asciiEqualsIgnoreCase(reqHostPart.substr(1U, entry.size()), entry);
    }
    return false;
}

[[nodiscard]] bool hasUnsafePercentEncoding(std::string_view s) noexcept {
    for (std::size_t i = 0U; i < s.size(); ++i) {
        if (s[i] != '%') {
            continue;
        }
        if (i + 2U >= s.size()) {
            return true;
        }
        const char h1 = asciiLower(s[i + 1U]);
        const char h2 = asciiLower(s[i + 2U]);
        const bool okHex =
            ((h1 >= '0' && h1 <= '9') || (h1 >= 'a' && h1 <= 'f')) &&
            ((h2 >= '0' && h2 <= '9') || (h2 >= 'a' && h2 <= 'f'));
        if (!okHex) {
            return true;
        }
        // Refuse encoded NUL (%00), slash (%2f), or backslash (%5c).
        if ((h1 == '0' && h2 == '0') || (h1 == '2' && h2 == 'f') ||
            (h1 == '5' && h2 == 'c')) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool isSafePath(std::string_view path) noexcept {
    if (path.empty() || path[0] != '/') {
        return false;
    }
    for (char c : path) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x21U || uc > 0x7EU || c == '\\') {
            return false;
        }
    }
    if (hasUnsafePercentEncoding(path)) {
        return false;
    }
    if (path == "/") {
        return true;
    }
    // Disallow empty segments ("//") and dot traversal ("." or "..").
    std::size_t pos = 1U;
    while (pos <= path.size()) {
        const std::size_t slash = path.find('/', pos);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view seg = path.substr(pos, end - pos);
        if (seg.empty()) {
            if (slash != std::string_view::npos) {
                return false;
            }
            break;
        }
        if (seg == "." || seg == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        pos = slash + 1U;
    }
    return true;
}

[[nodiscard]] bool isValidQuery(std::string_view query) noexcept {
    for (char c : query) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x21U || uc > 0x7EU) {
            return false;
        }
    }
    return !hasUnsafePercentEncoding(query);
}

[[nodiscard]] bool looksLikeHttpVersion(std::string_view v) noexcept {
    return v.size() == 8U && v.substr(0U, 5U) == "HTTP/" &&
           v[5] >= '0' && v[5] <= '9' && v[6] == '.' && v[7] >= '0' && v[7] <= '9';
}

[[nodiscard]] ParseRequestResult makeParseError(int status, std::string reason) {
    ParseRequestResult r;
    r.errorStatus = status;
    r.errorReason = std::move(reason);
    return r;
}

[[nodiscard]] HttpResponse makeErrorResponse(int status,
                                             std::string reason,
                                             std::string allowHeader = {}) {
    HttpResponse resp;
    resp.status = status;
    resp.reason = std::move(reason);
    resp.allowHeader = std::move(allowHeader);
    resp.body = dumpJson(Json{{"error", resp.reason}, {"status", status}});
    return resp;
}

}  // namespace

std::string HttpResponse::serialize() const {
    std::string out;
    out.reserve(160U + body.size());
    out.append("HTTP/1.1 ");
    out.append(std::to_string(status));
    out.push_back(' ');
    out.append(reason);
    out.append("\r\nContent-Type: ");
    out.append(contentType);
    out.append("\r\nContent-Length: ");
    out.append(std::to_string(body.size()));
    if (!allowHeader.empty()) {
        out.append("\r\nAllow: ");
        out.append(allowHeader);
    }
    out.append("\r\nConnection: close\r\n\r\n");
    out.append(body);
    return out;
}

OscQueryTree OscQueryTree::build(const NamespaceDescription& desc) {
    Json tracksContents = Json::object();
    std::set<std::uint64_t> seenTracks;

    for (const auto& track : desc.tracks) {
        if (track.stableId == 0U || !seenTracks.insert(track.stableId).second) {
            continue;
        }
        const std::string tKey = std::to_string(track.stableId);
        const std::string tPath = "/adi/track/" + tKey;

        Json tContents = Json::object();
        tContents["volume"] =
            makeLeafNode(tPath + "/volume", track.volume, "Volume", track.volumeText);
        tContents["pan"] = makeLeafNode(tPath + "/pan", track.pan, "Pan", track.panText);
        tContents["mute"] = makeLeafNode(tPath + "/mute", track.mute ? 1.0f : 0.0f, "Mute", "");
        tContents["solo"] = makeLeafNode(tPath + "/solo", track.solo ? 1.0f : 0.0f, "Solo", "");
        tContents["arm"] = makeLeafNode(tPath + "/arm", track.arm ? 1.0f : 0.0f, "Arm", "");

        if (!track.sends.empty()) {
            Json sendsContents = Json::object();
            std::set<std::uint64_t> seenSends;
            const std::string sendsBase = tPath + "/send";
            for (const auto& send : track.sends) {
                if (send.stableId == 0U || !seenSends.insert(send.stableId).second) {
                    continue;
                }
                const std::string sKey = std::to_string(send.stableId);
                sendsContents[sKey] =
                    makeLeafNode(sendsBase + "/" + sKey, send.value, send.name, send.valueText);
            }
            if (!sendsContents.empty()) {
                tContents["send"] =
                    makeContainerNode(sendsBase, "Sends", std::move(sendsContents));
            }
        }

        if (!track.devices.empty()) {
            const std::string devBase = tPath + "/device";
            Json devContents = buildDevicesContainer(devBase, track.devices);
            if (!devContents.empty()) {
                tContents["device"] =
                    makeContainerNode(devBase, "Devices", std::move(devContents));
            }
        }

        tracksContents[tKey] = makeContainerNode(
            tPath, track.name.empty() ? ("Track " + tKey) : track.name, std::move(tContents));
    }

    Json masterContents = Json::object();
    masterContents["volume"] =
        makeLeafNode("/adi/master/volume", desc.master.volume, "Volume", desc.master.volumeText);
    masterContents["pan"] =
        makeLeafNode("/adi/master/pan", desc.master.pan, "Pan", desc.master.panText);
    masterContents["mute"] =
        makeLeafNode("/adi/master/mute", desc.master.mute ? 1.0f : 0.0f, "Mute", "");
    if (!desc.master.devices.empty()) {
        Json masterDev = buildDevicesContainer("/adi/master/device", desc.master.devices);
        if (!masterDev.empty()) {
            masterContents["device"] =
                makeContainerNode("/adi/master/device", "Devices", std::move(masterDev));
        }
    }

    Json adiContents = Json::object();
    adiContents["master"] = makeContainerNode(
        "/adi/master", desc.master.name.empty() ? "Main" : desc.master.name,
        std::move(masterContents));
    adiContents["track"] = makeContainerNode("/adi/track", "Tracks", std::move(tracksContents));

    Json rootContents = Json::object();
    rootContents["adi"] = makeContainerNode(
        "/adi", desc.rootDescription.empty() ? "ADI" : desc.rootDescription,
        std::move(adiContents));

    Json root = makeContainerNode(
        "/", desc.rootDescription.empty() ? "ADI" : desc.rootDescription,
        std::move(rootContents));

    OscQueryTree tree;
    tree.rootJson_ = dumpJson(root);
    indexNodes(root, tree.nodesByPath_);
    return tree;
}

std::optional<std::string> OscQueryTree::jsonForPath(std::string_view path) const {
    const auto it = nodesByPath_.find(path);
    if (it == nodesByPath_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool OscQueryTree::contains(std::string_view path) const noexcept {
    return nodesByPath_.find(path) != nodesByPath_.end();
}

std::string renderHostInfoJson(const HostInfo& info) {
    Json j{
        {"EXTENSIONS",
         Json{
             {"ACCESS", true},
             {"DESCRIPTION", true},
             {"RANGE", true},
             {"VALUE", true},
         }},
        {"NAME", info.name.empty() ? "ADI DAW" : info.name},
        {"OSC_PORT", info.oscPort},
        {"OSC_TRANSPORT", info.oscTransport.empty() ? "UDP" : info.oscTransport},
    };
    if (!info.oscIp.empty()) {
        j["OSC_IP"] = info.oscIp;
    }
    return dumpJson(j);
}

bool isHostAllowed(std::string_view hostHeader, const HostInfo& hostInfo) noexcept {
    const auto parsed = parseHostHeader(hostHeader);
    if (!parsed.has_value()) {
        return false;
    }

    if (hostInfo.httpPort != 0U) {
        const std::uint16_t effectivePort = parsed->port.value_or(80U);
        if (effectivePort != hostInfo.httpPort) {
            return false;
        }
    }

    const std::string_view h = parsed->hostPart;
    if (asciiEqualsIgnoreCase(h, "localhost") || h == "127.0.0.1" ||
        asciiEqualsIgnoreCase(h, "[::1]")) {
        return true;
    }

    if (!hostInfo.oscIp.empty() && matchesAllowedHostEntry(h, hostInfo.oscIp)) {
        return true;
    }

    for (const auto& entry : hostInfo.allowedHosts) {
        if (matchesAllowedHostEntry(h, entry)) {
            return true;
        }
    }

    return false;
}

ParseRequestResult parseHttpRequest(std::span<const std::byte> raw, const RequestLimits& limits) {
    if (raw.empty()) {
        return makeParseError(400, "Bad Request");
    }
    if (raw.size() > limits.maxRequestBytes) {
        return makeParseError(413, "Content Too Large");
    }

    const std::string_view text(reinterpret_cast<const char*>(raw.data()), raw.size());
    const std::size_t headerEnd = text.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos) {
        return makeParseError(400, "Bad Request");
    }

    const std::string_view head = text.substr(0U, headerEnd);
    if (head.find('\0') != std::string_view::npos) {
        return makeParseError(400, "Bad Request");
    }

    const std::size_t firstCrlf = head.find("\r\n");
    const std::string_view reqLine =
        firstCrlf == std::string_view::npos ? head : head.substr(0U, firstCrlf);

    const std::size_t sp1 = reqLine.find(' ');
    if (sp1 == std::string_view::npos) {
        return makeParseError(400, "Bad Request");
    }
    const std::size_t sp2 = reqLine.find(' ', sp1 + 1U);
    if (sp2 == std::string_view::npos || reqLine.find(' ', sp2 + 1U) != std::string_view::npos) {
        return makeParseError(400, "Bad Request");
    }

    const std::string_view method = reqLine.substr(0U, sp1);
    const std::string_view target = reqLine.substr(sp1 + 1U, sp2 - (sp1 + 1U));
    const std::string_view version = reqLine.substr(sp2 + 1U);

    if (!isValidToken(method)) {
        return makeParseError(400, "Bad Request");
    }
    if (target.empty()) {
        return makeParseError(400, "Bad Request");
    }
    if (target.size() > limits.maxTargetBytes) {
        return makeParseError(414, "URI Too Long");
    }
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        if (looksLikeHttpVersion(version)) {
            return makeParseError(505, "HTTP Version Not Supported");
        }
        return makeParseError(400, "Bad Request");
    }

    const std::size_t qPos = target.find('?');
    const std::string_view path =
        qPos == std::string_view::npos ? target : target.substr(0U, qPos);
    const std::string_view query =
        qPos == std::string_view::npos ? std::string_view{} : target.substr(qPos + 1U);

    if (!isSafePath(path) || !isValidQuery(query)) {
        return makeParseError(400, "Bad Request");
    }

    // Validate header lines and require exactly one syntactically valid Host header.
    std::string_view hostHeader;
    std::size_t hostCount = 0U;
    if (firstCrlf != std::string_view::npos) {
        std::size_t pos = firstCrlf + 2U;
        std::size_t headerCount = 0U;
        while (pos < head.size()) {
            const std::size_t nextCrlf = head.find("\r\n", pos);
            const std::size_t lineEnd = nextCrlf == std::string_view::npos ? head.size() : nextCrlf;
            const std::string_view line = head.substr(pos, lineEnd - pos);

            ++headerCount;
            if (headerCount > limits.maxHeaders) {
                return makeParseError(431, "Request Header Fields Too Large");
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0U) {
                return makeParseError(400, "Bad Request");
            }
            const std::string_view name = line.substr(0U, colon);
            const std::string_view value = line.substr(colon + 1U);
            if (!isValidToken(name) || !isValidHeaderValue(value)) {
                return makeParseError(400, "Bad Request");
            }
            if (asciiEqualsIgnoreCase(name, "host")) {
                ++hostCount;
                hostHeader = trimOws(value);
            }

            if (nextCrlf == std::string_view::npos) {
                break;
            }
            pos = nextCrlf + 2U;
        }
    }

    if (hostCount != 1U || !parseHostHeader(hostHeader).has_value()) {
        return makeParseError(400, "Bad Request");
    }

    HttpRequest req;
    req.method = std::string(method);
    req.target = std::string(target);
    req.path = std::string(path);
    req.query = std::string(query);
    req.version = std::string(version);
    req.host = std::string(hostHeader);

    ParseRequestResult ok;
    ok.request = std::move(req);
    return ok;
}

ParseRequestResult parseHttpRequest(std::string_view raw, const RequestLimits& limits) {
    return parseHttpRequest(std::as_bytes(std::span<const char>(raw.data(), raw.size())), limits);
}

HttpResponse handleHttpRequest(const OscQueryTree& tree,
                               const HostInfo& hostInfo,
                               std::span<const std::byte> rawRequest,
                               const RequestLimits& limits) {
    const ParseRequestResult parsed = parseHttpRequest(rawRequest, limits);
    if (!parsed.ok()) {
        return makeErrorResponse(parsed.errorStatus, parsed.errorReason);
    }

    const HttpRequest& req = *parsed.request;
    if (!isHostAllowed(req.host, hostInfo)) {
        return makeErrorResponse(403, "Forbidden");
    }

    if (req.method != "GET") {
        return makeErrorResponse(405, "Method Not Allowed", "GET");
    }

    if (!req.query.empty()) {
        if (req.query == "HOST_INFO" && req.path == "/") {
            HttpResponse resp;
            resp.status = 200;
            resp.reason = "OK";
            resp.body = renderHostInfoJson(hostInfo);
            return resp;
        }
        return makeErrorResponse(400, "Bad Request");
    }

    if (const auto body = tree.jsonForPath(req.path)) {
        HttpResponse resp;
        resp.status = 200;
        resp.reason = "OK";
        resp.body = *body;
        return resp;
    }

    return makeErrorResponse(404, "Not Found");
}

HttpResponse handleHttpRequest(const OscQueryTree& tree,
                               const HostInfo& hostInfo,
                               std::string_view rawRequest,
                               const RequestLimits& limits) {
    return handleHttpRequest(
        tree, hostInfo, std::as_bytes(std::span<const char>(rawRequest.data(), rawRequest.size())),
        limits);
}

}  // namespace adi::oscquery
