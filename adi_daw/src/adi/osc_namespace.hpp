// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-09: OSC fixed namespace, client allow-list, write attribution, and
// no-echo feedback scheduling (ADR-0185 d1, d4; ADR-0188 d7; ADR-0181 d2).
//
// Fixed namespace roots:
//   /adi/transport/...  play, stop, record, loop, tempo, re_enable_automation
//   /adi/track/{n}/...  volume, pan, mute, solo, arm, send/{s},
//                       device/{d}/bypass, device/{d}/param/{p}[/nudge|/gesture]
//   /adi/master/...     volume, pan, mute,
//                       device/{d}/bypass, device/{d}/param/{p}[/nudge|/gesture]
//   /adi/focused/...    param/{p}[/nudge|/gesture], macro/{m}, bypass,
//                       dial[/nudge|/gesture]
//
// All indices ({n}, {s}, {d}, {p}, {m}) are canonical 1-based positive base-10
// integers (no leading zeros, no sign). Structural edits are not part of this
// namespace and are always refused (ADR-0181 d2).
//
// Every write dispatched to WriteSink is stamped actor = Actor::User and
// actorDetail = "osc:<client>". Releasing a momentary trigger (value <= 0 on
// play, stop, or re_enable_automation) is ignored and never stops transport.
// Relative nudges are never coalesced.
//
// Feedback is sent on change: a value that arrived from a client in the
// current flush window is never echoed back to that sender within that flush,
// while other peers receive it at once; on a subsequent flush (or a later
// change), the sender receives the settled/updated value.

#pragma once

#include "adi/ops.hpp"
#include "adi/osc_codec.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace adi::osc {

/// Attribution carried by every write emitted from the OSC namespace.
struct WriteOrigin {
    Actor actor = Actor::User;
    std::string actorDetail;  ///< Always "osc:<client>"
    std::string client;       ///< Raw client address/identifier

    friend bool operator==(const WriteOrigin&, const WriteOrigin&) = default;
};

/// Service gate and optional client allow-list (ADR-0185 d4; ADR-0188 d7).
/// Off by default; when allowedClients is empty, any non-empty client is
/// accepted while enabled; when non-empty, only listed clients are accepted.
struct ClientPolicy {
    bool enabled = false;
    std::vector<std::string> allowedClients;

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
    TrackPan,
    TrackMute,
    TrackSolo,
    TrackArm,
    TrackSend,
    TrackDeviceBypass,
    TrackDeviceParam,
    TrackDeviceParamNudge,
    TrackDeviceParamGesture,

    MasterVolume,
    MasterPan,
    MasterMute,
    MasterDeviceBypass,
    MasterDeviceParam,
    MasterDeviceParamNudge,
    MasterDeviceParamGesture,

    FocusedParam,
    FocusedParamNudge,
    FocusedParamGesture,
    FocusedMacro,
    FocusedBypass,
    FocusedDial,
    FocusedDialNudge,
    FocusedDialGesture,
};

struct ResolvedAddress {
    TargetKind kind = TargetKind::TransportPlay;
    std::uint32_t trackIndex = 0;   ///< 1-based for /adi/track/{n}/...
    std::uint32_t sendIndex = 0;    ///< 1-based for .../send/{s}
    std::uint32_t deviceIndex = 0;  ///< 1-based for .../device/{d}/...
    std::uint32_t paramIndex = 0;   ///< 1-based for .../param/{p} or macro/{m}

    friend bool operator==(const ResolvedAddress&, const ResolvedAddress&) = default;
};

/// Parse one address in the fixed /adi/{transport,track,master,focused}
/// namespace. Returns nullopt for any unknown, malformed, or structural path.
[[nodiscard]] std::optional<ResolvedAddress> resolveAddress(std::string_view address) noexcept;

/// Build the canonical actor_detail string ("osc:<client>").
[[nodiscard]] std::string makeActorDetail(std::string_view client);

