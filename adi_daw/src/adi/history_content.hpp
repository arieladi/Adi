// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <functional>
namespace adi::history_detail {
// Owner transaction: visit a legacy checkpoint's typed state, including media
// absent from the current pool. Rewrites refresh the optional content link.
// Older read-only backups may lack the new main-schema content tables.
void visitCheckpointState(SQLite::Database&,const std::function<bool(Payload&)>&,bool write);
}
