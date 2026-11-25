// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/param_ops.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "juce/builtin_host.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace adi;
using namespace adi::engine;

constexpr int sampleRate = 48000;
constexpr int sourceLatency = 379;
constexpr int markerFrame = 65536;
constexpr std::array levels{0.5f, -0.25f};

// A stereo step with a one-frame gap. Both emerge 379 frames late. The gap
// falls in the open part of the gate so the physical delay is audible.
class DelayedSignal final : public Node {
public:
    std::int32_t latencySamples() const noexcept override { return sourceLatency; }

    void process(const NodeIo& io) noexcept override {
        const int silent = std::clamp(sourceLatency - rendered_, 0, io.frames);
        const int gap = sourceLatency + markerFrame - rendered_;
        for (std::size_t c = 0; c < levels.size(); ++c) {
            float* output = io.out[c] + io.blockOffset;
            std::fill_n(output, silent, 0.0f);
            std::fill_n(output + silent, io.frames - silent, levels[c]);
            if (gap >= 0 && gap < io.frames) output[gap] = 0.0f;
        }
        rendered_ += io.frames;
    }

private:
    int rendered_ = 0;
};

void render(const Store& store, int blockSize, bool compensate) {
    DelayedSignal source;
    Session session;
    session.graph().setFadeFrames(0);
    session.setSourcesFor([&](std::int64_t track) {
        return track == 1 ? std::vector<Node*>{&source} : std::vector<Node*>{};
    });
    SessionSpec spec;
    spec.sampleRate = sampleRate;
    spec.maxFrames = blockSize;
    spec.delayCompensation = compensate;
    if (!session.load(store, device::withBuiltinDevices(), spec))
        throw std::runtime_error(session.error());
    auto* shaper = session.instanceFor(1);
    if (!shaper || !shaper->loaded() || session.graph().currentGraph()->latencySamples() != sourceLatency)
        throw std::runtime_error("expected the stock ADI-Shaper after the delayed source");

    using S = dsp::AdiShaper;
    for (auto point : {S::Point1, S::Point2, S::Point3, S::Point4}) {
        const int id = 1 + S::bandParam(S::Stage::Volume, S::Band::Mid, point);
        if (!shaper->setParam(std::to_string(id), device::ParamValue::fromNormalized(0)))
            throw std::runtime_error("could not program the first two quarters of the gate");
    }

    // The first quarter is 16,384 frames; each following quarter is 32,768.
    // Work in samples independently of the Session's tempo/transport code.
    // A segment starting between ticks can round the first sample exactly on
    // an edge to the preceding plateau when converting seconds back to beats.
    constexpr int start = 4096;
    const int offset = compensate ? sourceLatency : 0;
    const int opens = 16384 + 32768 + offset;
    const int closes = 16384 + 3 * 32768 + offset;
    const int end = closes + 1500;
    session.transport().locate(start);
    session.transport().play();
    std::array<std::vector<float>, 2> buffers;
    for (auto& buffer : buffers) buffer.resize(static_cast<std::size_t>(blockSize));
    float* planes[]{buffers[0].data(), buffers[1].data()};
    AudioIo io;
    io.out = planes;
    io.numOut = 2;
    for (int at = start; at < end; at += io.frames) {
        io.frames = std::min(blockSize, end - at);
        session.process(io);
        for (int i = 0; i < io.frames; ++i) {
            const int sample = at + i;
            const bool open = sample >= opens && sample < closes && sample != start + markerFrame + sourceLatency;
            for (std::size_t c = 0; c < levels.size(); ++c) {
                const float expected = open ? levels[c] : 0.0f;
                const auto actual = planes[c][i];
                const bool edge = sample == opens || sample == closes;
                const bool onPrecedingPlateau = edge && std::abs(actual - (levels[c] - expected)) <= 1.0e-6f;
                if (!(std::abs(actual - expected) <= 1.0e-6f || onPrecedingPlateau)) {
                    std::printf("block %d, PDC %d, sample %d, channel %zu: got %.8f, want %.8f\n",
                                blockSize, static_cast<int>(compensate), sample, c, actual, expected);
                    throw std::runtime_error("Volume gate did not follow its receiving beat");
                }
            }
        }
    }
}
using StereoCapture = std::array<std::array<float, 8192>, 2>;