/// Typed write interface invoked by OscNamespace. Every callback receives the
/// stamped WriteOrigin (Actor::User, "osc:<client>").
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

    // Track (1-based indices)
    virtual void onTrackVolume(std::uint32_t trackIndex, float normalized,
                               const WriteOrigin&) = 0;
    virtual void onTrackPan(std::uint32_t trackIndex, float normalized,
                            const WriteOrigin&) = 0;
    virtual void onTrackMute(std::uint32_t trackIndex, bool muted,
                             const WriteOrigin&) = 0;
    virtual void onTrackSolo(std::uint32_t trackIndex, bool soloed,
                             const WriteOrigin&) = 0;
    virtual void onTrackArm(std::uint32_t trackIndex, bool armed,
                            const WriteOrigin&) = 0;
    virtual void onTrackSend(std::uint32_t trackIndex, std::uint32_t sendIndex,
                             float normalized, const WriteOrigin&) = 0;
    virtual void onTrackDeviceBypass(std::uint32_t trackIndex, std::uint32_t deviceIndex,
                                     bool bypassed, const WriteOrigin&) = 0;
    virtual void onTrackDeviceParam(std::uint32_t trackIndex, std::uint32_t deviceIndex,
                                    std::uint32_t paramIndex, float normalized,
                                    const WriteOrigin&) = 0;
    virtual void onTrackDeviceParamNudge(std::uint32_t trackIndex, std::uint32_t deviceIndex,
                                         std::uint32_t paramIndex, std::int32_t ticks,
                                         bool fine, const WriteOrigin&) = 0;
    virtual void onTrackDeviceParamGesture(std::uint32_t trackIndex, std::uint32_t deviceIndex,
                                           std::uint32_t paramIndex, bool active,
                                           const WriteOrigin&) = 0;

    // Master
    virtual void onMasterVolume(float normalized, const WriteOrigin&) = 0;
    virtual void onMasterPan(float normalized, const WriteOrigin&) = 0;
    virtual void onMasterMute(bool muted, const WriteOrigin&) = 0;
    virtual void onMasterDeviceBypass(std::uint32_t deviceIndex, bool bypassed,
                                      const WriteOrigin&) = 0;
    virtual void onMasterDeviceParam(std::uint32_t deviceIndex, std::uint32_t paramIndex,
                                     float normalized, const WriteOrigin&) = 0;
    virtual void onMasterDeviceParamNudge(std::uint32_t deviceIndex, std::uint32_t paramIndex,
                                          std::int32_t ticks, bool fine,
                                          const WriteOrigin&) = 0;
    virtual void onMasterDeviceParamGesture(std::uint32_t deviceIndex, std::uint32_t paramIndex,
                                            bool active, const WriteOrigin&) = 0;

    // Focused
    virtual void onFocusedParam(std::uint32_t paramIndex, float normalized,
                                const WriteOrigin&) = 0;
    virtual void onFocusedParamNudge(std::uint32_t paramIndex, std::int32_t ticks,
                                     bool fine, const WriteOrigin&) = 0;
    virtual void onFocusedParamGesture(std::uint32_t paramIndex, bool active,
                                       const WriteOrigin&) = 0;
    virtual void onFocusedMacro(std::uint32_t macroIndex, float normalized,
                                const WriteOrigin&) = 0;
    virtual void onFocusedBypass(bool bypassed, const WriteOrigin&) = 0;
    virtual void onFocusedDial(float normalized, const WriteOrigin&) = 0;
    virtual void onFocusedDialNudge(std::int32_t ticks, bool fine,
                                    const WriteOrigin&) = 0;
    virtual void onFocusedDialGesture(bool active, const WriteOrigin&) = 0;
};

enum class DispatchStatus {
    Applied = 0,
    IgnoredRelease,  ///< Momentary button release (<= 0); valid no-op
    Disabled,
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

/// Stateful dispatcher and feedback scheduler for the fixed OSC namespace.
class OscNamespace {
public:
    explicit OscNamespace(ClientPolicy policy = {});

    void setPolicy(ClientPolicy policy);
    [[nodiscard]] const ClientPolicy& policy() const noexcept { return policy_; }

    /// Register or remove a client peer that receives feedback flushes.
    /// Peers not allowed by policy() receive no feedback while disallowed.
    void addFeedbackPeer(std::string client);
    void removeFeedbackPeer(std::string_view client);

    /// Dispatch one decoded OSC message from `client` into `sink`.
    [[nodiscard]] DispatchStatus dispatchMessage(const Message& message,
                                                 std::string_view client,
                                                 WriteSink& sink);

    /// Dispatch a decoded OSC message or bundle from `client` into `sink`.
    [[nodiscard]] PacketDispatchResult dispatchPacket(const Packet& packet,
                                                      std::string_view client,
                                                      WriteSink& sink);

    /// Record the latest feed value for a namespace address (finite float).
    /// Returns false if `value` is non-finite or `address` is empty.
    bool updateFeedValue(std::string_view address, float value);

    /// Produce outbound feedback for one flush tick.
    ///
    /// Rules (PLAN SRF-09):
    ///   - Feedback is sent only when a peer's last-sent value differs from
    ///     the current feed value (or has not been sent yet).
    ///   - An address written by a sender during the current flush window is
    ///     NOT echoed back to that sender during this flush; other peers do
    ///     receive it immediately.
    ///   - At the end of flushFeedback(), the current flush's suppression
    ///     window clears, so the next flush confirms the settled value (or
    ///     delivers any later change) to that sender.
    [[nodiscard]] std::vector<FeedbackMessage> flushFeedback();

private:
    ClientPolicy policy_;
    std::set<std::string, std::less<>> peers_;
    std::map<std::string, float, std::less<>> feedValues_;
    std::map<std::string, std::map<std::string, float, std::less<>>, std::less<>> lastSent_;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> suppressedThisFlush_;
};

}  // namespace adi::osc
