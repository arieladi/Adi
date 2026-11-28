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
#include "adi/engine/launch_warp_source.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>
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
    // Use the stored bank with exactly one driver, as Session does. Session
    // intentionally rejects an injected bank that owns playback page state.
    LaunchAudioRuntime runtime(program, {1}, blockSize);
    LaunchAudioDriver driver(1);
    Transport transport;
    transport.play();
    struct Output {
        std::array<float, blockSize> left{}, right{};
        std::array<LaunchTransition, 2> edges{};
        std::size_t count = 0;
    } output;
    const auto render = [&] {
        LaunchBlock block;
        block.frames = blockSize;
        block.clock = &clock;
        block.tickAtSample = [](const void* state, std::int64_t sample) noexcept {
            return static_cast<const TempoMap*>(state)->secondsToTicks(static_cast<double>(sample) / rate);
        };
        block.sampleAtTick = [](const void* state, std::int64_t tick) noexcept -> std::int64_t {
            return std::llround(static_cast<const TempoMap*>(state)->ticksToSeconds(tick) * rate);
        };
        float* channels[]{output.left.data(), output.right.data()};
        NodeIo io;
        io.out = channels;
        io.channels = 2;
        io.frames = blockSize;
        io.sampleRate = rate;
        output.left.fill(0.0f);
        output.right.fill(0.0f);
        {
            audio::CallbackScope scope;
            runtime.begin(driver, block, transport, transport.position(), &output,
                [](void* data, const LaunchTransition& edge, std::int64_t) noexcept {
                    auto& rendered = *static_cast<Output*>(data);
                    if (rendered.count < rendered.edges.size()) rendered.edges[rendered.count++] = edge;
                });
            runtime.overlay(1, io);
            runtime.finish();
        }
        transport.advance(blockSize);
    };
    const auto post = [&](std::int64_t id) {
        const std::array requests{LaunchRequest{LaunchRequestKind::ScenePress, {}, id, {}, incarnation}};
        require(driver.post(requests), "queue one literal Legato test scene");
    };
    post(10);
    render();
    post(sceneId);
    render();
    require(output.count == 1 && output.left.back() > 0.49f,
        "the old run still plays before the Legato boundary");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!worker.served.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(worker.served.load(std::memory_order_acquire), "queue the future Legato source page before take-over");
    while (transport.position() <= barFrames) render();
    const auto offset = static_cast<std::size_t>(barFrames - (transport.position() - blockSize));
    require(output.count == 2 && output.edges[1].newClip == targetId && output.edges[1].sample == barFrames &&
        output.edges[1].sourceTick == 4 * textproj::kPPQ,
        "Legato adopts the due source phase at exactly frame 96000");
    for (std::size_t i = offset; i < output.left.size(); ++i)
        require(output.left[i] == valueAt(barFrames + static_cast<std::int64_t>(i - offset)),
            "the first far paged Legato samples match the prepared native source");
    require(pages.underrunSamples() == 0 && allocations == 0 && audio::callbackFileIo == 0,
        "Legato queues bounded pages without underrun or callback work");
    std::printf("PASS -- far paged Legato takes over at frame %lld with zero underruns\n",
        static_cast<long long>(barFrames));
}

void nativePagedLegato(std::string_view mode, bool looping = false) {
    constexpr std::uint32_t rate = 48'000;
    constexpr std::int64_t targetId = 106, targetScene = 60;
    constexpr std::int64_t coldDue = 24'000, warmDue = 48'000;
    constexpr auto ppq = textproj::kPPQ;
    constexpr auto ticksForFrames = [](std::int64_t frames) { return (frames * ppq + 12'000) / 24'000; };
    constexpr auto loopTail = ticksForFrames(48);
    constexpr auto loopBegin = 5 * ppq + loopTail;
    constexpr auto loopLength = ppq;
    constexpr auto loopCue = 6 * ppq;
    constexpr auto loopHeadPage = (120'048 + 239'000) / 8192;
    constexpr auto oneShotColdPage = 48'000 / 8192;
    constexpr std::uint64_t incarnation = 89;
    Fixture fixture;
    {
        const std::uint32_t frames = looping ? 623'000 : 384'000;
        std::vector<float> waveform(frames);
        for (std::size_t index = 0; index < waveform.size(); ++index)
            waveform[index] = .125f + static_cast<float>(index % 9323) / 32768.f;
        audio::WavWriter wave(fixture.directory.path() / "native-legato.wav",rate,1,audio::WavFormat::Float32);
        wave.write(waveform.data(),frames);
        wave.close();
        std::vector<WarpMarker> geometry{{0,0}};
        if (looping) {
            // Requesting a block here touches all eight raw pages and
            // displaces the separately primed loop head.
            geometry.push_back({60'000,ticksForFrames(60'000)});
            geometry.push_back({300'000,ticksForFrames(61'000)});
        }
        geometry.push_back({frames,16*ppq});
        const auto markers = audio::writeWarpMap(geometry,frames);
        require(static_cast<bool>(markers),"encode the long native Warp markers");
        auto& db = fixture.store->db();
        db.exec("INSERT INTO scenes(id,ord,name) VALUES(60,9,'Native Legato'),(61,10,'Tempo cue');"
                "UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=61;"
                "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(6,'native-legato','native-legato.wav','wav');"
                "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,content_offset_ticks)"
                " VALUES(106,1,'audio',0,86486400,5765760);"
                "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,60,106);"
                "INSERT INTO clip_launch(clip_id,legato,ram_mode) VALUES(106,1,0)");
        if (looping) {
            SQLite::Statement clip(db,"UPDATE clips SET content_offset_ticks=?,length_ticks=?,loop_enabled=1,"
                                      "loop_start_ticks=?,loop_len_ticks=? WHERE id=106");
            clip.bind(1,static_cast<std::int64_t>(5*ppq));
            clip.bind(2,static_cast<std::int64_t>(11*ppq));
            clip.bind(3,static_cast<std::int64_t>(loopBegin));
            clip.bind(4,static_cast<std::int64_t>(loopLength));
            clip.exec();
        }
        SQLite::Statement row(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers)"
                                 " VALUES(106,6,?,1,?,?)");
        row.bind(1,static_cast<std::int64_t>(frames));
        row.bind(2,std::string(mode));
        row.bind(3,markers.bytes.data(),static_cast<int>(markers.bytes.size()));
        row.exec();
    }

    struct Gate {
        const std::int64_t page;
        std::atomic<bool> evicted{false}, held{false}, resume{false};
    } gate{looping ? loopHeadPage : oneShotColdPage};
    const ClipPlayback::RequestSnapshotProbeForTest probe{.context=&gate,
        .beforePageClaim=[](void* raw,std::int64_t,std::int64_t page) noexcept {
            auto& state = *static_cast<Gate*>(raw);
            if (page != state.page || state.resume.load(std::memory_order_acquire)) return;
            state.held.store(true,std::memory_order_release);
            while (!state.resume.load(std::memory_order_acquire)) std::this_thread::yield();
        },
        .afterPageClaimAttempt=[](void* raw,std::int64_t oldPage,std::int64_t requestedPage,bool claimed) noexcept {
            auto& state = *static_cast<Gate*>(raw);
            if (claimed && oldPage == state.page && requestedPage != state.page)
                state.evicted.store(true,std::memory_order_release);
        }};
    TempoMap clock;
    clock.events.push_back({0,120,0});
    if (looping) clock.events.push_back({2*ppq,125,0});
    const auto sampleAt = [&](std::int64_t tick) { return std::llround(clock.ticksToSeconds(tick)*rate); };
    const auto warmSourceTick = looping ? loopCue : 3*ppq;
    const LaunchWarpPosition warmCue{warmDue,2*ppq,warmSourceTick};
    const auto warmHead = looping
        ? std::optional<LaunchWarpPosition>{{sampleAt(2*ppq+loopTail),2*ppq+loopTail,loopBegin}}
        : std::nullopt;
    const auto prepare = [&] {
        const auto model = rows::readModel(*fixture.store);
        const auto project = readClipProject(*fixture.store,model,true);
        std::vector<std::string> problems;
        auto bank = prepareStoredLaunchProject(*project,clock,rate,2,blockSize,incarnation,problems,
            kSessionResidentPcmBudget,looping ? std::optional<double>{120} : std::nullopt);
        require(bank && problems.empty(),"prepare the persisted native Legato source");
        return bank;
    };
    const auto bank = prepare();
    fixture.store->db().exec("UPDATE clip_launch SET ram_mode=1 WHERE clip_id=106");
    const auto referenceBank = prepare();
    const auto* target = bank->clip(targetId);
    const auto* reference = referenceBank->clip(targetId);
    require(target && target->warpPageReader() && target->followsTempo() &&
            !target->warpPageReader()->wholeSourceFitsPageCache() &&
            reference && reference->followsTempo() && !reference->usesPages(),
            "compare the long native page worker to a resident source using the same Warp mode");
    auto& pages = *target->warpPageReader();
    struct ResumeOnExit {
        Gate& gate;
        ~ResumeOnExit() { gate.resume.store(true,std::memory_order_release); }
    } resume{gate};
    ClipPlayback::setWarpRequestSnapshotProbeForTest(pages,&probe);

    LaunchAudioRuntime runtime(bank,{1},blockSize);
    LaunchAudioDriver driver(1);
    Transport transport;
    transport.play();
    struct Output {
        std::array<float,blockSize> left{}, right{};
        std::array<LaunchTransition,4> edges{};
        std::size_t count = 0;
    } output;
    const auto render = [&] {
        LaunchBlock block;
        block.frames = blockSize;
        block.global = LaunchQuantization::Quarter;
        block.clock = &clock;
        block.tickAtSample = [](const void* state,std::int64_t sample) noexcept {
            return static_cast<const TempoMap*>(state)->secondsToTicks(static_cast<double>(sample) / rate);
        };
        block.sampleAtTick = [](const void* state,std::int64_t tick) noexcept -> std::int64_t {
            return std::llround(static_cast<const TempoMap*>(state)->ticksToSeconds(tick)*rate);
        };
        float* channels[]{output.left.data(),output.right.data()};
        NodeIo io;
        io.out = channels;
        io.channels = 2;
        io.frames = blockSize;
        io.sampleRate = rate;
        {
            const audio::CallbackScope scope;
            runtime.begin(driver,block,transport,transport.position(),&output,
                [](void* data,const LaunchTransition& edge,std::int64_t) noexcept {
                    auto& current = *static_cast<Output*>(data);
                    if (current.count < current.edges.size()) current.edges[current.count++] = edge;
                });
            runtime.overlay(1,io);
            runtime.finish(driver);
        }
        transport.advance(blockSize);
    };
    const auto post = [&](std::int64_t sceneId,std::int64_t clipId) {
        const std::array requests{LaunchRequest{LaunchRequestKind::SlotPress,1,sceneId,clipId,incarnation}};
        require(driver.post(requests),"queue the manual quantized slot press");
    };
    const auto wait = [](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ready();
    };

    if (looping) {
        constexpr auto denseSource = ticksForFrames(60'110);
        constexpr auto denseTick = 3*ppq;
        {
            const audio::CallbackScope scope;
            target->prefetchWarp({sampleAt(denseTick),denseTick,denseSource});
        }
        require(wait([&] { return gate.evicted.load(std::memory_order_acquire); }),
                "the dense request displaces the saved loop-head page");
    }
    post(10,101);
    render();
    require(output.count == 1 && output.edges[0].newClip == 101,"start the outgoing resident source");
    post(targetScene,targetId);
    render();
    require(wait([&] { return gate.held.load(std::memory_order_acquire); }),
            "the real worker asks for the exact far cue and is held before claiming its page");
    if (looping) {
        bool tailReady = false;
        {
            const audio::CallbackScope scope;
            tailReady = target->reserveWarpCue({coldDue,ppq,loopCue},blockSize);
            if (tailReady) pages.releaseCue();
        }
        require(tailReady,"the disjoint launch tail alone is fully resident while the loop head is held");
    }
    while (transport.position() <= coldDue) render();
    require(output.count == 1 && pages.underrunSamples() == 0,
            "cold scheduled native Legato preserves the old run and never starts an underrun");
    for (const auto sample : output.left)
        require(std::abs(sample - .5f) < .0005f,"outgoing PCM remains audible through the refused cue");
    gate.resume.store(true,std::memory_order_release);

    post(targetScene,targetId);
    render();
    require(wait([&] {
        const audio::CallbackScope scope;
        if (!target->reserveWarpCue(warmCue,blockSize,warmHead ? std::span<const LaunchWarpPosition>(&*warmHead,1)
                                                          : std::span<const LaunchWarpPosition>{})) return false;
        pages.releaseCue();
        return true;
    }),"the second scheduled forecast fills its exact native pages");
    while (transport.position() <= warmDue) render();
    const auto offset = static_cast<std::int32_t>(warmDue - (transport.position() - blockSize));
    require(output.count == 2 && output.edges[1].newClip == targetId &&
            output.edges[1].sample == warmDue && output.edges[1].sourceTick == warmSourceTick &&
            output.edges[1].offset == offset,
            "the warmed cue changes run at the exact sample and trimmed musical phase");
    std::array<float,blockSize> expectedLeft{}, expectedRight{};
    float* channels[]{expectedLeft.data(),expectedRight.data()};
    const AudioIo expected{channels,nullptr,2,0,blockSize,0};
    const auto firstFrames = warmHead ? static_cast<std::int32_t>(warmHead->frame - warmDue) : blockSize-offset;
    renderLaunchAudio(expected,offset,firstFrames,reference,0,1.0f,-1,warmCue);
    if (warmHead) {
        require(firstFrames == 46 && offset + firstFrames < blockSize,
                "the effective 125 BPM clock puts the reanchored head inside the actual warm callback");
        renderLaunchAudio(expected,offset+firstFrames,blockSize-offset-firstFrames,reference,0,1.0f,-1,*warmHead);
    }
    bool audible = false;
    for (auto frame = std::int32_t{0}; frame < blockSize; ++frame) {
        const auto index = static_cast<std::size_t>(frame);
        if (frame < offset) {
            require(std::abs(output.left[index] - .5f) < .0005f,"the warm cue leaves the pre-boundary PCM intact");
            continue;
        }
        audible |= std::abs(expectedLeft[index]) > .05f;
        require(output.left[index] == expectedLeft[index] && output.right[index] == expectedRight[index],
                "each warm cue sample equals the resident native Warp reference");
    }
    require(audible && pages.underrunSamples() == 0 && allocations == 0 && audio::callbackFileIo == 0,
            "the warm native cue is audible with zero misses, callback allocations or callback file reads");
    std::printf("PASS -- native %.*s %s refused the cold cue at %lld and matched every warm sample at %lld\n",
                static_cast<int>(mode.size()),mode.data(),looping ? "loop" : "one-shot",
                static_cast<long long>(coldDue),static_cast<long long>(warmDue));
}

