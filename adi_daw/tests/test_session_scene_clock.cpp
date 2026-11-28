// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace { std::atomic<unsigned> allocations{0}; }
void adi::test::countReplacedNew() noexcept { if (adi::audio::onAudioThread) ++allocations; }

namespace {
using namespace adi;
using namespace adi::engine;
constexpr auto quarter = textproj::kPPQ;
constexpr std::int32_t blockSize = 257;

void require(bool success, const std::string& message) {
    if (!success) throw std::runtime_error(message);
}

struct ClockProbe final : Node {
    std::array<DeviceTransport, 8> changes{};
    std::size_t count = 0;

    [[nodiscard]] bool wantsDeviceTransport() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override {
        if (const auto* clock = io.deviceTransport; clock && clock->playing && count < changes.size()) {
            const auto& previous = changes[count == 0 ? 0 : count - 1];
            if (count == 0 || clock->bpm != previous.bpm || clock->numerator != previous.numerator ||
                clock->denominator != previous.denominator)
                changes[count++] = *clock;
        }
        for (std::int32_t channel = 0; channel < io.channels; ++channel)
            std::fill_n(io.out[channel] + io.blockOffset, io.frames, 0.0f);
    }
};

struct Render {
    Session& session;
    std::array<LaunchSceneTransition, 6> scenes{};
    std::size_t count = 0;
    std::array<LaunchTransition, 8> clips{};
    std::size_t clipCount = 0;
    std::int64_t stream = 0;
    std::array<float, blockSize> left{}, right{};

