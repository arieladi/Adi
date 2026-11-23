// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-10: unit tests for OSCQuery v1 tree generation, stable ids across track
// insertion, ?HOST_INFO, GET-only HTTP request handling, and absence of LISTEN.

#include "adi/oscquery.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

using namespace adi::oscquery;
using Json = nlohmann::json;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void section(const char* name) {
    std::printf("[%s]\n", name);
}

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

NamespaceDescription makeTwoTrackFixture() {
    NamespaceDescription desc;
    desc.rootDescription = "ADI";

    TrackDescriptor t1;
    t1.stableId = 101;
    t1.name = "Drums";
    t1.volume = 0.75f;
    t1.volumeText = "-2.5 dB";
    t1.pan = 0.5f;
    t1.panText = "C";
    t1.mute = false;
    t1.solo = false;
    t1.arm = true;

    TrackDescriptor t2;
    t2.stableId = 102;
    t2.name = "Bass";
    t2.volume = 0.5f;
    t2.volumeText = "-6.0 dB";
    t2.pan = 0.25f;
    t2.panText = "25L";
    t2.mute = true;
    t2.solo = false;
    t2.arm = false;

    desc.tracks = {t1, t2};

    desc.master.name = "Main";
    desc.master.volume = 1.0f;
    desc.master.volumeText = "0.0 dB";
    desc.master.pan = 0.5f;
    desc.master.panText = "C";
    desc.master.mute = false;

    return desc;
}

Json makeLeafGolden(const std::string& fullPath, double value, const std::string& description) {
    return Json{
        {"ACCESS", 3},
        {"DESCRIPTION", description},
        {"FULL_PATH", fullPath},
        {"RANGE", Json::array({Json{{"MAX", 1.0}, {"MIN", 0.0}}})},
        {"TYPE", "f"},
        {"VALUE", Json::array({value})},
    };
}

void testGoldenTreeTwoTracksAndMain() {
    section("golden JSON tree: two tracks and Main");

    const auto desc = makeTwoTrackFixture();
    const auto tree = OscQueryTree::build(desc);

    const Json expected = Json{
        {"CONTENTS",
         Json{
             {"adi",
              Json{
                  {"CONTENTS",
                   Json{
                       {"master",
                        Json{
                            {"CONTENTS",
                             Json{
                                 {"mute", makeLeafGolden("/adi/master/mute", 0.0, "Mute")},
                                 {"pan", makeLeafGolden("/adi/master/pan", 0.5, "Pan: C")},
                                 {"volume",
                                  makeLeafGolden("/adi/master/volume", 1.0, "Volume: 0.0 dB")},
                             }},
                            {"DESCRIPTION", "Main"},
                            {"FULL_PATH", "/adi/master"},
                        }},
                       {"track",
                        Json{
                            {"CONTENTS",
                             Json{
                                 {"101",
                                  Json{
                                      {"CONTENTS",
                                       Json{
                                           {"arm",
                                            makeLeafGolden("/adi/track/101/arm", 1.0, "Arm")},
                                           {"mute",
                                            makeLeafGolden("/adi/track/101/mute", 0.0, "Mute")},
                                           {"pan",
                                            makeLeafGolden("/adi/track/101/pan", 0.5, "Pan: C")},
                                           {"solo",
                                            makeLeafGolden("/adi/track/101/solo", 0.0, "Solo")},
                                           {"volume",
                                            makeLeafGolden("/adi/track/101/volume", 0.75,
                                                           "Volume: -2.5 dB")},
                                       }},
                                      {"DESCRIPTION", "Drums"},
                                      {"FULL_PATH", "/adi/track/101"},
                                  }},
                                 {"102",
                                  Json{
                                      {"CONTENTS",
                                       Json{
                                           {"arm",
                                            makeLeafGolden("/adi/track/102/arm", 0.0, "Arm")},
                                           {"mute",
                                            makeLeafGolden("/adi/track/102/mute", 1.0, "Mute")},
                                           {"pan",
                                            makeLeafGolden("/adi/track/102/pan", 0.25, "Pan: 25L")},
                                           {"solo",
                                            makeLeafGolden("/adi/track/102/solo", 0.0, "Solo")},
                                           {"volume",
                                            makeLeafGolden("/adi/track/102/volume", 0.5,
                                                           "Volume: -6.0 dB")},
                                       }},
                                      {"DESCRIPTION", "Bass"},
                                      {"FULL_PATH", "/adi/track/102"},
                                  }},
                             }},
                            {"DESCRIPTION", "Tracks"},
                            {"FULL_PATH", "/adi/track"},
                        }},
                   }},
                  {"DESCRIPTION", "ADI"},
                  {"FULL_PATH", "/adi"},
              }},
         }},
        {"DESCRIPTION", "ADI"},
        {"FULL_PATH", "/"},
    };

    const std::string golden = expected.dump();
    check(tree.rootJson() == golden, "root JSON matches golden string exactly");

    const auto parsed = Json::parse(tree.rootJson());
    check(parsed == expected, "parsed root JSON equals golden object");
    check(!jsonHasKeyRecursive(parsed, "LISTEN"), "no LISTEN key anywhere in tree");
    check(tree.rootJson().find("LISTEN") == std::string::npos, "no LISTEN token in tree text");

    // Direct subtree lookup matches the same node inside root.
    const auto volNode = tree.jsonForPath("/adi/track/101/volume");
    check(volNode.has_value(), "/adi/track/101/volume exists");
    if (volNode.has_value()) {
        check(Json::parse(*volNode) == makeLeafGolden("/adi/track/101/volume", 0.75,
                                                      "Volume: -2.5 dB"),
              "subtree lookup matches leaf golden");
    }
}

