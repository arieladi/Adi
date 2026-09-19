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
#include <algorithm>
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

// --- designators ------------------------------------------------------------
void testDesignators() {
    section("designators");
    eq(designator({"trk", "Rhythm", "Drums"}), "\"/trk/Rhythm/Drums\"", "a nested track");
    eq(designator({"trk", "Bass", "clip", "Verse"}), "\"/trk/Bass/clip/Verse\"", "a clip");
    eq(designator({"scene", "Chorus"}), "\"/scene/Chorus\"", "a scene");
    eq(designator({}), "\"\"", "an empty path is representable");

    // The whole path is ONE quoted string, so a name with a space does not
    // change the token's shape -- renaming Bass to Bass Gtr is a text edit
    // inside the quotes, not a re-quoting of the line.
    eq(designator({"trk", "Bass Gtr"}), "\"/trk/Bass Gtr\"", "a space needs no extra quoting");

    // A literal slash must not forge a path boundary.
    eq(designator({"trk", "AC/DC"}), "\"/trk/AC\\u{2F}DC\"", "a literal / is escaped");
    check(designator({"trk", "a/b"}) != designator({"trk", "a", "b"}),
          "an escaped slash is distinct from a real segment break");

    // Everything section 4 escapes is still escaped inside a designator.
    eq(designator({"trk", "a\xE2\x80\xAE""b"}), "\"/trk/a\\u{202E}b\"",
       "a bidi control cannot hide inside a designator either");
    eq(designator({"trk", "a\nb"}), "\"/trk/a\\nb\"", "a newline cannot break the line");
    eq(designator({"trk", "a\"b"}), "\"/trk/a\\\"b\"", "a quote cannot close the token");

    section("track roles");
    eq(std::string(roleRoot(roleOfKind("audio"))),      "trk",    "audio is a track");
    eq(std::string(roleRoot(roleOfKind("instrument"))), "trk",    "so is an instrument");
    eq(std::string(roleRoot(roleOfKind("group"))),      "trk",    "so is a group");
    eq(std::string(roleRoot(roleOfKind("return"))),     "ret",    "a return has its own space");
    eq(std::string(roleRoot(roleOfKind("master"))),     "master", "so does the master");
    eq(std::string(roleRoot(roleOfKind("vca"))),        "vca",    "so do VCAs");
    eq(std::string(roleRoot(roleOfKind("tempo"))),      "glob",   "the tempo lane is global");
    eq(std::string(roleRoot(roleOfKind("signature"))),  "glob",   "so is the signature lane");
    eq(std::string(roleRoot(roleOfKind("marker"))),     "glob",   "so is the marker lane");
    // A kind a newer writer invented. Refusing would lose a file we can
    // otherwise render, so it lands in /trk.
    eq(std::string(roleRoot(roleOfKind("holographic"))), "trk",
       "an unknown kind is a track, not a refusal");

    section("unresolvable references");
    eq(unresolved("bus"), "\"!unresolved(bus)\"", "a bus endpoint, which is reachable today");
    eq(unresolved("track"), "\"!unresolved(track)\"", "a dangling track reference");
    check(unresolved("bus").find("0") == std::string::npos,
          "never a number, so it cannot be mistaken for an id");

    section("media prefixes");
    {
        // Distinct at 16, so 16 it is.
        const auto p = mediaPrefixes({"1f4a9c2e7b0d3a51aaaa", "9b3e0c7d2a145f88bbbb"});
        eq(p[0], "1f4a9c2e7b0d3a51", "the minimum prefix is 16 hex digits");
        eq(p[1], "9b3e0c7d2a145f88", "and both are cut to the same length");
    }
    {
        // Colliding at 16, so it grows -- in steps of 4, not 1.
        const auto p = mediaPrefixes({"1f4a9c2e7b0d3a51aaaa", "1f4a9c2e7b0d3a51bbbb"});
        check(p[0].size() == 20, "the length grows by 4 when 16 collides");
        check(p[0] != p[1], "and it separates them");
    }
    {
        // Two rows may legitimately share a hash: idx_media_hash is not unique.
        // No prefix can separate those, and none should try.
        const auto p = mediaPrefixes({"1f4a9c2e7b0d3a51aaaa", "1f4a9c2e7b0d3a51aaaa"});
        eq(p[0], p[1], "identical hashes get identical prefixes");
        check(p[0].size() == 16, "and the length does not grow chasing them");
        // They are then disambiguated exactly like any other duplicate label.
        const auto labels = assignLabels({p[0], p[1]});
        eq(labels[0], "1f4a9c2e7b0d3a51~1", "the duplicate falls through to ~k");
        eq(labels[1], "1f4a9c2e7b0d3a51~2", "both suffixed, as always");
    }
    {
        const auto p = mediaPrefixes({});
        check(p.empty(), "an empty media pool yields no prefixes");
    }
    {
        // Shorter than the minimum: take what there is rather than read past it.
        const auto p = mediaPrefixes({"abc"});
        eq(p[0], "abc", "a short hash is not padded or over-read");
    }
}

