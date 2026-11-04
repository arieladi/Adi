// SPDX-License-Identifier: GPL-3.0-or-later
//
// Step 7.1: the UI's two seams, headless. ADR-0180 d2, ADR-0050 d3, ADR-0124.
//
// No JUCE, no window, no component. `SnapshotReader` is tested against a
// HAND-BUILT `Snapshot` with no Store at all, which is the point of the
// no-committed-state rule: if the read half needs a database to be exercised,
// the shell above it will need one too.
//
// The check that matters most is `outlivesItsOwner`. The read half exists
// because the obvious implementation -- hold the publisher's pointer -- is a
// use-after-free, and an earlier draft of this file's header did exactly that.
// That check builds a snapshot, hands a reader its own owning reference, drops
// every other reference, and then reads. It passes here and would be a
// use-after-free under the design this one replaced.

#include "temp_directory.hpp"

#include "adi/engine/project_view.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/ui/op_submitter.hpp"
#include "adi/ui/arrangement_geometry.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/textproj.hpp"
#include "adi/ui/meters.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/range_edits.hpp"
#include "adi/ui/meter_edits.hpp"
#include "adi/ui/momentary_latch.hpp"
#include "adi/ui/reference_audition.hpp"
#include "adi/ui/session_state.hpp"
#include "adi/ui/send_edits.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "adi/ui/track_edits.hpp"
#include "adi/ui/track_order.hpp"
#include "adi/ui/track_tree.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace adi;

int g_checks = 0, g_failures = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }

// A requires-expression only substitutes in a dependent context, so the "this
// member must not exist" checks below need a template to hang on.
template <typename T>
concept HasPublish = requires(T& t) { t.publish(); };
template <typename T>
concept HasCollect = requires(T& t) { t.collect(); };
template <typename T>
inline constexpr bool kHasPublish = HasPublish<T>;
template <typename T>
inline constexpr bool kHasCollect = HasCollect<T>;

// --- the hand-built snapshot -------------------------------------------------

ui::SnapshotPtr makeSnapshot(std::uint64_t seq, int sampleRate,
                             std::vector<std::pair<std::int64_t, bool>> tracks) {
    auto s = std::make_shared<engine::Snapshot>();
    s->seq = seq;
    s->sampleRate = sampleRate;
    auto tempo = std::make_shared<engine::TempoMap>();
    tempo->events.push_back(engine::TempoEvent{0, 120.0, 0});
    s->tempo = tempo;
    for (const auto& [id, soloed] : tracks) {
        auto t = std::make_shared<engine::TrackNode>();
        t->id = id;
        t->name = "track " + std::to_string(id);
        t->soloed = soloed;
        s->tracks.push_back(std::move(t));
    }
    return s;
}

// --- SnapshotReader ----------------------------------------------------------

void theWrongThingDoesNotCompile() {
    section("a component cannot store a reader (UI-ARCHITECTURE.md)");
    static_assert(!std::is_copy_constructible_v<ui::SnapshotReader>);
    static_assert(!std::is_move_constructible_v<ui::SnapshotReader>);
    static_assert(!std::is_copy_assignable_v<ui::SnapshotReader>);
    static_assert(!std::is_move_assignable_v<ui::SnapshotReader>);
    static_assert(!std::is_default_constructible_v<ui::SnapshotReader>);
    check(true, "SnapshotReader is neither copyable nor movable");

    // The read half must not have grown a publish path. ADR-0180 d2 and the
    // plan both say nothing in adi::ui publishes; this pins it so that adding
    // one later fails the build rather than a review.
    static_assert(!kHasPublish<ui::SnapshotReader>);
    static_assert(!kHasCollect<ui::SnapshotReader>);
    check(true, "SnapshotReader has neither publish() nor collect()");
}

void invalidReaderAnswersEmpty() {
    section("a reader before the first publication");
    const ui::SnapshotReader r{nullptr};
    check(!r.valid(), "invalid");
    check(!static_cast<bool>(r), "falsey");
    check(r.seq() == 0, "seq is 0");
    check(r.sampleRate() == 0, "sampleRate is 0");
    check(r.tempo() == nullptr, "tempo is null");
    check(r.get() == nullptr, "get() is null");
    check(r.trackCount() == 0, "no tracks");
    check(r.tracks().empty(), "tracks() is an empty range, not a crash");
    check(r.findTrack(1) == nullptr, "findTrack finds nothing");
    check(!r.anySoloed(), "nothing is soloed");
}

void readsTheModel() {
    section("a reader over a hand-built snapshot -- no Store");
    const auto snap = makeSnapshot(7, 44100, {{10, false}, {20, true}});
    const ui::SnapshotReader r{snap};
    check(r.valid(), "valid");
    check(r.seq() == 7, "carries the publication's seq");
    check(r.sampleRate() == 44100, "sampleRate");
    check(r.trackCount() == 2, "two tracks");
    check(r.tempo() != nullptr && r.tempo()->events.size() == 1, "tempo map is readable");
    check(r.findTrack(10) != nullptr, "finds track 10");
    check(r.findTrack(10) != nullptr && r.findTrack(10)->name == "track 10", "and it is the right one");
    check(r.findTrack(999) == nullptr, "does not find a track that is not there");
    check(r.anySoloed(), "sees the soloed track");
    check(r.get() == snap.get(), "get() is the same object, not a copy");

    const auto quiet = makeSnapshot(8, 48000, {{10, false}});
    const ui::SnapshotReader r2{quiet};
    check(!r2.anySoloed(), "anySoloed is false when none is");
}

void onePublicationIsCheckable() {
    section("ADR-0050 d3: every component in one drain reads ONE snapshot");
    const auto snap = makeSnapshot(3, 48000, {{1, false}});
    const ui::SnapshotReader a{snap};
    const ui::SnapshotReader b{snap};
    check(a.sameAs(b) && b.sameAs(a), "two readers over one publication are identical");

    const auto other = makeSnapshot(4, 48000, {{1, false}});
    const ui::SnapshotReader c{other};
    check(!a.sameAs(c), "readers over different publications are not");
    check(a.seq() != c.seq(), "and the skew is visible in the seq");
}

void outlivesItsOwner() {
    section("the reader owns what it reads -- the use-after-free that was");
    // Build a snapshot, give the reader its own owning reference, then drop
    // every other reference. A design that held the publisher's raw pointer
    // reads freed memory here; this one reads its own root.
    std::unique_ptr<ui::SnapshotReader> reader;
    std::uint64_t seqBefore = 0;
    {
        auto snap = makeSnapshot(42, 96000, {{5, true}});
        seqBefore = snap->seq;
        check(snap.use_count() == 1, "one reference before the reader");
        reader = std::make_unique<ui::SnapshotReader>(snap);
        check(snap.use_count() == 2, "the reader took its own reference");
        snap.reset();   // the publisher, the cell, everyone else: gone
    }
    check(reader->valid(), "still valid after every other reference is gone");
    check(reader->seq() == seqBefore, "still the same publication");
    check(reader->sampleRate() == 96000, "still reads its fields");
    check(reader->trackCount() == 1, "still reads its tracks");
    check(reader->findTrack(5) != nullptr, "still resolves a track");
    check(reader->findTrack(5) != nullptr && reader->findTrack(5)->name == "track 5",
          "and the track's own storage is alive too");
    check(reader->anySoloed(), "and its nodes are intact");
}

// --- OpSubmitter -------------------------------------------------------------

std::unique_ptr<Store> freshProject(const fs::path& p) {
    StoreError err = StoreError::Ok;
    auto st = Store::createEmpty(p, err);
    if (st) st->db().exec("INSERT INTO project(id,name) VALUES (1,'Untitled')");
    return st;
}

OpRequest trackCreate(std::int64_t id, const char* name) {
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", id}, {"kind", "audio"}, {"name", name}};
    r.label = std::string("Create ") + name;
    r.targetKind = "track";
    r.targetId = id;
    return r;
}

std::int64_t countTracks(Store& s) {
    SQLite::Statement st(s.db(), "SELECT COUNT(*) FROM tracks");
    return st.executeStep() ? st.getColumn(0).getInt64() : -1;
}

void submitsOneOp() {
    section("one op, one undo step");
    adi::test::TempDirectory temp("ui_seams", "one");
    auto store = freshProject(temp.path() / "p.adi");
    check(store != nullptr, "store created");
    if (!store) return;

    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    const auto r = sub.submit(trackCreate(1, "Drums"));
    check(r.ok, "submit succeeded");
    check(static_cast<bool>(r), "and is truthy");
    check(r.txnId != 0, "carries a transaction id");
    check(r.seqs.size() == 1, "one op, one seq");
    check(r.error.empty(), "no error");
    check(!r.stale, "not stale");
    check(countTracks(*store) == 1, "the track is in the project");
}

void batchIsOneUndoStep() {
    section("a batch is ONE undo step, and reverts as one");
    adi::test::TempDirectory temp("ui_seams", "batch");
    auto store = freshProject(temp.path() / "p.adi");
    if (!store) { check(false, "store created"); return; }

    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    std::vector<OpRequest> batch{trackCreate(1, "Drums"), trackCreate(2, "Bass")};
    batch[0].selBefore = Payload{{"tracks", Payload::array()}};
    batch[1].selBefore = Payload{{"tracks", Payload::array({1})}};

    const auto r = sub.submit(std::move(batch));
    check(r.ok, "batch committed");
    check(r.seqs.size() == 2, "two ops");
    check(countTracks(*store) == 2, "both tracks exist");

    check(sub.canUndo(), "there is something to undo");
    const auto step = sub.nextUndo();
    check(step.has_value(), "and the history can describe it");
    check(step && step->opCount == 2, "ONE undo step covering both ops");
    check(step && step->txnId == r.txnId, "and it is the transaction we committed");

    const auto undone = sub.undo();
    check(undone.ok, "undo applied");
    check(countTracks(*store) == 0, "both tracks went together");

    const auto redone = sub.redo();
    check(redone.ok, "redo applied");
    check(countTracks(*store) == 2, "and both came back together");
}

