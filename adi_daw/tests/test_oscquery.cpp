// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-10: unit tests for OSCQuery v1 tree generation, stable 64-bit ids across
// track insertion, invalid UTF-8 replacement safety, and 1-to-1 SRF-09 address
// parity. HTTP/HOST_INFO/Host-header tests live in test_oscquery_http.cpp so
// each TU stays small and well within MSVC compiler heap limits.

#include "adi/osc_namespace.hpp"
#include "adi/oscquery.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

void runOscQueryHttpTests(int& checks, int& failures);

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

void collectLeafFullPaths(const Json& node, std::vector<std::string>& out) {
    if (!node.is_object()) {
        return;
    }
    if (node.contains("TYPE") && node.contains("FULL_PATH") && node.at("FULL_PATH").is_string()) {
        out.push_back(node.at("FULL_PATH").get<std::string>());
    }
    if (const auto it = node.find("CONTENTS"); it != node.end() && it->is_object()) {
        for (auto child = it->begin(); child != it->end(); ++child) {
            collectLeafFullPaths(child.value(), out);
        }
    }
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

void testGoldenTreeTwoTracksAndMain() {
    section("golden JSON tree: two tracks and Main");

    const auto desc = makeTwoTrackFixture();
    const auto tree = OscQueryTree::build(desc);

    static constexpr const char* kGoldenJson = R"json({
  "CONTENTS": {
    "adi": {
      "CONTENTS": {
        "master": {
          "CONTENTS": {
            "mute": {
              "ACCESS": 3, "DESCRIPTION": "Mute", "FULL_PATH": "/adi/master/mute",
              "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [0.0]
            },
            "pan": {
              "ACCESS": 3, "DESCRIPTION": "Pan: C", "FULL_PATH": "/adi/master/pan",
              "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [0.5]
            },
            "volume": {
              "ACCESS": 3, "DESCRIPTION": "Volume: 0.0 dB", "FULL_PATH": "/adi/master/volume",
              "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [1.0]
            }
          },
          "DESCRIPTION": "Main",
          "FULL_PATH": "/adi/master"
        },
        "track": {
          "CONTENTS": {
            "101": {
              "CONTENTS": {
                "arm": {
                  "ACCESS": 3, "DESCRIPTION": "Arm", "FULL_PATH": "/adi/track/101/arm",
                  "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [1.0]
                },
                "mute": {
                  "ACCESS": 3, "DESCRIPTION": "Mute", "FULL_PATH": "/adi/track/101/mute",
                  "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [0.0]
                },
                "pan": {
                  "ACCESS": 3, "DESCRIPTION": "Pan: C", "FULL_PATH": "/adi/track/101/pan",
                  "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [0.5]
                },
                "solo": {
                  "ACCESS": 3, "DESCRIPTION": "Solo", "FULL_PATH": "/adi/track/101/solo",
                  "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [0.0]
                },
                "volume": {
                  "ACCESS": 3, "DESCRIPTION": "Volume: -2.5 dB", "FULL_PATH": "/adi/track/101/volume",
                  "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [0.75]
                }
              },
              "DESCRIPTION": "Drums",
              "FULL_PATH": "/adi/track/101"
            },
            "102": {
              "CONTENTS": {
                "arm": {
                  "ACCESS": 3, "DESCRIPTION": "Arm", "FULL_PATH": "/adi/track/102/arm",
                  "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [0.0]
                },
                "mute": {
                  "ACCESS": 3, "DESCRIPTION": "Mute", "FULL_PATH": "/adi/track/102/mute",
                  "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [1.0]
                },
                "pan": {
                  "ACCESS": 3, "DESCRIPTION": "Pan: 25L", "FULL_PATH": "/adi/track/102/pan",
                  "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [0.25]
                },
                "solo": {
                  "ACCESS": 3, "DESCRIPTION": "Solo", "FULL_PATH": "/adi/track/102/solo",
                  "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [0.0]
                },
                "volume": {
                  "ACCESS": 3, "DESCRIPTION": "Volume: -6.0 dB", "FULL_PATH": "/adi/track/102/volume",
                  "RANGE": [{"MAX": 1.0, "MIN": 0.0}], "TYPE": "f", "VALUE": [0.5]
                }
              },
              "DESCRIPTION": "Bass",
              "FULL_PATH": "/adi/track/102"
            }
          },
          "DESCRIPTION": "Tracks",
          "FULL_PATH": "/adi/track"
        }
      },
      "DESCRIPTION": "ADI",
      "FULL_PATH": "/adi"
    }
  },
  "DESCRIPTION": "ADI",
  "FULL_PATH": "/"
})json";

    const Json expected = Json::parse(kGoldenJson);
    const std::string golden = expected.dump();
    check(tree.rootJson() == golden, "root JSON matches golden string exactly");

    const auto parsed = Json::parse(tree.rootJson());
    check(parsed == expected, "parsed root JSON equals golden object");
    check(!jsonHasKeyRecursive(parsed, "LISTEN"), "no LISTEN key anywhere in tree");
    check(tree.rootJson().find("LISTEN") == std::string::npos, "no LISTEN token in tree text");

    const auto volNode = tree.jsonForPath("/adi/track/101/volume");
    check(volNode.has_value(), "/adi/track/101/volume exists");
    if (volNode.has_value()) {
        const Json expectedVol = Json::parse(
            R"json({"ACCESS":3,"DESCRIPTION":"Volume: -2.5 dB","FULL_PATH":"/adi/track/101/volume","RANGE":[{"MAX":1.0,"MIN":0.0}],"TYPE":"f","VALUE":[0.75]})json");
        check(Json::parse(*volNode) == expectedVol, "subtree lookup matches leaf golden");
    }
}

