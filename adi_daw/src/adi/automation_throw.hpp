// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// AU-10 (ADR-0254 d3): the Throw. Over a time selection, one undo step writes
// a rise, a hold and a fall on the chosen target's automation lane, through
// AU-02's shared splice (automation_splice_store.hpp), so the lane outside the
// selection and its release is unchanged and its old curve returns at the edge.
//
// The target is the caller's: a send to a return (the command closes what goes
// into the effect, so the tail rings on after the range), or a device's own
// mix or wet parameter on the track (closing it closes the tail with it; the
// release holds it open). Which parameter is a device's mix comes from SRF-14's
// role tags, and a rack's wet chain waits for DEV-01: neither is decided here.
#include "adi/automation_splice_store.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace adi::automation {
struct ThrowSpec {
    /// The time selection, in the lane's native clock (ticks, or nanoseconds
    /// for a lane on the seconds clock). begin < end.
    std::int64_t begin = 0, end = 0;
    /// How long after `begin` the target takes to open; 0 opens it at once.
    std::int64_t rise = 0;
    /// How long after `end` it takes to close again; at least one unit. On a
    /// send this is how long the input stays open; on a mix, how long the
    /// tail is held open.
    std::int64_t release = 1;
    /// The target's value while thrown, in the lane's own units.
    double open = 1.0;
};
/// The lane's value at a native position, as it plays now (its default with
/// no points). Owner thread; reads the Store only.
[[nodiscard]] std::optional<double> laneValueAt(Store &, std::int64_t laneId, std::int64_t native,
                                                double sampleRate, std::string &error);
/// One sealed splice over [begin, end + release]: from the lane's own value at
/// `begin` it rises to `open` by begin + rise, holds to `end`, and falls back
/// to the lane's own value at end + release, so the lane is continuous at both
/// edges. The lane must exist and be a global lane (not a clip's).
[[nodiscard]] PreparedEnvelopeSplice prepareThrow(Store &, std::int64_t laneId, const ThrowSpec &,
                                                  double sampleRate,
                                                  std::string_view label = "Throw");
} // namespace adi::automation
