// SPDX-License-Identifier: GPL-3.0-or-later
//
// The graph builder: a project's routing topology, derived from a `rows::Model`.
//
// PURE, AND DELIBERATELY NOT A `Graph`. `planGraph` returns a description —
// which nodes exist, what feeds what, which bus, and which node is the output —
// and instantiates nothing. Three reasons, in order of how much they matter:
//
//   1. The real nodes DO NOT EXIST YET. A track node needs clip playback and a
//      device chain; neither is built. A planner that returned a live `Graph`
//      would have to invent placeholder nodes, and the shape of the plan would
//      then be a property of the placeholders.
//   2. Topology is where the rules live. ADR-0044's auto-routing, ADR-0065's
//      absence-means-default, ADR-0045's indifference to what a track contains
//      — all of them are decisions about *edges*, and every one can be checked
//      against a hand-built Model with no audio anywhere near it.
//   3. It keeps the seam the rest of the engine already has: `rows::Model` is a
//      value, `buildTree` is pure, `buildSnapshot` is pure, and this is the
//      third thing that turns the same value into a different shape.
//
// Realising a plan into a `Graph` is a later step and a small one: walk the
// nodes, construct, `connect`, `setOutput`. The part worth testing is here.

#pragma once

#include "adi/engine/graph.hpp"
#include "adi/store_rows.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adi::engine {

/// What a planned node will become. The distinction is not cosmetic: a group
/// sums and has no content of its own (ADR-0044), and the master is the one
/// node whose output leaves the graph.
enum class PlannedKind { Track, Group, Return, Vca, Master };

[[nodiscard]] const char* toString(PlannedKind) noexcept;

struct PlannedNode {
    std::int64_t trackId = 0;
    PlannedKind kind = PlannedKind::Track;
    std::string name;

    /// How the destination was arrived at, so a reader of the plan can tell a
    /// materialised default from a decision (ADR-0065).
    enum class Route { None, Default, Auto, User };
    Route route = Route::None;
};

struct PlannedEdge {
    std::size_t from = 0;      ///< index into GraphPlan::nodes
    std::size_t to = 0;
    Bus bus = Bus::Main;
    std::string kind;          ///< the `routing.kind` that produced it, or "default"
};

struct GraphPlan {
    std::vector<PlannedNode> nodes;
    std::vector<PlannedEdge> edges;

    /// The master. Unset when a project has none, which is legal — a project
    /// mid-construction — and means the plan cannot be realised yet.
    std::optional<std::size_t> output;

    /// True when the routing has a cycle. Also named in `problems`, but as a
    /// FLAG rather than only as prose: realisation has to refuse a cyclic plan
    /// before it constructs anything, and deciding that by matching a sentence
    /// makes the refusal depend on the wording of an error message.
    bool cycle = false;

    /// Everything that could not be planned, named rather than dropped: a
    /// routing row pointing at a track that is not there, a cycle, an endpoint
    /// kind this version does not understand. A plan that silently omitted them
    /// would be a project that silently lost a connection.
    std::vector<std::string> problems;

    [[nodiscard]] std::optional<std::size_t> indexOf(std::int64_t trackId) const;
};

/// Derive the topology. Total: no input produces a failure to return, including
/// a cycle, a missing master, or routing that names nothing.
///
/// Deterministic: the same Model always yields the same plan, in the same
/// order. Nodes come out in track-id order rather than in `Model` order,
/// because ADR-0021's oracle compares bytes and a plan that depended on row
/// order would make the graph depend on how SQLite happened to return rows.
[[nodiscard]] GraphPlan planGraph(const rows::Model&);

}  // namespace adi::engine
