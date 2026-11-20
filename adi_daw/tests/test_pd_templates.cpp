// SPDX-License-Identifier: GPL-3.0-or-later
//
// DEV-10's three starter templates (pd/templates/): an audio effect, an
// instrument and a MIDI effect a user starts a Pd device from.
//
// A template is the first patch a user sees, so it has to be right in every
// way the host will check later: its declarations parse with no problem, it
// walks clean under PD4 against the running build, it opens in libpd with
// nothing in the console, and it does what its comment says -- the audio
// effect is unity at its default and follows its Gain, the instrument plays
// the note it is given at the pitch and level it says and falls silent over
// Release.

#include "adi/engine/graph.hpp"
#include "juce/pd_declarations.hpp"
#include "juce/pd_device.hpp"
#include "juce/pd_engine.hpp"
#include "juce/pd_object_walk.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace adi::device;
namespace engine = adi::engine;

int checks = 0, failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}
void section(const char* name) { std::printf("\n[%s]\n", name); }

const std::filesystem::path kPd = ADI_PD_PATCH_DIR;
const std::filesystem::path kTemplates = kPd / "templates";

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void testEveryTemplateIsSound() {
    section("DEV-10 -- every template parses, walks clean and opens quietly");
    const PdVocabulary vocab(PdRuntime::creatableClasses());
    PdAbstractions abs;
    for (const char* a : {"adi.param.pd", "adi.array.pd", "adi.transport.pd"}) abs.emplace(a, slurp(kPd / a));
    int seen = 0;
    for (const char* name : {"Audio Effect.pd", "Instrument.pd", "MIDI Effect.pd"}) {
        const std::string text = slurp(kTemplates / name);
        ++seen;
        const auto decls = parsePdDeclarations(text);
        check(!text.empty() && decls.problems.empty() && decls.params.size() == 1,
              std::string(name) + ": one parameter, no declaration problem");
        const auto walk = walkPdObjects(text, abs, vocab);
        std::string why;
        for (const auto& l : walk.describe()) why += " " + l + ";";
        check(walk.allowed(), std::string(name) + ": walks clean" + why);
        LibPdEngine eng(kTemplates.string(), name, 2, 2);
        eng.addSearchPath(kPd.string());
        PdLatencyReceiver lat;
        std::string error;
        const bool opened = eng.open(lat, error);
        std::string console;
        for (const auto& l : eng.consoleLines())
            if (l.find("error") != std::string::npos || l.find("couldn't") != std::string::npos ||
                l.find("...") != std::string::npos)
                console += " [" + l + "]";
        check(opened && console.empty(), std::string(name) + ": opens with nothing wrong in the console" +
                                             error + console);
    }
    check(seen == 3, "three templates");
}

struct Run {
    LibPdEngine eng;
    PdLatencyReceiver lat;
    std::vector<engine::Event> events;
    Run(const char* name) : eng(kTemplates.string(), name, 2, 2) { eng.addSearchPath(kPd.string()); }
    bool open() {
        std::string e;
        if (!eng.open(lat, e)) return false;
        eng.prepare(48000.0, 256);
        return true;
    }
    /// One 256-frame block; `in` may be null for silence.
    std::vector<float> block(const float* in) {
        static const std::vector<float> zeros(256, 0.f);
        std::vector<float> l(256), r(256);
        const float* ip[2] = {in != nullptr ? in : zeros.data(), in != nullptr ? in : zeros.data()};
        float* op[2] = {l.data(), r.data()};
        engine::NodeIo io;
        io.in = ip;
        io.out = op;
        io.channels = 2;
        io.frames = 256;
        io.events = engine::EventSpan{events.data(), static_cast<std::int32_t>(events.size())};
        eng.process(io);
        events.clear();
        return l;
    }
};

