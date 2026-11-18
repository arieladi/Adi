// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/automation_splice.hpp"
#include "adi/ops.hpp"

namespace adi::automation {
/// Absolute positions in the destination lane's declared native clock. No
/// projected sample positions or persistent IDs are accepted from the caller.
struct NativeSpliceKnot {
    AutomationPoint point{};
    std::optional<engine::CurveSlice> slice;
};
struct PreparedEnvelopeSplice {
    bool ok = false;
    std::string error;
    std::vector<OpRequest> requests;
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
                      std::size_t pointBudget = kSplicePointBudget);
} // namespace adi::automation
