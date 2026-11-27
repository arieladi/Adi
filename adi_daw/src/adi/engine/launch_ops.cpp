// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_ops.hpp"

#include "adi/applied_batches.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/engine/launch_gestures.hpp"
#include "adi/engine/session.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace adi::engine {
namespace {
constexpr Field incarnation{.key = "incarnation", .type = FieldType::Int,
    .description = "Launch generation captured with the command", .minimum = 1, .fullUint64 = true};
constexpr Field track{.key = "track", .type = FieldType::Int,
    .description = "Literal track ID", .minimum = 1};
constexpr Field scene{.key = "scene", .type = FieldType::Int,
    .description = "Literal scene ID", .minimum = 1};
constexpr Field clip{.key = "clip", .type = FieldType::Int,
    .description = "Literal clip ID", .minimum = 1};
constexpr Field velocity{.key = "velocity", .type = FieldType::Real, .required = false,
    .description = "Normalized launch velocity, default 1", .minimum = 0, .maximum = 1};
constexpr Field release{.key = "release", .type = FieldType::Bool, .required = false,
    .description = "Submit a release edge, default false"};
constexpr auto optionalTrack = [] {
    auto field = track;
    field.required = false;
    return field;
}();
constexpr std::array clipFields{track, scene, clip, incarnation, release, velocity};
constexpr std::array sceneFields{scene, incarnation, velocity};
constexpr std::array stopTrackFields{track, incarnation};
constexpr std::array stopAllFields{incarnation};
constexpr std::array cancelFields{scene, incarnation};
constexpr std::array backFields{optionalTrack, incarnation};

// The journal records an audit row outside Undo. Only the live owner forwards
// a successfully committed batch; SQL execution itself never touches playback.
bool auditLaunchOperation(OpContext&, const Payload&, std::string&) { return true; }

constexpr OpDescriptor descriptors[]{
    {"session.launchClip", "Launch or release a Session clip", Scope::Session, EngineImpact::None,
        clipFields, false, true, auditLaunchOperation, nullptr, ""},
    {"session.launchScene", "Launch a Session scene", Scope::Session, EngineImpact::None,
        sceneFields, false, true, auditLaunchOperation, nullptr, ""},
    {"session.stopTrack", "Stop Session playback on a track", Scope::Session, EngineImpact::None,
        stopTrackFields, false, true, auditLaunchOperation, nullptr, ""},
    {"session.stopAll", "Stop Session playback on all tracks", Scope::Session, EngineImpact::None,
        stopAllFields, false, true, auditLaunchOperation, nullptr, ""},
    {"session.cancelSceneLaunch", "Cancel a queued Session scene", Scope::Session, EngineImpact::None,
        cancelFields, false, true, auditLaunchOperation, nullptr, ""},
    {"session.backToArrangement", "Resume Arrangement playback on one or all tracks",
        Scope::Session, EngineImpact::None, backFields, false, true, auditLaunchOperation, nullptr, ""},
};

constexpr std::array requestKinds{
    LaunchRequestKind::SlotPress, LaunchRequestKind::ScenePress, LaunchRequestKind::StopTrack,
    LaunchRequestKind::StopAll, LaunchRequestKind::CancelScene, LaunchRequestKind::BackToArrangement};
static_assert(requestKinds.size() == std::size(descriptors));

const OpDescriptor* findDescriptor(std::string_view name) noexcept {
    const auto found = std::ranges::find(descriptors, name, &OpDescriptor::name);
    return found == std::end(descriptors) ? nullptr : found;
}

std::optional<std::int64_t> id(const Payload& payload, const char* key) {
    const auto found = payload.find(key);
    return found == payload.end() ? std::nullopt : std::optional{found->get<std::int64_t>()};
}

template<class Operation, class Name>
LaunchOperationResult dispatch(Session& session, std::span<const Operation> operations,
                               Name name, bool ignoreNonLaunchOperations) {
    using Status = LaunchOperationStatus;
    std::array<LaunchRequest, LaunchGestureQueue::capacity> requests;
    std::size_t count = 0;
    for (std::size_t i = 0; i < operations.size(); ++i) {
        const auto& op = operations[i];
        const auto* descriptor = findDescriptor(name(op));
        if (!descriptor) {
            if (ignoreNonLaunchOperations)
                continue;
            return {Status::UnknownOperation, i, {}};
        }
        if (count == requests.size())
            return {Status::OversizeGesture, i, {}};
        auto issues = validateSubmission(*descriptor, op.payload);
        if (!issues.empty())
            return {Status::InvalidPayload, i, std::move(issues)};
        const auto index = static_cast<std::size_t>(descriptor - std::begin(descriptors));
        const auto kind = requestKinds[index] == LaunchRequestKind::SlotPress && op.payload.value("release", false)
            ? LaunchRequestKind::SlotRelease : requestKinds[index];
        requests[count++] = {kind, id(op.payload, "track"), id(op.payload, "scene"), id(op.payload, "clip"),
            op.payload.at("incarnation").template get<std::uint64_t>(), op.payload.value("velocity", 1.0f)};
    }
    if (count == 0)
        return {};
    return {session.submitLaunchGesture(std::span(requests).first(count)) ? Status::Queued : Status::MailboxUnavailable,
        0, {}};
}
} // namespace

std::span<const OpDescriptor> launchOperationDescriptors() noexcept { return descriptors; }

bool isSessionLaunchOperation(std::string_view name) noexcept { return findDescriptor(name) != nullptr; }

LaunchOperationResult dispatchLaunchOperations(Session& session, std::span<const OpRequest> operations) {
    if (audio::onAudioThread)
        return {LaunchOperationStatus::AudioThread, 0, {}};
    if (operations.size() > LaunchGestureQueue::capacity)
        return {LaunchOperationStatus::OversizeGesture, LaunchGestureQueue::capacity, {}};
    return dispatch(session, operations, [](const OpRequest& command) { return std::string_view(command.opType); }, false);
}

LaunchOperationResult dispatchLaunchOperations(Session& session, const AppliedBatch& batch) {
    if (audio::onAudioThread)
        return {LaunchOperationStatus::AudioThread, 0, {}};
    if (batch.kind() != AppliedBatchKind::Commit)
        return {};
    if (!batch.source().alive() || !batch.source().sameConnection(session.appliedNotifications()))
        return {LaunchOperationStatus::UnrelatedProject, 0, {}};
    return dispatch(session, batch.operations(), [](const AppliedOperation& command) {
        return std::string_view(command.dispatchedType);
    }, true);
}
} // namespace adi::engine
