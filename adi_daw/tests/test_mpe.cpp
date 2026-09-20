// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0054: no MIDI byte survives the input parser.
//
// The mandate's failure mode is that nothing fails. A 7-bit value sitting in a
// double looks exactly like a 14-bit one until somebody plays a Continuum and
// hears stair-steps, and by then it is in the op log, the blobs and the
// automation lanes. So the checks here are mostly about RESOLUTION SURVIVING,
// not about values being roughly right.

#include "adi/engine/mpe_input.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace adi;
using namespace adi::engine;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }

/// Collects what the parser emits.
struct Collect {
    std::vector<Event> events;
    static bool sink(void* ctx, const Event& e) {
        static_cast<Collect*>(ctx)->events.push_back(e);
        return true;
    }
};

void feed(MpeParser& p, Collect& c, std::initializer_list<int> bytes) {
    std::vector<std::uint8_t> b;
    for (int v : bytes) b.push_back(static_cast<std::uint8_t>(v));
    p.feed(b.data(), static_cast<std::int32_t>(b.size()), &Collect::sink, &c);
}

// --- the structural guard ----------------------------------------------------

void testTheTypeItself() {
    section("ADR-0054 -- the event's value type is floating point, structurally");

    // The trap the ADR names is "a single std::uint8_t in an event struct".
    // This is the check that a future tidy-up cannot get past.
    static_assert(std::is_floating_point_v<decltype(Event::value)>,
                  "Event::value must be floating point -- ADR-0054");
    static_assert(sizeof(Event::value) >= 8,
                  "Event::value must be a double, not a float -- VST3's "
                  "INoteExpressionController and CLAP's note expression both "
                  "take a double, so the narrow point would be ours");
    check(std::is_floating_point_v<decltype(Event::value)>,
          "Event::value is floating point");
    check(sizeof(Event::value) == 8, "and it is 8 bytes, not 4");

    // noteId is what expression anchors to, and 8 bits of it would run out
    // after 256 notes in a session.
    check(sizeof(Event::noteId) == 8, "noteId is 64-bit, so it never wraps in a session");
}

// --- resolution ---------------------------------------------------------------

void testFourteenBitBendSurvives() {
    section("a 14-bit bend arrives with 14 bits, not 7");

    MpeParser p;
    p.setZone(MpeZone::lower());
    Collect c;

    // Start a note so the bend has something to anchor to.
    feed(p, c, {0x91, 60, 100});          // ch2 note on

    // Two bend words one LSB step apart. If anything in the chain truncated
    // to 7 bits these would be equal, and the whole mandate would be lost
    // with every test still green.
    feed(p, c, {0xE1, 0x01, 0x40});       // word = 8192 + 1
    feed(p, c, {0xE1, 0x02, 0x40});       // word = 8192 + 2

    check(c.events.size() == 3, "three events");
    if (c.events.size() < 3) return;
    const double a = c.events[1].value;
    const double b = c.events[2].value;
    check(a != b, "two adjacent 14-bit bend words produce DIFFERENT doubles -- "
                  "this is the check that fails the moment anything truncates");
    check(b > a, "and in the right direction");

    // A 7-bit-only chain would step ~0.75 semitones at a +/-48 range; the
    // real step is ~0.0059. Assert the magnitude, so a chain that kept 14
    // bits but scaled them wrongly is also caught.
    const double step = b - a;
    check(step > 0.0 && step < 0.01,
          "one LSB is a small step, saw " + std::to_string(step) +
          " semitones (a 7-bit chain would show ~0.75)");
}

void testBendRangeAndCentre() {
    section("the bend range is MPE's +/-48, and the centre asymmetry is handled");

    check(std::abs(bendToSemitones(8192, 48.0)) < 1e-12, "centre is exactly zero");
    check(std::abs(bendToSemitones(16383, 48.0) - 48.0) < 1e-9,
          "full up reaches exactly +range -- there are 8191 steps above centre, "
          "not 8192, and dividing by the wrong one falls short");
    check(std::abs(bendToSemitones(0, 48.0) + 48.0) < 1e-9,
          "full down reaches exactly -range");

    // The default that matters. MPE's per-note range is +/-48 and a host that
    // assumes the MIDI default of +/-2 transposes every gesture by a factor
    // of 24 -- not subtle, but only audible with a real controller.
    MpeZone z = MpeZone::lower();
    check(z.memberBendSemitones == 48.0, "a member channel defaults to +/-48");
    check(z.masterBendSemitones == 2.0, "the master channel to the ordinary +/-2");

    MpeParser p;
    p.setZone(z);
    Collect c;
    feed(p, c, {0x91, 60, 100});
    feed(p, c, {0xE1, 0x7F, 0x7F});        // member, full up
    feed(p, c, {0xE0, 0x7F, 0x7F});        // master, full up
    check(c.events.size() == 3, "three events");
    if (c.events.size() < 3) return;
    check(std::abs(c.events[1].value - 48.0) < 1e-9, "member bend uses the member range");
    check(std::abs(c.events[2].value - 2.0) < 1e-9, "master bend uses the master range");
}

