// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-09: OSC fixed namespace, bind-interface & client allow-list policy,
// attribution, gesture bracketing, and no-echo feedback scheduling.

#include "adi/osc_namespace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace adi::osc {

namespace {

constexpr float kMinTempoBpm = 20.0f;
constexpr float kMaxTempoBpm = 999.0f;
constexpr float kMaxNudgeFloat = 1000000.0f;
constexpr std::uint16_t kMinNonPrivilegedPort = 1024U;
constexpr std::size_t kMaxPathSegments = 8U;

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

/// Split a slash-prefixed OSC path into non-empty segments.
/// Returns 0 on missing leading '/', trailing '/', empty segment, or > max.
[[nodiscard]] std::size_t splitPath(std::string_view address,
                                    std::array<std::string_view, kMaxPathSegments>& out) noexcept {
    if (address.size() < 2U || address[0] != '/' || address.back() == '/') {
        return 0U;
    }

    std::size_t count = 0U;
    std::size_t pos = 1U;
    while (pos < address.size()) {
        const std::size_t slash = address.find('/', pos);
        const std::size_t end = slash == std::string_view::npos ? address.size() : slash;
        if (end == pos) {
            return 0U;  // empty segment ("//")
        }
        if (count >= kMaxPathSegments) {
            return 0U;
        }
        out[count++] = address.substr(pos, end - pos);
        if (slash == std::string_view::npos) {
            break;
        }
        pos = slash + 1U;
    }
    return count;
}

/// Parse a canonical positive decimal uint64 (1..UINT64_MAX, no leading zeros, no sign).
[[nodiscard]] std::optional<std::uint64_t> parsePositiveId(std::string_view s) noexcept {
    if (s.empty() || s[0] < '1' || s[0] > '9') {
        return std::nullopt;
    }
    std::uint64_t acc = 0U;
    constexpr std::uint64_t kMaxU64 = (std::numeric_limits<std::uint64_t>::max)();
    for (char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (acc > (kMaxU64 - digit) / 10U) {
            return std::nullopt;
        }
        acc = (acc * 10U) + digit;
    }
    return acc;
}

[[nodiscard]] std::optional<float> extractNormalized(const std::vector<Argument>& args) noexcept {
    if (args.size() != 1U) {
        return std::nullopt;
    }
    const auto* f = std::get_if<float>(&args[0]);
    if (!f || !std::isfinite(*f)) {
        return std::nullopt;
    }
    return std::clamp(*f, 0.0f, 1.0f);
}

[[nodiscard]] std::optional<bool> extractBoolArg(const Argument& arg) noexcept {
    if (const auto* i = std::get_if<std::int32_t>(&arg)) {
        return *i > 0;
    }
    if (const auto* f = std::get_if<float>(&arg)) {
        if (!std::isfinite(*f)) {
            return std::nullopt;
        }
        return *f > 0.0f;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<bool> extractBool(const std::vector<Argument>& args) noexcept {
    if (args.size() != 1U) {
        return std::nullopt;
    }
    return extractBoolArg(args[0]);
}

[[nodiscard]] std::optional<float> extractTempo(const std::vector<Argument>& args) noexcept {
    if (args.size() != 1U) {
        return std::nullopt;
    }
    float bpm = 0.0f;
    if (const auto* f = std::get_if<float>(&args[0])) {
        bpm = *f;
    } else if (const auto* i = std::get_if<std::int32_t>(&args[0])) {
        bpm = static_cast<float>(*i);
    } else {
        return std::nullopt;
    }
    if (!std::isfinite(bpm) || bpm < kMinTempoBpm || bpm > kMaxTempoBpm) {
        return std::nullopt;
    }
    return bpm;
}

struct NudgeValue {
    std::int32_t ticks = 0;
    bool fine = false;
};

[[nodiscard]] std::optional<NudgeValue> extractNudge(const std::vector<Argument>& args) noexcept {
    if (args.empty() || args.size() > 2U) {
        return std::nullopt;
    }
    NudgeValue out;
    if (const auto* i = std::get_if<std::int32_t>(&args[0])) {
        out.ticks = *i;
    } else if (const auto* f = std::get_if<float>(&args[0])) {
        if (!std::isfinite(*f) || *f < -kMaxNudgeFloat || *f > kMaxNudgeFloat) {
            return std::nullopt;
        }
        out.ticks = static_cast<std::int32_t>(std::lround(*f));
    } else {
        return std::nullopt;
    }
    if (args.size() == 2U) {
        const auto fine = extractBoolArg(args[1]);
        if (!fine.has_value()) {
            return std::nullopt;
        }
        out.fine = *fine;
    }
    return out;
}

[[nodiscard]] float normalizeZero(float v) noexcept {
    return v == 0.0f ? 0.0f : v;
}

[[nodiscard]] bool isGestureKind(TargetKind kind) noexcept {
    switch (kind) {
        case TargetKind::TrackVolumeGesture:
        case TargetKind::TrackPanGesture:
        case TargetKind::TrackSendGesture:
        case TargetKind::TrackDeviceParamGesture:
        case TargetKind::MasterVolumeGesture:
        case TargetKind::MasterPanGesture:
        case TargetKind::MasterDeviceParamGesture:
        case TargetKind::FocusedParamGesture:
        case TargetKind::FocusedMacroGesture:
        case TargetKind::FocusedDialGesture:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool isNudgeKind(TargetKind kind) noexcept {
    switch (kind) {
        case TargetKind::TrackDeviceParamNudge:
        case TargetKind::MasterDeviceParamNudge:
        case TargetKind::FocusedParamNudge:
        case TargetKind::FocusedDialNudge:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] std::string_view stripSuffix(std::string_view s, std::string_view suffix) noexcept {
    if (s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix) {
        return s.substr(0U, s.size() - suffix.size());
    }
    return s;
}

/// Base control address for gesture tracking (strips "/gesture" or "/nudge").
[[nodiscard]] std::string_view gestureControlAddress(std::string_view address,
                                                     TargetKind kind) noexcept {
    if (isGestureKind(kind)) {
        return stripSuffix(address, "/gesture");
    }
    if (isNudgeKind(kind)) {
        return stripSuffix(address, "/nudge");
    }
    return address;
}

/// Map a continuous control target to its corresponding gesture TargetKind.
[[nodiscard]] std::optional<TargetKind> gestureKindFor(TargetKind kind) noexcept {
    switch (kind) {
        case TargetKind::TrackVolume:
        case TargetKind::TrackVolumeGesture:
            return TargetKind::TrackVolumeGesture;
        case TargetKind::TrackPan:
        case TargetKind::TrackPanGesture:
            return TargetKind::TrackPanGesture;
        case TargetKind::TrackSend:
        case TargetKind::TrackSendGesture:
            return TargetKind::TrackSendGesture;
        case TargetKind::TrackDeviceParam:
        case TargetKind::TrackDeviceParamNudge:
        case TargetKind::TrackDeviceParamGesture:
            return TargetKind::TrackDeviceParamGesture;
        case TargetKind::MasterVolume:
        case TargetKind::MasterVolumeGesture:
            return TargetKind::MasterVolumeGesture;
        case TargetKind::MasterPan:
        case TargetKind::MasterPanGesture:
            return TargetKind::MasterPanGesture;
        case TargetKind::MasterDeviceParam:
        case TargetKind::MasterDeviceParamNudge:
        case TargetKind::MasterDeviceParamGesture:
            return TargetKind::MasterDeviceParamGesture;
        case TargetKind::FocusedParam:
        case TargetKind::FocusedParamNudge:
        case TargetKind::FocusedParamGesture:
            return TargetKind::FocusedParamGesture;
        case TargetKind::FocusedMacro:
        case TargetKind::FocusedMacroGesture:
            return TargetKind::FocusedMacroGesture;
        case TargetKind::FocusedDial:
        case TargetKind::FocusedDialNudge:
        case TargetKind::FocusedDialGesture:
            return TargetKind::FocusedDialGesture;
        default:
            return std::nullopt;
    }
}

[[nodiscard]] bool isWildcardInterface(std::string_view iface) noexcept {
    return iface == "0.0.0.0" || iface == "::" || iface == "[::]" || iface == "*" ||
           iface == "0:0:0:0:0:0:0:0" || iface == "[0:0:0:0:0:0:0:0]";
}

}  // namespace

std::string_view toString(BindPolicyStatus status) noexcept {
    switch (status) {
        case BindPolicyStatus::Ok: return "ok";
        case BindPolicyStatus::EmptyInterface: return "empty_interface";
        case BindPolicyStatus::InvalidInterface: return "invalid_interface";
        case BindPolicyStatus::WildcardInterfaceRefused: return "wildcard_interface_refused";
        case BindPolicyStatus::NonLoopbackRequiresTrustedAck:
            return "non_loopback_requires_trusted_ack";
        case BindPolicyStatus::InvalidPort: return "invalid_port";
    }
    return "unknown";
}

bool isLoopbackInterface(std::string_view interfaceAddress) noexcept {
    return interfaceAddress == "127.0.0.1" || interfaceAddress == "::1" ||
           interfaceAddress == "[::1]" || asciiEqualsIgnoreCase(interfaceAddress, "localhost");
}

BindPolicyStatus validateBindPolicy(const BindPolicy& bind) noexcept {
    if (bind.interfaceAddress.empty()) {
        return BindPolicyStatus::EmptyInterface;
    }
    for (char c : bind.interfaceAddress) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x21U || uc > 0x7EU) {
            return BindPolicyStatus::InvalidInterface;
        }
    }
    if (isWildcardInterface(bind.interfaceAddress)) {
        return BindPolicyStatus::WildcardInterfaceRefused;
    }
    if (bind.udpPort < kMinNonPrivilegedPort) {
        return BindPolicyStatus::InvalidPort;
    }
    if (!isLoopbackInterface(bind.interfaceAddress) && !bind.trustedNetworkAcknowledged) {
        return BindPolicyStatus::NonLoopbackRequiresTrustedAck;
    }
    return BindPolicyStatus::Ok;
}

bool ClientPolicy::allows(std::string_view client) const noexcept {
    if (!enabled || !bindValid() || client.empty()) {
        return false;
    }
    if (allowedClients.empty()) {
        return true;
    }
    return std::find(allowedClients.begin(), allowedClients.end(), client) !=
           allowedClients.end();
}

std::string makeActorDetail(std::string_view client) {
    std::string out;
    out.reserve(4U + client.size());
    out.append("osc:");
    out.append(client);
    return out;
}

std::string_view toString(DispatchStatus status) noexcept {
    switch (status) {
        case DispatchStatus::Applied: return "applied";
        case DispatchStatus::IgnoredRelease: return "ignored_release";
        case DispatchStatus::Disabled: return "disabled";
        case DispatchStatus::InvalidBindPolicy: return "invalid_bind_policy";
        case DispatchStatus::WrongThread: return "wrong_thread";
        case DispatchStatus::ClockRegression: return "clock_regression";
        case DispatchStatus::InvalidClient: return "invalid_client";
        case DispatchStatus::ClientNotAllowed: return "client_not_allowed";
        case DispatchStatus::UnknownAddress: return "unknown_address";
        case DispatchStatus::InvalidArgument: return "invalid_argument";
    }
    return "unknown";
}

std::string_view canonicalFeedAddress(std::string_view address, TargetKind kind) noexcept {
    if (isGestureKind(kind)) {
        return {};
    }
    if (isNudgeKind(kind)) {
        return stripSuffix(address, "/nudge");
    }
    return address;
}

std::optional<ResolvedAddress> resolveAddress(std::string_view address) noexcept {
    std::array<std::string_view, kMaxPathSegments> seg{};
    const std::size_t n = splitPath(address, seg);
    if (n < 3U || seg[0] != "adi") {
        return std::nullopt;
    }

    ResolvedAddress out;

    // /adi/transport/...
    if (seg[1] == "transport") {
        if (n != 3U) {
            return std::nullopt;
        }
        if (seg[2] == "play") {
            out.kind = TargetKind::TransportPlay;
            return out;
        }
        if (seg[2] == "stop") {
            out.kind = TargetKind::TransportStop;
            return out;
        }
        if (seg[2] == "record") {
            out.kind = TargetKind::TransportRecord;
            return out;
        }
        if (seg[2] == "loop") {
            out.kind = TargetKind::TransportLoop;
            return out;
        }
        if (seg[2] == "tempo") {
            out.kind = TargetKind::TransportTempo;
            return out;
        }
        if (seg[2] == "re_enable_automation") {
            out.kind = TargetKind::ReEnableAutomation;
            return out;
        }
        return std::nullopt;
    }

    // /adi/track/{stableId}/...
    if (seg[1] == "track") {
        if (n < 4U) {
            return std::nullopt;
        }
        const auto tid = parsePositiveId(seg[2]);
        if (!tid.has_value()) {
            return std::nullopt;
        }
        out.trackId = *tid;

        if (n == 4U) {
            if (seg[3] == "volume") {
                out.kind = TargetKind::TrackVolume;
                return out;
            }
            if (seg[3] == "pan") {
                out.kind = TargetKind::TrackPan;
                return out;
            }
            if (seg[3] == "mute") {
                out.kind = TargetKind::TrackMute;
                return out;
            }
            if (seg[3] == "solo") {
                out.kind = TargetKind::TrackSolo;
                return out;
            }
            if (seg[3] == "arm") {
                out.kind = TargetKind::TrackArm;
                return out;
            }
            return std::nullopt;
        }

        if (n == 5U && seg[4] == "gesture") {
            if (seg[3] == "volume") {
                out.kind = TargetKind::TrackVolumeGesture;
                return out;
            }
            if (seg[3] == "pan") {
                out.kind = TargetKind::TrackPanGesture;
                return out;
            }
            return std::nullopt;
        }

        if (seg[3] == "send" && (n == 5U || n == 6U)) {
            const auto sid = parsePositiveId(seg[4]);
            if (!sid.has_value()) {
                return std::nullopt;
            }
            out.sendId = *sid;
            if (n == 5U) {
                out.kind = TargetKind::TrackSend;
                return out;
            }
            if (seg[5] == "gesture") {
                out.kind = TargetKind::TrackSendGesture;
                return out;
            }
            return std::nullopt;
        }

        if (seg[3] == "device" && n >= 6U) {
            const auto did = parsePositiveId(seg[4]);
            if (!did.has_value()) {
                return std::nullopt;
            }
            out.deviceId = *did;
            if (n == 6U && seg[5] == "bypass") {
                out.kind = TargetKind::TrackDeviceBypass;
                return out;
            }
            if (seg[5] == "param" && (n == 7U || n == 8U)) {
                const auto pid = parsePositiveId(seg[6]);
                if (!pid.has_value()) {
                    return std::nullopt;
                }
                out.paramId = *pid;
                if (n == 7U) {
                    out.kind = TargetKind::TrackDeviceParam;
                    return out;
                }
                if (seg[7] == "nudge") {
                    out.kind = TargetKind::TrackDeviceParamNudge;
                    return out;
                }
                if (seg[7] == "gesture") {
                    out.kind = TargetKind::TrackDeviceParamGesture;
                    return out;
                }
            }
        }
        return std::nullopt;
    }

    // /adi/master/...
    if (seg[1] == "master") {
        if (n == 3U) {
            if (seg[2] == "volume") {
                out.kind = TargetKind::MasterVolume;
                return out;
            }
            if (seg[2] == "pan") {
                out.kind = TargetKind::MasterPan;
                return out;
            }
            if (seg[2] == "mute") {
                out.kind = TargetKind::MasterMute;
                return out;
            }
            return std::nullopt;
        }
        if (n == 4U && seg[3] == "gesture") {
            if (seg[2] == "volume") {
                out.kind = TargetKind::MasterVolumeGesture;
                return out;
            }
            if (seg[2] == "pan") {
                out.kind = TargetKind::MasterPanGesture;
                return out;
            }
            return std::nullopt;
        }
        if (seg[2] == "device" && n >= 5U) {
            const auto did = parsePositiveId(seg[3]);
            if (!did.has_value()) {
                return std::nullopt;
            }
            out.deviceId = *did;
            if (n == 5U && seg[4] == "bypass") {
                out.kind = TargetKind::MasterDeviceBypass;
                return out;
            }
            if (seg[4] == "param" && (n == 6U || n == 7U)) {
                const auto pid = parsePositiveId(seg[5]);
                if (!pid.has_value()) {
                    return std::nullopt;
                }
                out.paramId = *pid;
                if (n == 6U) {
                    out.kind = TargetKind::MasterDeviceParam;
                    return out;
                }
                if (seg[6] == "nudge") {
                    out.kind = TargetKind::MasterDeviceParamNudge;
                    return out;
                }
                if (seg[6] == "gesture") {
                    out.kind = TargetKind::MasterDeviceParamGesture;
                    return out;
                }
            }
        }
        return std::nullopt;
    }

    // /adi/focused/...
    if (seg[1] == "focused") {
        if (n == 3U) {
            if (seg[2] == "bypass") {
                out.kind = TargetKind::FocusedBypass;
                return out;
            }
            if (seg[2] == "dial") {
                out.kind = TargetKind::FocusedDial;
                return out;
            }
            return std::nullopt;
        }
        if (n == 4U && seg[2] == "dial") {
            if (seg[3] == "nudge") {
                out.kind = TargetKind::FocusedDialNudge;
                return out;
            }
            if (seg[3] == "gesture") {
                out.kind = TargetKind::FocusedDialGesture;
                return out;
            }
            return std::nullopt;
        }
        if (seg[2] == "macro" && (n == 4U || n == 5U)) {
            const auto macroIdx = parsePositiveId(seg[3]);
            if (!macroIdx.has_value()) {
                return std::nullopt;
            }
            out.paramId = *macroIdx;
            if (n == 4U) {
                out.kind = TargetKind::FocusedMacro;
                return out;
            }
            if (seg[4] == "gesture") {
                out.kind = TargetKind::FocusedMacroGesture;
                return out;
            }
            return std::nullopt;
        }
        if (seg[2] == "param" && (n == 4U || n == 5U)) {
            const auto pid = parsePositiveId(seg[3]);
            if (!pid.has_value()) {
                return std::nullopt;
            }
            out.paramId = *pid;
            if (n == 4U) {
                out.kind = TargetKind::FocusedParam;
                return out;
            }
            if (seg[4] == "nudge") {
                out.kind = TargetKind::FocusedParamNudge;
                return out;
            }
            if (seg[4] == "gesture") {
                out.kind = TargetKind::FocusedParamGesture;
                return out;
            }
        }
        return std::nullopt;
    }

    return std::nullopt;
}

OscNamespace::OscNamespace(ClientPolicy policy, std::uint64_t quietWindowMs)
    : policy_(std::move(policy)),
      quietWindowMs_(quietWindowMs > 0U ? quietWindowMs : kDefaultQuietWindowMs) {}

void OscNamespace::setQuietWindowMs(std::uint64_t quietWindowMs) noexcept {
    if (!isOnOwnerThread() || quietWindowMs == 0U) {
        return;
    }
    quietWindowMs_ = quietWindowMs;
}

void OscNamespace::setPolicy(ClientPolicy policy) {
    if (!isOnOwnerThread()) {
        return;
    }
    policy_ = std::move(policy);
}

std::size_t OscNamespace::setPolicy(ClientPolicy policy, WriteSink& sink) {
    if (!isOnOwnerThread()) {
        return 0U;
    }
    policy_ = std::move(policy);
    if (!policy_.enabled || !policy_.bindValid()) {
        return endAllGestures(sink);
    }
    std::vector<std::string> disallowedClients;
    for (const auto& [client, gestures] : activeGestures_) {
        (void)gestures;
        if (!policy_.allows(client)) {
            disallowedClients.push_back(client);
        }
    }
    std::size_t closed = 0U;
    for (const auto& client : disallowedClients) {
        closed += endClientGestures(client, sink);
    }
    return closed;
}

void OscNamespace::addFeedbackPeer(std::string client) {
    if (!isOnOwnerThread() || client.empty()) {
        return;
    }
    peers_.insert(std::move(client));
}

void OscNamespace::removeFeedbackPeer(std::string_view client) {
    if (!isOnOwnerThread()) {
        return;
    }
    if (const auto it = peers_.find(client); it != peers_.end()) {
        peers_.erase(it);
    }
    if (const auto it = lastSent_.find(client); it != lastSent_.end()) {
        lastSent_.erase(it);
    }
    if (const auto it = suppressedThisFlush_.find(client); it != suppressedThisFlush_.end()) {
        suppressedThisFlush_.erase(it);
    }
    if (const auto it = confirmNextFlush_.find(client); it != confirmNextFlush_.end()) {
        confirmNextFlush_.erase(it);
    }
}

std::size_t OscNamespace::removeFeedbackPeer(std::string_view client, WriteSink& sink) {
    if (!isOnOwnerThread()) {
        return 0U;
    }
    removeFeedbackPeer(client);
    return endClientGestures(client, sink);
}

void OscNamespace::emitGesture(const ResolvedAddress& target,
                               bool active,
                               const WriteOrigin& origin,
                               WriteSink& sink) const {
    switch (target.kind) {
        case TargetKind::TrackVolumeGesture:
            sink.onTrackVolumeGesture(target.trackId, active, origin);
            break;
        case TargetKind::TrackPanGesture:
            sink.onTrackPanGesture(target.trackId, active, origin);
            break;
        case TargetKind::TrackSendGesture:
            sink.onTrackSendGesture(target.trackId, target.sendId, active, origin);
            break;
        case TargetKind::TrackDeviceParamGesture:
            sink.onTrackDeviceParamGesture(target.trackId, target.deviceId, target.paramId,
                                           active, origin);
            break;
        case TargetKind::MasterVolumeGesture:
            sink.onMasterVolumeGesture(active, origin);
            break;
        case TargetKind::MasterPanGesture:
            sink.onMasterPanGesture(active, origin);
            break;
        case TargetKind::MasterDeviceParamGesture:
            sink.onMasterDeviceParamGesture(target.deviceId, target.paramId, active, origin);
            break;
        case TargetKind::FocusedParamGesture:
            sink.onFocusedParamGesture(target.paramId, active, origin);
            break;
        case TargetKind::FocusedMacroGesture:
            sink.onFocusedMacroGesture(target.paramId, active, origin);
            break;
        case TargetKind::FocusedDialGesture:
            sink.onFocusedDialGesture(active, origin);
            break;
        default:
            break;
    }
}

bool OscNamespace::hasActiveGesture(std::string_view client,
                                    std::string_view feedAddr) const noexcept {
    const auto clientIt = activeGestures_.find(client);
    if (clientIt == activeGestures_.end()) {
        return false;
    }
    return clientIt->second.find(feedAddr) != clientIt->second.end();
}

std::size_t OscNamespace::endClientGestures(std::string_view client, WriteSink& sink) {
    if (!isOnOwnerThread()) {
        return 0U;
    }
    const auto it = activeGestures_.find(client);
    if (it == activeGestures_.end()) {
        return 0U;
    }
    WriteOrigin origin;
    origin.actor = Actor::User;
    origin.client = it->first;
    origin.actorDetail = makeActorDetail(it->first);

    auto gestures = std::move(it->second);
    activeGestures_.erase(it);

    for (const auto& [addr, g] : gestures) {
        (void)addr;
        emitGesture(g.target, false, origin, sink);
    }
    return gestures.size();
}

std::size_t OscNamespace::endAllGestures(WriteSink& sink) {
    if (!isOnOwnerThread()) {
        return 0U;
    }
    auto all = std::move(activeGestures_);
    activeGestures_.clear();

    std::size_t closed = 0U;
    for (const auto& [client, gestures] : all) {
        WriteOrigin origin;
        origin.actor = Actor::User;
        origin.client = client;
        origin.actorDetail = makeActorDetail(client);
        for (const auto& [addr, g] : gestures) {
            (void)addr;
            emitGesture(g.target, false, origin, sink);
            ++closed;
        }
    }
    return closed;
}

std::size_t OscNamespace::pollGestures(std::uint64_t nowMs, WriteSink& sink) {
    if (!isOnOwnerThread()) {
        return 0U;
    }
    if (lastNowMs_.has_value() && nowMs < *lastNowMs_) {
        return 0U;
    }
    lastNowMs_ = nowMs;

    std::vector<std::pair<std::string, ResolvedAddress>> expired;
    for (auto clientIt = activeGestures_.begin(); clientIt != activeGestures_.end();) {
        auto& gmap = clientIt->second;
        for (auto gIt = gmap.begin(); gIt != gmap.end();) {
            if (!gIt->second.explicitTouch &&
                static_cast<std::uint64_t>(nowMs - gIt->second.lastActivityMs) >= quietWindowMs_) {
                expired.emplace_back(clientIt->first, gIt->second.target);
                gIt = gmap.erase(gIt);
            } else {
                ++gIt;
            }
        }
        if (gmap.empty()) {
            clientIt = activeGestures_.erase(clientIt);
        } else {
            ++clientIt;
        }
    }

    for (const auto& [client, target] : expired) {
        WriteOrigin origin;
        origin.actor = Actor::User;
        origin.client = client;
        origin.actorDetail = makeActorDetail(client);
        emitGesture(target, false, origin, sink);
    }
    return expired.size();
}

DispatchStatus OscNamespace::dispatchMessage(const Message& message,
                                             std::string_view client,
                                             WriteSink& sink) {
    return dispatchMessage(message, client, sink, lastNowMs_.value_or(0U));
}

DispatchStatus OscNamespace::dispatchMessage(const Message& message,
                                             std::string_view client,
                                             WriteSink& sink,
                                             std::uint64_t nowMs) {
    if (!isOnOwnerThread()) {
        return DispatchStatus::WrongThread;
    }
    if (!policy_.enabled) {
        return DispatchStatus::Disabled;
    }
    if (!policy_.bindValid()) {
        return DispatchStatus::InvalidBindPolicy;
    }
    if (client.empty()) {
        return DispatchStatus::InvalidClient;
    }
    if (!policy_.allows(client)) {
        return DispatchStatus::ClientNotAllowed;
    }
    if (lastNowMs_.has_value() && nowMs < *lastNowMs_) {
        return DispatchStatus::ClockRegression;
    }
    lastNowMs_ = nowMs;

    const auto resolved = resolveAddress(message.address);
    if (!resolved.has_value()) {
        return DispatchStatus::UnknownAddress;
    }

    WriteOrigin origin;
    origin.actor = Actor::User;
    origin.client = std::string(client);
    origin.actorDetail = makeActorDetail(client);

    const std::string_view feedAddr = canonicalFeedAddress(message.address, resolved->kind);
    const std::string_view controlAddr = gestureControlAddress(message.address, resolved->kind);

    auto markSuppressed = [&]() {
        if (!feedAddr.empty()) {
            suppressedThisFlush_[origin.client].emplace(feedAddr);
        }
    };

    auto rejectInvalidArg = [&]() {
        // Even a refused write on a feed-bearing address suppresses same-flush
        // echo and schedules confirmation of the true value on the next flush.
        markSuppressed();
        return DispatchStatus::InvalidArgument;
    };

    // Helper for explicit .../gesture messages.
    auto handleExplicitGesture = [&]() -> DispatchStatus {
        const auto active = extractBool(message.args);
        if (!active.has_value()) {
            return DispatchStatus::InvalidArgument;
        }
        if (*active) {
            auto& clientMap = activeGestures_[origin.client];
            const auto gIt = clientMap.find(controlAddr);
            if (gIt == clientMap.end()) {
                clientMap.emplace(std::string(controlAddr),
                                  ActiveGesture{*resolved, true, nowMs});
                emitGesture(*resolved, true, origin, sink);
            } else {
                // Upgrade an open implicit gesture to explicit without a duplicate begin.
                gIt->second.explicitTouch = true;
                gIt->second.lastActivityMs = nowMs;
            }
            return DispatchStatus::Applied;
        }

        // Release: emit end ONLY if THIS client holds an open gesture (explicit or implicit)
        // on this control. Unmatched or cross-client releases are no-ops (IgnoredRelease).
        const auto clientIt = activeGestures_.find(origin.client);
        if (clientIt == activeGestures_.end()) {
            return DispatchStatus::IgnoredRelease;
        }
        const auto gIt = clientIt->second.find(controlAddr);
        if (gIt == clientIt->second.end()) {
            return DispatchStatus::IgnoredRelease;
        }
        const ResolvedAddress target = gIt->second.target;
        clientIt->second.erase(gIt);
        if (clientIt->second.empty()) {
            activeGestures_.erase(clientIt);
        }
        emitGesture(target, false, origin, sink);
        return DispatchStatus::Applied;
    };

    // Helper for continuous strip/param/macro/dial writes (ADR-0181 d1/d2, SRF-02):
    //   - If this client already holds an explicit touch gesture, write inside it.
    //   - If this client holds an implicit gesture that has already expired under
    //     delayed polling (elapsed >= quietWindowMs_), close the old gesture first
    //     and open a fresh implicit gesture so separated runs are never merged.
    //   - Otherwise open (or renew) one implicit gesture, left open until quiet expiry,
    //     explicit .../gesture 0, or lifecycle cleanup.
    auto withStripGesture = [&](auto&& applyWrite) {
        const auto gKind = gestureKindFor(resolved->kind);
        if (!gKind.has_value()) {
            applyWrite();
            return;
        }
        ResolvedAddress gestureTarget = *resolved;
        gestureTarget.kind = *gKind;

        auto& clientMap = activeGestures_[origin.client];
        const auto gIt = clientMap.find(controlAddr);
        if (gIt == clientMap.end()) {
            clientMap.emplace(std::string(controlAddr),
                              ActiveGesture{gestureTarget, false, nowMs});
            emitGesture(gestureTarget, true, origin, sink);
        } else if (!gIt->second.explicitTouch) {
            if (static_cast<std::uint64_t>(nowMs - gIt->second.lastActivityMs) >= quietWindowMs_) {
                const ResolvedAddress expiredTarget = gIt->second.target;
                gIt->second.target = gestureTarget;
                gIt->second.lastActivityMs = nowMs;
                emitGesture(expiredTarget, false, origin, sink);
                emitGesture(gestureTarget, true, origin, sink);
            } else {
                gIt->second.lastActivityMs = nowMs;
            }
        }
        applyWrite();
    };

    switch (resolved->kind) {
        case TargetKind::TransportPlay: {
            const auto pressed = extractBool(message.args);
            if (!pressed.has_value()) {
                return rejectInvalidArg();
            }
            if (!*pressed) {
                return DispatchStatus::IgnoredRelease;
            }
            sink.onTransportPlay(origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TransportStop: {
            const auto pressed = extractBool(message.args);
            if (!pressed.has_value()) {
                return rejectInvalidArg();
            }
            if (!*pressed) {
                return DispatchStatus::IgnoredRelease;
            }
            sink.onTransportStop(origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TransportRecord: {
            const auto active = extractBool(message.args);
            if (!active.has_value()) {
                return rejectInvalidArg();
            }
            sink.onTransportRecord(*active, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TransportLoop: {
            const auto enabled = extractBool(message.args);
            if (!enabled.has_value()) {
                return rejectInvalidArg();
            }
            sink.onTransportLoop(*enabled, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TransportTempo: {
            const auto bpm = extractTempo(message.args);
            if (!bpm.has_value()) {
                return rejectInvalidArg();
            }
            sink.onTransportTempo(*bpm, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::ReEnableAutomation: {
            const auto pressed = extractBool(message.args);
            if (!pressed.has_value()) {
                return rejectInvalidArg();
            }
            if (!*pressed) {
                return DispatchStatus::IgnoredRelease;
            }
            sink.onReEnableAutomation(origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }

        case TargetKind::TrackVolume: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() { sink.onTrackVolume(resolved->trackId, *v, origin); });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackVolumeGesture:
            return handleExplicitGesture();
        case TargetKind::TrackPan: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() { sink.onTrackPan(resolved->trackId, *v, origin); });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackPanGesture:
            return handleExplicitGesture();
        case TargetKind::TrackMute: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            sink.onTrackMute(resolved->trackId, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackSolo: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            sink.onTrackSolo(resolved->trackId, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackArm: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            sink.onTrackArm(resolved->trackId, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackSend: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() {
                sink.onTrackSend(resolved->trackId, resolved->sendId, *v, origin);
            });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackSendGesture:
            return handleExplicitGesture();
        case TargetKind::TrackDeviceBypass: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            sink.onTrackDeviceBypass(resolved->trackId, resolved->deviceId, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackDeviceParam: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() {
                sink.onTrackDeviceParam(resolved->trackId, resolved->deviceId, resolved->paramId,
                                        *v, origin);
            });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackDeviceParamNudge: {
            const auto n = extractNudge(message.args);
            if (!n.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() {
                sink.onTrackDeviceParamNudge(resolved->trackId, resolved->deviceId,
                                             resolved->paramId, n->ticks, n->fine, origin);
            });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackDeviceParamGesture:
            return handleExplicitGesture();

        case TargetKind::MasterVolume: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() { sink.onMasterVolume(*v, origin); });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterVolumeGesture:
            return handleExplicitGesture();
        case TargetKind::MasterPan: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() { sink.onMasterPan(*v, origin); });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterPanGesture:
            return handleExplicitGesture();
        case TargetKind::MasterMute: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            sink.onMasterMute(*v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterDeviceBypass: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            sink.onMasterDeviceBypass(resolved->deviceId, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterDeviceParam: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() {
                sink.onMasterDeviceParam(resolved->deviceId, resolved->paramId, *v, origin);
            });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterDeviceParamNudge: {
            const auto n = extractNudge(message.args);
            if (!n.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() {
                sink.onMasterDeviceParamNudge(resolved->deviceId, resolved->paramId, n->ticks,
                                              n->fine, origin);
            });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterDeviceParamGesture:
            return handleExplicitGesture();

        case TargetKind::FocusedParam: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() { sink.onFocusedParam(resolved->paramId, *v, origin); });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedParamNudge: {
            const auto n = extractNudge(message.args);
            if (!n.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() {
                sink.onFocusedParamNudge(resolved->paramId, n->ticks, n->fine, origin);
            });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedParamGesture:
            return handleExplicitGesture();
        case TargetKind::FocusedMacro: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() { sink.onFocusedMacro(resolved->paramId, *v, origin); });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedMacroGesture:
            return handleExplicitGesture();
        case TargetKind::FocusedBypass: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            sink.onFocusedBypass(*v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedDial: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() { sink.onFocusedDial(*v, origin); });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedDialNudge: {
            const auto n = extractNudge(message.args);
            if (!n.has_value()) {
                return rejectInvalidArg();
            }
            withStripGesture([&]() { sink.onFocusedDialNudge(n->ticks, n->fine, origin); });
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedDialGesture:
            return handleExplicitGesture();
    }

    return DispatchStatus::UnknownAddress;
}

PacketDispatchResult OscNamespace::dispatchPacket(const Packet& packet,
                                                  std::string_view client,
                                                  WriteSink& sink) {
    return dispatchPacket(packet, client, sink, lastNowMs_.value_or(0U));
}

PacketDispatchResult OscNamespace::dispatchPacket(const Packet& packet,
                                                  std::string_view client,
                                                  WriteSink& sink,
                                                  std::uint64_t nowMs) {
    PacketDispatchResult res;
    if (!isOnOwnerThread()) {
        res.gateStatus = DispatchStatus::WrongThread;
        return res;
    }
    if (!policy_.enabled) {
        res.gateStatus = DispatchStatus::Disabled;
        return res;
    }
    if (!policy_.bindValid()) {
        res.gateStatus = DispatchStatus::InvalidBindPolicy;
        return res;
    }
    if (client.empty()) {
        res.gateStatus = DispatchStatus::InvalidClient;
        return res;
    }
    if (!policy_.allows(client)) {
        res.gateStatus = DispatchStatus::ClientNotAllowed;
        return res;
    }
    if (lastNowMs_.has_value() && nowMs < *lastNowMs_) {
        res.gateStatus = DispatchStatus::ClockRegression;
        return res;
    }

    const auto msgs = flattenMessages(packet);
    res.messageStatuses.reserve(msgs.size());
    for (const Message* m : msgs) {
        const DispatchStatus st = dispatchMessage(*m, client, sink, nowMs);
        res.messageStatuses.push_back(st);
        if (st == DispatchStatus::Applied) {
            ++res.appliedCount;
        } else if (st == DispatchStatus::IgnoredRelease) {
            ++res.ignoredReleaseCount;
        } else {
            ++res.refusedCount;
        }
    }
    return res;
}

bool OscNamespace::updateFeedValue(std::string_view address, float value) {
    if (!isOnOwnerThread() || address.empty() || !std::isfinite(value)) {
        return false;
    }
    feedValues_[std::string(address)] = normalizeZero(value);
    return true;
}

std::vector<FeedbackMessage> OscNamespace::flushFeedback(std::uint64_t nowMs, WriteSink& sink) {
    (void)pollGestures(nowMs, sink);
    return flushFeedback();
}

std::vector<FeedbackMessage> OscNamespace::flushFeedback() {
    std::vector<FeedbackMessage> out;
    if (!isOnOwnerThread()) {
        return out;
    }
    if (!policy_.enabled || !policy_.bindValid()) {
        suppressedThisFlush_.clear();
        confirmNextFlush_.clear();
        return out;
    }

    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> nextConfirm;

    for (const auto& peer : peers_) {
        if (!policy_.allows(peer)) {
            continue;
        }
        const auto suppressedIt = suppressedThisFlush_.find(peer);
        const auto confirmIt = confirmNextFlush_.find(peer);
        auto& sentForPeer = lastSent_[peer];

        for (const auto& [addr, val] : feedValues_) {
            const bool isSuppressed =
                suppressedIt != suppressedThisFlush_.end() &&
                suppressedIt->second.find(addr) != suppressedIt->second.end();
            if (isSuppressed) {
                // Suppress echo in this flush, and schedule next-flush
                // confirmation of the true settled value (even if unchanged).
                nextConfirm[peer].insert(addr);
                continue;
            }
            const bool needsConfirm =
                confirmIt != confirmNextFlush_.end() &&
                confirmIt->second.find(addr) != confirmIt->second.end();
            const auto prevIt = sentForPeer.find(addr);
            if (needsConfirm || prevIt == sentForPeer.end() || prevIt->second != val) {
                out.push_back(FeedbackMessage{peer, addr, val});
                sentForPeer[addr] = val;
            }
        }
    }

    suppressedThisFlush_.clear();
    confirmNextFlush_ = std::move(nextConfirm);
    return out;
}

}  // namespace adi::osc
