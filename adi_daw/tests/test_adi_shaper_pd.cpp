// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/builtin_catalog.hpp"
#include "adi/dsp/adi_shaper.hpp"
#include "adi/engine/events.hpp"
#include "adi/textproj.hpp"
#include "juce/builtin_host.hpp"
#include "juce/pd_engine.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr auto uid = "org.adidaw.builtin.adi-shaper";
using adi::device::DeviceInstance;
using adi::device::ParamValue;
void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}
auto open(int maxFrames = 64) {
    adi::rows::PluginRef reference;
    reference.format = "internal";
    reference.uid = uid;
    adi::engine::DeviceRequest request;
    request.ref = &reference;
    std::string error;
    auto device = adi::device::makeBuiltinDevice(request, error);
    if (!device) throw std::runtime_error(error);
    device->prepare(48000, maxFrames);
    return device;
}
void set(DeviceInstance& device, int id, double value) {
    check(device.setParam(std::to_string(id), ParamValue::fromNormalized(value)), "parameter write");
}

void writeCrossedCanvas(const std::filesystem::path& file) {
    std::string canvas(adi::builtinCanvas(uid));
    const std::string left = "#X connect 1 0 2 0;", right = "#X connect 1 1 2 1;";
    const auto l = canvas.find(left), r = canvas.find(right);
    check(l != std::string::npos && r != std::string::npos, "find both output connections on the shipped canvas");
    canvas.replace(l, left.size(), "#X connect 1 0 2 1;");
    canvas.replace(r, right.size(), "#X connect 1 1 2 0;");
    std::ofstream output(file, std::ios::binary);
    output << canvas;
    output.close();
    check(output.good(), "write edited project canvas");
}

void filterPeakRecall() {
    auto source = open();
    set(*source, 134, 1);
    set(*source, 140, .25);
    set(*source, 163, .75);
    set(*source, 186, 1);
    set(*source, 913, 1);
    set(*source, 914, 1);
    const auto saved = source->saveState("state");
    auto device = open();
    check(device->loadState("state", saved), "recall independent peak switches");
    check(device->getParam("913").real == 1 && device->getParam("914").real == 1
          && device->getParam("915").real == 0, "the three saved peak switches are independent");
    check(device->getParam("140").real == 1 && device->getParam("163").real == 3
          && device->getParam("186").real == 4, "legacy normalized Filter selections retain their meaning");

    const auto impulse = [](DeviceInstance& effect) {
        effect.prepare(48000, 64);
        std::array<std::array<float, 64>, 2> input{}, output{};
        input[0][0] = -.25f;
        input[1][11] = .375f;
        const float* in[]{input[0].data(), input[1].data()};
        float* out[]{output[0].data(), output[1].data()};
        adi::engine::NodeIo io;
        io.channels = 2;
        io.frames = 64;
        io.in = in;
        io.out = out;
        effect.process(io);
        return output;
    };
    const auto peak = impulse(*device);
    check(peak == impulse(*source), "peak state reproduces stereo audio after reopening");
    auto legacy = adi::Payload::from_cbor(saved);
    for (int id = 913; id <= 915; ++id) {
        legacy.at("params").erase(std::to_string(id));
        legacy.at("real").erase(std::to_string(id));
    }
    set(*device, 915, 1);
    check(device->loadState("state", adi::Payload::to_cbor(legacy)), "recall an older state on the running Filter");
    check(device->getParam("913").real == 0 && device->getParam("914").real == 0
          && device->getParam("915").real == 0, "absent peak switches restore their defaults");
    set(*source, 913, 0);
    set(*source, 914, 0);
    const auto restored = impulse(*device);
    check(restored != peak && restored == impulse(*source), "sparse recall restores the selected high-pass response");
}

void filterPeakPd(const std::string& directory, bool crossed) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
    adi::device::PdLatencyReceiver latency;
    std::string error;
    path.addSearchPath(ADI_PD_PATCH_DIR);
    check(path.open(latency, error) && path.isNative() == !crossed, "open the requested peak processing path");
    path.prepare(48000, 64);
    path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
    Shaper expected, plain;
    const auto send = [&](Shaper::Param id, float value) {
        check(path.sendParameter(static_cast<int>(id) + 1, value), "send a Filter control");
        expected.set(id, value);
        if (id < Shaper::BaseCount) plain.set(id, value);
    };
    send(Shaper::FilterOn, 1);
    send(static_cast<Shaper::Param>(Shaper::FilterOn + 1), 3);
    send(Shaper::FilterLowOffset, -61);
    send(Shaper::FilterMidOffset, 43);
    send(Shaper::FilterHighOffset, -27);
    for (int b = 0; b < 3; ++b) {
        const auto band = static_cast<Shaper::Band>(b);
        for (int i = 0; i < 8; ++i) {
            send(Shaper::modParam(Shaper::Stage::Filter, band, static_cast<Shaper::ModControl>(Shaper::Primary1 + i)), 55);
            send(Shaper::modParam(Shaper::Stage::Filter, band, static_cast<Shaper::ModControl>(Shaper::Secondary1 + i)), 40);
        }
    }
    using Block = std::array<std::array<float, 64>, 2>;
    Block input{}, actual{}, wanted{}, unboosted{};
    const float* in[]{input[0].data(), input[1].data()};
    float* out[]{actual[0].data(), actual[1].data()};
    adi::engine::NodeIo io;
    io.channels = 2;
    io.frames = 64;
    io.in = in;
    io.out = out;
    for (int block = 0; block < 10; ++block) {
        if (block > 0) {
            const int band = (block - 1) / 3;
            const auto peak = static_cast<Shaper::Param>(Shaper::FilterLowPeak + band);
            const auto type = Shaper::modParam(Shaper::Stage::Filter, static_cast<Shaper::Band>(band), Shaper::Type);
            const auto nextType = static_cast<float>(band + 1);
            if (block % 3 == 1) {
                send(peak, 1);
            } else if (block % 3 == 2) {
                // Delay this write in the oracle by one block. Editing the underlying
                // choice while Peak is active must preserve its running filter history.
                check(path.sendParameter(static_cast<int>(type) + 1, nextType), "edit the underlying Filter selection");
                plain.set(type, nextType);
            } else {
                expected.set(type, nextType);
                send(peak, 0);
            }
        }
        for (int i = 0; i < io.frames; ++i) {
            const auto n = block * io.frames + i;
            input[0][static_cast<std::size_t>(i)] = static_cast<float>(.3 * std::sin(.071 * n)) + (n % 11 == 1 ? .4f : 0.f);
            input[1][static_cast<std::size_t>(i)] = static_cast<float>(.2 * std::cos(.163 * n)) - (n % 17 == 3 ? .3f : 0.f);
        }
        expected.process(in[0], in[1], wanted[0].data(), wanted[1].data(), 64);
        plain.process(in[0], in[1], unboosted[0].data(), unboosted[1].data(), 64);
        path.process(io);
        for (std::size_t ch = 0; ch < 2; ++ch)
            check(actual[crossed ? 1 - ch : ch] == wanted[ch], "each band receives its peak switch on the live stereo canvas");
        if (block % 3 == 1) check(wanted != unboosted, "the selected peak band changes the stereo waveform");
    }
    check(latency.latencySamples() == 0, "peak processing adds zero latency");
}

void stereoFilterPd(const std::string& directory, bool crossed) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    constexpr auto mid = Shaper::Band::Mid;
    adi::device::PdLatencyReceiver latency;
    Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
    path.addSearchPath(ADI_PD_PATCH_DIR);
    std::string error;
    check(path.open(latency, error), "open stereo filter");
    path.prepare(48000, 64);
    path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
    check(path.sendParameter(Shaper::FilterOn + 1, 1), "enable filter");
    for (int n = 0; n < 8; ++n) {
        const auto point = static_cast<Shaper::ModControl>(Shaper::Primary1 + n);
        check(path.sendParameter(Shaper::modParam(Shaper::Stage::Filter, mid, point) + 1, 50), "set filter cutoff");
    }
    check(path.sendParameter(851, 50), "set middle band's stereo offset");
    std::array<float, 64> input{}, left{}, right{};
    const float* in[]{input.data(), input.data()};
    float* out[]{left.data(), right.data()};
    adi::engine::NodeIo io;
    io.in = in;
    io.out = out;
    io.channels = 2;
    io.frames = 64;
    path.process(io);
    input[0] = .025f;
    path.process(io);
    const float higher = crossed ? right[0] : left[0], lower = crossed ? left[0] : right[0];
    check(std::abs(higher / .025 - .14177298) < 3e-5 && std::abs(lower / .025 - .02680684) < 3e-5,
          "stereo cutoff reaches the native wrapper and editable external");
}

void steepFilterRecall() {
    auto source = open();
    for (int i = 0; i < 5; ++i) {
        for (const int id : {140, 163, 186}) {
            set(*source, id, i * .25);
            check(source->getParam(std::to_string(id)).real == i, "legacy normalized Filter type positions");
        }
    }
    for (int id = 1; id <= 415; ++id) set(*source, id, (id % 5) * .25);
    set(*source, 916, 1);
    set(*source, 918, 1);
    auto saved = adi::Payload::from_cbor(source->saveState("state"));
    auto restored = open();
    check(restored->loadState("state", adi::Payload::to_cbor(saved)), "recall steep Filter selectors");
    check(restored->getParam("916").real == 1 && restored->getParam("917").real == 0
          && restored->getParam("918").real == 1, "independent steep selectors survive reopening");
    saved.erase("real");
    for (int id = 916; id <= 918; ++id) saved.at("params").erase(std::to_string(id));
    set(*restored, 917, 1);
    check(restored->loadState("state", adi::Payload::to_cbor(saved)), "recall normalized legacy state over steep Filter");
    for (int id = 1; id <= 415; ++id) {
        const auto key = std::to_string(id);
        check(restored->getParam(key).normalized == source->getParam(key).normalized,
              "normalized legacy controls survive a sparse recall");
    }
    for (int id = 916; id <= 918; ++id)
        check(restored->getParam(std::to_string(id)).real == 0, "absent steep selectors return to their default");
}

void steepFilterPd(const std::string& directory, bool crossed) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
    adi::device::PdLatencyReceiver latency;
    std::string error;
    path.addSearchPath(ADI_PD_PATCH_DIR);
    check(path.open(latency, error) && path.isNative() == !crossed, "open the requested steep Filter path");
    path.prepare(48000, 64);
    path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
    Shaper expected, ordinary;
    expected.prepare(48000);
    ordinary.prepare(48000);
    const auto send = [&](Shaper::Param id, float value) {
        check(path.sendParameter(static_cast<int>(id) + 1, value), "send the live Filter parameter");
        expected.set(id, value);
        ordinary.set(id, value);
    };
    send(Shaper::FilterOn, 1);
    send(static_cast<Shaper::Param>(Shaper::FilterOn + 1), 3);
    for (int i = 0; i < 3; ++i) {
        const auto band = static_cast<Shaper::Band>(i);
        send(Shaper::modParam(Shaper::Stage::Filter, band, Shaper::Type), static_cast<float>(2 * i));
        for (int j = 0; j < 8; ++j) {
            send(Shaper::modParam(Shaper::Stage::Filter, band, static_cast<Shaper::ModControl>(Shaper::Primary1 + j)), 53);
            send(Shaper::modParam(Shaper::Stage::Filter, band, static_cast<Shaper::ModControl>(Shaper::Secondary1 + j)), 42);
        }
    }
    using Block = std::array<std::array<float, 64>, 2>;
    Block input{}, got{}, want{}, unmodified{};
    const float* in[]{input[0].data(), input[1].data()};
    float* out[]{got[0].data(), got[1].data()};
    adi::engine::NodeIo io;
    io.channels = 2;
    io.frames = 64;
    io.in = in;
    io.out = out;
    for (int block = 0; block < 19; ++block) {
        const int index = block == 0 ? 0 : (block - 1) / 6;
        const int step = block == 0 ? 0 : (block - 1) % 6 + 1;
        const auto selector = static_cast<Shaper::Param>(Shaper::FilterLowSteep + index);
        const auto type = Shaper::modParam(Shaper::Stage::Filter, static_cast<Shaper::Band>(index), Shaper::Type);
        if (step == 1) {
            // The oracle receives no edit while a 6 dB or band-pass response is ringing.
            check(path.sendParameter(static_cast<int>(selector) + 1, 1), "toggle steep on an unaffected type");
        } else if (step == 2) {
            expected.set(selector, 1);
            send(type, 1);
        } else if (step == 3) {
            send(type, 3);
        } else if (step == 4) {
            send(type, static_cast<float>(2 * index));
        } else if (step == 5) {
            check(path.sendParameter(static_cast<int>(selector) + 1, 0), "clear steep on an unaffected type");
        } else if (step == 6) {
            expected.set(selector, 0);
            send(type, 1);
        }
        for (std::size_t i = 0; i < input[0].size(); ++i) {
            const int n = block * 64 + static_cast<int>(i);
            input[0][i] = static_cast<float>(.29 * std::sin(.061 * n)) + (n % 13 == 2 ? .35f : 0.f);
            input[1][i] = static_cast<float>(.23 * std::cos(.19 * n)) - (n % 19 == 4 ? .43f : 0.f);
        }
        expected.process(in[0], in[1], want[0].data(), want[1].data(), 64);
        ordinary.process(in[0], in[1], unmodified[0].data(), unmodified[1].data(), 64);
        path.process(io);
        for (std::size_t ch = 0; ch < 2; ++ch)
            check(got[crossed ? 1 - ch : ch] == want[ch], "live steep selection and unaffected filter history agree");
        if (step == 2 || step == 3)
            check(want != unmodified, "both supported steep responses change the chosen band's audio");
        if (step == 1 || step == 5)
            check(want == unmodified, "unsupported steep edits preserve every running sample");
    }
    check(latency.latencySamples() == 0, "steep Filter adds zero latency");
}

