// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/launch_audio.hpp"
#include "adi/engine/launch_audio_runtime.hpp"
#include "adi/engine/launch_paged_audio.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/session.hpp"
#include "adi/media/blake3.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace { unsigned allocations = 0; }
void adi::test::countReplacedNew() noexcept { if (adi::audio::onAudioThread) ++allocations; }

namespace {
using namespace adi;
using namespace adi::engine;
constexpr std::uint32_t rate = 48000;
constexpr std::int32_t blockFrames = 257;
constexpr std::int64_t quarter = textproj::kPPQ;
constexpr std::int64_t sceneFrame = 2 * rate;
constexpr std::int64_t toneOffset = 6000;
constexpr std::int64_t dryOffset = 1500;
constexpr std::int64_t legatoOffset = 11400;
constexpr std::int64_t legatoSource = 2 * quarter / 5;

void require(bool pass, const char* message) {
    if (!pass) throw std::runtime_error(message);
}

struct Pulse { std::int64_t frame; float level; };

void writeSource(Store& store, int id, std::uint32_t frames, std::size_t channel, std::span<const Pulse> pulses) {
    const auto name = std::to_string(id) + ".wav";
    const auto path = store.path().parent_path() / name;
    std::vector<float> pcm(static_cast<std::size_t>(frames) * 2);
    constexpr int halfWidth = 96;
    for (const auto pulse : pulses) {
        for (int offset = -halfWidth; offset <= halfWidth; ++offset) {
            const auto frame = static_cast<std::size_t>(pulse.frame + offset);
            const auto window = static_cast<float>(0.5 * (1 + std::cos(std::numbers::pi * offset / halfWidth)));
            pcm[frame * 2 + channel] = pulse.level * window;
        }
    }
    audio::WavWriter writer(path, rate, 2, audio::WavFormat::Float32);
    writer.write(pcm.data(), frames);
    writer.close();
    const auto hash = media::blake3File(path);
    require(static_cast<bool>(hash), "hash worker R3 slot source");
    SQLite::Statement media(store.db(), "INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) "
                                         "VALUES(?,?,?,'wav',48000,2,?)");
    media.bind(1,id); media.bind(2,hash.hex); media.bind(3,name); media.bind(4,frames); media.exec();
}

void addSlot(Store& store, int clip, int scene, float level, std::int64_t duration,
             std::int64_t pulseOffset, bool legato, std::span<const Pulse> extra = {}) {
    const auto frames = static_cast<std::uint32_t>(duration * (rate / 2) / quarter);
    std::vector<Pulse> pulses{{pulseOffset,level}};
    pulses.insert(pulses.end(),extra.begin(),extra.end());
    writeSource(store,clip,frames,0,pulses);
    auto& db = store.db();
    SQLite::Statement placement(db, "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,loop_enabled,loop_len_ticks) "
                                    "VALUES(?,1,'audio',0,?,1,?)");
    placement.bind(1,clip); placement.bind(2,duration); placement.bind(3,duration); placement.exec();
    const auto markers = audio::writeWarpMap(std::array<WarpMarker,2>{{{0,0},{frames,duration}}},frames);
    require(static_cast<bool>(markers), "encode worker R3 markers");
    SQLite::Statement audioClip(db, "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) "
                                    "VALUES(?,?,?,1,'complex',?)");
    audioClip.bind(1,clip); audioClip.bind(2,clip); audioClip.bind(3,frames);
    audioClip.bind(4,markers.bytes.data(),static_cast<int>(markers.bytes.size())); audioClip.exec();
    SQLite::Statement cell(db, "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,?,?)");
    cell.bind(1,scene); cell.bind(2,clip); cell.exec();
    SQLite::Statement launch(db, "INSERT INTO clip_launch(clip_id,legato,ram_mode,clip_fade) VALUES(?,?,0,0)");
    launch.bind(1,clip); launch.bind(2,legato ? 1 : 0); launch.exec();
}

bool launchScene(Store& store, const Session& session, int scene) {
    OpRequest request;
    request.opType = "session.launchScene";
    request.payload = {{"scene",scene},{"incarnation",session.launchIncarnation()}};
    return OpJournal(store).commit(request).ok;
}

struct Playback {
    Session& session;
    std::array<std::vector<float>,2> pcm;
    std::int64_t first = 0, performedAt = -1;
    std::int64_t enteredPhase = -1, rawStoppedAt = -1;
    double performedBpm = 0;

