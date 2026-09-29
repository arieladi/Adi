// SPDX-License-Identifier: GPL-3.0-or-later
//
// ProjectView (ADR-0201): the message-thread cell the UI's SnapshotReader
// copies from. What it must guarantee: a refresh after a commit, an undo or a
// redo shows the change; a snapshot a frame already holds never changes under
// it; unchanged tracks are shared rather than rebuilt.
#include "temp_directory.hpp"

#include "adi/engine/project_view.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

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
    auto store = Store::create(temp.path() / "p.adi", e);
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
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    run();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
