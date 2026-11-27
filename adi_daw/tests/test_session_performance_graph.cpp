// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/launch_capture.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/session_performance_capture.hpp"
#include "adi/store.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
std::size_t callbackAllocations = 0;
}

void adi::test::countReplacedNew() noexcept {
    if (adi::audio::onAudioThread)
        ++callbackAllocations;
}

namespace {
using namespace adi;
using namespace adi::engine;

constexpr std::uint32_t sampleRate = 48000;
constexpr std::int32_t blockFrames = 100;
constexpr std::int64_t frames = 1000;
using Samples = std::array<float, static_cast<std::size_t>(frames)>;

void require(bool condition, const std::string& reason) {
    if (!condition)
        throw std::runtime_error(reason);
}

struct Project {
    test::TempDirectory folder{"session_performance_graph", "capture"};
    std::filesystem::path file = folder.path() / "song.adi";
    std::unique_ptr<Store> store;

    Project() {
        StoreError error{};
        store = Store::createEmpty(file, error);
        require(static_cast<bool>(store), "create Session performance project");
        test::applySessionSchema(store->db());
        test::seedSessionMarker(store->db());
        media("arrangement.wav", 1, 0.125f);
        media("first.wav", 2, 0.25f);
        media("second.wav", 3, -0.375f);
        store->db().exec(
            "INSERT INTO project(id,name,sample_rate) VALUES(1,'Session performance',48000);"
            "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Clips',0),(9,'master','Main',1);"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES"
            "(10,1,'audio',0,5765760),(11,1,'audio',0,5765760),(12,1,'audio',0,5765760);"
            "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(10,1,2048),(11,2,2048),(12,3,2048);"
            "INSERT INTO scenes(id,ord,name) VALUES(20,0,'First'),(21,1,'Second');"
            "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,20,11),(1,21,12);");
        OpRequest quantization;
        quantization.opType = "transport.setLaunchQuantization";
        quantization.payload = {{"code", 0}};
        require(OpJournal(*store).commit(quantization).ok, "use the immediate performance grid");
    }

