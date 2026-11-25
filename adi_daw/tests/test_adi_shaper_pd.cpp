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

void panDelayBlend(const std::string& directory, bool crossed) {
    using Engine = adi::device::LibPdEngine;
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
    struct Reading { float blend, bandMix, masterMix, leftNow, leftAt21, rightNow; };
    constexpr std::array readings{
        Reading{0,   100, 100, .550475776f, 0,           1.303077579f},
        Reading{25,  100, 100, .068809472f, .481666297f, 1.303077579f},
        Reading{50,  100, 100, 0,           .550475776f, 1.303077579f},
        Reading{75,  100, 100, 0,           .775237918f, 1.151538730f},
        Reading{100, 100, 100, 0,          1,            1},
        Reading{100,   0, 100, 1,          0,            1},
        Reading{100, 100,   0, 1,          0,            1}};
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
              && path.sendParameter(1, row.masterMix), "deliver blend and dry/wet controls");
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
}

void roundedDriveCanvas(const std::string& directory, bool crossed) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    adi::device::PdLatencyReceiver latency;
    Engine path(directory, "ADI-Shaper.pd", 2, 2, Engine::Execution::Automatic, Engine::BlockMode::WholeBlocks);
    path.addSearchPath(ADI_PD_PATCH_DIR);
    std::string error;
    check(path.open(latency, error) && path.isNative() == !crossed, "open the selected Drive playback path");
    path.prepare(48000, 64);
    path.bindParameters(adi::device::parsePdDeclarations(adi::builtinCanvas(uid)));
    const auto send = [&](Shaper::Param id, float value) {
        check(path.sendParameter(static_cast<int>(id) + 1, value), "send Drive setup");
    };
    send(Shaper::DriveOn, 1);
    send(Shaper::driveParam(Shaper::Band::Mid, Shaper::DriveStyle), 1);
    send(Shaper::DriveMidRounded, 1);
    for (int i = 0; i < 8; ++i)
        send(Shaper::driveParam(Shaper::Band::Mid, static_cast<Shaper::DriveControl>(Shaper::Drive1 + i)), 50);
    std::array<float, 64> left{}, right{}, outL{}, outR{};
    const float* in[]{left.data(), right.data()};
    float* out[]{outL.data(), outR.data()};
    adi::engine::NodeIo io;
    io.in = in;
    io.out = out;
    io.channels = 2;
    io.frames = 64;
    for (int block = 0; block < 12; ++block) {
        left.fill(0);
        right.fill(0);
        if (block == 6) left[36] = .97f;
        if (block == 9) right[56] = -.38f;
        path.process(io);
        // The two original impulse peaks arrive 16 frames after each marker.
        if (block == 6)
            check(std::abs((crossed ? outR : outL)[52] - .339567f) < .001f, "positive rounded Drive peak");
        if (block == 10)
            check(std::abs((crossed ? outL : outR)[8] + .310638f) < .001f, "negative rounded Drive peak");
    }
}

