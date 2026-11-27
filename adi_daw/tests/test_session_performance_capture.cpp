// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/session_performance_capture.hpp"

#include "adi/blob.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/digest.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/midi_clips.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace {
using namespace adi;
using namespace adi::engine;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

constexpr std::int64_t tick(std::int64_t frame) {
    // All fixture boundaries fall on exact ticks at 120 BPM / 48 kHz.
    return frame * textproj::kPPQ / 24000;
}

LaunchTransition transition(std::int64_t from, std::int64_t to, std::int64_t frame,
                            std::int64_t sourceFrame = 0, bool arrangement = false) {
    LaunchTransition edge;
    edge.track = 21;
    edge.oldClip = from;
    edge.newClip = to;
    edge.sourceTick = tick(sourceFrame);
    edge.sample = frame;
    edge.tick = tick(frame);
    edge.arrangement = arrangement;
    return edge;
}

struct Project {
    test::TempDirectory temporary{"session_performance_capture", "project"};
    std::filesystem::path path = temporary.path() / "Session.adi";
    std::unique_ptr<Store> store;

    Project() {
        StoreError error{};
        store = Store::create(path, error);
        require(store != nullptr, "create test project");
        test::applySessionSchema(db());
        db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
                  "INSERT INTO tracks(id,kind,name) VALUES(21,'midi','Notes'),(22,'midi','Unaffected'),(23,'audio','Samples');"
                  "INSERT INTO lanes(id,track_id,ord,name) VALUES(11,21,0,'Take');"
                  "INSERT INTO scenes(id,ord,name) VALUES(1,0,'First');"
                  "INSERT INTO clips(id,track_id,lane_id,kind,name,pos_ticks,length_ticks) VALUES"
                  "(31,21,NULL,'midi','Arrangement',0,240240),"
                  "(32,21,11,'midi','Take',0,240240),"
                  "(33,22,NULL,'midi','Unaffected',0,240240),"
                  "(51,21,NULL,'midi','Session notes',0,144144),"
                  "(52,23,NULL,'audio','Session loop',0,240240);"
                  "UPDATE clips SET loop_enabled=1,loop_start_ticks=0,loop_len_ticks=24025 WHERE id=52;"
                  "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(88,'source.wav hash','source.wav','wav');"
                  "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(52,88,0,1000);"
                  "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(21,1,51),(23,1,52);");
        NoteRecord note{};
        note.note_id = 176;
        note.start_ticks = tick(100);
        note.dur_ticks = tick(60);
        note.key = 60;
        note.channel = 2;
        note.vel_on = 99;
        note.vel_off = 33;
        note.probability = 10000;
        const std::array records{note};
        const auto bytes = writeStream<NoteRecord>(FourCC::Notes, records);
        SQLite::Statement insert(db(), "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,51,'notes',?)");
        insert.bind(1, bytes.data(), static_cast<int>(bytes.size()));
        insert.exec();
    }

    SQLite::Database& db() { return store->db(); }
    std::int64_t count(const std::string& sql) { return db().execAndGet(sql).getInt64(); }

    SessionPerformanceSource freeze() const {
        std::string error;
        auto source = captureSessionPerformanceSource(*store, error);
        if (!source) throw std::runtime_error(error);
        return std::move(*source);
    }
};

LaunchCapturePlan performance() {
    std::array<LaunchTransition, 3> storage{};
    LaunchCaptureBuffer buffer(storage);
    require(buffer.append(transition(0, 51, 300, 50)) && buffer.append(transition(51, 0, 500)) &&
            buffer.append(transition(0, 0, 700, 0, true)), "perform source-phase launch, Stop, Back to Arrangement");
    return planLaunchCapture(buffer, {200, tick(200)}, {900, tick(900)}, {});
}

void verifyRenderedMidi(Store& store) {
    Transport transport;
    MidiClips playback(rows::readModel(store), transport, 48000.0, makeMidiClipState());
    const auto nodes = playback.sourcesFor(21);
    require(nodes.size() == 1 && playback.problems().empty(), "build reopened Arrangement MIDI player");
    auto& source = *nodes.front();
    transport.play();
    std::array<Event, 32> storage{};
    EventList events(storage.data(), static_cast<std::int32_t>(storage.size()));
    std::array<std::int64_t, 2> samples{};
    std::array<Event, 2> notes{};
    std::size_t count = 0;
    constexpr std::int32_t block = 64;
    for (std::int64_t offset = 0; offset < 1024; offset += block) {
        events.clear();
        source.sourceEvents(events, block);
        for (const auto& event : events) {
            require(count < samples.size(), "reopened MIDI delivers one onset and one release");
            samples[count] = offset + event.frame;
            notes[count++] = event;
            if (event.type == EventType::NoteOn) source.noteOnDelivered(event);
            if (event.type == EventType::NoteOff) source.noteOffDelivered(event);
        }
        transport.advance(block);
    }
    require(count == 2 && samples[0] == 350 && samples[1] == 410 &&
            notes[0].type == EventType::NoteOn && notes[0].value == 99.0 / 127 &&
            notes[1].type == EventType::NoteOff && notes[1].value == 33.0 / 127 &&
            notes[0].noteId == notes[1].noteId && notes[0].channel == 2 && notes[0].dim == 60,
            "reopened MIDI renders the performed note timing, key, channel, and velocities exactly");
}