    void through(std::int64_t end) {
        std::array<float,blockFrames> left{}, right{};
        float* channels[]{left.data(),right.data()};
        while (first < end) {
            if (session.transport().playing())
                require(session.clips()->prime(std::chrono::seconds(5)),"prime parallel dry PCM off the callback");
            const auto frames = static_cast<std::int32_t>(std::min<std::int64_t>(blockFrames,end-first));
            AudioIo io{channels,nullptr,2,0,frames,first};
            bool rendered;
            {
                audio::CallbackScope callback;
                auto block = session.beginRender();
                block.observeLaunchTransitions(this,[](void* context,const LaunchTransition& edge,std::int64_t) noexcept {
                    auto& playback = *static_cast<Playback*>(context);
                    if (edge.newClip == 102) playback.enteredPhase = edge.sourceTick;
                    if (edge.oldClip == 102 && edge.newClip == 0) playback.rawStoppedAt = edge.sample;
                });
                block.observeSceneLaunches(this,[](void* context,const LaunchSceneTransition& scene,std::int64_t) noexcept {
                    auto& playback = *static_cast<Playback*>(context);
                    if (scene.scene == 20) {
                        playback.performedAt = scene.sample;
                        playback.performedBpm = scene.tempoBpm;
                    }
                });
                rendered = block.process(io);
            }
            require(rendered,"render R3 slot and parallel dry source through Session");
            std::copy_n(left.begin(),frames,pcm[0].begin()+first);
            std::copy_n(right.begin(),frames,pcm[1].begin()+first);
            first += frames;
        }
    }
};

enum class SceneCase { Restart, Legato, LegatoColdAfterRetime };

void workerTempoScene(SceneCase variant) {
    const auto useLegato = variant != SceneCase::Restart;
    const auto expectRefusal = variant == SceneCase::LegatoColdAfterRetime;
    test::TempDirectory folder{"session_r3_worker","tempo scene"};
    StoreError error{};
    auto store = Store::createEmpty(folder.path()/"r3_slot.adi",error);
    require(static_cast<bool>(store),"create worker R3 project");
    test::applySessionSchema(store->db());
    auto& db = store->db();
    db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Worker R3 slots',48000);"
            "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
            "(1,'audio','R3 slots',0),(2,'audio','Dry',1),(9,'master','Main',2);"
            "INSERT INTO scenes(id,ord) VALUES(10,0),(20,1);"
            "UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=20;"
            "INSERT INTO clip_slots(track_id,scene_id,has_stop) VALUES(2,10,0),(2,20,0)");
    if (expectRefusal) db.exec("UPDATE scenes SET tempo_bpm=90 WHERE id=20");
    addSlot(*store,101,10,0.65f,useLegato ? 3 * quarter / 5 : quarter / 2,toneOffset,false);
    addSlot(*store,102,20,-0.78f,useLegato ? 7 * quarter / 12 : quarter / 2,
            useLegato ? legatoOffset : toneOffset,useLegato);
    constexpr std::uint32_t dryFrames = 130000;
    // Separate the dry landmarks from the R3 transients so the stereo
    // analysis of a left-only clip cannot move the dry peak measurement.
    const auto dryAfter = sceneFrame + (useLegato ? toneOffset : dryOffset);
    writeSource(*store,103,dryFrames,1,std::array{Pulse{dryOffset,0.5f},Pulse{dryAfter,0.5f}});
    SQLite::Statement dry(db,"INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(103,2,'audio',0,?)");
    dry.bind(1,8*quarter); dry.exec();
    SQLite::Statement audioDry(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(103,103,?)");
    audioDry.bind(1,dryFrames); audioDry.exec();

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{},SessionSpec{2,rate,blockFrames})) throw std::runtime_error(session.error());
    const auto delay = session.graph().currentGraph()->latencySamples();
    std::printf("Complex paged slot graph delay %d samples at %u Hz\n",delay,rate);
    require(delay == 2048,"a supported Complex slot declares the worker stretcher latency");
    constexpr std::int64_t finish = sceneFrame+12000;
    const auto frames = static_cast<std::size_t>(finish);
    Playback playback{session,{std::vector<float>(frames),std::vector<float>(frames)}};
    require(launchScene(*store,session,10),"launch initial persisted R3 scene");
    playback.through(blockFrames);
    const auto admitted = expectRefusal ? session.prepareSceneLaunch(*store,20,4*quarter)
                                        : launchScene(*store,session,20);
    if (expectRefusal) {
        // At 120 BPM the 14,000-frame target fits the window. At 90 BPM it
        // needs 18,667 frames, so the new bank must refuse before publication.
        require(!admitted && session.error().find("complete bounded playback window") != std::string::npos,
                "the retimed cold Legato target is refused with its readiness reason");
        playback.through(finish);
        require(playback.performedAt < 0 && playback.enteredPhase < 0 &&
                session.beginRender().bpmAtTick(4*quarter) == 120,
                "refusing the cold candidate leaves the old running launch and tempo intact");
        std::printf("Cold 90 BPM target needs 18667 frames; old 120 BPM scene kept playing\n");
        return;
    }
    if (!admitted) throw std::runtime_error("launch persisted tempo scene: " + session.error());
    playback.through(finish);
    require(playback.performedAt == sceneFrame && playback.performedBpm == 140,
            "the new tempo performs on the exact saved scene boundary");
    const auto expectedPhase = useLegato ? legatoSource : 0;
    require(std::abs(playback.enteredPhase-expectedPhase) <= quarter/(rate/2)+1,
            "worker R3 enters at the driver-selected source phase");
    require(session.graph().currentGraph()->latencySamples() == delay,"slot delay stays stable through the tempo scene");

    const auto peak = [&](std::size_t channel,std::int64_t frame,bool negative) {
        const auto& signal = playback.pcm[channel];
        const auto begin = signal.begin()+frame-256, end = signal.begin()+frame+257;
        const auto best = negative ? std::min_element(begin,end) : std::max_element(begin,end);
        if (std::abs(*best) < 0.4f)
            std::fprintf(stderr,"channel %zu near frame %lld has peak %.6f FS\n",channel,
                         static_cast<long long>(frame),static_cast<double>(*best));
        require(std::abs(*best) >= 0.4f,"the R3 slot and dry landmarks remain audible");
        return static_cast<std::int64_t>(best-signal.begin())-frame;
    };
    const auto before = peak(0,toneOffset+delay,false);
    const auto stepOffset = useLegato ? legatoOffset-legatoSource*(rate/2)/quarter : toneOffset;
    const auto fasterToneOffset = (stepOffset * 120 + 70) / 140;
    const auto after = peak(0,sceneFrame+fasterToneOffset+delay,true);
    require(std::abs(before) <= 160 && std::abs(after) <= 160,"R3 slot landmarks follow both scene clocks");
    require(peak(1,dryOffset+delay,false) == 0 && peak(1,dryAfter+delay,false) == 0,
            "parallel dry audio aligns to the reported slot delay exactly once");
    require(db.execAndGet("SELECT bpm FROM tempo_map WHERE pos_ticks=0").getDouble() == 120,
            "performing the tempo scene leaves the authored clock intact");
    require(session.problems().empty() && session.clips()->underrunSamples() == 0 &&
            allocations == 0 && audio::callbackFileIo == 0,
            "the worker-backed scene has no source problems, callback allocation or file I/O");
    std::printf("Complex %s scene at %lld, inherited %lld ticks, 120 to 140 BPM peak errors %lld/%lld samples, "
                "dry error 0, callback allocations %u\n",useLegato ? "Legato" : "restart",
                static_cast<long long>(playback.performedAt),static_cast<long long>(playback.enteredPhase),
                static_cast<long long>(before),static_cast<long long>(after),allocations);
}
enum class ExternalCase { Active, Idle, StartsDuringPreparation };

