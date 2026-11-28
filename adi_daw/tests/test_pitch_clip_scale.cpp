// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/ops.hpp"
#include "adi/pitch_clip_scale.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace {
using namespace adi;
using namespace adi::audio;
using namespace adi::clip_edit;

int checks = 0;
int failures = 0;

void check(bool passed, const char* description) {
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

double quantizeFrom(const PitchClipScaleCapture& captured, double cents) {
    if (!captured) return std::numeric_limits<double>::quiet_NaN();
    const PitchAnalysis source{48000, 100, {{0, 100, cents, 0, 0, {{25, cents}, {75, cents}}}}};
    const PitchEditCommand command[] = {{0, *captured.quantize}};
    PitchEditState edits;
    if (applyPitchEdits(source, edits, command) != PitchEditError::None) return std::numeric_limits<double>::quiet_NaN();
    return cents + resolvePitchNoteEdit(source, edits, 0).pitchShiftCents;
}

void storeCapture() {
    test::TempDirectory temporary{"pitch_scale", "owner"};
    StoreError error{};
    auto store = Store::createEmpty(temporary.path() / "voice.adi", error);
    if (!store) throw std::runtime_error(toString(error));
    auto& database = store->db();
    database.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Voice');"
                  "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES"
                  "(10,1,'audio',0,5765760),(11,1,'audio',5765760,5765760);"
                  "INSERT INTO key_map(id,pos_ticks,root,scale_id,scale_mask) VALUES"
                  "(1,0,2,'major',2741),(2,5765760,4,'custom',1);"
                  "INSERT INTO tuning_systems(id,name,source,period_cents) VALUES(7,'Tritave','scala',1901.955);"
                  "INSERT INTO tuning_degrees(tuning_id,degree,cents,name) VALUES(7,0,0,'Unison'),(7,2,175.5,'Second');"
                  "INSERT INTO key_map_tunings VALUES(2,7);INSERT INTO key_map_degrees VALUES(2,0),(2,2)");

    const auto inherited = capturePitchClipScale(*store, 10);
    check(inherited && !inherited.quantize->scale.scale()->local && inherited.quantize->tonicCents == 200 &&
          quantizeFrom(inherited, 6510) == 6600, "audio clip takes D major from its arrangement start");
    OpRequest setScale;
    setScale.opType = "clip.setScale";
    setScale.payload = {{"id", 10}, {"root", 5}, {"scale", "major"}, {"mask", 2741}};
    const auto changed = OpJournal(*store).commit(setScale);
    check(changed.ok, "existing clip scale operation sets a local 12-TET scale on the audio clip");
    const auto local = capturePitchClipScale(*store, 10);
    check(local && local.quantize->scale.scale()->local && local.quantize->tonicCents == 500 &&
          quantizeFrom(local, 6510) == 6500 && quantizeFrom(inherited, 6510) == 6600,
          "local F major overrides the timeline and earlier captures stay immutable");

    const auto missingTonic = capturePitchClipScale(*store, 11);
    check(!missingTonic && missingTonic.problem == PitchClipScaleProblem::TonicRequired,
          "custom tuning distinguishes its required tonic from a pitch-class root");
    const auto invalidTonic = capturePitchClipScale(*store, 11, std::numeric_limits<double>::infinity());
    check(!invalidTonic && invalidTonic.problem == PitchClipScaleProblem::InvalidTonic, "custom tonic must be a supported absolute pitch");
    const auto custom = capturePitchClipScale(*store, 11, 6080.25);
    check(custom && custom.quantize->tonicCents == 6080.25 && custom.quantize->scale.periodCents() == 1901.955 &&
          std::abs(quantizeFrom(custom, 6250) - 6255.75) < 1e-9,
          "custom scale quantizes against the caller's fractional absolute tonic and non-octave period");
    const auto missingClip = capturePitchClipScale(*store, 99);
    check(!missingClip && missingClip.problem == PitchClipScaleProblem::Read && missingClip.detail, "missing clip names its owner-read error");

    unsigned queries = 0;
    sqlite3_trace_v2(database.getHandle(), SQLITE_TRACE_STMT,
        [](unsigned, void* state, void*, void*) -> int { ++*static_cast<unsigned*>(state); return 0; }, &queries);
    {
        CallbackScope callback;
        const auto refused = capturePitchClipScale(*store, 10);
        check(!refused && refused.problem == PitchClipScaleProblem::Realtime && !refused.detail,
              "scale capture refuses callback use before constructing a context");
    }
    sqlite3_trace_v2(database.getHandle(), 0, nullptr, nullptr);
    check(queries == 0, "callback refusal performs zero SQL statements");
    std::printf("6510 cents captures to D major %.2f and F major %.2f cents; custom 6250 cents captures to %.2f cents\n",
                quantizeFrom(inherited, 6510), quantizeFrom(local, 6510), quantizeFrom(custom, 6250));
}

