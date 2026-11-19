// SPDX-License-Identifier: GPL-3.0-or-later
//
// DEV-16 step 1 -- the `.amxd` reader and translator (src/juce/pd_amxd.*).
//
// Every device here is written by this test: a Max patcher in JSON, wrapped in
// the `ampf`/`meta`/`ptch` chunks an `.amxd` file is. No Max for Live device
// from anywhere else is read. With Pd built, each translation is walked by PD4
// against the running build and opened in libpd, and its sound is checked
// against what the Max patch does: a gain of 0.5 is input x 0.5.

#include "juce/pd_amxd.hpp"
#include "juce/pd_declarations.hpp"
#include "juce/pd_object_walk.hpp"
#if defined(ADI_WITH_PD)
#include "adi/engine/graph.hpp"
#include "juce/pd_device.hpp"
#include "juce/pd_engine.hpp"
#endif

#include <cmath>
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

void chunk(std::vector<std::uint8_t>& out, const char* tag, const std::string& body) {
    out.insert(out.end(), tag, tag + 4);
    const auto n = static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    out.insert(out.end(), body.begin(), body.end());
}

/// An .amxd: ampf (kind), meta, ptch (the JSON, NUL-terminated as Max writes it).
std::vector<std::uint8_t> amxd(const std::string& patcherJson, const char* kind = "aaaa",
                               const char* patchTag = "ptch") {
    std::vector<std::uint8_t> b;
    chunk(b, "ampf", kind);
    chunk(b, "meta", std::string(4, '\0'));
    chunk(b, patchTag, patcherJson + std::string(1, '\0'));
    return b;
}

std::string box(const std::string& id, const std::string& text, double x, double y,
                const std::string& outlets, const std::string& maxclass = "newobj",
                const std::string& extra = "") {
    return R"({"box":{"id":")" + id + R"(","maxclass":")" + maxclass + R"(","text":")" + text +
           R"(","patching_rect":[)" + std::to_string(x) + "," + std::to_string(y) +
           R"(,50,22],"outlettype":[)" + outlets + "]" + extra + "}}";
}
std::string line(const std::string& s, int o, const std::string& d, int i) {
    return R"({"patchline":{"source":[")" + s + "\"," + std::to_string(o) + R"(],"destination":[")" + d +
           "\"," + std::to_string(i) + "]}}";
}
std::string patcher(const std::vector<std::string>& boxes, const std::vector<std::string>& lines) {
    std::string j = R"({"patcher":{"fileversion":1,"boxes":[)";
    for (std::size_t i = 0; i < boxes.size(); ++i) j += (i ? "," : "") + boxes[i];
    j += R"(],"lines":[)";
    for (std::size_t i = 0; i < lines.size(); ++i) j += (i ? "," : "") + lines[i];
    return j + "]}}";
}

const std::string kSig = R"("signal")";
const std::string kSig2 = R"("signal","signal")";

/// plugin~ -> *~ 0.5 (each side) -> plugout~.
std::string gainDevice() {
    return patcher({box("obj-1", "plugin~", 20, 20, kSig2), box("obj-2", "*~ 0.5", 20, 80, kSig),
                    box("obj-3", "*~ 0.5", 120, 80, kSig), box("obj-4", "plugout~", 20, 140, "")},
                   {line("obj-1", 0, "obj-2", 0), line("obj-1", 1, "obj-3", 0), line("obj-2", 0, "obj-4", 0),
                    line("obj-3", 0, "obj-4", 1)});
}

/// plugin~ -> *~ <- live.dial "Amount" 0..1, 0.25 -> plugout~.
std::string dialDevice() {
    const std::string attrs =
        R"(,"varname":"amount","saved_attribute_attributes":{"valueof":{"parameter_longname":"Amount",)"
        R"("parameter_mmin":0.0,"parameter_mmax":1.0,"parameter_initial_enable":1,"parameter_initial":[0.25],)"
        R"("parameter_type":0,"parameter_unitstyle":5}})";
    return patcher({box("obj-1", "plugin~", 20, 20, kSig2), box("obj-5", "", 200, 20, R"("","float")",
                                                               "live.dial", attrs),
                    box("obj-2", "*~", 20, 80, kSig), box("obj-3", "*~", 120, 80, kSig),
                    box("obj-4", "plugout~", 20, 140, "")},
                   {line("obj-1", 0, "obj-2", 0), line("obj-1", 1, "obj-3", 0), line("obj-5", 0, "obj-2", 1),
                    line("obj-5", 0, "obj-3", 1), line("obj-2", 0, "obj-4", 0), line("obj-3", 0, "obj-4", 1)});
}

