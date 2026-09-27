// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `[adi.param]` and `[adi.array]` declaration parse -- ADR-0177, ADR-0183.
//
// NOTHING HERE TOUCHES libpd, and that is the property under test as much as
// any assertion below. ADR-0177 fix 3 puts the declared set behind
// `device.loadState`: the DAW reads the declarations from the stored patch TEXT
// without running Pd, so adding a parameter is one op that undo reverses. A
// parser that needed Pd would have to open the patch to read it, and opening a
// patch to learn what is in it is exactly the "the DAW scans the patch" that
// the panel document named as the original proposal's fatal flaw.
//
// The two objects are tested together, from one table of cases where the rules
// are shared, because the director's instruction is that they move as identical
// siblings. A rule proved for one and assumed for the other is how they drift.

#include "juce/pd_declarations.hpp"

#include <cstdio>
#include <string>

namespace {

using namespace adi::device;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void eqi(long long got, long long want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %lld, want %lld\n", what.c_str(), got, want);
    }
}
void eqs(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got \"%s\", want \"%s\"\n",
                    what.c_str(), got.c_str(), want.c_str());
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

/// A patch file, written the way the generator writes one: `$` escaped.
std::string patch(const std::string& body) {
    return "#N canvas 0 50 450 300 12;\n" + body;
}
std::string obj(const std::string& text) {
    return "#X obj 20 40 " + text + ";\n";
}

bool hasProblem(const PdDeclarations& d, PdDeclProblem p) {
    for (const auto& r : d.problems) if (r.problem == p) return true;
    return false;
}

// ---------------------------------------------------------------------------

void testTheSharedRules() {
    section("ADR-0177/0183 -- what the two objects share, they share exactly");

    // $0 first. Pd's send/receive names are global within an instance, so a
    // declaration that did not carry $0 could not build a name of its own.
    for (const char* cls : {"adi.param", "adi.array"}) {
        const auto bad = parsePdDeclarations(patch(obj(std::string(cls) + " 7 1 2 3 4 - lin Name")));
        check(hasProblem(bad, PdDeclProblem::NotDollarZero),
              std::string(cls) + " without $0 is refused");
        check(bad.params.empty() && bad.arrays.empty(),
              std::string(cls) + " without $0 declares nothing");
    }

    // The id is a positive integer below 2^31 and nothing else.
    for (const char* cls : {"adi.param", "adi.array"}) {
        for (const char* id : {"0", "-1", "2147483648", "cutoff", "1.5"}) {
            const auto d = parsePdDeclarations(
                patch(obj(std::string(cls) + " \\$0 " + id + " 1 2 3 4 - lin Name")));
            check(hasProblem(d, PdDeclProblem::BadId),
                  std::string(cls) + " rejects the id \"" + id + "\"");
        }
    }

    // A duplicate id keeps the FIRST and reports the second (ADR-0177 d2).
    const auto dupP = parsePdDeclarations(patch(
        obj("adi.param \\$0 3 0 1 0 - lin First") + obj("adi.param \\$0 3 0 1 0 - lin Second")));
    eqi(static_cast<long long>(dupP.params.size()), 1, "one parameter survives a duplicate id");
    eqs(dupP.params[0].name, "First", "and it is the FIRST, not the last");
    check(hasProblem(dupP, PdDeclProblem::DuplicateId), "the second is reported");

    const auto dupA = parsePdDeclarations(patch(
        obj("adi.array \\$0 3 64 30 -90 0 dB First") + obj("adi.array \\$0 3 64 30 -90 0 dB Second")));
    eqi(static_cast<long long>(dupA.arrays.size()), 1, "and the same for arrays");
    eqs(dupA.arrays[0].name, "First", "first again");

    // Separate id spaces: an array and a parameter may both be id 1, because
    // only the parameter's id is plugin_params.param_id.
    const auto both = parsePdDeclarations(patch(
        obj("adi.param \\$0 1 0 1 0 - lin Gain") + obj("adi.array \\$0 1 64 30 -90 0 dB Spectrum")));
    eqi(static_cast<long long>(both.params.size()), 1, "a param with id 1");
    eqi(static_cast<long long>(both.arrays.size()), 1, "and an array with id 1 coexist");
    eqi(static_cast<long long>(both.problems.size()), 0, "with nothing reported");

    // A bad declaration is ignored and the DEVICE STILL LOADS -- the others
    // around it survive, which is the whole of ADR-0177 decision 2.
    const auto mixed = parsePdDeclarations(patch(
        obj("adi.param \\$0 1 0 1 0 - lin Good") +
        obj("adi.param \\$0 x 0 1 0 - lin Broken") +
        obj("adi.array \\$0 2 64 30 -90 0 dB AlsoGood")));
    eqi(static_cast<long long>(mixed.params.size()), 1, "the good parameter survives");
    eqi(static_cast<long long>(mixed.arrays.size()), 1, "so does the array after the broken one");
    eqi(static_cast<long long>(mixed.problems.size()), 1, "and the broken one is reported, once");

    // The underscore rule, shared: `Cutoff_Freq` reads "Cutoff Freq".
    const auto lp = parsePdDeclarations(patch(obj("adi.param \\$0 1 0 1 0 - lin Cutoff_Freq")));
    eqs(lp.params.at(0).name, "Cutoff Freq", "a parameter's underscores are spaces");
    const auto la = parsePdDeclarations(patch(obj("adi.array \\$0 1 64 30 -90 0 dB Left_Spectrum")));
    eqs(la.arrays.at(0).name, "Left Spectrum", "and an array's are too");

    // `-` means no unit, not a unit named "-".
    check(lp.params.at(0).unit.empty(), "a parameter's `-` unit is empty");
    eqs(la.arrays.at(0).unit, "dB", "and a real unit survives");
}

