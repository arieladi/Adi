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

#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/ui/op_submitter.hpp"
#include "adi/ui/snapshot_reader.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
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
    auto st = Store::create(p, err);
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

    ui::OpSubmitter sub(*store);
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

    ui::OpSubmitter sub(*store);
    std::vector<OpRequest> batch{trackCreate(1, "Drums"), trackCreate(2, "Bass")};
    batch[0].selBefore = Payload{{"tracks", Payload::array()}};
    batch[1].selBefore = Payload{{"tracks", Payload::array({1})}};

    const auto r = sub.submit(std::move(batch));
    check(r.ok, "batch committed");
    check(r.seqs.size() == 2, "two ops");
    check(countTracks(*store) == 2, "both tracks exist");

    History h(*store);
    check(h.canUndo(), "there is something to undo");
    const auto step = h.nextUndo();
    check(step.has_value(), "and the history can describe it");
    check(step && step->opCount == 2, "ONE undo step covering both ops");
    check(step && step->txnId == r.txnId, "and it is the transaction we committed");

    const auto undone = h.undo();
    check(undone.ok, "undo applied");
    check(countTracks(*store) == 0, "both tracks went together");

    const auto redone = h.redo();
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

    ui::OpSubmitter sub(*store);
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

    ui::OpSubmitter sub(*store);
    const auto r = sub.submit(std::vector<OpRequest>{});
    check(r.ok, "reports success");
    check(r.txnId == 0, "opened no transaction");
    check(r.seqs.empty(), "wrote no ops");

    History h(*store);
    check(!h.canUndo(), "and left nothing to undo");
}

void stampsTheActor() {
    section("the submitter says who acted, without a call site repeating it");
    adi::test::TempDirectory temp("ui_seams", "actor");
    auto store = freshProject(temp.path() / "p.adi");
    if (!store) { check(false, "store created"); return; }

    ui::OpSubmitter agent(*store, Actor::Agent, "adi-vst");
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

    ui::OpSubmitter sub(*store);
    OpRequest bad;
    bad.opType = "track.create";
    bad.payload = {{"id", 1}};   // no `kind`, which the catalogue requires
    const auto r = sub.submit(std::move(bad));
    check(!r.ok, "refused");
    check(!r.error.empty() || !r.issues.empty(), "and said why");
    check(countTracks(*store) == 0, "no track was written");

    History h(*store);
    check(!h.canUndo(), "and no undo entry was left behind");
}

}  // namespace

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

    std::printf("%s -- %d checks, %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