void testTheContainer() {
    section("DEV-16 -- the .amxd container");
    const auto ok = translateAmxd(amxd(gainDevice()));
    check(ok.ok() && ok.kind == AmxdKind::AudioEffect, "ampf 'aaaa' + meta + ptch reads as an audio effect");
    const std::vector<std::uint8_t> junk{'R', 'I', 'F', 'F', 0, 0, 0, 0, 0, 0, 0, 0};
    check(translateAmxd(junk).error == "not an .amxd file", "a file that is not ampf is refused");
    check(translateAmxd(amxd(gainDevice(), "aaaa", "ciph")).error.find("encrypted") != std::string::npos,
          "an encrypted (ciph) device is not read");
    check(translateAmxd(amxd("mx@c....", "aaaa")).error.find("frozen") != std::string::npos,
          "a frozen (mx@c) device is refused in this step");
    const auto midi = translateAmxd(amxd(gainDevice(), "mmmm"));
    check(midi.kind == AmxdKind::MidiEffect && !midi.ok(), "a MIDI effect is recognised and not translated yet");
    auto cut = amxd(gainDevice());
    cut.resize(cut.size() - 10);
    check(translateAmxd(cut).error.find("past the end") != std::string::npos, "a truncated chunk is refused");
}

void testTheTranslation() {
    section("DEV-16 -- what a translation says");
    const auto g = translateAmxd(amxd(gainDevice()));
    check(g.pdPatch.find("adc~ 1 2") != std::string::npos && g.pdPatch.find("dac~ 1 2") != std::string::npos,
          "plugin~/plugout~ become adc~/dac~ 1 2, where DEV-10 reads the ports");
    check(g.pdPatch.find("*~ 0.5") != std::string::npos, "*~ 0.5 stays *~ 0.5");

    const auto d = translateAmxd(amxd(dialDevice()));
    check(d.ok() && d.parameters == std::vector<std::string>{"Amount"}, "the live.dial is one parameter, 'Amount'");
    const auto decls = parsePdDeclarations(d.pdPatch);
    check(decls.params.size() == 1 && decls.problems.empty(), "and parses as one [adi.param] with no problem");
    if (decls.params.size() == 1) {
        const auto& p = decls.params[0];
        check(p.id == 1 && p.min == 0.0 && p.max == 1.0 && p.def == 0.25 && p.unit == "%" &&
                  p.curve == PdCurve::Lin && p.name == "Amount",
              "id 1, 0..1, default 0.25, unit %, linear, named Amount");
    }
    check(d.pdPatch.find("#X obj 20 80 *~;") != std::string::npos ||
              d.pdPatch.find("#X obj 20 80 *~ 0;") != std::string::npos,
          "a *~ fed a float on its right stays the scalar form");

    // A message's commas and dollars are escaped for Pd's file format.
    const auto m = translateMaxPatcher(
        patcher({box("obj-1", "set $1, 2", 10, 10, R"("")", "message")}, {}), AmxdKind::AudioEffect);
    check(m.ok() && m.pdPatch.find("#X msg 10 10 set \\$1 \\,  2;") != std::string::npos,
          "a message 'set $1, 2' is written 'set \\$1 \\, 2'");
}

void testRightToLeftIsMadeExplicit() {
    section("DEV-16 -- Max's right-to-left order becomes a [t a a]");
    const auto t = translateMaxPatcher(
        patcher({box("obj-1", "loadbang", 100, 10, R"("bang")"), box("obj-2", "print L", 10, 80, ""),
                 box("obj-3", "print R", 200, 80, "")},
                {line("obj-1", 0, "obj-2", 0), line("obj-1", 0, "obj-3", 0)}),
        AmxdKind::AudioEffect);
    check(t.ok() && t.pdPatch.find(" t a a;") != std::string::npos, "the fan-out goes through [t a a]");
    // Boxes 0,1,2 are loadbang, print L, print R; the trigger is 3. Its right
    // outlet (1) fires first and must reach the right-hand print (2).
    check(t.pdPatch.find("#X connect 3 1 2 0;") != std::string::npos &&
              t.pdPatch.find("#X connect 3 0 1 0;") != std::string::npos,
          "its right outlet feeds the box on the right, its left outlet the box on the left");
    const auto s = translateMaxPatcher(gainDevice(), AmxdKind::AudioEffect);
    check(s.pdPatch.find(" t a") == std::string::npos, "signal connections are never routed through a trigger");
}