void combinedFilterRecall() {
    constexpr std::array ids{850,851,852,913,914,915,916,917,918};
    constexpr std::array values{0.,.75,1.,1.,0.,1.,0.,1.,1.};
    auto source = open();
    for (std::size_t i = 0; i < ids.size(); ++i) set(*source, ids[i], values[i]);
    const auto saved = adi::Payload::from_cbor(source->saveState("state"));
    for (const bool realOnly : {false, true}) {
        auto state = saved;
        if (realOnly) {
            for (const auto id : ids) state.at("params").erase(std::to_string(id));
        } else {
            state.erase("real");
        }
        const auto group = realOnly ? "real" : "params";
        for (int keep = 0; keep < 3; ++keep) {
            auto sparse = state;
            auto target = open();
            for (std::size_t i = 0; i < ids.size(); ++i) {
                set(*target, ids[i], i < 3 ? .125 : 1.);
                if (static_cast<int>(i % 3) != keep) sparse.at(group).erase(std::to_string(ids[i]));
            }
            check(target->loadState("state", adi::Payload::to_cbor(sparse)), "overlap sparse recall");
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const auto wanted = static_cast<int>(i % 3) == keep ? values[i] : (i < 3 ? .5 : 0.);
                check(target->getParam(std::to_string(ids[i])).normalized == wanted, "sparse recall preserves present Filter controls and defaults absent ones");
            }
        }
    }
}

void combinedFilterPd(const std::string& directory, bool crossed) {
    using Engine = adi::device::LibPdEngine;
    using Filter = adi::dsp::AdiShaperFilter;
    using Mode = Filter::Mode;
    Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
    adi::device::PdLatencyReceiver latency;
    std::string error;
    path.addSearchPath(ADI_PD_PATCH_DIR);
    check(path.open(latency, error) && path.isNative() == !crossed, "select native or edited combined Filter");
    path.prepare(48000, 64);
    path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
    const auto send = [&](int id, float real) { check(path.sendParameter(id, real), "combined Filter write"); };
    send(134, 1);
    for (int i = 0; i < 8; ++i) {
        send(170 + i, 50);
        send(178 + i, 75);
    }
    Filter first, second;
    // The independent oracle uses only the previous 12 dB kernel responses.
    // A signed dry addition forms Peak; two complete sections form the steep paths.
    enum class Response { Low, LowSteep, High, HighSteep, Peak, Band };
    using R = Response;
    struct Event { int id; float value; R response; };
    constexpr std::array<Event,18> events{{
        Event{163, 1, R::Low}, {851, -50, R::Low}, {917, 1, R::LowSteep},
        {914, 1, R::Peak}, {163, 3, R::Peak}, {917, 0, R::Peak},
        {917, 1, R::Peak}, {851, 75, R::Peak}, {914, 0, R::HighSteep},
        {917, 0, R::High}, {163, 1, R::Low}, {917, 1, R::LowSteep},
        {163, 4, R::Band}, {917, 0, R::Band}, {914, 1, R::Peak},
        {163, 1, R::Peak}, {914, 0, R::Low}, {851, 0, R::Low}
    }};
    using Block = std::array<std::array<float, 64>, 2>;
    Block in{}, out{};
    const float* inputs[]{in[0].data(), in[1].data()};
    float* outputs[]{out[0].data(), out[1].data()};
    adi::engine::NodeIo io;
    io.channels = 2; io.frames = 64; io.in = inputs; io.out = outputs;
    R previous = R::Band;
    unsigned index = 0;
    for (const auto& e : events) {
        send(e.id, e.value);
        if (e.id == 851) {
            first.setStereoOffset(.01f * e.value);
            second.setStereoOffset(.01f * e.value);
        }
        if (e.response != previous) {
            const auto mode = e.response == R::Low || e.response == R::LowSteep ? Mode::LowPass12
                : e.response == R::High || e.response == R::HighSteep ? Mode::HighPass12 : Mode::BandPass12;
            first.setMode(mode);
            second.setMode(mode);
            first.reset();
            second.reset();
        }
        for (std::size_t i=0; i<in[0].size(); ++i, ++index) {
            in[0][i] = static_cast<float>(static_cast<int>(index % 79) - 38) / 47.f;
            in[1][i] = static_cast<float>(19 - static_cast<int>(index % 43)) / 27.f;
        }
        path.process(io);
        for (std::size_t i=0; i<in[0].size(); ++i) {
            float l=in[0][i], r=in[1][i];
            first.process(l, r, .5f, .75f);
            if (e.response == R::Peak) { l=in[0][i]+l; r=in[1][i]+r; }
            if (e.response == R::LowSteep || e.response == R::HighSteep) second.process(l, r, .5f, .2f);
            check(out[crossed ? 1 : 0][i] == l && out[crossed ? 0 : 1][i] == r, "live Filter overlap differs from independent 12 dB signed output");
        }
        previous=e.response;
    }
    check(latency.latencySamples() == 0, "combined Filter has zero latency");
}

void cachedPdTransport(const std::string& directory, bool native) {
    using Engine = adi::device::LibPdEngine;
    adi::device::PdLatencyReceiver latency;
    Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
    path.addSearchPath(ADI_PD_PATCH_DIR);
    std::string error;
    check(path.open(latency, error), "open whole-block gate for cached transport");
    path.prepare(48000, 64);
    path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
    for (int id = 23; id <= 26; ++id) check(path.sendParameter(id, 0), "program silent first two quarters");

    std::array<float, 64> input{}, left{}, right{};
    input.fill(.5f);
    const float* in[]{input.data(), input.data()};
    float* out[]{left.data(), right.data()};
    adi::engine::NodeIo io;
    io.in = in;
    io.out = out;
    io.frames = 64;
    io.channels = 2;
    Engine::Transport manual;
    manual.beat = 3;
    path.setTransport(manual);
    path.process(io);
    check(left == input && right == input, "explicit stopped beat three holds the open gate without a graph position");

    adi::engine::TransportInfo coarse;
    coarse.bpm = 9000;
    coarse.playing = true;
    coarse.beat = 4;
    coarse.ticksInQuarter = adi::textproj::kPPQ * 9 / 10;
    io.transport = &coarse;
    path.process(io);
    const auto transition = left;
    check(left.front() == .5f && left.back() == 0 && right == transition,
          "live block crosses the four-quarter wrap");
    io.transport = nullptr;
    path.process(io);
    check(left == transition && right == transition, "missing graph position repeats the last seven-field message");

    if (native) {
        adi::engine::DeviceTransport exact;
        exact.quarterNotes = 1;
        io.deviceTransport = &exact;
        path.process(io);
        const std::array<float, 64> silent{};
        check(left == silent && right == silent, "exact stopped segment position takes priority over the cached fields");
    }
}

void quantizerBiasPd(const std::string& directory, bool crossed) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    adi::device::PdLatencyReceiver latency;
    Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
    path.addSearchPath(ADI_PD_PATCH_DIR);
    std::string error;
    check(path.open(latency, error), "open biased quantizer");
    path.prepare(48000, 64);
    path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
    check(path.sendParameter(Shaper::CrushOn + 1, 1), "enable Crush");
    for (int n = 0; n < 8; ++n) {
        const auto point = static_cast<Shaper::ModControl>(Shaper::Primary1 + n);
        const int id = Shaper::modParam(Shaper::Stage::Crush, Shaper::Band::Mid, point) + 1;
        check(path.sendParameter(id, 0), "select lowest resolution");
    }
    check(path.sendParameter(866, 75), "select 75 percent bias");
    std::array<float, 64> inL{}, inR{}, outL{}, outR{};
    const float* in[]{inL.data(), inR.data()};
    float* out[]{outL.data(), outR.data()};
    adi::engine::NodeIo io;
    io.in = in; io.out = out; io.channels = 2; io.frames = 64;
    path.process(io);
    inL.fill(.43f); inR.fill(-.38f);
    std::fill(inL.begin() + 16, inL.begin() + 32, -.71f);
    std::fill(inR.begin() + 16, inR.begin() + 32, .91f);
    path.process(io);
    struct Tap { int frame; double left, right; };
    constexpr std::array taps{Tap{0, .45574504, 0.}, Tap{1, .64351350, 0.},
        Tap{16, .15879554, .39158368}, Tap{17, -.01331603, .64359742}, Tap{32, .15884674, .37855777}};
    for (const auto& t : taps) {
        const auto frame = static_cast<std::size_t>(t.frame);
        check(std::abs(outL[frame] - (crossed ? t.right : t.left)) < 3e-6 &&
              std::abs(outR[frame] - (crossed ? t.left : t.right)) < 3e-6,
              "quantizer bias reaches both channels in native and editable Pd");
    }
}

void stereoSweepPd(const std::string& directory, bool crossed) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    constexpr auto mid = Shaper::Band::Mid;
    adi::device::PdLatencyReceiver latency;
    Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
    path.addSearchPath(ADI_PD_PATCH_DIR);
    std::string error;
    check(path.open(latency, error), "open stereo sweep");
    path.prepare(48000, 64);
    path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
    check(path.sendParameter(Shaper::SweepOn + 1, 1), "enable sweep");
    check(path.sendParameter(Shaper::modParam(Shaper::Stage::Sweep, mid, Shaper::Type) + 1, 2),
          "select the four-stage phaser");
    for (int n = 0; n < 8; ++n) {
        const auto point = static_cast<Shaper::ModControl>(Shaper::Primary1 + n);
        check(path.sendParameter(Shaper::modParam(Shaper::Stage::Sweep, mid, point) + 1, 50), "set sweep center");
        const auto feedback = static_cast<Shaper::ModControl>(Shaper::Secondary1 + n);
        check(path.sendParameter(Shaper::modParam(Shaper::Stage::Sweep, mid, feedback) + 1, 0), "zero feedback");
    }
    check(path.sendParameter(854, 50), "set middle band's stereo offset");
    std::array<float, 64> input{}, left{}, right{};
    const float* in[]{input.data(), input.data()};
    float* out[]{left.data(), right.data()};
    adi::engine::NodeIo io;
    io.in = in;
    io.out = out;
    io.channels = 2;
    io.frames = 64;
    path.process(io);
    input[0] = .0625f;
    path.process(io);
    const float highCenter = crossed ? right[0] : left[0], lowCenter = crossed ? left[0] : right[0];
    check(std::abs(highCenter / .0625 - .43743840) < 3e-5 && std::abs(lowCenter / .0625 - .86488026) < 3e-5,
          "stereo Sweep reaches the native wrapper and editable external");
}

