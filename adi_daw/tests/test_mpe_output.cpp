// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0097: MPE+ out through VST3, three routes, and the one rule all of them
// keep -- the controller's channel never reaches a plugin.
//
// The failure this suite exists for is the silent one. Every route produces
// well-formed events; the wrong route, or a route with one wrong number,
// produces a plugin that plays -- just flat, or in the wrong part, or with
// every note bending when one does.

#include "adi/engine/mpe_input.hpp"
#include "adi/engine/mpe_output.hpp"

#include <cmath>
#include <cstdio>
#include <string>
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

Event on(std::int32_t frame, std::uint64_t id, int key, int chan = 2, double vel = 0.8) {
    Event e;
    e.type = EventType::NoteOn;
    e.frame = frame;
    e.noteId = id;
    e.dim = static_cast<std::uint16_t>(key);
    e.channel = static_cast<std::uint8_t>(chan);
    e.value = vel;
    return e;
}

Event off(std::int32_t frame, std::uint64_t id, int key, int chan = 2) {
    Event e = on(frame, id, key, chan, 0.0);
    e.type = EventType::NoteOff;
    return e;
}

Event expr(std::int32_t frame, std::uint64_t id, ExpressionDim d, double v, int chan = 2) {
    Event e;
    e.type = EventType::NoteExpression;
    e.frame = frame;
    e.noteId = id;
    e.dim = static_cast<std::uint16_t>(d);
    e.channel = static_cast<std::uint8_t>(chan);
    e.value = v;
    return e;
}

/// Storage plus the list over it.
struct Out {
    std::vector<MpeOut> store;
    MpeOutList list;
    explicit Out(int cap = 1024) : store(static_cast<std::size_t>(cap)),
                                   list(store.data(), cap) {}
    [[nodiscard]] std::vector<MpeOut> all() const { return {list.begin(), list.end()}; }
};

void route(MpeRouter& r, const std::vector<Event>& ev, Out& o, std::int32_t start = 0) {
    r.route(ev.data(), static_cast<std::int32_t>(ev.size()), start, o.list);
}

int countKind(const std::vector<MpeOut>& v, MpeOut::Kind k) {
    int n = 0;
    for (const auto& o : v) if (o.kind == k) ++n;
    return n;
}

const MpeOut* firstNoteOn(const std::vector<MpeOut>& v, std::uint64_t id) {
    for (const auto& o : v) if (o.kind == MpeOut::Kind::NoteOn && o.noteId == id) return &o;
    return nullptr;
}

/// The Control events for one channel and controller, in order.
std::vector<MpeOut> controls(const std::vector<MpeOut>& v, int chan, int ctrl) {
    std::vector<MpeOut> r;
    for (const auto& o : v)
        if (o.kind == MpeOut::Kind::Control && o.channel == chan && o.ctrl == ctrl) r.push_back(o);
    return r;
}

ExpressionCaps reachable() {
    ExpressionCaps c;
    c.controllerReachable = true;
    return c;
}

// ---------------------------------------------------------------------------

void testBendEncodingInvertsTheParser() {
    section("ADR-0097 -- the bend encoder is the exact inverse of the input parser");

    // Every one of the 16384 words, decoded by the parser and re-encoded. A
    // symmetric encoder (8192 both ways) survives the three spot checks below
    // and fails here: the upper half comes back one word high.
    int wrong = 0, firstWrong = -1;
    for (int w = 0; w < 16384; ++w) {
        const double s = bendToSemitones(static_cast<std::uint16_t>(w), 48.0);
        if (semitonesToBendWord(s, 48.0) != w) {
            ++wrong;
            if (firstWrong < 0) firstWrong = w;
        }
    }
    check(wrong == 0, "all 16384 words survive decode -> encode, saw " + std::to_string(wrong) +
                          " wrong, first at " + std::to_string(firstWrong));

    check(semitonesToBendWord(0.0, 48.0) == 8192, "no bend is the centre word, 8192");
    check(semitonesToBendWord(48.0, 48.0) == 16383, "+range reaches the top word");
    check(semitonesToBendWord(-48.0, 48.0) == 0, "-range reaches the bottom word");
    check(semitonesToBendWord(60.0, 48.0) == 16383 && semitonesToBendWord(-60.0, 48.0) == 0,
          "beyond the range clamps rather than wrapping");
    check(semitonesToBendWord(std::nan(""), 48.0) == 8192 && semitonesToBendWord(3.0, 0.0) == 8192,
          "a NaN or a zero range is no bend, not undefined behaviour");
    check(std::abs(semitonesToBendExact(12.0, 48.0) - 10239.75) < 1e-9,
          "the exact form is unrounded: +12 of 48 is 8192 + 8191/4 = 10239.75");
}

