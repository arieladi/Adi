// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/builtin_catalog.hpp"
#include "adi/dsp/adi_shaper.hpp"
#include "adi/textproj.hpp"
#include "juce/builtin_host.hpp"
#include "juce/pd_engine.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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

void modulationPd(bool filterOn, bool liquidOn, bool driveOn = false, bool crushOn = false,
                  adi::dsp::AdiShaper::Stage source = adi::dsp::AdiShaper::Stage::Volume) {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    using Stage = Shaper::Stage;
    constexpr auto mid = Shaper::Band::Mid;
    constexpr int frames = 4096;
    Engine native(ADI_SHAPER_PATCH_DIR, "ADi-SHAPER.pd", 2, 2);
    Engine pd(ADI_SHAPER_PATCH_DIR, "ADi-SHAPER.pd", 2, 2,
              Engine::Execution::PureData, Engine::BlockMode::Buffered);
    const auto decl = adi::device::parsePdDeclarations(adi::builtinCanvas(uid));
    std::string error;
    std::array<adi::device::PdLatencyReceiver, 2> latency;
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
        send(Shaper::FilterOn, filterOn ? 1.0f : 0.0f);
        send(Shaper::LiquidOn, liquidOn ? 1.0f : 0.0f);
        send(Shaper::DriveOn, driveOn ? 1.0f : 0.0f);
        send(Shaper::CrushOn, crushOn ? 1.0f : 0.0f);
        // Compare the unsolicited latency report to an explicit query.
        const int latencySamples = driveOn ? 16 : 0;
        check(latency[i].latencySamples() == latencySamples, "both paths publish Drive enable latency");
        path.requestLatencyReport();
        check(latency[i].latencySamples() == latencySamples, "editable query reports the current core latency");
        send(Shaper::LiquidMidPolarity, 1.0f);
        for (auto stage : {Stage::Filter, Stage::Liquid, Stage::Crush}) {
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
        if (source >= Stage::Pitch) {
            const auto start = source == Stage::Pitch ? Shaper::PitchOn : source == Stage::Noise ? Shaper::NoiseOn : Shaper::ReverbOn;
            send(start, 1);
            send(static_cast<Shaper::Param>(start + 1), 3);
            for (auto band : {Shaper::Band::Low, mid, Shaper::Band::High}) {
                send(Shaper::modParam(source, band, Shaper::ModClock), 1);
                send(Shaper::modParam(source, band, Shaper::ModRate), 31.f + 13.f * static_cast<float>(band));
                send(Shaper::modParam(source, band, Shaper::ModMix), 75.f);
                send(Shaper::modParam(source, band, Shaper::PrimaryCurve), 1);
                send(Shaper::modParam(source, band, Shaper::SecondaryCurve), 1);
                for (int j = 0; j < 8; ++j) {
                    const float value = static_cast<float>(j);
                    send(Shaper::modParam(source, band, static_cast<Shaper::ModControl>(Shaper::Primary1 + j)),
                         source == Stage::Pitch ? -12.f + 3.f * value : 42.f + 6.f * value);
                    send(Shaper::modParam(source, band, static_cast<Shaper::ModControl>(Shaper::Secondary1 + j)),
                         source == Stage::Noise ? 8.f - 2.f * value : 82.f - 6.f * value);
                }
                if (source == Stage::Noise) {
                    send(Shaper::modParam(source, band, Shaper::Type), band == mid ? 1.f : 2.f);
                    send(Shaper::noiseParam(band, Shaper::NoiseMono), 0);
                    send(Shaper::noiseParam(band, Shaper::NoiseOnly), 1);
                    send(Shaper::noiseParam(band, Shaper::NoiseTrim), 85);
                    send(Shaper::noiseParam(band, Shaper::NoiseHighPass), 12);
                    send(Shaper::noiseParam(band, Shaper::NoiseLowPass), 92);
                } else if (source == Stage::Reverb) {
                    send(Shaper::reverbParam(band, Shaper::RoomSize), 38);
                    send(Shaper::reverbParam(band, Shaper::RoomDecay), 83);
                    send(Shaper::reverbParam(band, Shaper::RoomGate), 1);
                    send(Shaper::reverbParam(band, Shaper::RoomWidth), 73);
                    send(Shaper::reverbParam(band, Shaper::RoomPredelay), 4);
                    send(Shaper::reverbParam(band, Shaper::RoomClearTails), 1);
                }
            }
        }
    }
    check(native.isNative() && !pd.isNative() && native.adapterLatencySamples() == 0
          && pd.adapterLatencySamples() == 64, "separate native and buffered editable paths");
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
        f += n;
    }
    double errorPeak = 0, effectPeak = 0;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 64; i < frames; ++i) {
            errorPeak = std::max(errorPeak, static_cast<double>(std::abs(editable[ch][i] - direct[ch][i - 64])));
            const auto index = i - 64;
            const float alignedDry = !driveOn ? input[ch][index] : index >= 16 ? input[ch][index - 16] : 0.0f;
            effectPeak = std::max(effectPeak, static_cast<double>(std::abs(alignedDry - direct[ch][index])));
            check(std::isfinite(editable[ch][i]) && std::isfinite(direct[ch][i - 64]), "finite modulation output");
        }
    }
    check(effectPeak > 0.02 && errorPeak < 1.0e-6, "curved stereo modulation agrees across irregular native and Pd blocks");
    if (driveOn) for (std::size_t i = 0; i < paths.size(); ++i) {
        check(paths[i]->sendParameter(static_cast<int>(Shaper::DriveOn) + 1, 0), "disable active Drive");
        check(latency[i].latencySamples() == 0, "disabling Drive publishes zero latency");
    }
}

