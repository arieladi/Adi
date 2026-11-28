// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/clip_playback.hpp"
#include "adi/engine/launch_audio_runtime.hpp"
#include "adi/engine/launch_gain_edits.hpp"
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
#include <filesystem>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace { unsigned callbackAllocations = 0; }
void adi::test::countReplacedNew() noexcept {
    if (adi::audio::onAudioThread) ++callbackAllocations;
}

namespace {
using namespace adi;
using namespace adi::engine;
constexpr std::int32_t rate = 48'000, blockFrames = 257, lastBeat = 12;
constexpr auto quarter = textproj::kPPQ;
constexpr auto ticksPerFrame = quarter * 140 / (60 * rate) + 1;
constexpr std::int64_t tempoSceneFrame = 2 * rate;

void require(bool pass, const char* message) {
    if (!pass) throw std::runtime_error(message);
}

std::int64_t frameAtBeat(double beat) {
    constexpr double slope = 20.0 / lastBeat;
    return std::llround(rate * 60.0 / slope * std::log1p(slope * beat / 120.0));
}

std::int64_t frameAfterTempoScene(int beats) {
    return tempoSceneFrame + std::llround(beats * (60.0 * rate / 140));
}

std::unique_ptr<Store> sceneProject(const std::filesystem::path& folder) {
    StoreError error{};
    auto store = Store::createEmpty(folder/"song.adi", error);
    require(static_cast<bool>(store), "create project");
    test::applySessionSchema(store->db());
    store->db().exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Session Warp',48000);"
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Slots',0),(9,'master','Main',1);"
        "INSERT INTO scenes(id,ord) VALUES(10,0),(20,1);");
    return store;
}

void addClip(Store& store, const std::filesystem::path& folder, int clip, int scene,
             int beats, bool legato, int pulseOffsetFrames = 0, int track = 1, int sourceFramesPerBeat = rate / 2) {
    constexpr int halfWidth = 48;
    const auto frames = static_cast<std::uint32_t>(beats * sourceFramesPerBeat);
    const auto name = std::to_string(clip) + ".wav";
    const auto file = folder / name;
    std::vector<float> pcm(frames);
    for (int beat = 0; beat < beats; ++beat) for (int offset = -halfWidth; offset <= halfWidth; ++offset) {
        const auto frame = beat * sourceFramesPerBeat + pulseOffsetFrames + offset;
        if (frame < 0) continue;
        const auto level = legato ? -0.5 - 0.1 * beat : 0.55;
        pcm[static_cast<std::size_t>(frame)] = static_cast<float>(level * (0.5 + 0.5 * std::cos(offset * std::numbers::pi / halfWidth)));
    }
    {
        audio::WavWriter writer(file, rate, 1, audio::WavFormat::Float32);
        writer.write(pcm.data(), frames);
        writer.close();
    }
    const auto digest = media::blake3File(file);
    require(static_cast<bool>(digest), "hash native slot audio");
    auto& db = store.db();
    SQLite::Statement mediaRow(db, "INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) VALUES(?,?,?,'wav',48000,1,?)");
    mediaRow.bind(1, clip); mediaRow.bind(2, digest.hex); mediaRow.bind(3, name); mediaRow.bind(4, frames); mediaRow.exec();
    SQLite::Statement clipRow(db, "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,loop_enabled,loop_len_ticks) VALUES(?,?,'audio',0,?,1,?)");
    clipRow.bind(1, clip); clipRow.bind(2, track); clipRow.bind(3, beats * quarter); clipRow.bind(4, beats * quarter);
    clipRow.exec();
    const auto markers = audio::writeWarpMap(std::array<WarpMarker,2>{{{0,0},{frames, beats*quarter}}}, frames);
    require(static_cast<bool>(markers), "encode native slot Warp source map");
    SQLite::Statement audioRow(db, "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) VALUES(?,?,?,1,'re-pitch',?)");
    audioRow.bind(1, clip); audioRow.bind(2, clip); audioRow.bind(3, frames);
    audioRow.bind(4, markers.bytes.data(), static_cast<int>(markers.bytes.size())); audioRow.exec();
    SQLite::Statement slot(db, "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(?,?,?)");
    slot.bind(1, track); slot.bind(2, scene); slot.bind(3, clip); slot.exec();
    SQLite::Statement settings(db, "INSERT INTO clip_launch(clip_id,legato,ram_mode,clip_fade) VALUES(?,?,1,0)");
    settings.bind(1, clip); settings.bind(2, legato ? 1 : 0); settings.exec();
}

struct Playback {
    struct Edge { std::int64_t newClip = 0, sample = 0, sourceTick = 0, track = 0, tick = 0; };
    struct Scene { std::int64_t scene = 0, sample = 0; double bpm = 0; };
    Session& session;
    std::span<float> pcm;
    std::array<Edge,8> edges{};
    std::array<Scene,4> scenes{};
    std::size_t count = 0;
    std::size_t sceneCount = 0;
    std::int64_t first = 0;
    bool paged = false;

    void through(std::int64_t target) {
        std::array<float,blockFrames> left{}, right{};
        float* out[]{left.data(), right.data()};
        while (first < target) {
            const auto frames = static_cast<std::int32_t>(std::min<std::int64_t>(blockFrames, target - first));
            AudioIo io{out, nullptr, 2, 0, frames, first};
            bool rendered;
            {
                const audio::CallbackScope scope;
                auto block = session.beginRender();
                block.observeLaunchTransitions(this, [](void* opaque, const LaunchTransition& edge, std::int64_t) noexcept {
                    auto& self = *static_cast<Playback*>(opaque);
                    if (self.count < self.edges.size()) {
                        self.edges[self.count++] = {edge.newClip, edge.sample, edge.sourceTick, edge.track, edge.tick};
                    }
                });
                block.observeSceneLaunches(this, [](void* opaque, const LaunchSceneTransition& edge, std::int64_t) noexcept {
                    auto& self = *static_cast<Playback*>(opaque);
                    if (self.sceneCount < self.scenes.size()) {
                        self.scenes[self.sceneCount++] = {edge.scene, edge.sample, edge.tempoBpm};
                    }
                });
                rendered = block.process(io);
            }
            require(rendered, "render actual persisted launch graph");
            std::copy_n(left.begin(), frames, pcm.begin() + first);
            first += frames;
            if (paged) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

void scene(Store& store, const Session& session, int sceneId) {
    OpRequest gesture;
    gesture.opType = "session.launchScene";
    gesture.payload = {{"scene", sceneId}, {"incarnation", session.launchIncarnation()}};
    require(OpJournal(store).commit(gesture).ok, "submit actual scene launch operation");
}

void sceneWithGain(Store& store, const Session& session, int sceneId, int clipId, double gainDb) {
    std::array<OpRequest,2> operations;
    operations[0].opType = "clip.setGain";
    operations[0].payload = {{"id", clipId}, {"gain", gainDb}};
    operations[1].opType = "session.launchScene";
    operations[1].payload = {{"scene", sceneId}, {"incarnation", session.launchIncarnation()}};
    require(OpJournal(store).commit(operations).ok, "commit the authored gain change with the tempo scene");
}

std::vector<float> exercise(bool paged = false, double storedTranspose = 0) {
    test::TempDirectory temporary{"session_warp_integration", "project"};
    auto store = sceneProject(temporary.path());
    auto& db = store->db();
    SQLite::Statement tempo(db, "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,1),(?,140,0)");
    tempo.bind(1, lastBeat * quarter); tempo.exec();
    addClip(*store, temporary.path(), 101, 10, 3, false);
    addClip(*store, temporary.path(), 102, 20, 4, true);
    if (paged) db.exec("UPDATE clip_launch SET ram_mode=0");
    SQLite::Statement transpose(db, "UPDATE audio_clips SET transpose_semis=? WHERE clip_id IN (101,102)");
    transpose.bind(1, storedTranspose);
    transpose.exec();

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.launchIncarnation() != 0, "load persisted launch slots");
    for (const auto& problem : session.problems()) std::printf("slot preparation: %s\n", problem.c_str());
    session.transport().play();
    std::vector<float> pcm(static_cast<std::size_t>(frameAtBeat(lastBeat)));
    Playback playback{session, pcm};
    playback.paged = paged;
    playback.through(blockFrames);
    scene(*store, session, 10);
    playback.through(frameAtBeat(4) + blockFrames);
    scene(*store, session, 20);
    playback.through(frameAtBeat(lastBeat));
    std::printf("%zu launch transitions, expected bars %lld and %lld, second source phase %lld ticks\n", playback.count,
                static_cast<long long>(frameAtBeat(4)), static_cast<long long>(frameAtBeat(8)),
                static_cast<long long>(playback.edges[1].sourceTick));
    require(playback.count == 2 && playback.edges[0].newClip == 101 && playback.edges[1].newClip == 102,
            "both persisted scenes fired on the slot track");
    require(playback.edges[0].sample == frameAtBeat(4) && playback.edges[1].sample == frameAtBeat(8),
            "the launch lane used both analytically integrated bar boundaries");
    require(std::abs(playback.edges[1].sourceTick - quarter) <= ticksPerFrame,
            "second scene receives one beat of Legato phase from the shorter first loop");
    require(std::all_of(pcm.begin(), pcm.begin()+frameAtBeat(4), [](float value) { return value == 0; }),
            "the track is silent before the first launch");

    std::int64_t worst = 0;
    unsigned missing = 0;
    for (int beat = 4; beat < lastBeat; ++beat) {
        const auto expected = frameAtBeat(beat);
        const auto from = beat == 4 || beat == 8 ? expected : expected-4096;
        const auto begin = pcm.begin() + from;
        const auto end = pcm.begin() + expected + 4096;
        const auto peak = beat < 8 ? std::max_element(begin,end) : std::min_element(begin,end);
        const auto offsetError = std::abs((peak-pcm.begin())-expected);
        if (std::abs(*peak) < 0.4f) ++missing;
        else worst = std::max(worst, offsetError);
        std::printf("beat %d: peak %.6f, error %lld samples\n", beat, *peak, static_cast<long long>(offsetError));
    }
    std::printf("scene launches at %lld and %lld, Legato %lld ticks, %u/8 missing, worst %lld samples, allocations %u\n",
                static_cast<long long>(playback.edges[0].sample), static_cast<long long>(playback.edges[1].sample),
                static_cast<long long>(playback.edges[1].sourceTick), missing, static_cast<long long>(worst), callbackAllocations);
    require(missing == 0 && worst <= 2 && pcm[static_cast<std::size_t>(frameAtBeat(8))] < -0.58f,
            "slot pulses follow the ramp and the switched source enters on the Legato phase");
    require(session.problems().empty(), "prepare the persisted media under a ramp without slot problems");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
            "the native slot callback performs no allocation or file I/O");
    SQLite::Statement stored(db, "SELECT count(*) FROM audio_clips WHERE transpose_semis=? AND clip_id IN (101,102)");
    stored.bind(1, storedTranspose);
    require(stored.executeStep() && stored.getColumn(0).getInt()==2,
        "Re-Pitch leaves the saved transpose values intact");
    return pcm;
}

void enableTempoScene(Store& store) {
    store.db().exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
        "UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=20");
}

void checkTempoScene(Store& store, Session& session, const Playback& playback) {
    require(playback.sceneCount == 2 && playback.scenes[1].scene == 20 &&
        playback.scenes[1].sample == tempoSceneFrame && playback.scenes[1].bpm == 140,
        "the 140 BPM scene performs once at frame 96000 on the actual launch lane");
    require(session.beginRender().bpmAtTick(4 * quarter) == 140,
        "the rendered clock applies the scene tempo at the bar");
    require(store.db().execAndGet("SELECT bpm FROM tempo_map WHERE pos_ticks=0").getDouble() == 120,
        "scene tempo leaves the saved 120 BPM project tempo intact");
    require(session.problems().empty(), "the new scene bank reports no playback problem");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
        "the tempo scene callback performs no allocation or file I/O");
}

std::int64_t checkTempoPulses(std::span<const float> pcm, std::span<const float> expectedLevels,
                             float levelTolerance, const char* name) {
    std::int64_t worst = 0;
    for (std::size_t beat = 0; beat < expectedLevels.size(); ++beat) {
        const auto expected = frameAfterTempoScene(static_cast<int>(beat));
        const auto from = beat == 0 ? expected : expected - 256;
        const auto peak = std::min_element(pcm.begin() + from, pcm.begin() + expected + 257);
        const auto error = std::abs((peak - pcm.begin()) - expected);
        worst = std::max(worst, error);
        std::printf("%s pulse %zu: level %.6f, error %lld samples\n", name, beat, *peak, static_cast<long long>(error));
        require(std::abs(*peak - expectedLevels[beat]) <= levelTolerance,
            "each scene pulse is audible at the expected clip phase and level");
    }
    return worst;
}

void preparedTempoScene(bool paged) {
    test::TempDirectory folder{"session_scene_warp", "prepared"};
    auto store = sceneProject(folder.path());
    enableTempoScene(*store);
    addClip(*store, folder.path(), 101, 10, 3, false);
    addClip(*store, folder.path(), 102, 20, 4, true);
    // This unwarped slot keeps the scene on the prepared-bank path even when
    // the replacement's Re-Pitch media would be eligible for native Warp.
    store->db().exec("UPDATE audio_clips SET warp_enabled=0,warp_markers=NULL WHERE clip_id=101;"
        "UPDATE clip_launch SET legato=0 WHERE clip_id=102");
    // The transposed Complex variant exceeds the streaming pitch range and
    // exercises the prepared paging fallback through the same tempo scene.
    if (paged) store->db().exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=102;"
        "UPDATE audio_clips SET warp_mode='complex',transpose_semis=12.25 WHERE clip_id=102");

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(), "initial tempo bank is prepared");
    if (paged) require(session.graph().currentGraph()->latencySamples() == 0,
        "out-of-domain R3 slot keeps the prepared paging path");
    const auto last = frameAfterTempoScene(paged ? 1 : 3) + 1024;
    std::vector<float> pcm(static_cast<std::size_t>(last));
    Playback playback{session, pcm};
    scene(*store, session, 10);
    playback.through(blockFrames);
    scene(*store, session, 20);
    playback.through(last);
    checkTempoScene(*store, session, playback);
    require(playback.count == 2 && playback.edges[0].sample == 0 &&
        playback.edges[1].sample == tempoSceneFrame && playback.edges[1].newClip == 102 &&
        playback.edges[1].sourceTick == 0, "the addressed slot starts from source zero at the exact tempo bar");
    constexpr std::array<float,2> complexPeaks{-.09f, -.543f};
    constexpr std::array<float,4> resampledPeaks{-.5f, -.6f, -.7f, -.8f};
    const auto error = paged ? checkTempoPulses(pcm, complexPeaks, 0.08f, "Paged Complex")
        : checkTempoPulses(pcm, resampledPeaks, 0.03f, "RAM Re-Pitch");
    require(error <= (paged ? 192 : 2), "prepared slot audio follows the performed 140 BPM playback grid");
}