    void media(const char* name, std::int64_t id, float level) {
        const auto path = folder.path() / name;
        std::array<float, 2048> signal;
        signal.fill(level);
        audio::WavWriter writer(path, sampleRate, 1, audio::WavFormat::Float32);
        writer.write(signal.data(), static_cast<std::uint32_t>(signal.size()));
        writer.close();
        const auto hash = adi::media::blake3File(path);
        require(static_cast<bool>(hash), "hash capture source audio");
        SQLite::Statement row(store->db(),
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(?,?,?,'wav')");
        row.bind(1, id);
        row.bind(2, hash.hex);
        row.bind(3, name);
        row.exec();
    }
};

void performCommand(Store& project, const Session& session, std::int64_t sample) {
    OpRequest command;
    command.payload = {{"incarnation", session.launchIncarnation()}};
    switch (sample) {
    case 100:
    case 300:
        command.opType = "session.launchScene";
        command.payload["scene"] = sample == 100 ? 20 : 21;
        break;
    case 500:
        command.opType = "session.stopTrack";
        command.payload["track"] = 1;
        break;
    case 700:
        command.opType = "session.backToArrangement";
        break;
    default:
        return;
    }
    require(OpJournal(project).commit(command).ok, "commit a live launch or handover");
}

struct SceneReceipts {
    std::array<LaunchSceneTransition, 8> edges{};
    std::size_t size = 0;
};

Samples render(Store& project, LaunchCaptureBuffer* capture = nullptr, std::int64_t* endTick = nullptr,
               bool firstSceneOnly = false, SceneReceipts* scenes = nullptr) {
    Session session;
    session.graph().setFadeFrames(0);
    SessionSpec spec{1, sampleRate, blockFrames};
    spec.performanceState = capture != nullptr;
    spec.offline = !spec.performanceState;
    spec.synchronousClipReads = true;
    if (!session.load(project, {}, spec)) throw std::runtime_error(session.error());
    session.transport().play();
    Samples result{};
    for (std::int64_t sample = 0; sample < frames; sample += blockFrames) {
        if (capture && (!firstSceneOnly || sample == 100))
            performCommand(project, session, sample);
        require(session.clips()->prime(), "prime the render's Arrangement readers");
        auto* destination = result.data() + sample;
        AudioIo io;
        io.out = &destination;
        io.numOut = 1;
        io.frames = blockFrames;
        io.streamTimeSamples = sample;
        bool rendered = false;
        {
            auto block = session.beginRender();
            if (capture)
                block.observeLaunchTransitions(capture, [](void* context, const LaunchTransition& edge,
                                                           std::int64_t) noexcept {
                    (void)static_cast<LaunchCaptureBuffer*>(context)->append(edge);
                });
            if (scenes)
                block.observeSceneLaunches(scenes, [](void* context, const LaunchSceneTransition& edge,
                                                      std::int64_t) noexcept {
                    auto& receipts = *static_cast<SceneReceipts*>(context);
                    if (receipts.size < receipts.edges.size()) receipts.edges[receipts.size++] = edge;
                });
            if (endTick)
                *endTick = block.tickAtSample(frames);
            rendered = block.process(io);
        }
        require(rendered, "render the pinned Session graph");
    }
    return result;
}

void followThroughGraph() {
    Project project;
    project.media("scene-follow.wav", 4, 0.625f);
    project.store->db().exec(
        "INSERT INTO scenes(id,ord,name) VALUES(22,2,'Scene Follow target');"
        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(13,1,'audio',0,5765760);"
        "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(13,4,2048);"
        "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,22,13);"
        "UPDATE scenes SET follow_enabled=1,follow_duration_ticks=48048,follow_a=9,follow_a_scene_id=22 WHERE id=20;"
        "INSERT INTO clip_launch(clip_id,follow_enabled,follow_linked,follow_duration_ticks,follow_a)"
        " VALUES(11,1,0,24024,4),(12,1,0,24024,1);");
    std::array<LaunchTransition, 8> events{};
    LaunchCaptureBuffer enabled(events);
    const auto heard = render(*project.store, &enabled, nullptr, true);
    for (std::size_t frame = 0; frame < heard.size(); ++frame) {
        const auto expected = frame < 100 ? 0.125f : frame < 200 ? 0.25f
            : frame < 300 ? -0.375f : 0.625f;
        require(heard[frame] == expected, "the scene follow wins over the due clip Stop at frame 300");
    }
    require(enabled.transitions().size() == 3 && enabled.transitions()[2].newClip == 13,
            "Scene follow delivers one final track transition");
    OpRequest disable;
    disable.opType = "session.setFollowActionsEnabled";
    disable.payload = {{"on", false}};
    require(OpJournal(*project.store).commit(disable).ok, "disable Follow Actions on the persisted session");
    LaunchCaptureBuffer disabled(events);
    const auto quietFollow = render(*project.store, &disabled, nullptr, true);
    require(disabled.transitions().size() == 1 && quietFollow[200] == 0.25f && quietFollow.back() == 0.25f,
            "the pinned global switch suppresses both clip and scene follows");
}

void proof(bool captureClock) {
    Project project;
    if (captureClock) {
        project.store->db().exec("UPDATE scenes SET tempo_bpm=180,tempo_enabled=1,signature_numerator=7,"
            "signature_denominator=8,signature_enabled=1 WHERE id=21;"
            "UPDATE clips SET gain_db=6.020599913279624 WHERE id=11;");
    }
    const auto arrangementBefore = render(*project.store);
    std::string error;
    const auto source = captureSessionPerformanceSource(*project.store, error);
    require(source.has_value(), error);
    std::array<LaunchTransition, 8> events{};
    LaunchCaptureBuffer buffer(events);
    std::int64_t endTick = 0;
    SceneReceipts scenes;
    const auto heard = render(*project.store, &buffer, &endTick, false, captureClock ? &scenes : nullptr);
    const std::int64_t edge = captureClock ? 400 : 300;
    const float firstLevel = captureClock ? 0.5f : 0.25f;
    for (std::size_t frame = 0; frame < heard.size(); ++frame) {
        const float expected = frame < 100 || frame >= 700 ? 0.125f
            : frame < static_cast<std::size_t>(edge) ? firstLevel : frame < 500 ? -0.375f : 0.0f;
        require(heard[frame] == expected, "heard the literal Session performance at its sample boundaries");
    }
    const auto plan = planLaunchCapture(buffer, {0, 0}, {frames, endTick}, {});
    require(plan.error == LaunchCaptureError::None && plan.spans.size() == 3, "fold the live capture edges");
    std::optional<SessionPerformanceClock> clock;
    if (captureClock) {
        require(scenes.size == 2 && scenes.edges[1].sample == edge && scenes.edges[1].tempoBpm == 180 &&
            scenes.edges[1].signatureNumerator == 7 && scenes.edges[1].signatureDenominator == 8,
            "collect the actual tempo and meter receipt at the performed scene boundary");
        clock.emplace(SessionPerformanceClock{{0, 0}, {frames, endTick}, 120, std::span(scenes.edges).first(scenes.size)});
    }
    const auto prepared = prepareSessionPerformance(*project.store, *source, plan, sampleRate, clock,
        SessionPerformanceAudio{Precision::Float32, true});
    require(prepared.ok, prepared.error);
    const auto committed = clip_edit::commitPrepared(*project.store, prepared);
    require(committed.ok, committed.error);
    require(render(*project.store) == heard, "Arrangement capture matches 1000 live frames bit-for-bit");
    const auto wroteClock = [&] {
        auto& db = project.store->db();
        return db.execAndGet("SELECT count(*) FROM tempo_map WHERE bpm=180").getInt() == 1 &&
            db.execAndGet("SELECT count(*) FROM time_signature_map WHERE numerator=7 AND denominator=8").getInt() == 1;
    };
    require(!captureClock || wroteClock(), "the arrangement contains both performed clock changes");
    require(History(*project.store).undo().ok && render(*project.store) == arrangementBefore,
            "one undo restores the original Arrangement PCM");
    require(!captureClock || !wroteClock(), "the same Undo restores the authored clock");
    require(History(*project.store).redo().ok && render(*project.store) == heard,
            "one redo restores the captured waveform");
    require(project.store->close(), "close the captured project");
    project.store.reset();
    StoreError openError{};
    project.store = Store::open(project.file, openError, true);
    require(project.store && render(*project.store) == heard, "read-only reopen preserves the rendered take");
    require(!captureClock || wroteClock(), "read-only reopen preserves the performed Arrangement clock");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0, "capture processing stays callback-safe");
    std::puts(captureClock ? "PASS -- captured actual tempo and meter with 1000 bit-identical graph frames" :
        "PASS -- captured 1000 real graph frames bit-identically through commit, redo and reopen");
}
} // namespace

int main() {
    try {
        proof(false);
        proof(true);
        followThroughGraph();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
