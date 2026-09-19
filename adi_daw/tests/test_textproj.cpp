// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the canonical text projection primitives — ADR-0007,
// docs/format/TEXT-PROJECTION.md.
//
// These are the parts where determinism is actually won or lost. ADR-0021's
// replay oracle compares BYTES, so every check here is an equality against an
// exact expected string, never a tolerance and never a property that would
// accept two different renderings of the same value.
//
// No framework, matching tests/test_main.cpp.

#include "adi/textproj.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void eq(const std::string& got, const std::string& want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got  %s\n          want %s\n",
                    what, got.c_str(), want.c_str());
    }
}

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what); }
}

void section(const char* s) { std::printf("[%s]\n", s); }

using namespace adi::textproj;

// --- durations --------------------------------------------------------------
void testDurations() {
    section("durations as fractions of a whole note");
    eq(renderDuration(kWhole),         "1/1",  "whole note");
    eq(renderDuration(kWhole / 2),     "1/2",  "half");
    eq(renderDuration(kPPQ),           "1/4",  "quarter");
    eq(renderDuration(kWhole / 16),    "1/16", "sixteenth");
    eq(renderDuration(kWhole / 64),    "1/64", "sixty-fourth");
    eq(renderDuration(3 * kWhole / 8), "3/8",  "dotted quarter");
    eq(renderDuration(kWhole / 12),    "1/12", "eighth triplet");
    eq(renderDuration(kWhole / 80),    "1/80", "sixteenth quintuplet");
    eq(renderDuration(2 * kWhole),     "2/1",  "two whole notes stay a fraction");
    eq(renderDuration(7 * kWhole / 3), "7/3",  "an improper fraction is not reduced away");

    // The case that dominates a real project. A recorded note is not on the
    // grid, does not reduce, and must not render as a 7-digit fraction.
    eq(renderDuration(kWhole / 16 + 1), std::to_string(kWhole / 16 + 1) + "t",
       "an unquantised duration falls back to raw ticks");
    // Achievable denominators are exactly the divisors of kWhole, which is
    // 2^9 * 3^2 * 5 * 7 * 11 * 13 (SPEC 4.2 factorises ADI_PPQ; a whole note is
    // four of them). So 1024 is not reachable as a denominator at all -- 512 is
    // the power-of-two ceiling -- and the bound is a cutoff, not a target.
    eq(renderDuration(kWhole / 512), "1/512", "512 is the largest power-of-two denominator");
    eq(renderDuration(kWhole / 720), "1/720", "a non-power-of-two divisor below the bound");
    eq(renderDuration(kWhole / 1152), std::to_string(kWhole / 1152) + "t",
       "a divisor above the bound falls back to ticks");

    // Total, not merely correct on legal input.
    eq(renderDuration(0),  "0t",  "zero is rendered, not divided by");
    eq(renderDuration(-1), "-1t", "a negative duration is rendered, not UB");
}

// --- floats -----------------------------------------------------------------
void testFloats() {
    section("floats round-trip and their shape is pinned");
    eq(renderF64(0.1),       "0.1",                "f64 shortest round-trip");
    eq(renderF64(1.0 / 3.0), "0.3333333333333333", "f64 one third");
    eq(renderF32(0.1f),      "0.1",                "f32 formats AS f32");

    // The bug the spec forbids in as many words: an f32 formatted through
    // double. AEXP.value, AEXP.tension, AAUT.tension and ANOT.tuning_cents are
    // all f32, so this is the difference between `0.1` and `0.10000000149011612`
    // on a large fraction of the lines in a file.
    check(renderF32(0.1f) != renderF64(static_cast<double>(0.1f)),
          "an f32 is never widened to f64 for rendering");
    eq(renderF64(static_cast<double>(0.1f)), "0.10000000149011612",
       "and this is what widening would have printed");

    // -0.0 is a different bit pattern and the oracle compares bytes.
    eq(renderF64(0.0),   "0.0",  "positive zero");
    eq(renderF64(-0.0),  "-0.0", "negative zero is distinct");
    eq(renderF32(-0.0f), "-0.0", "negative zero, f32");
    check(renderF64(0.0) != renderF64(-0.0), "the two zeroes do not collide");

    // Every finite value carries a `.` or an `e`, so a float never renders as a
    // bare integer.
    eq(renderF64(1.0),         "1.0",        "an integral f64 still shows a point");
    eq(renderF64(-42.0),       "-42.0",      "negative integral");
    eq(renderF32(16777216.0f), "16777216.0", "f32 at its integer precision limit");

    // Exponent shape is pinned here, not inherited: `1e+308` and `1e308` are
    // both valid shortest round-trips, so the format has to choose.
    eq(renderF64(1e308),  "1.0e308",  "no plus sign in the exponent");
    eq(renderF64(1e-308), "1.0e-308", "a negative exponent keeps its sign");
    check(renderF64(1e308).find('+') == std::string::npos, "no '+' anywhere");

    // Non-finite. Payloads are deliberately not rendered: x87 on ADR-0022's
    // i386 leg quietens signalling NaNs in transit, so rendering a payload
    // would fail the replay oracle on our own CI.
    eq(renderF64(std::numeric_limits<double>::quiet_NaN()),  "nan",  "nan");
    eq(renderF64(std::numeric_limits<double>::infinity()),   "inf",  "positive infinity");
    eq(renderF64(-std::numeric_limits<double>::infinity()),  "-inf", "negative infinity");
    eq(renderF32(std::numeric_limits<float>::quiet_NaN()),   "nan",  "nan, f32");
    eq(renderF32(-std::numeric_limits<float>::infinity()),   "-inf", "negative infinity, f32");
}