void testTheParameterSchema() {
    section("ADR-0177 -- the declaration is complete, and checked");

    const auto d = parsePdDeclarations(patch(
        obj("adi.param \\$0 1 20 20000 1000 Hz log Cutoff") +
        obj("adi.param \\$0 2 -24 24 0 dB lin Drive") +
        obj("adi.param \\$0 3 0 2 0 - menu Loop_Mode Off Forward Back_and_forth")));
    eqi(static_cast<long long>(d.params.size()), 3, "the three from the ADR's own example parse");
    eqi(static_cast<long long>(d.problems.size()), 0, "with nothing reported");

    check(d.param(1)->curve == PdCurve::Log, "Hz log");
    eqs(d.param(1)->unit, "Hz", "its unit");
    check(d.param(2)->curve == PdCurve::Lin, "dB lin");
    check(d.param(3)->curve == PdCurve::Menu, "and a menu");
    eqi(static_cast<long long>(d.param(3)->items.size()), 3, "with three items");
    eqs(d.param(3)->items[2], "Back and forth", "whose underscores are spaces too");

    // A log curve with min 0 has no mapping at all, and saying so beats
    // drawing a knob that does nothing over half its travel.
    check(hasProblem(parsePdDeclarations(patch(obj("adi.param \\$0 1 0 20000 1000 Hz log F"))),
                     PdDeclProblem::LogNeedsPositiveMin),
          "log with min 0 is refused");

    // An exponent, as M4L's.
    const auto e = parsePdDeclarations(patch(obj("adi.param \\$0 1 0 1 0 - 2.5 Shape")));
    check(e.params.at(0).curve == PdCurve::Exponent, "a positive number is an exponent");
    check(e.params.at(0).exponent > 2.4 && e.params.at(0).exponent < 2.6, "and is kept");
    check(hasProblem(parsePdDeclarations(patch(obj("adi.param \\$0 1 0 1 0 - -2 S"))),
                     PdDeclProblem::BadCurve),
          "a negative exponent is not a curve");

    check(hasProblem(parsePdDeclarations(patch(obj("adi.param \\$0 1 1 0 0 - lin S"))),
                     PdDeclProblem::BadRange),
          "min at or above max is refused");
    check(hasProblem(parsePdDeclarations(patch(obj("adi.param \\$0 1 0 1 9 - lin S"))),
                     PdDeclProblem::BadRange),
          "a default outside the range is refused");
    check(hasProblem(parsePdDeclarations(patch(obj("adi.param \\$0 1 0 2 0 - menu M A B"))),
                     PdDeclProblem::MenuItemCount),
          "a menu with the wrong number of items is refused");
    check(hasProblem(parsePdDeclarations(patch(obj("adi.param \\$0 1 0 1 0 - lin S Extra"))),
                     PdDeclProblem::WrongArity),
          "only a menu may carry items after the name");
}