void testWhatIsRefusedIsNamed() {
    section("DEV-16 -- what cannot be translated is refused, box by box");
    const auto r = translateMaxPatcher(
        patcher({box("obj-1", "% 1.", 10, 10, R"("float")"), box("obj-2", "cycle~ 440", 10, 50, kSig),
                 box("obj-3", "sig~ 0.1", 100, 10, kSig), box("obj-4", "line~", 10, 90, R"("signal","bang")"),
                 box("obj-5", "zl.group 4", 10, 130, R"("","")"), box("obj-6", "+ 1.", 200, 10, R"("float")")},
                {line("obj-3", 0, "obj-2", 1), line("obj-4", 1, "obj-6", 0)}),
        AmxdKind::AudioEffect);
    check(!r.ok() && r.pdPatch.empty(), "nothing is produced when any box is refused");
    std::vector<std::string> ids;
    for (const auto& b : r.refused) ids.push_back(b.id + ":" + b.maxClass);
    check(ids == std::vector<std::string>{"obj-1:%", "obj-2:cycle~", "obj-4:line~", "obj-5:zl.group"},
          "refused: [% 1.] (float modulo), [cycle~] with its phase inlet in use, [line~]'s end bang, [zl.group]; "
          "[+ 1.] and [sig~] pass");
    if (!r.refused.empty())
        check(r.refused[0].reason.find("modulo") != std::string::npos, "with the reason: " + r.refused[0].reason);
    // Inside a subpatcher, a refusal names the path to it.
    const std::string inner = R"(,"patcher":{"boxes":[)" + box("obj-1", "zl.len", 10, 10, R"("")") + R"(],"lines":[]})";
    const auto sub = translateMaxPatcher(patcher({box("obj-9", "p helper", 10, 10, "", "newobj", inner)}, {}),
                                         AmxdKind::AudioEffect);
    check(sub.refused.size() == 1 && sub.refused[0].id == "p helper/obj-1",
          "a box inside [p helper] is named 'p helper/obj-1'");
}

#if defined(ADI_WITH_PD)
std::vector<float> render(const std::string& pdText, float& gainOut, int& latency, std::string& error) {
    const auto dir = std::filesystem::temp_directory_path() / "adi-amxd-test";
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "device.pd", std::ios::binary) << pdText; }
    LibPdEngine eng(dir.string(), "device.pd", 2, 2);
    eng.addSearchPath(ADI_PD_PATCH_DIR);
    PdLatencyReceiver lat;
    if (!eng.open(lat, error)) return {};
    constexpr int kBlock = 256, kBlocks = 16;
    eng.prepare(48000.0, kBlock);
    latency = eng.adapterLatencySamples();
    std::vector<float> in(kBlock * kBlocks), outL(in.size()), outR(in.size());
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * 441.0 * static_cast<double>(i) / 48000.0));
    std::vector<float> scratchL(kBlock), scratchR(kBlock), oL(kBlock), oR(kBlock);
    for (int b = 0; b < kBlocks; ++b) {
        std::copy_n(in.begin() + b * kBlock, kBlock, scratchL.begin());
        std::copy_n(in.begin() + b * kBlock, kBlock, scratchR.begin());
        const float* ip[2] = {scratchL.data(), scratchR.data()};
        float* op[2] = {oL.data(), oR.data()};
        adi::engine::NodeIo io;
        io.in = ip;
        io.out = op;
        io.channels = 2;
        io.frames = kBlock;
        eng.process(io);
        std::copy(oL.begin(), oL.end(), outL.begin() + b * kBlock);
    }
    // The gain the patch applied, read where the delayed input is loud.
    double num = 0, den = 0;
    for (std::size_t i = static_cast<std::size_t>(latency) + 512; i < in.size(); ++i) {
        num += static_cast<double>(outL[i]) * in[i - static_cast<std::size_t>(latency)];
        den += static_cast<double>(in[i - static_cast<std::size_t>(latency)]) * in[i - static_cast<std::size_t>(latency)];
    }
    gainOut = static_cast<float>(num / den);
    std::vector<float> residue;
    for (std::size_t i = static_cast<std::size_t>(latency) + 512; i < in.size(); ++i)
        residue.push_back(outL[i] - gainOut * in[i - static_cast<std::size_t>(latency)]);
    return residue;
}