LaunchTrackStatus trackState(const Session& session) {
    std::array<LaunchTrackStatus,4> tracks{};
    LaunchStatusHeader header;
    require(session.readLaunchStatus(tracks, header) && header.trackCount == 1 && tracks[0].track == 1,
        "read the actual slot track's published launch state");
    return tracks[0];
}

void continuingTempoScene(bool legato, bool paged = false, bool changeAuthoredGain = false) {
    test::TempDirectory folder{"session_scene_warp", legato ? "legato" : "retained"};
    auto store = sceneProject(folder.path());
    enableTempoScene(*store);
    addClip(*store, folder.path(), 101, 10, 3, true);
    if (legato) addClip(*store, folder.path(), 102, 20, 4, true);
    else store->db().exec("INSERT INTO clip_slots(track_id,scene_id,has_stop) VALUES(1,20,0)");
    if (paged) store->db().exec("UPDATE clip_launch SET ram_mode=0");

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(), "prepare the native Warp slot");
    const auto last = frameAfterTempoScene(3) + 1024;
    std::vector<float> pcm(static_cast<std::size_t>(last));
    Playback playback{session, pcm};
    playback.paged = paged;
    scene(*store, session, 10);
    playback.through(blockFrames);
    const auto before = trackState(session);
    require(before.clip == 101 && before.run != 0, "the first scene starts the initial native Warp run");
    const LaunchClipGainEdit liveGain{1, 101, session.launchIncarnation(), before.run,
                                      0.5f, ClipUpdateRate::None};
    require(session.postLaunchClipGainEdit(liveGain), "post half gain to the initial run");
    playback.through(rate / 2 + 1);
    require(std::abs(pcm[rate / 2] + 0.30f) < 0.01f, "half gain is audible before the tempo scene is queued");
    if (changeAuthoredGain) {
        sceneWithGain(*store,session,20,101,20.0 * std::log10(0.5));
    } else {
        scene(*store, session, 20);
    }
    playback.through(last);
    require(std::abs(pcm[rate] + 0.35f) < 0.01f,
        changeAuthoredGain ? "the changed authored baseline replaces the old live multiplier" :
        "the continuing run keeps live gain between graph adoption and the scene bar");
    checkTempoScene(*store, session, playback);
    const auto after = trackState(session);
    if (legato) {
        require(playback.count == 2 && playback.edges[1].sample == tempoSceneFrame &&
            std::abs(playback.edges[1].sourceTick - quarter) <= ticksPerFrame &&
            after.clip == 102 && after.run != before.run,
            "the tempo scene starts the replacement run at the inherited one-beat phase");
    } else {
        require(playback.count == 1 && after.clip == 101 && after.run == before.run,
            "a tempo scene with an empty No Stop cell preserves the same running clip and run identity");
    }
    constexpr std::array<float,4> retainedPeaks{-.30f, -.35f, -.25f, -.30f};
    constexpr std::array<float,4> legatoPeaks{-.6f, -.7f, -.8f, -.5f};
    const auto error = checkTempoPulses(pcm, legato ? legatoPeaks : retainedPeaks, legato ? 0.08f : 0.04f,
        legato ? "Legato tempo scene" : "Retained tempo scene");
    require(error <= 2 && pcm[static_cast<std::size_t>(tempoSceneFrame)] < (legato ? -0.58f : -0.28f),
        "the new tempo follows the inherited phase and the current run's gain at the bar and loop wrap");
}

enum class MixedScene { Retain, Legato, KeepFixed, ColdWarp, ChangeNativeGain };

void mixedTempoScene(MixedScene mode) {
    const bool legato = mode == MixedScene::Legato;
    const bool paged = legato || mode == MixedScene::ColdWarp;
    test::TempDirectory folder{"session_scene_warp", legato ? "mixed-legato" : "mixed-retained"};
    auto store = sceneProject(folder.path());
    enableTempoScene(*store);
    store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(2,'audio','Fixed',1)");
    addClip(*store, folder.path(), 101, 10, 3, true, 0, 1, legato ? 16000 : rate / 2);
    addClip(*store, folder.path(), 201, 10, 3, false, rate / 8, 2);
    if (legato) {
        addClip(*store, folder.path(), 102, 20, 4, true, 0, 1, 16000);
        addClip(*store, folder.path(), 202, 20, 4, false, rate / 8, 2);
    } else {
        store->db().exec("INSERT INTO clip_slots(track_id,scene_id,has_stop) VALUES(1,20,0),(2,20,1)");
    }
    store->db().exec("UPDATE audio_clips SET warp_enabled=0,warp_markers=NULL WHERE clip_id>=200");
    if (paged) store->db().exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id<200");
    if (mode == MixedScene::KeepFixed)
        store->db().exec("UPDATE clip_slots SET has_stop=0 WHERE track_id=2 AND scene_id=20");

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(), "prepare a mixed native and fixed Session bank");
    const auto last = frameAfterTempoScene(3) + 1024;
    std::vector<float> pcm(static_cast<std::size_t>(last));
    Playback playback{session, pcm};
    playback.paged = paged;
    scene(*store, session, 10);
    playback.through(blockFrames);
    std::array<LaunchTrackStatus,4> before{};
    LaunchStatusHeader status;
    require(session.readLaunchStatus(before,status) && status.trackCount == 2 &&
        before[0].clip == 101 && before[1].clip == 201, "start both tracks of the mixed Session bank");
    if (mode == MixedScene::Retain || mode == MixedScene::ChangeNativeGain) {
        const LaunchClipGainEdit gain{1,101,session.launchIncarnation(),before[0].run,0.5f,ClipUpdateRate::None};
        require(session.postLaunchClipGainEdit(gain), "apply live gain to the retained native slot");
        playback.through(rate / 2 + 1);
        require(std::abs(pcm[rate / 2] + 0.30f) < 0.01f, "hear the initial live gain before queuing the mixed scene");
    }
    if (mode == MixedScene::KeepFixed || mode == MixedScene::ColdWarp || mode == MixedScene::ChangeNativeGain) {
        if (mode == MixedScene::ChangeNativeGain) sceneWithGain(*store,session,20,101,20.0 * std::log10(0.5));
        else require(!session.prepareSceneLaunch(*store,20,4 * quarter), "refuse an incompatible continuation");
        const auto expected = mode == MixedScene::KeepFixed ? "Stop or non-legato" :
            mode == MixedScene::ColdWarp ? "Session clips#101" : "unchanged Warp source audio and clip gain";
        require(session.error().find(expected) != std::string::npos, "name the unsupported continuation");
        playback.through(tempoSceneFrame + blockFrames);
        require(playback.sceneCount == 1 && session.beginRender().bpmAtTick(4 * quarter) == 120,
            "leave the previous bank and clock running after a refused scene");
        if (mode == MixedScene::ChangeNativeGain)
            require(std::abs(pcm[tempoSceneFrame] + 0.30f) < 0.01f, "keep the old source gain after rejecting No Stop");
        return;
    }
    if (mode == MixedScene::Retain) sceneWithGain(*store,session,20,201,-6.0);
    else scene(*store, session, 20);
    playback.through(last);
    checkTempoScene(*store, session, playback);
    require(pcm[rate / 8] > 0.54f, "the fixed source plays before the tempo scene");
    const auto fixedAfter = pcm[static_cast<std::size_t>(tempoSceneFrame + rate / 8)];
    require(legato ? fixedAfter > 0.54f : fixedAfter == 0, "the fixed track restarts or stops on the scene bar");
    std::array<LaunchTrackStatus,4> after{};
    require(session.readLaunchStatus(after,status) && status.trackCount == 2, "read both tracks after the scene");
    if (legato) {
        require(after[0].clip == 102 && after[0].run != before[0].run && after[1].clip == 202,
            "the native Legato cue and fixed restart launch new runs");
        const auto edges = std::span(playback.edges).first(playback.count);
        require(std::ranges::any_of(edges, [](const auto& edge) {
            return edge.track == 1 && edge.newClip == 102 && edge.sample == tempoSceneFrame &&
                std::abs(edge.sourceTick - quarter) <= ticksPerFrame;
        }), "native Legato inherits the old source phase at the bar");
    } else {
        require(after[0].clip == 101 && after[0].run == before[0].run && after[1].clip == 0,
            "the fixed track stops while No Stop retains the native run");
    }
    constexpr std::array<float,4> retainedPeaks{-.30f, -.35f, -.25f, -.30f};
    constexpr std::array<float,4> legatoPeaks{-.6f, -.7f, -.8f, -.5f};
    const auto error = checkTempoPulses(pcm, legato ? legatoPeaks : retainedPeaks, 0.08f,
        legato ? "Mixed Legato" : "Mixed retained");
    require(error <= 2, "the native track keeps its phase after the mixed scene");
}

void absoluteWarpFallback() {
    test::TempDirectory folder{"session_scene_warp", "absolute-fallback"};
    auto store = sceneProject(folder.path());
    enableTempoScene(*store);
    addClip(*store, folder.path(), 101, 20, 3, true);
    store->db().exec("UPDATE clips SET time_base=1,pos_ticks=NULL,pos_ns=0,length_ticks=NULL,"
        "length_ns=250000000,loop_enabled=0,loop_len_ticks=NULL WHERE id=101");
    const auto model = rows::readModel(*store);
    const auto source = readClipProject(*store,model,true);
    TempoMap clock;
    clock.events = {{0,120,0},{4 * quarter,140,0}};
    std::vector<std::string> problems;
    // Enough PCM for 250 ms of prepared stereo, below the full native source.
    const auto bank = prepareStoredLaunchProject(*source,clock,rate,2,blockFrames,1,problems,128000,140.0);
    require(bank && problems.empty(), "prepare an absolute-time Warp slot on the fallback bank");
    const auto* clip = bank->clip(101);
    const auto slots = bank->view().slots;
    require(clip && !clip->followsTempo() && slots.size() == 1 && clip->frames() == rate / 4 &&
        clip->frameForTick(slots[0].endTick,bank->bpm()) == rate / 4,
        "the fixed bank preserves all 250 ms of the absolute-time slot");
}

void pinUnwarpedOneShot(Store& store, std::int64_t startNs, std::int64_t durationNs) {
    auto& db = store.db();
    db.exec("UPDATE audio_clips SET warp_enabled=0,warp_markers=NULL WHERE clip_id=101");
    SQLite::Statement clip(db,"UPDATE clips SET time_base=1,pos_ticks=NULL,pos_ns=0,length_ticks=NULL,"
                              "length_ns=?,loop_enabled=0,loop_len_ticks=NULL WHERE id=101");
    clip.bind(1,durationNs);
    clip.exec();
    SQLite::Statement pins(db,"INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(101,?,?)");
    pins.bind(1,startNs);
    pins.bind(2,startNs + durationNs);
    pins.exec();
}

enum class PhysicalTempoChange { None, Early, Late, ChangedGain };

std::vector<float> renderPhysicalOneShot(Store& store, bool ram, std::int64_t span, PhysicalTempoChange change) {
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(), "the pinned one-shot opens without source errors");

    std::vector<float> pcm(static_cast<std::size_t>(span + blockFrames));
    Playback playback{session, pcm};
    playback.paged = !ram;
    scene(store,session,10);
    const auto cut = change == PhysicalTempoChange::Early ? 12000 : span - 9000;
    const auto cutTick = cut * quarter / (rate / 2);
    playback.through(cut - 100);
    const auto status = [&] {
        std::array<LaunchTrackStatus,1> tracks{};
        LaunchStatusHeader header;
        require(session.readLaunchStatus(tracks,header) && header.trackCount == 1 &&
                tracks.front().clip == 101 && tracks.front().run > 0,
                "the persisted unwarped slot owns an active driver run");
        return tracks.front();
    };
    const auto before = status();
    const LaunchClipRegionEdit region{1,101,session.launchIncarnation(),before.run,
        {quarter / 8,2 * quarter,{}},ClipUpdateRate::None};
    if (!ram) {
        require(!session.postLaunchClipRegionEdit(region), "paged regions still reject direct live edits");
        const LaunchClipGainEdit gain{1,101,session.launchIncarnation(),before.run,0.5f,ClipUpdateRate::None};
        require(session.postLaunchClipGainEdit(gain), "post a live gain to the paged one-shot");
    }

    const auto changedGain = change == PhysicalTempoChange::ChangedGain;
    if (changedGain) store.db().exec("UPDATE clips SET gain_db=-6 WHERE id=101");
    if (change != PhysicalTempoChange::None) {
        const bool accepted = session.setExternalTempoAt(store,140,cutTick);
        if (changedGain) {
            require(!accepted && session.error().find("needs the active unwarped page bank") != std::string::npos,
                    "a changed stored gain cannot reuse or replace the active long page bank");
        } else {
            require(accepted, "ordinary performed tempo admits the pinned one-shot");
        }
    }
    playback.through(cut + 1);
    const auto after = status();
    require(after.run == before.run && after.remainingSamples == span - cut - 1,
            "the running slot and its physical remaining duration survive the tempo candidate");
    if (!ram) require(!session.postLaunchClipRegionEdit(region), "retiming preserves the paged region edit rule");
    const bool retimed = change != PhysicalTempoChange::None && !changedGain;
    require(session.beginRender().bpmAtTick(cutTick) == (retimed ? 140 : 120),
            "the driver selects the admitted clock with its source bank");
    if (!ram && retimed)
        require(session.refresh(store), "refresh the identical project after audible tempo adoption");

    playback.through(span + blockFrames);
    require(playback.count == 2 && playback.edges[0].newClip == 101 && playback.edges[0].sample == 0 &&
            playback.edges[1].newClip == 0 && playback.edges[1].sample == span,
            "the one-shot stops at its physical end sample");
    for (auto first = std::int64_t{3000}; first < span; first += 12000) {
        const auto expected = !ram && first >= cut ? .275f : .55f;
        require(std::abs(pcm[static_cast<std::size_t>(first)] - expected) < 1e-6f,
                "the absolute source pin keeps attack phase and the live gain");
    }
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
            "unwarped one-shot playback performs no callback allocation or file I/O");
    return pcm;
}

