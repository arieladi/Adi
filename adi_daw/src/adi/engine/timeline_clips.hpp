// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/store_rows.hpp"
#include <ranges>

namespace adi::engine {

// The arrangement plays the comp in the main lane by default. Editing and
// persistence still see the stored takes through Model::clips. The selection
// belongs here so audio, MIDI, envelopes and offline readers agree.
[[nodiscard]] inline auto timelineClips(const rows::Model& model) {
    return model.clips | std::views::filter([](const rows::Clip& clip) {
        return !clip.laneId.has_value();
    });
}

} // namespace adi::engine
