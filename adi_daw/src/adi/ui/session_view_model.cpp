// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/session_view_model.hpp"

#include "adi/store_rows.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace adi::ui::session_view {
namespace {
using engine::LaunchQueuedAction;
using engine::LaunchTrackSource;
using engine::launch::SlotRecordingState;
using Index = std::vector<std::pair<std::int64_t, std::size_t>>;

std::optional<ColumnKind> kindOf(const std::string& kind) {
    if (kind == "audio" || kind == "midi" || kind == "instrument")
        return ColumnKind::Clips;
    if (kind == "group")
        return ColumnKind::Group;
    if (kind == "return")
        return ColumnKind::Return;
    return std::nullopt;
}

std::optional<std::size_t> lookup(const Index& index, std::int64_t id) noexcept {
    const auto it = std::lower_bound(index.begin(), index.end(), id,
                                     [](const std::pair<std::int64_t, std::size_t>& e, std::int64_t v) { return e.first < v; });
    if (it == index.end() || it->first != id)
        return std::nullopt;
    return it->second;
}

bool showsRecording(SlotRecordingState state) noexcept {
    return state == SlotRecordingState::Armed || state == SlotRecordingState::Recording;
}
} // namespace

SlotState stateOf(const SlotFacts& facts, const SlotLive& live) noexcept {
    if (live.recording)
        return SlotState::Recording;
    switch (live.queued) {
    case SlotQueue::Start: return SlotState::QueuedStart;
    case SlotQueue::Stop: return SlotState::QueuedStop;
    case SlotQueue::Record: return SlotState::QueuedRecord;
    case SlotQueue::None: break;
    }
    if (live.playing)
        return SlotState::Playing;
    return facts.clip != 0 || facts.groupOccupied ? SlotState::Stopped : SlotState::Empty;
}

ModelInput modelInput(const rows::Model& model, const engine::launch::LaunchData& data) {
    ModelInput in;
    // The Arrangement's order: each parent's tracks by rank (the returns after the rest), then by
    // index_in_parent, then by identity; a group followed by what it holds. Main is the scene column.
    std::map<std::int64_t, std::vector<const rows::Track*>> children;
    for (const auto& t : model.tracks)
        if (t.kind != "master")
            children[t.parentId.value_or(0)].push_back(&t);
    const auto rank = [](const rows::Track& t) { return t.kind == "return" ? 1 : 0; };
    for (auto& entry : children)
        std::sort(entry.second.begin(), entry.second.end(), [&](const rows::Track* a, const rows::Track* b) {
            if (rank(*a) != rank(*b))
                return rank(*a) < rank(*b);
            if (a->indexInParent != b->indexInParent)
                return a->indexInParent < b->indexInParent;
            return a->id < b->id;
        });
    std::vector<const rows::Track*> stack;
    if (const auto top = children.find(0); top != children.end())
        stack.assign(top->second.rbegin(), top->second.rend());
    std::set<std::int64_t> seen; // a parent that loops back ends the walk rather than the window
    while (!stack.empty()) {
        const auto* t = stack.back();
        stack.pop_back();
        if (!seen.insert(t->id).second)
            continue;
        if (const auto kind = kindOf(t->kind))
            in.tracks.push_back({t->id, *kind, t->parentId.value_or(0), t->name, t->color, t->recordArmed});
        if (const auto held = children.find(t->id); held != children.end())
            stack.insert(stack.end(), held->second.rbegin(), held->second.rend());
    }
    in.scenes.assign(data.scenes().begin(), data.scenes().end());
    for (const auto& s : data.slots())
        in.slots.push_back({s.key.trackId, s.key.sceneId, s.clipId.value_or(0), s.hasStop});
    for (const auto& c : model.slotClips) {
        const auto settings = data.settings(c.id);
        in.clips.push_back({c.id, c.name, c.color, c.muted,
                            settings ? settings->mode : engine::LaunchMode::Trigger,
                            settings && settings->follow.enabled});
    }
    return in;
}

