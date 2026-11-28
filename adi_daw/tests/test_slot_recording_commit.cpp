// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/controller_data.hpp"
#include "adi/engine/slot_recording.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/recording_commands.hpp"
#include "adi/store.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using namespace adi;
using namespace adi::engine;
using namespace adi::engine::launch;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

constexpr auto keys = midiInputKey("keys");
constexpr auto pads = midiInputKey("pads");

struct Project {
    test::TempDirectory dir{"slot_recording", "attach"};
    std::filesystem::path path = dir.path() / "Song.adi";
    std::unique_ptr<Store> store;

    Project() {
        StoreError error{};
        store = Store::create(dir.path() / "Song.adi", error);
        require(store != nullptr, "create recording project");
        test::applySessionSchema(db());
        db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','Audio'),(22,'midi','MIDI');"
                  "INSERT INTO scenes(id,ord,name) VALUES(11,0,'Capture');"
                  "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) VALUES"
                  "(31,21,'audio','Arrangement audio',0,5765760),"
                  "(32,22,'midi','Arrangement MIDI',0,5765760)");
    }

    SQLite::Database& db() { return store->db(); }

    std::int64_t scalar(const std::string& sql) { return db().execAndGet(sql).getInt64(); }

    SlotRecordingResult capture(bool includeAudio = true, MidiInputFilter filter = {keys, 3}, bool includeMidi = true) {
        SlotInputRecorder recorder;
        AudioRecordingSpec format;
        format.workFolder = dir.path();
        format.sourceProject = db().execAndGet("SELECT file FROM pragma_database_list WHERE name='main'").getString();
        format.sampleRate = 48000;
        format.maxFrames = 128;
        const std::array targets{SlotRecordingTarget{{21, 11}, {0}}, SlotRecordingTarget{{22, 11}, {}, filter}};
        std::string error;
        auto selected = includeAudio ? std::span(targets).subspan(0) : std::span(targets).subspan(1);
        if (!includeMidi) selected = selected.first(1);
        require(recorder.start(format, selected, error), "prime real slot capture");

        std::array<float, 96> pcm{};
        for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<float>(i) / 128;
        const float* channels[]{pcm.data()};
        AudioIo io;
        io.in = channels;
        io.numIn = 1;
        io.frames = static_cast<std::int32_t>(pcm.size());
        io.streamTimeSamples = 1000;
        const std::array events{
            SlotMidiInput{1015, 1015, {15, EventType::NoteOn, 2, 60, 42, 0, 100.0 / 127}, keys},
            SlotMidiInput{1016, 1016, {16, EventType::NoteOn, 2, 64, 42, 0, 90.0 / 127}, pads},
            SlotMidiInput{1021, 1021, {21, EventType::NoteExpression, 2, 0, 42, 0, -1.25}, keys},
            SlotMidiInput{1022, 1022, {22, EventType::NoteExpression, 2, 0, 42, 0, -2.25}, pads},
            SlotMidiInput{1024, 1024, {24, EventType::Controller, 2, 0, 0, 74, 0.625}, keys},
            SlotMidiInput{1025, 1025, {25, EventType::NoteExpression, 2, 0, 0, 0, -1}, keys},
            SlotMidiInput{1026, 1026, {26, EventType::NoteOff, 2, 64, 42, 0, 33.0 / 127}, pads},
            SlotMidiInput{1029, 1029, {29, EventType::Controller, 2, 0, 0, controllers::kPitchBend, .5}, keys},
            SlotMidiInput{1030, 1030, {30, EventType::NoteOff, 2, 60, 42, 0, 29.0 / 127}, keys},
            SlotMidiInput{1031, 1031, {31, EventType::NoteOn, 2, 67, 43, 0, 45.0 / 127}, keys},
        };
        recorder.process(io, SlotRecordingWindow{1013, 1093}, events, 1);
        for (int attempt = 0; attempt < 5000; ++attempt) {
            if (auto result = recorder.poll(true)) return std::move(*result);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("native WAV writer did not finish");
    }
};

