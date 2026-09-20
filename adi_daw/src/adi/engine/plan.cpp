// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/plan.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace adi::engine {
namespace {

PlannedKind kindOf(const std::string& trackKind) {
    if (trackKind == "master") return PlannedKind::Master;
    if (trackKind == "return") return PlannedKind::Return;
    if (trackKind == "vca")    return PlannedKind::Vca;
    if (trackKind == "group")  return PlannedKind::Group;
    // ADR-0045: 'audio', 'midi' and 'instrument' are HINTS, and the planner is
    // the place that must not read anything into them. A track is a track; what
    // it contains is a fact about its clips, not about its kind.
    return PlannedKind::Track;
}

/// Is `kind` a routing endpoint that names a track?
bool namesATrack(const std::string& kind) { return kind == "track"; }

}  // namespace

const char* toString(PlannedKind k) noexcept {
    switch (k) {
        case PlannedKind::Track:  return "track";
        case PlannedKind::Group:  return "group";
        case PlannedKind::Return: return "return";
        case PlannedKind::Vca:    return "vca";
        case PlannedKind::Master: return "master";
    }
    return "track";
}

std::optional<std::size_t> GraphPlan::indexOf(std::int64_t trackId) const {
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].trackId == trackId) return i;
    return std::nullopt;
}

