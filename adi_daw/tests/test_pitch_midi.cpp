// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_midi.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/digest.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/engine/mpe_output.hpp"
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
          reverse.data->notes[0].dur_ticks == 7 * quarter / 12 &&
          reverse.data->notes[1].start_ticks == quarter &&
          reverse.data->notes[2].start_ticks == 5 * quarter / 2,
          "reverse reflects both Warp marker clocks as the pager does");
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

void clipPlaybackClock() {
    Fixture f;
    f.store->db().exec("UPDATE audio_clips SET transpose_semis=12 WHERE clip_id=11");
    PitchMidiOptions options;
    options.keepTuning = true;
    options.volumeMode = PitchMidiVolumeMode::NoteGain;
    const std::vector<float> pcm(48000, 0.25f);
    options.velocitySource = pcm;
    const auto octave = projectPitchMidi(*f.store, f.analysis, {}, options);
    check(octave && octave.data->notes.size() == 3 && octave.data->notes.front().key == 69 &&
          octave.data->notes.front().tuning_cents == 27.5f && octave.data->notes.front().dur_ticks == quarter / 5 &&
          octave.data->notes[1].start_ticks == 2 * quarter / 5,
          "unwarped octave transpose doubles playback rate and halves projected note time");
    check(octave && !octave.data->gainCurves.empty() && octave.data->gainCurves.front().points.size() == 42,
          "unwarped transpose leaves amplitude windows indexed in the 48 kHz source");

    f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=0,loop_len_ticks=5765760 WHERE id=11");
    const auto legacy = projectPitchMidi(*f.store, f.analysis, {}, options);
    check(octave && legacy && legacy.data->notes.size() == octave.data->notes.size() &&
          legacy.data->notes.back().start_ticks == octave.data->notes.back().start_ticks &&
          legacy.data->notes.back().dur_ticks == octave.data->notes.back().dur_ticks,
          "a legacy unwarped loop outside the transposed window projects the audible single pass");
    f.store->db().exec("UPDATE clips SET loop_enabled=0,loop_len_ticks=NULL WHERE id=11");

    f.store->db().exec("UPDATE audio_clips SET transpose_semis=7 WHERE clip_id=11");
    const auto rounded = projectPitchMidi(*f.store, f.analysis, {}, options);
    const auto effectiveCents = 1200 * std::log2(std::round(48000 * std::exp2(7.0 / 12)) / 48000);
    check(rounded && !rounded.data->notes.empty() && rounded.data->notes.front().key == 64 &&
          std::abs(rounded.data->notes.front().tuning_cents - (27.5 + effectiveCents - 700)) < 1e-4,
          "fractional tuning uses the rounded physical playback rate");

    // The source is 0.8 seconds and this map consumes 0.8 seconds at 120 BPM.
    constexpr std::array identity{WarpMarker{0, 0}, WarpMarker{38400, 8 * quarter / 5}};
    const auto map = audio::writeWarpMap(identity, 38400);
    if (!map) throw std::runtime_error("fixture identity Warp map");
    SQLite::Statement set(f.store->db(), "UPDATE audio_clips SET warp_enabled=1,warp_mode='re-pitch',"
                                       "warp_markers=?,transpose_semis=12 WHERE clip_id=11");
    set.bind(1, map.bytes.data(), static_cast<int>(map.bytes.size()));
    set.exec();
    const auto repitched = projectPitchMidi(*f.store, f.analysis, {}, options);
    check(repitched && repitched.data->notes.size() == 3 && repitched.data->notes.front().key == 57 &&
          repitched.data->notes.front().tuning_cents == 27.5f &&
          repitched.data->notes.front().dur_ticks == 2 * quarter / 5,
          "identity Re-Pitch Warp ignores the retained clip transpose");
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

void installPitchSchema(Store& store) {
    std::ifstream schema(ADI_PITCH_SCHEMA_PATH);
    if (!schema) throw std::runtime_error("cannot open the pitch schema fixture");
    store.db().exec(std::string(std::istreambuf_iterator<char>(schema), {}));
}

std::vector<std::int64_t> extractionMarkers(const Store& store) {
    SQLite::Statement rows(store.db(), "SELECT clip_id FROM clip_pitch_extractions ORDER BY clip_id");
    std::vector<std::int64_t> result;
    while (rows.executeStep()) result.push_back(rows.getColumn(0).getInt64());
    return result;
}

void extractionLineage() {
    Fixture f;
    f.warp();
    installPitchSchema(*f.store);
    const auto initial = digestProject(*f.store).text;
    PitchMidiOptions options;
    options.keepTuning = true;
    const auto first = preparePitchMidi(*f.store, f.analysis, options);
    check(first && digestProject(*f.store).text == initial, "lineage prepare leaves the project unchanged");
    if (!first) return;
    const auto old = first.data->midiClip;
    const auto track = first.data->track;
    const auto entered = commitPitchMidi(*f.store, *first.data);
    check(entered.ok && extractionMarkers(*f.store) == std::vector{old},
          "new-track extraction marks the output in its creation transaction");
    if (!entered.ok) return;
    const auto created = digestProject(*f.store).text;
    check(History(*f.store).undo().ok && digestProject(*f.store).text == initial &&
          History(*f.store).redo().ok && digestProject(*f.store).text == created,
          "Undo and Redo of notes-only new-track extraction include its marker and native notes");

    {
        SQLite::Statement lane(f.store->db(), "INSERT INTO lanes(id,track_id) VALUES(19,?)");
        lane.bind(1, track);
        lane.exec();
        SQLite::Statement clips(f.store->db(),
            "INSERT INTO clips(id,track_id,lane_id,kind,name,pos_ticks,length_ticks) VALUES"
            "(101,?,NULL,'midi','Old extraction outside the new range',?,?),"
            "(102,?,NULL,'midi','Unrelated MIDI',?,?),"
            "(103,?,19,'midi','Take lane extraction',?,?),"
            "(104,2,NULL,'midi','Different track extraction',?,?)");
        const std::array positions{6 * quarter, 8 * quarter, quarter};
        for (int i = 0; i < 3; ++i) {
            clips.bind(i * 3 + 1, track);
            clips.bind(i * 3 + 2, positions[static_cast<std::size_t>(i)]);
            clips.bind(i * 3 + 3, quarter);
        }
        clips.bind(10, quarter);
        clips.bind(11, quarter);
        clips.exec();
    }
    f.store->db().exec("UPDATE tracks SET kind='midi' WHERE id=2;"
                       "INSERT INTO clip_pitch_extractions(clip_id) VALUES(101),(103),(104)");
    const auto snapshotBefore = digestProject(*f.store).text;
    if (!f.store->close()) throw std::runtime_error("cannot close extraction lineage fixture");
    StoreError error{};
    f.store = Store::open(f.temp.path() / "a.adi", error);
    check(f.store && digestProject(*f.store).text == snapshotBefore,
          "initial markers and native output reopen exactly");
    if (!f.store) return;
    options.destinationTrack = track;
    options.includePitchCurve = true;

    f.store->db().exec("UPDATE clips SET pos_ticks=5765760 WHERE id=102");
    const auto overlapBefore = digestProject(*f.store).text;
    check(!preparePitchMidi(*f.store, f.analysis, options) && digestProject(*f.store).text == overlapBefore,
          "an unrelated overlapping main-lane clip protects itself and every prior extraction");
    f.store->db().exec("UPDATE clips SET pos_ticks=46126080 WHERE id=102");
    check(digestProject(*f.store).text == snapshotBefore, "clearing fixture overlap restores the setup exactly");

    const auto replacement = preparePitchMidi(*f.store, f.analysis, options);
    check(replacement && replacement.data->track == track && digestProject(*f.store).text == snapshotBefore,
          "selected extraction discovers marked outputs after reopening without preparing writes");
    if (!replacement) {
        if (replacement.detail) std::printf("Lineage preparation error: %s\n", replacement.detail->c_str());
        return;
    }
    const auto newer = replacement.data->midiClip;
    const auto committed = commitPitchMidi(*f.store, *replacement.data);
    check(committed.ok && committed.seqs.size() == 3 &&
          extractionMarkers(*f.store) == std::vector<std::int64_t>{103, 104, newer},
          "selected extraction replaces all and only the marked main-lane clips in one action");
    if (!committed.ok) {
        std::printf("Lineage commit error: %s\n", committed.error.c_str());
        return;
    }
    const auto fresh = engine::readClipDetails(*f.store, newer, true, true);
    check(f.store->db().execAndGet("SELECT count(*) FROM clips WHERE id IN (101,102,103,104)").getInt() == 3 &&
          !engine::readClipDetails(*f.store, old).clip && fresh.clip && fresh.clip->noteExpressions.curves.size() == 3,
          "whole replacement preserves unrelated clips and writes the new native pitch curves");

    const auto snapshotAfter = digestProject(*f.store).text;
    const auto copied = copyPitchMidi(*f.store, f.analysis);
    check(copied && copied.data->image.at("children").value("clip_pitch_extractions", Payload::array()).empty(),
          "detached clipboard output has no automatic-replacement marker");
    check(History(*f.store).undo().ok && digestProject(*f.store).text == snapshotBefore &&
          History(*f.store).redo().ok && digestProject(*f.store).text == snapshotAfter,
          "one Undo and Redo restore exact output bytes, all prior marks, and unrelated clips");
    if (!f.store->close()) throw std::runtime_error("cannot close replacement lineage fixture");
    f.store = Store::open(f.temp.path() / "a.adi", error);
    check(f.store && digestProject(*f.store).text == snapshotAfter &&
          History(*f.store).undo().ok && digestProject(*f.store).text == snapshotBefore &&
          History(*f.store).redo().ok && digestProject(*f.store).text == snapshotAfter,
          "reopen retains replacement lineage and exact journal replay");
    if (!f.store) return;
    PitchMidiOptions melodyOptions;
    melodyOptions.content = PitchMidiContent::DetectedMelody;
    const auto melody = preparePitchMidi(*f.store, f.analysis, melodyOptions);
    check(melody && commitPitchMidi(*f.store, *melody.data).ok &&
          extractionMarkers(*f.store) == std::vector<std::int64_t>{103, 104, newer},
          "Convert Melody creates an unmarked clip");
    OpRequest removeSource;
    removeSource.opType = "clip.delete";
    removeSource.payload = {{"id", 11}};
    check(OpJournal(*f.store).commit(std::span(&removeSource, 1)).ok &&
          extractionMarkers(*f.store) == std::vector<std::int64_t>{103, 104, newer},
          "output marker outlives the audio source clip");
}

void markFirstSelectedExtraction() {
    Fixture f;
    installPitchSchema(*f.store);
    f.store->db().exec("UPDATE tracks SET kind='instrument' WHERE id=2");
    PitchMidiOptions options;
    options.destinationTrack = 2;
    const auto placed = preparePitchMidi(*f.store, f.analysis, options);
    check(placed && commitPitchMidi(*f.store, *placed.data).ok &&
          extractionMarkers(*f.store) == std::vector{placed.data->midiClip},
          "first extraction on a selected instrument track records its lineage");
}

void unfinishedExtractionSchema() {
    Fixture f;
    installPitchSchema(*f.store);
    f.store->db().exec("DROP TABLE clip_pitch_extractions");
    const auto oldSchema = digestProject(*f.store).text;
    const auto prepared = preparePitchMidi(*f.store, f.analysis);
    check(prepared && commitPitchMidi(*f.store, *prepared.data).ok &&
          !f.store->db().tableExists("clip_pitch_extractions") && History(*f.store).undo().ok &&
          digestProject(*f.store).text == oldSchema,
          "extraction and Undo still work in an earlier staged project without the marker table");
}

void durableEdits() {
    Fixture f;
    f.warp();
    installPitchSchema(*f.store);
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

void durableGeometry() {
    Fixture f;
    f.warp();
    installPitchSchema(*f.store);
    auto draft = audio::draftPitchSegments(f.analysis.analysis, {});
    if (!draft) throw std::runtime_error("fixture pitch segmentation is invalid");
    const std::array<audio::PitchSegmentationCommand, 1> remove{audio::PitchDeleteSegment{3}};
    if (audio::applyPitchSegmentation(f.analysis.analysis, *draft.draft, remove) != audio::PitchEditError::None)
        throw std::runtime_error("fixture could not delete its segment");
    const auto authored = prepareClipPitchEdits(f.analysis, draft.draft->edits, &draft.draft->geometry);
    if (!authored.request || !OpJournal(*f.store).commit(*authored.request).ok)
        throw std::runtime_error("fixture could not store segmentation");

    const auto extraction = preparePitchMidi(*f.store, f.analysis);
    check(extraction && extraction.data->projection.notes.size() == 2 &&
          extraction.data->projection.notes[0].key == 57 && extraction.data->projection.notes[1].key == 72,
          "edited extraction uses effective geometry and excludes a deleted segment");
    const auto copied = copyPitchMidi(*f.store, f.analysis);
    if (copied) {
        const auto& bytes = copied.data->image.at("children").at("event_streams").at(0).at("data").get_binary();
        StreamReader<NoteRecord> stream({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()}, FourCC::Notes);
        check(stream.ok() && stream.count() == 2 && stream.at(1)->key == 72,
              "edited clipboard uses effective geometry");
    } else check(false, "edited clipboard uses effective geometry");
    PitchMidiOptions raw;
    raw.content = PitchMidiContent::DetectedMelody;
    const auto melody = preparePitchMidi(*f.store, f.analysis, raw);
    check(melody && melody.data->projection.notes.size() == 3 && melody.data->projection.notes[1].key == 69,
          "Convert Melody keeps the original detected segment");
    auto incompatible = f.analysis;
    ++incompatible.analysisVersion;
    check(preparePitchMidi(*f.store, incompatible).problem == PitchMidiProblem::Edits,
          "geometry with the same source hash still checks the analysis version");

    const audio::PitchSegmentation noSegments{{}, draft.draft->geometry.nextId};
    const auto cleared = prepareClipPitchEdits(f.analysis, {}, &noSegments);
    if (!cleared.request || !OpJournal(*f.store).commit(*cleared.request).ok)
        throw std::runtime_error("fixture could not store empty geometry");
    check(preparePitchMidi(*f.store, f.analysis).problem == PitchMidiProblem::Empty &&
          copyPitchMidi(*f.store, f.analysis).problem == PitchMidiProblem::Empty,
          "an explicit empty geometry stays empty in extraction and clipboard");

    f.store->db().exec("UPDATE clip_pitch_edits SET edits=X'00' WHERE clip_id=11");
    check(preparePitchMidi(*f.store, f.analysis).problem == PitchMidiProblem::Edits &&
          copyPitchMidi(*f.store, f.analysis).problem == PitchMidiProblem::Edits,
          "malformed geometry on the current source refuses extraction");
    check(static_cast<bool>(preparePitchMidi(*f.store, f.analysis, raw)),
          "Convert Melody does not depend on authored geometry decoding");
    SQLite::Statement changed(f.store->db(), "UPDATE media_files SET hash_blake3=? WHERE id=1");
    f.analysis.source.hash = std::string(64, 'b');
    changed.bind(1, f.analysis.source.hash);
    changed.exec();
    const auto dormant = preparePitchMidi(*f.store, f.analysis);
    check(dormant && dormant.data->projection.notes.size() == 3 && copyPitchMidi(*f.store, f.analysis),
          "a reattached source ignores an older source's dormant authored geometry");
}

void amplitudeExtraction() {
    Fixture f;
    f.warp();
    installPitchSchema(*f.store);
    audio::PitchEditState state;
    const std::array commands{audio::PitchEditCommand{1, audio::PitchSetGain{20 * std::log10(2.0)}},
                              audio::PitchEditCommand{1, audio::PitchSetTiming{7200,16800}}};
    if (audio::applyPitchEdits(f.analysis.analysis, state, commands) != audio::PitchEditError::None)
        throw std::runtime_error("invalid amplitude fixture");
    const auto authored = prepareClipPitchEdits(f.analysis, state);
    if (!authored.request || !OpJournal(*f.store).commit(*authored.request).ok)
        throw std::runtime_error("could not store segment gain");
    auto& db = f.store->db();
    const auto before = digestProject(*f.store).text;
    const std::array<std::size_t, 1> selected{1};
    PitchMidiOptions options;
    options.selectedNotes = selected;
    options.velocity = 97;
    options.volumeMode = PitchMidiVolumeMode::NoteGain;
    check(preparePitchMidi(*f.store, f.analysis, options).problem == PitchMidiProblem::Source,
          "continuous volume requires source PCM");
    std::vector<float> pcm(static_cast<std::size_t>(f.analysis.source.frames), 0.5f);
    std::fill_n(pcm.begin() + 4800, 4800, 0.25f);
    options.velocitySource = pcm;
    const auto prepared = preparePitchMidi(*f.store, f.analysis, options);
    check(prepared && prepared.data->projection.gainCurves.size() == 1 &&
          prepared.data->projection.notes[0].vel_on == 97,
          "per-note gain preserves the fixed attack velocity");
    if (!prepared) return;
    const auto placed = commitPitchMidi(*f.store, *prepared.data);
    const auto clip = engine::readClipDetails(*f.store, prepared.data->midiClip, true, true);
    const auto after = digestProject(*f.store).text;
    check(placed.ok && clip.clip && clip.clip->notes.size() == 1 &&
          clip.clip->notes[0].startTicks == quarter / 8 && clip.clip->notes[0].durationTicks == quarter / 2 &&
          clip.clip->noteExpressions.curves.size() == 1, "authored timing and AEXP gain commit to a new MIDI track");
    if (clip.clip && clip.clip->noteExpressions.curves.size() == 1) {
        const auto& gain = clip.clip->noteExpressions.curves.front();
        check(gain.dimension == static_cast<int>(ExpressionDim::Gain) && gain.points.size() > 2 &&
              gain.points.front().time_ticks == 0 && gain.points.back().time_ticks == quarter / 2 &&
              std::abs(gain.points.front().value + 3.0103f) < 0.001f &&
              std::abs(gain.points.back().value - 3.0103f) < 0.001f,
              "gain AEXP round-trips a measured six-dB source step plus authored gain");
        engine::Event native;
        native.type = engine::EventType::NoteExpression;
        native.dim = static_cast<std::uint16_t>(ExpressionDim::Gain);
        native.paramId = engine::kNativeNoteExpressionUnits;
        native.value = gain.points.back().value;
        engine::MpeOut host;
        check(engine::noteExpressionOut(native, {}, host) &&
              std::abs(host.plain - std::numbers::sqrt2) < 1e-6 &&
              std::abs(host.value - std::numbers::sqrt2 / 4) < 1e-6,
              "native note expression converts persisted dB to linear CLAP and normalized VST3 gain");
    }
    check(History(*f.store).undo().ok && digestProject(*f.store).text == before &&
          History(*f.store).redo().ok && digestProject(*f.store).text == after &&
          History(*f.store).undo().ok, "one undo and redo capture the new track's gain stream exactly");

    options.volumeMode = PitchMidiVolumeMode::ChannelController;
    options.volumeController = 121;
    check(preparePitchMidi(*f.store, f.analysis, options).problem == PitchMidiProblem::Edits,
          "channel mode messages are refused as volume controllers");
    options.volumeController = 7;
    const auto newController = preparePitchMidi(*f.store, f.analysis, options);
    check(newController && commitPitchMidi(*f.store, *newController.data).ok &&
          controllers::readLanes(*f.store, newController.data->midiClip).lanes.size() == 1 &&
          History(*f.store).undo().ok && digestProject(*f.store).text == before,
          "new-track native Capture journals volume and undo in the same batch");
    f.store->db().exec("UPDATE tracks SET kind='midi' WHERE id=2");
    options.destinationTrack = 2;
    options.volumeController = 11;
    const auto controllerBefore = digestProject(*f.store).text;
    const auto controller = preparePitchMidi(*f.store, f.analysis, options);
    check(controller && controller.data->projection.volumeCurve &&
          controller.data->projection.notes[0].vel_on == 97, "ACTL mode also preserves the fixed attack");
    if (!controller) return;
    const auto controllerCommit = commitPitchMidi(*f.store, *controller.data);
    const auto lanes = controllers::readLanes(*f.store, controller.data->midiClip);
    check(controllerCommit.ok && lanes.ok && lanes.lanes.size() == 1 &&
          lanes.lanes[0].controller == 11 && lanes.lanes[0].channel == 0 &&
          lanes.lanes[0].points.size() > 2 &&
          lanes.lanes[0].points.front().timeTicks == quarter / 8 &&
          std::abs(lanes.lanes[0].points.front().value - std::numbers::sqrt2 / 2) < 1e-6 &&
          lanes.lanes[0].points.back().value == 1,
          "ACTL round-trips selected CC and clamps normalized gain at unity");
    const auto controllerAfter = digestProject(*f.store).text;
    check(History(*f.store).undo().ok && digestProject(*f.store).text == controllerBefore &&
          History(*f.store).redo().ok && digestProject(*f.store).text == controllerAfter &&
          History(*f.store).undo().ok && db.execAndGet("SELECT count(*) FROM tracks").getInt() == 2,
          "selected-track ACTL is one undo step and creates no extra tracks");

    f.store->db().exec("UPDATE audio_clips SET reverse=1 WHERE clip_id=11");
    f.analysis.reversed = true;
    options.volumeMode = PitchMidiVolumeMode::NoteGain;
    const auto reversed = projectPitchMidi(*f.store, f.analysis, state, options);
    check(reversed && reversed.data->gainCurves.front().points.front().value > 3 &&
          reversed.data->gainCurves.front().points.back().value < -3,
          "volume observations reverse with their edited segment");

    f.store->db().exec("UPDATE audio_clips SET reverse=0 WHERE clip_id=11;"
        "UPDATE clips SET length_ticks=23063040,loop_enabled=1,"
        "loop_start_ticks=5765760,loop_len_ticks=5765760 WHERE id=11");
    f.analysis.reversed = false;
    options.volumeMode = PitchMidiVolumeMode::ChannelController;
    const std::array<std::size_t, 1> loopNote{2};
    options.selectedNotes = loopNote;
    const auto looped = projectPitchMidi(*f.store, f.analysis, state, options);
    check(looped && looped.data->notes.size() == 3 && looped.data->volumeCurve &&
          looped.data->volumeCurve->points.back().time == 4 * quarter - 1 &&
          std::any_of(looped.data->volumeCurve->points.begin(), looped.data->volumeCurve->points.end(),
              [](const auto& point) { return point.time == 3 * quarter; }),
          "loop repetition places CC attacks and keeps the final point inside the clip");
    std::stop_source stopped;
    stopped.request_stop();
    options.stop = stopped.get_token();
    check(projectPitchMidi(*f.store, f.analysis, state, options).problem == PitchMidiProblem::Cancelled,
          "pre-cancelled volume projection returns no prepared data");
}

void channelBendExtraction() {
    PitchMidiProjection projected;
    projected.lengthTicks = 2 * quarter;
    NoteRecord note{};
    note.start_ticks = quarter / 4;
    note.dur_ticks = quarter;
    note.note_id = 1;
    note.key = 57;
    note.channel = 3;
    note.tuning_cents = -100;
    projected.notes.push_back(note);
    note.start_ticks += note.dur_ticks;
    note.dur_ticks = quarter / 4;
    note.note_id = 2;
    note.key = 60;
    note.tuning_cents = 200;
    projected.notes.push_back(note);

    const auto staticBends = projectPitchMidiChannelBend(projected, PitchMidiBendMode::Static, 2);
    using Kind = PitchMidiBendEvent::Kind;
    const std::vector<PitchMidiBendEvent> expected{
        {0, 0, 8192, Kind::InitialCenter},
        {quarter / 4, 1, 4096, Kind::Attack},
        {5 * quarter / 4, 1, 8192, Kind::ReleaseCenter},
        {5 * quarter / 4, 2, 16383, Kind::Attack},
        {3 * quarter / 2, 2, 8192, Kind::ReleaseCenter}};
    check(staticBends && staticBends.data->channel == 3 && staticBends.data->rangeSemitones == 2 &&
          staticBends.data->events == expected,
          "static bend keeps initial and final rests centered and resets before an adjacent note attacks");

    const auto makePoint = [](std::int64_t tick, float value) {
        ExpressionPoint point{};
        point.time_ticks = tick;
        point.value = value;
        point.curve = static_cast<std::uint8_t>(Curve::Linear);
        return point;
    };
    projected.pitchCurves = {{1, {makePoint(0, -1), makePoint(quarter, 2)}},
                            {2, {makePoint(0, 0), makePoint(quarter / 4, 0)}}};
    const auto continuous = projectPitchMidiChannelBend(projected, PitchMidiBendMode::Continuous, 2);
    check(continuous && continuous.data->events.size() > expected.size() &&
          continuous.data->events[1] == PitchMidiBendEvent{quarter / 4, 1, 0, Kind::Attack} &&
          continuous.data->events.back() == expected.back(),
          "continuous bend combines note tuning with pitch expression and reaches the negative endpoint");
    if (continuous) {
        const auto& events = continuous.data->events;
        const auto has = [&events](PitchMidiBendEvent event) {
            return std::find(events.begin(), events.end(), event) != events.end();
        };
        check(has({3 * quarter / 4, 1, 6144, Kind::Movement}) &&
              has({5 * quarter / 4 - 1, 1, 12287, Kind::Movement}) &&
              std::equal(expected.begin() + 2, expected.end(), events.end() - 3),
              "sustained linear contour produces timed movement through release and centers between notes");
    }

    check(projectPitchMidiChannelBend(projected, PitchMidiBendMode::Static, 0).problem == PitchMidiProblem::Edits &&
          projectPitchMidiChannelBend(projected, PitchMidiBendMode::Static, 25).problem == PitchMidiProblem::Edits,
          "ordinary channel bend requires an explicit 1 to 24 semitone sensitivity");
    const auto overflow = projectPitchMidiChannelBend(projected, PitchMidiBendMode::Static, 1);
    check(overflow.problem == PitchMidiProblem::Edits && !overflow.data && overflow.detail,
          "a bend beyond sensitivity fails without publishing clamped data");
    projected.pitchCurves[0].points.back().value = 3.125f;
    check(projectPitchMidiChannelBend(projected, PitchMidiBendMode::Continuous, 2).problem == PitchMidiProblem::Edits,
          "out-of-range contour endpoint fails even at the exclusive note end");
    projected.pitchCurves.pop_back();
    check(projectPitchMidiChannelBend(projected, PitchMidiBendMode::Continuous, 24).problem == PitchMidiProblem::Edits,
          "continuous bend requires the projected pitch contour for every note");
    --projected.notes.back().start_ticks;
    const auto overlap = projectPitchMidiChannelBend(projected, PitchMidiBendMode::Static, 2);
    check(overlap.problem == PitchMidiProblem::Edits && !overlap.data && overlap.detail,
          "a one-tick overlap cannot share the channel bend stream");

    PitchMidiProjection longSustain;
    longSustain.lengthTicks = 6000 * quarter;
    note.start_ticks = 0;
    note.dur_ticks = longSustain.lengthTicks;
    note.tuning_cents = 0;
    longSustain.notes.push_back(note);
    longSustain.pitchCurves = {{note.note_id, {makePoint(0, -1), makePoint(note.dur_ticks, 1)}}};
    const auto budget = projectPitchMidiChannelBend(longSustain, PitchMidiBendMode::Continuous, 2);
    check(budget.problem == PitchMidiProblem::Edits && !budget.data && budget.detail &&
          budget.detail->find("sample budget") != std::string::npos,
          "long changing contours have a bounded sample workload even when MIDI words coalesce");

    std::stop_source cancelled;
    cancelled.request_stop();
    check(projectPitchMidiChannelBend(projected, PitchMidiBendMode::Static, 2, cancelled.get_token()).problem ==
          PitchMidiProblem::Cancelled, "pre-cancelled channel bend returns no projection");

    Fixture f;
    f.warp();
    PitchMidiOptions options;
    options.keepTuning = true;
    options.includePitchCurve = true;
    f.analysis.analysis.notes[1].curve = {{4800, 5700}, {9600, 5727.5}, {14399, 5750}};
    const auto warp = projectPitchMidi(*f.store, f.analysis, {}, options);
    check(static_cast<bool>(warp), "projection captures a source contour through Warp");
    if (!warp) return;
    const auto warpedBends = projectPitchMidiChannelBend(*warp.data, PitchMidiBendMode::Continuous, 2);
    check(warpedBends && warpedBends.data->events.size() > 10 &&
          warpedBends.data->events[1] == PitchMidiBendEvent{0, 1, 8192, Kind::Attack} &&
          std::find(warpedBends.data->events.begin(), warpedBends.data->events.end(),
                    PitchMidiBendEvent{quarter / 4, 1, 9318, Kind::Movement}) != warpedBends.data->events.end() &&
          warpedBends.data->events.back() == PitchMidiBendEvent{3 * quarter, 3, 8192, Kind::ReleaseCenter},
          "channel bend uses the captured Warp contour and exact clip-end reset");
}

void nativeChannelBendExtraction() {
    Fixture f;
    f.warp();
    const std::vector<float> pcm(48000, 0.25f);
    PitchMidiOptions options;
    options.velocitySource = pcm;
    options.volumeMode = PitchMidiVolumeMode::ChannelController;
    options.volumeController = 11;
    options.channelBend = PitchMidiBendMode::Static;
    const auto before = digestProject(*f.store).text;
    const auto steady = preparePitchMidi(*f.store, f.analysis, options);
    check(steady && steady.data->projection.channelBendCurve &&
          steady.data->projection.notes.front().tuning_cents == 27.5f &&
          digestProject(*f.store).text == before,
          "native static bend automatically captures tuning and prepares without writes");
    if (!steady || !steady.data->projection.channelBendCurve) return;
    const auto& staticPoints = steady.data->projection.channelBendCurve->points;
    check(staticPoints.size() >= 2 && staticPoints.front().time == 0 &&
          std::lround(controllers::pitchBendWordExact(staticPoints.front().value)) == 9318 &&
          staticPoints[1].time == quarter / 2 && staticPoints[1].value == 0.5,
          "static attack overwrites initial center at tick zero and the rest starts centered");
    const auto placed = commitPitchMidi(*f.store, *steady.data);
    if (!placed.ok) std::printf("Native pitch bend commit: %s\n", placed.error.c_str());
    const auto baseClip = engine::readClipDetails(*f.store, steady.data->midiClip, true, true);
    const auto held = controllers::readLanes(*f.store, steady.data->midiClip);
    const auto bendLane = std::find_if(held.lanes.begin(), held.lanes.end(), [](const auto& lane) {
        return lane.controller == controllers::kPitchBend;
    });
    check(placed.ok && baseClip.clip && baseClip.clip->notes.size() == 3 &&
          baseClip.clip->notes.front().tuningCents == 0 && baseClip.clip->noteExpressions.curves.empty() &&
          held.ok && held.lanes.size() == 2 && bendLane != held.lanes.end() && bendLane->channel == 0 &&
          bendLane->points.size() == staticPoints.size() &&
          bendLane->points.front().value == staticPoints.front().value,
          "new-track Capture stores untuned ANOT, an exact wheel, and the separate volume CC");
    if (!placed.ok) return;
    const auto after = digestProject(*f.store).text;
    check(History(*f.store).undo().ok && digestProject(*f.store).text == before &&
          History(*f.store).redo().ok && digestProject(*f.store).text == after &&
          History(*f.store).undo().ok && digestProject(*f.store).text == before,
          "one Undo and Redo restore exact wheel and volume stream bytes");

    installPitchSchema(*f.store);
    f.store->db().exec("UPDATE tracks SET kind='instrument' WHERE id=2");
    f.analysis.analysis.notes[1].curve = {{4800, 5700}, {9600, 5750}, {14399, 5700}};
    options.channelBend = PitchMidiBendMode::Continuous;
    options.volumeMode = PitchMidiVolumeMode::NoteGain;
    options.destinationTrack = 2;
    const auto moving = preparePitchMidi(*f.store, f.analysis, options);
    check(moving && moving.data->projection.channelBendCurve &&
          moving.data->projection.pitchCurves.size() == 3,
          "native continuous bend captures the analyzed contour and keeps detached pitch data");
    if (!moving || !moving.data->projection.channelBendCurve) return;
    const auto& motion = moving.data->projection.channelBendCurve->points;
    const auto knot = std::find_if(motion.begin(), motion.end(), [](const auto& point) { return point.time == quarter / 4; });
    check(knot != motion.end() && std::lround(controllers::pitchBendWordExact(knot->value)) == 10240 &&
          std::all_of(motion.begin(), motion.end(), [&moving](const auto& point) {
              return point.time < moving.data->projection.lengthTicks &&
                     point.curve == static_cast<std::uint8_t>(Curve::Hold);
          }), "continuous bend stores exact wire movement as clip-local, half-open Hold points");
    const auto committed = commitPitchMidi(*f.store, *moving.data);
    const auto expressive = engine::readClipDetails(*f.store, moving.data->midiClip, true, true);
    check(committed.ok && expressive.clip && expressive.clip->noteExpressions.curves.size() == 3 &&
          std::all_of(expressive.clip->noteExpressions.curves.begin(), expressive.clip->noteExpressions.curves.end(),
              [](const auto& curve) { return curve.dimension == static_cast<int>(ExpressionDim::Gain); }),
          "selected-track wheel extraction stores per-note Gain without duplicate Pitch expression");
    const auto copied = copyPitchMidi(*f.store, f.analysis, options);
    check(copied && copied.data->image.at("children").at("event_streams").size() == 2 &&
          copied.data->image.at("children").at("event_streams").back().at("stream_kind") == "pitchbend" &&
          copied.data->image.at("children").at("event_streams").back().at("controller").is_null(),
          "clipboard image keeps the standard channel-only bend stream address");

    f.analysis.analysis.notes[1].curve[1].cents = 6000;
    const auto prior = digestProject(*f.store).text;
    const auto impossible = preparePitchMidi(*f.store, f.analysis, options);
    check(impossible.problem == PitchMidiProblem::Edits && !impossible.data && impossible.detail &&
          digestProject(*f.store).text == prior,
          "an out-of-range contour refuses the native extraction before replacing an earlier output");
}

void callback() {
    Fixture f;
    unsigned allocations = callbackAllocations.load();
    PitchMidiProblem projectionProblem, preparationProblem, bendProblem;
    // Built before the scope: an MSVC Debug std::vector allocates even when empty.
    const audio::PitchEditState noEdits;
    const PitchMidiProjection projected;
    {
        audio::CallbackScope scope;
        projectionProblem = projectPitchMidi(*f.store, f.analysis, noEdits).problem;
        preparationProblem = preparePitchMidi(*f.store, f.analysis).problem;
        bendProblem = projectPitchMidiChannelBend(projected, PitchMidiBendMode::Continuous, 2).problem;
    }
    check(projectionProblem == PitchMidiProblem::Realtime &&
          preparationProblem == PitchMidiProblem::Realtime && bendProblem == PitchMidiProblem::Realtime &&
          callbackAllocations.load() == allocations,
          "worker-only projection, channel bend and preparation refuse a callback without allocation");
}
}

int main() {
    try {
        projection(); clipPlaybackClock(); journal(); expressionAndDestinations(); analyzedMelody(); extractionLineage();
        markFirstSelectedExtraction(); unfinishedExtractionSchema(); durableEdits(); durableGeometry();
        amplitudeExtraction(); channelBendExtraction(); nativeChannelBendExtraction(); callback();
    }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
