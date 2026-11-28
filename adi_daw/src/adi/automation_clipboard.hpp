// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/automation_time_edit.hpp"
#include "adi/automation_splice_store.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/clip_commands.hpp"
namespace adi::edit {
// Owned clipboard data. Preparation remains inside the caller's read snapshot.
Payload copyAutomationTime(Store&,const TimeMap&,std::int64_t beginTicks,std::int64_t endTicks,
                           std::int64_t beginNanos,std::int64_t endNanos,double rate);
void pasteAutomationTime(Store&,RowEdits&,clip_edit::CopyIds&,const Payload&,
                        std::int64_t atTicks,std::int64_t ticks,std::int64_t atNanos,
                        std::int64_t nanos,double rate);
// A musical time selection in literal automation lanes, in display order.
struct AutomationRangeCopy {
    bool ok=false;
    std::string error;
    std::int64_t ticks=0;
    std::vector<Payload> envelopes;
};
[[nodiscard]] AutomationRangeCopy copyAutomationRange(Store&,std::span<const automation::LaneSpec>,
    std::int64_t begin,std::int64_t end,double rate);
[[nodiscard]] clip_edit::PreparedEdit prepareDeleteAutomationRange(Store&,std::span<const automation::LaneSpec>,
    std::int64_t begin,std::int64_t end,double rate);
[[nodiscard]] clip_edit::PreparedEdit preparePasteAutomationRange(Store&,const AutomationRangeCopy&,
    std::span<const automation::LaneSpec>,std::int64_t at,double rate);

// Display value bounds before the gesture, and the new bottom/top values at
// the left and right time edges. Equal maps at the two edges stretch vertically;
// different maps skew. Values are in the lane's own units.
struct AutomationValueTransform {
    double minimum=0,maximum=1;
    double leftMinimum=0,leftMaximum=1,rightMinimum=0,rightMaximum=1;
};

struct AutomationTransform {
    std::int64_t sourceBegin=0,sourceEnd=0,destinationBegin=0,destinationEnd=0;
    std::optional<AutomationValueTransform> values;
};

// Transform one existing musical Arrangement envelope. Outside the combined
// source and destination span the splice preserves native rows and curve domains.
// A narrower destination holds its nearer transformed endpoint in vacated time.
[[nodiscard]] automation::PreparedEnvelopeSplice prepareAutomationTransform(
    Store&,std::int64_t laneId,const AutomationTransform&,double rate,
    std::string_view label="Transform Automation",std::optional<Payload> selection={});

// Independent material snapshot, including all global envelopes on the track.
Payload copyAutomationMaterial(Store&,const TimeMap&,std::int64_t track,
                               clip_edit::TimePoint begin,clip_edit::TimePoint end,double rate);
// Replace the destination span, preserving its envelope outside both edges.
// An empty clipboard clears the selected span.
void placeAutomationMaterial(Store&,RowEdits&,clip_edit::CopyIds&,const TimeMap&,
                             std::int64_t track,clip_edit::TimePoint begin,clip_edit::TimePoint end,
                             const Payload& clipboard,double rate);
}