void testRouteResolution() {
    section("ADR-0097 -- which route a plugin gets");

    ExpressionCaps unknown;
    check(resolveRoute(RouteChoice::Auto, unknown) == ExpressionRoute::NoteExpression,
          "controller unreachable -> NoteExpression, VST3's own model");

    ExpressionCaps ne = reachable();
    ne.noteExpression = true;
    check(resolveRoute(RouteChoice::Auto, ne) == ExpressionRoute::NoteExpression,
          "a plugin that takes note expression gets it");

    ExpressionCaps mpe = reachable();
    for (std::uint32_t c = 0; c < 16; ++c) mpe.bendParam[c] = 1000 + c;
    check(mpe.perChannelBend() == 16, "16 channels, 16 distinct bend parameters");
    check(resolveRoute(RouteChoice::Auto, mpe) == ExpressionRoute::MpeMidi,
          "bend mapped per channel -> MpeMidi");

    ExpressionCaps both = mpe;
    both.noteExpression = true;
    check(resolveRoute(RouteChoice::Auto, both) == ExpressionRoute::NoteExpression,
          "both declared -> NoteExpression: doubles, no channel juggling");

    ExpressionCaps global = reachable();
    global.bendParam[0] = 7;
    check(resolveRoute(RouteChoice::Auto, global) == ExpressionRoute::Plain,
          "bend on channel 0 only is a GLOBAL bend -> Plain");

    ExpressionCaps oneParam = reachable();
    oneParam.bendParam.fill(7);
    check(oneParam.perChannelBend() == 1, "every channel onto one parameter counts once");
    check(resolveRoute(RouteChoice::Auto, oneParam) == ExpressionRoute::Plain,
          "and is a global bend too -- per-note bend would move every note");

    check(resolveRoute(RouteChoice::Auto, reachable()) == ExpressionRoute::Plain,
          "a controller that declares nothing -> Plain");

    check(resolveRoute(RouteChoice::MpeMidi, unknown) == ExpressionRoute::MpeMidi &&
              resolveRoute(RouteChoice::Plain, ne) == ExpressionRoute::Plain &&
              resolveRoute(RouteChoice::NoteExpression, mpe) == ExpressionRoute::NoteExpression,
          "an explicit choice always wins over what was detected");
}

void testTheControllersChannelNeverReachesThePlugin() {
    section("ADR-0097 -- backward compatibility: the controller's channel is transport");

    for (ExpressionRoute rt : {ExpressionRoute::NoteExpression, ExpressionRoute::Plain}) {
        MpeRouter r;
        r.configure(rt, ExpressionCaps{});
        Out o;
        std::vector<Event> ev;
        for (int ch = 1; ch <= 16; ++ch)
            ev.push_back(on(0, static_cast<std::uint64_t>(ch), 40 + ch, ch));
        route(r, ev, o);
        bool allZero = true;
        for (const auto& x : o.all())
            if (x.kind == MpeOut::Kind::NoteOn && x.channel != 0) allZero = false;
        check(allZero && countKind(o.all(), MpeOut::Kind::NoteOn) == 16,
              std::string(rt == ExpressionRoute::Plain ? "Plain" : "NoteExpression") +
                  ": notes from channels 1..16 ALL go out on channel 0 -- a plugin listening "
                  "on channel 1 hears every one");
    }

    // MpeMidi chooses its own channels: three notes that all arrived on the
    // controller's channel 2 get three different member channels.
    MpeRouter r;
    r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{});
    Out o;
    route(r, {on(0, 1, 60, 2), on(0, 2, 64, 2), on(0, 3, 67, 2)}, o);
    const auto v = o.all();
    const MpeOut* a = firstNoteOn(v, 1);
    const MpeOut* b = firstNoteOn(v, 2);
    const MpeOut* c = firstNoteOn(v, 3);
    check(a && b && c, "all three notes go out");
    if (a && b && c) {
        check(a->channel != b->channel && b->channel != c->channel && a->channel != c->channel,
              "on three DIFFERENT member channels, though all arrived on channel 2");
        check(a->channel != 0 && b->channel != 0 && c->channel != 0,
              "and never on channel 0, the master");
    }
}

