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
#include "temp_directory.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
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

void testAClassNamedByAPathIsRefused() {
    section("PD4 -- a class named by an absolute or parent path is refused by name");
    for (const char* name : {"/tmp/evil", "../evil", "a/../../evil", "C:/x/evil", "c:\\x\\evil", "\\\\server\\evil"}) {
        const std::string p = std::string(kHeader) + "#X obj 10 10 osc~;\n#X obj 10 40 " + name + ";\n";
        const PdAbstractions stored{{name, std::string(kHeader) + "#X obj 1 1 inlet~;\n"}};
        const auto w = walkPdObjects(p, stored, small());
        check(!w.allowed() && w.refused.size() == 1 && w.refused[0].reason == PdObjectRefusal::PathNamed &&
                  w.refused[0].box == 2,
              std::string("[") + name + "] is refused as path-named, even with an abstraction stored by that name");
        const auto lines = w.describe();
        check(!lines.empty() && lines[0].find("named by a path") != std::string::npos,
              "described as: " + (lines.empty() ? std::string("-") : lines[0]));
    }
    // [/] is division, a class this build makes, not a path.
    PdVocabulary withDivide({"osc~", "/"});
    check(walkPdObjects(std::string(kHeader) + "#X obj 10 10 / 2;\n", {}, withDivide).allowed(), "[/ 2] (division) is allowed");
    // A plain subfolder name stays an ordinary name, resolved against what is stored.
    const PdAbstractions lib{{"mylib/voice", std::string(kHeader) + "#X obj 1 1 inlet~;\n"}};
    check(walkPdObjects(std::string(kHeader) + "#X obj 10 10 mylib/voice;\n", lib, small()).allowed(),
          "[mylib/voice] with that abstraction stored is allowed");
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

// PD2 (plugdata's test scenarios): a seeded patch-text fuzz. Every shipped
// patch is mutated -- bytes cut, hostile tokens put in, lines duplicated,
// swapped or cut short -- and each mutant goes through the walk, which must
// return and name every refusal. Each mutant the walk allows is then opened
// in Pd, prepared and run, as DEV-10's loader would: it must open and run, or
// fail with a message; never crash. A fixed seed and a generator of our own
// (not <random>'s distributions, which differ between libraries): the same
// mutants on every platform, every run.
struct FuzzRandom {
    std::uint64_t state;
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
};

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto stop = end == std::string::npos ? text.size() : end + 1;
        lines.push_back(text.substr(start, stop - start));
        start = stop;
    }
    return lines;
}

std::string mutate(std::string text, FuzzRandom& random) {
    static const char* const kTokens[] = {
        "#X obj 10 10 ", "#N canvas 0 0 100 100 12;\n", "#X restore 0 0 pd sub;\n", "$0", "$1-x", "\\;", ";",
        "\\,", ",", "\n", "#X connect 0 0 999 0;\n", "#X connect 999 7 0 0;\n", "#X connect -1 0 0 0;\n",
        "-1e+38", "nan", "inf", "99999999999999999999", "#A set 1 2 3;\n", "#X array a 100 float 0;\n",
        "#X coords 0 1 100 -1 200 140 1;\n", "pd ", "clone 4 ", "../x ", "/abs ", "C:/x ", "#X msg 10 10 ",
        "#X text 0 0 ", "#X floatatom 0 0 5 0 0 0 - - -;\n", "{", "}", "\\$0", "#X obj 0 0 f;\n",
        "#X obj 0 0 adi.param \\$0 1 x 0 1 0 lin -;\n", "#X obj 0 0 osc~ 440;\n", "#X obj 0 0 dac~;\n",
        "#X obj 0 0 osc~ foo;\n", "#X connect 0 100000 1 0;\n", "#X connect 0 0 1 100000;\n"};
    const std::size_t operations = 1 + random.below(4);
    for (std::size_t op = 0; op < operations; ++op) {
        const std::size_t at = random.below(text.size() + 1);
        const std::size_t kind = random.below(6);
        if (kind == 0) {
            text.erase(at, 1 + random.below(16));
        } else if (kind == 1) {
            text.insert(at, kTokens[random.below(std::size(kTokens))]);
        } else if (kind == 2) {
            text.insert(at, std::string(1 + random.below(400), 'a'));
        } else if (kind == 5) {
            text.resize(at);
        } else {
            auto lines = linesOf(text);
            if (lines.size() < 2) {
                continue;
            }
            const std::size_t a = random.below(lines.size()), b = random.below(lines.size());
            if (kind == 3) {
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(b), lines[a]);
            } else {
                std::swap(lines[a], lines[b]);
            }
            text.clear();
            for (const auto& line : lines) {
                text += line;
            }
        }
    }
    return text;
}

