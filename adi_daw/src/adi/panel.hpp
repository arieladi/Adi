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

// ---------------------------------------------------------------------------
// ADR-0198: the per-parameter record the feed sends (ADR-0181 d3).
// ---------------------------------------------------------------------------

/// How a control should be drawn. Mirrors `device::ParamShape`, deliberately
/// duplicated rather than included: this header is pure and JUCE-free, so it
/// compiles on every ABI the suite runs on, and the device layer adapts.
enum class Shape { Continuous, Switch, Menu };

/// WHICH SOURCE last moved a parameter -- ADR-0188 d4's "last touched wins".
///
/// The source that sent the latest change owns the parameter; a gesture from
/// another takes it over; and an absolute control that is NOT the owner
/// catches up by Takeover Mode rather than jumping. None of that can be
/// decided without knowing who moved it last, and the record is where the
/// several controls that share a parameter meet.
///
/// Here from the start, on win's instruction, because it is one field on a
/// record nobody writes to yet and a retrofit across every writer later.
enum class Source {
    None,          ///< nothing has touched it this session
    Mouse,         ///< a control in ADI's own UI
    Surface,       ///< a control surface (ADR-0181)
    Automation,    ///< a lane (ADR-0165)
    Modulation,    ///< a modulator (ADR-0046)
    Plugin,        ///< the user moved it in the plug-in's own window
    Agent,         ///< the AI agent, through an op
};

/// One parameter, as ADR-0181 d3 says the feed sends it.
struct Record {
    std::string id;
    std::string name;

    /// Normalized 0..1, the wire unit (ADR-0124).
    double stored = 0.0;
    /// What is actually sounding when it differs from `stored` -- a lane or a
    /// modulator is driving it. Equal to `stored` when nothing is.
    double playing = 0.0;
    /// True when `playing` is a different value from `stored`, so a UI can
    /// draw Bitwig's ring and Live's LED without comparing doubles itself.
    bool   driven = false;

    /// The PLUG-IN'S OWN text for `playing`, units and all. Empty when the
    /// plug-in offers none, and then the caller formats the number.
    std::string text;

    Shape shape = Shape::Continuous;
    std::int32_t stepCount = 0;   ///< steps including both ends; 0 if unknown

    /// A lane names this parameter (ADR-0165).
    bool automated = false;
    /// ...and the user has taken it over, so the lane is not being followed
    /// (ADR-0162). `overridden` without `automated` is not a state.
    bool overridden = false;
    /// The plug-in no longer declares it: kept, shown, and playing nothing
    /// (ADR-0177 d4, ADR-0179).
    bool missing = false;

    Source lastTouchedBy = Source::None;
};

/// Derive the fields a record computes from the ones a caller reads off a
/// device, and enforce the invariants. ADR-0198.
///
/// The caller fills `id`, `name`, `stored`, `playing`, `text`, `shape`,
/// `stepCount`, `automated`, `overridden`, `missing` and `lastTouchedBy` --
/// those come from the device contract and the session, which this header
/// cannot see, because it is pure and JUCE-free so that it builds on every
/// ABI the suite runs on. What is derived here is what would otherwise be
/// derived differently by each caller:
///
/// - **`driven`** is `playing` differing from `stored` by more than
///   `epsilon`. A UI must not compare two doubles itself and reach a
///   different answer from a surface doing the same comparison, which is the
///   disagreement ADR-0181 d3's single feed exists to prevent.
/// - **A MISSING parameter plays nothing** (ADR-0177 d4): `playing` is forced
///   to `stored` and `driven` to false. Its lane is kept and its stored value
///   is kept, and neither is sounding.
/// - **`overridden` without `automated` is not a state.** Override means a
///   user took a LANE over (ADR-0162); with no lane there is nothing to
///   override, and a record claiming it would light Live's Re-Enable button
///   for a parameter that has nothing to re-enable.
void finalize(std::vector<Record>& records, double epsilon = 1e-9);

/// ADI Airwindows (ADR-0171): which records belong to the ACTIVE algorithm.
///
/// A suite declares every algorithm's parameters at once, with fixed ids, so
/// that a lane never silently retargets when the algorithm changes
/// (`plugins/airwindows/Source/airwindows_clap.hpp`): parameter 0 is the
/// Algorithm, 1 is Auto Gain, and algorithm `a`'s parameter `k` is
/// `100 + 64a + k`. A panel that showed all of them would show every
/// algorithm's controls at once -- 147 algorithms' worth on the Distortion
/// suite.
///
/// Returns indices into `records`, in their original order: the two shared
/// parameters first, then the active algorithm's own.
///
/// `algorithm` is the Algorithm parameter's VALUE, not an index into anything
/// else. A record whose id is not a number, or is outside every algorithm's
/// block, is treated as shared and kept -- the conservative answer, because
/// hiding a control the user needs is worse than showing one they do not.
[[nodiscard]] std::vector<std::size_t> activeAlgorithmParams(
    const std::vector<Record>& records, std::int32_t algorithm);

/// The scheme's constants, named once so a caller cannot drift from the
/// plug-in. They match `airwindows_clap.hpp` and are asserted against it.
inline constexpr std::int64_t kAwAlgorithmId  = 0;
inline constexpr std::int64_t kAwAutoGainId   = 1;
inline constexpr std::int64_t kAwParamBase    = 100;
inline constexpr std::int64_t kAwParamStride  = 64;

}  // namespace adi::panel
