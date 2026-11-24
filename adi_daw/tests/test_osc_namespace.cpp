// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-09: unit tests for the fixed OSC namespace (unified stable 64-bit ids),
// bind-interface & client allow-list policy, user + osc:<client> attribution,
// gesture brackets, momentary/delta rules, no-echo & confirmation feedback,
// and message-thread ownership contract.

#include "adi/osc_namespace.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace adi;
using namespace adi::osc;

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

struct RecordedWrite {
    TargetKind kind = TargetKind::TransportPlay;
    std::uint64_t track = 0;
    std::uint64_t send = 0;
    std::uint64_t device = 0;
    std::uint64_t param = 0;
    float fval = 0.0f;
    std::int32_t ival = 0;
    bool bval = false;
    WriteOrigin origin;
};

class RecordingSink final : public WriteSink {
public:
    std::vector<RecordedWrite> writes;

    void onTransportPlay(const WriteOrigin& o) override {
        writes.push_back({TargetKind::TransportPlay, 0, 0, 0, 0, 0.0f, 0, true, o});
    }
    void onTransportStop(const WriteOrigin& o) override {
        writes.push_back({TargetKind::TransportStop, 0, 0, 0, 0, 0.0f, 0, true, o});
    }
    void onTransportRecord(bool active, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TransportRecord, 0, 0, 0, 0, 0.0f, 0, active, o});
    }
    void onTransportLoop(bool enabled, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TransportLoop, 0, 0, 0, 0, 0.0f, 0, enabled, o});
    }
    void onTransportTempo(float bpm, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TransportTempo, 0, 0, 0, 0, bpm, 0, false, o});
    }
    void onReEnableAutomation(const WriteOrigin& o) override {
        writes.push_back({TargetKind::ReEnableAutomation, 0, 0, 0, 0, 0.0f, 0, true, o});
    }

    void onTrackVolume(std::uint64_t t, float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackVolume, t, 0, 0, 0, v, 0, false, o});
    }
    void onTrackVolumeGesture(std::uint64_t t, bool active, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackVolumeGesture, t, 0, 0, 0, 0.0f, 0, active, o});
    }
    void onTrackPan(std::uint64_t t, float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackPan, t, 0, 0, 0, v, 0, false, o});
    }
    void onTrackPanGesture(std::uint64_t t, bool active, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackPanGesture, t, 0, 0, 0, 0.0f, 0, active, o});
    }
    void onTrackMute(std::uint64_t t, bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackMute, t, 0, 0, 0, 0.0f, 0, v, o});
    }
    void onTrackSolo(std::uint64_t t, bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackSolo, t, 0, 0, 0, 0.0f, 0, v, o});
    }
    void onTrackArm(std::uint64_t t, bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackArm, t, 0, 0, 0, 0.0f, 0, v, o});
    }
    void onTrackSend(std::uint64_t t, std::uint64_t s, float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackSend, t, s, 0, 0, v, 0, false, o});
    }
    void onTrackSendGesture(std::uint64_t t, std::uint64_t s, bool active,
                            const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackSendGesture, t, s, 0, 0, 0.0f, 0, active, o});
    }
    void onTrackDeviceBypass(std::uint64_t t, std::uint64_t d, bool v,
                             const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackDeviceBypass, t, 0, d, 0, 0.0f, 0, v, o});
    }
    void onTrackDeviceParam(std::uint64_t t, std::uint64_t d, std::uint64_t p, float v,
                            const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackDeviceParam, t, 0, d, p, v, 0, false, o});
    }
    void onTrackDeviceParamNudge(std::uint64_t t, std::uint64_t d, std::uint64_t p,
                                 std::int32_t ticks, bool fine, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackDeviceParamNudge, t, 0, d, p, 0.0f, ticks, fine, o});
    }
    void onTrackDeviceParamGesture(std::uint64_t t, std::uint64_t d, std::uint64_t p,
                                   bool active, const WriteOrigin& o) override {
        writes.push_back(
            {TargetKind::TrackDeviceParamGesture, t, 0, d, p, 0.0f, 0, active, o});
    }

    void onMasterVolume(float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterVolume, 0, 0, 0, 0, v, 0, false, o});
    }
    void onMasterVolumeGesture(bool active, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterVolumeGesture, 0, 0, 0, 0, 0.0f, 0, active, o});
    }
    void onMasterPan(float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterPan, 0, 0, 0, 0, v, 0, false, o});
    }
    void onMasterPanGesture(bool active, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterPanGesture, 0, 0, 0, 0, 0.0f, 0, active, o});
    }
    void onMasterMute(bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterMute, 0, 0, 0, 0, 0.0f, 0, v, o});
    }
    void onMasterDeviceBypass(std::uint64_t d, bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterDeviceBypass, 0, 0, d, 0, 0.0f, 0, v, o});
    }
    void onMasterDeviceParam(std::uint64_t d, std::uint64_t p, float v,
                             const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterDeviceParam, 0, 0, d, p, v, 0, false, o});
    }
    void onMasterDeviceParamNudge(std::uint64_t d, std::uint64_t p, std::int32_t ticks,
                                  bool fine, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterDeviceParamNudge, 0, 0, d, p, 0.0f, ticks, fine, o});
    }
    void onMasterDeviceParamGesture(std::uint64_t d, std::uint64_t p, bool active,
                                    const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterDeviceParamGesture, 0, 0, d, p, 0.0f, 0, active, o});
    }

    void onFocusedParam(std::uint64_t p, float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedParam, 0, 0, 0, p, v, 0, false, o});
    }
    void onFocusedParamNudge(std::uint64_t p, std::int32_t ticks, bool fine,
                             const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedParamNudge, 0, 0, 0, p, 0.0f, ticks, fine, o});
    }
    void onFocusedParamGesture(std::uint64_t p, bool active, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedParamGesture, 0, 0, 0, p, 0.0f, 0, active, o});
    }
    void onFocusedMacro(std::uint64_t m, float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedMacro, 0, 0, 0, m, v, 0, false, o});
    }
    void onFocusedMacroGesture(std::uint64_t m, bool active, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedMacroGesture, 0, 0, 0, m, 0.0f, 0, active, o});
    }
    void onFocusedBypass(bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedBypass, 0, 0, 0, 0, 0.0f, 0, v, o});
    }
    void onFocusedDial(float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedDial, 0, 0, 0, 0, v, 0, false, o});
    }
    void onFocusedDialNudge(std::int32_t ticks, bool fine, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedDialNudge, 0, 0, 0, 0, 0.0f, ticks, fine, o});
    }
    void onFocusedDialGesture(bool active, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedDialGesture, 0, 0, 0, 0, 0.0f, 0, active, o});
    }
};

