// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/blob.hpp"
#include "adi/engine/capture_scene.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::engine;

constexpr std::uint32_t sampleRate = 48000;
constexpr std::int32_t framesPerBlock = 64;
constexpr std::array<float, 5> pattern{0.125f, 0.25f, 0.375f, 0.5f, 0.625f};
int checks = 0, failures = 0;

void check(bool result, const char* description) {
    ++checks;
    if (!result) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

void require(bool result, std::string message) {
    if (!result)
        throw std::runtime_error(std::move(message));
}

struct Project {
    test::TempDirectory root{"capture-scene", "session"};
    std::filesystem::path file = root.path() / "capture.adi";
    std::unique_ptr<Store> store;
    std::vector<std::byte> notes;

    Project() {
        StoreError error;
        store = Store::createEmpty(file, error);
        require(static_cast<bool>(store), "create scene capture project");
        test::applySessionSchema(store->db());
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
            "(1,'audio','Audio',0),(2,'midi','MIDI',1),(9,'master','Main',2);"
            "INSERT INTO plugin_refs(id,format,uid,name,subtype) "
            "VALUES(1,'vst3','test.capture','MIDI Witness','instrument');"
            "INSERT INTO device_chains(id,track_id) VALUES(1,2);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'MIDI Witness');"
            "INSERT INTO scenes(id,ord,name) VALUES(10,3,'First'),(20,8,'Second'),(30,100,'Third');"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'capture-fixture','source.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,name,time_base,pos_ticks,pos_ns,length_ticks,length_ns) VALUES"
            "(11,1,'audio','Playing audio',1,NULL,0,NULL,85000000),"
            "(22,2,'midi','Playing MIDI',0,0,NULL,5765760,NULL),"
            "(33,1,'audio','Queued only',1,NULL,0,NULL,85000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES"
            "(11,1,0,4080),(33,1,0,4080);"
            "INSERT INTO clip_slots(track_id,scene_id,clip_id,has_stop) VALUES"
            "(1,10,11,0),(2,20,22,1),(1,30,33,1);"
            "INSERT INTO clip_launch(clip_id,launch_mode,launch_quant,legato,velocity_amount,follow_enabled,"
            "follow_linked,follow_loop_count,follow_duration_ticks,follow_a,follow_a_scene_id,"
            "follow_b,follow_b_chance,clip_fade,ram_mode)"
            " VALUES(11,1,1,1,0.25,1,0,3,5765760,9,20,1,40,0,1),"
            " (33,0,5,0,0,0,1,1,23063040,0,NULL,0,0,0,1);");
        NoteRecord note{};
        note.start_ticks = 24024;
        note.dur_ticks = 480480;
        note.note_id = 0xfedcba9876543210ULL;
        note.key = 70;
        note.vel_on = 90;
        note.vel_off = 31;
        note.channel = 2;
        note.probability = 10000;
        notes = writeStream<NoteRecord>(FourCC::Notes, std::vector{note});
        require(store->putEventStream(22, "notes", notes), "write fixture notes");
        test::seedSessionMarker(store->db());

        constexpr auto fileFrames = std::uint32_t{4080};
        std::vector<float> wave(fileFrames);
        for (std::size_t i = 0; i < wave.size(); ++i)
            wave[i] = pattern[i % pattern.size()];
        audio::WavWriter writer(root.path() / "source.wav", sampleRate, 1, audio::WavFormat::Float32);
        writer.write(wave.data(), fileFrames);
        writer.close();
    }

    std::int64_t scalar(const std::string& sql) const { return store->db().execAndGet(sql).getInt64(); }
    std::string sceneOrder() const {
        return store->db().execAndGet("SELECT group_concat(id, '|') FROM (SELECT id FROM scenes ORDER BY ord)")
            .getString();
    }
};

struct MidiWitness final : device::DeviceInstance {
    device::DeviceIdentity descriptor;
    std::array<Event, 8> notes{};
    std::size_t count = 0;

