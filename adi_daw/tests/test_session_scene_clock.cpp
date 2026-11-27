// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>

namespace { std::atomic<unsigned> allocations{0}; }
void adi::test::countReplacedNew() noexcept { if (adi::audio::onAudioThread) ++allocations; }

namespace {
using namespace adi;
using namespace adi::engine;
constexpr auto quarter = textproj::kPPQ;
constexpr std::int32_t blockSize = 257;

void require(bool success, const std::string& message) {
    if (!success) throw std::runtime_error(message);
}

struct ClockProbe final : Node {
    std::array<DeviceTransport, 8> changes{};
    std::size_t count = 0;

    [[nodiscard]] bool wantsDeviceTransport() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override {
        if (const auto* clock = io.deviceTransport; clock && clock->playing && count < changes.size()) {
            const auto& previous = changes[count == 0 ? 0 : count - 1];
            if (count == 0 || clock->bpm != previous.bpm || clock->numerator != previous.numerator ||
                clock->denominator != previous.denominator)
                changes[count++] = *clock;
        }
        for (std::int32_t channel = 0; channel < io.channels; ++channel)
            std::fill_n(io.out[channel] + io.blockOffset, io.frames, 0.0f);
    }
};

struct Render {
    Session& session;
    std::array<LaunchSceneTransition, 6> scenes{};
    std::size_t count = 0;
    std::int64_t stream = 0;
    std::array<float, blockSize> left{}, right{};

    void runThrough(std::int64_t sample) {
        float* outputs[]{left.data(), right.data()};
        while (session.transport().position() <= sample) {
            AudioIo io{outputs, nullptr, 2, 0, blockSize, stream};
            bool rendered;
            {
                auto block = session.beginRender();
                block.observeSceneLaunches(this, [](void* user, const LaunchSceneTransition& scene, std::int64_t) noexcept {
                    auto& self = *static_cast<Render*>(user);
                    if (self.count < self.scenes.size()) self.scenes[self.count++] = scene;
                });
                rendered = block.process(io);
            }
            require(rendered, "render exact scene clock through a pinned graph");
            stream += blockSize;
        }
    }
};

void exercise() {
    test::TempDirectory folder{"scene_clock", "project"};
    StoreError error{};
    auto store = Store::createEmpty(folder.path() / "clock.adi", error);
    require(static_cast<bool>(store), "create scene clock project");
    test::applySessionSchema(store->db());
    store->db().exec(
        "INSERT INTO project(id,name,sample_rate) VALUES(1,'Scene clock',48000);"
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Clock probe',0),(9,'master','Main',1);"
        "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
        "INSERT INTO time_signature_map(id,pos_ticks,numerator,denominator) VALUES(1,0,4,4);"
        "INSERT INTO scenes(id,ord,tempo_bpm,tempo_enabled,signature_numerator,signature_denominator,signature_enabled) "
        "VALUES(10,0,140,1,4,4,0),(20,1,125,0,7,8,1),(30,2,120,0,4,4,0);");
    ClockProbe device;
    Session session;
    session.setSourcesFor([&](std::int64_t track) {
        return track == 1 ? std::vector<Node*>{&device} : std::vector<Node*>{};
    });
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2, 48000, blockSize})) throw std::runtime_error(session.error());
    const auto incarnation = session.launchIncarnation();
    require(incarnation != 0, "publish persisted scene clock metadata");
    LaunchRequest unprepared;
    unprepared.kind = LaunchRequestKind::ScenePress;
    unprepared.scene = 10;
    unprepared.incarnation = incarnation;
    require(!session.postLaunchGesture(std::array{unprepared}), "ordinary clock-bearing scene waits for owner preparation");

    session.transport().play();
    Render render{session};
    render.runThrough(0);
    require(!session.prepareSceneLaunch(*store, 10, 0), "already elapsed boundary refuses the whole command");
    require(session.prepareSceneLaunch(*store, 10, 4 * quarter), "prepare exact tempo scene: " + session.error());
    require(!session.prepareSceneLaunch(*store, 20, 8 * quarter), "an unperformed committed scene cannot be replaced");
    render.runThrough(96'000);
    require(render.count == 1 && render.scenes[0].scene == 10 && render.scenes[0].sample == 96'000 &&
        render.scenes[0].preparedToken != 0 && render.scenes[0].offset != 0,
        "tempo scene performed at the exact interior sample");
    require(device.count == 2 && device.changes[0].bpm == 120 && device.changes[1].bpm == 140 &&
        device.changes[1].sample == 96'000 && device.changes[1].numerator == 4,
        "device uses the shared 120-to-140 BPM clock at the exact scene boundary");

    require(session.prepareSceneLaunch(*store, 20, 8 * quarter), "prepare independent 7/8 scene: " + session.error());
    render.runThrough(178'286);
    require(render.count == 2 && render.scenes[1].scene == 20 && render.scenes[1].sample == 178'286 &&
        device.count == 3 && device.changes[2].sample == 178'286 && device.changes[2].bpm == 140 &&
        device.changes[2].numerator == 7 && device.changes[2].denominator == 8,
        "meter changes independently while disabled scene tempo stays dormant");

    OpRequest next;
    next.opType = "session.launchScene";
    next.payload = {{"scene", 30}, {"incarnation", incarnation}};
    require(OpJournal(*store).commit(next).ok, "audit a normal scene after the prepared meter change");
    render.runThrough(250'286);
    require(render.count == 3 && render.scenes[2].scene == 30 && render.scenes[2].sample == 250'286 &&
        render.scenes[2].tick == 8 * quarter + 7 * quarter / 2,
        "next ordinary scene launches on the actual 7/8 Global grid");
    require(store->db().execAndGet("SELECT bpm FROM tempo_map WHERE pos_ticks=0").getDouble() == 120 &&
        store->db().execAndGet("SELECT count(*) FROM tempo_map").getInt() == 1 &&
        store->db().execAndGet("SELECT numerator FROM time_signature_map WHERE pos_ticks=0").getInt() == 4 &&
        store->db().execAndGet("SELECT count(*) FROM time_signature_map").getInt() == 1,
        "the effective clock leaves authored tempo and signature rows intact");
    require(allocations == 0 && audio::callbackFileIo == 0, "scene clock handling performs no allocation or file I/O on the driver");
    std::printf("PASS -- 120→140 at 96000, 7/8 at 178286, next scene at 250286, 0 callback allocations/file calls\n");
}
} // namespace

int main() {
    try { exercise(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
