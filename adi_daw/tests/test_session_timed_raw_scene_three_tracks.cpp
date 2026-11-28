// SPDX-License-Identifier: AGPL-3.0-or-later
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/clip_playback.hpp"
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
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace adi::test {
unsigned sceneClockCallbackAllocations() noexcept;
}

namespace {
using namespace adi;
using namespace adi::engine;

constexpr std::uint32_t rate = 48'000;
constexpr std::int32_t blockFrames = 257;
constexpr std::int64_t cut = 96'000, prepareAt = cut - 396, crossing = prepareAt + blockFrames;
constexpr auto bar = 4 * textproj::kPPQ;
constexpr std::uint32_t mediaFrames = 240'000;
constexpr std::int64_t durationNs = 4'000'000'000;
constexpr std::array<std::int64_t,6> clips{101,102,103,201,202,203};
constexpr std::array<std::int64_t,6> starts{2400,3600,4200,4800,6000,7200};
constexpr std::array<std::int64_t,3> nearStarts{88'000,90'000,92'000};
constexpr std::array<float,6> levels{.125f,.25f,.375f,-.125f,-.25f,-.375f};
static_assert(starts[5] + cut - blockFrames > 3 * ClipPlayback::readAheadFrames(rate));
static_assert(starts[3] + cut - nearStarts[0] < ClipPlayback::readAheadFrames(rate));
static_assert(starts[4] + cut - nearStarts[1] < ClipPlayback::readAheadFrames(rate));
static_assert(starts[5] + cut - nearStarts[2] < ClipPlayback::readAheadFrames(rate));

void require(bool pass, const char* detail) {
    if (!pass) throw std::runtime_error(detail);
}

float signal(std::size_t index, std::int64_t frame) {
    return levels[index] + static_cast<float>((frame % (239 + 2 * static_cast<std::int64_t>(index))) - 119) / 8192.f;
}

std::unique_ptr<Store> prepareProject(const test::TempDirectory& folder) {
    StoreError error{};
    const auto project = folder.path() / "scene.adi";
    auto store = Store::createEmpty(project,error);
    require(static_cast<bool>(store),"create the saved three-track raw tempo Scene");
    test::applySessionSchema(store->db());
    auto& db = store->db();
    db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Three raw tracks',48000);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
            "(1,'audio','A',0),(2,'audio','B',1),(3,'audio','C',2),(9,'master','Main',3);"
            "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
            "INSERT INTO scenes(id,ord,tempo_bpm,tempo_enabled) VALUES(10,0,120,0),(20,1,140,1);");
    for (std::size_t index = 0; index < clips.size(); ++index) {
        const auto id = clips[index];
        const auto track = static_cast<std::int64_t>(index % 3 + 1);
        const bool target = index >= 3;
        const auto name = std::to_string(id) + ".wav";
        const auto path = folder.path() / name;
        std::vector<float> samples(mediaFrames);
        for (std::size_t frame = 0; frame < samples.size(); ++frame)
            samples[frame] = signal(index,static_cast<std::int64_t>(frame));
        {
            audio::WavWriter wav(path,rate,1,audio::WavFormat::Float32);
            wav.write(samples.data(),mediaFrames);
            wav.close();
        }
        const auto digest = media::blake3File(path);
        require(static_cast<bool>(digest),"hash a raw Scene WAV");
        SQLite::Statement media(db,"INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames)"
                                   " VALUES(?,?,?,'wav',48000,1,240000)");
        media.bind(1,id); media.bind(2,digest.hex); media.bind(3,name); media.exec();
        SQLite::Statement clip(db,"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,loop_enabled)"
                                  " VALUES(?,?,'audio',1,0,?,0)");
        clip.bind(1,id); clip.bind(2,track); clip.bind(3,durationNs); clip.exec();
        SQLite::Statement raw(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled)"
                                 " VALUES(?,?,240000,0)");
        raw.bind(1,id); raw.bind(2,id); raw.exec();
        const auto ns = starts[index] * 1'000'000'000 / rate;
        SQLite::Statement pins(db,"INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(?,?,?)");
        pins.bind(1,id); pins.bind(2,ns); pins.bind(3,ns + durationNs); pins.exec();
        SQLite::Statement slot(db,"INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(?,?,?)");
        slot.bind(1,track); slot.bind(2,target ? 20 : 10); slot.bind(3,id); slot.exec();
        SQLite::Statement launch(db,"INSERT INTO clip_launch(clip_id,legato,ram_mode,clip_fade,launch_quant)"
                                    " VALUES(?,?,?,0,?)");
        launch.bind(1,id); launch.bind(2,target ? 1 : 0); launch.bind(3,target ? 0 : 1);
        launch.bind(4,target ? 8 : 1); launch.exec();
    }
    store.reset();
    store = Store::open(project,error);
    require(static_cast<bool>(store),"reopen the persisted three-track project");
    return store;
}

