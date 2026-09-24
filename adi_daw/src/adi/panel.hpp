// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plug-in panel -- ADR-0150, ADR-0154.
//
// What a device's panel shows, and the Parameter List's search. Pure functions
// over what the plug-in declares and what the project recorded, so the UI
// (mac, step 7) and the agent ask the same question and get the same answer.
//
// LIVE'S RULE, from the manual (23.3.1, p.460): a plug-in with 64 or fewer
// modifiable parameters shows all of them as sliders; one with more opens with
// an empty panel and a prompt to configure it. Once the user configures a panel
// (`device.setPanel`), it shows exactly what they chose, in their order.
#pragma once

#include "adi/store_rows.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace adi::panel {

/// Live's threshold: "up to 64 modifiable parameters" are shown unasked.
inline constexpr std::size_t kDefaultLimit = 64;

/// One parameter the plug-in declares, as the panel needs it. `modifiable`
/// is the device contract's `automatable`: a read-only or hidden meter is not
/// a slider and does not count towards the 64.
struct Declared {
    std::string id;
    std::string name;
    bool modifiable = true;
};

struct Entry {
    std::string id;
    bool missing = false;   ///< configured, but the plug-in no longer declares it
};

struct Resolved {
    std::vector<Entry> entries;   ///< what the panel shows, in order
    bool configured = false;      ///< the project has a `device_panels` row
    /// Not configured and over the limit: the empty panel with Live's prompt,
    /// "To add plug-in parameters to this panel, click the Configure button".
    bool showsConfigureHint = false;
    std::size_t modifiableCount = 0;
};

/// The panel for `deviceId`, from the project's rows and what the plug-in
/// declares now. A placeholder (the plug-in is missing) declares nothing: its
/// configured entries all come back `missing`, and an unconfigured one is
/// empty without the hint, because there is nothing to configure from.
[[nodiscard]] Resolved resolve(const rows::Model& model, std::int64_t deviceId,
                               const std::vector<Declared>& declared);

/// The Parameter List (ADR-0150 d3): every declared parameter matching
/// `query`, best first. Every word of the query must appear in the name,
/// case-insensitively (the id matches only when the whole query is the id); a word that starts a word in the name ranks
/// above one found inside a word, an exact name above both, and among equals
/// the plug-in's own order wins. An empty query lists everything in that
/// order. `limit` 0 means no limit.
struct Match {
    std::size_t index = 0;   ///< into `declared`
    int score = 0;           ///< higher is better; for tests and tie-breaking
};
[[nodiscard]] std::vector<Match> search(const std::vector<Declared>& declared,
                                        std::string_view query, std::size_t limit = 0);

}  // namespace adi::panel
