// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/clip_update_rate.hpp"
#include "adi/engine/launch_capture.hpp"
#include "adi/engine/launch_gain_edits.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/session_performance_capture.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <type_traits>

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
        std::array<float, 2048> signal;
        signal.fill(level);
        media(name, id, signal);
    }

    void media(const char* name, std::int64_t id, std::span<const float> signal, std::uint16_t channels = 1,
               audio::WavFormat format = audio::WavFormat::Float32) {
        const auto path = folder.path() / name;
        audio::WavWriter writer(path, sampleRate, channels, format);
        writer.write(signal.data(), static_cast<std::uint32_t>(signal.size() / channels));
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

struct LiveAudioEdit {
    std::optional<LaunchClipRegion> region{};
    float gain = 1.0f;
};

template<class Sample = float>
std::array<Sample, static_cast<std::size_t>(frames)> render(Store& project, LaunchCaptureBuffer* capture = nullptr,
               std::int64_t* endTick = nullptr, bool firstSceneOnly = false, SceneReceipts* scenes = nullptr,
               std::int32_t width = blockFrames, const LiveAudioEdit* liveEdit = nullptr,
               std::int64_t captureFrom = 0) {
    Session session;
    session.graph().setFadeFrames(0);
    SessionSpec spec{1, sampleRate, width};
    constexpr bool wide = std::is_same_v<Sample, double>;
    spec.precision = wide ? Precision::Float64 : Precision::Float32;
    spec.performanceState = capture != nullptr;
    spec.offline = !spec.performanceState;
    spec.synchronousClipReads = true;
    if (!session.load(project, {}, spec)) throw std::runtime_error(session.error());
    session.transport().play();
    std::array<Sample, static_cast<std::size_t>(frames)> result{};
    for (std::int64_t sample = 0; sample < frames; sample += width) {
        if (capture && (!firstSceneOnly || sample == 100))
            performCommand(project, session, sample);
        if (liveEdit && sample == 200) {
            std::array<LaunchTrackStatus, 1> tracks{};
            LaunchStatusHeader status;
            require(session.readLaunchStatus(tracks, status) && status.endSample == sample &&
                    tracks[0].clip == 11 && tracks[0].run != 0,
                    "address the live audio run after its first 100 samples");
            if (liveEdit->region)
                require(session.postLaunchClipRegionEdit({1, 11, status.incarnation, tracks[0].run,
                            *liveEdit->region, ClipUpdateRate::None}), "enable the loop for this live run only");
            if (liveEdit->gain != 1.0f)
                require(session.postLaunchClipGainEdit({1, 11, status.incarnation, tracks[0].run,
                            liveEdit->gain, ClipUpdateRate::None}), "apply the gain to this live run only");
        }
        require(session.clips()->prime(), "prime the render's Arrangement readers");
        auto* destination = result.data() + sample;
        AudioIo io;
        if constexpr (wide) io.out64 = &destination;
        else io.out = &destination;
        io.numOut = 1;
        io.frames = static_cast<std::int32_t>(std::min<std::int64_t>(width, frames - sample));
        io.streamTimeSamples = sample;
        bool rendered = false;
        {
            auto block = session.beginRender();
            if (capture && sample >= captureFrom) block.observeLaunchCapture(*capture);
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

void fractionalRawLoop() {
    Project project;
    std::array<float, 2048> signal{};
    for (std::size_t index = 0; index < signal.size(); ++index)
        signal[index] = static_cast<float>(index) / 2048.0f;
    project.media("fractional-loop.wav", 4, signal);
    project.store->db().exec(
        "UPDATE audio_clips SET media_id=4,src_start_frames=37,src_len_frames=1024 WHERE clip_id=11;"
        "UPDATE clips SET length_ticks=240240,content_offset_ticks=720,loop_enabled=1,"
        "loop_start_ticks=1200,loop_len_ticks=5826 WHERE id=11;"
        "INSERT INTO clip_launch(clip_id,ram_mode) VALUES(11,1);");
    const auto arrangementBefore = render(*project.store);
    std::string error;
    const auto source = captureSessionPerformanceSource(*project.store, error);
    require(source.has_value(), error);
    std::array<LaunchTransition, 4> events{};
    std::array<LaunchCapturedPcmRange, 64> pcm{};
    std::array<LaunchPcmTrackCursor, 1> tracks{{{.track = 1}}};
    LaunchCaptureBuffer buffer(events, pcm, tracks);
    std::int64_t endTick = 0;
    const auto heard = render(*project.store, &buffer, &endTick, true);
    constexpr std::int64_t start = 100;
    constexpr std::int64_t firstWrapTick = 24024 + 7026 - 720;
    const auto atSample = [](std::int64_t tick) {
        return (tick * 24000 + textproj::kPPQ / 2) / textproj::kPPQ;
    };
    auto rangeStart = start;
    auto wrap = atSample(firstWrapTick);
    std::int64_t iterations = 0;
    for (std::int64_t frame = start; frame < frames; ++frame) {
        if (frame == wrap) {
            rangeStart = wrap;
            wrap = atSample(firstWrapTick + (++iterations * 5826));
        }
        const auto sourceFrame = 37 + (iterations == 0 ? 3 : 5) + (frame - rangeStart);
        require(heard[static_cast<std::size_t>(frame)] == signal[static_cast<std::size_t>(sourceFrame)],
                "live Session plays each independently rounded fractional wrap from the raw window");
    }
    const auto plan = planLaunchCapture(buffer, {0, 0}, {frames, endTick}, {});
    require(plan.error == LaunchCaptureError::None && plan.spans.size() == 1 &&
            plan.pcmRanges.size() == static_cast<std::size_t>(iterations + 1),
            "one uninterrupted run supplies a tape interval per audible wrap");
    require(plan.pcmRanges[0].beginSample == 100 && plan.pcmRanges[0].endSample == 126 &&
            plan.pcmRanges[0].firstSourceFrame == 3 && plan.pcmRanges[1].endSample == 150 &&
            plan.pcmRanges[2].endSample == 175 && plan.pcmRanges[1].firstSourceFrame == 5,
            "PCM receipts pin alternating wrap lengths without using callback boundaries");
    constexpr SessionPerformanceAudio proof{Precision::Float32, true};
    auto bad = plan;
    bad.pcmTraceEnabled = false;
    bad.pcmRanges.clear();
    const auto absent = prepareSessionPerformance(*project.store, *source, bad, sampleRate, std::nullopt, proof);
    require(!absent.ok && absent.error.find("fractional") != std::string::npos,
            "fractional loops still refuse without performed PCM proof");
    bad = plan;
    ++bad.pcmRanges[1].beginSample;
    require(!prepareSessionPerformance(*project.store, *source, bad, sampleRate, std::nullopt, proof).ok,
            "an incomplete trace cannot silently omit a sample");
    bad = plan;
    bad.pcmRanges[1].firstSourceFrame = 1024;
    require(!prepareSessionPerformance(*project.store, *source, bad, sampleRate, std::nullopt, proof).ok,
            "a trace cannot read outside the frozen source window");

    const auto prepared = prepareSessionPerformance(*project.store, *source, plan, sampleRate, std::nullopt, proof);
    require(prepared.ok, prepared.error);
    const auto committed = clip_edit::commitPrepared(*project.store, prepared);
    require(committed.ok, committed.error);
    require(project.store->db().execAndGet("SELECT count(*) FROM clips c JOIN audio_clips a ON a.clip_id=c.id "
        "JOIN clip_playback_details p ON p.clip_id=c.id WHERE a.media_id=4 AND c.id<>11 AND c.time_base=1 "
        "AND c.loop_enabled=0 AND a.src_start_frames=37 AND a.src_len_frames=1024 "
        "AND p.content_offset_ns IS NOT NULL AND p.source_end_ns IS NOT NULL").getInt() ==
        static_cast<int>(plan.pcmRanges.size()), "each wrap uses the original source window and literal linear pins");
    require(render(*project.store, nullptr, nullptr, false, nullptr, 127) == heard,
            "raw-loop Arrangement matches the performed frames at a different callback width");
    require(History(*project.store).undo().ok && render(*project.store) == arrangementBefore &&
            History(*project.store).redo().ok && render(*project.store) == heard,
            "one Undo and Redo restore the complete fractional-loop take");
    require(project.store->close(), "close the raw-loop capture");
    project.store.reset();
    StoreError openError{};
    project.store = Store::open(project.file, openError, true);
    require(project.store && render(*project.store) == heard, "reopened raw-loop capture retains exact PCM");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0, "the optional PCM tape is callback-safe");
    std::puts("PASS -- raw Session loop wraps at frames 126, 150 and 175 replay bit-for-bit");
}

void authoredWarpLoopWithPcmTape() {
    Project project;
    std::array<float, 2048> waveform{};
    for (std::size_t index = 0; index < waveform.size(); ++index)
        waveform[index] = static_cast<float>(index % 61) / 128.0f - 0.25f;
    project.media("warp-loop.wav", 4, waveform);
    const auto warp = audio::writeWarpMap(std::array<WarpMarker, 2>{{{0, 0}, {2048, 600600}}}, 2048);
    require(static_cast<bool>(warp), "encode a nonidentity Warp map for the fixed-clock loop");
    {
        auto& db = project.store->db();
        db.exec("UPDATE clips SET length_ticks=240240,loop_enabled=1,loop_start_ticks=0,loop_len_ticks=60060 WHERE id=11;"
                "INSERT INTO clip_launch(clip_id,ram_mode) VALUES(11,1);");
        SQLite::Statement audioRow(db, "UPDATE audio_clips SET media_id=4,warp_enabled=1,"
                                       "warp_mode='re-pitch',warp_markers=? WHERE clip_id=11");
        audioRow.bind(1, warp.bytes.data(), static_cast<int>(warp.bytes.size()));
        audioRow.exec();
    }
    std::string error;
    const auto source = captureSessionPerformanceSource(*project.store, error);
    require(source.has_value(), error);
    std::array<LaunchTransition, 4> events{};
    LaunchCaptureBuffer edgesOnly(events);
    std::int64_t endTick = 0;
    const auto expected = render(*project.store, &edgesOnly, &endTick, true);
    constexpr auto start = 100, loop = 250;
    require(std::equal(expected.begin() + start, expected.begin() + start + loop, expected.begin() + start + loop) &&
            *std::min_element(expected.begin() + start, expected.begin() + start + loop) < -0.1f &&
            *std::max_element(expected.begin() + start, expected.begin() + start + loop) > 0.1f,
            "the resident Warp clip plays a nonconstant loop at its exact integral wrap");
    const auto authoredPlan = planLaunchCapture(edgesOnly, {0, 0}, {frames, endTick}, {});
    constexpr SessionPerformanceAudio proof{Precision::Float32, true};
    const auto authored = prepareSessionPerformance(*project.store, *source, authoredPlan, sampleRate, std::nullopt, proof);
    require(authored.ok, "the performed loop is already representable by its authored Warp map " + authored.error);

    std::array<LaunchCapturedPcmRange, 8> pcm{};
    std::array<LaunchPcmTrackCursor, 1> tracks{{{.track = 1}}};
    LaunchCaptureBuffer tape(events, pcm, tracks);
    require(render(*project.store, &tape, &endTick, true) == expected,
            "enabling raw trace storage does not change the performed Warp waveform");
    const auto plan = planLaunchCapture(tape, {0, 0}, {frames, endTick}, {});
    const auto prepared = prepareSessionPerformance(*project.store, *source, plan, sampleRate, std::nullopt, proof);
    require(prepared.ok, "an optional raw tape must not refuse an exactly reconstructible Warp loop " + prepared.error);
    require(plan.pcmRanges.empty(), "prepared Warp samples never enter the raw file-offset tape");
    const auto arrangementBefore = render(*project.store);
    const auto committed = clip_edit::commitPrepared(*project.store, prepared);
    require(committed.ok, committed.error);
    require(render(*project.store, nullptr, nullptr, false, nullptr, 127) == expected,
            "the captured Warp loop replays all 1000 samples at another callback width");
    require(History(*project.store).undo().ok && render(*project.store) == arrangementBefore &&
            History(*project.store).redo().ok && render(*project.store) == expected,
            "one Undo and Redo restore the captured Warp loop");
    require(project.store->close(), "close the captured Warp loop project");
    project.store.reset();
    StoreError openError{};
    project.store = Store::open(project.file, openError, true);
    require(project.store && render(*project.store, nullptr, nullptr, false, nullptr, 127) == expected,
            "reopening the project preserves the exact Warp loop");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0, "the Warp loop capture stays callback-safe");
    std::puts("PASS -- optional PCM tape keeps an authored Warp loop replayable at exact integral wraps");
}

void liveRawRegion() {
    Project project;
    std::array<float, 2048> signal{};
    for (std::size_t index = 0; index < signal.size(); ++index)
        signal[index] = static_cast<float>(index) / 2048.0f;
    project.media("edited-region.wav", 4, signal);
    project.store->db().exec(
        "UPDATE audio_clips SET media_id=4,src_start_frames=37,src_len_frames=1024 WHERE clip_id=11;"
        "UPDATE clips SET length_ticks=240240 WHERE id=11;"
        "INSERT INTO clip_launch(clip_id,ram_mode) VALUES(11,1);");
    const auto arrangementBefore = render(*project.store);
    std::string error;
    const auto source = captureSessionPerformanceSource(*project.store, error);
    require(source.has_value(), error);

    constexpr std::int64_t launchAt = 100, editAt = 200, sourceStart = 37;
    constexpr std::int64_t loopFirst = 50, loopFrames = 25;
    const LiveAudioEdit edit{.region = LaunchClipRegion{0, 240240, LaunchClipLoopRegion{12012, 18018}}};
    std::array<LaunchTransition, 4> noTapeEdges{};
    LaunchCaptureBuffer noTape(noTapeEdges);
    std::int64_t endTick = 0;
    const auto withoutTape = render(*project.store, &noTape, &endTick, true, nullptr, blockFrames, &edit);
    const auto refused = planLaunchCapture(noTape, {0, 0}, {frames, endTick}, {});
    require(refused.error == LaunchCaptureError::SourceEdited,
            "a transient geometry edit without a PCM tape cannot be replayed from authored geometry");

    std::array<LaunchTransition, 4> events{};
    std::array<LaunchCapturedPcmRange, 64> pcm{};
    std::array<LaunchPcmTrackCursor, 1> tracks{{{.track = 1}}};
    LaunchCaptureBuffer buffer(events, pcm, tracks);
    const auto heard = render(*project.store, &buffer, &endTick, true, nullptr, blockFrames, &edit);
    require(heard == withoutTape, "the trace leaves the live performance unchanged");
    for (std::int64_t frame = 0; frame < frames; ++frame) {
        const auto local = frame < editAt ? frame - launchAt : loopFirst + (frame - editAt) % loopFrames;
        const auto expected = frame < launchAt ? 0.125f : signal[static_cast<std::size_t>(sourceStart + local)];
        require(heard[static_cast<std::size_t>(frame)] == expected,
                "the immediate edit wraps the unlooped raw run to the literal source frame at sample 200");
    }
    require(project.store->db().execAndGet("SELECT loop_enabled FROM clips WHERE id=11").getInt() == 0,
            "the run-local edit leaves the frozen slot authored as a one-shot");
    const auto plan = planLaunchCapture(buffer, {0, 0}, {frames, endTick}, {});
    require(plan.error == LaunchCaptureError::None && plan.spans.size() == 1 &&
            plan.pcmRanges.size() == 1 + (frames - editAt) / loopFrames,
            "the captured run keeps one identity and has one source range per audible region");
    const auto& first = plan.pcmRanges.front();
    require(first.beginSample == launchAt && first.endSample == editAt && first.firstSourceFrame == 0 &&
            !first.liveRegionEdit, "the PCM tape distinguishes the authored prefix from edited playback");
    for (std::size_t index = 1; index < plan.pcmRanges.size(); ++index) {
        const auto& range = plan.pcmRanges[index];
        const auto begin = editAt + static_cast<std::int64_t>(index - 1) * loopFrames;
        require(range.run == first.run && range.beginSample == begin && range.endSample == begin + loopFrames &&
                range.firstSourceFrame == loopFirst && range.liveRegionEdit,
                "every performed loop range names its source cursor and live edit");
    }

    constexpr SessionPerformanceAudio playbackProof{Precision::Float32, true};
    auto incomplete = plan;
    ++incomplete.pcmRanges[1].beginSample;
    require(!prepareSessionPerformance(*project.store, *source, incomplete, sampleRate, std::nullopt, playbackProof).ok,
            "a region edit with a missing PCM sample cannot publish an Arrangement take");
    const auto prepared = prepareSessionPerformance(*project.store, *source, plan, sampleRate, std::nullopt, playbackProof);
    require(prepared.ok, prepared.error);
    const auto committed = clip_edit::commitPrepared(*project.store, prepared);
    require(committed.ok, committed.error);
    require(render(*project.store, nullptr, nullptr, false, nullptr, 127) == heard,
            "the edited raw region replays bit-for-bit on an alternate Arrangement callback width");
    require(History(*project.store).undo().ok && render(*project.store) == arrangementBefore &&
            History(*project.store).redo().ok && render(*project.store) == heard,
            "one Undo and Redo restore the complete region edit take");
    require(project.store->close(), "close the performed region capture");
    project.store.reset();
    StoreError openError{};
    project.store = Store::open(project.file, openError, true);
    require(project.store && render(*project.store) == heard, "reopen preserves the exact region edit take");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0, "the edited raw PCM trace remains callback-safe");
    std::puts("PASS -- a live raw loop enabled at frame 200 replays 1000 frames bit-for-bit through Undo, Redo and reopen");
}

template<class Sample>
void liveRawGain() {
    constexpr bool wide = std::is_same_v<Sample, double>;
    constexpr auto multiplier = wide ? 10.0f : 0.5f;
    Project project;
    std::array<float, 2048> signal{};
    for (std::size_t index = 0; index < signal.size(); ++index)
        signal[index] = static_cast<float>(index) / 32848.0f;
    project.media("edited-gain.wav", 4, signal);
    project.store->db().exec(
        "UPDATE audio_clips SET media_id=4,src_start_frames=37,src_len_frames=1024 WHERE clip_id=11;"
        "UPDATE clips SET length_ticks=240240 WHERE id=11;"
        "INSERT INTO clip_launch(clip_id,ram_mode) VALUES(11,1);");
    const auto arrangementBefore = render<Sample>(*project.store);
    std::string error;
    const auto source = captureSessionPerformanceSource(*project.store, error);
    require(source.has_value(), error);
    const LiveAudioEdit edit{.gain = multiplier};

    std::int64_t endTick = 0;
    std::array<LaunchTransition, 4> noTapeEdges{};
    LaunchCaptureBuffer noTape(noTapeEdges);
    const auto withoutTape = render<Sample>(*project.store, &noTape, &endTick, true, nullptr, blockFrames, &edit);
    require(planLaunchCapture(noTape, {0, 0}, {frames, endTick}, {}).error == LaunchCaptureError::SourceEdited,
            "a live gain change without a PCM tape cannot be replayed from authored gain");

    std::array<LaunchTransition, 4> events{};
    std::array<LaunchCapturedPcmRange, 4> pcm{};
    std::array<LaunchPcmTrackCursor, 1> tracks{{{.track = 1}}};
    LaunchCaptureBuffer buffer(events, pcm, tracks);
    const auto heard = render<Sample>(*project.store, &buffer, &endTick, true, nullptr, blockFrames, &edit);
    require(heard == withoutTape, "tracing the gain does not change the Session mix");
    for (std::size_t frame = 0; frame < heard.size(); ++frame) {
        const auto expected = frame < 100 ? Sample{0.125} :
            static_cast<Sample>(signal[37 + frame - 100]) * (frame < 200 ? Sample{1} : static_cast<Sample>(multiplier));
        require(heard[frame] == expected, "the native raw gain scales its exact signal from frame 200 onward");
    }
    if constexpr (wide)
        require(std::any_of(heard.begin() + 200, heard.end(), [](double sample) {
            return static_cast<double>(static_cast<float>(sample)) != sample;
        }), "the Float64 waveform contains samples a Float32 output would round away");
    const auto plan = planLaunchCapture(buffer, {0, 0}, {frames, endTick}, {});
    require(plan.error == LaunchCaptureError::None && plan.spans.size() == 1 && plan.pcmRanges.size() == 2,
            "one continuous raw run supplies two exact gain ranges");
    const auto& before = plan.pcmRanges[0];
    const auto& after = plan.pcmRanges[1];
    require(before.beginSample == 100 && before.endSample == 200 && before.firstSourceFrame == 0 &&
            before.playbackGain == 1.0f && !before.liveRegionEdit &&
            after.beginSample == 200 && after.endSample == frames && after.firstSourceFrame == 100 &&
            after.playbackGain == multiplier && !after.liveRegionEdit && before.run == after.run,
            "a new PCM gain interval starts without resetting source phase or run identity");
    constexpr SessionPerformanceAudio audioProof{wide ? Precision::Float64 : Precision::Float32, false};
    auto invalidGain = plan;
    invalidGain.pcmRanges[1].playbackGain = std::numeric_limits<float>::infinity();
    require(!prepareSessionPerformance(*project.store, *source, invalidGain, sampleRate, std::nullopt, audioProof).ok,
            "the replay writer refuses a non-finite multiplier");
    const auto prepared = prepareSessionPerformance(*project.store, *source, plan, sampleRate, std::nullopt, audioProof);
    require(prepared.ok, prepared.error);
    const auto committed = clip_edit::commitPrepared(*project.store, prepared);
    require(committed.ok, committed.error);
    require(render<Sample>(*project.store, nullptr, nullptr, false, nullptr, 127) == heard,
            "the captured gain step replays bit-for-bit at a different Arrangement block size");
    require(History(*project.store).undo().ok && render<Sample>(*project.store) == arrangementBefore &&
            History(*project.store).redo().ok && render<Sample>(*project.store) == heard,
            "one Undo and Redo restore the complete raw gain take");
    require(project.store->close(), "close the performed gain capture");
    project.store.reset();
    StoreError openError{};
    project.store = Store::open(project.file, openError, true);
    require(project.store && render<Sample>(*project.store) == heard, "reopen preserves the exact gain step");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0, "the gain tape remains callback-safe");
    std::printf("PASS -- live raw gain changes at frame 200 and replays bit-for-bit in Float%s through Undo, Redo and reopen\n",
                wide ? "64" : "32");
}