const std::array ids{recording::ClipIds{41, 91, "Recorded audio"}, recording::ClipIds{42, 0, "Recorded MIDI"}};
const TempoMap captureClock{{{0, 140.0, 0}, {12000, 150.0, 0}}};

template<class T> std::vector<T> stream(Project& project, const std::string& sql, FourCC type) {
    SQLite::Statement query(project.db(), sql);
    require(query.executeStep(), "stored captured stream exists");
    const auto column = query.getColumn(0);
    const auto data = std::span(static_cast<const std::byte*>(column.getBlob()),
                                static_cast<std::size_t>(column.getBytes()));
    StreamReader<T> reader(data, type);
    require(reader.ok(), "captured stream has native binary layout");
    std::vector<T> records;
    for (std::uint32_t index = 0; index < reader.count(); ++index) records.push_back(*reader.at(index));
    return records;
}

std::int64_t sampleAt(std::int64_t tick) {
    return std::llround(captureClock.ticksToSeconds(tick) * 48000);
}

void mixedCommit() {
    Project project;
    auto take = project.capture();
    const auto privateWav = take.audio->tracks.front().file;
    auto edit = recording::prepareSlotCommit(*project.store, take, ids, captureClock);
    if (!edit.ok) throw std::runtime_error(edit.error);
    require(project.scalar("SELECT count(*) FROM clips") == 2 && project.scalar("SELECT count(*) FROM clip_slots") == 0,
            "preparing a take leaves the original rows and empty slots alone");
    const auto result = recording::commit(*project.store, edit);
    if (!result.ok) throw std::runtime_error(result.error);
    require(project.scalar("SELECT count(*) FROM clips WHERE id IN (31,32) AND pos_ticks=0 AND length_ticks=5765760") == 2,
            "recording into slots leaves overlapping Arrangement clips intact");
    require(project.scalar("SELECT count(*) FROM clip_slots WHERE scene_id=11 AND clip_id IN (41,42)") == 2 &&
            project.scalar("SELECT count(*) FROM clips WHERE id IN (41,42) AND pos_ticks=0 AND lane_id IS NULL AND loop_enabled=1") == 2,
            "audio and MIDI are zero-origin loop clips bound to the frozen slots");
    const auto length = project.scalar("SELECT length_ticks FROM clips WHERE id=42");
    require(sampleAt(length) == 80 && length == project.scalar("SELECT length_ticks FROM clips WHERE id=41"),
            "clip length follows the captured tempo map rather than the project clock");
    const auto notes = stream<NoteRecord>(project, "SELECT data FROM event_streams WHERE clip_id=42 AND stream_kind='notes'", FourCC::Notes);
    require(notes.size() == 2 && sampleAt(notes[0].start_ticks) == 2 && sampleAt(notes[0].start_ticks + notes[0].dur_ticks) == 17 &&
            notes[0].vel_on == 100 && notes[0].vel_off == 29 && notes[0].channel == 2 &&
            sampleAt(notes[1].start_ticks + notes[1].dur_ticks) == 80,
            "the selected endpoint keeps exact notes and excludes matching note IDs from other ports");
    const auto bend = stream<ExpressionPoint>(project, "SELECT data FROM note_expression WHERE clip_id=42 AND dimension=0", FourCC::Expression);
    const auto cc = stream<AutomationPoint>(project, "SELECT data FROM event_streams WHERE clip_id=42 AND stream_kind='cc' AND channel=2 AND controller=74", FourCC::Controller);
    require(bend.size() == 1 && sampleAt(notes[0].start_ticks + bend[0].time_ticks) == 8 && bend[0].value == -1.25f &&
            cc.size() == 1 && sampleAt(cc[0].time) == 11 && cc[0].value == 0.625, "binary expression and controller values survive");
    const auto pitch = stream<AutomationPoint>(project,
        "SELECT data FROM event_streams WHERE clip_id=42 AND stream_kind='pitchbend' AND channel=2 AND controller IS NULL",
        FourCC::Controller);
    require(pitch.size() == 2 && sampleAt(pitch[0].time) == 12 && pitch[0].value == .25 &&
            sampleAt(pitch[1].time) == 16 && pitch[1].value == .5,
            "legacy wheel input and typed neutral persist on one channel pitch lane at their exact samples");
    const auto noteBytes = project.db().execAndGet("SELECT hex(data) FROM event_streams WHERE clip_id=42 AND stream_kind='notes'").getString();

    const auto mediaRow = media::mediaRow(project.db(), 91);
    std::string locateError;
    const auto asset = media::resolveMedia(media::projectFolder(*project.store), mediaRow, locateError);
    require(!asset.empty() && !std::filesystem::exists(privateWav), "final WAV replaces the private take after commit");
    std::array<float, 80> audio{};
    {
        adi::audio::WavReader reader(asset);
        require(reader.read(audio.data(), 80) == 80, "published audio has 80 frames");
    }
    for (std::size_t i = 0; i < audio.size(); ++i) require(audio[i] == static_cast<float>(i + 13) / 128, "published PCM is exact");

    require(History(*project.store).undo().ok && project.scalar("SELECT count(*) FROM clips") == 2 &&
            project.scalar("SELECT count(*) FROM clip_slots") == 0 && project.scalar("SELECT count(*) FROM media_files") == 0,
            "one Undo removes both captured slots and their media rows");
    require(History(*project.store).redo().ok && project.scalar("SELECT count(*) FROM clip_slots") == 2,
            "one Redo restores both captured slots");
    project.store.reset();
    StoreError error{};
    project.store = Store::open(project.path, error, true);
    require(project.store && project.scalar("SELECT count(*) FROM clip_slots") == 2 &&
            project.scalar("SELECT count(*) FROM event_streams WHERE clip_id=42") == 3 &&
            project.db().execAndGet("SELECT hex(data) FROM event_streams WHERE clip_id=42 AND stream_kind='notes'").getString() == noteBytes,
            "native audio and MIDI slot rows reopen from the database");
}