// The submitter relies on this and deliberately does not re-implement it: a
// planted defect showed that stripping selBefore in the submitter changed
// nothing, because ops.cpp writes the column only for i == 0. What is checked
// here is therefore the JOURNAL's guarantee, which is the contract the
// submitter leans on -- not the submitter's own behaviour.
void selBeforeOnlyOnTheFirst() {
    section("ADR-0021: the journal records sel_before on the FIRST op only");
    adi::test::TempDirectory temp("ui_seams", "sel");
    auto store = freshProject(temp.path() / "p.adi");
    if (!store) { check(false, "store created"); return; }

    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    std::vector<OpRequest> batch{trackCreate(1, "A"), trackCreate(2, "B")};
    batch[0].selBefore = Payload{{"tracks", Payload::array()}};
    batch[1].selBefore = Payload{{"tracks", Payload::array({1})}};
    const auto r = sub.submit(std::move(batch));
    check(r.ok, "committed");

    SQLite::Statement st(store->db(),
        "SELECT COUNT(*) FROM ops WHERE txn_id = ? AND sel_before IS NOT NULL");
    st.bind(1, r.txnId);
    const auto withSel = st.executeStep() ? st.getColumn(0).getInt64() : -1;
    check(withSel == 1, "exactly one op in the transaction carries sel_before");
}

void emptyBatchWritesNothing() {
    section("an empty batch is a no-op, not an error and not an undo entry");
    adi::test::TempDirectory temp("ui_seams", "empty");
    auto store = freshProject(temp.path() / "p.adi");
    if (!store) { check(false, "store created"); return; }

    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    const auto r = sub.submit(std::vector<OpRequest>{});
    check(r.ok, "reports success");
    check(r.txnId == 0, "opened no transaction");
    check(r.seqs.empty(), "wrote no ops");

    check(!sub.canUndo(), "and left nothing to undo");
}

void stampsTheActor() {
    section("the submitter says who acted, without a call site repeating it");
    adi::test::TempDirectory temp("ui_seams", "actor");
    auto store = freshProject(temp.path() / "p.adi");
    if (!store) { check(false, "store created"); return; }

    engine::ProjectView view;
    ui::OpSubmitter agent(*store, view, Actor::Agent, "adi-vst");
    check(agent.actor() == Actor::Agent, "the submitter reports its actor");
    const auto r = agent.submit(trackCreate(1, "Agent track"));
    check(r.ok, "committed");

    SQLite::Statement st(store->db(), "SELECT actor, actor_detail FROM ops WHERE txn_id = ?");
    st.bind(1, r.txnId);
    const bool got = st.executeStep();
    check(got, "the op was logged");
    if (got) {
        check(st.getColumn(0).getString() == std::string(toString(Actor::Agent)),
              "logged as the agent, not as the user");
        check(st.getColumn(1).getString() == "adi-vst", "with the detail it was given");
    }
}

void aRefusedOpWritesNothing() {
    section("a refused op reports why, and leaves no trace");
    adi::test::TempDirectory temp("ui_seams", "refuse");
    auto store = freshProject(temp.path() / "p.adi");
    if (!store) { check(false, "store created"); return; }

    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    OpRequest bad;
    bad.opType = "track.create";
    bad.payload = {{"id", 1}};   // no `kind`, which the catalogue requires
    const auto r = sub.submit(std::move(bad));
    check(!r.ok, "refused");
    check(!r.error.empty() || !r.issues.empty(), "and said why");
    check(countTracks(*store) == 0, "no track was written");

    check(!sub.canUndo(), "and no undo entry was left behind");
}


// --- the ADR-0201 refresh contract -------------------------------------------

void everyWriteRefreshesTheView() {
    section("ADR-0201: a successful write rebuilds the view, a refused one does not");
    adi::test::TempDirectory temp("ui_seams", "refresh");
    auto store = freshProject(temp.path() / "p.adi");
    if (!store) { check(false, "store created"); return; }

    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);

    check(view.generation() == 0, "nothing published before the first write");
    check(view.current() == nullptr, "and current() is null");
    check(!ui::SnapshotReader{view.current()}.valid(), "a reader over it is simply invalid");

    const auto a = sub.submit(trackCreate(1, "Drums"));
    check(a.ok, "committed");
    check(view.generation() == 1, "the commit refreshed the view");
    check(a.generation == 1, "and the result reports the generation it produced");
    {
        const ui::SnapshotReader r{view.current()};
        check(r.valid(), "the frame now has a snapshot");
        check(r.trackCount() == 1, "which contains the new track");
        check(r.findTrack(1) != nullptr, "by id");
    }

    // A refused op must not burn a generation: nothing was written, so telling
    // every component to re-read would be a lie about the project moving.
    OpRequest bad;
    bad.opType = "track.create";
    bad.payload = {{"id", 2}};   // no `kind`
    const auto refused = sub.submit(std::move(bad));
    check(!refused.ok, "refused");
    check(refused.generation == 0, "a refused write reports no generation");
    check(view.generation() == 1, "and did NOT refresh the view");

    const auto undone = sub.undo();
    check(undone.ok, "undo applied");
    check(view.generation() == 2, "undo refreshed the view too");
    check(ui::SnapshotReader{view.current()}.trackCount() == 0, "and the track is gone from it");

    const auto redone = sub.redo();
    check(redone.ok, "redo applied");
    check(view.generation() == 3, "redo refreshed the view too");
    check(ui::SnapshotReader{view.current()}.trackCount() == 1, "and the track is back");
}

void aFrameKeepsItsSnapshot() {
    section("ADR-0201: a snapshot a frame holds never changes under it");
    adi::test::TempDirectory temp("ui_seams", "frame");
    auto store = freshProject(temp.path() / "p.adi");
    if (!store) { check(false, "store created"); return; }

    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    check(sub.submit(trackCreate(1, "Drums")).ok, "first track");

    // The frame takes ONE reference and every component shares it (ADR-0050 d3,
    // closed by ADR-0201 d2). A commit lands while the frame is still drawing.
    const ui::SnapshotReader frame{view.current()};
    const ui::SnapshotReader alsoThisFrame{view.current()};
    check(frame.sameAs(alsoThisFrame), "two components in one frame read one snapshot");
    check(frame.trackCount() == 1, "the frame sees one track");

    check(sub.submit(trackCreate(2, "Bass")).ok, "a second track commits mid-frame");
    check(frame.trackCount() == 1, "the frame STILL sees one track");
    check(frame.findTrack(2) == nullptr, "and cannot see the new one");
    check(view.generation() == 2, "while the view has moved on");

    const ui::SnapshotReader nextFrame{view.current()};
    check(nextFrame.trackCount() == 2, "the next frame sees both");
    check(!frame.sameAs(nextFrame), "and is a different publication");
}

}  // namespace

void projectViewTree() {
    section("Bitwig's project view: groups in groups, a view keeps Main (Master Reference 10)");
    // Figure 32's project: Drums holds Clap/Snare/Rimshot and Hats; Body holds Bass, Synths and
    // Fx And Hits, which holds Rises and Hits; Main last.
    const adi::ui::TrackTree tree({{1, 0, true, false, 0x40e0e0, "Drums", 0},
                                   {2, 1, false, false, 0, "Clap/Snare/Rimshot", 0},
                                   {3, 1, false, false, 0, "Hats & Percs", 1},
                                   {4, 0, true, false, 0xe080c0, "Body", 1},
                                   {5, 4, false, false, 0, "Bass", 0},
                                   {6, 4, false, false, 0, "Synths", 1},
                                   {7, 4, true, false, 0x4080ff, "Fx And Hits", 2},
                                   {8, 7, false, false, 0, "Rises", 0},
                                   {9, 7, false, false, 0, "Hits", 1},
                                   {10, 0, false, true, 0, "Main", 0}});
    const auto groups = tree.groups();
    check(groups.size() == 3 && groups[0].id == 1 && groups[0].depth == 0 && groups[1].id == 4 &&
              groups[1].depth == 0 && groups[2].id == 7 && groups[2].depth == 1 &&
              groups[2].row->name == "Fx And Hits",
          "the groups in the arrangement's order, each with how deep it sits");
    check(tree.shows(7) == std::set<std::int64_t>{7, 8, 9, 10}, "a group shows itself, what is in it, and Main");
    check(tree.shows(4) == std::set<std::int64_t>{4, 5, 6, 7, 8, 9, 10}, "and what is in the groups inside it");
    check(tree.shows(0).size() == 10, "0 is the whole project");
    check(tree.isGroup(7) && !tree.isGroup(8) && !tree.isGroup(99), "only a group is a group");
    // A parent that loops back must not hang the window.
    const adi::ui::TrackTree loop({{1, 2, true, false, 0, "a", 0}, {2, 1, true, false, 0, "b", 0}});
    check(loop.groups().size() == 2 && loop.shows(1).count(2) == 1, "a loop in the parents ends the walk");
    // Until the snapshot carries groups (N6), every track stands alone.
    const adi::ui::TrackTree alone({{1, 0, false, false, 0, "Main", 0}, {2, 0, false, false, 0, "Track", 0}});
    check(alone.groups().empty() && alone.shows(0).size() == 2, "without groups, nothing to choose");
}

