// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/launch_audio.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/session.hpp"
#include "adi/media/blake3.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
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
             std::int64_t pulseOffset, bool legato) {
    const auto frames = static_cast<std::uint32_t>(duration * (rate / 2) / quarter);
    writeSource(store,clip,frames,0,std::array{Pulse{pulseOffset,level}});
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

} // namespace

int main() {
    try {
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
