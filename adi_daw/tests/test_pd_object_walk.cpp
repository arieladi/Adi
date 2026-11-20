// SPDX-License-Identifier: GPL-3.0-or-later
//
// PD4 -- the allowed-object walk (src/juce/pd_object_walk.*) and the
// vocabulary read from the running build (src/juce/pd_vocabulary.cpp).
//
// The walk's own cases use a small hand-made vocabulary, so they say exactly
// which names exist. The running build's vocabulary is then checked on what
// it must hold (vanilla's objects, ADI's compiled-in externals), what it must
// not, and on every patch ADI ships: each one must walk clean, or a device
// that opens today would be refused by the walk tomorrow.

#include "juce/pd_declarations.hpp"
#include "juce/pd_object_walk.hpp"
#if defined(ADI_WITH_PD)
#include "juce/pd_engine.hpp"
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace adi::device;

int checks = 0, failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

void section(const char* name) { std::printf("\n[%s]\n", name); }

PdVocabulary small() {
    return PdVocabulary({"osc~", "dac~", "adc~", "*~", "+~", "inlet~", "outlet~", "inlet", "outlet",
                         "metro", "f", "t", "pd", "clone", "lop~", "declare", "adi.param"});
}

const char* kHeader = "#N canvas 0 50 450 300 12;\n";

void testAFlatPatch() {
    section("PD4 -- a patch of known objects is allowed");
    const std::string p = std::string(kHeader) +
                          "#X obj 10 10 osc~ 440;\n#X obj 10 40 *~ 0.5;\n#X obj 10 70 dac~;\n"
                          "#X msg 100 10 \\; pd dsp 1;\n#X connect 0 0 1 0;\n#X connect 1 0 2 0;\n";
    const auto w = walkPdObjects(p, {}, small());
    check(w.allowed(), "osc~ -> *~ -> dac~ is allowed");
    check(w.objects == 3, "three object boxes counted (messages and connections are not boxes): " +
                              std::to_string(w.objects));
}

void testAnUnknownObjectIsNamedWhereItIs() {
    section("PD4 -- a refused box is named by file, subpatch, class and box number");
    const std::string p = std::string(kHeader) +
                          "#X obj 10 10 adc~;\n"
                          "#N canvas 0 0 300 200 lfo 0;\n"
                          "#X obj 10 10 osc~ 2;\n"
                          "#X obj 10 40 cyclone/cycle~ 2;\n"
                          "#X restore 10 40 pd lfo;\n"
                          "#X obj 10 80 dac~;\n";
    const auto w = walkPdObjects(p, {}, small());
    check(!w.allowed() && w.refused.size() == 1, "one box refused");
    if (w.refused.size() == 1) {
        const auto& r = w.refused[0];
        check(r.reason == PdObjectRefusal::Unknown && r.className == "cyclone/cycle~",
              "an unknown class, by its name: " + r.className);
        check(r.file.empty() && r.subpatch == "pd lfo", "in the patch, inside 'pd lfo': '" +
                                                            r.subpatch + "'");
        check(r.box == 3, "box 3, counting object boxes in file order: " + std::to_string(r.box));
        const auto lines = w.describe();
        check(lines.size() == 1 && lines[0] == "unknown object 'cyclone/cycle~' in the patch, pd lfo, box 3",
              "described as: " + (lines.empty() ? std::string("-") : lines[0]));
    }
}

void testAbstractionsAreWalkedOnce() {
    section("PD4 -- stored abstractions are walked, once each, by [name] or [clone]");
    const PdAbstractions abs{
        {"voice.pd", std::string(kHeader) + "#X obj 10 10 inlet~;\n#X obj 10 40 lop~ 1000;\n"
                                            "#X obj 10 70 unknown~;\n#X obj 10 100 outlet~;\n"},
        {"env", std::string(kHeader) + "#X obj 10 10 inlet;\n#X obj 10 40 outlet;\n"},
    };
    const std::string p = std::string(kHeader) +
                          "#X obj 10 10 voice;\n#X obj 10 40 voice;\n#X obj 10 70 clone 8 voice;\n"
                          "#X obj 10 100 env;\n";
    const auto w = walkPdObjects(p, abs, small());
    check(w.abstractions == std::vector<std::string>{"voice.pd", "env"},
          "voice.pd (named as [voice]) and env, in first-use order");
    check(w.refused.size() == 1, "voice's one unknown box is reported once, not three times: " +
                                     std::to_string(w.refused.size()));
    if (!w.refused.empty())
        check(w.refused[0].file == "voice.pd" && w.refused[0].box == 3 &&
                  w.refused[0].className == "unknown~",
              "in abstraction 'voice.pd', box 3");
    check(w.objects == 4 + 4 + 2, "4 + voice's 4 + env's 2 boxes: " + std::to_string(w.objects));

    for (const char* clone : {"clone -s 1 voice 8", "clone 8 voice", "clone voice 8", "clone -x -d 4 voice"}) {
        const std::string q = std::string(kHeader) + "#X obj 10 10 " + clone + ";\n";
        const PdAbstractions clean{{"voice", std::string(kHeader) + "#X obj 1 1 inlet~;\n"}};
        const auto c = walkPdObjects(q, clean, small());
        check(c.allowed() && c.abstractions.size() == 1, std::string("[") + clone + "] walks 'voice'");
    }
    const auto missing = walkPdObjects(std::string(kHeader) + "#X obj 10 10 clone 4 nothere;\n", {}, small());
    check(!missing.allowed() && missing.refused[0].className == "nothere",
          "[clone 4 nothere] is refused, naming 'nothere'");
}