void panDelayBlend(const std::string& directory, bool crossed) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    adi::device::PdLatencyReceiver latency;
    Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
    path.addSearchPath(ADI_PD_PATCH_DIR);
    std::string error;
    check(path.open(latency, error), "open Pan blend canvas");
    check(path.isNative() == !crossed, "the edited Pan canvas runs through the external");
    path.prepare(48000, 64);
    path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
    for (int id = 107; id <= 114; ++id) check(path.sendParameter(id, -50.f), "set the static mid-band balance");

    // Independently measured impulse gains with a 2 ms range. The two
    // input markers differ in frame, sign and amplitude to expose crossfeed.
    struct Reading { float blend, bandMix, masterMix, leftNow, leftAt21, rightNow, law = 0; };
    constexpr std::array readings{
        Reading{0,   100, 100, .550475776f, 0,           1.303077579f},
        Reading{25,  100, 100, .068809472f, .481666297f, 1.303077579f},
        Reading{50,  100, 100, 0,           .550475776f, 1.303077579f},
        Reading{75,  100, 100, 0,           .775237918f, 1.151538730f},
        Reading{100, 100, 100, 0,          1,            1},
        Reading{100,   0, 100, 1,          0,            1},
        Reading{100, 100,   0, 1,          0,            1},
        Reading{0,   100, 100, .40842030f, 0,            1.4874946f, 1},
        Reading{25,  100, 100, .0625f,     .4375f,       1.5f,       2},
        Reading{75,  100, 100, 0,          .75f,         1.f,        3}};
    std::array<float, 64> left{}, right{}, outL{}, outR{};
    const float* in[]{left.data(), right.data()};
    float* out[]{outL.data(), outR.data()};
    adi::engine::NodeIo io;
    io.in = in;
    io.out = out;
    io.channels = 2;
    io.frames = 64;
    for (const auto& row : readings) {
        check(path.sendParameter(857, row.blend) && path.sendParameter(102, row.bandMix)
              && path.sendParameter(1, row.masterMix) && path.sendParameter(926, row.law),
              "deliver Pan gain law, delay blend and dry/wet controls");
        left.fill(0);
        right.fill(0);
        for (int block = 0; block < 32; ++block) path.process(io);
        left[0] = .5f;
        right[7] = -.25f;
        path.process(io);
        const auto& actualLeft = crossed ? outR : outL;
        const auto& actualRight = crossed ? outL : outR;
        for (std::size_t n = 0; n < left.size(); ++n) {
            const float expectedLeft = n == 0 ? .5f * row.leftNow : n == 21 ? .5f * row.leftAt21 : 0.f;
            const float expectedRight = n == 7 ? -.25f * row.rightNow : 0.f;
            check(std::abs(actualLeft[n] - expectedLeft) < 2e-6f && std::abs(actualRight[n] - expectedRight) < 2e-6f,
                  "settled Pan blend reproduces both measured channel gains and the 21-frame tap");
        }
    }
    check(path.sendParameter(857, 100.f) && path.sendParameter(102, 100.f) && path.sendParameter(1, 100.f),
          "set a fully wet mid-band delay");
    struct Range { float milliseconds; int frames; };
    for (const auto range : {Range{16.875f, 110}, Range{0.f, 0}, Range{40.f, 240}, Range{2.f, 21}}) {
        check(path.sendParameter(923, range.milliseconds) && path.sendParameter(922, 1.f) && path.sendParameter(924, 3.f),
              "apply the physical range independently to each Pan band");
        left.fill(0.f);
        right.fill(0.f);
        for (int i = 0; i < 33; ++i) path.process(io);
        for (int block = 0; block < 5; ++block) {
            left[0] = block == 0 ? .5f : 0.f;
            right[3] = block == 0 ? -.25f : 0.f;
            path.process(io);
            const auto& far = crossed ? outR : outL;
            const auto& nearChannel = crossed ? outL : outR;
            for (int i = 0; i < 64; ++i) {
                const int frame = 64 * block + i;
                check(std::abs(far[i] - (frame == range.frames ? .5f : 0.f)) < 1e-6f
                      && std::abs(nearChannel[i] - (frame == 3 ? -.25f : 0.f)) < 1e-6f,
                      "the range message reaches the audible delay through the selected canvas");
            }
        }
    }
    check(path.sendParameter(Shaper::PanBands + 1, 3) && path.sendParameter(Shaper::PanBands + 2, 82.40689f)
          && path.sendParameter(Shaper::PanBands + 3, 1147.8298f), "select the three Pan crossover bands");
    for (int band = 0; band < 3; ++band) {
        check(path.sendParameter(856 + band, 0.f) && path.sendParameter(925 + band, static_cast<float>(band + 1)),
              "send distinct Pan gain laws to all three bands");
        for (int point = 0; point < 8; ++point) {
            const auto id = Shaper::bandParam(Shaper::Stage::Pan, static_cast<Shaper::Band>(band),
                                              static_cast<Shaper::BandControl>(Shaper::Point1 + point));
            check(path.sendParameter(id + 1, -50.f), "set each band's Pan contour");
        }
    }
    left.fill(0.f);
    right.fill(0.f);
    for (int block = 0; block < 32; ++block) path.process(io);
    left[0] = 1.f;
    right[0] = -.5f;
    path.process(io);
    const auto& actualLeft = crossed ? outR : outL;
    const auto& actualRight = crossed ? outL : outR;
    // First-order crossover impulse coefficients at frames 0 and 17.
    struct BandTap { std::size_t frame; double low, mid, high; };
    for (const auto tap : {BandTap{0, .00037551573, .07461194992, .92501249313},
                           BandTap{17, .00877333507, .00309078321, -.01186411828}}) {
        const double expectedL = std::pow(.550475776, 1.5) * tap.low + .5 * (tap.mid + tap.high);
        const double expectedR = -.5 * (std::pow(1.303077579, 1.5) * tap.low + 1.5 * tap.mid + tap.high);
        check(std::abs(actualLeft[tap.frame] - expectedL) < 2.e-6 && std::abs(actualRight[tap.frame] - expectedR) < 2.e-6,
              "each routed gain law weights its own Pan band on both speakers");
    }

}

void panRangeState(DeviceInstance& device) {
    constexpr std::array ids{922, 923, 924};
    constexpr std::array positions{.25, .5, 1.};
    std::array<double, 3> values{};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto id = std::to_string(ids[i]);
        const auto* desc = device.paramAt(706 + static_cast<int>(i));
        check(desc && desc->id == id && std::abs(device.getParam(id).real - 2.) < 1e-5,
              "the three appended range controls default to two milliseconds");
        set(device, ids[i], positions[i]);
        values[i] = device.getParam(id).real;
    }
    check(values[0] < values[1] && values[1] < values[2] && std::abs(values[2] - 40.) < 1e-5,
          "the range controls store distinct physical values up to forty milliseconds");
    const auto saved = device.saveState("state");
    auto next = open();
    check(next->loadState("state", saved), "restore per-band delay ranges");
    for (std::size_t i = 0; i < ids.size(); ++i)
        check(std::abs(next->getParam(std::to_string(ids[i])).real - values[i]) < 1e-5,
              "each range retains its stored value");
    auto sparse = adi::Payload::from_cbor(saved);
    for (const auto* group : {"params", "real"})
        for (int id : ids) sparse.at(group).erase(std::to_string(id));
    check(next->loadState("state", adi::Payload::to_cbor(sparse)), "apply an earlier state over edited ranges");
    for (int id : ids)
        check(std::abs(next->getParam(std::to_string(id)).real - 2.) < 1e-5,
              "missing range controls restore the calibrated two milliseconds");
}

void panBlendState(DeviceInstance& device) {
    constexpr std::array ids{856, 857, 858};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto id = std::to_string(ids[i]);
        const auto* desc = device.paramAt(415 + static_cast<int>(i));
        check(desc && desc->id == id && device.getParam(id).real == 50., "each Pan delay blend defaults to 50 percent");
        set(device, ids[i], .25 + .25 * static_cast<double>(i));
    }
    const auto saved = device.saveState("state");
    auto next = open();
    check(next->loadState("state", saved), "restore three distinct Pan delay blends");
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto id = std::to_string(ids[i]);
        check(next->getParam(id).real == 25. + 25. * static_cast<double>(i), "Pan delay blend keeps its stable ID");
        set(*next, ids[i], 0.);
    }
    auto sparse = adi::Payload::from_cbor(saved);
    for (const auto* group : {"params", "real"})
        for (int id : ids) sparse.at(group).erase(std::to_string(id));
    check(next->loadState("state", adi::Payload::to_cbor(sparse)), "recall the prior Pan state on a running device");
    for (int id : ids) check(next->getParam(std::to_string(id)).real == 50., "omitted Pan blend recalls its original setting");
}

void driveShapeState(DeviceInstance& device) {
    constexpr std::array physical{1,0,1, 0,1,1, 1,2,3};
    for (std::size_t i = 0; i < physical.size(); ++i) {
        const auto host = 928 + static_cast<int>(i);
        const auto* desc = device.paramAt(656 + static_cast<int>(i));
        const auto id = std::to_string(host);
        check(desc && desc->id == id && device.getParam(id).real == 0., "Drive choices append with Off selected");
        set(device, host, physical[i] / (host < 934 ? 1. : 3.));
    }
    const auto state = device.saveState("state");
    auto next = open();
    check(next->loadState("state", state), "restore the three bands' Drive choices together");
    auto sparse = adi::Payload::from_cbor(state);
    for (std::size_t i = 0; i < physical.size(); ++i) {
        const auto id = std::to_string(928 + static_cast<int>(i));
        check(std::abs(next->getParam(id).real - physical[i]) < 1e-5, "band restores both switches and its menu");
        sparse.at("params").erase(id);
        sparse.at("real").erase(id);
    }
    check(next->loadState("state", adi::Payload::to_cbor(sparse)), "recall a state preceding the Drive choices");
    for (int id = 928; id <= 936; ++id)
        check(next->getParam(std::to_string(id)).real == 0., "sparse recall returns to the prior Drive mode");

    for (const int first : {928, 931, 934}) {
        auto previous = adi::Payload::from_cbor(state);
        for (int id = 928; id <= 936; ++id) {
            if (id >= first && id < first+3) continue;
            previous.at("params").erase(std::to_string(id));
            previous.at("real").erase(std::to_string(id));
        }
        check(next->loadState("state", state) && next->loadState("state", adi::Payload::to_cbor(previous)),
              "recall a state saved with only one optional Drive family");
        for (int id = 928; id <= 936; ++id) {
            const auto index = static_cast<std::size_t>(id-928);
            const double expected = id >= first && id < first+3 ? physical[index] : 0.;
            check(std::abs(next->getParam(std::to_string(id)).real-expected) < 1e-5,
                  "intermediate states retain the original IDs and clear absent families");
        }
    }
}

void driveShapeCanvas(const std::string& directory, bool crossed) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    // Independent stereo impulses at frames 320 and 723, half-strength, 48 kHz.
    enum Shape { rounded, power, sine, triangle, saw };
    struct Case { std::array<Shape,3> bands; bool split; std::array<float,3> left, right; };
    constexpr std::array<Case,8> cases{{
        {{rounded,rounded,rounded},false,{.33306658f},{-.32755145f}},
        {{power,power,power},false,{.44623795f},{-.41529372f}},
        {{sine,sine,sine},false,{.20818031f},{-.18044062f}},
        {{triangle,triangle,triangle},false,{.15459639f},{-.21635081f}},
        {{saw,saw,saw},false,{.31630379f},{-.17763694f}},
        {{rounded,power,sine},true,{.34421364f,.04694234f,-.03637318f},{-.31950659f,-.04419442f,.03664365f}},
        {{triangle,rounded,power},true,{.61537170f,-.04379823f,.00870728f},{-.58093429f,.04487343f,-.00920766f}},
        {{power,saw,rounded},true,{.50446570f,.02793778f,.04626411f},{-.53372568f,-.03096548f,-.04416558f}}}};
    constexpr std::array offsets{16,24,51};
    for (const auto& settings : cases) {
        adi::device::PdLatencyReceiver latency;
        Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
        path.addSearchPath(ADI_PD_PATCH_DIR);
        std::string error;
        check(path.open(latency, error) && path.isNative() == !crossed, "open the expected Drive playback path");
        path.prepare(48000, 64);
        path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
        const auto send = [&](Shaper::Param id, float v) {
            check(path.sendParameter(static_cast<int>(id)+1, v), "send Drive fold control");
        };
        send(Shaper::DriveOn, 1);
        if (settings.split) {
            send(static_cast<Shaper::Param>(Shaper::DriveOn+1), 3);
            send(static_cast<Shaper::Param>(Shaper::DriveOn+2), 160);
            send(static_cast<Shaper::Param>(Shaper::DriveOn+3), 2560);
        }
        for (int b = 0; b < 3; ++b) {
            const auto band = static_cast<Shaper::Band>(b);
            const auto choice = settings.bands[static_cast<std::size_t>(b)];
            const int style = (choice == rounded || choice == sine) ? 1 : choice == saw ? 2 : 0;
            send(Shaper::driveParam(band, Shaper::DriveStyle), static_cast<float>(style));
            send(static_cast<Shaper::Param>(Shaper::DriveLowPower+b), 1);
            send(static_cast<Shaper::Param>(Shaper::DriveLowRounded+b), 1);
            send(static_cast<Shaper::Param>(Shaper::DriveLowFold+b),
                 choice >= sine ? static_cast<float>(choice-sine+1) : 0.f);
            for (int i = 0; i < 8; ++i)
                send(Shaper::driveParam(band, static_cast<Shaper::DriveControl>(Shaper::Drive1+i)), 50);
        }
        std::array<float, 64> left{}, right{}, outL{}, outR{};
        const float* in[]{left.data(), right.data()};
        float* out[]{outL.data(), outR.data()};
        adi::engine::NodeIo io;
        io.in = in;
        io.out = out;
        io.channels = 2;
        io.frames = 64;
        for (int block = 0; block < 13; ++block) {
            left.fill(0);
            right.fill(0);
            if (block == 5) left[0] = .73f;
            if (block == 11) right[19] = -.61f;
            path.process(io);
            for (std::size_t i = 0; i < (settings.split ? offsets.size() : 1); ++i) {
                const int leftFrame = 320+offsets[i], rightFrame = 723+offsets[i];
                if (leftFrame/64 == block)
                    check(std::abs((crossed ? outR : outL)[leftFrame%64]-settings.left[i]) < 5e-5f,
                          "Drive gives the measured response to the positive marker");
                if (rightFrame/64 == block)
                    check(std::abs((crossed ? outL : outR)[rightFrame%64]-settings.right[i]) < 5e-5f,
                          "Drive gives the measured response to the negative marker");
            }
        }
    }
}

void panTransferState(DeviceInstance& device) {
    for (int i = 0; i < 3; ++i) {
        const auto id = std::to_string(919 + i);
        const auto* desc = device.paramAt(703 + i);
        check(desc && desc->id == id && device.getParam(id).real == 0, "each band appends a stereo transfer switch");
        set(device, 919 + i, i == 1 ? 0. : 1.);
    }
    const auto saved = device.saveState("state");
    auto restored = open();
    check(restored->loadState("state", saved), "restore independent stereo transfer modes");
    auto oldState = adi::Payload::from_cbor(saved);
    for (int i = 0; i < 3; ++i) {
        const auto id = std::to_string(919 + i);
        check(restored->getParam(id).real == (i == 1 ? 0. : 1.), "Pan band keeps its own transfer mode");
        oldState.at("params").erase(id);
        oldState.at("real").erase(id);
    }
    check(restored->loadState("state", adi::Payload::to_cbor(oldState)), "restore earlier Pan state on a running device");
    for (int id = 919; id <= 921; ++id)
        check(restored->getParam(std::to_string(id)).real == 0, "old states select the original Pan balance law");
}

