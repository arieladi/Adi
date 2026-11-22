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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
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

void testMoreLiveControls() {
    section("DEV-16 step 4 -- live.text (toggle) and live.menu become parameters");
    const std::string toggle =
        R"(,"mode":1,"saved_attribute_attributes":{"valueof":{"parameter_longname":"Bypass","parameter_type":2,)"
        R"("parameter_enum":["off","on"],"parameter_initial_enable":1,"parameter_initial":[1]}})";
    const std::string menu =
        R"(,"saved_attribute_attributes":{"valueof":{"parameter_longname":"Shape","parameter_type":2,)"
        R"("parameter_enum":["Sine","Saw Up","Square"]}})";
    const std::string button = R"(,"mode":0)";
    const auto t = translateMaxPatcher(
        patcher({box("obj-1", "", 10, 10, R"("")", "live.text", toggle), box("obj-2", "", 100, 10, R"("")", "live.menu", menu)},
                {}),
        AmxdKind::AudioEffect);
    const auto d = parsePdDeclarations(t.pdPatch);
    check(t.ok() && d.params.size() == 2 && d.problems.empty(), "two parameters, no problem");
    if (d.params.size() == 2) {
        check(d.params[0].curve == PdCurve::Toggle && d.params[0].def == 1.0 && d.params[0].name == "Bypass",
              "live.text in toggle mode: a toggle, default on, 'Bypass'");
        check(d.params[1].curve == PdCurve::Menu && d.params[1].items ==
                  std::vector<std::string>{"Sine", "Saw Up", "Square"},
              "live.menu: a menu with its three items");
    }
    const auto b = translateMaxPatcher(patcher({box("obj-1", "", 10, 10, R"("")", "live.text", button)}, {}),
                                       AmxdKind::AudioEffect);
    check(!b.ok() && b.refused.size() == 1, "live.text in button mode is refused for now");
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
    check(ids == std::vector<std::string>{"obj-5:zl.group"},
          "refused: [zl.group]; [% 1.] (an integer modulo in Max), [+ 1.], [sig~], [cycle~] with its phase inlet in "
          "use and [line~] with its end bang pass");
    if (!r.refused.empty())
        check(r.refused[0].reason.find("no translation") != std::string::npos, "with the reason: " + r.refused[0].reason);
    // Inside a subpatcher, a refusal names the path to it.
    const std::string inner = R"(,"patcher":{"boxes":[)" + box("obj-1", "zl.len", 10, 10, R"("")") + R"(],"lines":[]})";
    const auto sub = translateMaxPatcher(patcher({box("obj-9", "p helper", 10, 10, "", "newobj", inner)}, {}),
                                         AmxdKind::AudioEffect);
    check(sub.refused.size() == 1 && sub.refused[0].id == "p helper/obj-1",
          "a box inside [p helper] is named 'p helper/obj-1'");
}

/// A patcher's own object (what a [p] or a bpatcher holds), not the file's wrapper.
std::string inside(const std::vector<std::string>& boxes, const std::vector<std::string>& lines) {
    const std::string whole = patcher(boxes, lines);
    return whole.substr(11, whole.size() - 12);   // drop {"patcher": and the last }
}

/// An embedded bpatcher (maxclass bpatcher, embed 1) holding `inner`, with its arguments.
std::string bpatcherBox(const std::string& id, const std::string& name, const std::string& args, double x, double y,
                        const std::string& outlets, const std::string& inner, bool embed = true) {
    return box(id, "", x, y, outlets, "bpatcher",
               R"(,"name":")" + name + R"(","embed":)" + (embed ? "1" : "0") + R"(,"args":[)" + args + "]" +
                   (embed ? R"(,"patcher":)" + inner : std::string()));
}

/// inlet -> <text> -> outlet.
std::string through(const std::string& text, const std::string& outlets) {
    return inside({box("obj-1", "", 10, 10, R"("")", "inlet"), box("obj-2", text, 10, 50, outlets),
                   box("obj-3", "", 10, 90, "", "outlet")},
                  {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0)});
}

/// plugin~ -> a bpatcher (each side, its own arguments) -> plugout~.
std::string bpatcherDevice(const std::string& inner, const std::string& argsL, const std::string& argsR) {
    return patcher({box("obj-1", "plugin~", 20, 20, kSig2), bpatcherBox("obj-2", "gain.maxpat", argsL, 20, 80, kSig, inner),
                    bpatcherBox("obj-3", "gain.maxpat", argsR, 120, 80, kSig, inner),
                    box("obj-4", "plugout~", 20, 140, "")},
                   {line("obj-1", 0, "obj-2", 0), line("obj-1", 1, "obj-3", 0), line("obj-2", 0, "obj-4", 0),
                    line("obj-3", 0, "obj-4", 1)});
}

