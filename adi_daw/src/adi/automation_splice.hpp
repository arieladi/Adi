// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/blob.hpp"
#include "adi/engine/curves.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adi::automation {
/// Owned decoded values in one lane's native clock. The owner supplies the
/// corresponding absolute sample positions from the same captured tempo map.
struct SpliceKnot {
    AutomationPoint point{};
    std::int64_t sample = 0;
    std::optional<engine::CurveSlice> slice;
};
struct SplicePosition {
    std::int64_t native = 0, sample = 0;
};
struct SpliceBounds {
    SplicePosition begin, end;
};
enum class SplicePart { Original, Replacement, Guard };
struct SpliceOrigin {
    SplicePart part = SplicePart::Guard;
    std::size_t index = 0;
    bool operator==(const SpliceOrigin &) const = default;
};
struct SplicedKnot {
    SpliceKnot knot;
    SpliceOrigin pointOrigin;
    /// Origin of an outgoing curve range, including a newly sliced domain.
    /// Two pieces can derive from one old range; its persistent ID may only
    /// survive on one piece. The owner allocates the other through CopyIds.
    std::optional<SpliceOrigin> rangeOrigin;
};
struct SplicePlan {
    bool ok = false;
    std::string error;
    std::vector<SplicedKnot> knots;
};
inline constexpr std::size_t kSplicePointBudget = 1u << 20;
/// Pure owner-side foundation for AU-05. Replace [begin,end), retaining the
/// old rendered curve outside it and incoming values inside it. Replacement
/// includes both exact boundary positions; its final knot is an incoming End
/// value, with no outgoing slice. Coincident guards preserve discontinuities.
///
/// Original/replacement IDs are values, never allocated or repaired here.
/// Guards have ID zero and distinct provenance, even beside legacy ID zero.
/// The owner assigns persistent point/range IDs and retains complete native
/// rows/header/trailer when assembling its single captured transaction. It
/// validates the AAUT layout before decoding; this API cannot carry unknown
/// record tails. No Store, SQL, settings, transport, file or callback access.
[[nodiscard]] SplicePlan planEnvelopeSplice(std::span<const SpliceKnot> original,
                                            double defaultValue, SpliceBounds,
                                            std::span<const SpliceKnot> replacement,
                                            std::size_t pointBudget = kSplicePointBudget);
} // namespace adi::automation
