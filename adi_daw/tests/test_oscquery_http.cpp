// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-10: HTTP GET responder, ?HOST_INFO, limits, and Host-header DNS-rebinding
// unit tests (split from test_oscquery.cpp to keep each TU small for MSVC).

#include "adi/oscquery.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace adi::oscquery;
using Json = nlohmann::json;

bool jsonHasKeyRecursive(const Json& j, std::string_view key) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == key) {
                return true;
            }
            if (jsonHasKeyRecursive(it.value(), key)) {
                return true;
            }
        }
    } else if (j.is_array()) {
        for (const auto& elem : j) {
            if (jsonHasKeyRecursive(elem, key)) {
                return true;
            }
        }
    }
    return false;
}

NamespaceDescription makeFixture() {
    NamespaceDescription desc;
    desc.rootDescription = "ADI";

    TrackDescriptor t1;
    t1.stableId = 101;
    t1.name = "Drums";
    t1.volume = 0.75f;
    t1.volumeText = "-2.5 dB";
    t1.pan = 0.5f;
    t1.panText = "C";
    t1.arm = true;

    desc.tracks = {t1};
    desc.master.name = "Main";
    desc.master.volume = 1.0f;
    desc.master.volumeText = "0.0 dB";
    desc.master.pan = 0.5f;
    desc.master.panText = "C";
    return desc;
}

}  // namespace