Message msgF(std::string addr, float v) {
    return Message{std::move(addr), {Argument{v}}};
}

Message msgI(std::string addr, std::int32_t v) {
    return Message{std::move(addr), {Argument{v}}};
}

const RecordedWrite* findFirst(const std::vector<RecordedWrite>& writes, TargetKind kind) {
    for (const auto& w : writes) {
        if (w.kind == kind) {
            return &w;
        }
    }
    return nullptr;
}

void testAllNamespaceAddressesAndStableIds() {
    section("namespace resolution, 64-bit stable ids, and osc:<client> attribution");

    ClientPolicy pol;
    pol.enabled = true;
    OscNamespace ns(pol);
    RecordingSink sink;
    const std::string client = "192.168.1.42";

    // High 64-bit stable ids (matching CO-01 (space << 32) | counter and SRF-10).
    constexpr std::uint64_t kTrackId = (1ULL << 32) | 42ULL;
    constexpr std::uint64_t kSendId = (2ULL << 32) | 7ULL;
    constexpr std::uint64_t kDevId = (3ULL << 32) | 99ULL;
    constexpr std::uint64_t kParamId = (4ULL << 32) | 1001ULL;
    const std::string tStr = std::to_string(kTrackId);
    const std::string sStr = std::to_string(kSendId);
    const std::string dStr = std::to_string(kDevId);
    const std::string pStr = std::to_string(kParamId);

    const std::vector<Message> messages = {
        // /adi/transport
        msgF("/adi/transport/play", 1.0f),
        msgF("/adi/transport/stop", 1.0f),
        msgI("/adi/transport/record", 1),
        msgI("/adi/transport/loop", 0),
        msgF("/adi/transport/tempo", 128.5f),
        msgF("/adi/transport/re_enable_automation", 1.0f),
        // /adi/track/{stableId}
        msgF("/adi/track/" + tStr + "/volume", 0.8f),
        msgF("/adi/track/" + tStr + "/pan", 0.25f),
        msgI("/adi/track/" + tStr + "/mute", 1),
        msgI("/adi/track/" + tStr + "/solo", 1),
        msgI("/adi/track/" + tStr + "/arm", 1),
        msgF("/adi/track/" + tStr + "/send/" + sStr, 0.6f),
        msgI("/adi/track/" + tStr + "/device/" + dStr + "/bypass", 1),
        msgF("/adi/track/" + tStr + "/device/" + dStr + "/param/" + pStr, 0.42f),
        Message{"/adi/track/" + tStr + "/device/" + dStr + "/param/" + pStr + "/nudge",
                {Argument{std::int32_t{-2}}, Argument{std::int32_t{1}}}},
        msgI("/adi/track/" + tStr + "/device/" + dStr + "/param/" + pStr + "/gesture", 1),
        msgI("/adi/track/" + tStr + "/device/" + dStr + "/param/" + pStr + "/gesture", 0),
        // /adi/master
        msgF("/adi/master/volume", 0.9f),
        msgF("/adi/master/pan", 0.5f),
        msgI("/adi/master/mute", 0),
        msgI("/adi/master/device/" + dStr + "/bypass", 0),
        msgF("/adi/master/device/" + dStr + "/param/" + pStr, 0.33f),
        msgI("/adi/master/device/" + dStr + "/param/" + pStr + "/nudge", 5),
        msgI("/adi/master/device/" + dStr + "/param/" + pStr + "/gesture", 0),
        // /adi/focused
        msgF("/adi/focused/param/" + pStr, 0.7f),
        msgI("/adi/focused/param/" + pStr + "/nudge", 1),
        msgI("/adi/focused/param/" + pStr + "/gesture", 1),
        msgI("/adi/focused/param/" + pStr + "/gesture", 0),
        msgF("/adi/focused/macro/8", 0.15f),
        msgI("/adi/focused/bypass", 1),
        msgF("/adi/focused/dial", 0.55f),
        Message{"/adi/focused/dial/nudge", {Argument{std::int32_t{3}}, Argument{1.0f}}},
        msgI("/adi/focused/dial/gesture", 0),
    };

    for (const auto& m : messages) {
        const auto st = ns.dispatchMessage(m, client, sink);
        check(st == DispatchStatus::Applied, "applied " + m.address);
    }

    for (std::size_t i = 0U; i < sink.writes.size(); ++i) {
        const auto& o = sink.writes[i].origin;
        check(o.actor == Actor::User, "write[" + std::to_string(i) + "] actor == User");
        check(o.actorDetail == "osc:192.168.1.42",
              "write[" + std::to_string(i) + "] actorDetail == osc:<client>");
        check(o.client == client, "write[" + std::to_string(i) + "] client matches");
    }

    const auto* tempo = findFirst(sink.writes, TargetKind::TransportTempo);
    check(tempo && tempo->fval == 128.5f, "tempo value");
    const auto* send = findFirst(sink.writes, TargetKind::TrackSend);
    check(send && send->track == kTrackId && send->send == kSendId && send->fval == 0.6f,
          "track send 64-bit stable ids and value");
    const auto* nudge = findFirst(sink.writes, TargetKind::TrackDeviceParamNudge);
    check(nudge && nudge->track == kTrackId && nudge->device == kDevId &&
              nudge->param == kParamId && nudge->ival == -2 && nudge->bval,
          "track device param nudge 64-bit ids and fine=true");
    const auto* dialNudge = findFirst(sink.writes, TargetKind::FocusedDialNudge);
    check(dialNudge && dialNudge->ival == 3 && dialNudge->bval, "focused dial nudge");
}

