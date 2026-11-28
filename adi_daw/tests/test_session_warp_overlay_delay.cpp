// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/launch_overlay_delay.hpp"
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
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace { unsigned callbackAllocations = 0; }
void adi::test::countReplacedNew() noexcept {
    if (adi::audio::onAudioThread) ++callbackAllocations;
}

namespace {
using namespace adi;
using namespace adi::engine;
constexpr std::uint32_t rate = 48000, sourceFrames = 4 * rate;
constexpr std::int32_t maxFrames = 257;
constexpr auto quarter = textproj::kPPQ;

void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

void addMedia(Store& store, int id, const char* name, std::size_t channel,
              std::uint32_t first, std::uint32_t last, float value) {
    std::vector<float> pcm(2 * sourceFrames);
    for (auto frame = first; frame < last; ++frame) pcm[2 * frame + channel] = value;
    const auto path = store.path().parent_path() / name;
    {
        audio::WavWriter wav(path, rate, 2, audio::WavFormat::Float32);
        wav.write(pcm.data(), sourceFrames);
        wav.close();
    }
    const auto digest = media::blake3File(path);
    require(static_cast<bool>(digest), "hash overlay fixture");
    SQLite::Statement media(store.db(), "INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) "
                                      "VALUES(?,?,?,'wav',48000,2,?)");
    media.bind(1, id); media.bind(2, digest.hex); media.bind(3, name); media.bind(4, sourceFrames); media.exec();
}

void buildProject(Store& store, bool ram, std::int64_t launchFrame) {
    test::applySessionSchema(store.db());
    auto& db = store.db();
    db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Overlay delay',48000);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) "
            "VALUES(1,'audio','Mixed',0),(2,'audio','Reference',1),(9,'master','Main',2);"
            "INSERT INTO scenes(id,ord) VALUES(10,0);");
    SQLite::Statement tempo(db, "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,?),(?, ?,0)");
    tempo.bind(1, ram ? 1 : 0); tempo.bind(2, 8 * quarter); tempo.bind(3, ram ? 140 : 120); tempo.exec();
    addMedia(store, 1, "arr.wav", 0, 0, sourceFrames, 0.15f);
    addMedia(store, 2, "ref.wav", 1, static_cast<std::uint32_t>(launchFrame),
        static_cast<std::uint32_t>(launchFrame + 1), 0.75f);
    addMedia(store, 3, "slot.wav", 0, 0, 4000, -0.7f);
    SQLite::Statement clips(db, "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) "
                               "VALUES(1,1,'audio',0,?),(2,2,'audio',0,?),(3,1,'audio',0,?)");
    for (int i = 1; i <= 3; ++i) clips.bind(i, 8 * quarter);
    clips.exec();
    const auto markers = audio::writeWarpMap(std::array{WarpMarker{0,0},WarpMarker{sourceFrames,8 * quarter}}, sourceFrames);
    require(static_cast<bool>(markers), "encode overlay fixture Warp map");
    SQLite::Statement audioRows(db, "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) "
                                   "VALUES(1,1,?,1,'complex',?),(2,2,?,0,'beats',NULL),(3,3,?,?,'re-pitch',?)");
    auto bindMap = [&](int field) { audioRows.bind(field, markers.bytes.data(), static_cast<int>(markers.bytes.size())); };
    audioRows.bind(1, sourceFrames); bindMap(2); audioRows.bind(3, sourceFrames);
    audioRows.bind(4, sourceFrames); audioRows.bind(5, ram ? 1 : 0);
    if (ram) bindMap(6); else audioRows.bind(6);
    audioRows.exec();
    db.exec("INSERT INTO clip_slots(track_id,scene_id,clip_id,has_stop) VALUES(1,10,3,1),(2,10,NULL,0);");
    SQLite::Statement settings(db, "INSERT INTO clip_launch(clip_id,ram_mode,clip_fade) VALUES(3,?,0)");
    settings.bind(1, ram ? 1 : 0); settings.exec();
}

struct Playback {
    Session& session;
    std::vector<float> left, right;
    std::int64_t first = 0, launch = -1;
    std::int32_t latency = 0;
    std::uint64_t generation = 0;

