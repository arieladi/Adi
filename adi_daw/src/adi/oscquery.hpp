// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-10: OSCQuery v1 core — namespace tree builder, ?HOST_INFO, and pure
// HTTP/1.1 GET-only request parser and responder (ADR-0185 d4; ADR-0188 d7).
//
// Design rules (PLAN SRF-10):
//   * Self-contained: compiles without osc_codec or adi_core so its fuzz
//     target (tests/fuzz_oscquery.cpp) builds from src/adi/oscquery.cpp alone.
//   * Stable ids: tracks, sends, devices, and parameters are addressed by
//     their stable positive ids, so inserting or reordering tracks never
//     renumbers existing OSCQuery paths.
//   * v1 only: FULL_PATH, TYPE ("f"), VALUE, RANGE ([{MIN:0, MAX:1}]),
//     ACCESS (3 — read and write together, ADR-0188 d7), and DESCRIPTION
//     (name + Record.text). No LISTEN key (v2 waits for a transport ruling).
//   * HTTP GET only: `/`, a tree path, and `/?HOST_INFO`. No CORS `*`, no
//     file endpoints, no socket or DNS-SD code here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adi::oscquery {

/// One float parameter leaf (normalized 0..1, TYPE "f", ACCESS 3).
struct ParamDescriptor {
    std::uint64_t stableId = 0;  ///< Required for indexed items (send/param)
    std::string segment;         ///< Optional explicit segment (e.g. "volume")
    std::string name;            ///< Human display name
    std::string valueText;       ///< Record.text (e.g. "-6.0 dB"); optional
    float value = 0.0f;          ///< Normalized 0..1 (clamped if finite)
};

/// One device container under a track or Main.
struct DeviceDescriptor {
    std::uint64_t stableId = 0;  ///< Required positive stable id
    std::string name;
    bool bypassed = false;
    std::vector<ParamDescriptor> params;
};

/// One track container under /adi/track/{stableId}.
struct TrackDescriptor {
    std::uint64_t stableId = 0;  ///< Required positive stable id
    std::string name;
    float volume = 0.0f;
    std::string volumeText;
    float pan = 0.5f;
    std::string panText;
    bool mute = false;
    bool solo = false;
    bool arm = false;
    std::vector<ParamDescriptor> sends;
    std::vector<DeviceDescriptor> devices;
};

/// Main (master) bus under /adi/master.
struct MasterDescriptor {
    std::string name = "Main";
    float volume = 0.0f;
    std::string volumeText;
    float pan = 0.5f;
    std::string panText;
    bool mute = false;
    std::vector<DeviceDescriptor> devices;
};

/// Input snapshot from the parameter feed used to build an OSCQuery v1 tree.
struct NamespaceDescription {
    std::string rootDescription = "ADI";
    std::vector<TrackDescriptor> tracks;
    MasterDescriptor master;
};

/// Metadata returned by `GET /?HOST_INFO`.
struct HostInfo {
    std::string name = "ADI DAW";
    std::string oscIp;            ///< Optional; omitted when empty
    std::uint16_t oscPort = 9000;
    std::string oscTransport = "UDP";
};

/// Explicit caps protecting the pure HTTP request parser.
struct RequestLimits {
    std::size_t maxRequestBytes = 8192;
    std::size_t maxTargetBytes = 1024;
    std::size_t maxHeaders = 64;
};

/// Parsed HTTP/1.x request line (method, target split into path + query).
struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
};

struct ParseRequestResult {
    std::optional<HttpRequest> request;
    int errorStatus = 0;
    std::string errorReason;

    [[nodiscard]] bool ok() const noexcept {
        return errorStatus == 0 && request.has_value();
    }
};

/// Pure HTTP/1.1 response produced by handleHttpRequest().
struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::string contentType = "application/json; charset=utf-8";
    std::string allowHeader;  ///< Populated on 405 Method Not Allowed ("GET")
    std::string body;

    /// Format as an HTTP/1.1 wire response (Connection: close, no CORS *).
    [[nodiscard]] std::string serialize() const;
};

/// Immutable OSCQuery v1 node tree built from a NamespaceDescription.
class OscQueryTree {
public:
    OscQueryTree() = default;

    /// Build a deterministic OSCQuery v1 tree from `desc`.
    /// Entries with invalid/duplicate stable ids are skipped deterministically.
    [[nodiscard]] static OscQueryTree build(const NamespaceDescription& desc);

    /// Return the serialized JSON for `path` (e.g. "/" or "/adi/master/volume"),
    /// or nullopt if `path` is not present in the tree.
    [[nodiscard]] std::optional<std::string> jsonForPath(std::string_view path) const;

    /// Return the full root ("/") tree JSON.
    [[nodiscard]] const std::string& rootJson() const noexcept { return rootJson_; }

    /// Check whether `path` exists in the tree.
    [[nodiscard]] bool contains(std::string_view path) const noexcept;

private:
    std::string rootJson_;
    std::map<std::string, std::string, std::less<>> nodesByPath_;
};

/// Render the OSCQuery v1 `?HOST_INFO` JSON document (no LISTEN key).
[[nodiscard]] std::string renderHostInfoJson(const HostInfo& info);

/// Parse a raw HTTP/1.x request buffer under `limits`.
[[nodiscard]] ParseRequestResult parseHttpRequest(std::span<const std::byte> raw,
                                                  const RequestLimits& limits = {});

/// Convenience overload for string_view callers.
[[nodiscard]] ParseRequestResult parseHttpRequest(std::string_view raw,
                                                  const RequestLimits& limits = {});

/// Pure function from raw HTTP request bytes to an HTTP response over `tree`
/// and `hostInfo`. Supports GET `/`, GET `<path>`, and GET `/?HOST_INFO` only.
[[nodiscard]] HttpResponse handleHttpRequest(const OscQueryTree& tree,
                                             const HostInfo& hostInfo,
                                             std::span<const std::byte> rawRequest,
                                             const RequestLimits& limits = {});

/// Convenience overload for string_view callers.
[[nodiscard]] HttpResponse handleHttpRequest(const OscQueryTree& tree,
                                             const HostInfo& hostInfo,
                                             std::string_view rawRequest,
                                             const RequestLimits& limits = {});

}  // namespace adi::oscquery