void testBpatchersTranslate() {
    section("DEV-16 step 5 -- an embedded bpatcher becomes [pd name], its #1 to #9 and #0 replaced as Max does");
    const auto t = translateMaxPatcher(bpatcherDevice(through("*~ #1", kSig), "0.5", "0.25"), AmxdKind::AudioEffect);
    check(t.ok() && t.pdPatch.find("#X obj 10 50 *~ 0.5;") != std::string::npos &&
              t.pdPatch.find("#X obj 10 50 *~ 0.25;") != std::string::npos &&
              t.pdPatch.find("#X restore 20 80 pd gain;") != std::string::npos &&
              t.pdPatch.find("#X obj 10 10 inlet~;") != std::string::npos,
          "[*~ #1] in two instances with arguments 0.5 and 0.25: [*~ 0.5] and [*~ 0.25], each a [pd gain] "
          "with signal ports");
    // A [p] inside a bpatcher takes the bpatcher's arguments; a bpatcher
    // inside it takes its own.
    const std::string withP = inside({box("obj-1", "", 10, 10, R"("")", "inlet"),
                                      box("obj-2", "p deeper", 10, 50, kSig, "newobj", R"(,"patcher":)" + through("*~ #1", kSig)),
                                      box("obj-3", "", 10, 90, "", "outlet")},
                                     {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0)});
    const auto p = translateMaxPatcher(bpatcherDevice(withP, "0.5", "0.5"), AmxdKind::AudioEffect);
    check(p.ok() && p.pdPatch.find("*~ 0.5;") != std::string::npos && p.pdPatch.find("#1") == std::string::npos,
          "a [p] inside a bpatcher replaces #1 with the bpatcher's argument");
    const std::string nested =
        inside({box("obj-1", "", 10, 10, R"("")", "inlet"), box("obj-2", "*~ #1", 10, 40, kSig),
                bpatcherBox("obj-3", "inner.maxpat", "0.25", 10, 70, kSig, through("*~ #1", kSig)),
                box("obj-4", "", 10, 100, "", "outlet")},
               {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0), line("obj-3", 0, "obj-4", 0)});
    const auto n = translateMaxPatcher(bpatcherDevice(nested, "0.5", "0.5"), AmxdKind::AudioEffect);
    check(n.ok() && n.pdPatch.find("#X obj 10 40 *~ 0.5;") != std::string::npos &&
              n.pdPatch.find("#X obj 10 50 *~ 0.25;") != std::string::npos,
          "a bpatcher inside a bpatcher replaces #1 with its own argument");
    // #0: one name per instance, under the device's $0.
    const auto z = translateMaxPatcher(bpatcherDevice(through("s #0-v", R"("")"), "", ""), AmxdKind::AudioEffect);
    check(z.ok() && z.pdPatch.find("s \\$0-bp-obj-2-v;") != std::string::npos &&
              z.pdPatch.find("s \\$0-bp-obj-3-v;") != std::string::npos,
          "[s #0-v] in two instances: [s $0-bp-obj-2-v] and [s $0-bp-obj-3-v]");
    const auto m = translateMaxPatcher(
        patcher({bpatcherBox("obj-2", "msg.maxpat", "7", 10, 10, R"("")",
                             inside({box("obj-1", "#1 #0", 10, 10, R"("")", "message")}, {}))},
                {}),
        AmxdKind::AudioEffect);
    check(m.ok() && m.pdPatch.find("#X msg 10 10 7 \\$0-bp-obj-2;") != std::string::npos,
          "and in a message box: [7 $0-bp-obj-2(");
    // Refused, with the path and the reason.
    const auto missing = translateMaxPatcher(bpatcherDevice(through("*~ #2", kSig), "0.5", "0.5"), AmxdKind::AudioEffect);
    check(!missing.ok() && missing.refused.size() == 2 && missing.refused[0].id == "bpatcher gain/obj-2" &&
              missing.refused[0].reason.find("#2 has no argument") != std::string::npos,
          "#2 with one argument is refused, not guessed: 'bpatcher gain/obj-2'" +
              (missing.refused.empty() ? std::string() : " (" + missing.refused[0].reason + ")"));
    const auto file = translateMaxPatcher(
        patcher({box("obj-1", "plugin~", 20, 20, kSig2), bpatcherBox("obj-2", "elsewhere.maxpat", "", 20, 80, kSig, "", false),
                 box("obj-4", "plugout~", 20, 140, "")},
                {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-4", 0)}),
        AmxdKind::AudioEffect);
    check(!file.ok() && file.refused.size() == 1 && file.refused[0].id == "obj-2" &&
              file.refused[0].maxClass == "bpatcher" &&
              file.refused[0].reason.find("outside the device") != std::string::npos,
          "a bpatcher of a patcher file outside the device is refused, named");
    const auto oddArg = translateMaxPatcher(bpatcherDevice(through("*~ #1", kSig), R"("two words")", "0.5"),
                                            AmxdKind::AudioEffect);
    check(!oddArg.ok() && !oddArg.refused.empty() && oddArg.refused[0].id == "obj-2",
          "an argument with a space in it is refused");
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
        {"[% 3] fed 8.9 gives 2 (8 % 3)", probe("8.9", "% 3", R"("int")"), {"X: 2"}},
        {"[% 2.5] fed 7.3 gives 1 (Max's % is an int modulo: 7 % 2)", probe("7.3", "% 2.5", R"("int")"), {"X: 1"}},
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

/// A device's left output, sampled at the given times (ms) after the latency.
std::vector<double> sampledAt(const std::string& json, const std::vector<double>& ms, std::string& error) {
    const auto t = translateMaxPatcher(json, AmxdKind::AudioEffect);
    if (!t.ok()) {
        error = " (refused: " + (t.refused.empty() ? t.error : t.refused[0].reason) + ")";
        return std::vector<double>(ms.size(), -99.0);
    }
    int latency = 0;
    const auto out = renderOut(t.pdPatch, 48000 / 2, latency, error);
    std::vector<double> v;
    for (const double m : ms) {
        const auto i = static_cast<std::size_t>(latency) + static_cast<std::size_t>(std::lround(m * 48.0));
        v.push_back(i < out.size() ? static_cast<double>(out[i]) : -99.0);
    }
    return v;
}

void checkSamples(const std::string& what, const std::vector<double>& got, const std::vector<double>& ms,
                  const std::vector<double>& expect, const std::string& error) {
    double worst = 0;
    std::string shown;
    for (std::size_t k = 0; k < got.size(); ++k) {
        worst = std::max(worst, std::fabs(got[k] - expect[k]));
        shown += (k ? ", " : "") + std::to_string(static_cast<int>(ms[k])) + " ms " + std::to_string(got[k]);
    }
    check(error.empty() && worst < 1e-3, what + ": " + shown + error);
}

void testLineEnds() {
    section("DEV-16 step 7 -- [line~]'s ramp time, its end bang and its initial value, as Max's reference says");
    // 100 into the right inlet, then 1 into the left (Max's right-to-left
    // order from one loadbang): a ramp over 100 ms. At 200 ms a lone 0 with no
    // new time jumps: the time went back to 0 after it was used.
    {
        const std::string json = patcher(
            {box("obj-1", "loadbang", 100, 10, R"("bang")"), box("obj-2", "1", 10, 40, R"("")", "message"),
             box("obj-3", "100", 200, 40, R"("")", "message"), box("obj-5", "delay 200", 300, 40, R"("bang")"),
             box("obj-6", "0", 300, 60, R"("")", "message"), box("obj-4", "line~", 10, 80, R"("signal","bang")"),
             box("obj-7", "plugout~", 10, 140, "")},
            {line("obj-1", 0, "obj-2", 0), line("obj-1", 0, "obj-3", 0), line("obj-1", 0, "obj-5", 0),
             line("obj-2", 0, "obj-4", 0), line("obj-3", 0, "obj-4", 1), line("obj-5", 0, "obj-6", 0),
             line("obj-6", 0, "obj-4", 0), line("obj-4", 0, "obj-7", 0)});
        const std::vector<double> ms{0, 50, 100, 150, 199, 201, 250};
        std::string error;
        checkSamples("a time of 100 in the right inlet, then 1: a ramp; a lone 0 later jumps",
                     sampledAt(json, ms, error), ms, {0, 0.5, 1, 1, 1, 0, 0}, error);
    }
    // The end bang: line A plays "0 0 1 100"; its right outlet jumps line B
    // to -2, and the output is A + B. Bang after the whole list (100 ms), not
    // after its first segment (0 ms).
    const auto endBang = [](const std::string& later) {
        std::vector<std::string> boxes{
            box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "0 0 1 100", 10, 40, R"("")", "message"),
            box("obj-3", "line~", 10, 80, R"("signal","bang")"), box("obj-4", "-2", 200, 110, R"("")", "message"),
            box("obj-5", "line~", 200, 140, R"("signal","bang")"), box("obj-6", "+~", 10, 180, kSig),
            box("obj-7", "plugout~", 10, 220, "")};
        std::vector<std::string> lines{line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0),
                                       line("obj-3", 1, "obj-4", 0), line("obj-4", 0, "obj-5", 0),
                                       line("obj-3", 0, "obj-6", 0), line("obj-5", 0, "obj-6", 1),
                                       line("obj-6", 0, "obj-7", 0)};
        if (!later.empty()) {   // at 50 ms, a new list cuts the first short
            boxes.push_back(box("obj-8", "delay 50", 120, 10, R"("bang")"));
            boxes.push_back(box("obj-9", later, 120, 40, R"("")", "message"));
            lines.push_back(line("obj-1", 0, "obj-8", 0));
            lines.push_back(line("obj-8", 0, "obj-9", 0));
            lines.push_back(line("obj-9", 0, "obj-3", 0));
        }
        return patcher(boxes, lines);
    };
    {
        const std::vector<double> ms{1, 50, 99, 101, 200};
        std::string error;
        checkSamples("\"0 0 1 100\": the right outlet bangs at 100 ms, when the whole list has played",
                     sampledAt(endBang(""), ms, error), ms, {0.01, 0.5, 0.99, -1, -1}, error);
    }
    {
        // Cut short at 50 ms by "0 100": no bang at 100 ms; one at 150.
        const std::vector<double> ms{60, 120, 149, 151};
        std::string error;
        checkSamples("cut short at 50 ms by \"0 100\": no bang for the cut list, one when the new one ends",
                     sampledAt(endBang("0 100"), ms, error), ms, {0.45, 0.15, 0.005, -2}, error);
    }
    {
        // A lone jump does not bang (an assumption until reading 14 says).
        const std::string json = patcher(
            {box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "1", 10, 40, R"("")", "message"),
             box("obj-3", "line~", 10, 80, R"("signal","bang")"), box("obj-4", "-2", 200, 110, R"("")", "message"),
             box("obj-5", "line~", 200, 140, R"("signal","bang")"), box("obj-6", "+~", 10, 180, kSig),
             box("obj-7", "plugout~", 10, 220, "")},
            {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0), line("obj-3", 1, "obj-4", 0),
             line("obj-4", 0, "obj-5", 0), line("obj-3", 0, "obj-6", 0), line("obj-5", 0, "obj-6", 1),
             line("obj-6", 0, "obj-7", 0)});
        const std::vector<double> ms{0, 10, 100};
        std::string error;
        checkSamples("a lone jump to 1 does not bang (assumed; reading 14)", sampledAt(json, ms, error), ms, {1, 1, 1},
                     error);
    }
    // [line~ 0.5]: 0.5 from the first sample; a ramp to 1 over 100 ms from a
    // loadbang starts there.
    {
        const std::string still =
            patcher({box("obj-3", "line~ 0.5", 10, 80, R"("signal","bang")"), box("obj-7", "plugout~", 10, 140, "")},
                    {line("obj-3", 0, "obj-7", 0)});
        const std::vector<double> ms{0, 100};
        std::string error;
        checkSamples("[line~ 0.5] starts at 0.5", sampledAt(still, ms, error), ms, {0.5, 0.5}, error);
        const std::string ramp = patcher(
            {box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "1 100", 10, 40, R"("")", "message"),
             box("obj-3", "line~ 0.5", 10, 80, R"("signal","bang")"), box("obj-7", "plugout~", 10, 140, "")},
            {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0), line("obj-3", 0, "obj-7", 0)});
        const std::vector<double> ms2{0, 50, 100};
        std::string error2;
        checkSamples("and \"1 100\" from a loadbang ramps from 0.5", sampledAt(ramp, ms2, error2), ms2, {0.5, 0.75, 1},
                     error2);
    }
}