void testUnknownAddressesAndInvalidArgs() {
    section("unknown addresses and invalid arguments refused");

    ClientPolicy pol;
    pol.enabled = true;
    OscNamespace ns(pol);
    RecordingSink sink;

    const std::vector<std::string> badAddresses = {
        "/",
        "/adi",
        "/adi/unknown",
        "/other/track/1/volume",
        "/adi/transport/pause",
        "/adi/transport/play/extra",
        "/adi/track/0/volume",
        "/adi/track/01/volume",
        "/adi/track/-1/volume",
        "/adi/track/abc/volume",
        "/adi/track/18446744073709551616/volume",  // UINT64_MAX + 1 overflow
        "/adi/track/1/delete",
        "/adi/track/1/volume/",
        "/adi/track/1/mute/gesture",
        "/adi/track/1/send/0",
        "/adi/track/1/device/0/param/1",
        "/adi/track/1/device/1/param/0",
        "/adi/master/arm",
        "/adi/master/device/01/bypass",
        "/adi/focused/macro/0",
        "/adi/focused/unknown",
    };

    for (const auto& addr : badAddresses) {
        check(!resolveAddress(addr).has_value(), "resolveAddress refuses " + addr);
        const auto st = ns.dispatchMessage(msgF(addr, 0.5f), "10.0.0.5", sink);
        check(st == DispatchStatus::UnknownAddress, "dispatch refuses unknown " + addr);
    }

    // UINT64_MAX itself is valid.
    const auto maxIdRes = resolveAddress("/adi/track/18446744073709551615/volume");
    check(maxIdRes.has_value() &&
              maxIdRes->trackId == (std::numeric_limits<std::uint64_t>::max)(),
          "UINT64_MAX stable id accepted");

    const float nanVal = std::numeric_limits<float>::quiet_NaN();
    const float infVal = std::numeric_limits<float>::infinity();
    const std::vector<Message> badArgs = {
        Message{"/adi/master/volume", {}},
        Message{"/adi/master/volume", {Argument{0.5f}, Argument{0.5f}}},
        Message{"/adi/master/volume", {Argument{std::string{"0.5"}}}},
        Message{"/adi/master/volume", {Argument{Blob{std::byte{1}}}}},
        msgF("/adi/master/volume", nanVal),
        msgF("/adi/master/volume", infVal),
        msgF("/adi/master/volume", -infVal),
        msgF("/adi/transport/tempo", 10.0f),
        msgF("/adi/transport/tempo", 1500.0f),
        msgF("/adi/transport/tempo", nanVal),
        Message{"/adi/transport/play", {Argument{std::string{"1"}}}},
        Message{"/adi/focused/dial/nudge", {Argument{std::string{"+1"}}}},
    };

    for (const auto& m : badArgs) {
        const auto st = ns.dispatchMessage(m, "10.0.0.5", sink);
        check(st == DispatchStatus::InvalidArgument, "invalid arg refused for " + m.address);
    }

    check(sink.writes.empty(), "zero writes emitted on all refused messages");
}