    void runThrough(std::int64_t sample) {
        float* outputs[]{left.data(), right.data()};
        while (session.transport().position() <= sample) {
            AudioIo io{outputs, nullptr, 2, 0, blockSize, stream};
            bool rendered;
            {
                auto block = session.beginRender();
                block.observeSceneLaunches(this, [](void* user, const LaunchSceneTransition& scene, std::int64_t) noexcept {
                    auto& self = *static_cast<Render*>(user);
                    if (self.count < self.scenes.size()) self.scenes[self.count++] = scene;
                });
                block.observeLaunchTransitions(this, [](void* user, const LaunchTransition& edge, std::int64_t) noexcept {
                    auto& self = *static_cast<Render*>(user);
                    if (self.clipCount < self.clips.size()) self.clips[self.clipCount++] = edge;
                });
                rendered = block.process(io);
            }
            require(rendered, "render exact scene clock through a pinned graph");
            stream += blockSize;
        }
    }
};

void exercise() {
    test::TempDirectory folder{"scene_clock", "project"};
    StoreError error{};
    auto store = Store::createEmpty(folder.path() / "clock.adi", error);
    require(static_cast<bool>(store), "create scene clock project");
    test::applySessionSchema(store->db());
    store->db().exec(
        "INSERT INTO project(id,name,sample_rate) VALUES(1,'Scene clock',48000);"
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Clock probe',0),(9,'master','Main',1);"
        "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
        "INSERT INTO time_signature_map(id,pos_ticks,numerator,denominator) VALUES(1,0,4,4);"
        "INSERT INTO scenes(id,ord,tempo_bpm,tempo_enabled,signature_numerator,signature_denominator,signature_enabled) "
        "VALUES(10,0,140,1,4,4,0),(20,1,125,0,7,8,1),(30,2,120,0,4,4,0);");
    ClockProbe device;
    Session session;
    session.setSourcesFor([&](std::int64_t track) {
        return track == 1 ? std::vector<Node*>{&device} : std::vector<Node*>{};
    });
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2, 48000, blockSize})) throw std::runtime_error(session.error());
    const auto incarnation = session.launchIncarnation();
    require(incarnation != 0, "publish persisted scene clock metadata");
    LaunchRequest unprepared;
    unprepared.kind = LaunchRequestKind::ScenePress;
    unprepared.scene = 10;
    unprepared.incarnation = incarnation;
    require(!session.postLaunchGesture(std::array{unprepared}), "ordinary clock-bearing scene waits for owner preparation");

    session.transport().play();
    Render render{session};
    render.runThrough(0);
    require(!session.prepareSceneLaunch(*store, 10, 0), "already elapsed boundary refuses the whole command");
    const auto audit = [&](std::int64_t scene, std::uint64_t stamp,
                           const char* operation = "session.launchScene") {
        OpRequest next;
        next.opType = operation;
        next.payload = {{"scene", scene}, {"incarnation", stamp}};
        require(OpJournal(*store).commit(next).ok, "audit a scene launch through the live owner");
    };
    audit(10, incarnation + 1);
    audit(10, incarnation);
    require(!session.prepareSceneLaunch(*store, 20, 8 * quarter), "an unperformed committed scene cannot be replaced");
    require(!session.setExternalTempo(*store, 150) && !session.setExternalTempoAt(*store, 160, 6 * quarter),
        "both external clock owners wait for the committed scene");
    render.runThrough(400);
    audit(10, incarnation, "session.cancelSceneLaunch");
    render.runThrough(700);
    require(render.count == 0 && device.count == 1 && device.changes[0].bpm == 120,
        "cancel replaces the unperformed clock and driver action together");
    audit(10, incarnation);
    render.runThrough(96'000);
    require(render.count == 1 && render.scenes[0].scene == 10 && render.scenes[0].sample == 96'000 &&
        render.scenes[0].preparedToken != 0 && render.scenes[0].offset != 0,
        "tempo scene performed at the exact interior sample");
    require(device.count == 2 && device.changes[0].bpm == 120 && device.changes[1].bpm == 140 &&
        device.changes[1].sample == 96'000 && device.changes[1].numerator == 4,
        "device uses the shared 120-to-140 BPM clock at the exact scene boundary");

    audit(20, incarnation);
    render.runThrough(178'286);
    require(render.count == 2 && render.scenes[1].scene == 20 && render.scenes[1].sample == 178'286 &&
        device.count == 3 && device.changes[2].sample == 178'286 && device.changes[2].bpm == 140 &&
        device.changes[2].numerator == 7 && device.changes[2].denominator == 8,
        "meter changes independently while disabled scene tempo stays dormant");

    audit(30, incarnation);
    render.runThrough(250'286);
    require(render.count == 3 && render.scenes[2].scene == 30 && render.scenes[2].sample == 250'286 &&
        render.scenes[2].tick == 8 * quarter + 7 * quarter / 2,
        "next ordinary scene launches on the actual 7/8 Global grid");
    require(store->db().execAndGet("SELECT bpm FROM tempo_map WHERE pos_ticks=0").getDouble() == 120 &&
        store->db().execAndGet("SELECT count(*) FROM tempo_map").getInt() == 1 &&
        store->db().execAndGet("SELECT numerator FROM time_signature_map WHERE pos_ticks=0").getInt() == 4 &&
        store->db().execAndGet("SELECT count(*) FROM time_signature_map").getInt() == 1,
        "the effective clock leaves authored tempo and signature rows intact");
    require(allocations == 0 && audio::callbackFileIo == 0, "scene clock handling performs no allocation or file I/O on the driver");
    std::printf("PASS -- 120→140 at 96000, 7/8 at 178286, next scene at 250286, 0 callback allocations/file calls\n");
}

void stopPreparedScenes(Store& store) {
    store.db().exec(
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(2,'audio','Other Session track',2);"
        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(103,2,'audio',0,11531520);"
        "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) "
        "SELECT 103,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers FROM audio_clips WHERE clip_id=102;"
        "INSERT INTO clip_slots(track_id,scene_id,clip_id,has_stop) VALUES(2,10,NULL,0),(2,20,103,1);"
        "INSERT INTO clip_launch(clip_id,clip_fade,ram_mode) VALUES(103,0,1);");

    enum class StopCase { Track, AllEarly, AllLate };
    constexpr std::int64_t cut = 96'000;
    constexpr auto boundary = 4 * quarter;
    for (const auto mode : {StopCase::Track, StopCase::AllEarly, StopCase::AllLate}) {
        Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(store, {}, {2,48000,blockSize})) throw std::runtime_error(session.error());
        const auto stamp = session.launchIncarnation();
        const auto audit = [&](const char* operation, Payload payload) {
            payload["incarnation"] = stamp;
            OpRequest request;
            request.opType = operation;
            request.payload = std::move(payload);
            require(OpJournal(store).commit(request).ok,
                "audit a stop or scene on the live Session owner");
        };
        audit("session.launchScene", {{"scene", 10}});
        Render render{session};
        render.runThrough(0);
        audit("session.launchScene", {{"scene", 20}});
        require(!session.prepareSceneLaunch(store, 20, 8 * quarter), "prepare the audited clock scene before requesting stop");
        // The late case is inside a single buffer of the cut. The new clock
        // can no longer be withdrawn, but its tracks can still be stopped.
        render.runThrough(mode == StopCase::AllLate ? cut - blockSize : 400);
        if (mode == StopCase::Track) audit("session.stopTrack", {{"track", 1}});
        else audit("session.stopAll", {});

        float before = 0, afterFade = 0;
        while (session.transport().position() <= cut + 192) {
            const auto first = session.transport().position();
            render.runThrough(first);
            if (first <= cut - 1 && cut - 1 < first + blockSize)
                before = render.left[static_cast<std::size_t>(cut - 1 - first)];
            if (first <= cut + 192 && cut + 192 < first + blockSize)
                afterFade = render.left[static_cast<std::size_t>(cut + 192 - first)];
        }
        const auto edges = std::span(render.clips).first(render.clipCount);
        require(std::ranges::any_of(edges, [&](const LaunchTransition& edge) {
            return edge.track == 1 && edge.oldClip == 101 && edge.newClip == 0 && edge.sample == cut;
        }), "stop the already-running track at the bar");
        require(std::ranges::none_of(edges, [](const LaunchTransition& edge) { return edge.newClip == 102; }) &&
            std::abs(before - 0.25f) < 0.0005f, "old audio reaches the stop boundary and its canceled follower stays silent");

        double bpm;
        {
            const auto pin = session.beginRender();
            bpm = pin.bpmAtTick(boundary);
        }
        if (mode == StopCase::AllEarly) {
            require(render.count == 1 && bpm == 120 && afterFade == 0,
                "early StopAll withdraws the queued tempo action and all replacement clips");
        } else if (mode == StopCase::AllLate) {
            require(render.count == 2 && render.scenes[1].sample == cut && bpm == 140 && afterFade == 0,
                "late StopAll keeps the pinned clock edge and no scene follower keeps playing");
        } else {
            require(render.count == 2 && render.scenes[1].sample == cut && bpm == 140 &&
                std::ranges::any_of(edges, [&](const LaunchTransition& edge) {
                    return edge.track == 2 && edge.newClip == 103 && edge.sample == cut;
                }) && std::abs(afterFade + 0.25f) < 0.0005f,
                "StopTrack leaves the prepared tempo and other track's literal follower intact");
        }
    }
    require(allocations == 0 && audio::callbackFileIo == 0, "prepared scene stops run with no callback allocation or file I/O");
    std::printf("PASS -- audited Track and All stops mask prepared audio at 96000, including early and late clock withdrawal\n");
}

void verifyHandover(Store& store) {
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(store, {}, {2,48000,blockSize})) throw std::runtime_error(session.error());
    LaunchRequest start;
    start.kind = LaunchRequestKind::ScenePress;
    start.scene = 10;
    start.incarnation = session.launchIncarnation();
    require(session.postLaunchGesture(std::array{start}), "queue initial resident sound");
    Render render{session};
    render.runThrough(0);
    require(!session.prepareSceneLaunch(store, 30, 4 * quarter), "retaining an old audio run needs a phase handover");
    if (!session.prepareSceneLaunch(store, 20, 4 * quarter)) throw std::runtime_error(session.error());

    constexpr auto cut = 96'000;
    constexpr std::array samples{cut - 1,cut,cut + 192,cut + 9000,cut + 11'000,cut + 22'000};
    std::array<float, samples.size()> heard{};
    bool rebuilt = false;
    while (session.transport().position() <= samples.back()) {
        const auto first = session.transport().position();
        render.runThrough(first);
        for (std::size_t index = 0; index < samples.size(); ++index) {
            if (samples[index] >= first && samples[index] < first + blockSize)
                heard[index] = render.left[static_cast<std::size_t>(samples[index] - first)];
        }
        if (!rebuilt && first <= cut && first + blockSize > cut) {
            require(session.setDelayCompensation(false), "rebuild graph during outgoing old-tempo fade");
            rebuilt = true;
        }
    }
    const auto close = [](float actual, float expected) { return std::abs(actual - expected) < 0.0005f; };
    require(render.count == 2 && render.scenes[1].sample == cut && render.scenes[1].tempoBpm == 140 &&
        close(heard[0], 0.25f) && close(heard[1], 0.25f) && close(heard[2], -0.25f) &&
        close(heard[3], -0.25f) && close(heard[4], 0.375f) && close(heard[5], 0.0f),
        "old source/fade and new 140 BPM Warp bank agree with the performed scene boundary");
    require(allocations == 0 && audio::callbackFileIo == 0, "paired media preparation keeps callback allocation and I/O at zero");
    std::printf("PASS -- paired banks preserve the 192-frame fade at 96000, including graph rebuild, then play Warp at 140 BPM\n");
}