void panTransferPath(const std::string& patchDir, bool crossed) {
    using Shaper = adi::dsp::AdiShaper;
    adi::device::LibPdEngine path(patchDir, "ADI-Shaper.pd", 2, 2);
    adi::device::PdLatencyReceiver latency;
    std::string error;
    path.addSearchPath(ADI_PD_PATCH_DIR);
    check(path.open(latency, error), "open the Pan transfer canvas");
    path.prepare(48000, 256);
    path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
    check(path.sendParameter(920, 1) && path.sendParameter(857, 0), "select middle-band stereo transfer");
    for (int i = 0; i < 8; ++i) {
        const auto id = Shaper::bandParam(Shaper::Stage::Pan, Shaper::Band::Mid,
                                         static_cast<Shaper::BandControl>(Shaper::Point1 + i));
        check(path.sendParameter(static_cast<int>(id) + 1, 50), "set the Pan transfer contour");
    }
    std::array<float, 256> left{}, right{}, outLeft{}, outRight{};
    left[80] = .25f;
    right[113] = -.75f;
    const float* inputs[]{left.data(), right.data()};
    float* outputs[]{outLeft.data(), outRight.data()};
    adi::engine::NodeIo io;
    io.in = inputs; io.out = outputs; io.channels = 2; io.frames = 256;
    path.process(io);
    const int offset = path.adapterLatencySamples();
    check(offset == (crossed ? 64 : 0), "canonical and edited Pan paths use the expected adapter");
    for (int i = 0; i < 256 - offset; ++i) {
        const auto dst = static_cast<std::size_t>(offset + i);
        const auto src = static_cast<std::size_t>(i);
        check(outputs[crossed ? 1 : 0][dst] == left[src] + .5f * right[src] &&
              outputs[crossed ? 0 : 1][dst] == .5f * right[src], "both Pan matrix columns emerge from the selected canvas");
    }
}

void panGainLawState(DeviceInstance& device) {
    constexpr std::array ids{925, 926, 927};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto id = std::to_string(ids[i]);
        const auto* desc = device.paramAt(709 + static_cast<int>(i));
        check(desc && desc->id == id && device.getParam(id).real == 0., "Pan gain laws append in band order at the original gain");
        set(device, ids[i], static_cast<double>(i + 1) / 3.);
    }
    const auto saved = device.saveState("state");
    auto next = open();
    check(next->loadState("state", saved), "restore three distinct Pan gain laws");
    for (std::size_t i = 0; i < ids.size(); ++i)
        check(next->getParam(std::to_string(ids[i])).real == static_cast<double>(i + 1), "recall the selected law for each band");
    auto sparse = adi::Payload::from_cbor(saved);
    for (const auto* group : {"params", "real"})
        for (int id : ids) sparse.at(group).erase(std::to_string(id));
    check(next->loadState("state", adi::Payload::to_cbor(sparse)), "recall an earlier Pan state over the three active laws");
    for (int id : ids) check(next->getParam(std::to_string(id)).real == 0., "an omitted Pan gain law recalls the original curve");
    check(next->getParam("857").real == device.getParam("857").real, "the saved Pan delay blend survives gain law recall");
}

void modulationPd(const std::string& editedDir, bool filterOn, bool sweepOn, bool driveOn = false,
                  bool crushOn = false, bool noiseOn = false, bool reverbOn = false, bool followerOn = false,
                  bool holdOn = false, unsigned holdSources = 0, bool filtered = false) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    using Stage = Shaper::Stage;
    constexpr auto mid = Shaper::Band::Mid;
    constexpr int frames = 4096;
    const bool onlyVolume = !filterOn && !sweepOn && !driveOn && !crushOn && !noiseOn && !reverbOn && !followerOn;
    std::array<adi::device::PdLatencyReceiver, 2> latency;
    std::vector<std::pair<Shaper::Param, float>> oracleSends;
    Engine native(ADI_SHAPER_PATCH_DIR, "ADI-Shaper.pd", 2, 2);
    Engine pd(editedDir, "ADI-Shaper.pd", 2, 2);
    const auto decl = adi::device::parsePdDeclarations(adi::builtinCanvas(uid));
    std::string error;
    std::array<Engine*, 2> paths{&native, &pd};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto& path = *paths[i];
        path.addSearchPath(ADI_PD_PATCH_DIR);
        check(path.open(latency[i], error), "modulation canvas opens");
        path.prepare(48000, 97);
        path.bindParameters(decl);
        auto send = [&](Shaper::Param id, float natural) {
            check(path.sendParameter(static_cast<int>(id) + 1, natural), "modulation message accepted");
            if (holdOn && i == 0) oracleSends.emplace_back(id, natural);
        };
        if (followerOn) {
            send(Shaper::WidthBands, 3);
            for (const auto band : {Shaper::Band::Low, mid, Shaper::Band::High}) {
                send(Shaper::followerParam(band, Shaper::FollowerOn), 1);
                send(Shaper::followerParam(band, Shaper::FollowerMode), band == mid ? 1.f : 0.f);
                send(Shaper::followerParam(band, Shaper::FollowerAmount), 70);
                send(Shaper::followerParam(band, Shaper::FollowerDepth), band == mid ? 2.f : 1.f);
                send(Shaper::followerParam(band, Shaper::FollowerOffset), 54);
                send(Shaper::followerParam(band, Shaper::FollowerThreshold), 2);
                send(Shaper::followerParam(band, Shaper::FollowerAttack), 25);
                send(Shaper::followerParam(band, Shaper::FollowerHold), 7);
                send(Shaper::followerParam(band, Shaper::FollowerRelease), 43);
            }
        }
        if (onlyVolume) {
            for (int j = 0; j < 8; ++j)
                send(Shaper::bandParam(Stage::Volume, mid, static_cast<Shaper::BandControl>(Shaper::Point1 + j)), 50);
        }
        if (filterOn && sweepOn) {
            // Feed both processors the first-order middle band on both signal paths.
            send(Shaper::VolumeBands, 3);
            send(static_cast<Shaper::Param>(Shaper::VolumeBands + 1), 82.40689f);
            send(static_cast<Shaper::Param>(Shaper::VolumeBands + 2), 1147.8298f);
            for (const auto band : {Shaper::Band::Low, Shaper::Band::High})
                for (int j = 0; j < 8; ++j)
                    send(Shaper::bandParam(Stage::Volume, band, static_cast<Shaper::BandControl>(Shaper::Point1 + j)), 0);
        }
        if (noiseOn) {
            send(Shaper::NoiseOn, 1);
            send(Shaper::modParam(Stage::Noise, mid, Shaper::Type), 1);
            send(Shaper::noiseParam(mid, Shaper::NoiseMono), 0);
            send(Shaper::noiseParam(mid, Shaper::NoiseOnly), 1);
        }
        send(Shaper::FilterOn, filterOn ? 1.0f : 0.0f);
        send(Shaper::SweepOn, sweepOn ? 1.0f : 0.0f);
        send(Shaper::DriveOn, driveOn ? 1.0f : 0.0f);
        send(Shaper::CrushOn, crushOn ? 1.0f : 0.0f);
        // Exercise actual reordered processing on both the canonical and edited canvases.
        if (driveOn && crushOn) send(Shaper::orderParam(Stage::Crush), 0);
        send(Shaper::SweepMidPolarity, 1.0f);
        for (auto stage : {Stage::Filter, Stage::Sweep, Stage::Crush}) {
            send(Shaper::modParam(stage, mid, Shaper::Type), stage == Stage::Crush ? 0.0f : 1.0f);
            send(Shaper::modParam(stage, mid, Shaper::ModClock), 1);
            send(Shaper::modParam(stage, mid, Shaper::ModRate), 79);
            send(Shaper::modParam(stage, mid, Shaper::PrimaryCurve), 1);
            send(Shaper::modParam(stage, mid, Shaper::SecondaryCurve), 1);
            for (int j = 0; j < 8; ++j) {
                send(Shaper::modParam(stage, mid, static_cast<Shaper::ModControl>(Shaper::Primary1 + j)),
                     15.f + 9.f * static_cast<float>(j));
                send(Shaper::modParam(stage, mid, static_cast<Shaper::ModControl>(Shaper::Secondary1 + j)),
                     70.f - 7.f * static_cast<float>(j));
            }
        }
        send(Shaper::driveParam(mid, Shaper::DriveMix), 67);
        send(Shaper::driveParam(mid, Shaper::DriveStyle), 2);
        send(Shaper::driveParam(mid, Shaper::DriveClock), 1);
        send(Shaper::driveParam(mid, Shaper::DriveRate), 61);
        send(Shaper::driveParam(mid, Shaper::DriveCurve), 1);
        for (int j = 0; j < 8; ++j)
            send(Shaper::driveParam(mid, static_cast<Shaper::DriveControl>(Shaper::Drive1 + j)),
                 80.f - 7.f * static_cast<float>(j));
        if (reverbOn) {
            send(Shaper::ReverbOn, 1);
            send(static_cast<Shaper::Param>(Shaper::ReverbOn + 1), 3);
            for (auto band : {Shaper::Band::Low, mid, Shaper::Band::High}) {
                send(Shaper::modParam(Stage::Reverb, band, Shaper::ModMix), 80);
                send(Shaper::modParam(Stage::Reverb, band, Shaper::ModClock), 1);
                send(Shaper::modParam(Stage::Reverb, band, Shaper::ModRate), 29);
                send(Shaper::modParam(Stage::Reverb, band, Shaper::PrimaryCurve), 1);
                send(Shaper::modParam(Stage::Reverb, band, Shaper::SecondaryCurve), 1);
                for (int j = 0; j < 8; ++j) {
                    const auto point = static_cast<float>(j);
                    send(Shaper::modParam(Stage::Reverb, band, static_cast<Shaper::ModControl>(Shaper::Primary1 + j)),
                         42.f + 6.f * point);
                    send(Shaper::modParam(Stage::Reverb, band, static_cast<Shaper::ModControl>(Shaper::Secondary1 + j)),
                         82.f - 6.f * point);
                }
                send(Shaper::reverbParam(band, Shaper::RoomSize), 38);
                send(Shaper::reverbParam(band, Shaper::RoomDecay), 83);
                send(Shaper::reverbParam(band, Shaper::RoomGate), 1);
                send(Shaper::reverbParam(band, Shaper::RoomWidth), 73);
                send(Shaper::reverbParam(band, Shaper::RoomPredelay), 4);
                send(Shaper::reverbParam(band, Shaper::RoomClearTails), 1);
            }
        }
        if (holdOn) {
            send(static_cast<Shaper::Param>(Shaper::CrushOn + 1), 3);
            if (holdSources != 0) {
                send(static_cast<Shaper::Param>(Shaper::CrushOn + 2), 400);
                send(static_cast<Shaper::Param>(Shaper::CrushOn + 3), 2000);
                for (int j = 0; j < 8; ++j)
                    send(Shaper::bandParam(Stage::Volume, mid, static_cast<Shaper::BandControl>(Shaper::Point1 + j)), 50);
            }
            for (int b = 0; b < 3; ++b) {
                const auto band = static_cast<Shaper::Band>(b);
                using Control = adi::dsp::AdiShaperFollower::Control;
                const float index = static_cast<float>(b);
                if (holdSources != 0) {
                    send(Shaper::holdFollowerSourceParam(band), (holdSources & (1U << b)) != 0 ? 1.f : 0.f);
                    for (int j = 0; j < 8; ++j)
                        send(Shaper::modParam(Stage::Crush, band, static_cast<Shaper::ModControl>(Shaper::Secondary1 + j)),
                             25.f + 12.5f * index);
                }
                if (filtered) {
                    send(Shaper::holdHighPassParam(band), 10.f + 29.f * index);
                    send(Shaper::holdLowPassParam(band), 30.f + 18.f * index);
                }
                const std::array controls{1.f, b == 1 ? 1.f : 0.f, 25.f + 22.f * index, index,
                                          60.f - 12.f * index, 1.f + 3.f * index, 3.f + index,
                                          7.f + index, 15.f + index};
                for (std::size_t n = 0; n < controls.size(); ++n)
                    send(Shaper::holdFollowerParam(band, static_cast<Control>(n)), controls[n]);
            }
        }
    }
    check(native.isNative() && !pd.isNative() && native.adapterLatencySamples() == 0
          && pd.adapterLatencySamples() == 64, "automatic dispatch selects native and buffered edited paths");
    std::array<std::array<float, frames>, 2> input{}, direct{}, editable{};
    for (int f = 0; f < frames; ++f) {
        input[0][static_cast<std::size_t>(f)] = static_cast<float>(.35 * std::sin(.071 * f));
        input[1][static_cast<std::size_t>(f)] = static_cast<float>(.17 * std::cos(.133 * f));
        if (holdSources != 0) {
            input[0][static_cast<std::size_t>(f)] += static_cast<float>(.32 * std::sin(.012 * f));
            input[1][static_cast<std::size_t>(f)] += static_cast<float>(.28 * std::sin(1.53 * f));
        }
    }
    constexpr std::array segments{7, 61, 19, 97};
    for (int f = 0, block = 0; f < frames; ++block) {
        const auto n = std::min(segments[static_cast<std::size_t>(block) % segments.size()], frames - f);
        const float* in[]{input[0].data() + f, input[1].data() + f};
        float* outNative[]{direct[0].data() + f, direct[1].data() + f};
        float* outPd[]{editable[0].data() + f, editable[1].data() + f};
        adi::engine::NodeIo io;
        io.channels = 2;
        io.frames = n;
        io.in = in;
        io.out = outNative;
        native.process(io);
        io.out = outPd;
        pd.process(io);
        if (f < 64 && f + n >= 64) {
            // The editable path delivers queued parameters before its first complete Pd tick.
            const int expected = driveOn ? 16 : 0;
            for (std::size_t i = 0; i < paths.size(); ++i) {
                check(latency[i].latencySamples() == expected, "Drive latency is published when parameters reach DSP");
                latency[i].report(1);
                paths[i]->requestLatencyReport();
                check(latency[i].latencySamples() == expected, "latency query returns the current core latency");
            }
        }
        f += n;
    }
    double errorPeak = 0, effectPeak = 0;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        // The edited canvas routes the external's output to the opposite speaker.
        const auto& crossed = editable[1 - ch];
        for (std::size_t i = 0; i < 64; ++i) check(crossed[i] == 0, "editable adapter startup silence");
        for (std::size_t i = 64; i < frames; ++i) {
            const auto index = i - 64;
            check(std::isfinite(crossed[i]) && std::isfinite(direct[ch][index]), "finite modulation output");
            errorPeak = std::max(errorPeak, static_cast<double>(std::abs(crossed[i] - direct[ch][index])));
            if (onlyVolume)
                check(crossed[i] == 0.5f * input[ch][index] && direct[ch][index] == crossed[i],
                      "half gain is sample-exact through native and edited stereo signal paths");
            const float alignedDry = !driveOn ? input[ch][index] : index >= 16 ? input[ch][index - 16] : 0.0f;
            effectPeak = std::max(effectPeak, static_cast<double>(std::abs(alignedDry - direct[ch][index])));
        }
    }
    check(effectPeak > 0.02 && errorPeak < 1.0e-6, "curved stereo modulation agrees across irregular native and Pd blocks");
    if (holdOn) {
        using Capture = std::array<std::array<float, frames>, 2>;
        const auto bands = {Shaper::Band::Low, mid, Shaper::Band::High};
        // The device owns its Time history and cannot be copied, so each comparison
        // replays the sends the native path received into a fresh core.
        const auto renderWith = [&](auto&& adjust) {
            auto capture = std::make_unique<Capture>();
            auto rack = std::make_unique<Shaper>();
            rack->prepare(48000);
            for (const auto& [param, value] : oracleSends) rack->set(param, value);
            adjust(*rack);
            rack->process(input[0].data(), input[1].data(), (*capture)[0].data(), (*capture)[1].data(), frames);
            return capture;
        };
        const auto peakFromDirect = [&](const Capture& capture) {
            double peak = 0;
            for (std::size_t ch = 0; ch < 2; ++ch)
                for (std::size_t f = 0; f < frames; ++f)
                    peak = std::max(peak, static_cast<double>(std::abs(capture[ch][f] - direct[ch][f])));
            return peak;
        };
        if (filtered) {
            const auto unfiltered = renderWith([&](Shaper& core) {
                for (auto band : bands) {
                    core.set(Shaper::holdHighPassParam(band), 0);
                    core.set(Shaper::holdLowPassParam(band), 100);
                }
            });
            const double difference = peakFromDirect(*unfiltered);
            if (holdSources == 0) check(difference < 1e-6, "stored detector cutoffs are dormant on linked bands");
            else check(difference > .001, "active detector filtering reaches the registered audio");
        }
        const auto oracle = renderWith([](Shaper&) {});
        const auto plain = renderWith([&](Shaper& core) {
            for (auto band : bands) core.set(Shaper::holdFollowerParam(band, adi::dsp::AdiShaperFollower::Control::On), 0);
        });
        if (holdSources != 0) {
            const auto linked = renderWith([&](Shaper& core) {
                for (auto band : bands) core.set(Shaper::holdFollowerSourceParam(band), 0);
            });
            check(peakFromDirect(*linked) > .001, "selecting the full stereo source changes the registered audio for this band");
        }
        double movement = 0;
        for (std::size_t ch = 0; ch < 2; ++ch) {
            for (std::size_t f = 0; f < frames; ++f) {
                check(std::abs((*oracle)[ch][f] - direct[ch][f]) < 1.e-6f, "native controls produce the direct Hold follower response");
                movement = std::max(movement, static_cast<double>(std::abs((*oracle)[ch][f] - (*plain)[ch][f])));
            }
        }
        check(movement > .02, "the signed Hold followers change the rendered audio");
    }
    if (driveOn) {
        const float* in[]{input[0].data(), input[1].data()};
        float* out[]{direct[0].data(), direct[1].data()};
        adi::engine::NodeIo io;
        io.channels = 2;
        io.frames = 64;
        io.in = in;
        io.out = out;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            check(paths[i]->sendParameter(static_cast<int>(Shaper::DriveOn) + 1, 0), "disable active Drive");
            paths[i]->process(io);
            check(latency[i].latencySamples() == 0, "disabling Drive publishes zero latency on delivery");
        }
    }
}