void testMomentaryPlayAndUncoalescedDeltas() {
    section("momentary play release and uncoalesced deltas");

    ClientPolicy pol;
    pol.enabled = true;
    OscNamespace ns(pol);
    RecordingSink sink;

    check(ns.dispatchMessage(msgF("/adi/transport/play", 1.0f), "ipad", sink) ==
              DispatchStatus::Applied,
          "play press applied");
    check(ns.dispatchMessage(msgF("/adi/transport/play", 0.0f), "ipad", sink) ==
              DispatchStatus::IgnoredRelease,
          "play release ignored");
    check(ns.dispatchMessage(msgI("/adi/transport/play", 0), "ipad", sink) ==
              DispatchStatus::IgnoredRelease,
          "play int 0 release ignored");
    check(sink.writes.size() == 1U && sink.writes[0].kind == TargetKind::TransportPlay,
          "only TransportPlay fired, never TransportStop");

    // Relative deltas in one bundle are never coalesced (inside an explicit gesture).
    sink.writes.clear();
    check(ns.dispatchMessage(msgI("/adi/focused/dial/gesture", 1), "ipad", sink) ==
              DispatchStatus::Applied,
          "dial gesture begin");
    Bundle b;
    b.timeTag = TimeTag{1ULL};
    b.elements.push_back(Packet{msgI("/adi/focused/dial/nudge", 1)});
    b.elements.push_back(Packet{msgI("/adi/focused/dial/nudge", 1)});
    b.elements.push_back(Packet{msgI("/adi/focused/dial/nudge", -1)});
    const auto res = ns.dispatchPacket(Packet{b}, "ipad", sink);
    check(ns.dispatchMessage(msgI("/adi/focused/dial/gesture", 0), "ipad", sink) ==
              DispatchStatus::Applied,
          "dial gesture end");
    check(res.accepted() && res.appliedCount == 3U && res.refusedCount == 0U,
          "all 3 nudges in bundle applied");
    check(sink.writes.size() == 5U,
          "explicit gesture begin + 3 distinct nudges + gesture end (never coalesced)");
    if (sink.writes.size() == 5U) {
        check(sink.writes[0].kind == TargetKind::FocusedDialGesture && sink.writes[0].bval,
              "gesture begin first");
        check(sink.writes[1].kind == TargetKind::FocusedDialNudge && sink.writes[1].ival == 1 &&
                  sink.writes[2].kind == TargetKind::FocusedDialNudge && sink.writes[2].ival == 1 &&
                  sink.writes[3].kind == TargetKind::FocusedDialNudge && sink.writes[3].ival == -1,
              "nudge order and signs preserved");
        check(sink.writes[4].kind == TargetKind::FocusedDialGesture && !sink.writes[4].bval,
              "gesture end last");
    }
}

