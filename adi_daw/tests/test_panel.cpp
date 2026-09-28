// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plug-in panel (ADR-0150, ADR-0154): Live's 64-parameter rule, a
// configured panel exactly as saved, and the Parameter List's search.

#include "adi/panel.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace adi;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }
void eqi(std::int64_t got, std::int64_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %lld, want %lld\n", what.c_str(),
                    static_cast<long long>(got), static_cast<long long>(want));
    }
}

std::vector<panel::Declared> plugin(std::size_t modifiable, std::size_t hidden = 0) {
    std::vector<panel::Declared> v;
    for (std::size_t i = 0; i < modifiable; ++i)
        v.push_back({"p" + std::to_string(i), "Param " + std::to_string(i), true});
    for (std::size_t i = 0; i < hidden; ++i)
        v.push_back({"h" + std::to_string(i), "Meter " + std::to_string(i), false});
    return v;
}

void testLivesRule() {
    section("ADR-0150 d1 -- 64 or fewer modifiable parameters all shown; more opens empty with the hint");
    const rows::Model none;
    const auto at64 = panel::resolve(none, 1, plugin(64, 10));
    check(!at64.configured && at64.entries.size() == 64 && !at64.showsConfigureHint,
          "64 modifiable: all 64 shown, no hint (hidden meters do not count)");
    check(at64.entries.front().id == "p0" && at64.entries.back().id == "p63", "in the plug-in's order");
    const auto at65 = panel::resolve(none, 1, plugin(65));
    check(at65.entries.empty() && at65.showsConfigureHint, "65: an empty panel with the Configure hint (Serum 2, Pro-Q 3)");
    check(at65.modifiableCount == 65, "and it knows how many it could show");
    const auto zero = panel::resolve(none, 1, {});
    check(zero.entries.empty() && !zero.showsConfigureHint,
          "a placeholder declares nothing: empty, and no hint, since there is nothing to configure from");
}

void testAConfiguredPanel() {
    section("ADR-0154 -- a configured panel shows exactly what was saved, in order, even when empty");
    rows::Model m;
    m.devicePanels.push_back({7, {"p3", "p1", "gone"}});
    m.devicePanels.push_back({8, {}});
    const auto p = panel::resolve(m, 7, plugin(200));
    check(p.configured && p.entries.size() == 3, "three entries, though the plug-in has 200");
    check(p.entries[0].id == "p3" && p.entries[1].id == "p1", "in the saved order");
    check(!p.entries[0].missing && p.entries[2].missing, "a parameter the plug-in no longer declares is kept, marked missing");
    const auto e = panel::resolve(m, 8, plugin(10));
    check(e.configured && e.entries.empty() && !e.showsConfigureHint,
          "a panel the user emptied stays empty: configured, not the default");
    const auto other = panel::resolve(m, 9, plugin(10));
    check(!other.configured && other.entries.size() == 10, "another device is on the default");
    const auto ghost = panel::resolve(m, 7, {});
    check(ghost.entries.size() == 3 && ghost.entries[0].missing && ghost.entries[1].missing,
          "a missing plug-in keeps its configured panel, every entry marked missing");
}

void testTheParameterList() {
    section("ADR-0150 d3 -- the Parameter List: every word must match; word starts and exact names first");
    const std::vector<panel::Declared> d = {
        {"1", "Osc A Level", true},
        {"2", "Filter 1 Cutoff", true},
        {"3", "Filter 1 Res", true},
        {"4", "Env 2 Filter Amount", true},
        {"5", "Cutoff", true},
        {"6", "LFO Rate", true},
        {"7", "Filter 2 Cutoff", true},
    };
    const auto all = panel::search(d, "");
    check(all.size() == d.size() && all.front().index == 0 && all.back().index == 6,
          "an empty query lists everything, in the plug-in's order");
    const auto fc = panel::search(d, "filter cutoff");
    check(fc.size() == 2 && fc[0].index == 1 && fc[1].index == 6,
          "\"filter cutoff\": both filters' cutoffs, plug-in order among equals");
    const auto cut = panel::search(d, "Cutoff");
    check(!cut.empty() && cut[0].index == 4, "the exact name ranks first, whatever the case");
    check(cut.size() == 3, "and every name containing it follows");
    const auto res = panel::search(d, "RES");
    check(res.size() == 1 && res[0].index == 2, "case does not matter");
    check(panel::search(d, "filter 3").empty(), "every word must match: no \"Filter 3\"");
    const auto amt = panel::search(d, "amo");
    check(amt.size() == 1 && amt[0].index == 3, "a word prefix inside the name finds it");
    const auto limited = panel::search(d, "filter", 2);
    check(limited.size() == 2, "the limit holds");
    const auto byId = panel::search(d, "6");
    check(byId.size() == 1 && byId[0].index == 5, "the whole query as an id finds it");
    check(panel::search(d, "filter 3").empty() && panel::search(d, "res 3").empty(),
          "but a word never matches an id: VST3 ids are bare numbers");
    const auto inside = panel::search({{"a", "Resonance", true}, {"b", "Res", true}}, "es");
    check(inside.size() == 2, "a match inside a word still counts");
    const auto starts = panel::search({{"a", "Presence", true}, {"b", "Res Boost", true}}, "res");
    check(starts.size() == 2 && starts[0].index == 1, "but a word start ranks above it");
}

}  // namespace