void projectViewRowsFromTheSnapshot() {
    section("the tracks' tree from the snapshot (#299; the project file until then)");
    adi::test::TempDirectory temp("ui_seams", "rows");
    auto store = freshProject(temp.path() / "p.adi");
    check(store != nullptr, "store created");
    if (!store) return;
    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    auto create = [&sub](std::int64_t id, const char* kind, const char* name, std::int64_t parent) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", kind}, {"name", name}};
        if (parent != 0) r.payload["parent"] = parent;
        r.label = "Create track";
        return sub.submit(std::move(r)).ok;
    };
    check(create(1, "master", "Main", 0) && create(2, "group", "Body", 0) && create(3, "group", "Fx And Hits", 2) &&
              create(4, "audio", "Hits", 3) && create(5, "midi", "Lead", 0),
          "Main, Body holding Fx And Hits holding Hits, and Lead alone");
    const ui::SnapshotReader reader(view.current());
    std::vector<ui::TrackRow> rows;
    ui::rowsFromSnapshot(reader, rows);
    auto row = [&rows](std::int64_t id) -> const ui::TrackRow* {
        for (const auto& r : rows)
            if (r.id == id) return &r;
        return nullptr;
    };
    check(rows.size() == reader.tracks().size() && rows.size() == 5, "a row for every track the snapshot shows");
    bool inOrder = true;
    for (std::size_t i = 0; i < rows.size(); ++i)
        inOrder = inOrder && rows[i].id == reader.tracks()[i]->id;
    check(inOrder, "in the snapshot's order");
    check(row(1) && row(1)->main && !row(1)->group && row(2) && row(2)->group && row(2)->parent == 0 && row(3) &&
              row(3)->group && row(3)->parent == 2 && row(4) && row(4)->parent == 3 && !row(4)->group && row(5) &&
              !row(5)->main && row(5)->parent == 0 && row(4)->name == "Hits",
          "each with what it sits in, and whether it is a group or Main");
    const ui::TrackTree tree(rows);
    check(tree.groups().size() == 2 && tree.shows(3) == std::set<std::int64_t>{1, 3, 4},
          "the tree: Fx And Hits shows itself, Hits and Main");

    // Live's order (UI-ARCHITECTURE §4, manual 18.3): each group, then what it holds; Main last.
    ui::TrackOrder order;
    auto places = [&order] {
        std::vector<std::pair<std::int64_t, std::uint32_t>> out;
        for (const auto& p : order.places()) out.emplace_back(p.id, p.depth);
        return out;
    };
    order.build(reader, rows);
    check(order.fits(reader) && places() == std::vector<std::pair<std::int64_t, std::uint32_t>>{
                                              {2, 0}, {3, 1}, {4, 2}, {5, 0}, {1, 0}},
          "the order: Body, Fx And Hits inside it, Hits inside that, Lead, then Main");
    check(order.places()[2].parent == 3 && order.places()[1].group && order.places()[4].main &&
              reader.tracks()[order.places()[2].at]->id == 4,
          "each place knows its group, whether it is one or Main, and its track in the snapshot");
    // Rows a broken project could give: a loop, a group that is gone, a track inside itself.
    std::vector<ui::TrackRow> broken{{1, 0, false, true, 0, "Main", 0}, {2, 3, true, false, 0, "a", 0},
                                     {3, 2, true, false, 0, "b", 0},    {4, 99, false, false, 0, "c", 0},
                                     {5, 5, false, false, 0, "d", 0}};
    order.build(reader, broken);
    check(places() == std::vector<std::pair<std::int64_t, std::uint32_t>>{{4, 0}, {5, 0}, {1, 0}, {2, 0}, {3, 0}},
          "a missing group or a track in itself at the top; a loop after the rest, each once");
    // Without rows, the snapshot's own order.
    order.build(reader, {});
    bool same = order.places().size() == reader.tracks().size();
    for (std::size_t i = 0; same && i < reader.tracks().size(); ++i)
        same = order.places()[i].id == reader.tracks()[i]->id;
    check(same, "without rows, the snapshot's order");
    const ui::SnapshotReader other(nullptr);
    check(!order.fits(other), "an order fits only the snapshot it was built for");

    // Refilled in place: the same tracks again change nothing and keep the names the snapshot gives.
    auto again = rows;
    for (auto& r : again) r.name = "old";
    ui::rowsFromSnapshot(reader, again);
    bool sameRows = again.size() == rows.size();
    for (std::size_t i = 0; sameRows && i < rows.size(); ++i)
        sameRows = again[i].id == rows[i].id && again[i].parent == rows[i].parent && again[i].name == rows[i].name &&
                   again[i].group == rows[i].group && again[i].main == rows[i].main;
    check(sameRows, "refilled from the same snapshot, the rows are the same, names included");
}

void metersCountBars() {
    section("bars, beats and sixteenths in the project's meters (#299; Live 12 manual 6.1)");
    using textproj::Meter;
    constexpr std::int64_t q = textproj::kPPQ;
    // 4/4 from zero, 3/4 from bar 3 (eight quarters), then 6/8 a quarter into a bar of 3/4.
    const std::vector<Meter> meters{{0, 4, 4}, {8 * q, 3, 4}, {15 * q, 6, 8}};
    check(ui::barPosition(0, meters) == ui::BarPosition{1, 1, 1} &&
              ui::barPosition(9 * q + q / 4, meters) == ui::BarPosition{3, 2, 2},
          "4/4 then 3/4: bar 3 begins at the change, its second beat a sixteenth in");
    // Bar 4 of 3/4 is 11 to 14 quarters; the change at 15 cuts bar 5 after one quarter, and that
    // short bar still counts, so 6/8 begins bar 6. A beat of 6/8 is an eighth: two sixteenths.
    check(ui::barPosition(15 * q, meters) == ui::BarPosition{6, 1, 1} &&
              ui::barPosition(15 * q + q / 2 + q / 4, meters) == ui::BarPosition{6, 2, 2},
          "a change part-way through a bar starts a new one; a beat of 6/8 holds two sixteenths");
    bool agrees = true;
    for (std::int64_t t = 0; t < 24 * q; t += q / 4) {
        const auto at = ui::barPosition(t, meters);
        const auto text = textproj::renderPosition(t, meters);
        agrees = agrees && text.rfind(std::to_string(at.bar) + "|" + std::to_string(at.beat) + "|", 0) == 0;
    }
    check(agrees, "every sixteenth agrees with the text projection's bar and beat");
    check(ui::barPosition(5 * q, {}) == ui::BarPosition{2, 2, 1} && ui::meterAt(5 * q, {}).numerator == 4,
          "without meters, 4/4 throughout");
    check(ui::meterAt(10 * q, meters).numerator == 3 && ui::meterAt(20 * q, meters).denominator == 8,
          "the signature in force at a tick");
    check(ui::barSpan(8 * q, 4 * q + q / 4, meters) == ui::BarPosition{1, 1, 1} &&
              ui::barSpan(0, 6 * q, meters) == ui::BarPosition{1, 2, 0},
          "a length in the start's signature: 3/4 makes four quarters and a sixteenth 1.1.1");
    std::vector<std::pair<std::int64_t, std::int64_t>> bars;
    ui::forEachBar(0, 18 * q, meters, [&bars](std::int64_t tick, std::int64_t bar) { bars.emplace_back(tick, bar); });
    const std::vector<std::pair<std::int64_t, std::int64_t>> expected{
        {0, 1}, {4 * q, 2}, {8 * q, 3}, {11 * q, 4}, {14 * q, 5}, {15 * q, 6}, {18 * q, 7}};
    check(bars == expected, "every bar line, numbered, across both changes");
    bars.clear();
    ui::forEachBar(10 * q, 16 * q, meters, [&bars](std::int64_t tick, std::int64_t bar) { bars.emplace_back(tick, bar); });
    check((bars == std::vector<std::pair<std::int64_t, std::int64_t>>{{11 * q, 4}, {14 * q, 5}, {15 * q, 6}}),
          "only the bars in the range, still numbered from the start");
}

