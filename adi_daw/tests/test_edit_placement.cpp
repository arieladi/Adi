// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/edit_placement.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace adi::edit_plan;
int checks = 0;
int failures = 0;
void check(bool condition, const char* what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

const PlacementLane mainLane{0, std::nullopt};
const PlacementLane otherTrack{-4, std::nullopt};
const PlacementLane takeLane{0, -7};

PlacementInput inputFor(PlacementSpan span = {-100, 200}) {
    PlacementInput input;
    input.scopeLanes = {mainLane, otherTrack, takeLane};
    input.scopeSpan = span;
    return input;
}

const OriginalSurvivors& source(const IntervalPlan& plan, std::int64_t id) {
    const auto found = std::find_if(plan.originals.begin(), plan.originals.end(),
                                   [id](const auto& item) { return item.id == id; });
    if (found == plan.originals.end()) {
        throw std::runtime_error("missing original in interval plan");
    }
    return *found;
}

void expectError(const PlacementInput& input, PlacementError error, const char* what) {
    const auto plan = resolvePlacement(input);
    check(!plan.ok && plan.error == error, what);
    check(plan.originals.empty() && plan.removed.empty() && plan.incoming.empty()
          && plan.cleared.empty(), "refusal has no partial placement effects");
    check(std::string(toString(plan.error)).size() > 3, "refusal has a named diagnostic");
}

void intervalCases() {
    const std::array<PlacementSpan, 7> masks{{{20, 30}, {-10, 20}, {80, 110},
                                            {-10, 110}, {-10, 0}, {100, 110}, {0, 100}}};
    const std::array<std::vector<PlacementSpan>, 7> expected{{
        {{0, 20}, {30, 100}}, {{20, 100}}, {{0, 80}}, {}, {{0, 100}}, {{0, 100}}, {}}};
    for (std::size_t index = 0; index < masks.size(); ++index) {
        auto input = inputFor();
        input.originals = {{-1, mainLane, {0, 100}}};
        input.incoming = {{0, mainLane, masks[index]}};
        const auto plan = resolvePlacement(input);
        check(plan.ok, "middle, start, end, whole and touching placements are admitted");
        check(source(plan, -1).survivors == expected[index], "exact expected survivor intervals");
        check(plan.removed == (expected[index].empty() ? std::vector<std::int64_t>{-1}
                                                       : std::vector<std::int64_t>{}),
              "only a fully removed original is listed as removed");
        check(plan.incoming == input.incoming, "incoming identity and placement are retained");
    }

    auto input = inputFor({20, 30});
    input.scopeLanes = {mainLane};
    input.originals = {{0, mainLane, {0, 100}}, {-2, otherTrack, {0, 100}},
                       {-3, takeLane, {0, 100}}, {8, mainLane, {100, 110}}};
    input.incoming = {{-1, mainLane, {20, 30}}};
    const auto partial = resolvePlacement(input);
    check(partial.ok && source(partial, 0).survivors == std::vector<PlacementSpan>{{0, 20}, {30, 100}},
          "victim crossing scope bounds retains both outer portions");
    check(source(partial, -2).survivors == std::vector<PlacementSpan>{{0, 100}}
          && source(partial, -3).survivors == std::vector<PlacementSpan>{{0, 100}}
          && source(partial, 8).survivors == std::vector<PlacementSpan>{{100, 110}},
          "disjoint track, take lane and time are unchanged");
    check(partial.originals.front().id == -3 && partial.originals.back().id == 8,
          "signed identities including zero have deterministic ordering");
    input.clock = PlacementClock::Nanoseconds;
    const auto nanos = resolvePlacement(input);
    check(nanos.ok && nanos.clock == PlacementClock::Nanoseconds
          && nanos.originals == partial.originals, "nanosecond operation clock uses the same exact algebra");
}

void unionAndIdentity() {
    auto input = inputFor();
    input.originals = {{3, mainLane, {0, 100}}};
    input.incoming = {{10, mainLane, {10, 20}}, {11, mainLane, {80, 90}}};
    input.clear = {{mainLane, {35, 40}}, {mainLane, {38, 45}}, {mainLane, {45, 50}},
                   {mainLane, {60, 65}}, {mainLane, {35, 40}}};
    const auto plan = resolvePlacement(input);
    check(plan.ok && source(plan, 3).survivors == std::vector<PlacementSpan>{
              {0, 10}, {20, 35}, {50, 60}, {65, 80}, {90, 100}},
          "more than two masks produce staged final fragments without filling gaps");
    check(plan.cleared == std::vector<ClearInterval>{{mainLane, {10, 20}},
              {mainLane, {35, 50}}, {mainLane, {60, 65}}, {mainLane, {80, 90}}},
          "overlapping, touching and repeated masks form a canonical lane union");
    check(source(plan, 3).id == 3 && plan.incoming.front().id == 10,
          "kernel returns original lineage without inventing fragment IDs");

    input.clear = {{mainLane, {10, 90}}};
    const auto rangePaste = resolvePlacement(input);
    check(rangePaste.ok && source(rangePaste, 3).survivors
          == std::vector<PlacementSpan>{{0, 10}, {90, 100}},
          "explicit range mask clears clipboard silence as well as its members");

    input = inputFor();
    input.originals = {{1, mainLane, {0, 10}}, {2, mainLane, {20, 30}},
                       {3, mainLane, {40, 50}}};
    input.incoming = {{1, mainLane, {20, 30}}, {2, mainLane, {0, 10}}};
    input.relocated = {2, 1};
    const auto swap = resolvePlacement(input);
    check(swap.ok && swap.removed.empty() && source(swap, 1).relocated
          && source(swap, 1).survivors.empty() && source(swap, 2).relocated,
          "swap excludes every moved original before clearing any destination");
    std::reverse(input.originals.begin(), input.originals.end());
    std::reverse(input.incoming.begin(), input.incoming.end());
    std::reverse(input.relocated.begin(), input.relocated.end());
    check(resolvePlacement(input) == swap, "swaps are independent of all input orders");

    input = inputFor();
    input.originals = {{-4, mainLane, {10, 30}}};
    input.incoming = {{-5, mainLane, {10, 30}}};
    const auto copy = resolvePlacement(input);
    check(copy.ok && copy.removed == std::vector<std::int64_t>{-4},
          "copy destination can clear its own original because copies are not excluded");
}

void emptyIntervals() {
    auto input = inputFor();
    input.originals = {{1, mainLane, {10, 10}}, {2, mainLane, {20, 20}},
                       {3, mainLane, {30, 30}}, {4, mainLane, {0, 40}}};
    input.incoming = {{5, mainLane, {10, 30}}, {6, mainLane, {20, 20}}};
    const auto plan = resolvePlacement(input);
    check(plan.ok && plan.removed == std::vector<std::int64_t>{2},
          "zero-length original is removed only strictly inside a mask");
    check(source(plan, 1).survivors == std::vector<PlacementSpan>{{10, 10}}
          && source(plan, 3).survivors == std::vector<PlacementSpan>{{30, 30}},
          "zero-length originals at both mask boundaries survive");
    check(source(plan, 4).survivors == std::vector<PlacementSpan>{{0, 10}, {30, 40}},
          "empty intervals do not create phantom positive fragments");
    input.incoming = {{5, mainLane, {20, 20}}};
    const auto emptyDestination = resolvePlacement(input);
    check(emptyDestination.ok && emptyDestination.cleared.empty()
          && emptyDestination.removed.empty()
          && source(emptyDestination, 4).survivors == std::vector<PlacementSpan>{{0, 40}},
          "zero-length destination clears no existing material");
    input.incoming = {{2, mainLane, {35, 35}}};
    input.relocated = {2};
    const auto emptyMove = resolvePlacement(input);
    check(emptyMove.ok && source(emptyMove, 2).relocated && emptyMove.removed.empty(),
          "zero-length clips can relocate while retaining their identity");
    input.relocated.clear();
    input.incoming = {{5, mainLane, {10, 20}}, {6, mainLane, {20, 30}}};
    const auto touchingMasks = resolvePlacement(input);
    check(touchingMasks.ok && touchingMasks.removed == std::vector<std::int64_t>{2},
          "zero-length interior test uses the canonical union of touching masks");
}

void invalidInputs() {
    auto valid = inputFor();
    valid.originals = {{0, mainLane, {0, 20}}};
    valid.incoming = {{1, mainLane, {10, 30}}};
    auto input = valid;
    input.scopeLanes.clear();
    expectError(input, PlacementError::MalformedScope, "empty lane scope refuses");
    input = valid;
    input.scopeLanes.push_back(mainLane);
    expectError(input, PlacementError::MalformedScope, "duplicate scope lane refuses");
    input = valid;
    input.clock = static_cast<PlacementClock>(99);
    expectError(input, PlacementError::InvalidClock, "unknown clock refuses");
    input = valid;
    input.scopeSpan = {1, -1};
    expectError(input, PlacementError::InvalidSpan, "reversed scope span refuses");
    input = valid;
    input.originals.push_back(input.originals.front());
    expectError(input, PlacementError::DuplicateOriginalId, "duplicate original identity refuses");
    input = valid;
    input.incoming.push_back(input.incoming.front());
    expectError(input, PlacementError::DuplicateIncomingId, "duplicate incoming identity refuses");
    input = valid;
    input.incoming.front().id = 0;
    expectError(input, PlacementError::IncomingIdentityCollision, "implicit original relocation refuses");
    input.relocated = {0, 0};
    expectError(input, PlacementError::DuplicateRelocatedId, "duplicate relocation refuses");
    input = valid;
    input.relocated = {1};
    expectError(input, PlacementError::MissingRelocatedOriginal, "relocation without original refuses");
    input.relocated = {0};
    expectError(input, PlacementError::MissingRelocatedIncoming, "relocation without incoming refuses");
    input = valid;
    input.incoming.push_back({2, mainLane, {29, 40}});
    expectError(input, PlacementError::OverlappingIncoming, "overlapping group destinations refuse");
    input.incoming.back().span.begin = 30;
    check(resolvePlacement(input).ok, "touching incoming group destinations do not overlap");
    input.incoming.back() = {2, takeLane, {10, 30}};
    check(resolvePlacement(input).ok, "same span on different literal lanes does not overlap");
    input = valid;
    input.incoming.front().lane = {999, std::nullopt};
    expectError(input, PlacementError::OutsideScope, "undeclared incoming lane refuses");
    input = valid;
    input.incoming.front().span = {190, 210};
    expectError(input, PlacementError::OutsideScope, "incoming span outside scope refuses");
    input = valid;
    input.clear = {{takeLane, {-101, 1}}};
    expectError(input, PlacementError::OutsideScope, "explicit mask outside scope refuses");
    input = valid;
    input.incoming.front().id = 0;
    input.relocated = {0};
    input.scopeSpan = {10, 30};
    expectError(input, PlacementError::OutsideScope, "relocated source must be inside scope");
    input = valid;
    input.originals.front().span = {30, 20};
    expectError(input, PlacementError::InvalidSpan, "reversed original refuses");
    input = valid;
    input.incoming.front().span = {30, 20};
    expectError(input, PlacementError::InvalidSpan, "reversed incoming refuses");
    input = valid;
    input.clear = {{mainLane, {30, 20}}};
    expectError(input, PlacementError::InvalidSpan, "reversed clear mask refuses");

    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    input = valid;
    input.originals.front().span = {minimum, maximum};
    expectError(input, PlacementError::UnrepresentableSpan, "overflowing original duration refuses");
    input = valid;
    input.incoming.front().span = {minimum, maximum};
    expectError(input, PlacementError::UnrepresentableSpan, "overflowing incoming duration refuses");
    input = valid;
    input.clear = {{mainLane, {minimum, maximum}}};
    expectError(input, PlacementError::UnrepresentableSpan, "overflowing clear duration refuses");
    input = valid;
    input.scopeSpan = {minimum, maximum};
    expectError(input, PlacementError::UnrepresentableSpan, "overflowing scope duration refuses");
    input = inputFor({minimum, -1});
    input.originals = {{minimum, mainLane, {minimum, -1}}};
    input.incoming = {{maximum, mainLane, {minimum + 10, minimum + 20}}};
    const auto low = resolvePlacement(input);
    check(low.ok && source(low, minimum).survivors == std::vector<PlacementSpan>{
              {minimum, minimum + 10}, {minimum + 20, -1}},
          "extreme negative coordinates and IDs remain representable without overflow");
    input = inputFor({0, maximum});
    input.originals = {{minimum, mainLane, {0, maximum}}};
    input.incoming = {{maximum, mainLane, {maximum - 20, maximum - 10}}};
    check(resolvePlacement(input).ok, "extreme positive coordinates remain representable");
}

constexpr std::size_t cellCount = 128;
const std::array<PlacementLane, 3> fuzzLanes{{{-2, std::nullopt}, {0, std::nullopt}, {5, std::nullopt}}};
using Cells = std::array<std::optional<std::int64_t>, cellCount>;
using Board = std::array<Cells, 3>;

std::size_t laneIndex(PlacementLane lane) {
    const auto found = std::find(fuzzLanes.begin(), fuzzLanes.end(), lane);
    if (found == fuzzLanes.end()) {
        throw std::runtime_error("unexpected fuzz lane");
    }
    return static_cast<std::size_t>(found - fuzzLanes.begin());
}

bool occupies(PlacementSpan span, std::size_t cell) {
    const auto position = static_cast<std::int64_t>(cell);
    return span.begin <= position && position < span.end;
}

bool oracleMask(const PlacementInput& input, PlacementLane lane, std::size_t cell) {
    for (const auto& item : input.incoming) {
        if (item.lane == lane && occupies(item.span, cell)) {
            return true;
        }
    }
    for (const auto& item : input.clear) {
        if (item.lane == lane && occupies(item.span, cell)) {
            return true;
        }
    }
    return false;
}

void seededSequence() {
    std::mt19937 random(0x02174003U);
    // Library shuffle implementations may consume different amounts of random
    // state. Keep that independent of the cross-toolchain edit sequence.
    std::mt19937 permutationRandom(0x02174004U);
    std::vector<OriginalInterval> state;
    std::int64_t nextId = -20;
    for (const auto lane : fuzzLanes) {
        state.push_back({nextId++, lane, {0, 12}});
        state.push_back({nextId++, lane, {30, 45}});
        state.push_back({nextId++, lane, {80, 110}});
    }
    const auto pick = [&](std::size_t count) {
        return static_cast<std::size_t>(random()) % count;
    };
    for (std::size_t step = 0; step < 40; ++step) {
        auto input = inputFor({0, static_cast<std::int64_t>(cellCount)});
        input.scopeLanes.assign(fuzzLanes.begin(), fuzzLanes.end());
        input.originals = state;
        const auto lane = fuzzLanes[pick(fuzzLanes.size())];
        const auto begin = static_cast<std::int64_t>(pick(100));
        const auto length = static_cast<std::int64_t>(pick(20) + 1);
        if (step % 5 == 0 && state.size() >= 2) {
            const auto first = pick(state.size());
            auto second = pick(state.size() - 1);
            if (second >= first) {
                ++second;
            }
            const auto& a = state[first];
            const auto& b = state[second];
            input.relocated = {a.id, b.id};
            input.incoming = {{a.id, b.lane, b.span}, {b.id, a.lane, a.span}};
        } else if (step % 5 == 1 && !state.empty()) {
            const auto& original = state[pick(state.size())];
            input.relocated = {original.id};
            input.incoming = {{original.id, lane, {begin, begin + length}}};
        } else if (step % 5 == 2) {
            input.clear = {{lane, {begin, begin + length}}, {lane, {115, 120}}};
        } else {
            input.incoming = {{nextId++, lane, {begin, begin + length}}};
            if (step % 5 == 3) {
                input.clear = {{lane, {begin, 120}}};
            }
        }

        const auto plan = resolvePlacement(input);
        check(plan.ok, "seeded 40-edit three-track placement is admitted");
        if (!plan.ok) {
            throw std::runtime_error(toString(plan.error));
        }
        auto permuted = input;
        std::shuffle(permuted.scopeLanes.begin(), permuted.scopeLanes.end(), permutationRandom);
        std::shuffle(permuted.originals.begin(), permuted.originals.end(), permutationRandom);
        std::shuffle(permuted.incoming.begin(), permuted.incoming.end(), permutationRandom);
        std::shuffle(permuted.relocated.begin(), permuted.relocated.end(), permutationRandom);
        std::shuffle(permuted.clear.begin(), permuted.clear.end(), permutationRandom);
        check(resolvePlacement(permuted) == plan, "seeded result is invariant under input permutation");

        Board expected{};
        Board actual{};
        std::vector<OriginalInterval> next;
        for (const auto& original : state) {
            const bool moved = std::find(input.relocated.begin(), input.relocated.end(), original.id)
                               != input.relocated.end();
            std::vector<PlacementSpan> expectedFragments;
            std::optional<std::int64_t> run;
            for (std::size_t cell = 0; cell <= cellCount; ++cell) {
                const bool remains = cell < cellCount && !moved && occupies(original.span, cell)
                                     && !oracleMask(input, original.lane, cell);
                if (remains) {
                    expected[laneIndex(original.lane)][cell] = original.id;
                    if (!run) {
                        run = static_cast<std::int64_t>(cell);
                    }
                } else if (run) {
                    expectedFragments.push_back({*run, static_cast<std::int64_t>(cell)});
                    run.reset();
                }
            }
            const auto& fragments = source(plan, original.id);
            check(fragments.relocated == moved && fragments.survivors == expectedFragments,
                  "independent cell oracle matches every original's exact survivor intervals");
            check((std::find(plan.removed.begin(), plan.removed.end(), original.id) != plan.removed.end())
                      == (!moved && expectedFragments.empty()),
                  "independent cell oracle matches removed identity membership");
            bool first = true;
            for (const auto fragment : fragments.survivors) {
                next.push_back({first ? original.id : nextId++, original.lane, fragment});
                first = false;
                for (std::size_t cell = 0; cell < cellCount; ++cell) {
                    if (occupies(fragment, cell)) {
                        check(!actual[laneIndex(original.lane)][cell].has_value(),
                              "survivor occupancy is disjoint");
                        actual[laneIndex(original.lane)][cell] = original.id;
                    }
                }
            }
        }
        for (const auto& item : input.incoming) {
            for (std::size_t cell = 0; cell < cellCount; ++cell) {
                if (occupies(item.span, cell)) {
                    expected[laneIndex(item.lane)][cell] = item.id;
                }
            }
        }
        for (const auto& item : plan.incoming) {
            next.push_back({item.id, item.lane, item.span});
            for (std::size_t cell = 0; cell < cellCount; ++cell) {
                if (occupies(item.span, cell)) {
                    check(!actual[laneIndex(item.lane)][cell].has_value(),
                          "all destinations were cleared before incoming placement");
                    actual[laneIndex(item.lane)][cell] = item.id;
                }
            }
        }
        check(actual == expected, "independent three-track occupancy oracle matches final placement");
        for (std::size_t a = 0; a < next.size(); ++a) {
            for (std::size_t b = a + 1; b < next.size(); ++b) {
                check(next[a].id != next[b].id, "owner-assigned final fragment IDs remain unique");
                check(next[a].lane != next[b].lane
                          || std::max(next[a].span.begin, next[b].span.begin)
                              >= std::min(next[a].span.end, next[b].span.end),
                      "independent pairwise overlap check passes after every edit");
            }
        }
        state = std::move(next);
    }
}

} // namespace

int main() {
    try {
        intervalCases();
        unionAndIdentity();
        emptyIntervals();
        invalidInputs();
        seededSequence();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