// --- escaping ---------------------------------------------------------------
void testEscaping() {
    section("escaping");
    eq(quoteString("Keys"), "\"Keys\"",   "a plain name");
    eq(quoteString(""),     "\"\"",       "the empty name is representable");
    eq(quoteString("a\"b"), "\"a\\\"b\"", "a quote");
    eq(quoteString("a\\b"), "\"a\\\\b\"", "a backslash");
    eq(quoteString("a\nb"), "\"a\\nb\"",  "a newline cannot close a block");
    eq(quoteString("a\tb"), "\"a\\tb\"",  "a tab");
    eq(quoteString("a\rb"), "\"a\\rb\"",  "a carriage return");
    eq(quoteString(std::string("a\0b", 3)), "\"a\\u{0}b\"",  "a NUL");
    eq(quoteString("a\x7F""b"),             "\"a\\u{7F}b\"", "DEL");

    // Non-ASCII that is not in the MUST-escape set passes through as its bytes.
    eq(quoteString("Caf\xC3\xA9"),         "\"Caf\xC3\xA9\"",         "ordinary non-ASCII is not escaped");
    eq(quoteString("\xF0\x9F\x8E\xB9"),    "\"\xF0\x9F\x8E\xB9\"",    "a 4-byte scalar survives");

    // The security rule. U+202E reverses the display of the rest of the line,
    // so a diff hunk could be made to show something other than what it says.
    eq(quoteString("a\xE2\x80\xAE""b"), "\"a\\u{202E}b\"", "RIGHT-TO-LEFT OVERRIDE is escaped");
    eq(quoteString("a\xE2\x80\x8F""b"), "\"a\\u{200F}b\"", "RLM is escaped");
    eq(quoteString("a\xE2\x81\xA6""b"), "\"a\\u{2066}b\"", "an isolate control is escaped");
    eq(quoteString("a\xEF\xBB\xBF""b"), "\"a\\u{FEFF}b\"", "a BOM inside a name is escaped");
    eq(quoteString("a\xE2\x80\xA8""b"), "\"a\\u{2028}b\"", "LINE SEPARATOR is escaped");
    eq(quoteString("a\xC2\x85""b"),     "\"a\\u{85}b\"",   "a C1 control is escaped");

    // Invalid UTF-8 is reachable: names come from a TEXT column with no
    // validation. Escaping the raw byte is total and injective; substituting
    // U+FFFD would merge two distinct names into one label.
    eq(quoteString(std::string("a\xFF""b")),    "\"a\\x{FF}b\"",            "a lone invalid byte");
    eq(quoteString(std::string("\xC0\xAF")),    "\"\\x{C0}\\x{AF}\"",       "an overlong encoding is rejected");
    eq(quoteString(std::string("\xED\xA0\x80")),"\"\\x{ED}\\x{A0}\\x{80}\"","a surrogate encoding is rejected");
    eq(quoteString(std::string("\xE2\x82")),    "\"\\x{E2}\\x{82}\"",       "a truncated sequence at end of string");
    check(quoteString(std::string("a\xFF""b")) != quoteString(std::string("a\xFE""b")),
          "two different invalid bytes do not render alike");

    section("bare versus quoted");
    check(isBareSafe("Keys"),      "a plain name is bare");
    check(!isBareSafe(""),         "the empty name is never bare");
    check(!isBareSafe("Drum Kit"), "a space forces quoting");
    check(!isBareSafe("a/b"),      "the path separator forces quoting");
    check(!isBareSafe("Bus#2"),    "a literal # forces quoting, so it cannot read as a rank");
    check(!isBareSafe("~x"),       "a literal ~ forces quoting, so it cannot read as a collision suffix");
    check(!isBareSafe("@dev"),     "a literal @ forces quoting, so it cannot read as a selector");
    check(!isBareSafe("a\xE2\x80\xAE""b"), "a bidi control is never bare");
    eq(renderLabelToken("Keys"),     "Keys",         "a bare token");
    eq(renderLabelToken("Drum Kit"), "\"Drum Kit\"", "a quoted token");
}