void testNoteExpressionRoute() {
    section("ADR-0097 -- NoteExpression: doubles, anchored to the note id");

    MpeRouter r;
    r.configure(ExpressionRoute::NoteExpression, ExpressionCaps{});
    Out o;
    route(r, {on(0, 7, 60, 5, 1.4), expr(0, 7, ExpressionDim::Pitch, 12.0),
              expr(0, 7, ExpressionDim::Pressure, 0.3), expr(0, 7, ExpressionDim::Timbre, 0.6),
              expr(0, 7, ExpressionDim::Gain, 0.9), expr(0, 7, ExpressionDim::Pan, 0.2),
              off(10, 7, 60, 5)}, o);
    const auto v = o.all();

    check(countKind(v, MpeOut::Kind::Control) == 0, "no channel messages and no MCM at all");
    const MpeOut* n = firstNoteOn(v, 7);
    check(n != nullptr && n->key == 60 && n->value == 1.0,
          "the note keeps its key, and a velocity above 1 is clamped to VST3's 0..1");

    auto typeOf = [&](std::size_t i) { return i < v.size() ? v[i].exprType : 0xDEADu; };
    check(v.size() == 7, "one output per input, saw " + std::to_string(v.size()));
    check(typeOf(1) == static_cast<std::uint32_t>(Vst3NoteExprType::Tuning) &&
              std::abs(v[1].value - (12.0 / 240.0 + 0.5)) < 1e-15,
          "pitch -> Tuning, by the SDK's own formula, as a double");
    check(typeOf(2) == static_cast<std::uint32_t>(Vst3NoteExprType::Expression) &&
              typeOf(3) == static_cast<std::uint32_t>(Vst3NoteExprType::Brightness) &&
              typeOf(4) == static_cast<std::uint32_t>(Vst3NoteExprType::Volume) &&
              typeOf(5) == static_cast<std::uint32_t>(Vst3NoteExprType::Pan),
          "pressure, timbre, gain and pan -> Expression, Brightness, Volume, Pan");
    check(v.size() == 7 && v[6].kind == MpeOut::Kind::NoteOff && v[6].channel == 0,
          "and the note ends on channel 0 too");
    check(v.size() == 7 && v[1].dim == static_cast<std::uint16_t>(ExpressionDim::Pitch) &&
              v[1].plain == 12.0 && v[2].plain == 0.3,
          "each expression also carries its dimension and the engine's plain value -- "
          "semitones for CLAP, which must not be converted back from VST3's scale");
    check(v.size() == 7 && v[1].key == 60,
          "and the key of the note it addresses, for CLAP's (channel, key, id) address");

    Out oUnknown;
    route(r, {expr(20, 999, ExpressionDim::Pitch, 1.0)}, oUnknown);
    check(oUnknown.list.size() == 1 && oUnknown.list.at(0).key == -1,
          "an expression for a note the router is not tracking says key -1, not 0");

    // A plugin that answered the physical-UI mapping gets ITS types.
    ExpressionCaps pui = reachable();
    pui.noteExpression = true;
    pui.pitchType = 100001;
    pui.timbreType = 100002;
    pui.pressureType = 100003;
    MpeRouter r2;
    r2.configure(ExpressionRoute::NoteExpression, pui);
    Out o2;
    route(r2, {on(0, 1, 60), expr(0, 1, ExpressionDim::Pitch, 12.0),
               expr(0, 1, ExpressionDim::Timbre, 0.6), expr(0, 1, ExpressionDim::Pressure, 0.3)}, o2);
    const auto w = o2.all();
    check(w.size() == 4 && w[1].exprType == 100001 && w[2].exprType == 100002 &&
              w[3].exprType == 100003,
          "X, Y and pressure go to the types the plugin named");
    check(w.size() == 4 && std::abs(w[1].value - 0.625) < 1e-15,
          "and X, not being Tuning, is VST3's physical X: 0.5 + 12/96 = 0.625");

    // A plugin-defined dimension has no VST3 type to name: dropped, counted.
    MpeRouter r3;
    r3.configure(ExpressionRoute::NoteExpression, ExpressionCaps{});
    Out o3;
    Event custom = expr(0, 1, ExpressionDim::Pitch, 0.5);
    custom.dim = 64;
    route(r3, {custom}, o3);
    check(o3.list.size() == 0 && r3.dropped(static_cast<ExpressionDim>(64)) == 1,
          "a dimension >= 64 is dropped and counted, not sent as something else");

    MpeOut single;
    Event pv;
    pv.type = EventType::ParamValue;
    check(!noteExpressionOut(pv, ExpressionCaps{}, single),
          "a parameter event has no event form -- it travels in IParameterChanges");

    // The single-event form `Vst3EventList::add` uses, which is a separate
    // path from the router's and must keep the same rule.
    bool zero = true;
    for (int ch = 1; ch <= 16; ++ch) {
        MpeOut a, b;
        noteExpressionOut(on(0, 1, 60, ch), ExpressionCaps{}, a);
        noteExpressionOut(off(0, 1, 60, ch), ExpressionCaps{}, b);
        if (a.channel != 0 || b.channel != 0 || a.kind != MpeOut::Kind::NoteOn ||
            b.kind != MpeOut::Kind::NoteOff)
            zero = false;
    }
    check(zero, "noteExpressionOut puts note-ons and note-offs from every channel on channel 0");
}

void testMpeMidiRoute() {
    section("ADR-0097 -- MpeMidi: member channels, reset before every note");

    MpeRouter r;
    r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{});
    Out o;
    route(r, {on(256, 1, 60)}, o, 256);
    auto v = o.all();

    // The Configuration Message first, on the master channel.
    check(v.size() >= 3 && v[0].kind == MpeOut::Kind::Control && v[0].channel == 0 &&
              v[0].ctrl == 101 && v[0].word == 0 && v[1].ctrl == 100 && v[1].word == 6 &&
              v[2].ctrl == 6 && v[2].word == 15,
          "the MPE Configuration Message comes first: RPN 6 = 15 members, on channel 0");
    check(v.size() >= 3 && v[0].frame == 256 && v[2].frame == 256,
          "at the start of the segment, before any note");

    const MpeOut* n = firstNoteOn(v, 1);
    check(n != nullptr && n->channel == 1, "the first note gets member channel 1");
    const int ch = n != nullptr ? n->channel : 1;
    // Reset before the note: bend centre, timbre neutral, pressure zero -- all
    // at the note's frame, all BEFORE it in the list.
    std::size_t noteAt = 0;
    for (std::size_t i = 0; i < v.size(); ++i) if (&v[i] == n) noteAt = i;
    const auto bend = controls(v, ch, kCtrlPitchBend);
    const auto tim = controls(v, ch, kCtrlTimbre);
    const auto prs = controls(v, ch, kCtrlAfterTouch);
    check(bend.size() == 1 && bend[0].word == 8192, "its bend is reset to centre");
    check(tim.size() == 1 && tim[0].word == 64, "its timbre to MPE's neutral 64");
    check(prs.size() == 1 && prs[0].word == 0, "its pressure to zero");
    check(noteAt == 6, "and all three come BEFORE the note-on, saw it at " +
                           std::to_string(noteAt));

    // The MCM is sent once, not per segment.
    Out o2;
    route(r, {}, o2);
    check(o2.list.size() == 0, "the MCM is not repeated on the next segment");

    // A note whose starting values arrive at the same instant starts WITH
    // them: MPE sends initial bend and timbre ahead of the note-on.
    Out o3;
    route(r, {on(10, 2, 64), expr(10, 2, ExpressionDim::Pitch, 2.0),
              expr(10, 2, ExpressionDim::Timbre, 0.8)}, o3);
    v = o3.all();
    const MpeOut* n2 = firstNoteOn(v, 2);
    check(n2 != nullptr && n2->channel == 2, "the second note gets channel 2");
    const int ch2 = n2 != nullptr ? n2->channel : 2;
    const auto b2 = controls(v, ch2, kCtrlPitchBend);
    const auto t2 = controls(v, ch2, kCtrlTimbre);
    check(!b2.empty() && b2[0].word == semitonesToBendWord(2.0, 48.0),
          "its first bend is its OWN starting bend, not centre");
    check(!t2.empty() && t2[0].word == 102, "and its first timbre is its own: 0.8 -> 102");
    check(!v.empty() && v[0].kind == MpeOut::Kind::Control, "both ahead of the note-on");

    // Expression follows the note to its channel.
    Out o4;
    route(r, {expr(20, 1, ExpressionDim::Pitch, -48.0), expr(20, 1, ExpressionDim::Pressure, 1.0),
              expr(20, 1, ExpressionDim::Timbre, 0.25), expr(20, 1, ExpressionDim::Gain, 0.5),
              expr(20, 1, ExpressionDim::Pan, 0.5)}, o4);
    v = o4.all();
    check(v.size() == 3, "pitch, pressure and timbre go out; gain and pan cannot");
    check(v.size() == 3 && v[0].channel == 1 && v[0].ctrl == kCtrlPitchBend && v[0].word == 0,
          "-48 bends note 1's channel to word 0");
    check(v.size() == 3 && v[1].ctrl == kCtrlAfterTouch && v[1].word == 127 && v[1].channel == 1,
          "pressure is channel pressure on the same channel");
    check(v.size() == 3 && v[2].ctrl == kCtrlTimbre && v[2].word == 32 && v[2].channel == 1,
          "timbre is CC74 on the same channel");
    check(r.dropped(ExpressionDim::Gain) == 1 && r.dropped(ExpressionDim::Pan) == 1,
          "and gain and pan are COUNTED, not silently lost");
    check(countKind(v, MpeOut::Kind::Expression) == 0,
          "no note-expression events on this route -- JUCE's client would drop them");

    // Ends on the channel it started on, and the channel is freed.
    Out o5;
    route(r, {off(30, 1, 60, 9)}, o5);
    v = o5.all();
    check(v.size() == 1 && v[0].kind == MpeOut::Kind::NoteOff && v[0].channel == 1 && v[0].key == 60,
          "the note-off goes to the note's member channel, whatever channel it arrived on");
    check(r.channelOf(1) == -1 && r.liveNotes() == 1, "and the note is forgotten");

    Out o6;
    route(r, {expr(40, 99, ExpressionDim::Pitch, 1.0), off(40, 98, 50)}, o6);
    check(o6.list.size() == 0 && r.unknownNotes() == 2,
          "expression and a note-off for notes that are not sounding go nowhere, counted");
}