void testStripGestureBrackets() {
    section("ADR-0181 d2 gesture brackets on strip writes");

    ClientPolicy pol;
    pol.enabled = true;
    OscNamespace ns(pol);
    RecordingSink sink;

    // 1. Single-shot strip writes auto-bracket with begin + write + end.
    check(ns.dispatchMessage(msgF("/adi/track/42/volume", 0.7f), "touchosc", sink) ==
              DispatchStatus::Applied,
          "single-shot track volume applied");
    check(sink.writes.size() == 3U, "single-shot track volume emits begin/write/end");
    if (sink.writes.size() == 3U) {
        check(sink.writes[0].kind == TargetKind::TrackVolumeGesture &&
                  sink.writes[0].track == 42U && sink.writes[0].bval,
              "auto gesture begin for track 42 volume");
        check(sink.writes[1].kind == TargetKind::TrackVolume && sink.writes[1].track == 42U &&
                  sink.writes[1].fval == 0.7f,
              "track 42 volume write");
        check(sink.writes[2].kind == TargetKind::TrackVolumeGesture &&
                  sink.writes[2].track == 42U && !sink.writes[2].bval,
              "auto gesture end for track 42 volume");
    }

    // 2. Explicit strip touch gesture holds across multiple writes without double-bracketing.
    sink.writes.clear();
    check(ns.dispatchMessage(msgI("/adi/track/42/pan/gesture", 1), "touchosc", sink) ==
              DispatchStatus::Applied,
          "explicit pan gesture begin");
    check(ns.dispatchMessage(msgF("/adi/track/42/pan", 0.3f), "touchosc", sink) ==
              DispatchStatus::Applied,
          "pan move 1 inside gesture");
    check(ns.dispatchMessage(msgF("/adi/track/42/pan", 0.6f), "touchosc", sink) ==
              DispatchStatus::Applied,
          "pan move 2 inside gesture");
    check(ns.dispatchMessage(msgI("/adi/track/42/pan/gesture", 0), "touchosc", sink) ==
              DispatchStatus::Applied,
          "explicit pan gesture end");
    check(sink.writes.size() == 4U,
          "explicit pan gesture emits [begin, move1, move2, end] without inner brackets");
    if (sink.writes.size() == 4U) {
        check(sink.writes[0].kind == TargetKind::TrackPanGesture && sink.writes[0].bval, "begin");
        check(sink.writes[1].kind == TargetKind::TrackPan && sink.writes[1].fval == 0.3f, "move1");
        check(sink.writes[2].kind == TargetKind::TrackPan && sink.writes[2].fval == 0.6f, "move2");
        check(sink.writes[3].kind == TargetKind::TrackPanGesture && !sink.writes[3].bval, "end");
    }

    // 3. Single-shot send, master volume, master pan, and focused macro also auto-bracket.
    sink.writes.clear();
    check(ns.dispatchMessage(msgF("/adi/track/42/send/9", 0.4f), "touchosc", sink) ==
              DispatchStatus::Applied,
          "single-shot send applied");
    check(sink.writes.size() == 3U && sink.writes[0].kind == TargetKind::TrackSendGesture &&
              sink.writes[0].bval && sink.writes[1].kind == TargetKind::TrackSend &&
              sink.writes[2].kind == TargetKind::TrackSendGesture && !sink.writes[2].bval,
          "send auto-bracketed");

    sink.writes.clear();
    check(ns.dispatchMessage(msgF("/adi/master/volume", 0.8f), "touchosc", sink) ==
              DispatchStatus::Applied,
          "single-shot master volume applied");
    check(sink.writes.size() == 3U && sink.writes[0].kind == TargetKind::MasterVolumeGesture &&
              sink.writes[0].bval && sink.writes[1].kind == TargetKind::MasterVolume &&
              sink.writes[2].kind == TargetKind::MasterVolumeGesture && !sink.writes[2].bval,
          "master volume auto-bracketed");

    sink.writes.clear();
    check(ns.dispatchMessage(msgF("/adi/focused/macro/3", 0.5f), "touchosc", sink) ==
              DispatchStatus::Applied,
          "single-shot focused macro applied");
    check(sink.writes.size() == 3U && sink.writes[0].kind == TargetKind::FocusedMacroGesture &&
              sink.writes[0].bval && sink.writes[1].kind == TargetKind::FocusedMacro &&
              sink.writes[2].kind == TargetKind::FocusedMacroGesture && !sink.writes[2].bval,
          "focused macro auto-bracketed");

    // 4. Client disconnect cleanup closes open gestures cleanly.
    sink.writes.clear();
    check(ns.dispatchMessage(msgI("/adi/master/pan/gesture", 1), "touchosc", sink) ==
              DispatchStatus::Applied,
          "open master pan gesture");
    check(ns.dispatchMessage(msgI("/adi/track/42/send/9/gesture", 1), "touchosc", sink) ==
              DispatchStatus::Applied,
          "open send gesture");
    sink.writes.clear();
    const std::size_t closed = ns.endClientGestures("touchosc", sink);
    check(closed == 2U && sink.writes.size() == 2U, "endClientGestures closes both open gestures");
    for (const auto& w : sink.writes) {
        check(!w.bval && w.origin.actorDetail == "osc:touchosc",
              "cleanup gesture end carries active=false and osc:touchosc");
    }
    check(ns.endAllGestures(sink) == 0U, "no remaining open gestures");
}

