// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/project_view.hpp"

#include "adi/store.hpp"

namespace adi::engine {

std::uint64_t ProjectView::refresh(const Store& store) {
    // Build the next one from the previous so unchanged tracks are shared by
    // pointer, then swap. A frame still holding the previous snapshot keeps it.
    std::shared_ptr<const Snapshot> next =
        current_ ? SnapshotBuilder::fromStore(store, *current_) : SnapshotBuilder::fromStore(store);
    current_ = std::move(next);
    return ++generation_;
}

}  // namespace adi::engine
