// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/edit_rows.hpp"
#include "adi/edit_time.hpp"
#include "adi/clip_image.hpp"
namespace adi::edit {
/// Prepare global envelopes only; clip envelopes follow their material.
/// The caller holds a read snapshot and owns the resulting row delta.
void insertAutomationTime(Store&,RowEdits&,clip_edit::CopyIds&,const TimeMap&,
                          std::int64_t atTicks,std::int64_t ticks,
                          std::int64_t atNanos,std::int64_t nanos,double rate);
void deleteAutomationTime(Store&,RowEdits&,clip_edit::CopyIds&,const TimeMap&,
                         std::int64_t beginTicks,std::int64_t endTicks,
                         std::int64_t beginNanos,std::int64_t endNanos,double rate);
}