void testBindInterfacePolicyAndAllowList() {
    section("bind-interface policy, default-off gate, and client allow-list");

    // Default policy is off, loopback 127.0.0.1:9000.
    {
        OscNamespace ns;
        RecordingSink sink;
        check(!ns.policy().enabled, "OSC is off by default");
        check(ns.policy().bind.interfaceAddress == "127.0.0.1", "default bind is loopback");
        check(ns.policy().bindValid(), "default loopback bind policy is valid");
        check(ns.dispatchMessage(msgF("/adi/master/volume", 0.5f), "192.168.1.10", sink) ==
                  DispatchStatus::Disabled,
              "disabled service refuses write");
        check(sink.writes.empty(), "no writes while disabled");
    }

    // BindPolicy validation rules (ADR-0185 d4, ADR-0188 d7).
    {
        BindPolicy bp;
        check(validateBindPolicy(bp) == BindPolicyStatus::Ok, "127.0.0.1:9000 ok");
        bp.interfaceAddress = "::1";
        check(validateBindPolicy(bp) == BindPolicyStatus::Ok, "::1 loopback ok");
        bp.interfaceAddress = "localhost";
        check(validateBindPolicy(bp) == BindPolicyStatus::Ok, "localhost loopback ok");

        bp.interfaceAddress = "";
        check(validateBindPolicy(bp) == BindPolicyStatus::EmptyInterface, "empty interface refused");
        bp.interfaceAddress = "127.0.0.1 ";
        check(validateBindPolicy(bp) == BindPolicyStatus::InvalidInterface,
              "whitespace in interface refused");

        // Wildcard binds always refused, even with trustedNetworkAcknowledged.
        bp.trustedNetworkAcknowledged = true;
        for (const char* wild : {"0.0.0.0", "::", "[::]", "*"}) {
            bp.interfaceAddress = wild;
            check(validateBindPolicy(bp) == BindPolicyStatus::WildcardInterfaceRefused,
                  std::string("wildcard refused: ") + wild);
        }

        // Non-loopback requires trustedNetworkAcknowledged.
        bp.interfaceAddress = "192.168.1.20";
        bp.trustedNetworkAcknowledged = false;
        check(validateBindPolicy(bp) == BindPolicyStatus::NonLoopbackRequiresTrustedAck,
              "LAN interface refused without trusted-network ack");
        bp.trustedNetworkAcknowledged = true;
        check(validateBindPolicy(bp) == BindPolicyStatus::Ok,
              "LAN interface accepted with trusted-network ack");

        // Privileged ports refused.
        bp.udpPort = 80U;
        check(validateBindPolicy(bp) == BindPolicyStatus::InvalidPort, "port 80 refused");
        bp.udpPort = 0U;
        check(validateBindPolicy(bp) == BindPolicyStatus::InvalidPort, "port 0 refused");

        // Dispatcher & flush fail closed when bind policy is invalid.
        ClientPolicy pol;
        pol.enabled = true;
        pol.bind.interfaceAddress = "0.0.0.0";
        pol.bind.trustedNetworkAcknowledged = true;
        OscNamespace ns(pol);
        ns.addFeedbackPeer("192.168.1.10");
        ns.updateFeedValue("/adi/master/volume", 0.5f);
        RecordingSink sink;
        check(ns.dispatchMessage(msgF("/adi/master/volume", 0.5f), "192.168.1.10", sink) ==
                  DispatchStatus::InvalidBindPolicy,
              "invalid bind policy refuses dispatchMessage");
        const auto pr = ns.dispatchPacket(Packet{msgF("/adi/master/volume", 0.5f)},
                                          "192.168.1.10", sink);
        check(!pr.accepted() && pr.gateStatus == DispatchStatus::InvalidBindPolicy,
              "invalid bind policy refuses dispatchPacket");
        check(ns.flushFeedback().empty(), "invalid bind policy emits no feedback");
    }

    // Enabled with allow-list: listed client accepted, unlisted refused.
    {
        ClientPolicy pol;
        pol.enabled = true;
        pol.bind.interfaceAddress = "192.168.1.20";
        pol.bind.trustedNetworkAcknowledged = true;
        pol.allowedClients = {"192.168.1.10", "192.168.1.11"};
        OscNamespace ns(pol);
        RecordingSink sink;

        check(ns.dispatchMessage(msgF("/adi/master/volume", 0.5f), "", sink) ==
                  DispatchStatus::InvalidClient,
              "empty client refused");
        check(ns.dispatchMessage(msgF("/adi/master/volume", 0.5f), "192.168.1.99", sink) ==
                  DispatchStatus::ClientNotAllowed,
              "unlisted client refused");
        check(sink.writes.empty(), "no writes from unlisted client");

        Bundle b;
        b.elements.push_back(Packet{msgF("/adi/master/volume", 0.5f)});
        const auto pr = ns.dispatchPacket(Packet{b}, "192.168.1.99", sink);
        check(!pr.accepted() && pr.gateStatus == DispatchStatus::ClientNotAllowed &&
                  sink.writes.empty(),
              "bundle from unlisted client refused");

        check(ns.dispatchMessage(msgF("/adi/master/volume", 0.5f), "192.168.1.10", sink) ==
                  DispatchStatus::Applied,
              "listed client accepted");
        check(sink.writes.size() == 3U &&
                  sink.writes[1].origin.actorDetail == "osc:192.168.1.10",
              "listed client stamped osc:<client>");
    }
}

