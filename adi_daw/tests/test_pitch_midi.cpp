// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_midi.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/digest.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/history.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {
std::atomic<unsigned> callbackAllocations{0};
}
void adi::test::countReplacedNew() noexcept {
    if (adi::audio::onAudioThread) ++callbackAllocations;
}

namespace {
using namespace adi;
using namespace adi::clip_edit;
constexpr auto quarter = textproj::kPPQ;
int checks = 0, failures = 0;

void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}

struct Fixture {
    test::TempDirectory temp{"pitch_midi", "project"};
    std::unique_ptr<Store> store;
    media::ClipPitchAnalysis analysis{
        11, {std::string(64, 'a'), 48000, 1, 48000}, 4800, 43200, false, 1, 0,
        {48000, 48000,
         {{0, 3200, 5500, 0, 0, {{1600, 5500}}},
          {4800, 14400, 5727.5, 0, 0, {{9600, 5727.5}}},
          {24000, 33600, 6900, 0, 0, {{28800, 6900}}},
          {37600, 47000, 7200, 0, 0, {{40000, 7200}}}}}};

    Fixture() {
        StoreError error{};
        store = Store::createEmpty(temp.path() / "a.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        store->db().exec(
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
            "(1,'audio','Source',0),(2,'audio','Next',1);"
            "INSERT INTO media_files(id,hash_blake3,orig_name,sample_rate,channels,frames) VALUES"
            "(1,'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','source.wav',48000,1,48000);"
            "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) VALUES"
            "(11,1,'audio','Singer',5765760,17297280);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(11,1,4800,38400)");
    }

    void warp() {
        constexpr std::array points{WarpMarker{0,0}, WarpMarker{19200,quarter}, WarpMarker{38400,3 * quarter}};
        const auto markers = audio::writeWarpMap(points, 38400);
        if (!markers) throw std::runtime_error("fixture Warp map");
        SQLite::Statement set(store->db(), "UPDATE audio_clips SET warp_enabled=1,"
                                           "warp_mode='beats',warp_markers=? WHERE clip_id=11");
        set.bind(1, markers.bytes.data(), static_cast<int>(markers.bytes.size()));
        set.exec();
    }
};

void projection() {
    Fixture f;
    f.warp();
    const auto take = projectPitchMidi(*f.store, f.analysis, {});
    check(take && take.data->positionTicks == quarter && take.data->lengthTicks == 3 * quarter &&
          take.data->notes.size() == 3, "Warp projection clips source bounds");
    if (!take || take.data->notes.size() != 3) return;
    const auto& notes = take.data->notes;
    check(notes[0].start_ticks == 0 && notes[0].dur_ticks == quarter / 2 &&
          notes[1].start_ticks == quarter && notes[1].dur_ticks == quarter &&
          notes[2].start_ticks + notes[2].dur_ticks == 3 * quarter,
          "nonlinear Warp uses the source marker segments and clips the last note");

    audio::PitchEditState state;
    const std::array commands{audio::PitchEditCommand{1, audio::PitchSetShift{100}},
                              audio::PitchEditCommand{1, audio::PitchSetTiming{7200,16800}},
                              audio::PitchEditCommand{2, audio::PitchSetGain{-std::numeric_limits<double>::infinity()}}};
    check(audio::applyPitchEdits(f.analysis.analysis, state, commands) == audio::PitchEditError::None,
          "fixture edits are valid");
    const std::array<std::size_t, 2> selected{1,2};
    PitchMidiOptions options;
    options.selectedNotes = selected;
    options.velocity = 91;
    options.keepTuning = true;
    const auto edited = projectPitchMidi(*f.store, f.analysis, state, options);
    check(edited && edited.data->notes.size() == 1, "selection and authored mute omit other notes");
    if (edited && edited.data->notes.size() == 1) {
        const auto& note = edited.data->notes.front();
        check(note.key == 58 && note.tuning_cents == 27.5f && note.vel_on == 91 &&
              note.start_ticks == quarter / 8 && note.dur_ticks == quarter / 2,
              "edited pitch and timing project exactly through Warp");
    }
    const std::array<std::size_t, 2> duplicate{1,1};
    options.selectedNotes = duplicate;
    check(!projectPitchMidi(*f.store, f.analysis, state, options), "duplicate selection fails closed");

    f.store->db().exec("UPDATE audio_clips SET reverse=1 WHERE clip_id=11");
    check(!projectPitchMidi(*f.store, f.analysis, {}), "a stale analysis direction is rejected");
    f.analysis.reversed = true;
    const auto reverse = projectPitchMidi(*f.store, f.analysis, {});
    check(reverse && reverse.data->notes.size() == 3 && reverse.data->notes[0].key == 72 &&
          reverse.data->notes[1].key == 69 && reverse.data->notes[2].key == 57 &&
          reverse.data->notes[2].start_ticks == 2 * quarter,
          "reverse mirrors source intervals before Warp");
    f.store->db().exec("UPDATE audio_clips SET reverse=0 WHERE clip_id=11;"
        "UPDATE clips SET length_ticks=23063040,loop_enabled=1,"
        "loop_start_ticks=5765760,loop_len_ticks=5765760 WHERE id=11");
    f.analysis.reversed = false;
    const auto repeated = projectPitchMidi(*f.store, f.analysis, {});
    check(repeated && repeated.data->notes.size() == 4 &&
          repeated.data->notes[1].start_ticks == quarter && repeated.data->notes[2].start_ticks == 2 * quarter &&
          repeated.data->notes[3].start_ticks == 3 * quarter && repeated.data->notes[3].key == 69,
          "loop projection keeps the lead-in and repeats only the loop range");
}