void testMemberChannelAllocation() {
    section("ADR-0097 -- allocation: least recently released, and share rather than cut");

    MpeRouter r;
    r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{}, 3);
    Out o;
    route(r, {on(0, 1, 60), on(0, 2, 62), on(0, 3, 64)}, o);
    check(o.list.size() == 3 + 3 * 4, "MCM, then three notes of four events each");
    check(o.list.size() > 2 && o.list.at(2).word == 3, "the MCM names 3 member channels");
    check(r.channelOf(1) == 1 && r.channelOf(2) == 2 && r.channelOf(3) == 3,
          "three notes, channels 1, 2, 3");

    Out o2;
    route(r, {off(1, 2, 62), off(2, 1, 60)}, o2);        // 2 released first, then 1
    Out o3;
    route(r, {on(3, 4, 67)}, o3);
    check(r.channelOf(4) == 2,
          "a new note takes the channel released LONGEST ago (2), not the latest (1) -- "
          "which may still be ringing out; saw " + std::to_string(r.channelOf(4)));

    // Full: 1 free channel left (1). Fill it, then one more.
    Out o4;
    route(r, {on(4, 5, 69)}, o4);
    check(r.channelOf(5) == 1, "the last free channel is used");
    Out o5;
    route(r, {on(5, 6, 71)}, o5);
    const auto v = o5.all();
    check(r.sharedChannels() == 1, "a fourth note on three channels SHARES one, counted");
    check(r.channelOf(6) == 3,
          "the channel whose note began longest ago (note 3, channel 3); saw " +
              std::to_string(r.channelOf(6)));
    check(countKind(v, MpeOut::Kind::NoteOff) == 0,
          "and no note is cut off to make room -- the player is still holding it");
    check(r.liveNotes() == 4, "four notes sounding on three channels");
}