// What the fuzz found first: `#X connect` on a canvas with no objects walked
// off Pd's empty object list and crashed the process (canvas_connect).
void testAConnectOnAnEmptyCanvasFailsByName() {
    section("PD2 -- a connection on a canvas with no objects fails by name, no crash");
    adi::test::TempDirectory tmp("pd_object_walk", "connect");
    {
        std::ofstream file(tmp.path() / "empty.pd", std::ios::binary | std::ios::trunc);
        file << "#N canvas 0 0 100 100 12;\n#X connect 0 0 0 0;\n#X connect 8 0 1 0;\n#X connect -1 0 0 0;\n";
    }
    LibPdEngine eng(tmp.path().string(), "empty.pd", 2, 2);
    PdLatencyReceiver latency;
    std::string error;
    const bool opened = eng.open(latency, error);
    std::string console;
    for (const auto& line : eng.consoleLines()) {
        console += line + "\n";
    }
    std::size_t failed = 0;
    for (auto at = console.find("connection failed"); at != std::string::npos;
         at = console.find("connection failed", at + 1)) {
        ++failed;
    }
    check(opened && failed == 3, "the patch opens and its three connections fail on the console (" +
                                     std::to_string(failed) + ")\n          console: " + console);
}

// What the review of the fuzz found: a box that failed to create ([osc~ foo])
// was given dummy ports up to a connection's number, unbounded: 1e8 outlets,
// each by walking the list -- a hang, then a NULL write without memory.
void testAPortPastTheBoundOnABrokenBoxFails() {
    section("PD2 -- a connection to port 1e8 of a box that failed to create fails, no hang");
    adi::test::TempDirectory tmp("pd_object_walk", "ports");
    {
        std::ofstream file(tmp.path() / "ports.pd", std::ios::binary | std::ios::trunc);
        file << "#N canvas 0 0 100 100 12;\n#X obj 0 0 osc~ foo;\n#X obj 0 40 osc~ bar;\n"
                "#X connect 0 100000000 1 0;\n#X connect 0 0 1 100000000;\n#X connect 0 3 1 2;\n";
    }
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 400 && !done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!done.load()) {
            std::printf("  FAIL  a connection to port 1e8 of a broken box hung the open (20 s)\n");
            std::fflush(stdout);
            std::_Exit(3);
        }
    });
    {
        LibPdEngine eng(tmp.path().string(), "ports.pd", 2, 2);
        PdLatencyReceiver latency;
        std::string error;
        const bool opened = eng.open(latency, error);
        std::string console;
        for (const auto& line : eng.consoleLines()) {
            console += line + "\n";
        }
        std::size_t failed = 0;
        for (auto at = console.find("connection failed"); at != std::string::npos;
             at = console.find("connection failed", at + 1)) {
            ++failed;
        }
        check(opened && failed == 2,
              "it opens, the two connections past the bound fail on the console, and the one within it is made "
              "(" + std::to_string(failed) + " failed)\n          console: " + console);
    }
    done.store(true);
    watchdog.join();
}

