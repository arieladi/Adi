// SPDX-License-Identifier: AGPL-3.0-or-later
//
// DF-E37 (c) -- a built-in's saved values survive a range change
// (src/juce/builtin_values.*, ADR-0188 d3).
//
// The review's smallest cases, each saved under one declaration and read under
// another: Saturator's Output at -12 dB (range -60..0, then Live's -36..0),
// Frequency at 1000 Hz (a log range, 20..22000, then 30..18500) and Shifter's
// Window at 40 ms (1..1000, then Live's 10..350). Before, the position was
// kept and the value moved: -7.2 dB, about 1207 Hz, about 81 ms.

#include "juce/builtin_values.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace adi::device;
using adi::Payload;
namespace bv = adi::device::builtin_values;

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

std::vector<PdParamDecl> declare(const char* text) { return parsePdDeclarations(text).params; }

const char* kBefore =
    "#N canvas 0 0 100 100 12;\n"
    "#X obj 0 0 adi.param \\$0 1 -60 0 0 dB lin Output;\n"
    "#X obj 0 0 adi.param \\$0 2 20 22000 1000 Hz log Frequency;\n"
    "#X obj 0 0 adi.param \\$0 3 1 1000 80 ms lin Window;\n"
    "#X obj 0 0 adi.param \\$0 4 0 100 50 % lin Sustain;\n"
    "#X obj 0 0 adi.param \\$0 5 0 3 1 - menu Mode A B C D;\n";
const char* kAfter =
    "#N canvas 0 0 100 100 12;\n"
    "#X obj 0 0 adi.param \\$0 1 -36 0 0 dB lin Output;\n"
    "#X obj 0 0 adi.param \\$0 2 30 18500 1000 Hz log Frequency;\n"
    "#X obj 0 0 adi.param \\$0 3 10 350 80 ms lin Window;\n"
    "#X obj 0 0 adi.param \\$0 4 -70 0 0 dB lin Sustain;\n"
    "#X obj 0 0 adi.param \\$0 5 0 3 1 - menu Mode A B C D;\n";

double valueAfter(const std::vector<std::pair<std::size_t, float>>& read, const std::vector<PdParamDecl>& decl,
                  std::size_t index) {
    for (const auto& [i, position] : read)
        if (i == index) return bv::natural(decl[i], position);
    return std::nan("");
}

void testARangeChangeKeepsTheValue() {
    std::printf("\n[DF-E37 c -- a value saved under one range plays the same under another]\n");
    const auto before = declare(kBefore), after = declare(kAfter);
    const std::vector<float> positions{static_cast<float>(bv::normalized(before[0], -12)),
                                       static_cast<float>(bv::normalized(before[1], 1000)),
                                       static_cast<float>(bv::normalized(before[2], 40)),
                                       static_cast<float>(bv::normalized(before[3], 50)),
                                       static_cast<float>(bv::normalized(before[4], 2))};
    Payload state = Payload::object();
    state["version"] = 1;
    state["future"] = "kept";
    state["params"]["99"] = 0.25;   // an id this version does not declare
    bv::write(state, before, positions.data());
    check(state["future"] == "kept" && state["params"]["99"] == 0.25, "other fields and unknown ids survive a save");

    std::vector<std::pair<std::size_t, float>> read;
    check(bv::read(state, after, read), "the state reads under the new declaration");
    const double output = valueAfter(read, after, 0), freq = valueAfter(read, after, 1),
                 window = valueAfter(read, after, 2), sustain = valueAfter(read, after, 3),
                 mode = valueAfter(read, after, 4);
    std::printf("  Output %.4f dB, Frequency %.3f Hz, Window %.4f ms, Sustain %.2f dB, Mode %.0f\n", output, freq,
                window, sustain, mode);
    check(std::abs(output + 12) < 1e-4, "Output -12 dB stays -12 dB (was -7.2 when the position was kept)");
    check(std::abs(freq - 1000) < 1e-2, "Frequency 1000 Hz stays 1000 Hz on the log curve (was about 1207)");
    check(std::abs(window - 40) < 1e-4, "Window 40 ms stays 40 ms (was about 81)");
    check(std::abs(mode - 2) < 1e-9, "a menu item stays the same item");
    check(std::abs(sustain - bv::natural(after[3], positions[3])) < 1e-6,
          "a value whose unit changed (% to dB) falls back to its position, not 50 dB clamped");

    // A value outside the new range is clamped, as ADR-0188 d3 says.
    Payload wide = Payload::object();
    wide["version"] = 1;
    bv::write(wide, before, positions.data());
    wide["real"]["1"] = -50.0;
    check(bv::read(wide, after, read) && std::abs(valueAfter(read, after, 0) + 36) < 1e-6,
          "Output -50 dB under a -36..0 range clamps to -36");

    // A bad known value refuses the state, and nothing is to change.
    Payload bad = wide;
    bad["real"]["2"] = "loud";
    check(!bv::read(bad, after, read) && read.empty(), "a non-number real value refuses the state");
    Payload badPosition = wide;
    badPosition["params"]["99"] = 2.0;
    check(!bv::read(badPosition, after, read), "a position outside 0..1, even for an unknown id, refuses it");

    // A state from before this field keeps its positions (pre-release loss).
    Payload legacy = Payload::object();
    legacy["version"] = 1;
    legacy["params"]["1"] = positions[0];
    check(bv::read(legacy, after, read) && std::abs(valueAfter(read, after, 0) - bv::natural(after[0], positions[0])) < 1e-6,
          "a version-1 state with positions only keeps its positions (documented loss)");
}

void testSetParamHonoursRealWhereItIsTrue() {
    std::printf("\n[DF-E37 c -- setParam honours a real value where the engine's real is the true one]\n");
    const auto after = declare(kAfter);
    const auto lin = bv::positionFor(after[0], ParamValue::withReal(0.0, -12.0));
    check(lin && std::abs(bv::natural(after[0], *lin) + 12) < 1e-6, "a linear parameter takes its real value");
    // The mirror's real is a linear reading of the position; on a log curve it
    // is not the value, so the position is taken there.
    const double pos = bv::normalized(after[1], 1000);
    const double linearReading = after[1].min + pos * (after[1].max - after[1].min);
    const auto log = bv::positionFor(after[1], ParamValue::withReal(pos, linearReading));
    check(log && std::abs(bv::natural(after[1], *log) - 1000) < 1e-2,
          "a log parameter takes its position, not the mirror's linear 'real'");
    const auto menu = bv::positionFor(after[4], ParamValue::withReal(0.0, 2.4));
    check(menu && std::abs(bv::natural(after[4], *menu) - 2) < 1e-9, "a menu value rounds to an item");
    check(!bv::positionFor(after[0], ParamValue::fromNormalized(1.5)), "a position outside 0..1 is refused");
    check(!bv::positionFor(after[0], ParamValue::withReal(0.5, std::nan(""))), "a non-finite real is refused");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_builtin_value_tests -- DF-E37 (c)\n");
    testARangeChangeKeepsTheValue();
    testSetParamHonoursRealWhereItIsTrue();
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
