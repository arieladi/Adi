// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-09: OSC fixed namespace, bind-interface & client allow-list policy,
// write attribution, gesture bracketing, and no-echo feedback scheduling
// (ADR-0185 d1, d4; ADR-0188 d7; ADR-0181 d2, d3).
//
// Fixed namespace roots:
//   /adi/transport/...  play, stop, record, loop, tempo, re_enable_automation
//   /adi/track/{id}/... volume[/gesture], pan[/gesture], mute, solo, arm,
//                       send/{sid}[/gesture],
//                       device/{did}/bypass,
//                       device/{did}/param/{pid}[/nudge|/gesture]
//   /adi/master/...     volume[/gesture], pan[/gesture], mute,
//                       device/{did}/bypass,
//                       device/{did}/param/{pid}[/nudge|/gesture]
//   /adi/focused/...    param/{pid}[/nudge|/gesture],
//                       macro/{m}[/gesture], bypass,
//                       dial[/nudge|/gesture]
//
// Identity contract (unified with SRF-10 OSCQuery):
//   All identifiers ({id}, {sid}, {did}, {pid}, {m}) are canonical positive
//   base-10 64-bit integers (1..UINT64_MAX, no leading zeros, no sign).
//   Track, send, device, and parameter ids are stable row/feed ids — NOT
//   1-based list positions — so every FULL_PATH published by SRF-10 resolves
//   directly here and stays stable when tracks are inserted or reordered.
//   Structural edits are not part of this namespace and are always refused
//   (ADR-0181 d2).
//
// Attribution & gesture contract (ADR-0181 d1, d2; ADR-0185 d4; SRF-02):
//   Every write dispatched to WriteSink is stamped actor = Actor::User and
//   actorDetail = "osc:<client>". Releasing a momentary trigger (value <= 0 on
//   play, stop, or re_enable_automation) is ignored and never stops transport.
//   Relative nudges are never coalesced on the wire. Continuous strip/parameter
//   writes (volume, pan, send, macro, device param, dial, and nudges) are
//   always gesture-bracketed:
//     - If the client holds an explicit .../gesture 1 touch, writes sit inside
//       that open gesture until .../gesture 0 or explicit cleanup.
//     - Otherwise the first write opens an implicit gesture and keeps it open
//       across continued writes on the same control, closing it once after a
//       monotonic quiet interval via pollGestures(nowMs, sink) or
//       flushFeedback(nowMs, sink) (following SRF-02 QuietSession semantics:
//       delayed polling closes an expired implicit gesture before opening a
//       fresh one for a later write). An explicit .../gesture 0 also closes an
//       open implicit gesture.
//     - An explicit .../gesture 0 with no matching open gesture for that client
//       is ignored (IgnoredRelease) and never reaches WriteSink.
//     - Owners disabling the service, de-listing a client, or forgetting a peer
//       should pass WriteSink to setPolicy(policy, sink) /
//       removeFeedbackPeer(client, sink), or call endAllGestures(sink) /
//       endClientGestures(client, sink) beforehand.
//
// Feedback contract (PLAN SRF-09):
//   Feedback is sent on change. A write (including a nudge, a clamped write,
//   an unchanged write, or a refused write on a feed-bearing address) never
//   echoes back to its sender within the flush window it arrived in, while
//   other allowed peers receive any change at once; on the next flush after
//   the sender stops writing, the settled true feed value is confirmed back
//   to the sender even if the value did not change.
//
// Threading contract (ADR-0181 d3, ADR-0010):
//   OscNamespace is strictly single-threaded and message-thread-owned.
//   - The UDP socket reader thread MUST NOT call dispatch*, flushFeedback,
//     updateFeedValue, setPolicy, or gesture methods directly; it hands raw
//     packets to a message-thread queue.
//   - The audio thread MUST NEVER touch OscNamespace; feed values are read
//     from lock-free slots on the message thread before calling
//     updateFeedValue() and flushFeedback().
//   Calls from a thread other than ownerThread_ fail closed (WrongThread /
//   no-op) without mutating internal state.

#pragma once

#include "adi/ops.hpp"
#include "adi/osc_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace adi::osc {