void staleStageRefresh() {
    Project project;
    project.db().exec("DELETE FROM clips WHERE id=31");
    auto take = project.capture(true, {}, false);
    const auto audioIds = std::span(ids).first(1);
    require(take.audio && take.targets.size() == 1, "capture one audio-only Session slot");
    const auto privateWav = take.audio->tracks.front().file;
    const auto folder = media::projectFolder(*project.store);
    const auto staged = recording::stageTake(*take.audio, folder);
    auto edit = recording::prepareSlotCommit(*project.store, take, audioIds, captureClock, &staged);
    require(staged.ok && edit.ok && recording::publishStagedTake(edit), "publish a verified audio-only slot take");
    const auto publicWav = folder / media::pathFromUtf8(
        edit.requests.front().payload.at("row").at("rel_path").get<std::string>());
    require(recording::refreshStagedCommit(*project.store, staged, edit, audioIds) == recording::StagedRefresh::ready,
            "a current staged slot take keeps its prepared placement");

    OpRequest rename;
    rename.opType = "track.rename";
    rename.payload = {{"id", 21}, {"name", "Renamed during slot publication"}};
    require(OpJournal(*project.store).commit(rename).ok, "move the journal head while the slot WAV is published");
    require(recording::refreshStagedCommit(*project.store, staged, edit, audioIds) == recording::StagedRefresh::refused &&
            !edit.ok && edit.error.find("Session slot recording needs fresh preparation") != std::string::npos,
            "a stale slot take refuses the Arrangement refresh route");
    require(!recording::commit(*project.store, edit).ok && project.scalar("SELECT count(*) FROM clips") == 1 &&
            project.scalar("SELECT count(*) FROM clip_slots") == 0 && project.scalar("SELECT count(*) FROM media_files") == 0 &&
            std::filesystem::is_regular_file(privateWav), "refresh preserves the private take without writing any placement");
    if (std::filesystem::exists(publicWav)) {
        const auto hash = media::blake3File(privateWav);
        require(hash && media::blake3File(publicWav).hex == hash.hex &&
                edit.error.find(media::pathUtf8(publicWav)) != std::string::npos,
                "refresh rollback reports the verified public copy when it is retained");
    }
    const auto restaged = recording::stageTake(*take.audio, folder);
    auto retry = recording::prepareSlotCommit(*project.store, take, audioIds, captureClock, &restaged);
    require(restaged.ok && retry.ok && recording::publishStagedTake(retry) && recording::commit(*project.store, retry).ok &&
            project.scalar("SELECT clip_id FROM clip_slots WHERE track_id=21 AND scene_id=11") == 41 &&
            project.scalar("SELECT count(*) FROM clips") == 2,
            "fresh Session preparation attaches the recovered WAV to the original slot");
    require(recording::cleanupStagedTake(retry).empty(), "remove finalized private staging after refresh refusal");
}

