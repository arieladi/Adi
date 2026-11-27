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
#include <filesystem>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
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
             int beats, bool legato, int pulseOffsetFrames = 0) {
    constexpr int sourceFramesPerBeat = rate / 2, halfWidth = 48;
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
    SQLite::Statement clipRow(db, "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,loop_enabled,loop_len_ticks) VALUES(?,1,'audio',0,?,1,?)");
    clipRow.bind(1, clip); clipRow.bind(2, beats * quarter); clipRow.bind(3, beats * quarter); clipRow.exec();
    const auto markers = audio::writeWarpMap(std::array<WarpMarker,2>{{{0,0},{frames, beats*quarter}}}, frames);
    require(static_cast<bool>(markers), "encode native slot Warp source map");
    SQLite::Statement audioRow(db, "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) VALUES(?,?,?,1,'re-pitch',?)");
    audioRow.bind(1, clip); audioRow.bind(2, clip); audioRow.bind(3, frames);
    audioRow.bind(4, markers.bytes.data(), static_cast<int>(markers.bytes.size())); audioRow.exec();
    SQLite::Statement slot(db, "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,?,?)");
    slot.bind(1, scene); slot.bind(2, clip); slot.exec();
    SQLite::Statement settings(db, "INSERT INTO clip_launch(clip_id,legato,ram_mode,clip_fade) VALUES(?,?,1,0)");
    settings.bind(1, clip); settings.bind(2, legato ? 1 : 0); settings.exec();
}

struct Playback {
    struct Edge { std::int64_t newClip = 0, sample = 0, sourceTick = 0; };
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
                        self.edges[self.count++] = {edge.newClip, edge.sample, edge.sourceTick};
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

std::vector<float> exercise(bool paged = false) {
    test::TempDirectory temporary{"session_warp_integration", "project"};
    auto store = sceneProject(temporary.path());
    auto& db = store->db();
    SQLite::Statement tempo(db, "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,1),(?,140,0)");
    tempo.bind(1, lastBeat * quarter); tempo.exec();
    addClip(*store, temporary.path(), 101, 10, 3, false);
    addClip(*store, temporary.path(), 102, 20, 4, true);
    if (paged) db.exec("UPDATE clip_launch SET ram_mode=0");

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
    if (paged) store->db().exec("UPDATE clip_launch SET ram_mode=0 WHERE clip_id=102;"
        "UPDATE audio_clips SET warp_mode='complex' WHERE clip_id=102");

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(), "initial tempo bank is prepared");
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
    constexpr std::array<float,2> complexPeaks{-.304f, -.426f};
    constexpr std::array<float,4> resampledPeaks{-.5f, -.6f, -.7f, -.8f};
    const auto error = paged ? checkTempoPulses(pcm, complexPeaks, 0.08f, "Paged Complex")
        : checkTempoPulses(pcm, resampledPeaks, 0.03f, "RAM Re-Pitch");
    require(error <= (paged ? 64 : 2), "prepared slot audio follows the performed 140 BPM playback grid");
}

LaunchTrackStatus trackState(const Session& session) {
    std::array<LaunchTrackStatus,4> tracks{};
    LaunchStatusHeader header;
    require(session.readLaunchStatus(tracks, header) && header.trackCount == 1 && tracks[0].track == 1,
        "read the actual slot track's published launch state");
    return tracks[0];
}

void continuingTempoScene(bool legato, bool paged = false) {
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
    scene(*store, session, 20);
    playback.through(last);
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
    constexpr std::array<float,4> retainedPeaks{-.6f, -.7f, -.5f, -.6f};
    constexpr std::array<float,4> legatoPeaks{-.6f, -.7f, -.8f, -.5f};
    const auto error = checkTempoPulses(pcm, legato ? legatoPeaks : retainedPeaks, 0.08f,
        legato ? "Legato tempo scene" : "Retained tempo scene");
    require(error <= 2 && pcm[static_cast<std::size_t>(tempoSceneFrame)] < -0.58f,
        "the inherited phase follows 140 BPM in the boundary callback and after the next loop wrap");
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
        require(exercise(true) == resident, "paged and resident Warp play the same samples through the ramp and loop");
        preparedTempoScene(false);
        preparedTempoScene(true);
        continuingTempoScene(false);
        continuingTempoScene(true);
        continuingTempoScene(true, true);
        residentModeRamp("beats", 12);
        // At 48 kHz allow 23 ms for the 65 ms grain's 25% fluctuation
        // and for the overlap of grains while the tempo changes.
        residentModeRamp("texture", 1100);
    }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL -- %s\n", error.what()); return 1; }
}