/// Attribution carried by every write emitted from the OSC namespace.
struct WriteOrigin {
    Actor actor = Actor::User;
    std::string actorDetail;  ///< Always "osc:<client>"
    std::string client;       ///< Raw client address/identifier

    friend bool operator==(const WriteOrigin&, const WriteOrigin&) = default;
};

/// Network interface binding policy enforced before opening the UDP socket
/// (ADR-0185 d4, ADR-0188 d7, ADR-0181 d2).
///
/// Defaults to loopback ("127.0.0.1") on port 9000. Wildcard binds
/// ("0.0.0.0", "::", "*") are always refused — the user must pick a specific
/// interface. Any non-loopback interface requires trustedNetworkAcknowledged
/// ("Use this only on trusted networks", ADR-0188 d7).
struct BindPolicy {
    std::string interfaceAddress = "127.0.0.1";
    std::uint16_t udpPort = 9000U;
    bool trustedNetworkAcknowledged = false;

    friend bool operator==(const BindPolicy&, const BindPolicy&) = default;
};

enum class BindPolicyStatus {
    Ok = 0,
    EmptyInterface,
    InvalidInterface,
    WildcardInterfaceRefused,
    NonLoopbackRequiresTrustedAck,
    InvalidPort,
};

[[nodiscard]] std::string_view toString(BindPolicyStatus) noexcept;
[[nodiscard]] bool isLoopbackInterface(std::string_view interfaceAddress) noexcept;
[[nodiscard]] BindPolicyStatus validateBindPolicy(const BindPolicy& bind) noexcept;

/// Service gate, bind-interface policy, and optional client allow-list
/// (ADR-0185 d4; ADR-0188 d7).
/// Off by default; when allowedClients is empty, any non-empty client is
/// accepted while enabled and bind is valid; when non-empty, only listed
/// clients are accepted.
struct ClientPolicy {
    bool enabled = false;
    std::vector<std::string> allowedClients;
    BindPolicy bind;

    [[nodiscard]] bool bindValid() const noexcept {
        return validateBindPolicy(bind) == BindPolicyStatus::Ok;
    }
    [[nodiscard]] bool allows(std::string_view client) const noexcept;
};

enum class TargetKind {
    TransportPlay,
    TransportStop,
    TransportRecord,
    TransportLoop,
    TransportTempo,
    ReEnableAutomation,

    TrackVolume,
    TrackVolumeGesture,
    TrackPan,
    TrackPanGesture,
    TrackMute,
    TrackSolo,
    TrackArm,
    TrackSend,
    TrackSendGesture,
    TrackDeviceBypass,
    TrackDeviceParam,
    TrackDeviceParamNudge,
    TrackDeviceParamGesture,

    MasterVolume,
    MasterVolumeGesture,
    MasterPan,
    MasterPanGesture,
    MasterMute,
    MasterDeviceBypass,
    MasterDeviceParam,
    MasterDeviceParamNudge,
    MasterDeviceParamGesture,

    FocusedParam,
    FocusedParamNudge,
    FocusedParamGesture,
    FocusedMacro,
    FocusedMacroGesture,
    FocusedBypass,
    FocusedDial,
    FocusedDialNudge,
    FocusedDialGesture,
};

struct ResolvedAddress {
    TargetKind kind = TargetKind::TransportPlay;
    std::uint64_t trackId = 0;   ///< Stable positive track id for /adi/track/{id}/...
    std::uint64_t sendId = 0;    ///< Stable positive send id for .../send/{sid}
    std::uint64_t deviceId = 0;  ///< Stable positive device id for .../device/{did}/...
    std::uint64_t paramId = 0;   ///< Stable positive param id (or 1-based macro slot)

    friend bool operator==(const ResolvedAddress&, const ResolvedAddress&) = default;
};

/// Parse one address in the fixed /adi/{transport,track,master,focused}
/// namespace. Returns nullopt for any unknown, malformed, or structural path.
[[nodiscard]] std::optional<ResolvedAddress> resolveAddress(std::string_view address) noexcept;

/// Return the canonical feed address affected by `address` / `kind`:
///   - Nudge paths strip "/nudge" so feedback suppresses & confirms the base
///     parameter/dial address.
///   - Gesture paths carry no feed value and return an empty string_view.
///   - Value/trigger paths return `address` unchanged.
[[nodiscard]] std::string_view canonicalFeedAddress(std::string_view address,
                                                    TargetKind kind) noexcept;

