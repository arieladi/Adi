// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace juce { class File; }

namespace adi::backup {

// Recycle one expired backup, leaving it in place when recycling is unavailable.
[[nodiscard]] bool moveToTrash(const juce::File&);

}  // namespace adi::backup