void groupAndUngroupAsBatches() {
    section("Group Tracks and Ungroup Tracks: one batch each, from the snapshot (Live 12 manual 18.3)");
    adi::test::TempDirectory temp("ui_seams", "groups");
    auto store = freshProject(temp.path() / "p.adi");
    check(store != nullptr, "store created");
    if (!store) return;
    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    // Every index 0, as a file from before index_in_parent was kept: the order falls to the ids.
    bool made = true;
    for (const auto& [id, kind, name] : {std::tuple{1, "master", "Main"}, std::tuple{2, "audio", "Kick"},
                                         std::tuple{3, "audio", "Snare"}, std::tuple{4, "midi", "Bass"}}) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", kind}, {"name", name}};
        r.label = "Create track";
        made = sub.submit(std::move(r)).ok && made;
    }
    check(made, "Main, Kick, Snare and Bass, all at index 0");
    auto order = [&view] {
        const ui::SnapshotReader reader(view.current());
        std::vector<ui::TrackRow> rows;
        ui::rowsFromSnapshot(reader, rows);
        ui::TrackOrder built;
        built.build(reader, rows);
        std::vector<std::pair<std::int64_t, std::uint32_t>> out;
        for (const auto& p : built.places()) out.emplace_back(p.id, p.depth);
        return out;
    };
    using Order = std::vector<std::pair<std::int64_t, std::uint32_t>>;
    const Order flat{{2, 0}, {3, 0}, {4, 0}, {1, 0}};
    check(order() == flat, "Kick, Snare, Bass, then Main");

    auto edit = ui::groupTrack(view.current()->tracks, 3);
    check(edit.refusal.empty() && edit.select == 5 && edit.ops.size() == 3 && edit.ops[0].opType == "track.create" &&
              edit.ops[0].payload["index"] == 1 && edit.ops[0].payload["parent"].is_null() &&
              edit.ops[1].opType == "track.setParent" && edit.ops[2].opType == "track.reorder" &&
              edit.ops[2].payload["id"] == 4 && edit.ops[2].payload["index"] == 2,
          "Snare grouped: the group at Snare's place, Snare into it, Bass's index put after it; Main untouched");
    const auto groupOps = static_cast<int>(edit.ops.size());
    check(sub.submit(std::move(edit.ops)).ok && sub.nextUndo() && sub.nextUndo()->opCount == groupOps &&
              sub.nextUndo()->label == "Group Tracks",
          "one undo step, named as Live's Edit menu names it");
    check(order() == Order{{2, 0}, {5, 0}, {3, 1}, {4, 0}, {1, 0}}, "Kick, the group holding Snare, Bass, Main");
    check(sub.undo().ok && order() == flat, "undo puts it back");
    check(sub.redo().ok, "redo");

    auto undo = ui::ungroupTrack(view.current()->tracks, 5);
    check(undo.refusal.empty() && undo.select == 3 && undo.ops.back().opType == "track.delete",
          "Ungroup: Snare out, then the group removed, last");
    const auto ungroupOps = static_cast<int>(undo.ops.size());
    check(sub.submit(std::move(undo.ops)).ok && order() == flat && sub.nextUndo()->opCount == ungroupOps &&
              sub.nextUndo()->label == "Ungroup Tracks",
          "Snare back where the group stood; one more step");
    check(sub.undo().ok && order() == Order{{2, 0}, {5, 0}, {3, 1}, {4, 0}, {1, 0}}, "undo brings the group back");

    // A group inside a group: Ungroup puts its tracks in the outer one, where it stood.
    auto nested = ui::groupTrack(view.current()->tracks, 5);
    check(nested.select == 6 && sub.submit(std::move(nested.ops)).ok &&
              order() == Order{{2, 0}, {6, 0}, {5, 1}, {3, 2}, {4, 0}, {1, 0}},
          "the group grouped: groups nest");
    auto inner = ui::ungroupTrack(view.current()->tracks, 5);
    check(sub.submit(std::move(inner.ops)).ok && order() == Order{{2, 0}, {6, 0}, {3, 1}, {4, 0}, {1, 0}},
          "the inner group ungrouped: Snare in the outer one");

    check(ui::groupTrack(view.current()->tracks, 1).refusal == "Main and return tracks can't be grouped." &&
              ui::groupTrack(view.current()->tracks, 99).refusal == "Select a track to group." &&
              ui::ungroupTrack(view.current()->tracks, 2).refusal == "Select a group track to ungroup.",
          "Main, a missing track and a track that is not a group are refused, with the reason");
    check(ui::canGroup(view.current()->tracks, 4) && !ui::canGroup(view.current()->tracks, 1) &&
              ui::canUngroup(view.current()->tracks, 6) && !ui::canUngroup(view.current()->tracks, 4),
          "what the menu offers follows the same rules");

    // Live's Group Tracks on several selected tracks (manual 18.3): Kick and the group after it into
    // one new group, in their order; Snare, selected with its group, goes with it.
    auto several = ui::groupTracks(view.current()->tracks, {2, 6, 3});
    check(several.refusal.empty() && several.select == 7 && sub.submit(std::move(several.ops)).ok &&
              order() == Order{{7, 0}, {2, 1}, {6, 1}, {3, 2}, {4, 0}, {1, 0}} && sub.nextUndo()->label == "Group Tracks",
          "Kick and the group holding Snare grouped together, one step; Snare goes with its group");
    check(ui::groupTracks(view.current()->tracks, {3, 4}).refusal == "Select tracks in one group to group them together.",
          "tracks in different groups are refused, with the reason");
    check(sub.undo().ok && order() == Order{{2, 0}, {6, 0}, {3, 1}, {4, 0}, {1, 0}}, "undo puts them back");

    // Live's track drag (the Arrangement View: "dragging them above or below other tracks"; 18.3:
    // "dragged into or out of the group"): one batch, one step.
    const Order before{{2, 0}, {6, 0}, {3, 1}, {4, 0}, {1, 0}};
    auto moved = ui::moveTracks(view.current()->tracks, {4}, {0, 2});
    check(moved.refusal.empty() && sub.submit(std::move(moved.ops)).ok &&
              order() == Order{{4, 0}, {2, 0}, {6, 0}, {3, 1}, {1, 0}} && sub.nextUndo()->label == "Move Track",
          "Bass dragged above Kick: first, one step named Move Track; Main stays last");
    check(sub.undo().ok && order() == before, "undo puts Bass back");
    auto into = ui::moveTracks(view.current()->tracks, {2}, {6, 0});
    check(into.refusal.empty() && into.ops.front().opType == "track.setParent" && into.ops.front().payload["parent"] == 6 &&
              sub.submit(std::move(into.ops)).ok && order() == Order{{6, 0}, {3, 1}, {2, 1}, {4, 0}, {1, 0}},
          "Kick dragged into the group: after Snare in it, routed by track.setParent");
    check(sub.undo().ok && order() == before, "undo takes Kick out again");
    auto out = ui::moveTracks(view.current()->tracks, {3}, {0, 0});
    check(out.refusal.empty() && out.ops.front().payload["parent"].is_null() && sub.submit(std::move(out.ops)).ok &&
              order() == Order{{2, 0}, {6, 0}, {4, 0}, {3, 0}, {1, 0}},
          "Snare dragged out of the group below the last track: at the top level, before Main");
    check(sub.undo().ok && order() == before, "undo puts Snare back in its group");
    auto pair = ui::moveTracks(view.current()->tracks, {4, 2}, {0, 6});
    check(pair.refusal.empty() && sub.submit(std::move(pair.ops)).ok &&
              order() == Order{{2, 0}, {4, 0}, {6, 0}, {3, 1}, {1, 0}} && sub.nextUndo()->label == "Move Tracks",
          "Kick and Bass selected and dragged above the group: side by side, in their order, one step");
    check(sub.undo().ok && order() == before, "undo");
    auto whole = ui::moveTracks(view.current()->tracks, {3, 6}, {0, 0});
    check(whole.refusal.empty() && sub.submit(std::move(whole.ops)).ok &&
              order() == Order{{2, 0}, {4, 0}, {6, 0}, {3, 1}, {1, 0}},
          "the group dragged below Bass, Snare selected with it: Snare goes with its group");
    check(sub.undo().ok && order() == before, "undo");
    check(ui::moveTracks(view.current()->tracks, {2}, {0, 2}).ops.empty() &&
              ui::moveTracks(view.current()->tracks, {2}, {0, 6}).ops.empty() &&
              ui::moveTracks(view.current()->tracks, {3}, {6, 0}).ops.empty(),
          "dropped where they stand, nothing moves and no step is made");
    // Live's Insert Track (manual 18.2): right after the selected track, in its group; the tracks
    // after it move down one, in the same step.
    auto inserted = ui::insertTrack(view.current()->tracks, 2, false);
    check(inserted.refusal.empty() && inserted.select == 7 && inserted.ops.front().payload["index"] == 1 &&
              inserted.ops.front().payload["parent"].is_null() && sub.submit(std::move(inserted.ops)).ok &&
              order() == Order{{2, 0}, {7, 0}, {6, 0}, {3, 1}, {4, 0}, {1, 0}} &&
              sub.nextUndo()->label == "Insert Audio Track",
          "an audio track inserted with Kick selected: right after Kick, the group and Bass after it, one step");
    check(sub.undo().ok && order() == before, "undo removes it");
    auto inGroup = ui::insertTrack(view.current()->tracks, 3, true);
    check(inGroup.ops.front().payload["parent"] == 6 && sub.submit(std::move(inGroup.ops)).ok &&
              order() == Order{{2, 0}, {6, 0}, {3, 1}, {7, 1}, {4, 0}, {1, 0}} &&
              view.current()->findTrack(7)->kind == "midi" && sub.nextUndo()->label == "Insert MIDI Track",
          "a MIDI track inserted with Snare selected: in Snare's group, after it");
    check(sub.undo().ok, "undo");
    for (const std::int64_t selected : {std::int64_t{1}, std::int64_t{0}}) {
        auto last = ui::insertTrack(view.current()->tracks, selected, false);
        check(sub.submit(std::move(last.ops)).ok && order() == Order{{2, 0}, {6, 0}, {3, 1}, {4, 0}, {7, 0}, {1, 0}},
              selected == 1 ? "with Main selected, after the last track, before Main"
                            : "with nothing selected, after the last track");
        check(sub.undo().ok && order() == before, "undo");
    }
    check(ui::moveTracks(view.current()->tracks, {6}, {6, 0}).refusal == "A group can't go inside itself." &&
              ui::moveTracks(view.current()->tracks, {1}, {0, 2}).refusal == "Main stays last." &&
              ui::moveTracks(view.current()->tracks, {2}, {4, 0}).refusal == "Tracks go into a group track only." &&
              ui::moveTracks(view.current()->tracks, {}, {0, 0}).refusal == "Select a track to move.",
          "a group into itself, Main, a track that is not a group and nothing are refused, with the reason");

    // The returns are reordered among themselves, at the foot (Live drags them as it drags tracks).
    bool returnsMade = true;
    for (const auto& [id, name] : {std::pair{20, "A Return"}, std::pair{21, "B Return"}}) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", "return"}, {"name", name}, {"index", id}};
        r.label = "Insert Return Track";
        returnsMade = sub.submit(std::move(r)).ok && returnsMade;
    }
    const Order withReturns{{2, 0}, {6, 0}, {3, 1}, {4, 0}, {20, 0}, {21, 0}, {1, 0}};
    check(returnsMade && order() == withReturns, "two returns, after the tracks, before Main");
    auto swapped = ui::moveTracks(view.current()->tracks, {21}, {0, 20});
    check(swapped.refusal.empty() && sub.submit(std::move(swapped.ops)).ok &&
              order() == Order{{2, 0}, {6, 0}, {3, 1}, {4, 0}, {21, 0}, {20, 0}, {1, 0}} && sub.nextUndo()->label == "Move Track",
          "B dragged above A: first of the returns, one step; the tracks and Main stay");
    check(sub.undo().ok && order() == withReturns, "undo");
    auto last = ui::moveTracks(view.current()->tracks, {20}, {0, 0});
    check(sub.submit(std::move(last.ops)).ok && order() == Order{{2, 0}, {6, 0}, {3, 1}, {4, 0}, {21, 0}, {20, 0}, {1, 0}},
          "A dragged below the last return: after B, before Main");
    check(sub.undo().ok && ui::moveTracks(view.current()->tracks, {21}, {0, 0}).ops.empty(),
          "B dropped where it stands: no step");
    check(ui::moveTracks(view.current()->tracks, {20, 2}, {0, 0}).refusal == "Return tracks move among the returns only." &&
              ui::moveTracks(view.current()->tracks, {20}, {6, 0}).refusal == "Return tracks move among the returns only.",
          "a return with a track, or into a group, is refused, with the reason");
    check(sub.undo().ok && sub.undo().ok && order() == before, "the returns undone");

    // A group holding a soloed track (manual 18.3: its Solo shows half coloured).
    OpRequest solo;
    solo.opType = "track.setSolo";
    solo.payload = {{"id", 3}, {"soloed", true}};
    solo.label = "Solo Track";
    check(sub.submit(std::move(solo)).ok && ui::holdsSoloed(view.current()->tracks, 6) &&
              !ui::holdsSoloed(view.current()->tracks, 3) && !ui::holdsSoloed(view.current()->tracks, 4),
          "the group holding soloed Snare holds a soloed track; Snare and Bass hold none");
    check(sub.undo().ok && !ui::holdsSoloed(view.current()->tracks, 6), "unsoloed, the group holds none");

    // Live's colour chooser (manual 2.3.2): one op per track whose colour differs; what a group holds.
    auto coloured = ui::colourTracks(view.current()->tracks, {2, 6}, 5, "Recolour Track");
    check(coloured.refusal.empty() && coloured.ops.size() == 2 && coloured.ops[0].opType == "track.setColor" &&
              coloured.ops[0].payload["color"] == 5 && coloured.ops[1].payload["id"] == 6,
          "Kick and the group coloured 5: one op each");
    check(sub.submit(std::move(coloured.ops)).ok &&
              ui::colourTracks(view.current()->tracks, {2, 6}, 5, "Recolour Track").ops.empty(),
          "coloured again with the same colour, nothing changes");
    check(ui::colourTracks(view.current()->tracks, {}, 5, "Recolour Track").refusal == "Select a track to colour.",
          "with nothing selected, the reason");
    check(ui::heldBy(view.current()->tracks, 6) == std::vector<std::int64_t>{3} && ui::heldBy(view.current()->tracks, 2).empty(),
          "the group holds Snare; Kick holds nothing");
    check(sub.undo().ok, "undo");
}

