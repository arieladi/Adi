// SPDX-License-Identifier: AGPL-3.0-or-later
#define ADI_TEST_COUNT_REPLACED_DELETE
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/engine/capture_scene.hpp"
#include "adi/engine/graph.hpp"
#include "adi/engine/launch_audio_runtime.hpp"
#include "adi/engine/launch_capture.hpp"
#include "adi/engine/launch_gain_edits.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
int allocations = 0;
int releases = 0;
std::atomic<const void*> watchedSource{nullptr};
std::atomic<bool> releasedWatchedSource{false};
}
void adi::test::countReplacedNew() noexcept { if (adi::audio::onAudioThread) ++allocations; }
void adi::test::countReplacedDelete(void* pointer) noexcept {
    if (!pointer) return;
    if (adi::audio::onAudioThread) ++releases;
    if (pointer == watchedSource.load(std::memory_order_relaxed))
        releasedWatchedSource.store(true,std::memory_order_relaxed);
}

namespace {
using namespace adi;
using namespace adi::engine;
constexpr std::int32_t maximumFrames = 257;
constexpr std::int32_t wideFrames = 4096;
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
    std::array<LaunchTransition, 4> captureStorage{};
    LaunchCaptureBuffer capture{captureStorage};

    explicit Fixture(bool varying = false, std::int32_t maxFrames = maximumFrames) {
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
        spec.maxFrames = maxFrames;
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
        std::array<float, wideFrames> left{}, right{};
        float* outputs[]{left.data(), right.data()};
        AudioIo io;
        io.out = outputs;
        io.numOut = 2;
        io.frames = size;
        io.streamTimeSamples = sample;
        auto block = session.beginRender();
        block.observeLaunchCapture(capture);
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
    require(planLaunchCapture(fixture.capture, {0, 0}, {fixture.sample, textproj::kPPQ}, {}).error ==
        LaunchCaptureError::SourceEdited, "capture rejects a live gain revision instead of writing different audio");
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
    require(planLaunchCapture(fixture.capture, {0, 0}, {fixture.sample, textproj::kPPQ}, {}).error ==
        LaunchCaptureError::SourceEdited, "capture detects a live loop revision without adding a launch edge");
    std::array<LaunchTrackStatus, 2> rows{};
    LaunchStatusHeader status;
    require(fixture.session.readLaunchStatus(rows, status) && rows.front().run == fixture.run &&
        rows.front().loopStartTick == ticks(first) && rows.front().loopLengthTicks == ticks(last) - ticks(first),
        "published Track Status reports the applied loop on the original run");
}

float replacementSignal(std::int64_t frame) {
    return -0.25f + static_cast<float>((3 * frame + 7) % 128) / 1024.0f;
}

std::unique_ptr<LaunchAudioClip> replacementSource(std::size_t frames = 10'000) {
    std::vector<float> pcm(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        pcm[frame] = replacementSignal(static_cast<std::int64_t>(frame));
    return std::make_unique<LaunchAudioClip>(LaunchAudioClip::fromSamples(clipId,48'000,1,std::move(pcm)));
}

void verifySourceLeaseInterleave() {
    adi::engine::detail::LaunchSourceEditLease lease;
    require(!lease.idleToken(),"a driver without a completed start is closed to PCM posts");
    lease.resume();
    const auto staged = lease.idleToken();
    require(staged.has_value(),"owner obtains the rolling token before writing candidate PCM");
    lease.suspend();
    require(!lease.publish(*staged) && !lease.held() && !lease.idleToken(),
        "a last callback stopping between staging and publication cannot leave an orphan reservation");
    lease.resume();
    require(!lease.publish(*staged),"stop and resume cannot make the pre-stop token current again");
    const auto fresh = lease.idleToken();
    require(fresh && lease.publish(*fresh) && lease.held(),"the new generation accepts the new owner token");
    lease.suspend();
    require(!lease.held(),"the last callback also clears a reservation when publication wins the race");
}

void verifyPreparedSource() {
    Fixture fixture(true,wideFrames);
    fixture.render(2500,signal);
    const auto run = fixture.run;
    require(run != 0,"observe the original resident audio run");
    const LaunchClipSourceEdit edit{1,clipId,incarnation,run,ClipUpdateRate::Sixteenth};
    auto wrongShape = replacementSource(9999);
    require(!fixture.session.postLaunchClipSourceEdit(edit,wrongShape) && wrongShape,
        "incompatible prepared PCM keeps ownership with the caller");
    auto stale = edit;
    ++stale.run;
    auto staleSource = replacementSource();
    require(fixture.session.postLaunchClipSourceEdit(stale,staleSource) && !staleSource,
        "post a prepared candidate with a stale run stamp");
    fixture.render(13,signal);
    fixture.render(1,signal); // callback retirement makes the sole prepared slot reusable

    auto newSource = replacementSource();
    require(fixture.session.postLaunchClipSourceEdit(edit,newSource) && !newSource,
        "admit a resident replacement for the addressed run");
    CaptureInsertedScene sceneCapture;
    sceneCapture.edit.ok = true;
    const auto refusedScene = commitCaptureAndInsertScene(*fixture.store,fixture.session,sceneCapture);
    require(!refusedScene.durable.ok && refusedScene.durable.error == "Session scene capture needs the authored clip source",
        "refuse Capture and Insert Scene before any authored copy when a PCM edit is held");
    auto competing = replacementSource();
    require(!fixture.session.postLaunchClipSourceEdit(edit,competing) && competing,
        "the bounded handover refuses a second simultaneous revision");
    require(fixture.session.postLaunchClipGainEdit({1,clipId,incarnation,run,0.5f,ClipUpdateRate::ThirtySecond}),
        "queue a gain change at a separate boundary in the same callback");
    const auto expected = [](std::int64_t frame) {
        const auto pcm = frame < sixteenthSample ? signal(frame) : replacementSignal(frame);
        return pcm * (frame < sixteenthSample / 2 ? 1.0f : 0.5f);
    };
    fixture.render(wideFrames,expected);
    require(fixture.session.setDelayCompensation(false), "rebuild while keeping the original PCM bank");
    fixture.render(maximumFrames,expected);
    require(fixture.transitions == 1 && fixture.run == run,
        "prepared PCM switches without a new launch, source phase or run identity");
    const std::array stop{LaunchRequest{LaunchRequestKind::StopTrack,1,{},{},incarnation}};
    require(fixture.session.postLaunchGesture(stop),"stop the edited run");
    fixture.render(17,[](auto) { return 0.0f; });
    const auto restart = fixture.sample;
    const std::array start{LaunchRequest{LaunchRequestKind::SlotPress,1,10,clipId,incarnation}};
    require(fixture.session.postLaunchGesture(start),"launch a fresh copy of the authored source");
    fixture.capture = LaunchCaptureBuffer{fixture.captureStorage};
    fixture.render(33,[&](auto frame) { return signal(frame-restart); });
    require(fixture.transitions == 2 && fixture.run != run && !fixture.capture.sourceEdited(),
        "ordinary relaunch retires the prior edited run and records an unmodified source");
    auto immediate = replacementSource();
    watchedSource.store(immediate.get(),std::memory_order_relaxed);
    require(fixture.session.postLaunchClipSourceEdit({1,clipId,incarnation,fixture.run,ClipUpdateRate::None},
        immediate),"reuse the prepared slot for a fresh immediate edit");
    fixture.render(maximumFrames,[&](auto frame) { return replacementSignal(frame-restart); });
    require(fixture.capture.sourceEdited() &&
        planLaunchCapture(fixture.capture, {restart,0}, {fixture.sample,textproj::kPPQ}, {}).error ==
            LaunchCaptureError::SourceEdited,"exact capture refuses a replacement even without a gain edit");

    (void)fixture.session.tick(1);
    require(!releasedWatchedSource.load(std::memory_order_relaxed),"owner collection keeps the live PCM lease");
    const auto pause = fixture.session.transport().position();
    fixture.session.transport().play(false);
    constexpr std::int32_t pausedFrames = 17;
    fixture.render(pausedFrames,[](auto) { return 0.0f; });
    require(!fixture.session.launchSourceEditReserved() && !releasedWatchedSource.load(std::memory_order_relaxed),
        "stopped transport releases its reservation after the last callback without freeing PCM on the driver");
    (void)fixture.session.tick(2);
    require(releasedWatchedSource.load(std::memory_order_relaxed),"the owner collects a retired PCM buffer without another edit");
    watchedSource.store(nullptr,std::memory_order_relaxed);
    fixture.session.transport().play();
    require(fixture.session.transport().position() == pause,"the source clock stayed at the paused position");
    fixture.render(maximumFrames,[&](auto frame) { return signal(frame-restart-pausedFrames); });
    require(fixture.transitions == 2,"resume keeps the original run at its authored phase without the retired PCM");

    auto deviceStopSource = replacementSource();
    releasedWatchedSource.store(false,std::memory_order_relaxed);
    watchedSource.store(deviceStopSource.get(),std::memory_order_relaxed);
    const LaunchClipSourceEdit afterPause{1,clipId,incarnation,fixture.run,ClipUpdateRate::None};
    require(fixture.session.postLaunchClipSourceEdit(afterPause,deviceStopSource),"post a source after the device resumed");
    fixture.render(maximumFrames,[&](auto frame) { return replacementSignal(frame-restart-pausedFrames); });
    fixture.session.release();
    require(!fixture.session.launchSourceEditReserved() && releasedWatchedSource.load(std::memory_order_relaxed),
        "quiescent device release closes and reclaims its prepared source without another callback");
    watchedSource.store(nullptr,std::memory_order_relaxed);
    auto whileReleased = replacementSource();
    require(!fixture.session.postLaunchClipSourceEdit(afterPause,whileReleased) && whileReleased,
        "the prior rolling clock cannot admit a PCM source while the device is released");
    fixture.session.prepare(48'000,wideFrames);
    require(!fixture.session.postLaunchClipSourceEdit(afterPause,whileReleased) && whileReleased,
        "device prepare alone cannot reopen the source handover");
    fixture.render(maximumFrames,[&](auto frame) { return signal(frame-restart-pausedFrames); });
    require(fixture.session.postLaunchClipSourceEdit(afterPause,whileReleased) && !whileReleased,
        "a new rendered callback reopens source handovers for the actual live run");
    std::printf("PASS -- prepared PCM changes at frame %lld, keeps source phase and composes with gain\n",
        static_cast<long long>(sixteenthSample));
}

void verifyPcmUpdateAcrossArrangementLoop() {
    Fixture fixture(true);
    fixture.render(static_cast<std::int32_t>(readySample),signal);
    fixture.session.transport().loop(0,1000);
    auto prepared = replacementSource();
    require(fixture.session.postLaunchClipSourceEdit({1,clipId,incarnation,fixture.run,ClipUpdateRate::Sixteenth},
        prepared),"queue across an Arrangement loop shorter than the Clip Update Rate grid");
    while (fixture.sample <= sixteenthSample + maximumFrames)
        fixture.render(maximumFrames,[](auto frame) {
            return frame < sixteenthSample ? signal(frame) : replacementSignal(frame);
        });
    require(fixture.session.transport().position() < 1000 && fixture.transitions == 1,
        "Arrangement wrap carries the edit deadline and running PCM phase without a relaunch");
}

void verifyPcmBankReplacement() {
    const auto bank = [](float value) {
        LaunchSlot slot;
        slot.track = 1;
        slot.scene = 10;
        slot.clip = clipId;
        slot.endTick = textproj::kPPQ;
        return std::make_shared<const LaunchAudioProject>(incarnation,120,std::vector<std::int64_t>{1},
            std::vector{slot},std::vector<std::int64_t>{10},
            std::vector{LaunchAudioClip::fromSamples(clipId,48'000,1,std::vector<float>(10'000,value))});
    };
    LaunchAudioRuntime first(bank(0.125f),{1},maximumFrames);
    LaunchAudioRuntime second(bank(0.25f),{1},maximumFrames);
    LaunchAudioRuntime third(bank(0.5f),{1},maximumFrames);
    LaunchAudioDriver driver(1);
    Transport transport;
    transport.play();
    const auto render = [&](LaunchAudioRuntime& runtime, auto expected) {
        constexpr int frames = 17;
        LaunchBlock timing;
        timing.frames = frames;
        timing.global = LaunchQuantization::None;
        timing.tickAtSample = [](const void*,std::int64_t sample) noexcept {
            return (sample * textproj::kPPQ + 12'000) / 24'000;
        };
        timing.sampleAtTick = [](const void*,std::int64_t tick) noexcept {
            return (tick * 24'000 + textproj::kPPQ / 2) / textproj::kPPQ;
        };
        std::array<float,frames> pcm{};
        float* channels[]{pcm.data()};
        NodeIo io;
        io.out = channels;
        io.channels = 1;
        io.frames = frames;
        io.sampleRate = 48'000;
        const auto origin = transport.position();
        {
            audio::CallbackScope scope;
            runtime.begin(driver,timing,transport,origin,nullptr,nullptr);
            runtime.overlay(1,io);
            runtime.finish();
        }
        for (auto index = std::size_t{0}; index < pcm.size(); ++index)
            require(pcm[index] == expected(origin + static_cast<std::int64_t>(index)),
                "a source replacement stays on its addressed bank and keeps the sample phase");
        transport.advance(frames);
    };
    const std::array start{LaunchRequest{LaunchRequestKind::SlotPress,1,10,clipId,incarnation}};
    require(driver.post(start),"launch from the currently rendered bank");
    render(first,[](auto) { return 0.125f; });
    LaunchStatusHeader header;
    std::array<LaunchTrackStatus,1> track{};
    require(driver.readStatus(track,header) && track[0].run != 0,"observe the run before a bank handover");
    const auto previousRun = track[0].run;
    const auto restart = transport.position();
    auto guessedSource = replacementSource();
    require(first.postSourceEdit(driver,{1,clipId,incarnation,previousRun + 1,ClipUpdateRate::None},
        guessedSource) && driver.post(start),"queue a future run stamp alongside a fresh launch");
    render(first,[](auto) { return 0.125f; });
    require(driver.readStatus(track,header) && track[0].run == previousRun + 1,
        "a new run sounds authored PCM even when its identity was guessed before admission");
    render(first,[](auto) { return 0.125f; });

    const LaunchClipSourceEdit edit{1,clipId,incarnation,track[0].run,ClipUpdateRate::None};
    auto replacement = replacementSource();
    const auto alternate = [restart](std::int64_t frame) { return replacementSignal(frame-restart); };
    require(second.postSourceEdit(driver,edit,replacement),"post against a newer owner-published bank");
    render(first,[](auto) { return 0.125f; });
    render(second,alternate);
    render(third,[](auto) { return 0.5f; });
    require(driver.sourceEditReserved(),"retain prepared PCM until every earlier callback range has finished");
    render(third,[](auto) { return 0.5f; });
    require(!driver.sourceEditReserved(),"retire an obsolete PCM lease when the next callback starts");
    replacement = replacementSource();
    require(third.postSourceEdit(driver,edit,replacement),"reuse only the retired prepared slot");
    render(third,alternate);
    driver.suspend(); // the pinned graph temporarily has no Session audio runtime
    require(!driver.sourceEditReserved(),"a missing runtime retires its final PCM lease");
    replacement = replacementSource();
    require(!third.postSourceEdit(driver,edit,replacement) && replacement,
        "a suspended driver refuses a new source even if the owner still sees an old rolling clock");
    render(third,[](auto) { return 0.5f; });
    require(allocations == 0 && releases == 0 && audio::callbackFileIo == 0,
        "prepared source changes and retirements allocate, release and read no files inside callbacks");
}
} // namespace

int main() {
    try {
        verify(ClipUpdateRate::Sixteenth, sixteenthSample);
        verify(ClipUpdateRate::None, readySample);
        verifyRegion(ClipUpdateRate::Sixteenth, sixteenthSample);
        verifyRegion(ClipUpdateRate::None, readySample);
        verifySourceLeaseInterleave();
        verifyPreparedSource();
        verifyPcmUpdateAcrossArrangementLoop();
        verifyPcmBankReplacement();
        require(allocations == 0, "gain scheduling and playback allocate nothing inside callbacks");
        std::printf("PASS -- running gain and loop edits switch at frames %lld (1/16) and %lld (None), 0 sample error\n",
            static_cast<long long>(sixteenthSample), static_cast<long long>(readySample));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
