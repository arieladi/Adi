// SPDX-License-Identifier: GPL-3.0-or-later
//
// ProjectView (ADR-0201): the message-thread cell the UI's SnapshotReader
// copies from. What it must guarantee: a refresh after a commit, an undo or a
// redo shows the change; a snapshot a frame already holds never changes under
// it; unchanged tracks are shared rather than rebuilt.
#include "temp_directory.hpp"

#include "adi/blob.hpp"
#include "adi/engine/project_view.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/ui/op_submitter.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <array>
#include <cstdio>
#include <memory>
#include <string>

namespace {
using namespace adi;
using namespace adi::engine;

int checks = 0, failures = 0;
void check(bool ok, const std::string& name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}

void run() {
    adi::test::TempDirectory temp("project_view", "cell");
    StoreError e = StoreError::Ok;
    auto store = Store::createEmpty(temp.path() / "p.adi", e);
    check(store != nullptr, "a scratch project opens");
    if (!store) return;
    store->db().exec("INSERT INTO project(id,name,sample_rate) VALUES (1,'V',48000)");

    ProjectView view;
    check(view.current() == nullptr && view.generation() == 0, "nothing before the first refresh");

    OpJournal j(*store);
    for (int i = 1; i <= 5; ++i) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", i}, {"kind", "audio"}, {"name", "T" + std::to_string(i)}};
        j.commit(r);
    }
    check(view.refresh(*store) == 1, "the first refresh is generation 1");
    const auto first = view.current();
    check(first && first->tracks.size() == 5, "the first snapshot has the five tracks");

    // A frame holds `first`. A commit and a refresh must not change it.
    OpRequest vol;
    vol.opType = "mixer.setVolume";
    vol.payload = {{"id", 3}, {"db", -9.0}};
    check(j.commit(vol).ok, "a fader move commits");
    view.refresh(*store);
    const auto second = view.current();
    check(second->findTrack(3)->volumeDb == -9.0f, "after a refresh the view shows the fader move");
    check(first->findTrack(3)->volumeDb == 0.0f,
          "the snapshot a frame already took is untouched: it is immutable and still alive");
    check(SnapshotBuilder::sharedNodeCount(*first, *second) == 5,
          "the four untouched tracks and the tempo map are shared, not rebuilt");

    // Undo and redo move along the history; the view follows on refresh.
    History h(*store);
    check(h.undo().ok, "undo the fader move");
    view.refresh(*store);
    check(view.current()->findTrack(3)->volumeDb == 0.0f, "after undo the view shows the old value");
    check(h.redo().ok, "redo it");
    view.refresh(*store);
    check(view.current()->findTrack(3)->volumeDb == -9.0f, "after redo the view shows it again");
    check(view.generation() == 4, "one generation per refresh");

    // Nothing changed: a refresh still produces a new generation, and shares everything.
    const auto before = view.current();
    view.refresh(*store);
    check(SnapshotBuilder::sharedNodeCount(*before, *view.current()) == 6,
          "a refresh with no change shares all five tracks and the tempo map");
}