void testTranslationsWalkAndPlay() {
    section("DEV-16 -- translations pass PD4's walk and play as the Max patch does");
    std::string slurped;
    {
        std::ifstream f(std::filesystem::path(ADI_PD_PATCH_DIR) / "adi.param.pd", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        slurped = ss.str();
    }
    const PdVocabulary vocab(PdRuntime::creatableClasses());
    const PdAbstractions abs{{"adi.param.pd", slurped}};
    struct Case { const char* name; std::string json; float gain; };
    for (const Case& c : {Case{"plugin~ -> *~ 0.5 -> plugout~", gainDevice(), 0.5f},
                          Case{"plugin~ -> *~ <- live.dial 0.25 -> plugout~", dialDevice(), 0.25f}}) {
        const auto t = translateAmxd(amxd(c.json));
        const auto w = walkPdObjects(t.pdPatch, abs, vocab);
        check(t.ok() && w.allowed(), std::string(c.name) + ": translated and walks clean");
        float gain = 0;
        int latency = -1;
        std::string error;
        const auto residue = render(t.pdPatch, gain, latency, error);
        double worst = 0;
        for (float r : residue) worst = std::max(worst, std::fabs(static_cast<double>(r)));
        check(error.empty() && !residue.empty() && std::fabs(gain - c.gain) < 1e-6 && worst < 1e-6,
              std::string(c.name) + ": opens in libpd and plays input x " + std::to_string(gain) + " at " +
                  std::to_string(latency) + " samples (worst residue " + std::to_string(worst) + ")" +
                  (error.empty() ? "" : " -- " + error));
    }
}

/// Opens a translated control patch and returns what its [print]s printed.
std::vector<std::string> printed(const std::string& pd) {
    const auto dir = std::filesystem::temp_directory_path() / "adi-amxd-test";
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "control.pd", std::ios::binary) << pd; }
    LibPdEngine eng(dir.string(), "control.pd", 0, 0);
    PdLatencyReceiver lat;
    std::string error;
    std::vector<std::string> out;
    if (!eng.open(lat, error)) return {"open failed: " + error};
    for (const auto& l : eng.consoleLines())
        if (l.rfind("X:", 0) == 0) out.push_back(l);
    return out;
}

/// loadbang -> [message] -> <box> -> [print X], every outlet of the box printed.
std::string probe(const std::string& message, const std::string& text, const std::string& outlets, int outs = 1) {
    std::vector<std::string> boxes{box("obj-1", "loadbang", 10, 10, R"("bang")"),
                                   box("obj-2", message, 10, 40, R"("")", "message"),
                                   box("obj-3", text, 10, 80, outlets)};
    std::vector<std::string> lines{line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0)};
    for (int k = 0; k < outs; ++k) {
        boxes.push_back(box("obj-p" + std::to_string(k), "print X", 10.0 + 100.0 * k, 140, ""));
        lines.push_back(line("obj-3", k, "obj-p" + std::to_string(k), 0));
    }
    return patcher(boxes, lines);
}