    void through(std::int64_t end) {
        while (first < end) {
            require(session.clips()->prime(std::chrono::seconds(5)), "prime mixed track pages");
            const auto frames = static_cast<std::int32_t>(std::min<std::int64_t>(maxFrames, end - first));
            float* out[]{left.data() + first, right.data() + first};
            AudioIo io{out, nullptr, 2, 0, frames, first};
            bool rendered;
            {
                const audio::CallbackScope callback;
                auto block = session.beginRender();
                block.observeLaunchTransitions(this, [](void* opaque, const LaunchTransition& edge, std::int64_t) noexcept {
                    if (edge.newClip == 3) static_cast<Playback*>(opaque)->launch = edge.sample;
                });
                latency = block.latencySamples();
                generation = block.generation();
                rendered = block.process(io);
            }
            require(rendered, "render mixed Arrangement and Session track");
            first += frames;
        }
    }
};

void exercise(bool ram) {
    test::TempDirectory temporary{"session_warp_overlay", ram ? "resident" : "paged"};
    StoreError error{};
    auto store = Store::createEmpty(temporary.path() / "song.adi", error);
    require(static_cast<bool>(store), "create overlay project");
    const auto edge = ram ? std::llround(rate * 24.0 * std::log1p(10.0 / 120.0)) : 2 * rate;
    buildProject(*store, ram, edge);

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2, rate, maxFrames})) throw std::runtime_error(session.error());
    require(session.launchIncarnation() != 0, "load the persisted slot");
    require(session.problems().empty(), "admit the mixed track without source errors");
    session.transport().play();
    const auto total = static_cast<std::size_t>(edge + rate / 2);
    Playback playback{session, std::vector<float>(total), std::vector<float>(total)};
    playback.through(maxFrames);
    OpRequest launch;
    launch.opType = "session.launchScene";
    launch.payload = {{"scene", 10}, {"incarnation", session.launchIncarnation()}};
    require(OpJournal(*store).commit(launch).ok, "queue the slot on the next bar");
    require(playback.latency > maxFrames, "the Arrangement R3 clip declares latency on the slot track");
    playback.through(edge + playback.latency / 2);
    const auto before = playback.generation;
    require(session.setPrecision(Precision::Float64), "replace the graph while the launch waits in its delay");
    playback.through(static_cast<std::int64_t>(total));
    require(playback.generation > before, "the graph changed while the slot played");

    const auto slot = std::find_if(playback.left.begin(), playback.left.end(), [](float value) { return value < -0.4f; });
    const auto reference = std::max_element(playback.right.begin(), playback.right.end());
    const auto firstSlot = slot - playback.left.begin();
    const auto firstReference = reference - playback.right.begin();
    std::printf("%s slot: edge %lld, R3 delay %d, slot %lld, compensated reference %lld\n",
                ram ? "resident" : "paged", static_cast<long long>(playback.launch), playback.latency,
                static_cast<long long>(firstSlot), static_cast<long long>(firstReference));
    require(playback.launch == edge, "the logical launch remains on the bar");
    require(*reference > 0.5f && firstReference == edge + playback.latency, "the parallel reference is compensated");
    require(slot != playback.left.end() && firstSlot == firstReference, "the Session overlay uses the Arrangement track delay");
    require(std::abs(playback.left[static_cast<std::size_t>(firstSlot + 6000)]) < 1e-4f,
            "the delayed slot's silence also replaces the Arrangement");
}

void delayMixAndSeek() {
    LaunchOverlayDelay delay(3, 1);
    Transport transport;
    transport.play();
    std::array<double,3> source{0.5, 0.125, 0.0}, dest{0.25, 0.25, 0.25};
    double* const in[]{source.data()};
    double* const out[]{dest.data()};
    const std::array<std::uint8_t,3> mask{1, 0, 1};
    delay.mix(transport, 0, 3, in, mask.data(), out);
    require(dest == std::array<double,3>{0.25, 0.25, 0.25}, "delay does not emit unfilled history");
    transport.advance(3);
    delay.mix(transport, 0, 3, in, mask.data(), out);
    require(dest == std::array<double,3>{0.5, 0.375, 0.0}, "delay carries the overlay, additive tail, and replacing silence");
    transport.locate(3);
    dest.fill(0.25);
    delay.mix(transport, 0, 3, in, mask.data(), out);
    require(dest == std::array<double,3>{0.25, 0.25, 0.25}, "a seek discards buffered slot audio even at the same timeline position");
}
} // namespace

int main() {
    try {
        delayMixAndSeek();
        exercise(true);
        exercise(false);
        require(callbackAllocations == 0 && audio::callbackFileIo == 0, "mixed track callbacks stay allocation and file-I/O free");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