PitchAnalysis analysisFrom(std::span<const double> centers) {
    constexpr std::uint64_t framesPerNote = 3000;
    PitchAnalysis analysis{48000, centers.size() * framesPerNote, {}};
    for (std::size_t index = 0; index < centers.size(); ++index) {
        const auto start = index * framesPerNote;
        analysis.notes.push_back({start, start + framesPerNote, centers[index], 0, 0, {{start, centers[index]}}});
    }
    return analysis;
}

bool firstScale(const PitchClipScaleSuggestions& suggestion, std::int64_t root, std::string_view id,
                std::size_t votes, double correction) {
    if (!suggestion || suggestion.ranked().empty()) return false;
    const auto& top = suggestion.ranked().front();
    const auto& scale = *top.quantize.scale.scale();
    return scale.root == root && scale.id == id && top.matchedNotes == votes &&
           std::abs(top.meanCorrectionCents - correction) < 1e-9;
}

void standardSuggestions() {
    const std::array cents{6012., 6198., 6404., 6494., 6710., 6897., 7101., 7200.,
                           6200., 6400., 6600., 6700., 6900., 7100., 7300., 7400.};
    const auto analysis = analysisFrom(cents);
    std::array<std::size_t, cents.size()> all{};
    std::iota(all.begin(), all.end(), std::size_t{0});
    const auto shown = std::span(all).first(8);
    const auto laterPhrase = std::span(all).last(8);
    std::string error;
    const auto scale = engine::compileScaleContext({}, {}, error);
    check(scale.has_value(), "native major scale is available for suggestions");
    if (!scale) return;
    const PitchQuantize context{*scale, 0};
    PitchEditState edits;

    const auto visible = suggestPitchClipScales(analysis, edits, context, shown);
    check(firstScale(visible, 0, "major", 8, 4.75) && visible.testedNotes == 8 && visible.ranked().size() == 24,
          "empty selection scores the displayed C-major phrase alone and ranks all standard keys");
    const auto selected = suggestPitchClipScales(analysis, edits, context, all, laterPhrase);
    check(firstScale(selected, 2, "major", 8, 0) && selected.testedNotes == 8,
          "explicit selection ranks only the selected D-major phrase and uses the ending tonic");
    const auto offscreen = suggestPitchClipScales(analysis, edits, context, shown, laterPhrase);
    check(!offscreen && offscreen.error == PitchEditError::InvalidValue && offscreen.ranked().empty(),
          "a selection outside the displayed set is refused");
    const std::array invalid{analysis.notes.size()};
    const auto malformed = suggestPitchClipScales(analysis, edits, context, invalid);
    check(!malformed && malformed.error == PitchEditError::UnknownNote, "invalid displayed index is refused");

    const std::array raise{PitchEditCommand{3, PitchSetShift{100}}, PitchEditCommand{2, PitchSetGain{-70}},
                           PitchEditCommand{2, PitchSetTiming{6000, 6300}}};
    check(applyPitchEdits(analysis, edits, raise) == PitchEditError::None, "fixture has a raised pitch and quiet, shortened note");
    const auto altered = suggestPitchClipScales(analysis, edits, context, shown);
    check(firstScale(altered, 7, "major", 8, 4.75) && altered.testedNotes == 8,
          "a semitone edit changes the key while a quiet note at one-tenth duration keeps its vote");
    const std::array mute{PitchEditCommand{3, PitchSetGain{-std::numeric_limits<double>::infinity()}}};
    check(applyPitchEdits(analysis, edits, mute) == PitchEditError::None, "fixture mutes the raised note");
    const auto withoutMuted = suggestPitchClipScales(analysis, edits, context, shown);
    check(firstScale(withoutMuted, 0, "major", 7, 32.0 / 7) && withoutMuted.testedNotes == 7,
          "a muted note has no weight and the ending tonic resolves equivalent pitch sets");
    const std::array<std::size_t, 2> mutedSelection{3, 3};
    const auto muted = suggestPitchClipScales(analysis, edits, context, shown, mutedSelection);
    check(muted && muted.ranked().empty() && muted.testedNotes == 0, "a muted selection yields no suggestion and keeps its scope");
    const std::array<std::size_t, 3> repeated{2, 2, 0};
    const auto deduped = suggestPitchClipScales(analysis, edits, context, shown, repeated);
    check(deduped && deduped.testedNotes == 2, "repeated indices do not add votes");

    engine::ClipScale wholeTone;
    wholeTone.id = "whole-tone";
    wholeTone.mask = 0x0555;
    const auto capturedWholeTone = engine::compileScaleContext(wholeTone, {}, error);
    check(capturedWholeTone.has_value(), "custom 12-TET membership is available");
    if (capturedWholeTone) {
        const std::array wholeTones{6000., 6200., 6400., 6600., 6800., 7000., 7200.};
        const auto suggestion = suggestPitchClipScales(analysisFrom(wholeTones), {}, {*capturedWholeTone, 0},
                                                       std::span(all).first(wholeTones.size()));
        check(firstScale(suggestion, 0, "whole-tone", wholeTones.size(), 0) && suggestion.ranked().size() == 25,
              "the captured 12-TET scale participates alongside major and minor");
    }

    if (!visible.ranked().empty() && !selected.ranked().empty() && !altered.ranked().empty()) {
        std::printf("Scale fits: displayed %zu/%zu at %.2f cents, selected %zu/%zu at %.2f cents, edited root %lld\n",
                    visible.ranked().front().matchedNotes, visible.testedNotes, visible.ranked().front().meanCorrectionCents,
                    selected.ranked().front().matchedNotes, selected.testedNotes, selected.ranked().front().meanCorrectionCents,
                    static_cast<long long>(altered.ranked().front().quantize.scale.scale()->root));
    }
    const CallbackScope callback;
    const auto refused = suggestPitchClipScales(analysis, edits, context, shown);
    check(!refused && refused.error == PitchEditError::Realtime && !refused.candidates,
          "scale suggestions refuse callback use before constructing candidates");
}

