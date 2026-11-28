// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/automation_splice.hpp"
#include "adi/ops.hpp"
#include <string_view>

namespace adi {
struct OpTransactionInputs;
namespace clip_edit { class CopyIds; }
namespace edit { class RowEdits; class TimeMap; }
}
namespace adi::automation {
/// Absolute positions in the destination lane's declared native clock. No
/// projected sample positions or persistent IDs are accepted from the caller.
struct NativeSpliceKnot {
    AutomationPoint point{};
    std::optional<engine::CurveSlice> slice;
};
/// Add a replacement to the caller's row plan using its read snapshot and
/// shared ID allocator. Earlier staged replacements and new lanes are visible.
/// Optional guards describe the stored snapshot. Discard the plan on failure.
void stageEnvelopeSplice(Store &, edit::RowEdits &, clip_edit::CopyIds &, const edit::TimeMap &,
                         std::int64_t laneId, std::int64_t begin, std::int64_t end,
                         std::span<const NativeSpliceKnot> replacement, double sampleRate,
                         OpTransactionInputs *guards = nullptr,
                         std::size_t pointBudget = kSplicePointBudget);
struct PreparedEnvelopeSplice {
    bool ok = false;
    std::string error;
    std::vector<OpRequest> requests;
    /// AU-03: the native bounds project to one sample; the caller may write
    /// the points directly instead (a recording pass shorter than a sample).
    bool subSample = false;
};
/// AU-02/AU-03: what a recording pass or a shape insertion asks of a splice:
/// the automation_data row it addresses (a new stream takes this ID), the
/// Undo label and the actor detail (a thinning report).
struct EnvelopeSpliceRequest {
    std::optional<std::int64_t> dataId;
    std::string_view label = "Replace Automation";
    std::string_view actorDetail{};
    std::optional<Payload> selection{};
};
/// Owner-thread storage adapter for AU05. Explicit incoming values include
/// begin and end; no move/copy/delete material policy is selected here. Captures
/// complete lane/data/range/tempo rows in one read snapshot and returns one
/// sealed arranger.splice. Ordinary journal/submitter commit enforces the same
/// source/head/read-set guards. A rollback requires fresh preparation.
///
/// Only current AAUT v1/32-byte records are editable. Header flags and opaque
/// trailer/SQL columns survive; future layouts refuse before any write. Outside
/// original point IDs survive literally, incoming and guard IDs are fresh in
/// this stream; split range rows retain their identity on at most one piece.
[[nodiscard]] PreparedEnvelopeSplice
prepareEnvelopeSplice(Store &, std::int64_t laneId, std::int64_t begin, std::int64_t end,
                      std::span<const NativeSpliceKnot> replacement, double sampleRate,
                      std::size_t pointBudget = kSplicePointBudget,
                      std::string_view label = "Replace Automation", std::optional<Payload> selection = {});
/// The same with a request's data address, label and actor detail. Its
/// boundaries must be ordered; one sample between them sets `subSample`.
[[nodiscard]] PreparedEnvelopeSplice
prepareEnvelopeSplice(Store &, std::int64_t laneId, std::int64_t begin, std::int64_t end,
                      std::span<const NativeSpliceKnot> replacement, double sampleRate,
                      std::size_t pointBudget, EnvelopeSpliceRequest request);
/// The Arrangement command's default tolerance as a fraction of the
/// displayed parameter range. Live's reduction tolerance is unmeasured.
inline constexpr double kSimplifyRelativeTolerance = .005;
/// Prepare one sealed edit in the lane's native clock, or return an empty
/// request list when no reduction fits. Existing endpoint identities and
/// authored curves outside the fitted intervals stay in place.
[[nodiscard]] PreparedEnvelopeSplice
prepareSimplifyEnvelope(Store &, std::int64_t laneId, std::int64_t begin, std::int64_t end,
                        double sampleRate, double maxValueError, std::optional<Payload> selection = {});
} // namespace adi::automation