void testFourteenBitTimbre() {
    section("MPE+ 14-bit CC: the LSB is waited for, and a bare MSB is still 7-bit");

    MpeParser p;
    p.setZone(MpeZone::lower());
    Collect c;
    feed(p, c, {0x91, 60, 100});           // note on

    // A bare MSB is a legitimate ordinary CC and must still arrive -- waiting
    // for an LSB that never comes would drop every non-MPE+ controller.
    feed(p, c, {0xB1, 74, 64});
    check(c.events.size() == 2, "a bare CC74 emits immediately");
    if (c.events.size() < 2) return;
    check(std::abs(c.events[1].value - (64.0 / 127.0)) < 1e-12,
          "and at 7-bit resolution, which is all the wire carried");

    // Now the LSB completing the pair. 74 + 32 = 106.
    feed(p, c, {0xB1, 106, 1});
    check(c.events.size() == 3, "the LSB produces a 14-bit event");
    if (c.events.size() < 3) return;
    const double v14 = c.events[2].value;
    const double expect = ((64.0 * 128.0) + 1.0) / 16383.0;
    check(std::abs(v14 - expect) < 1e-12,
          "assembled as (MSB << 7) | LSB over 16383, saw " + std::to_string(v14));
    check(v14 != c.events[1].value, "and it differs from the 7-bit value");

    // An LSB with no MSB ever seen must NOT be promoted: that would invent
    // precision out of a value the wire did not send.
    MpeParser p2;
    p2.setZone(MpeZone::lower());
    Collect c2;
    feed(p2, c2, {0x91, 60, 100});
    feed(p2, c2, {0xB1, 106, 99});
    check(c2.events.size() == 1, "a lone LSB emits nothing -- no MSB, no 14-bit value");
}

// --- identity -----------------------------------------------------------------

void testNoteIdIsIdentityAndChannelIsNot() {
    section("ADR-0054 -- the note id is identity; the channel is transport");

    MpeParser p;
    p.setZone(MpeZone::lower());
    Collect c;

    // The same key on two member channels is TWO notes. A reader that
    // reconstructed identity from the channel, or from the key, would merge
    // them -- which is the thing the ADR forbids.
    feed(p, c, {0x91, 60, 100});           // ch2
    feed(p, c, {0x92, 60, 100});           // ch3, same key
    check(c.events.size() == 2, "two note-ons");
    if (c.events.size() < 2) return;
    check(c.events[0].noteId != c.events[1].noteId,
          "the same key on two channels gets two distinct note ids");
    check(c.events[0].noteId != 0 && c.events[1].noteId != 0,
          "and neither is 0, which means unassigned");

    // Expression on ch3 must anchor to ch3's note, not ch2's.
    feed(p, c, {0xE2, 0x00, 0x60});
    check(c.events.size() == 3, "the bend arrived");
    if (c.events.size() < 3) return;
    check(c.events[2].noteId == c.events[1].noteId,
          "expression anchors to the note on ITS channel");
    check(c.events[2].noteId != c.events[0].noteId, "and not to the other one");

    // The check that actually separates a MINTED id from one derived from the
    // channel. Two notes on the SAME channel, one after the other, must get
    // different ids -- a channel-derived id gives them the same one, and the
    // "two channels, two ids" check above passes happily for such an id.
    // Found by planting exactly that defect and watching only one unrelated
    // check fail.
    Collect same;
    feed(p, same, {0x91, 64, 100});
    feed(p, same, {0x81, 64, 0});
    feed(p, same, {0x91, 64, 100});
    check(same.events.size() == 3, "on, off, on");
    if (same.events.size() == 3)
        check(same.events[0].noteId != same.events[2].noteId,
              "the same key played twice on ONE channel gets two ids -- "
              "identity is minted, never derived from the channel");

    // Note ids are never reused, even across a reset. Expression already
    // queued against an old id would otherwise land on a new note, which is
    // an audible wrong note rather than a dropped one.
    const std::uint64_t last = c.events[1].noteId;
    p.reset();
    Collect c3;
    feed(p, c3, {0x91, 60, 100});
    check(!c3.events.empty() && c3.events[0].noteId > last,
          "a note id is never reused after a reset");
}

void testVelocityZeroIsANoteOff() {
    section("a note-on with velocity 0 is a note off");
    MpeParser p;
    p.setZone(MpeZone::lower());
    Collect c;
    feed(p, c, {0x91, 60, 100});
    feed(p, c, {0x91, 60, 0});             // the classic stuck-note case
    check(c.events.size() == 2, "two events");
    if (c.events.size() < 2) return;
    check(c.events[1].type == EventType::NoteOff, "the second is a note OFF");
    check(c.events[1].noteId == c.events[0].noteId, "anchored to the note it ends");
    check(p.activeNoteOn(2) == 0, "and the channel is free again");

    // A note-off for a key we are not holding gets no id rather than a wrong one.
    Collect c2;
    feed(p, c2, {0x81, 72, 0});
    check(!c2.events.empty() && c2.events[0].noteId == 0,
          "an unmatched note-off carries no note id");
}

