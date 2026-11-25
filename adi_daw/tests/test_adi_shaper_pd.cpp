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
#include <utility>
#include <vector>

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
                  bool crushOn = false, bool noiseOn = false, bool reverbOn = false, bool followerOn = false) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    using Stage = Shaper::Stage;
    constexpr auto mid = Shaper::Band::Mid;
    constexpr int frames = 4096;
    const bool onlyVolume = !filterOn && !sweepOn && !driveOn && !crushOn && !noiseOn && !reverbOn && !followerOn;
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
        check(latency.latencySamples() == 0 && path.adapterLatencySamples() == 0, "whole-block stage test has zero adapter and reported delay");
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
    check(info && info->name == "ADI-Shaper" && device->paramCount() == 712, "device registration");
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
    for (const auto* group : {"params", "real"})
        for (int id = 134; id <= 463; ++id) state.at(group).erase(std::to_string(id));
    device = open();
    set(*device, 416, 1.0);
    set(*device, 423, 0.);
    check(device->loadState("state", adi::Payload::to_cbor(state)), "restore earlier curve and width");
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
    combinedStageState(editedDir);
    std::puts("ADI-Shaper built-in state, transport and editable modulation parity passed.");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