void testStepFourControls() {
    section("DEV-16 step 4 -- prepend, append, f, i, !-, gate, played in libpd");
    struct Case { const char* what; std::string json; std::vector<std::string> expect; };
    // [gate 2]: choose outlet 2, then send 5 through; only outlet 2 prints.
    const std::string gate = patcher(
        {box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "t b b", 10, 30, R"("bang","bang")"),
         box("obj-3", "2", 120, 50, R"("")", "message"), box("obj-4", "5", 10, 50, R"("")", "message"),
         box("obj-5", "gate 2", 10, 80, R"("","")"), box("obj-6", "print Y", 10, 120, ""),
         box("obj-7", "print X", 100, 120, "")},
        {line("obj-1", 0, "obj-2", 0), line("obj-2", 1, "obj-3", 0), line("obj-2", 0, "obj-4", 0),
         line("obj-3", 0, "obj-5", 0), line("obj-4", 0, "obj-5", 1), line("obj-5", 0, "obj-6", 0),
         line("obj-5", 1, "obj-7", 0)});
    const std::vector<Case> cases{
        {"[prepend set] fed 3 gives the message set 3", probe("3", "prepend set", R"("")"), {"X: set 3"}},
        {"[append foo] fed 3 gives 3 foo", probe("3", "append foo", R"("")"), {"X: 3 foo"}},
        {"[f 0.5] banged gives 0.5", probe("bang", "f 0.5", R"("float")"), {"X: 0.5"}},
        {"[i] fed 2.9 gives 2", probe("2.9", "i", R"("int")"), {"X: 2"}},
        {"[!- 10] fed 3.5 gives 7 (int)", probe("3.5", "!- 10", R"("int")"), {"X: 7"}},
        {"[!- 10.] fed 3.5 gives 6.5", probe("3.5", "!- 10.", R"("float")"), {"X: 6.5"}},
        {"[gate 2] set to 2 passes 5 out of its second outlet", gate, {"X: 5"}},
    };
    for (const auto& c : cases) {
        const auto t = translateMaxPatcher(c.json, AmxdKind::AudioEffect);
        const auto got = t.ok() ? printed(t.pdPatch) : std::vector<std::string>{"refused"};
        std::string shown;
        for (const auto& g : got) shown += (shown.empty() ? "" : " | ") + g;
        check(got == c.expect, std::string(c.what) + ": " + shown);
    }
}

