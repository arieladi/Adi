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
        patcher({box("obj-1", "+ 1", 10, 10, R"("int")"), box("obj-2", "cycle~ 440", 10, 50, kSig),
                 box("obj-3", "sig~ 0.1", 100, 10, kSig), box("obj-4", "line~", 10, 90, kSig),
                 box("obj-5", "zl.group 4", 10, 130, R"("","")"), box("obj-6", "+ 1.", 200, 10, R"("float")")},
                {line("obj-3", 0, "obj-2", 1)}),
        AmxdKind::AudioEffect);
    check(!r.ok() && r.pdPatch.empty(), "nothing is produced when any box is refused");
    std::vector<std::string> ids;
    for (const auto& b : r.refused) ids.push_back(b.id + ":" + b.maxClass);
    check(ids == std::vector<std::string>{"obj-1:+", "obj-2:cycle~", "obj-4:line~", "obj-5:zl.group"},
          "refused: [+ 1] (int), [cycle~] with its phase inlet in use, [line~], [zl.group]; [+ 1.] and [sig~] pass");
    if (!r.refused.empty())
        check(r.refused[0].reason.find("integer") != std::string::npos, "with the reason: " + r.refused[0].reason);
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
#endif
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