void workerExternalTempo(ExternalCase variant) {
    const bool active = variant == ExternalCase::Active;
    const bool late = variant == ExternalCase::StartsDuringPreparation;
    test::TempDirectory folder{"session_r3_worker","external tempo"};
    StoreError error{};
    auto store = Store::createEmpty(folder.path()/"r3_external.adi",error);
    require(static_cast<bool>(store),"create the external-tempo R3 project");
    test::applySessionSchema(store->db());
    auto& db = store->db();
    db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'External Worker R3',48000);"
            "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
            "(1,'audio','R3 slots',0),(9,'master','Main',1);"
            "INSERT INTO scenes(id,ord) VALUES(10,0);"
            "INSERT INTO session_state(key,value) VALUES('transport.launchQuantization','0')");
    addSlot(*store,101,10,0.65f,quarter/2,toneOffset,false);

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{},SessionSpec{2,rate,blockFrames})) throw std::runtime_error(session.error());
    const auto delay = session.graph().currentGraph()->latencySamples();
    require(delay == 2048,"the persisted Complex slot starts with worker PDC");
    constexpr std::int64_t finish = 40000;
    const auto frames = static_cast<std::size_t>(finish);
    Playback playback{session,{std::vector<float>(frames),std::vector<float>(frames)}};
    if (active) require(launchScene(*store,session,10),"launch the persisted R3 slot");
    else session.transport().play();
    playback.through(blockFrames);
    std::array<LaunchTrackStatus,2> tracks{};
    LaunchStatusHeader status;
    require(session.readLaunchStatus(tracks,status) && status.trackCount == 1 &&
        (active ? tracks[0].clip == 101 && tracks[0].run != 0 : tracks[0].clip == 0),
        "observe the driver-owned source before preparing the new clock");
    auto run = tracks[0].run;
    const bool accepted = session.setExternalTempoAt(*store,140,quarter/2,[&] {
        if (!late) return true;
        // Let the old driver start the slot after owner readiness has passed
        // and before publishing the external tempo candidate.
        require(launchScene(*store,session,10),"start the slot during preparation");
        playback.through(2 * blockFrames);
        require(session.readLaunchStatus(tracks,status) && tracks[0].clip == 101 && tracks[0].run != 0,
            "observe the newly started driver-owned R3 run");
        run = tracks[0].run;
        return true;
    });
    const auto diagnostic = session.error();
    playback.through(finish);
    if (active || late) {
        const auto later = 12000 + toneOffset + (late ? blockFrames : 0);
        const auto nearest = playback.pcm[0].begin()+later+delay;
        const auto peak = *std::max_element(nearest-256,nearest+257);
        std::printf("Active R3 external prepare %s, later peak %.6f, BPM %.0f\n",accepted ? "accepted" : "refused",
                    static_cast<double>(peak),session.beginRender().bpmAtTick(quarter/2));
        if (late) require(accepted,"the owner observed the idle old bank before the slot started");
        else require(!accepted && diagnostic.find("Session clips#101") != std::string::npos,
            "name the unsupported active R3 slot before publishing external tempo");
        require(session.readLaunchStatus(tracks,status) && tracks[0].run == run && peak > 0.4f &&
            session.beginRender().bpmAtTick(quarter/2) == 120 && session.beginRender().latencySamples() == delay,
            "keep the old run, clock and audible compensated loop until adoption is safe");
        if (late) {
            session.transport().play(false);
            require(session.beginRender().bpmAtTick(quarter/2) == 140,
                "adopt the held clock after stopping the protected R3 run");
        }
    } else {
        require(accepted && session.beginRender().bpmAtTick(quarter/2) == 140,
            "an idle fixed slot does not block the ordinary external tempo path");
    }
}
void preparedWarpToRawScene(bool loop) {
    constexpr auto oldElapsed = std::int64_t{6000};
    constexpr auto rawStart = std::int64_t{4800};
    constexpr auto rawCue = rawStart + oldElapsed;
    constexpr auto oldLaunch = sceneFrame - oldElapsed;
    constexpr auto loopBegin = 9600, loopEnd = 21600, loopLength = loopEnd - loopBegin;
    constexpr auto firstWrap = sceneFrame + loopEnd - rawCue;
    constexpr auto linkedStop = firstWrap + 2*loopLength;
    constexpr auto dryBefore = sceneFrame - 600, dryAfter = sceneFrame + 1200;
    constexpr auto finish = sceneFrame + 48000 + 4096;
    const auto render = [&](bool tempo,bool fade,bool pending = false) {
        test::TempDirectory folder{"r3_to_raw_scene",fade ? "fade" : "no-fade"};
        StoreError error{};
        auto store = Store::createEmpty(folder.path()/"song.adi",error);
        require(static_cast<bool>(store),"create worker-to-raw scene project");
        test::applySessionSchema(store->db());
        auto& db = store->db();
        db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Worker Warp to raw',48000);"
                "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
                "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
                "(1,'audio','Warp and raw',0),(2,'audio','Dry',1),(9,'master','Main',2);"
                "INSERT INTO scenes(id,ord) VALUES(10,0),(20,1);"
                "INSERT INTO clip_slots(track_id,scene_id,has_stop) VALUES(2,10,0),(2,20,0)");
        if (tempo) db.exec("UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=20");
        addSlot(*store,101,10,.8f,2*quarter,oldElapsed+96,false);
        db.exec("UPDATE clips SET loop_enabled=0,loop_len_ticks=NULL WHERE id=101;"
                "UPDATE clip_launch SET launch_quant=1 WHERE clip_id=101");
        if (fade) db.exec("UPDATE clip_launch SET clip_fade=1 WHERE clip_id=101");

        writeSource(*store,102,60000,0,std::array{Pulse{rawCue,-.6f},Pulse{rawCue+12000,-.7f}});
        db.exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,loop_enabled)"
                "VALUES(102,1,'audio',1,0,1000000000,0);"
                "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled) VALUES(102,102,60000,0);"
                "INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns)"
                "VALUES(102,100000000,1100000000);"
                "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,20,102);"
                "INSERT INTO clip_launch(clip_id,legato,ram_mode,clip_fade) VALUES(102,1,0,0);");
        if (loop) {
            db.exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=2306304,loop_len_ticks=2882880 WHERE id=102;"
                    "UPDATE clip_playback_details SET loop_begin_ns=200000000,loop_end_ns=450000000 WHERE clip_id=102;"
                    "UPDATE clip_launch SET follow_enabled=1,follow_linked=1,follow_loop_count=3,follow_a=1 WHERE clip_id=102;");
        }

        constexpr auto dryFrames = 150000;
        writeSource(*store,103,dryFrames,1,std::array{Pulse{dryBefore,.5f},Pulse{dryAfter,.5f}});
        db.exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,loop_enabled)"
                "VALUES(103,2,'audio',1,0,3000000000,0);"
                "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled) VALUES(103,103,150000,0);");

        Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(*store,{},SessionSpec{2,rate,blockFrames})) throw std::runtime_error(session.error());
        const auto delay = session.graph().currentGraph()->latencySamples();
        require(delay == 2048,"the outgoing Complex worker declares 2048 frames");
        session.transport().play();
        Playback playback{session,{std::vector<float>(finish),std::vector<float>(finish)}};
        playback.through(oldLaunch);
        OpRequest start;
        start.opType="session.launchClip";
        start.payload={{"track",1},{"scene",10},{"clip",101},{"incarnation",session.launchIncarnation()}};
        require(OpJournal(*store).commit(start).ok,"launch the persisted outgoing worker Warp");
        playback.through(sceneFrame-3*blockFrames);
        if (pending) {
            require(session.refresh(*store),"publish a fresh replacement for the outgoing prepared reader");
            require(session.graph().currentTransportState() != session.graph().adoptedTransportState(),
                    "the replacement reader is not adopted");
        }
        const auto* before = session.graph().currentTransportState();
        require(launchScene(*store,session,20),"queue the raw Legato scene");
        if (pending) {
            require(session.error().find("prepared Warp scene waits for the adopted outgoing bank") != std::string::npos &&
                    session.graph().currentTransportState() == before,"the scene refuses a fresh prepared replacement bank");
            return playback.pcm;
        }
        playback.through(sceneFrame+1);
        LaunchStatusHeader status{};
        std::array<LaunchTrackStatus,2> rows{};
        require(session.readLaunchStatus(rows,status),"read the driver-selected run");
        const auto incoming = rows[0];
        bool exactLoop = true;
        if (loop) {
            playback.through(firstWrap);
            require(session.readLaunchStatus(rows,status),"read the partial lap up to its boundary");
            exactLoop &= rows[0].run == incoming.run && rows[0].completedLoops == 0;
            playback.through(firstWrap+1);
            require(session.readLaunchStatus(rows,status),"read the first physical wrap");
            exactLoop &= rows[0].run == incoming.run && rows[0].completedLoops == 1;
            playback.through(firstWrap+loopLength+1);
            require(session.readLaunchStatus(rows,status),"read the second physical wrap");
            exactLoop &= rows[0].run == incoming.run && rows[0].completedLoops == 2;
        }
        playback.through(finish);
        const auto& left=playback.pcm[0];
        const auto& right=playback.pcm[1];
        const auto heardHead=static_cast<std::size_t>(sceneFrame+delay);
        if (loop) {
            audio::WavReader original(folder.path() / "102.wav");
            std::vector<float> raw(static_cast<std::size_t>(2*loopEnd));
            require(original.read(raw.data(),loopEnd) == loopEnd,"read saved stereo loop PCM");
            auto source = rawCue;
            const auto fadeEnd = heardHead + static_cast<std::size_t>(fade ? launchFadeFrames(rate) : 0);
            const auto delayedStop = static_cast<std::size_t>(linkedStop+delay);
            for (auto frame=heardHead; frame<delayedStop; ++frame) {
                if (frame >= fadeEnd)
                    exactLoop &= left[frame] == raw[static_cast<std::size_t>(2*source)];
                if (++source == loopEnd) source = loopBegin;
            }
            exactLoop &= playback.rawStoppedAt == linkedStop &&
                std::all_of(left.begin()+linkedStop+delay,left.end(),[](float sample) { return sample == 0; });
            std::printf("Prepared Warp to raw loop: first wrap %lld, linked Stop %lld/%lld, exact PCM %d\n",
                static_cast<long long>(firstWrap),static_cast<long long>(playback.rawStoppedAt),
                static_cast<long long>(linkedStop),exactLoop);
        }
        const auto rightPeak = [&](std::int64_t at) {
            const auto peak=std::max_element(right.begin()+at-96,right.begin()+at+97);
            const auto position=static_cast<std::int64_t>(peak-right.begin());
            if (std::abs(*peak - .5f) > 1e-4f || position != at)
                std::printf("    dry expected %lld, peak %lld at %.8f, exact sample %.8f\n",
                    static_cast<long long>(at),static_cast<long long>(position),static_cast<double>(*peak),
                    static_cast<double>(right[static_cast<std::size_t>(at)]));
            return std::abs(*peak - .5f) <= 1e-4f && position == at;
        };
        std::printf("R3 to raw tempo=%d fade=%d: scene %lld/%g, phase %lld, graph delay %d, dry aligned %d/%d, raw head %.6f\n",
            tempo,fade,static_cast<long long>(playback.performedAt),playback.performedBpm,
            static_cast<long long>(playback.enteredPhase),session.graph().currentGraph()->latencySamples(),
            rightPeak(dryBefore+delay),rightPeak(dryAfter+delay),static_cast<double>(left[heardHead]));
        if (!session.error().empty()) std::printf("  %s\n",session.error().c_str());
        for (const auto& problem : session.problems()) std::printf("  %s\n",problem.c_str());
        std::printf("    active %lld, raw later %.8f, allocations %u, I/O %u\n",
            static_cast<long long>(rows[0].clip),static_cast<double>(left[heardHead+12000]),
            allocations,audio::callbackFileIo.load());
        require(playback.performedAt == sceneFrame && (tempo ? playback.performedBpm == 140 :
                    session.beginRender().bpmAtTick(4*quarter) == 120) &&
                playback.enteredPhase == rawCue*quarter/(rate/2) && incoming.clip == 102 &&
                session.graph().currentGraph()->latencySamples() == delay &&
                rightPeak(dryBefore+delay) && rightPeak(dryAfter+delay) &&
                (loop ? exactLoop : left[heardHead+12000] == -.7f) &&
                session.problems().empty() && audio::callbackFileIo == 0 && allocations == 0,
                "the worker exits at the scene and its raw replacement remains PDC aligned");
        if (!fade) require(left[heardHead] == -.6f,"the raw attack follows one physical graph delay");
        return playback.pcm;
    };

    const auto steadyFade=render(false,true);
    const auto tempoFade=render(true,true);
    const auto steadyDry=render(false,false);
    const auto tempoDry=render(true,false);
    require(steadyFade == tempoFade && steadyDry == tempoDry,
            "the outgoing old bank and its bounded fade match the unchanged clock exactly");
    constexpr auto delay = 2048;
    const auto from = sceneFrame+delay;
    const auto to = from+launchFadeFrames(rate);
    float greatest = 0;
    auto leaked = false;
    for (auto frame = sceneFrame; frame < finish; ++frame) {
        const auto delta = std::abs(tempoFade[0][static_cast<std::size_t>(frame)]-tempoDry[0][static_cast<std::size_t>(frame)]);
        if (frame < from || frame >= to) leaked |= delta != 0;
        else greatest=std::max(greatest,delta);
    }
    std::printf("Old prepared R3 fade: largest residual %.6f only in physical frames [%lld,%lld)\n",
        static_cast<double>(greatest),static_cast<long long>(from),static_cast<long long>(to));
    require(greatest > .05f && !leaked,"only the finite old before-bank fade follows the logical cut");
    (void)render(true,true,true);
}