/// Build the canonical actor_detail string ("osc:<client>").
[[nodiscard]] std::string makeActorDetail(std::string_view client);

/// Typed write interface invoked by OscNamespace on the message thread.
/// Every callback receives the stamped WriteOrigin (Actor::User, "osc:<client>").
/// Track, send, device, and param parameters are stable positive 64-bit ids.
class WriteSink {
public:
    virtual ~WriteSink() = default;

    // Transport
    virtual void onTransportPlay(const WriteOrigin&) = 0;
    virtual void onTransportStop(const WriteOrigin&) = 0;
    virtual void onTransportRecord(bool active, const WriteOrigin&) = 0;
    virtual void onTransportLoop(bool enabled, const WriteOrigin&) = 0;
    virtual void onTransportTempo(float bpm, const WriteOrigin&) = 0;
    virtual void onReEnableAutomation(const WriteOrigin&) = 0;

    // Track (stable positive ids)
    virtual void onTrackVolume(std::uint64_t trackId, float normalized,
                               const WriteOrigin&) = 0;
    virtual void onTrackVolumeGesture(std::uint64_t trackId, bool active,
                                      const WriteOrigin&) = 0;
    virtual void onTrackPan(std::uint64_t trackId, float normalized,
                            const WriteOrigin&) = 0;
    virtual void onTrackPanGesture(std::uint64_t trackId, bool active,
                                   const WriteOrigin&) = 0;
    virtual void onTrackMute(std::uint64_t trackId, bool muted,
                             const WriteOrigin&) = 0;
    virtual void onTrackSolo(std::uint64_t trackId, bool soloed,
                             const WriteOrigin&) = 0;
    virtual void onTrackArm(std::uint64_t trackId, bool armed,
                            const WriteOrigin&) = 0;
    virtual void onTrackSend(std::uint64_t trackId, std::uint64_t sendId,
                             float normalized, const WriteOrigin&) = 0;
    virtual void onTrackSendGesture(std::uint64_t trackId, std::uint64_t sendId,
                                    bool active, const WriteOrigin&) = 0;
    virtual void onTrackDeviceBypass(std::uint64_t trackId, std::uint64_t deviceId,
                                     bool bypassed, const WriteOrigin&) = 0;
    virtual void onTrackDeviceParam(std::uint64_t trackId, std::uint64_t deviceId,
                                    std::uint64_t paramId, float normalized,
                                    const WriteOrigin&) = 0;
    virtual void onTrackDeviceParamNudge(std::uint64_t trackId, std::uint64_t deviceId,
                                         std::uint64_t paramId, std::int32_t ticks,
                                         bool fine, const WriteOrigin&) = 0;
    virtual void onTrackDeviceParamGesture(std::uint64_t trackId, std::uint64_t deviceId,
                                           std::uint64_t paramId, bool active,
                                           const WriteOrigin&) = 0;

    // Master
    virtual void onMasterVolume(float normalized, const WriteOrigin&) = 0;
    virtual void onMasterVolumeGesture(bool active, const WriteOrigin&) = 0;
    virtual void onMasterPan(float normalized, const WriteOrigin&) = 0;
    virtual void onMasterPanGesture(bool active, const WriteOrigin&) = 0;
    virtual void onMasterMute(bool muted, const WriteOrigin&) = 0;
    virtual void onMasterDeviceBypass(std::uint64_t deviceId, bool bypassed,
                                      const WriteOrigin&) = 0;
    virtual void onMasterDeviceParam(std::uint64_t deviceId, std::uint64_t paramId,
                                     float normalized, const WriteOrigin&) = 0;
    virtual void onMasterDeviceParamNudge(std::uint64_t deviceId, std::uint64_t paramId,
                                          std::int32_t ticks, bool fine,
                                          const WriteOrigin&) = 0;
    virtual void onMasterDeviceParamGesture(std::uint64_t deviceId, std::uint64_t paramId,
                                            bool active, const WriteOrigin&) = 0;