void testNoEchoNudgeAndRefusedWriteConfirmation() {
    section("no-echo feedback, nudge feed suppression, and refused/unchanged confirmation");

    ClientPolicy pol;
    pol.enabled = true;
    pol.allowedClients = {"clientA", "clientB"};
    OscNamespace ns(pol);
    ns.addFeedbackPeer("clientA");
    ns.addFeedbackPeer("clientB");
    ns.addFeedbackPeer("unlistedC");

    RecordingSink sink;

    // Initial state flushed to both allowed peers.
    check(ns.updateFeedValue("/adi/track/42/volume", 0.25f), "initial track volume feed");
    check(ns.updateFeedValue("/adi/track/42/device/7/param/3", 0.50f), "initial param feed");
    check(ns.updateFeedValue("/adi/focused/dial", 0.40f), "initial dial feed");
    check(!ns.updateFeedValue("/adi/track/42/volume", std::numeric_limits<float>::quiet_NaN()),
          "NaN feed value refused");
    {
        const auto fb = ns.flushFeedback();
        check(fb.size() == 6U, "initial flush sends all 3 values to both allowed peers only");
    }
    check(ns.flushFeedback().empty(), "steady state emits no feedback");

    // 1. Nudge MUST suppress the base parameter feed address (not ".../nudge"),
    // so clientA gets NO echo in Flush 1 while clientB receives it immediately.
    check(ns.dispatchMessage(msgI("/adi/track/42/device/7/param/3/nudge", 2), "clientA", sink) ==
              DispatchStatus::Applied,
          "param nudge applied");
    ns.updateFeedValue("/adi/track/42/device/7/param/3", 0.55f);
    {
        const auto fb1 = ns.flushFeedback();
        check(fb1.size() == 1U, "nudge flush 1 suppresses echo of base param address to clientA");
        if (fb1.size() == 1U) {
            check(fb1[0] == FeedbackMessage{"clientB", "/adi/track/42/device/7/param/3", 0.55f},
                  "nudge flush 1 delivers updated param to clientB");
        }
    }
    {
        const auto fb2 = ns.flushFeedback();
        check(fb2.size() == 1U &&
                  fb2[0] == FeedbackMessage{"clientA", "/adi/track/42/device/7/param/3", 0.55f},
              "nudge flush 2 confirms settled param value to clientA");
    }
    check(ns.flushFeedback().empty(), "nudge flush 3 quiet");

    // Focused dial nudge also suppresses /adi/focused/dial (even when unchanged / clamped!).
    check(ns.dispatchMessage(msgI("/adi/focused/dial/nudge", 5), "clientA", sink) ==
              DispatchStatus::Applied,
          "dial nudge applied");
    // Feed remains unchanged at 0.40f (e.g. clamped or locked).
    {
        const auto fb1 = ns.flushFeedback();
        check(fb1.empty(), "unchanged dial nudge flush 1 emits no echo to clientA");
    }
    {
        const auto fb2 = ns.flushFeedback();
        check(fb2.size() == 1U && fb2[0] == FeedbackMessage{"clientA", "/adi/focused/dial", 0.40f},
              "unchanged dial nudge flush 2 confirms true value 0.40 back to clientA");
    }
    check(ns.flushFeedback().empty(), "unchanged dial nudge flush 3 quiet");

    // 2. Unchanged or host-refused absolute write MUST be confirmed to sender on next flush.
    check(ns.dispatchMessage(msgF("/adi/track/42/volume", 0.95f), "clientA", sink) ==
              DispatchStatus::Applied,
          "volume write applied to sink");
    // Host refuses or leaves feed unchanged at 0.25f (lastSent_ for clientA is already 0.25f!).
    {
        const auto fb1 = ns.flushFeedback();
        check(fb1.empty(), "refused/unchanged write flush 1 suppresses echo to clientA");
    }
    {
        const auto fb2 = ns.flushFeedback();
        check(fb2.size() == 1U &&
                  fb2[0] == FeedbackMessage{"clientA", "/adi/track/42/volume", 0.25f},
              "refused/unchanged write flush 2 confirms true value 0.25 back to clientA");
    }
    check(ns.flushFeedback().empty(), "refused/unchanged write flush 3 quiet");

    // 3. Dispatcher-refused write (InvalidArgument on a valid feed address) ALSO confirms true
    // value on the next flush so a misbehaving sender re-syncs.
    check(ns.dispatchMessage(Message{"/adi/track/42/volume", {Argument{std::string{"bad"}}}},
                             "clientA", sink) == DispatchStatus::InvalidArgument,
          "invalid argument write refused");
    {
        const auto fb1 = ns.flushFeedback();
        check(fb1.empty(), "invalid-arg write flush 1 emits nothing");
    }
    {
        const auto fb2 = ns.flushFeedback();
        check(fb2.size() == 1U &&
                  fb2[0] == FeedbackMessage{"clientA", "/adi/track/42/volume", 0.25f},
              "invalid-arg write flush 2 confirms true value 0.25 to clientA");
    }
    check(ns.flushFeedback().empty(), "invalid-arg write flush 3 quiet");

    // 4. Continuous drag over two consecutive flushes: zero echo while dragging, confirmed after.
    check(ns.dispatchMessage(msgF("/adi/track/42/volume", 0.50f), "clientA", sink) ==
              DispatchStatus::Applied,
          "drag step 1");
    ns.updateFeedValue("/adi/track/42/volume", 0.50f);
    {
        const auto fb = ns.flushFeedback();
        check(fb.size() == 1U && fb[0] == FeedbackMessage{"clientB", "/adi/track/42/volume", 0.50f},
              "drag flush 1: no echo to A, live to B");
    }
    check(ns.dispatchMessage(msgF("/adi/track/42/volume", 0.60f), "clientA", sink) ==
              DispatchStatus::Applied,
          "drag step 2");
    ns.updateFeedValue("/adi/track/42/volume", 0.60f);
    {
        const auto fb = ns.flushFeedback();
        check(fb.size() == 1U && fb[0] == FeedbackMessage{"clientB", "/adi/track/42/volume", 0.60f},
              "drag flush 2: still no echo to A, live to B");
    }
    {
        const auto fb = ns.flushFeedback();
        check(fb.size() == 1U && fb[0] == FeedbackMessage{"clientA", "/adi/track/42/volume", 0.60f},
              "post-drag flush confirms final value to A");
    }
    check(ns.flushFeedback().empty(), "post-drag next flush quiet");

    // 5. External change reaches both peers.
    ns.updateFeedValue("/adi/track/42/volume", 0.40f);
    {
        const auto fb = ns.flushFeedback();
        check(fb.size() == 2U, "external change reaches both clientA and clientB");
    }
}

