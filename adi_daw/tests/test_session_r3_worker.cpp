// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
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
constexpr std::int64_t fasterToneOffset = (toneOffset * 120 + 70) / 140;

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

void addSlot(Store& store, int clip, int scene, float level) {
    constexpr std::uint32_t frames = rate / 4;
    writeSource(store,clip,frames,0,std::array{Pulse{toneOffset,level}});
    auto& db = store.db();
    SQLite::Statement placement(db, "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,loop_enabled,loop_len_ticks) "
                                    "VALUES(?,1,'audio',0,?,1,?)");
    placement.bind(1,clip); placement.bind(2,quarter/2); placement.bind(3,quarter/2); placement.exec();
    const auto markers = audio::writeWarpMap(std::array<WarpMarker,2>{{{0,0},{frames,quarter/2}}},frames);
    require(static_cast<bool>(markers), "encode worker R3 markers");
    SQLite::Statement audioClip(db, "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) "
                                    "VALUES(?,?,?,1,'complex',?)");
    audioClip.bind(1,clip); audioClip.bind(2,clip); audioClip.bind(3,frames);
    audioClip.bind(4,markers.bytes.data(),static_cast<int>(markers.bytes.size())); audioClip.exec();
    SQLite::Statement cell(db, "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,?,?)");
    cell.bind(1,scene); cell.bind(2,clip); cell.exec();
    SQLite::Statement launch(db, "INSERT INTO clip_launch(clip_id,legato,ram_mode,clip_fade) VALUES(?,0,0,0)");
    launch.bind(1,clip); launch.exec();
}

void launchScene(Store& store, const Session& session, int scene) {
    OpRequest request;
    request.opType = "session.launchScene";
    request.payload = {{"scene",scene},{"incarnation",session.launchIncarnation()}};
    require(OpJournal(store).commit(request).ok, "launch actual persisted tempo scene");
}

struct Playback {
    Session& session;
    std::array<std::vector<float>,2> pcm;
    std::int64_t first = 0, performedAt = -1;
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

void workerTempoScene() {
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
    addSlot(*store,101,10,0.65f);
    addSlot(*store,102,20,-0.78f);
    constexpr std::uint32_t dryFrames = 130000;
    // Separate the dry landmarks from the R3 transients so the stereo
    // analysis of a left-only clip cannot move the dry peak measurement.
    writeSource(*store,103,dryFrames,1,std::array{Pulse{dryOffset,0.5f},Pulse{sceneFrame+dryOffset,0.5f}});
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
    constexpr std::int64_t finish = sceneFrame+8000;
    const auto frames = static_cast<std::size_t>(finish);
    Playback playback{session,{std::vector<float>(frames),std::vector<float>(frames)}};
    launchScene(*store,session,10);
    playback.through(blockFrames);
    launchScene(*store,session,20);
    playback.through(finish);
    require(playback.performedAt == sceneFrame && playback.performedBpm == 140,
            "the new tempo performs on the exact saved scene boundary");
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
    const auto after = peak(0,sceneFrame+fasterToneOffset+delay,true);
    require(std::abs(before) <= 160 && std::abs(after) <= 160,"R3 slot landmarks follow both scene clocks");
    require(peak(1,dryOffset+delay,false) == 0 && peak(1,sceneFrame+dryOffset+delay,false) == 0,
            "parallel dry audio aligns to the reported slot delay exactly once");
    require(db.execAndGet("SELECT bpm FROM tempo_map WHERE pos_ticks=0").getDouble() == 120,
            "performing the tempo scene leaves the authored clock intact");
    require(session.problems().empty() && session.clips()->underrunSamples() == 0 &&
            allocations == 0 && audio::callbackFileIo == 0,
            "the worker-backed scene has no source problems, callback allocation or file I/O");
    std::printf("Complex slot scene at %lld, 120 to 140 BPM peak errors %lld/%lld samples, dry error 0, callback allocations %u\n",
                static_cast<long long>(playback.performedAt),static_cast<long long>(before),static_cast<long long>(after),allocations);
}
} // namespace

int main() {
    try { workerTempoScene(); }
    catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
}
