// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/measure/sync_spike.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
using namespace adi::measure;
int checks = 0, failures = 0;
void check(bool okay, const char* label) {
    ++checks;
    if (!okay) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

void knownSpikes() {
    constexpr std::uint64_t desired = 1234;
    std::vector<float> reference(4097), separate(reference.size()), returned(reference.size());
    renderSyncSpike(0, reference, desired, 21983);
    for (std::size_t at = 0; at < separate.size(); at += 511) {
        renderSyncSpike(at, std::span(separate).subspan(at, std::min<std::size_t>(511, separate.size() - at)),
                        desired, 21983);
    }
    check(reference == separate && reference[desired] == 0.8f, "the seeded spike is independent of output partitioning");
    renderSyncSpike(0, returned, desired + 257, 211040);
    check(!std::equal(reference.begin(), reference.begin() + 1000, returned.begin()),
          "a distinct seed generates independent surrounding samples");
    for (auto& sample : returned) sample *= -0.5f;
    const auto outgoing = findSyncSpike(reference);
    const auto incoming = findSyncSpike(returned);
    check(outgoing && outgoing.frame == desired && outgoing.value == 0.8f,
          "a noisy fixture locates its marker at the requested sample 1234");
    check(incoming && outgoing && incoming.frame - outgoing.frame == 257 && incoming.value == -0.4f,
          "signed attenuation and independent dither leave the exact integer delay observable");
    const auto selected = findSyncSpike(std::span(returned).subspan(1000, 1000));
    check(selected && selected.frame + 1000 == incoming.frame, "marker coordinates are relative to the inspected span");
}

void refusalAndRinging() {
    std::vector<float> capture(2001);
    check(findSyncSpike(capture).status == SyncSpikeStatus::SignalTooLow,
          "a silent return has no sync marker");
    capture[1234] = 0.8f;
    capture[1239] = 0.54f;
    check(findSyncSpike(capture).status == SyncSpikeStatus::Ambiguous,
          "a second strong event outside the local impulse makes alignment ambiguous");
    capture[1239] = 0;
    capture[1238] = 0.54f;
    const auto ringing = findSyncSpike(capture);
    check(ringing && ringing.frame == 1234,
          "adjacent ringing belongs to the same isolated event");
    capture[101] = std::numeric_limits<float>::quiet_NaN();
    check(findSyncSpike(capture).status == SyncSpikeStatus::InvalidInput &&
              findSyncSpike({}).status == SyncSpikeStatus::InvalidInput,
          "nonfinite and empty captures are refused");
}
}

int main() {
    knownSpikes();
    refusalAndRinging();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