    const device::DeviceIdentity& identity() const noexcept override { return descriptor; }
    bool loaded() const noexcept override { return true; }
    EventFlow eventFlow() const noexcept override { return EventFlow::Consume; }
    void process(const NodeIo& io) noexcept override {
        for (const auto& event : io.events)
            if ((event.type == EventType::NoteOn || event.type == EventType::NoteOff) && count < notes.size())
                notes[count++] = event;
        for (std::int32_t channel = 0; channel < io.channels; ++channel)
            std::fill_n(io.out[channel] + io.blockOffset, io.frames, 0.0f);
    }
};

LaunchRequest slotRequest(std::uint64_t incarnation, std::int64_t track,
                          std::int64_t scene, std::int64_t clip) {
    LaunchRequest request;
    request.kind = LaunchRequestKind::SlotPress;
    request.incarnation = incarnation;
    request.track = track;
    request.scene = scene;
    request.clip = clip;
    return request;
}

bool render(Session& session, std::int64_t start) {
    std::array<float, framesPerBlock> left{}, right{};
    float* channels[]{left.data(), right.data()};
    AudioIo io;
    io.frames = framesPerBlock;
    io.numOut = 2;
    io.out = channels;
    io.streamTimeSamples = start;
    auto block = session.beginRender();
    if (!block.process(io))
        return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto value = pattern[(static_cast<std::size_t>(start) + i) % pattern.size()];
        if (left[i] != value || right[i] != value) {
            std::printf("captured render sample %lld was %.9g/%.9g, expected %.9g\n",
                static_cast<long long>(start + static_cast<std::int64_t>(i)), left[i], right[i], value);
            return false;
        }
    }
    return true;
}

