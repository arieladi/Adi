// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/plan.hpp"
#include "adi/ops.hpp"
#include "adi/store_rows.hpp"
#include <map>
#include <set>
namespace adi::freeze {
struct GroupMember {
    std::int64_t id = 0;
    bool armed = false;
};
using GroupScopes = std::map<std::int64_t, std::vector<GroupMember>>;
GroupScopes groups(SQLite::Database &);
std::set<std::int64_t> groupMembers(const rows::Model &, std::int64_t root);
// Capture requires a full, unsoloed mix. External audio/key inputs and nested
// existing caches require later modes; never silently bake an incomplete bus.
void checkGroup(const rows::Model &, std::int64_t root, const std::set<std::int64_t> &members,
                bool capturing);
bool childAudioRoutesOutside(const rows::Model &, std::int64_t root, const std::set<std::int64_t> &members);
Payload groupState(SQLite::Database &, std::int64_t root);
OpRequest groupChange(std::int64_t root, const Payload &before, const Payload &after);
void validateGroups(Store &);
bool groupControlsLive(SQLite::Database &, std::span<const OpRequest>);
bool groupApply(OpContext &, const Payload &, std::string &);
bool groupInverse(OpContext &, const Payload &, Payload &, std::string &);
// Filter an already planned graph; removing rows before planning could invent
// a default route where an explicit child output used to be.
void pruneGroupPlan(engine::GraphPlan &, const std::set<std::int64_t> &removed,
                    const std::set<std::int64_t> &cachedRoots);
} // namespace adi::freeze