void testStepFourSignals() {
    section("DEV-16 step 4 -- selector~, slide~ and biquad~, as Max computes them");
    // [selector~ 2 1]: input 1 (left) passes; the right is muted.
    const std::string sel = patcher(
        {box("obj-1", "plugin~", 20, 20, kSig2), box("obj-2", "selector~ 2 1", 20, 80, kSig),
         box("obj-3", "plugout~", 20, 140, "")},
        {line("obj-1", 0, "obj-2", 1), line("obj-1", 1, "obj-2", 2), line("obj-2", 0, "obj-3", 0),
         line("obj-2", 0, "obj-3", 1)});
    const auto ts = translateMaxPatcher(sel, AmxdKind::AudioEffect);
    float gain = 0;
    int latency = 0;
    std::string error;
    const auto residue = render(ts.pdPatch, gain, latency, error);
    double worst = 0;
    for (float r : residue) worst = std::max(worst, std::fabs(static_cast<double>(r)));
    check(ts.ok() && error.empty() && std::fabs(gain - 1.0f) < 1e-6 && worst < 1e-6,
          "[selector~ 2 1] passes its first input exactly: x " + std::to_string(gain) + error);

    // [slide~ 10 10] of a step to 1: y[n] = 1 - 0.9^(n+1), Max's own formula.
    const std::string slide = patcher(
        {box("obj-1", "sig~ 1", 20, 20, kSig), box("obj-2", "slide~ 10 10", 20, 80, kSig),
         box("obj-3", "plugout~", 20, 140, "")},
        {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0), line("obj-2", 0, "obj-3", 1)});
    const auto tl = translateMaxPatcher(slide, AmxdKind::AudioEffect);
    const auto out = tl.ok() ? renderOut(tl.pdPatch, 512, latency, error) : std::vector<float>{};
    double dev = 0;
    for (int n = 0; n < 40 && static_cast<std::size_t>(latency + n) < out.size(); ++n)
        dev = std::max(dev, std::fabs(out[static_cast<std::size_t>(latency + n)] - (1.0 - std::pow(0.9, n + 1))));
    check(tl.ok() && !out.empty() && dev < 1e-6,
          "[slide~ 10 10] on a step follows 1 - 0.9^(n+1) sample for sample (worst " + std::to_string(dev) + ")" + error);

    // [biquad~] given Max's "1 0 0 -0.5 0" (y = x + 0.5 y1): DC gain 2.
    const std::string bq = patcher(
        {box("obj-1", "loadbang", 120, 10, R"("bang")"), box("obj-2", "1 0 0 -0.5 0", 120, 40, R"("")", "message"),
         box("obj-3", "sig~ 1", 20, 40, kSig), box("obj-4", "biquad~ @smooth 0", 20, 80, kSig),
         box("obj-5", "plugout~", 20, 140, "")},
        {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-4", 0), line("obj-3", 0, "obj-4", 0),
         line("obj-4", 0, "obj-5", 0), line("obj-4", 0, "obj-5", 1)});
    const auto tb = translateMaxPatcher(bq, AmxdKind::AudioEffect);
    const auto bout = tb.ok() ? renderOut(tb.pdPatch, 4096, latency, error) : std::vector<float>{};
    const double settled = bout.empty() ? -1 : bout.back();
    const double first = bout.size() > static_cast<std::size_t>(latency) ? bout[static_cast<std::size_t>(latency)] : -1;
    check(tb.ok() && std::fabs(settled - 2.0) < 1e-5 && std::fabs(first - 1.0) < 1e-6,
          "Max's biquad~ \"1 0 0 -0.5 0\" starts at 1 and settles at 2: " + std::to_string(first) + ", " +
              std::to_string(settled) + error);
    // With its smoothing on (Max's default), coefficients that change at run
    // time are refused: how Max smooths them is not yet read.
    std::string smoothed = bq;
    const auto at0 = smoothed.find("biquad~ @smooth 0");
    if (at0 != std::string::npos) smoothed.replace(at0, std::string("biquad~ @smooth 0").size(), "biquad~");
    const auto tsm = translateMaxPatcher(smoothed, AmxdKind::AudioEffect);
    check(!tsm.ok() && tsm.refused.size() == 1 && tsm.refused[0].reason.find("smooth") != std::string::npos,
          "the same with Max's default smoothing is refused, naming it");
    std::string saved = bq;
    const std::string typed = R"("text":"biquad~ @smooth 0")";
    const auto at1 = saved.find(typed);
    if (at1 != std::string::npos) saved.replace(at1, typed.size(), R"("saved_object_attributes":{"smooth":0},"text":"biquad~")");
    check(translateMaxPatcher(saved, AmxdKind::AudioEffect).ok(), "and smoothing off as a saved attribute translates");
    const auto args = translateMaxPatcher(
        patcher({box("obj-1", "biquad~ 0.5 0.1 0.2 -0.3 0.4", 10, 10, kSig)}, {}), AmxdKind::AudioEffect);
    check(args.ok() && args.pdPatch.find("biquad~ 0.3 -0.4 0.5 0.1 0.2") != std::string::npos,
          "and its arguments a0 a1 a2 b1 b2 become Pd's fb1 fb2 ff1 ff2 ff3 = -b1 -b2 a0 a1 a2");
}

void testBpatchersPlay() {
    section("DEV-16 step 5 -- bpatchers walk, open and play as Max's do");
    std::string slurped;
    {
        std::ifstream f(std::filesystem::path(ADI_PD_PATCH_DIR) / "adi.param.pd", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        slurped = ss.str();
    }
    const PdVocabulary vocab(PdRuntime::creatableClasses());
    const PdAbstractions abs{{"adi.param.pd", slurped}};
    for (const float g : {0.5f, 0.25f}) {
        const auto t = translateAmxd(amxd(bpatcherDevice(through("*~ #1", kSig), g == 0.5f ? "0.5" : "0.25", "1")));
        check(t.ok() && walkPdObjects(t.pdPatch, abs, vocab).allowed(),
              "a gain bpatcher with argument " + std::to_string(g) + ": translated and walks clean");
        float gain = 0;
        int latency = -1;
        std::string error;
        const auto residue = render(t.pdPatch, gain, latency, error);
        double worst = 0;
        for (float r : residue) worst = std::max(worst, std::fabs(static_cast<double>(r)));
        check(error.empty() && !residue.empty() && std::fabs(gain - g) < 1e-6 && worst < 1e-6,
              "and plays input x " + std::to_string(gain) + " (worst residue " + std::to_string(worst) + ")" + error);
    }
    // #0 is the instance's own: two bpatchers that send to [r #0-v] each hear
    // only themselves, so 1 and 2 come out once each (shared, each twice).
    const std::string store = inside({box("obj-1", "", 10, 10, R"("")", "inlet"), box("obj-2", "s #0-v", 10, 50, ""),
                                      box("obj-3", "r #0-v", 100, 10, R"("")"), box("obj-4", "", 100, 90, "", "outlet")},
                                     {line("obj-1", 0, "obj-2", 0), line("obj-3", 0, "obj-4", 0)});
    const auto own = translateMaxPatcher(
        patcher({box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "1", 10, 40, R"("")", "message"),
                 bpatcherBox("obj-3", "store.maxpat", "", 10, 80, R"("")", store), box("obj-4", "print X", 10, 140, ""),
                 box("obj-5", "loadbang", 200, 10, R"("bang")"), box("obj-6", "2", 200, 40, R"("")", "message"),
                 bpatcherBox("obj-7", "store.maxpat", "", 200, 80, R"("")", store), box("obj-8", "print X", 200, 140, "")},
                {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0), line("obj-3", 0, "obj-4", 0),
                 line("obj-5", 0, "obj-6", 0), line("obj-6", 0, "obj-7", 0), line("obj-7", 0, "obj-8", 0)}),
        AmxdKind::AudioEffect);
    auto heard = own.ok() ? printed(own.pdPatch) : std::vector<std::string>{"refused"};
    std::sort(heard.begin(), heard.end());
    std::string shown;
    for (const auto& h : heard) shown += (shown.empty() ? "" : " | ") + h;
    check(heard == std::vector<std::string>{"X: 1", "X: 2"}, "two instances' [s #0-v] -> [r #0-v] stay apart: " + shown);
    // A float argument stays a float, as in Max: [+ #1] with 2. is the float form.
    const std::vector<std::pair<std::string, std::string>> floats{{"2.0", "X: 2.5"}, {"2", "X: 2"}};
    for (const auto& [arg, expect] : floats) {
        const auto a = translateMaxPatcher(
            patcher({box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "0.5", 10, 40, R"("")", "message"),
                     bpatcherBox("obj-3", "add.maxpat", arg, 10, 80, R"("")", through("+ #1", R"("")")),
                     box("obj-4", "print X", 10, 140, "")},
                    {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0), line("obj-3", 0, "obj-4", 0)}),
            AmxdKind::AudioEffect);
        const auto got = a.ok() ? printed(a.pdPatch) : std::vector<std::string>{"refused"};
        check(got == std::vector<std::string>{expect},
              "[+ #1] with argument " + arg + " fed 0.5 prints " + (got.empty() ? std::string("nothing") : got[0]));
    }
}