void timePd() {
    using Engine = adi::device::LibPdEngine;
    using Shaper = adi::dsp::AdiShaper;
    constexpr auto mid = Shaper::Band::Mid;
    constexpr int samples = 768, seek = 128, marker = 144;
    Engine native(ADI_SHAPER_PATCH_DIR, "ADi-SHAPER.pd", 2, 2);
    Engine pd(ADI_SHAPER_PATCH_DIR, "ADi-SHAPER.pd", 2, 2, Engine::Execution::PureData,
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
    check(info && info->name == "ADi-SHAPER" && device->paramCount() == count, "device registration");
    for (int id = 1; id <= count; ++id)
        check(device->paramAt(id - 1) && device->paramAt(id - 1)->id == std::to_string(id), "contiguous stable parameter IDs");
    check(device->wantsDeviceTransport() && device->latencySamples() == 0, "position and latency");
    for (int id = 23; id <= 26; ++id) set(*device, id, 0);  // half-bar gate
    for (int id = 65; id <= 72; ++id) set(*device, id, 0);  // mono
    // Restore an actual earlier-format parameter map, with no Filter or Liquid keys.
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
    // Reusing a live device must sound the same as a fresh instance when
    // the incoming project predates the last six modules.
    for (const int id : {287, 341, 416, 464, 536, 626}) set(*device, id, 1);
    set(*device, 715, 1);
    device->process(io);
    check(device->latencySamples() == 16, "live newer state enables Drive delay");
    check(device->loadState("state", adi::Payload::to_cbor(modState)), "load older project over active new modules");
    device->process(io);
    for (const int id : {287, 341, 416, 464, 536, 626})
        check(device->getParam(std::to_string(id)).real == 0, "missing stage resets on reused device");
    check(device->getParam("715").real == 0 && device->latencySamples() == 0,
          "missing option and previous latency reset on load");
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
    auto eightStageState = adi::Payload::from_cbor(device->saveState("state"));
    for (int id = 464; id <= count; ++id) eightStageState.at("params").erase(std::to_string(id));
    device = open();
    check(device->loadState("state", adi::Payload::to_cbor(eightStageState)) && device->getParam("416").real == 1
          && std::abs(device->getParam("463").real - 31) < 1e-4, "Time enable and last curve point persist");
    check(device->getParam("464").real == 0 && device->getParam("536").real == 0
          && device->getParam("626").real == 0 && device->getParam("632").real == 25,
          "eight-stage sessions restore with the three new stages off and the measured Reverb mix default");
    set(*device, 464, 1);
    set(*device, 536, 1);
    set(*device, 626, 1);
    set(*device, 715, 1);
    const auto sourceState = device->saveState("state");
    device = open();
    check(device->loadState("state", sourceState) && device->getParam("464").real == 1
          && device->getParam("536").real == 1 && device->getParam("626").real == 1 && device->getParam("715").real == 1,
          "three additional stage switches and the final high-band option recall");
    set(*device, 287, 0);
    device->process(io);
    check(device->latencySamples() == 0, "host removes Drive PDC when disabled");
    modulationPd(true, false);
    modulationPd(false, true);
    modulationPd(true, true);
    modulationPd(false, false, true, false);
    modulationPd(false, false, false, true);
    modulationPd(false, false, true, true);
    using Stage = adi::dsp::AdiShaper::Stage;
    for (auto source : {Stage::Pitch, Stage::Noise, Stage::Reverb})
        modulationPd(false, false, false, false, source);
    timePd();
    std::puts("ADi-SHAPER built-in state, transport and editable modulation parity passed.");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