void journal() {
    Fixture f;
    f.store->db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120),(2,11531520,60)");
    PitchMidiOptions options;
    options.content = PitchMidiContent::DetectedMelody;
    const auto before = digestProject(*f.store).text;
    const auto prepared = preparePitchMidi(*f.store, f.analysis, options);
    check(prepared && digestProject(*f.store).text == before, "prepare Melody reads the snapshot without writing rows");
    if (!prepared) {
        std::printf("Prepare error: %s\n", prepared.detail ? prepared.detail->c_str() : "(none)");
        return;
    }
    const auto result = commitPitchMidi(*f.store, *prepared.data);
    check(result.ok && result.seqs.size() == 3, "one journal transaction writes sibling shift, track and MIDI clip");
    if (!result.ok) {
        std::printf("Commit error: %s\n", result.error.c_str());
        return;
    }
    const auto clip = engine::readClipDetails(*f.store, prepared.data->midiClip);
    check(clip.clip && clip.clip->notes.size() == 3 &&
          clip.clip->notes[0].key == 57 && clip.clip->notes[0].tuningCents == 0 &&
          clip.clip->notes[1].key == 69 && clip.clip->notes[1].startTicks == 4 * quarter / 5 &&
          clip.clip->notes[1].startTicks + clip.clip->notes[1].durationTicks == 11 * quarter / 10,
          "Melody emits exact semitone notes across a project tempo step");
    check(f.store->db().execAndGet("SELECT count(*) FROM event_streams").getInt() == 1,
          "each extracted clip has one native notes stream");
    const auto after = digestProject(*f.store).text;
    check(History(*f.store).undo().ok && digestProject(*f.store).text == before &&
          f.store->db().execAndGet("SELECT count(*) FROM event_streams").getInt() == 0,
          "one undo removes the track, clip and stream and restores sibling placement");
    check(History(*f.store).redo().ok && digestProject(*f.store).text == after,
          "redo restores the exact MIDI bytes without media or analysis");
    check(commitPitchMidi(*f.store, *prepared.data).stale && digestProject(*f.store).text == after,
          "a changed journal head rejects a captured extraction atomically");
}