void verifyResidentTempoHistory(Store& store) {
    constexpr auto firstStep = 4 * quarter;
    constexpr auto secondStep = 8 * quarter;
    constexpr auto nextStep = 12 * quarter;
    constexpr std::int64_t firstCut = 96'000, secondCut = 178'286;
    store.db().exec(
        "INSERT INTO scenes(id,ord,tempo_bpm,tempo_enabled) VALUES(40,3,160,1),(50,4,120,1),(60,5,120,0);"
        "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(3,'unavailable','missing.wav','wav');"
        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(104,1,'audio',0,5765760);"
        "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(104,3,24000);"
        "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,60,104);"
        "INSERT INTO clip_launch(clip_id,ram_mode) VALUES(104,1);"
        "UPDATE clip_launch SET launch_quant=1 WHERE clip_id=102;");
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(store, {}, {2,48000,blockSize})) throw std::runtime_error(session.error());
    const auto stamp = session.launchIncarnation();
    const auto audit = [&](const char* operation, std::int64_t scene) {
        OpRequest message;
        message.opType = operation;
        message.payload = {{"scene",scene},{"incarnation",stamp}};
        require(OpJournal(store).commit(message).ok, "commit the scene audit");
    };
    const auto tempoAt = [&](std::int64_t tick) { return session.beginRender().bpmAtTick(tick); };
    session.transport().play();
    Render render{session};
    render.runThrough(0);
    audit("session.launchScene",20);
    render.runThrough(firstCut);
    require(render.count == 1 && render.scenes[0].sample == firstCut && render.scenes[0].tick == firstStep,
        "the first resident bank changes on the audited 140 BPM boundary");

    audit("session.launchScene",40);
    render.runThrough(secondCut);
    require(render.count == 2 && render.scenes[1].sample == secondCut && render.scenes[1].tick == secondStep &&
        tempoAt(0) == 120 && tempoAt(firstStep) == 140 && tempoAt(secondStep) == 160,
        "two audited tempo changes retain their exact whole-tick performed history");
    audit("session.launchScene",50);
    require(tempoAt(nextStep) == 120, "a later scene can prepare a previous resident tempo");
    audit("session.cancelSceneLaunch",50);
    require(tempoAt(nextStep) == 160, "withdrawing the later scene restores the complete clock and bank history");

    const auto hear = [&](std::int64_t start) {
        constexpr std::array<std::int64_t,2> offsets{9500,11'000};
        std::array<float,offsets.size()> result{};
        session.transport().locate(start);
        const std::array requests{LaunchRequest{LaunchRequestKind::SlotPress,1,20,102,stamp}};
        require(session.postLaunchGesture(requests), "launch the same Warp clip after seeking to a retained tempo");
        while (session.transport().position() <= start + offsets.back()) {
            const auto first = session.transport().position();
            render.runThrough(first);
            for (std::size_t index = 0; index < offsets.size(); ++index) {
                const auto sample = start + offsets[index];
                if (sample >= first && sample < first + blockSize)
                    result[index] = render.left[static_cast<std::size_t>(sample - first)];
            }
        }
        return result;
    };
    const auto matches = [](auto actual, float first, float second) {
        return std::abs(actual[0] - first) < 0.0005f && std::abs(actual[1] - second) < 0.0005f;
    };
    require(matches(hear(1000),-0.25f,-0.25f) && matches(hear(firstCut+1000),-0.25f,0.375f) &&
        matches(hear(secondCut+1000),0.375f,0.375f), "seek selects the physical Warp projection for each of three resident tempos");
    OpRequest arrangementEdit;
    arrangementEdit.opType = "clip.create";
    arrangementEdit.payload = {{"id",120},{"track",1},{"kind","midi"},{"pos",16*quarter},{"length",quarter}};
    const auto arrangement = OpJournal(store).commit(arrangementEdit);
    require(arrangement.ok && session.refresh(store),
        "an unrelated Arrangement clip can rebuild history with a preexisting unavailable slot");
    require(store.db().execAndGet("SELECT clip_id FROM clip_slots WHERE track_id=1 AND scene_id=60").getInt64() == 104 &&
        tempoAt(0) == 120 && tempoAt(firstStep) == 140 && tempoAt(secondStep) == 160 &&
        matches(hear(1000),-0.25f,-0.25f) && matches(hear(secondCut+1000),0.375f,0.375f),
        "the unrelated edit keeps all admitted clocks and the audible source projections");

    const auto source = store.path().parent_path()/"before.wav";
    const auto offline = source.parent_path()/"temporarily-offline.wav";
    std::filesystem::rename(source,offline);
    const auto refused = !session.refresh(store);
    std::filesystem::rename(offline,source);
    require(refused && tempoAt(secondStep) == 160,
        "a previously admitted source going offline refuses the new history publication");
    require(session.refresh(store), "restore the proven resident audio and rebuild");

    store.db().exec("UPDATE clips SET gain_db=6.020599913279624 WHERE id=102");
    require(!session.prepareSceneLaunch(store,50,nextStep) && tempoAt(nextStep) == 160,
        "a stale launch source refuses the new clock and keeps the prior publication");
    require(session.refresh(store), "publish one newly authored source image for all three tempos");
    require(matches(hear(1000),-0.50f,-0.50f) && matches(hear(firstCut+1000),-0.50f,0.75f) &&
        matches(hear(secondCut+1000),0.75f,0.75f), "each retained tempo adopts the same new source image");

    const auto writeSource = [&](float first, float second) {
        std::vector<float> pcm(12000,first);
        std::fill(pcm.begin()+6000,pcm.end(),second);
        audio::WavWriter wave(store.path().parent_path()/"warped.wav",48000,1,audio::WavFormat::Float32);
        wave.write(pcm.data(),static_cast<std::uint32_t>(pcm.size()));
        wave.close();
    };
    writeSource(0.125f,-0.0625f); // The Store rows and media hash still describe the original path.
    require(!session.prepareSceneLaunch(store,50,nextStep) && tempoAt(nextStep) == 160,
        "a changed file revision refuses reuse even when all Store rows are identical");
    require(session.refresh(store), "reprepare all audio projections from the new file revision");
    require(matches(hear(1000),0.25f,0.25f) && matches(hear(firstCut+1000),0.25f,-0.125f) &&
        matches(hear(secondCut+1000),-0.125f,-0.125f), "all three tempos adopt the same externally edited audio file");
    require(!session.setExternalTempoAt(store,135,nextStep,[] { return false; }) && tempoAt(nextStep) == 160 &&
        matches(hear(1000),0.25f,0.25f), "a failed external tempo publication preserves retained audio and clock history");

    const auto setRam = [&](bool ram) {
        OpRequest edit;
        edit.opType = "session.setLaunchSettings";
        edit.payload = {{"clip",102},{"ram",ram}};
        require(OpJournal(store).commit(edit).ok,"commit the RAM mode edit");
        if (!session.refresh(store)) throw std::runtime_error(session.error());
        require((store.db().execAndGet("SELECT ram_mode FROM clip_launch WHERE clip_id=102").getInt() != 0) == ram,
            "successful bank publication preserves the authored RAM mode");
    };
    setRam(false);
    require(tempoAt(0) == 120 && tempoAt(firstStep) == 140 && tempoAt(secondStep) == 160 &&
        matches(hear(1000),0.25f,-0.125f) && matches(hear(firstCut+1000),0.25f,-0.125f) &&
        matches(hear(secondCut+1000),-0.125f,-0.125f),
        "RAM Off keeps the two newest banks audible across seeks and preserves the performed tempo map");
    setRam(true);
    require(matches(hear(1000),0.25f,-0.125f),"RAM On rebuilds only the retained pair after the paged history was published");

    require(session.setExternalTempoAt(store,135,nextStep) && tempoAt(nextStep) == 135,
        "ordinary external tempo changes can replace a retained scene clock after rollback");
    require(render.count == 2 && allocations == 0 && audio::callbackFileIo == 0,
        "retained bank seeks neither perform new scenes nor add driver allocation or file access");
    writeSource(-0.25f,0.375f);
    store.db().exec("UPDATE clips SET gain_db=0 WHERE id=102;"
        "DELETE FROM clips WHERE id=120; DELETE FROM scenes WHERE id=60; DELETE FROM media_files WHERE id=3;");
    std::printf("PASS -- audited 120/140/160 BPM banks survive rollback, seeks and a source refresh at exact frames\n");
}