void testCyclePhaseIsAnOffset() {
    section("DEV-16 step 6 -- [cycle~]'s phase inlet offsets the phase, as in Max");
    // [cycle~ 375] (128 samples a period at 48 kHz, an exact phase step)
    // with 0.25 in its phase inlet: cos(2 pi (375 n / 48000 + 0.25)) from the
    // first sample. As a float, the offset then moves to 0.5 at 100 ms (sample
    // 4,800, half way through a period): an offset gives cos(2 pi (f t + 0.5))
    // from there, where a reset of the phase (Pd's [osc~]) would give
    // cos(2 pi (f (t - 0.1) + 0.5)), half a period away.
    for (const bool signal : {true, false}) {
        const std::string json =
            signal ? patcher({box("obj-1", "sig~ 0.25", 120, 20, kSig), box("obj-2", "cycle~ 375", 20, 80, kSig),
                              box("obj-3", "plugout~", 20, 140, "")},
                             {line("obj-1", 0, "obj-2", 1), line("obj-2", 0, "obj-3", 0), line("obj-2", 0, "obj-3", 1)})
                   : patcher({box("obj-1", "loadbang", 120, 10, R"("bang")"), box("obj-4", "0.25", 120, 40, R"("")", "message"),
                              box("obj-5", "delay 100", 220, 40, R"("bang")"), box("obj-6", "0.5", 220, 60, R"("")", "message"),
                              box("obj-2", "cycle~ 375", 20, 80, kSig), box("obj-3", "plugout~", 20, 140, "")},
                             {line("obj-1", 0, "obj-4", 0), line("obj-4", 0, "obj-2", 1), line("obj-1", 0, "obj-5", 0),
                              line("obj-5", 0, "obj-6", 0), line("obj-6", 0, "obj-2", 1), line("obj-2", 0, "obj-3", 0),
                              line("obj-2", 0, "obj-3", 1)});
        const auto t = translateMaxPatcher(json, AmxdKind::AudioEffect);
        int latency = 0;
        std::string error;
        const auto out = t.ok() ? renderOut(t.pdPatch, 6144, latency, error) : std::vector<float>{};
        double worst = out.empty() ? 1 : 0;
        for (std::size_t n = 0; n < 5312 && static_cast<std::size_t>(latency) + n < out.size(); ++n) {
            const double offset = !signal && n >= 4800 ? 0.5 : 0.25;
            worst = std::max(worst, std::fabs(out[static_cast<std::size_t>(latency) + n] -
                                              std::cos(2 * 3.141592653589793 * (375.0 * static_cast<double>(n) / 48000.0 + offset))));
        }
        check(t.ok() && out.size() >= static_cast<std::size_t>(latency) + 5312 && worst < 1e-4,
              std::string(signal ? "a signal offset of 0.25" : "a float offset of 0.25, then 0.5 at 100 ms") +
                  ": cos(2 pi (f t + offset)) sample for sample (worst " + std::to_string(worst) + ")" + error);
    }
}

/// Prints from a translated control patch, sorted (for checks where the
/// order between prints is not the point).
std::vector<std::string> sortedPrints(const std::string& json) {
    const auto t = translateMaxPatcher(json, AmxdKind::AudioEffect);
    auto got = t.ok() ? printed(t.pdPatch) : std::vector<std::string>{"refused: " + (t.refused.empty() ? t.error : t.refused[0].reason)};
    return got;
}
std::string shownAll(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& g : v) s += (s.empty() ? "" : " | ") + g;
    return s;
}
bool refusedFor(const std::string& json, const std::string& why) {
    const auto t = translateMaxPatcher(json, AmxdKind::AudioEffect);
    return !t.ok() && !t.refused.empty() && t.refused[0].reason.find(why) != std::string::npos;
}

