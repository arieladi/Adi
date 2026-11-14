// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/mix/AlignedKey.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace {
thread_local bool realtime = false;
thread_local unsigned allocations = 0, deallocations = 0;
}
void *operator new(std::size_t n) {
    if (realtime)
        ++allocations;
    if (auto *memory = std::malloc(n ? n : 1))
        return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
    if (p && realtime)
        ++deallocations;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
struct Audio {
    bool previous = realtime;
    Audio() { realtime = true; }
    ~Audio() { realtime = previous; }
};
template <class Sample> void exercise() {
    using Key = adi::mix::AlignedKey<Sample>;
    Key key;
    check(!key.capture(nullptr, 0, 1, 0).valid, "unprepared transport rejects");
    key.prepare(17, 64);
    std::array<Sample, 64> left{}, right{};
    std::array<const Sample *, 2> input{left.data(), right.data()};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = static_cast<Sample>(i + 1);
        right[i] = -left[i];
    }
    typename Key::View view;
    {
        Audio call;
        view = key.capture(input.data(), 2, 64, 0);
    }
    check(view.valid && view.channels == 2 && view.samples, "connected stereo view");
    check(view.samples[0][0] == 1 && view.samples[1][63] == -64, "zero delay preserves key samples");
    left.fill(0);
    right.fill(0);
    check(view.samples[0][0] == 1 && view.samples[1][63] == -64, "in-place main cannot overwrite captured key");

    key.clear();
    std::array<Sample, 257> timeline{};
    for (std::size_t i = 0; i < timeline.size(); ++i)
        timeline[i] = static_cast<Sample>(i + 1);
    std::size_t cursor = 0;
    bool exact = true;
    constexpr std::array<std::int32_t, 9> slices{1, 17, 3, 64, 7, 43, 2, 56, 64};
    constexpr std::array<std::int32_t, 9> delays{17, 17, 0, 5, 17, 1, 17, 3, 17};
    for (std::size_t part = 0; part < slices.size(); ++part) {
        const std::array<const Sample *, 1> mono{timeline.data() + cursor};
        const auto count = slices[part];
        const auto delay = static_cast<std::size_t>(delays[part]);
        {
            Audio call;
            view = key.capture(mono.data(), 1, count, delays[part]);
        }
        exact = exact && view.valid && view.channels == 2;
        for (std::int32_t i = 0; i < count; ++i) {
            const auto at = cursor + static_cast<std::size_t>(i);
            const auto expected = at < delay ? Sample{0} : timeline[at - delay];
            exact = exact && view.samples[0][i] == expected && view.samples[1][i] == expected;
        }
        cursor += static_cast<std::size_t>(count);
    }
    check(exact && cursor == timeline.size(), "mono broadcast, ring wrap and changing admitted delay across slices");

    key.clear();
    left.fill(0);
    right.fill(0);
    left[0] = static_cast<Sample>(7);
    const std::array<const Sample *, 2> partial{left.data(), nullptr};
    view = key.capture(partial.data(), 2, 1, 17);
    check(view.valid && view.samples[0][0] == 0 && view.samples[1][0] == 0, "partial stereo begins with delayed silence");
    {
        Audio call;
        view = key.capture(nullptr, 0, 17, 17);
    }
    check(view.samples && view.samples[0][16] == 7 && view.samples[1][16] == 0, "key disconnection drains delayed last sample");
    const auto beforeSleep = key.statistics();
    {
        Audio call;
        view = key.capture(nullptr, 0, 64, 17);
    }
    check(view.valid && !view.samples && view.channels == 0, "drained absent key returns null");
    check(key.statistics().historyFrames == beforeSleep.historyFrames, "drained absent key does zero sample work");
    left.fill(0);
    left[0] = static_cast<Sample>(13);
    {
        Audio call;
        view = key.capture(partial.data(), 2, 17, 17);
    }
    exact = view.valid && view.samples;
    for (std::int32_t i = 0; i < 17; ++i)
        exact = exact && view.samples[0][i] == 0 && view.samples[1][i] == 0;
    check(exact, "reconnect after sleeping cannot resurrect stale key samples");
    {
        Audio call;
        view = key.capture(nullptr, 0, 1, 17);
    }
    check(view.valid && view.samples && view.samples[0][0] == 13, "reconnected key keeps exact delayed onset");

    key.clear();
    left.fill(static_cast<Sample>(3));
    input = {left.data(), left.data()};
    {
        Audio call;
        view = key.capture(input.data(), 2, 17, 17, false);
    }
    check(view.valid && !view.samples && key.statistics().materializedFrames == 0,
          "disabled downstream retains history without materialization");
    {
        Audio call;
        view = key.capture(nullptr, 0, 17, 17);
    }
    exact = view.valid && view.samples;
    for (std::int32_t i = 0; i < 17; ++i)
        exact = exact && view.samples[0][i] == 3 && view.samples[1][i] == 3;
    check(exact, "downstream wake receives original upstream history");

    key.clear();
    left.fill(0);
    view = key.capture(input.data(), 2, 64, 17);
    check(view.valid && view.channels == 2, "connected zero remains an explicit external key");
    auto reject = [&](std::int32_t channels, std::int32_t frames, std::int32_t delay, const Sample *const *source) {
        const auto before = key.statistics();
        const auto result = key.capture(source, channels, frames, delay);
        check(!result.valid && before.historyFrames == key.statistics().historyFrames,
              "invalid transport call rejects without advancing history");
    };
    reject(2, 65, 17, input.data());
    reject(2, -1, 17, input.data());
    reject(2, 1, 18, input.data());
    reject(2, 1, -1, input.data());
    reject(3, 1, 17, input.data());
    reject(1, 1, 17, nullptr);
    view = key.capture(nullptr, 0, 0, 17);
    check(view.valid, "empty segment is accepted");

    key.prepare(0, 64);
    right.fill(static_cast<Sample>(11));
    input = {right.data(), left.data()};
    view = key.capture(input.data(), 2, 64, 0);
    check(view.valid && view.channels == 2 && view.samples[0][63] == 11 && view.samples[1][63] == 0,
          "key planes remain independent of a downstream mono main");
    input = {left.data(), right.data()};
    view = key.capture(input.data(), 2, 1, 0);
    check(view.valid && view.samples[0][0] == 0 && view.samples[1][0] == 11,
          "right-only stereo key survives transport to a mono-main detector");
    check(key.capture(nullptr, 0, 64, 0).samples == nullptr, "zero-delay key disconnection sleeps immediately");
    bool threw = false;
    try {
        key.prepare(-1, 64);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    check(threw && key.capture(input.data(), 2, 1, 0).valid, "invalid prepare leaves prior capacity usable");
}
} // namespace
int main() {
    exercise<float>();
    exercise<double>();
    check(allocations == 0 && deallocations == 0, "audio capture allocates and deallocates zero");
    std::printf("mix aligned key: %d checks / %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