void testStableIdsWhenTrackInsertedBefore() {
    section("stable ids when a track is inserted before existing tracks");

    auto beforeDesc = makeTwoTrackFixture();
    // Add a send and device to track 101 to verify nested stable ids too.
    beforeDesc.tracks[0].sends.push_back(ParamDescriptor{501, "", "Send A", "-inf dB", 0.0f});
    DeviceDescriptor dev;
    dev.stableId = 701;
    dev.name = "Utility";
    dev.bypassed = false;
    dev.params.push_back(ParamDescriptor{901, "", "Gain", "0.0 dB", 0.5f});
    beforeDesc.tracks[0].devices.push_back(dev);

    const auto treeBefore = OscQueryTree::build(beforeDesc);

    // Insert a brand-new track (stableId 205) BEFORE track 101 and track 102.
    auto afterDesc = beforeDesc;
    TrackDescriptor inserted;
    inserted.stableId = 205;
    inserted.name = "Intro";
    inserted.volume = 0.25f;
    inserted.volumeText = "-12.0 dB";
    inserted.pan = 0.5f;
    inserted.panText = "C";
    afterDesc.tracks.insert(afterDesc.tracks.begin(), inserted);

    const auto treeAfter = OscQueryTree::build(afterDesc);

    // Every existing path under 101, 102, and master must stay byte-identical.
    const std::vector<std::string> stablePaths = {
        "/adi/track/101",
        "/adi/track/101/volume",
        "/adi/track/101/send/501",
        "/adi/track/101/device/701",
        "/adi/track/101/device/701/bypass",
        "/adi/track/101/device/701/param/901",
        "/adi/track/102",
        "/adi/track/102/volume",
        "/adi/master",
        "/adi/master/volume",
    };

    for (const auto& path : stablePaths) {
        const auto a = treeBefore.jsonForPath(path);
        const auto b = treeAfter.jsonForPath(path);
        check(a.has_value() && b.has_value() && *a == *b,
              "stable path unchanged after insertion before it: " + path);
    }

    // And the newly inserted track is reachable under its own stable id.
    check(treeAfter.contains("/adi/track/205/volume"), "new track 205 present");
}

void testHostInfoAndHttpHandling() {
    section("?HOST_INFO and pure GET-only HTTP handler");

    const auto tree = OscQueryTree::build(makeTwoTrackFixture());
    HostInfo info;
    info.name = "ADI DAW";
    info.oscIp = "192.168.1.5";
    info.oscPort = 9001;
    info.oscTransport = "UDP";

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
        "GET / HTTP/1.1\r\n",  // missing \r\n\r\n
        "GET /\r\n\r\n",       // missing version
        "GET / HTTP/1.1 EXTRA\r\n\r\n",
        "GET relative HTTP/1.1\r\n\r\n",
        "GET /adi/../master HTTP/1.1\r\n\r\n",
        "GET //adi/master HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.1\r\nBadHeaderNoColon\r\n\r\n",
        "GET / HTTP/1.1\r\n: empty-name\r\n\r\n",
        std::string("GET / HTTP/1.1\r\nX-Null: a\0b\r\n\r\n", 31),
    };
    for (std::size_t i = 0U; i < malformed.size(); ++i) {
        const auto resp = handleHttpRequest(tree, info, malformed[i]);
        check(resp.status == 400, "malformed[" + std::to_string(i) + "] returns 400");
    }

    // Unsupported HTTP version -> 505.
    {
        const auto resp = handleHttpRequest(tree, info, "GET / HTTP/2.0\r\n\r\n");
        check(resp.status == 505, "HTTP/2.0 returns 505");
    }

    // Oversized request -> 413; oversized target -> 414; too many headers -> 431.
    {
        RequestLimits lim;
        lim.maxRequestBytes = 64;
        const std::string big(128, 'A');
        const auto resp = handleHttpRequest(tree, info, big, lim);
        check(resp.status == 413, "oversized request returns 413");
    }
    {
        RequestLimits lim;
        lim.maxTargetBytes = 16;
        const std::string req = "GET /adi/track/101/volume HTTP/1.1\r\n\r\n";
        const auto resp = handleHttpRequest(tree, info, req, lim);
        check(resp.status == 414, "oversized target returns 414");
    }
    {
        RequestLimits lim;
        lim.maxHeaders = 2;
        const std::string req =
            "GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n";
        const auto resp = handleHttpRequest(tree, info, req, lim);
        check(resp.status == 431, "excessive headers returns 431");
    }
}

}  // namespace

int main() {
    testGoldenTreeTwoTracksAndMain();
    testStableIdsWhenTrackInsertedBefore();
    testHostInfoAndHttpHandling();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