void float64Fold(audio::WavFormat format) {
    Project project;
    const bool floatSource = format == audio::WavFormat::Float32;
    const auto fullScale = format == audio::WavFormat::Pcm16 ? 32768 : 8388608;
    const std::array<std::array<int, 2>, 6> stereo{{
        {-fullScale, -fullScale}, {fullScale - 1, fullScale - 2},
        {-fullScale, -fullScale + 1}, {fullScale - 1, -fullScale},
        {0, 1}, {27, -3},
    }};
    std::array<float, 4096> signal{};
    for (std::size_t index = 0; index < signal.size(); index += 2) {
        const auto& pair = stereo[(index / 2) % stereo.size()];
        signal[index] = floatSource ? 0.1f : static_cast<float>(pair[0]) / static_cast<float>(fullScale);
        signal[index + 1] = floatSource ? 0.2f : static_cast<float>(pair[1]) / static_cast<float>(fullScale);
    }
    project.media("folded-gain.wav", 4, signal, 2, format);
    project.store->db().exec("UPDATE audio_clips SET media_id=4,channel_mode=3,src_start_frames=37,"
        "src_len_frames=1024 WHERE clip_id=11;"
        "INSERT INTO clip_launch(clip_id,ram_mode) VALUES(11,1);");
    std::string error;
    const auto source = captureSessionPerformanceSource(*project.store, error);
    require(source.has_value(), error);
    std::array<LaunchTransition, 4> events{};
    std::array<LaunchCapturedPcmRange, 4> pcm{};
    std::array<LaunchPcmTrackCursor, 1> tracks{{{.track = 1}}};
    LaunchCaptureBuffer buffer(events, pcm, tracks);
    const LiveAudioEdit edit{.gain = 10.0f};
    std::int64_t endTick = 0;
    const auto heard = render<double>(*project.store, &buffer, &endTick, true, nullptr, blockFrames, &edit);
    const auto plan = planLaunchCapture(buffer, {0, 0}, {frames, endTick}, {});
    require(plan.error == LaunchCaptureError::None && plan.pcmRanges.size() == 2,
            "capture both stereo Fold gain ranges from the resident run");
    const auto prepared = prepareSessionPerformance(*project.store, *source, plan, sampleRate, std::nullopt,
        SessionPerformanceAudio{Precision::Float64, false});
    if (floatSource) {
        require(heard[200] == static_cast<double>(0.15f) * 10 && heard[200] !=
                (static_cast<double>(0.1f) + static_cast<double>(0.2f)) / 2 * 10,
                "the stored Float32 stereo fold can round before the wide graph applies live gain");
        require(!prepared.ok && prepared.error.find("channel folding") != std::string::npos,
                "capture refuses the inexact Float32 Fold path");

        std::array<LaunchTransition, 4> authoredEdges{};
        LaunchCaptureBuffer authoredBuffer(authoredEdges);
        const auto authored = render<double>(*project.store, &authoredBuffer, &endTick, true);
        constexpr auto arrangementFold = (static_cast<double>(0.1f) + static_cast<double>(0.2f)) / 2;
        require(authored[100] == static_cast<double>(0.15f) && authored[100] != arrangementFold,
                "an unedited resident Float32 stereo Fold also rounds before the wide graph");
        const auto authoredPlan = planLaunchCapture(authoredBuffer, {0, 0}, {frames, endTick}, {});
        require(authoredPlan.error == LaunchCaptureError::None && authoredPlan.pcmRanges.empty(),
                "the authored Fold has no PCM edit ranges");
        const auto authoredWide = prepareSessionPerformance(*project.store, *source, authoredPlan, sampleRate,
            std::nullopt, SessionPerformanceAudio{Precision::Float64, true});
        const auto defaultProof = prepareSessionPerformance(*project.store, *source, authoredPlan, sampleRate);
        require(defaultProof.ok && clip_edit::commitPrepared(*project.store, defaultProof).ok,
                "the existing default-precision authored Fold capture is still allowed");
        const auto wideReplay = render<double>(*project.store, nullptr, nullptr, false, nullptr, 127);
        require(wideReplay[100] == arrangementFold && wideReplay[100] != authored[100] &&
                render(*project.store)[100] == 0.15f,
                "the real unedited Arrangement Fold agrees at Float32 and differs at Float64");
        require(!authoredWide.ok && authoredWide.error.find("channel folding") != std::string::npos,
                "explicit Float64 replay also refuses an inexact Fold without a PCM edit tape");
        return;
    }
    for (std::size_t frame = 0; frame < heard.size(); ++frame) {
        auto expected = 0.125;
        if (frame >= 100) {
            const auto& pair = stereo[(37 + frame - 100) % stereo.size()];
            const auto mean = static_cast<double>(pair[0] + pair[1]) / (2 * fullScale);
            expected = mean * (frame < 200 ? 1 : 10);
        }
        require(heard[frame] == expected, "the live wide fold retains every integer PCM fraction through the gain step");
    }
    require(prepared.ok, prepared.error);
    const auto committed = clip_edit::commitPrepared(*project.store, prepared);
    require(committed.ok, committed.error);
    require(render<double>(*project.store, nullptr, nullptr, false, nullptr, 127) == heard,
            "the captured integer stereo Fold replays exactly at a different callback width");
    require(project.store->close(), "close the captured integer stereo Fold project");
    project.store.reset();
    StoreError openError{};
    project.store = Store::open(project.file, openError, true);
    require(project.store && render<double>(*project.store, nullptr, nullptr, false, nullptr, 127) == heard,
            "reopening preserves every wide integer stereo Fold sample");
    require(callbackAllocations == 0 && audio::callbackFileIo == 0, "the stereo Fold trace remains callback-safe");
    std::printf("PASS -- stereo PCM%d Fold gain replays bit-for-bit in Float64 through reopen\n",
                format == audio::WavFormat::Pcm16 ? 16 : 24);
}

