// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/freeze_groups.hpp"
#include "adi/freeze_state.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace adi::freeze {
namespace {
Payload decode(const SQLite::Column &column) {
    const auto size = column.getBytes();
    if (size < 1 || size > 1024 * 1024)
        throw std::runtime_error("invalid frozen group membership size");
    const auto *data = static_cast<const std::byte *>(column.getBlob());
    std::string error;
    const auto value = decodeFromLog({data, static_cast<std::size_t>(size)}, error);
    if (!value)
        throw std::runtime_error("invalid frozen group membership: " + error);
    return *value;
}
std::vector<GroupMember> membersOf(const Payload &members) {
    if (!members.is_array() || members.size() > 4095)
        throw std::runtime_error("invalid frozen group members");
    std::vector<GroupMember> result;
    std::set<std::int64_t> unique;
    for (const auto &item : members) {
        if (!item.is_object() || item.size() != 2 || !item.contains("id") || !item.contains("armed"))
            throw std::runtime_error("invalid frozen group member");
        const auto id = detail::signedInteger(item.at("id")), armed = detail::signedInteger(item.at("armed"));
        if (id == 0 || (armed != 0 && armed != 1) || !unique.insert(id).second)
            throw std::runtime_error("invalid frozen group member identity or arm");
        result.push_back({id, armed != 0});
    }
    return result;
}
Payload memberPayload(const std::vector<GroupMember> &members) {
    Payload rows = Payload::array();
    for (const auto &member : members)
        rows.push_back({{"id", member.id}, {"armed", member.armed ? 1 : 0}});
    return rows;
}
} // namespace
GroupScopes groups(SQLite::Database &db) {
    GroupScopes result;
    if (!db.tableExists("frozen_groups"))
        return result;
    SQLite::Statement query(db, "SELECT track_id,members FROM frozen_groups ORDER BY track_id");
    while (query.executeStep())
        result.emplace(query.getColumn(0).getInt64(), membersOf(decode(query.getColumn(1))));
    return result;
}
std::set<std::int64_t> groupMembers(const rows::Model &model, std::int64_t root) {
    const auto found = std::find_if(model.tracks.begin(), model.tracks.end(),
                                    [root](const auto &track) { return track.id == root; });
    if (found == model.tracks.end() || found->kind != "group")
        throw std::runtime_error("whole-group freeze requires a group track");
    std::set<std::int64_t> members{root};
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto &track : model.tracks)
            if (track.parentId && members.contains(*track.parentId) && !members.contains(track.id)) {
                if (members.size() >= 4096)
                    throw std::runtime_error("frozen group exceeds 4096 tracks");
                members.insert(track.id);
                changed = true;
            }
    }
    return members;
}
void checkGroup(const rows::Model &model, std::int64_t root, const std::set<std::int64_t> &members,
                bool capturing) {
    const auto plan = engine::planGraph(model);
    if (!model.problems.empty() || !plan.problems.empty())
        throw std::runtime_error("group freeze requires a resolved routing graph");
    for (const auto &track : model.tracks) {
        if (capturing && track.soloed)
            throw std::runtime_error("turn off solo before capturing a whole group mix");
        if (!members.contains(track.id) || track.id == root)
            continue;
        if (track.kind == "master" || track.frozen)
            throw std::runtime_error("unfreeze group members first; a group cannot contain the master");
    }
    for (const auto &edge : plan.edges)
        if (members.contains(plan.nodes.at(edge.to).trackId) &&
            !members.contains(plan.nodes.at(edge.from).trackId))
            throw std::runtime_error(
                "group freeze is unavailable with audio or sidechain inputs from outside the group");
    for (const auto &strip : model.strips)
        if (strip.trackId != root && members.contains(strip.trackId) && strip.vcaGroupId &&
            !members.contains(*strip.vcaGroupId))
            throw std::runtime_error("whole-group freeze requires its baked VCA controls inside the group");
}
bool childAudioRoutesOutside(const rows::Model &model, std::int64_t root,
                             const std::set<std::int64_t> &members) {
    const auto plan = engine::planGraph(model);
    return std::any_of(plan.edges.begin(), plan.edges.end(), [&](const auto &edge) {
        const auto source = plan.nodes.at(edge.from).trackId;
        return source != root && members.contains(source) &&
               !members.contains(plan.nodes.at(edge.to).trackId);
    });
}
Payload groupState(SQLite::Database &db, std::int64_t root) {
    const auto all = groups(db);
    const auto found = all.find(root);
    return {{"members", found == all.end() ? Payload() : memberPayload(found->second)}};
}
OpRequest groupChange(std::int64_t root, const Payload &before, const Payload &after) {
    OpRequest request;
    request.opType = "group.setFreezeScope";
    request.payload = {{"id", root}, {"before", before}, {"after", after}};
    request.label = "Freeze Group";
    request.targetKind = "track";
    request.targetId = root;
    return request;
}
void validateGroups(Store &store) {
    const auto scopes = groups(store.db());
    if (scopes.empty())
        return;
    const auto model = rows::readModel(store);
    for (const auto &[root, members] : scopes) {
        if (!detail::captureRow(store.db(), "frozen_renders", "track_id", root))
            throw std::runtime_error("frozen group has no root cache");
        auto expected = groupMembers(model, root);
        expected.erase(root);
        std::set<std::int64_t> actual;
        for (const auto &member : members) {
            actual.insert(member.id);
            const auto row = detail::captureRow(store.db(), "tracks", "id", member.id);
            if (!row || row->at("record_armed") != 0)
                throw std::runtime_error("frozen group member is missing or armed");
        }
        if (actual != expected)
            throw std::runtime_error("frozen group membership changed; unfreeze before editing its tree");
        auto scope = expected;
        scope.insert(root);
        checkGroup(model, root, scope, false);
    }
}
bool groupControlsLive(SQLite::Database &db, std::span<const OpRequest> requests) {
    const auto scopes = groups(db);
    if (scopes.empty())
        return true;
    std::set<std::int64_t> baked;
    for (const auto &[root, members] : scopes) {
        (void)root;
        for (const auto &member : members)
            baked.insert(member.id);
    }
    for (const auto &request : requests) {
        if (request.opType == "track.rename" || request.opType == "track.setColor" ||
            request.opType == "track.setLocked" || request.opType == "project.setName")
            continue;
        if (request.payload.contains("id") && baked.contains(detail::signedInteger(request.payload.at("id"))))
            return false;
    }
    return true;
}
bool groupApply(OpContext &ctx, const Payload &payload, std::string &error) {
    try {
        const auto root = detail::signedInteger(payload.at("id"));
        const auto &before = payload.at("before");
        const auto &after = payload.at("after");
        for (const auto *value : {&before, &after})
            if (!value->is_object() || value->size() != 1 || !value->contains("members"))
                throw std::runtime_error("invalid captured group scope");
        if (groupState(ctx.db, root) != before)
            throw std::runtime_error("captured group scope is stale");
        const bool installing = !after.at("members").is_null();
        if (installing && !before.at("members").is_null())
            throw std::runtime_error("unfreeze before replacing a group scope");
        if (!installing && before.at("members").is_null())
            return true;
        const auto members = membersOf((installing ? after : before).at("members"));
        if (installing) {
            const auto model = rows::readModel(ctx.store);
            auto expected = groupMembers(model, root);
            expected.erase(root);
            std::set<std::int64_t> actual;
            for (const auto &member : members)
                actual.insert(member.id);
            if (actual != expected)
                throw std::runtime_error("captured group member list is incomplete");
        }
        for (const auto &member : members) {
            const auto row = detail::captureRow(ctx.db, "tracks", "id", member.id);
            if (!row || row->at("record_armed") != (installing ? (member.armed ? 1 : 0) : 0))
                throw std::runtime_error("captured group arm state is stale");
            SQLite::Statement arm(ctx.db, "UPDATE tracks SET record_armed=? WHERE id=?");
            arm.bind(1, installing ? 0 : (member.armed ? 1 : 0));
            arm.bind(2, member.id);
            arm.exec();
        }
        SQLite::Statement remove(ctx.db, "DELETE FROM frozen_groups WHERE track_id=?");
        remove.bind(1, root);
        remove.exec();
        if (installing) {
            const auto bytes = encodePayload(after.at("members"));
            SQLite::Statement insert(ctx.db, "INSERT INTO frozen_groups(track_id,members) VALUES(?,?)");
            insert.bind(1, root);
            insert.bind(2, bytes.data(), static_cast<int>(bytes.size()));
            insert.exec();
        }
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool groupInverse(OpContext &, const Payload &input, Payload &output, std::string &) {
    output = {{"id", input.at("id")}, {"before", input.at("after")}, {"after", input.at("before")}};
    return true;
}
void pruneGroupPlan(engine::GraphPlan &plan, const std::set<std::int64_t> &removed,
                    const std::set<std::int64_t> &cachedRoots) {
    std::vector<std::size_t> remap(plan.nodes.size(), std::numeric_limits<std::size_t>::max());
    std::vector<engine::PlannedNode> nodes;
    for (std::size_t i = 0; i < plan.nodes.size(); ++i)
        if (!removed.contains(plan.nodes[i].trackId)) {
            remap[i] = nodes.size();
            nodes.push_back(plan.nodes[i]);
        }
    std::vector<engine::PlannedEdge> edges;
    for (auto edge : plan.edges)
        if (!removed.contains(plan.nodes.at(edge.from).trackId) &&
            !removed.contains(plan.nodes.at(edge.to).trackId) &&
            !cachedRoots.contains(plan.nodes.at(edge.to).trackId)) {
            edge.from = remap.at(edge.from);
            edge.to = remap.at(edge.to);
            edges.push_back(std::move(edge));
        }
    if (plan.output) {
        const auto output = remap.at(*plan.output);
        if (output == std::numeric_limits<std::size_t>::max())
            plan.output.reset();
        else
            plan.output = output;
    }
    plan.nodes = std::move(nodes);
    plan.edges = std::move(edges);
}
} // namespace adi::freeze