void roundedDriveState(DeviceInstance& device) {
    for (int i = 0; i < 3; ++i) {
        const auto id = std::to_string(928 + i);
        const auto* desc = device.paramAt(418 + i);
        check(desc && desc->id == id && device.getParam(id).real == 0., "rounded switches append off");
        set(device, 928 + i, i == 1 ? 0. : 1.);
    }
    auto next = open();
    const auto state = device.saveState("state");
    check(next->loadState("state", state), "recall independent clipping choices");
    auto sparse = adi::Payload::from_cbor(state);
    for (int i = 0; i < 3; ++i) {
        const auto id = std::to_string(928 + i);
        check(next->getParam(id).real == (i == 1 ? 0. : 1.), "restore each band's rounding switch");
        for (const auto* group : {"params", "real"}) sparse.at(group).erase(id);
    }
    check(next->loadState("state", adi::Payload::to_cbor(sparse)), "restore a state preceding the rounding switches");
    for (int i = 928; i <= 930; ++i)
        check(next->getParam(std::to_string(i)).real == 0., "sparse recall restores the earlier clipping law");
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
        if (filterOn && sweepOn) {
            // Feed both processors the first-order middle band on both signal paths.
            send(Shaper::VolumeBands, 3);
            send(static_cast<Shaper::Param>(Shaper::VolumeBands + 1), 82.40689f);
            send(static_cast<Shaper::Param>(Shaper::VolumeBands + 2), 1147.8298f);
            for (const auto band : {Shaper::Band::Low, Shaper::Band::High})
                for (int j = 0; j < 8; ++j)
                    send(Shaper::bandParam(Stage::Volume, band, static_cast<Shaper::BandControl>(Shaper::Point1 + j)), 0);
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

} // namespace

int main() try {
    auto device = open();
    const auto* info = adi::findBuiltin(uid);
    check(info && info->name == "ADI-Shaper" && device->paramCount() == 421, "device registration");
    for (int id = 1; id <= 415; ++id)
        check(device->paramAt(id - 1) && device->paramAt(id - 1)->id == std::to_string(id), "contiguous stable parameter IDs");
    panBlendState(*device);
    roundedDriveState(*device);
    device = open();
    check(device->wantsDeviceTransport() && device->latencySamples() == 0, "position and latency");
    for (int id = 23; id <= 26; ++id) set(*device, id, 0);  // half-bar gate
    for (int id = 65; id <= 72; ++id) set(*device, id, 0);  // mono
    // Restore an actual earlier-format parameter map, with no Filter or Sweep keys.
    auto state = adi::Payload::from_cbor(device->saveState("state"));
    for (int id = 134; id <= 415; ++id) state.at("params").erase(std::to_string(id));
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
    for (int id = 287; id <= 415; ++id) modState.at("params").erase(std::to_string(id));
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
    const auto sevenStageState = device->saveState("state");
    device = open();
    check(device->loadState("state", sevenStageState), "restore seven-stage project state");
    device->process(io);
    check(device->latencySamples() == 16 && device->getParam("341").real == 1, "active Drive latency restores with Crush");
    // Recall a sparse pre-Drive project into a running instance with Drive active.
    check(device->loadState("state", adi::Payload::to_cbor(modState)), "recall sparse project over an active instance");
    device->process(io);
    check(device->latencySamples() == 0 && device->getParam("287").real == 0
          && device->getParam("341").real == 0 && device->getParam("209").real == 1,
          "missing stages regain defaults and release PDC while saved stages stay enabled");
    // A value saved only in its unit (no position) is read, not reset to its default.
    auto realOnly = adi::Payload::from_cbor(sevenStageState);
    realOnly.at("params").erase("170");
    check(realOnly.at("real").contains("170") && std::abs(open()->getParam("170").real - 37) > 1,
          "fixture: Filter value saved in its unit differs from the default");
    device = open();
    check(device->loadState("state", adi::Payload::to_cbor(realOnly))
          && std::abs(device->getParam("170").real - 37) < 1e-4, "a value saved only in its unit is kept");
    automationKeepsTheClock();
    savedSplitFrequency();
    adi::test::TempDirectory edited{"shaper_pd", "crossed_outputs"};
    writeCrossedCanvas(edited.path() / "ADI-Shaper.pd");
    const auto editedDir = edited.path().string();
    panDelayBlend(ADI_SHAPER_PATCH_DIR, false);
    panDelayBlend(editedDir, true);
    roundedDriveCanvas(ADI_SHAPER_PATCH_DIR, false);
    roundedDriveCanvas(editedDir, true);
    cachedPdTransport(ADI_SHAPER_PATCH_DIR, true);
    cachedPdTransport(editedDir, false);
    modulationPd(editedDir, false, false);
    modulationPd(editedDir, true, false);
    modulationPd(editedDir, false, true);
    modulationPd(editedDir, true, true);
    modulationPd(editedDir, false, false, true, false);
    modulationPd(editedDir, false, false, false, true);
    modulationPd(editedDir, false, false, true, true);
    std::puts("ADI-Shaper built-in state, transport and editable modulation parity passed.");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
