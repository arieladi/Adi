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
#include <cmath>
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

float signal(std::int64_t frame) {
    return 0.125f + static_cast<float>(frame % 64) / 512.0f;
}

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

    explicit Fixture(bool varying = false) {
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
        std::vector<float> media(10000, 0.25f);
        if (varying)
            for (std::size_t frame = 0; frame < media.size(); ++frame)
                media[frame] = signal(static_cast<std::int64_t>(frame));
        spec.launchAudio = std::make_shared<const LaunchAudioProject>(incarnation, 120,
            std::vector<std::int64_t>{1}, std::vector{slot}, std::vector<std::int64_t>{10},
            std::vector{LaunchAudioClip::fromSamples(clipId, 48000, 1, std::move(media))},
            LaunchQuantization::None);
        session.graph().setFadeFrames(0);
        require(session.load(*store, {}, spec), "build Session gain-edit graph");
        session.transport().play();
        const std::array start{LaunchRequest{LaunchRequestKind::SlotPress, 1, 10, clipId, incarnation}};
        require(session.postLaunchGesture(start), "run the original prepared clip");
    }

    template<class Expected> void render(std::int32_t size, Expected expectedAt) {
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
            const auto expected = expectedAt(sample + i);
            if (left[static_cast<std::size_t>(i)] != expected || right[static_cast<std::size_t>(i)] != expected) {
                std::fprintf(stderr, "frame %lld rendered [%g,%g], expected %g\n", static_cast<long long>(sample + i),
                    static_cast<double>(left[static_cast<std::size_t>(i)]),
                    static_cast<double>(right[static_cast<std::size_t>(i)]), static_cast<double>(expected));
                throw std::runtime_error("clip update missed its expected sample");
            }
        }
        sample += size;
    }

    void block(std::int32_t size, std::int64_t gainBoundary) {
        render(size, [&](auto frame) { return frame < gainBoundary ? 0.25f : 0.125f; });
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

void verifyRegion(ClipUpdateRate rate, std::int64_t boundary) {
    Fixture fixture(true);
    fixture.render(static_cast<std::int32_t>(readySample), signal);
    require(fixture.run != 0, "observe the region edit's live clip run");
    const auto ticks = [](std::int64_t frame) {
        return std::llround(static_cast<double>(frame) * static_cast<double>(textproj::kPPQ) / 24'000.0);
    };
    const auto first = boundary - 2;
    const auto last = boundary + 4;
    LaunchClipRegionEdit edit{1, clipId, incarnation, fixture.run,
        {0, ticks(9000), LaunchClipLoopRegion{ticks(first), ticks(last)}}, rate};
    auto wrong = edit;
    wrong.region.end = ticks(10001);
    require(!fixture.session.postLaunchClipRegionEdit(wrong), "refuse a region outside the prepared audio buffer");
    require(fixture.session.postLaunchClipRegionEdit(edit), "queue a resident loop change on the live run");
    const auto expected = [&](auto frame) {
        return signal(frame < last ? frame : first + (frame - last) % (last - first));
    };
    while (fixture.sample <= last + maximumFrames) fixture.render(maximumFrames, expected);
    require(fixture.session.setDelayCompensation(false), "keep a revised region through graph replacement");
    fixture.render(maximumFrames, expected);
    require(fixture.transitions == 1, "region revision emits no launch edge or new run");
    std::array<LaunchTrackStatus, 2> rows{};
    LaunchStatusHeader status;
    require(fixture.session.readLaunchStatus(rows, status) && rows.front().run == fixture.run &&
        rows.front().loopStartTick == ticks(first) && rows.front().loopLengthTicks == ticks(last) - ticks(first),
        "published Track Status reports the applied loop on the original run");
}
} // namespace

int main() {
    try {
        verify(ClipUpdateRate::Sixteenth, sixteenthSample);
        verify(ClipUpdateRate::None, readySample);
        verifyRegion(ClipUpdateRate::Sixteenth, sixteenthSample);
        verifyRegion(ClipUpdateRate::None, readySample);
        require(allocations == 0, "gain scheduling and playback allocate nothing inside callbacks");
        std::printf("PASS -- running gain and loop edits switch at frames %lld (1/16) and %lld (None), 0 sample error\n",
            static_cast<long long>(sixteenthSample), static_cast<long long>(readySample));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