void testTheArraySchema() {
    section("ADR-0183 -- an array declares what a renderer would otherwise guess");

    const auto d = parsePdDeclarations(patch(
        obj("adi.array \\$0 1 512 30 -90 0 dB Spectrum") +
        obj("adi.array \\$0 2 1 20 -1 1 - Correlation")));
    eqi(static_cast<long long>(d.arrays.size()), 2, "both parse");
    eqi(d.array(1)->length, 512, "length");
    check(d.array(1)->rate > 29.9 && d.array(1)->rate < 30.1, "rate in hertz");
    check(d.array(1)->min < -89.9 && d.array(1)->max > -0.1, "and the value range");
    eqs(d.array(2)->unit, "", "a correlation has no unit, and `-` says so");

    // The bounds are sanity, not features: a patch that declares a billion
    // cells must not make the DAW allocate them (ADR-0085's reasoning).
    check(hasProblem(parsePdDeclarations(patch(obj("adi.array \\$0 1 99999999 30 -90 0 dB S"))),
                     PdDeclProblem::BadLength),
          "an absurd length is refused rather than allocated");
    check(hasProblem(parsePdDeclarations(patch(obj("adi.array \\$0 1 0 30 -90 0 dB S"))),
                     PdDeclProblem::BadLength),
          "and so is a length of zero");
    check(hasProblem(parsePdDeclarations(patch(obj("adi.array \\$0 1 512 0 -90 0 dB S"))),
                     PdDeclProblem::BadRate),
          "a rate of zero is refused -- the reader needs to know what stale means");
    check(hasProblem(parsePdDeclarations(patch(obj("adi.array \\$0 1 512 99999 -90 0 dB S"))),
                     PdDeclProblem::BadRate),
          "and so is a rate no display could use");
    check(hasProblem(parsePdDeclarations(patch(obj("adi.array \\$0 1 512 30 0 -90 dB S"))),
                     PdDeclProblem::BadRange),
          "min above max is refused");
    check(hasProblem(parsePdDeclarations(patch(obj("adi.array \\$0 1 512 30 -90 0 dB"))),
                     PdDeclProblem::WrongArity),
          "a short declaration is refused rather than half-read");
}

void testTheFileFormat() {
    section("ADR-0177 fix 3 -- read from the patch TEXT, with Pd not running");

    // A record ends at an unescaped `;` and may span lines. The generator
    // wraps long object boxes, so this is the normal case, not an edge one.
    const std::string wrapped =
        "#N canvas 0 50 450 300 12;\n"
        "#X obj 20 40 adi.param \\$0 1 20 20000 1000 Hz log\n"
        "Cutoff;\n";
    const auto w = parsePdDeclarations(wrapped);
    eqi(static_cast<long long>(w.params.size()), 1, "a declaration wrapped across lines parses");
    eqs(w.params.at(0).name, "Cutoff", "with its last atom intact");

    // Everything that is not an object box is skipped: comments naming
    // adi.param, message boxes, connections.
    const std::string noise =
        "#N canvas 0 50 450 300 12;\n"
        "#X text 20 10 adi.param \\$0 1 0 1 0 - lin NotADeclaration;\n"
        "#X msg 20 30 adi.param \\$0 2 0 1 0 - lin AlsoNot;\n"
        "#X obj 20 40 adi.param \\$0 3 0 1 0 - lin Real;\n"
        "#X connect 0 0 1 0;\n";
    const auto nz = parsePdDeclarations(noise);
    eqi(static_cast<long long>(nz.params.size()), 1, "only the object box declares");
    eqi(nz.params.at(0).id, 3, "and it is the right one");

    // Subpatches share the device's $0, so declarations in them count
    // (ADR-0177 decision 1).
    const std::string sub =
        "#N canvas 0 50 450 300 12;\n"
        "#N canvas 0 50 450 300 sub 0;\n"
        "#X obj 20 40 adi.array \\$0 1 64 30 -90 0 dB InASubpatch;\n"
        "#X restore 20 100 pd sub;\n";
    eqi(static_cast<long long>(parsePdDeclarations(sub).arrays.size()), 1,
        "a declaration inside a subpatch is found");

    // An empty file, and a file that is not a patch at all.
    eqi(static_cast<long long>(parsePdDeclarations("").params.size()), 0, "an empty file is empty");
    eqi(static_cast<long long>(parsePdDeclarations("not a patch").problems.size()), 0,
        "and a file that is not a patch reports nothing rather than crying");
}

void testTheReceiveNames() {
    section("ADR-0177 d5 -- the names the host sends to");

    eqs(pdParamReceiveName(1003, 7), "1003-adi-7", "a parameter's receive name");
    eqs(pdArrayReceiveName(1003, 7), "1003-adiarr-7",
        "and an array's, on a different stem so the two cannot be confused");
    check(pdParamReceiveName(1003, 7) != pdArrayReceiveName(1003, 7),
          "which is the point: a patch addressing an array where a parameter is "
          "meant fails outright instead of half working");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_pd_declaration_tests -- [adi.param] and [adi.array], parsed from text\n\n");
    testTheSharedRules();
    testTheParameterSchema();
    testTheArraySchema();
    testTheFileFormat();
    testTheReceiveNames();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