void testMaxIntegersAreHeard() {
    section("DEV-16 step 2 -- Max's integers, made explicit with [i], as Pd plays them");
    struct Case { const char* what; std::string json; std::vector<std::string> expect; };
    const std::vector<Case> cases{
        {"[+ 1] fed 2.7 gives 3 (Max truncates its input)", probe("2.7", "+ 1", R"("int")"), {"X: 3"}},
        {"[+ 1.] fed 2.7 gives 3.7 (the float form)", probe("2.7", "+ 1.", R"("float")"), {"X: 3.7"}},
        {"[/ 2] fed 7 gives 3 (integer division)", probe("7", "/ 2", R"("int")"), {"X: 3"}},
        {"[> 0] fed 0.5 gives 0 (0 > 0 in ints)", probe("0.5", "> 0", R"("int")"), {"X: 0"}},
        {"[pack] fed 2.9 gives the list 2 0", probe("2.9", "pack", R"("")"), {"X: 2 0"}},
        {"[pack 0 0.] fed 2.9 gives 2 0", probe("2.9", "pack 0 0.", R"("")"), {"X: 2 0"}},
        {"[unpack] fed 3.7 4.2 gives 4 then 3, right to left", probe("3.7 4.2", "unpack", R"("int","int")", 2),
         {"X: 4", "X: 3"}},
        {"[t i f] fed 2.5 gives 2.5 then 2", probe("2.5", "t i f", R"("int","float")", 2), {"X: 2.5", "X: 2"}},
    };
    for (const auto& c : cases) {
        const auto t = translateMaxPatcher(c.json, AmxdKind::AudioEffect);
        const auto got = t.ok() ? printed(t.pdPatch) : std::vector<std::string>{"refused"};
        std::string shown;
        for (const auto& g : got) shown += (shown.empty() ? "" : " | ") + g;
        check(got == c.expect, std::string(c.what) + ": " + shown);
    }
}

void testSubpatchersCarrySignalsAndControls() {
    section("DEV-16 step 2 -- [p name] becomes [pd name], with ~ ports where signals cross");
    // [p half]: inlet -> *~ 0.5 -> outlet, between plugin~ and plugout~.
    const std::string half = R"(,"patcher":{"boxes":[)" + box("obj-1", "", 10, 10, R"("")", "inlet") + "," +
                             box("obj-2", "*~ 0.5", 10, 50, kSig) + "," + box("obj-3", "", 10, 90, "", "outlet") +
                             R"(],"lines":[)" + line("obj-1", 0, "obj-2", 0) + "," + line("obj-2", 0, "obj-3", 0) +
                             "]}";
    const std::string json =
        patcher({box("obj-1", "plugin~", 20, 20, kSig2), box("obj-2", "p half", 20, 80, kSig, "newobj", half),
                 box("obj-3", "p half", 120, 80, kSig, "newobj", half), box("obj-4", "plugout~", 20, 140, "")},
                {line("obj-1", 0, "obj-2", 0), line("obj-1", 1, "obj-3", 0), line("obj-2", 0, "obj-4", 0),
                 line("obj-3", 0, "obj-4", 1)});
    const auto t = translateMaxPatcher(json, AmxdKind::AudioEffect);
    check(t.ok() && t.pdPatch.find("#X obj 10 10 inlet~;") != std::string::npos &&
              t.pdPatch.find("#X obj 10 90 outlet~;") != std::string::npos &&
              t.pdPatch.find("#X restore 20 80 pd half;") != std::string::npos,
          "the subpatcher's ports are inlet~/outlet~ and it is restored as [pd half]");
    float gain = 0;
    int latency = -1;
    std::string error;
    const auto residue = render(t.pdPatch, gain, latency, error);
    double worst = 0;
    for (float r : residue) worst = std::max(worst, std::fabs(static_cast<double>(r)));
    check(error.empty() && std::fabs(gain - 0.5f) < 1e-6 && worst < 1e-6,
          "and plays input x " + std::to_string(gain) + " (worst residue " + std::to_string(worst) + ")" + error);

    // A control subpatcher: [p inc] = inlet -> [+ 1] -> outlet.
    const std::string inc = R"(,"patcher":{"boxes":[)" + box("obj-1", "", 10, 10, R"("")", "inlet") + "," +
                            box("obj-2", "+ 1", 10, 50, R"("int")") + "," + box("obj-3", "", 10, 90, "", "outlet") +
                            R"(],"lines":[)" + line("obj-1", 0, "obj-2", 0) + "," + line("obj-2", 0, "obj-3", 0) +
                            "]}";
    const auto c = translateMaxPatcher(probe("1.5", "p inc", R"("")"), AmxdKind::AudioEffect);
    std::string withInc = probe("1.5", "p inc", R"("")");
    const auto pos = withInc.find(R"("text":"p inc")");
    if (pos != std::string::npos) {
        const auto end = withInc.find('}', pos);
        withInc.insert(end, inc);
    }
    const auto ci = translateMaxPatcher(withInc, AmxdKind::AudioEffect);
    check(!c.ok(), "a [p] with no patcher in it is refused");
    check(ci.ok() && printed(ci.pdPatch) == std::vector<std::string>{"X: 2"},
          "[p inc] (inlet -> [+ 1] -> outlet) fed 1.5 prints 2, through a control inlet and outlet");
}