void supersededUnwarpedBank(Store& store, std::int64_t span) {
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    session.transport().play();
    constexpr auto onset = 3000;
    const auto target = span - 9000;
    const auto publish = target - 100;
    const auto tick = target * quarter / (rate / 2);
    std::vector<float> pcm(static_cast<std::size_t>(span));
    Playback playback{session,pcm};
    playback.paged = true;
    playback.through(publish);
    require(playback.count == 0, "the owner observes the long paged bank idle");
    store.db().exec("UPDATE clips SET gain_db=-6 WHERE id=101");
    const bool published = session.setExternalTempoAt(store,140,tick,[&] {
        scene(store,session,10);
        playback.through(publish + 1);
        require(playback.count == 1 && playback.edges[0].sample == publish,
                "the old driver starts the slot after the owner checked it");
        return true;
    });
    require(published && session.refresh(store), "a compatible refresh supersedes the new clock publication");
    playback.through(publish + onset + 1);
    require(session.beginRender().bpmAtTick(tick) == 120 &&
            std::abs(pcm[static_cast<std::size_t>(publish + onset)] - .55f) < 1e-6f,
            "the superseding publication still holds the audible bank and its clock");
    session.transport().play(false);
    require(session.beginRender().bpmAtTick(tick) == 140, "the driver admits the held clock after stop");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
            "holding the actual page bank adds no callback allocation or file reads");
}

void physicalOneShotsFollowExternalTempo() {
    for (const bool ram : {true,false}) {
        test::TempDirectory folder{"session_unwarped_one_shot", ram ? "ram" : "paged"};
        auto store = sceneProject(folder.path());
        // The long paged case cannot fit the bounded whole-window admission.
        // Loop and Legato still need their own admission.
        const auto seconds = ram ? 1 : 4;
        const auto pinNs = ram ? std::int64_t{50000000} : std::int64_t{50031250}; // paged: 2401.5 samples
        const int trimFrame = ram ? 2400 : 2402;
        addClip(*store,folder.path(),101,10,4 * seconds + 1,false,trimFrame + 3000,1,12000);
        auto& db = store->db();
        db.exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
                "INSERT INTO session_state(key,value) VALUES('transport.launchQuantization','0')");
        pinUnwarpedOneShot(*store,pinNs,static_cast<std::int64_t>(seconds) * 1000000000);
        db.exec(ram ? "UPDATE clip_launch SET ram_mode=1 WHERE clip_id=101"
                    : "UPDATE clip_launch SET ram_mode=0 WHERE clip_id=101");
        const auto span = static_cast<std::int64_t>(seconds) * rate;
        const auto reference = renderPhysicalOneShot(*store,ram,span,PhysicalTempoChange::None);
        if (ram)
            require(renderPhysicalOneShot(*store,ram,span,PhysicalTempoChange::Early) == reference,
                    "early external tempo preserves all resident one-shot PCM");
        require(renderPhysicalOneShot(*store,ram,span,PhysicalTempoChange::Late) == reference,
                "late external tempo preserves all PCM and the natural stop");
        if (!ram) {
            supersededUnwarpedBank(*store,span);
            store->db().exec("UPDATE clips SET gain_db=0 WHERE id=101");
            require(renderPhysicalOneShot(*store,ram,span,PhysicalTempoChange::ChangedGain) == reference,
                    "a source mismatch keeps the already audible long page bank");
        }
        std::printf("Physical unwarped %s: %lld exact frames through ordinary tempo updates\n",
                    ram ? "RAM" : "paged",static_cast<long long>(span));
    }
}

void retainPagedOneShotOnSlotRefresh() {
    test::TempDirectory folder{"session_unwarped_one_shot", "unrelated-slot"};
    auto store = sceneProject(folder.path());
    store->db().exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120)");
    addClip(*store,folder.path(),101,10,17,false,5402,1,12000);
    pinUnwarpedOneShot(*store,50031250,4000000000);
    store->db().exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=101");
    addClip(*store,folder.path(),102,20,3,false);
    constexpr auto span = 4 * rate;
    constexpr auto refreshFrame = 2 * rate;
    constexpr auto tempoTick = quarter / 2;
    const auto gain = [&](int clip) {
        OpRequest edit;
        edit.opType = "clip.setGain";
        edit.payload = {{"id", clip}, {"gain", -6.0}};
        require(OpJournal(*store).commit(edit).ok, "persist a slot gain change");
    };
    const auto render = [&](bool editSlots) {
        Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
        std::vector<float> pcm(span + blockFrames);
        Playback playback{session,pcm};
        playback.paged = true;
        scene(*store,session,10);
        playback.through(6000);
        require(session.setExternalTempoAt(*store,140,tempoTick),"start the performed variable tempo");
        playback.through(refreshFrame);
        if (editSlots) {
            const auto before = trackState(session);
            const auto swaps = session.graph().stats().swaps;
            gain(102);
            require(session.refresh(*store),"refresh the gain of a different stored slot");
            playback.through(refreshFrame + 1);
            const auto after = trackState(session);
            require(before.run != 0 && before.run == after.run && after.clip == 101 &&
                    after.remainingSamples == span - refreshFrame - 1 &&
                    session.graph().stats().swaps > swaps && session.problems().empty(),
                    "the edited graph adopts while the unchanged long source retains its warm page bank");

            const auto adopted = session.graph().stats().swaps;
            gain(101);
            require(session.refresh(*store),"prepare a refresh for the running stored slot");
            playback.through(refreshFrame + rate / 2);
            require(session.graph().stats().swaps == adopted && trackState(session).run == before.run,
                    "an incompatible replacement cannot drop the actual active reader");
        }
        playback.through(span + blockFrames);
        require(playback.count == 2 && playback.edges[0].sample == 0 && playback.edges[1].newClip == 0 &&
                playback.edges[1].sample == span && callbackAllocations == 0 && audio::callbackFileIo == 0,
                "the long slot reaches its physical end with no callback allocation or file I/O");
        return pcm;
    };
    const auto reference = render(false);
    require(render(true) == reference, "persisted independent and guarded source edits preserve the full one-shot PCM");
    std::printf("Paged source reused after a different slot edit: %d identical frames\n",span);
}

void relaunchEditedPinnedOneShot() {
    test::TempDirectory folder{"session_unwarped_one_shot", "pause-relaunch"};
    auto store = sceneProject(folder.path());
    auto& db = store->db();
    db.exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
            "INSERT INTO session_state(key,value) VALUES('transport.launchQuantization','0')");
    addClip(*store,folder.path(),101,10,17,false,5402,1,12000);
    pinUnwarpedOneShot(*store,50031250,4000000000);
    db.exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=101");
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    constexpr auto park = 120000;
    constexpr auto nextPulse = 123000;
    std::vector<float> pcm(nextPulse + 3000 + 16);
    Playback playback{session,pcm};
    playback.paged = true;
    scene(*store,session,10);
    playback.through(6000);
    require(session.setExternalTempoAt(*store,140,quarter/2),"perform a tempo change before the same-clock edit");
    playback.through(park);
    const auto old = trackState(session);
    const auto swaps = session.graph().stats().swaps;
    require(old.run != 0 && old.clip == 101,"start the original long pinned run");

    const auto editGain = [&](double gain) {
        OpRequest edit;
        edit.opType = "clip.setGain";
        edit.payload = {{"id", 101}, {"gain", gain}};
        require(OpJournal(*store).commit(edit).ok && session.refresh(*store),
                "prepare the overwritten slot at the already adopted clock");
    };
    editGain(-3.0);
    editGain(-6.020599913279624);
    require(session.problems().empty(),"the replacement has its ordinary source start prepared");
    playback.through(park + 1);
    session.transport().play(false);
    playback.through(park + 2);
    require(session.graph().stats().swaps == swaps && trackState(session).run == old.run,
            "global pause cannot replace the warm reader of a retained run");
    session.transport().play();
    playback.through(nextPulse + 2);
    require(trackState(session).run == old.run && std::abs(pcm[nextPulse + 1] - .55f) < 1e-6f,
            "global resume still hears the original late source phase");

    session.transport().play(false);
    OpRequest stop;
    stop.opType = "session.stopTrack";
    stop.payload = {{"track",1},{"incarnation",session.launchIncarnation()}};
    require(OpJournal(*store).commit(stop).ok,"queue explicit Slot Stop on the old graph");
    playback.through(nextPulse + 3);
    require(trackState(session).clip == 0,"explicit Slot Stop clears the retained run");
    playback.through(nextPulse + 4);
    require(session.graph().stats().swaps > swaps,"the stopped slot admits its prepared replacement");
    const auto launchStream = playback.first;
    OpRequest launch;
    launch.opType = "session.launchClip";
    launch.payload = {{"track",1},{"scene",10},{"clip",101},{"incarnation",session.launchIncarnation()}};
    require(OpJournal(*store).commit(launch).ok,"relaunch the edited slot");
    playback.through(launchStream + 3001);
    const auto next = trackState(session);
    require(next.clip == 101 && next.run != 0 && next.run != old.run &&
            std::abs(pcm[static_cast<std::size_t>(launchStream + 3000)] - .275f) < 1e-6f &&
            callbackAllocations == 0 && audio::callbackFileIo == 0,
            "relaunch hears the latest saved gain from the prepared start without callback allocation or I/O");
    std::printf("Paused pinned run resumed at .55 FS and restarted at .275 FS on the same clock\n");
}

void pausedSupersessionReturnsToSameClock(bool nonconstant) {
    test::TempDirectory folder{"session_unwarped_one_shot","pause-clock-alias"};
    auto store = sceneProject(folder.path());
    auto& db = store->db();
    db.exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
            "INSERT INTO session_state(key,value) VALUES('transport.launchQuantization','0');");
    constexpr auto boundary = quarter;
    if (nonconstant) {
        SQLite::Statement step(db,"INSERT INTO tempo_map(pos_ticks,bpm) VALUES(?,140)");
        step.bind(1,boundary);
        step.exec();
    }
    addClip(*store,folder.path(),101,10,17,false,5402,1,12000);
    pinUnwarpedOneShot(*store,50031250,4000000000);
    db.exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=101");
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    std::vector<float> pcm(130000);
    Playback playback{session,pcm};
    playback.paged = true;
    scene(*store,session,10);
    playback.through(122999);
    const auto before = trackState(session);
    const auto swaps = session.graph().stats().swaps;
    const auto bpmA = nonconstant ? 140.0 : 120.0;
    require(before.clip == 101 && before.run > 0 && session.beginRender().bpmAtTick(boundary) == bpmA,
            "driver adopts the original clock A with a warm late physical run");

    db.exec("UPDATE clips SET gain_db=-6.020599913279624 WHERE id=101");
    SQLite::Statement edit(db,"INSERT INTO tempo_map(pos_ticks,bpm) VALUES(?,150) ON CONFLICT(pos_ticks) DO UPDATE SET bpm=excluded.bpm");
    edit.bind(1,boundary);
    edit.exec();
    require(session.refresh(*store),"publish a different authored clock B and a changed source");
    playback.through(123000);
    require(trackState(session).run == before.run && session.graph().stats().swaps == swaps &&
            session.beginRender().bpmAtTick(boundary) == bpmA,"driver holds the original A while B is pending");

    const char* restore = nonconstant ? "UPDATE tempo_map SET bpm=140 WHERE pos_ticks=?" : "DELETE FROM tempo_map WHERE pos_ticks=?";
    SQLite::Statement restoreClock(db,restore);
    restoreClock.bind(1,boundary);
    restoreClock.exec();
    require(session.refresh(*store),"publish C with the exact original A clock but changed source");
    require(session.problems().empty(),"restoring the adopted clock primes the edited slot");
    session.transport().play(false);
    playback.through(123001);
    const auto swapsPaused = session.graph().stats().swaps;
    require(trackState(session).run == before.run,"global pause keeps the old physical run identity");
    session.transport().play();
    playback.through(123002);
    const auto resumed = pcm[123001];

    session.transport().play(false);
    OpRequest stop;
    stop.opType = "session.stopTrack";
    stop.payload = {{"track",1},{"incarnation",session.launchIncarnation()}};
    require(OpJournal(*store).commit(stop).ok,"queue explicit Slot Stop");
    playback.through(123004);
    require(trackState(session).clip == 0,"stop the retained run");
    const auto relaunch = playback.first;
    OpRequest launch;
    launch.opType = "session.launchClip";
    launch.payload = {{"track",1},{"scene",10},{"clip",101},{"incarnation",session.launchIncarnation()}};
    require(OpJournal(*store).commit(launch).ok,"relaunch after explicit Stop");
    playback.through(relaunch + 3001);
    const auto restarted = pcm[static_cast<std::size_t>(relaunch+3000)];
    std::printf("%s A -> B pending -> A: swaps %lld -> %lld on pause, late resume %.6f, edited restart %.6f, clip %lld\n",
                nonconstant ? "two-segment" : "steady",static_cast<long long>(swaps),static_cast<long long>(swapsPaused),
                static_cast<double>(resumed),static_cast<double>(restarted),static_cast<long long>(trackState(session).clip));
    require(swapsPaused == swaps && std::abs(resumed - .55f) < 1e-6f && std::abs(restarted - .275f) < 1e-6f &&
            callbackAllocations == 0 && audio::callbackFileIo == 0,
            "equal clock across superseded B preserves old PCM and prepares a correct new start");
}