void holdFollowerRecall() {
    constexpr std::array defaults{0., 0., 50., 0., 50., 0., 10., 0., 20.};
    auto source = open();
    set(*source, 341, 1);  // An earlier project can already have Crush active.
    auto earlier = adi::Payload::from_cbor(source->saveState("state"));
    std::array<double, 27> expected{};
    for (int b = 0; b < 3; ++b) {
        for (int n = 0; n < 9; ++n) {
            const int offset = 9 * b + n, id = 877 + offset;
            const auto key = std::to_string(id);
            const auto* descriptor = source->paramAt(724 + offset);
            check(descriptor && descriptor->id == key && std::abs(source->getParam(key).real - defaults[static_cast<std::size_t>(n)]) < 1.e-5,
                  "each band's nine controls have stable IDs and the manual detector defaults");
            for (const auto* group : {"params", "real"})
                if (earlier.contains(group)) earlier.at(group).erase(key);
            const double normalized = n < 2 ? 1. : n == 3 ? (b == 0 ? .5 : 1.) : (.17 + .17 * b + .023 * n);
            set(*source, id, normalized);
            expected[static_cast<std::size_t>(offset)] = source->getParam(key).real;
            check(std::abs(source->getParam(key).real - defaults[static_cast<std::size_t>(n)]) > .01,
                  "every new control was moved before project recall");
        }
    }
    auto manualOnly = adi::Payload::from_cbor(source->saveState("state"));
    for (int band = 0; band < 3; ++band) {
        const auto id = std::to_string(904 + band);
        const auto* descriptor = source->paramAt(751 + band);
        check(descriptor && descriptor->id == id && source->getParam(id).real == 0, "three stable Hold source selectors default to linked");
        for (auto* payload : {&earlier, &manualOnly})
            for (const auto* group : {"params", "real"})
                if (payload->contains(group)) payload->at(group).erase(id);
        set(*source, 904 + band, band == 1 ? 0. : 1.);
    }
    auto restored = open();
    check(restored->loadState("state", source->saveState("state")), "restore 27 Hold follower settings");
    for (std::size_t i = 0; i < expected.size(); ++i)
        check(std::abs(restored->getParam(std::to_string(877 + i)).real - expected[i]) < 1.e-5,
              "all follower settings round-trip through registered device state");
    check(restored->getParam("904").real == 1 && restored->getParam("905").real == 0 && restored->getParam("906").real == 1,
          "each Hold source selection recalls independently");
    set(*restored, 905, 1.);
    check(restored->loadState("state", adi::Payload::to_cbor(manualOnly)), "restore an earlier manual-follower project");
    for (int band = 0; band < 3; ++band)
        check(restored->getParam(std::to_string(904 + band)).real == 0
              && restored->getParam(std::to_string(877 + 9 * band)).real == 1,
              "all previous manual followers remain active on their own linked band");
    check(restored->loadState("state", adi::Payload::to_cbor(earlier)) && restored->getParam("341").real == 1,
          "an earlier sparse project restores over an active Hold follower");
    for (std::size_t i = 0; i < expected.size(); ++i)
        check(std::abs(restored->getParam(std::to_string(877 + i)).real - defaults[i % 9]) < 1.e-5,
              "a missing follower restores disabled with original defaults in every band");
}

void automatedCrossoverPd(const std::string& editedDir) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    constexpr int frames = 8192, tick = 64;
    using Stereo = std::array<std::array<float, frames>, 2>;
    Stereo input{}, nativeAudio{}, editedAudio{};
    for (int n = 0; n < frames; ++n) {
        input[0][n] = static_cast<float>(.4 * std::sin(.115 * n));
        input[1][n] = static_cast<float>(.3 * std::cos(.237 * n));
    }
    const auto render = [&](const std::string& directory, bool native, Stereo& audio) {
        adi::device::PdLatencyReceiver latency;
        Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
        path.addSearchPath(ADI_PD_PATCH_DIR);
        std::string error;
        check(path.open(latency, error) && path.isNative() == native, "open the selected stereo crossover path");
        path.prepare(48000, tick);
        path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
        check(path.sendParameter(2, 1) && path.sendParameter(3, 160), "set an initial low split");
        for (int j = 0; j < 8; ++j) {
            const auto id = Shaper::bandParam(Shaper::Stage::Volume, Shaper::Band::Mid,
                                               static_cast<Shaper::BandControl>(Shaper::Point1 + j));
            check(path.sendParameter(id + 1, 0), "isolate the low band");
        }
        for (int first = 0; first < frames; first += tick) {
            if (first == 512) check(path.sendParameter(3, 3200), "start an audible cutoff change");
            if (first == 3072) check(path.sendParameter(3, 500), "reverse the moving cutoff");
            const float* in[]{input[0].data() + first, input[1].data() + first};
            float* out[]{audio[0].data() + first, audio[1].data() + first};
            adi::engine::NodeIo io;
            io.channels = 2;
            io.frames = tick;
            io.in = in;
            io.out = out;
            path.process(io);
        }
    };
    render(ADI_SHAPER_PATCH_DIR, true, nativeAudio);
    render(editedDir, false, editedAudio);
    check(nativeAudio[0] == editedAudio[1] && nativeAudio[1] == editedAudio[0],
          "live cutoff and reversal agree sample for sample through the edited stereo canvas");
    const auto peak = [&](int first, int last) {
        float value = 0;
        for (int n = first; n < last; ++n) value = std::max(value, std::abs(nativeAudio[0][n]));
        return value;
    };
    check(peak(512, 576) < .09f && peak(2600, 3000) > .29f && peak(7800, 8192) < .24f,
          "both registered frequency edits are heard with the measured direction and gradual onset");
}
// The host splits a block at every lane event (DF-E50), and every piece of
// the segment carries the transport of the segment's first sample. A synced
// curve must keep its own clock after the first piece: lane values for an
// unrelated parameter, at the value it already has, change nothing.
void automationKeepsTheClock() {
    constexpr int frames = 512;
    using Block = std::array<std::array<float, frames>, 2>;
    const auto render = [](bool lanes) {
        auto device = open();
        device->prepare(48000, frames);
        set(*device, 20, 0);  // Volume Mid cycle: a 32nd of a quarter note
        for (int j = 0; j < 8; ++j) set(*device, 23 + j, j / 7.0);
        Block input{}, output{};
        for (auto& channel : input) channel.fill(.5f);
        const float* in[]{input[0].data(), input[1].data()};
        float* out[]{output[0].data(), output[1].data()};
        adi::engine::DeviceTransport position;
        position.playing = true;
        position.quarterNotes = .5;
        adi::engine::NodeIo io;
        io.deviceTransport = &position;
        io.in = in;
        io.out = out;
        io.channels = 2;
        io.frames = frames;
        std::array<adi::engine::Event, 5> events{};
        const double mix = device->getParam("1").normalized;
        for (std::size_t i = 0; i < events.size(); ++i) {
            events[i].frame = static_cast<std::int32_t>(96 * (i + 1));
            events[i].type = adi::engine::EventType::ParamValue;
            events[i].paramId = 1;
            events[i].value = mix;
        }
        if (lanes) io.events = adi::engine::EventSpan{events.data(), static_cast<std::int32_t>(events.size())};
        device->process(io);
        return output;
    };
    const auto plain = render(false), automated = render(true);
    check(plain[0][0] != plain[0][300], "the synced curve moves within the block");
    check(plain == automated, "lane events split the block without rewinding a synced curve");
}

void savedSplitFrequency() {
    using Shaper = adi::dsp::AdiShaper;
    auto device = open();
    set(*device, 2, 1); // three bands
    const auto hz = [](double value) { return std::log(value / 20.0) / std::log(1000.0); };
    set(*device, 4, hz(400.0));
    set(*device, 3, hz(573.9149));
    set(*device, Shaper::VolumeLowSlope + 1, .5);
    set(*device, Shaper::VolumeHighSlope + 1, 1);
    for (const auto band : {Shaper::Band::Low, Shaper::Band::Mid})
        for (int j = 0; j < 8; ++j)
            set(*device, Shaper::bandParam(Shaper::Stage::Volume, band,
                static_cast<Shaper::BandControl>(Shaper::Point1 + j)) + 1, 0);
    const auto state = device->saveState("state");
    device = open();
    check(device->loadState("state", state), "recall the requested split frequencies");
    check(std::abs(device->getParam("3").real - 573.9149) < .001 &&
          std::abs(device->getParam("4").real - 400.0) < .001, "derived cutoff spacing preserves both saved knobs");
    std::array<float, 64> impulse{}, left{}, right{};
    impulse[0] = 1;
    const float* inputs[]{impulse.data(), impulse.data()};
    float* outputs[]{left.data(), right.data()};
    adi::engine::NodeIo io;
    io.channels = 2;
    io.frames = 64;
    io.in = inputs;
    io.out = outputs;
    device->process(io);
    check(left == right && std::abs(left[0] - .74138374f) < 1e-6f &&
          std::abs(left[16] - .03517162f) < 1e-6f, "recalled upper band uses the measured effective spacing");
}


