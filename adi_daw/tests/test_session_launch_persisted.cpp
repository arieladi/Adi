// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/launch_audio_runtime.hpp"
#include "adi/engine/launch_paged_audio.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <thread>

namespace { std::atomic<unsigned> allocations{0}; }
void adi::test::countReplacedNew() noexcept { if (adi::audio::onAudioThread) ++allocations; }

namespace {
using namespace adi;
using namespace adi::engine;
constexpr std::int32_t blockSize = 257;
constexpr std::int64_t barFrames = 96'000;

void require(bool success, const std::string& message) {
    if (!success) throw std::runtime_error(message);
}

struct Fixture {
    test::TempDirectory directory{"session_launch_persisted", "song"};
    std::unique_ptr<Store> store;

    Fixture() {
        StoreError error{};
        store = Store::createEmpty(directory.path() / "song.adi", error);
        require(static_cast<bool>(store), "create persisted Session fixture");
        test::applySessionSchema(store->db());
        write("raw.wav", 44100, 176400, 0.125f, 0.25f);
        write("warp.wav", 48000, 12000, -0.25f, -0.25f);
        store->db().exec(
            "INSERT INTO project(id,name,sample_rate) VALUES(1,'Persisted launch',48000);"
            "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Slot source',0),(9,'master','Main',1);"
            "INSERT INTO scenes(id,ord,name) VALUES(10,0,'Resample'),(20,1,'Warp');"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'raw','raw.wav','wav'),(2,'warp','warp.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,gain_db) VALUES"
            "(101,1,'audio',0,46126080,6.020599913279624),(102,1,'audio',0,5765760,0);"
            "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,channel_mode) VALUES(101,1,176400,2);"
            "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,10,101),(1,20,102);"
            "INSERT INTO clip_launch(clip_id,ram_mode) VALUES(101,1),(102,1);");
        const auto anchors = audio::writeWarpMap(std::vector<WarpMarker>{{0, 0}, {12000, textproj::kPPQ}}, 12000);
        require(static_cast<bool>(anchors), "encode the fixed-tempo Warp map");
        SQLite::Statement warp(store->db(), "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) "
                                          "VALUES(102,2,12000,1,'re-pitch',?)");
        warp.bind(1, anchors.bytes.data(), static_cast<int>(anchors.bytes.size()));
        warp.exec();
    }

    void write(const char* name, std::uint32_t rate, std::uint32_t frames, float left, float right) const {
        std::vector<float> samples(static_cast<std::size_t>(frames) * 2);
        for (std::size_t i = 0; i < frames; ++i) {
            samples[2 * i] = left;
            samples[2 * i + 1] = right;
        }
        audio::WavWriter wav(directory.path() / name, rate, 2, audio::WavFormat::Float32);
        wav.write(samples.data(), frames);
        wav.close();
    }
};

OpRequest scene(std::int64_t id, std::uint64_t stamp) {
    OpRequest command;
    command.opType = "session.launchScene";
    command.payload = {{"scene", id}, {"incarnation", stamp}};
    return command;
}

struct Render {
    Session& session;
    std::int64_t stream = 0;
    std::array<LaunchTransition, 4> edges{};
    std::size_t size = 0;
    std::array<float, blockSize> left{}, right{};