    // Focused
    virtual void onFocusedParam(std::uint64_t paramId, float normalized,
                                const WriteOrigin&) = 0;
    virtual void onFocusedParamNudge(std::uint64_t paramId, std::int32_t ticks,
                                     bool fine, const WriteOrigin&) = 0;
    virtual void onFocusedParamGesture(std::uint64_t paramId, bool active,
                                       const WriteOrigin&) = 0;
    virtual void onFocusedMacro(std::uint64_t macroIndex, float normalized,
                                const WriteOrigin&) = 0;
    virtual void onFocusedMacroGesture(std::uint64_t macroIndex, bool active,
                                       const WriteOrigin&) = 0;
    virtual void onFocusedBypass(bool bypassed, const WriteOrigin&) = 0;
    virtual void onFocusedDial(float normalized, const WriteOrigin&) = 0;
    virtual void onFocusedDialNudge(std::int32_t ticks, bool fine,
                                    const WriteOrigin&) = 0;
    virtual void onFocusedDialGesture(bool active, const WriteOrigin&) = 0;
};

enum class DispatchStatus {
    Applied = 0,
    IgnoredRelease,     ///< Momentary button release (<= 0) or unmatched gesture end; valid no-op
    Disabled,
    InvalidBindPolicy,  ///< Enabled, but bind interface/port policy is invalid
    WrongThread,        ///< Called from a thread other than the message-thread owner
    ClockRegression,    ///< Monotonic timestamp moved backwards; refused without mutation
    InvalidClient,
    ClientNotAllowed,
    UnknownAddress,
    InvalidArgument,
};

[[nodiscard]] std::string_view toString(DispatchStatus) noexcept;

struct PacketDispatchResult {
    DispatchStatus gateStatus = DispatchStatus::Applied;
    std::vector<DispatchStatus> messageStatuses;
    std::size_t appliedCount = 0;
    std::size_t ignoredReleaseCount = 0;
    std::size_t refusedCount = 0;

    [[nodiscard]] bool accepted() const noexcept {
        return gateStatus == DispatchStatus::Applied;
    }
};

/// One outbound feedback message produced by flushFeedback().
/// The network layer encodes this with juce_osc and sends it to `client`.
struct FeedbackMessage {
    std::string client;
    std::string address;
    float value = 0.0f;

    friend bool operator==(const FeedbackMessage&, const FeedbackMessage&) = default;
};

/// Message-thread-owned dispatcher and feedback scheduler for the fixed OSC
/// namespace. See file header for threading and gesture contracts.
class OscNamespace {
public:
    static constexpr std::uint64_t kDefaultQuietWindowMs = 250U;

    explicit OscNamespace(ClientPolicy policy = {},
                          std::uint64_t quietWindowMs = kDefaultQuietWindowMs);

    /// Rebind ownership to the calling message thread (for hand-off after
    /// construction on a setup thread; caller guarantees no concurrent use).
    void adoptCurrentThread() noexcept { ownerThread_ = std::this_thread::get_id(); }

    /// True iff the calling thread is the owning message thread.
    [[nodiscard]] bool isOnOwnerThread() const noexcept {
        return std::this_thread::get_id() == ownerThread_;
    }

    /// Configure the nonzero quiet interval (in monotonic milliseconds) used to
    /// close implicit gestures when a client sends no explicit .../gesture 0.
    /// Zero or off-thread calls are ignored.
    void setQuietWindowMs(std::uint64_t quietWindowMs) noexcept;
    [[nodiscard]] std::uint64_t quietWindowMs() const noexcept { return quietWindowMs_; }

    /// Update service/allow-list policy. The sink-taking overload immediately
    /// closes open gestures for all clients if the service is disabled or its
    /// bind policy is invalid, or for every client no longer allowed by the new
    /// policy. Callers using the 1-arg overload MUST call endAllGestures(sink)
    /// or endClientGestures(client, sink) before disabling or de-listing.
    void setPolicy(ClientPolicy policy);
    std::size_t setPolicy(ClientPolicy policy, WriteSink& sink);
    [[nodiscard]] const ClientPolicy& policy() const noexcept { return policy_; }

    /// Register or remove a client peer that receives feedback flushes.
    /// Peers not allowed by policy() receive no feedback while disallowed.
    /// The sink-taking removeFeedbackPeer() also ends any open gestures held by
    /// `client`; callers using the 1-arg overload MUST call endClientGestures()
    /// before forgetting an active client.
    void addFeedbackPeer(std::string client);
    void removeFeedbackPeer(std::string_view client);
    std::size_t removeFeedbackPeer(std::string_view client, WriteSink& sink);