void testPressureAndMasterChannel() {
    section("pressure, and the master channel is not a member");
    MpeParser p;
    p.setZone(MpeZone::lower());
    Collect c;
    feed(p, c, {0x91, 60, 100});
    feed(p, c, {0xD1, 64});                // channel pressure on a member
    check(c.events.size() == 2, "pressure arrived");
    if (c.events.size() < 2) return;
    check(c.events[1].dim == static_cast<std::uint16_t>(ExpressionDim::Pressure),
          "as the Pressure dimension");
    check(std::abs(c.events[1].value - (64.0 / 127.0)) < 1e-12, "scaled to 0..1");
    check(c.events[1].noteId == c.events[0].noteId, "anchored to the note");

    // Channel 1 is the master, not a member: its messages are global and
    // carry no note id.
    Collect c2;
    feed(p, c2, {0xD0, 64});
    check(!c2.events.empty() && c2.events[0].noteId == 0,
          "master-channel pressure is global and anchors to nothing");
    check(!p.zone().isMember(1), "channel 1 is the master, not a member");
    check(p.zone().isMember(2) && p.zone().isMember(16), "2..16 are members");
}

void testNoZoneStillWorks() {
    section("plain MIDI with no zone configured still produces notes");
    MpeParser p;                            // no setZone
    Collect c;
    feed(p, c, {0x90, 60, 100});
    feed(p, c, {0xE0, 0x00, 0x60});
    check(c.events.size() == 2, "a note and a bend");
    if (c.events.size() < 2) return;
    check(c.events[0].noteId != 0, "the note still gets an id");
    check(c.events[1].noteId == 0,
          "but the bend is global -- with no zone there is no per-note pitch");
}

void testGarbageIsRefused() {
    section("malformed input emits nothing rather than guessing");
    MpeParser p;
    p.setZone(MpeZone::lower());
    Collect c;
    feed(p, c, {0x40, 60, 100});           // a data byte as status
    feed(p, c, {0xF8});                     // realtime clock
    feed(p, c, {0x91});                     // truncated note on
    feed(p, c, {0x91, 60});                 // still truncated
    check(c.events.empty(), "nothing was emitted for four malformed messages");

    std::uint8_t one = 0x90;
    check(p.feed(&one, 1, &Collect::sink, &c) == 0, "a one-byte message emits nothing");
    check(p.feed(nullptr, 3, &Collect::sink, &c) == 0, "and a null pointer does not crash");
}

void testThroughputAtTheMandatedRate() {
    section("ADR-0054's arithmetic: 500 Hz x 3 dims x polyphony");

    // The numbers ADR-0056 derived, driven through the parser rather than
    // asserted: at 4096 frames and 48 kHz a 500 Hz stream is 42.7 update
    // frames per block, and three dimensions per note makes 128 events per
    // note per block.
    MpeParser p;
    p.setZone(MpeZone::lower());
    Collect c;
    const int poly = 10;
    for (int i = 0; i < poly; ++i)
        feed(p, c, {0x91 + i, 60 + i, 100});
    const std::size_t afterNotes = c.events.size();
    check(afterNotes == static_cast<std::size_t>(poly), "ten notes on ten channels");

    const int updateFrames = 43;            // 500 Hz across 4096 @ 48 kHz
    for (int f = 0; f < updateFrames; ++f)
        for (int i = 0; i < poly; ++i) {
            feed(p, c, {0xE1 + i, f & 0x7F, 0x40});                 // pitch
            feed(p, c, {0xD1 + i, static_cast<std::uint8_t>(f)});   // pressure
            feed(p, c, {0xB1 + i, 74, static_cast<std::uint8_t>(f)});
            feed(p, c, {0xB1 + i, 106, static_cast<std::uint8_t>(f)});
        }
    const std::size_t expr = c.events.size() - afterNotes;
    // 3 dims per note per frame: pitch, pressure, and one timbre event per
    // completed 14-bit pair (the bare MSB also emits, so 4 per dim-set).
    check(expr == static_cast<std::size_t>(updateFrames * poly * 4),
          "every update survived, saw " + std::to_string(expr));
    check(p.sinkRejections() == 0, "and nothing was dropped");
    check(p.notesStarted() == poly, "ten notes started");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_mpe_tests -- ADR-0054, the input parser\n\n");
    testTheTypeItself();
    testFourteenBitBendSurvives();
    testBendRangeAndCentre();
    testFourteenBitTimbre();
    testNoteIdIsIdentityAndChannelIsNot();
    testVelocityZeroIsANoteOff();
    testPressureAndMasterChannel();
    testNoZoneStillWorks();
    testGarbageIsRefused();
    testThroughputAtTheMandatedRate();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
