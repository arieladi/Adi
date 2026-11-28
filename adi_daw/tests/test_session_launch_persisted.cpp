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

void nativePagedLegato(std::string_view mode) {
    constexpr std::uint32_t rate = 48'000;
    constexpr std::int64_t targetId = 106, targetScene = 60;
    constexpr std::int64_t coldDue = 24'000, warmDue = 48'000;
    constexpr auto ppq = textproj::kPPQ;
    constexpr auto coldPage = 48'000 / 8192;
    constexpr std::uint64_t incarnation = 89;
    Fixture fixture;
    {
        constexpr std::uint32_t frames = 384'000;
        std::vector<float> waveform(frames);
        for (std::size_t index = 0; index < waveform.size(); ++index)
            waveform[index] = .125f + static_cast<float>(index % 9323) / 32768.f;
        audio::WavWriter wave(fixture.directory.path() / "native-legato.wav",rate,1,audio::WavFormat::Float32);
        wave.write(waveform.data(),frames);
        wave.close();
        const auto markers = audio::writeWarpMap(std::array{WarpMarker{0,0},WarpMarker{frames,16*ppq}},frames);
        require(static_cast<bool>(markers),"encode the long native Warp markers");
        auto& db = fixture.store->db();
        db.exec("INSERT INTO scenes(id,ord,name) VALUES(60,9,'Native Legato'),(61,10,'Tempo cue');"
                "UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=61;"
                "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(6,'native-legato','native-legato.wav','wav');"
                "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,content_offset_ticks)"
                " VALUES(106,1,'audio',0,86486400,5765760);"
                "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,60,106);"
                "INSERT INTO clip_launch(clip_id,legato,ram_mode) VALUES(106,1,0)");
        SQLite::Statement row(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers)"
                                 " VALUES(106,6,384000,1,?,?)");
        row.bind(1,std::string(mode));
        row.bind(2,markers.bytes.data(),static_cast<int>(markers.bytes.size()));
        row.exec();
    }

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
    TempoMap clock;
    clock.events.push_back({0,120,0});
    const auto prepare = [&] {
        const auto model = rows::readModel(*fixture.store);
        const auto project = readClipProject(*fixture.store,model,true);
        std::vector<std::string> problems;
        auto bank = prepareStoredLaunchProject(*project,clock,rate,2,blockSize,incarnation,problems);
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

    post(10,101);
    render();
    require(output.count == 1 && output.edges[0].newClip == 101,"start the outgoing resident source");
    post(targetScene,targetId);
    render();
    require(wait([&] { return gate.held.load(std::memory_order_acquire); }),
            "the real worker asks for the exact far cue and is held before claiming its page");
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
        if (!target->reserveWarpCue({warmDue,2*ppq,3*ppq},blockSize)) return false;
        pages.releaseCue();
        return true;
    }),"the second scheduled forecast fills its exact native pages");
    while (transport.position() <= warmDue) render();
    const auto offset = static_cast<std::int32_t>(warmDue - (transport.position() - blockSize));
    require(output.count == 2 && output.edges[1].newClip == targetId &&
            output.edges[1].sample == warmDue && output.edges[1].sourceTick == 3*ppq &&
            output.edges[1].offset == offset,
            "the warmed cue changes run at the exact sample and trimmed musical phase");
    std::array<float,blockSize> expectedLeft{}, expectedRight{};
    float* channels[]{expectedLeft.data(),expectedRight.data()};
    const AudioIo expected{channels,nullptr,2,0,blockSize,0};
    renderLaunchAudio(expected,offset,blockSize-offset,reference,0,1.0f,-1,{warmDue,2*ppq,3*ppq});
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
    std::printf("PASS -- native %.*s refused the cold cue at %lld and matched every warm sample at %lld\n",
                static_cast<int>(mode.size()),mode.data(),
                static_cast<long long>(coldDue),static_cast<long long>(warmDue));
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
        Fixture legato;
        pagedLegato(legato);
        for (const auto mode : {"beats","texture","re-pitch"}) nativePagedLegato(mode);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