void samePositionTimeLocate() {
    using Shaper = adi::dsp::AdiShaper;
    constexpr auto mid = Shaper::Band::Mid;
    constexpr int delay = 239; // Whole-frame tap at the bottom of the shortest fixed range at 48 kHz.
    using Stereo = std::array<std::array<float, 352>, 2>;
    Stereo input{};
    input[0][0] = .5f;
    input[1][0] = -.75f;
    input[0][97] = -.25f;
    input[1][97] = .125f;

    for (bool locate : {false, true}) {
        auto device = open(256);
        set(*device, Shaper::TimeOn + 1, 1.);
        set(*device, Shaper::timeParam(mid, Shaper::TimeRange) + 1, 5. / 7.);
        for (int point = Shaper::Time1; point <= Shaper::Time8; ++point)
            set(*device, Shaper::timeParam(mid, static_cast<Shaper::TimeControl>(point)) + 1, 0.);

        adi::engine::DeviceTransport position;
        position.playing = false;
        position.bpm = 120.;
        position.sample = 48000;
        position.quarterNotes = 2.;
        adi::engine::TransportInfo transport;
        transport.playing = false;
        transport.bpm = position.bpm;
        transport.timelineSample = position.sample;
        transport.transportRevision = 4;
        Stereo actual{};
        const auto render = [&](int start, int frames) {
            const float* inputs[]{input[0].data() + start, input[1].data() + start};
            float* outputs[]{actual[0].data() + start, actual[1].data() + start};
            adi::engine::NodeIo io;
            io.transport = &transport;
            io.deviceTransport = &position;
            io.sampleRate = 48000.;
            io.frames = frames;
            io.channels = 2;
            io.in = inputs;
            io.out = outputs;
            device->process(io);
        };
        render(0, 83);
        if (locate) ++transport.transportRevision;
        render(83, 237);
        render(320, 32);

        Stereo expected{};
        for (std::size_t channel = 0; channel < expected.size(); ++channel) {
            if (!locate) expected[channel][delay] = input[channel][0];
            expected[channel][97 + delay] = input[channel][97];
        }
        check(actual == expected, locate ? "stopped locate clears earlier Time audio once and plays new stereo input"
                                        : "a repeated stopped position preserves both stereo Time markers");
    }
}

void timePd() {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    constexpr auto mid = Shaper::Band::Mid;
    constexpr int samples = 768, seek = 128, marker = 144;
    Engine native(ADI_SHAPER_PATCH_DIR, "ADI-Shaper.pd", 2, 2);
    Engine pd(ADI_SHAPER_PATCH_DIR, "ADI-Shaper.pd", 2, 2, Engine::Execution::PureData,
              Engine::BlockMode::Buffered);
    const auto decl = adi::device::parsePdDeclarations(adi::builtinCanvas(uid));
    std::array<adi::device::PdLatencyReceiver, 2> latency;
    std::array<Engine*, 2> paths{&native, &pd};
    std::array<std::array<float, samples>, 2> input{};
    input[0][0] = .5f;
    input[1][0] = -.75f;
    input[0][marker] = -.25f;
    input[1][marker] = .5f;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto& path = *paths[i];
        std::string error;
        path.addSearchPath(ADI_PD_PATCH_DIR);
        check(path.open(latency[i], error), "Time fixture opens canvas");
        path.prepare(48000, 97);
        path.bindParameters(decl);
        auto param = [&](Shaper::Param id, float value) {
            check(path.sendParameter(static_cast<int>(id) + 1, value), "Time parameter message");
        };
        param(Shaper::TimeOn, 1);
        param(Shaper::MasterMix, 50);
        param(Shaper::timeParam(mid, Shaper::TimeMix), 50);
        param(Shaper::timeParam(mid, Shaper::TimeRange), 5);
        for (int point = 0; point < 8; ++point)
            param(Shaper::timeParam(mid, static_cast<Shaper::TimeControl>(Shaper::Time1 + point)), 0);
        path.requestLatencyReport();
        const int adapter = path.adapterLatencySamples();
        check(latency[i].latencySamples() == 0 && adapter == (path.isNative() ? 0 : 64),
              "Time reports zero intentional-delay compensation on both paths");
        std::array<std::array<float, samples>, 2> result{};
        constexpr std::array sizes{31, 7, 66, 17, 97};
        for (int f = 0, block = 0; f < samples; ++block) {
            const int n = std::min(sizes[static_cast<std::size_t>(block) % sizes.size()],
                                   (f < seek ? seek : samples) - f);
            const int timeline = f + (f < seek ? 0 : 192000);
            adi::engine::DeviceTransport exact;
            exact.playing = true;
            exact.sample = timeline;
            exact.quarterNotes = timeline / 24000.0;
            adi::engine::TransportInfo transport;
            transport.playing = true;
            transport.timelineSample = timeline;
            transport.bar = 1 + timeline / 96000;
            transport.beat = 1 + timeline / 24000 % 4;
            transport.ticksInQuarter = std::llround((timeline % 24000) * (adi::textproj::kPPQ / 24000.0));
            const float* in[]{input[0].data() + f, input[1].data() + f};
            float* out[]{result[0].data() + f, result[1].data() + f};
            adi::engine::NodeIo io;
            io.deviceTransport = &exact;
            io.transport = &transport;
            io.in = in;
            io.out = out;
            io.channels = 2;
            io.frames = n;
            path.process(io);
            f += n;
        }
        for (std::size_t ch = 0; ch < input.size(); ++ch) {
            for (int f = 0; f < samples - adapter; ++f) {
                const float expected = .75f * input[ch][static_cast<std::size_t>(f)]
                                     + (f == marker + 239 ? .25f * input[ch][marker] : 0.f);
                check(result[ch][static_cast<std::size_t>(f + adapter)] == expected,
                      "Time preserves immediate dry, resets stale history on seek and returns the new delayed marker");
            }
        }
    }
}
// The final three ranks retain the stage identities of the full project format.
// Pitch parameters 464..535 and Pitch rank 724 may occur in an imported state.
void combinedDeclarations(DeviceInstance& device) {
    int ordinal = 418;
    const auto next = [&](int id) {
        const auto* descriptor = device.paramAt(ordinal++);
        check(descriptor && descriptor->id == std::to_string(id), "combined stages retain their declaration order");
    };
    for (int id = 716; id <= 722; ++id) next(id);
    for (int id = 536; id <= 715; ++id) next(id);
    for (int id = 416; id <= 463; ++id) next(id);
    for (int id : {723, 725, 726}) next(id);
    for (int id = 928; id <= 936; ++id) next(id);
    for (int id = 754; id <= 764; ++id) next(id);
    for (int id = 727; id <= 753; ++id) next(id);
    for (int id = 919; id <= 927; ++id) next(id);
    for (int id = 850; id <= 852; ++id) next(id);
    for (int id = 913; id <= 918; ++id) next(id);
    check(ordinal == device.paramCount(), "all combined stage descriptors are accounted for");
    for (int id : {464, 500, 535, 724})
        check(!device.setParam(std::to_string(id), ParamValue::fromNormalized(1.)), "absent Pitch controls reject writes");
    check(device.getParam("723").real == 78 && device.getParam("725").real == 98
          && device.getParam("726").real == 108, "stage ranks retain their original identity defaults");
}

using StageAudio = std::array<std::vector<float>, 2>;

template<class Device>
StageAudio combinedAudio(Device& device, bool crossed) {
    constexpr std::size_t frames = 8192;
    StageAudio result{std::vector<float>(frames), std::vector<float>(frames)};
    std::array<float, 64> left{}, right{};
    const float* inputs[]{left.data(), right.data()};
    adi::engine::NodeIo io;
    io.channels = 2;
    io.frames = 64;
    io.in = inputs;
    for (std::size_t offset = 0; offset < frames; offset += left.size()) {
        for (std::size_t i = 0; i < left.size(); ++i) {
            const auto t = static_cast<double>(offset + i);
            left[i] = static_cast<float>(.32 * std::cos(.039 * t));
            right[i] = static_cast<float>(.18 * std::sin(.071 * t));
        }
        float* outputs[]{result[crossed ? 1 : 0].data() + offset, result[crossed ? 0 : 1].data() + offset};
        io.out = outputs;
        device.process(io);
    }
    return result;
}

double combinedDifference(const StageAudio& a, const StageAudio& b) {
    double peak = 0.;
    for (std::size_t ch = 0; ch < a.size(); ++ch) {
        for (std::size_t i = 0; i < a[ch].size(); ++i) {
            check(std::isfinite(a[ch][i]) && std::isfinite(b[ch][i]), "finite combined stage audio");
            peak = std::max(peak, std::abs(static_cast<double>(a[ch][i]) - b[ch][i]));
        }
    }
    return peak;
}

