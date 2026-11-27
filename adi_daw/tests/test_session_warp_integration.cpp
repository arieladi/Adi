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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
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

void require(bool pass, const char* message) {
    if (!pass) throw std::runtime_error(message);
}

std::int64_t frameAtBeat(int beat) {
    constexpr double slope = 20.0 / lastBeat;
    return std::llround(rate * 60.0 / slope * std::log1p(slope * beat / 120.0));
}

void addClip(Store& store, const std::filesystem::path& folder, int clip, int scene,
             int beats, bool legato) {
    constexpr int sourceFramesPerBeat = rate / 2, halfWidth = 48;
    const auto frames = static_cast<std::uint32_t>(beats * sourceFramesPerBeat);
    const auto name = std::to_string(clip) + ".wav";
    const auto file = folder / name;
    std::vector<float> pcm(frames);
    for (int beat = 0; beat < beats; ++beat) for (int offset = -halfWidth; offset <= halfWidth; ++offset) {
        const auto frame = beat * sourceFramesPerBeat + offset;
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
    Session& session;
    std::span<float> pcm;
    std::array<Edge,8> edges{};
    std::size_t count = 0;
    std::int64_t first = 0;

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
                rendered = block.process(io);
            }
            require(rendered, "render actual persisted launch graph");
            std::copy_n(left.begin(), frames, pcm.begin() + first);
            first += frames;
        }
    }
};

void scene(Store& store, const Session& session, int sceneId) {
    OpRequest gesture;
    gesture.opType = "session.launchScene";
    gesture.payload = {{"scene", sceneId}, {"incarnation", session.launchIncarnation()}};
    require(OpJournal(store).commit(gesture).ok, "submit actual scene launch operation");
}

void exercise() {
    test::TempDirectory temporary{"session_warp_integration", "project"};
    StoreError error{};
    auto store = Store::createEmpty(temporary.path()/"song.adi", error);
    require(static_cast<bool>(store), "create project");
    test::applySessionSchema(store->db());
    auto& db = store->db();
    db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Session Warp',48000);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Slots',0),(9,'master','Main',1);"
            "INSERT INTO scenes(id,ord) VALUES(10,0),(20,1);");
    SQLite::Statement tempo(db, "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,1),(?,140,0)");
    tempo.bind(1, lastBeat * quarter); tempo.exec();
    addClip(*store, temporary.path(), 101, 10, 3, false);
    addClip(*store, temporary.path(), 102, 20, 4, true);

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.launchIncarnation() != 0, "load persisted launch slots");
    for (const auto& problem : session.problems()) std::printf("slot preparation: %s\n", problem.c_str());
    session.transport().play();
    std::vector<float> pcm(static_cast<std::size_t>(frameAtBeat(lastBeat)));
    Playback playback{session, pcm};
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
    constexpr auto ticksPerFrame = quarter * 140 / (60 * rate) + 1;
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
}
}

int main() {
    try { exercise(); }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL -- %s\n", error.what()); return 1; }
}