GraphPlan planGraph(const rows::Model& m) {
    GraphPlan plan;

    // --- nodes, in track-id order ------------------------------------------
    //
    // Sorted rather than taken in Model order: `readModel` does not ORDER BY on
    // tracks, so the row order is SQLite's business, and a plan that inherited
    // it would make the graph depend on how a query happened to come back.
    std::vector<const rows::Track*> tracks;
    tracks.reserve(m.tracks.size());
    for (const auto& t : m.tracks) tracks.push_back(&t);
    std::sort(tracks.begin(), tracks.end(),
              [](const rows::Track* a, const rows::Track* b) { return a->id < b->id; });

    std::map<std::int64_t, std::size_t> byId;
    for (const rows::Track* t : tracks) {
        PlannedNode n;
        n.trackId = t->id;
        n.kind = kindOf(t->kind);
        n.name = t->name;
        byId[t->id] = plan.nodes.size();
        plan.nodes.push_back(std::move(n));
    }

    // The master is the graph's output. First by id when there are several,
    // and the duplicate is reported: two masters is a corrupt project, and
    // picking one silently would make which one depend on row order.
    std::vector<std::int64_t> masters;
    for (const rows::Track* t : tracks)
        if (kindOf(t->kind) == PlannedKind::Master) masters.push_back(t->id);
    if (masters.size() > 1)
        plan.problems.push_back("more than one master track; using the lowest id");
    if (!masters.empty()) plan.output = byId[masters.front()];
    else plan.problems.push_back("no master track: the plan has no output");

    // --- explicit routing ---------------------------------------------------
    //
    // Sorted for the same reason the nodes are, and because the edge order into
    // a node decides its summation order, which ADR-0056 requires to be fixed.
    std::vector<const rows::Routing*> routes;
    routes.reserve(m.routing.size());
    for (const auto& r : m.routing) routes.push_back(&r);
    std::sort(routes.begin(), routes.end(),
              [](const rows::Routing* a, const rows::Routing* b) { return a->id < b->id; });

    // Which tracks have their main output decided explicitly, and how.
    std::map<std::int64_t, PlannedNode::Route> mainDecided;

    for (const rows::Routing* r : routes) {
        if (!r->enabled) continue;

        if (r->kind == "vca") {
            // A VCA is a control relationship, not audio. It belongs to the
            // mixer, and planning it as an edge would make a fader into a
            // summing input.
            continue;
        }
        if (r->kind == "cue") {
            plan.problems.push_back("routing#" + std::to_string(r->id) +
                                    ": cue sends are not planned yet");
            continue;
        }

        // A hardware port is not a row and never was (SPEC 6.7). It is device
        // I/O, not a node, so it is skipped without being called an error.
        if (r->srcKind == "hw_in" || r->srcKind == "hw_out" ||
            r->dstKind == "hw_in" || r->dstKind == "hw_out")
            continue;

        if (!namesATrack(r->srcKind) || !namesATrack(r->dstKind)) {
            plan.problems.push_back("routing#" + std::to_string(r->id) +
                                    ": endpoint kind '" + r->srcKind + "'/'" +
                                    r->dstKind + "' is not a track");
            continue;
        }

        const auto from = byId.find(r->srcId);
        const auto to = byId.find(r->dstId);
        if (from == byId.end() || to == byId.end()) {
            plan.problems.push_back("routing#" + std::to_string(r->id) +
                                    ": names a track that is not there");
            continue;
        }

        PlannedEdge e;
        e.from = from->second;
        e.to = to->second;
        e.bus = (r->kind == "sidechain") ? Bus::Sidechain : Bus::Main;
        e.kind = r->kind;
        plan.edges.push_back(e);

        if (r->kind == "main") {
            // ADR-0065: a 'user' row is the user's own routing and a 'auto' row
            // is a materialised default. Either way the main output is decided
            // and the default does not also apply -- two main edges from one
            // track would double it into its destination.
            const auto how = (r->origin == "auto") ? PlannedNode::Route::Auto
                                                   : PlannedNode::Route::User;
            const auto seen = mainDecided.find(r->srcId);
            if (seen != mainDecided.end())
                plan.problems.push_back("tracks#" + std::to_string(r->srcId) +
                                        " has more than one main output");
            mainDecided[r->srcId] = how;
            plan.nodes[from->second].route = how;
        }
    }

    // --- the default, for every track that did not decide ------------------
    //
    // ADR-0065: absence of a main row means "route to my parent, or to the
    // master if I have none". The rule, not a row -- so the common project
    // stores no routing at all and this is where its graph comes from.
    for (const rows::Track* t : tracks) {
        if (mainDecided.count(t->id)) continue;
        if (kindOf(t->kind) == PlannedKind::Master) continue;   // the output itself
        if (kindOf(t->kind) == PlannedKind::Vca) continue;      // control, not audio

        std::optional<std::int64_t> dst;
        if (t->parentId && byId.count(*t->parentId)) dst = *t->parentId;
        else if (t->parentId) {
            plan.problems.push_back("tracks#" + std::to_string(t->id) +
                                    ": parent " + std::to_string(*t->parentId) +
                                    " does not exist");
        } else if (!masters.empty()) {
            dst = masters.front();
        }
        if (!dst) continue;   // no parent and no master: silent, and legal

        PlannedEdge e;
        e.from = byId[t->id];
        e.to = byId[*dst];
        e.bus = Bus::Main;
        e.kind = "default";
        plan.edges.push_back(e);
        plan.nodes[byId[t->id]].route = PlannedNode::Route::Default;
    }

    // --- cycles -------------------------------------------------------------
    //
    // Found here rather than left to `Graph::prepare`, which would also refuse
    // it (ADR-0055) but only after a caller had built every node. A plan that
    // cannot be realised should say so before anything is constructed.
    {
        const std::size_t n = plan.nodes.size();
        std::vector<std::size_t> indegree(n, 0);
        for (const auto& e : plan.edges)
            if (e.to < n) ++indegree[e.to];

        std::vector<std::size_t> ready;
        for (std::size_t i = 0; i < n; ++i)
            if (indegree[i] == 0) ready.push_back(i);

        std::size_t seen = 0;
        while (!ready.empty()) {
            const std::size_t id = ready.back();
            ready.pop_back();
            ++seen;
            for (const auto& e : plan.edges)
                if (e.from == id && e.to < n && --indegree[e.to] == 0)
                    ready.push_back(e.to);
        }
        if (seen != n)
            plan.problems.push_back(
                "the routing has a cycle; " + std::to_string(seen) + " of " +
                std::to_string(n) + " nodes are reachable in order");
    }

    return plan;
}

}  // namespace adi::engine