void runOscQueryHttpTests(int& checks, int& failures) {
    auto check = [&](bool cond, const std::string& what) {
        ++checks;
        if (!cond) {
            ++failures;
            std::printf("  FAIL  %s\n", what.c_str());
        }
    };

    std::printf("[?HOST_INFO, pure GET-only HTTP handler, and Host DNS-rebinding defense]\n");

    const auto tree = OscQueryTree::build(makeFixture());
    HostInfo info;
    info.name = "ADI DAW";
    info.oscIp = "192.168.1.5";
    info.oscPort = 9001;
    info.oscTransport = "UDP";
    info.allowedHosts = {"adi-studio.local", "fe80::1"};

    // GET /?HOST_INFO
    {
        const auto resp =
            handleHttpRequest(tree, info, "GET /?HOST_INFO HTTP/1.1\r\nHost: localhost\r\n\r\n");
        check(resp.status == 200, "?HOST_INFO returns 200");
        const auto j = Json::parse(resp.body);
        check(j.at("NAME") == "ADI DAW", "HOST_INFO NAME");
        check(j.at("OSC_TRANSPORT") == "UDP", "HOST_INFO OSC_TRANSPORT == UDP");
        check(j.at("OSC_PORT") == 9001, "HOST_INFO OSC_PORT == 9001");
        check(j.at("OSC_IP") == "192.168.1.5", "HOST_INFO OSC_IP");
        check(j.at("EXTENSIONS").at("ACCESS") == true, "EXTENSIONS.ACCESS");
        check(j.at("EXTENSIONS").at("DESCRIPTION") == true, "EXTENSIONS.DESCRIPTION");
        check(j.at("EXTENSIONS").at("RANGE") == true, "EXTENSIONS.RANGE");
        check(j.at("EXTENSIONS").at("VALUE") == true, "EXTENSIONS.VALUE");
        check(!jsonHasKeyRecursive(j, "LISTEN"), "no LISTEN key in HOST_INFO");
        check(resp.body.find("LISTEN") == std::string::npos, "no LISTEN text in HOST_INFO");

        const std::string wire = resp.serialize();
        check(wire.find("HTTP/1.1 200 OK\r\n") == 0U, "HTTP status line 200");
        check(wire.find("Access-Control-Allow-Origin") == std::string::npos,
              "no CORS * header emitted");
    }

    // Allowed Host headers (loopback, oscIp, IPv6 bracketed, allowedHosts, case-insensitive).
    for (const char* goodHost : {"localhost", "LOCALHOST:8080", "127.0.0.1", "127.0.0.1:9001",
                                 "[::1]", "[::1]:9001", "192.168.1.5", "192.168.1.5:8080",
                                 "adi-studio.local", "ADI-STUDIO.LOCAL:8080", "[fe80::1]"}) {
        const std::string req =
            std::string("GET / HTTP/1.1\r\nHost: ") + goodHost + "\r\n\r\n";
        const auto resp = handleHttpRequest(tree, info, req);
        check(resp.status == 200, std::string("allowed Host accepted: ") + goodHost);
    }

    // DNS rebinding & foreign Host headers refused with 403 Forbidden.
    for (const char* evilHost : {"evil.com", "evil.com:9001", "localhost.evil.com",
                                 "127.0.0.1.evil.com", "192.168.1.99", "169.254.169.254",
                                 "[fe80::2]"}) {
        const std::string req =
            std::string("GET / HTTP/1.1\r\nHost: ") + evilHost + "\r\n\r\n";
        const auto resp = handleHttpRequest(tree, info, req);
        check(resp.status == 403, std::string("disallowed Host refused with 403: ") + evilHost);
    }

    // Explicit httpPort enforcement on Host header.
    {
        HostInfo portInfo = info;
        portInfo.httpPort = 8080U;
        check(handleHttpRequest(tree, portInfo,
                                "GET / HTTP/1.1\r\nHost: localhost:8080\r\n\r\n")
                      .status == 200,
              "matching Host port accepted");
        check(handleHttpRequest(tree, portInfo,
                                "GET / HTTP/1.1\r\nHost: localhost:9999\r\n\r\n")
                      .status == 403,
              "mismatched Host port refused with 403");
        check(handleHttpRequest(tree, portInfo, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
                      .status == 403,
              "implicit port 80 refused when httpPort == 8080");
    }

    // Missing, duplicate, empty, or malformed Host header -> 400 Bad Request.
    const std::vector<std::string> badHostReqs = {
        "GET / HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: \r\n\r\n",
        "GET / HTTP/1.1\r\nHost: localhost\r\nHost: localhost\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a b\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: user@localhost\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: localhost:0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: localhost:99999\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: localhost:080\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: [::1\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: localhost.\r\n\r\n",
    };
    for (std::size_t i = 0U; i < badHostReqs.size(); ++i) {
        const auto resp = handleHttpRequest(tree, info, badHostReqs[i]);
        check(resp.status == 400, "bad Host[" + std::to_string(i) + "] returns 400");
    }

    // GET / and GET /adi/master/volume
    {
        const auto rRoot =
            handleHttpRequest(tree, info, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        check(rRoot.status == 200 && rRoot.body == tree.rootJson(), "GET / returns full tree");

        const auto rPath = handleHttpRequest(
            tree, info, "GET /adi/master/volume HTTP/1.1\r\nHost: localhost\r\n\r\n");
        check(rPath.status == 200 && rPath.body == *tree.jsonForPath("/adi/master/volume"),
              "GET /adi/master/volume returns leaf JSON");
    }

    // Any method other than GET is refused with 405 Method Not Allowed.
    for (const char* method : {"POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"}) {
        const std::string req =
            std::string(method) + " / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        const auto resp = handleHttpRequest(tree, info, req);
        check(resp.status == 405, std::string(method) + " refused with 405");
        check(resp.allowHeader == "GET", std::string(method) + " sets Allow: GET");
        check(resp.serialize().find("Allow: GET\r\n") != std::string::npos,
              std::string(method) + " wire response carries Allow: GET");
    }

    // Unknown path refused with 404 Not Found.
    for (const char* badPath : {"/adi/track/999/volume", "/adi/master/unknown", "/etc/passwd",
                                "/adi/track/101/volume/"}) {
        const std::string req =
            std::string("GET ") + badPath + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
        const auto resp = handleHttpRequest(tree, info, req);
        check(resp.status == 404, std::string("unknown path ") + badPath + " returns 404");
    }

    // Unsupported v2 query (?LISTEN) or non-root ?HOST_INFO refused cleanly.
    {
        const auto rListen = handleHttpRequest(
            tree, info, "GET /?LISTEN HTTP/1.1\r\nHost: localhost\r\n\r\n");
        check(rListen.status == 400, "?LISTEN refused with 400");

        const auto rPathHostInfo = handleHttpRequest(
            tree, info, "GET /adi/master?HOST_INFO HTTP/1.1\r\nHost: localhost\r\n\r\n");
        check(rPathHostInfo.status == 400, "non-root ?HOST_INFO refused with 400");
    }

    // Malformed requests refused with 400 or 505.
    const std::vector<std::string> malformed = {
        "",
        "GET / HTTP/1.1\r\n",
        "GET /\r\n\r\n",
        "GET / HTTP/1.1 EXTRA\r\nHost: localhost\r\n\r\n",
        "GET relative HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "GET /adi/../master HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "GET //adi/master HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "GET / HTTP/1.1\r\nBadHeaderNoColon\r\n\r\n",
        "GET / HTTP/1.1\r\n: empty-name\r\n\r\n",
        std::string("GET / HTTP/1.1\r\nX-Null: a\0b\r\n\r\n", 31),
    };
    for (std::size_t i = 0U; i < malformed.size(); ++i) {
        const auto resp = handleHttpRequest(tree, info, malformed[i]);
        check(resp.status == 400, "malformed[" + std::to_string(i) + "] returns 400");
    }

    // Unsupported HTTP version -> 505.
    check(handleHttpRequest(tree, info, "GET / HTTP/2.0\r\nHost: localhost\r\n\r\n").status == 505,
          "HTTP/2.0 returns 505");

    // Oversized request -> 413; oversized target -> 414; too many headers -> 431.
    {
        RequestLimits lim;
        lim.maxRequestBytes = 64;
        const std::string big(128, 'A');
        check(handleHttpRequest(tree, info, big, lim).status == 413,
              "oversized request returns 413");
    }
    {
        RequestLimits lim;
        lim.maxTargetBytes = 16;
        const std::string req = "GET /adi/track/101/volume HTTP/1.1\r\nHost: localhost\r\n\r\n";
        check(handleHttpRequest(tree, info, req, lim).status == 414,
              "oversized target returns 414");
    }
    {
        RequestLimits lim;
        lim.maxHeaders = 2;
        const std::string req =
            "GET / HTTP/1.1\r\nHost: localhost\r\nA: 1\r\nB: 2\r\n\r\n";
        check(handleHttpRequest(tree, info, req, lim).status == 431,
              "excessive headers returns 431");
    }
}
