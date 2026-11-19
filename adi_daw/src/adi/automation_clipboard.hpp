// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/automation_time_edit.hpp"
namespace adi::edit {
// Owned clipboard data. Preparation remains inside the caller's read snapshot.
Payload copyAutomationTime(Store&,const TimeMap&,std::int64_t beginTicks,std::int64_t endTicks,
                           std::int64_t beginNanos,std::int64_t endNanos,double rate);
void pasteAutomationTime(Store&,RowEdits&,clip_edit::CopyIds&,const Payload&,
                        std::int64_t atTicks,std::int64_t ticks,std::int64_t atNanos,
                        std::int64_t nanos,double rate);
}