// ---------------------------------------------------------------------------
// ADR-0198: the ADI Airwindows active-algorithm filter.
// ---------------------------------------------------------------------------

namespace {

/// A record with a CLAP-style hex id, as the device contract hands them over.
adi::panel::Record rec(std::int64_t id, const char* name) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(id));
    adi::panel::Record r;
    r.id = buf;
    r.name = name;
    return r;
}

void testAirwindowsActiveAlgorithmFilter() {
    section("ADR-0198 -- a suite shows only the ACTIVE algorithm's parameters");

    using namespace adi::panel;

    // A suite the shape ADI Airwindows declares: two shared parameters, then
    // each algorithm's block at 100 + 64a + k.
    std::vector<Record> all{
        rec(kAwAlgorithmId, "Algorithm"),
        rec(kAwAutoGainId,  "Auto Gain"),
        rec(100 + 0 * 64 + 0, "A0 p0"),
        rec(100 + 0 * 64 + 1, "A0 p1"),
        rec(100 + 1 * 64 + 0, "A1 p0"),
        rec(100 + 1 * 64 + 1, "A1 p1"),
        rec(100 + 2 * 64 + 0, "A2 p0"),
    };

    const auto a0 = activeAlgorithmParams(all, 0);
    eqi(static_cast<std::int64_t>(a0.size()), 4,
        "algorithm 0: the two shared plus its own two");
    check(a0.size() == 4 && all[a0[0]].name == "Algorithm" &&
              all[a0[1]].name == "Auto Gain" && all[a0[2]].name == "A0 p0" &&
              all[a0[3]].name == "A0 p1",
          "in the plug-in's own order, shared first");

    const auto a1 = activeAlgorithmParams(all, 1);
    eqi(static_cast<std::int64_t>(a1.size()), 4, "algorithm 1: four again");
    check(a1.size() == 4 && all[a1[2]].name == "A1 p0" && all[a1[3]].name == "A1 p1",
          "and they are ALGORITHM 1's, not algorithm 0's -- the whole point, "
          "since every algorithm's parameters are declared at once");

    const auto a2 = activeAlgorithmParams(all, 2);
    eqi(static_cast<std::int64_t>(a2.size()), 3, "algorithm 2 declares only one");

    // An algorithm with no parameters still shows the shared two.
    const auto a9 = activeAlgorithmParams(all, 9);
    eqi(static_cast<std::int64_t>(a9.size()), 2,
        "an algorithm with no parameters of its own still shows the shared two");

    // THE CONSERVATIVE CASES. This filter exists for one plug-in family, and
    // anything it cannot place is KEPT: hiding a control the user needs is
    // worse than showing one they do not.
    std::vector<Record> odd{
        rec(kAwAlgorithmId, "Algorithm"),
        Record{},                       // id "" -- not a number at all
        rec(50, "below the base"),      // shared-looking, not one of the two
        rec(100 + 0 * 64, "A0 p0"),
    };
    odd[1].id = "not-hex";
    odd[1].name = "Some other plug-in's";
    const auto kept = activeAlgorithmParams(odd, 0);
    eqi(static_cast<std::int64_t>(kept.size()), 4,
        "an unparseable id and one below the base are both KEPT");
}

/// The filter's constants must equal the plug-in's. They are declared in two
/// places on purpose -- `panel.hpp` is pure and JUCE-free so it builds on every
/// ABI, and the plug-in is built apart from the DAW -- so this is the check
/// that stops them drifting.
void testTheFilterMatchesThePluginsOwnConstants() {
    section("ADR-0198 -- the panel's constants are the plug-in's constants");

    // From plugins/airwindows/Source/airwindows_clap.hpp, which is the
    // authority (plugins/README.md says the same; SUITES.md does NOT document
    // the scheme, which cost a wrong turn when a brief said it did).
    static_assert(adi::panel::kAwAlgorithmId  == 0,   "Algorithm is id 0");
    static_assert(adi::panel::kAwAutoGainId   == 1,   "Auto Gain is id 1");
    static_assert(adi::panel::kAwParamBase    == 100, "the first block starts at 100");
    static_assert(adi::panel::kAwParamStride  == 64,  "and each is 64 wide");
    check(true, "the scheme is 100 + 64a + k, asserted at compile time");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_panel_tests -- the plug-in panel (ADR-0150, ADR-0154)\n\n");
    testLivesRule();
    testAConfiguredPanel();
    testTheParameterList();
    testAirwindowsActiveAlgorithmFilter();
    testTheFilterMatchesThePluginsOwnConstants();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
