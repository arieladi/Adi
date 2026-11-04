// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
namespace adi::freeze {
bool validated(Store &) noexcept;
void remember(Store &) noexcept;
Payload state(SQLite::Database &, std::int64_t track);
std::vector<Payload> renders(SQLite::Database &);
// Pure SQL project constraint. Unknown edits get a full causal check; live
// control batches may omit hashing after their affected scope is classified.
void validate(Store &, bool source = true);
bool liveControlBatch(std::span<const OpRequest>);
bool apply(OpContext &, const Payload &, std::string &);
bool inverse(OpContext &, const Payload &, Payload &, std::string &);
} // namespace adi::freeze
