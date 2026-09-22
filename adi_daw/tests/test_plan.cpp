// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the graph builder — src/adi/engine/plan.{hpp,cpp}.
//
// Every one of these builds a `rows::Model` by hand. No database, no audio, no
// nodes: the planner's whole job is topology, and topology is exactly what can
// be checked against a value. That is the third time this seam has paid for
// itself, after `buildTree` and `buildSnapshot`.

#include "adi/engine/plan.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace adi;
using namespace adi::engine;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }

rows::Track track(std::int64_t id, const char* kind, const char* name,
                  std::optional<std::int64_t> parent = std::nullopt) {
    rows::Track t;
    t.id = id;
    t.kind = kind;
    t.name = name;
    t.parentId = parent;
    return t;
}

rows::Routing route(std::int64_t id, std::int64_t from, std::int64_t to,
                    const char* kind = "main", const char* origin = "user") {
    rows::Routing r;
    r.id = id;
    r.srcKind = "track";
    r.srcId = from;
    r.dstKind = "track";
    r.dstId = to;
    r.kind = kind;
    r.origin = origin;
    return r;
}

/// Is there an edge from track `a` to track `b` on `bus`?
bool hasEdge(const GraphPlan& p, std::int64_t a, std::int64_t b,
             Bus bus = Bus::Main) {
    const auto ia = p.indexOf(a), ib = p.indexOf(b);
    if (!ia || !ib) return false;
    for (const auto& e : p.edges)
        if (e.from == *ia && e.to == *ib && e.bus == bus) return true;
    return false;
}

int edgeCount(const GraphPlan& p, std::int64_t from) {
    const auto i = p.indexOf(from);
    if (!i) return 0;
    int n = 0;
    for (const auto& e : p.edges) if (e.from == *i) ++n;
    return n;
}

std::string problems(const GraphPlan& p) {
    std::string s;
    for (const auto& x : p.problems) { s += "\n      - "; s += x; }
    return s.empty() ? std::string(" (none)") : s;
}

// ===========================================================================

void testDefaultRouting() {
    section("ADR-0065 -- absence of a main row means the default");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "audio", "Kick"),
                track(3, "midi", "Keys")};
    // No routing rows at all. This is the common project.
    const GraphPlan p = planGraph(m);

    check(p.problems.empty(), "nothing to report:" + problems(p));
    check(p.nodes.size() == 3, "three nodes");
    check(p.output.has_value() && p.nodes[*p.output].trackId == 1,
          "the master is the output");
    check(hasEdge(p, 2, 1), "Kick routes to the master with no row saying so");
    check(hasEdge(p, 3, 1), "and so does Keys");
    check(edgeCount(p, 1) == 0, "the master routes nowhere");

    const auto k = p.indexOf(2);
    check(k && p.nodes[*k].route == PlannedNode::Route::Default,
          "and the plan says the route came from the rule, not a row");
}

void testGroupsAutoRoute() {
    section("ADR-0044 -- children route into their group, and it into the master");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "group", "Drums"),
                track(3, "audio", "Kick", 2),
                track(4, "audio", "Snare", 2)};
    const GraphPlan p = planGraph(m);

    check(p.problems.empty(), "nothing to report:" + problems(p));
    check(hasEdge(p, 3, 2) && hasEdge(p, 4, 2), "both children feed the group");
    check(hasEdge(p, 2, 1), "and the group feeds the master");
    check(!hasEdge(p, 3, 1), "a child does NOT also reach the master directly -- "
                             "it would be summed twice");

    const auto g = p.indexOf(2);
    check(g && p.nodes[*g].kind == PlannedKind::Group, "the group is planned as one");
}

void testAuxSendsAreRefusedAndSurfaced() {
    section("ADR-0072 -- an aux send is refused, and SAID so rather than dropped");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "audio", "Reverb Bus"),
                track(3, "audio", "Snare")};
    // The snare's main output, plus a send into the reverb bus. The main edge
    // must survive; only the send is refused.
    m.routing = {route(10, 3, 1, "main", "user"),
                 route(11, 3, 2, "send", "user")};

    const GraphPlan p = planGraph(m);

    // The NEGATIVE half, and it is the half that matters. Before ADR-0072 a
    // send fell through to a Bus::Main edge -- a parallel path making the
    // top-level graph an arbitrary DAG, which is what the ruling abolishes. A
    // refactor that re-adds the fall-through has to fail here.
    check(!hasEdge(p, 3, 2), "no edge is planned for the send");
    check(hasEdge(p, 3, 1), "and the track's own main output is untouched");

    // Refused is not the same as dropped. Quietly rewiring a signal path is
    // the failure ADR-0011 exists to prevent, and a routing row is not exempt.
    bool named = false;
    for (const auto& q : p.problems)
        if (q.find("routing#11") != std::string::npos &&
            q.find("ADR-0072") != std::string::npos) named = true;
    check(named, "the refusal names the row and the ADR:" + problems(p));

    bool suggests = false;
    for (const auto& q : p.problems)
        if (q.find("rack") != std::string::npos || q.find("group") != std::string::npos)
            suggests = true;
    check(suggests, "and says what to use instead");

    // A sidechain is NOT a send and must keep working: ADR-0043 requires a
    // live sidechain to prevent suspension, and ADR-0056 added Bus::Sidechain
    // to express it. An over-broad refusal would take it out with the sends.
    rows::Model m2;
    m2.tracks = {track(1, "master", "Master"),
                 track(2, "audio", "Bass"),
                 track(3, "audio", "Kick")};
    m2.routing = {route(20, 3, 2, "sidechain", "user")};
    const GraphPlan p2 = planGraph(m2);
    check(hasEdge(p2, 3, 2, Bus::Sidechain), "a sidechain edge is still planned");
    bool sidechainRefused = false;
    for (const auto& q : p2.problems)
        if (q.find("routing#20") != std::string::npos) sidechainRefused = true;
    check(!sidechainRefused, "and is not caught by the send refusal:" + problems(p2));
}