void testStepNine() {
    section("DEV-16 step 9 -- the audit against Max's reference pages");
    // A bare [/~] divides by 1.
    {
        const std::string json = patcher({box("obj-1", "plugin~", 20, 20, kSig2), box("obj-2", "/~", 20, 80, kSig),
                                          box("obj-3", "/~", 120, 80, kSig), box("obj-4", "plugout~", 20, 140, "")},
                                         {line("obj-1", 0, "obj-2", 0), line("obj-1", 1, "obj-3", 0), line("obj-2", 0, "obj-4", 0),
                                          line("obj-3", 0, "obj-4", 1)});
        float gain = 0;
        int latency = 0;
        std::string error;
        const auto t = translateMaxPatcher(json, AmxdKind::AudioEffect);
        const auto residue = t.ok() ? render(t.pdPatch, gain, latency, error) : std::vector<float>{};
        check(t.ok() && !residue.empty() && std::fabs(gain - 1.0f) < 1e-6, "a bare [/~] passes its input (x " + std::to_string(gain) + ")" + error);
    }
    // [selector~ 2 1] told 3 plays input 2 (sig~ 0.5), told -1 nothing.
    for (const auto& [choice, want] : std::vector<std::pair<std::string, double>>{{"3", 0.5}, {"-1", 0.0}, {"1", 0.25}}) {
        const std::string json = patcher(
            {box("obj-1", "loadbang", 200, 10, R"("bang")"), box("obj-2", choice, 200, 40, R"("")", "message"),
             box("obj-3", "sig~ 0.25", 20, 40, kSig), box("obj-4", "sig~ 0.5", 100, 40, kSig),
             box("obj-5", "selector~ 2 1", 20, 80, kSig), box("obj-6", "plugout~", 20, 140, "")},
            {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-5", 0), line("obj-3", 0, "obj-5", 1), line("obj-4", 0, "obj-5", 2),
             line("obj-5", 0, "obj-6", 0), line("obj-5", 0, "obj-6", 1)});
        const auto t = translateMaxPatcher(json, AmxdKind::AudioEffect);
        int latency = 0;
        std::string error;
        const auto out = t.ok() ? renderOut(t.pdPatch, 1024, latency, error) : std::vector<float>{};
        const double got = out.empty() ? -9 : out.back();
        check(std::fabs(got - want) < 1e-6, "[selector~ 2 1] told " + choice + ": " + std::to_string(got) + " (Max: " + std::to_string(want) + ")" + error);
    }
    // [gate 2] told 3, then 5: 5 out of outlet 2 (the rightmost). The
    // loadbang feeds the right-hand message first (Max's order).
    const auto gated = [](const std::string& choice) {
        return patcher({box("obj-1", "loadbang", 100, 10, R"("bang")"), box("obj-2", choice, 200, 40, R"("")", "message"),
                        box("obj-3", "5", 10, 40, R"("")", "message"), box("obj-4", "gate 2", 200, 80, R"("","")"),
                        box("obj-5", "print Y", 150, 140, ""), box("obj-6", "print X", 250, 140, "")},
                       {line("obj-1", 0, "obj-2", 0), line("obj-1", 0, "obj-3", 0), line("obj-2", 0, "obj-4", 0),
                        line("obj-3", 0, "obj-4", 1), line("obj-4", 0, "obj-5", 0), line("obj-4", 1, "obj-6", 0)});
    };
    check(sortedPrints(gated("3")) == std::vector<std::string>{"X: 5"}, "[gate 2] told 3: the rightmost outlet (" + shownAll(sortedPrints(gated("3"))) + ")");
    // A bang into [gate 2 1]'s left inlet reports 1 out of the left outlet.
    {
        const std::string json = patcher({box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-4", "gate 2 1", 10, 80, R"("","")"),
                                          box("obj-5", "print X", 10, 140, "")},
                                         {line("obj-1", 0, "obj-4", 0), line("obj-4", 0, "obj-5", 0)});
        check(sortedPrints(json) == std::vector<std::string>{"X: 1"}, "a bang into [gate 2 1] reports 1 (" + shownAll(sortedPrints(json)) + ")");
    }
    // A bare [t] sends the number out of both outlets.
    check(sortedPrints(probe("5", "t", R"("int","int")", 2)) == std::vector<std::string>{"X: 5", "X: 5"},
          "a bare [t] fed 5 sends 5 twice (" + shownAll(sortedPrints(probe("5", "t", R"("int","int")", 2))) + ")");
    // One outlet into both inlets of [* ]: the right inlet first, 3 x 3 = 9,
    // whatever order the patch lines are listed in.
    {
        const std::string json = patcher({box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "3", 10, 40, R"("")", "message"),
                                          box("obj-3", "*", 10, 80, R"("int")", "newobj", R"(,"numinlets":2)"),
                                          box("obj-4", "print X", 10, 140, "")},
                                         {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0), line("obj-2", 0, "obj-3", 1),
                                          line("obj-3", 0, "obj-4", 0)});
        check(sortedPrints(json) == std::vector<std::string>{"X: 9"}, "[3( into both inlets of [*]: 9, the right inlet first (" + shownAll(sortedPrints(json)) + ")");
    }
    // [loadbang] banged in its inlet bangs again.
    {
        const std::string json = patcher({box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "loadbang", 10, 60, R"("bang")"),
                                          box("obj-3", "print X", 10, 120, "")},
                                         {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0)});
        check(sortedPrints(json) == std::vector<std::string>{"X: bang", "X: bang"}, "a [loadbang] banged in its inlet bangs again (" + shownAll(sortedPrints(json)) + ")");
    }
    // A message box's ---name reaches [r ---name].
    {
        const std::string json = patcher({box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "; ---v 7", 10, 40, R"("")", "message"),
                                          box("obj-3", "r ---v", 200, 80, R"("")"), box("obj-4", "print X", 200, 140, "")},
                                         {line("obj-1", 0, "obj-2", 0), line("obj-3", 0, "obj-4", 0)});
        check(sortedPrints(json) == std::vector<std::string>{"X: 7"}, "[; ---v 7( reaches [r ---v] (" + shownAll(sortedPrints(json)) + ")");
    }
    // A message box's right inlet sets it: 7 set, then a bang sends 7.
    {
        const std::string json = patcher({box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-2", "7", 200, 40, R"("")", "message"),
                                          box("obj-3", "0", 10, 80, R"("")", "message"), box("obj-4", "print X", 10, 140, "")},
                                         {line("obj-1", 0, "obj-2", 0), line("obj-1", 0, "obj-3", 0), line("obj-2", 0, "obj-3", 1),
                                          line("obj-3", 0, "obj-4", 0)});
        check(sortedPrints(json) == std::vector<std::string>{"X: 7"}, "7 into a message box's right inlet, then a bang: 7 (" + shownAll(sortedPrints(json)) + ")");
    }
    // Number boxes: a flonum from 0 to 1 fed 1.5 gives 1; a number box in
    // Decimal Floating-Point (format 6) fed 0.75 gives 0.75.
    {
        std::string json = probe("1.5", "", R"("","bang")", 1);
        const std::string from = R"("maxclass":"newobj","text":"")";
        json.replace(json.find(from), from.size(), R"("maxclass":"flonum","minimum":0.0,"maximum":1.0,"text":"")");
        check(sortedPrints(json) == std::vector<std::string>{"X: 1"}, "a flonum from 0 to 1 fed 1.5 gives 1 (" + shownAll(sortedPrints(json)) + ")");
    }
    {
        std::string json = probe("0.75", "", R"("","bang")", 1);
        const std::string from = R"("maxclass":"newobj","text":"")";
        json.replace(json.find(from), from.size(), R"("maxclass":"number","format":6,"text":"")");
        check(sortedPrints(json) == std::vector<std::string>{"X: 0.75"}, "a number box in format 6 fed 0.75 gives 0.75 (" + shownAll(sortedPrints(json)) + ")");
    }
    // Refused, with the reason.
    check(refusedFor(patcher({box("obj-1", R"(\"my name\")", 10, 10, R"("")", "message")}, {}), "quoted"), "a quoted message is refused");
    check(refusedFor(patcher({box("obj-1", "plugout~ 2 1", 10, 10, "")}, {}), "channel arguments"), "plugout~ 2 1 is refused (Max ignores the arguments)");
    check(translateMaxPatcher(patcher({box("obj-1", "plugout~ 1 2", 10, 10, "")}, {}), AmxdKind::AudioEffect).ok(), "plugout~ 1 2 translates");
    check(refusedFor(patcher({box("obj-1", "phasor~ 110", 10, 10, kSig), box("obj-2", "phasor~ 220", 10, 60, kSig)}, {line("obj-1", 0, "obj-2", 1)}),
                     "hard sync"),
          "a signal into [phasor~]'s right inlet is refused (hard sync)");
    check(refusedFor(patcher({box("obj-1", "biquad~ 1 0 0 0 1.2", 10, 10, kSig)}, {}), "unstable"), "an unstable biquad~ is refused");
    {
        const std::string attrs = R"(,"saved_attribute_attributes":{"valueof":{"parameter_longname":"Steps","parameter_mmin":0.0,)"
                                  R"("parameter_mmax":64.0,"parameter_type":0,"parameter_steps":4}})";
        check(refusedFor(patcher({box("obj-1", "", 10, 10, R"("","float")", "live.dial", attrs)}, {}), "Steps"), "a live.dial with Steps is refused");
        check(refusedFor(patcher({box("obj-1", "", 10, 10, R"("","","float")", "live.menu",
                                      R"(,"saved_attribute_attributes":{"valueof":{"parameter_type":2,"parameter_enum":["a","b"]}})"),
                                  box("obj-2", "print X", 10, 60, "")},
                                 {line("obj-1", 2, "obj-2", 0)}),
                         "value outlet"),
              "live.menu's right outlet is refused");
    }
}