void testStableIdsWhenTrackInsertedBefore() {
    section("stable ids when a track is inserted before existing tracks");

    auto beforeDesc = makeTwoTrackFixture();
    beforeDesc.tracks[0].sends.push_back(ParamDescriptor{501, "", "Send A", "-inf dB", 0.0f});
    DeviceDescriptor dev;
    dev.stableId = 701;
    dev.name = "Utility";
    dev.bypassed = false;
    dev.params.push_back(ParamDescriptor{901, "", "Gain", "0.0 dB", 0.5f});
    beforeDesc.tracks[0].devices.push_back(dev);

    const auto treeBefore = OscQueryTree::build(beforeDesc);

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

    check(treeAfter.contains("/adi/track/205/volume"), "new track 205 present");
}

void testInvalidUtf8ReplacedWithoutThrowing() {
    section("invalid UTF-8 in names replaced with U+FFFD without throwing");

    NamespaceDescription desc;
    desc.rootDescription = std::string("ADI-\xFF\xFE");

    TrackDescriptor t;
    t.stableId = 10;
    t.name = std::string("Vox\xE2\x82");        // truncated 3-byte UTF-8 sequence
    t.volume = 0.5f;
    t.volumeText = std::string("-6\xC0\xAF dB");  // overlong slash
    t.pan = 0.5f;
    t.panText = std::string("\xED\xA0\x80");      // lone UTF-16 surrogate in UTF-8

    DeviceDescriptor d;
    d.stableId = 20;
    d.name = std::string("Bad\x80Plugin");
    d.params.push_back(ParamDescriptor{30, "", std::string("Cut\xFE"), std::string("1\xFF kHz"),
                                       0.25f});
    t.devices.push_back(d);
    desc.tracks.push_back(t);

    desc.master.name = std::string("Main\xFF");

    bool threw = false;
    OscQueryTree tree;
    try {
        tree = OscQueryTree::build(desc);
    } catch (...) {
        threw = true;
    }
    check(!threw, "OscQueryTree::build() never throws on invalid UTF-8");
    check(!tree.rootJson().empty(), "rootJson produced");

    bool parseThrew = false;
    Json parsed;
    try {
        parsed = Json::parse(tree.rootJson());
    } catch (...) {
        parseThrew = true;
    }
    check(!parseThrew, "serialized rootJson is valid UTF-8 JSON");
    // UTF-8 encoding of U+FFFD is "\xEF\xBF\xBD".
    check(tree.rootJson().find("\xEF\xBF\xBD") != std::string::npos,
          "invalid UTF-8 bytes replaced with U+FFFD");

    // HostInfo with invalid UTF-8 also replaces rather than throwing.
    HostInfo hi;
    hi.name = std::string("Studio\xFF\x80");
    bool hiThrew = false;
    std::string hiJson;
    try {
        hiJson = renderHostInfoJson(hi);
    } catch (...) {
        hiThrew = true;
    }
    check(!hiThrew && hiJson.find("\xEF\xBF\xBD") != std::string::npos,
          "renderHostInfoJson replaces invalid UTF-8 without throwing");
}

