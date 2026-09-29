// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI's source of project state (ADR-0201): the one "frozen cell" a frame's
// SnapshotReader copies from.
//
// Everything here is MESSAGE THREAD. That is the whole design: ops commit on the
// message thread, the UI renders on it, and the audio thread never reads an
// engine::Snapshot (it plays from the graph, ADR-0077). So there is no
// publisher, no epoch, no collect(): an old snapshot lives exactly as long as
// the last shared_ptr to it, and a frame that took one keeps a consistent view
// while the next commit builds another.
//
// If a non-message thread ever needs to publish or read a Snapshot, this class
// is the wrong tool and SnapshotPublisher (publisher.hpp) is the right one; say
// so in an ADR first.

#pragma once

#include "adi/engine/snapshot.hpp"

#include <cstdint>
#include <memory>

namespace adi {
class Store;
}

namespace adi::engine {

class ProjectView {
public:
    /// Rebuild from the store, sharing every node that did not change with the
    /// current snapshot (SnapshotBuilder). Call after a load and after every
    /// committed op, undo and redo. Returns the new generation.
    std::uint64_t refresh(const Store& store);

    /// The current snapshot, or null before the first refresh. A frame takes
    /// this ONCE and hands the same pointer to every component (ADR-0050 d3).
    [[nodiscard]] std::shared_ptr<const Snapshot> current() const noexcept { return current_; }

    /// Increases by one on every refresh, so a component can tell "nothing
    /// changed" without comparing snapshots.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    std::shared_ptr<const Snapshot> current_;
    std::uint64_t generation_ = 0;
};

}  // namespace adi::engine
