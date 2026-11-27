// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/engine/launch_audio_runtime.hpp"
#include "adi/engine/launch_gain_edits.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
int allocations = 0;
}
void adi::test::countReplacedNew() noexcept { if (adi::audio::onAudioThread) ++allocations; }

namespace {
using namespace adi;
using namespace adi::engine;
constexpr std::int32_t maximumFrames = 257;
constexpr std::int64_t readySample = 211, sixteenthSample = 6000;
constexpr std::int64_t clipId = 100;
constexpr std::uint64_t incarnation = 7;

void require(bool good, const char* message) {
    if (!good)
        throw std::runtime_error(message);
}

struct Fixture {
    test::TempDirectory temp{"launch_gain_runtime", "prepared_gain"};
    std::unique_ptr<Store> store;
    Session session;
    std::int64_t sample = 0;
    std::uint64_t run = 0;
    std::size_t transitions = 0;

    Fixture() {
        StoreError error{};
        store = Store::createEmpty(temp.path() / "gain.adi", error);
        require(static_cast<bool>(store), "create Session gain-edit project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES "
                         "(1,'audio','Audio',0),(2,'master','Main',1)");
        LaunchSlot slot;
        slot.track = 1;
        slot.scene = 10;
        slot.clip = clipId;
        slot.endTick = textproj::kPPQ;
        SessionSpec spec;
        spec.sampleRate = 48000;
        spec.maxFrames = maximumFrames;
        spec.performanceState = true;
        spec.synchronousClipReads = true;
        spec.launchAudio = std::make_shared<const LaunchAudioProject>(incarnation, 120,
            std::vector<std::int64_t>{1}, std::vector{slot}, std::vector<std::int64_t>{10},
            std::vector{LaunchAudioClip::fromSamples(clipId, 48000, 1, std::vector<float>(10000, 0.25f))},
            LaunchQuantization::None);
        session.graph().setFadeFrames(0);
        require(session.load(*store, {}, spec), "build Session gain-edit graph");
        session.transport().play();
        const std::array start{LaunchRequest{LaunchRequestKind::SlotPress, 1, 10, clipId, incarnation}};
        require(session.postLaunchGesture(start), "run the original prepared clip");
    }

    void block(std::int32_t size, std::int64_t gainBoundary) {
        std::array<float, maximumFrames> left{}, right{};
        float* outputs[]{left.data(), right.data()};
        AudioIo io;
        io.out = outputs;
        io.numOut = 2;
        io.frames = size;
        io.streamTimeSamples = sample;
        auto block = session.beginRender();
        block.observeLaunchTransitions(this, [](void* context, const LaunchTransition& transition,
                                                std::int64_t) noexcept {
            auto& fixture = *static_cast<Fixture*>(context);
            if (transition.newClip == clipId) {
                fixture.run = transition.newRun;
                ++fixture.transitions;
            }
        });
        require(block.process(io), "render Session clip gain edit through the graph");
        for (std::int32_t i = 0; i < size; ++i) {
            const auto expected = sample + i < gainBoundary ? 0.25f : 0.125f;
            if (left[static_cast<std::size_t>(i)] != expected || right[static_cast<std::size_t>(i)] != expected) {
                std::fprintf(stderr, "frame %lld rendered [%g,%g], expected %g\n", static_cast<long long>(sample + i),
                    static_cast<double>(left[static_cast<std::size_t>(i)]),
                    static_cast<double>(right[static_cast<std::size_t>(i)]), static_cast<double>(expected));
                throw std::runtime_error("gain update missed its expected sample");
            }
        }
        sample += size;
    }
};

void verify(ClipUpdateRate rate, std::int64_t boundary) {
    Fixture fixture;
    fixture.block(static_cast<std::int32_t>(readySample), boundary);
    require(fixture.run != 0, "the owner can address the actual performed clip run");
    auto edit = LaunchClipGainEdit{1, clipId, incarnation, fixture.run, 0.5f, rate};
    require(fixture.session.postLaunchClipGainEdit(edit), "queue the prepared gain revision on the live run");
    while (fixture.sample <= boundary + maximumFrames) fixture.block(maximumFrames, boundary);
    require(fixture.session.setDelayCompensation(false), "a compatible rebuilt graph retains active gain state");
    fixture.block(maximumFrames, boundary);
    edit.run += 1;
    edit.multiplier = 0.25f;
    require(fixture.session.postLaunchClipGainEdit(edit), "pass the stale run stamp through the bounded mailbox");
    fixture.block(maximumFrames, boundary);
    require(fixture.transitions == 1, "gain delivery preserves run identity with no synthetic retrigger");
}
} // namespace

int main() {
    try {
        verify(ClipUpdateRate::Sixteenth, sixteenthSample);
        verify(ClipUpdateRate::None, readySample);
        require(allocations == 0, "gain scheduling and playback allocate nothing inside callbacks");
        std::printf("PASS -- running gain switches at frames %lld (1/16) and %lld (None), 0 sample error\n",
            static_cast<long long>(sixteenthSample), static_cast<long long>(readySample));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