void testThreadingContract() {
    section("message-thread ownership contract between dispatch and flush");

    ClientPolicy pol;
    pol.enabled = true;
    OscNamespace ns(pol);
    ns.addFeedbackPeer("clientA");
    check(ns.isOnOwnerThread(), "constructed on owning message thread");
    check(ns.updateFeedValue("/adi/master/volume", 0.5f), "owner thread updates feed");

    DispatchStatus offThreadMsg = DispatchStatus::Applied;
    DispatchStatus offThreadPkt = DispatchStatus::Applied;
    bool offThreadFeed = true;
    std::size_t offThreadFlushCount = 99U;
    bool offThreadWritesEmpty = false;

    std::thread worker([&]() {
        RecordingSink workerSink;
        offThreadMsg = ns.dispatchMessage(msgF("/adi/master/volume", 0.9f), "clientA", workerSink);
        offThreadPkt = ns.dispatchPacket(Packet{msgF("/adi/master/volume", 0.9f)},
                                         "clientA", workerSink)
                           .gateStatus;
        offThreadFeed = ns.updateFeedValue("/adi/master/volume", 0.9f);
        offThreadFlushCount = ns.flushFeedback().size();
        offThreadWritesEmpty = workerSink.writes.empty();
    });
    worker.join();

    check(offThreadMsg == DispatchStatus::WrongThread,
          "off-thread dispatchMessage fails closed with WrongThread");
    check(offThreadPkt == DispatchStatus::WrongThread,
          "off-thread dispatchPacket fails closed with WrongThread");
    check(!offThreadFeed, "off-thread updateFeedValue returns false");
    check(offThreadFlushCount == 0U, "off-thread flushFeedback returns empty");
    check(offThreadWritesEmpty, "off-thread calls invoke zero sink callbacks");

    // Owner thread state was untouched and flushes the original 0.5f value cleanly.
    const auto fb = ns.flushFeedback();
    check(fb.size() == 1U && fb[0] == FeedbackMessage{"clientA", "/adi/master/volume", 0.5f},
          "owner thread state intact after rejected off-thread calls");

    // Explicit hand-off via adoptCurrentThread() transfers ownership cleanly.
    DispatchStatus adoptedStatus = DispatchStatus::WrongThread;
    std::thread messageThread([&]() {
        ns.adoptCurrentThread();
        RecordingSink sink;
        adoptedStatus = ns.dispatchMessage(msgF("/adi/master/volume", 0.75f), "clientA", sink);
    });
    messageThread.join();
    check(adoptedStatus == DispatchStatus::Applied, "adoptCurrentThread transfers ownership");
}

}  // namespace

int main() {
    testAllNamespaceAddressesAndStableIds();
    testUnknownAddressesAndInvalidArgs();
    testMomentaryPlayAndUncoalescedDeltas();
    testStripGestureBrackets();
    testBindInterfacePolicyAndAllowList();
    testNoEchoNudgeAndRefusedWriteConfirmation();
    testThreadingContract();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
