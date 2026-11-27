// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/ops.hpp"
#include "adi/pitch_clip_scale.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

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

} // namespace

int main() {
    storeCapture();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