void coldPagedOneShotOnAuthoredRamp() {
    test::TempDirectory folder{"session_unwarped_one_shot", "cold-ramp"};
    auto store = sceneProject(folder.path());
    auto& db = store->db();
    SQLite::Statement tempo(db,"INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,1),(?,140,0)");
    tempo.bind(1,lastBeat * quarter);
    tempo.exec();
    constexpr auto span = 4 * rate;
    constexpr auto pinNs = 50031250; // 2401.5 samples at 48 kHz
    addClip(*store,folder.path(),101,10,17,false,5402,1,12000);
    pinUnwarpedOneShot(*store,pinNs,4000000000);
    db.exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=101");

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    session.transport().play();
    const auto launch = frameAtBeat(4);
    const auto finish = launch + span + blockFrames;
    std::vector<float> pcm(static_cast<std::size_t>(finish));
    Playback playback{session,pcm};
    playback.paged = true;
    playback.through(blockFrames);
    scene(*store,session,10);
    playback.through(finish);

    int pulses = 0;
    for (auto frame = launch + 3000; frame < launch + span; frame += 12000)
        if (std::abs(pcm[static_cast<std::size_t>(frame)] - .55f) < 1e-6f) ++pulses;
    const auto stop = playback.count == 2 ? playback.edges[1].sample : -1;
    std::printf("Cold paged one-shot on authored ramp: %d/16 attacks, launch %lld, stop %lld\n",
        pulses,static_cast<long long>(playback.edges[0].sample),static_cast<long long>(stop));
    require(session.problems().empty() && pulses == 16, "a fresh project primes and streams the long physical slot");
    require(playback.count == 2 && playback.edges[0].newClip == 101 && playback.edges[0].sample == launch &&
            playback.edges[1].newClip == 0 && stop == launch + span,
            "the ramp quantizes the launch but preserves the complete physical one-shot");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
            "cold paged playback starts without allocation or file reads in the callback");
}

void physicalOneShotToWarpLegato() {
    test::TempDirectory folder{"session_unwarped_one_shot", "legato-source"};
    auto store = sceneProject(folder.path());
    enableTempoScene(*store);
    addClip(*store,folder.path(),101,10,17,false,0,1,12000);
    pinUnwarpedOneShot(*store,100000000,4000000000);
    addClip(*store,folder.path(),102,20,3,true);

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    std::vector<float> pcm(static_cast<std::size_t>(tempoSceneFrame + blockFrames));
    Playback playback{session, pcm};
    scene(*store,session,10);
    playback.through(blockFrames);
    scene(*store,session,20);
    playback.through(tempoSceneFrame + blockFrames);
    checkTempoScene(*store,session,playback);
    require(playback.count == 2 && playback.edges[1].newClip == 102 &&
            playback.edges[1].sample == tempoSceneFrame && playback.edges[1].sourceTick == quarter,
            "the native Legato target inherits four elapsed beats modulo its three-beat source loop");
    require(std::abs(pcm[static_cast<std::size_t>(tempoSceneFrame)] + .6f) < .02f,
            "the changed-tempo native Warp attack keeps the inherited phase after a pinned source");
}

void physicalOneShotRetainedByTempoScene() {
    for (const bool ram : {true,false}) {
        test::TempDirectory folder{"session_unwarped_one_shot", ram ? "no-stop-ram" : "no-stop-paged"};
        auto store = sceneProject(folder.path());
        enableTempoScene(*store);
        constexpr auto span = 4 * rate;
        addClip(*store,folder.path(),101,10,17,false,5402,1,12000);
        pinUnwarpedOneShot(*store,50031250,4000000000); // Start is 2401.5 frames.
        store->db().exec("INSERT INTO clip_slots(track_id,scene_id,has_stop) VALUES(1,20,0)");
        if (!ram) store->db().exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=101");

        const auto render = [&](bool change, bool changeGain = false) {
            Session session;
            session.graph().setFadeFrames(0);
            if (!session.load(*store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
            std::vector<float> pcm(span + blockFrames);
            Playback playback{session,pcm};
            playback.paged = !ram;
            scene(*store,session,10);
            playback.through(blockFrames);
            const auto status = [&] {
                std::array<LaunchTrackStatus,1> tracks{};
                LaunchStatusHeader header;
                require(session.readLaunchStatus(tracks,header) && header.trackCount == 1 && tracks[0].clip == 101,
                        "the pinned one-shot keeps its active Session slot");
                return tracks[0];
            };
            const auto before = status();
            require(session.postLaunchClipGainEdit({1,101,session.launchIncarnation(),before.run,0.5f,ClipUpdateRate::None}),
                    "set live gain on the active pinned run");
            playback.through(tempoSceneFrame - 2 * blockFrames);
            if (changeGain) sceneWithGain(*store,session,20,101,-6.0);
            else if (change) scene(*store,session,20);
            if (change && !changeGain && !session.error().empty())
                throw std::runtime_error("No Stop one-shot: " + session.error());
            playback.through(tempoSceneFrame + 1);
            const auto after = status();
            require(after.run == before.run && after.remainingSamples == span - tempoSceneFrame - 1,
                    "the scene preserves the run and its exact physical remaining frames");
            if (change && !changeGain) checkTempoScene(*store,session,playback);
            if (changeGain) {
                require(session.error().find("unchanged unwarped") != std::string::npos && playback.sceneCount == 1 &&
                        session.beginRender().bpmAtTick(4 * quarter) == 120,
                        "No Stop refuses a different source gain and keeps the old scene clock");
            }
            playback.through(span + blockFrames);
            require(playback.count == 2 && playback.edges[0].sample == 0 && playback.edges[1].newClip == 0 &&
                    playback.edges[1].sample == span, "the one-shot stops exactly on its original physical end");
            require(pcm[3000] == .275f && pcm[span - 9000] == .275f && callbackAllocations == 0 && audio::callbackFileIo == 0,
                    "the pinned source and live gain stay audible without callback allocation or file I/O");
            return pcm;
        };

        const auto reference = render(false);
        require(render(true) == reference, "No Stop keeps every physical one-shot PCM sample across the tempo scene");
        if (!ram)
            require(render(true,true) == reference, "a refused source change leaves the entire active page bank sounding");
        std::printf("No Stop physical %s one-shot: scene %lld, stop %d, identical PCM\n",ram ? "RAM" : "paged",
                    static_cast<long long>(tempoSceneFrame),span);
    }
}

std::unique_ptr<Store> physicalLoopProject(const std::filesystem::path& folder, bool ram) {
    auto store = sceneProject(folder);
    addClip(*store,folder,101,10,5,false,3000,1,6000);
    pinUnwarpedOneShot(*store,50000000,450000000);
    auto& db = store->db();
    db.exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
            "UPDATE clips SET loop_enabled=1,loop_start_ticks=1153152,loop_len_ticks=2882880;"
            "UPDATE clip_playback_details SET loop_begin_ns=100000000,loop_end_ns=350000000;");
    db.exec(ram ? "UPDATE clip_launch SET ram_mode=1" : "UPDATE clip_launch SET ram_mode=0");
    return store;
}

void physicalLoopRetainedByTempoScene() {
    constexpr auto firstLap = 14400;
    constexpr auto period = 12000;
    constexpr auto followStop = firstLap + 8 * period;
    for (const bool ram : {true, false}) {
        test::TempDirectory folder{"unwarped_loop_scene", ram ? "ram" : "paged"};
        auto store = physicalLoopProject(folder.path(), ram);
        store->db().exec("UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=20;"
                         "INSERT INTO clip_slots(track_id,scene_id,has_stop) VALUES(1,20,0);"
                         "UPDATE clip_launch SET follow_enabled=1,follow_linked=1,follow_loop_count=9,follow_a=1 "
                         "WHERE clip_id=101");
        store.reset();
        StoreError openError{};
        store = Store::open(folder.path() / "song.adi", openError);
        require(static_cast<bool>(store), "reopen the saved pinned-loop scene");

        const auto render = [&](bool launchTempoScene, bool changeGain = false) {
            Session session;
            session.graph().setFadeFrames(0);
            if (!session.load(*store, {}, {2, rate, blockFrames})) throw std::runtime_error(session.error());
            std::vector<float> pcm(followStop + blockFrames);
            Playback playback{session, pcm};
            playback.paged = !ram;
            scene(*store, session, 10);
            playback.through(blockFrames);
            const auto initial = trackState(session);
            require(initial.clip == 101 && initial.run != 0 &&
                    session.postLaunchClipGainEdit({1, 101, session.launchIncarnation(), initial.run,
                                                    0.5f, ClipUpdateRate::None}),
                    "set live gain on the running pinned loop");
            playback.through(tempoSceneFrame - 2 * blockFrames);
            if (changeGain) sceneWithGain(*store, session, 20, 101, -6.0);
            else if (launchTempoScene) {
                scene(*store, session, 20);
                if (!session.error().empty()) throw std::runtime_error("No Stop pinned loop: " + session.error());
            }
            playback.through(tempoSceneFrame + 1);
            const auto retained = trackState(session);
            require(retained.clip == 101 && retained.run == initial.run && retained.completedLoops == 7 &&
                    playback.count == 1, "the scene retains the active clip, run and physical lap count");
            if (changeGain) {
                require(session.error().find("unchanged unwarped") != std::string::npos &&
                        playback.sceneCount == 1 && session.beginRender().bpmAtTick(4 * quarter) == 120,
                        "a changed saved source gain blocks the tempo scene without interrupting playback");
            } else if (launchTempoScene) {
                checkTempoScene(*store, session, playback);
            }

            playback.through(followStop - 1);
            const auto finalLap = trackState(session);
            require(finalLap.clip == 101 && finalLap.run == initial.run && finalLap.completedLoops == 8,
                    "the original run finishes the eighth wrap before linked Follow Stop");
            playback.through(followStop + blockFrames);
            require(playback.count == 2 && playback.edges[0].sample == 0 && playback.edges[1].newClip == 0 &&
                    playback.edges[1].sample == followStop && trackState(session).clip == 0,
                    "linked Follow Stop ends the same run after its ninth physical lap");
            require(pcm[18600] == .275f && pcm[108600] == .275f &&
                    std::all_of(pcm.begin() + followStop, pcm.end(), [](float frame) { return frame == 0; }),
                    "the original gain is audible on both sides of the scene and the stop is silent");
            require(session.problems().empty() && callbackAllocations == 0 && audio::callbackFileIo == 0,
                    "the retained loop has ready PCM with no callback allocation or file I/O");
            return pcm;
        };

        const auto steady = render(false);
        require(render(true) == steady, "the tempo scene preserves every sample of the pinned loop");
        if (!ram) require(render(true, true) == steady, "a refused source edit keeps the original paged run");
        std::printf("No Stop physical %s loop: scene %lld, Follow Stop %d, identical PCM\n",
                    ram ? "RAM" : "paged", static_cast<long long>(tempoSceneFrame), followStop);
    }
}

void physicalLoopRegionRetainedByTempoScene() {
    test::TempDirectory folder{"unwarped_loop_scene", "live-region"};
    auto store = physicalLoopProject(folder.path(), true);
    store->db().exec("UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=20;"
                     "INSERT INTO clip_slots(track_id,scene_id,has_stop) VALUES(1,20,0)");
    const auto render = [&](bool change) {
        Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(*store, {}, {2, rate, blockFrames})) throw std::runtime_error(session.error());
        std::vector<float> pcm(110000);
        Playback playback{session, pcm};
        scene(*store, session, 10);
        playback.through(blockFrames);
        const auto initial = trackState(session);
        const LaunchClipRegionEdit edit{1, 101, session.launchIncarnation(), initial.run,
            {quarter / 8, quarter * 8 / 10, LaunchClipLoopRegion{quarter / 4, quarter * 6 / 10}}, ClipUpdateRate::None};
        require(session.postLaunchClipRegionEdit(edit), "queue live loop bounds on the pinned RAM clip");
        playback.through(70000);
        require(trackState(session).loopStartTick == quarter / 4, "the live loop bounds apply before the tempo scene");
        if (change) scene(*store, session, 20);
        playback.through(static_cast<std::int64_t>(pcm.size()));
        if (change) checkTempoScene(*store, session, playback);
        require(trackState(session).run == initial.run && playback.count == 1,
                "No Stop keeps the original run after the live loop edit");
        return pcm;
    };
    const auto steady = render(false);
    require(render(true) == steady, "No Stop preserves every PCM sample with the live loop bounds");
}