void Model::rebuild(ModelInput in) {
    // The engine's last frame, kept by identity so the grid does not go blank until the next one.
    std::vector<engine::LaunchTrackStatus> kept;
    for (std::size_t c = 0; c < columns_.size(); ++c)
        if (columnHasStatus_[c])
            kept.push_back(columnStatus_[c]);

    columns_.clear();
    for (auto& t : in.tracks)
        columns_.push_back({t.id, t.kind, t.parent, std::move(t.name), t.color, t.armed});
    scenes_.clear();
    std::int64_t number = 1;
    for (auto& s : in.scenes)
        scenes_.push_back({s.id, s.order, number++, std::move(s.name), s.color, s.tempoBpm, s.tempoEnabled,
                           s.signatureNumerator, s.signatureDenominator, s.signatureEnabled, s.follow.enabled, 0});
    columnIndex_.clear();
    for (std::size_t c = 0; c < columns_.size(); ++c)
        columnIndex_.emplace_back(columns_[c].track, c);
    std::sort(columnIndex_.begin(), columnIndex_.end());
    sceneIndex_.clear();
    for (std::size_t r = 0; r < scenes_.size(); ++r)
        sceneIndex_.emplace_back(scenes_[r].id, r);
    std::sort(sceneIndex_.begin(), sceneIndex_.end());

    const auto columns = columns_.size(), rows = scenes_.size();
    facts_.assign(columns * rows, SlotFacts{});
    std::map<std::int64_t, const ModelInput::Clip*> clips;
    for (const auto& c : in.clips)
        clips.emplace(c.id, &c);
    for (const auto& s : in.slots) {
        const auto c = columnOf(s.track);
        const auto r = rowOf(s.scene);
        if (!c || !r || columns_[*c].kind != ColumnKind::Clips)
            continue;
        auto& f = facts_[at(*c, *r)];
        f.hasStop = s.hasStop;
        if (s.clip == 0)
            continue;
        f.clip = s.clip;
        if (const auto found = clips.find(s.clip); found != clips.end()) {
            f.name = found->second->name;
            f.color = found->second->color;
            f.deactivated = found->second->deactivated;
            f.follow = found->second->follow;
            f.mode = found->second->mode;
        }
        ++scenes_[*r].occupied;
    }

    // A group's slots stand for the clips of the tracks it holds at any depth (manual 7.1): shaded
    // where one holds a clip, in the left-most such clip's colour; a Stop button where none does.
    const auto within = [&](std::size_t k, std::int64_t group) {
        auto parent = columns_[k].parent;
        for (std::size_t steps = 0; parent != 0 && steps <= columns; ++steps) {
            if (parent == group)
                return true;
            const auto up = columnOf(parent);
            if (!up)
                return false;
            parent = columns_[*up].parent;
        }
        return false;
    };
    held_.clear();
    heldRange_.assign(columns, {0, 0});
    for (std::size_t c = 0; c < columns; ++c) {
        if (columns_[c].kind != ColumnKind::Group)
            continue;
        const auto begin = held_.size();
        for (std::size_t k = 0; k < columns; ++k)
            if (k != c && columns_[k].kind == ColumnKind::Clips && within(k, columns_[c].track))
                held_.push_back(k);
        heldRange_[c] = {begin, held_.size()};
        for (std::size_t r = 0; r < rows; ++r) {
            auto& g = facts_[at(c, r)];
            for (const auto k : heldColumns(c)) {
                const auto& f = facts_[at(k, r)];
                if (f.clip != 0 && !g.groupOccupied) {
                    g.groupOccupied = true;
                    g.color = f.color;
                }
            }
            g.hasStop = !g.groupOccupied;
        }
    }

    live_.assign(columns * rows, SlotLive{});
    nextLive_ = live_;
    trackLive_.assign(columns, TrackLive{});
    nextTrackLive_ = trackLive_;
    sceneLive_.assign(rows, SceneLive{});
    nextSceneLive_ = sceneLive_;
    columnStatus_.assign(columns, engine::LaunchTrackStatus{});
    columnHasStatus_.assign(columns, 0);
    for (const auto& s : kept)
        if (const auto c = columnOf(s.track)) {
            columnStatus_[*c] = s;
            columnHasStatus_[*c] = 1;
        }
    recording_.reserve(std::max<std::size_t>(columns * rows, recording_.size()));
    ++projectRevision_;
    resolveSelection();
    (void)publish();
}

std::optional<std::size_t> Model::columnOf(std::int64_t track) const noexcept { return lookup(columnIndex_, track); }
std::optional<std::size_t> Model::rowOf(std::int64_t scene) const noexcept { return lookup(sceneIndex_, scene); }

std::span<const std::size_t> Model::heldColumns(std::size_t c) const noexcept {
    if (c >= heldRange_.size())
        return {};
    const auto [begin, end] = heldRange_[c];
    return std::span<const std::size_t>(held_).subspan(begin, end - begin);
}

