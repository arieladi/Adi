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

/// The send edge a routing row became, or null.
const PlannedEdge* sendEdge(const GraphPlan& p, std::int64_t routingId) {
    for (const auto& e : p.edges)
        if (e.routingId == routingId) return &e;
    return nullptr;
}

bool mentions(const GraphPlan& p, const std::string& a, const std::string& b = "") {
    for (const auto& q : p.problems)
        if (q.find(a) != std::string::npos && q.find(b) != std::string::npos) return true;
    return false;
}

void testASendIsPlanned() {
    section("ADR-0203 d3 -- a send is planned, as Live: it reverses ADR-0072's refusal");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "return", "Reverb"),
                track(3, "audio", "Snare")};
    // The snare's main output, plus a send into the reverb return.
    m.routing = {route(10, 3, 1, "main", "user"),
                 route(11, 3, 2, "send", "user")};

    const GraphPlan p = planGraph(m);
    const PlannedEdge* send = sendEdge(p, 11);
    check(send != nullptr && hasEdge(p, 3, 2), "the send is an edge from the snare into the return");
    check(send != nullptr && send->kind == "send" && send->bus == Bus::Main && send->tap == Tap::PostFader,
          "a send, on the main bus, leaving after the fader (Post)");
    check(hasEdge(p, 3, 1), "and the track's own main output is untouched");
    check(!mentions(p, "routing#11"), "and no problem names the row:" + problems(p));
    check(hasEdge(p, 2, 1), "the return goes to the master by the default");
    check(!p.cycle, "and nothing loops");
    // The main edge comes first: a send never decides a main output, and
    // the summation order into a junction stays the main edges' (ADR-0056).
    check(!p.edges.empty() && p.edges.back().routingId == 11, "the send is planned after every main edge");

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
    check(!sidechainRefused, "and is not mistaken for a send:" + problems(p2));
}