    void block() {
        float* output[]{left.data(), right.data()};
        AudioIo io{output, nullptr, 2, 0, blockSize, stream};
        bool rendered = false;
        {
            auto driver = session.beginRender();
            driver.observeLaunchTransitions(this, [](void* user, const LaunchTransition& edge, std::int64_t) noexcept {
                auto& self = *static_cast<Render*>(user);
                if (self.size < self.edges.size()) self.edges[self.size++] = edge;
            });
            rendered = driver.process(io);
        }
        require(rendered, "render persisted Session graph");
        stream += blockSize;
    }
};

void persisted(Fixture& fixture) {
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*fixture.store, {}, SessionSpec{2, 48000, blockSize}))
        throw std::runtime_error(session.error());
    const auto incarnation = session.launchIncarnation();
    require(incarnation != 0 && session.problems().empty(), "stored clips are prepared through the native source reader");
    Render render{session};
    const auto actions = [&](std::int64_t id, std::uint64_t stamp) {
        const auto commands = std::array{scene(id, stamp)};
        const auto result = OpJournal(*fixture.store).commit(commands);
        require(result.ok, "admit reserved scene operation");
    };
    actions(10, incarnation + 1);
    render.block();
    require(!session.transport().playing() && render.size == 0, "stale scene press cannot start transport");
    actions(10, incarnation);
    render.block();
    require(session.transport().playing() && render.size == 1 && render.edges[0].sample == 0 &&
        render.edges[0].newClip == 101, "valid scene starts playback from stopped through the driver");
    require(std::abs(render.left.back() - 0.5f) < 0.0005f && render.left.back() == render.right.back(),
        "44.1 kHz slot resamples to 48 kHz with source channel mode and clip gain");
    require(session.setDelayCompensation(false) && session.launchIncarnation() == incarnation,
        "ordinary graph rebuild keeps persisted launch incarnation");
    render.block();
    require(render.size == 1 && std::abs(render.left.back() - 0.5f) < 0.0005f,
        "same run survives ordinary graph rebuild");
    actions(20, incarnation);
    while (session.transport().position() < barFrames + 1024) render.block();
    require(render.size == 2 && render.edges[1].sample == barFrames && render.edges[1].newClip == 102,
        "second persisted scene changes exactly at frame 96000");
    require(std::abs(render.left.back() + 0.25f) < 0.0005f && render.left.back() == render.right.back(),
        "Warp prepared the second clip at the Song tempo");
    while (session.transport().position() < barFrames + 16'000) render.block();
    require(render.left.back() < -0.249f,
        "fixed-tempo Warp remains audible after the unstretched 12000-frame source ends");
    require(allocations == 0 && audio::callbackFileIo == 0, "launch callback allocates and reads no files");
    std::printf("PASS -- persisted scenes switch at %lld, 44.1 kHz source converted and 2x Warp verified, "
                "0 callback allocations/file calls\n", static_cast<long long>(render.edges[1].sample));
}