void captureAndReopen() {
    Project project;
    const auto original = digestProject(*project.store).text;
    const auto source = project.freeze();
    const auto plan = performance();
    const auto edit = prepareSessionPerformance(*project.store, source, plan, 48000);
    if (!edit.ok) throw std::runtime_error(edit.error);
    require(edit.requests.size() == 1 && edit.requests.front().opType == "capture.sessionPerformance", "one reserved capture op");
    require(digestProject(*project.store).text == original, "capture preparation writes nothing");
    const auto result = clip_edit::commitPrepared(*project.store, edit);
    if (!result.ok) throw std::runtime_error(result.error);

    require(project.count("SELECT count(*) FROM clip_slots WHERE clip_id IN (51,52)") == 2 &&
            project.count("SELECT count(*) FROM clips WHERE id=51 AND pos_ticks=0 AND content_offset_ticks=0") == 1,
            "capture keeps both original Session clips owned by their slots");
    require(project.count("SELECT count(*) FROM clips WHERE id IN (32,33) AND pos_ticks=0 AND length_ticks=240240") == 2,
            "nonparticipating track and take lane retain their original clips");
    require(project.count("SELECT count(*) FROM clips WHERE id=31 AND length_ticks=72072") == 1 &&
            project.count("SELECT count(*) FROM clips WHERE name='Arrangement' AND pos_ticks=168168 AND length_ticks=72072") == 1,
            "only the Session-owned part is cleared, including the explicit Stop interval");
    require(project.count("SELECT count(*) FROM clips WHERE name='Session notes' AND pos_ticks=72072 AND "
                          "length_ticks=48048 AND content_offset_ticks=12012 AND lane_id IS NULL") == 1,
            "captured clip preserves the performed source phase and Arrangement placement");
    require(project.count("SELECT count(*) FROM event_streams a,event_streams b JOIN clips c ON c.id=b.clip_id "
                          "WHERE a.clip_id=51 AND c.name='Session notes' AND c.id<>51 AND a.data=b.data AND a.id<>b.id") == 1,
            "new MIDI stream ID retains the exact authored notes and release bytes");
    const auto committed = digestProject(*project.store).text;
    require(History(*project.store).undo().ok && digestProject(*project.store).text == original,
            "one Undo restores the whole Arrangement and original clip rows");
    require(History(*project.store).redo().ok && digestProject(*project.store).text == committed,
            "one Redo applies the frozen literal data");

    project.store.reset();
    StoreError error{};
    project.store = Store::open(project.path, error, false);
    require(project.store && digestProject(*project.store).text == committed,
            "reopen retains captured clips and their exact bytes");
    require(History(*project.store).undo().ok && digestProject(*project.store).text == original &&
            History(*project.store).redo().ok && digestProject(*project.store).text == committed,
            "reopen can replay the same literal capture op");
    verifyRenderedMidi(*project.store);
}

void refusals() {
    Project project;
    const auto source = project.freeze();
    const auto original = digestProject(*project.store).text;
    LaunchCapturePlan plan{LaunchCaptureError::None, {{23, 52, 0, {300, tick(300)}, {350, tick(350)}}}};
    plan.spans.front().fade = true;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok, "unsupported Session fade fails closed");
    plan = performance();
    plan.spans.front().velocity = 0.73f;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok, "unsupported velocity scaling fails closed");
    plan = performance();
    ++plan.spans.front().begin.sample;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok, "uncapturable sample edge fails closed");
    plan = performance();
    plan.spans.front().clip = 31;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok, "Arrangement is not an admitted Session source");
    const LaunchCapturePlan fractional{LaunchCaptureError::None,
        {{23, 52, 0, {300, tick(300)}, {550, tick(550)}}}};
    const auto refused = prepareSessionPerformance(*project.store, source, fractional, 48000);
    require(!refused.ok && refused.error.find("fractional") != std::string::npos, "fractional audio wrap requires exact performed ranges");
    require(digestProject(*project.store).text == original, "unsupported captures do not write any rows");

    OpRequest rename;
    rename.opType = "scene.rename";
    rename.payload = {{"id", 1}, {"name", "Changed"}};
    require(OpJournal(*project.store).commit(rename).ok, "change the frozen capture's journal head");
    require(!prepareSessionPerformance(*project.store, source, performance(), 48000).ok,
            "cannot reconstruct an in-flight capture from changed source rows");
}

void sourceMediaChanged() {
    Project project;
    const auto path = project.path.parent_path() / "source.wav";
    {
        audio::WavWriter writer(path, 48000, 1, audio::WavFormat::Float32);
        const std::array<float, 1000> pcm{};
        writer.write(pcm.data(), 1000);
        writer.close();
    }
    const auto hash = media::blake3File(path);
    require(static_cast<bool>(hash), "verify the capture's actual audio source bytes");
    SQLite::Statement update(project.db(), "UPDATE media_files SET hash_blake3=? WHERE id=88");
    update.bind(1, hash.hex);
    update.exec();
    const auto source = project.freeze();
    const auto original = digestProject(*project.store).text;
    const LaunchCapturePlan plan{LaunchCaptureError::None, {{23, 52, 0, {300, tick(300)}, {350, tick(350)}}}};
    const auto prepared = prepareSessionPerformance(*project.store, source, plan, 48000);
    if (!prepared.ok) throw std::runtime_error(prepared.error);
    {
        std::ofstream changed(path, std::ios::binary | std::ios::app);
        changed.put('x');
        require(static_cast<bool>(changed), "change audio bytes after Arrangement preparation");
    }
    require(!clip_edit::commitPrepared(*project.store, prepared).ok && digestProject(*project.store).text == original,
            "late external media change rolls back rows and history");
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok,
            "original performed image does not adopt newly changed media");
}

} // namespace

int main() {
    try {
        captureAndReopen();
        refusals();
        sourceMediaChanged();
        std::printf("Session performance capture replays literal Arrangement ranges and native MIDI bytes.\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