StageAudio restoredStageAudio(const adi::Payload& state, const std::string& editedDir) {
    using Engine = adi::device::LibPdEngine;
    auto restored = open();
    check(restored->loadState("state", adi::Payload::to_cbor(state)), "restore combined stage project");
    const auto decl = adi::device::parsePdDeclarations(adi::builtinCanvas(uid));
    const auto savedAgain = adi::Payload::from_cbor(restored->saveState("state"));
    for (const auto& param : decl.params) {
        const auto key = std::to_string(param.id);
        for (const auto* group : {"params", "real", "units"})
            check(savedAgain.at(group).at(key) == state.at(group).at(key), "normalized values, units and real values round-trip exactly");
    }
    const auto expected = combinedAudio(*restored, false);
    // DeviceInstance implements project state. Each physical value recovered from
    // that state is also delivered via both registered Pd parameter paths.
    for (bool crossed : {false, true}) {
        adi::device::PdLatencyReceiver latency;
        Engine path(crossed ? editedDir : ADI_SHAPER_PATCH_DIR, "ADI-Shaper.pd", 2, 2,
                    Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
        std::string error;
        path.addSearchPath(ADI_PD_PATCH_DIR);
        check(path.open(latency, error) && path.isNative() != crossed, "select canonical native and crossed generic Pd routes");
        path.prepare(48000, 64);
        path.bindParameters(decl);
        for (const auto& param : decl.params) {
            const auto value = restored->getParam(std::to_string(param.id));
            check(path.sendParameter(param.id, static_cast<float>(value.real)), "send every recalled value through its declared receiver");
        }
        check(!path.sendParameter(724, 0) && !path.sendParameter(464, 1), "unbound Pitch receivers remain absent");
        const auto actual = combinedAudio(path, crossed);
        check(latency.latencySamples() == restored->latencySamples() && path.adapterLatencySamples() == 0,
              "both whole-block routes report the recalled device latency without an adapter delay");
        check(combinedDifference(expected, actual) < 2.e-6, "restored project and both Pd routes reproduce the same stereo stream");
    }
    return expected;
}

void combinedStageState(const std::string& editedDir) {
    auto device = open();
    combinedDeclarations(*device);
    for (int id = 716; id <= 722; ++id) set(*device, id, (12. + 10. * (id - 716)) / 127.);
    set(*device, 723, 80. / 127.);
    set(*device, 725, 50. / 127.);
    set(*device, 726, 110. / 127.);
    for (int id = 23; id <= 30; ++id) set(*device, id, 0.); // Volume silences the full-band input.
    for (int id : {416, 536, 626, 438, 441, 572, 575, 576, 661, 664, 665}) set(*device, id, 1.);
    set(*device, 440, .5);      // Independent free-running Time, Noise and Room clocks.
    set(*device, 574, .5625);
    set(*device, 663, .4375);
    set(*device, 436, 5. / 7.); // 2.5 ms Time range
    set(*device, 570, 1.);      // Pink Noise responds to both level and pitch.
    set(*device, 593, 0.);      // Independent noise in the two channels
    set(*device, 660, .75);     // Room mix
    set(*device, 682, .25);     // Short room
    for (int i = 0; i < 8; ++i) {
        set(*device, 442 + i, (2. + (3*i)%11) / 16.);  // Time height
        set(*device, 577 + i, (7. + (5*i)%9) / 16.);   // Noise level
        set(*device, 585 + i, (3. + (7*i)%11) / 16.);  // Noise pitch
        set(*device, 666 + i, (6. + (2*i)%9) / 16.);   // Room send
        set(*device, 674 + i, (5. + (7*i)%10) / 16.);  // Room return
    }
    const auto state = adi::Payload::from_cbor(device->saveState("state"));
    check(state.at("real").at("442").get<double>() == 12.5 && state.at("real").at("577").get<double>() == 43.75
          && state.at("real").at("585").get<double>() == -15. && state.at("real").at("666").get<double>() == 37.5
          && state.at("real").at("674").get<double>() == 31.25, "independent curves save their authored values in the correct units");
    const auto sound = restoredStageAudio(state, editedDir);
    const StageAudio silent{std::vector<float>(sound[0].size()), std::vector<float>(sound[1].size())};
    check(combinedDifference(sound, silent) > .001, "Noise after Volume reaches the output");
    for (const int rank : {723, 725, 726}) {
        check(device->loadState("state", adi::Payload::to_cbor(state)), "restore independent rank fixture");
        set(*device, rank, 1. / 127.);
        const auto moved = restoredStageAudio(adi::Payload::from_cbor(device->saveState("state")), editedDir);
        if (rank == 725)
            check(combinedDifference(moved, silent) == 0., "Volume after Noise removes its complete output");
        check(combinedDifference(moved, sound) > .0001, "moving the individual Time, Noise or Room rank changes stereo audio");
    }
    auto withPitch = state;
    for (const int id : {464, 500, 535, 724}) {
        const auto key = std::to_string(id);
        withPitch["params"][key] = .75;
        withPitch["real"][key] = 93.;
    }
    check(combinedDifference(sound, restoredStageAudio(withPitch, editedDir)) == 0., "saved Pitch fields leave all represented stages unchanged");
    auto previous = state;
    for (const auto* group : {"params", "real"}) {
        for (int id = 416; id <= 463; ++id) previous.at(group).erase(std::to_string(id));
        for (int id = 536; id <= 715; ++id) previous.at(group).erase(std::to_string(id));
        for (const int id : {723, 725, 726}) previous.at(group).erase(std::to_string(id));
    }
    check(device->loadState("state", adi::Payload::to_cbor(previous)), "restore a project that predates the three added stages");
    check(device->getParam("416").real == 0 && device->getParam("536").real == 0 && device->getParam("626").real == 0,
          "older project restores Time, Noise and Room to Off");
    combinedDeclarations(*device);
    check(combinedDifference(combinedAudio(*device, false), silent) == 0., "old project again plays just the original zero Volume gate");
}
void earlierFilterProject(const std::string& editedDir) {
    auto device = open();
    set(*device, 134, 1.);       // Full-band Filter
    set(*device, 163, .25);      // Previous 12 dB low-pass choice
    set(*device, 287, 1.);       // A newer Drive control from the earlier project
    set(*device, 932, 1.);
    for (int i = 0; i < 8; ++i) {
        set(*device, 170 + i, (4. + i) / 16.);
        set(*device, 178 + i, .375);
        set(*device, 315 + i, .5);
    }
    const auto original = adi::Payload::from_cbor(device->saveState("state"));
    const auto legacyAudio = restoredStageAudio(original, editedDir);
    auto previous = original;
    for (const int id : {850, 851, 852, 913, 914, 915, 916, 917, 918}) {
        set(*device, id, 1.);
        for (const auto* group : {"params", "real", "units"}) previous.at(group).erase(std::to_string(id));
    }
    check(previous.at("params").size() == 712, "earlier project retains every previous parameter");
    const auto withPeak = adi::Payload::from_cbor(device->saveState("state"));
    check(combinedDifference(legacyAudio, restoredStageAudio(withPeak, editedDir)) > .0001,
          "the active signed Peak changes the Filter output on both device routes");
    check(device->loadState("state", adi::Payload::to_cbor(previous)), "reopen a project predating the Filter extensions");
    const auto recalled = adi::Payload::from_cbor(device->saveState("state"));
    for (const auto* group : {"params", "real", "units"})
        check(recalled.at(group) == original.at(group), "the older Filter project restores all values and units");
    check(combinedDifference(legacyAudio, restoredStageAudio(recalled, editedDir)) == 0.,
          "reopening restores the earlier Filter and Drive samples on both device routes");
}

void combinedControlRecall(const std::string& editedDir) {
    auto device = open();
    const auto defaults = adi::Payload::from_cbor(device->saveState("state"));
    // The earlier ten-stage project already contained an audible legacy Pan and Drive.
    set(*device, 287, 1.);
    set(*device, 857, .3);
    for (int point = 0; point < 8; ++point) {
        set(*device, 107 + point, .25);
        set(*device, 315 + point, .5);
    }
    auto earlier = adi::Payload::from_cbor(device->saveState("state"));
    const auto legacySound = restoredStageAudio(earlier, editedDir);
    const auto appended = [](int id) {
        return (id >= 727 && id <= 764) || (id >= 850 && id <= 852) || (id >= 913 && id <= 936);
    };
    for (int id = 1; id <= 936; ++id) {
        if (!appended(id)) continue;
        for (const auto* group : {"params", "real", "units"}) earlier.at(group).erase(std::to_string(id));
    }

    set(*device, 736, 1.);       // Width follower, mid band
    set(*device, 755, .5);       // Volume loop trigger
    set(*device, 920, 1.);       // Transfer the opposite Pan channel
    set(*device, 923, .5);       // Twenty milliseconds
    set(*device, 926, 1.);       // Independent standard Pan gain law
    set(*device, 932, 1.);       // Mid-band signed Drive
    const auto populated = adi::Payload::from_cbor(device->saveState("state"));
    const auto newSound = restoredStageAudio(populated, editedDir);
    check(combinedDifference(newSound, legacySound) > .001, "simultaneous modulation, Pan and Drive choices change stereo audio");
    check(device->loadState("state", adi::Payload::to_cbor(earlier)), "recall the earlier ten-stage project over active controls");
    const auto recalled = adi::Payload::from_cbor(device->saveState("state"));
    for (int id = 1; id <= 936; ++id) {
        if (!appended(id)) continue;
        const auto key = std::to_string(id);
        for (const auto* group : {"params", "real", "units"})
            check(recalled.at(group).at(key) == defaults.at(group).at(key), "all appended parameters regain their original defaults");
    }
    check(combinedDifference(restoredStageAudio(recalled, editedDir), legacySound) == 0.,
          "sparse ten-stage project reproduces the original stereo samples after active controls are cleared");
}

void audioTriggerPd() {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    constexpr std::size_t length = 8192;
    std::array<std::vector<float>, 2> native;
    const auto declarations = adi::device::parsePdDeclarations(adi::builtinCanvas(uid));
    for (auto execution : {Engine::Execution::Automatic, Engine::Execution::PureData}) {
        adi::device::PdLatencyReceiver latency;
        Engine path(ADI_SHAPER_PATCH_DIR, "ADI-Shaper.pd", 2, 2, execution, Engine::BlockMode::WholeBlocks);
        std::string error;
        path.addSearchPath(ADI_PD_PATCH_DIR);
        check(path.open(latency, error), "audio-trigger canvas opens");
        check(path.isNative() == (execution == Engine::Execution::Automatic), "requested DSP route is active");
        path.prepare(48000, 256);
        path.bindParameters(declarations);
        const auto write = [&](Shaper::Param control, float value) {
            check(path.sendParameter(static_cast<int>(control) + 1, value), "trigger control accepts its declaration ID");
        };
        write(Shaper::TriggerMidMode, 2);
        write(Shaper::TriggerMidEnd, 75);
        write(Shaper::bandParam(Shaper::Stage::Volume, Shaper::Band::Mid, Shaper::Cycle), .5f);
        write(Shaper::bandParam(Shaper::Stage::Volume, Shaper::Band::Mid, Shaper::Point1), 0);
        std::array<std::vector<float>, 2> signal{std::vector<float>(length), std::vector<float>(length, .0125f)};
        signal[0][1111] = .7f;
        for (std::size_t frame = 0; frame < length; frame += 256) {
            const float* in[]{signal[0].data() + frame, signal[1].data() + frame};
            float* out[]{signal[0].data() + frame, signal[1].data() + frame};
            adi::engine::NodeIo io;
            io.in = in;
            io.out = out;
            io.frames = 256;
            io.channels = 2;
            path.process(io);
        }
        check(latency.latencySamples() == 672 && signal[1][1781] == .0125f && signal[1][1782] == 0.f &&
              signal[1][3283] == .0125f, "reported delay and non-grid onset reach the shipped device");
        if (path.isNative()) native = std::move(signal);
        else check(signal == native, "editable Pd and registered native Volume retrigger match sample for sample");
    }
}

} // namespace