void testASeededFuzzOfPatchTextNeverCrashes() {
    section("PD2 -- a seeded fuzz of patch text: the walk names every refusal, Pd opens or says why");
    const std::filesystem::path dir = ADI_PD_PATCH_DIR;
    // The same bytes in the same order everywhere: a directory's order is
    // unspecified, and a checkout may turn LF into CRLF.
    std::vector<std::filesystem::path> paths;
    for (const auto& folder : {dir, dir / "devices"}) {
        for (const auto& e : std::filesystem::directory_iterator(folder)) {
            if (e.path().extension() == ".pd") {
                paths.push_back(e.path());
            }
        }
    }
    std::sort(paths.begin(), paths.end(), [&](const std::filesystem::path& a, const std::filesystem::path& b) {
        return a.lexically_relative(dir).generic_string() < b.lexically_relative(dir).generic_string();
    });
    PdAbstractions abs;
    std::vector<std::string> corpus;
    for (const auto& path : paths) {
        auto text = slurp(path);
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        if (path.filename().string().rfind("adi.", 0) == 0) {
            abs.emplace(path.filename().string(), text);
        }
        corpus.push_back(text);
    }
    check(corpus.size() >= 20, "the corpus is every shipped patch: " + std::to_string(corpus.size()));
    const PdVocabulary vocabulary(PdRuntime::creatableClasses());
    adi::test::TempDirectory tmp("pd_object_walk", "fuzz");

    constexpr int kMutants = 600, kMaxOpens = 150;
    FuzzRandom random{20261120u};
    int allowed = 0, refused = 0, unnamed = 0, opens = 0, ran = 0, failedWithMessage = 0, failedSilently = 0;
    std::string firstUnnamed;
    std::vector<std::vector<float>> in(2, std::vector<float>(64, 0.f)), out(2, std::vector<float>(64, 0.f));
    const float* inPointers[2] = {in[0].data(), in[1].data()};
    float* outPointers[2] = {out[0].data(), out[1].data()};
    for (int m = 0; m < kMutants; ++m) {
        const auto mutant = mutate(corpus[random.below(corpus.size())], random);
        const auto walk = walkPdObjects(mutant, abs, vocabulary);
        // Named: one line per refusal, each naming its class (a malformed
        // canvas has none) where the box is.
        const auto lines = walk.describe();
        bool named = lines.size() == walk.refused.size();
        for (std::size_t i = 0; named && i < walk.refused.size(); ++i) {
            const auto& r = walk.refused[i];
            named = r.reason == PdObjectRefusal::Malformed ||
                    (!r.className.empty() && r.box > 0 && lines[i].find(r.className) != std::string::npos);
        }
        if (!named) {
            ++unnamed;
            if (firstUnnamed.empty()) {
                firstUnnamed = "mutant " + std::to_string(m) + ": " + (lines.empty() ? std::string("-") : lines[0]);
            }
        }
        if (!walk.allowed()) {
            ++refused;
            continue;
        }
        ++allowed;
        if (opens >= kMaxOpens) {
            continue;
        }
        ++opens;
        {
            std::ofstream file(tmp.path() / "mutant.pd", std::ios::binary | std::ios::trunc);
            file << mutant;
        }
        LibPdEngine eng(tmp.path().string(), "mutant.pd", 2, 2);
        eng.addSearchPath(dir.string());
        eng.addSearchPath((dir / "devices").string());
        PdLatencyReceiver latency;
        std::string error;
        if (!eng.open(latency, error)) {
            (error.empty() ? failedSilently : failedWithMessage) += 1;
            continue;
        }
        eng.prepare(48000.0, 64);
        adi::engine::NodeIo io;
        io.in = inPointers;
        io.out = outPointers;
        io.channels = 2;
        io.frames = 64;
        for (int block = 0; block < 16; ++block) {
            eng.process(io);
        }
        ++ran;
    }
    check(unnamed == 0, std::to_string(kMutants) + " mutants walked: " + std::to_string(allowed) + " allowed, " +
                            std::to_string(refused) + " refused, every refusal named by class and box (" +
                            std::to_string(unnamed) + " not" + (firstUnnamed.empty() ? "" : "; " + firstUnnamed) + ")");
    check(opens >= 50 && ran + failedWithMessage == opens && failedSilently == 0,
          std::to_string(opens) + " allowed mutants opened in Pd: " + std::to_string(ran) + " ran 16 blocks, " +
              std::to_string(failedWithMessage) + " failed with a message, " + std::to_string(failedSilently) +
              " without one");
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
    testAClassNamedByAPathIsRefused();
    testTheTokeniserIsShared();
#if defined(ADI_WITH_PD)
    testTheRunningBuildsVocabulary();
    testAConnectOnAnEmptyCanvasFailsByName();
    testAPortPastTheBoundOnABrokenBoxFails();
    testASeededFuzzOfPatchTextNeverCrashes();
#endif
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