void sendsAsBatches() {
    section("Sends: one knob per return, a new send's row and level as one batch (ADR-0203 d3)");
    adi::test::TempDirectory temp("ui_seams", "sends");
    auto store = freshProject(temp.path() / "p.adi");
    check(store != nullptr, "store created");
    if (!store) return;
    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    bool made = true;
    // B Delay made first at index 1, A Reverb at index 0: the arrangement's order is A, then B.
    for (const auto& [id, kind, name, index] :
         {std::tuple{1, "master", "Main", 0}, std::tuple{2, "audio", "Kick", 0}, std::tuple{3, "group", "Drums", 1},
          std::tuple{11, "return", "B Delay", 1}, std::tuple{10, "return", "A Reverb", 0}}) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", kind}, {"name", name}, {"index", index}};
        r.label = "Create track";
        made = sub.submit(std::move(r)).ok && made;
    }
    check(made, "Main, Kick, a group and two returns");
    const auto& tracks = view.current()->tracks;
    std::vector<std::int64_t> returns;
    ui::returnsInOrder(tracks, returns);
    check(returns == std::vector<std::int64_t>{10, 11}, "the returns in the arrangement's order: A, then B");
    check(!ui::hasSends(*view.current()->findTrack(1)) && ui::hasSends(*view.current()->findTrack(3)) &&
              ui::hasSends(*view.current()->findTrack(10)),
          "every track has sends but Main; groups and returns too");
    const auto* kick = view.current()->findTrack(2);
    const auto* reverb = view.current()->findTrack(10);
    check(ui::findSend(view.current()->sends, 2, 10) == nullptr && ui::sendLevel(nullptr) == ui::kSendSilenceDb &&
              ui::sendOn(*kick, nullptr) && !ui::sendOn(*reverb, nullptr),
          "without a row: fully down; a track's send on, a return's off until Enable Send");

    // Turning Kick's send A up writes the row, then its level: one undo step.
    auto edit = ui::setSend(view.current()->sends, 2, 10, -12);
    check(edit.refusal.empty() && edit.ops.size() == 2 && edit.ops[0].opType == "routing.connect" &&
              edit.ops[0].payload["kind"] == "send" && edit.ops[0].payload["src"] == 2 &&
              edit.ops[0].payload["dst"] == 10 && edit.ops[1].opType == "routing.setGain" &&
              edit.ops[1].payload["id"] == edit.ops[0].payload["id"] && edit.ops[1].payload["db"] == -12.0,
          "a new send: the row, then its level");
    check(sub.submit(std::move(edit.ops)).ok && sub.nextUndo() && sub.nextUndo()->opCount == 2 &&
              sub.nextUndo()->label == "Change Send Level",
          "one undo step, named");
    const auto* send = ui::findSend(view.current()->sends, 2, 10);
    check(send != nullptr && send->gainDb == -12.0 && send->enabled && !send->preFader &&
              ui::sendOn(*view.current()->findTrack(2), send),
          "the snapshot holds Kick's send A at -12 dB, on, post fader");
    const auto first = send != nullptr ? send->id : 0;

    // A row that exists: its level only. The bottom stop writes silence; the top stays at 0 dB.
    auto again = ui::setSend(view.current()->sends, 2, 10, ui::kSendFloorDb);
    check(again.ops.size() == 1 && again.ops[0].opType == "routing.setGain" && again.ops[0].payload["id"] == first &&
              again.ops[0].payload["db"] == ui::kSendSilenceDb,
          "fully down writes silence (-150 dB), on the same row");
    check(sub.submit(std::move(again.ops)).ok && ui::findSend(view.current()->sends, 2, 10)->gainDb == ui::kSendSilenceDb,
          "and lands");
    check(ui::setSend(view.current()->sends, 2, 10, -90).ops.empty() &&
              ui::setSend(view.current()->sends, 3, 10, ui::kSendFloorDb).ops.empty(),
          "a level that would not change writes nothing: no empty undo step, no row left fully down");
    auto top = ui::setSend(view.current()->sends, 2, 10, 6);
    check(top.ops.size() == 1 && top.ops[0].payload["db"] == ui::kSendTopDb, "past the top stays at 0 dB");
    check(sub.undo().ok && ui::findSend(view.current()->sends, 2, 10)->gainDb == -12.0, "undo brings -12 dB back");
    check(sub.undo().ok && ui::findSend(view.current()->sends, 2, 10) == nullptr, "and then removes the row");
    check(sub.redo().ok && ui::findSend(view.current()->sends, 2, 10) != nullptr, "redo");

    // A second send gets the next id.
    auto second = ui::setSend(view.current()->sends, 3, 11, -6);
    check(second.ops.size() == 2 && second.ops[0].payload["id"] == first + 1 && sub.submit(std::move(second.ops)).ok &&
              ui::findSend(view.current()->sends, 3, 11) != nullptr &&
              ui::findSend(view.current()->sends, 3, 11)->gainDb == -6.0,
          "the group's send B: a row with the next id");

    // A return's send: off until Enable Send, which makes the row fully down; Enable Send again
    // (unticked) switches it off; a return may send to itself.
    auto enable = ui::enableSend(view.current()->sends, 10, 11, true);
    check(enable.refusal.empty() && enable.ops.size() == 2 && enable.ops[0].opType == "routing.connect" &&
              enable.ops[1].payload["db"] == ui::kSendSilenceDb && sub.submit(std::move(enable.ops)).ok &&
              sub.nextUndo()->label == "Enable Send",
          "Enable Send on A's send B: the row, fully down, one step");
    const auto* ab = ui::findSend(view.current()->sends, 10, 11);
    check(ab != nullptr && ab->enabled && ui::sendOn(*view.current()->findTrack(10), ab), "A's send B is on");
    check(ui::enableSend(view.current()->sends, 10, 11, true).refusal == "The send is on already." &&
              ui::enableSend(view.current()->sends, 10, 10, false).refusal == "The send is off already.",
          "nothing to do is said, not written");
    auto off = ui::enableSend(view.current()->sends, 10, 11, false);
    check(off.ops.size() == 1 && off.ops[0].opType == "routing.setEnabled" && sub.submit(std::move(off.ops)).ok &&
              !ui::findSend(view.current()->sends, 10, 11)->enabled &&
              !ui::sendOn(*view.current()->findTrack(10), ui::findSend(view.current()->sends, 10, 11)),
          "unticked: the row switched off, kept");
    auto on = ui::enableSend(view.current()->sends, 10, 11, true);
    check(on.ops.size() == 1 && on.ops[0].opType == "routing.setEnabled" && sub.submit(std::move(on.ops)).ok &&
              ui::findSend(view.current()->sends, 10, 11)->enabled,
          "ticked again: the same row back on");
    auto loop = ui::enableSend(view.current()->sends, 10, 10, true);
    check(sub.submit(std::move(loop.ops)).ok && ui::findSend(view.current()->sends, 10, 10) != nullptr,
          "A may send to itself (Live's feedback loop)");

    // A return's Pre/Post (ADR-0203 d3): one switch on the return, every send into it moved.
    check(!ui::returnPre(view.current()->sends, 10) && !ui::returnPre(view.current()->sends, 12),
          "a return reads Post; one no send reaches reads Post too");
    auto pre = ui::setReturnPre(view.current()->sends, 10, true);
    check(pre.refusal.empty() && pre.ops.size() == 2 && pre.ops[0].opType == "routing.setPreFader" &&
              pre.ops[0].payload["pre"] == true && sub.submit(std::move(pre.ops)).ok &&
              sub.nextUndo()->label == "Change Pre/Post" && sub.nextUndo()->opCount == 2 &&
              ui::returnPre(view.current()->sends, 10) && ui::findSend(view.current()->sends, 2, 10)->preFader,
          "Pre moves both sends into A before the fader, one undo step");
    auto newPre = ui::setSend(view.current()->sends, 3, 10, -9);
    check(newPre.ops.size() == 3 && newPre.ops[1].opType == "routing.setPreFader" && sub.submit(std::move(newPre.ops)).ok &&
              ui::findSend(view.current()->sends, 3, 10)->preFader && ui::returnPre(view.current()->sends, 10),
          "a new send into a Pre return taps before the fader too");
    check(ui::setReturnPre(view.current()->sends, 12, true).refusal ==
              "No track sends to this return yet, so there is nothing to switch.",
          "a return no send reaches has nothing to switch, and says so");
    check(sub.undo().ok && sub.undo().ok && !ui::returnPre(view.current()->sends, 10), "undo puts A back to Post");

    // Enable All Sends on B Delay (Live 12 §18.4): a row to A and to itself, each with its own id, fully
    // down, one step; again, nothing to do.
    const std::vector<std::int64_t> both{10, 11};
    auto all = ui::enableAllSends(view.current()->sends, 11, both);
    check(all.refusal.empty() && all.ops.size() == 4 && all.ops[0].opType == "routing.connect" &&
              all.ops[2].opType == "routing.connect" && all.ops[2].payload["id"] != all.ops[0].payload["id"] &&
              sub.submit(std::move(all.ops)).ok && sub.nextUndo()->label == "Enable All Sends" &&
              ui::findSend(view.current()->sends, 11, 10) != nullptr && ui::findSend(view.current()->sends, 11, 11) != nullptr,
          "Enable All Sends: B to A and B to itself, two new rows, one undo step");
    check(ui::enableAllSends(view.current()->sends, 11, both).refusal == "Every send of this return is on already.",
          "again: every send is on already");
}