enum class WorkerCueCase {
    Warm, ColdCue, WarmWrap, ColdWrap, ColdDistantHead, RepeatWrap,
    ColdRepeatStart, ColdLaterStart, ReleasedBeforeCut,
};

void workerLegato(WorkerCueCase testCase, bool immediate = false) {
    const auto coldStart = testCase == WorkerCueCase::ColdRepeatStart || testCase == WorkerCueCase::ColdLaterStart ||
        testCase == WorkerCueCase::ReleasedBeforeCut;
    const auto repeat = testCase == WorkerCueCase::RepeatWrap || coldStart;
    const auto restarted = testCase == WorkerCueCase::RepeatWrap;
    const auto laterRestart = testCase == WorkerCueCase::ColdLaterStart;
    const auto heldImmediate = immediate && repeat;
    const auto loop = testCase == WorkerCueCase::WarmWrap || testCase == WorkerCueCase::ColdWrap ||
        testCase == WorkerCueCase::ColdDistantHead || restarted;
    const auto coldHead = testCase == WorkerCueCase::ColdWrap || testCase == WorkerCueCase::ColdDistantHead;
    const auto refused = testCase == WorkerCueCase::ColdCue || testCase == WorkerCueCase::ColdWrap ||
        testCase == WorkerCueCase::ColdRepeatStart;
    const auto immediateWrap = loop && testCase != WorkerCueCase::ColdDistantHead;
    constexpr auto due = 144000;
    constexpr auto repeatPeriod = static_cast<int>(rate) / 16, maxFrames = 4096;
    constexpr auto onset = due + 140;
    constexpr auto page = 8192, nearTail = 37, prefix = 17;
    const auto blockSize = repeat ? maxFrames : blockFrames;
    const auto firstSlice = (laterRestart || heldImmediate ? blockFrames : blockSize) - prefix;
    const auto initialAge = heldImmediate ? repeatPeriod : due;
    const auto restartDelay = heldImmediate ? nearTail : repeatPeriod;
    const auto wrapsBeforeRestart = immediateWrap && !heldImmediate;
    const auto savedStart = repeat ? 8*page-1 : 3455;
    const auto cue = savedStart + initialAge;
    const auto loopHead = heldImmediate ? savedStart-1000 : repeat ? 4*page-1 : page-1;
    const auto nextAt = due+restartDelay;
    const auto releaseWrap = nextAt+initialAge+nearTail;
    constexpr auto extent = 10 * quarter;
    constexpr std::uint64_t incarnation = 111;
    test::TempDirectory folder{"scheduled_r3_legato",refused ? "cold" : "warm"};
    StoreError error{};
    auto store = Store::createEmpty(folder.path()/"song.adi",error);
    require(static_cast<bool>(store),"create the scheduled worker Legato project");
    test::applySessionSchema(store->db());
    auto& db = store->db();
    db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Scheduled worker Legato',48000);"
            "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Slots',0),(9,'master','Main',1);"
            "INSERT INTO scenes(id,ord) VALUES(10,0),(20,1)");
    addSlot(*store,101,10,.55f,extent,initialAge+140,false);
    const std::array restartAttack{Pulse{savedStart+120,-.65f},Pulse{loopHead+300,.7f},
                                   Pulse{savedStart+repeatPeriod+120,.7f}};
    addSlot(*store,102,20,-.8f,extent,cue+(immediateWrap ? 20 : 40),true,
            repeat ? std::span<const Pulse>{restartAttack} : std::span<const Pulse>{});
    db.exec("UPDATE clips SET loop_enabled=0,loop_len_ticks=NULL;"
            "UPDATE audio_clips SET warp_enabled=0,warp_markers=NULL WHERE clip_id=101;"
            "UPDATE clip_launch SET ram_mode=1,launch_quant=1 WHERE clip_id=101;"
            "UPDATE clip_launch SET launch_quant=8 WHERE clip_id=102");
    if (repeat) db.exec("UPDATE clip_launch SET launch_mode=3,launch_quant=14 WHERE clip_id=102");
    if (immediate) db.exec("UPDATE clip_launch SET launch_quant=1 WHERE clip_id=102");
    const auto frameToTick = [](int frame) {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(frame)*quarter/(rate/2)));
    };
    const auto startTick = frameToTick(savedStart);
    {
        SQLite::Statement trim(db,"UPDATE clips SET content_offset_ticks=?,length_ticks=length_ticks-? WHERE id=102");
        trim.bind(1,startTick);
        trim.bind(2,startTick);
        trim.exec();
    }
    if (loop) {
        SQLite::Statement savedLoop(db,"UPDATE clips SET loop_enabled=1,loop_start_ticks=?,loop_len_ticks=? WHERE id=102");
        const auto head = frameToTick(loopHead);
        const auto end = startTick + frameToTick(initialAge) + frameToTick(immediateWrap ? nearTail : 10000);
        savedLoop.bind(1,head);
        savedLoop.bind(2,end-head);
        savedLoop.exec();
    } else if (heldImmediate) {
        SQLite::Statement savedEnd(db,"UPDATE clips SET length_ticks=? WHERE id=102");
        savedEnd.bind(1,frameToTick(initialAge+nearTail));
        savedEnd.exec();
    }
    store.reset();
    store = Store::open(folder.path()/"song.adi",error);
    require(static_cast<bool>(store),"reopen the saved long worker slot");
    struct Gate {
        std::int64_t heldPage, cueMarkerPage, evictWith;
        std::atomic<bool> armed{false}, blocked{false}, resume{false};
        std::atomic<bool> cueReady{false}, evicted{false}, headReady{false};
    } gate{heldImmediate ? 7 : coldStart ? 8 : coldHead ? 0 : 18,
           heldImmediate ? 1 : repeat ? 27 : 19, heldImmediate ? 0 : -1};
    const ClipPlayback::RequestSnapshotProbeForTest probe{.context=&gate,
        .beforePageClaim=[](void* opaque,std::int64_t,std::int64_t number) noexcept {
            auto& state = *static_cast<Gate*>(opaque);
            if (number == state.heldPage && state.armed.load(std::memory_order_acquire)) {
                state.blocked.store(true,std::memory_order_release);
                while (!state.resume.load(std::memory_order_acquire)) std::this_thread::yield();
            }
            if (number == state.cueMarkerPage) state.cueReady.store(true,std::memory_order_release);
            if (number == 1 && state.blocked.load(std::memory_order_acquire))
                state.headReady.store(true,std::memory_order_release);
        },
        .afterPageClaimAttempt=[](void* opaque,std::int64_t previous,std::int64_t number,bool claimed) noexcept {
            auto& state = *static_cast<Gate*>(opaque);
            if (claimed && previous == state.heldPage && (number == state.evictWith || number >= 17))
                state.evicted.store(true,std::memory_order_release);
        }};
    const auto project = readClipProject(*store,rows::readModel(*store),true);
    TempoMap clock;
    clock.events.push_back({0,120,0});
    std::vector<std::string> problems;
    const auto bank = prepareStoredLaunchProject(*project,clock,rate,2,blockSize,incarnation,problems);
    const auto* target = bank ? bank->clip(102) : nullptr;
    require(target && target->pageReader() && target->latencySamples() == 2048 &&
            !target->allWorkerPhasesPrimed() && problems.empty(),"prepare the long aligned worker bank");
    auto& pages = *target->pageReader();
    struct ResumeOnExit {
        Gate& gate;
        ~ResumeOnExit() { gate.resume.store(true,std::memory_order_release); }
    } resume{gate};
    pages.setRequestSnapshotProbeForTest(&probe);
    auto runtime = std::make_shared<LaunchAudioRuntime>(bank,std::vector<std::int64_t>{1},blockSize);
    LaunchAudioDriver driver(1);
    Transport transport;
    ClipPlayback track(project,transport,rate,2,blockSize);
    track.attachLaunchAudio(1,runtime);
    auto* source = track.sourcesFor(1).front();
    const auto delay = source->latencySamples();
    require(delay == 2048,"the actual track source declares worker latency");
    transport.play();
    const auto heardAttack = static_cast<std::size_t>(onset+delay);
    const auto finish = heldImmediate ? releaseWrap+delay+512
        : repeat ? due+2*repeatPeriod+delay+blockFrames : onset+delay+blockFrames;
    std::vector<float> pcm(static_cast<std::size_t>(finish));
    struct Events { std::array<LaunchTransition,5> edges{}; std::size_t count = 0; } events;
    const auto through = [&](std::int64_t end) {
        while (transport.position() < end) {
            LaunchBlock block;
            block.frames = static_cast<std::int32_t>(std::min<std::int64_t>(blockSize,end-transport.position()));
            block.clock = &clock;
            block.tickAtSample = [](const void* raw,std::int64_t frame) noexcept {
                return static_cast<const TempoMap*>(raw)->secondsToTicks(static_cast<double>(frame)/rate);
            };
            block.sampleAtTick = [](const void* raw,std::int64_t tick) noexcept -> std::int64_t {
                return std::llround(static_cast<const TempoMap*>(raw)->ticksToSeconds(tick)*rate);
            };
            std::array<float,maxFrames> right{};
            float* channels[]{pcm.data()+transport.position(),right.data()};
            NodeIo io;
            io.out = channels;
            io.channels = 2;
            io.frames = block.frames;
            io.sampleRate = rate;
            {
                audio::CallbackScope callback;
                runtime->begin(driver,block,transport,transport.position(),&events,
                    [](void* opaque,const LaunchTransition& edge,std::int64_t) noexcept {
                        auto& report = *static_cast<Events*>(opaque);
                        if (report.count < report.edges.size()) report.edges[report.count++] = edge;
                    });
                source->process(io);
                runtime->finish();
            }
            transport.advance(block.frames);
        }
    };
    const auto waitUntil = [](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            if (ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    const auto waitFor = [&](const std::atomic<bool>& reached) {
        return waitUntil([&]{ return reached.load(std::memory_order_acquire); });
    };
    const auto readReady = [&](LaunchPagedAudio& reader,int frame,std::span<float> destination) {
        {
            const audio::CallbackScope callback;
            reader.prefetch(102,frame);
        }
        const auto count = static_cast<std::int32_t>(destination.size());
        return waitUntil([&] {
            const audio::CallbackScope callback;
            if (!reader.reserveWorkerCue(102,frame,count,false)) return false;
            const auto ready = reader.read(102,frame,count,false);
            if (ready) std::ranges::copy(reader.channel(0),destination.begin());
            reader.releaseCue(102);
            return ready;
        });
    };
    if (coldHead || coldStart) {
        // A disjoint idle read evicts Start or Loop Begin while warming the cue.
        {
            const audio::CallbackScope callback;
            (void)pages.read(102,heldImmediate ? 0 : cue,1,false);
        }
        require(waitFor(gate.cueReady) && gate.evicted.load(std::memory_order_acquire),
                "prepare the cue while evicting its disjoint alternate phase");
    }
    const auto underrunsBefore = pages.underrunSamples();
    if (coldHead || coldStart || (!immediate && refused)) gate.armed.store(true,std::memory_order_release);
    through(due-initialAge);
    require(driver.post(std::array{LaunchRequest{LaunchRequestKind::SlotPress,1,10,101,incarnation}}),"start the saved dry slot");
    const auto pressAt = immediate ? due : due - (repeat ? 2000 : 12000);
    through(pressAt);
    std::array<LaunchTrackStatus,1> rows{};
    LaunchStatusHeader status{};
    require(driver.readStatus(rows,status) && rows[0].clip == 101,"read the outgoing run");
    const auto run = rows[0].run;
    const auto phase = startTick+frameToTick(initialAge);
    if (immediate && coldStart) {
        {
            const audio::CallbackScope callback;
            pages.prefetch(102,cue);
        }
        require(waitFor(gate.blocked),"warm the cue while leaving the first saved Start page cold");
    } else if (immediate && !refused) {
        std::array<float,blockFrames> prepared{};
        require(readReady(pages,heldImmediate ? savedStart : cue,prepared),"prepare the exact immediate cue before pressing");
    }
    require(driver.post(std::array{LaunchRequest{LaunchRequestKind::SlotPress,1,20,102,incarnation}}),"post the worker Legato cue");
    if (!immediate) {
        through(pressAt+blockFrames);
        require(driver.readStatus(rows,status) && rows[0].queued.legatoSourceTick == phase &&
                !rows[0].queued.legatoSourceFrame && target->frameForTick(phase,120) == cue,
                "forecast the same exact aligned PCM cue across the page boundary");
        require(waitFor(coldHead || coldStart || refused ? gate.blocked : gate.cueReady),
                "the forecast requested the cue or its disjoint alternate phase");
    }
    std::vector<float> expected(static_cast<std::size_t>(firstSlice));
    std::array<float,blockFrames> expectedLater{};
    std::array<float,512> expectedAfterRelease{};
    if (!refused) {
        const auto readReference = [&](int frame,std::span<float> destination) {
            const audio::CallbackScope callback;
            if (!pages.read(102,frame,static_cast<std::int32_t>(destination.size()),false)) return false;
            std::ranges::copy(pages.channel(0),destination.begin());
            return true;
        };
        std::span<float> oracle{expected};
        if (repeat || immediate) {
            const auto reference = prepareStoredLaunchProject(*project,clock,rate,2,blockSize,incarnation,problems);
            const auto* referenceClip = reference ? reference->clip(102) : nullptr;
            require(referenceClip && referenceClip->pageReader(),"prepare the separate aligned PCM reference");
            auto& reader = *referenceClip->pageReader();
            if (laterRestart) require(readReady(reader,savedStart,expectedLater),"read the separate later-grid Start");
            if (restarted) {
                auto restart = oracle.subspan(static_cast<std::size_t>(restartDelay));
                require(readReady(reader,savedStart,restart) && *std::min_element(restart.begin(),restart.end()) < -.5f,
                        "the saved page-crossing Start is audibly distinct");
                oracle = oracle.first(static_cast<std::size_t>(restartDelay));
            }
            if (heldImmediate) require(readReady(reader,loopHead,expectedAfterRelease),"read the ordinary loop head after Release");
            if (wrapsBeforeRestart) {
                auto head = oracle.subspan(nearTail);
                require(readReady(reader,loopHead,head) && *std::max_element(head.begin(),head.end()) > .5f,
                        "the disjoint loop head supplies the rest of the first lap");
                oracle = oracle.first(nearTail);
            }
            require(readReady(reader,cue,oracle),"read the independently prepared worker cue");
        } else {
            if (immediateWrap) {
                require(readReference(loopHead,oracle.subspan(nearTail)),"read the ready wrap head");
                oracle = oracle.first(nearTail);
            }
            require(readReference(cue,oracle),"read the ready worker cue");
        }
        require(*std::min_element(oracle.begin(),oracle.end()) < -.5f,"the inherited cue has audible worker PCM");
    }
    const auto release = [&] {
        require(driver.post(std::array{LaunchRequest{LaunchRequestKind::SlotRelease,1,20,102,incarnation}}),
                "release the held Repeat");
    };
    if (!immediate) through(due-prefix);
    if (testCase == WorkerCueCase::ReleasedBeforeCut) release();
    through(due+firstSlice);
    require(driver.readStatus(rows,status),"read the post-grid state");
    gate.resume.store(true,std::memory_order_release);
    if ((immediate && refused) || testCase == WorkerCueCase::ColdRepeatStart) {
        std::array<float,blockFrames> recovered{};
        require(readReady(pages,coldStart ? savedStart : cue,recovered),"the missing phase becomes ready after refusal");
    } else if (refused) require(waitFor(coldHead ? gate.headReady : gate.cueReady),
            "the previously missing page completed after the refused grid");
    if (laterRestart) {
        require(waitUntil([&] {
            const audio::CallbackScope callback;
            if (!pages.reserveWorkerCue(102,savedStart,blockFrames,false)) return false;
            pages.releaseCue(102);
            return true;
        }),"the active aligned reader fills its deferred Start from normal read-ahead");
        through(due+repeatPeriod+blockFrames);
    }
    if (restarted || laterRestart) release();
    through(finish);
    if (heldImmediate && restarted) {
        require(driver.readStatus(rows,status) && rows[0].clip == 102 && rows[0].completedLoops == 1 &&
                std::equal(expectedAfterRelease.begin(),expectedAfterRelease.end(),pcm.begin()+releaseWrap+delay),
                "Release cancels another restart and the exact normal wrap remains audible through PDC");
    }
    const auto repeated = restarted || laterRestart;
    const auto heardAfterRelease = static_cast<std::size_t>(due+2*repeatPeriod+delay+120);
    const auto pass = refused ? (events.count == 1 && rows[0].run == run && pcm[heardAttack] == .55f)
        : (events.count == (repeated ? 3u : 2u) && rows[0].clip == 102 && events.edges[1].sample == due &&
           events.edges[1].sourceTick == phase &&
           rows[0].completedLoops == ((immediateWrap && !restarted) || heldImmediate ? 1u : 0u) &&
           (!repeated || (events.edges[2].sample == nextAt && events.edges[2].sourceTick == startTick &&
                          (heldImmediate || pcm[heardAfterRelease] > .5f))) &&
           std::equal(expected.begin(),expected.end(),pcm.begin()+due+delay) &&
           (!laterRestart || std::equal(expectedLater.begin(),expectedLater.end(),pcm.begin()+due+repeatPeriod+delay)));
    const auto lost = pages.underrunSamples() - underrunsBefore;
    std::printf("%s worker Legato case %d: cue %d, delay %d, clip %lld, events %zu, old attack %.6f, missing %u\n",
        immediate ? "Immediate" : "Scheduled",static_cast<int>(testCase),cue,delay,static_cast<long long>(rows[0].clip),events.count,
        static_cast<double>(pcm[heardAttack]),lost);
    require(pass && lost == 0 && allocations == 0 && audio::callbackFileIo == 0,
            "the warm cue is sample-exact through track PDC and the cold cue keeps the old run with no late replay");
}

enum class WorkerFollowCase { Warm, Cold, Manual, Stop };

void linkedWorkerLegato(bool noGrid, WorkerFollowCase testCase) {
    constexpr int rawStart = 2400, loopHead = 124800, loopEnd = 134400;
    constexpr int physicalEnd = loopEnd - rawStart, period = loopEnd - loopHead;
    constexpr int cue = 18*8192-1, manualCue = 16*8192-1, cutFrames = blockFrames-17;
    constexpr std::uint64_t incarnation = 113;
    constexpr auto extent = 10 * quarter;
    const auto due = noGrid ? physicalEnd : 144000;
    const auto phaseAt = [&](int at) { return loopHead + (at - physicalEnd) % period; };
    const auto inherited = phaseAt(due) - rawStart;
    const auto cold = testCase == WorkerFollowCase::Cold;
    const auto manual = testCase == WorkerFollowCase::Manual;
    const auto stop = testCase == WorkerFollowCase::Stop;
    const auto frameToTick = [](int frame) {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(frame)*quarter/(rate/2)));
    };
    const auto frameToNs = [](std::int64_t frame) {
        return (frame*1'000'000'000 + rate/2)/rate;
    };
    test::TempDirectory folder{"linked_worker_legato",noGrid ? "physical end" : "quarter"};
    StoreError error{};
    auto store = Store::createEmpty(folder.path()/"song.adi",error);
    require(static_cast<bool>(store),"create the saved linked worker project");
    test::applySessionSchema(store->db());
    auto& db = store->db();
    db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Linked worker Legato',48000);"
            "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Slots',0),(9,'master','Main',1);"
            "INSERT INTO scenes(id,ord) VALUES(10,0),(20,1),(30,2)");
    addSlot(*store,101,10,.55f,extent,phaseAt(due+140),false);
    addSlot(*store,102,20,-.8f,extent,cue+40,true);
    if (manual) addSlot(*store,103,30,-.7f,extent,manualCue+40,true);
    db.exec("UPDATE audio_clips SET warp_enabled=0,warp_markers=NULL WHERE clip_id=101;"
            "UPDATE clip_launch SET ram_mode=1,launch_quant=1,follow_enabled=1,follow_linked=1,"
            "follow_loop_count=1,follow_a=4 WHERE clip_id=101;"
            "UPDATE clips SET time_base=1,pos_ticks=NULL,pos_ns=0,length_ticks=NULL,length_ns=4500000000 WHERE id=101;"
            "UPDATE clips SET loop_enabled=0,loop_len_ticks=NULL WHERE id>=102");
    {
        SQLite::Statement loop(db,"UPDATE clips SET loop_start_ticks=?,loop_len_ticks=? WHERE id=101");
        loop.bind(1,frameToTick(loopHead)); loop.bind(2,frameToTick(period)); loop.exec();
        SQLite::Statement pins(db,"INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns,"
                                  "loop_begin_ns,loop_end_ns) VALUES(101,?,?,?,?)");
        pins.bind(1,frameToNs(rawStart)); pins.bind(2,frameToNs(216000));
        pins.bind(3,frameToNs(loopHead)); pins.bind(4,frameToNs(loopEnd)); pins.exec();
        SQLite::Statement quantization(db,"UPDATE clip_launch SET launch_quant=? WHERE clip_id>=102");
        quantization.bind(1,noGrid ? 1 : 8); quantization.exec();
        const auto trim = [&](int id,int desiredCue) {
            const auto start = frameToTick(desiredCue-inherited);
            SQLite::Statement slot(db,"UPDATE clips SET content_offset_ticks=?,length_ticks=length_ticks-? WHERE id=?");
            slot.bind(1,start); slot.bind(2,start); slot.bind(3,id); slot.exec();
        };
        trim(102,cue);
        if (manual) trim(103,manualCue);
    }
    store.reset();
    store = Store::open(folder.path()/"song.adi",error);
    require(static_cast<bool>(store),"reopen the linked worker rows");
    struct Gate {
        std::int64_t heldPage;
        std::atomic<bool> entered{false}, warmed{false}, resume{false};
    } gate{cold ? 18 : manual && !noGrid ? 16 : -1};
    const ClipPlayback::RequestSnapshotProbeForTest probe{.context=&gate,
        .beforePageClaim=[](void* opaque,std::int64_t,std::int64_t number) noexcept {
            auto& state = *static_cast<Gate*>(opaque);
            if (number == 19) state.warmed.store(true,std::memory_order_release);
            if (number != state.heldPage) return;
            state.entered.store(true,std::memory_order_release);
            while (!state.resume.load(std::memory_order_acquire)) std::this_thread::yield();
        }};
    const auto project = readClipProject(*store,rows::readModel(*store),true);
    TempoMap clock;
    clock.events.push_back({0,120,0});
    std::vector<std::string> problems;
    const auto bank = prepareStoredLaunchProject(*project,clock,rate,2,blockFrames,incarnation,problems);
    const auto* target = bank ? bank->clip(102) : nullptr;
    require(target && target->pageReader() && target->latencySamples() == 2048 &&
            !target->allWorkerPhasesPrimed() && problems.empty() &&
            bank->view().slots.front().physicalStartFrame == rawStart,
            "prepare a physical outgoing loop and a long aligned incoming source");
    auto& pages = *target->pageReader();
    struct ResumeOnExit { Gate& gate; ~ResumeOnExit(){ gate.resume.store(true,std::memory_order_release); } } resume{gate};
    pages.setRequestSnapshotProbeForTest(&probe);
    auto runtime = std::make_shared<LaunchAudioRuntime>(bank,std::vector<std::int64_t>{1},blockFrames);
    LaunchAudioDriver driver(1);
    Transport transport;
    ClipPlayback track(project,transport,rate,2,blockFrames);
    track.attachLaunchAudio(1,runtime);
    auto* source = track.sourcesFor(1).front();
    const auto delay = source->latencySamples();
    require(delay == 2048,"the attached physical source reports the worker compensation");
    transport.play();
    const auto finish = due+delay+blockFrames+100;
    std::vector<float> pcm(static_cast<std::size_t>(finish));
    struct Trace { std::array<LaunchTransition,4> edges{}; std::size_t count = 0; } trace;
    const auto through = [&](int end) {
        while (transport.position() < end) {
            LaunchBlock block;
            block.frames = static_cast<std::int32_t>(std::min<std::int64_t>(blockFrames,end-transport.position()));
            block.global = stop ? (noGrid ? LaunchQuantization::None : LaunchQuantization::Quarter)
                                : LaunchQuantization::Bar;
            block.clock = &clock;
            block.tickAtSample = [](const void* raw,std::int64_t frame) noexcept {
                return static_cast<const TempoMap*>(raw)->secondsToTicks(static_cast<double>(frame)/rate);
            };
            block.sampleAtTick = [](const void* raw,std::int64_t tick) noexcept -> std::int64_t {
                return std::llround(static_cast<const TempoMap*>(raw)->ticksToSeconds(tick)*rate);
            };
            std::array<float,blockFrames> right{};
            float* channels[]{pcm.data()+transport.position(),right.data()};
            NodeIo io;
            io.out = channels;
            io.channels = 2;
            io.frames = block.frames;
            io.sampleRate = rate;
            {
                const audio::CallbackScope callback;
                runtime->begin(driver,block,transport,transport.position(),&trace,
                    [](void* opaque,const LaunchTransition& edge,std::int64_t) noexcept {
                        auto& report = *static_cast<Trace*>(opaque);
                        if (report.count < report.edges.size()) report.edges[report.count++] = edge;
                    });
                source->process(io);
                runtime->finish();
            }
            transport.advance(block.frames);
        }
    };
    const auto waitUntil = [](auto ready) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
        do {
            if (ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    const auto observed = [&](const std::atomic<bool>& event) {
        return waitUntil([&] { return event.load(std::memory_order_acquire); });
    };
    require(driver.post(std::array{LaunchRequest{LaunchRequestKind::SlotPress,1,10,101,incarnation}}),"start the physical raw loop");
    through(2*blockFrames);
    std::array<LaunchTrackStatus,1> rows{};
    LaunchStatusHeader status{};
    require(driver.readStatus(rows,status) && rows[0].clip == 101 && rows[0].queued.action == LaunchQueuedAction::None,
            "a Linked Follow does not occupy public queued status");
    const auto oldRun = rows[0].run;
    require(observed(cold ? gate.entered : gate.warmed),"private forecast requests the exact far R3 PCM pages");
    std::array<float,cutFrames> expected{};
    if (testCase == WorkerFollowCase::Warm) {
        const audio::CallbackScope callback;
        require(pages.read(102,cue,cutFrames,false),"measure the exact aligned cue");
        std::ranges::copy(pages.channel(0),expected.begin());
    }
    if (manual || stop) {
        through(noGrid ? due : due-blockFrames);
        const auto request = manual ? LaunchRequest{LaunchRequestKind::SlotPress,1,30,103,incarnation}
            : LaunchRequest{LaunchRequestKind::StopTrack,1,{},{},incarnation};
        require(driver.post(std::array{request}),"post the coincident manual Start or Stop");
        if (!noGrid) {
            through(due-17);
            require(driver.readStatus(rows,status) && rows[0].queued.action ==
                (manual ? LaunchQueuedAction::Start : LaunchQueuedAction::Stop),"publish only the coincident user action");
            if (manual) require(observed(gate.entered),"the manual candidate's second cue page remains cold");
        }
    } else {
        through(due-17);
    }
    through(due+cutFrames);
    require(driver.readStatus(rows,status) && rows[0].queued.action == LaunchQueuedAction::None,
            "settle the linked or coincident request once");
    gate.resume.store(true,std::memory_order_release);
    if (cold || manual) {
        const auto id = manual ? 103 : 102;
        const auto frame = manual ? manualCue : cue;
        {
            const audio::CallbackScope callback;
            pages.prefetch(id,frame);
        }
        require(waitUntil([&] {
            const audio::CallbackScope callback;
            if (!pages.reserveWorkerCue(id,frame,blockFrames,false)) return false;
            pages.releaseCue(id);
            return true;
        }),"the refused pages can finish warming without a late replay");
    }
    through(finish);
    const auto heard = due+delay;
    const auto warm = testCase == WorkerFollowCase::Warm;
    const auto pass = warm
        ? (trace.count == 2 && trace.edges[1].newClip == 102 && trace.edges[1].sample == due &&
           target->frameForTick(trace.edges[1].sourceTick,120) == cue &&
           *std::min_element(expected.begin(),expected.end()) < -.5f &&
           std::equal(expected.begin(),expected.end(),pcm.begin()+heard))
        : stop ? (trace.count == 2 && trace.edges[1].newClip == 0 && trace.edges[1].sample == due &&
                  std::all_of(pcm.begin()+heard,pcm.end(),[](float sample) { return sample == 0; }))
               : (trace.count == 1 && rows[0].run == oldRun && pcm[static_cast<std::size_t>(heard+140)] == .55f);
    std::printf("Linked worker Legato %s case %d: cut %d, cue %d, clip %lld, transitions %zu, underruns %u\n",
                noGrid ? "None" : "Quarter",static_cast<int>(testCase),due,cue,static_cast<long long>(rows[0].clip),
                trace.count,pages.underrunSamples());
    require(pass && pages.underrunSamples() == 0 && allocations == 0 && audio::callbackFileIo == 0,
            "the exact private worker cue obeys the physical lap and manual precedence with no callback faults");
}

} // namespace

