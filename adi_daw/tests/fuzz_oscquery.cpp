// SPDX-License-Identifier: GPL-3.0-or-later
//
// libFuzzer harness for the OSCQuery v1 HTTP request parser & responder — SRF-10.
//
// THE PROPERTY UNDER TEST
//
//   No untrusted HTTP request buffer causes a crash, hang, out-of-bounds read,
//   undefined behaviour, CORS wildcard leakage, or LISTEN key emission.
//
// Built automatically by CMakeLists.txt when ADI_BUILD_FUZZERS=ON against
// src/adi/oscquery.cpp alone (without osc_codec or adi_core).

#include "adi/oscquery.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace {

using namespace adi::oscquery;

const OscQueryTree& fixtureTree() {
    static const OscQueryTree kTree = []() {
        NamespaceDescription desc;
        TrackDescriptor t1;
        t1.stableId = 101;
        t1.name = "Drums";
        t1.volume = 0.75f;
        t1.volumeText = "-2.5 dB";
        t1.pan = 0.5f;
        t1.panText = "C";
        desc.tracks.push_back(t1);
        desc.master.volume = 1.0f;
        desc.master.volumeText = "0.0 dB";
        return OscQueryTree::build(desc);
    }();
    return kTree;
}

const HostInfo& fixtureHostInfo() {
    static const HostInfo kInfo{"ADI DAW", "127.0.0.1", 9000, "UDP"};
    return kInfo;
}

void exercise(std::span<const std::byte> input, const RequestLimits& limits) {
    const auto parsed = parseHttpRequest(input, limits);
    if (parsed.ok()) {
        if (parsed.errorStatus != 0 || !parsed.request.has_value()) {
            __builtin_trap();
        }
        if (parsed.request->method.empty() || parsed.request->target.empty() ||
            parsed.request->path.empty() || parsed.request->path[0] != '/') {
            __builtin_trap();
        }
    } else {
        if (parsed.errorStatus < 400 || parsed.errorStatus >= 600 || parsed.request.has_value()) {
            __builtin_trap();
        }
    }

    const HttpResponse resp = handleHttpRequest(fixtureTree(), fixtureHostInfo(), input, limits);
    if (resp.status < 200 || resp.status >= 600 || resp.reason.empty() || resp.body.empty()) {
        __builtin_trap();
    }
    if (resp.status == 405 && resp.allowHeader != "GET") {
        __builtin_trap();
    }
    if (resp.body.find("LISTEN") != std::string::npos) {
        __builtin_trap();
    }

    const std::string wire = resp.serialize();
    if (wire.find("Access-Control-Allow-Origin") != std::string::npos) {
        __builtin_trap();
    }
    if (wire.find("\r\n\r\n") == std::string::npos) {
        __builtin_trap();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto bytes = std::as_bytes(std::span<const std::uint8_t>(data, size));

    // Default production limits.
    exercise(bytes, RequestLimits{});

    // Tight limits to exercise 413 / 414 / 431 caps on small fuzzer inputs.
    RequestLimits tight;
    tight.maxRequestBytes = 96;
    tight.maxTargetBytes = 32;
    tight.maxHeaders = 4;
    exercise(bytes, tight);

    return 0;
}