void testParameterPath() {
    section("ADR-0097 -- MpeMidi through IMidiMapping: parameters, unquantised");

    ExpressionCaps caps = reachable();
    for (std::uint32_t c = 0; c < 16; ++c) {
        caps.bendParam[c] = 1000 + c;
        caps.pressureParam[c] = 2000 + c;
        caps.timbreParam[c] = 3000 + c;
    }
    caps.rpnParam = {500, 501, 502};

    MpeRouter r;
    r.configure(ExpressionRoute::MpeMidi, caps);
    Out o;
    route(r, {on(0, 1, 60), expr(5, 1, ExpressionDim::Pitch, 12.0),
              expr(5, 1, ExpressionDim::Pressure, 0.3337)}, o);
    const auto v = o.all();

    check(v.size() >= 3 && v[0].paramId == 500 && v[1].paramId == 501 && v[2].paramId == 502,
          "the MCM lands on the parameters CC101/100/6 are mapped to");
    check(v.size() >= 3 && std::abs(v[2].value - 15.0 / 127.0) < 1e-15,
          "carrying word/127, which a 7-bit decoder turns back into 15 exactly");

    const auto bend = controls(v, 1, kCtrlPitchBend);
    check(bend.size() == 2 && bend[0].paramId == 1001, "channel 1's bend goes to ITS parameter");
    check(bend.size() == 2 && std::abs(bend[0].value * 16383.0 - 8192.0) < 1e-9,
          "the reset is word 8192 over 16383");
    check(bend.size() == 2 && std::abs(bend[1].value * 16383.0 - 10239.75) < 1e-9 &&
              bend[1].word == 10240,
          "+12 semitones goes as the UNROUNDED word 10239.75 over 16383, to a double -- "
          "the wire word 10240 is only for the legacy event");
    const auto prs = controls(v, 1, kCtrlAfterTouch);
    check(prs.size() == 2 && prs[1].paramId == 2001 && prs[1].value == 0.3337,
          "pressure reaches its parameter as the double it was, not as n/127");

    // Why 16383 and not 16384: JUCE's VST3 client decodes a bend parameter as
    // the integer part of value x 16384, and a CC as the integer part of
    // value x 128. Over 16383 and 127, every word comes back exactly.
    int bendMiss = 0, ccMiss = 0;
    for (int w = 0; w < 16384; ++w) {
        int back = static_cast<int>(static_cast<double>(w) / 16383.0 * 16384.0);
        if (back > 16383) back = 16383;
        if (back != w) ++bendMiss;
    }
    for (int w = 0; w < 128; ++w) {
        int back = static_cast<int>(static_cast<double>(w) / 127.0 * 128.0);
        if (back > 127) back = 127;
        if (back != w) ++ccMiss;
    }
    check(bendMiss == 0 && ccMiss == 0,
          "every bend word and every CC value survives JUCE's decoder, missed " +
              std::to_string(bendMiss) + " and " + std::to_string(ccMiss));

    // Unmapped channels fall back to the legacy event, per message.
    MpeRouter r2;
    r2.configure(ExpressionRoute::MpeMidi, ExpressionCaps{});
    Out o2;
    route(r2, {on(0, 1, 60)}, o2);
    bool allLegacy = true;
    for (const auto& x : o2.all())
        if (x.kind == MpeOut::Kind::Control && x.paramId != kNoParam) allLegacy = false;
    check(allLegacy, "with no mapping every channel message is a legacy event");
}

void testPlainRoute() {
    section("ADR-0097 -- Plain: standard MIDI, poly aftertouch, the rest counted");

    MpeRouter r;
    r.configure(ExpressionRoute::Plain, ExpressionCaps{});
    Out o;
    route(r, {on(0, 1, 60, 7), expr(1, 1, ExpressionDim::Pressure, 0.5),
              expr(1, 1, ExpressionDim::Pitch, 3.0), expr(1, 1, ExpressionDim::Timbre, 0.2),
              expr(1, 1, ExpressionDim::Gain, 0.2), expr(1, 1, ExpressionDim::Pan, 0.2)}, o);
    const auto v = o.all();
    check(v.size() == 2, "the note and its pressure, nothing else; saw " + std::to_string(v.size()));
    check(v.size() == 2 && v[1].kind == MpeOut::Kind::PolyPressure && v[1].channel == 0 &&
              v[1].key == 60 && v[1].noteId == 1 && v[1].value == 0.5,
          "pressure is POLY aftertouch on the note's key -- per note, as MIDI 1.0 allows");
    check(r.dropped(ExpressionDim::Pitch) == 1 && r.dropped(ExpressionDim::Timbre) == 1 &&
              r.dropped(ExpressionDim::Gain) == 1 && r.dropped(ExpressionDim::Pan) == 1,
          "pitch, timbre, gain and pan have no per-note MIDI 1.0 form: counted");
    check(countKind(v, MpeOut::Kind::Control) == 0,
          "no channel bend: on one channel it would bend EVERY note");

    Out o2;
    route(r, {on(2, 2, 60, 8)}, o2);
    check(r.keyCollisions() == 1,
          "a second finger on the same key, collapsed onto channel 0, is counted");
}

void testSwitchingRoutesEndsNotesWhereTheyStarted() {
    section("ADR-0097 -- switching route ends every note on its own channel first");

    MpeRouter r;
    r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{});
    Out o;
    route(r, {on(0, 1, 60), on(0, 2, 64)}, o);

    Out o2;
    r.switchTo(ExpressionRoute::Plain, 17, o2.list);
    const auto v = o2.all();
    check(v.size() == 2 && v[0].kind == MpeOut::Kind::NoteOff && v[1].kind == MpeOut::Kind::NoteOff,
          "two note-offs");
    bool right = v.size() == 2;
    for (const auto& x : v) {
        if (x.noteId == 1 && (x.channel != 1 || x.key != 60)) right = false;
        if (x.noteId == 2 && (x.channel != 2 || x.key != 64)) right = false;
        if (x.frame != 17) right = false;
    }
    check(right, "each on the MEMBER channel and key its note began on, at the switch frame");
    check(r.current() == ExpressionRoute::Plain && r.liveNotes() == 0, "then the route changes");

    Out o3;
    route(r, {off(20, 1, 60)}, o3);
    check(o3.list.size() == 0 && r.unknownNotes() == 1,
          "the note's own off, arriving later, is not sent a second time on channel 0");

    Out o4;
    r.switchTo(ExpressionRoute::Plain, 0, o4.list);
    check(o4.list.size() == 0, "switching to the route already in use does nothing");

    Out o5;
    r.switchTo(ExpressionRoute::MpeMidi, 0, o5.list);
    route(r, {}, o5);
    check(o5.list.size() == 3 && o5.list.at(0).ctrl == 101,
          "switching INTO MpeMidi sends the Configuration Message again");

    r.reset();
    Out o6;
    route(r, {}, o6);
    check(o6.list.size() == 3, "and so does a reset: a re-activated plugin has forgotten it");
}

