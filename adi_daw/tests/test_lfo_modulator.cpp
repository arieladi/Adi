// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's LFO modulator, the kernel (DEV-07's device side, ADR-0249 d8).
// Each property is checked against a number derived apart from the code: the
// waveforms at known phases, the period from the rate or the song's tempo,
// Phase as a shift of a quarter cycle, Offset and Depth as a line. Parity with
// Live's LFO waits on a Live 12.4 reading.
#include "adi/dsp/lfo_modulator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <string>
#include <vector>

namespace {
bool counting = false;
std::size_t allocations = 0;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}
}  // namespace
void* operator new(std::size_t n) {
    if (counting) {
        ++allocations;
    }
    if (void* p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using L = adi::dsp::LfoModulator;
constexpr double kRate = 48000;

/// `frames` values in blocks of `block`; synced, the song starts at `from`.
std::vector<double> render(L& lfo, std::size_t frames, std::size_t block, std::int64_t from = 0, double bpm = 120) {
    std::vector<double> out(frames);
    for (std::size_t at = 0; at < frames; at += block) {
        const auto n = std::min(block, frames - at);
        L::Position position;
        position.sample = from + static_cast<std::int64_t>(at);
        position.bpm = bpm;
        lfo.render(out.data() + at, n, position);
    }
    return out;
}
L made(int shape, double rate = 1, bool sync = false) {
    L lfo;
    lfo.set(L::Shape, shape);
    lfo.set(L::Rate, rate);
    lfo.set(L::Sync, sync ? 1 : 0);
    lfo.prepare(kRate);
    return lfo;
}

void waveforms() {
    // 1 Hz at 48 kHz: a cycle is 48,000 samples, sample k at phase k / 48,000.
    struct Point {
        int shape;
        std::size_t at;
        double want;
    };
    const Point points[] = {{L::Sine, 0, 0},         {L::Sine, 12000, 1},       {L::Sine, 36000, -1},
                            {L::Triangle, 0, 0},     {L::Triangle, 12000, 1},   {L::Triangle, 24000, 0},
                            {L::Triangle, 36000, -1}, {L::SawUp, 0, -1},        {L::SawUp, 24000, 0},
                            {L::SawDown, 0, 1},      {L::SawDown, 24000, 0},    {L::Square, 1000, 1},
                            {L::Square, 25000, -1}};
    for (const auto& p : points) {
        auto lfo = made(p.shape);
        const auto out = render(lfo, 48000, 64);
        check(std::abs(out[p.at] - p.want) < 1e-9, "shape " + std::to_string(p.shape) + " at sample " +
                                                         std::to_string(p.at) + " is " + std::to_string(p.want));
    }
    // A plant: one sample later is not the same value (the tolerance is tight).
    auto saw = made(L::SawUp);
    check(std::abs(render(saw, 48000, 64)[24001] - 0) > 1e-9, "plant: a sample later reads another value");
    // Random holds one value a cycle, in [-1, 1): at 10 Hz it changes 9 times
    // in a second, 4,800 samples apart (free running, a boundary falls within
    // a sample of its nominal place: the phase is summed, not computed).
    auto random = made(L::Random, 10);
    const auto r = render(random, 48000, 64);
    bool bounded = true, spaced = true;
    std::vector<std::size_t> changes;
    for (std::size_t i = 0; i < r.size(); ++i) {
        bounded = bounded && r[i] >= -1 && r[i] < 1;
        if (i > 0 && r[i] != r[i - 1]) {
            changes.push_back(i);
        }
    }
    for (std::size_t k = 0; k < changes.size(); ++k) {
        const auto nominal = 4800 * (k + 1);
        spaced = spaced && changes[k] + 1 >= nominal && changes[k] <= nominal + 1;
    }
    check(bounded && spaced && changes.size() == 9, "Random holds a value in [-1, 1) for each cycle of 4,800 samples");
    // Wander, on the same clock, passes each cycle's random value at its
    // start and moves smoothly between them.
    auto wander = made(L::Wander, 10);
    const auto w = render(wander, 48000, 64);
    double step = 0, missed = 0;
    for (std::size_t i = 1; i < w.size(); ++i) {
        step = std::max(step, std::abs(w[i] - w[i - 1]));
    }
    for (auto at : changes) {
        missed = std::max(missed, std::abs(w[at] - r[at]));
    }
    check(!changes.empty() && missed < 1e-6 && step < 2 * std::numbers::pi / 4800,
          "Wander passes each cycle's random value and moves smoothly between them");
}

void rates() {
    // Free: 4.8 Hz is a cycle of 10,000 samples.
    auto lfo = made(L::Sine, 4.8);
    const auto out = render(lfo, 40000, 97);
    check(std::abs(out[2500] - 1) < 1e-9 && std::abs(out[12500] - 1) < 1e-9 && std::abs(out[32500] - 1) < 1e-9,
          "4.8 Hz: the crest every 10,000 samples");
    // Synced: 1/4 at 120 BPM is 24,000 samples a cycle at 48 kHz.
    auto synced = made(L::Sine, 1, true);
    synced.set(L::Division, 4);
    const auto s = render(synced, 96000, 64);
    check(std::abs(s[6000] - 1) < 1e-12 && std::abs(s[30000] - 1) < 1e-12 && std::abs(s[18000] + 1) < 1e-12,
          "synced 1/4 at 120 BPM: a cycle of 24,000 samples");
    // A locate: rendered from sample 100,000 it reads the song's own phase.
    auto located = made(L::Sine, 1, true);
    located.set(L::Division, 4);
    const auto from = render(located, 4800, 64, 100000);
    auto through = made(L::Sine, 1, true);
    through.set(L::Division, 4);
    const auto all = render(through, 104800, 64);
    check(std::equal(from.begin(), from.end(), all.begin() + 100000),
          "synced: a locate lands on the song's phase, bit for bit");
    // A tempo segment: from sample 48,000 at 2 quarter notes, 60 BPM; 1/4 is
    // then 48,000 samples, and the crest a quarter cycle in, at 60,000.
    auto segment = made(L::Sine, 1, true);
    segment.set(L::Division, 4);
    std::vector<double> v(1);
    L::Position p;
    p.sample = 60000;
    p.originSample = 48000;
    p.originQuarterNotes = 2;
    p.bpm = 60;
    segment.render(v.data(), 1, p);
    check(std::abs(v[0] - 1) < 1e-12, "synced across a tempo segment: the segment's origin and tempo set the phase");
}

void phaseOffsetDepth() {
    auto lfo = made(L::Sine);
    lfo.set(L::Phase, 90);
    check(std::abs(render(lfo, 1, 1)[0] - 1) < 1e-12, "Phase 90 degrees starts a sine at its crest");
    auto line = made(L::SawUp);
    line.set(L::Depth, 0.5);
    line.set(L::Offset, 0.5);
    const auto out = render(line, 48000, 64);
    check(std::abs(out[0]) < 1e-12 && std::abs(out[24000] - 0.5) < 1e-12, "Offset 0.5, Depth 0.5: 0 to 1");
}

void blockSizes() {
    // Every shape, free and synced, with Jitter and Smooth on too: the same
    // bits at every block size from 1 to 4,096.
    bool identical = true;
    for (int shape = 0; shape < L::Shapes; ++shape) {
        for (bool sync : {false, true}) {
            for (double jitter : {0.0, 0.7}) {
                auto base = made(shape, 3.3, sync);
                base.set(L::Division, 2);
                base.set(L::Jitter, jitter);
                base.set(L::Smooth, jitter > 0 ? 0.4 : 0.0);
                base.prepare(kRate);
                const auto one = render(base, 30000, 1, 12345);
                for (std::size_t block : {32u, 33u, 64u, 100u, 128u, 256u, 512u, 1000u, 1024u, 2048u, 4096u}) {
                    auto other = made(shape, 3.3, sync);
                    other.set(L::Division, 2);
                    other.set(L::Jitter, jitter);
                    other.set(L::Smooth, jitter > 0 ? 0.4 : 0.0);
                    other.prepare(kRate);
                    identical = identical && render(other, 30000, block, 12345) == one;
                }
            }
        }
    }
    check(identical, "every shape, free and synced, jittered and smoothed: the same bits at block sizes 1 to 4,096");
}

void smoothAndAllocation() {
    // Smooth on a 0.1 Hz square (50 ms against a 5 s half cycle): the edge
    // glides, then lands exactly on the target.
    auto lfo = made(L::Square, 0.1);
    lfo.set(L::Smooth, 1);
    const auto out = render(lfo, 480000, 64);
    check(out[240001] > -1 && out[240001] < 1 && out[479999] == -1.0, "Smooth glides across an edge, then settles exactly");
    // Rendering allocates nothing.
    auto quiet = made(L::Wander, 2, true);
    std::vector<double> buffer(512);
    L::Position position;
    counting = true;
    for (int b = 0; b < 100; ++b) {
        position.sample = b * 512;
        quiet.render(buffer.data(), buffer.size(), position);
    }
    counting = false;
    check(allocations == 0, "render allocates nothing");
}

}  // namespace

int main() {
    waveforms();
    rates();
    phaseOffsetDepth();
    blockSizes();
    smoothAndAllocation();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