int main() try {
    auto device = open();
    const auto* info = adi::findBuiltin(uid);
    check(info && info->name == "ADI-Shaper" && device->paramCount() == 763, "device registration");
    for (int id = 727; id <= 753; ++id) {
        const auto* control = device->paramAt(676 + id - 727);
        check(control && control->id == std::to_string(id), "Width follower follows the existing trigger declarations");
    }
    for (int id = 1; id <= 415; ++id)
        check(device->paramAt(id - 1) && device->paramAt(id - 1)->id == std::to_string(id), "contiguous stable parameter IDs");
    for (int i = 0; i < 90; ++i) {
        const auto* option = device->paramAt(425 + i);
        check(option && option->id == std::to_string(536 + i), "Noise preserves every earlier declaration index");
    }
    auto prior = adi::Payload::from_cbor(device->saveState("state"));
    for (const auto* group : {"params", "real"})
        for (int id = 536; id <= 625; ++id) prior.at(group).erase(std::to_string(id));
    set(*device, 536, 1.);
    check(device->getParam("536").real == 1., "enable Noise on a running device");
    check(device->loadState("state", adi::Payload::to_cbor(prior)) && device->getParam("536").real == 0.,
          "a pre-Noise state restores the disabled stage on a running device");
    for (int id = 626; id <= 715; ++id) {
        const auto* descriptor = device->paramAt(515 + id - 626);
        check(descriptor && descriptor->id == std::to_string(id), "room descriptor keeps its saved ID");
    }
    for (int id : {464, 535, 724, 855, 2049})
        check(!device->setParam(std::to_string(id), ParamValue::fromNormalized(1)), "undeclared ID rejected");
    for (int id = 416; id <= 463; ++id) {
        const auto* desc = device->paramAt(605 + id - 416);
        check(desc && desc->id == std::to_string(id), "Time appends its stable IDs after the previous descriptors");
    }
    for (int band = 0; band < 3; ++band) {
        const auto id = std::to_string(850 + band);
        check(device->paramAt(712 + band) && device->paramAt(712 + band)->id == id
              && device->getParam(id).real == 0, "three stable stereo offset IDs default to center");
    }
    const auto savedOffsets = [&] {
        auto source = open();
        set(*source, 850, 0.);
        set(*source, 851, .75);
        set(*source, 852, 1.);
        return source->saveState("state");
    }();
    check(device->loadState("state", savedOffsets) && device->getParam("850").real == -100
          && device->getParam("851").real == 50 && device->getParam("852").real == 100,
          "each band's signed stereo offset round-trips independently");
    device = open();
    for (int band = 0; band < 3; ++band) {
        const auto id = std::to_string(853 + band);
        check(device->paramAt(721 + band) && device->paramAt(721 + band)->id == id
              && device->getParam(id).real == 0, "three stable Sweep stereo offset IDs default to center");
    }
    const auto savedSweepOffsets = [&] {
        auto source = open();
        set(*source, 853, 0.);
        set(*source, 854, .75);
        set(*source, 855, 1.);
        return source->saveState("state");
    }();
    check(device->loadState("state", savedSweepOffsets) && device->getParam("853").real == -100
          && device->getParam("854").real == 50 && device->getParam("855").real == 100,
          "each band's signed Sweep stereo offset round-trips independently");
    device = open();
    for (int band = 0; band < 3; ++band) {
        const auto id = std::to_string(865 + band);
        check(device->paramAt(760 + band) && device->paramAt(760 + band)->id == id
              && device->getParam(id).real == 0, "three stable quantizer biases default to zero");
    }
    const auto savedBias = [&] {
        auto source = open();
        set(*source, 865, 0.);
        set(*source, 866, .75);
        set(*source, 867, 1.);
        return source->saveState("state");
    }();
    check(device->loadState("state", savedBias) && device->getParam("865").real == 0
          && device->getParam("866").real == 75 && device->getParam("867").real == 100,
          "each band's quantizer bias round-trips independently");
    device = open();
    for (int i = 0; i < 3; ++i)
        check(device->paramAt(715 + i) && device->paramAt(715 + i)->id == std::to_string(913 + i),
              "append the peak switches after the legacy parameters");
    for (int i = 0; i < 3; ++i)
        check(device->paramAt(718 + i) && device->paramAt(718 + i)->id == std::to_string(916 + i),
              "append the steep switches after the other Filter extensions");
    steepFilterRecall();
    filterPeakRecall();
    combinedFilterRecall();
    panBlendState(*device);
    panTransferState(*device);
    panRangeState(*device);
    panGainLawState(*device);
    device = open();
    for (int id = 754; id <= 764; ++id)
        check(device->paramAt(665 + id - 754)->id == std::to_string(id), "new trigger controls have sparse stable IDs");
    auto sparse = adi::Payload::from_cbor(device->saveState("state"));
    for (int id = 754; id <= 764; ++id) {
        sparse.at("params").erase(std::to_string(id));
        sparse.at("real").erase(std::to_string(id));
    }
    std::array<float, 64> probeL{}, probeR{};
    const float* probeInputs[]{probeL.data(), probeR.data()};
    float* probeOutputs[]{probeL.data(), probeR.data()};
    adi::engine::NodeIo probe;
    probe.channels = 2;
    probe.frames = 64;
    probe.in = probeInputs;
    probe.out = probeOutputs;
    set(*device, 755, 1);
    device->process(probe);
    check(device->getParam("755").real == 2 && device->latencySamples() == 672,
          "running one-shot publishes its delay before recall");
    check(device->loadState("state", adi::Payload::to_cbor(sparse)), "recall a project predating audio retrigger");
    device->process(probe);
    check(device->getParam("755").real == 0 && device->latencySamples() == 0,
          "a sparse project resets detector delay and restores legacy transport sync");
    check(device->wantsDeviceTransport() && device->latencySamples() == 0, "position and latency");
    for (int id = 23; id <= 26; ++id) set(*device, id, 0);  // half-bar gate
    for (int id = 65; id <= 72; ++id) set(*device, id, 0);  // mono
    // Restore an actual earlier-format parameter map, with no Filter or Sweep keys.
    auto state = adi::Payload::from_cbor(device->saveState("state"));
    for (const auto* group : {"params", "real", "units"})
        for (int id : {850, 851, 852, 853, 854, 855, 865, 866, 867, 913, 914, 915, 916, 917, 918}) state.at(group).erase(std::to_string(id));
    for (const auto* group : {"params", "real"})
        for (int id = 134; id <= 463; ++id) state.at(group).erase(std::to_string(id));
    device = open();
    set(*device, 416, 1.0);
    set(*device, 423, 0.);
    set(*device, 851, 1.);
    set(*device, 854, 1.);
    set(*device, 866, 1.);
    check(device->loadState("state", adi::Payload::to_cbor(state)) && device->getParam("851").real == 0
          && device->getParam("854").real == 0 && device->getParam("866").real == 0,
          "restore earlier curve, width, centered Filter and Sweep and unbiased Crush");
    check(device->getParam("416").real == 0. && device->getParam("423").real == 100.,
          "older sessions reset Time enable and band mix on a running device");
    check(device->getParam("134").real == 0 && device->getParam("209").real == 0,
          "earlier sessions open with both optional stages disabled");

    std::array<float, 64> l, r, outL{}, outR{};
    l.fill(.375f);
    r.fill(.25f);
    const float* inputs[]{l.data(), r.data()};
    float* outputs[]{outL.data(), outR.data()};
    adi::engine::DeviceTransport position;
    position.playing = true;
    adi::engine::NodeIo io;
    io.deviceTransport = &position;
    io.in = inputs;
    io.out = outputs;
    io.channels = 2;
    io.frames = 64;
    auto render = [&](double qn, bool playing, float expected) {
        position.quarterNotes = qn;
        position.playing = playing;
        device->process(io);
        for (std::size_t i = 0; i < outL.size(); ++i)
            check(outL[i] == expected && outR[i] == expected, "gate, width and transport position");
    };
    render(3.25, true, .3125f);
    render(.25, true, 0);
    render(2.125, false, .3125f);
    render(0, true, 0);
    render(4 * 400000.0 + 3, true, .3125f);
    // A block-level 2/2 bar/beat/remainder can be identical at QN 0 and 1.
    // The exact position on the internal device must distinguish both. A
    // later change to 3/4 must retain the actual position of bar two.
    set(*device, 25, 1);
    set(*device, 26, 1);
    adi::engine::TransportInfo blockStart;
    blockStart.playing = true;
    blockStart.timeSigNumerator = 2;
    blockStart.timeSigDenominator = 2;
    io.transport = &blockStart;
    position.numerator = 2;
    position.denominator = 2;
    render(0, true, 0);
    render(1, true, .3125f);
    blockStart.timeSigNumerator = 3;
    blockStart.timeSigDenominator = 4;
    blockStart.bar = 2;
    position.numerator = 3;
    position.denominator = 4;
    render(4, true, 0);

    // The graph split at frame 31. The segment position is already advanced.
    outL.fill(-1);
    outR.fill(-1);
    io.blockOffset = 31;
    io.frames = 17;
    position.quarterNotes = 1.25;
    device->process(io);
    for (std::size_t i = 0; i < outL.size(); ++i) {
        const float expected = i >= 31 && i < 48 ? .3125f : -1.0f;
        check(outL[i] == expected && outR[i] == expected, "exact sub-block position and bounds");
    }
    set(*device, 134, 1);
    set(*device, 209, 1);
    set(*device, 170, 0.37);
    set(*device, 253, 0.72);
    set(*device, 285, 1);
    // Omit newer parameters in both stored representations. Descriptor positions
    // account for the earlier Pan controls whose persistent IDs are sparse.
    auto earlierState = [&](int oldCount) {
        auto state = adi::Payload::from_cbor(device->saveState("state"));
        for (int index = oldCount; index < device->paramCount(); ++index) {
            const auto& id = device->paramAt(index)->id;
            state.at("params").erase(id);
            state.at("real").erase(id);
        }
        return state;
    };
    auto modState = earlierState(286);
    device = open();
    check(device->loadState("state", adi::Payload::to_cbor(modState)) && device->getParam("134").real == 1
          && device->getParam("209").real == 1 && std::abs(device->getParam("170").real - 37) < 1e-4
          && std::abs(device->getParam("253").real - 72) < 1e-4 && device->getParam("285").real == 1,
          "both stages and shapes restore from earlier project state");
    check(device->getParam("287").real == 0 && device->getParam("341").real == 0 && device->latencySamples() == 0,
          "earlier five-stage projects restore without drive latency or degradation");
    const auto firstEpoch = device->latencyEpoch();
    set(*device, 287, 1);
    set(*device, 341, 1);
    device->process(io);
    check(device->latencySamples() == 16 && device->latencyEpoch() != firstEpoch, "catalogued host republishes Drive latency");
    auto sevenStageState = earlierState(418);
    device = open();
    check(device->loadState("state", adi::Payload::to_cbor(sevenStageState)), "restore seven-stage project state");
    device->process(io);
    check(device->latencySamples() == 16 && device->getParam("341").real == 1 && device->getParam("416").real == 0
          && device->getParam("463").real == 100, "old state restores Drive, Crush and neutral Time defaults");
    set(*device, 416, 1);
    set(*device, 463, .31);
    const auto eightStageState = device->saveState("state");
    device = open();
    check(device->loadState("state", eightStageState) && device->getParam("416").real == 1
          && std::abs(device->getParam("463").real - 31) < 1e-4, "Time enable and last curve point persist");
    // Recall a sparse pre-Drive project into a running instance with Drive active.
    check(device->loadState("state", adi::Payload::to_cbor(modState)), "recall sparse project over an active instance");
    device->process(io);
    check(device->latencySamples() == 0 && device->getParam("287").real == 0
          && device->getParam("341").real == 0 && device->getParam("209").real == 1,
          "missing stages regain defaults and release PDC while saved stages stay enabled");
    // A value saved only in its unit (no position) is read, not reset to its default.
    auto realOnly = sevenStageState;
    realOnly.at("params").erase("170");
    check(realOnly.at("real").contains("170") && std::abs(open()->getParam("170").real - 37) > 1,
          "fixture: Filter value saved in its unit differs from the default");
    device = open();
    check(device->loadState("state", adi::Payload::to_cbor(realOnly))
          && std::abs(device->getParam("170").real - 37) < 1e-4, "a value saved only in its unit is kept");
    auto preOrder = sevenStageState;
    for (int i = 0; i < 7; ++i) {
        const auto id = std::to_string(716 + i);
        const auto* desc = device->paramAt(418 + i);
        check(desc && desc->id == id, "stage order appends after the existing Pan controls");
        preOrder.at("params").erase(id);
        preOrder.at("real").erase(id);
        set(*device, 716 + i, (68.0 - 10.0 * i) / 127.0);
    }
    const auto reversedOrder = device->saveState("state");
    device = open();
    check(device->loadState("state", reversedOrder), "load reordered stages");
    for (int i = 0; i < 7; ++i)
        check(device->getParam(std::to_string(716+i)).real == 68 - 10*i, "saved rank restored");
    check(device->loadState("state", adi::Payload::to_cbor(preOrder)), "load a project without ranks");
    for (int i = 0; i < 7; ++i)
        check(device->getParam(std::to_string(716+i)).real == 8 + 10*i, "old project restores legacy order");
    auto beforeNoise = sevenStageState;
    for (int id = 536; id <= 625; ++id) {
        beforeNoise.at("params").erase(std::to_string(id));
        beforeNoise.at("real").erase(std::to_string(id));
    }
    set(*device, 536, 1);
    check(device->loadState("state", adi::Payload::to_cbor(beforeNoise)) && device->getParam("536").real == 0,
          "a pre-Noise state disables an already running Noise stage");
    device = open();
    set(*device, 67, .75);  // Middle band's Width point at the current beat.
    auto earlier = adi::Payload::from_cbor(device->saveState("state"));
    for (int id = 626; id <= 715; ++id) {
        earlier.at("params").erase(std::to_string(id));
        earlier.at("real").erase(std::to_string(id));
    }
    set(*device, 626, 1);
    set(*device, 660, 1);
    set(*device, 715, 1);
    device->process(io);
    check(outL[31] != l[31], "active full-wet room changes the signal before recall");
    const auto roomState = device->saveState("state");
    check(device->loadState("state", adi::Payload::to_cbor(earlier)), "recall pre-room state on a running room");
    device->process(io);
    check(device->getParam("626").real == 0 && device->getParam("660").real == 25
          && device->getParam("715").real == 0, "earlier state restores room defaults");
    constexpr float sentinel = -5;
    outL.fill(sentinel);
    outR.fill(sentinel);
    io.blockOffset = 0;
    io.frames = 64;
    device->process(io);
    const auto restoredLeft = outL, restoredRight = outR;
    auto untouched = open();
    set(*untouched, 67, .75);
    outL.fill(sentinel);
    outR.fill(sentinel);
    untouched->process(io);
    check(outL == restoredLeft && outR == restoredRight && outL.front() != sentinel && outL.front() != l.front(),
          "loading the earlier state over a running room restores its stereo audio");
    device = open();
    check(device->loadState("state", roomState) && device->getParam("626").real == 1
          && device->getParam("660").real == 100 && device->getParam("715").real == 1,
          "room control endpoints recall");
    auto beforeFollower = sevenStageState;
    for (int id = 727; id <= 753; ++id) {
        beforeFollower.at("params").erase(std::to_string(id));
        beforeFollower.at("real").erase(std::to_string(id));
    }
    set(*device, 736, 1);
    set(*device, 753, .83);
    const auto followerState = device->saveState("state");
    device = open();
    check(device->loadState("state", followerState) && device->getParam("736").real == 1 &&
          std::abs(device->getParam("753").real - 83) < 1e-4, "Width follower settings persist");
    check(device->loadState("state", adi::Payload::to_cbor(beforeFollower)) &&
          device->getParam("736").real == 0 && std::abs(device->getParam("753").real - 20) < 1e-4,
          "an old state restores the disabled follower and original release");
    audioTriggerPd();
    automationKeepsTheClock();
    savedSplitFrequency();
    adi::test::TempDirectory edited{"shaper_pd", "crossed_outputs"};
    writeCrossedCanvas(edited.path() / "ADI-Shaper.pd");
    const auto editedDir = edited.path().string();
    panDelayBlend(ADI_SHAPER_PATCH_DIR, false);
    panDelayBlend(editedDir, true);
    stereoSweepPd(ADI_SHAPER_PATCH_DIR, false);
    stereoSweepPd(editedDir, true);
    quantizerBiasPd(ADI_SHAPER_PATCH_DIR, false);
    quantizerBiasPd(editedDir, true);
    holdFollowerRecall();
    {
        auto recalled = open();
        set(*recalled, 905, 1);  // Earlier session with a full-stage middle-band detector.
        auto olderSession = adi::Payload::from_cbor(recalled->saveState("state"));
        std::array<double, 6> physical{};
        for (int n = 0; n < 6; ++n) {
            const auto id = std::to_string(907 + n);
            const auto* descriptor = recalled->paramAt(754 + n);
            check(descriptor && descriptor->id == id && recalled->getParam(id).real == (n < 3 ? 0 : 100),
                  "each new detector edge defaults to open");
            for (const auto* group : {"params", "real"}) olderSession.at(group).erase(id);
            set(*recalled, 907 + n, .12 + .123 * n);
            physical[static_cast<std::size_t>(n)] = recalled->getParam(id).real;
        }
        auto restored = open();
        check(restored->loadState("state", recalled->saveState("state")), "restore six detector cutoffs");
        for (int n = 0; n < 6; ++n)
            check(std::abs(restored->getParam(std::to_string(907 + n)).real - physical[static_cast<std::size_t>(n)]) < 1e-5,
                  "the physical cutoff survives recall");
        check(restored->loadState("state", adi::Payload::to_cbor(olderSession)) && restored->getParam("905").real == 1,
              "the earlier full-stage detector still reads the same source");
        for (int n = 0; n < 6; ++n)
            check(restored->getParam(std::to_string(907 + n)).real == (n < 3 ? 0 : 100),
                  "reopening an older full-input session removes both filter edges");
    }
    modulationPd(editedDir, false, false, false, true, false, false, false, true);
    for (const unsigned mask : {1U, 2U, 4U})
        modulationPd(editedDir, false, false, false, true, false, false, false, true, mask);
    modulationPd(editedDir, false, false, false, true, false, false, false, true, 0, true);
    modulationPd(editedDir, false, false, false, true, false, false, false, true, 7, true);
    driveShapeState(*open());
    driveShapeCanvas(ADI_SHAPER_PATCH_DIR, false);
    driveShapeCanvas(editedDir, true);
    cachedPdTransport(ADI_SHAPER_PATCH_DIR, true);
    cachedPdTransport(editedDir, false);
    automatedCrossoverPd(editedDir);
    modulationPd(editedDir, false, false);
    modulationPd(editedDir, false, false, false, false, true);
    modulationPd(editedDir, false, false, false, false, false, true);
    modulationPd(editedDir, false, false, false, false, false, false, true);
    panTransferPath(ADI_SHAPER_PATCH_DIR, false);
    panTransferPath(editedDir, true);
    modulationPd(editedDir, true, false);
    modulationPd(editedDir, false, true);
    modulationPd(editedDir, true, true);
    modulationPd(editedDir, false, false, true, false);
    modulationPd(editedDir, false, false, false, true);
    modulationPd(editedDir, false, false, true, true);
    timePd();
    samePositionTimeLocate();
    combinedStageState(editedDir);
    filterPeakPd(ADI_SHAPER_PATCH_DIR, false);
    filterPeakPd(editedDir, true);
    stereoFilterPd(ADI_SHAPER_PATCH_DIR, false);
    stereoFilterPd(editedDir, true);
    steepFilterPd(ADI_SHAPER_PATCH_DIR, false);
    steepFilterPd(editedDir, true);
    combinedFilterPd(ADI_SHAPER_PATCH_DIR, false);
    combinedFilterPd(editedDir, true);
    earlierFilterProject(editedDir);
    combinedControlRecall(editedDir);
    std::puts("ADI-Shaper built-in state, transport and editable modulation parity passed.");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