void staleStage() {
    staleStageRefresh();
    Project project;
    auto take = project.capture();
    const auto privateWav = take.audio->tracks.front().file;
    const auto staged = recording::stageTake(*take.audio, media::projectFolder(*project.store));
    require(staged.ok, "stage a captured audio and MIDI take off the callback");
    auto edit = recording::prepareSlotCommit(*project.store, take, ids, captureClock, &staged);
    require(edit.ok && recording::publishStagedTake(edit), "publish the verified media before final admission");
    const auto publicWav = media::projectFolder(*project.store) / media::pathFromUtf8(
        edit.requests.front().payload.at("row").at("rel_path").get<std::string>());
    OpRequest competing;
    competing.opType = "session.setSlotClip";
    competing.payload = {{"track", 22}, {"scene", 11}, {"clip", 32}};
    require(OpJournal(*project.store).commit(competing).ok, "another edit fills the MIDI slot");
    const auto stale = recording::commit(*project.store, edit);
    require(!stale.ok && stale.stale && project.scalar("SELECT clip_id FROM clip_slots WHERE track_id=22") == 32 &&
            project.scalar("SELECT count(*) FROM media_files") == 0 && std::filesystem::is_regular_file(privateWav),
            "stale commit keeps the new occupant, rolls back media and keeps the recoverable take");
#if defined(_WIN32)
    require(!std::filesystem::exists(publicWav), "stale commit removes the verified public WAV");
#else
    const auto hash = media::blake3File(privateWav);
    require(hash && media::blake3File(publicWav).hex == hash.hex &&
            stale.error.find(media::pathUtf8(publicWav)) != std::string::npos,
            "POSIX rollback reports the retained verified public copy");
#endif
    auto retry = recording::prepareSlotCommit(*project.store, take, ids, captureClock, &staged);
    require(!retry.ok && std::filesystem::is_regular_file(privateWav), "an occupied slot refuses a fresh attachment");
    require(History(*project.store).undo().ok, "clear the competing edit exactly");
    // The first publication moved its staged file. Re-stage from the
    // retained private take after rollback removes the public copy.
    const auto restaged = recording::stageTake(*take.audio, media::projectFolder(*project.store));
    require(restaged.ok, "recover the verified original WAV after a stale commit");
    retry = recording::prepareSlotCommit(*project.store, take, ids, captureClock, &restaged);
    require(retry.ok && recording::publishStagedTake(retry) && recording::commit(*project.store, retry).ok,
            "the same private WAV can be published and attached after the conflict is cleared");
    require(recording::cleanupStagedTake(retry).empty(), "remove finalized private staging after retry");
}