void testWhatCannotBeKnownStatically() {
    section("PD4 -- recursion, run-time class names and broken canvases are refused");
    const PdAbstractions loop{{"a", std::string(kHeader) + "#X obj 1 1 b;\n"},
                              {"b", std::string(kHeader) + "#X obj 1 1 a;\n"}};
    const auto r = walkPdObjects(std::string(kHeader) + "#X obj 1 1 a;\n", loop, small());
    check(r.refused.size() == 1 && r.refused[0].reason == PdObjectRefusal::Recursive &&
              r.refused[0].file == "b",
          "a -> b -> a: refused inside 'b' as recursive");

    const auto d = walkPdObjects(std::string(kHeader) + "#X obj 1 1 \\$1~;\n#X obj 1 1 clone 4 \\$2;\n",
                                 {}, small());
    check(d.refused.size() == 2 && d.refused[0].reason == PdObjectRefusal::DynamicClass &&
              d.refused[0].className == "$1~" && d.refused[1].reason == PdObjectRefusal::DynamicClass,
          "[$1~] and [clone 4 $2]: decided only when Pd runs");

    const auto m = walkPdObjects(std::string(kHeader) + "#X obj 1 1 osc~;\n#X restore 1 1 pd x;\n", {},
                                 small());
    check(m.refused.size() == 1 && m.refused[0].reason == PdObjectRefusal::Malformed,
          "a restore with no subpatch open is malformed");
    const auto open = walkPdObjects(std::string(kHeader) + "#N canvas 0 0 1 1 x 0;\n#X obj 1 1 osc~;\n", {},
                                    small());
    check(open.refused.size() == 1 && open.refused[0].reason == PdObjectRefusal::Malformed,
          "a subpatch never closed is malformed");

    std::string deep = kHeader;
    for (int i = 0; i < kPdWalkMaxDepth + 2; ++i) deep += "#N canvas 0 0 1 1 s 0;\n";
    deep += "#X obj 1 1 osc~;\n";
    for (int i = 0; i < kPdWalkMaxDepth + 2; ++i) deep += "#X restore 1 1 pd s;\n";
    const auto t = walkPdObjects(deep, {}, small());
    check(t.refused.size() == 1 && t.refused[0].reason == PdObjectRefusal::TooDeep,
          "subpatches nested past the limit are refused, not walked");
}

void testTheTokeniserIsShared() {
    section("PD4 -- the walk and the declaration parse read one tokeniser");
    const std::string p = std::string(kHeader) +
                          "#X obj 10 10 adi.param \\$0 1 0 1 0.5 - lin Gain;\n"
                          "#X obj 10 40 osc~ 1\\;2;\n";
    const auto records = pdRecords(p);
    check(records.size() == 3, "three records: the canvas and two objects");
    check(records.size() == 3 && records[1][5] == "$0", "an escaped \\$0 is the atom $0");
    check(records.size() == 3 && records[2].size() == 6 && records[2][5] == "1;2",
          "an escaped ; stays inside its atom");
    const auto decls = parsePdDeclarations(p);
    check(decls.params.size() == 1 && decls.params[0].box == 1, "the declaration parse still finds box 1");
}

#if defined(ADI_WITH_PD)
std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void testTheRunningBuildsVocabulary() {
    section("PD4 -- the vocabulary is read from the running build");
    const PdVocabulary v(PdRuntime::creatableClasses());
    check(v.size() > 150, "vanilla's classes and aliases are there: " + std::to_string(v.size()));
    for (const char* name : {"osc~", "metro", "pd", "clone", "inlet~", "outlet~", "f", "float", "t",
                             "trigger", "expr~", "vline~", "tabread4~", "declare", "list", "symbol",
                             "bang", "adi.eq_eight~",
                             "adi.colorcab~"})
        check(v.contains(name), std::string("has [") + name + "]");
    for (const char* name : {"adi.param", "cyclone/cycle~", "zexy/z~", "nonexistent~"})
        check(!v.contains(name), std::string("has no [") + name + "] (an abstraction or not built in)");

    // Every patch ADI ships walks clean against it, with ADI's own
    // abstractions stored as a device would store them.
    const std::filesystem::path dir = ADI_PD_PATCH_DIR;
    PdAbstractions abs;
    for (const auto& folder : {dir, dir / "devices"})
        for (const auto& e : std::filesystem::directory_iterator(folder))
            if (e.path().extension() == ".pd" && e.path().filename().string().rfind("adi.", 0) == 0)
                abs.emplace(e.path().filename().string(), slurp(e.path()));
    int walked = 0;
    for (const auto& folder : {dir, dir / "devices"})
        for (const auto& e : std::filesystem::directory_iterator(folder)) {
            if (e.path().extension() != ".pd") continue;
            const auto w = walkPdObjects(slurp(e.path()), abs, v);
            ++walked;
            std::string why;
            for (const auto& line : w.describe()) why += " " + line + ";";
            check(w.allowed(), e.path().filename().string() + " walks clean (" +
                                   std::to_string(w.objects) + " boxes)" + why);
        }
    check(walked >= 20, "every shipped patch walked: " + std::to_string(walked));
}
#endif

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_pd_object_walk_tests -- PD4, the allowed-object walk\n");
    testAFlatPatch();
    testAnUnknownObjectIsNamedWhereItIs();
    testAbstractionsAreWalkedOnce();
    testWhatCannotBeKnownStatically();
    testTheTokeniserIsShared();
#if defined(ADI_WITH_PD)
    testTheRunningBuildsVocabulary();
#endif
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