void customSuggestions() {
    const std::array cents{6080.25, 6255.75, 6580.25, 7982.205};
    const std::array<std::size_t, cents.size()> all{0, 1, 2, 3};
    const auto analysis = analysisFrom(cents);
    engine::ClipScale definition;
    definition.root = 4;
    definition.id = "tritave";
    definition.tuningId = 7;
    definition.degrees = {0, 2};
    const engine::TuningDefinition tuning{
        7, "Tritave", "scala", {}, 1901.955, {{0, 0, "Unison"}, {2, 175.5, "Second"}, {9, 500, "Ninth"}}
    };
    std::string error;
    const auto scale = engine::compileScaleContext(definition, tuning, error);
    check(scale.has_value(), "custom scale compiles for suggestions");
    if (!scale) return;
    const PitchQuantize context{*scale, 6080.25};
    const auto suggested = suggestPitchClipScales(analysis, {}, context, all);
    check(firstScale(suggested, 4, "tritave", 3, 81.125) && suggested.ranked().size() == 1 &&
          suggested.ranked().front().quantize.tonicCents == 6080.25 &&
          suggested.ranked().front().quantize.scale.periodCents() == 1901.955,
          "custom suggestions retain absolute tonic and score exact degrees across non-octave periods");
    if (!suggested.ranked().empty()) {
        std::printf("Custom tuning fits %zu/%zu notes; mean correction %.3f cents\n",
                    suggested.ranked().front().matchedNotes, suggested.testedNotes,
                    suggested.ranked().front().meanCorrectionCents);
    }

    definition.degrees.clear();
    const auto empty = engine::compileScaleContext(definition, tuning, error);
    check(empty.has_value(), "custom context may have empty membership");
    if (!empty) return;
    const auto unsupported = suggestPitchClipScales(analysis, {}, PitchQuantize{*empty, 6080.25}, all);
    check(unsupported && unsupported.ranked().empty(), "an empty custom scale supplies no candidate");
}

} // namespace

int main() {
    storeCapture();
    standardSuggestions();
    customSuggestions();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