void testCapture() {
    Project project;
    CaptureInsertedScene prepared;
    std::int64_t copiedAudio = 0, copiedMidi = 0;
    {
        Session session;
        SessionSpec spec;
        spec.sampleRate = sampleRate;
        spec.maxFrames = framesPerBlock;
        session.graph().setFadeFrames(0);
        MidiWitness* witness = nullptr;
        const auto loaded = session.load(*project.store, [&](const DeviceRequest&, std::string&) {
            auto instrument = std::make_unique<MidiWitness>();
            witness = instrument.get();
            return instrument;
        }, spec);
        require(loaded, session.error());
        require(witness != nullptr, "MIDI witness loaded");
        const auto incarnation = session.launchIncarnation();
        require(incarnation != 0, "Session launch runtime loaded");
        // MIDI uses the Set's default bar here. Both clip launches start at 0.
        const std::array start{slotRequest(incarnation, 1, 10, 11), slotRequest(incarnation, 2, 20, 22)};
        require(session.postLaunchGesture(start), "start the two stored slots");
        session.transport().play();
        check(render(session, 0), "source waveform plays before capture");
        require(session.postLaunchGesture(std::array{slotRequest(incarnation, 1, 30, 33)}),
            "queue a different slot for the next bar");
        check(render(session, 64), "queued slot leaves the current waveform active");
        require(witness->count == 1 && witness->notes[0].type == EventType::NoteOn &&
                witness->notes[0].frame == 36, "MIDI note is held across the capture boundary");
        const auto voice = witness->notes[0].noteId;
        std::array<LaunchTrackStatus, 16> rows{};
        LaunchStatusHeader header;
        require(session.readLaunchStatus(rows, header), "read coherent driver status");
        require(header.trackCount == 2, "two launch-enabled tracks");
        check(rows[0].clip == 11 && rows[0].queued.clip == 33 && rows[1].clip == 22,
              "driver reports current active clips independently of the queued launch");
        SceneCaptureSnapshot snapshot{header, rows, incarnation, session.realisedAppliedSequence(),
            OpJournal(*project.store).headSeq()};

        auto stale = snapshot;
        ++stale.currentIncarnation;
        check(!prepareCaptureAndInsertScene(*project.store, stale, 10).edit.ok, "wrong incarnation refuses");
        stale = snapshot;
        stale.publishedHead = 42;
        check(!prepareCaptureAndInsertScene(*project.store, stale, 10).edit.ok, "wrong published head refuses");
        stale = snapshot;
        ++stale.publishedSequence;
        check(!prepareCaptureAndInsertScene(*project.store, stale, 10).edit.ok,
              "wrong published sequence refuses");
        stale = snapshot;
        stale.header.transportPlaying = false;
        check(!prepareCaptureAndInsertScene(*project.store, stale, 10).edit.ok, "stopped status refuses");
        auto emptyRows = rows;
        emptyRows[0].source = LaunchTrackSource::Arrangement;
        emptyRows[1].source = LaunchTrackSource::Silence;
        auto empty = snapshot;
        empty.tracks = emptyRows;
        check(!prepareCaptureAndInsertScene(*project.store, empty, 10).edit.ok, "no active Session clips refuses");
        emptyRows[0] = rows[0];
        emptyRows[0].clip = 33;
        check(!prepareCaptureAndInsertScene(*project.store, empty, 10).edit.ok,
              "driver identity that no longer owns its slot refuses");
        check(!prepareCaptureAndInsertScene(*project.store, snapshot, 1234).edit.ok, "missing selection refuses");
        check(project.scalar("SELECT count(*) FROM ops") == 0 &&
              project.scalar("SELECT count(*) FROM scenes") == 3, "preparation leaves project unchanged");

        prepared = prepareCaptureAndInsertScene(*project.store, snapshot, 10, "Captured");
        require(prepared.edit.ok, prepared.edit.error);
        require(prepared.transfers.size() == 2, "one captured clip per currently playing slot");
        copiedAudio = prepared.transfers[0].newClip;
        copiedMidi = prepared.transfers[1].newClip;
        check(prepared.scene == 31 && prepared.incarnation == incarnation && prepared.statusRevision == header.revision &&
              prepared.transfers[0] == CapturedSceneTransfer{1, 10, 11, copiedAudio, rows[0].sourceTick, rows[0].run} &&
              prepared.transfers[1] == CapturedSceneTransfer{2, 20, 22, copiedMidi, rows[1].sourceTick, rows[1].run},
              "preparation records literal source and destination identities for the handover");
        const auto applied = commitCaptureAndInsertScene(*project.store, session, prepared);
        require(applied.durable.ok, applied.durable.error);
        require(applied.retargetQueued, applied.retargetError);
        check(project.sceneOrder() == "10|31|20|30", "new scene inserts directly below the selection");
        const auto edit = History(*project.store).nextUndo();
        check(edit && edit->label == "Capture and Insert Scene" &&
              edit->opCount == static_cast<int>(prepared.edit.requests.size()) &&
              edit->txnId == applied.durable.txnId, "one Undo entry covers the whole capture");
        check(project.scalar("SELECT count(*) FROM clip_slots WHERE scene_id=31") == 2 &&
              project.scalar("SELECT count(*) FROM clip_slots WHERE scene_id=31 AND has_stop=0") == 1,
              "new scene preserves the two occupied cells and their Stop property");
        auto originalSetting = adi::detail::captureRow(project.store->db(), "clip_launch", "clip_id", 11);
        require(originalSetting.has_value(), "original launch settings row");
        (*originalSetting)["clip_id"] = copiedAudio;
        check(adi::detail::captureRow(project.store->db(), "clip_launch", "clip_id", copiedAudio) == originalSetting &&
              !adi::detail::captureRow(project.store->db(), "clip_launch", "clip_id", copiedMidi),
              "clone preserves the complete launch settings row and absence for a default clip");
        check(project.store->getEventStream(copiedMidi, "notes") == project.notes,
              "copied note stream retains full-width identities and exact bytes");
        check(commitCaptureAndInsertScene(*project.store, prepared).stale,
              "reusing a prepared capture after a published write refuses");

        check(render(session, 128) && render(session, 192),
              "capture publishes without resetting the playing waveform across 128 samples");
        std::array<LaunchTrackStatus, 16> after{};
        LaunchStatusHeader afterHeader;
        require(session.readLaunchStatus(after, afterHeader), "read status after capture handover");
        check(after[0].clip == copiedAudio && after[1].clip == copiedMidi &&
              after[0].scene == 31 && after[1].scene == 31 &&
              after[0].run == rows[0].run && after[1].run == rows[1].run &&
              after[0].queued.clip == 33, "both running slots take on the new scene without changing their run");
        check(witness->count == 1, "the held MIDI voice does not retrigger in the new scene");
        for (std::int64_t sample = 256; sample < 2112; sample += framesPerBlock)
            require(render(session, sample), "retargeted audio retains its source position");
        check(witness->count == 2 && witness->notes[1].type == EventType::NoteOff &&
              witness->notes[1].noteId == voice && witness->notes[1].frame == 52,
              "the copied MIDI slot releases the original voice at sample 2100");
    }
    auto undone = History(*project.store).undo();
    require(undone.ok, undone.error);
    check(project.sceneOrder() == "10|20|30" && project.scalar("SELECT ord FROM scenes WHERE id=30") == 100 &&
          project.scalar("SELECT count(*) FROM clips") == 3, "Undo restores exact original ordering and material");
    auto redone = History(*project.store).redo();
    require(redone.ok, redone.error);
    check(project.sceneOrder() == "10|31|20|30" && project.store->getEventStream(copiedMidi, "notes") == project.notes,
          "Redo keeps literal copy identities and exact note bytes");
    require(project.store->close(), "close scene capture project");
    StoreError error;
    auto reopened = Store::open(project.file, error, true);
    require(static_cast<bool>(reopened), "reopen scene capture project read-only");
    check(reopened->db().execAndGet("SELECT count(*) FROM clip_slots WHERE scene_id=31").getInt() == 2 &&
          reopened->getEventStream(copiedMidi, "notes") == project.notes,
          "reopen retains captured scene and clip material");
    check(commitCaptureAndInsertScene(*reopened, prepared).stale,
          "same path reopened with a new Store connection refuses an old prepared capture");
}