void testASendThatCannotPlayIsRefusedByRow() {
    section("ADR-0203 d3 -- a send that cannot play is refused as its row, named, and the rest plans");

    rows::Model m;
    m.tracks = {track(1, "master", "Master"),
                track(2, "audio", "X"),
                track(3, "audio", "Y", 5),
                track(4, "return", "R"),
                track(5, "group", "G"),
                track(6, "vca", "V")};
    rows::Routing pre = route(23, 2, 4, "send", "user");
    pre.preFader = true;
    rows::Routing off = route(24, 2, 4, "send", "user");
    off.enabled = false;
    m.routing = {route(20, 2, 3, "send", "user"),   // X -> Y: plays
                 route(21, 3, 2, "send", "user"),   // Y -> X: would close the loop
                 route(22, 5, 3, "send", "user"),   // a group into its own child: a loop
                 pre,                               // a Pre send: before the fader
                 off,                               // switched off: planned all the same
                 route(25, 2, 1, "send", "user"),   // into the master
                 route(26, 4, 4, "send", "user"),   // a return into itself
                 route(27, 4, 2, "send", "user"),   // a return's own send, back into X, which feeds it
                 route(28, 2, 6, "send", "user"),   // into a VCA
                 route(29, 2, 99, "send", "user"),  // a track that is not there
                 route(30, 2, 2, "send", "user"),   // an audio track into itself: a loop
                 route(0, 2, 4, "send", "user")};   // id 0, which planned edges keep for "not a send"
    const GraphPlan p = planGraph(m);
    check(sendEdge(p, 20) != nullptr, "X -> Y plays");
    check(sendEdge(p, 21) == nullptr && mentions(p, "routing#21", "would close a loop"),
          "Y -> X would close the loop with it: the later row is refused, named" + problems(p));
    check(sendEdge(p, 22) == nullptr && mentions(p, "routing#22", "would close a loop"),
          "a group into its own child is a loop too");
    check(!p.cycle, "and the plan has no cycle: the loops were refused, not planned");
    check(sendEdge(p, 23) != nullptr && sendEdge(p, 23)->tap == Tap::PostFx && !mentions(p, "routing#23"),
          "a Pre send is planned, leaving after the effects and before the fader");
    check(sendEdge(p, 24) != nullptr, "a send switched off is planned, so Enable Send never retimes");
    check(sendEdge(p, 25) == nullptr && mentions(p, "routing#25", "master"), "a send into the master is refused");
    const PlannedEdge* loop = sendEdge(p, 26);
    check(loop != nullptr && loop->feedback && loop->from == loop->to && loop->tap == Tap::PostFader &&
              !mentions(p, "routing#26"),
          "a return into itself, Live's feedback loop, is planned: a feedback edge, after the fader" + problems(p));
    bool otherFeedback = false;
    for (const PlannedEdge& e : p.edges) otherFeedback = otherFeedback || (e.feedback && e.routingId != 26);
    check(!otherFeedback, "and it is the only feedback edge: every other edge is the topology's");
    check(sendEdge(p, 27) == nullptr && mentions(p, "routing#27", "would close a loop"),
          "a return's own send back into a track that feeds it is a loop, refused");
    check(sendEdge(p, 28) == nullptr && mentions(p, "routing#28", "VCA"), "a VCA takes no send");
    check(sendEdge(p, 29) == nullptr && mentions(p, "routing#29", "not there"), "nor does a track that is not there");
    check(sendEdge(p, 30) == nullptr && mentions(p, "routing#30", "would close a loop") &&
              !mentions(p, "routing#30", "feedback loop"),
          "an audio track into itself is a loop refused, not Live's return loop to come");
    // X -> R with routing id 0 would be an edge the rest of the engine takes
    // for a plain route: no send node, no level, no switch.
    const auto ix = p.indexOf(2), ir = p.indexOf(4);
    bool idZeroPlanned = false;
    for (const PlannedEdge& e : p.edges)
        idZeroPlanned = idZeroPlanned || (ix && ir && e.from == *ix && e.to == *ir && e.routingId == 0);
    check(!idZeroPlanned && mentions(p, "routing#0", "other than 0"),
          "a send row with id 0 is refused, named: it would play as a plain route" + problems(p));
    check(hasEdge(p, 2, 1) && hasEdge(p, 3, 5) && hasEdge(p, 5, 1) && hasEdge(p, 4, 1),
          "and every track's own output is planned as before");

    // A return's own sends (ADR-0203 d3): into another return, and into a
    // track that does not feed it, they play; two returns feeding each other
    // wait on Live's being measured (AWAITING 7(f)), the later row losing.
    {
        rows::Model r;
        r.tracks = {track(1, "master", "Master"), track(2, "audio", "X"), track(4, "return", "R1"),
                    track(5, "return", "R2"), track(6, "audio", "Y")};
        rows::Routing off = route(33, 4, 6, "send", "user");
        off.enabled = false;   // Enable Send off: planned all the same
        r.routing = {route(30, 2, 4, "send", "user"),    // X -> R1
                     route(31, 4, 5, "send", "user"),    // R1 -> R2: plays
                     route(32, 5, 4, "send", "user"),    // R2 -> R1: a ring between returns
                     off};                               // R1 -> Y, switched off
        const GraphPlan pr = planGraph(r);
        const PlannedEdge* r12 = sendEdge(pr, 31);
        check(r12 != nullptr && r12->tap == Tap::PostFader && !mentions(pr, "routing#31"),
              "a return's own send into another return plays, after its fader" + problems(pr));
        check(sendEdge(pr, 32) == nullptr && mentions(pr, "routing#32", "ring between returns") && !pr.cycle,
              "two returns feeding each other: the later row is refused, named, and nothing loops");
        check(sendEdge(pr, 33) != nullptr, "a return's own send switched off is planned, so Enable Send never retimes");
        check(hasEdge(pr, 4, 1) && hasEdge(pr, 5, 1), "and both returns still go to the master");
    }

    // A return's loop Pre, and a loop beside a return's other sends: the loop
    // is outside the topology, so it closes no cycle and refuses no row.
    {
        rows::Model r;
        r.tracks = {track(1, "master", "Master"), track(2, "audio", "X"), track(4, "return", "R1"),
                    track(5, "return", "R2")};
        rows::Routing pre = route(41, 4, 4, "send", "user");
        pre.preFader = true;
        r.routing = {route(40, 2, 4, "send", "user"),   // X -> R1
                     pre,                               // R1 into itself, Pre
                     route(42, 4, 5, "send", "user"),   // R1 -> R2
                     route(43, 5, 5, "send", "user")};  // R2 into itself
        const GraphPlan pr = planGraph(r);
        const PlannedEdge* l1 = sendEdge(pr, 41);
        const PlannedEdge* l2 = sendEdge(pr, 43);
        check(l1 != nullptr && l1->feedback && l1->tap == Tap::PostFx && l2 != nullptr && l2->feedback &&
                  sendEdge(pr, 42) != nullptr && !pr.cycle && pr.problems.empty(),
              "a Pre loop leaves after the effects; two returns each with a loop, one feeding the other, "
              "is no cycle and refuses nothing" + problems(pr));
    }

    // A loop through a sidechain is a loop: the return keys a device on X,
    // so a send from X into it would close R -> X -> R, and the whole
    // project, not just the row, would be refused if the check missed it.
    rows::Model k;
    k.tracks = {track(1, "master", "Master"), track(2, "audio", "X"), track(4, "return", "R")};
    k.routing = {route(10, 4, 2, "sidechain", "user"),   // R keys X
                 route(11, 2, 4, "send", "user")};       // X -> R
    const GraphPlan pk = planGraph(k);
    check(sendEdge(pk, 11) == nullptr && mentions(pk, "routing#11", "would close a loop") && !pk.cycle,
          "a send closing a loop through a sidechain is refused as its row, and the plan has no cycle" +
              problems(pk));
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

    // CHANGED BACK BY ADR-0203 d3. ADR-0072 had abolished this; the
    // director's ruling restores it, compensated like every other path.
    check(hasEdge(p, 3, 4, Bus::Main), "a send sums into its destination, on the main bus");

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
    a.tracks.push_back(track(4, "return", "R"));
    a.routing = {route(10, 2, 1, "main", "user"), route(11, 3, 1, "main", "user"),
                 route(12, 2, 4, "send", "user"), route(13, 3, 4, "send", "user")};

    rows::Model b;
    b.tracks = {track(3, "audio", "Y"), track(1, "master", "M"),
                track(2, "audio", "X")};
    b.tracks.push_back(track(4, "return", "R"));
    b.routing = {route(13, 3, 4, "send", "user"), route(11, 3, 1, "main", "user"),
                 route(12, 2, 4, "send", "user"), route(10, 2, 1, "main", "user")};

    const GraphPlan pa = planGraph(a), pb = planGraph(b);

    bool same = pa.nodes.size() == pb.nodes.size() &&
                pa.edges.size() == pb.edges.size() &&
                pa.output == pb.output;
    for (std::size_t i = 0; same && i < pa.nodes.size(); ++i)
        if (pa.nodes[i].trackId != pb.nodes[i].trackId) same = false;
    for (std::size_t i = 0; same && i < pa.edges.size(); ++i)
        if (pa.edges[i].from != pb.edges[i].from || pa.edges[i].to != pb.edges[i].to ||
            pa.edges[i].routingId != pb.edges[i].routingId)
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
        testASendIsPlanned();
        testASendThatCannotPlayIsRefusedByRow();
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
