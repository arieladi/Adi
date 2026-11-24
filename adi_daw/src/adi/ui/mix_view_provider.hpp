// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/snapshot.hpp"
#include <cstdint>
#include <memory>
#include <vector>
namespace adi::ui {
// The reference-channel policy shared by the Mix View and the rest of the window.
class MixViewProvider {
  public:
    virtual ~MixViewProvider() = default;
    // The ★ Reference track among the snapshot's tracks (ADR-0209 §4, d11; the `reference` role of
    // Q8's `tracks.role`), or 0 when none is. Called while painting: never allocates.
    virtual std::int64_t reference(const std::vector<std::shared_ptr<const engine::TrackNode>> &) const noexcept {
        return 0;
    }
};
} // namespace adi::ui