void midiPreviewNotes() {
    adi::test::TempDirectory temp("project_view", "midi-preview");
    StoreError error{};
    auto store = Store::createEmpty(temp.path() / "midi.adi", error);
    check(store != nullptr, "a project for MIDI previews opens");
    if (!store) return;
    store->db().exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'MIDI previews',48000);"
                     "INSERT INTO tracks(id,kind,name) VALUES(1,'midi','First'),(2,'midi','Second');"
                     "INSERT INTO lanes(id,track_id,kind) VALUES(22,1,'take');"
                     "INSERT INTO clips(id,track_id,lane_id,kind,pos_ticks,length_ticks,content_offset_ticks,"
                     "loop_enabled,loop_start_ticks,loop_len_ticks) VALUES"
                     "(11,1,NULL,'midi',0,1920,16,1,120,960),(12,1,22,'midi',0,1920,0,0,0,NULL),"
                     "(13,2,NULL,'midi',0,1920,0,0,0,NULL)");
    NoteRecord note{};
    note.note_id = 1;
    note.start_ticks = 20;
    note.dur_ticks = 120;
    note.key = 61;
    note.vel_on = 82;
    note.probability = 10000;
    const auto seed = [&](std::int64_t clip) {
        const auto bytes = writeStream<NoteRecord>(FourCC::Notes, std::array{note});
        return store->putEventStream(clip, "notes", bytes);
    };
    check(seed(11), "seed the main clip's native ANOT");
    note.flags = NoteFlags::Muted;
    check(seed(12), "seed the take clip's muted note");
    note.note_id = 0;
    check(seed(13), "seed a legacy unassigned note identity");

    ProjectView view;
    view.refresh(*store);
    ui::OpSubmitter submitter(*store, view);
    const auto before = view.current();
    const auto main = [](const auto& snap) { return snap->findTrack(1)->clips.front(); };
    const auto take = [](const auto& snap) { return snap->findTrack(1)->takeClips.front(); };
    const auto other = [](const auto& snap) { return snap->findTrack(2)->clips.front(); };
    check(main(before)->midiPreview && main(before)->midiPreview->startMarker == 16 &&
          main(before)->midiPreview->loopStart == 120 && main(before)->midiPreview->loopLength == 960 &&
          main(before)->midiPreview->bars == std::vector<MidiClipPreview::Bar>{{20,120,61,true}},
          "main bars retain the MIDI source clock and native note values");
    check(take(before)->midiPreview && take(before)->midiPreview->bars == std::vector<MidiClipPreview::Bar>{{20,120,61,false}} &&
          other(before)->midiPreview && other(before)->midiPreview->bars.size() == 1,
          "take previews retain muted notes and legacy unassigned notes remain visible");
    const auto insert = [&](std::int64_t clip) {
        OpRequest op;
        op.opType = "note.insert";
        op.payload = {{"clip",clip},{"note",2},{"start",100},{"dur",120},{"key",70}};
        return submitter.submit(std::move(op));
    };
    check(insert(11).ok, "commit a scoped note insertion in the main clip");
    const auto afterMain = view.current();
    check(main(afterMain)->midiPreview->bars.size() == 2 && main(before)->midiPreview->bars.size() == 1 &&
          take(afterMain) == take(before) && other(afterMain) == other(before) && view.localEdit().has_value(),
          "a local note insertion copies just the changed clip and preserves its old snapshot");
    check(insert(12).ok, "commit a scoped note insertion in the take clip");
    const auto afterTake = view.current();
    check(take(afterTake)->midiPreview->bars.size() == 2 && main(afterTake) == main(afterMain) && other(afterTake) == other(before),
          "take-lane note updates share the main clip and the unaffected track");
    check(submitter.undo().ok, "undo the take note insertion");
    const auto undone = view.current();
    check(take(undone)->midiPreview->bars.size() == 1 && main(undone) == main(afterTake) && other(undone) == other(before),
          "full Undo projection restores the old bars and shares unchanged clips");

    OpRequest move;
    move.opType = "clip.move";
    move.payload = {{"id",11},{"to",960},{"track",1}};
    check(submitter.submit(std::move(move)).ok && main(view.current())->posTicks == 960 &&
          main(view.current())->midiPreview == main(undone)->midiPreview,
          "moving an arrangement clip does not replace its source preview");
    OpRequest loop;
    loop.opType = "clip.setLoop";
    loop.payload = {{"id",11},{"on",false}};
    check(submitter.submit(std::move(loop)).ok && main(view.current())->midiPreview->loopLength == 0 &&
          main(view.current())->midiPreview->bars == main(undone)->midiPreview->bars,
          "a loop edit changes the drawing clock and retains the native note bars");
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    run();
    midiPreviewNotes();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
