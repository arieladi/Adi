// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-09: unit tests for the fixed OSC namespace, client allow-list,
// user + osc:<client> attribution, momentary/delta rules, and no-echo feedback.

#include "adi/osc_namespace.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
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
    std::uint32_t track = 0;
    std::uint32_t send = 0;
    std::uint32_t device = 0;
    std::uint32_t param = 0;
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

    void onTrackVolume(std::uint32_t t, float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackVolume, t, 0, 0, 0, v, 0, false, o});
    }
    void onTrackPan(std::uint32_t t, float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackPan, t, 0, 0, 0, v, 0, false, o});
    }
    void onTrackMute(std::uint32_t t, bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackMute, t, 0, 0, 0, 0.0f, 0, v, o});
    }
    void onTrackSolo(std::uint32_t t, bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackSolo, t, 0, 0, 0, 0.0f, 0, v, o});
    }
    void onTrackArm(std::uint32_t t, bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackArm, t, 0, 0, 0, 0.0f, 0, v, o});
    }
    void onTrackSend(std::uint32_t t, std::uint32_t s, float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackSend, t, s, 0, 0, v, 0, false, o});
    }
    void onTrackDeviceBypass(std::uint32_t t, std::uint32_t d, bool v,
                             const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackDeviceBypass, t, 0, d, 0, 0.0f, 0, v, o});
    }
    void onTrackDeviceParam(std::uint32_t t, std::uint32_t d, std::uint32_t p, float v,
                            const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackDeviceParam, t, 0, d, p, v, 0, false, o});
    }
    void onTrackDeviceParamNudge(std::uint32_t t, std::uint32_t d, std::uint32_t p,
                                 std::int32_t ticks, bool fine, const WriteOrigin& o) override {
        writes.push_back({TargetKind::TrackDeviceParamNudge, t, 0, d, p, 0.0f, ticks, fine, o});
    }
    void onTrackDeviceParamGesture(std::uint32_t t, std::uint32_t d, std::uint32_t p,
                                   bool active, const WriteOrigin& o) override {
        writes.push_back(
            {TargetKind::TrackDeviceParamGesture, t, 0, d, p, 0.0f, 0, active, o});
    }

    void onMasterVolume(float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterVolume, 0, 0, 0, 0, v, 0, false, o});
    }
    void onMasterPan(float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterPan, 0, 0, 0, 0, v, 0, false, o});
    }
    void onMasterMute(bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterMute, 0, 0, 0, 0, 0.0f, 0, v, o});
    }
    void onMasterDeviceBypass(std::uint32_t d, bool v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterDeviceBypass, 0, 0, d, 0, 0.0f, 0, v, o});
    }
    void onMasterDeviceParam(std::uint32_t d, std::uint32_t p, float v,
                             const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterDeviceParam, 0, 0, d, p, v, 0, false, o});
    }
    void onMasterDeviceParamNudge(std::uint32_t d, std::uint32_t p, std::int32_t ticks,
                                  bool fine, const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterDeviceParamNudge, 0, 0, d, p, 0.0f, ticks, fine, o});
    }
    void onMasterDeviceParamGesture(std::uint32_t d, std::uint32_t p, bool active,
                                    const WriteOrigin& o) override {
        writes.push_back({TargetKind::MasterDeviceParamGesture, 0, 0, d, p, 0.0f, 0, active, o});
    }

    void onFocusedParam(std::uint32_t p, float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedParam, 0, 0, 0, p, v, 0, false, o});
    }
    void onFocusedParamNudge(std::uint32_t p, std::int32_t ticks, bool fine,
                             const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedParamNudge, 0, 0, 0, p, 0.0f, ticks, fine, o});
    }
    void onFocusedParamGesture(std::uint32_t p, bool active, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedParamGesture, 0, 0, 0, p, 0.0f, 0, active, o});
    }
    void onFocusedMacro(std::uint32_t m, float v, const WriteOrigin& o) override {
        writes.push_back({TargetKind::FocusedMacro, 0, 0, 0, m, v, 0, false, o});
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

void testAllNamespaceAddressesAndAttribution() {
    section("namespace resolution and osc:<client> attribution");

    ClientPolicy pol;
    pol.enabled = true;
    OscNamespace ns(pol);
    RecordingSink sink;
    const std::string client = "192.168.1.42";

    const std::vector<Message> messages = {
        // /adi/transport
        msgF("/adi/transport/play", 1.0f),
        msgF("/adi/transport/stop", 1.0f),
        msgI("/adi/transport/record", 1),
        msgI("/adi/transport/loop", 0),
        msgF("/adi/transport/tempo", 128.5f),
        msgF("/adi/transport/re_enable_automation", 1.0f),
        // /adi/track/{n}
        msgF("/adi/track/3/volume", 0.8f),
        msgF("/adi/track/3/pan", 0.25f),
        msgI("/adi/track/3/mute", 1),
        msgI("/adi/track/3/solo", 1),
        msgI("/adi/track/3/arm", 1),
        msgF("/adi/track/3/send/2", 0.6f),
        msgI("/adi/track/3/device/4/bypass", 1),
        msgF("/adi/track/3/device/4/param/7", 0.42f),
        Message{"/adi/track/3/device/4/param/7/nudge", {Argument{std::int32_t{-2}}, Argument{std::int32_t{1}}}},
        msgI("/adi/track/3/device/4/param/7/gesture", 1),
        // /adi/master
        msgF("/adi/master/volume", 0.9f),
        msgF("/adi/master/pan", 0.5f),
        msgI("/adi/master/mute", 0),
        msgI("/adi/master/device/1/bypass", 0),
        msgF("/adi/master/device/1/param/3", 0.33f),
        msgI("/adi/master/device/1/param/3/nudge", 5),
        msgI("/adi/master/device/1/param/3/gesture", 0),
        // /adi/focused
        msgF("/adi/focused/param/2", 0.7f),
        msgI("/adi/focused/param/2/nudge", 1),
        msgI("/adi/focused/param/2/gesture", 1),
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

    check(sink.writes.size() == messages.size(), "every valid address emitted one write");
    for (std::size_t i = 0U; i < sink.writes.size(); ++i) {
        const auto& o = sink.writes[i].origin;
        check(o.actor == Actor::User, "write[" + std::to_string(i) + "] actor == User");
        check(o.actorDetail == "osc:192.168.1.42",
              "write[" + std::to_string(i) + "] actorDetail == osc:<client>");
        check(o.client == client, "write[" + std::to_string(i) + "] client matches");
    }

    // Spot-check typed payloads & indices.
    if (sink.writes.size() == messages.size()) {
        check(sink.writes[4].kind == TargetKind::TransportTempo && sink.writes[4].fval == 128.5f,
              "tempo value");
        check(sink.writes[11].kind == TargetKind::TrackSend && sink.writes[11].track == 3U &&
                  sink.writes[11].send == 2U && sink.writes[11].fval == 0.6f,
              "track send indices and value");
        check(sink.writes[14].kind == TargetKind::TrackDeviceParamNudge &&
                  sink.writes[14].track == 3U && sink.writes[14].device == 4U &&
                  sink.writes[14].param == 7U && sink.writes[14].ival == -2 &&
                  sink.writes[14].bval,
              "track device param nudge fine=true");
        check(sink.writes[29].kind == TargetKind::FocusedDialNudge &&
                  sink.writes[29].ival == 3 && sink.writes[29].bval,
              "focused dial nudge");
    }
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
        "/adi/track/1/delete",
        "/adi/track/1/volume/",
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

    // Invalid argument types, counts, NaN/Inf, out-of-range tempo.
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

    // Releasing a momentary Play (0.0f or 0) must never stop transport.
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

    // Relative deltas in one bundle are never coalesced.
    sink.writes.clear();
    Bundle b;
    b.timeTag = TimeTag{1ULL};
    b.elements.push_back(Packet{msgI("/adi/focused/dial/nudge", 1)});
    b.elements.push_back(Packet{msgI("/adi/focused/dial/nudge", 1)});
    b.elements.push_back(Packet{msgI("/adi/focused/dial/nudge", -1)});
    const auto res = ns.dispatchPacket(Packet{b}, "ipad", sink);
    check(res.accepted() && res.appliedCount == 3U && res.refusedCount == 0U,
          "all 3 nudges in bundle applied");
    check(sink.writes.size() == 3U, "3 distinct nudge callbacks invoked (never coalesced)");
    if (sink.writes.size() == 3U) {
        check(sink.writes[0].ival == 1 && sink.writes[1].ival == 1 && sink.writes[2].ival == -1,
              "nudge order and signs preserved");
    }
}

void testClientAllowListAndDefaultOff() {
    section("default-off gate and client allow-list");

    // Off by default (ADR-0188 d7).
    {
        OscNamespace ns;
        RecordingSink sink;
        check(!ns.policy().enabled, "OSC is off by default");
        check(ns.dispatchMessage(msgF("/adi/master/volume", 0.5f), "192.168.1.10", sink) ==
                  DispatchStatus::Disabled,
              "disabled service refuses write");
        check(sink.writes.empty(), "no writes while disabled");
    }

    // Enabled with allow-list: listed client accepted, unlisted refused.
    {
        ClientPolicy pol;
        pol.enabled = true;
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

        // Bundle from unlisted client is refused atomically.
        Bundle b;
        b.elements.push_back(Packet{msgF("/adi/master/volume", 0.5f)});
        const auto pr = ns.dispatchPacket(Packet{b}, "192.168.1.99", sink);
        check(!pr.accepted() && pr.gateStatus == DispatchStatus::ClientNotAllowed &&
                  sink.writes.empty(),
              "bundle from unlisted client refused");

        // Listed client succeeds.
        check(ns.dispatchMessage(msgF("/adi/master/volume", 0.5f), "192.168.1.10", sink) ==
                  DispatchStatus::Applied,
              "listed client accepted");
        check(sink.writes.size() == 1U &&
                  sink.writes[0].origin.actorDetail == "osc:192.168.1.10",
              "listed client stamped osc:<client>");
    }

    // Enabled with empty allow-list: any non-empty client accepted.
    {
        ClientPolicy pol;
        pol.enabled = true;
        OscNamespace ns(pol);
        RecordingSink sink;
        check(ns.dispatchMessage(msgF("/adi/master/volume", 0.4f), "10.1.2.3", sink) ==
                  DispatchStatus::Applied,
              "empty allow-list admits non-empty client");
    }
}

void testNoEchoFeedbackRule() {
    section("no-echo feedback rule");

    ClientPolicy pol;
    pol.enabled = true;
    pol.allowedClients = {"clientA", "clientB"};
    OscNamespace ns(pol);
    ns.addFeedbackPeer("clientA");
    ns.addFeedbackPeer("clientB");
    ns.addFeedbackPeer("unlistedC");  // disallowed by policy -> must receive nothing

    RecordingSink sink;

    // Initial state flushed to both allowed peers.
    check(ns.updateFeedValue("/adi/track/1/volume", 0.25f), "initial feed value accepted");
    check(!ns.updateFeedValue("/adi/track/1/volume", std::numeric_limits<float>::quiet_NaN()),
          "NaN feed value refused");
    {
        const auto fb = ns.flushFeedback();
        check(fb.size() == 2U, "initial flush sends to both allowed peers only");
        check(fb[0] == FeedbackMessage{"clientA", "/adi/track/1/volume", 0.25f},
              "initial flush clientA");
        check(fb[1] == FeedbackMessage{"clientB", "/adi/track/1/volume", 0.25f},
              "initial flush clientB");
    }

    // No change -> no feedback.
    check(ns.flushFeedback().empty(), "steady state emits no feedback");

    // Flush 1: clientA sends 0.75f; feed updates to 0.75f.
    // clientA MUST NOT get an echo in Flush 1; clientB MUST receive 0.75f.
    check(ns.dispatchMessage(msgF("/adi/track/1/volume", 0.75f), "clientA", sink) ==
              DispatchStatus::Applied,
          "clientA volume write applied");
    ns.updateFeedValue("/adi/track/1/volume", 0.75f);
    {
        const auto fb1 = ns.flushFeedback();
        check(fb1.size() == 1U, "flush 1 suppresses echo to clientA");
        if (fb1.size() == 1U) {
            check(fb1[0] == FeedbackMessage{"clientB", "/adi/track/1/volume", 0.75f},
                  "flush 1 delivers update to clientB");
        }
    }

    // Flush 2: suppression window cleared -> clientA receives confirmation; clientB gets nothing.
    {
        const auto fb2 = ns.flushFeedback();
        check(fb2.size() == 1U, "flush 2 confirms settled value to clientA");
        if (fb2.size() == 1U) {
            check(fb2[0] == FeedbackMessage{"clientA", "/adi/track/1/volume", 0.75f},
                  "flush 2 confirms 0.75 to clientA");
        }
    }

    // Flush 3: settled -> nothing emitted.
    check(ns.flushFeedback().empty(), "flush 3 emits nothing after confirmation");

    // Flush 4: a later external change (e.g. automation) -> both peers receive it.
    ns.updateFeedValue("/adi/track/1/volume", 0.40f);
    {
        const auto fb4 = ns.flushFeedback();
        check(fb4.size() == 2U, "later change reaches both clientA and clientB");
        if (fb4.size() == 2U) {
            check(fb4[0] == FeedbackMessage{"clientA", "/adi/track/1/volume", 0.40f},
                  "later change to clientA");
            check(fb4[1] == FeedbackMessage{"clientB", "/adi/track/1/volume", 0.40f},
                  "later change to clientB");
        }
    }

    // Continuous drag by clientA over two consecutive flushes: zero echo while dragging.
    check(ns.dispatchMessage(msgF("/adi/track/1/volume", 0.50f), "clientA", sink) ==
              DispatchStatus::Applied,
          "drag step 1");
    ns.updateFeedValue("/adi/track/1/volume", 0.50f);
    {
        const auto fb = ns.flushFeedback();
        check(fb.size() == 1U && fb[0].client == "clientB" && fb[0].value == 0.50f,
              "drag flush 1: no echo to A, live to B");
    }
    check(ns.dispatchMessage(msgF("/adi/track/1/volume", 0.60f), "clientA", sink) ==
              DispatchStatus::Applied,
          "drag step 2");
    ns.updateFeedValue("/adi/track/1/volume", 0.60f);
    {
        const auto fb = ns.flushFeedback();
        check(fb.size() == 1U && fb[0].client == "clientB" && fb[0].value == 0.60f,
              "drag flush 2: still no echo to A, live to B");
    }
    // Drag stops; next flush confirms 0.60f to clientA.
    {
        const auto fb = ns.flushFeedback();
        check(fb.size() == 1U && fb[0].client == "clientA" && fb[0].value == 0.60f,
              "post-drag flush confirms final value to A");
    }
}

}  // namespace

int main() {
    testAllNamespaceAddressesAndAttribution();
    testUnknownAddressesAndInvalidArgs();
    testMomentaryPlayAndUncoalescedDeltas();
    testClientAllowListAndDefaultOff();
    testNoEchoFeedbackRule();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