/// The left output of a translated device, with silence in.
std::vector<float> renderOut(const std::string& pdText, int frames, int& latency, std::string& error) {
    const auto dir = std::filesystem::temp_directory_path() / "adi-amxd-test";
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "ramp.pd", std::ios::binary) << pdText; }
    LibPdEngine eng(dir.string(), "ramp.pd", 2, 2);
    PdLatencyReceiver lat;
    if (!eng.open(lat, error)) return {};
    constexpr int kBlock = 256;
    eng.prepare(48000.0, kBlock);
    latency = eng.adapterLatencySamples();
    std::vector<float> zeros(kBlock, 0.f), l(kBlock), r(kBlock), out;
    for (int done = 0; done < frames; done += kBlock) {
        const float* ip[2] = {zeros.data(), zeros.data()};
        float* op[2] = {l.data(), r.data()};
        adi::engine::NodeIo io;
        io.in = ip;
        io.out = op;
        io.channels = 2;
        io.frames = kBlock;
        eng.process(io);
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

void testLineSegmentsAreChained() {
    section("DEV-16 step 3 -- Max's [line~] list becomes chained [vline~] segments");
    const std::string json = patcher(
        {box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "0 0 1 100 0 200", 10, 40, R"("")", "message"),
         box("obj-3", "line~", 10, 80, R"("signal","bang")"), box("obj-4", "plugout~", 10, 120, "")},
        {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0), line("obj-3", 0, "obj-4", 0),
         line("obj-3", 0, "obj-4", 1)});
    const auto t = translateMaxPatcher(json, AmxdKind::AudioEffect);
    check(t.ok() && t.pdPatch.find("pd max-line~") != std::string::npos, "translated through [pd max-line~]");
    int latency = 0;
    std::string error;
    const auto out = renderOut(t.pdPatch, 48000 / 2, latency, error);
    const auto at = [&](double ms) {
        const auto i = static_cast<std::size_t>(latency) + static_cast<std::size_t>(std::lround(ms * 48.0));
        return i < out.size() ? static_cast<double>(out[i]) : -99.0;
    };
    const std::pair<double, double> expect[] = {{0, 0.0}, {50, 0.5}, {100, 1.0}, {200, 0.5}, {300, 0.0}, {400, 0.0}};
    for (const auto& [ms, v] : expect)
        check(error.empty() && std::fabs(at(ms) - v) < 1e-3,
              "at " + std::to_string(static_cast<int>(ms)) + " ms: " + std::to_string(at(ms)) + " (Max: " +
                  std::to_string(v) + ")" + error);
}

void testTheOrderIsHeard() {
    section("DEV-16 -- and the right-to-left order is what Pd does");
    const auto t = translateMaxPatcher(
        patcher({box("obj-1", "loadbang", 100, 10, R"("bang")"), box("obj-2", "print L", 10, 80, ""),
                 box("obj-3", "print R", 200, 80, "")},
                {line("obj-1", 0, "obj-2", 0), line("obj-1", 0, "obj-3", 0)}),
        AmxdKind::AudioEffect);
    const auto dir = std::filesystem::temp_directory_path() / "adi-amxd-test";
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "order.pd", std::ios::binary) << t.pdPatch; }
    LibPdEngine eng(dir.string(), "order.pd", 0, 0);
    PdLatencyReceiver lat;
    std::string error;
    check(eng.open(lat, error), "the order patch opens: " + error);
    std::vector<std::string> prints;
    for (const auto& l : eng.consoleLines())
        if (l.rfind("L:", 0) == 0 || l.rfind("R:", 0) == 0) prints.push_back(l.substr(0, 1));
    check(prints == std::vector<std::string>{"R", "L"}, "the loadbang reaches the right-hand print first");
}
#endif

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_pd_amxd_tests -- DEV-16 step 1, the .amxd translator\n");
    testTheContainer();
    testTheTranslation();
    testRightToLeftIsMadeExplicit();
    testWhatIsRefusedIsNamed();
#if defined(ADI_WITH_PD)
    testTranslationsWalkAndPlay();
    testTheOrderIsHeard();
    testMaxIntegersAreHeard();
    testSubpatchersCarrySignalsAndControls();
    testLineSegmentsAreChained();
#endif
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