void audioHandover() {
    test::TempDirectory folder{"scene_audio_clock", "project"};
    StoreError error{};
    auto store = Store::createEmpty(folder.path() / "clock.adi", error);
    require(static_cast<bool>(store), "create actual scene audio project");
    test::applySessionSchema(store->db());
    const auto media = [&](const char* name, std::uint32_t frames, float first, float late) {
        std::vector<float> pcm(frames, first);
        std::fill(pcm.begin() + frames / 2, pcm.end(), late);
        audio::WavWriter wave(folder.path() / name, 48000, 1, audio::WavFormat::Float32);
        wave.write(pcm.data(), frames);
        wave.close();
    };
    media("before.wav", 144000, 0.25f, 0.25f);
    media("warped.wav", 12000, -0.25f, 0.375f);
    store->db().exec(
        "INSERT INTO project(id,name,sample_rate) VALUES(1,'Scene audio',48000);"
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio slots',0),(9,'master','Main',1);"
        "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
        "INSERT INTO scenes(id,ord,tempo_bpm,tempo_enabled) VALUES(10,0,120,0),(20,1,140,1),(30,2,145,1);"
        "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'before','before.wav','wav'),(2,'warped','warped.wav','wav');"
        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(101,1,'audio',0,69189120),(102,1,'audio',0,11531520);"
        "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(101,1,144000);"
        "INSERT INTO clip_slots(track_id,scene_id,clip_id,has_stop) VALUES(1,10,101,1),(1,20,102,1),(1,30,NULL,0);"
        "INSERT INTO clip_launch(clip_id,clip_fade,ram_mode) VALUES(101,1,1),(102,1,1);");
    const auto anchors = audio::writeWarpMap(std::vector<WarpMarker>{{0,0},{12000,quarter}}, 12000);
    require(static_cast<bool>(anchors), "encode scene Warp map");
    SQLite::Statement warp(store->db(), "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) "
                                       "VALUES(102,2,12000,1,'re-pitch',?)");
    warp.bind(1, anchors.bytes.data(), static_cast<int>(anchors.bytes.size()));
    warp.exec();

    verifyHandover(*store);
    verifyResidentTempoHistory(*store);
    stopPreparedScenes(*store);
}