void expressionAndDestinations() {
    Fixture f;
    f.warp();
    auto& db = f.store->db();
    db.exec("UPDATE tracks SET kind='midi' WHERE id=2;"
            "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) "
            "VALUES(12,2,'midi','Previous',5765760,17297280)");
    const auto before = digestProject(*f.store).text;
    auto& note = f.analysis.analysis.notes[1];
    note.curve = {{4800,5700},{9600,5800},{14399,5700}};
    std::vector<float> pcm(static_cast<std::size_t>(f.analysis.source.frames), 0.5f);
    std::fill_n(pcm.begin() + 4800, 9600, 0.25f);
    PitchMidiOptions options;
    const std::array<std::size_t, 2> selected{1,2};
    options.selectedNotes = selected;
    options.keepTuning = true;
    options.includePitchCurve = true;
    options.velocitySource = pcm;
    options.destinationTrack = 2;
    check(!preparePitchMidi(*f.store, f.analysis, options) && digestProject(*f.store).text == before,
          "selected destination preserves overlapping unrelated MIDI");
    options.previousExtractionClip = 12;
    const auto prepared = preparePitchMidi(*f.store, f.analysis, options);
    check(prepared && prepared.data->track == 2 && digestProject(*f.store).text == before,
          "selected MIDI destination is prepared without writes");
    if (!prepared) {
        std::printf("Prepare destination error: %s\n", prepared.detail ? prepared.detail->c_str() : "(none)");
        return;
    }
    const auto commit = commitPitchMidi(*f.store, *prepared.data);
    check(commit.ok && db.execAndGet("SELECT count(*) FROM tracks").getInt() == 2,
          "placement on an existing track creates no tracks");
    if (!commit.ok) {
        std::printf("MIDI placement error: %s\n", commit.error.c_str());
        return;
    }
    const auto read = engine::readClipDetails(*f.store, prepared.data->midiClip, true, true);
    check(read.clip && read.clip->notes.size() == 2 && read.clip->notes[0].velocity == 35 &&
          read.clip->notes[1].velocity == 71 && read.clip->notes[0].tuningCents == 27.5f,
          "dynamic velocity uses source RMS and static tuning retains fractional pitch");
    if (!read.clip) return;
    const auto& curves = read.clip->noteExpressions.curves;
    check(curves.size() == 2 && curves[0].points.size() > 2 &&
          curves[0].points.front().time_ticks == 0 &&
          std::abs(curves[0].points.front().value + 0.275f) < 1e-6f &&
          curves[0].points[1].time_ticks == quarter / 4 &&
          std::abs(curves[0].points[1].value - 0.725f) < 1e-6f &&
          curves[0].points.back().time_ticks == quarter / 2,
          "continuous pitch is native note-relative AEXP with exact Warp placement");
    check(db.execAndGet("SELECT count(*) FROM clips WHERE id=12").getInt() == 0,
          "explicit previous extraction is replaced on the selected destination");
    const auto after = digestProject(*f.store).text;
    check(History(*f.store).undo().ok && digestProject(*f.store).text == before &&
          History(*f.store).redo().ok && digestProject(*f.store).text == after,
          "one undo restores destination and redo restores both native streams");

    options.destinationTrack.reset();
    options.previousExtractionClip.reset();
    const auto copied = copyPitchMidi(*f.store, f.analysis, options);
    check(copied && digestProject(*f.store).text == after, "copy produces a detached MIDI clip image without writes");
    if (!copied) return;
    const auto pasted = preparePaste(*f.store, *copied.data, 50, 2, {0, 5 * quarter}, 48000);
    const auto paste = commitPrepared(*f.store, pasted);
    const auto relocated = engine::readClipDetails(*f.store, 50, true, true);
    check(paste.ok && relocated.clip && relocated.clip->posTicks == 5 * quarter &&
          relocated.clip->noteExpressions.curves.size() == 2,
          "native clipboard paste relocates MIDI and keeps per-note pitch");
    if (!paste.ok) std::printf("MIDI clipboard paste error: %s\n", paste.error.c_str());
}