struct LaunchEdges {
    std::array<std::pair<LaunchTransition, std::int64_t>, 8> tracks{};
    std::array<std::pair<LaunchSceneTransition, std::int64_t>, 8> scenes{};
    std::size_t trackCount = 0, sceneCount = 0;

    void render(Session& session, std::int64_t sample) {
        std::array<float, framesPerBlock> left{}, right{};
        float* channels[]{left.data(), right.data()};
        AudioIo io;
        io.frames = framesPerBlock;
        io.numOut = 2;
        io.out = channels;
        io.streamTimeSamples = sample;
        auto block = session.beginRender();
        block.observeLaunchTransitions(this, [](void* ctx, const LaunchTransition& edge, std::int64_t time) noexcept {
            auto& output = *static_cast<LaunchEdges*>(ctx);
            if (output.trackCount < output.tracks.size())
                output.tracks[output.trackCount++] = {edge, time};
        });
        block.observeSceneLaunches(this, [](void* ctx, const LaunchSceneTransition& edge, std::int64_t time) noexcept {
            auto& output = *static_cast<LaunchEdges*>(ctx);
            if (output.sceneCount < output.scenes.size())
                output.scenes[output.sceneCount++] = {edge, time};
        });
        require(block.process(io), "render the live queued target through the published graph");
    }
};

OpRequest targetEdit(const char* type, Payload payload) {
    OpRequest result;
    result.opType = type;
    result.payload = std::move(payload);
    result.label = "Queued Session target test";
    return result;
}