void midiOnly() {
    Project project;
    auto take = project.capture(false, {});
    const auto midiIds = std::span(ids).subspan(1);
    auto bad = take;
    bad.sourceProject += ".other";
    require(!recording::prepareSlotCommit(*project.store, bad, midiIds, captureClock).ok,
            "MIDI-only capture cannot attach to a different project");
    require(!recording::prepareSlotCommit(*project.store, take, midiIds, {}).ok,
            "MIDI-only capture requires the performed clock");
    auto edit = recording::prepareSlotCommit(*project.store, take, midiIds, captureClock);
    require(edit.ok && recording::commit(*project.store, edit).ok &&
            project.scalar("SELECT count(*) FROM clip_slots WHERE clip_id=42") == 1 &&
            project.scalar("SELECT count(*) FROM media_files") == 0,
            "attach native MIDI without an audio file or media row");
    const auto notes = stream<NoteRecord>(project, "SELECT data FROM event_streams WHERE clip_id=42 AND stream_kind='notes'", FourCC::Notes);
    require(notes.size() == 3 && notes[0].key == 60 && notes[1].key == 64 && notes[0].note_id != notes[1].note_id &&
            sampleAt(notes[0].start_ticks + notes[0].dur_ticks) == 17 && notes[0].vel_off == 29 &&
            sampleAt(notes[1].start_ticks + notes[1].dur_ticks) == 13 && notes[1].vel_off == 33,
            "identical raw note IDs from multiple accepted ports close their own notes");
    const auto expression = stream<ExpressionPoint>(project,
        "SELECT data FROM note_expression WHERE clip_id=42 AND note_id=" + std::to_string(notes[1].note_id), FourCC::Expression);
    require(expression.size() == 1 && expression[0].value == -2.25f &&
            sampleAt(notes[1].start_ticks + expression[0].time_ticks) == 9,
            "per-note expression follows the note owned by its input port");
}