/// loadbang -> two messages, the right-hand one first (Max's order) -> a box
/// (the right message into `rightInlet`) -> [print X].
std::string twoMessages(const std::string& first, int firstInlet, const std::string& second, const std::string& text,
                        const std::string& maxclass = "newobj", const std::string& extra = "") {
    return patcher({box("obj-1", "loadbang", 100, 10, R"("bang")"), box("obj-2", first, 200, 40, R"("")", "message"),
                    box("obj-3", second, 10, 40, R"("")", "message"), box("obj-4", text, 10, 80, R"("")", maxclass, extra),
                    box("obj-5", "print X", 10, 140, "")},
                   {line("obj-1", 0, "obj-2", 0), line("obj-1", 0, "obj-3", 0), line("obj-2", 0, "obj-4", firstInlet),
                    line("obj-3", 0, "obj-4", 0), line("obj-4", 0, "obj-5", 0)});
}

void testStepTen() {
    section("DEV-16 step 10 -- the audit's messages: lists, set, reset, clear, bang, stored arguments");
    struct Case {
        const char* what;
        std::string json;
        std::vector<std::string> expect;
    };
    const std::vector<Case> cases{
        {"[+] fed the list 3 4 gives 7", probe("3 4", "+", R"("int")"), {"X: 7"}},
        {"[* 2] fed the list 3 5 gives 15", probe("3 5", "* 2", R"("int")"), {"X: 15"}},
        {"[/] fed 7 gives 7 (int division by 0 divides by 1)", probe("7", "/", R"("int")"), {"X: 7"}},
        {"[/ 4] with 0 in its right inlet, fed 100, gives 100", twoMessages("0", 1, "100", "/ 4"), {"X: 100"}},
        {"[pack] fed the list 3 4 gives 3 4", probe("3 4", "pack", R"("")"), {"X: 3 4"}},
        {"[pack 0. 0] fed 1.5 2.7 gives 1.5 2", probe("1.5 2.7", "pack 0. 0", R"("")"), {"X: 1.5 2"}},
        {"[pack 5 0] banged gives 5 0", probe("bang", "pack 5 0", R"("")"), {"X: 5 0"}},
        {"[!- 10.] fed 3 4 and then 1 gives 7 and 9", twoMessages("3 4", 0, "1", "!- 10."), {"X: 7", "X: 9"}},
        {"[i] set to 5, then banged, gives 5", twoMessages("set 5", 0, "bang", "i"), {"X: 5"}},
        {"[+ 1] set to 5, then banged, gives 6", twoMessages("set 5", 0, "bang", "+ 1"), {"X: 6"}},
        {"[prepend 1] set to 2, then fed 64, gives 2 64", twoMessages("set 2", 0, "64", "prepend 1"), {"X: 2 64"}},
        {"[prepend foo] banged gives foo bang", probe("bang", "prepend foo", R"("")"), {"X: foo bang"}},
        {"[$1 100( fed 5, then banged, sends 5 100 twice", twoMessages("5", 0, "bang", "$1 100", "message"), {"X: 5 100", "X: 5 100"}},
    };
    for (const auto& c : cases) {
        const auto got = sortedPrints(c.json);
        check(got == c.expect, std::string(c.what) + ": " + shownAll(got));
    }
    // A number box set to 64 and then banged sends 64.
    {
        std::string json = twoMessages("set 64", 0, "bang", "");
        const std::string from = R"("id":"obj-4","maxclass":"newobj","text":"")";
        json.replace(json.find(from), from.size(), R"("id":"obj-4","maxclass":"number","text":"")");
        const auto got = sortedPrints(json);
        check(got == std::vector<std::string>{"X: 64"}, "a number box set to 64, then banged, sends 64: " + shownAll(got));
    }
    // [reset( into [cycle~ 375] at 100 ms (half way through a period): the
    // phase starts again at 0 there.
    {
        const std::string json = patcher(
            {box("obj-1", "loadbang", 200, 10, R"("bang")"), box("obj-2", "delay 100", 200, 30, R"("bang")"),
             box("obj-3", "reset", 200, 50, R"("")", "message"), box("obj-4", "cycle~ 375", 20, 80, kSig),
             box("obj-5", "plugout~", 20, 140, "")},
            {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-3", 0), line("obj-3", 0, "obj-4", 0), line("obj-4", 0, "obj-5", 0),
             line("obj-4", 0, "obj-5", 1)});
        const auto t = translateMaxPatcher(json, AmxdKind::AudioEffect);
        int latency = 0;
        std::string error;
        const auto out = t.ok() ? renderOut(t.pdPatch, 6144, latency, error) : std::vector<float>{};
        double worst = out.empty() ? 1 : 0;
        for (std::size_t n = 4700; n < 4900 && static_cast<std::size_t>(latency) + n < out.size(); ++n) {
            const double phase = 375.0 * static_cast<double>(n < 4800 ? n : n - 4800) / 48000.0;
            worst = std::max(worst, std::fabs(out[static_cast<std::size_t>(latency) + n] - std::cos(2 * 3.141592653589793 * phase)));
        }
        check(t.ok() && worst < 1e-4, "[reset( into [cycle~ 375] at 100 ms restarts its phase (worst " + std::to_string(worst) + ")" + error);
    }
    {
        const auto t = translateMaxPatcher(
            patcher({box("obj-1", "clear", 10, 10, R"("")", "message"), box("obj-2", "biquad~ @smooth 0", 10, 50, kSig)},
                    {line("obj-1", 0, "obj-2", 0)}),
            AmxdKind::AudioEffect);
        check(t.ok() && t.pdPatch.find("set 0 0") != std::string::npos && t.pdPatch.find("route clear") != std::string::npos,
              "[clear( into biquad~ reaches Pd's biquad~ as set 0 0");
    }
    check(refusedFor(patcher({box("obj-1", "reset", 10, 10, R"("")", "message"), box("obj-2", "slide~ 10 10", 10, 50, kSig)},
                             {line("obj-1", 0, "obj-2", 0)}),
                     "reset"),
          "[reset( into slide~ is refused");
}

