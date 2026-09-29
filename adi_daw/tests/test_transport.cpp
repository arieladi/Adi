// SPDX-License-Identifier: GPL-3.0-or-later
//
// Transport (transport.hpp): the playhead every clip reader, strip, device and
// scope tap reads. Each answer is checked against a model that walks the
// timeline one sample at a time, so the closed-form wrap arithmetic in
// sampleAt and contiguousFrames is tested by something that cannot share its
// mistakes. Case ideas came from the drone's test-ideas pass; its expected
// values were wrong in three of eight cases, so every value here is derived
// from the model, not from that report.
#include "adi/engine/transport.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace {
using adi::engine::Transport;

int checks = 0, failures = 0;
void check(bool ok, const std::string& name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}

// The slow truth: where the playhead is after `steps` samples, walked one at a
// time. A position already at or past the loop end re-enters the loop first,
// which is what sampleAt does for a playhead the user located past the end.
struct Model {
    std::int64_t position = 0, begin = 0, end = 0;
    bool looping = false;

    std::int64_t at(std::int64_t steps) const {
        std::int64_t p = position;
        if (looping && p >= end) p = begin + (p - begin) % (end - begin);
        for (std::int64_t i = 0; i < std::max<std::int64_t>(0, steps); ++i) {
            ++p;
            if (looping && p == end) p = begin;
        }
        return p;
    }
};

Transport make(const Model& m) {
    Transport t;
    t.locate(m.position);
    t.loop(m.begin, m.end, m.looping);
    return t;
}

void basics() {
    Transport t;
    check(t.position() == 0 && !t.playing(), "a new transport is stopped at 0");
    const auto r0 = t.revision();
    t.locate(-1);
    check(t.position() == 0, "locate clamps a negative sample to 0");
    t.locate(42);
    check(t.position() == 42 && t.revision() == r0 + 2, "locate sets the position and bumps the revision");
    const auto r1 = t.revision();
    t.play(false);
    check(t.revision() == r1, "play(false) while stopped changes nothing");
    t.play(true);
    check(t.playing() && t.revision() == r1 + 1, "play(true) starts and bumps the revision once");
}

void loopShapes() {
    Transport t;
    t.loop(50, 20, true);
    check(t.sampleAt(40) == 40, "end before begin is not a loop: the playhead runs straight through");
    t.loop(100, 100, true);
    check(t.sampleAt(150) == 150, "an empty loop is not a loop");
    t.loop(-5, 10, true);
    check(t.sampleAt(10) == 0, "a negative loop start clamps to 0, so the loop is 0..10");
    t.loop(0, 100, false);
    check(t.sampleAt(150) == 150, "a disabled loop does not wrap");
}

void pastTheEnd() {
    // The case the drone got wrong: located past the end of an active loop.
    Transport t;
    t.locate(150);
    t.loop(0, 100, true);
    check(t.sampleAt(0) == 50, "past the end, offset 0 re-enters the loop at (150 - 0) mod 100");
    check(t.sampleAt(50) == 0, "past the end, offset 50 lands exactly on the loop start");
    check(t.contiguousFrames(50, 30) == 30, "30 frames from the loop start do not reach the end");
    check(t.contiguousFrames(0, 80) == 50, "from 50, only 50 frames remain before the wrap");
}

void stoppedAndEmptyAdvance() {
    Transport t;
    t.locate(10);
    t.advance(64);
    check(t.position() == 10, "advance does nothing while stopped");
    t.play();
    t.advance(0);
    t.advance(-5);
    check(t.position() == 10, "advance by zero or a negative count does nothing");
    t.advance(64);
    check(t.position() == 74, "advance moves a playing transport");
}

void saturation() {
    Transport t;
    const auto max = std::numeric_limits<std::int64_t>::max();
    t.locate(max - 5);
    check(t.sampleAt(100) == max, "a position near the end of time saturates instead of overflowing");
    check(t.sampleAt(-7) == max - 5, "a negative offset is read as 0");
}

// Every combination in a small box, against the walking model: begin 0..12,
// end 0..16, position 0..40, offsets and counts 0..40. Both sides of every
// boundary (at the loop end, one before, one after, far past) are inside it.
void againstTheModel() {
    long mismatches = 0, cases = 0;
    for (std::int64_t begin = 0; begin <= 12; ++begin)
        for (std::int64_t end = 0; end <= 16; ++end)
            for (int looping = 0; looping <= 1; ++looping)
                for (std::int64_t position = 0; position <= 40; ++position) {
                    Model m{position, begin, end, looping == 1 && end > begin};
                    const Transport t = make(Model{position, begin, end, looping == 1});
                    for (std::int64_t off = 0; off <= 40; off += 3) {
                        ++cases;
                        if (t.sampleAt(off) != m.at(off)) ++mismatches;
                        for (std::int64_t count = 0; count <= 40; count += 7) {
                            const std::int64_t got = t.contiguousFrames(off, count);
                            // contiguousFrames promises frames up to (not including) the wrap.
                            const std::int64_t want =
                                m.looping ? std::min(count, end - m.at(off)) : count;
                            if (got != want) ++mismatches;
                        }
                    }
                }
    check(mismatches == 0, "sampleAt and contiguousFrames agree with the walking model (" +
                               std::to_string(cases) + " cases, " + std::to_string(mismatches) +
                               " mismatches)");
}

// advance is sampleAt applied to the playhead: many small advances must land
// where one big one does, whatever the block sizes, loop or not.
void advanceComposes() {
    bool same = true;
    for (int looping = 0; looping <= 1; ++looping)
        for (std::int64_t start : {0, 7, 29, 95}) {
            Transport a, b;
            for (Transport* t : {&a, &b}) {
                t->locate(start);
                t->loop(10, 37, looping == 1);
                t->play();
            }
            std::int64_t total = 0;
            for (std::int32_t block : {1, 3, 64, 5, 17, 32, 2, 11}) {
                a.advance(block);
                total += block;
            }
            b.advance(static_cast<std::int32_t>(total));
            same = same && a.position() == b.position();
        }
    check(same, "advancing in blocks of any size lands where one advance does");
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    basics();
    loopShapes();
    pastTheEnd();
    stoppedAndEmptyAdvance();
    saturation();
    againstTheModel();
    advanceComposes();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