// --- ordering ---------------------------------------------------------------

Member mem(std::vector<SortKey> keys, std::string skel,
           std::vector<std::uint32_t> out = {}, std::vector<std::uint32_t> in = {}) {
    Member m;
    m.keys = std::move(keys);
    m.skeleton = std::move(skel);
    m.out_refs = std::move(out);
    m.in_refs = std::move(in);
    return m;
}

std::string seq(const std::vector<std::uint32_t>& order,
                const std::vector<std::string>& names) {
    std::string s;
    for (std::uint32_t i : order) { s += names[i]; s += ' '; }
    if (!s.empty()) s.pop_back();
    return s;
}

void testOrderingKeys() {
    section("K1 -- declared semantic keys");
    {
        // The rule from TEXT-PROJECTION 5.3. Sorting rendered position tokens
        // bytewise puts `10|1|0` before `2|1|0`; sort keys are raw values.
        std::vector<Member> ms = {mem({SortKey::integer(10)}, "a"),
                                  mem({SortKey::integer(2)},  "b"),
                                  mem({SortKey::integer(100)},"c")};
        const auto r = canonicalOrder(ms);
        eq(seq(r.order, {"10", "2", "100"}), "2 10 100", "integers sort as integers");
        check(r.status == OrderStatus::Exact, "no ties, so exact");
    }
    {
        std::vector<Member> ms = {mem({SortKey::text("b")}, "x"),
                                  mem({SortKey::integer(1)}, "x"),
                                  mem({SortKey::null()}, "x"),
                                  mem({SortKey::real(1.5)}, "x")};
        const auto r = canonicalOrder(ms);
        eq(seq(r.order, {"text", "int", "null", "real"}), "null int real text",
           "null < integer < real < text");
    }
    {
        // std::sort on a comparator built from `<` over doubles is undefined
        // behaviour when NaN is present, not merely wrong. This must be a
        // strict weak ordering over every bit pattern a REAL column can hold --
        // and schema.sql has no STRICT tables, so it can hold anything.
        const double nan = std::numeric_limits<double>::quiet_NaN();
        std::vector<Member> ms = {mem({SortKey::real(nan)},  "a"),
                                  mem({SortKey::real(0.0)},  "b"),
                                  mem({SortKey::real(-0.0)}, "c"),
                                  mem({SortKey::real(-1.0)}, "d")};
        const auto r = canonicalOrder(ms);
        eq(seq(r.order, {"nan", "+0", "-0", "-1"}), "-1 -0 +0 nan",
           "-0.0 before +0.0, NaN last");
    }
}

void testOrderingSkeleton() {
    section("K2 -- skeletons break K1 ties");
    std::vector<Member> ms = {mem({SortKey::integer(0)}, "zulu"),
                              mem({SortKey::integer(0)}, "alpha"),
                              mem({SortKey::integer(0)}, "mike")};
    const auto r = canonicalOrder(ms);
    eq(seq(r.order, {"zulu", "alpha", "mike"}), "alpha mike zulu",
       "equal keys fall through to the skeleton");
    check(r.status == OrderStatus::Exact, "skeletons separated them, so exact");
    check(r.largest_tied_class == 1, "nothing remained tied");
}

void testOrderingRefinement() {
    section("K3 -- the reference graph breaks what content cannot");
    {
        // Two members identical in every field. Only the graph around them
        // differs: member 2 cites member 0, nothing cites member 1. This is the
        // case the whole refinement step exists for -- and note it is reachable
        // input, because no sibling ordinal in the core model is unique.
        std::vector<Member> ms = {
            mem({SortKey::integer(0)}, "same", {}, {2}),   // cited by 2
            mem({SortKey::integer(0)}, "same", {}, {}),    // cited by nobody
            mem({SortKey::integer(1)}, "citer", {0}, {}),
        };
        const auto r = canonicalOrder(ms);
        check(r.largest_tied_class == 1, "refinement separated the twins");
        check(r.status == OrderStatus::Exact, "and the result is exact");
        eq(seq(r.order, {"cited", "uncited", "citer"}), "uncited cited citer",
           "the twins are ordered by their position in the graph");
    }
    {
        // Genuinely indistinguishable: same fields, same graph position.
        // Refinement cannot and should not separate these.
        std::vector<Member> ms = {mem({SortKey::integer(0)}, "same"),
                                  mem({SortKey::integer(0)}, "same")};
        const auto r = canonicalOrder(ms);
        check(r.largest_tied_class == 2, "a genuine tie is reported, not hidden");
        // Expectation corrected after the fuzzer found the leak behind it. With
        // no renderer, K4 cannot run, and the underlying sort is stable -- so
        // the tied pair would have kept INPUT order while the status claimed
        // canonicity. Refinement is incomplete, so we cannot certify that
        // swapping them is unobservable without rendering both.
        check(r.status == OrderStatus::Ambiguous,
              "an unresolvable tie is declared, never left in storage order");
    }
}