/// The review of steps 9 and 10 (three skeptics each).
void testStepTenReviewed() {
    section("DEV-16 steps 9 and 10, reviewed: stored symbols, set through the limits, a bang that sets, next, setwidth, Pd's stability test");
    {
        // [symbol foo( into [$1(, then a bang: foo twice.
        const auto got = sortedPrints(twoMessages("symbol foo", 0, "bang", "$1", "message"));
        check(got == std::vector<std::string>{"X: foo", "X: foo"}, "a symbol into [$1(, then a bang: foo twice (" + shownAll(got) + ")");
    }
    // An int number box with maximum 127, set to 200 and to 3.7, then banged: 127 and 3.
    for (const auto& [set, want] : std::vector<std::pair<std::string, std::string>>{{"set 200", "X: 127"}, {"set 3.7", "X: 3"}}) {
        std::string json = twoMessages(set, 0, "bang", "");
        const std::string from = R"("id":"obj-4","maxclass":"newobj","text":"")";
        json.replace(json.find(from), from.size(), R"("id":"obj-4","maxclass":"number","maximum":127.0,"text":"")");
        const auto got = sortedPrints(json);
        check(got == std::vector<std::string>{want}, "a number box (maximum 127) given '" + set + "', then banged: " + shownAll(got));
    }
    // A bang into a message box's right inlet sets it to bang: a later bang sends bang.
    {
        const std::string json = patcher({box("obj-1", "loadbang", 10, 10, R"("bang")"), box("obj-3", "hello", 10, 80, R"("")", "message"),
                                          box("obj-4", "print X", 10, 140, "")},
                                         {line("obj-1", 0, "obj-3", 1), line("obj-1", 0, "obj-3", 0), line("obj-3", 0, "obj-4", 0)});
        const auto got = sortedPrints(json);
        check(got == std::vector<std::string>{"X: bang"}, "a bang into a message box's right inlet, then into its left: bang (" + shownAll(got) + ")");
    }
    check(refusedFor(patcher({box("obj-1", "next", 10, 10, R"("")", "message"), box("obj-2", "gate 3 1", 10, 50, R"("","","")")},
                             {line("obj-1", 0, "obj-2", 0)}),
                     "next"),
          "[next( into [gate] is refused");
    // [live.thisdevice] -> [setwidth 300( -> back into it: one load bang, no loop.
    {
        const std::string json = patcher({box("obj-1", "live.thisdevice", 10, 10, R"("bang","int","int")"),
                                          box("obj-2", "setwidth 300", 10, 60, R"("")", "message"), box("obj-3", "print X", 100, 60, "")},
                                         {line("obj-1", 0, "obj-2", 0), line("obj-2", 0, "obj-1", 0), line("obj-1", 0, "obj-3", 0)});
        const auto got = sortedPrints(json);
        check(got == std::vector<std::string>{"X: bang"}, "live.thisdevice -> [setwidth 300( -> itself: one bang, no loop (" + shownAll(got) + ")");
    }
    // Pd's own stability test: fb1 = 2.5, fb2 = -1.55 (real poles 1.36 and 1.14) is refused.
    check(refusedFor(patcher({box("obj-1", "biquad~ 1. 0. 0. -2.5 1.55", 10, 10, kSig)}, {}), "unstable"),
          "biquad~ 1 0 0 -2.5 1.55 (poles outside the unit circle, Pd's fb1 > 2) is refused");
    check(translateMaxPatcher(patcher({box("obj-1", "biquad~ 1. 0. 0. -1.8 0.81", 10, 10, kSig)}, {}), AmxdKind::AudioEffect).ok(),
          "a stable biquad~ (a double pole at 0.9) translates");
    // The third review: a pole exactly on the unit circle is refused (Pd's
    // float test decides it, and silences 1 0 0 -1.95 0.95); and a message
    // led by a word sends the same when it arrives and when banged later.
    check(refusedFor(patcher({box("obj-1", "biquad~ 1. 0. 0. -1.95 0.95", 10, 10, kSig)}, {}), "unit circle"),
          "biquad~ 1 0 0 -1.95 0.95 (a pole at 1) is refused");
    {
        const auto got = sortedPrints(twoMessages("hello 3", 0, "bang", "$1 $2", "message"));
        check(got == std::vector<std::string>{"X: hello 3", "X: hello 3"}, "[hello 3( into [$1 $2(, then a bang: hello 3 both times (" + shownAll(got) + ")");
    }
    // The fourth review: "set" into a $ message box changes its text without
    // output (a later bang sends the new text); and stable low resonances
    // (a 7.6 Hz pole pair of radius 0.9999, a 5 Hz highpass) are not refused.
    {
        const auto got = sortedPrints(twoMessages("set 5 6", 0, "bang", "$1 $2", "message"));
        check(got == std::vector<std::string>{"X: 5 6"}, "[set 5 6( into [$1 $2(, then a bang: 5 6 once (" + shownAll(got) + ")");
    }
    // The fifth review: "set" into a message that sends to a ---name is refused.
    check(refusedFor(twoMessages("set 5 6", 0, "bang", "$1 $2; ---foo $1", "message"), "set into a message"),
          "[set 5 6( into [$1 $2; ---foo $1( is refused");
    // The sixth review: a set that arrives indirectly (here through a gate) at
    // [1; ---foo 2( is dropped, not copied into the part that feeds ---foo:
    // the bang that follows sends 1 out and 2 to ---foo, once each.
    {
        const std::string json = patcher(
            {box("obj-1", "loadbang", 100, 10, R"("bang")"), box("obj-2", "set 7", 200, 40, R"("")", "message"),
             box("obj-7", "gate 1 1", 200, 60, R"("")"), box("obj-3", "bang", 10, 40, R"("")", "message"),
             box("obj-4", "1; ---foo 2", 10, 80, R"("")", "message"), box("obj-5", "print X", 10, 140, ""),
             box("obj-6", "r ---foo", 300, 100, R"("")"), box("obj-8", "print X", 300, 140, "")},
            {line("obj-1", 0, "obj-2", 0), line("obj-1", 0, "obj-3", 0), line("obj-2", 0, "obj-7", 1), line("obj-7", 0, "obj-4", 0),
             line("obj-3", 0, "obj-4", 0), line("obj-4", 0, "obj-5", 0), line("obj-6", 0, "obj-8", 0)});
        auto got = sortedPrints(json);
        std::sort(got.begin(), got.end());
        check(got == std::vector<std::string>{"X: 1", "X: 2"}, "an indirect set into [1; ---foo 2( is dropped (" + shownAll(got) + ")");
    }
    check(translateMaxPatcher(patcher({box("obj-1", "biquad~ 1. 0. 0. -1.99979902 0.99980001", 10, 10, kSig)}, {}), AmxdKind::AudioEffect).ok(),
          "a 7.6 Hz resonance of radius 0.9999 translates");
    check(translateMaxPatcher(patcher({box("obj-1", "biquad~ 0.9995373 -1.9990746 0.9995373 -1.999074391 0.999074819", 10, 10, kSig)}, {}),
                              AmxdKind::AudioEffect)
              .ok(),
          "a 5 Hz highpass translates");
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
    testMoreLiveControls();
    testRightToLeftIsMadeExplicit();
    testWhatIsRefusedIsNamed();
    testBpatchersTranslate();
#if defined(ADI_WITH_PD)
    testTranslationsWalkAndPlay();
    testTheOrderIsHeard();
    testMaxIntegersAreHeard();
    testSubpatchersCarrySignalsAndControls();
    testLineSegmentsAreChained();
    testStepFourControls();
    testStepFourSignals();
    testBpatchersPlay();
    testCyclePhaseIsAnOffset();
    testLineEnds();
    testStepNine();
    testStepTen();
    testStepTenReviewed();
#endif
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