void tempoSceneFromArrangementLoop(bool startsAtEnd) {
    constexpr auto loopEnd = 2 * textproj::kPPQ;
    Fixture fixture;
    fixture.store->db().exec("UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=20");
    OpRequest loop;
    loop.opType = "transport.setLoop";
    loop.payload = {{"on", true}, {"start", 0}, {"end", loopEnd}};
    require(OpJournal(*fixture.store).commit(loop).ok, "save the Arrangement Loop brace");

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*fixture.store, {}, {2, 48'000, blockSize})) throw std::runtime_error(session.error());
    bool started;
    {
        auto block = session.beginRender();
        started = block.startPlayback(startsAtEnd ? loopEnd : loopEnd - textproj::kPPQ, false);
    }
    require(started, "use the Arrangement playback entry point");
    Render render{session};
    render.block();
    require(session.transport().looping() != startsAtEnd, "observe the effective loop after starting playback");
    const auto launch = scene(20, session.launchIncarnation());
    require(OpJournal(*fixture.store).commit(launch).ok, "submit a saved tempo scene");
    const auto end = (startsAtEnd ? barFrames / 2 : barFrames) + 1024;
    while (render.stream < end) render.block();
    const auto bpm = session.beginRender().bpmAtTick(4 * textproj::kPPQ);
    if (startsAtEnd) {
        require(render.size == 1 && render.edges[0].sample == barFrames && render.edges[0].newClip == 102 &&
            std::abs(render.left.back() + 0.25f) < 0.0005f,
            "bypassed Loop brace permits an audible tempo scene on the next bar");
        require(bpm == 140, "scene clock performs at the bar");
    } else {
        require(render.size == 0 && session.transport().looping() && bpm == 120,
            "active Loop brace keeps its clock when the scene bar is unreachable");
    }
}

void tempoScenesWithArrangementLoop() {
    constexpr auto quarter = textproj::kPPQ;
    constexpr auto loopEnd = 2 * quarter;
    struct Scenario {
        std::int64_t startTick, expectedBar; // zero when the driver must refuse
        bool loopBeforeStart;
        std::optional<bool> pendingLoopEdit;
    };
    const std::array cases{
        Scenario{quarter, 0, true, {}},
        Scenario{loopEnd, 1, true, {}},
        Scenario{3 * quarter, 1, true, {}},
        Scenario{4 * quarter - quarter / 128, 2, true, {}},
        Scenario{quarter, 0, false, true},
        Scenario{quarter, 0, true, false},
    };
    for (const auto& item : cases) {
        Fixture fixture;
        fixture.store->db().exec("UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=20");
        const auto saveLoop = [&](bool on) {
            OpRequest loop;
            loop.opType = "transport.setLoop";
            loop.payload = {{"on", on}, {"start", 0}, {"end", loopEnd}};
            require(OpJournal(*fixture.store).commit(loop).ok, "save the Arrangement Loop brace");
        };
        saveLoop(item.loopBeforeStart);

        Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(*fixture.store, {}, {2, 48'000, blockSize})) throw std::runtime_error(session.error());
        bool started = false;
        {
            auto block = session.beginRender();
            started = block.startPlayback(item.startTick, false);
        }
        require(started, "use the Arrangement playback entry point");
        const auto driverHasLoop = item.loopBeforeStart && item.startTick < loopEnd;
        require(session.transport().looping() == driverHasLoop, "observe the effective loop at the playback start");
        if (item.pendingLoopEdit) {
            saveLoop(*item.pendingLoopEdit);
            require(session.refresh(*fixture.store), "publish an Arrangement loop edit to the owner");
            require(session.transport().looping() == driverHasLoop, "driver still reports the previous loop state");
        }
        require(OpJournal(*fixture.store).commit(scene(20, session.launchIncarnation())).ok, "audit a saved tempo scene");

        Render render{session};
        const auto due = item.expectedBar * barFrames;
        const auto frames = due ? due - session.transport().position() + 1024 : 2 * barFrames + 1024;
        while (render.stream < frames) render.block();
        const auto boundary = std::max(item.expectedBar, std::int64_t{1}) * 4 * quarter;
        double bpmAtBoundary, bpmBeforeBoundary;
        {
            const auto clock = session.beginRender();
            bpmAtBoundary = clock.bpmAtTick(boundary);
            bpmBeforeBoundary = clock.bpmAtTick(boundary - 1);
        }
        if (item.expectedBar != 0) {
            require(render.size == 1 && render.edges[0].sample == due && render.edges[0].newClip == 102 &&
                std::abs(render.left.back() + 0.25f) < 0.0005f,
                "a bypassed Loop brace permits an audible tempo scene at the reachable bar");
            require(bpmAtBoundary == 140 && bpmBeforeBoundary == 120,
                "tempo starts at the exact admitted bar, including when the previous bar is too close");
        } else {
            require(render.size == 0 && session.transport().looping() == item.pendingLoopEdit.value_or(driverHasLoop) &&
                bpmAtBoundary == 120,
                "an active or unacknowledged Loop brace keeps scene audio and tempo unchanged");
        }
    }
    require(allocations == 0 && audio::callbackFileIo == 0, "loop-aware scene callbacks allocate and read no files");
    std::printf("PASS -- audited tempo scenes keep active loops and play at the reachable bar at or beyond Loop End\n");
}

struct PagedRender {
    static constexpr std::uint32_t rate = 48'000;
    LaunchAudioRuntime runtime;
    LaunchAudioDriver driver{1};
    Transport transport;
    const TempoMap& clock;
    std::array<float, blockSize> left{}, right{};
    std::array<LaunchTransition, 4> edges{};
    std::size_t count = 0;

    PagedRender(const std::shared_ptr<const LaunchAudioProject>& program, const TempoMap& tempo)
        : runtime(program, {1}, blockSize), clock(tempo) { transport.play(); }

    void block(LaunchAudioRuntime* replacement = nullptr) {
        auto& activeRuntime = replacement ? *replacement : runtime;
        LaunchBlock block;
        block.frames = blockSize;
        block.clock = &clock;
        block.tickAtSample = [](const void* state, std::int64_t sample) noexcept {
            return static_cast<const TempoMap*>(state)->secondsToTicks(static_cast<double>(sample) / rate);
        };
        block.sampleAtTick = [](const void* state, std::int64_t tick) noexcept -> std::int64_t {
            return std::llround(static_cast<const TempoMap*>(state)->ticksToSeconds(tick) * rate);
        };
        float* channels[]{left.data(), right.data()};
        NodeIo io;
        io.out = channels;
        io.channels = 2;
        io.frames = blockSize;
        io.sampleRate = rate;
        left.fill(0.0f);
        right.fill(0.0f);
        {
            audio::CallbackScope scope;
            activeRuntime.begin(driver, block, transport, transport.position(), this,
                [](void* data, const LaunchTransition& edge, std::int64_t) noexcept {
                    auto& rendered = *static_cast<PagedRender*>(data);
                    if (rendered.count < rendered.edges.size()) rendered.edges[rendered.count++] = edge;
                });
            activeRuntime.overlay(1, io);
            activeRuntime.finish();
        }
        transport.advance(blockSize);
    }
};

void pagedLegato(Fixture& fixture) {
    constexpr std::uint32_t rate = 48'000;
    constexpr std::int64_t targetId = 105;
    constexpr std::int64_t sceneId = 50;
    constexpr std::uint64_t incarnation = 88;
    const auto valueAt = [](std::int64_t frame) { return 0.125f + static_cast<float>(frame % 4096) / 8192.0f; };
    {
        std::vector<float> waveform(static_cast<std::size_t>(barFrames * 2));
        for (std::size_t frame = 0; frame < waveform.size(); ++frame)
            waveform[frame] = valueAt(static_cast<std::int64_t>(frame));
        audio::WavWriter wave(fixture.directory.path() / "legato.wav", rate, 1, audio::WavFormat::Float32);
        wave.write(waveform.data(), static_cast<std::uint32_t>(waveform.size()));
        wave.close();
    }
    fixture.store->db().exec(
        "INSERT INTO scenes(id,ord,name) VALUES(50,9,'Far paged Legato');"
        "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(5,'legato','legato.wav','wav');"
        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(105,1,'audio',0,46126080);"
        "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(105,5,192000);"
        "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,50,105);"
        "INSERT INTO clip_launch(clip_id,legato,ram_mode) VALUES(105,1,0);");

    struct WorkerProbe {
        std::atomic<bool> requested{false}, served{false};
    } worker;
    // The page containing frame 96,000 lies well outside all original
    // launch-start pages. Wait for the real disk worker while transport holds.
    constexpr auto farPage = barFrames / 8192;
    const ClipPlayback::RequestSnapshotProbeForTest probe{&worker,
        [](void* context, std::int64_t first) noexcept {
            static_cast<WorkerProbe*>(context)->requested.store(first == farPage, std::memory_order_relaxed);
        }, [](void* context) noexcept {
            auto& state = *static_cast<WorkerProbe*>(context);
            if (state.requested.load(std::memory_order_relaxed)) state.served.store(true, std::memory_order_release);
        }};
    const auto model = rows::readModel(*fixture.store);
    const auto source = readClipProject(*fixture.store, model, true);
    std::vector<std::string> problems;
    TempoMap clock;
    clock.events.push_back({0, 120, 0});
    const auto program = prepareStoredLaunchProject(*source, clock, rate, 2, blockSize, incarnation, problems);
    const auto* target = program ? program->clip(targetId) : nullptr;
    require(target && target->pageReader() && problems.empty(), "prepare Legato source with RAM Off");
    auto& pages = *target->pageReader();
    pages.setRequestSnapshotProbeForTest(&probe);
    // The stored bank owns one driver, as it does when loaded by Session.
    PagedRender render(program, clock);
    const auto post = [&](std::int64_t id) {
        const std::array requests{LaunchRequest{LaunchRequestKind::ScenePress, {}, id, {}, incarnation}};
        require(render.driver.post(requests), "queue one literal Legato test scene");
    };
    post(10);
    render.block();
    post(sceneId);
    render.block();
    require(render.count == 1 && render.left.back() > 0.49f,
        "the old run still plays before the Legato boundary");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!worker.served.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(worker.served.load(std::memory_order_acquire), "queue the future Legato source page before take-over");
    while (render.transport.position() <= barFrames) render.block();
    const auto offset = static_cast<std::size_t>(barFrames - (render.transport.position() - blockSize));
    require(render.count == 2 && render.edges[1].newClip == targetId && render.edges[1].sample == barFrames &&
        render.edges[1].sourceTick == 4 * textproj::kPPQ,
        "Legato adopts the due source phase at exactly frame 96000");
    for (std::size_t i = offset; i < render.left.size(); ++i)
        require(render.left[i] == valueAt(barFrames + static_cast<std::int64_t>(i - offset)),
            "the first far paged Legato samples match the prepared native source");
    require(pages.underrunSamples() == 0 && allocations == 0 && audio::callbackFileIo == 0,
        "Legato queues bounded pages without underrun or callback work");
    LaunchStatusHeader header;
    std::array<LaunchTrackStatus, 1> status{};
    require(render.driver.readStatus(status, header) && status[0].run != 0,
        "read the long paged clip's active run");
    const LaunchClipRegionEdit cold{1, targetId, incarnation, status[0].run,
        {4 * textproj::kPPQ, 6 * textproj::kPPQ}, ClipUpdateRate::Sixteenth};
    require(!render.runtime.postRegionEdit(render.driver, cold, render.transport.position()),
        "refuse a live paged edit without guaranteed retained coverage");
    std::printf("PASS -- far paged Legato takes over at frame %lld with zero underruns\n",
        static_cast<long long>(barFrames));
}

void pagedLegatoWithoutLead(Fixture& fixture) {
    constexpr std::uint32_t rate = 48'000;
    constexpr std::int64_t targetId = 106, sceneId = 60;
    constexpr std::uint64_t incarnation = 89;
    constexpr std::size_t sourceFrames = 64'000;
    const auto valueAt = [](std::int64_t frame) { return 0.25f + static_cast<float>(frame % 4096) / 8192.0f; };
    {
        std::vector<float> waveform(sourceFrames);
        for (std::size_t frame = 0; frame < waveform.size(); ++frame)
            waveform[frame] = valueAt(static_cast<std::int64_t>(frame));
        audio::WavWriter wave(fixture.directory.path() / "short-legato.wav", rate, 1, audio::WavFormat::Float32);
        wave.write(waveform.data(), static_cast<std::uint32_t>(waveform.size()));
        wave.close();
    }
    fixture.store->db().exec(
        "INSERT INTO scenes(id,ord,name) VALUES(60,9,'Immediate paged Legato');"
        "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(6,'short-legato','short-legato.wav','wav');"
        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,loop_enabled,loop_start_ticks,loop_len_ticks) "
        "VALUES(106,1,'audio',0,15375360,1,7687680,7447440);"
        "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(106,6,64000);"
        "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,60,106);"
        "INSERT INTO clip_launch(clip_id,legato,launch_quant,ram_mode) VALUES(106,1,1,0);");

    // Detain the sole paged source's worker before posting the launch. An
    // audible result must come from pages retained by graph preparation.
    struct WorkerGate { std::atomic<bool> entered{false}, release{false}; } gate;
    const ClipPlayback::RequestSnapshotProbeForTest probe{&gate, [](void* context, std::int64_t) noexcept {
        auto& worker = *static_cast<WorkerGate*>(context);
        worker.entered.store(true, std::memory_order_release);
        while (!worker.release.load(std::memory_order_acquire)) std::this_thread::yield();
    }};
    const auto model = rows::readModel(*fixture.store);
    const auto source = readClipProject(*fixture.store, model, true);
    TempoMap clock;
    clock.events.push_back({0, 120, 0});
    std::vector<std::string> problems;
    const auto program = prepareStoredLaunchProject(*source, clock, rate, 2, blockSize, incarnation, problems);
    const auto* target = program ? program->clip(targetId) : nullptr;
    require(target && target->pageReader() && problems.empty(), "keep short Legato target on bounded disk pages");
    auto& pages = *target->pageReader();
    PagedRender render(program, clock);

    const auto unblock = [](WorkerGate* worker) noexcept { worker->release.store(true, std::memory_order_release); };
    const std::unique_ptr<WorkerGate, decltype(unblock)> unblockOnExit(&gate, unblock);
    pages.setRequestSnapshotProbeForTest(&probe);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!gate.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(gate.entered.load(std::memory_order_acquire), "hold the paged worker before a no-lead request");

    const std::array start{LaunchRequest{LaunchRequestKind::ScenePress, {}, 10, {}, incarnation}};
    require(render.driver.post(start), "start the resident outgoing clip");
    do { render.block(); } while (render.transport.position() < 45'000);
    const auto due = render.transport.position();
    const std::array takeOver{LaunchRequest{LaunchRequestKind::SlotPress, 1, sceneId, targetId, incarnation}};
    require(render.driver.post(takeOver), "post the immediate Legato target just before rendering");
    render.block();
    require(render.count == 2 && render.edges[1].newClip == targetId && render.edges[1].sample == due &&
            target->frameForTick(render.edges[1].sourceTick, clock.bpmAt(0)) == due,
            "immediate paged Legato keeps the exact outgoing phase and due sample");
    for (std::size_t frame = 0; frame < render.left.size(); ++frame)
        require(render.left[frame] == valueAt(due + static_cast<std::int64_t>(frame)) &&
                render.right[frame] == render.left[frame],
            "every first-callback sample reaches the retained far phase with the worker held");
    require(pages.underrunSamples() == 0 && allocations == 0 && audio::callbackFileIo == 0,
        "no-lead Legato needs no underrun, allocation or file I/O on the callback");
    std::printf("PASS -- immediate paged Legato with its worker held starts at %lld\n", static_cast<long long>(due));

    const auto ticks = [](std::int64_t frame) { return (frame * textproj::kPPQ + 12'000) / 24'000; };
    constexpr std::int64_t editSample = 48'000, loopStart = 63'500, loopLength = 12;
    LaunchStatusHeader header;
    std::array<LaunchTrackStatus, 1> status{};
    require(render.driver.readStatus(status, header) && status[0].run != 0,
        "read the short paged clip's active run");
    const auto run = status[0].run;
    const LaunchClipRegionEdit edit{1, targetId, incarnation, run,
        {ticks(loopStart), ticks(sourceFrames), LaunchClipLoopRegion{ticks(loopStart), ticks(loopStart + loopLength)}},
        ClipUpdateRate::Sixteenth};
    auto outside = edit;
    outside.region.end = ticks(sourceFrames + 1);
    require(!render.runtime.postRegionEdit(render.driver, outside, render.transport.position()),
        "refuse a live region ending outside retained audio");
    require(render.runtime.postRegionEdit(render.driver, edit, render.transport.position()),
        "queue a quantized loop beyond the authored loop in the retained bank");
    while (render.transport.position() < editSample + 2 * blockSize) {
        const auto sample = render.transport.position();
        render.block();
        for (std::size_t frame = 0; frame < render.left.size(); ++frame) {
            const auto at = sample + static_cast<std::int64_t>(frame);
            const auto phase = at < editSample ? at : loopStart + (at - editSample) % loopLength;
            require(render.left[frame] == valueAt(phase) && render.right[frame] == render.left[frame],
                "live paged audio follows the edited source phase from its exact quantized sample");
        }
    }
    require(render.count == 2 && render.driver.readStatus(status, header) && status[0].run == run &&
        status[0].loopStartTick == ticks(loopStart) && status[0].loopLengthTicks == ticks(loopStart + loopLength) - ticks(loopStart),
        "the paged loop update publishes on the original run");
    require(pages.underrunSamples() == 0 && allocations == 0 && audio::callbackFileIo == 0,
        "editing the retained region needs no underrun, callback allocation or file I/O");
    std::printf("PASS -- paged live loop change with its worker held occurs at %lld\n", static_cast<long long>(editSample));

    auto pending = edit;
    pending.region.start = ticks(loopStart - 1);
    require(render.runtime.postRegionEdit(render.driver, pending, render.transport.position()),
        "queue another edit against the currently retained bank");
    fixture.store->db().exec("UPDATE clip_launch SET legato=0 WHERE clip_id=106");
    const auto changedModel = rows::readModel(*fixture.store);
    const auto changedSource = readClipProject(*fixture.store, changedModel, true);
    const auto coldProgram = prepareStoredLaunchProject(*changedSource, clock, rate, 2, blockSize, incarnation, problems);
    const auto* cold = coldProgram ? coldProgram->clip(targetId) : nullptr;
    require(cold && cold->pageReader() && problems.empty(), "publish the same running clip with ordinary paged media");
    LaunchAudioRuntime replacement(coldProgram, {1}, blockSize);
    const auto replacementSample = render.transport.position();
    constexpr std::int64_t authoredLoopStart = 32'000, authoredLoopEnd = 63'000;
    const auto restoredPhase = authoredLoopStart + loopStart +
        (replacementSample - editSample) % loopLength - authoredLoopEnd;
    while (render.transport.position() < 54'000 + blockSize) {
        const auto sample = render.transport.position();
        render.block(&replacement);
        for (std::size_t frame = 0; frame < render.left.size(); ++frame)
            require(render.left[frame] == valueAt(restoredPhase + sample - replacementSample + static_cast<std::int64_t>(frame)),
                "a bank without retained pages restores the authored source and refuses the pending region");
    }
    require(render.count == 2 && render.driver.readStatus(status, header) && status[0].run == run &&
        status[0].loopStartTick == ticks(authoredLoopStart) &&
        cold->pageReader()->underrunSamples() == 0 && allocations == 0 && audio::callbackFileIo == 0,
        "published cache replacement preserves the run without playing a cold edited range");
}

void ramModes(Fixture& fixture) {
    constexpr std::uint32_t rate = 44'100, length = rate * 4;
    {
        std::vector<float> waveform(static_cast<std::size_t>(length) * 2);
        for (std::uint32_t at = 0; at < length; ++at) {
            const auto value = at < 33'075 ? 0.125f : 0.375f;
            waveform[2 * at] = waveform[2 * at + 1] = value;
        }
        audio::WavWriter wave(fixture.directory.path() / "loop.wav", rate, 2, audio::WavFormat::Float32);
        wave.write(waveform.data(), length);
        wave.close();
    }
    fixture.store->db().exec(
        "INSERT INTO scenes(id,ord,name) VALUES(30,2,'Page loop'),(40,3,'RAM loop');"
        "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(3,'loop','loop.wav','wav');"
        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,content_offset_ticks,loop_enabled,loop_start_ticks,loop_len_ticks) VALUES"
        "(103,1,'audio',0,23063040,11523832,1,5765760,5765760),"
        "(104,1,'audio',0,23063040,11523832,1,5765760,5765760);"
        "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(103,3,176400),(104,3,176400);"
        "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,30,103),(1,40,104);"
        "INSERT INTO clip_launch(clip_id,ram_mode) VALUES(103,0),(104,1);");

    const auto model = rows::readModel(*fixture.store);
    const auto source = readClipProject(*fixture.store, model, true);
    std::vector<std::string> problems;
    TempoMap clock;
    clock.events.push_back({0, 120, 0});
    const auto project = prepareStoredLaunchProject(*source, clock, 48000, 2, blockSize, 87, problems);
    const auto* paged = project ? project->clip(103) : nullptr;
    const auto* resident = project ? project->clip(104) : nullptr;
    require(problems.empty() && paged && resident && paged->pageReader() && !resident->pageReader() &&
        paged->bytes() == 0 && resident->bytes() == resident->frames() * 2 * sizeof(float) &&
        paged->pageReader()->pageBytesCeiling() < resident->bytes(), "RAM setting selects full PCM or bounded pages");

    const auto launchLoop = [&](std::int64_t id) {
        Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(*fixture.store, {}, SessionSpec{2, 48000, blockSize}))
            throw std::runtime_error(session.error());
        const auto command = scene(id, session.launchIncarnation());
        require(OpJournal(*fixture.store).commit(command).ok, "queue loop scene");
        Render render{session};
        render.block();
        return render.left;
    };
    const auto fromPages = launchLoop(30);
    const auto fromRam = launchLoop(40);
    require(fromPages == fromRam && std::abs(fromPages[31] - 0.375f) < 0.0001f &&
        std::abs(fromPages[32] - 0.125f) < 0.0001f, "both modes resample and wrap on exact frame 32");
    require(allocations == 0 && audio::callbackFileIo == 0, "paged loop renders without callback allocations or file calls");
    std::printf("PASS -- RAM mode %llu resident bytes versus a %llu-byte audio-page cap, loop wraps at frame 32\n",
        static_cast<unsigned long long>(resident->bytes()),
        static_cast<unsigned long long>(paged->pageReader()->pageBytesCeiling()));
}
} // namespace

int main() {
    try {
        Fixture fixture;
        persisted(fixture);
        ramModes(fixture);
        tempoSceneFromArrangementLoop(false);
        tempoSceneFromArrangementLoop(true);
        tempoScenesWithArrangementLoop();
        Fixture legato;
        pagedLegato(legato);
        Fixture immediateLegato;
        pagedLegatoWithoutLead(immediateLegato);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