enum class RawClockCase { Seek, Retain, Stop };

void rawAudioClockBank(RawClockCase variant) {
    constexpr std::int64_t cut = 96'000;
    constexpr std::int64_t offsetFrames = 240;
    constexpr auto sampleValue = [](std::int64_t frame) { return static_cast<float>(frame) / 16384.0f; };
    test::TempDirectory folder{"scene_clock_raw", "project"};
    StoreError error{};
    auto store = Store::createEmpty(folder.path() / "raw.adi", error);
    require(static_cast<bool>(store), "create fixed-source tempo scene project");
    test::applySessionSchema(store->db());
    const auto pcm = [&] {
        std::vector<float> result(48000);
        for (std::size_t index = 0; index < result.size(); ++index)
            result[index] = sampleValue(static_cast<std::int64_t>(index));
        return result;
    }();
    {
        audio::WavWriter wave(folder.path() / "ramp.wav", 48000, 1, audio::WavFormat::Float32);
        wave.write(pcm.data(), static_cast<std::uint32_t>(pcm.size()));
        wave.close();
    }
    store->db().exec(
        "INSERT INTO project(id,name,sample_rate) VALUES(1,'Raw tempo scenes',48000);"
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Source',0),(9,'master','Main',1);"
        "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
        "INSERT INTO scenes(id,ord,tempo_bpm,tempo_enabled) VALUES(10,0,120,0),(20,1,140,1);"
        "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'ramp','ramp.wav','wav');"
        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(101,1,'audio',0,11531520);"
        "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled) VALUES(101,1,48000,0);"
        "INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(101,5000000,1000000000);"
        "INSERT INTO clip_slots(track_id,scene_id,clip_id,has_stop) VALUES(1,10,101,1);"
        "INSERT INTO clip_launch(clip_id,launch_quant,clip_fade,ram_mode) VALUES(101,1,0,1);");
    const bool retain = variant != RawClockCase::Seek;
    if (retain) store->db().exec("INSERT INTO clip_slots(track_id,scene_id,clip_id,has_stop) VALUES(1,20,NULL,0)");
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store, {}, {2,48000,blockSize})) throw std::runtime_error(session.error());
    session.transport().play();
    Render render{session};
    if (retain) session.transport().locate(cut - 4096);
    render.runThrough(session.transport().position());
    const auto stamp = session.launchIncarnation();
    const std::array launch{LaunchRequest{LaunchRequestKind::SlotPress, 1, 10, 101, stamp}};
    OpRequest change;
    change.opType = "session.launchScene";
    change.payload = {{"scene", 20}, {"incarnation", stamp}};
    if (retain) {
        auto first = session.transport().position();
        require(session.postLaunchGesture(launch), "launch the native resident source before the tempo change");
        render.runThrough(first);
        std::array<LaunchTrackStatus,1> status;
        LaunchStatusHeader header;
        require(session.readLaunchStatus(status,header) && status[0].clip == 101, "read the retained run identity");
        if (variant == RawClockCase::Retain) {
            const LaunchClipRegionEdit edit{1,101,stamp,status[0].run,{quarter/4,quarter},ClipUpdateRate::Bar};
            require(session.postLaunchClipRegionEdit(edit), "queue an edit to the retained source region");
            require(!session.prepareSceneLaunch(*store,20,4*quarter) &&
                session.error().find("unedited playback bounds") != std::string::npos,
                "a tempo scene waits for unconsumed region edits");
            render.runThrough(session.transport().position());
            require(!session.prepareSceneLaunch(*store,20,4*quarter) &&
                session.error().find("unedited playback bounds") != std::string::npos,
                "a tempo scene refuses a pending region edit on its retained run");
            first = session.transport().position();
            require(session.postLaunchGesture(launch), "restart the clip with its original source window");
            render.runThrough(first);
            require(session.readLaunchStatus(status,header) && status[0].clip == 101, "read the replacement run identity");
        }
        const auto run = status[0].run;
        const auto beforeEdges = render.clipCount;
        require(OpJournal(*store).commit(change).ok, "admit the tempo scene with an explicit No Stop cell");
        const LaunchClipRegionEdit edit{1,101,stamp,run,{quarter/4,quarter},ClipUpdateRate::None};
        require(!session.postLaunchClipRegionEdit(edit), "protect the retained source window until the tempo handover");
        render.runThrough(cut - blockSize);
        const auto blockStart = session.transport().position();
        require(blockStart < cut && blockStart + blockSize > cut, "inspect one callback that spans the tempo boundary");
        if (variant == RawClockCase::Stop) {
            OpRequest stop;
            stop.opType = "session.stopTrack";
            stop.payload = {{"track",1},{"incarnation",stamp}};
            require(OpJournal(*store).commit(stop).ok, "audit a Stop Track while the retained tempo scene waits");
        }
        render.runThrough(cut);
        require(render.count == 1 && render.scenes[0].sample == cut && render.scenes[0].tick == 4 * quarter &&
            render.scenes[0].tempoBpm == 140, "the prepared scene applies tempo at the performed cut");
        for (std::int32_t frame = 0; frame < blockSize; ++frame) {
            const auto sample = blockStart + frame;
            const auto expected = variant == RawClockCase::Stop && sample >= cut
                ? 0.0f : sampleValue(offsetFrames + sample - first);
            const auto index = static_cast<std::size_t>(frame);
            require(render.left[index] == expected && render.right[index] == expected,
                "each output frame uses continuous native PCM through the cut or is silent after an explicit Stop");
        }
        require(session.readLaunchStatus(status,header), "read active Session state after the tempo change");
        if (variant == RawClockCase::Stop) {
            require(status[0].source == LaunchTrackSource::Silence && render.clipCount == beforeEdges + 1 &&
                render.clips[beforeEdges].oldRun == run && render.clips[beforeEdges].newClip == 0 &&
                render.clips[beforeEdges].sample == cut, "explicit Stop owns the same-sample track edge");
        } else {
            require(status[0].run == run && render.clipCount == beforeEdges, "No Stop retains the same run without a launch edge");
            require(session.postLaunchClipRegionEdit(edit), "retained clips accept region edits after the clock performs");
        }
        require(allocations == 0 && audio::callbackFileIo == 0, "native tempo scene crossing has no driver allocation or file reads");
        std::puts(variant == RawClockCase::Stop
            ? "PASS -- audited Stop Track takes precedence at the native tempo boundary"
            : "PASS -- No Stop retains bit-exact native PCM and run identity at the tempo boundary");
        return;
    }
    require(OpJournal(*store).commit(change).ok, "audit the prepared tempo scene on the live owner");
    render.runThrough(cut);
    require(render.count == 1 && render.scenes[0].sample == cut && render.scenes[0].tempoBpm == 140,
        "clock scene already performed before the transport seeks");

    session.transport().locate(1000);
    require(session.postLaunchGesture(launch), "launch the persisted Warp-off source in the old bank");
    render.runThrough(1000);
    const auto verify = [&](std::int64_t frame, const char* message) {
        require(std::abs(render.left.front() - sampleValue(frame)) < 1e-6f &&
            std::abs(render.left.back() - sampleValue(frame + blockSize - 1)) < 1e-6f, message);
    };
    verify(offsetFrames, "native audio begins at its authored nonzero physical start");
    std::array<LaunchTrackStatus, 1> status;
    LaunchStatusHeader header;
    require(session.readLaunchStatus(status, header) && status[0].clip == 101, "raw source is the current Session run");
    const auto activeRun = status[0].run;
    const auto launchEdges = render.clipCount;
    session.transport().locate(cut + 1000);
    render.runThrough(cut + 1000);
    verify(offsetFrames + blockSize, "forward seek into the new bank preserves the next native source frame");
    session.transport().locate(2000);
    render.runThrough(2000);
    verify(offsetFrames + 2 * blockSize, "backward seek into the old bank preserves the next native source frame");
    require(session.readLaunchStatus(status, header) && status[0].run == activeRun && render.clipCount == launchEdges,
        "raw phase is continuous across the two clock banks within one stable launch run");
    require(allocations == 0 && audio::callbackFileIo == 0, "raw clock-bank seeks allocate or read no files in the driver");
    std::printf("PASS -- Warp-off source preserves its physical phase in both directions across prepared tempo banks\n");
}
} // namespace

int main() {
    try {
        exercise();
        audioHandover();
        rawAudioClockBank(RawClockCase::Seek);
        rawAudioClockBank(RawClockCase::Stop);
        rawAudioClockBank(RawClockCase::Retain);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