int main() {
    try {
        for (const auto noGrid : {false,true})
            for (const auto testCase : {WorkerFollowCase::Warm,WorkerFollowCase::Cold,
                                        WorkerFollowCase::Manual,WorkerFollowCase::Stop})
                linkedWorkerLegato(noGrid,testCase);
        for (const auto testCase : {WorkerCueCase::Warm,WorkerCueCase::ColdCue,WorkerCueCase::WarmWrap,
                                    WorkerCueCase::ColdWrap,WorkerCueCase::ColdDistantHead,WorkerCueCase::RepeatWrap,
                                    WorkerCueCase::ColdRepeatStart,WorkerCueCase::ColdLaterStart,WorkerCueCase::ReleasedBeforeCut})
            workerLegato(testCase);
        for (const auto testCase : {WorkerCueCase::Warm,WorkerCueCase::ColdCue,WorkerCueCase::RepeatWrap,
                                    WorkerCueCase::ColdRepeatStart})
            workerLegato(testCase,true);
        workerTempoScene(SceneCase::Restart);
        workerTempoScene(SceneCase::Legato);
        workerTempoScene(SceneCase::LegatoColdAfterRetime);
        preparedWarpToRawScene(false);
        preparedWarpToRawScene(true);
        workerExternalTempo(ExternalCase::Active);
        workerExternalTempo(ExternalCase::Idle);
        workerExternalTempo(ExternalCase::StartsDuringPreparation);
    }
    catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
}
