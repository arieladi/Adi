// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace adi::ui {
// The Arrangement preview uses absolute musical ticks, including the end of a held note so far.
struct LiveMidiNote {
    std::int64_t start = 0, end = 0;
    int key = 0;
};

struct LiveMidiTake {
    std::int64_t track = 0, start = 0, end = 0;
    std::vector<LiveMidiNote> notes;
};
} // namespace adi::ui
