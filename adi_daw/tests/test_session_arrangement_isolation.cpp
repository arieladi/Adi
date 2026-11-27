// SPDX-License-Identifier: AGPL-3.0-or-later
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/wav_file.hpp"
#include "adi/blob.hpp"
#include "adi/clip_commands.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/engine/timeline_clips.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/render/freeze_capture.hpp"
#include "adi/render/queue.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace {
using namespace adi;
using namespace adi::engine;
namespace fs = std::filesystem;

constexpr std::uint32_t frames = 512, rate = 48'000;
constexpr std::int32_t block = 127;

void require(bool success, const std::string& what) {
    if (!success) throw std::runtime_error(what);
}

bool equalPcm(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](float x, float y) {
        return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
    });
}

std::vector<float> readWav(const fs::path& path) {
    audio::WavReader reader(path);
    require(reader.sampleRate() == rate && reader.channels() == 2 && reader.frames() == frames &&
        reader.format() == audio::WavFormat::Float32, "expected 512 frames of stereo Float32 PCM");
    std::vector<float> pcm(frames * 2);
    require(reader.read(pcm.data(), frames) == frames, "read full Arrangement WAV");
    return pcm;
}

struct Fixture {
    test::TempDirectory temporary{"session_arrangement_isolation", "project"};
    std::unique_ptr<Store> owner;

    Fixture() {
        StoreError error{};
        owner = Store::createEmpty(temporary.path() / "song.adi", error);
        require(static_cast<bool>(owner), "create Arrangement project");
        writeMedia(1, "arrangement.wav", 1.0f);
        owner->db().exec(
            "INSERT INTO project(id,name,sample_rate) VALUES(1,'Session Arrangement Proof',48000);"
            "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES "
            "(1,'audio','Arrangement',0),(2,'midi','MIDI',1),(9,'master','Main',2);"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,500000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,512);");
        fs::create_directory(temporary.path() / "output");
    }

    void addSlots() {
        test::applySessionSchema(owner->db());
        writeMedia(2, "session.wav", 2.0f);
        owner->db().exec(
            "INSERT INTO scenes(id,ord,name) VALUES(10,0,'First'),(20,1,'Second');"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES"
            "(3,'missing-slot','missing-session.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES"
            "(101,1,'audio',1,0,500000000),(102,1,'audio',1,0,500000000);"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(103,2,'midi',0,5765760);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES"
            "(101,2,0,512),(102,3,0,512);"
            "INSERT INTO clip_slots(track_id,scene_id,clip_id,has_stop) VALUES"
            "(1,10,101,1),(1,20,102,1),(2,10,103,1),(2,20,NULL,0);"
            "INSERT INTO clip_launch(clip_id) VALUES(101),(102),(103);");
        NoteRecord note{};
        note.dur_ticks = 120000;
        note.note_id = 1;
        note.key = 60;
        note.vel_on = 100;
        note.probability = 10000;
        const auto blob = writeStream<NoteRecord>(FourCC::Notes, std::array{note});
        require(owner->putEventStream(103, "notes", blob), "store the MIDI slot's authored note stream");
    }