void testSrf09AddressParity() {
    section("1-to-1 stable-id address parity with SRF-09 resolveAddress()");

    constexpr std::uint64_t kTrackId = (1ULL << 32) | 42ULL;
    constexpr std::uint64_t kSendId = (2ULL << 32) | 9ULL;
    constexpr std::uint64_t kDevId = (3ULL << 32) | 77ULL;
    constexpr std::uint64_t kParamId = (4ULL << 32) | 501ULL;
    constexpr std::uint64_t kMasterDevId = (5ULL << 32) | 12ULL;
    constexpr std::uint64_t kMasterParamId = (6ULL << 32) | 88ULL;

    NamespaceDescription desc;
    TrackDescriptor t;
    t.stableId = kTrackId;
    t.name = "Lead";
    t.sends.push_back(ParamDescriptor{kSendId, "", "Reverb", "-10 dB", 0.3f});
    DeviceDescriptor dev;
    dev.stableId = kDevId;
    dev.name = "Filter";
    dev.params.push_back(ParamDescriptor{kParamId, "", "Cutoff", "1 kHz", 0.5f});
    t.devices.push_back(dev);
    desc.tracks.push_back(t);

    DeviceDescriptor mdev;
    mdev.stableId = kMasterDevId;
    mdev.name = "Limiter";
    mdev.params.push_back(ParamDescriptor{kMasterParamId, "", "Ceiling", "-0.3 dB", 0.9f});
    desc.master.devices.push_back(mdev);

    const auto tree = OscQueryTree::build(desc);
    const Json root = Json::parse(tree.rootJson());
    std::vector<std::string> leaves;
    collectLeafFullPaths(root, leaves);

    check(leaves.size() == 13U, "collected all 13 leaf paths (track + send + devices + master)");
    for (const auto& leaf : leaves) {
        const auto resolved = adi::osc::resolveAddress(leaf);
        check(resolved.has_value(), "SRF-09 resolveAddress accepts SRF-10 leaf: " + leaf);
    }

    const auto pTrackParam = adi::osc::resolveAddress(
        "/adi/track/" + std::to_string(kTrackId) + "/device/" + std::to_string(kDevId) +
        "/param/" + std::to_string(kParamId));
    check(pTrackParam.has_value() && pTrackParam->kind == adi::osc::TargetKind::TrackDeviceParam &&
              pTrackParam->trackId == kTrackId && pTrackParam->deviceId == kDevId &&
              pTrackParam->paramId == kParamId,
          "SRF-09 decodes exact 64-bit stable track/device/param ids from SRF-10 path");

    const auto pMasterParam = adi::osc::resolveAddress(
        "/adi/master/device/" + std::to_string(kMasterDevId) + "/param/" +
        std::to_string(kMasterParamId));
    check(pMasterParam.has_value() &&
              pMasterParam->kind == adi::osc::TargetKind::MasterDeviceParam &&
              pMasterParam->deviceId == kMasterDevId && pMasterParam->paramId == kMasterParamId,
          "SRF-09 decodes exact 64-bit stable master device/param ids from SRF-10 path");
}

}  // namespace

int main() {
    testGoldenTreeTwoTracksAndMain();
    testStableIdsWhenTrackInsertedBefore();
    testInvalidUtf8ReplacedWithoutThrowing();
    testSrf09AddressParity();
    runOscQueryHttpTests(g_checks, g_failures);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