void routedMidiSlots() {
    Project project;
    project.db().exec("INSERT INTO tracks(id,kind,name) VALUES(23,'midi','Pads'),(24,'midi','Foot controller')");
    constexpr auto keyboardInput = midiInputKey("keyboard");
    constexpr auto padInput = midiInputKey("pad-device");
    constexpr auto otherInput = midiInputKey("another-device");
    std::array targets{SlotRecordingTarget{{22, 11}, {}, {keyboardInput, 3}},
        SlotRecordingTarget{{23, 11}, {}, {padInput, 3}}, SlotRecordingTarget{{24, 11}, {}, {padInput, 5}}};
    AudioRecordingSpec format;
    format.sourceProject = project.db().execAndGet("SELECT file FROM pragma_database_list WHERE name='main'").getString();
    format.sampleRate = 48000;
    format.maxFrames = 64;
    SlotInputRecorder recorder;
    std::string error;
    require(recorder.start(format, targets, error), "arm three MIDI slots with independent port/channel input");
    targets.front().midiInput = {}; // control changes after arm do not broaden the frozen filter

    AudioIo io;
    io.frames = 64;
    io.streamTimeSamples = 100;
    const auto note = [&](std::int32_t frame, EventType type, std::uint8_t channel, std::uint16_t key,
                          std::uint64_t id, std::uint64_t port) {
        return SlotMidiInput{io.streamTimeSamples + frame, io.streamTimeSamples + frame,
            {frame, type, channel, key, id, 0, 67.0 / 127}, port};
    };
    const std::array events{
        note(5, EventType::NoteOn, 2, 60, 51, keyboardInput),
        note(6, EventType::NoteOn, 2, 64, 52, padInput),
        note(7, EventType::NoteOn, 4, 68, 53, padInput),
        note(8, EventType::NoteOn, 4, 72, 54, keyboardInput),
        note(9, EventType::NoteOn, 2, 76, 55, otherInput),
        SlotMidiInput{110, 110, {10, EventType::NoteExpression, 2, 0, 52, 0, 1.25}, padInput},
        SlotMidiInput{111, 111, {11, EventType::Controller, 4, 0, 0, 74, 0.75}, padInput},
        SlotMidiInput{112, 112, {12, EventType::Controller, 2, 0, 0, controllers::kPitchBend, 0.625}, padInput},
        SlotMidiInput{113, 113, {13, EventType::NoteExpression, 2, 0, 0, 0, 1.0}, padInput},
        note(15, EventType::NoteOff, 2, 60, 51, keyboardInput),
        note(16, EventType::NoteOff, 2, 64, 52, padInput),
        note(17, EventType::NoteOff, 4, 68, 53, padInput)};
    recorder.process(io, SlotRecordingWindow{103, 142}, events, 1);
    const auto take = recorder.poll(true);
    require(take && take->state == SlotRecordingState::Complete && take->frames == 39 &&
            take->targets.front().midiInput == MidiInputFilter{keyboardInput, 3} &&
            std::count_if(take->midi.begin(), take->midi.end(), [&](const auto& packet) {
                return packet.kind == MidiCapturePacket::Kind::Event && packet.port == padInput;
            }) == 8, "the take freezes input filters and records every physical packet port");
    constexpr std::array<std::int64_t, 3> clips{61, 62, 63};
    const std::array clipIds{recording::ClipIds{clips[0], 0, "Keyboard"},
        recording::ClipIds{clips[1], 0, "Pads"}, recording::ClipIds{clips[2], 0, "Foot controller"}};
    auto edit = recording::prepareSlotCommit(*project.store, *take, clipIds, captureClock);
    if (!edit.ok) throw std::runtime_error(edit.error);
    const auto committed = recording::commit(*project.store, edit);
    if (!committed.ok) throw std::runtime_error(committed.error);
    constexpr std::array<std::uint8_t, 3> keysBySlot{60, 64, 68};
    for (std::size_t index = 0; index < clips.size(); ++index) {
        const auto notes = stream<NoteRecord>(project, "SELECT data FROM event_streams WHERE stream_kind='notes' AND clip_id=" +
            std::to_string(clips[index]), FourCC::Notes);
        require(notes.size() == 1 && notes.front().key == keysBySlot[index] &&
                sampleAt(notes.front().start_ticks) == 2 + static_cast<std::int64_t>(index) &&
                sampleAt(notes.front().start_ticks + notes.front().dur_ticks) == 12 + static_cast<std::int64_t>(index),
                "each slot receives only its own input, at the original quantized capture offset");
    }
    require(project.scalar("SELECT count(*) FROM note_expression WHERE clip_id=62") == 1 &&
            project.scalar("SELECT count(*) FROM note_expression") == 1 &&
            project.scalar("SELECT count(*) FROM event_streams WHERE clip_id=63 AND stream_kind='cc' AND channel=4 AND controller=74") == 1 &&
            project.scalar("SELECT count(*) FROM event_streams WHERE stream_kind='cc'") == 1,
            "expression and controller ownership follow the same frozen MIDI input");
    const auto wheel = stream<AutomationPoint>(project,
        "SELECT data FROM event_streams WHERE clip_id=62 AND stream_kind='pitchbend' AND channel=2 AND controller IS NULL",
        FourCC::Controller);
    require(project.scalar("SELECT count(*) FROM event_streams WHERE stream_kind='pitchbend'") == 1 &&
            wheel.size() == 2 && sampleAt(wheel[0].time) == 9 && wheel[0].value == 0.625 &&
            sampleAt(wheel[1].time) == 10 && wheel[1].value == 0.75,
            "native and physical pitch-wheel packets keep their normalized values in the routed channel's lane");
    require(History(*project.store).undo().ok &&
            project.scalar("SELECT count(*) FROM clip_slots") == 0 &&
            project.scalar("SELECT count(*) FROM event_streams WHERE stream_kind='pitchbend'") == 0 &&
            project.scalar("SELECT count(*) FROM clips WHERE id IN (31,32)") == 2 &&
            History(*project.store).redo().ok &&
            project.scalar("SELECT count(*) FROM clip_slots WHERE scene_id=11 AND clip_id IN (61,62,63)") == 3 &&
            project.scalar("SELECT count(*) FROM event_streams WHERE clip_id=62 AND stream_kind='pitchbend'") == 1,
            "one Undo and Redo cover all routed slots and leave Arrangement alone");
}

} // namespace

int main() {
    try {
        mixedCommit();
        staleStage();
        midiOnly();
        routedMidiSlots();
        std::printf("Session slot commit preserves 80 PCM frames, exact MIDI and one Undo.\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