    void writeMedia(std::int64_t id, const char* name, float multiplier) {
        std::vector<float> signal(frames * 2);
        for (std::uint32_t i = 0; i < frames; ++i) {
            signal[2 * i] = multiplier * static_cast<float>(i % 31 + 1) / 128.0f;
            signal[2 * i + 1] = -signal[2 * i];
        }
        const auto path = temporary.path() / name;
        audio::WavWriter wav(path, rate, 2, audio::WavFormat::Float32);
        wav.write(signal.data(), frames);
        wav.close();
        const auto hash = media::blake3File(path);
        require(static_cast<bool>(hash), "hash fixture media");
        SQLite::Statement row(owner->db(),
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(?,?,?,'wav')");
        row.bind(1, id);
        row.bind(2, hash.hex);
        row.bind(3, name);
        row.exec();
    }

    std::unique_ptr<Store> reader() {
        StoreError error{};
        auto store = Store::open(owner->path(), error, true);
        require(static_cast<bool>(store), "open Arrangement project read-only");
        return store;
    }
};

std::vector<float> play(Store& store) {
    Session session;
    session.graph().setFadeFrames(0);
    SessionSpec spec{2, rate, block};
    spec.synchronousClipReads = true;
    if (!session.load(store, {}, spec)) throw std::runtime_error(session.error());
    session.transport().play();
    std::vector<float> result(frames * 2);
    std::array<float, block> left{}, right{};
    float* buffers[]{left.data(), right.data()};
    for (std::uint32_t first = 0; first < frames;) {
        require(session.clips()->prime(), "prime direct Arrangement render");
        const auto count = std::min<std::uint32_t>(block, frames - first);
        session.process({buffers, nullptr, 2, 0, static_cast<std::int32_t>(count), first});
        for (std::uint32_t frame = 0; frame < count; ++frame) {
            result[2 * (first + frame)] = left[frame];
            result[2 * (first + frame) + 1] = right[frame];
        }
        first += count;
    }
    require(session.midiClips()->monitorRanges(2).empty(), "MIDI slot has no Arrangement playback span");
    return result;
}

struct Capture {
    std::vector<float> dry, wet;
    std::string fingerprint;
};

Capture captureFreeze(Store& store) {
    render::FreezeCaptureSpec spec;
    spec.tracks = {1};
    spec.endSample = frames;
    spec.session = {2, rate, block};
    const auto capture = render::captureFreezeAudio(store, spec);
    require(capture.ready() && capture.streams().size() == 1, "freeze needs only Arrangement media: " + capture.error());
    require(!fs::exists(capture.directory() / "asset-2.wav"), "freeze does not pin slot sources");
    const auto& stream = capture.streams().front();
    return {readWav(stream.preFx), readWav(stream.postFx), stream.fingerprint};
}

std::vector<float> deliver(const Fixture& fixture, const char* name) {
    render::Queue queue;
    render::Job job;
    job.project = fixture.owner->path();
    job.directory = fixture.temporary.path() / "output";
    job.exactWavFilename = name;
    job.endSample = frames;
    job.session = {2, rate, block};
    job.encoding.format = audio::WavFormat::Float32;
    queue.add(job);
    queue.run();
    const auto result = queue.snapshot().front().result;
    require(result.state == render::JobState::Completed, "export needs only Arrangement media: " + result.error);
    require(result.mediaVerification && result.mediaVerification->identities.size() == 1,
        "export hashes only the Arrangement source");
    return readWav(result.output);
}

void checkRows(Store& store) {
    const auto model = rows::readModel(store);
    require(model.problems.empty() && model.clips.size() == 1 && model.slotClips.size() == 3 &&
        model.notes.empty() && model.slotNotes.size() == 1 && model.slotNotes.front().clipId == 103,
        "typed rows retain slot clips and their notes separately");
    const auto playing = timelineClips(model);
    require(playing.begin() != playing.end() && playing.begin()->id == 1 && std::distance(playing.begin(), playing.end()) == 1,
        "Arrangement accessor selects only the ordinary material");
    const auto snapshot = SnapshotBuilder::fromStore(store);
    const auto track = std::ranges::find_if(snapshot->tracks, [](const auto& row) { return row->id == 1; });
    require(track != snapshot->tracks.end() && (*track)->clips.size() == 1 && (*track)->clips.front()->id == 1,
        "Arrangement snapshot cannot place a slotted audio clip at zero");
    const auto range = clip_edit::copyRange(store, {{1}, {1, 0}, {1, 5000000}}, rate);
    require(range.ok && range.images.size() == 1, "Arrangement range does not copy slots: " + range.error);
}

void tempoChangesOnlyArrangement(Store& store) {
    constexpr auto quarter = textproj::kPPQ;
    SQLite::Statement musical(store.db(),
        "UPDATE clips SET time_base=0,pos_ticks=0,length_ticks=?,pos_ns=NULL,length_ns=NULL WHERE id IN (1,101)");
    musical.bind(1, quarter);
    musical.exec();
    const auto arrangementLength = [&] {
        return store.db().execAndGet("SELECT length_ticks FROM clips WHERE id=1").getInt64();
    };
    const auto slotLength = [&] {
        return store.db().execAndGet("SELECT length_ticks FROM clips WHERE id=101").getInt64();
    };
    OpRequest tempo;
    tempo.opType = "project.insertTempoEvent";
    tempo.payload = {{"pos", quarter / 2}, {"bpm", 180.0}};
    const auto add = OpJournal(store).commit(tempo);
    require(add.ok, "insert a tempo change across both audio clips: " + add.error);
    require(arrangementLength() == 5 * quarter / 4 && slotLength() == quarter,
            "a tempo change preserves unwarped Arrangement duration and leaves the authored slot length");
    require(History(store).undo().ok && arrangementLength() == quarter && slotLength() == quarter &&
            History(store).redo().ok && arrangementLength() == 5 * quarter / 4 && slotLength() == quarter,
            "tempo Undo and Redo preserve the same Arrangement-only ownership");
}
} // namespace

int main() {
    try {
        Fixture fixture;
        auto old = fixture.reader();
        const auto baseline = play(*old);
        const auto baselineFreeze = captureFreeze(*old);
        const auto baselineExport = deliver(fixture, "baseline.wav");
        old.reset();

        fixture.addSlots();
        auto session = fixture.reader();
        checkRows(*session);
        const auto sessionPcm = play(*session);
        const auto sessionFreeze = captureFreeze(*session);
        const auto sessionExport = deliver(fixture, "with-slots.wav");
        require(equalPcm(baseline, sessionPcm) && equalPcm(baselineFreeze.dry, sessionFreeze.dry) &&
                equalPcm(baselineFreeze.wet, sessionFreeze.wet) && equalPcm(baselineExport, sessionExport),
                "512 stereo samples are bit-identical in player, dry/wet freeze and export");
        require(baselineFreeze.fingerprint == sessionFreeze.fingerprint, "slots leave freeze's causal fingerprint unchanged");
        tempoChangesOnlyArrangement(*fixture.owner);
        fixture.owner->db().exec("DELETE FROM clip_slots WHERE track_id=1 AND scene_id=10");
        require(!equalPcm(baseline, play(*session)), "the same PCM leaks into Arrangement when it loses slot ownership");
        std::printf("PASS -- %u stereo frames bit-identical through player, both freeze taps and export\n", frames);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "FAIL -- %s\n", failure.what());
        return 1;
    }
}