void testUserRoutingWins() {
    section("ADR-0065 -- a user row overrides the default and is not doubled");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "group", "Bus"),
                track(3, "audio", "Odd", 2)};
    // Inside the group, but routed to the master by hand.
    m.routing = {route(10, 3, 1, "main", "user")};

    const GraphPlan p = planGraph(m);
    check(p.problems.empty(), "nothing to report:" + problems(p));
    check(hasEdge(p, 3, 1), "the hand-made route is planned");
    check(!hasEdge(p, 3, 2),
          "and the default to its parent is NOT also applied -- two main edges "
          "from one track would sum it into two places");
    check(edgeCount(p, 3) == 1, "exactly one output");

    const auto t = p.indexOf(3);
    check(t && p.nodes[*t].route == PlannedNode::Route::User,
          "the plan records that a person chose this");
}

void testAutoRowIsTheDefaultMaterialised() {
    section("ADR-0065 -- an auto row and the default agree");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "group", "Bus"),
                track(3, "audio", "Kick", 2)};
    m.routing = {route(10, 3, 2, "main", "auto")};

    const GraphPlan p = planGraph(m);
    check(hasEdge(p, 3, 2), "the auto row plans the same edge the rule would");
    check(edgeCount(p, 3) == 1, "and only once");

    const auto t = p.indexOf(3);
    check(t && p.nodes[*t].route == PlannedNode::Route::Auto,
          "distinguishable from a user choice, which is why origin exists");
}

void testSidechainAndSends() {
    section("routing kinds map to buses");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "audio", "Kick"),
                track(3, "audio", "Bass"),
                track(4, "return", "Verb")};
    m.routing = {route(10, 2, 3, "sidechain", "user"),
                 route(11, 3, 4, "send", "user")};

    const GraphPlan p = planGraph(m);
    check(hasEdge(p, 2, 3, Bus::Sidechain),
          "a sidechain row becomes a sidechain edge (ADR-0056)");

    // CHANGED BY ADR-0072. This asserted "a send sums into its destination",
    // which was correct under ADR-0067 and is the behaviour the ruling
    // abolishes: a send makes the top-level graph an arbitrary DAG, and the
    // compensation can be got wrong on every path through it.
    check(!hasEdge(p, 3, 4, Bus::Main), "a send no longer sums into its destination");

    // Neither is a main output, so both tracks still take the default. This
    // half is unchanged and is the reason the refusal has to be narrow: it
    // must take out the send and nothing else.
    check(hasEdge(p, 2, 1), "the kick still reaches the master");
    check(hasEdge(p, 3, 1), "and so does the bass");
}

void testVcaIsNotAudio() {
    section("a VCA is a control relationship, not a summing input");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "vca", "Drums VCA"),
                track(3, "audio", "Kick")};
    m.routing = {route(10, 3, 2, "vca", "user")};

    const GraphPlan p = planGraph(m);
    check(!hasEdge(p, 3, 2), "no audio edge to the VCA -- a fader is not a bus");
    check(edgeCount(p, 2) == 0, "and the VCA itself feeds nothing");
    check(hasEdge(p, 3, 1), "the kick still routes to the master normally");
}

void testHybridChangesNothing() {
    section("ADR-0045 -- what a track contains does not change its topology");

    rows::Model base;
    base.tracks = {track(1, "master", "Master"), track(2, "audio", "Both")};
    const GraphPlan empty = planGraph(base);

    rows::Model hybrid = base;
    rows::Clip a;
    a.id = 1; a.trackId = 2; a.kind = "audio"; a.posTicks = 0; a.lengthTicks = 100;
    rows::Clip b;
    b.id = 2; b.trackId = 2; b.kind = "midi"; b.posTicks = 0; b.lengthTicks = 100;
    hybrid.clips = {a, b};
    const GraphPlan both = planGraph(hybrid);

    check(empty.edges.size() == both.edges.size(),
          "a track holding audio AND midi plans exactly as one holding neither");
    check(hasEdge(both, 2, 1), "and still routes to the master");

    // And the kind hint changes nothing either.
    rows::Model asMidi = base;
    asMidi.tracks[1].kind = "midi";
    check(planGraph(asMidi).edges.size() == empty.edges.size(),
          "tracks.kind is a hint and the planner reads nothing into it");
}