struct Playback {
    Session& session;
    std::span<float> pcm;
    std::array<LaunchTransition,9> edges{};
    LaunchSceneTransition scene{};
    std::size_t edgeCount = 0, sceneCount = 0;
    std::int64_t cursor = 0;
    bool crossedOnOldClock = false;

    void through(std::int64_t end) {
        std::array<float,blockFrames> left{}, right{};
        float* output[]{left.data(),right.data()};
        while (cursor < end) {
            const auto frames = static_cast<std::int32_t>(std::min<std::int64_t>(blockFrames,end - cursor));
            const AudioIo io{output,nullptr,2,0,frames,cursor};
            {
                auto render = session.beginRender();
                if (cursor == crossing && frames == blockFrames)
                    crossedOnOldClock = render.bpmAtTick(bar) == 120;
                render.observeLaunchTransitions(this,[](void* opaque,const LaunchTransition& edge,std::int64_t) noexcept {
                    auto& playback = *static_cast<Playback*>(opaque);
                    if (playback.edgeCount < playback.edges.size()) playback.edges[playback.edgeCount] = edge;
                    ++playback.edgeCount;
                });
                render.observeSceneLaunches(this,[](void* opaque,const LaunchSceneTransition& edge,std::int64_t) noexcept {
                    auto& playback = *static_cast<Playback*>(opaque);
                    playback.scene = edge;
                    ++playback.sceneCount;
                });
                require(render.process(io),"render the real three-track Session callback");
            }
            require(std::equal(left.begin(),left.begin()+frames,right.begin()),"all three raw voices have matched stereo PCM");
            std::copy_n(left.begin(),frames,pcm.begin()+cursor);
            cursor += frames;
            if (cursor >= prepareAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

std::array<LaunchTrackStatus,3> readTracks(const Session& session) {
    std::array<LaunchTrackStatus,3> rows{};
    LaunchStatusHeader header;
    require(session.readLaunchStatus(rows,header) && header.trackCount == rows.size(),"read all three stored audio tracks");
    return rows;
}

enum class Outcome { Warm, ColdThird, StopTrack };

void exercise(Outcome outcome) {
    const bool warm = outcome == Outcome::Warm;
    const bool coldThird = outcome == Outcome::ColdThird;
    const bool stop = outcome == Outcome::StopTrack;
    const auto mode = warm ? "warm" : coldThird ? "cold-third" : "stop";
    auto startedAt = nearStarts;
    if (coldThird) startedAt[2] = blockFrames;
    constexpr auto end = cut + 3 * blockFrames;
    test::TempDirectory folder{"three_raw_timed_scene",mode};
    auto store = prepareProject(folder);
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(),"load the old stored raw sources");
    session.transport().play();
    std::vector<float> pcm(static_cast<std::size_t>(end));
    Playback playback{session,pcm};
    const auto initialAllocations = test::sceneClockCallbackAllocations();
    const auto initialFileIo = audio::callbackFileIo.load();
    const auto order = coldThird ? std::array<std::size_t,3>{2,0,1} : std::array<std::size_t,3>{0,1,2};
    for (const auto index : order) {
        playback.through(startedAt[index]);
        LaunchRequest start;
        start.kind = LaunchRequestKind::SlotPress;
        start.incarnation = session.launchIncarnation();
        start.track = static_cast<std::int64_t>(index + 1);
        start.clip = clips[index];
        start.scene = 10;
        require(session.postLaunchGesture({&start,1}),"queue a saved outgoing raw slot");
    }
    playback.through(prepareAt);
    const auto old = readTracks(session);
    for (std::size_t index = 0; index < old.size(); ++index)
        require(old[index].clip == clips[index] && old[index].run != 0,"keep every outgoing run alive up to the Scene");
    require(playback.edgeCount == 3 && playback.sceneCount == 0,"only the three outgoing slots have launched");
    if (!session.prepareSceneLaunch(*store,20,bar)) throw std::runtime_error(session.error());
    if (stop) {
        playback.through(crossing);
        OpRequest command;
        command.opType = "session.stopTrack";
        command.payload = {{"track",2},{"incarnation",session.launchIncarnation()}};
        require(OpJournal(*store).commit(command).ok,"queue competing StopTrack on the old clock");
    }
    playback.through(end);

    const auto after = readTracks(session);
    require(playback.crossedOnOldClock && session.beginRender().bpmAtTick(bar) == (warm ? 140 : 120) &&
            playback.sceneCount == (warm ? 1u : 0u) && playback.edgeCount == (warm ? 6u : stop ? 4u : 3u),
            "clock, receipt and all three Starts are one driver decision");
    if (warm) require(playback.scene.sample == cut && playback.scene.tempoBpm == 140,
                      "the single receipt uses the same exact 140 BPM cut");
    if (stop) {
        const auto& edge = playback.edges[3];
        require(edge.track == 2 && edge.oldClip == clips[1] && edge.oldRun == old[1].run &&
                edge.newClip == 0 && edge.sample == cut && edge.tick == bar,
                "B stops exactly once on the old clock while the timed Scene refuses");
    }
    for (std::size_t index = 0; index < after.size(); ++index) {
        if (warm) {
            const auto& edge = playback.edges[3 + index];
            require(edge.sample == cut && edge.oldClip == clips[index] && edge.newClip == clips[index + 3] &&
                    after[index].clip == clips[index + 3] && after[index].run != old[index].run,
                    "all three warmed readers launch at the exact same sample");
        } else if (stop && index == 1) {
            require(after[index].clip == 0,"B stays stopped after the refused Scene");
        } else {
            require(after[index].clip == old[index].clip && after[index].run == old[index].run,
                    "every track unaffected by Stop keeps its original run");
        }
    }
    for (auto sample = std::int64_t{0}; sample < end; ++sample) {
        auto expected = 0.f;
        for (std::size_t index = 0; index < startedAt.size(); ++index) {
            if (sample < startedAt[index] || (stop && index == 1 && sample >= cut)) continue;
            const auto clip = warm && sample >= cut ? index + 3 : index;
            expected += signal(clip,sample - startedAt[index] + starts[clip]);
        }
        require(pcm[static_cast<std::size_t>(sample)] == expected,
                "all three stereo voices or the exact old suffix match saved PCM");
    }
    require(test::sceneClockCallbackAllocations() == initialAllocations && audio::callbackFileIo.load() == initialFileIo,
            "bounded three-track admission performs no callback allocation or file reads");
    std::printf("Three-track raw Scene %s: exact stereo PCM, %zu clip edges, %zu Scene receipts\n",
                mode,playback.edgeCount,playback.sceneCount);
}
} // namespace

namespace adi::test {
void runThreeTrackTimedRawSceneTests() {
    for (const auto outcome : {Outcome::Warm,Outcome::ColdThird,Outcome::StopTrack}) exercise(outcome);
}
} // namespace adi::test