void persistedPhysicalLoopTempo() {
    bool matched = true;
    for (const bool ram : {true,false}) {
        for (const bool linkedFollow : {false,true}) {
            test::TempDirectory folder{"unwarped_loop_link",ram ? "ram" : "paged"};
            auto store = physicalLoopProject(folder.path(),ram);
            auto& db = store->db();
            if (linkedFollow)
                db.exec("UPDATE clip_launch SET follow_enabled=1,follow_linked=1,follow_loop_count=3,follow_a=1");
            if (!ram) addClip(*store,folder.path(),102,20,3,false);
            constexpr auto firstLap = 14400;
            constexpr auto period = 12000;
            constexpr auto linkedStop = firstLap + 2 * period;
            constexpr auto performedStop = 84000;
            const auto render = [&](bool retime) {
                Session session;
                session.graph().setFadeFrames(0);
                if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
                std::vector<float> pcm(97000);
                Playback playback{session,pcm};
                playback.paged = !ram;
                scene(*store,session,10);
                playback.through(6000);
                if (retime)
                    require(session.setExternalTempoAt(*store,140,quarter/2), "perform the ordinary 140 BPM boundary");
                if (retime && !ram) {
                    playback.through(6001);
                    const auto swaps = session.graph().stats().swaps;
                    OpRequest edit;
                    edit.opType = "clip.setGain";
                    edit.payload = {{"id", 102}, {"gain", -6.0}};
                    require(OpJournal(*store).commit(edit).ok && session.refresh(*store),
                            "refresh a different slot during the pinned paged loop");
                    playback.through(6002);
                    require(session.graph().stats().swaps > swaps && session.problems().empty(),
                            "the changed graph adopts with the original loop reader still warm");
                }
                playback.through(firstLap);
                const auto beforeWrap = trackState(session);
                playback.through(firstLap+1);
                const auto afterWrap = trackState(session);
                if (!linkedFollow) {
                    OpRequest stop;
                    stop.opType = "session.stopTrack";
                    stop.payload = {{"track",1},{"incarnation",session.launchIncarnation()}};
                    require(OpJournal(*store).commit(stop).ok, "queue manual Slot Stop on the active clock");
                }
                playback.through(static_cast<std::int64_t>(pcm.size()));
                const auto stop = playback.count == 2 && playback.edges[1].newClip == 0 ? playback.edges[1].sample : -1;
                const auto expected = linkedFollow ? linkedStop : retime ? performedStop : 96000;
                std::printf("Physical loop %s %s link=%d: wraps %llu/%llu; pulse .55 -> %.6f at 18600; stop %lld (expected %d)\n",
                    ram ? "RAM" : "paged",linkedFollow ? "linked Follow" : "manual",
                    retime, static_cast<unsigned long long>(beforeWrap.completedLoops),
                    static_cast<unsigned long long>(afterWrap.completedLoops),static_cast<double>(pcm[18600]),
                    static_cast<long long>(stop),expected);
                for (const auto& problem : session.problems()) std::printf("  %s\n",problem.c_str());
                matched &= playback.count == 2 && playback.edges[0].sample == 0 && stop == expected &&
                    beforeWrap.run != 0 && beforeWrap.run == afterWrap.run &&
                    beforeWrap.completedLoops == 0 && afterWrap.completedLoops == 1 &&
                    session.problems().empty() && callbackAllocations == 0 && audio::callbackFileIo == 0;
                return pcm;
            };
            const auto steady = render(false);
            const auto retimed = render(true);
            const auto end = linkedFollow ? linkedStop : performedStop;
            matched &= std::equal(steady.begin(),steady.begin()+end,retimed.begin()) &&
                std::all_of(retimed.begin()+end,retimed.end(),[](float frame) {return frame == 0;});
        }
    }
    require(matched, "pinned loops preserve physical laps, follow and quantized stop under ordinary tempo");
}

void physicalLoopFollowUsesTargetGrid() {
    test::TempDirectory folder{"unwarped_loop_link","follow-target-grid"};
    auto store = physicalLoopProject(folder.path(),true);
    addClip(*store,folder.path(),102,20,2,false,1200);
    store->db().exec("UPDATE clip_launch SET follow_enabled=1,follow_linked=1,follow_loop_count=3,follow_a=4 WHERE clip_id=101;"
                    "UPDATE clip_launch SET launch_quant=12 WHERE clip_id=102");
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    constexpr auto earlyGrid = 37714, actualGrid = 42857;
    constexpr auto attack = actualGrid + 1029; // 1200 source frames at 120→140 BPM
    std::vector<float> pcm(attack + blockFrames);
    Playback playback{session,pcm};
    scene(*store,session,10);
    playback.through(6000);
    require(session.setExternalTempoAt(*store,140,quarter / 2), "perform tempo before the linked lap deadline");
    playback.through(earlyGrid + 1);
    require(playback.count == 1 && trackState(session).clip == 101,
            "the earlier musical grid does not steal time from the linked physical lap");
    playback.through(attack + blockFrames);
    require(playback.count == 2 && playback.edges[1].newClip == 102 && playback.edges[1].sample == actualGrid,
            "linked Follow resolves the explicit sixteenth grid after the actual third lap");
    const auto peak = std::max_element(pcm.begin()+attack-4,pcm.begin()+attack+5);
    require(std::abs((peak - pcm.begin()) - attack) <= 1 && std::abs(*peak - .55f) < .03f &&
            session.problems().empty() &&
            callbackAllocations == 0 && audio::callbackFileIo == 0,
            "the quantized target plays the correct source and adds no callback I/O or allocation");
}

void physicalFollowKeepsCoincidentGridSample() {
    test::TempDirectory folder{"unwarped_loop_link","follow-grid-tie"};
    auto store = physicalLoopProject(folder.path(),true);
    addClip(*store,folder.path(),102,20,2,false);
    store->db().exec("UPDATE tempo_map SET bpm=140 WHERE pos_ticks=0;"
                    "UPDATE clip_playback_details SET loop_end_ns=157145833 WHERE clip_id=101;"
                    "UPDATE clip_launch SET follow_enabled=1,follow_linked=1,follow_loop_count=1,follow_a=4 WHERE clip_id=101;"
                    "UPDATE clip_launch SET launch_quant=12 WHERE clip_id=102");
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    constexpr auto coincident = 5143; // source frames 2400–7543, and tick quarter/4 rounds here
    std::vector<float> pcm(10543);
    Playback playback{session,pcm};
    scene(*store,session,10);
    playback.through(coincident);
    require(playback.count == 1, "linked Follow waits for its complete physical lap");
    playback.through(static_cast<std::int64_t>(pcm.size()));
    std::printf("Physical Follow at a coincident sixteenth grid: %lld (expected %d)\n",
        static_cast<long long>(playback.edges[1].sample),coincident);
    require(playback.count == 2 && playback.edges[1].newClip == 102 && playback.edges[1].sample == coincident &&
            playback.edges[1].tick == quarter / 4,
            "a target sixteenth rounded onto the physical lap is eligible on that sample");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
            "settling the coincident grid is allocation and I/O free");
}

void laterFollowKeepsEarlierLegatoForecast() {
    test::TempDirectory folder{"unwarped_loop_link","legato-before-follow"};
    auto store = physicalLoopProject(folder.path(),true);
    // The inherited source at 82,288 is beyond the native bank's initial pages.
    addClip(*store,folder.path(),102,20,2,true,82288,1,192000);
    store->db().exec("UPDATE clip_launch SET follow_enabled=1,follow_linked=1,follow_loop_count=20,follow_a=1 WHERE clip_id=101;"
                    "UPDATE clip_launch SET launch_quant=8,ram_mode=0 WHERE clip_id=102");
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    constexpr auto due = 22286; // one quarter under the performed 120→140 clock
    const auto expected = std::int64_t{10286} * quarter / (rate / 2);
    std::vector<float> pcm(due + blockFrames);
    Playback playback{session,pcm};
    playback.paged = true;
    scene(*store,session,10);
    playback.through(6000);
    require(session.setExternalTempoAt(*store,140,quarter / 2), "retime with the linked physical Follow still queued");
    playback.through(17000);
    const LaunchRequest request{LaunchRequestKind::SlotPress,1,20,102,session.launchIncarnation()};
    require(session.postLaunchGesture(std::array{request}), "queue the earlier native paged Legato slot");
    playback.through(17000 + blockFrames);
    const auto queued = trackState(session).queued;
    require(queued.clip == 102 && queued.tick == quarter && queued.legatoSourceTick &&
            std::abs(*queued.legatoSourceTick - expected) <= 1,
            "a much later linked Follow permits prefetch of the earlier physical Legato phase");
    playback.through(due + blockFrames);
    require(playback.count == 2 && playback.edges[1].newClip == 102 && playback.edges[1].sample == due &&
            std::abs(playback.edges[1].sourceTick - expected) <= 1 && pcm[due] < -.4f,
            "the actual earlier Legato launch starts on its prefetched source attack");
    require(session.problems().empty() && callbackAllocations == 0 && audio::callbackFileIo == 0,
            "prefetching the earlier Legato phase keeps callback work allocation and I/O free");
}

void pinnedLiveRegionEdit(std::string_view kind, bool retime, bool flatRebase = false) {
    test::TempDirectory folder{"physical_live_region","resident"};
    auto store = sceneProject(folder.path());
    auto& db = store->db();
    db.exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);");
    addClip(*store,folder.path(),101,10,7,false,3000,1,12000);
    pinUnwarpedOneShot(*store,100000000,1000000000);
    constexpr auto tick = [](std::int64_t frame) { return frame * quarter / (rate / 2); };
    if (kind == "loop") {
        db.exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=2306304,loop_len_ticks=8072064;"
                "UPDATE clip_playback_details SET loop_begin_ns=200000000,loop_end_ns=900000000;");
    }
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(), "open a resident non-Legato source with absolute pins");
    std::vector<float> pcm(54000);
    Playback playback{session,pcm};
    scene(*store,session,10);
    playback.through(6000);
    if (retime) require(session.setExternalTempoAt(*store,140,quarter / 2), "perform 140 BPM after frame 12000");
    constexpr auto editAt = 18000;
    playback.through(editAt);
    const auto before = trackState(session);
    require(before.clip == 101 && before.run != 0 && before.completedLoops == 0, "the non-Legato run is active at edit time");
    require(session.beginRender().bpmAtTick(quarter / 2) == (retime ? 140 : 120), "the driver has selected the expected clock");
    LaunchClipRegion region{tick(4800),tick(52800),{}};
    std::int64_t expected = 0;
    if (kind == "end") {
        region.end = tick(40000);
        expected = 40000 - 4800;
    } else if (kind == "start") {
        region.start = tick(30000);
        expected = editAt + 52800 - 30000;
    } else if (kind == "loop") {
        region.loop = LaunchClipLoopRegion{tick(12000),tick(36000)};
        expected = 36000 - 4800;
    } else {
        throw std::runtime_error("expected end, start, or loop");
    }
    const LaunchClipRegionEdit edit{1,101,session.launchIncarnation(),before.run,region,ClipUpdateRate::None};
    require(session.postLaunchClipRegionEdit(edit), "the exact physical marker edit is accepted by the RAM path");
    if (flatRebase) {
        playback.through(editAt + 1);
        require(session.setExternalTempo(*store,20), "keep the active physical edit across a new tick-zero tempo");
    }
    std::int64_t actual = -1;
    if (kind == "loop") {
        while (playback.first <= expected) {
            const auto at = playback.first;
            playback.through(at + 1);
            if (trackState(session).completedLoops != 0) {
                actual = at;
                break;
            }
        }
    } else {
        playback.through(expected + blockFrames);
        if (playback.count == 2) actual = playback.edges[1].sample;
    }
    const auto pulse = kind == "end" ? 34200 : 27000;
    std::printf("%.*s at %d BPM: expected boundary %lld, actual %lld, difference %lld, pulse[%d]=%.6f\n",
        static_cast<int>(kind.size()),kind.data(),flatRebase ? 20 : retime ? 140 : 120,static_cast<long long>(expected),
        static_cast<long long>(actual),static_cast<long long>(actual-expected),pulse,static_cast<double>(pcm[static_cast<std::size_t>(pulse)]));
    if (kind != "loop") require(actual == expected && pcm[static_cast<std::size_t>(pulse)] == .55f,
                                "stop at the physical End without dropping the selected source pulse");
    else require(actual == expected,"wrap exactly on the edited physical Loop End");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
            "physical marker editing adds no callback allocations or file reads");
}

void pinnedRepeatUsesItsLaunchClock() {
    for (const bool ram : {true,false}) {
        for (const auto variant : {0,1,2}) {
            const bool loop = variant == 1;
            const bool explicitGrid = variant == 2;
            test::TempDirectory folder{"session_pinned_repeat",ram ? "ram" : "paged"};
            auto store = sceneProject(folder.path());
            auto& db = store->db();
            SQLite::Statement tempo(db,"INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,1),(?,140,0)");
            tempo.bind(1,lastBeat * quarter);
            tempo.exec();
            db.exec("INSERT INTO session_state(key,value) VALUES('transport.launchQuantization','0')");
            addClip(*store,folder.path(),101,10,2,false,5400);
            pinUnwarpedOneShot(*store,50000000,loop ? 400000000 : 200000000);
            if (loop) {
                db.exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=1153152,loop_len_ticks=2882880;"
                        "UPDATE clip_playback_details SET loop_begin_ns=100000000,loop_end_ns=350000000");
            }
            SQLite::Statement controls(db,"UPDATE clip_launch SET launch_mode=3,launch_quant=?,ram_mode=?");
            controls.bind(1,explicitGrid ? 12 : 0); // explicit sixteenth, otherwise Global None
            controls.bind(2,ram ? 1 : 0);
            controls.exec();

            Session session;
            session.graph().setFadeFrames(0);
            if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
            const auto press = [&](bool release) {
                OpRequest gesture;
                gesture.opType = "session.launchClip";
                gesture.payload = {{"track",1},{"scene",10},{"clip",101},
                                   {"incarnation",session.launchIncarnation()},{"release",release}};
                require(OpJournal(*store).commit(gesture).ok,"send a persisted Repeat gesture");
            };
            const auto physical = static_cast<std::int64_t>(loop ? 14400 : 9600);
            const std::array<std::int64_t,3> starts{0,explicitGrid ? frameAtBeat(.25) : physical,
                explicitGrid ? frameAtBeat(.5) : 2 * physical};
            const auto through = starts.back() + physical + (loop ? 12601 : blockFrames);
            std::vector<float> pcm(static_cast<std::size_t>(through));
            Playback playback{session,pcm};
            playback.paged = !ram;
            press(false);
            playback.through(starts.back() + 1);
            press(true);
            playback.through(through);
            std::printf("Pinned Repeat %s %s: starts %lld/%lld/%lld, pulse %.6f, transitions %zu\n",
                        ram ? "RAM" : "paged",loop ? "loop" : explicitGrid ? "grid" : "one-shot",
                        static_cast<long long>(playback.edges[0].sample),static_cast<long long>(playback.edges[1].sample),
                        static_cast<long long>(playback.edges[2].sample),static_cast<double>(pcm[3000]),playback.count);
            for (const auto& problem : session.problems()) std::printf("  %s\n",problem.c_str());
            for (std::size_t i = 0; i < starts.size(); ++i) {
                require(playback.edges[i].newClip == 101 && playback.edges[i].sample == starts[i] &&
                        playback.edges[i].sourceTick == quarter / 10 &&
                        std::abs(pcm[static_cast<std::size_t>(starts[i] + 3000)] - .55f) < 1e-6f,
                        "held Repeat starts at the selected timer and restarts from the pinned source Start");
            }
            if (loop) {
                const auto wrap = starts.back() + physical;
                require(playback.count == 3 && trackState(session).completedLoops == 2 &&
                        std::abs(pcm[static_cast<std::size_t>(wrap + 600)] - .55f) < 1e-6f &&
                        std::abs(pcm[static_cast<std::size_t>(wrap + 12600)] - .55f) < 1e-6f,
                        "release cancels retriggers and resumes the ordinary pinned Loop Begin");
            } else {
                require(playback.count == 4 && playback.edges[3].newClip == 0 &&
                        playback.edges[3].sample == starts.back() + physical,
                        "release cancels retriggers and leaves the final physical one-shot to finish");
            }
            require(session.problems().empty() && callbackAllocations == 0 && audio::callbackFileIo == 0,
                    "pinned Repeat runs on the ramp without callback allocation or I/O");
        }
    }
}