StereoCapture renderProjectState(Store& store, int blockSize, bool record, bool reverseOrder) {
    DelayedSignal input;
    Session session;
    session.graph().setFadeFrames(0);
    session.setSourcesFor([&](std::int64_t track) {
        return track == 1 ? std::vector<Node*>{&input} : std::vector<Node*>{};
    });
    SessionSpec spec;
    spec.sampleRate = sampleRate;
    spec.maxFrames = blockSize;
    if (!session.load(store, device::withBuiltinDevices(), spec))
        throw std::runtime_error(session.error());
    auto* instance = session.instanceFor(1);
    if (!instance || !instance->loaded() || session.stats().statesLoaded != (record ? 0 : 1))
        throw std::runtime_error("shaper project state did not reach the registered device");

    // Persist independent values for Time height, Noise level and pitch, and room send and return.
    // These are the stable project IDs on either side of the space reserved for Pitch.
    struct Setting { int id; double value; };
    constexpr std::array settings{
        Setting{2, 0.}, Setting{416, 1.}, Setting{442, .625}, Setting{443, .875},
        Setting{536, 1.}, Setting{570, .5}, Setting{577, .25}, Setting{585, .375}, Setting{594, 1.},
        Setting{626, 1.}, Setting{660, .75}, Setting{666, .5}, Setting{674, .875},
        Setting{723, 1.}, Setting{725, 0.}, Setting{726, 64. / 127.}};
    if (record) {
        ParamOps edits;
        if (edits.attachSession(session) != 1) throw std::runtime_error("cannot attach shaper project capture");
        for (const auto& setting : settings)
            if (!instance->setParam(std::to_string(setting.id), device::ParamValue::fromNormalized(setting.value)))
                throw std::runtime_error("cannot set shaper project parameter " + std::to_string(setting.id));
        std::vector<OpRequest> requests;
        if (edits.snapshot(1, 1000, requests) == 0) throw std::runtime_error("empty shaper project snapshot");
        const auto result = OpJournal(store).commit(requests);
        if (!result.ok) throw std::runtime_error(result.error);
    }
    for (const auto& setting : settings)
        if (std::abs(instance->getParam(std::to_string(setting.id)).normalized - setting.value) > 1.e-6)
            throw std::runtime_error("shaper parameter changed on project recall " + std::to_string(setting.id));
    if (reverseOrder) {
        if (!instance->setParam("723", device::ParamValue::fromNormalized(0.)) ||
            !instance->setParam("725", device::ParamValue::fromNormalized(1.)))
            throw std::runtime_error("cannot reverse Time and Noise in the recalled shaper");
    }
    StereoCapture out{};
    session.transport().play();
    for (int frame = 0; frame < static_cast<int>(out[0].size()); frame += blockSize) {
        float* channels[]{out[0].data() + frame, out[1].data() + frame};
        AudioIo io;
        io.frames = blockSize;
        io.numOut = 2;
        io.out = channels;
        session.process(io);
    }
    return out;
}

void reopenedStages(std::unique_ptr<Store>& store, const std::filesystem::path& file) {
    const auto before = renderProjectState(*store, 256, true, false);
    if (!store->close()) throw std::runtime_error("could not close the staged shaper project");
    store.reset();
    StoreError error{};
    auto reopened = Store::open(file, error);
    if (!reopened) throw std::runtime_error(toString(error));
    if (renderProjectState(*reopened, 256, false, false) != before)
        throw std::runtime_error("reopened Time, Noise and room differ from the saved render");
    const auto large = renderProjectState(*reopened, 2048, false, false);
    const auto reversed = renderProjectState(*reopened, 2048, false, true);
    double orderEnergy = 0.;
    for (std::size_t ch = 0; ch < before.size(); ++ch) {
        for (std::size_t frame = 0; frame < before[ch].size(); ++frame) {
            if (!(std::abs(before[ch][frame] - large[ch][frame]) < 1.e-5))
                throw std::runtime_error("reopened shaper changed with callback size");
            const double delta = static_cast<double>(large[ch][frame]) - reversed[ch][frame];
            orderEnergy += delta * delta;
        }
    }
    if (!(orderEnergy > 1.e-5 * before[0].size()))
        throw std::runtime_error("restoring the reverse stage order made no audible difference");
}

} // namespace

int main() {
    try {
        adi::test::TempDirectory temp{"adi_shaper_session", "beat_gate"};
        adi::StoreError error{};
        auto store = adi::Store::create(temp.path() / "gate.adi", error);
        if (!store) throw std::runtime_error(adi::toString(error));
        auto& db = store->db();
        db.exec("UPDATE project SET sample_rate=48000;"
                "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Signal'),(9,'master','Main');"
                "INSERT INTO mixer_strip(track_id) VALUES(1),(9);"
                "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES"
                "(1,'internal','org.adidaw.builtin.adi-shaper','ADI-Shaper','effect');"
                "INSERT INTO device_chains(id,track_id) VALUES(1,1);"
                "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'ADI-Shaper');"
                "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,175.78125,0);");
        {
            SQLite::Statement change(db, "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(?,87.890625,0)");
            change.bind(1, adi::textproj::kPPQ);
            change.exec();
        }
        for (const int size : {256, 2048}) {
            for (const bool compensate : {true, false}) render(*store, size, compensate);
        }
        reopenedStages(store, temp.path() / "gate.adi");
        std::puts("ADI-Shaper Session beat gate passed at both block sizes with compensation on and off.");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL %s\n", e.what());
        return 1;
    }
}
