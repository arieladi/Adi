// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/note_identity.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>

using namespace adi::engine;
namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void simultaneousSources() {
    PluginNoteIds ids;
    // The former modulo fold mapped both IDs to 9.
    const auto clip = eventIdentity(1, 5);
    const auto live = eventIdentity(2, 1);
    const auto first = ids.assign(clip);
    const auto second = ids.assign(live);
    check(first && second && first != second, "the native voice IDs distinguish a clip from live input");
    check(ids.assign(clip) == first && ids.lookup(live) == second, "one held voice keeps its native ID");
    check(!ids.lookup(eventIdentity(3, 3)), "an unmatched addressed release cannot become a wildcard");
    if (!first || !second) return;
    check(ids.noteEnd(*first) && !ids.lookup(clip) && ids.lookup(live) == second,
          "exact NOTE_END frees one mapping without touching the other source");
    const auto replacement = ids.assign(clip);
    check(replacement && replacement != first && !ids.noteEnd(*first) && ids.lookup(clip) == replacement,
          "a stale NOTE_END does not release a recycled slot");
    check(ids.release(live) && !ids.lookup(live), "an admitted release retires its mapping");
}

void millionAdmissions() {
    struct Held {
        std::uint64_t engineId = 0;
        std::int32_t pluginId = -1;
    };
    std::array<Held, PluginNoteIds::capacity> held{};
    PluginNoteIds ids;
    std::mt19937_64 rng(170022);
    bool distinct = true;
    for (std::uint32_t serial = 1; serial <= 1'000'000; ++serial) {
        auto& slot = held[static_cast<std::size_t>(rng() % held.size())];
        if (slot.engineId)
            distinct = (serial % 2 == 0 ? ids.release(slot.engineId) : ids.noteEnd(slot.pluginId)) && distinct;
        slot.engineId = eventIdentity(static_cast<std::uint32_t>(1 + rng() % 31), serial);
        const auto wire = ids.assign(slot.engineId);
        if (!wire) {
            distinct = false;
            break;
        }
        slot.pluginId = *wire;
        for (const auto& other : held)
            if (&other != &slot && other.pluginId == *wire) distinct = false;
    }
    check(distinct, "one million admissions with random lifetimes keep all held native IDs distinct");
    check(!ids.assign(eventIdentity(40, 1)), "a full map refuses new attacks without evicting an active voice");
    check(ids.noteEnd(held.front().pluginId) && ids.assign(eventIdentity(40, 1)),
          "a full map can admit another attack after NOTE_END");
}
}

int main() {
    const auto first = allocateEventSourcePrefix();
    const auto second = allocateEventSourcePrefix();
    check(first && second && first != second && eventSourcePrefix(eventIdentity(first, 7)) == first,
          "independent event producers receive nonzero prefixes in the upper 32 bits");
    simultaneousSources();
    millionAdmissions();
    if (failures == 0) std::puts("PASS -- fixed note identity and one million admissions");
    return failures ? 1 : 0;
}
