// SPDX-License-Identifier: GPL-3.0-or-later
//
// The immutable project model the audio thread reads. ADR-0010, ADR-0019.
//
// This is NOT the `.adi` and NOT the SQLite row set. It is what the audio thread
// is allowed to see: plain data, already resolved, with no handles to anything
// that could block. The store loads into this; the audio thread never reaches
// past it.
//
// STRUCTURAL SHARING IS THE POINT, NOT AN OPTIMISATION
//
// ADR-0019 rejected "rebuild the world on every edit" because it is O(project)
// per publication, which reintroduces inside the engine exactly the cost
// ADR-0001 exists to avoid in the file. A 200-track project does not survive
// rebuilding every track because one fader moved.
//
// So nodes are shared_ptr<const>, and a new snapshot reuses every node that did
// not change. Editing one clip copies the path from root to that clip — its
// track, and the snapshot — and shares the other 199 tracks by pointer. The
// snapshot is then a cheap root, and publication costs what the edit cost.
//
// Everything here is `const` after construction. Not by convention: the audio
// thread holds a `const T*` and the builders return `shared_ptr<const>`, so
// mutating a live snapshot does not compile.

#pragma once

#include "adi/engine/publisher.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adi {
class Store;
}

namespace adi::engine {

/// A tempo event, resolved. SPEC §4.4.
struct TempoEvent {
    std::int64_t posTicks = 0;
    double bpm = 120.0;
    int curve = 0;

    // Compared by value so a rebuilt tempo map that happens to be identical can
    // be shared with the previous snapshot rather than replacing it.
    friend bool operator==(const TempoEvent&, const TempoEvent&) = default;
};

/// The tempo map, shared between snapshots whenever it has not changed — which
/// is almost always, since tempo edits are rare and fader moves are not.
struct TempoMap {
    std::vector<TempoEvent> events;   // sorted by posTicks, always has one at 0

    /// Ticks to seconds, integrating the map. Pure and allocation-free, so the
    /// audio thread may call it.
    [[nodiscard]] double ticksToSeconds(std::int64_t ticks) const noexcept;
    [[nodiscard]] std::int64_t secondsToTicks(double seconds) const noexcept;
    [[nodiscard]] double bpmAt(std::int64_t ticks) const noexcept;
};

struct ClipNode {
    std::int64_t id = 0;
    std::int64_t posTicks = 0;
    std::int64_t lengthTicks = 0;
    bool muted = false;
    float gainDb = 0.0f;
};

struct TrackNode {
    std::int64_t id = 0;
    std::string name;            // owned; the audio thread only reads it
    bool muted = false;
    bool soloed = false;
    float volumeDb = 0.0f;
    float pan = 0.0f;
    std::vector<std::shared_ptr<const ClipNode>> clips;
};

/// What the audio thread sees. Derives from Sequenced so the publisher can
/// stamp it; nothing else touches `seq`.
struct Snapshot : Sequenced {
    std::shared_ptr<const TempoMap> tempo;
    std::vector<std::shared_ptr<const TrackNode>> tracks;
    int sampleRate = 48000;

    [[nodiscard]] const TrackNode* findTrack(std::int64_t id) const noexcept;
    [[nodiscard]] bool anySoloed() const noexcept;
};

/// Builds snapshots, reusing nodes from the previous one.
///
/// Kept as a class rather than a free function because the reuse needs the
/// previous snapshot, and threading that through every call site is how someone
/// eventually passes the wrong one and silently loses sharing — the failure
/// being slower, not wrong, which is the kind nobody notices.
class SnapshotBuilder {
public:
    /// Reads the whole project. Use for the first snapshot, or after a change
    /// too broad to describe.
    static std::unique_ptr<Snapshot> fromStore(const Store&);

    /// Reads the project but reuses every node from `previous` that is
    /// unchanged. Returns a snapshot sharing structure with it.
    static std::unique_ptr<Snapshot> fromStore(const Store&, const Snapshot& previous);

    /// How many nodes the two snapshots share by pointer. For tests: the sharing
    /// claim above is checkable, so it is checked.
    static std::size_t sharedNodeCount(const Snapshot&, const Snapshot&);
};

using ProjectPublisher = SnapshotPublisher<Snapshot>;

}  // namespace adi::engine