std::string renderNames(const std::vector<std::uint32_t>& order, void* ctx) {
    const auto* names = static_cast<const std::vector<std::string>*>(ctx);
    return seq(order, *names);
}

void testOrderingK4() {
    section("K4 -- minimise over the output, never over a member");
    {
        // Two members that tie all the way through refinement but whose
        // renderings differ. K4 must pick the lexicographically least OUTPUT.
        std::vector<Member> ms = {mem({SortKey::integer(0)}, "same"),
                                  mem({SortKey::integer(0)}, "same")};
        std::vector<std::string> names = {"zulu", "alpha"};
        const auto r = canonicalOrder(ms, &renderNames, &names);
        eq(seq(r.order, names), "alpha zulu", "the least rendering wins");
        check(r.status == OrderStatus::Exact, "resolved, so exact");
    }
    {
        // Above the bound the projector refuses rather than inventing an order.
        // An arbitrary order here would be a leak wearing a canonical hat.
        std::vector<Member> ms;
        for (int i = 0; i < 9; ++i) ms.push_back(mem({SortKey::integer(0)}, "same"));
        const auto r = canonicalOrder(ms);
        check(r.status == OrderStatus::Ambiguous, "a class of 9 exceeds the bound");
        check(r.largest_tied_class == 9, "and the size is reported");
        check(r.order.size() == 9, "output is still produced, so the projector stays total");
    }
    {
        std::vector<Member> ms;
        for (int i = 0; i < 8; ++i) ms.push_back(mem({SortKey::integer(0)}, "same"));
        std::vector<std::string> names = {"h","g","f","e","d","c","b","a"};
        const auto r = canonicalOrder(ms, &renderNames, &names);
        check(r.status == OrderStatus::Exact, "a class of exactly 8 is within the bound");
        eq(seq(r.order, names), "a b c d e f g h", "and all 8! permutations are searched");
    }
}

void testOrderingNoLeak() {
    section("storage order does not leak");
    // THE property. Build one collection, then present it in every possible
    // input order -- which is what a different rowid assignment, a different
    // insertion history or a different query plan would look like -- and
    // require the output sequence to be identical every time.
    const std::vector<std::string> names = {"kick", "snare", "hat", "ride"};
    auto build = [](const std::vector<std::size_t>& perm) {
        // Member i cites member (i+1) mod 4, so the graph is a cycle and every
        // member has one in-edge and one out-edge: content, not position, has
        // to do the separating.
        std::vector<Member> src = {
            mem({SortKey::integer(0)}, "kick",  {1}, {3}),
            mem({SortKey::integer(1)}, "snare", {2}, {0}),
            mem({SortKey::integer(2)}, "hat",   {3}, {1}),
            mem({SortKey::integer(3)}, "ride",  {0}, {2}),
        };
        // Remap into the requested input order, rewriting refs to match.
        std::vector<std::uint32_t> where(4);
        for (std::size_t p = 0; p < perm.size(); ++p)
            where[perm[p]] = static_cast<std::uint32_t>(p);
        std::vector<Member> out;
        for (std::size_t p = 0; p < perm.size(); ++p) {
            Member m = src[perm[p]];
            for (auto& r : m.out_refs) r = where[r];
            for (auto& r : m.in_refs)  r = where[r];
            out.push_back(std::move(m));
        }
        return out;
    };

    std::vector<std::size_t> perm = {0, 1, 2, 3};
    std::string expected;
    int permutations = 0;
    bool stable = true;
    do {
        std::vector<std::string> shuffled;
        for (std::size_t i : perm) shuffled.push_back(names[i]);
        const auto ms = build(perm);
        const auto r = canonicalOrder(ms);
        const std::string got = seq(r.order, shuffled);
        if (permutations == 0) expected = got;
        else if (got != expected) stable = false;
        ++permutations;
    } while (std::next_permutation(perm.begin(), perm.end()));

    check(permutations == 24, "all 24 input orders were tried");
    check(stable, "every input order produces the same output order");
    eq(expected, "kick snare hat ride", "and it is the content order");
}

}  // namespace

int main() {
    std::printf("adi_tests_textproj -- canonical text projection (ADR-0007)\n\n");
    testDurations();
    testFloats();
    testEscaping();
    testLabels();
    testDeterminism();
    testDesignators();
    testOrderingKeys();
    testOrderingSkeleton();
    testOrderingRefinement();
    testOrderingK4();
    testOrderingNoLeak();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