bool Model::publish() noexcept {
    const auto columns = columns_.size(), rows = scenes_.size();
    std::fill(nextLive_.begin(), nextLive_.end(), SlotLive{});
    std::fill(nextTrackLive_.begin(), nextTrackLive_.end(), TrackLive{});
    std::fill(nextSceneLive_.begin(), nextSceneLive_.end(), SceneLive{});
    for (std::size_t c = 0; c < columns; ++c) {
        if (!columnHasStatus_[c] || columns_[c].kind != ColumnKind::Clips)
            continue;
        const auto& s = columnStatus_[c];
        auto& t = nextTrackLive_[c];
        t.source = s.source;
        t.playingClip = s.clip;
        t.playingScene = s.scene;
        t.queued = s.queued.action;
        t.completedLoops = s.completedLoops;
        t.loopLengthTicks = s.loopLengthTicks;
        t.remainingSamples = s.remainingSamples;
        const auto playingRow = s.clip != 0 ? rowOf(s.scene) : std::nullopt;
        if (playingRow) {
            nextLive_[at(c, *playingRow)].playing = true;
            ++nextSceneLive_[*playingRow].playing;
        }
        switch (s.queued.action) {
        case LaunchQueuedAction::Start:
            if (const auto r = rowOf(s.queued.scene))
                nextLive_[at(c, *r)].queued = SlotQueue::Start;
            break;
        case LaunchQueuedAction::Stop:
            // A Stop for this run, or for the whole track (its Clip Stop button, an empty slot's
            // Stop in a launched scene): the playing slot and the track's Stop button both wait.
            t.stopQueued = true;
            if (playingRow && (s.queued.clip == 0 || s.queued.clip == s.clip))
                nextLive_[at(c, *playingRow)].queued = SlotQueue::Stop;
            break;
        case LaunchQueuedAction::BackToArrangement:
        case LaunchQueuedAction::None:
            break;
        }
    }
    if (showsRecording(recordingState_))
        for (const auto& key : recording_) {
            const auto c = columnOf(key.trackId);
            const auto r = rowOf(key.sceneId);
            if (!c || !r || columns_[*c].kind != ColumnKind::Clips)
                continue;
            auto& l = nextLive_[at(*c, *r)];
            if (recordingState_ == SlotRecordingState::Recording)
                l.recording = true;
            else
                l.queued = SlotQueue::Record;
        }
    for (std::size_t c = 0; c < columns; ++c) {
        if (columns_[c].kind != ColumnKind::Group)
            continue;
        auto& t = nextTrackLive_[c];
        for (const auto k : heldColumns(c)) {
            const auto& h = nextTrackLive_[k];
            if (h.source == LaunchTrackSource::Session)
                t.source = LaunchTrackSource::Session;
            t.stopQueued = t.stopQueued || h.stopQueued;
        }
        for (std::size_t r = 0; r < rows; ++r) {
            auto& g = nextLive_[at(c, r)];
            for (const auto k : heldColumns(c)) {
                const auto& h = nextLive_[at(k, r)];
                g.playing = g.playing || h.playing;
                g.recording = g.recording || h.recording;
                if (g.queued == SlotQueue::None)
                    g.queued = h.queued;
            }
        }
    }
    if (pendingScene_)
        if (const auto r = rowOf(*pendingScene_))
            nextSceneLive_[*r].triggered = true;
    const bool changed = nextLive_ != live_ || nextTrackLive_ != trackLive_ || nextSceneLive_ != sceneLive_;
    if (changed) {
        live_.swap(nextLive_);
        trackLive_.swap(nextTrackLive_);
        sceneLive_.swap(nextSceneLive_);
        ++liveRevision_;
    }
    return changed;
}

bool Model::applyStatus(std::span<const engine::LaunchTrackStatus> statuses, const engine::LaunchStatusHeader& header,
                        std::optional<std::int64_t> pendingScene) noexcept {
    std::fill(columnHasStatus_.begin(), columnHasStatus_.end(), std::uint8_t{0});
    for (const auto& s : statuses)
        if (const auto c = columnOf(s.track)) {
            columnStatus_[*c] = s;
            columnHasStatus_[*c] = 1;
        }
    const bool headerChanged = backToArrangement_ != header.backToArrangement ||
                               transportPlaying_ != header.transportPlaying || pendingScene_ != pendingScene ||
                               statusIncarnation_ != header.incarnation;
    backToArrangement_ = header.backToArrangement;
    transportPlaying_ = header.transportPlaying;
    pendingScene_ = pendingScene;
    statusIncarnation_ = header.incarnation;
    const bool live = publish();
    if (headerChanged && !live)
        ++liveRevision_;
    return headerChanged || live;
}