void audioDropAsOneStep() {
    section("An audio file dropped on the arrangement: the pool row, the clip and its audio, one step (#308)");
    adi::test::TempDirectory temp("ui_seams", "drop");
    auto store = freshProject(temp.path() / "p.adi");
    check(store != nullptr, "store created");
    if (!store) return;
    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    bool made = true;
    for (const auto& [id, kind, name] : {std::tuple{1, "audio", "Drums"}, std::tuple{2, "midi", "Bass"},
                                         std::tuple{3, "return", "A Reverb"}, std::tuple{4, "master", "Main"}}) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", kind}, {"name", name}};
        r.label = "Create track";
        made = sub.submit(std::move(r)).ok && made;
    }
    check(made, "an audio track, a MIDI track, a return and Main");
    const auto& snap = *view.current();
    check(ui::audioDropRefusal(snap, 1).empty() && ui::audioDropRefusal(snap, 0).empty() &&
              !ui::audioDropRefusal(snap, 2).empty() && !ui::audioDropRefusal(snap, 3).empty() &&
              !ui::audioDropRefusal(snap, 4).empty() && !ui::audioDropRefusal(snap, 99).empty(),
          "an audio track or the drop area takes the file; MIDI, a return, Main or no track refuses it");
    // Half a second at 44.1 kHz: a beat at the default 120 BPM.
    const auto wav = temp.path() / "snare.wav";
    {
        std::vector<float> samples(22050, .25f);
        audio::WavWriter writer(wav, 44100, 1, audio::WavFormat::Pcm16);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    }
    auto capture = [&store, &wav](std::int64_t id) {
        ui::AudioImport import;
        OpRequest captured;
        const auto r = media::prepareImport(*store, wav, id, 1, captured);
        if (r.ok && r.audio) {
            import.mediaId = r.mediaId;
            import.sampleRate = r.audio->sampleRate;
            import.frames = r.audio->frames;
            import.media = std::move(captured);
        }
        return import;
    };
    const auto bar2 = textproj::kWhole;
    auto edit = ui::dropAudio(*view.current(), 1, bar2, "snare", capture(1));
    check(edit.refusal.empty() && edit.ops.size() == 3 && edit.ops[0].opType == "media.import" &&
              edit.ops[1].opType == "clip.create" && edit.ops[1].payload["kind"] == "audio" &&
              edit.ops[1].payload["pos"] == bar2 && edit.ops[2].opType == "clip.attachAudio" &&
              edit.ops[2].payload["frames"] == 22050 && edit.select == 1 && edit.clip == 1,
          "on Drums at bar 2: the pool row, the clip, its audio");
    const auto length = edit.ops[1].payload["length"].get<std::int64_t>();
    check(std::abs(length - textproj::kPPQ) <= 1, "half a second at 120 BPM is one beat long");
    check(sub.submit(std::move(edit.ops)).ok && sub.nextUndo()->opCount == 3 &&
              sub.nextUndo()->label == "Insert Audio Clip",
          "one undo step, named");
    const auto* drums = view.current()->findTrack(1);
    check(drums != nullptr && drums->clips.size() == 1 && drums->clips[0]->name == "snare" &&
              drums->clips[0]->posTicks == bar2,
          "the snapshot has the clip on Drums at bar 2");

    // The same file again, below the tracks: the pool row is reused, and a new audio track holds it.
    auto again = ui::dropAudio(*view.current(), 0, 0, "snare", capture(2));
    check(again.refusal.empty() && again.ops.size() == 3 && again.ops[0].opType == "track.create" &&
              again.ops[0].payload["kind"] == "audio" && again.ops[0].payload["id"] == 5 &&
              again.ops[1].payload["track"] == 5 && again.ops[2].payload["media"] == 1 && again.select == 5 &&
              again.clip == 2,
          "the drop area: a new audio track, the pool's row reused");
    check(sub.submit(std::move(again.ops)).ok && view.current()->findTrack(5) != nullptr &&
              view.current()->findTrack(5)->clips.size() == 1,
          "and lands");
    check(sub.undo().ok && view.current()->findTrack(5) == nullptr, "undo takes the new track away with its clip");
    check(sub.undo().ok && view.current()->findTrack(1)->clips.empty(), "and the first drop");
    check(ui::dropAudio(*view.current(), 1, 0, "x", ui::AudioImport{}).refusal == "That file holds no audio ADI can play." &&
              !ui::dropAudio(*view.current(), 2, 0, "x", capture(3)).refusal.empty(),
          "no audio, or a MIDI track, is refused with the reason");
}

void rangeEditsAsBatches() {
    section("A time selection across tracks: Delete and 0 on its whole clips, one step each (Live 12 manual 6.9)");
    adi::test::TempDirectory temp("ui_seams", "range");
    auto store = freshProject(temp.path() / "p.adi");
    check(store != nullptr, "store created");
    if (!store) return;
    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    const auto bar = textproj::kWhole;
    bool made = true;
    for (std::int64_t id = 1; id <= 3; ++id) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", "midi"}, {"name", "Track " + std::to_string(id)}, {"index", id - 1}};
        r.label = "Create track";
        made = sub.submit(std::move(r)).ok && made;
    }
    // Track 1: bar 1; Track 2: bar 2; Track 3: from bar 2.5 to 3.5, across the selection's end.
    for (const auto& [id, track, pos, length] :
         {std::tuple<std::int64_t, std::int64_t, std::int64_t, std::int64_t>{1, 1, 0, bar},
          {2, 2, bar, bar}, {3, 3, bar + bar / 2, bar}}) {
        OpRequest r;
        r.opType = "clip.create";
        r.payload = {{"id", id}, {"track", track}, {"kind", "midi"}, {"pos", pos}, {"length", length}};
        r.label = "Create clip";
        made = sub.submit(std::move(r)).ok && made;
    }
    check(made, "three tracks, a clip on each");

    // The tracks a selection covers: from its track to its far track, either way round.
    const ui::SnapshotReader reader(view.current());
    ui::ArrangementGeometry geo;
    geo.selection.track = 1;
    check(geo.selectedTracks(reader) == std::vector<std::int64_t>{1} && !geo.inSelection(reader, 2),
          "without a far track, the selection is its one track");
    geo.selection.toTrack = 3;
    check(geo.selectedTracks(reader) == std::vector<std::int64_t>{1, 2, 3} && geo.inSelection(reader, 2),
          "with one, every track between");
    geo.selection.track = 3;
    geo.selection.toTrack = 1;
    check(geo.selectedTracks(reader) == std::vector<std::int64_t>{1, 2, 3}, "dragged upwards too");

    const std::vector<std::int64_t> all{1, 2, 3};
    const auto content = ui::rangeContent(view.current()->tracks, all, 0, 2 * bar);
    check(content.inside.size() == 2 && content.crossing == 1, "bars 1 and 2: two clips inside, one across the end");
    auto off = ui::deactivateRange(view.current()->tracks, all, 0, 2 * bar);
    check(off.refusal.empty() && off.ops.size() == 2 && off.ops[0].opType == "clip.setMute" &&
              off.ops[0].payload["muted"] == true,
          "0 deactivates the two clips inside");
    const auto note = ui::rangeNote(view.current()->tracks, all, 0, 2 * bar);
    check(note.find("1 clip crosses") != std::string::npos && note.find("split") != std::string::npos,
          "and says the one across the edge waits for Split");
    check(sub.submit(std::move(off.ops)).ok && sub.nextUndo()->opCount == 2 &&
              sub.nextUndo()->label == "Deactivate Clips",
          "one undo step, named");
    auto on = ui::deactivateRange(view.current()->tracks, all, 0, 2 * bar);
    check(on.ops.size() == 2 && on.ops[0].payload["muted"] == false && on.ops[0].label == "Activate Clips",
          "0 again, every one deactivated: activates them");
    auto gone = ui::deleteRange(view.current()->tracks, all, 0, 2 * bar);
    check(gone.ops.size() == 2 && gone.ops[0].opType == "clip.delete" && sub.submit(std::move(gone.ops)).ok &&
              sub.nextUndo()->label == "Delete Clips" && view.current()->findTrack(1)->clips.empty() &&
              view.current()->findTrack(2)->clips.empty() && view.current()->findTrack(3)->clips.size() == 1,
          "Delete removes the clips inside, on every track; the one across the edge stays");
    check(sub.undo().ok && view.current()->findTrack(1)->clips.size() == 1 && view.current()->findTrack(2)->clips.size() == 1,
          "one undo brings both back");
    check(ui::deleteRange(view.current()->tracks, {3}, 0, 2 * bar).refusal.find("crosses") != std::string::npos &&
              ui::deleteRange(view.current()->tracks, all, 5 * bar, 6 * bar).refusal ==
                  "There is nothing in the selection to delete.",
          "only a clip across the edge, or nothing: refused, with the reason");

    // The far track is kept with the rest of the selection, and a selection written without one
    // reads as one track.
    {
        ui::SessionState session(*store);
        ui::Selection chosen;
        chosen.track = 1;
        chosen.toTrack = 3;
        chosen.start = 0;
        chosen.end = bar;
        std::string why;
        check(session.select(chosen, why), "a selection across tracks is kept");
    }
    check(ui::SessionState(*store).selection().toTrack == 3, "and read back with its far track");
    // The tracks picked with Ctrl/Cmd+click are kept too, so an undo puts them back.
    {
        ui::SessionState session(*store);
        ui::Selection chosen;
        chosen.track = 1;
        chosen.picked = {3, 4};
        std::string why;
        check(session.select(chosen, why), "a selection with picked tracks is kept");
    }
    check(ui::SessionState(*store).selection().picked == std::vector<std::int64_t>{3, 4},
          "and read back with them");

    // Clips picked with Ctrl/Cmd+click (Live's "Select Multiple Items", §42.5): the geometry counts
    // them, Delete and 0 act on them all, and the session keeps them.
    geo.selection = {};
    geo.selection.clip = 1;
    geo.selection.pickedClips = {3, 1};
    check(geo.clipSelected(1) && geo.clipSelected(3) && !geo.clipSelected(2) &&
              geo.selectedClips() == std::vector<std::int64_t>{1, 3},
          "the clicked clip and the one picked besides it are selected, the clicked one first");
    auto quiet = ui::deactivateClips(view.current()->tracks, geo.selectedClips());
    check(quiet.refusal.empty() && quiet.ops.size() == 2 && quiet.ops[0].label == "Deactivate Clips" &&
              sub.submit(std::move(quiet.ops)).ok && view.current()->findTrack(1)->clips[0]->muted &&
              view.current()->findTrack(3)->clips[0]->muted && !view.current()->findTrack(2)->clips[0]->muted,
          "0 deactivates both picked clips, one step; the third stays");
    auto loud = ui::deactivateClips(view.current()->tracks, geo.selectedClips());
    check(loud.ops.size() == 2 && loud.ops[0].label == "Activate Clips" && loud.ops[0].payload["muted"] == false,
          "0 again, both off: activates them");
    auto one = ui::deactivateClips(view.current()->tracks, {2});
    check(one.ops.size() == 1 && one.ops[0].label == "Deactivate Clip", "one clip is named as one");
    auto both = ui::deleteClips(view.current()->tracks, geo.selectedClips());
    check(both.ops.size() == 2 && both.ops[0].label == "Delete Clips" && sub.submit(std::move(both.ops)).ok &&
              view.current()->findTrack(1)->clips.empty() && view.current()->findTrack(3)->clips.empty() &&
              view.current()->findTrack(2)->clips.size() == 1,
          "Delete removes both picked clips, one step");
    check(sub.undo().ok && sub.undo().ok && view.current()->findTrack(1)->clips.size() == 1 &&
              !view.current()->findTrack(1)->clips[0]->muted,
          "two undos bring them back, active");
    check(ui::deleteClips(view.current()->tracks, {99}).refusal == "Select a clip to delete.",
          "no clip left to act on: refused, with the reason");
    // Dragged together (§6.7): each keeps its track, by the same offset; none before the start.
    auto later = ui::moveClips(view.current()->tracks, {1, 3}, bar);
    check(later.ops.size() == 2 && later.ops[0].label == "Move Clips" && later.ops[0].payload["to"] == bar &&
              later.ops[0].payload["track"] == 1 && later.ops[1].payload["to"] == bar + bar / 2 + bar &&
              later.ops[1].payload["track"] == 3 && sub.submit(std::move(later.ops)).ok &&
              view.current()->findTrack(1)->clips[0]->posTicks == bar,
          "two clips moved a bar later together, each on its own track, one step");
    check(sub.undo().ok && view.current()->findTrack(1)->clips[0]->posTicks == 0, "undo");
    auto early = ui::moveClips(view.current()->tracks, {2, 3}, -2 * bar);
    check(early.ops.size() == 2 && early.ops[0].payload["to"] == 0 && early.ops[1].payload["to"] == bar / 2,
          "moved earlier than the start allows: the offset stops where the earliest reaches bar 1");
    check(ui::moveClips(view.current()->tracks, {1}, -bar).ops.empty() &&
              ui::moveClips(view.current()->tracks, {99}, bar).refusal == "Select a clip to move.",
          "a clip already at the start moves nowhere; a missing one is refused");
    // Across tracks, each by the same rows, while every one lands on a track that can take it.
    const std::vector<std::int64_t> lanes{1, 2, 3};
    const auto down = ui::clipsAcross(view.current()->tracks, lanes, {1, 2}, 1);
    check(down && *down == ui::ClipTracks{{1, 2}, {2, 3}}, "two clips a row down: track 1's to 2, track 2's to 3");
    check(!ui::clipsAcross(view.current()->tracks, lanes, {1, 2}, 2) &&
              !ui::clipsAcross(view.current()->tracks, {1, 2}, {3}, 0) &&
              ui::clipsAcross(view.current()->tracks, lanes, {1}, 0)->empty(),
          "past the last lane, or from a track not in the lanes: none moves across; no rows: none changes track");
    auto lower = ui::moveClips(view.current()->tracks, {1, 2}, 0, *down);
    check(lower.ops.size() == 2 && lower.ops[0].payload["track"] == 2 && lower.ops[1].payload["track"] == 3 &&
              sub.submit(std::move(lower.ops)).ok && view.current()->findTrack(1)->clips.empty() &&
              view.current()->findTrack(3)->clips.size() == 2,
          "moved a row down with no change of time: each on the next track, one step");
    check(sub.undo().ok && view.current()->findTrack(1)->clips.size() == 1, "undo");
    {
        ui::SessionState session(*store);
        ui::Selection chosen;
        chosen.track = 1;
        chosen.clip = 1;
        chosen.pickedClips = {3};
        std::string why;
        check(session.select(chosen, why), "a selection with picked clips is kept");
    }
    check(ui::SessionState(*store).selection().pickedClips == std::vector<std::int64_t>{3}, "and read back with them");
}