void testTheAudioEffectIsUnityAndFollowsItsGain() {
    section("DEV-10 -- the audio effect: unity at 0 dB, and its Gain is heard");
    Run run("Audio Effect.pd");
    check(run.open(), "opens");
    const int lat = run.eng.adapterLatencySamples();
    std::vector<float> in, out;
    for (int b = 0; b < 40; ++b) {
        std::vector<float> x(256);
        for (int i = 0; i < 256; ++i)
            x[static_cast<std::size_t>(i)] = static_cast<float>(
                0.5 * std::sin(2.0 * 3.141592653589793 * 997.0 * (b * 256 + i) / 48000.0));
        const auto y = run.block(x.data());
        in.insert(in.end(), x.begin(), x.end());
        out.insert(out.end(), y.begin(), y.end());
    }
    // After the 20 ms ramp from silence, the output is the input, delayed.
    bool exact = true;
    for (std::size_t i = 2048; i < out.size(); ++i)
        if (out[i] != in[i - static_cast<std::size_t>(lat)]) { exact = false; break; }
    check(exact, "after its 20 ms ramp it is the input, " + std::to_string(lat) + " samples late, bit for bit");

    // Gain to -6 dB through the parameter's receive name, as the host sends it.
    check(run.eng.sendFloat("adi-1", -6.0f), "the Gain parameter receives at $0-adi-1");
    std::vector<float> x(256);
    double num = 0, den = 0;
    for (int b = 0; b < 20; ++b) {
        for (int i = 0; i < 256; ++i) x[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(0.13 * (b * 256 + i)));
        const auto y = run.block(x.data());
        if (b < 10) continue;   // past the ramp
        for (int i = 0; i < 256; ++i) {
            const std::size_t k = static_cast<std::size_t>(i);
            if (i < lat) continue;
            num += static_cast<double>(y[k]) * x[k - static_cast<std::size_t>(lat)];
            den += static_cast<double>(x[k - static_cast<std::size_t>(lat)]) * x[k - static_cast<std::size_t>(lat)];
        }
    }
    const double gainDb = 20.0 * std::log10(num / den);
    check(std::fabs(gainDb + 6.0) < 0.01, "Gain -6 dB plays at " + std::to_string(gainDb) + " dB");
}

void testTheInstrumentPlaysTheNote() {
    section("DEV-10 -- the instrument: A4 at its velocity, silent over Release");
    Run run("Instrument.pd");
    check(run.open(), "opens");
    engine::Event on;
    on.type = engine::EventType::NoteOn;
    on.noteId = 1;
    on.dim = 69;
    on.value = 1.0;
    run.events.push_back(on);
    std::vector<float> held;
    for (int b = 0; b < 40; ++b) {
        const auto y = run.block(nullptr);
        if (b >= 10) held.insert(held.end(), y.begin(), y.end());
    }
    double peak = 0;
    int crossings = 0;
    for (std::size_t i = 1; i < held.size(); ++i) {
        peak = std::max(peak, std::fabs(static_cast<double>(held[i])));
        if (held[i - 1] < 0 && held[i] >= 0) ++crossings;
    }
    const double hz = crossings * 48000.0 / static_cast<double>(held.size());
    check(std::fabs(peak - 0.25) < 0.005, "full velocity sounds at 0.25 peak: " + std::to_string(peak));
    check(std::fabs(hz - 440.0) < 5.0, "at 440 Hz: " + std::to_string(hz));

    engine::Event off = on;
    off.type = engine::EventType::NoteOff;
    off.value = 0.0;
    run.events.push_back(off);
    double after = 0;
    for (int b = 0; b < 60; ++b) {   // 320 ms: past the 200 ms Release
        const auto y = run.block(nullptr);
        if (b >= 45)
            for (float v : y) after = std::max(after, std::fabs(static_cast<double>(v)));
    }
    check(after < 1e-6, "and silent once Release has passed: " + std::to_string(after));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_pd_template_tests -- DEV-10's starter templates\n");
    testEveryTemplateIsSound();
    testTheAudioEffectIsUnityAndFollowsItsGain();
    testTheInstrumentPlaysTheNote();
    std::printf("\n%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