void testUnassignedIdsAndCapacity() {
    section("ADR-0097 -- unassigned note ids, and every limit counted");

    MpeRouter r;
    r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{});
    Out o;
    route(r, {on(0, 0, 60), on(0, 0, 62)}, o);
    Out o2;
    route(r, {off(5, 0, 62)}, o2);
    const auto v = o2.all();
    check(v.size() == 1 && v[0].channel == 2 && v[0].key == 62,
          "notes with id 0 are matched by key: 62 ends on ITS channel, 2");
    Out o3;
    route(r, {expr(6, 0, ExpressionDim::Pitch, 1.0)}, o3);
    check(o3.list.size() == 0 && r.unknownNotes() == 1,
          "expression needs an id to find its note; with 0 it is counted, not guessed");

    MpeRouter small;
    small.configure(ExpressionRoute::MpeMidi, ExpressionCaps{});
    Out tiny(2);
    route(small, {on(0, 1, 60)}, tiny);
    check(tiny.list.size() == 2 && tiny.list.dropped() == 5,
          "an output list that is too small counts what did not fit (7 wanted, 2 fit)");

    MpeRouter full;
    full.configure(ExpressionRoute::Plain, ExpressionCaps{});
    std::vector<Event> many;
    for (int i = 1; i <= MpeRouter::kMaxNotes + 1; ++i)
        many.push_back(on(0, static_cast<std::uint64_t>(i), i % 128));
    Out big(512);
    route(full, many, big);
    check(full.tableFull() == 1 && full.liveNotes() == MpeRouter::kMaxNotes,
          "the note beyond the table is refused and counted");
    check(countKind(big.all(), MpeOut::Kind::NoteOn) == MpeRouter::kMaxNotes,
          "and NOT sent -- untracked, its note-off would have nowhere to go");
}