void analyzedMelody() {
    constexpr std::uint32_t rate = 48000;
    test::TempDirectory temp{"pitch_midi", "audio"};
    const auto wave = temp.path() / "voice.wav";
    std::vector<float> samples(rate);
    constexpr std::array<double,3> frequencies{261.625565, 293.664768, 329.627557};
    for (std::size_t note = 0; note < frequencies.size(); ++note) {
        for (std::size_t frame = 0; frame < 12000; ++frame) {
            const auto angle = 2 * std::numbers::pi * frequencies[note] * static_cast<double>(frame) / rate;
            const auto gain = std::clamp(static_cast<double>(std::min(frame, 11999 - frame)) / 360, 0.0, 1.0);
            samples[note * 15000 + frame] = static_cast<float>(gain * (std::sin(angle) * 0.6 + std::sin(angle * 2) * 0.2));
        }
    }
    {
        audio::WavWriter writer(wave, rate, 1, audio::WavFormat::Float32);
        writer.write(samples.data(), rate);
        writer.close();
    }
    StoreError error{};
    auto store = Store::createEmpty(temp.path() / "a.adi", error);
    if (!store) throw std::runtime_error(toString(error));
    auto& db = store->db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Source');"
            "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) "
            "VALUES(11,1,'audio','Voice',0,11531520)");
    if (!media::importMedia(*store, wave, 1, 0).ok) throw std::runtime_error("fixture source import failed");
    db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(11,1,0,48000)");
    audio::DecodeCache decode(temp.path() / "decoded");
    media::AnalysisCache cache(temp.path() / "analysed");
    const auto analysis = media::buildClipPitchAnalysis(*store, 11, {}, &decode, &cache);
    check(analysis && analysis.data->analysis.notes.size() == 3, "the shared finder analyzes the scale in the source file");
    if (!analysis) return;
    PitchMidiOptions options;
    options.content = PitchMidiContent::DetectedMelody;
    const auto midi = preparePitchMidi(*store, *analysis.data, options);
    check(midi && midi.data->projection.notes.size() == 3 &&
          midi.data->projection.notes[0].key == 60 && midi.data->projection.notes[1].key == 62 &&
          midi.data->projection.notes[2].key == 64,
          "shared cached analysis converts the scale to its exact MIDI keys");
}

void durableEdits() {
    Fixture f;
    f.warp();
    std::ifstream schema(ADI_PITCH_SCHEMA_PATH);
    if (!schema) throw std::runtime_error("cannot open the pitch schema fixture");
    f.store->db().exec(std::string(std::istreambuf_iterator<char>(schema), {}));
    audio::PitchEditState state;
    const std::array commands{audio::PitchEditCommand{1, audio::PitchSetShift{100}},
                              audio::PitchEditCommand{1, audio::PitchSetTiming{7200,16800}},
                              audio::PitchEditCommand{2, audio::PitchSetGain{-std::numeric_limits<double>::infinity()}}};
    check(audio::applyPitchEdits(f.analysis.analysis, state, commands) == audio::PitchEditError::None,
          "fixture authored notes pass validation");
    const auto edit = prepareClipPitchEdits(f.analysis, state);
    check(edit.request && OpJournal(*f.store).commit(std::span(&*edit.request, 1)).ok,
          "fixture writes edits through the reserved project operation");
    const auto extraction = preparePitchMidi(*f.store, f.analysis);
    check(extraction && extraction.data->projection.notes.size() == 2 &&
          extraction.data->projection.notes.front().key == 58 &&
          extraction.data->projection.notes.front().start_ticks == quarter / 8,
          "Extract MIDI reads authored pitch, movement, and mute from the project");
    PitchMidiOptions options;
    options.content = PitchMidiContent::DetectedMelody;
    const auto melody = preparePitchMidi(*f.store, f.analysis, options);
    check(melody && melody.data->projection.notes.size() == 3 &&
          melody.data->projection.notes.front().key == 57 &&
          melody.data->projection.notes.front().start_ticks == 0,
          "Convert Melody reuses raw detection independently of authored pitch edits");
}

void callback() {
    Fixture f;
    unsigned allocations = callbackAllocations.load();
    PitchMidiProblem projectionProblem, preparationProblem;
    {
        audio::CallbackScope scope;
        projectionProblem = projectPitchMidi(*f.store, f.analysis, {}).problem;
        preparationProblem = preparePitchMidi(*f.store, f.analysis).problem;
    }
    check(projectionProblem == PitchMidiProblem::Realtime &&
          preparationProblem == PitchMidiProblem::Realtime && callbackAllocations.load() == allocations,
          "worker-only projection and preparation refuse a callback without allocation");
}
}

int main() {
    try { projection(); journal(); expressionAndDestinations(); analyzedMelody(); durableEdits(); callback(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