void fadeTailAtCaptureBoundary() {
    Project project;
    project.store->db().exec(
        "INSERT INTO clip_launch(clip_id,clip_fade,ram_mode) VALUES(11,1,1),(12,0,1)");
    std::string error;
    const auto source = captureSessionPerformanceSource(*project.store, error);
    require(source.has_value(), error);
    const auto tick = [](std::int64_t sample) { return sample * textproj::kPPQ / 24000; };
    const auto head = OpJournal(*project.store).headSeq();
    for (const auto captureFrom : {400, 300, 500}) {
        std::array<LaunchTransition, 8> edges{};
        LaunchCaptureBuffer tape(edges);
        std::int64_t endTick = 0;
        const auto audio = render(*project.store, &tape, &endTick, false, nullptr, blockFrames, nullptr, captureFrom);
        require(audio[400] > -0.375f && audio[491] > -0.375f && audio[492] == -0.375f,
                "the outgoing Fade spans the first 92 frames of the late capture");
        const std::array initial{LaunchCapturedStart{1, captureFrom == 300 ? 11 : 12,
            tick(captureFrom == 300 ? 200 : captureFrom - 300), 1.0f, captureFrom == 300}};
        const auto plan = planLaunchCapture(tape, {captureFrom, tick(captureFrom)}, {frames, endTick}, initial);
        if (captureFrom < 500) {
            require(tape.hasFadeTail() && plan.error == LaunchCaptureError::FadeTail,
                    "driver reports tails that begin at or before the first captured block");
            const auto edit = prepareSessionPerformance(*project.store, *source, plan, sampleRate);
            require(!edit.ok && edit.error.find("Fade tail") != std::string::npos &&
                    OpJournal(*project.store).headSeq() == head,
                    "an unsupported Fade tail refuses exact capture without writing an edit");
        } else {
            require(!tape.hasFadeTail() && plan.error == LaunchCaptureError::None,
                    "an expired Fade tail does not invalidate a later capture");
        }
    }
    require(callbackAllocations == 0 && audio::callbackFileIo == 0,
            "driver Fade receipts stay allocation and file-I/O free");
}

} // namespace

int main() {
    try {
        proof(false);
        proof(true);
        followThroughGraph();
        fractionalRawLoop();
        authoredWarpLoopWithPcmTape();
        liveRawRegion();
        liveRawGain<float>();
        liveRawGain<double>();
        float64Fold(audio::WavFormat::Float32);
        float64Fold(audio::WavFormat::Pcm16);
        float64Fold(audio::WavFormat::Pcm24);
        fadeTailAtCaptureBoundary();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