// --- labels and collisions --------------------------------------------------
void testLabels() {
    section("labels and collisions");
    {
        const auto out = assignLabels({"Kick", "Snare", "Hat"});
        eq(out[0], "Kick", "distinct names are untouched");
        eq(out[2], "Hat",  "distinct names are untouched (3)");
    }
    {
        // Every member of a colliding group is suffixed, the first included, so
        // a collision reads as a collision rather than a silent demotion.
        const auto out = assignLabels({"Audio 1", "Bass", "Audio 1"});
        eq(out[0], "Audio 1~1", "the incumbent is suffixed too");
        eq(out[1], "Bass",      "a non-colliding sibling is untouched");
        eq(out[2], "Audio 1~2", "the second gets ~2");
    }
    {
        const auto out = assignLabels({"", "", "Named"});
        eq(out[0], "#0",    "an empty label falls back to its rank");
        eq(out[1], "#1",    "ranks are 0-based and distinct, so they do not collide");
        eq(out[2], "Named", "a named sibling is unaffected");
    }
    {
        // A name that looks like a generated fallback must not merge with one.
        const auto out = assignLabels({"", "#0"});
        check(out[0] != out[1], "a literal #0 does not collide silently with the fallback");
        eq(out[0], "#0~1", "both are suffixed");
        eq(out[1], "#0~2", "both are suffixed (2)");
    }
    {
        // Legal per the schema: no unique index exists on any sibling ordinal,
        // so indistinguishable siblings are reachable input, not a hypothetical.
        const auto out = assignLabels({"Same", "Same", "Same"});
        eq(out[0], "Same~1", "three-way collision, first");
        eq(out[1], "Same~2", "three-way collision, second");
        eq(out[2], "Same~3", "three-way collision, third");
    }
    {
        const auto out = assignLabels({});
        check(out.empty(), "an empty container yields no labels");
    }
}

// --- determinism ------------------------------------------------------------
void testDeterminism() {
    section("determinism");
    // Cheap, but it is the actual property: these are pure functions of their
    // arguments with no state carried between calls.
    const std::vector<std::string> names = {
        "Keys", "", "a\nb", "Caf\xC3\xA9", "\xF0\x9F\x8E\xB9",
        std::string("\xFF\xFE"), "Audio 1"};
    for (const auto& n : names)
        check(quoteString(n) == quoteString(n), "quoteString is stable across calls");

    const std::vector<std::int64_t> ticks = {kWhole, kPPQ, kWhole / 12,
                                             kWhole / 16 + 1, 0};
    for (std::int64_t t : ticks)
        check(renderDuration(t) == renderDuration(t), "renderDuration is stable");

    const std::vector<double> ds = {0.1, -0.0, 1e308, 1.0 / 3.0};
    for (double d : ds)
        check(renderF64(d) == renderF64(d), "renderF64 is stable");

    check(assignLabels({"A", "A", "B"}) == assignLabels({"A", "A", "B"}),
          "assignLabels is stable");
}

}  // namespace

int main() {
    std::printf("adi_tests_textproj -- canonical text projection (ADR-0007)\n\n");
    testDurations();
    testFloats();
    testEscaping();
    testLabels();
    testDeterminism();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