    /// Close any open explicit or implicit gestures held by `client` (or all
    /// clients) and emit matching gesture-end callbacks on `sink`.
    std::size_t endClientGestures(std::string_view client, WriteSink& sink);
    std::size_t endAllGestures(WriteSink& sink);

    /// Advance the monotonic clock to `nowMs` and close every implicit gesture
    /// whose elapsed quiet time is >= quietWindowMs_ (SRF-02 QuietSession
    /// semantics). Refuses off-thread calls and clock regressions without
    /// mutating state. Returns count of gestures closed.
    std::size_t pollGestures(std::uint64_t nowMs, WriteSink& sink);

    /// Dispatch one decoded OSC message from `client` into `sink` at monotonic
    /// timestamp `nowMs` (milliseconds). The 3-arg overload reuses the last
    /// observed monotonic timestamp (or 0 if none).
    /// Must be called on the owning message thread.
    [[nodiscard]] DispatchStatus dispatchMessage(const Message& message,
                                                 std::string_view client,
                                                 WriteSink& sink,
                                                 std::uint64_t nowMs);
    [[nodiscard]] DispatchStatus dispatchMessage(const Message& message,
                                                 std::string_view client,
                                                 WriteSink& sink);

    /// Dispatch a decoded OSC message or bundle from `client` into `sink` at
    /// monotonic timestamp `nowMs` (milliseconds).
    /// Must be called on the owning message thread.
    [[nodiscard]] PacketDispatchResult dispatchPacket(const Packet& packet,
                                                      std::string_view client,
                                                      WriteSink& sink,
                                                      std::uint64_t nowMs);
    [[nodiscard]] PacketDispatchResult dispatchPacket(const Packet& packet,
                                                      std::string_view client,
                                                      WriteSink& sink);

    /// Record the latest feed value for a namespace address (finite float).
    /// Returns false if off owner thread, `value` is non-finite, or `address`
    /// is empty.
    bool updateFeedValue(std::string_view address, float value);

    /// Produce outbound feedback for one flush tick on the owning message thread.
    ///
    /// Rules (PLAN SRF-09):
    ///   - Feedback is sent when a peer's last-sent value differs from the
    ///     current feed value, has not been sent yet, or was scheduled for
    ///     next-flush confirmation after a write (including unchanged,
    ///     clamped, nudged, or refused writes).
    ///   - A feed address touched by a sender during the current flush window
    ///     is NOT echoed back to that sender during this flush; other peers do
    ///     receive any change immediately.
    ///   - At the end of flushFeedback(), addresses suppressed in this flush
    ///     are scheduled for confirmation on the next flush and the current
    ///     suppression window clears.
    /// The (nowMs, sink) overload also runs pollGestures(nowMs, sink) first.
    [[nodiscard]] std::vector<FeedbackMessage> flushFeedback();
    [[nodiscard]] std::vector<FeedbackMessage> flushFeedback(std::uint64_t nowMs, WriteSink& sink);

private:
    struct ActiveGesture {
        ResolvedAddress target;
        bool explicitTouch = false;
        std::uint64_t lastActivityMs = 0U;
    };

    void emitGesture(const ResolvedAddress& target, bool active, const WriteOrigin& origin,
                     WriteSink& sink) const;
    [[nodiscard]] bool hasActiveGesture(std::string_view client,
                                        std::string_view feedAddr) const noexcept;

    std::thread::id ownerThread_ = std::this_thread::get_id();
    ClientPolicy policy_;
    std::uint64_t quietWindowMs_ = kDefaultQuietWindowMs;
    std::optional<std::uint64_t> lastNowMs_;
    std::set<std::string, std::less<>> peers_;
    std::map<std::string, float, std::less<>> feedValues_;
    std::map<std::string, std::map<std::string, float, std::less<>>, std::less<>> lastSent_;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> suppressedThisFlush_;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> confirmNextFlush_;
    std::map<std::string, std::map<std::string, ActiveGesture, std::less<>>, std::less<>>
        activeGestures_;
};

}  // namespace adi::osc
