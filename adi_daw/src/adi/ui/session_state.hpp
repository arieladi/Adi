// SPDX-License-Identifier: GPL-3.0-or-later
//
// Selection is session state (ADR-0021, UI-ARCHITECTURE §2): one per project, in
// Layer 3's `session_state`, never an op and never an input to one. The UI resolves
// it to literals when it builds an op; an op carries it only as the advisory
// `sel_before` of its transaction, so undo can put the selection back.
//
// One source of truth for every window. A window presents a copy per frame, as it
// presents one snapshot per frame, and writes go back through here.
#pragma once
#include "adi/ops.hpp"
#include <cstdint>
#include <string>
namespace adi {
class Store;
}
namespace adi::ui {
struct Selection {
    std::int64_t track = 0, clip = 0;
    std::int64_t start = 0, end = 0; // a time selection, in ticks; start == end is none
    std::int64_t insert = 0;         // the insert marker
    bool marked = false;             // the insert marker has been placed
    bool operator==(const Selection &) const = default;
};
class SessionState {
  public:
    explicit SessionState(Store &);
    const Selection &selection() const noexcept { return selection_; }
    // Changes whenever the selection does, so a window knows to present it again.
    std::uint64_t generation() const noexcept { return generation_; }
    // Always takes effect for this session; written to the file when it can be (a
    // read-only project keeps its selection in memory). False only on a write error.
    bool select(const Selection &, std::string &error);
    // The advisory blob for an op's `sel_before` (ADR-0021).
    Payload advisory() const;
    // After an undo: the selection the undone transaction recorded, if any.
    void restore(const Payload &);

  private:
    Store &store_;
    Selection selection_;
    std::uint64_t generation_ = 1;
};
} // namespace adi::ui