void testDanglingAndMissing() {
    section("what cannot be planned is named, not dropped");

    {
        rows::Model m;
        m.tracks = {track(1, "master", "Master"), track(2, "audio", "Kick")};
        m.routing = {route(10, 2, 404, "main", "user")};
        const GraphPlan p = planGraph(m);
        check(p.problems.size() == 1, "one problem reported:" + problems(p));
        check(!p.problems.empty() &&
                  p.problems[0].find("not there") != std::string::npos,
              "naming the dangling route");
        // It did not decide the main output, so the default still applies:
        // losing the connection AND the default would be two failures.
        check(hasEdge(p, 2, 1), "and the track still reaches the master");
    }
    {
        rows::Model m;
        m.tracks = {track(1, "audio", "Lonely")};
        const GraphPlan p = planGraph(m);
        check(!p.output.has_value(), "no master means no output");
        check(!p.problems.empty(), "and it says so:" + problems(p));
    }
    {
        rows::Model m;
        m.tracks = {track(1, "master", "A"), track(2, "master", "B")};
        const GraphPlan p = planGraph(m);
        check(p.output.has_value() && p.nodes[*p.output].trackId == 1,
              "two masters: the lowest id wins, deterministically");
        check(!p.problems.empty(), "and the duplicate is reported");
    }
}

void testCycleIsReportedBeforeAnythingIsBuilt() {
    section("a routing cycle is caught in the plan, not in prepare");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "group", "A"),
                track(3, "group", "B")};
    m.routing = {route(10, 2, 3, "main", "user"),
                 route(11, 3, 2, "main", "user")};

    const GraphPlan p = planGraph(m);
    bool found = false;
    for (const auto& s : p.problems)
        if (s.find("cycle") != std::string::npos) found = true;
    check(found, "the cycle is reported:" + problems(p));

    // Total: it still returns a plan rather than throwing or hanging.
    check(p.nodes.size() == 3, "and a plan still comes back");
}

void testDeterministicOrder() {
    section("the same project plans identically, whatever order the rows arrive");

    rows::Model a;
    a.tracks = {track(1, "master", "M"), track(2, "audio", "X"),
                track(3, "audio", "Y")};
    a.routing = {route(10, 2, 1, "main", "user"), route(11, 3, 1, "main", "user")};

    rows::Model b;
    b.tracks = {track(3, "audio", "Y"), track(1, "master", "M"),
                track(2, "audio", "X")};
    b.routing = {route(11, 3, 1, "main", "user"), route(10, 2, 1, "main", "user")};

    const GraphPlan pa = planGraph(a), pb = planGraph(b);

    bool same = pa.nodes.size() == pb.nodes.size() &&
                pa.edges.size() == pb.edges.size() &&
                pa.output == pb.output;
    for (std::size_t i = 0; same && i < pa.nodes.size(); ++i)
        if (pa.nodes[i].trackId != pb.nodes[i].trackId) same = false;
    for (std::size_t i = 0; same && i < pa.edges.size(); ++i)
        if (pa.edges[i].from != pb.edges[i].from || pa.edges[i].to != pb.edges[i].to)
            same = false;

    check(same,
          "row order is SQLite's business and must not reach the graph -- "
          "ADR-0021's oracle compares bytes, and an edge order that varied "
          "would vary a floating-point sum");
}

void testDeepNesting() {
    section("a group inside a group routes all the way up");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "group", "Band"),
                track(3, "group", "Drums", 2),
                track(4, "audio", "Kick", 3)};
    const GraphPlan p = planGraph(m);

    check(p.problems.empty(), "nothing to report:" + problems(p));
    check(hasEdge(p, 4, 3), "kick into drums");
    check(hasEdge(p, 3, 2), "drums into band");
    check(hasEdge(p, 2, 1), "band into master");
    check(edgeCount(p, 4) == 1, "and the kick reaches the master by exactly one path");
}

}  // namespace

int main() {
    // Unbuffered: a crash must not take its own diagnosis with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_plan_tests -- the graph builder\n\n");
    try {
        testDefaultRouting();
        testGroupsAutoRoute();
        testUserRoutingWins();
        testAuxSendsAreRefusedAndSurfaced();
        testAutoRowIsTheDefaultMaterialised();
        testSidechainAndSends();
        testVcaIsNotAudio();
        testHybridChangesNothing();
        testDanglingAndMissing();
        testCycleIsReportedBeforeAnythingIsBuilt();
        testDeterministicOrder();
        testDeepNesting();
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