void repeatedBeatsLegato() {
    constexpr std::uint32_t rate = 48'000;
    constexpr std::int32_t framesPerBlock = 1024;
    constexpr std::int64_t targetId = 106, targetScene = 60;
    constexpr std::uint64_t incarnation = 90;
    constexpr auto ppq = textproj::kPPQ;
    constexpr auto ticksForFrames = [](std::int64_t frames) { return (frames * ppq + 12'000) / 24'000; };
    constexpr auto authoredOnset = ticksForFrames(120'000);
    constexpr auto loopBegin = authoredOnset + ticksForFrames(16);
    constexpr auto lapTicks = ppq / 96;  // 250 frames at 120 BPM
    constexpr auto initialTail = ticksForFrames(48);
    constexpr auto cueTick = loopBegin + lapTicks - initialTail;
    constexpr std::int64_t pageBoundary = 12 * 8192;
    constexpr auto coldPage = pageBoundary / 8192 - 1;

    Fixture fixture;
    {
        constexpr std::uint32_t frames = 384'000;
        std::vector<float> waveform(frames);
        for (std::size_t index = 0; index < waveform.size(); ++index)
            waveform[index] = .2f + .1f * static_cast<float>(index) / static_cast<float>(frames);
        audio::WavWriter wave(fixture.directory.path() / "beats-loop.wav",rate,1,audio::WavFormat::Float32);
        wave.write(waveform.data(),frames);
        wave.close();
        // The same authored onset yields P+4 at a slow head and P-4 at
        // the faster one. Only the latter needs the preceding raw page.
        const auto markers = audio::writeWarpMap(std::array{WarpMarker{0,0},
            WarpMarker{pageBoundary-12,authoredOnset},
            WarpMarker{pageBoundary+18'000,ticksForFrames(120'400)},WarpMarker{frames,16*ppq}},frames);
        require(static_cast<bool>(markers),"encode the Beats loop's authored onsets");
        auto& db = fixture.store->db();
        db.exec("INSERT INTO scenes(id,ord,name) VALUES(60,9,'Repeated Beats Legato'),(61,10,'Tempo cue');"
                "UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=61;"
                "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(6,'beats-loop','beats-loop.wav','wav');"
                "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(106,1,'audio',0,86486400);"
                "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,60,106);"
                "INSERT INTO clip_launch(clip_id,legato,ram_mode) VALUES(106,1,0)");
        SQLite::Statement clip(db,"UPDATE clips SET content_offset_ticks=?,loop_enabled=1,"
                                  "loop_start_ticks=?,loop_len_ticks=? WHERE id=106");
        clip.bind(1,static_cast<std::int64_t>(cueTick));
        clip.bind(2,static_cast<std::int64_t>(loopBegin));
        clip.bind(3,static_cast<std::int64_t>(lapTicks));
        clip.exec();
        SQLite::Statement row(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers)"
                                 " VALUES(106,6,384000,1,'beats',?)");
        row.bind(1,markers.bytes.data(),static_cast<int>(markers.bytes.size()));
        row.exec();
    }
    TempoMap clock;
    clock.events.push_back({0,120,0});
    for (const auto due : {ppq,2*ppq}) {
        const auto thirdHead = due + initialTail + 2*lapTicks;
        clock.events.push_back({thirdHead-ticksForFrames(16),240,0});
        clock.events.push_back({thirdHead+ticksForFrames(30),120,0});
    }
    const auto sampleAt = [&](std::int64_t tick) -> std::int64_t { return std::llround(clock.ticksToSeconds(tick)*rate); };
    const auto coldDue = sampleAt(ppq);
    const auto warmDue = sampleAt(2*ppq);
    // The previous callback ends on a rounded physical sample. Its inverse
    // tick supplies the outgoing phase at a cue on the next block's edge.
    const auto warmSourceTick = loopBegin + (cueTick - loopBegin +
        clock.secondsToTicks(static_cast<double>(warmDue)/rate)) % lapTicks;
    const auto wrapPositions = [&](std::int64_t tick,std::int64_t sourceTick) {
        std::array<LaunchWarpPosition,4> wraps;
        for (std::size_t index = 0; index < wraps.size(); ++index) {
            const auto at = tick + loopBegin + lapTicks - sourceTick + static_cast<std::int64_t>(index)*lapTicks;
            wraps[index] = {sampleAt(at),at,loopBegin};
        }
        return wraps;
    };
    const auto coldWraps = wrapPositions(ppq,cueTick);
    const auto warmWraps = wrapPositions(2*ppq,warmSourceTick);
    require(coldWraps[0].frame == coldDue+48 && coldWraps[2].frame == coldDue+540 &&
            coldWraps[3].frame == coldDue+775 && sampleAt(ppq+initialTail+4*lapTicks) == coldDue+1025,
            "the tempo burst changes only the interior Beats head inside the four-wrap buffer");

    struct Gate {
        std::atomic<bool> held{false}, resume{false};
    } gate;
    const ClipPlayback::RequestSnapshotProbeForTest probe{.context=&gate,
        .beforePageClaim=[](void* raw,std::int64_t,std::int64_t page) noexcept {
            auto& state = *static_cast<Gate*>(raw);
            if (page != coldPage || state.resume.load(std::memory_order_acquire)) return;
            state.held.store(true,std::memory_order_release);
            while (!state.resume.load(std::memory_order_acquire)) std::this_thread::yield();
        }};
    const auto prepare = [&] {
        const auto model = rows::readModel(*fixture.store);
        const auto project = readClipProject(*fixture.store,model,true);
        std::vector<std::string> problems;
        auto bank = prepareStoredLaunchProject(*project,clock,rate,2,framesPerBlock,incarnation,
                                               problems,kSessionResidentPcmBudget,120);
        require(bank && problems.empty(),"prepare the repeated Beats Loop");
        return bank;
    };
    const auto bank = prepare();
    fixture.store->db().exec("UPDATE clip_launch SET ram_mode=1 WHERE clip_id=106");
    const auto referenceBank = prepare();
    const auto* target = bank->clip(targetId);
    const auto* reference = referenceBank->clip(targetId);
    require(target && target->warpPageReader() && !target->warpPageReader()->wholeSourceFitsPageCache() &&
            reference && reference->followsTempo() && !reference->usesPages(),
            "compare the long paged Beats source to resident Beats");
    auto& pages = *target->warpPageReader();
    struct ResumeOnExit {
        Gate& gate;
        ~ResumeOnExit() { gate.resume.store(true,std::memory_order_release); }
    } resume{gate};
    ClipPlayback::setWarpRequestSnapshotProbeForTest(pages,&probe);

    LaunchAudioRuntime runtime(bank,{1},framesPerBlock);
    LaunchAudioDriver driver(1);
    Transport transport;
    transport.play();
    struct Output {
        std::array<float,framesPerBlock> left{}, right{};
        std::array<LaunchTransition,4> edges{};
        std::size_t count = 0;
    } output;
    const auto render = [&] {
        LaunchBlock block;
        block.frames = framesPerBlock;
        for (const auto due : {coldDue,warmDue}) {
            if (transport.position() < due && due-transport.position() < block.frames)
                block.frames = static_cast<std::int32_t>(due-transport.position());
        }
        block.global = LaunchQuantization::Quarter;
        block.clock = &clock;
        block.tickAtSample = [](const void* state,std::int64_t sample) noexcept {
            return static_cast<const TempoMap*>(state)->secondsToTicks(static_cast<double>(sample) / rate);
        };
        block.sampleAtTick = [](const void* state,std::int64_t tick) noexcept -> std::int64_t {
            return std::llround(static_cast<const TempoMap*>(state)->ticksToSeconds(tick)*rate);
        };
        float* channels[]{output.left.data(),output.right.data()};
        NodeIo io;
        io.out = channels;
        io.channels = 2;
        io.frames = block.frames;
        io.sampleRate = rate;
        {
            const audio::CallbackScope scope;
            runtime.begin(driver,block,transport,transport.position(),&output,
                [](void* data,const LaunchTransition& edge,std::int64_t) noexcept {
                    auto& current = *static_cast<Output*>(data);
                    if (current.count < current.edges.size()) current.edges[current.count++] = edge;
                });
            runtime.overlay(1,io);
            runtime.finish(driver);
        }
        transport.advance(block.frames);
    };
    const auto post = [&](std::int64_t sceneId,std::int64_t clipId) {
        const std::array requests{LaunchRequest{LaunchRequestKind::SlotPress,1,sceneId,clipId,incarnation}};
        require(driver.post(requests),"queue a manual Beats Legato press");
    };
    const auto wait = [](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ready();
    };
    const auto ready = [&](LaunchWarpPosition position,std::int32_t frames,
                          std::span<const LaunchWarpPosition> wraps = {}) {
        const audio::CallbackScope scope;
        if (!target->reserveWarpCue(position,frames,wraps)) return false;
        pages.releaseCue();
        return true;
    };

    post(10,101);
    render();
    require(output.count == 1 && output.edges[0].newClip == 101,"the outgoing run starts normally");
    post(targetScene,targetId);
    render();
    require(wait([&] { return gate.held.load(std::memory_order_acquire); }),
            "the worker asks for the raw page touched only by the interior Beats head");
    require(ready({coldDue,ppq,cueTick},48) && ready(coldWraps[0],250),
            "the initial cue and first complete lap are warm with the interior page still held");
    while (transport.position() <= coldDue) render();
    require(output.count == 1 && pages.underrunSamples() == 0 &&
            std::ranges::all_of(output.left,[](const float sample) { return std::abs(sample-.5f) < .0005f; }),
            "the missing interior lap keeps the entire cold callback on the old run");
    gate.resume.store(true,std::memory_order_release);

    post(targetScene,targetId);
    render();
    const LaunchWarpPosition warmCue{warmDue,2*ppq,warmSourceTick};
    require(wait([&] { return ready(warmCue,framesPerBlock,warmWraps); }),
            "the worker fills the union of all projected Beats lap windows");
    while (transport.position() <= warmDue) render();
    require(output.count == 2 && output.edges[1].newClip == targetId && output.edges[1].sample == warmDue &&
            output.edges[1].sourceTick == warmSourceTick && output.edges[1].offset == 0,
            "the warmed cue commits at the exact grid sample and saved Legato phase");

    std::array<float,framesPerBlock> expectedLeft{}, expectedRight{};
    float* channels[]{expectedLeft.data(),expectedRight.data()};
    const AudioIo expected{channels,nullptr,2,0,framesPerBlock,0};
    auto position = warmCue;
    const auto referenceThrough = [&](std::int64_t end) {
        const auto from = static_cast<std::int32_t>(position.frame-warmDue);
        const auto count = static_cast<std::int32_t>(end-position.frame);
        renderLaunchAudio(expected,from,count,reference,0,1.0f,-1,position);
    };
    for (const auto wrap : warmWraps) {
        referenceThrough(wrap.frame);
        position = wrap;
    }
    referenceThrough(warmDue+framesPerBlock);
    require(output.left == expectedLeft && output.right == expectedRight && output.left[540] > .1f &&
            pages.underrunSamples() == 0 && allocations == 0 && audio::callbackFileIo == 0,
            "all 1024 stereo samples, including the fast interior head, match resident Beats");
    std::printf("PASS -- Beats Legato leases four exact wraps, including the tempo-burst interior head\n");
}