void testFullOutputListPreservesState() {
    section("DF-E56 -- a full output list never mutates MpeRouter state");

    // 1. MpeMidi MCM: partial or zero capacity keeps mcmPending_ until all 3 controls push.
    {
        MpeRouter r;
        r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{}, 15);
        Out none(0);
        route(r, {}, none, 0);
        check(none.list.size() == 0 && none.list.dropped() == 3,
              "MCM with zero capacity drops all 3 controls");

        Out partial(2);
        route(r, {}, partial, 16);
        check(partial.list.size() == 2 && partial.list.dropped() == 1,
              "MCM with capacity 2 pushes 2 and drops 1");

        Out room(16);
        route(r, {}, room, 32);
        const auto v = room.all();
        check(v.size() == 3 && room.list.dropped() == 0 &&
                  v[0].kind == MpeOut::Kind::Control && v[0].channel == 0 && v[0].ctrl == 101 && v[0].word == 0 &&
                  v[1].ctrl == 100 && v[1].word == 6 && v[2].ctrl == 6 && v[2].word == 15 && v[0].frame == 32,
              "next call with room retries and emits the complete 3-message MCM");

        Out after(16);
        route(r, {}, after, 48);
        check(after.list.size() == 0, "once all 3 MCM controls push, MCM is no longer pending");
    }

    // 2. Combined MCM + note-on overflow leaves neither MCM finished nor the note live.
    {
        MpeRouter r;
        r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{}, 15);
        Out tiny(2);
        route(r, {on(0, 1, 60)}, tiny, 0);
        check(tiny.list.size() == 2 && tiny.list.dropped() == 5,
              "tiny list still reports 2 pushed and 5 dropped");
        check(r.liveNotes() == 0 && r.channelOf(1) == -1 && r.sharedChannels() == 0,
              "dropped note-on is not tracked as live");

        Out room(16);
        route(r, {on(10, 1, 60)}, room, 10);
        check(room.list.size() == 7 && room.list.dropped() == 0 &&
                  r.liveNotes() == 1 && r.channelOf(1) == 1,
              "retry with room emits full MCM + resets + note-on on channel 1");
    }

    // 3. MpeMidi note-on whose resets fit but NoteOn itself drops does not track or count shared.
    {
        MpeRouter r;
        r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{}, 1);
        Out init(16);
        route(r, {on(0, 1, 60)}, init, 0);
        check(r.liveNotes() == 1 && r.channelOf(1) == 1 && r.sharedChannels() == 0,
              "first note occupies the single member channel");

        Out onlyResets(3);
        route(r, {on(10, 2, 64)}, onlyResets, 10);
        check(onlyResets.list.size() == 3 && onlyResets.list.dropped() == 1,
              "resets fit while NoteOn overflows");
        check(r.liveNotes() == 1 && r.channelOf(2) == -1 && r.sharedChannels() == 0,
              "failed NoteOn push leaves liveNotes, channelOf and sharedChannels unchanged");

        Out room(8);
        route(r, {on(20, 2, 64)}, room, 20);
        check(room.list.size() == 4 && room.list.dropped() == 0 &&
                  r.liveNotes() == 2 && r.channelOf(2) == 1 && r.sharedChannels() == 1,
              "retrying NoteOn with room tracks the note and counts sharedChannels once");
    }

    // 4. Plain and NoteExpression note-on overflow leaves liveNotes and keyCollisions unchanged.
    {
        MpeRouter plain;
        plain.configure(ExpressionRoute::Plain, ExpressionCaps{});
        Out one(1);
        route(plain, {on(0, 1, 60), on(1, 2, 60)}, one, 0);
        check(one.list.size() == 1 && one.list.dropped() == 1,
              "Plain: first note fits, same-key second note overflows");
        check(plain.liveNotes() == 1 && plain.channelOf(1) == 0 && plain.channelOf(2) == -1 &&
                  plain.keyCollisions() == 0,
              "Plain: overflowed same-key note is not tracked and does not increment keyCollisions");

        Out room(4);
        route(plain, {on(2, 2, 60)}, room, 2);
        check(room.list.size() == 1 && plain.liveNotes() == 2 && plain.keyCollisions() == 1,
              "Plain: retry with room tracks the note and counts keyCollisions once");

        MpeRouter ne;
        ne.configure(ExpressionRoute::NoteExpression, ExpressionCaps{});
        Out empty(0);
        route(ne, {on(0, 7, 62)}, empty, 0);
        check(empty.list.dropped() == 1 && ne.liveNotes() == 0 && ne.channelOf(7) == -1,
              "NoteExpression: overflowed note-on is not tracked");
    }

    // 5. NoteOff overflow queues the NoteOff as pending, frees the note slot, and delivers it first on the next block.
    {
        MpeRouter r;
        r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{}, 2);
        Out init(32);
        route(r, {on(0, 1, 60), on(0, 2, 64)}, init, 0);
        check(r.liveNotes() == 2 && r.channelOf(1) == 1 && r.channelOf(2) == 2,
              "two live notes on channels 1 and 2");

        Out full(0);
        route(r, {off(10, 1, 60)}, full, 10);
        check(full.list.dropped() == 1 && r.liveNotes() == 1 && r.channelOf(1) == -1 && r.channelOf(2) == 2,
              "dropped NoteOff frees note 1's slot while keeping note 2 live");

        // Next block with empty input delivers the dropped NoteOff on its original channel.
        Out next(8);
        route(r, {}, next, 20);
        const auto v = next.all();
        check(v.size() == 1 && next.list.dropped() == 0 &&
                  v[0].kind == MpeOut::Kind::NoteOff && v[0].noteId == 1 &&
                  v[0].channel == 1 && v[0].key == 60 && v[0].frame == 20 &&
                  r.unknownNotes() == 0 && r.liveNotes() == 1,
              "next block delivers the dropped NoteOff on channel 1 at segmentStart");

        Out after(8);
        route(r, {}, after, 30);
        check(after.list.size() == 0, "delivered NoteOff is not sent a second time");
    }

    // 5b. 128 dropped NoteOffs do not fill the note table; next block drains all 128 and accepts 128 fresh notes.
    {
        MpeRouter r;
        r.configure(ExpressionRoute::Plain, ExpressionCaps{});
        std::vector<Event> ons, offs, fresh;
        for (int i = 1; i <= MpeRouter::kMaxNotes; ++i) {
            ons.push_back(on(0, static_cast<std::uint64_t>(i), i % 128));
            offs.push_back(off(1, static_cast<std::uint64_t>(i), i % 128));
            fresh.push_back(on(2, static_cast<std::uint64_t>(MpeRouter::kMaxNotes + i), i % 128));
        }
        Out start(256);
        route(r, ons, start, 0);
        check(r.liveNotes() == MpeRouter::kMaxNotes && r.tableFull() == 0,
              "table filled with 128 live notes");

        Out zero(0);
        route(r, offs, zero, 10);
        check(zero.list.dropped() == MpeRouter::kMaxNotes && r.liveNotes() == 0,
              "128 dropped NoteOffs release all 128 slots instead of stranding the table");

        Out nextBlock(512);
        route(r, fresh, nextBlock, 20);
        const auto v = nextBlock.all();
        check(v.size() == 2 * MpeRouter::kMaxNotes && nextBlock.list.dropped() == 0 &&
                  r.tableFull() == 0 && r.liveNotes() == MpeRouter::kMaxNotes,
              "next block delivers all 128 pending NoteOffs and admits 128 new notes with zero tableFull");
        bool offsFirst = v.size() == 2 * MpeRouter::kMaxNotes;
        for (int i = 0; offsFirst && i < MpeRouter::kMaxNotes; ++i) {
            const auto& o = v[static_cast<std::size_t>(i)];
            if (o.kind != MpeOut::Kind::NoteOff || o.frame != 20 || o.noteId != static_cast<std::uint64_t>(i + 1))
                offsFirst = false;
        }
        check(offsFirst, "all 128 pending NoteOffs go out first, in order, before any new NoteOn");
    }

    // 6. switchTo switches route at once; unpushed NoteOffs drain first on the next route() (2-block partial switch).
    {
        // Reviewer's probe: 120 live notes switched into a 104-slot list finishes within two blocks.
        MpeRouter r;
        r.configure(ExpressionRoute::Plain, ExpressionCaps{});
        std::vector<Event> notes;
        for (int i = 1; i <= 120; ++i)
            notes.push_back(on(0, static_cast<std::uint64_t>(i), i % 128));
        Out setup(256);
        route(r, notes, setup, 0);
        check(r.liveNotes() == 120 && r.current() == ExpressionRoute::Plain,
              "120 sounding notes on Plain before switchTo");

        // Block 1: switchTo into a 104-slot list switches route at once and queues the remaining 16 NoteOffs.
        Out block1(104);
        r.switchTo(ExpressionRoute::MpeMidi, 10, block1.list);
        check(block1.list.size() == 104 && block1.list.dropped() == 16 &&
                  r.current() == ExpressionRoute::MpeMidi && r.liveNotes() == 0,
              "partial switchTo pushes 104 NoteOffs, switches route immediately, and queues 16");
        // Calling route() on the same full list (as vst3_host does) preserves pending NoteOffs and MCM.
        route(r, {}, block1, 10);
        check(block1.list.size() == 104, "route() on the still-full block1 list leaves it full");

        // Block 2: fresh 104-slot list receives the remaining 16 NoteOffs FIRST, then the 3 MCM controls, then a note.
        Out block2(104);
        route(r, {on(30, 500, 60)}, block2, 20);
        const auto v2 = block2.all();
        check(v2.size() == 16 + 3 + 4 && block2.list.dropped() == 0 &&
                  r.liveNotes() == 1 && r.channelOf(500) == 1,
              "block 2 finishes all 16 pending NoteOffs, sends the 3-message MCM, and starts the new MPE note");
        bool orderOk = v2.size() == 16 + 3 + 4;
        for (int i = 0; orderOk && i < 16; ++i) {
            const auto& o = v2[static_cast<std::size_t>(i)];
            if (o.kind != MpeOut::Kind::NoteOff || o.channel != 0 || o.frame != 20 ||
                o.noteId != static_cast<std::uint64_t>(105 + i))
                orderOk = false;
        }
        if (orderOk) {
            orderOk = v2[16].kind == MpeOut::Kind::Control && v2[16].ctrl == 101 &&
                      v2[17].ctrl == 100 && v2[18].ctrl == 6 &&
                      v2[22].kind == MpeOut::Kind::NoteOn && v2[22].noteId == 500 && v2[22].channel == 1;
        }
        check(orderOk, "remaining 16 NoteOffs arrive on channel 0 ahead of MCM and new MpeMidi note");
    }

    // 6b. Partial switchTo from MpeMidi to Plain preserves original member channels across blocks, and reset clears pending.
    {
        MpeRouter r;
        r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{}, 3);
        Out init(32);
        route(r, {on(0, 1, 60), on(0, 2, 64), on(0, 3, 67)}, init, 0);

        Out one(1);
        r.switchTo(ExpressionRoute::Plain, 12, one.list);
        const auto v1 = one.all();
        check(v1.size() == 1 && one.list.dropped() == 2 &&
                  v1[0].kind == MpeOut::Kind::NoteOff && v1[0].noteId == 1 &&
                  v1[0].channel == 1 && v1[0].key == 60 && v1[0].frame == 12 &&
                  r.current() == ExpressionRoute::Plain && r.liveNotes() == 0,
              "partial MpeMidi->Plain switch emits 1 NoteOff, switches to Plain, queues remaining 2");

        Out rest(8);
        route(r, {on(20, 4, 72)}, rest, 15);
        const auto v2 = rest.all();
        check(v2.size() == 3 && rest.list.dropped() == 0 &&
                  v2[0].kind == MpeOut::Kind::NoteOff && v2[0].noteId == 2 && v2[0].channel == 2 && v2[0].frame == 15 &&
                  v2[1].kind == MpeOut::Kind::NoteOff && v2[1].noteId == 3 && v2[1].channel == 3 && v2[1].frame == 15 &&
                  v2[2].kind == MpeOut::Kind::NoteOn && v2[2].noteId == 4 && v2[2].channel == 0 && v2[2].frame == 20,
              "next route() drains notes 2 and 3 on their original member channels before the Plain note-on");

        // reset() drops any unsent pending NoteOffs (reactivated plug-in forgot its voices).
        Out zero(0);
        r.switchTo(ExpressionRoute::MpeMidi, 30, zero.list);
        check(zero.list.dropped() == 1 && r.current() == ExpressionRoute::MpeMidi,
              "zero-capacity switch queues note 4's NoteOff and switches route");
        r.reset();
        Out afterReset(8);
        route(r, {}, afterReset, 40);
        check(afterReset.list.size() == 3 && afterReset.list.at(0).kind == MpeOut::Kind::Control,
              "reset() clears pending NoteOffs so only the fresh MCM is sent");
    }

    // 7. Filling a bounded list across a dense block keeps router state matching emitted messages.
    {
        MpeRouter r;
        r.configure(ExpressionRoute::MpeMidi, ExpressionCaps{}, 15);
        std::vector<Event> dense;
        for (int i = 1; i <= 10; ++i) dense.push_back(on(i, static_cast<std::uint64_t>(i), 50 + i));
        for (int i = 1; i <= 10; ++i) dense.push_back(off(20 + i, static_cast<std::uint64_t>(i), 50 + i));
        Out bounded(16);  // 3 MCM + 3 complete notes (12) + 1 spare reset = 16
        route(r, dense, bounded, 0);
        const auto emitted = bounded.all();
        int sentOn = 0, sentOff = 0;
        for (const auto& o : emitted) {
            if (o.kind == MpeOut::Kind::NoteOn) ++sentOn;
            if (o.kind == MpeOut::Kind::NoteOff) ++sentOff;
        }
        check(bounded.list.size() == 16 && bounded.list.dropped() > 0 && sentOn == 3 && sentOff == 0,
              "bounded list fills mid-block after MCM and 3 notes");
        // All 3 notes that actually started received NoteOffs in the same block while out was full,
        // so they are released and queued as pending NoteOffs for the next block (and notes 4..10 never started).
        check(r.liveNotes() == 0 && r.channelOf(1) == -1 && r.channelOf(4) == -1,
              "dropped NoteOffs for started notes release their slots while dropped NoteOns were never tracked");
        Out next(16);
        route(r, {}, next, 40);
        const auto drained = next.all();
        check(drained.size() == 3 &&
                  drained[0].kind == MpeOut::Kind::NoteOff && drained[0].noteId == 1 && drained[0].channel == 1 &&
                  drained[1].kind == MpeOut::Kind::NoteOff && drained[1].noteId == 2 && drained[1].channel == 2 &&
                  drained[2].kind == MpeOut::Kind::NoteOff && drained[2].noteId == 3 && drained[2].channel == 3,
              "next block delivers NoteOffs for exactly the 3 notes that had started");
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_mpe_output_tests -- ADR-0097, MPE+ out through VST3\n\n");
    testBendEncodingInvertsTheParser();
    testRouteResolution();
    testTheControllersChannelNeverReachesThePlugin();
    testNoteExpressionRoute();
    testMpeMidiRoute();
    testMemberChannelAllocation();
    testParameterPath();
    testPlainRoute();
    testSwitchingRoutesEndsNotesWhereTheyStarted();
    testUnassignedIdsAndCapacity();
    testFullOutputListPreservesState();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
