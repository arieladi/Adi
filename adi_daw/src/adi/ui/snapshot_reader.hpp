// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI's read half. ADR-0180 d2, ADR-0050 d3, ADR-0181 d5.
//
// THIS IS NOT `SnapshotPublisher::AudioRead`, AND THAT IS THE WHOLE POINT.
//
// `publisher.hpp:38-41` states the three preconditions the publisher's
// reclamation rests on: exactly ONE reader, moving only forward, announcing
// before it dereferences. A second reader storing into `inUse_` breaks all
// three, and `collect()` then frees the snapshot the audio thread is
// rendering. ADR-0180 d2 refuses that.
//
// The refusal here is BY CONSTRUCTION, not by a second safety argument that
// would have to be re-derived every time someone touches this file:
// `SnapshotReader` holds a `shared_ptr<const Snapshot>` and never names a
// publisher. It cannot reach `inUse_`, so there is no ordering to get wrong.
// Its whole lifetime guarantee is refcounting.
//
// An earlier draft of this type took the snapshot by calling `peek()` on the
// publisher and copying through the returned pointer. That is a use-after-free:
// `peek()` hands back a bare `const T*` with no ownership, it sits under
// publisher.hpp's `audio thread` section header, and its own comment says the
// rule that makes it safe belongs to the announcing caller. Between the load
// and the copy, a publish from the device thread can retire that object and
// `collect()` can delete it. `host.hpp` already carries a guard against exactly
// this shape and `tests/test_host.cpp` regression-tests it. Hence: this type
// never holds a raw published pointer, and nothing in `adi::ui` publishes.
//
// WHERE THE SNAPSHOT COMES FROM: `engine::ProjectView` (ADR-0201).
//
// This type asked who owns the cell it reads and who calls
// `ProjectPublisher::collect()`. ADR-0201 answered it, and the answer dissolved
// the question rather than picking a side: NOTHING CROSSES THREADS. The audio
// thread never reads an `engine::Snapshot` -- it plays from the realised graph
// (ADR-0077) -- so there is no publisher here, no epoch and no `collect()`. An
// old snapshot lives exactly as long as the last `shared_ptr` to it, which is
// the frame that took it. That is precisely the lifetime this type already
// relied on, so the read half needed no change.
//
//   * `engine::ProjectView` holds the cell. `refresh(store)` rebuilds it with
//     structural sharing; `current()` is what a frame takes ONCE and hands to
//     every component. Construct readers from it: `SnapshotReader{view.current()}`.
//   * Nothing in `adi::ui` publishes, and this type still never names a
//     publisher. If a second thread ever needs a snapshot, ADR-0201 d5 says
//     `SnapshotPublisher` is the tool and it gets its own ADR.
//   * It is not the control read path. ADR-0181 d5 says every control reads the
//     parameter FEED, never the model directly. This is the MODEL path. The
//     feed is a separate message-thread coalescer built at 7.4, and this type
//     must not grow into it.
//
// WHAT ADR-0201 CLOSES, AND THE ONE THING IT DOES NOT
//
// ADR-0050 d3's rule -- one reference per frame, shared by every component --
// is CLOSED by ADR-0201 d2: a frame calls `current()` once and hands that one
// pointer down, so a timeline and a mixer in one frame cannot disagree. That
// was the failure d3 exists to prevent, and it is now structural.
//
// What remains is narrower and is NOT that failure. ADR-0180 d1 gives every
// WINDOW its own frame clock, so two windows drain at different moments and may
// briefly hold consecutive generations. Both read the same cell, so the skew is
// bounded by one refresh and resolves on the next drain. Whether a DAW should
// show two windows one generation apart is a question for the director about
// `ProjectView`, not about this type -- and `sameAs` keeps it observable rather
// than theoretical.

#pragma once

#include "adi/engine/snapshot.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace adi::ui {

/// One project snapshot, owned. A root and a refcount: the nodes are shared by
/// pointer, which is `snapshot.hpp`'s structural-sharing design.
using SnapshotPtr = std::shared_ptr<const engine::Snapshot>;

/// The model every component in one drain reads.
///
/// NEITHER COPYABLE NOR MOVABLE, DELIBERATELY. `UI-ARCHITECTURE.md` says no
/// component owns committed project state or "a cached copy of anything"; its
/// one refinement covers transient INTERACTION state that never survives commit
/// or cancel, which a retained project snapshot is not. Deleting both
/// constructors means a component CANNOT store one — the wrong thing does not
/// compile, rather than being forbidden by a comment nobody reads. It also
/// stops a stray copy pinning the whole node graph for as long as it lives.
///
/// Construct one per drain, pass it down by `const&`, let it go. A control
/// mid-drag keeps its own transient value and re-reads the model next drain.
class SnapshotReader {
public:
    explicit SnapshotReader(SnapshotPtr snapshot) noexcept : snapshot_(std::move(snapshot)) {}

    SnapshotReader(const SnapshotReader&) = delete;
    SnapshotReader& operator=(const SnapshotReader&) = delete;
    SnapshotReader(SnapshotReader&&) = delete;
    SnapshotReader& operator=(SnapshotReader&&) = delete;

    /// False before anything has been published. Every accessor below is safe
    /// to call on an invalid reader and returns an empty answer, because a UI
    /// that must branch on validity at every call site branches wrongly once.
    [[nodiscard]] bool valid() const noexcept { return snapshot_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /// The publication this reader is over. 0 when invalid. The publisher
    /// assigns it; nothing else may.
    [[nodiscard]] std::uint64_t seq() const noexcept { return snapshot_ ? snapshot_->seq : 0; }

    [[nodiscard]] int sampleRate() const noexcept {
        return snapshot_ ? snapshot_->sampleRate : 0;
    }

    [[nodiscard]] const engine::TempoMap* tempo() const noexcept {
        return snapshot_ ? snapshot_->tempo.get() : nullptr;
    }

    [[nodiscard]] const std::vector<std::shared_ptr<const engine::TrackNode>>& tracks() const noexcept {
        return snapshot_ ? snapshot_->tracks : empty();
    }

    [[nodiscard]] std::size_t trackCount() const noexcept { return tracks().size(); }

    /// Null for a track this publication does not have — including every track,
    /// when the reader is invalid.
    [[nodiscard]] const engine::TrackNode* findTrack(std::int64_t id) const noexcept {
        return snapshot_ ? snapshot_->findTrack(id) : nullptr;
    }

    [[nodiscard]] bool anySoloed() const noexcept {
        return snapshot_ && snapshot_->anySoloed();
    }

    /// The whole model, for a component that needs more than the accessors.
    /// Null when invalid. Borrowed: it is valid for this reader's lifetime and
    /// storing it defeats the point of the deleted copy constructor.
    [[nodiscard]] const engine::Snapshot* get() const noexcept { return snapshot_.get(); }

    /// True when both readers are over the SAME publication, by pointer.
    ///
    /// This is the one-snapshot-per-frame rule made checkable rather than
    /// asserted: every component in one drain must answer true against every
    /// other, and two windows that answer false are the skew named at the top
    /// of this file.
    [[nodiscard]] bool sameAs(const SnapshotReader& other) const noexcept {
        return snapshot_ == other.snapshot_;
    }

private:
    static const std::vector<std::shared_ptr<const engine::TrackNode>>& empty() noexcept {
        static const std::vector<std::shared_ptr<const engine::TrackNode>> none;
        return none;
    }

    SnapshotPtr snapshot_;
};

}  // namespace adi::ui