void testQueuedTargets() {
    enum class Edit { ReorderScene, DeleteScene, MoveClip, DeleteClip };
    for (const auto edit : {Edit::ReorderScene, Edit::DeleteScene, Edit::MoveClip, Edit::DeleteClip}) {
        Project project;
        project.store->db().exec(
            "UPDATE clip_launch SET follow_enabled=0 WHERE clip_id=11;"
            "INSERT INTO clips(id,track_id,kind,name,time_base,pos_ns,length_ns) "
            "VALUES(44,1,'audio','Different replacement',1,10000000000,85000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(44,1,0,4080);");
        Session session;
        SessionSpec spec;
        spec.sampleRate = sampleRate;
        spec.maxFrames = framesPerBlock;
        session.graph().setFadeFrames(0);
        const auto loaded = session.load(*project.store, [](const DeviceRequest&, std::string&) {
            return std::make_unique<MidiWitness>();
        }, spec);
        require(loaded, session.error());
        const auto incarnation = session.launchIncarnation();
        require(incarnation != 0, "load queued target runtime");
        constexpr auto nextBar = std::int64_t{96000};
        constexpr auto start = std::int64_t{95817};
        session.transport().locate(start);
        session.transport().play();
        require(session.postLaunchGesture(std::array{slotRequest(incarnation, 1, 10, 11)}),
                "start the stable source clip");
        LaunchEdges edges;
        edges.render(session, start);

        const bool sceneEdit = edit == Edit::ReorderScene || edit == Edit::DeleteScene;
        auto wanted = slotRequest(incarnation, 1, 30, 33);
        if (sceneEdit) {
            wanted = {};
            wanted.kind = LaunchRequestKind::ScenePress;
            wanted.scene = 20;
            wanted.incarnation = incarnation;
        }
        require(session.postLaunchGesture(std::array{wanted}), "queue the literal edit target");
        edges.render(session, start + framesPerBlock);
        std::array<LaunchTrackStatus, 16> before{};
        LaunchStatusHeader header;
        require(session.readLaunchStatus(before, header), "read queued target status");
        const auto& queued = before[sceneEdit ? 1u : 0u].queued;
        check(before[0].clip == 11 && queued.action == LaunchQueuedAction::Start &&
              queued.scene == (sceneEdit ? 20 : 30) && queued.clip == (sceneEdit ? 22 : 33),
              "pending launch retains the original scene and clip IDs");
        const auto oldRun = before[0].run;

        std::vector<OpRequest> batch;
        switch (edit) {
        case Edit::ReorderScene:
            batch.push_back(targetEdit("scene.reorder", {{"scenes", {20,10,30}}}));
            break;
        case Edit::DeleteScene:
            batch.push_back(targetEdit("scene.delete", {{"id", 20}}));
            break;
        case Edit::MoveClip:
            batch.push_back(targetEdit("session.setSlotClip", {{"track", 1}, {"scene", 20}, {"clip", 33}}));
            break;
        case Edit::DeleteClip:
            batch.push_back(targetEdit("session.clearSlot", {{"track", 1}, {"scene", 30}}));
            break;
        }
        if (!sceneEdit)
            batch.push_back(targetEdit("session.setSlotClip", {{"track", 1}, {"scene", 30}, {"clip", 44}}));
        const auto* publishedTiming = session.graph().currentTransportState();
        const auto applied = OpJournal(*project.store).commit(batch);
        require(applied.ok, applied.error);
        const auto refreshed = session.refresh(*project.store);
        require(refreshed, session.error());
        check(session.graph().currentTransportState() != publishedTiming,
              "the edit reached a new Session graph before the launch boundary");
        edges.trackCount = edges.sceneCount = 0;
        edges.render(session, start + 2 * framesPerBlock);
        std::array<LaunchTrackStatus, 16> after{};
        require(session.readLaunchStatus(after, header), "read launch state across the bar");

        if (edit == Edit::ReorderScene) {
            check(project.sceneOrder() == "20|10|30" && edges.sceneCount == 1 &&
                edges.scenes[0].first.scene == 20 && edges.scenes[0].second == nextBar,
                "a reordered scene stays queued by ID and fires at sample 96000");
            check(after[1].clip == 22 && std::any_of(edges.tracks.begin(),
                edges.tracks.begin() + static_cast<std::ptrdiff_t>(edges.trackCount), [](const auto& edge) {
                    return edge.first.newClip == 22 && edge.second == nextBar && edge.first.offset == 55;
                }), "scene follower retains its clip identity and exact within-block offset");
        } else {
            check(edges.trackCount == 0 && edges.sceneCount == 0 && after[0].clip == 11 &&
                  after[0].run == oldRun && after[0].queued.action == LaunchQueuedAction::None,
                  "the old run continues at the boundary when a queued literal target disappears");
            if (!sceneEdit)
                check(project.scalar("SELECT clip_id FROM clip_slots WHERE track_id=1 AND scene_id=30") == 44 &&
                      after[0].clip != 33 && after[0].clip != 44,
                      "neither the moved clip nor a replacement in its former cell fires");
        }
    }
}
} // namespace

int main() {
    try {
        testCapture();
        testQueuedTargets();
    } catch (const std::exception& error) {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
    std::printf("capture scene: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
