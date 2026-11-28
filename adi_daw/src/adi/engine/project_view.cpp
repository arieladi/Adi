// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/project_view.hpp"
#include "adi/session_schema.hpp"
#include "adi/engine/timeline_clip_sql.hpp"
#include "adi/engine/gesture_preview.hpp"
#include "adi/freeze_state.hpp"
#include "adi/gesture_capture_detail.hpp"
#include "adi/history.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <stdexcept>
#include <tuple>
namespace adi::engine {
namespace {
using StoreStamp = std::array<std::int64_t, 4>;
StoreStamp stamp(const Store& store) {
    auto& db = const_cast<SQLite::Database&>(store.db());
    return {sqlite3_total_changes64(db.getHandle()), db.execAndGet("PRAGMA data_version").getInt64(),
            db.execAndGet("PRAGMA main.schema_version").getInt64(), db.execAndGet("PRAGMA temp.schema_version").getInt64()};
}
}
ProjectView::ProjectView():endpoint_(std::make_shared<ProjectViewEndpoint>()) {endpoint_->owner_=this;}
ProjectView::~ProjectView() {endpoint_->owner_=nullptr;}
void ProjectView::adoptCommitted(std::shared_ptr<const Snapshot> value,const gesture::Base& base,
                                std::shared_ptr<const std::vector<std::uint8_t>> image) {
    const bool same=base_.source.sameConnection(base.source) && base_.head==base.head &&
        base_.sequence==base.sequence && image_ && image && *image_==*image;
    storeStamp_.reset();localEdit_.reset();
    committed_=std::move(value);base_=base;image_=std::move(image);++committedGeneration_;
    if(!same || !preview_) {current_=committed_;preview_=false;++presentationGeneration_;}
    ++generation_;
}
void ProjectView::showPreview(std::shared_ptr<const Snapshot> value) {
    current_=std::move(value);preview_=true;++presentationGeneration_;++generation_;
}
void ProjectView::showCommitted() noexcept {
    current_=committed_;preview_=false;++presentationGeneration_;++generation_;
}
std::uint64_t ProjectView::refresh(const Store& store,const std::set<std::int64_t>& overridden) {
    const auto source=adi::appliedSource(store);
    // Inspection of a read-only project needs a snapshot, but cannot begin
    // gestures and therefore has no editable endpoint registration.
    if(!store.readOnly()) {
        const auto registered=gesture::detail::registerProjectViewEndpoint(source,endpoint_);
        if(!registered.ok())throw std::runtime_error(registered.message);
    }
    auto& db = const_cast<SQLite::Database&>(store.db());
    std::optional<SQLite::Transaction> read;
    if (sqlite3_get_autocommit(db.getHandle()) != 0)
        read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
    const auto before = stamp(store);
    std::shared_ptr<const Snapshot> next=committed_ ? SnapshotBuilder::fromStore(store,*committed_,overridden)
                                                   : SnapshotBuilder::fromStore(store,overridden);
    adoptCommitted(std::move(next),{source,History(const_cast<Store&>(store)).head(),source.sequence()},
                   std::make_shared<const std::vector<std::uint8_t>>(capturePresentationImage(store)));
    if (read) {
        read->commit();
        if (before == stamp(store)) storeStamp_ = before;
    }
    return generation_;
}

bool ProjectView::storeCurrent(const Store& store) const {
    if (!committed_ || !storeStamp_ || sqlite3_get_autocommit(store.db().getHandle()) == 0) return false;
    try {
        const auto source = appliedSource(store);
        return base_.source.sameConnection(source) && base_.sequence == source.sequence() && *storeStamp_ == stamp(store);
    } catch (const std::exception&) { return false; }
}

void ProjectView::acknowledgeAdvisoryWrite(const Store& store) noexcept {
    if (!storeStamp_) return;
    const auto direct = sqlite3_changes64(store.db().getHandle());
    if (direct <= 1)
        acknowledgeAdvisoryWrite(store, direct);
}
void ProjectView::acknowledgeAdvisoryWrite(const Store& store, std::int64_t writtenRows) noexcept {
    if (!storeStamp_) return;
    try {
        const auto source = appliedSource(store);
        const auto observed = stamp(store);
        if (base_.source.sameConnection(source) && base_.sequence == source.sequence() &&
            writtenRows >= 0 && observed[0] - (*storeStamp_)[0] == writtenRows &&
            std::equal(observed.begin() + 1, observed.end(), storeStamp_->begin() + 1)) storeStamp_ = observed;
    } catch (const std::exception&) { }
}

std::optional<ProjectView::LocalEdit> ProjectView::prepareLocalEdit(const Store& store, const OpRequest& request) const {
    using Kind = LocalEdit::Kind;
    const auto op = std::string_view(request.opType);
    const bool transport = isEphemeralTransportNavigation(op);
    if ((!transport && op != "track.rename" && op != "clip.move" && op != "arranger.splice" && op != "note.insert") ||
        (preview_ && !transport) || request.capturedTransaction || !storeCurrent(store)) return {};
    try {
        // Known media and Session guards can only veto these edits. The
        // Session scene-delete trigger never fires for the operations above.
        // An unknown trigger could turn one edit into unrelated writes.
        auto& db = const_cast<SQLite::Database&>(store.db());
        if (db.execAndGet(
                "SELECT EXISTS(SELECT 1 FROM sqlite_temp_master WHERE type IN ('trigger','view','table'))")
                .getInt()) return {};
        SQLite::Statement triggers(db, "SELECT name,sql FROM sqlite_master WHERE type='trigger' AND NOT "
                "((tbl_name='media_files' AND name IN ('media_never_embedded_insert','media_never_embedded_update')) "
                "OR (tbl_name='media_blobs' AND name='media_blobs_forbidden'))");
        while (triggers.executeStep())
            if (!adi::detail::isKnownSessionTrigger(triggers.getColumn(0).getString(), triggers.getColumn(1).getString()))
                return {};
        if (transport) return LocalEdit{Kind::Transport, 0, generation_};
        if (op == "arranger.splice") {
            // The Arrangement's prepared move can splice overlapping clips
            // or automation. Admit the common form that only changes one
            // retained MIDI clip's position and/or track.
            const auto& changes = request.payload.at("changes");
            if (!changes.is_array() || changes.size() != 1) return {};
            const auto& change = changes.front();
            const auto& before = change.at("before");
            const auto& after = change.at("after");
            if (change.at("table") != "clips" || !before.is_object() || !after.is_object() ||
                before.at("kind") != "midi" || before.at("time_base") != 0 || !before.at("lane_id").is_null() ||
                before.size() != after.size()) return {};
            for (auto field = before.begin(); field != before.end(); ++field)
                if (field.key() != "track_id" && field.key() != "pos_ticks" && after.at(field.key()) != field.value()) return {};
            return LocalEdit{Kind::MidiPlacement, adi::detail::signedInteger(change.at("id")), generation_};
        }
        const auto kind = op == "track.rename" ? Kind::TrackName : op == "clip.move" ? Kind::MidiPlacement : Kind::MidiNotes;
        const auto id = adi::detail::signedInteger(request.payload.at(kind == Kind::MidiNotes ? "clip" : "id"));
        return LocalEdit{kind, id, generation_};
    } catch (const std::exception&) { return {}; }
}

bool ProjectView::applyLocalEdit(const Store& store, LocalEdit edit) {
    if (!storeStamp_ || edit.fromGeneration != generation_ || !freeze::validated(const_cast<Store&>(store))) return false;
    try {
        const auto source = appliedSource(store);
        const auto observed = stamp(store);
        if (!base_.source.sameConnection(source) || source.sequence() != base_.sequence + 1 ||
            !std::equal(observed.begin() + 1, observed.end(), storeStamp_->begin() + 1)) return false;
        const gesture::Base base{source, History(const_cast<Store&>(store)).head(), source.sequence()};
        using Kind = LocalEdit::Kind;
        if (edit.kind == Kind::Transport) {
            if (base.head != base_.head || observed != stamp(store)) return false;
            base_ = base;
            storeStamp_ = observed;
            return true;
        }
        std::shared_ptr<const Snapshot> next = committed_;
        if (edit.kind == Kind::TrackName) {
            SQLite::Statement row(store.db(), "SELECT name FROM tracks WHERE id=?");
            row.bind(1, edit.id);
            if (!row.executeStep()) return false;
            edit.name = row.getColumn(0).getString();
            auto patched = std::make_shared<Snapshot>(*committed_);
            const auto at = std::find_if(patched->tracks.begin(), patched->tracks.end(),
                                         [&](const auto& track) { return track->id == edit.id; });
            if (at == patched->tracks.end()) return false;
            auto renamed = std::make_shared<TrackNode>(**at);
            renamed->name = edit.name;
            *at = std::move(renamed);
            next = std::move(patched);
        } else if (edit.kind == Kind::MidiPlacement) {
            // A musical MIDI clip can move without reprojecting its sample
            // edges, take-lane ownership or fades from another clock.
            SQLite::Statement row(store.db(), "SELECT track_id,pos_ticks FROM clips WHERE id=? AND kind='midi' "
                                              "AND time_base=0 AND lane_id IS NULL" +
                                              excludeSlotClips(store.db(), "clips.id"));
            row.bind(1, edit.id);
            if (!row.executeStep()) return false;
            edit.toTrack = row.getColumn(0).getInt64();
            edit.position = row.getColumn(1).getInt64();
            auto patched = std::make_shared<Snapshot>(*committed_);
            auto old = patched->tracks.begin();
            std::shared_ptr<const ClipNode> clip;
            for (; old != patched->tracks.end(); ++old) {
                const auto& clips = (*old)->clips;
                const auto at = std::find_if(clips.begin(), clips.end(), [&](const auto& node) { return node->id == edit.id; });
                if (at != clips.end()) {
                    clip = *at;
                    break;
                }
            }
            const auto destination = std::find_if(patched->tracks.begin(), patched->tracks.end(),
                                                   [&](const auto& track) { return track->id == edit.toTrack; });
            if (!clip || destination == patched->tracks.end()) return false;
            edit.fromTrack = (*old)->id;
            auto origin = std::make_shared<TrackNode>(**old);
            std::erase(origin->clips, clip);
            *old = origin;
            auto moved = std::make_shared<ClipNode>(*clip);
            moved->posTicks = edit.position;
            auto target = old == destination ? std::move(origin) : std::make_shared<TrackNode>(**destination);
            const auto before = std::lower_bound(target->clips.begin(), target->clips.end(), moved, [](const auto& a, const auto& b) {
                return std::tie(a->posTicks, a->id) < std::tie(b->posTicks, b->id);
            });
            target->clips.insert(before, std::move(moved));
            *destination = std::move(target);
            if (old != destination) for (auto& remark : patched->remarks) {
                if (remark->targetKind != "clip" || remark->targetId != edit.id) continue;
                auto updated = std::make_shared<RemarkNode>(*remark);
                updated->trackId = edit.toTrack;
                updated->anchorMissing = false;
                remark = std::move(updated);
            }
            next = std::move(patched);
        } else if (edit.kind == Kind::MidiNotes) {
            next = SnapshotBuilder::withMidiClipNotes(store, committed_, edit.id);
            if (!next) return false;
        }
        if (observed != stamp(store)) return false;
        adoptCommitted(std::move(next), base, {});
        storeStamp_ = observed;
        localEdit_ = std::move(edit);
        return true;
    } catch (const std::exception&) { return false; }
}
} // namespace adi::engine