void returnsStandBeforeMain() {
    section("the returns after the tracks, Main last: Live's foot (manual 6.1)");
    adi::test::TempDirectory temp("ui_seams", "returns");
    auto store = freshProject(temp.path() / "p.adi");
    check(store != nullptr, "store created");
    if (!store) return;
    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    bool made = true;
    for (const auto& [id, kind, parent, index] :
         {std::tuple{1, "master", 0, 0}, std::tuple{2, "return", 0, 0}, std::tuple{3, "audio", 0, 0},
          std::tuple{4, "group", 0, 1}, std::tuple{5, "audio", 4, 0}}) {
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", id}, {"kind", kind}, {"name", "t"}, {"index", index}};
        if (parent != 0) r.payload["parent"] = parent;
        r.label = "Create track";
        made = sub.submit(std::move(r)).ok && made;
    }
    check(made, "Main and a return at index 0, a track, a group holding one");
    const ui::SnapshotReader reader(view.current());
    std::vector<ui::TrackRow> rows;
    ui::rowsFromSnapshot(reader, rows);
    ui::TrackOrder order;
    order.build(reader, rows);
    std::vector<std::int64_t> ids;
    for (const auto& p : order.places()) ids.push_back(p.id);
    check((ids == std::vector<std::int64_t>{3, 4, 5, 2, 1}), "the tracks and the group, then the return, then Main");
    check(order.places()[3].ret && order.places()[3].foot() && order.places()[4].foot() && !order.places()[2].foot(),
          "the return and Main make the foot");
    ui::ArrangementGeometry geometry;
    geometry.order = order;
    const auto all = geometry.rows(reader);
    check(geometry.footStart(all) == 3 && geometry.footTop(reader) == 3 * geometry.height,
          "without its view, the foot follows the tracks");
    geometry.lanesHeight = 1000;
    check(geometry.footTop(reader) == 1000 - 2 * geometry.height &&
              geometry.rowTop(reader, 1) == 1000 - geometry.height && geometry.rowTop(reader, 3) == 0,
          "with its view, it stands at the bottom of the lanes");
    check(ui::groupTrack(view.current()->tracks, 2).refusal == "Main and return tracks can't be grouped.",
          "a return can't be grouped");

    // A track's own colour (ADR-0211): an index into Live's palette, or none.
    auto colour = [&sub](std::int64_t id, std::int64_t value) {
        OpRequest r;
        r.opType = "track.setColor";
        r.payload = {{"id", id}, {"color", value}};
        r.label = "Recolour Track";
        return sub.submit(std::move(r)).ok;
    };
    check(colour(3, 26) && colour(5, 0xff0000), "one track given palette colour 26, another an RGB value");
    const ui::SnapshotReader coloured(view.current());
    ui::rowsFromSnapshot(coloured, rows);
    auto colourOf = [&rows](std::int64_t id) {
        for (const auto& r : rows)
            if (r.id == id) return r.colour;
        return -2;
    };
    check(colourOf(3) == 26 && colourOf(5) == -1 && colourOf(4) == -1 && colourOf(1) == -1,
          "the rows carry the index; none, or a value outside the palette, is -1");
}

void referenceTapAndHold() {
    section("the Reference's [REF] and Shift+R: a tap latches, a hold auditions (ADR-0209 d9)");
    ui::ReferenceAudition ref;
    check(!ref.on(), "off to begin with");
    ref.press(0);
    check(ref.on() && ref.held() && !ref.latched(), "down: on, held");
    ref.press(30);
    ref.release(199);
    check(ref.on() && ref.latched() && !ref.held(), "up within 200 ms: a tap latches it; a repeat changed nothing");
    ref.press(1000);
    check(ref.on() && !ref.latched(), "down again: on, held, the latch let go");
    ref.release(1100);
    check(!ref.on(), "a second tap lets it go");
    ref.press(2000);
    ref.release(2200);
    check(!ref.on(), "a hold of 200 ms or more auditions only while held");
    ref.release(2300);
    check(!ref.on(), "an up with nothing down changes nothing");
    ref.press(3000);
    ref.release(3010);
    ref.press(4000);
    ref.release(5000);
    check(!ref.on(), "latched, then held and let go: off");
}