bool Model::applyRecording(SlotRecordingState state, std::span<const engine::launch::SlotKey> targets) noexcept {
    if (state == recordingState_ && std::equal(targets.begin(), targets.end(), recording_.begin(), recording_.end()))
        return false;
    recordingState_ = state;
    recording_.clear();
    for (const auto& key : targets) {
        if (recording_.size() == recording_.capacity())
            break; // more targets than slots shown: the rest cannot be on the grid
        recording_.push_back(key);
    }
    return publish();
}

void Model::select(const Selection& wanted) noexcept {
    auto next = wanted;
    if (next.anchorTrack == 0)
        next.anchorTrack = next.track;
    if (next.anchorScene == 0)
        next.anchorScene = next.scene;
    const auto holdsSlots = [this](std::int64_t track) {
        const auto c = columnOf(track);
        return c && columns_[*c].kind != ColumnKind::Return;
    };
    bool valid = false;
    switch (next.focus) {
    case Focus::Slot:
        valid = holdsSlots(next.track) && holdsSlots(next.anchorTrack) && rowOf(next.scene) && rowOf(next.anchorScene);
        break;
    case Focus::Scene:
        next.track = next.anchorTrack = 0;
        valid = rowOf(next.scene) && rowOf(next.anchorScene);
        break;
    case Focus::Track:
        next.scene = next.anchorScene = 0;
        valid = columnOf(next.track) && columnOf(next.anchorTrack);
        break;
    case Focus::None:
        break;
    }
    if (!valid)
        next = Selection{};
    if (next != selection_) {
        selection_ = next;
        ++selectionRevision_;
    }
}

void Model::resolveSelection() noexcept {
    // A rebuild keeps what still stands; a range whose far end went collapses to its focus.
    auto s = selection_;
    const bool focusStands = s.focus == Focus::Slot    ? columnOf(s.track) && rowOf(s.scene)
                             : s.focus == Focus::Scene ? rowOf(s.scene).has_value()
                             : s.focus == Focus::Track ? columnOf(s.track).has_value()
                                                       : false;
    if (!focusStands) {
        select(Selection{});
        return;
    }
    if ((s.focus != Focus::Scene && !columnOf(s.anchorTrack)) || (s.focus != Focus::Track && !rowOf(s.anchorScene))) {
        s.anchorTrack = s.track;
        s.anchorScene = s.scene;
    }
    select(s);
}

std::optional<Model::Range> Model::selectedRange() const noexcept {
    const auto& s = selection_;
    // select() admits only identities the model shows, so both ends resolve.
    const auto ordered = [](std::optional<std::size_t> a, std::optional<std::size_t> b) {
        return std::pair{std::min(a.value_or(0), b.value_or(0)), std::max(a.value_or(0), b.value_or(0))};
    };
    switch (s.focus) {
    case Focus::Slot: {
        const auto [c0, c1] = ordered(columnOf(s.track), columnOf(s.anchorTrack));
        const auto [r0, r1] = ordered(rowOf(s.scene), rowOf(s.anchorScene));
        return Range{c0, c1, r0, r1};
    }
    case Focus::Scene: {
        const auto [r0, r1] = ordered(rowOf(s.scene), rowOf(s.anchorScene));
        return Range{0, 0, r0, r1};
    }
    case Focus::Track: {
        const auto [c0, c1] = ordered(columnOf(s.track), columnOf(s.anchorTrack));
        return Range{c0, c1, 0, 0};
    }
    case Focus::None:
        break;
    }
    return std::nullopt;
}

bool Model::slotSelected(std::size_t c, std::size_t r) const noexcept {
    const auto range = selection_.focus == Focus::Slot ? selectedRange() : std::nullopt;
    return range && c >= range->c0 && c <= range->c1 && r >= range->r0 && r <= range->r1;
}

bool Model::sceneSelected(std::size_t r) const noexcept {
    const auto range = selection_.focus == Focus::Scene ? selectedRange() : std::nullopt;
    return range && r >= range->r0 && r <= range->r1;
}

bool Model::trackSelected(std::size_t c) const noexcept {
    const auto range = selection_.focus == Focus::Track ? selectedRange() : std::nullopt;
    return range && c >= range->c0 && c <= range->c1;
}

} // namespace adi::ui::session_view