std::unique_ptr<Store> physicalLegatoProject(const std::filesystem::path& folder, bool ram, int targetBeats = 5,
                                            int targetPulse = 3086, int targetStep = 12000) {
    auto store = sceneProject(folder);
    auto& db = store->db();
    db.exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120)");
    addClip(*store,folder,101,10,7,false,3000,1,12000);
    pinUnwarpedOneShot(*store,50000000,1500000000);
    // The inherited target frame 27,086 carries the third signed pulse.
    addClip(*store,folder,102,20,targetBeats,true,targetPulse,1,targetStep);
    db.exec("UPDATE clips SET time_base=1,pos_ticks=NULL,pos_ns=0,length_ticks=NULL,length_ns=1000000000,"
            "loop_enabled=0,loop_len_ticks=NULL WHERE id=102;"
            "UPDATE audio_clips SET warp_enabled=0,warp_markers=NULL WHERE clip_id=102;"
            "INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(102,100000000,1100000000);"
            "UPDATE clip_launch SET launch_quant=8 WHERE clip_id=102;");
    if (!ram) db.exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=102");
    return store;
}

void physicalLegatoOneShotTempoScene() {
    constexpr auto cue = 3086 + 12000; // Second signed attack in the saved target.
    constexpr auto targetStart = 4800;
    constexpr auto elapsed = cue - targetStart;
    constexpr auto oldLaunch = tempoSceneFrame - elapsed;
    constexpr auto stop = oldLaunch + rate;
    enum class Scene { Steady, Tempo, ChangedSource, EditedWhilePending, ToggledWhilePending, LongPaged };
    const auto render = [&](bool ram, Scene type) {
        test::TempDirectory folder{"physical_legato_scene", ram ? "ram" : "paged"};
        const bool longTarget = type == Scene::LongPaged;
        auto store = physicalLegatoProject(folder.path(), ram, longTarget ? 17 : 5);
        auto& db = store->db();
        db.exec("UPDATE clip_launch SET launch_quant=1 WHERE clip_id=101");
        if (type != Scene::Steady) db.exec("UPDATE scenes SET tempo_bpm=140,tempo_enabled=1 WHERE id=20");
        if (longTarget) db.exec("UPDATE clip_playback_details SET source_end_ns=4100000000 WHERE clip_id=102;"
                                "UPDATE clips SET length_ns=4000000000 WHERE id=102");
        store.reset();
        StoreError openError{};
        store = Store::open(folder.path() / "song.adi", openError);
        require(static_cast<bool>(store), "reopen the saved unwarped Legato scene");

        Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(*store, {}, {2, rate, blockFrames})) throw std::runtime_error(session.error());
        session.transport().play();
        std::vector<float> pcm(static_cast<std::size_t>(stop + blockFrames));
        Playback playback{session, pcm};
        playback.paged = !ram;
        playback.through(oldLaunch);
        OpRequest start;
        start.opType = "session.launchClip";
        start.payload = {{"track", 1}, {"scene", 10}, {"clip", 101}, {"incarnation", session.launchIncarnation()}};
        require(OpJournal(*store).commit(start).ok, "launch the saved outgoing unwarped slot");
        playback.through(tempoSceneFrame - 3 * blockFrames);
        const auto previous = trackState(session);
        require(previous.clip == 101 && previous.run != 0 && playback.count == 1 &&
                playback.edges[0].sample == oldLaunch, "the outgoing run starts at its exact physical sample");
        if (type == Scene::ChangedSource) sceneWithGain(*store, session, 20, 102, -6.0);
        else scene(*store, session, 20);

        if (type == Scene::LongPaged || type == Scene::ChangedSource) {
            require(!session.error().empty(), "reject a cold page target or a changed saved source");
            playback.through(tempoSceneFrame + 1);
            require(playback.count == 1 && playback.sceneCount == 0 && trackState(session).run == previous.run &&
                    session.beginRender().bpmAtTick(4 * quarter) == 120,
                    "refusing the tempo scene preserves the old physical run and tempo");
            return pcm;
        }
        if (!session.error().empty()) throw std::runtime_error("Physical Legato tempo scene: " + session.error());
        if (type == Scene::EditedWhilePending || type == Scene::ToggledWhilePending) {
            OpRequest edit;
            if (type == Scene::EditedWhilePending) {
                edit.opType = "clip.setStartMarker";
                edit.payload = {{"id", 101}, {"ticks", quarter / 4}};
            } else {
                edit.opType = "session.setLaunchSettings";
                edit.payload = {{"clip", 102}, {"legato", false}};
            }
            require(OpJournal(*store).commit(edit).ok && !session.refresh(*store),
                    "a pending scene keeps its exact source and Legato target despite later saved edits");
        }
        playback.through(tempoSceneFrame + 1);
        const auto active = trackState(session);
        require(playback.count == 2 && playback.edges[1].sample == tempoSceneFrame &&
                playback.edges[1].newClip == 102 && active.clip == 102 && active.run != previous.run &&
                active.remainingSamples == rate - elapsed - 1 && pcm[tempoSceneFrame] == -.6f,
                "the scene starts at the inherited physical cue with the exact remaining duration");
        require(playback.sceneCount == 1 && playback.scenes[0].scene == 20 &&
                playback.scenes[0].sample == tempoSceneFrame &&
                session.beginRender().bpmAtTick(4 * quarter) == (type == Scene::Steady ? 120 : 140),
                "the scene event and its clock coincide with the inherited-cue launch");
        playback.through(stop + blockFrames);
        require(playback.count == 3 && playback.edges[2].sample == stop && playback.edges[2].newClip == 0 &&
                std::all_of(pcm.begin() + stop, pcm.end(), [](float sample) { return sample == 0; }) &&
                session.problems().empty() && callbackAllocations == 0 && audio::callbackFileIo == 0,
                "the inherited raw slot stops on its physical End without callback allocation or file I/O");
        std::printf("Physical Legato %s scene: cue %d, launch %lld, stop %lld, attack %.6f\n",
                    ram ? "RAM" : "paged", cue, static_cast<long long>(tempoSceneFrame),
                    static_cast<long long>(stop), static_cast<double>(pcm[tempoSceneFrame]));
        return pcm;
    };
    for (const bool ram : {true, false}) {
        const auto steady = render(ram, Scene::Steady);
        require(render(ram, Scene::Tempo) == steady, "tempo does not change any physical cue PCM");
        if (!ram) {
            require(render(false, Scene::EditedWhilePending) == steady,
                    "a saved outgoing Start edit preserves the pending handover PCM");
            require(render(false, Scene::ToggledWhilePending) == steady,
                    "a saved Legato toggle preserves the pending handover PCM");
        }
    }
    (void)render(false, Scene::LongPaged);
    (void)render(true, Scene::ChangedSource);
}

void physicalLegatoOneShotRetainedByTempoScene(bool ram) {
    test::TempDirectory folder{"physical_legato",ram ? "no-stop-ram" : "no-stop-paged"};
    auto store = sceneProject(folder.path());
    enableTempoScene(*store);
    auto& db = store->db();
    db.exec("UPDATE scenes SET ord=2 WHERE id=20;"
            "INSERT INTO scenes(id,ord) VALUES(15,1);"
            "INSERT INTO clip_slots(track_id,scene_id,has_stop) VALUES(1,20,0);");
    addClip(*store,folder.path(),101,10,5,false,3000,1,12000);
    pinUnwarpedOneShot(*store,50000000,800000000);
    addClip(*store,folder.path(),102,15,5,true,4800,1,12000);
    db.exec("UPDATE clips SET time_base=1,pos_ticks=NULL,pos_ns=0,length_ticks=NULL,length_ns=1000000000,"
            "loop_enabled=0,loop_len_ticks=NULL WHERE id=102;"
            "UPDATE audio_clips SET warp_enabled=0,warp_markers=NULL WHERE clip_id=102;"
            "INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(102,100000000,1100000000);"
            "UPDATE clip_launch SET launch_quant=1 WHERE clip_id IN (101,102);");
    if (!ram) db.exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=102");

    const auto render = [&](bool change, bool savedGainEdit) {
        Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
        constexpr std::int64_t from = 60000, switchAt = 72000, finish = 108000;
        std::vector<float> pcm(static_cast<std::size_t>(finish + blockFrames));
        Playback playback{session,pcm};
        playback.paged = !ram;
        session.transport().play();
        const auto press = [&](std::int64_t sceneId,std::int64_t clip) {
            const LaunchRequest request{LaunchRequestKind::SlotPress,1,sceneId,clip,session.launchIncarnation()};
            require(session.postLaunchGesture(std::array{request}),"post a regular persisted slot press");
        };
        playback.through(from);
        press(10,101);
        playback.through(switchAt);
        press(15,102);
        playback.through(switchAt + 1);
        const auto before = trackState(session);
        require(playback.count == 2 && playback.edges[1].newClip == 102 && playback.edges[1].sample == switchAt &&
                before.remainingSamples == finish - switchAt - 1 && std::abs(pcm[switchAt] + .6f) < 1e-6f,
                "ordinary Legato enters on its inherited 16,800-frame source cue");
        require(session.postLaunchClipGainEdit({1,102,session.launchIncarnation(),before.run,0.5f,ClipUpdateRate::None}),
                "set live gain on the inherited Legato run");
        playback.through(tempoSceneFrame - 2 * blockFrames);
        if (savedGainEdit) sceneWithGain(*store,session,20,102,-6.0);
        else if (change) scene(*store,session,20);
        if (change && !savedGainEdit && !session.error().empty())
            throw std::runtime_error("Legato No Stop: "+session.error());
        playback.through(tempoSceneFrame + 1);
        const auto after = trackState(session);
        const auto bpm = session.beginRender().bpmAtTick(4 * quarter);
        require(after.clip == 102 && after.run == before.run && after.remainingSamples == finish - tempoSceneFrame - 1 &&
                std::abs(pcm[tempoSceneFrame] + .4f) < 1e-6f,
                "No Stop keeps the same run, physical remainder, source pulse and live gain");
        if (change && !savedGainEdit) {
            require(playback.sceneCount == 1 && playback.scenes[0].scene == 20 && playback.scenes[0].sample == tempoSceneFrame &&
                    playback.scenes[0].bpm == 140 && bpm == 140 && session.problems().empty(),
                    "the actual No Stop scene performs once at 140 BPM");
        }
        if (savedGainEdit)
            require(session.error().find("unchanged unwarped") != std::string::npos && bpm == 120 && playback.sceneCount == 0,
                    "refuse a saved gain change and keep the existing source and clock");
        playback.through(finish + blockFrames);
        require(playback.count == 3 && playback.edges[2].newClip == 0 && playback.edges[2].sample == finish &&
                callbackAllocations == 0 && audio::callbackFileIo == 0,
                "the inherited one-shot still stops at its physical End without callback work");
        return pcm;
    };

    const auto reference = render(false,false);
    require(render(true,false) == reference,"No Stop keeps the entire inherited Legato signal sample-exact");
    require(render(true,true) == reference,"refused source gain replacement leaves the entire run sample-exact");
    std::printf("No Stop inherited Legato %s: cue 16800 at 72000, scene 96000, stop 108000, identical PCM\n",
                ram ? "RAM" : "paged");
}

