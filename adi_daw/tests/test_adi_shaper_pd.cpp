// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/builtin_catalog.hpp"
#include "adi/dsp/adi_shaper.hpp"
#include "adi/textproj.hpp"
#include "juce/builtin_host.hpp"
#include "juce/pd_engine.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
constexpr auto uid = "org.adidaw.builtin.adi-shaper";
using adi::device::DeviceInstance;
using adi::device::ParamValue;
void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}
auto open() {
    adi::rows::PluginRef reference;
    reference.format = "internal";
    reference.uid = uid;
    adi::engine::DeviceRequest request;
    request.ref = &reference;
    std::string error;
    auto device = adi::device::makeBuiltinDevice(request, error);
    if (!device) throw std::runtime_error(error);
    device->prepare(48000, 64);
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

void modulationPd(const std::string& editedDir, bool filterOn, bool sweepOn, bool driveOn = false,
                  bool crushOn = false) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    using Stage = Shaper::Stage;
    constexpr auto mid = Shaper::Band::Mid;
    constexpr int frames = 4096;
    const bool onlyVolume = !filterOn && !sweepOn && !driveOn && !crushOn;
    std::array<adi::device::PdLatencyReceiver, 2> latency;
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
        };
        if (onlyVolume) {
            for (int j = 0; j < 8; ++j)
                send(Shaper::bandParam(Stage::Volume, mid, static_cast<Shaper::BandControl>(Shaper::Point1 + j)), 50);
        }
        send(Shaper::FilterOn, filterOn ? 1.0f : 0.0f);
        send(Shaper::SweepOn, sweepOn ? 1.0f : 0.0f);
        send(Shaper::DriveOn, driveOn ? 1.0f : 0.0f);
        send(Shaper::CrushOn, crushOn ? 1.0f : 0.0f);
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
    }
    check(native.isNative() && !pd.isNative() && native.adapterLatencySamples() == 0
          && pd.adapterLatencySamples() == 64, "automatic dispatch selects native and buffered edited paths");
    std::array<std::array<float, frames>, 2> input{}, direct{}, editable{};
    for (int f = 0; f < frames; ++f) {
        input[0][static_cast<std::size_t>(f)] = static_cast<float>(.35 * std::sin(.071 * f));
        input[1][static_cast<std::size_t>(f)] = static_cast<float>(.17 * std::cos(.133 * f));
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
} // namespace

int main() try {
    auto device = open();
    const auto* info = adi::findBuiltin(uid);
    constexpr int count = adi::dsp::AdiShaper::Count;
    check(info && info->name == "ADI-Shaper" && device->paramCount() == count, "device registration");
    for (int id = 1; id <= count; ++id)
        check(device->paramAt(id - 1) && device->paramAt(id - 1)->id == std::to_string(id), "contiguous stable parameter IDs");
    check(device->wantsDeviceTransport() && device->latencySamples() == 0, "position and latency");
    for (int id = 23; id <= 26; ++id) set(*device, id, 0);  // half-bar gate
    for (int id = 65; id <= 72; ++id) set(*device, id, 0);  // mono
    // Restore an actual earlier-format parameter map, with no Filter or Sweep keys.
    auto state = adi::Payload::from_cbor(device->saveState("state"));
    for (int id = 134; id <= count; ++id) state.at("params").erase(std::to_string(id));
    device = open();
    check(device->loadState("state", adi::Payload::to_cbor(state)), "restore earlier curve and width");
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
    auto modState = adi::Payload::from_cbor(device->saveState("state"));
    for (int id = 287; id <= count; ++id) modState.at("params").erase(std::to_string(id));
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
    auto sevenStageState = adi::Payload::from_cbor(device->saveState("state"));
    for (int id = 416; id <= count; ++id) sevenStageState.at("params").erase(std::to_string(id));
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
    adi::test::TempDirectory edited{"shaper_pd", "crossed_outputs"};
    writeCrossedCanvas(edited.path() / "ADI-Shaper.pd");
    const auto editedDir = edited.path().string();
    cachedPdTransport(ADI_SHAPER_PATCH_DIR, true);
    cachedPdTransport(editedDir, false);
    modulationPd(editedDir, false, false);
    modulationPd(editedDir, true, false);
    modulationPd(editedDir, false, true);
    modulationPd(editedDir, true, true);
    modulationPd(editedDir, false, false, true, false);
    modulationPd(editedDir, false, false, false, true);
    modulationPd(editedDir, false, false, true, true);
    timePd();
    std::puts("ADI-Shaper built-in state, transport and editable modulation parity passed.");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