enum class NativeLegatoGesture {
    Immediate, HeldImmediate, HeldQuantized, HeldQuantizedAfterWrap, HeldDeferred, HeldImmediateDeferred,
    HeldTwoGridResets, HeldThreeGridResets, HeldFourGridResets
};

void nativeManualLegato(std::string_view mode, NativeLegatoGesture gesture = NativeLegatoGesture::Immediate) {
    const bool repeat = gesture != NativeLegatoGesture::Immediate;
    const bool afterWrap = gesture == NativeLegatoGesture::HeldQuantizedAfterWrap;
    const bool deferred = gesture == NativeLegatoGesture::HeldDeferred;
    const bool immediateDeferred = gesture == NativeLegatoGesture::HeldImmediateDeferred;
    const bool laterRepeat = deferred || immediateDeferred;
    const auto gridResets = gesture == NativeLegatoGesture::HeldFourGridResets ? 4u :
        gesture == NativeLegatoGesture::HeldThreeGridResets ? 3u : gesture == NativeLegatoGesture::HeldTwoGridResets ? 2u : 0u;
    const bool quantized = gesture == NativeLegatoGesture::HeldQuantized || afterWrap || deferred || gridResets != 0;
    constexpr std::int32_t maxFrames = 4096;
    const auto framesPerBlock = quantized ? maxFrames : blockSize;
    constexpr std::uint32_t rate = 48'000;
    constexpr std::int64_t targetId = 106, targetScene = 60;
    constexpr std::uint64_t incarnation = 91;
    constexpr auto ppq = textproj::kPPQ;
    const std::int64_t framesPerBeat = gridResets == 4 ? 7200 : gridResets == 3 ? 9000 : gridResets == 2 ? 12000 : 24000;
    const auto bpm = rate * 60.0 / static_cast<double>(framesPerBeat);
    const auto ticksForFrames = [framesPerBeat](std::int64_t frames) {
        return (frames * ppq + framesPerBeat / 2) / framesPerBeat;
    };
    const auto start = ticksForFrames(2'000);
    const auto loopEnd = ticksForFrames(deferred ? 51'000 : immediateDeferred ? 15'000 : afterWrap ? 16'000 : quantized ? 17'000 : 14'000);
    const auto loopBegin = repeat ? ticksForFrames(deferred ? 10'000 : immediateDeferred ? 4'000 : afterWrap ? 14'500 : quantized ? 15'000 : 10'000)
                                  : start;
    constexpr std::int64_t cueAge = 11'952;
    const std::int32_t restartOffset = gridResets != 0 ? static_cast<std::int32_t>(framesPerBeat / 8) :
        deferred ? 6000 : immediateDeferred ? 1048 : quantized ? 3000 : 48;
    constexpr std::int64_t coldPage = 1, startPage = 0;
    Fixture fixture;
    {
        const std::uint32_t frames = repeat ? 431'000 : 192'000;
        const std::uint16_t channels = afterWrap || immediateDeferred || gridResets != 0 ? 2 : 1;
        std::vector<float> waveform(static_cast<std::size_t>(frames)*channels);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto at = static_cast<float>(frame) / 20'000.f;
            waveform[frame*channels] = .1f + .3f*at;
            if (channels == 2) waveform[frame*channels+1] = -.2f + .1f*at;
        }
        audio::WavWriter wave(fixture.directory.path() / "immediate-native.wav",rate,channels,audio::WavFormat::Float32);
        wave.write(waveform.data(),frames);
        wave.close();
        std::vector<WarpMarker> geometry{{0,0}};
        if (repeat) {
            // A dense span away from both phases lets the existing worker
            // displace the initially warmed saved Start without test-only eviction.
            for (std::int64_t frame = 60'000; frame <= 61'000; frame += 20)
                geometry.push_back({60'000 + 240*(frame-60'000),ticksForFrames(frame)});
        }
        geometry.push_back({frames,gridResets > 2 ? ticksForFrames(96'000) : 8 * ppq});
        const auto markers = audio::writeWarpMap(geometry,frames);
        require(static_cast<bool>(markers),"encode the immediate native Warp map");
        auto& db = fixture.store->db();
        db.exec("INSERT INTO scenes(id,ord,name) VALUES(60,9,'Immediate Native Legato'),(61,10,'Tempo cue');"
                "UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=61;"
                "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(6,'immediate-native','immediate-native.wav','wav');"
                "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(106,1,'audio',0,40360320);"
                "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,60,106);"
                "INSERT INTO clip_launch(clip_id,legato,ram_mode) VALUES(106,1,0)");
        if (repeat) db.exec("UPDATE clip_launch SET launch_mode=3 WHERE clip_id=106");
        if (gridResets != 0) {
            SQLite::Statement tempo(db,"UPDATE tempo_map SET bpm=? WHERE pos_ticks=0");
            tempo.bind(1,bpm);
            tempo.exec();
        }
        if (deferred) db.exec("UPDATE clip_launch SET launch_quant=12 WHERE clip_id=106");
        else if (quantized) db.exec("UPDATE clip_launch SET launch_quant=14 WHERE clip_id=106");
        SQLite::Statement clip(db,"UPDATE clips SET content_offset_ticks=?,loop_enabled=1,"
                                  "loop_start_ticks=?,loop_len_ticks=? WHERE id=106");
        clip.bind(1,static_cast<std::int64_t>(start));
        clip.bind(2,static_cast<std::int64_t>(loopBegin));
        clip.bind(3,static_cast<std::int64_t>(loopEnd-loopBegin));
        clip.exec();
        SQLite::Statement row(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers)"
                                 " VALUES(106,6,?,1,?,?)");
        row.bind(1,static_cast<std::int64_t>(frames));
        row.bind(2,std::string(mode));
        row.bind(3,markers.bytes.data(),static_cast<int>(markers.bytes.size()));
        row.exec();
    }
    TempoMap clock;
    clock.events.push_back({0,bpm,0});
    const auto tickAt = [&](std::int64_t frame) { return clock.secondsToTicks(static_cast<double>(frame) / rate); };
    const auto sampleAt = [&](std::int64_t tick) -> std::int64_t { return std::llround(clock.ticksToSeconds(tick)*rate); };
    const auto prepare = [&](bool ram) {
        SQLite::Statement launch(fixture.store->db(),"UPDATE clip_launch SET ram_mode=? WHERE clip_id=106");
        launch.bind(1,ram ? 1 : 0);
        launch.exec();
        const auto model = rows::readModel(*fixture.store);
        const auto project = readClipProject(*fixture.store,model,true);
        std::vector<std::string> problems;
        auto bank = prepareStoredLaunchProject(*project,clock,rate,2,framesPerBlock,incarnation,problems);
        require(bank && problems.empty(),"prepare the immediate native Warp slot");
        return bank;
    };
    const auto referenceBank = prepare(true);
    const auto* reference = referenceBank->clip(targetId);
    require(reference && reference->followsTempo() && !reference->usesPages(),"prepare independent resident Warp");
    const auto wait = [](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ready();
    };
    const auto run = [&](bool cold) {
        struct Probe {
            const bool repeat;
            const std::int64_t heldPage;
            std::atomic<bool> armed{false}, held{false}, resume{false}, checked{false}, evicted{false};
            std::atomic<unsigned> snapshots{0}, coldClaims{0};
            std::atomic<std::int64_t> before{-1}, after{-1};
        } state{repeat && !immediateDeferred,deferred ? 6 : startPage};
        const ClipPlayback::RequestSnapshotProbeForTest probe{
            .context=&state,
            .afterSnapshot=[](void* raw,std::int64_t page) noexcept {
                auto& gate = *static_cast<Probe*>(raw);
                if (gate.repeat || !gate.armed.load(std::memory_order_acquire)) return;
                const auto visit = gate.snapshots.fetch_add(1,std::memory_order_relaxed);
                if (visit == 0) {
                    gate.before.store(page,std::memory_order_relaxed);
                    gate.held.store(true,std::memory_order_release);
                    while (!gate.resume.load(std::memory_order_acquire)) std::this_thread::yield();
                } else if (visit == 1) {
                    gate.after.store(page,std::memory_order_relaxed);
                }
            },
            .afterService=[](void* raw) noexcept {
                auto& gate = *static_cast<Probe*>(raw);
                if (!gate.repeat && gate.armed.load(std::memory_order_acquire) &&
                    gate.snapshots.load(std::memory_order_relaxed) >= 2)
                    gate.checked.store(true,std::memory_order_release);
            },
            .beforePageClaim=[](void* raw,std::int64_t,std::int64_t page) noexcept {
                auto& gate = *static_cast<Probe*>(raw);
                if (!gate.armed.load(std::memory_order_acquire)) return;
                if (!gate.repeat && page == coldPage) gate.coldClaims.fetch_add(1,std::memory_order_relaxed);
                if (gate.repeat && page == gate.heldPage) {
                    gate.held.store(true,std::memory_order_release);
                    while (!gate.resume.load(std::memory_order_acquire)) std::this_thread::yield();
                }
            },
            .afterPageClaimAttempt=[](void* raw,std::int64_t oldPage,std::int64_t page,bool claimed) noexcept {
                auto& gate = *static_cast<Probe*>(raw);
                if (gate.repeat && claimed && oldPage == gate.heldPage && page != gate.heldPage)
                    gate.evicted.store(true,std::memory_order_release);
            }};
        const auto bank = prepare(false);
        const auto* target = bank->clip(targetId);
        require(target && target->warpPageReader(),"prepare the RAM-off native source");
        auto& pages = *target->warpPageReader();
        struct ResumeOnExit {
            Probe& state;
            ~ResumeOnExit() { state.resume.store(true,std::memory_order_release); }
        } resume{state};
        ClipPlayback::setWarpRequestSnapshotProbeForTest(pages,&probe);

        const auto dryStart = cold ? std::int64_t{0} : blockSize;
        const auto due = gridResets != 0 ? 12'000 - 12'000 % restartOffset :
            deferred ? 48'000 : quantized ? 12'000 : dryStart + cueAge;
        const auto sourceTick = start + tickAt(due) - tickAt(dryStart);
        const LaunchWarpPosition cue{due,tickAt(due),sourceTick};
        const auto ordinaryEnd = cue.anchorTick + loopEnd-sourceTick;
        const auto headTick = quantized ? tickAt(due+restartOffset) : ordinaryEnd;
        const std::array starts{LaunchWarpPosition{sampleAt(ordinaryEnd),ordinaryEnd,loopBegin},
                                LaunchWarpPosition{sampleAt(headTick),headTick,start}};
        std::array<LaunchWarpPosition,kMaximumLaunchWarpCueWraps> gridStarts;
        const auto gridPlacements = std::span(gridStarts).first(gridResets);
        for (std::size_t index = 0; index < gridPlacements.size(); ++index) {
            const auto gridTick = tickAt(due + (static_cast<std::int64_t>(index) + 1) * restartOffset);
            gridPlacements[index] = {sampleAt(gridTick),gridTick,start};
        }
        const std::span<const LaunchWarpPosition> wraps = gridResets != 0 ? gridPlacements :
            deferred ? std::span(starts).first(1) : std::span(starts).last(immediateDeferred ? 0 : afterWrap ? 2 : 1);
        require(starts.back().frame-due == restartOffset &&
                (laterRepeat ? restartOffset > framesPerBlock : restartOffset < framesPerBlock),
                "the saved-Start deadline is on the expected callback");
        if (deferred) {
            require(wraps.front().frame == due+(cold ? 1000 : 1257) &&
                    wraps.front().frame < due+framesPerBlock,
                    "ordinary Wrap is in the first callback and Repeat is deferred");
        } else if (afterWrap) {
            const auto firstWrap = due + (cold ? 2000 : 2257);
            require(wraps.front().frame == firstWrap && firstWrap < wraps.back().frame &&
                    sampleAt(ordinaryEnd+loopEnd-loopBegin) > wraps.back().frame,
                    "exactly one ordinary wrap precedes the frozen saved-Start retrigger");
        } else if (gridResets != 0) {
            const auto lastReset = due + static_cast<std::int64_t>(gridResets) * restartOffset;
            const auto nextReset = sampleAt(tickAt(lastReset + restartOffset));
            require(starts.back().frame <= sampleAt(ordinaryEnd) && gridPlacements.back().frame == lastReset &&
                    lastReset < due + framesPerBlock && nextReset == lastReset + restartOffset &&
                    due + framesPerBlock < nextReset && sampleAt(headTick + loopEnd - start) >= due + framesPerBlock,
                    "all expected saved Start resets fit and the next grid reset and ordinary wraps are later");
        } else if (quantized) {
            require(cold ? sampleAt(ordinaryEnd) == wraps.back().frame : sampleAt(ordinaryEnd) > wraps.back().frame,
                    "the frozen Repeat grid lands exactly at End when cold and before it when warm");
        }
        const auto windowReady = [&](LaunchWarpPosition position,std::int32_t frames,
                                     std::span<const LaunchWarpPosition> after = {}) {
            const audio::CallbackScope scope;
            if (!target->reserveWarpCue(position,frames,after)) return false;
            pages.releaseCue();
            return true;
        };
        const auto ready = [&] { return windowReady(cue,framesPerBlock,wraps); };
        if (!repeat || immediateDeferred)
            require(!ready(),"the inherited cue is outside the owner's saved Start and loop pages");

        LaunchAudioRuntime runtime(bank,{1},framesPerBlock);
        LaunchAudioDriver driver(1);
        Transport transport;
        transport.play();
        struct Output {
            std::array<float,maxFrames> left{}, right{};
            std::array<LaunchTransition,8> edges{};
            std::size_t count = 0;
        } output;
        const auto render = [&](std::int32_t frames) {
            LaunchBlock block;
            block.frames = frames;
            block.global = LaunchQuantization::None;
            block.clock = &clock;
            block.tickAtSample = [](const void* raw,std::int64_t sample) noexcept {
                return static_cast<const TempoMap*>(raw)->secondsToTicks(static_cast<double>(sample) / rate);
            };
            block.sampleAtTick = [](const void* raw,std::int64_t tick) noexcept -> std::int64_t {
                return std::llround(static_cast<const TempoMap*>(raw)->ticksToSeconds(tick)*rate);
            };
            float* channels[]{output.left.data(),output.right.data()};
            NodeIo io;
            io.out = channels;
            io.channels = 2;
            io.frames = frames;
            io.sampleRate = rate;
            {
                const audio::CallbackScope scope;
                runtime.begin(driver,block,transport,transport.position(),&output,
                    [](void* data,const LaunchTransition& edge,std::int64_t) noexcept {
                        auto& current = *static_cast<Output*>(data);
                        if (current.count < current.edges.size()) current.edges[current.count++] = edge;
                    });
                runtime.overlay(1,io);
                runtime.finish(driver);
            }
            transport.advance(frames);
        };
        const auto post = [&](std::int64_t sceneId,std::int64_t clipId) {
            const std::array requests{LaunchRequest{LaunchRequestKind::SlotPress,1,sceneId,clipId,incarnation}};
            require(driver.post(requests),"post the native slot gesture");
        };
        if (!cold) {
            post(targetScene,targetId);
            render(blockSize);
            require(output.count == 1 && output.edges[0].newClip == targetId && output.edges[0].sourceTick == start,
                    "ordinary saved native playback reads the target's Start");
            if (!deferred)
                require(wait(ready),"normal native read-ahead fills the exact later cue and loop head");
        }
        const auto through = [&](std::int64_t end) {
            while (transport.position() < end)
                render(static_cast<std::int32_t>(std::min<std::int64_t>(framesPerBlock,end-transport.position())));
        };
        post(10,101);
        through(quantized ? due-512 : due);
        std::array<LaunchTrackStatus,1> rows{};
        LaunchStatusHeader status{};
        require(driver.readStatus(rows,status) && rows[0].clip == 101,"observe the outgoing dry run");
        const auto runId = rows[0].run;
        if (cold && deferred) {
            require(!ready(),"the scheduled inherited native window is genuinely cold");
            state.armed.store(true,std::memory_order_release);
        } else if (cold && repeat && !immediateDeferred) {
            constexpr auto denseFrame = 60'110;
            const auto denseTick = ticksForFrames(denseFrame);
            {
                const audio::CallbackScope scope;
                target->prefetchWarp({denseFrame,denseTick,denseTick});
            }
            require(wait([&] { return state.evicted.load(std::memory_order_acquire); }),
                    "ordinary bounded worker requests can evict the saved Repeat Start");
            state.armed.store(true,std::memory_order_release);
            if (!quantized) {
                const audio::CallbackScope scope;
                target->prefetchWarp(cue);
            }
        } else if (cold) {
            require(!ready(),"the private target cue is still genuinely cold before the press");
            state.armed.store(true,std::memory_order_release);
            require(wait([&] { return state.held.load(std::memory_order_acquire); }),
                    "observe and hold the worker's ordinary demand before the cold press");
        }

        post(targetScene,targetId);
        if (quantized) through(due);
        if (cold && deferred) {
            require(wait([&] { return state.held.load(std::memory_order_acquire); }),
                    "the scheduled forecast reaches the actually cold inherited raw page");
            require(!ready() && windowReady(wraps.front(),framesPerBlock) &&
                    windowReady(starts.back(),framesPerBlock),
                    "ordinary head and later saved Start are ready while the inherited cue alone is cold");
        } else if (cold && repeat && !immediateDeferred) {
            require(wait([&] { return state.held.load(std::memory_order_acquire); }),
                    "leave saved Start cold after filling the inherited Repeat cue");
            const auto beforeRepeats = wraps.first(wraps.size() - (gridResets != 0 ? gridResets : 1u));
            require(windowReady(cue,framesPerBlock,beforeRepeats) && !ready(),
                    "the disjoint Repeat Start is the only cold required phase");
        } else if (deferred) {
            require(wait(ready),"the scheduled worker forecast fills the inherited cue before the due callback");
        }
        const auto active = [framesPerBlock](const auto& channel) {
            return std::span<const float>(channel).first(static_cast<std::size_t>(framesPerBlock));
        };
        render(framesPerBlock);
        if (cold) {
            const auto oldPcm = [&](float sample) { return std::abs(sample-.5f) < .0005f; };
            require(output.count == 1 && pages.underrunSamples() == 0 &&
                    std::ranges::all_of(active(output.left),oldPcm) && std::ranges::all_of(active(output.right),oldPcm),
                    "the cold native press keeps every sample of the outgoing callback");
            state.resume.store(true,std::memory_order_release);
            if (!repeat || immediateDeferred) {
                require(wait([&] { return state.checked.load(std::memory_order_acquire); }) &&
                        state.coldClaims.load(std::memory_order_relaxed) == 0 &&
                        state.before.load(std::memory_order_relaxed) == state.after.load(std::memory_order_relaxed),
                        "the refused press posts no demand for its missing worker page");
                state.armed.store(false,std::memory_order_release);
                const audio::CallbackScope scope;
                target->prefetchWarp(cue,wraps);
            }
            require(wait(ready),"independent traffic can fill the formerly missing cue");
            for (unsigned block = 0; block < 4; ++block) {
                render(framesPerBlock);
                require(std::ranges::all_of(active(output.left),oldPcm),"warming cannot replay the discarded press");
            }
            require(driver.readStatus(rows,status) && rows[0].clip == 101 && rows[0].run == runId &&
                    rows[0].queued.action == LaunchQueuedAction::None && output.count == 1,
                    "the original run remains selected with no queued retry or late edge");
        } else {
            const auto expectedEdges = gridResets != 0 ? 3u + gridResets : repeat && !laterRepeat ? 4u : 3u;
            require(output.count == expectedEdges && output.edges[2].newClip == targetId &&
                    output.edges[2].sample == due && output.edges[2].sourceTick == sourceTick && output.edges[2].offset == 0,
                    "the warm native Legato switches at the exact sample and inherited tick");
            if (repeat && !laterRepeat)
                require(output.edges[3].oldClip == targetId && output.edges[3].newClip == targetId &&
                        output.edges[3].sample == due+restartOffset && output.edges[3].sourceTick == start &&
                        output.edges[3].newRun != output.edges[2].newRun && loopBegin != start,
                        "the held Repeat restarts at saved Start rather than the ordinary loop head");
            for (std::size_t index = 1; index < gridPlacements.size(); ++index) {
                const auto& edge = output.edges[3 + index];
                require(edge.oldClip == targetId && edge.newClip == targetId && edge.sample == gridPlacements[index].frame &&
                        edge.sourceTick == start && edge.newRun != output.edges[2 + index].newRun,
                        "the same callback takes each saved Start at the exact frozen grid");
            }
            std::array<float,maxFrames> expectedLeft{}, expectedRight{};
            float* channels[]{expectedLeft.data(),expectedRight.data()};
            const AudioIo oracle{channels,nullptr,2,0,framesPerBlock,0};
            auto position = cue;
            const auto renderReference = [&](std::int64_t end) {
                renderLaunchAudio(oracle,static_cast<std::int32_t>(position.frame-due),
                    static_cast<std::int32_t>(end-position.frame),reference,0,1.0f,-1,position);
            };
            for (const auto wrap : wraps) {
                renderReference(wrap.frame);
                position = wrap;
            }
            renderReference(due+framesPerBlock);
            require(std::ranges::equal(active(output.left),active(expectedLeft)) &&
                    std::ranges::equal(active(output.right),active(expectedRight)) &&
                    std::ranges::any_of(active(output.left),[](float sample) { return sample > .05f; }) &&
                    pages.underrunSamples() == 0,"the entire warm buffer matches resident Warp at every placement");
            if (afterWrap) {
                const auto at = static_cast<std::size_t>(wraps.front().frame-due);
                require(output.left[at] > .2f && output.right[at] < -.1f,
                        "the ordinary head is audibly distinct in both channels before the Repeat");
            }
            if (laterRepeat) {
                through(starts.back().frame);
                require(output.count == 3 && windowReady(starts.back(),framesPerBlock),
                        "ordinary native read-ahead retains saved Start through the deferred deadline");
                render(framesPerBlock);
                require(output.count == 4 && output.edges[3].oldClip == targetId &&
                        output.edges[3].newClip == targetId && output.edges[3].sample == starts.back().frame &&
                        output.edges[3].sourceTick == start && output.edges[3].offset == 0 &&
                        output.edges[3].newRun != output.edges[2].newRun,
                        "the later callback restarts at exact saved Start on the original deadline");
                renderLaunchAudio(oracle,0,framesPerBlock,reference,0,1.0f,-1,starts.back());
                require(std::ranges::equal(active(output.left),active(expectedLeft)) &&
                        std::ranges::equal(active(output.right),active(expectedRight)) && pages.underrunSamples() == 0,
                        "the deferred restart callback matches the resident saved Start");
            }
        }
        require(allocations == 0 && audio::callbackFileIo == 0,"immediate native cue work stays off the callback");
    };
    run(false);
    run(true);
    std::printf("PASS -- %s native %.*s %s reused warm pages and a refused cue never replayed\n",
                gridResets == 4 ? "four-grid quantized" : gridResets == 3 ? "three-grid quantized" :
                gridResets == 2 ? "two-grid quantized" : deferred ? "deferred quantized" : immediateDeferred ? "deferred immediate" :
                afterWrap ? "quantized after wrap" : quantized ? "quantized" : "immediate",
                static_cast<int>(mode.size()),mode.data(),
                repeat ? "held Repeat" : "Legato");
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
enum class NativeFollowCase { Warm, Cold, Manual, Stop };
enum class NativeFollowSource { Physical, Native, AlignedR3 };

void nativeLinkedLegato(bool noGrid, NativeFollowCase testCase, int wrapCount = -1,
                        std::int32_t dueFrames = blockSize, NativeFollowSource outgoing = NativeFollowSource::Physical) {
    constexpr std::uint32_t rate = 48000;
    constexpr std::uint64_t incarnation = 93;
    const auto physical = outgoing == NativeFollowSource::Physical;
    const auto aligned = outgoing == NativeFollowSource::AlignedR3;
    constexpr std::int64_t rawStart = 2400;
    const std::int64_t loopHead = physical ? 124800 : 29280;
    const std::int64_t loopEnd = physical ? 134400 : 39937;
    const auto firstEnd = loopEnd-rawStart, period = loopEnd-loopHead;
    constexpr std::int64_t seam = 18*8192-280;
    constexpr std::int64_t denseFrame = 60110, firstMultiHead = 48*8192+1800;
    const auto looping = wrapCount >= 0;
    const auto multipleWraps = wrapCount > 1;
    const auto firstWrap = wrapCount == 0 ? 400 : 37;
    const auto lapFrames = multipleWraps ? std::array{135,90,64}[static_cast<std::size_t>(wrapCount-2)] : seam+firstWrap-90000;
    const auto headFrame = seam+firstWrap-lapFrames;
    const auto sourceShift = multipleWraps ? firstMultiHead+4+12*32-(headFrame+16) : wrapCount == 1 ? 239000 : 0;
    const auto nativeColdPage = multipleWraps ? firstMultiHead/8192-1 : wrapCount == 1 ? (headFrame+sourceShift)/8192 : 18;
    const std::uint32_t sourceFrames = wrapCount > 0 ? 420000 : 180000;
    const auto timelineFrames = sourceFrames - sourceShift;
    constexpr auto ppq = textproj::kPPQ;
    const auto cold = testCase == NativeFollowCase::Cold;
    const auto manual = testCase == NativeFollowCase::Manual;
    const auto stop = testCase == NativeFollowCase::Stop;
    const auto warm = testCase == NativeFollowCase::Warm;
    const auto nsAt = [](std::int64_t frame) { return (frame*1'000'000'000+rate/2)/rate; };
    TempoMap clock;
    clock.events.push_back({0,137.3,0});
    const auto tickAt = [&](std::int64_t frame) { return clock.secondsToTicks(static_cast<double>(frame)/rate); };
    const auto sampleAt = [&](std::int64_t tick) -> std::int64_t {
        return std::llround(clock.ticksToSeconds(tick)*rate);
    };
    const auto rawHeadTick = physical ? clock.storedNanosecondsToTicks(nsAt(loopHead)) : tickAt(loopHead);
    const auto rawEndTick = physical ? clock.storedNanosecondsToTicks(nsAt(loopEnd)) : tickAt(loopEnd);
    const auto rawStartTick = physical ? clock.storedNanosecondsToTicks(nsAt(rawStart)) : tickAt(rawStart);
    const auto firstEndTick = rawEndTick-rawStartTick;
    const auto dueTick = noGrid ? (physical ? tickAt(firstEnd) : firstEndTick) : (physical ? 7 : 2)*ppq;
    const auto due = sampleAt(dueTick);
    require(noGrid ? due == firstEnd : (due > firstEnd && tickAt(due) != dueTick),
            "the saved End and fractional grid project onto their authored sample");
    const auto elapsed = (due-firstEnd)%period;
    const auto inherited = rawHeadTick-rawStartTick + (physical ? static_cast<std::int64_t>(
        static_cast<long double>(elapsed)*static_cast<long double>(rawEndTick-rawHeadTick)/period)
        : noGrid ? 0 : tickAt(due)-firstEndTick);
    require(physical || noGrid || (due-firstEnd < period && inherited != rawHeadTick-rawStartTick+dueTick-firstEndTick),
            "the old musical lap supplies the rendered phase rather than the authored grid tick");
    const auto nativePhase = tickAt(seam);
    const auto sourceStart = nativePhase-inherited;
    const auto manualCue = loopHead+elapsed-rawStart;
    const LaunchWarpPosition placement{due,dueTick,nativePhase};
    const auto nativeLoopBegin = tickAt(headFrame);
    const auto nativeLoopLength = nativePhase+tickAt(firstWrap)-nativeLoopBegin;
    std::array<LaunchWarpPosition,4> wrapPlacements{};
    auto headTick = dueTick+tickAt(firstWrap);
    for (auto i=0; i<wrapCount; ++i, headTick+=nativeLoopLength) {
        auto& wrap = wrapPlacements[static_cast<std::size_t>(i)];
        wrap = {sampleAt(headTick),headTick,nativeLoopBegin};
        require(wrap.frame-due == firstWrap+i*lapFrames && wrap.frame < due+blockSize,
                "each independently projected wrap belongs to the saved callback");
    }
    require(!looping || sampleAt(headTick) >= due+blockSize,
            "the next ordinary wrap falls outside the saved callback");
    const std::span<const LaunchWarpPosition> wraps(wrapPlacements.data(),static_cast<std::size_t>(std::max(0,wrapCount)));

    Fixture fixture;
    {
        std::vector<float> pcm(2*sourceFrames);
        for (std::uint32_t frame = 0; frame < sourceFrames; ++frame) {
            pcm[2*frame] = -.3f + .13f * std::sin(static_cast<float>(frame)*.009f);
            pcm[2*frame+1] = .3f + .07f * std::cos(static_cast<float>(frame)*.011f);
        }
        audio::WavWriter file(fixture.directory.path()/"linked-native.wav",rate,2,audio::WavFormat::Float32);
        file.write(pcm.data(),sourceFrames);
        file.close();
    }
    auto& db = fixture.store->db();
    db.exec("UPDATE tempo_map SET bpm=137.3;"
            "INSERT INTO scenes(id,ord,name,tempo_bpm,tempo_enabled) VALUES(40,3,'Unused tempo scene',137.3,1);"
            "UPDATE clips SET time_base=1,pos_ticks=NULL,pos_ns=0,length_ticks=NULL,length_ns=3900000000,loop_enabled=1 WHERE id=101;"
            "UPDATE clip_launch SET clip_fade=0,launch_quant=1,follow_enabled=1,follow_linked=1,follow_loop_count=1,follow_a=4 WHERE clip_id=101;"
            "UPDATE media_files SET rel_path='linked-native.wav' WHERE id=2;"
            "UPDATE clip_launch SET legato=1,clip_fade=0 WHERE clip_id=102");
    {
        SQLite::Statement oldGeometry(db,"UPDATE clips SET loop_start_ticks=?,loop_len_ticks=? WHERE id=101");
        oldGeometry.bind(1,rawHeadTick); oldGeometry.bind(2,rawEndTick-rawHeadTick); oldGeometry.exec();
        if (physical) {
            SQLite::Statement pins(db,"INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns,"
                                      "loop_begin_ns,loop_end_ns) VALUES(101,?,?,?,?)");
            pins.bind(1,nsAt(rawStart)); pins.bind(2,nsAt(189600));
            pins.bind(3,nsAt(loopHead)); pins.bind(4,nsAt(loopEnd)); pins.exec();
        } else {
            SQLite::Statement musical(db,"UPDATE clips SET time_base=0,pos_ticks=0,pos_ns=NULL,length_ticks=?,"
                                         "length_ns=NULL,content_offset_ticks=? WHERE id=101");
            musical.bind(1,tickAt(192000)-rawStartTick); musical.bind(2,rawStartTick); musical.exec();
            const auto markers = audio::writeWarpMap(std::array<WarpMarker,2>{{{0,0},{176400,tickAt(192000)}}},176400);
            require(static_cast<bool>(markers),"save the outgoing musical Warp map");
            SQLite::Statement warp(db,"UPDATE audio_clips SET warp_enabled=1,warp_mode=?,warp_markers=? WHERE clip_id=101");
            warp.bind(1,aligned ? "complex" : "re-pitch");
            warp.bind(2,markers.bytes.data(),static_cast<int>(markers.bytes.size())); warp.exec();
            if (aligned) db.exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=101");
        }
        std::vector<WarpMarker> markersToSave{{0,0}};
        if (wrapCount > 0) {
            // A distant dense segment evicts initially primed pages through
            // normal Warp demand.
            markersToSave.push_back({60000,tickAt(60000)});
            markersToSave.push_back({300000,tickAt(61000)});
        }
        if (multipleWraps) {
            // A 32x region inside the lap requires the preceding PCM page
            // for sinc lookbehind, while its slow head and inherited cue
            // fit in the next page.
            markersToSave.push_back({firstMultiHead,tickAt(headFrame)});
            markersToSave.push_back({firstMultiHead+4,tickAt(headFrame+4)});
            markersToSave.push_back({firstMultiHead+4+12*32,tickAt(headFrame+16)});
        }
        markersToSave.push_back({sourceFrames,tickAt(timelineFrames)});
        const auto markers = audio::writeWarpMap(markersToSave,sourceFrames);
        require(static_cast<bool>(markers),"save the long native Warp source map");
        SQLite::Statement nativeAudio(db,"UPDATE audio_clips SET src_len_frames=?,warp_markers=? WHERE clip_id=102");
        nativeAudio.bind(1,sourceFrames);
        nativeAudio.bind(2,markers.bytes.data(),static_cast<int>(markers.bytes.size()));
        nativeAudio.exec();
        SQLite::Statement nativeClip(db,"UPDATE clips SET content_offset_ticks=?,length_ticks=? WHERE id=102");
        nativeClip.bind(1,sourceStart); nativeClip.bind(2,tickAt(timelineFrames)-sourceStart); nativeClip.exec();
        if (looping) {
            SQLite::Statement loop(db,"UPDATE clips SET loop_enabled=1,loop_start_ticks=?,loop_len_ticks=? WHERE id=102");
            loop.bind(1,nativeLoopBegin); loop.bind(2,nativeLoopLength); loop.exec();
        }
        SQLite::Statement grid(db,"UPDATE clip_launch SET launch_quant=? WHERE clip_id=102");
        grid.bind(1,noGrid ? 1 : 8); grid.exec();
    }
    if (manual) {
        db.exec("INSERT INTO scenes(id,ord,name) VALUES(30,2,'Cold manual cue');"
                "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(103,1,'audio',1,0,3900000000);"
                "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(103,1,176400);"
                "INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(103,0,3900000000);"
                "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,30,103);"
                "INSERT INTO clip_launch(clip_id,legato,ram_mode,clip_fade) VALUES(103,1,0,0);");
        SQLite::Statement grid(db,"UPDATE clip_launch SET launch_quant=? WHERE clip_id=103");
        grid.bind(1,noGrid ? 1 : 8); grid.exec();
    }

    const auto prepare = [&](bool ram) {
        SQLite::Statement mode(db,"UPDATE clip_launch SET ram_mode=? WHERE clip_id=102");
        mode.bind(1,ram ? 1 : 0); mode.exec();
        const auto project = readClipProject(*fixture.store,rows::readModel(*fixture.store),true);
        std::vector<std::string> problems;
        auto bank = prepareStoredLaunchProject(*project,clock,rate,2,blockSize,incarnation,problems);
        require(bank && problems.empty(),"prepare the saved outgoing loop and long native linked target");
        return bank;
    };
    struct PageGate {
        std::int64_t page;
        std::atomic<bool> blocked{false}, resume{false}, evicting{false}, evicted{false};
        ClipPlayback::RequestSnapshotProbeForTest probe() noexcept {
            return {.context=this,
                .afterService=[](void* raw) noexcept {
                    auto& gate = *static_cast<PageGate*>(raw);
                    if (gate.evicting.exchange(false,std::memory_order_acq_rel))
                        gate.evicted.store(true,std::memory_order_release);
                },
                .beforePageClaim=[](void* raw,std::int64_t,std::int64_t number) noexcept {
                    auto& gate = *static_cast<PageGate*>(raw);
                    if (number != gate.page) return;
                    gate.blocked.store(true,std::memory_order_release);
                    while (!gate.resume.load(std::memory_order_acquire)) std::this_thread::yield();
                },
                .afterPageClaimAttempt=[](void* raw,std::int64_t previous,std::int64_t requested,bool claimed) noexcept {
                    auto& gate = *static_cast<PageGate*>(raw);
                    if (claimed && previous == gate.page && requested != previous)
                        gate.evicting.store(true,std::memory_order_release);
                }};
        }
    } nativeGate{cold ? nativeColdPage : -1}, manualGate{manual && !noGrid ? manualCue/8192 : -1};
    const auto nativeProbe = nativeGate.probe(), manualProbe = manualGate.probe();
    const auto resident = warm ? prepare(true) : nullptr;
    const auto bank = prepare(false);
    const auto* target = bank->clip(102);
    const auto* previousClip = bank->clip(101);
    require(target && previousClip && target->warpPageReader() && !target->warpPageReader()->wholeSourceFitsPageCache() &&
            bank->view().slots.front().physicalStartFrame == (physical ? rawStart : -1) &&
            previousClip->followsTempo() == (!physical && !aligned) &&
            previousClip->latencySamples() == (aligned ? 2048 : 0),
            "prepare the selected outgoing source and a native target with a bounded page cache");
    auto& pages = *target->warpPageReader();
    auto* previousPages = aligned ? previousClip->pageReader() : nullptr;
    require(!aligned || previousPages,"prepare the outgoing aligned worker reader");
    auto* manualPages = manual && bank->clip(103) ? bank->clip(103)->pageReader() : nullptr;
    require(!manual || manualPages,"prepare the separate manual paged source");
    struct ResumeOnExit {
        PageGate& first;
        PageGate& second;
        ~ResumeOnExit() {
            first.resume.store(true,std::memory_order_release);
            second.resume.store(true,std::memory_order_release);
        }
    } resume{nativeGate,manualGate};
    ClipPlayback::setWarpRequestSnapshotProbeForTest(pages,&nativeProbe);
    if (manualPages) manualPages->setRequestSnapshotProbeForTest(&manualProbe);

    auto runtime = std::make_shared<LaunchAudioRuntime>(bank,std::vector<std::int64_t>{1},blockSize);
    LaunchAudioDriver driver(1);
    Transport transport;
    std::optional<ClipPlayback> trackSource;
    Node* compensatedSource = nullptr;
    if (aligned) {
        const auto project = readClipProject(*fixture.store,rows::readModel(*fixture.store),true);
        trackSource.emplace(project,transport,rate,2,blockSize);
        trackSource->attachLaunchAudio(1,runtime);
        compensatedSource = trackSource->sourcesFor(1).front();
    }
    const auto delay = compensatedSource ? compensatedSource->latencySamples() : 0;
    require(delay == (aligned ? 2048 : 0),"the real overlay declares the outgoing worker latency");
    transport.play();
    struct Output {
        std::array<LaunchTransition,4> edges{};
        std::size_t count = 0;
        std::array<float,blockSize> left{}, right{};
    } output;
    const auto through = [&](std::int64_t end) {
        while (transport.position() < end) {
            LaunchBlock block;
            block.frames = static_cast<std::int32_t>(std::min<std::int64_t>(blockSize,end-transport.position()));
            block.global = stop ? (noGrid ? LaunchQuantization::None : LaunchQuantization::Quarter) : LaunchQuantization::Bar;
            block.clock = &clock;
            block.tickAtSample = [](const void* state,std::int64_t sample) noexcept {
                return static_cast<const TempoMap*>(state)->secondsToTicks(static_cast<double>(sample)/rate);
            };
            block.sampleAtTick = [](const void* state,std::int64_t tick) noexcept -> std::int64_t {
                return std::llround(static_cast<const TempoMap*>(state)->ticksToSeconds(tick)*rate);
            };
            std::ranges::fill(output.left,0);
            std::ranges::fill(output.right,0);
            float* channels[]{output.left.data(),output.right.data()};
            NodeIo io;
            io.out = channels;
            io.channels = 2;
            io.frames = block.frames;
            io.sampleRate = rate;
            {
                const audio::CallbackScope scope;
                runtime->begin(driver,block,transport,transport.position(),&output,
                    [](void* state,const LaunchTransition& edge,std::int64_t) noexcept {
                        auto& report = *static_cast<Output*>(state);
                        if (report.count < report.edges.size()) report.edges[report.count++] = edge;
                    });
                if (compensatedSource) compensatedSource->process(io);
                else runtime->overlay(1,io);
                runtime->finish(driver);
            }
            transport.advance(block.frames);
        }
    };
    const auto wait = [](auto ready) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (!ready() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ready();
    };
    const auto readyAt = [&](LaunchWarpPosition position, int frames,
                             std::span<const LaunchWarpPosition> boundaries = {}) {
        const audio::CallbackScope scope;
        if (!target->reserveWarpCue(position,frames,boundaries)) return false;
        pages.releaseCue();
        return true;
    };
    const auto nativeReady = [&] { return readyAt(placement,blockSize,wraps); };
    if (previousPages) {
        {
            const audio::CallbackScope scope;
            previousPages->prefetch(101,17000);
        }
        for (const auto frame : {2400,8192,16384,24576,32768})
            require(wait([&] {
                const audio::CallbackScope scope;
                if (!previousPages->reserveWorkerCue(101,frame,blockSize,false)) return false;
                previousPages->releaseCue(101);
                return true;
            }),"warm the outgoing aligned loop independently of the incoming native pages");
    }
    if (cold && wrapCount > 0) {
        const auto dense = tickAt(denseFrame);
        {
            const audio::CallbackScope scope;
            target->prefetchWarp({sampleAt(dense),dense,dense});
        }
        require(wait([&]{ return nativeGate.evicted.load(std::memory_order_acquire); }),
                "the dense Warp window evicts the primed head before Linked Follow is armed");
    }
    require(driver.post(std::array{LaunchRequest{LaunchRequestKind::SlotPress,1,10,101,incarnation}}),"start the saved outgoing source");
    through(blockSize);
    std::array<LaunchTrackStatus,1> rows{};
    LaunchStatusHeader status{};
    require(driver.readStatus(rows,status) && rows[0].clip == 101 && rows[0].queued.action == LaunchQueuedAction::None,
            "the private Linked Follow never appears as an armed user operation");
    const auto oldRun = rows[0].run;
    if (cold) {
        require(wait([&] { return nativeGate.blocked.load(std::memory_order_acquire); }) && !nativeReady(),
                "the private forecast hits the worker gate on a required cold page");
        if (multipleWraps) {
            require(readyAt(placement,firstWrap) && readyAt(wraps.front(),1) && !readyAt(wraps.front(),20),
                    "the inherited cue and loop head are warm while the sinc window inside the lap is cold");
        } else {
            require(readyAt(placement,wrapCount == 1 ? blockSize : 64),
                wrapCount == 1 ? "the full linear cue is warm while the disjoint Loop Begin remains cold"
                               : "the private forecast warms the cue head while its adjacent page stays cold");
        }
    } else {
        require(wait([&] { return nativeReady(); }),"the private forecast fills the native cue window");
    }
    if (manual || stop) {
        through(noGrid ? due : due-blockSize);
        const auto request = manual ? LaunchRequest{LaunchRequestKind::SlotPress,1,30,103,incarnation}
            : LaunchRequest{LaunchRequestKind::StopTrack,1,{},{},incarnation};
        require(driver.post(std::array{request}),"post a same-sample user action against a warmed Follow");
        if (!noGrid) {
            through(due-17);
            if (manual) require(wait([&]{return manualGate.blocked.load(std::memory_order_acquire);}),
                                "the coincident manual source is cold");
        }
    }
    through(due);
    const auto heard = due+delay;
    through(heard);
    through(heard+dueFrames);
    require(driver.readStatus(rows,status) && rows[0].queued.action == LaunchQueuedAction::None,
            "no gesture remains queued after the due callback");
    if (warm) {
        std::array<float,blockSize> left{}, right{};
        float* outputs[]{left.data(),right.data()};
        AudioIo io{outputs,nullptr,2,0,dueFrames,due};
        auto previous = placement;
        std::uint32_t renderedWraps = 0;
        for (const auto wrap : wraps) {
            if (wrap.frame >= due+dueFrames) break;
            renderLaunchAudio(io,static_cast<std::int32_t>(previous.frame-due),
                static_cast<std::int32_t>(wrap.frame-previous.frame),resident->clip(102),0,1,-1,previous);
            previous = wrap;
            ++renderedWraps;
        }
        const auto finalOffset = static_cast<std::int32_t>(previous.frame-due);
        renderLaunchAudio(io,finalOffset,dueFrames-finalOffset,resident->clip(102),0,1,-1,previous);
        require(rows[0].completedLoops == renderedWraps,"the accepted Link crosses each independently projected wrap");
        require(output.count == 2 && output.edges[1].newClip == 102 && output.edges[1].sample == due &&
                output.edges[1].tick == dueTick && output.edges[1].sourceTick == nativePhase &&
                output.left == left && output.right == right && left.front() < -.1f && right.front() > .1f,
                "the warmed native Follow exactly matches the two independent resident channels");
    } else if (stop) {
        require(output.count == 2 && output.edges[1].newClip == 0 && output.edges[1].sample == due &&
                std::ranges::all_of(output.left,[](float x) { return x == 0; }),
                "the same-sample stop wins and clears every output sample");
    } else {
        const auto count = static_cast<std::size_t>(dueFrames);
        const auto left = std::span(output.left).first(count);
        const auto right = std::span(output.right).first(count);
        const auto oldSample = [](float x) { return std::abs(x-.5f) < .0005f; };
        auto oldPcm = std::ranges::all_of(left,oldSample) && std::ranges::all_of(right,oldSample);
        if (previousPages) {
            const audio::CallbackScope scope;
            oldPcm = previousPages->read(101,loopHead+elapsed,dueFrames,false) && left.front() > .1f &&
                std::ranges::equal(left,previousPages->channel(0)) && std::ranges::equal(right,previousPages->channel(1));
        }
        require(output.count == 1 && rows[0].run == oldRun && oldPcm,
                "a cold Follow or tied cold manual keeps every sample of the outgoing run");
    }
    nativeGate.resume.store(true,std::memory_order_release);
    manualGate.resume.store(true,std::memory_order_release);
    if (cold) require(wait([&]{ return nativeReady(); }),"the missed cue warms without replaying");
    through(heard+2*blockSize);
    require(output.count == (cold || manual ? 1u : 2u) && pages.underrunSamples() == 0 &&
            (!previousPages || previousPages->underrunSamples() == 0) &&
            (!manualPages || manualPages->underrunSamples() == 0) && allocations == 0 && audio::callbackFileIo == 0,
            "the consumed action never replays and native page admission keeps the callback clean");
    std::printf("PASS -- native Linked %s source %d case %d, wraps %d, due slice %d, cut %lld, anchor %lld, phase %lld, delay %d\n",
        noGrid ? "None" : "Quarter",static_cast<int>(outgoing),static_cast<int>(testCase),wrapCount,dueFrames,
        static_cast<long long>(due),static_cast<long long>(dueTick),static_cast<long long>(nativePhase),delay);
}

} // namespace

int main() {
    try {
        for (const auto noGrid : {false,true})
            for (const auto testCase : {NativeFollowCase::Warm,NativeFollowCase::Cold,NativeFollowCase::Manual,NativeFollowCase::Stop})
                nativeLinkedLegato(noGrid,testCase);
        for (const auto noGrid : {false,true})
            for (const auto wrapCount : {0,1,2,3,4})
                for (const auto testCase : {NativeFollowCase::Warm,NativeFollowCase::Cold})
                    nativeLinkedLegato(noGrid,testCase,wrapCount);
        nativeLinkedLegato(false,NativeFollowCase::Cold,4,38);
        for (const auto outgoing : {NativeFollowSource::Native,NativeFollowSource::AlignedR3})
            for (const auto noGrid : {false,true})
                for (const auto testCase : {NativeFollowCase::Warm,NativeFollowCase::Cold})
                    nativeLinkedLegato(noGrid,testCase,-1,blockSize,outgoing);
        Fixture fixture;
        persisted(fixture);
        ramModes(fixture);
        tempoSceneFromArrangementLoop(false);
        tempoSceneFromArrangementLoop(true);
        Fixture legato;
        pagedLegato(legato);
        for (const auto mode : {"beats","texture","re-pitch"}) nativePagedLegato(mode);
        nativePagedLegato("re-pitch",true);
        nativePagedLegato("texture",true);
        repeatedBeatsLegato();
        for (const auto mode : {"beats","texture","re-pitch"}) nativeManualLegato(mode);
        for (const auto mode : {"re-pitch","texture","beats"}) nativeManualLegato(mode,NativeLegatoGesture::HeldImmediate);
        nativeManualLegato("re-pitch",NativeLegatoGesture::HeldQuantized);
        nativeManualLegato("re-pitch",NativeLegatoGesture::HeldQuantizedAfterWrap);
        nativeManualLegato("re-pitch",NativeLegatoGesture::HeldDeferred);
        for (const auto mode : {"beats","texture","re-pitch"}) nativeManualLegato(mode,NativeLegatoGesture::HeldImmediateDeferred);
        nativeManualLegato("re-pitch",NativeLegatoGesture::HeldTwoGridResets);
        nativeManualLegato("re-pitch",NativeLegatoGesture::HeldThreeGridResets);
        nativeManualLegato("re-pitch",NativeLegatoGesture::HeldFourGridResets);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