void physicalLegatoOneShotFollowsExternalTempo() {
    bool matched = true;
    enum class Case { Resident, Paged, Trimmed };
    for (const auto kind : {Case::Resident,Case::Paged,Case::Trimmed}) {
        const bool ram = kind != Case::Paged, trimEnd = kind == Case::Trimmed;
        test::TempDirectory folder{"physical_legato",trimEnd ? "trimmed" : ram ? "ram" : "paged"};
        auto store = physicalLegatoProject(folder.path(),ram);
        auto& db = store->db();
        if (ram) db.exec("UPDATE clip_launch SET follow_enabled=1,follow_linked=1,follow_a=1 WHERE clip_id=102");

        Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
        constexpr auto due = 22286;
        constexpr auto cue = 4800 + due;
        const auto end = trimEnd ? 35200 : 48000;
        const auto pause = ram ? 0 : 2 * blockFrames;
        std::vector<float> pcm(static_cast<std::size_t>(end + pause + blockFrames));
        Playback playback{session,pcm};
        playback.paged = !ram;
        scene(*store,session,10);
        playback.through(6000);
        require(session.setExternalTempoAt(*store,140,quarter / 2), "perform tempo before the queued unwarped Legato");
        playback.through(16000);
        const LaunchRequest request{LaunchRequestKind::SlotPress,1,20,102,session.launchIncarnation()};
        require(session.postLaunchGesture(std::array{request}), "queue the physical one-shot Legato target");
        playback.through(16000 + blockFrames);
        const auto forecast = trackState(session).queued;
        playback.through(due + 1);
        const auto active = trackState(session);
        if (trimEnd) {
            playback.through(due + 3000);
            const auto tick = [](std::int64_t frame) { return frame * quarter / (rate / 2); };
            const LaunchClipRegionEdit edit{1,102,session.launchIncarnation(),active.run,
                {tick(4800),tick(40000),{}},ClipUpdateRate::None};
            require(session.postLaunchClipRegionEdit(edit), "trim the cued RAM one-shot's physical End");
        }
        if (!ram) {
            constexpr auto pausedAt = due + 6000;
            playback.through(pausedAt);
            session.transport().play(false);
            playback.through(pausedAt + blockFrames);
            db.exec("UPDATE clips SET gain_db=-6 WHERE id=102");
            require(session.refresh(*store), "rebuild the different bounded page bank while transport is stopped");
            playback.through(pausedAt + pause);
            session.transport().play();
        }
        playback.through(end + pause + blockFrames);
        const auto stop = playback.count == 3 ? playback.edges[2].sample : -1;
        std::printf("Physical Legato %s: frame %lld, expected cue %d, attack %.6f, remaining %lld, stop %lld\n",
            trimEnd ? "RAM with End edit" : ram ? "RAM" : "paged",
            static_cast<long long>(playback.edges[1].sample),cue,static_cast<double>(pcm[due]),
            static_cast<long long>(active.remainingSamples),static_cast<long long>(stop));
        for (const auto& problem : session.problems()) std::printf("  %s\n",problem.c_str());
        matched &= forecast.clip == 102 && forecast.legatoSourceTick &&
            forecast.legatoSourceFrame && *forecast.legatoSourceFrame == cue && playback.count == 3 &&
            playback.edges[1].newClip == 102 && playback.edges[1].sample == due && playback.edges[2].newClip == 0 &&
            stop == end && active.remainingSamples == 48000 - due - 1 && std::abs(pcm[due] + .7f) < 1e-6f &&
            std::abs(pcm[static_cast<std::size_t>(due+12000+pause)] + (ram ? .8f : .8f * std::pow(10.f,-.3f))) < 1e-6f &&
            session.problems().empty();
    }
    require(matched, "both physical Legato backends keep the exact cue and remaining duration across tempo");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
            "the physical Legato cue performs no callback allocation or file I/O");
}

void physicalLegatoRebasesItsCueOnLaterBank() {
    test::TempDirectory folder{"physical_legato","later-flat-clock"};
    auto store = physicalLegatoProject(folder.path(),true);
    store->db().exec("INSERT INTO scenes(id,ord) VALUES(30,2)");
    addClip(*store,folder.path(),103,30,2,true,2107,1,12000);
    store->db().exec("UPDATE clip_launch SET launch_quant=1 WHERE clip_id=103");

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    constexpr auto due = 22286, rebase = due + 3000, next = rebase + 2;
    const auto clipTickAt20 = [](std::int64_t sourceFrame) { return sourceFrame * quarter / (3 * rate); };
    std::vector<float> pcm(next + blockFrames);
    Playback playback{session,pcm};
    scene(*store,session,10);
    playback.through(6000);
    require(session.setExternalTempoAt(*store,140,quarter / 2), "prepare the positive performed tempo boundary");
    playback.through(16000);
    const LaunchRequest first{LaunchRequestKind::SlotPress,1,20,102,session.launchIncarnation()};
    require(session.postLaunchGesture(std::array{first}), "take the first physical Legato cue");
    playback.through(rebase);
    require(playback.edges[1].sample == due && trackState(session).clip == 102, "start the original physical Legato run");
    require(session.setExternalTempo(*store,20), "replace the clock with a new tick-zero tempo");
    playback.through(rebase+1);
    const auto before = trackState(session);
    playback.through(next);
    const auto after = trackState(session);
    const LaunchRequest third{LaunchRequestKind::SlotPress,1,30,103,session.launchIncarnation()};
    require(session.postLaunchGesture(std::array{third}), "launch the next Warp Legato target on the new clock");
    playback.through(next + blockFrames);
    std::printf("Rebased physical Legato: 20 BPM source ticks %lld/%lld, next inherited %lld at %lld\n",
        static_cast<long long>(before.sourceTick),static_cast<long long>(after.sourceTick),
        static_cast<long long>(playback.edges[2].sourceTick),static_cast<long long>(playback.edges[2].sample));
    require(before.run == after.run && before.clip == 102 &&
            std::abs(before.sourceTick - clipTickAt20(4800 + rebase + 1)) <= 1 &&
            std::abs(after.sourceTick - clipTickAt20(4800 + next)) <= 1 && after.sourceTick > before.sourceTick,
            "physical source ticks advance in the new bank's geometry");
    require(playback.count == 3 && playback.edges[2].newClip == 103 && playback.edges[2].sample == next &&
            std::abs(playback.edges[2].sourceTick - clipTickAt20(next)) <= 1 && pcm[next] < -.4f,
            "the subsequent Warp Legato inherits the physical phase on the new geometry");
}

void physicalLegatoRepeat() {
    constexpr auto first = 22286; // Beat one with 120 BPM for half a beat, then 140 BPM.
    constexpr auto gridSecond = 42857, gridThird = 63429;
    for (const bool ram : {true, false}) {
        for (const auto variant : {0, 1, 2}) {
            const bool explicitGrid = variant == 1, loop = variant == 2;
            const auto start = loop ? 4801 : 4800;
            test::TempDirectory folder{"physical_legato_repeat", ram ? "ram" : "paged"};
            auto store = physicalLegatoProject(folder.path(), ram, 5, start, first);
            auto& db = store->db();
            db.exec("UPDATE clip_launch SET launch_mode=3 WHERE clip_id=102");
            if (!explicitGrid) db.exec("UPDATE clip_launch SET launch_quant=1 WHERE clip_id=102");
            if (loop) {
                // Start lies inside the loop. It rounds up by one sample
                // while the two loop pins round down.
                SQLite::Statement edit(db, "UPDATE clips SET loop_enabled=1,loop_start_ticks=?,loop_len_ticks=? WHERE id=102");
                edit.bind(1, quarter / 10);
                edit.bind(2, 8 * quarter / 5);
                edit.exec();
                db.exec("UPDATE clip_playback_details SET content_offset_ns=100010625,"
                        "loop_begin_ns=50010208,loop_end_ns=850010208,source_end_ns=1050000000 WHERE clip_id=102;"
                        "UPDATE clips SET length_ns=949989375 WHERE id=102");
            }
            store.reset();
            StoreError openError{};
            store = Store::open(folder.path() / "song.adi", openError);
            require(static_cast<bool>(store), "reopen the saved physical Legato Repeat clip");

            Session session;
            session.graph().setFadeFrames(0);
            if (!session.load(*store, {}, {2, rate, blockFrames})) throw std::runtime_error(session.error());
            const auto span = loop ? 40800 - start : rate;
            const auto second = explicitGrid ? gridSecond : span;
            const auto third = explicitGrid ? gridThird : 2 * span;
            const auto finish = third + span;
            const auto loopAttack = start - 2400;
            std::vector<float> pcm(static_cast<std::size_t>(finish + blockFrames + (loop ? loopAttack : 0)));
            Playback playback{session, pcm};
            playback.paged = !ram;
            scene(*store, session, 10);
            playback.through(6000);
            require(session.setExternalTempoAt(*store, 140, quarter / 2), "publish the performed tempo before Repeat");
            playback.through(explicitGrid ? 16000 : first);
            const auto press = [&](bool released) {
                OpRequest gesture;
                gesture.opType = "session.launchClip";
                gesture.payload = {{"track", 1}, {"scene", 20}, {"clip", 102},
                                   {"incarnation", session.launchIncarnation()}, {"release", released}};
                require(OpJournal(*store).commit(gesture).ok, "post the saved Repeat press or release");
            };
            press(false);
            if (explicitGrid) {
                playback.through(16000 + blockFrames);
                const auto queued = trackState(session).queued;
                require(queued.legatoSourceFrame == start + first, "the queued Repeat forecasts the exact physical cue");
            }
            playback.through(first + 1);
            const auto inherited = trackState(session);
            std::printf("Physical Repeat Legato %s %s: initial sample %.6f at %lld, remaining %lld\n",
                        ram ? "RAM" : "paged", loop ? "loop" : explicitGrid ? "grid" : "end",
                        static_cast<double>(pcm[first]), static_cast<long long>(playback.edges[1].sample),
                        static_cast<long long>(inherited.remainingSamples));
            require(playback.count == 2 && playback.edges[1].sample == first && inherited.clip == 102 &&
                    (loop || inherited.remainingSamples == span - first - 1) && pcm[first] == -.6f,
                    "the held Repeat first inherits the outgoing physical frame and remaining extent");
            playback.through(third + 1);
            press(true);
            playback.through(static_cast<std::int64_t>(pcm.size()));
            std::printf("  repeats at %lld/%lld, expected %d/%d\n",
                        static_cast<long long>(playback.edges[2].sample), static_cast<long long>(playback.edges[3].sample),
                        second, third);
            require(playback.edges[2].sample == second && playback.edges[2].newClip == 102 &&
                    playback.edges[3].sample == third && playback.edges[3].newClip == 102,
                    "subsequent repeats restart on the physical End or selected launch grid");
            require(pcm[static_cast<std::size_t>(second)] == -.5f && pcm[static_cast<std::size_t>(third)] == -.5f,
                    "the first sample of each held restart is audible at the literal physical Start");
            if (loop) {
                require(playback.count == 4 && trackState(session).completedLoops == 1 &&
                        pcm[static_cast<std::size_t>(finish + loopAttack)] == -.5f,
                        "release cancels loop retriggers and restores the ordinary physical loop");
            } else {
                require(playback.count == 5 && playback.edges[4].sample == finish && playback.edges[4].newClip == 0 &&
                        std::all_of(pcm.begin() + finish, pcm.end(), [](float frame) { return frame == 0; }),
                        "release cancels retriggers and leaves the final one-shot to finish normally");
            }
            require(session.problems().empty() && callbackAllocations == 0 && audio::callbackFileIo == 0,
                    "Repeat uses the admitted pages without callback allocation or file I/O");
        }
    }
}

void longPagedRepeatKeepsStart() {
    test::TempDirectory folder{"session_repeat", "long-paged"};
    auto store = sceneProject(folder.path());
    {
        auto& db = store->db();
        db.exec("INSERT INTO session_state(key,value) VALUES('transport.launchQuantization','0')");
        SQLite::Statement tempo(db, "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,1),(?,140,0)");
        tempo.bind(1, lastBeat * quarter);
        tempo.exec();
    }
    constexpr auto span = 4 * rate;
    static_assert(span > 3 * ClipPlayback::readAheadFrames(rate));
    // Start at the last sample of a page so the restarted block spans two.
    constexpr auto start = 8191;
    constexpr auto startNs = (1'000'000'000LL * start + rate / 2) / rate;
    addClip(*store, folder.path(), 101, 10, 17, false, start, 1, 12000);
    pinUnwarpedOneShot(*store, startNs, 4000000000);
    store->db().exec("UPDATE clip_launch SET launch_mode=3,ram_mode=0 WHERE clip_id=101");
    store.reset();
    StoreError openError{};
    store = Store::open(folder.path() / "song.adi", openError);
    require(static_cast<bool>(store), "reopen the long paged Repeat slot");

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2, rate, blockFrames})) throw std::runtime_error(session.error());
    const auto press = [&](bool release) {
        OpRequest gesture;
        gesture.opType = "session.launchClip";
        gesture.payload = {{"track", 1}, {"scene", 10}, {"clip", 101},
                           {"incarnation", session.launchIncarnation()}, {"release", release}};
        require(OpJournal(*store).commit(gesture).ok, "post a persisted Repeat gesture");
    };
    constexpr auto after = blockFrames - 1;
    std::vector<float> pcm(2 * span + blockFrames);
    Playback playback{session, pcm};
    playback.paged = true;
    press(false);
    // One frame of the old run precedes the wide restart section.
    playback.through(span - 1);
    playback.through(span + after);
    require(playback.count == 2 && playback.edges[0].sample == 0 &&
            playback.edges[1].sample == span && playback.edges[1].newClip == 101,
            "the held Repeat retriggers inside the callback after four physical seconds");
    require(pcm[0] == .55f && pcm[span - 12000] == .55f, "the first run renders its start and distant source PCM");
    std::printf("Long paged Repeat: first sample %.6f, restart sample %.6f at %d\n",
                static_cast<double>(pcm[0]), static_cast<double>(pcm[span]), span);
    require(std::equal(pcm.begin(), pcm.begin() + after, pcm.begin() + span),
            "the long paged Repeat keeps its audible Start through the restart callback");
    press(true);
    playback.through(2 * span + blockFrames);
    require(playback.count == 3 && playback.edges[2].sample == 2 * span && playback.edges[2].newClip == 0 &&
            session.problems().empty() && callbackAllocations == 0 && audio::callbackFileIo == 0,
            "the released long run completes with no callback allocation or file I/O");
}