void meterMarkers() {
    section("time signature markers: Insert, Edit and Delete, one step each (Live 12 manual 6.5)");
    for (const auto &[text, ok, num, den] :
         {std::tuple<const char *, bool, int, int>{"3/4", true, 3, 4}, {" 7 , 8 ", true, 7, 8}, {"12.16", true, 12, 16},
          {"5   4", true, 5, 4}, {"6 / 8", true, 6, 8}, {"99/1", true, 99, 1}, {"34", false, 0, 0},
          {"0/4", false, 0, 0}, {"100/4", false, 0, 0}, {"3/3", false, 0, 0}, {"3/32", false, 0, 0},
          {"3//4", false, 0, 0}, {"3/4x", false, 0, 0}, {"", false, 0, 0}, {"/4", false, 0, 0}}) {
        const auto value = ui::parseMeter(text);
        check(value.has_value() == ok && (!ok || (value->numerator == num && value->denominator == den)), text);
    }
    adi::test::TempDirectory temp("ui_seams", "meters");
    auto store = freshProject(temp.path() / "p.adi");
    check(store != nullptr, "store created");
    if (!store) return;
    engine::ProjectView view;
    ui::OpSubmitter sub(*store, view);
    const auto bar = textproj::kWhole;
    auto meters = [&view] { return ui::SnapshotReader(view.current()).meters(); };
    check(meters().size() <= 1 && ui::markerAt(meters(), 0) == 0, "no marker yet");
    const auto atStart = ui::insertMeter(meters(), 0, "3/4");
    check(atStart.ops.empty() && !atStart.refusal.empty(), "at the start: the control bar's, refused with the reason");
    const auto badText = ui::insertMeter(meters(), 2 * bar, "3/5");
    check(badText.ops.empty() && badText.refusal.find("3/4") != std::string::npos, "a value Live takes not: refused");
    auto inserted = ui::insertMeter(meters(), 2 * bar, "3/4");
    check(inserted.ops.size() == 1 && inserted.ops[0].label == "Insert Time Signature Change" &&
              sub.submit(std::move(inserted.ops)).ok,
          "Insert Time Signature Change at bar 3, one op");
    check(meters().size() == 2 && meters()[1].start_ticks == 2 * bar && meters()[1].numerator == 3 &&
              ui::markerAt(meters(), 2 * bar) == 1 && sub.nextUndo()->label == "Insert Time Signature Change",
          "a 3/4 marker at bar 3, one step");
    auto again = ui::insertMeter(meters(), 2 * bar, "6/8");
    check(again.ops.size() == 2 && again.ops[0].label == "Edit Time Signature" && sub.submit(std::move(again.ops)).ok &&
              meters().size() == 2 && meters()[1].numerator == 6 && meters()[1].denominator == 8,
          "inserted where a marker stands: it takes the value, as an edit");
    check(ui::editMeter(meters(), 2 * bar, "6 8").ops.empty() && ui::editMeter(meters(), 2 * bar, "6 8").refusal.empty(),
          "the same value is no edit");
    check(!ui::editMeter(meters(), bar, "2/4").refusal.empty() && !ui::editMeter(meters(), 0, "2/4").refusal.empty(),
          "no marker there, or the start's: Edit is refused");
    auto edited = ui::editMeter(meters(), 2 * bar, "5/4");
    check(sub.submit(std::move(edited.ops)).ok && meters()[1].numerator == 5 && meters()[1].denominator == 4 &&
              sub.nextUndo()->label == "Edit Time Signature" && sub.nextUndo()->opCount == 2,
          "Edit Time Signature: 5/4, one step of two ops");
    check(sub.undo().ok && meters()[1].numerator == 6 && meters()[1].denominator == 8, "undo: 6/8 again");
    check(!ui::deleteMeter(meters(), bar).refusal.empty() && !ui::deleteMeter(meters(), 0).refusal.empty(),
          "no marker there, or the start's: Delete is refused");
    auto removed = ui::deleteMeter(meters(), 2 * bar);
    check(removed.ops.size() == 1 && removed.ops[0].label == "Delete Time Signature Change" &&
              sub.submit(std::move(removed.ops)).ok && meters().size() == 1,
          "Delete: the marker gone, one step");
    check(sub.undo().ok && meters().size() == 2 && meters()[1].numerator == 6, "undo: it is back");
}

void momentaryLatch() {
    section("momentary latching: a key held 500 ms or more goes back on release (Live 42.25)");
    ui::MomentaryLatch latch;
    check(!latch.held() && !latch.release('s', 0), "nothing held: an up changes nothing");
    check(latch.press('s', 7, 1000) && latch.held() && latch.key() == 's', "down: the first press acts");
    check(!latch.press('s', 7, 1030) && !latch.press('s', 7, 1400), "a repeat while held does nothing");
    latch.made(42);
    check(!latch.release('s', 1499) && !latch.held(), "up within 500 ms: a tap stays");
    check(latch.press('s', 7, 2000), "down again: it acts again");
    latch.made(43);
    const auto back = latch.release('s', 2500);
    check(back && back->command == 7 && back->step == 43 && !latch.held(),
          "up after 500 ms: back from the command and the step it made");
    check(latch.press('z', 9, 3000) && !latch.release('s', 4000) && latch.held(),
          "another key's up lets nothing go");
    check(latch.press(1, 11, 3100) && latch.key() == 1, "another key's press takes over");
    const auto zoom = latch.release(1, 3700);
    check(zoom && zoom->command == 11 && zoom->step == 0, "a press that made no step goes back with none");
    check(!latch.release('z', 3800), "the first key, taken over, stays as it is");
    latch.press('s', 7, 5000);
    latch.made(44);
    latch.letGo();
    check(!latch.held() && !latch.release('s', 6000) && latch.press('s', 7, 6000),
          "let go unseen: nothing goes back, and the next press acts");
}

// Live's MIDI track: no audio mixer until it holds an instrument (look-live11 difference row 3); what
// it holds decides, not its kind's hint alone (ADR-0045).
void midiTrackMixer() {
    section("midi track mixer");
    struct Dev {
        std::int64_t track;
        bool instrument;
    };
    engine::TrackNode midiTrack, audioTrack, instrumentTrack;
    midiTrack.id = 1;
    midiTrack.kind = "midi";
    audioTrack.id = 2;
    audioTrack.kind = "audio";
    instrumentTrack.id = 3;
    instrumentTrack.kind = "instrument";
    const std::vector<Dev> nothing, effect{{1, false}}, synth{{1, true}, {2, false}};
    check(ui::isMidiTrack(midiTrack) && ui::isMidiTrack(instrumentTrack) && !ui::isMidiTrack(audioTrack),
          "a midi or instrument track is a MIDI track; an audio one is not");
    check(!ui::hasAudioMixer(midiTrack, &nothing) && !ui::hasAudioMixer(midiTrack, &effect) &&
              !ui::hasAudioMixer(midiTrack, static_cast<const std::vector<Dev> *>(nullptr)),
          "a MIDI track holding no instrument has no audio mixer");
    check(ui::hasAudioMixer(midiTrack, &synth) && !ui::hasAudioMixer(instrumentTrack, &synth),
          "an instrument on it gives it one; an instrument on another track does not");
    check(ui::hasAudioMixer(audioTrack, &nothing), "an audio track always has one");
    // A clip's drag across tracks (Live 12 manual 6.7): MIDI to MIDI, audio to audio; never onto a
    // group, a return or Main.
    engine::TrackNode group, ret, main;
    group.kind = "group";
    ret.kind = "return";
    main.kind = "master";
    check(ui::canTakeClip(instrumentTrack, midiTrack) && ui::canTakeClip(midiTrack, midiTrack) &&
              ui::canTakeClip(audioTrack, audioTrack),
          "a clip moves among MIDI tracks, or among audio tracks");
    check(!ui::canTakeClip(audioTrack, midiTrack) && !ui::canTakeClip(midiTrack, audioTrack),
          "never from a MIDI track onto an audio one, or back");
    check(!ui::canTakeClip(group, audioTrack) && !ui::canTakeClip(ret, audioTrack) && !ui::canTakeClip(main, midiTrack),
          "never onto a group, a return or Main");
}

// Live's Solo rules (manual 18.1, §42.19): Exclusive Solo, Ctrl/Cmd for several, the selected tracks
// together; Main has no Solo.
void soloRules() {
    section("solo rules");
    ui::SnapshotTracks tracks;
    auto add = [&tracks](std::int64_t id, const char *kind, bool soloed) {
        auto t = std::make_shared<engine::TrackNode>();
        t->id = id;
        t->kind = kind;
        t->soloed = soloed;
        tracks.push_back(t);
    };
    add(1, "master", false);
    add(2, "audio", true);
    add(3, "audio", false);
    add(4, "midi", false);
    auto ops = [](const ui::TrackEdit &e) {
        std::vector<std::pair<std::int64_t, bool>> out;
        for (const auto &o : e.ops)
            out.emplace_back(o.payload["id"].get<std::int64_t>(), o.payload["soloed"].get<bool>());
        return out;
    };
    using Ops = std::vector<std::pair<std::int64_t, bool>>;
    check(ops(ui::soloTracks(tracks, 3, {3}, true, false)) == Ops{{2, false}, {3, true}},
          "exclusive: soloing Snare unsolos Kick, one batch");
    check(ops(ui::soloTracks(tracks, 3, {3}, true, true)) == Ops{{3, true}}, "Ctrl/Cmd adds Snare, Kick stays");
    check(ops(ui::soloTracks(tracks, 3, {3}, false, false)) == Ops{{3, true}}, "Exclusive Solo off: Snare joins Kick");
    check(ops(ui::soloTracks(tracks, 3, {3, 4}, true, false)) == Ops{{2, false}, {3, true}, {4, true}},
          "with Snare and Bass selected, both solo together, Kick unsoloed");
    check(ops(ui::soloTracks(tracks, 2, {3, 4}, true, false)) == Ops{{2, false}},
          "Kick, soloed and not selected, unsolos alone");
    check(ui::soloTracks(tracks, 1, {}, true, false).refusal == "Main has no Solo." &&
              ui::soloTracks(tracks, 9, {}, true, false).refusal == "Select a track to solo.",
          "Main and a missing track are refused, with the reason");
    check(ui::soloTracks(tracks, 3, {3}, true, false).ops.front().label == "Solo Track" &&
              ui::soloTracks(tracks, 2, {}, true, false).ops.front().label == "Unsolo Track",
          "named Solo Track or Unsolo Track, as before");

    // Live's Track Activator (manual 18.1): the selected tracks together; Main has none.
    auto mutes = [](const ui::TrackEdit &e) {
        std::vector<std::pair<std::int64_t, bool>> out;
        for (const auto &o : e.ops)
            out.emplace_back(o.payload["id"].get<std::int64_t>(), o.payload["muted"].get<bool>());
        return out;
    };
    check(mutes(ui::activateTracks(tracks, 3, {3})) == Ops{{3, true}}, "Snare deactivated alone");
    check(mutes(ui::activateTracks(tracks, 3, {2, 3, 4})) == Ops{{2, true}, {3, true}, {4, true}},
          "with Kick, Snare and Bass selected, all three together");
    check(mutes(ui::activateTracks(tracks, 3, {2, 4})) == Ops{{3, true}}, "Snare, not among the selected, alone");
    check(ui::activateTracks(tracks, 1, {}).refusal == "Main has no Track Activator." &&
              ui::activateTracks(tracks, 3, {3}).ops.front().label == "Deactivate Track",
          "Main refused; the step named as before");
}

int main() {
    theWrongThingDoesNotCompile();
    invalidReaderAnswersEmpty();
    readsTheModel();
    onePublicationIsCheckable();
    outlivesItsOwner();

    submitsOneOp();
    batchIsOneUndoStep();
    selBeforeOnlyOnTheFirst();
    emptyBatchWritesNothing();
    stampsTheActor();
    aRefusedOpWritesNothing();
    everyWriteRefreshesTheView();
    aFrameKeepsItsSnapshot();
    projectViewTree();
    projectViewRowsFromTheSnapshot();
    metersCountBars();
    groupAndUngroupAsBatches();
    returnsStandBeforeMain();
    sendsAsBatches();
    audioDropAsOneStep();
    rangeEditsAsBatches();
    referenceTapAndHold();
    momentaryLatch();
    meterMarkers();
    midiTrackMixer();
    soloRules();

    std::printf("%s -- %d checks, %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
