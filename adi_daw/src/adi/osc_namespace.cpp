// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-09: OSC fixed namespace, client allow-list, attribution, and no-echo
// feedback scheduling.

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
constexpr std::size_t kMaxPathSegments = 8U;

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

/// Parse a canonical 1-based positive decimal uint32 (no leading zeros, no sign).
[[nodiscard]] std::optional<std::uint32_t> parsePositiveIndex(std::string_view s) noexcept {
    if (s.empty() || s[0] < '1' || s[0] > '9') {
        return std::nullopt;
    }
    std::uint64_t acc = 0U;
    constexpr std::uint64_t kMaxU32 = (std::numeric_limits<std::uint32_t>::max)();
    for (char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        acc = (acc * 10U) + static_cast<std::uint64_t>(c - '0');
        if (acc > kMaxU32) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(acc);
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

}  // namespace

bool ClientPolicy::allows(std::string_view client) const noexcept {
    if (!enabled || client.empty()) {
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
        case DispatchStatus::InvalidClient: return "invalid_client";
        case DispatchStatus::ClientNotAllowed: return "client_not_allowed";
        case DispatchStatus::UnknownAddress: return "unknown_address";
        case DispatchStatus::InvalidArgument: return "invalid_argument";
    }
    return "unknown";
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

    // /adi/track/{n}/...
    if (seg[1] == "track") {
        if (n < 4U) {
            return std::nullopt;
        }
        const auto trackIdx = parsePositiveIndex(seg[2]);
        if (!trackIdx.has_value()) {
            return std::nullopt;
        }
        out.trackIndex = *trackIdx;

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

        if (n == 5U && seg[3] == "send") {
            const auto sendIdx = parsePositiveIndex(seg[4]);
            if (!sendIdx.has_value()) {
                return std::nullopt;
            }
            out.kind = TargetKind::TrackSend;
            out.sendIndex = *sendIdx;
            return out;
        }

        if (seg[3] == "device" && n >= 6U) {
            const auto devIdx = parsePositiveIndex(seg[4]);
            if (!devIdx.has_value()) {
                return std::nullopt;
            }
            out.deviceIndex = *devIdx;
            if (n == 6U && seg[5] == "bypass") {
                out.kind = TargetKind::TrackDeviceBypass;
                return out;
            }
            if (seg[5] == "param" && (n == 7U || n == 8U)) {
                const auto paramIdx = parsePositiveIndex(seg[6]);
                if (!paramIdx.has_value()) {
                    return std::nullopt;
                }
                out.paramIndex = *paramIdx;
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
        if (seg[2] == "device" && n >= 5U) {
            const auto devIdx = parsePositiveIndex(seg[3]);
            if (!devIdx.has_value()) {
                return std::nullopt;
            }
            out.deviceIndex = *devIdx;
            if (n == 5U && seg[4] == "bypass") {
                out.kind = TargetKind::MasterDeviceBypass;
                return out;
            }
            if (seg[4] == "param" && (n == 6U || n == 7U)) {
                const auto paramIdx = parsePositiveIndex(seg[5]);
                if (!paramIdx.has_value()) {
                    return std::nullopt;
                }
                out.paramIndex = *paramIdx;
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
        if (n == 4U && seg[2] == "macro") {
            const auto macroIdx = parsePositiveIndex(seg[3]);
            if (!macroIdx.has_value()) {
                return std::nullopt;
            }
            out.kind = TargetKind::FocusedMacro;
            out.paramIndex = *macroIdx;
            return out;
        }
        if (seg[2] == "param" && (n == 4U || n == 5U)) {
            const auto paramIdx = parsePositiveIndex(seg[3]);
            if (!paramIdx.has_value()) {
                return std::nullopt;
            }
            out.paramIndex = *paramIdx;
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

OscNamespace::OscNamespace(ClientPolicy policy) : policy_(std::move(policy)) {}

void OscNamespace::setPolicy(ClientPolicy policy) {
    policy_ = std::move(policy);
}

void OscNamespace::addFeedbackPeer(std::string client) {
    if (!client.empty()) {
        peers_.insert(std::move(client));
    }
}

void OscNamespace::removeFeedbackPeer(std::string_view client) {
    if (const auto it = peers_.find(client); it != peers_.end()) {
        peers_.erase(it);
    }
    if (const auto it = lastSent_.find(client); it != lastSent_.end()) {
        lastSent_.erase(it);
    }
    if (const auto it = suppressedThisFlush_.find(client); it != suppressedThisFlush_.end()) {
        suppressedThisFlush_.erase(it);
    }
}

DispatchStatus OscNamespace::dispatchMessage(const Message& message,
                                             std::string_view client,
                                             WriteSink& sink) {
    if (!policy_.enabled) {
        return DispatchStatus::Disabled;
    }
    if (client.empty()) {
        return DispatchStatus::InvalidClient;
    }
    if (!policy_.allows(client)) {
        return DispatchStatus::ClientNotAllowed;
    }

    const auto resolved = resolveAddress(message.address);
    if (!resolved.has_value()) {
        return DispatchStatus::UnknownAddress;
    }

    WriteOrigin origin;
    origin.actor = Actor::User;
    origin.client = std::string(client);
    origin.actorDetail = makeActorDetail(client);

    auto markSuppressed = [&]() {
        suppressedThisFlush_[origin.client].insert(message.address);
    };

    switch (resolved->kind) {
        case TargetKind::TransportPlay: {
            const auto pressed = extractBool(message.args);
            if (!pressed.has_value()) {
                return DispatchStatus::InvalidArgument;
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
                return DispatchStatus::InvalidArgument;
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
                return DispatchStatus::InvalidArgument;
            }
            sink.onTransportRecord(*active, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TransportLoop: {
            const auto enabled = extractBool(message.args);
            if (!enabled.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTransportLoop(*enabled, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TransportTempo: {
            const auto bpm = extractTempo(message.args);
            if (!bpm.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTransportTempo(*bpm, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::ReEnableAutomation: {
            const auto pressed = extractBool(message.args);
            if (!pressed.has_value()) {
                return DispatchStatus::InvalidArgument;
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
                return DispatchStatus::InvalidArgument;
            }
            sink.onTrackVolume(resolved->trackIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackPan: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTrackPan(resolved->trackIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackMute: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTrackMute(resolved->trackIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackSolo: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTrackSolo(resolved->trackIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackArm: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTrackArm(resolved->trackIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackSend: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTrackSend(resolved->trackIndex, resolved->sendIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackDeviceBypass: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTrackDeviceBypass(resolved->trackIndex, resolved->deviceIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackDeviceParam: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTrackDeviceParam(resolved->trackIndex, resolved->deviceIndex,
                                    resolved->paramIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackDeviceParamNudge: {
            const auto n = extractNudge(message.args);
            if (!n.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTrackDeviceParamNudge(resolved->trackIndex, resolved->deviceIndex,
                                         resolved->paramIndex, n->ticks, n->fine, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::TrackDeviceParamGesture: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onTrackDeviceParamGesture(resolved->trackIndex, resolved->deviceIndex,
                                           resolved->paramIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }

        case TargetKind::MasterVolume: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onMasterVolume(*v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterPan: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onMasterPan(*v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterMute: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onMasterMute(*v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterDeviceBypass: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onMasterDeviceBypass(resolved->deviceIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterDeviceParam: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onMasterDeviceParam(resolved->deviceIndex, resolved->paramIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterDeviceParamNudge: {
            const auto n = extractNudge(message.args);
            if (!n.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onMasterDeviceParamNudge(resolved->deviceIndex, resolved->paramIndex, n->ticks,
                                          n->fine, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::MasterDeviceParamGesture: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onMasterDeviceParamGesture(resolved->deviceIndex, resolved->paramIndex, *v,
                                            origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }

        case TargetKind::FocusedParam: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onFocusedParam(resolved->paramIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedParamNudge: {
            const auto n = extractNudge(message.args);
            if (!n.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onFocusedParamNudge(resolved->paramIndex, n->ticks, n->fine, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedParamGesture: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onFocusedParamGesture(resolved->paramIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedMacro: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onFocusedMacro(resolved->paramIndex, *v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedBypass: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onFocusedBypass(*v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedDial: {
            const auto v = extractNormalized(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onFocusedDial(*v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedDialNudge: {
            const auto n = extractNudge(message.args);
            if (!n.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onFocusedDialNudge(n->ticks, n->fine, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
        case TargetKind::FocusedDialGesture: {
            const auto v = extractBool(message.args);
            if (!v.has_value()) {
                return DispatchStatus::InvalidArgument;
            }
            sink.onFocusedDialGesture(*v, origin);
            markSuppressed();
            return DispatchStatus::Applied;
        }
    }

    return DispatchStatus::UnknownAddress;
}

PacketDispatchResult OscNamespace::dispatchPacket(const Packet& packet,
                                                  std::string_view client,
                                                  WriteSink& sink) {
    PacketDispatchResult res;
    if (!policy_.enabled) {
        res.gateStatus = DispatchStatus::Disabled;
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

    const auto msgs = flattenMessages(packet);
    res.messageStatuses.reserve(msgs.size());
    for (const Message* m : msgs) {
        const DispatchStatus st = dispatchMessage(*m, client, sink);
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
    if (address.empty() || !std::isfinite(value)) {
        return false;
    }
    feedValues_[std::string(address)] = normalizeZero(value);
    return true;
}

std::vector<FeedbackMessage> OscNamespace::flushFeedback() {
    std::vector<FeedbackMessage> out;
    if (!policy_.enabled) {
        suppressedThisFlush_.clear();
        return out;
    }

    for (const auto& peer : peers_) {
        if (!policy_.allows(peer)) {
            continue;
        }
        const auto suppressedIt = suppressedThisFlush_.find(peer);
        auto& sentForPeer = lastSent_[peer];

        for (const auto& [addr, val] : feedValues_) {
            if (suppressedIt != suppressedThisFlush_.end() &&
                suppressedIt->second.find(addr) != suppressedIt->second.end()) {
                continue;
            }
            const auto prevIt = sentForPeer.find(addr);
            if (prevIt == sentForPeer.end() || prevIt->second != val) {
                out.push_back(FeedbackMessage{peer, addr, val});
                sentForPeer[addr] = val;
            }
        }
    }

    suppressedThisFlush_.clear();
    return out;
}

}  // namespace adi::osc