void physicalOneShotLinkedFollow(std::string_view action, bool ram, bool retime) {
    test::TempDirectory folder{"unwarped_follow", ram ? "one-shot-ram" : "one-shot-paged"};
    auto store = sceneProject(folder.path());
    auto& db = store->db();
    db.exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);");
    addClip(*store,folder.path(),101,10,5,false,4200,1,12000);
    pinUnwarpedOneShot(*store,50000000,800000000);
    if (!ram) db.exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=101");
    const bool stops = action == "stop";
    const bool grid = action == "grid";
    if (!stops) {
        addClip(*store,folder.path(),102,20,2,false,1200);
        db.exec(grid ? "UPDATE clip_launch SET launch_quant=12 WHERE clip_id=102"
                     : "UPDATE clip_launch SET launch_quant=1 WHERE clip_id=102");
    }
    db.exec(stops ? "UPDATE clip_launch SET follow_enabled=1,follow_linked=1,follow_a=1 WHERE clip_id=101"
                  : "UPDATE clip_launch SET follow_enabled=1,follow_linked=1,follow_a=4 WHERE clip_id=101");
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    constexpr auto end = 38400;
    constexpr auto lastSourcePulse = 37800;
    const auto target = grid ? (retime ? 42857 : 42000) : end;
    const auto attack = target + (retime ? 1029 : 1200);
    std::vector<float> pcm(static_cast<std::size_t>(attack + blockFrames));
    Playback playback{session,pcm};
    playback.paged = !ram;
    scene(*store,session,10);
    playback.through(6000);
    if (retime) require(session.setExternalTempoAt(*store,140,quarter/2), "apply 140 BPM before the linked deadline");
    playback.through(static_cast<std::int64_t>(pcm.size()));
    const auto count = grid ? std::size_t{3} : std::size_t{2};
    const auto at = playback.count >= 2 ? playback.edges[1].sample : -1;
    const auto last = playback.count >= 2 ? playback.edges[playback.count-1].sample : -1;
    std::printf("Pinned %s %.*s %d BPM: first edge=%lld final=%lld expected End=%d target=%d late PCM=%0.6f\n",
        ram ? "RAM" : "paged",static_cast<int>(action.size()),action.data(),retime ? 140 : 120,
        static_cast<long long>(at),static_cast<long long>(last),end,target,static_cast<double>(pcm[lastSourcePulse]));
    const auto expectedFinalClip = stops ? 0 : 102;
    require(playback.count == count && playback.edges[0].sample == 0 && playback.edges[0].newClip == 101 &&
            at == end && last == target && playback.edges[count-1].newClip == expectedFinalClip &&
            (!grid || playback.edges[1].newClip == 0) && pcm[lastSourcePulse] == .55f &&
            session.problems().empty(),"linked Follow waits for the complete physical one-shot, then respects the target grid");
    if (!stops) {
        const auto peak = std::max_element(pcm.begin()+attack-4,pcm.begin()+attack+5);
        require(std::abs((peak-pcm.begin())-attack) <= 1 && std::abs(*peak-.55f) < .04f,
                "an independent target PCM pulse confirms the exact launch phase");
    }
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
            "linked physical Follow performs no callback allocation or file I/O");
}

void physicalLegatoLoopFollowsExternalTempo() {
    enum class Entry { PastTargetEnd, AtOutgoingWrap, AtTargetEnd, FarCueAfterEdit };
    constexpr auto due = 22286;
    constexpr auto shortLoopEnd = 19200;
    constexpr auto manualStop = 84000;
    for (const bool ram : {true,false}) {
        for (const auto kind : {Entry::PastTargetEnd,Entry::AtOutgoingWrap,Entry::AtTargetEnd,Entry::FarCueAfterEdit}) {
            const bool editLegato = kind == Entry::FarCueAfterEdit;
            if (ram && editLegato) continue;
            const auto tag = kind == Entry::PastTargetEnd ? "past-end"
                : kind == Entry::AtOutgoingWrap ? "outgoing-wrap"
                : editLegato ? "exact-grid-saved-edit" : "target-wrap";
            const auto token = ram ? std::string(tag) : std::string(tag) + "-paged";
            test::TempDirectory folder{"physical_loop_legato",token.c_str()};
            auto store = physicalLegatoProject(folder.path(),ram);
            auto& db = store->db();
            const bool stopByFollow = kind != Entry::AtTargetEnd;
            const auto start = kind == Entry::AtTargetEnd ? 9600 : 4800;
            const auto loopBegin = kind == Entry::AtTargetEnd ? 3600 : editLegato ? 6000 : 9600;
            const auto loopEnd = kind == Entry::AtTargetEnd ? start + due : editLegato ? 40000 : shortLoopEnd;
            const auto cue = editLegato ? start + due : kind == Entry::AtOutgoingWrap ? start + 2400
                : loopBegin + (start + due - loopEnd) % (loopEnd - loopBegin);
            const auto wrap = due + loopEnd - cue;
            const auto stop = stopByFollow ? wrap + 2 * (loopEnd - loopBegin) : manualStop;
            const auto nanoseconds = [](std::int64_t frame) {
                return (frame * 1000000000 + rate / 2) / rate;
            };
            const auto setLoop = [&](int clip, int begin, int end) {
                db.exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=0,loop_len_ticks=5765760 WHERE id=" +
                        std::to_string(clip));
                SQLite::Statement update(db, "UPDATE clip_playback_details SET loop_begin_ns=?,loop_end_ns=? WHERE clip_id=?");
                update.bind(1,nanoseconds(begin)); update.bind(2,nanoseconds(end)); update.bind(3,clip); update.exec();
            };
            if (kind == Entry::AtTargetEnd)
                db.exec("UPDATE clip_playback_details SET content_offset_ns=200000000,source_end_ns=1083333333 WHERE clip_id=102");
            setLoop(102,loopBegin,loopEnd);
            if (editLegato) db.exec("UPDATE clip_launch SET legato=0 WHERE clip_id=102");
            if (kind == Entry::AtOutgoingWrap) setLoop(101,4800,2400+due);
            if (stopByFollow)
                db.exec("UPDATE clip_launch SET follow_enabled=1,follow_linked=1,follow_loop_count=3,follow_a=1 WHERE clip_id=102");
            store.reset();
            StoreError openError{};
            store = Store::open(folder.path() / "song.adi",openError);
            require(static_cast<bool>(store),"reopen the saved physical Legato loop");

            Session session;
            session.graph().setFadeFrames(0);
            if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
            std::vector<float> pcm(static_cast<std::size_t>(stop+blockFrames));
            Playback playback{session,pcm};
            playback.paged = !ram;
            scene(*store,session,10);
            playback.through(6000);
            require(session.setExternalTempoAt(*store,140,quarter / 2),"perform the clock change before loop Legato");
            if (editLegato) {
                playback.through(6001);
                OpRequest edit;
                edit.opType = "session.setLaunchSettings";
                edit.payload = {{"clip",102},{"legato",true}};
                require(OpJournal(*store).commit(edit).ok && session.refresh(*store),
                        "publish saved Legato for an idle paged loop");
                // The cue's page lies past both normal Start and loop-head
                // read-ahead, and the launch gives no earlier phase hint.
                playback.through(due);
            } else {
                playback.through(16000);
            }
            const LaunchRequest launch{LaunchRequestKind::SlotPress,1,20,102,session.launchIncarnation()};
            require(session.postLaunchGesture(std::array{launch}),"queue Legato into the physical loop");
            if (!editLegato) playback.through(16000+blockFrames);
            const auto forecast = trackState(session).queued;
            playback.through(wrap);
            const auto first = trackState(session);
            playback.through(wrap+1);
            const auto wrapped = trackState(session);
            if (!stopByFollow) {
                const LaunchRequest halt{LaunchRequestKind::StopTrack,1,{},{},session.launchIncarnation()};
                require(session.postLaunchGesture(std::array{halt}),"queue manual Stop after the cued loop wraps");
            }
            playback.through(static_cast<std::int64_t>(pcm.size()));

            audio::WavReader source(folder.path() / "102.wav");
            std::vector<float> raw(static_cast<std::size_t>(loopEnd));
            require(source.read(raw.data(),static_cast<std::uint32_t>(raw.size())) == raw.size(),
                    "read original PCM for the comparison");
            bool identical = true;
            auto read = cue;
            for (auto sample = due; sample < stop; ++sample) {
                identical &= pcm[static_cast<std::size_t>(sample)] == raw[static_cast<std::size_t>(read++)];
                if (read == loopEnd) read = loopBegin;
            }
            const auto actualStop = playback.count == 3 ? playback.edges[2].sample : -1;
            std::printf("Physical Legato loop %s %s: forecast %lld, cue %d, attack %.6f, wrap %d, stop %lld/%d, exact PCM %d\n",
                tag,ram ? "RAM" : "paged",static_cast<long long>(forecast.legatoSourceFrame.value_or(-1)),
                cue,static_cast<double>(pcm[due]),wrap,static_cast<long long>(actualStop),stop,identical);
            for (const auto& problem : session.problems()) std::printf("  %s\n",problem.c_str());
            const bool matchedCue = editLegato ? pcm[due] == -.7f : forecast.legatoSourceFrame == cue;
            require(matchedCue && playback.count == 3 && playback.edges[1].sample == due &&
                    playback.edges[1].newClip == 102 && first.clip == 102 && first.run == wrapped.run &&
                    first.completedLoops == 0 && wrapped.completedLoops == 1 && playback.edges[2].newClip == 0 &&
                    actualStop == stop && identical && session.problems().empty() &&
                    std::all_of(pcm.begin()+stop,pcm.end(),[](float sample) { return sample == 0; }),
                    "physical Legato uses the exact inherited sample, lap boundaries, and Stop");
        }
    }
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
            "physical loop Legato performs no callback allocation or file I/O");
}

void residentModeRamp(const char* mode, std::int64_t tolerance) {
    test::TempDirectory folder{"session_native_warp", mode};
    auto store = sceneProject(folder.path());
    auto& db = store->db();
    SQLite::Statement tempo(db, "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,1),(?,140,0)");
    tempo.bind(1, lastBeat * quarter); tempo.exec();
    // An interior pulse leaves room for Texture's grain jitter at launch and
    // across each loop wrap without changing the musical timing oracle.
    addClip(*store, folder.path(), 101, 10, 3, false, rate / 8);
    SQLite::Statement setting(db, "UPDATE audio_clips SET warp_mode=? WHERE clip_id=101");
    setting.bind(1, mode); setting.exec();

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    for (const auto& problem : session.problems()) std::printf("%s preparation: %s\n", mode, problem.c_str());
    session.transport().play();
    const auto end = frameAtBeat(10);
    std::vector<float> pcm(static_cast<std::size_t>(end));
    Playback playback{session, pcm};
    playback.through(blockFrames);
    scene(*store, session, 10);
    playback.through(end);
    require(playback.count == 1 && playback.edges[0].sample == frameAtBeat(4) && playback.edges[0].newClip == 101,
        "resident Warp launches at the exact analytically integrated bar");
    require(std::all_of(pcm.begin(), pcm.begin() + frameAtBeat(4), [](float value) { return value == 0; }),
        "the resident slot is silent before its launch");

    constexpr auto searchRadius = 1200;
    std::int64_t worst = 0;
    unsigned audible = 0;
    for (int beat = 4; beat < 10; ++beat) {
        const auto expected = frameAtBeat(beat + 0.25);
        const auto peak = std::max_element(pcm.begin() + expected - searchRadius,
            pcm.begin() + expected + searchRadius + 1);
        if (*peak < 0.08f) continue;
        worst = std::max(worst, std::abs((peak - pcm.begin()) - expected));
        ++audible;
    }
    std::printf("Resident %s: %u/6 audible, maximum grid error %lld samples, C++ allocations %u, file I/O %u\n",
        mode, audible, static_cast<long long>(worst), callbackAllocations, audio::callbackFileIo.load());
    require(audible == 6 && worst <= tolerance && session.problems().empty(),
        "resident Warp follows the authored ramp across the slot loop");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
        "resident Warp performs no callback allocation or file I/O");
}
}

int main() {
    try {
        const auto resident = exercise();
        require(exercise(false, 7) == resident,
            "resident Re-Pitch follows the same ramp and Legato phase with a saved pitch edit");
        require(exercise(true, 7) == resident,
            "paged Re-Pitch follows the same ramp and Legato phase with a saved pitch edit");
        preparedTempoScene(false);
        preparedTempoScene(true);
        continuingTempoScene(false);
        continuingTempoScene(false, false, true);
        continuingTempoScene(true);
        continuingTempoScene(true, true);
        mixedTempoScene(MixedScene::Retain);
        mixedTempoScene(MixedScene::Legato);
        mixedTempoScene(MixedScene::KeepFixed);
        mixedTempoScene(MixedScene::ColdWarp);
        mixedTempoScene(MixedScene::ChangeNativeGain);
        absoluteWarpFallback();
        physicalOneShotsFollowExternalTempo();
        retainPagedOneShotOnSlotRefresh();
        relaunchEditedPinnedOneShot();
        pausedSupersessionReturnsToSameClock(false);
        pausedSupersessionReturnsToSameClock(true);
        coldPagedOneShotOnAuthoredRamp();
        physicalOneShotToWarpLegato();
        physicalOneShotRetainedByTempoScene();
        persistedPhysicalLoopTempo();
        physicalLoopRetainedByTempoScene();
        physicalLoopRegionRetainedByTempoScene();
        physicalLoopFollowUsesTargetGrid();
        for (const auto* action : {"stop","next","grid"})
            for (const bool ram : {true,false})
                for (const bool retime : {false,true})
                    physicalOneShotLinkedFollow(action,ram,retime);
        physicalFollowKeepsCoincidentGridSample();
        laterFollowKeepsEarlierLegatoForecast();
        for (const auto kind : {"end","start","loop"}) {
            pinnedLiveRegionEdit(kind,false);
            pinnedLiveRegionEdit(kind,true);
        }
        pinnedLiveRegionEdit("end",true,true);
        physicalLegatoOneShotFollowsExternalTempo();
        physicalLegatoOneShotRetainedByTempoScene(true);
        physicalLegatoOneShotRetainedByTempoScene(false);
        physicalLegatoOneShotTempoScene();
        physicalLegatoRebasesItsCueOnLaterBank();
        physicalLegatoLoopFollowsExternalTempo();
        pinnedRepeatUsesItsLaunchClock();
        longPagedRepeatKeepsStart();
        physicalLegatoRepeat();
        residentModeRamp("beats", 12);
        // At 48 kHz allow 23 ms for the 65 ms grain's 25% fluctuation
        // and for the overlap of grains while the tempo changes.
        residentModeRamp("texture", 1100);
    }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL -- %s\n", error.what()); return 1; }
}
