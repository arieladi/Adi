// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// STACK's model (the Mix View's STACK tab, ADR-0209): which tracks are rows and where each row,
// chain item, module cell, chip, chevron and navigator line is; how tall every row is at this
// moment; what a point hits; and the interaction states that move them, each kept apart: hover,
// keyboard focus, the selection (read, never inferred), the search, the pins, the comparison pair,
// Max and the capture of a target being edited (UI-ARCHITECTURE §2).
//
// Pure and JUCE-free. It never reads a clock: every event carries a time in milliseconds and the
// owner calls advance(now), so results do not depend on the frame rate. No threads, no randomness,
// no allocation in a frame or in pointer, wheel, key and advance events once the rows are set (the
// exceptions are named where they are). It never edits the project: every event answers Effects
// that the owner carries out (select a track, rename one, show its devices, close the Mix View).
//
// A row holds the track's chain (its devices in chain order) and the cells of its channel
// processor: the modules the processor runs (today ADI-Q, ducking, Clip and ADI-L), each Absent,
// Dormant or Engaged. How many modules there are, their kinds and their order come from the input,
// never from this file. The modules draw as aligned columns (one x for every row) or as one
// combined card per row; both take their width from the same width budget.
//
// Numbers: every tunable lives in stack::Constants with its source. The fisheye's ratios and the
// width exponent are measured from the director's video (ADR-0209); every other number is win's
// placement, provisional until the experiment named beside it.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace adi::ui {
class SnapshotReader;
struct ArrangementGeometry;
class MixViewProvider;
namespace stack {
using TrackId = std::int64_t;  // tracks.id
using DeviceId = std::int64_t; // devices.id
using TimeMs = std::int64_t;   // one monotonic 64-bit millisecond count, the same for every event and frame

inline constexpr std::size_t kMaxModules = 8;   // a capacity: modules one channel processor can show
inline constexpr std::size_t kMaxEntries = 32;  // a capacity: rows away from their rest height at once
inline constexpr std::size_t kMaxLaneAnims = 8; // a capacity: chain lanes scrolling at once
inline constexpr TimeMs kNever = std::numeric_limits<TimeMs>::max();

enum class ModuleState : std::uint8_t {
    Absent,  // refused on this track (the track kind cannot have it); drawn as refused
    Dormant, // present and off
    Engaged
};
// A width class, not a product list: which w100 and fold tier an item or module gets.
enum class ItemClass : std::uint8_t { AdiQ, Ducking, Clip, AdiL, AdiStill, Eq, Dynamics, Analyser, View, Plugin, Missing };
inline constexpr std::size_t kItemClasses = 11;
// What an item shows depends only on its own current height (T0 to T5), or Max.
enum class Tier : std::uint8_t { T0, T1, T2, T3, T4, T5, Max };
namespace role {
inline constexpr std::uint8_t Kick = 1, Bass = 2, Vocal = 4;
}
enum class Easing : std::uint8_t { CubicOut, QuadOut, Linear };
// How the rows stay still under a resting pointer while heights animate.
enum class Anchor : std::uint8_t { PointerRowTop, PointerRowFraction, ListTop };
enum class MaxMode : std::uint8_t { ListMinusGaps, Fixed };
// When the width budget fits: the lane is not negative, or the lane can show items.
enum class FitRule : std::uint8_t { LaneNonNegative, LaneShowsItems };

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const noexcept;
    bool operator==(const Rect &) const = default;
};

// Every tunable number, one comment each. Absolute sizes are logical pixels at 100 % interface
// zoom; scaled(k) multiplies every absolute size by k, rounding half up, and keeps the ratios, the
// exponent, the times and the bounds, so a measurement of k changes one call.
struct Constants {
    // --- the base height (the toolbar's row-size slider) ---
    int baseMin = 28;      // smallest base row; win's placement, provisional until experiment X1
    int baseMax = 184;     // largest base row; win's placement, provisional until experiment X1
    int baseDefault = 100; // a new window's base row; win's placement, provisional until experiment X1
    std::array<int, 6> detents{28, 44, 68, 100, 136, 184}; // the slider's detents; win's placement, provisional until experiment X1
    int detentSnap = 3; // a slider value this close to a detent lands on it; win's placement, provisional until experiment X1
    // --- the fisheye ---
    int enlargedPct = 190; // the enlarged row, % of base; measured from the director's video (ADR-0209)
    int near1Pct = 129;    // a row next to it, % of base; measured from the director's video (ADR-0209)
    int near2Pct = 110;    // two rows away, % of base; measured from the director's video (ADR-0209)
    int gapPct = 31;       // a gap, % of the smaller of its two rows; measured from the director's video (ADR-0209)
    int gapMin = 6;        // the smallest gap; win's placement, provisional until experiment X1
    double widthExponent = 0.68; // widths grow as height^0.68; measured from the director's video (ADR-0209)
    int widthRefHeight = 100;    // the height at which a class has its w100 width; win's placement, provisional until experiment X1
    int enlargedCapBaseRows = 2; // the enlarged row leaves at least this many base rows of the list; win's placement, provisional until experiment X2
    // --- Max ---
    MaxMode maxMode = MaxMode::ListMinusGaps; // Max fills the list but its two gaps; win's placement, provisional until experiment X2
    int maxGaps = 2;    // gaps Max leaves in the list; win's placement, provisional until experiment X2
    int maxFixed = 240; // Max's height in the Fixed variant; win's placement, provisional until experiment X2
    // --- timing ---
    int heightAnimMs = 160;            // heights and gaps animate this long; win's placement, provisional until experiment X2
    Easing easing = Easing::CubicOut;  // their curve, 1 - (1 - u)^3; win's placement, provisional until experiment X2
    int dwellMs = 60;                  // the pointer stays this long in another row before it is enlarged; win's placement, provisional until experiment X2
    int scrollHoldOffMs = 120;         // hover waits this long after the last scroll; win's placement, provisional until experiment X2
    int leaveMs = 300;                 // rows go back to base this long after the pointer leaves; win's placement, provisional until experiment X2
    int revealMs = 120;                // an enlarged row cut off at the bottom scrolls into view over this; win's placement, provisional until experiment X2
    Anchor anchor = Anchor::PointerRowTop; // what holds still under the pointer: "the content point under the pointer" is read as the pointer's row's top (PointerRowFraction holds the pointer's fraction of the row instead); win's placement, provisional until experiment X2
    // --- tiers ---
    std::array<int, 5> tierFloors{44, 68, 100, 136, 184}; // the lowest height of T1 to T5; win's placement, provisional until experiment X1
    Tier chipMaxesUpTo = Tier::T2; // a chip click maximises while the BASE row is at or below this tier (the clicked row's own tier would make the click depend on whether its dwell had elapsed); win's placement, provisional until experiment X2
    // --- the chip column ---
    int chipColumn = 128;    // the name column; win's placement, provisional until experiment X1
    int chipHeight = 18;     // the chip; win's placement, provisional until experiment X1
    int chipInset = 6;       // the chip's inset in its column; win's placement, provisional until experiment X1
    int chipIndent = 8;      // the chip's indent per group level; win's placement, provisional until experiment X1
    int chipMinWidth = 40;   // the indent never narrows the chip below this; win's placement, provisional until experiment X1
    int chipButton = 16;     // the pin, collision-reference and Kick buttons; win's placement, provisional until experiment X1
    int chipButtonGap = 4;   // between those buttons and from the chip; win's placement, provisional until experiment X1
    int chipButtonClear = 2; // room left above the buttons before they move beside the chip; win's placement, provisional until experiment X1
    int chipButtonDy = 1;    // the buttons' drop beside the chip; win's placement, provisional until experiment X1
    int foldInsetX = 2;      // a group's fold triangle inside the chip, from its left; win's placement, provisional until experiment X1
    int foldInsetY = 3;      // and from its top; win's placement, provisional until experiment X1
    int foldSize = 12;       // the fold triangle's box; win's placement, provisional until experiment X1
    // --- widths, by ItemClass: AdiQ, Ducking, Clip, AdiL, AdiStill, Eq, Dynamics, Analyser, View, Plugin, Missing ---
    std::array<int, kItemClasses> w100{400, 260, 240, 300, 360, 320, 300, 280, 240, 240, 240}; // engaged width at widthRefHeight; win's placement, provisional until experiment X1
    std::array<int, kItemClasses> w100Dormant{240, 260, 240, 300, 360, 320, 300, 280, 240, 240, 240}; // a column no row engages; win's placement, provisional until experiment X1
    std::array<Tier, kItemClasses> foldBelow{Tier::T0, Tier::T0, Tier::T0, Tier::T0, Tier::T2, Tier::T0,
                                             Tier::T0, Tier::T1, Tier::T0, Tier::T3, Tier::T0}; // a chain item folds to a pill below this tier; win's placement, provisional until experiment X10
    int chainItemMin = 120;     // the narrowest unfolded chain item; win's placement, provisional until experiment X1
    int pill = 22;              // a folded item or a dormant or refused module pill; win's placement, provisional until experiment X1
    int itemGap = 10;           // between items, columns and the row's parts; win's placement, provisional until experiment X1
    int trayPillGap = 4;        // between pills in the dormant tray and in a pill lane; win's placement, provisional until experiment X1
    int cardGap = 6;            // between module cells inside the combined card; win's placement, provisional until experiment X1
    int laneMinForItems = 140;  // a narrower chain lane shows pills only; win's placement, provisional until experiment X1
    int columnStepPct = 85;     // the module widths' first step when the budget is short; win's placement, provisional until experiment X1
    FitRule fit = FitRule::LaneNonNegative; // what "fits" means for the budget's steps; win's placement, provisional until experiment X1
    int proposedBase = 68;      // the base row proposed when even the last step is short; win's placement, provisional until experiment X1
    int chevronW = 20;          // a lane chevron; win's placement, provisional until experiment X1
    int chevronH = 24;          // a lane chevron; win's placement, provisional until experiment X1
    int laneScrollMs = 120;     // a chevron's scroll by one item; win's placement, provisional until experiment X2
    int laneWheelPx = 60;       // sideways trackpad travel per chain item; win's placement, provisional until experiment X1
    // --- the navigator ---
    int navigatorWidth = 132;    // its width with its margin; win's placement, provisional until experiment X3
    int navigatorMargin = 8;     // the margin on its left; win's placement, provisional until experiment X3
    int navigatorCollapsed = 28; // collapsed to colour bars; win's placement, provisional until experiment X3
    int navLine = 14;            // one line per row; win's placement, provisional until experiment X3
    int navHeader = 16;          // the scope's header line; win's placement, provisional until experiment X3
    int navIndent = 8;           // indent per group level; win's placement, provisional until experiment X3
    int navBarW = 3;             // a line's colour bar; win's placement, provisional until experiment X3
    int navBarH = 11;            // a line's colour bar; win's placement, provisional until experiment X3
    int navJumpTop = 8;          // a jump puts its row this far below the list's top; win's placement, provisional until experiment X3
    int navJumpMs = 140;         // and takes this long; win's placement, provisional until experiment X3
    int dragThreshold = 3;       // a navigator press that moves further is a frame drag; win's placement, provisional until experiment X3
    // --- the landing (opened from a processor panel's chip) ---
    int landingHoldMs = 1500; // the landed row stays enlarged this long; win's placement, provisional until experiment X8
    int landingNum = 1;       // the landed row's top at landingNum / landingDen of the list; win's placement, provisional until experiment X8
    int landingDen = 3;       // see landingNum; win's placement, provisional until experiment X8
    bool followSelection = false; // a selection made elsewhere moves STACK's focus; win's placement, provisional until experiment X8
    // --- virtualisation ---
    int realisedMarginPct = 50;  // rows realised beyond each edge, % of the list's height (one screen in all); win's placement, provisional until experiment X1
    int subscribeMarginPx = 48;  // audio-costly subscriptions follow the visible rows plus this; win's placement, provisional until experiment X1
    // --- bounds (view state and the clock) ---
    int searchMaxBytes = 128;  // a bound: the search text
    int maxPins = 4096;        // a bound: pinned tracks
    int maxFolded = 4096;      // a bound: groups folded in STACK
    int maxPinSets = 32;       // a bound: stored pin sets
    int maxPinSetName = 64;    // a bound: a pin set's name, bytes
    int maxRanges = 4096;      // a bound: display-range entries
    int clockRebaseMs = 1000;  // a bound: a clock that goes back further than this is rebased, not frozen

    // Every absolute size times k, rounded half up; ratios, the exponent, times and bounds kept.
    Constants scaled(double k) const;
    bool operator==(const Constants &) const = default;
};

// --- inputs ---------------------------------------------------------------------------------------

// One module of the channel processor, in processing order. The key is stable across snapshots and
// is stored in view state: a provider never renumbers it.
struct ModuleKindInput {
    std::uint32_t key = 0;                  // non-zero, unique in the list
    std::string_view name;                  // its short name ("ADI-Q"); copied
    std::string_view search;                // the words a search finds it by while engaged; copied
    ItemClass widthClass = ItemClass::View; // which width the constants give it
    bool keepsDormantColumn = false;        // its column stays while no row engages it
    bool dormantPreview = false;            // a dormant cell draws a dim preview, not a pill
};
// One module of one track. `instance` names this instance of the module: the provider changes it
// when the module is reset, replaced or re-created, so an edit can never continue onto the new one.
// Several modules may share the processor's one device; `device` addresses edits and is never
// identity. The processor's own device is never also an ItemInput.
struct ModuleInput {
    ModuleState state = ModuleState::Absent;
    std::uint64_t instance = 0; // required (non-zero) unless Absent
    DeviceId device = 0;
};
// One device of the track's chain, in chain order. `identity` (required, non-zero) is what makes
// it the same instance (today the plug-in reference and uid; an engine instance token once there
// is one): a new identity under the same device id is a replacement.
struct ItemInput {
    DeviceId device = 0; // > 0
    std::uint64_t identity = 0;
    ItemClass cls = ItemClass::View;
    std::string_view name; // for the search; copied into the haystack
    bool available = true, enabled = true; // availability is distinct from the Device Activator
};
// One track of the snapshot. The owner passes every track (not only the shown ones) in
// TrackOrder's order (ArrangementGeometry::rows); the order is never stored.
struct TrackInput {
    TrackId id = 0;     // > 0, unique
    TrackId parent = 0; // the group it sits in; 0 at the top
    std::uint64_t identity = 0; // required: the track kind today, a creation token later; a change replaces the row
    std::uint64_t revision = 0; // 0, or a token that changes whenever anything below changes (skips the deep compare)
    std::uint32_t depth = 0;    // groups it sits in
    bool group = false, main = false, ret = false;
    bool inView = true;     // the window's project view shows it
    bool reference = false; // the Reference track: never a row (ADR-0209 d11)
    std::int32_t colour = -1; // Live's palette index 0..69, -1 for none (ADR-0211)
    std::uint8_t roles = 0;   // role:: bits
    std::string_view name;    // copied
    std::uint32_t itemBegin = 0, itemCount = 0; // its chain in RowsInput::items
    // Presentation membership supplied by the owner, independent of bypass or
    // runtime availability. The full track remains for ancestry and key roles.
    bool hasMixFunction = false;
};
// What setRows reads. `modules` holds tracks.size() x kinds.size() entries, track by track.
// Call setRows when `generation` (ProjectView::generation) or the provider's revision moves: the
// same pair again (non-zero) returns at once.
struct RowsInput {
    std::span<const ModuleKindInput> kinds;
    std::span<const TrackInput> tracks;
    std::span<const ItemInput> items;
    std::span<const ModuleInput> modules;
    std::uint64_t generation = 0;
    std::uint64_t providerRevision = 0;
};

// --- refs -----------------------------------------------------------------------------------------

// A row by track id and generation. A generation is never reused: a ref to a removed or replaced
// row stays dead, even when the id comes back.
struct RowRef {
    TrackId track = 0;
    std::uint32_t gen = 0; // 0: no row
    bool valid() const noexcept { return gen != 0; }
    bool operator==(const RowRef &) const = default;
};
enum class ItemPlace : std::uint8_t { None, Chain, Module }; // None: the row itself (its chip)
struct ItemRef {
    RowRef row;
    ItemPlace place = ItemPlace::None;
    std::uint32_t module = 0; // Module: the kind's key
    DeviceId device = 0;      // Chain: the device; Module: its device as last seen (not identity)
    std::uint32_t gen = 0;    // Chain and Module: the item's generation
    bool operator==(const ItemRef &) const = default;
};
// What openFromChip lands on: a module of the track's processor, a chain device (a single ADI-Q or
// ADI-L inserted as an ordinary device), or with neither the row.
struct ItemKey {
    std::uint32_t module = 0;
    DeviceId device = 0;
};
// What a live ItemRef addresses now (resolve()).
struct Resolved {
    DeviceId device = 0;
    std::uint64_t identity = 0; // Chain: the identity; Module: the instance
    ModuleState state = ModuleState::Absent;
    ItemClass cls = ItemClass::View;
    bool available = true, enabled = true;
};

// --- events and effects ---------------------------------------------------------------------------

struct Mods {
    bool shift = false, ctrl = false, alt = false;
};
enum class Button : std::uint8_t { Left, Right, Middle };
// notches for a mouse wheel, pixels for a trackpad; positive is down (and right). W2 converts.
struct WheelDelta {
    double notches = 0;
    double pixelsX = 0, pixelsY = 0;
};
enum class Key : std::uint8_t { Up, Down, Left, Right, Enter, Escape, Home, End, PageUp, PageDown, Tab, Rename };
enum class EscapeStep : std::uint8_t { None, Capture, RestoreMax, ClearSearch, CloseMixView };
enum class CaptureStatus : std::uint8_t {
    Live,
    Ended,        // ended by its owner (or the model as asked, see Effects::captureEnded)
    RowRemoved,   // its track left the rows
    RowReplaced,  // its track's identity changed
    ItemRemoved,  // its chain device or module left
    ItemReplaced, // its device's identity or its module's instance changed
    Behind        // the project moved on since the last setRows: refresh, then ask again
};
enum class HitPart : std::uint8_t {
    None, Above, Gap, Below, RowSpace, // RowSpace: inside a row, on nothing
    Chip, ChipFold, ChipPin, ChipCollision, ChipKick,
    ChainItem, ChainPill, ChevronLeft, ChevronRight,
    ModuleItem, ModulePreview, ModulePill, ModuleRefused,
    NavigatorHeader, NavigatorLine, NavigatorEmpty
};
struct Hit {
    HitPart part = HitPart::None;
    RowRef row;
    ItemRef item;
    std::size_t shown = 0; // the row's (or navigator line's) shown index
    Rect rect;             // the part's rectangle, view space
    Tier tier = Tier::T3;
    std::size_t kindIndex = 0; // a module part: its index in the kinds list
    bool operator==(const Hit &) const = default;
};
// What an event asks of the owner. The model writes nothing.
struct Effects {
    bool consumed = false;          // the key or wheel was STACK's
    bool repaint = false;           // something drawn changed (also visible through epoch())
    bool takeFocus = false;         // a press inside STACK: give it keyboard focus
    bool refused = false;           // the command was refused (a stale ref, a capture, a bound)
    bool windowViewChanged = false; // persist the per-window view state
    bool sharedViewChanged = false; // persist pins, pin sets, the comparison pair (project scope)
    TrackId select = 0;             // select this track in Live's one selection
    RowRef showDevices;             // a double-click on a chip: show its devices in the Device View
    RowRef rename;                  // rename this row's track (the owner begins a Text capture)
    EscapeStep escape = EscapeStep::None;
    CaptureStatus captureLost = CaptureStatus::Live; // the model ended the capture: cancel the gesture and say why
    std::uint32_t captureEnded = 0; // the token of a capture the model ended as a key asked: commit its field
    Hit hit;                        // pointerDown: what was pressed, for W2 to dispatch what W1 does not own
};

// --- layout outputs -------------------------------------------------------------------------------

struct Span {
    std::size_t first = 0, last = 0; // inclusive shown indices
    bool empty = true;
    std::size_t size() const noexcept { return empty ? 0 : last - first + 1; }
    bool contains(std::size_t i) const noexcept { return !empty && i >= first && i <= last; }
    bool operator==(const Span &) const = default;
};
struct RowGeo {
    RowRef ref;
    std::size_t shown = 0;
    double top = 0, height = 0, targetHeight = 0; // view space
    Tier tier = Tier::T3, targetTier = Tier::T3;  // by the current and the resting height
    std::uint32_t depth = 0;
    std::int32_t colour = -1;
    std::uint8_t roles = 0;
    bool group = false, folded = false, animating = false, enlarged = false, maximised = false, focused = false,
         selected = false, pinned = false, pointer = false, captured = false,
         retained = false, // shown only because it is captured
         jumpTarget = false, pairA = false, pairB = false;
    bool operator==(const RowGeo &) const = default;
};
enum class LaneMode : std::uint8_t { Items, Pills, None };
enum class ModuleLayout : std::uint8_t { Columns, Card };
enum class RowLayout : std::uint8_t { ChainFirst, ProcessorFirst };
enum class ModulePlace : std::uint8_t { Hidden, Column, Tray, Card };
enum class CellContent : std::uint8_t { Hidden, Item, DormantPreview, DormantPill, Refused, Blank };
struct LaneGeo {
    double x = 0, width = 0, offset = 0; // offset: the lane's scroll, px
    LaneMode mode = LaneMode::Items;
    std::uint32_t count = 0;      // chain items in the row
    std::uint32_t first = 0;      // the lane's first item
    std::uint32_t drawnFirst = 0; // the items forEachChainItem returns
    std::uint32_t drawnCount = 0;
    bool left = false, right = false;
    Rect leftChevron, rightChevron;
    bool frozen = false; // the captured row's lane, held as it was
    bool operator==(const LaneGeo &) const = default;
};
struct ItemGeo {
    ItemRef ref;
    ItemClass cls = ItemClass::View;
    std::uint32_t index = 0; // in the chain
    double x = 0, width = 0; // view space; the row's top and height are the item's
    Tier tier = Tier::T3;
    bool pill = false, clipped = false, focused = false;
    bool available = true, enabled = true;
    bool operator==(const ItemGeo &) const = default;
};
struct CellGeo {
    std::size_t kindIndex = 0;
    std::uint32_t key = 0;
    ModulePlace place = ModulePlace::Hidden;
    CellContent content = CellContent::Hidden;
    double x = 0, width = 0;
    bool pill = false, focused = false;
    ItemRef ref; // gen 0 when there is no module to address (Refused, Blank, Hidden)
    bool operator==(const CellGeo &) const = default;
};
struct ChipGeo {
    Rect chip, fold, pin, collision, kick;
    bool buttonsAbove = true;
    bool operator==(const ChipGeo &) const = default;
};
// The row's columns, the same for every row; recomputed on events, never per frame.
struct ProcessorPlan {
    double chipW = 0;
    LaneMode laneMode = LaneMode::Items;
    double laneX = 0, laneW = 0;
    ModuleLayout layout = ModuleLayout::Columns;
    std::size_t count = 0;                        // module kinds in the list
    std::array<ModulePlace, kMaxModules> place{}; // per kind index
    std::array<double, kMaxModules> x{}, w{};     // Column and Tray: x and width; Card: the cell's widest
    double processorX = 0, processorW = 0;        // the columns and tray, or the card
    double trayX = 0, trayW = 0;
    double rowsRight = 0, navX = 0, navW = 0;
    std::uint8_t budgetStep = 0; // 0 fits as is; 1 modules narrowed; 2 navigator collapsed too; 3 does not fit
    double factor = 1;
    double shortBy = 0;
    bool proposeBase = false, proposeProcessorFirst = false;
    bool operator==(const ProcessorPlan &) const = default;
};
// What planProcessor needs to know of one module kind across the scope.
struct KindScope {
    ItemClass widthClass = ItemClass::View;
    bool keepsDormantColumn = false;
    bool engaged = false; // some row in the scope has it engaged
    bool dormant = false; // some row in the scope has it dormant
};
struct NavGeo {
    bool shown = false, collapsed = false;
    Rect area, header, lines, frame;
    double scroll = 0;
    Span lineSpan; // the lines inside `lines`
    bool operator==(const NavGeo &) const = default;
};
struct NavLine {
    RowRef ref;
    double y = 0, indent = 0;
    std::int32_t colour = -1;
    std::uint8_t roles = 0;
    bool onScreen = false, group = false, pinned = false, hit = false, jumpTarget = false, pairA = false,
         pairB = false, pairPending = false;
    bool operator==(const NavLine &) const = default;
};

// --- the capture ----------------------------------------------------------------------------------

// What keeps an edited target still: a pointer drag on a knob or a band, a text field (a typed
// value, a rename), an open menu.
enum class CaptureKind : std::uint8_t { Drag, Text, Menu };
struct CaptureRef {
    std::uint32_t token = 0;
    CaptureKind kind = CaptureKind::Drag;
    ItemRef target;
    bool operator==(const CaptureRef &) const = default;
};
enum class CaptureRefusal : std::uint8_t { None, Busy, Stale, NotShown, Unavailable };
struct CaptureResult {
    CaptureRef ref;
    CaptureRefusal refusal = CaptureRefusal::None;
};

// --- view state (ui_view) -------------------------------------------------------------------------

enum class SearchMode : std::uint8_t { Filter, Jump };
enum class ChannelScope : std::uint8_t { WithTools, AllChannels };
enum class RowSpectrum : std::uint8_t { ProcessorInput, Off, TrackInput }; // carried for W5
enum class CollisionRule : std::uint8_t { RolePartner, OpenedFrom, Focused };
enum class Look : std::uint8_t { Live, Dark };                              // carried for W2
enum class DisplayRange : std::uint8_t { Auto, Db3, Db6, Db12, Db30 };      // carried for W6
struct ScrollAnchor {
    TrackId track = 0;
    double offset = 0;
    bool operator==(const ScrollAnchor &) const = default;
};
struct ComparePair {
    TrackId a = 0, b = 0;
    bool operator==(const ComparePair &) const = default;
};
struct PinSet {
    std::string name;
    std::vector<TrackId> ids;
    bool operator==(const PinSet &) const = default;
};
// A display range: module != 0 names a module of the track's processor, else `device` a chain device.
struct RangeEntry {
    TrackId track = 0;
    std::uint32_t module = 0;
    DeviceId device = 0;
    DisplayRange range = DisplayRange::Auto;
    bool operator==(const RangeEntry &) const = default;
};
// Per window, beside WindowState's Mix View fields. Keys in ui_view, under mixview.stack., each
// named as its field: baseHeight, autoZoom, navigator, navigatorCollapsed, filterPinned, search,
// searchMode, layout, moduleLayout, showChain, showDormant, rowSpectrum, collisionRule, overlay,
// look, scroll, moduleRange, folded, channelScope. W2's store writes these names, no translation between.
struct WindowViewState {
    int baseHeight = 100;
    bool autoZoom = true, navigator = true, navigatorCollapsed = false, filterPinned = false;
    std::string search;
    SearchMode searchMode = SearchMode::Filter;
    RowLayout layout = RowLayout::ChainFirst;
    ModuleLayout moduleLayout = ModuleLayout::Columns;
    bool showChain = true, showDormant = true;
    RowSpectrum rowSpectrum = RowSpectrum::ProcessorInput;
    CollisionRule collisionRule = CollisionRule::RolePartner;
    bool overlay = false;
    Look look = Look::Live;
    ScrollAnchor scroll;
    std::vector<RangeEntry> moduleRange; // sorted by (track, module, device)
    std::vector<TrackId> folded;         // STACK's own group folds, sorted
    ChannelScope channelScope = ChannelScope::WithTools;
    bool operator==(const WindowViewState &) const = default;
};
// Project scope, shared by the project's windows: pins, pinSets, mixview.comparePair.
struct SharedViewState {
    std::vector<TrackId> pins; // sorted; ids of deleted tracks are kept
    std::vector<PinSet> pinSets;
    ComparePair comparePair;
    bool operator==(const SharedViewState &) const = default;
};
// Never in it: hover, the enlarged row, Max, focus, captures, jumps, the landing; nor the order,
// which is the input's.
struct ViewState {
    WindowViewState window;
    SharedViewState shared;
    ViewState normalised(const Constants &) const;
    bool operator==(const ViewState &) const = default;
};

// --- work counters --------------------------------------------------------------------------------

struct Stats {
    std::uint64_t rowEvaluations = 0;  // rows a query computed or stepped over
    std::uint64_t entryVisits = 0;     // rows-away-from-rest entries a query touched
    std::uint64_t itemEvaluations = 0; // chain items and module cells computed
    std::uint64_t lineEvaluations = 0; // navigator lines computed
    std::uint64_t fullPasses = 0;      // O(rows) passes: a structural setRows, a filter, pins-only, folds, view state
    std::uint64_t rowCompares = 0;     // setRows' cheap per-track compare (O(rows) per call, by design)
    std::uint64_t rowUpdates = 0;      // tracks setRows found changed
    std::uint64_t haystackBuilds = 0;  // search texts folded
    std::uint64_t shownEdits = 0;      // single rows added to or removed from the shown list
    std::uint64_t ignoredInputs = 0;   // bad input refused and counted
    std::uint64_t entrySnaps = 0;      // animations snapped to rest for want of capacity
    std::uint64_t clockRebases = 0;    // a clock that went back
};
struct Focus {
    RowRef row;
    ItemRef item;
    bool keyboard = false; // STACK holds keyboard focus
};

// --- the laws, as pure functions ------------------------------------------------------------------

int fisheyeHeight(int base, std::size_t distance, double listHeight, const Constants &) noexcept;
int gapBetween(int above, int below, const Constants &) noexcept;
int chainItemWidth(ItemClass, double height, const Constants &) noexcept;
int columnWidth(ItemClass, bool engagedSomewhere, int base, double factor, const Constants &) noexcept;
Tier tierFor(double height, bool maximised, const Constants &) noexcept;
ProcessorPlan planProcessor(double listWidth, int base, std::span<const KindScope> kinds, const WindowViewState &,
                            const Constants &) noexcept;
// Case- and accent-insensitive search text: ASCII lowercased; Latin-1 and Latin Extended-A letters
// to their base letters; other code points and malformed bytes copied; control characters dropped.
void foldForSearch(std::string_view in, std::string &out);
// The search text as stored: control characters dropped, cut at a UTF-8 boundary to maxBytes.
void normaliseSearchText(std::string_view in, std::size_t maxBytes, std::string &out);

// --- snapshot and owned actual-device publication adapters ---------------------------------------

// Today's channel processor as MixViewProvider v1 reports it: ADI-Q, ducking, ADI-L (keys 1, 2, 4;
// key 3 is kept for Clip, which v1 does not report).
std::span<const ModuleKindInput> defaultModuleKinds() noexcept;
struct SnapshotRows {
    std::vector<TrackInput> tracks;
    std::vector<ModuleInput> modules;
    std::vector<ItemInput> items;
    std::span<const ModuleKindInput> kinds;
    // Adapter scratch only: source indices sorted by owning track, then source order. Reused so
    // unchanged publications allocate nothing; it never owns project state or supplies identity.
    std::vector<std::pair<TrackId, std::size_t>> chainOrder;
    RowsInput input(std::uint64_t generation, std::uint64_t providerRevision = 0) const noexcept;
};
// Every track of the reader's snapshot in the arrangement's order (geometry.rows(reader)), with
// inView from the geometry, the Reference from the provider (and the role), roles from TrackNode,
// and the modules from MixViewProvider v1. Refilled in place; names are views into the snapshot,
// valid while `reader` lives.
void tracksFromSnapshot(const SnapshotReader &, const ArrangementGeometry &, const MixViewProvider &,
                        SnapshotRows &out);

// A message-thread publication supplies these records in each track's actual chain order. Its
// owner holds the publication through setRows(). Identity is an opaque nonzero instance token,
// not the user-editable name or chain position. The adapter never queries a store/host, invents
// device capabilities or instantiates a processor. Missing/bypassed devices keep their positions.
struct ChainDeviceInput {
    TrackId track = 0;
    DeviceId device = 0;
    std::uint64_t identity = 0;
    ItemClass cls = ItemClass::Plugin;
    std::string_view name;
    bool available = true, enabled = true;
    bool mixFunction = false; // stable native identity, never inferred from name
};
// The actual-chain surface (ADR-0236/0237): every real device, no reserved module columns. Track
// order/scope/roles still come from the one snapshot. Unknown owners and invalid identities are
// ignored. Devices interleaved between tracks retain their supplied order within each track.
void tracksFromSnapshot(const SnapshotReader &, const ArrangementGeometry &, const MixViewProvider &,
                        std::span<const ChainDeviceInput>, SnapshotRows &out);

namespace detail {
template <class T>
struct Sink {
    void *ctx = nullptr;
    void (*call)(void *, const T &) = nullptr;
    void operator()(const T &value) const { call(ctx, value); }
};
template <class T, class F>
Sink<T> sinkFor(F &f) noexcept {
    Sink<T> sink;
    sink.ctx = const_cast<void *>(static_cast<const void *>(std::addressof(f)));
    sink.call = [](void *ctx, const T &value) { (*static_cast<F *>(ctx))(value); };
    return sink;
}
} // namespace detail
} // namespace stack

class StackModel {
  public:
    explicit StackModel(stack::Constants constants = {});
    const stack::Constants &constants() const noexcept { return c_; }

    // --- inputs ---
    // Copies what it needs (the spans may die after the call). The same input again allocates
    // nothing; growth (more tracks, longer names, more items) may allocate.
    stack::Effects setRows(const stack::RowsInput &, stack::TimeMs now);
    // Whether the project has moved on since the last setRows: refresh the rows before forwarding any
    // event that can reach an edit, and before any edit (an op may land between frames).
    bool stale(std::uint64_t currentGeneration) const noexcept { return currentGeneration != rowsGeneration_; }
    stack::Effects setViewport(double listWidth, double listHeight, stack::TimeMs now);
    stack::Effects setSelection(stack::TrackId, stack::TimeMs now);
    stack::Effects setKeyboardFocus(bool, stack::TimeMs now);

    // --- events ---
    stack::Effects pointerMove(double x, double y, bool buttonDown, stack::TimeMs now);
    stack::Effects pointerDown(double x, double y, stack::Button, stack::Mods, int clicks, stack::TimeMs now);
    stack::Effects pointerUp(double x, double y, stack::TimeMs now);
    stack::Effects pointerExit(stack::TimeMs now); // left STACK's list area
    stack::Effects wheel(double x, double y, const stack::WheelDelta &, stack::Mods, stack::TimeMs now);
    stack::Effects key(stack::Key, stack::Mods, stack::TimeMs now);
    stack::Effects advance(stack::TimeMs now);
    stack::TimeMs nextDeadline() const noexcept; // the next timer or animation end; kNever when idle
    // The enlarged row (§1.4): the row under the pointer after its dwell; the keyboard-focused row
    // while the pointer is outside the list; and, win's placements until the director strikes them:
    // - a key or command that moves the focus (Up, Down, focusRow, a navigator click) makes the
    //   focused row the enlarged one even while the pointer rests in another row, until the pointer
    //   moves (it is the only way the key's effect is visible under a resting pointer);
    // - "locked to the dragged row" while a drag is in progress is read as: no row's height changes
    //   for the length of the capture, so the captured target never moves (the brief's retention
    //   rule); the captured row keeps the height it had when the capture began, it is not grown to
    //   it. enlarged() keeps naming the row the heights enlarge, capture() the captured target;
    // - nothing is promoted while a mouse button is down; the dwell restarts at the release.
    bool busy() const noexcept;                  // something moves: keep the frame clock running

    // --- commands (toolbar, menus, other items) ---
    stack::Effects setBaseHeight(int px, stack::TimeMs now);
    stack::Effects stepBaseHeight(int detents, stack::TimeMs now);
    stack::Effects setAutoZoom(bool, stack::TimeMs now);
    stack::Effects setRowLayout(stack::RowLayout, stack::TimeMs now);
    stack::Effects setModuleLayout(stack::ModuleLayout, stack::TimeMs now);
    stack::Effects setShowChain(bool, stack::TimeMs now);
    stack::Effects setShowDormant(bool, stack::TimeMs now);
    stack::Effects setNavigator(bool shown, bool collapsed, stack::TimeMs now);
    stack::Effects setFilterPinned(bool, stack::TimeMs now);
    stack::Effects setChannelScope(stack::ChannelScope, stack::TimeMs now);
    stack::Effects setSearch(std::string_view, stack::TimeMs now); // allocates only past the reserved bound
    stack::Effects setSearchMode(stack::SearchMode, stack::TimeMs now);
    stack::Effects searchKey(stack::Key, stack::TimeMs now); // Up, Down, Enter, Escape while the field has focus
    stack::Effects setCollisionRule(stack::CollisionRule, stack::TimeMs now);
    stack::Effects setRowSpectrum(stack::RowSpectrum, stack::TimeMs now);
    stack::Effects setOverlay(bool, stack::TimeMs now);
    stack::Effects setLook(stack::Look, stack::TimeMs now);
    stack::Effects setModuleRange(const stack::RangeEntry &, stack::TimeMs now); // may allocate
    stack::Effects pin(const stack::RowRef &, stack::Mods, stack::TimeMs now);
    stack::Effects pinSameColour(const stack::RowRef &, stack::TimeMs now);
    stack::Effects storePinSet(std::string_view name, stack::TimeMs now); // may allocate
    stack::Effects recallPinSet(std::string_view name, stack::TimeMs now);
    stack::Effects deletePinSet(std::string_view name, stack::TimeMs now);
    stack::Effects setComparePair(stack::TrackId a, stack::TrackId b, stack::TimeMs now);
    stack::Effects clearComparePair(stack::TimeMs now);
    stack::Effects collisionButton(const stack::RowRef &, stack::TimeMs now); // the pair (this row, the focused row)
    stack::Effects maximise(const stack::RowRef &, stack::TimeMs now);
    stack::Effects restore(stack::TimeMs now);
    stack::Effects scrollLane(const stack::RowRef &, int items, stack::TimeMs now);
    stack::Effects toggleFold(const stack::RowRef &group, stack::TimeMs now);
    // For the accessibility layer and the keys: the same rules as a key focus change (select, take
    // focus, enlarge, reveal); a stale ref is refused.
    stack::Effects focusRow(const stack::RowRef &, stack::TimeMs now);
    stack::Effects focusItem(const stack::ItemRef &, stack::TimeMs now);
    stack::Effects reveal(const stack::RowRef &, stack::TimeMs now);
    stack::Effects scrollTo(double contentY, stack::TimeMs now); // a user scroll
    stack::Effects scrollBy(double dy, stack::TimeMs now);       // a user scroll
    stack::Effects openFromChip(stack::TrackId, stack::ItemKey, stack::TimeMs now); // the landing
    stack::Effects clearOpenedFrom(stack::TimeMs now);

    // --- the capture ---
    stack::CaptureResult beginCapture(stack::CaptureKind, const stack::ItemRef &target, stack::TimeMs now);
    stack::CaptureStatus captureStatus(const stack::CaptureRef &) const noexcept;
    // The one to ask before every edit: Behind while the project has moved on since the last setRows.
    stack::CaptureStatus captureStatus(const stack::CaptureRef &, std::uint64_t currentGeneration) const noexcept;
    stack::Effects endCapture(const stack::CaptureRef &, stack::TimeMs now); // end or cancel alike
    std::optional<stack::CaptureRef> capture() const noexcept;

    // --- refs ---
    bool live(const stack::RowRef &) const noexcept;
    bool live(const stack::ItemRef &) const noexcept; // compares generations, never the device
    std::optional<stack::Resolved> resolve(const stack::ItemRef &) const noexcept;
    stack::RowRef rowRef(stack::TrackId) const noexcept;
    stack::ItemRef chainRef(stack::TrackId, stack::DeviceId) const noexcept;
    stack::ItemRef moduleRef(stack::TrackId, std::uint32_t kindKey) const noexcept;

    // --- layout queries: noexcept, allocation-free, O(visible rows + entries) or better ---
    stack::Span visible() const noexcept;    // rows intersecting the list
    stack::Span realised() const noexcept;   // the visible rows plus realisedMarginPct each side
    stack::Span subscribed() const noexcept; // the visible rows plus subscribeMarginPx: spectra, taps
    std::array<stack::RowRef, 2> retained() const noexcept; // the captured and the focused row, outside realised()
    std::uint64_t epoch() const noexcept { return epoch_; }
    std::uint64_t realisedEpoch() const noexcept { return realisedEpoch_; }
    stack::RowGeo row(std::size_t shown) const noexcept;
    template <class F>
    void forEachVisibleRow(F &&f) const {
        walkRows(visible(), stack::detail::sinkFor<stack::RowGeo>(f));
    }
    template <class F>
    void forEachRow(stack::Span span, F &&f) const {
        walkRows(span, stack::detail::sinkFor<stack::RowGeo>(f));
    }
    stack::LaneGeo lane(std::size_t shown) const noexcept;
    // The chain items in the lane: the fully visible ones at rest, every one it cuts while it scrolls.
    template <class F>
    void forEachChainItem(std::size_t shown, F &&f) const {
        walkItems(shown, stack::detail::sinkFor<stack::ItemGeo>(f));
    }
    stack::CellGeo moduleCell(std::size_t shown, std::size_t kindIndex) const noexcept;
    template <class F>
    void forEachModuleCell(std::size_t shown, F &&f) const {
        walkCells(shown, stack::detail::sinkFor<stack::CellGeo>(f));
    }
    stack::ChipGeo chip(std::size_t shown) const noexcept;
    const stack::ProcessorPlan &plan() const noexcept { return planFrozen_ ? frozenPlan_ : plan_; }
    double scroll() const noexcept { return scroll_; }
    double contentHeight() const noexcept;
    double maxScroll() const noexcept;
    std::size_t shownCount() const noexcept { return shown_.size(); }
    stack::TrackId shownTrack(std::size_t shown) const noexcept;
    std::optional<std::size_t> shownIndex(stack::TrackId) const noexcept;
    std::string_view trackName(stack::TrackId) const noexcept;
    std::size_t moduleKindCount() const noexcept { return kindCount_; }
    std::uint32_t moduleKey(std::size_t kindIndex) const noexcept;
    std::string_view moduleName(std::size_t kindIndex) const noexcept; // the kind's short name, for pills, cells and accessibility
    stack::Hit hitTest(double x, double y) const noexcept;
    stack::NavGeo navigator() const noexcept;
    stack::NavLine navigatorLine(std::size_t shown) const noexcept;
    template <class F>
    void forEachNavigatorLine(F &&f) const {
        walkLines(stack::detail::sinkFor<stack::NavLine>(f));
    }

    // --- state: O(1) or O(log rows) each, collisionReference included (it reads a cache kept with the scope) ---
    const stack::Stats &stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = {}; }
    stack::RowRef enlarged() const noexcept;  // the row the heights enlarge (not the captured one)
    stack::TimeMs enlargedSince() const noexcept { return enlargedSince_; }
    stack::RowRef pointerRow() const noexcept; // the row under the pointer now, not dwell-promoted
    stack::Focus focus() const noexcept;
    stack::RowRef maximised() const noexcept;
    bool pinned(stack::TrackId) const noexcept;
    const std::vector<stack::TrackId> &pins() const noexcept { return shared_.pins; }
    stack::ComparePair comparePair() const noexcept { return shared_.comparePair; }
    stack::TrackId collisionReference(const stack::RowRef &) const noexcept;
    const stack::WindowViewState &windowView() const noexcept { return view_; }
    const stack::SharedViewState &sharedView() const noexcept { return shared_; }
    stack::TrackId selection() const noexcept { return selection_; }
    stack::TrackId openedFrom() const noexcept { return openedFrom_; }
    stack::RowRef jumpTarget() const noexcept;
    bool keyboardOwnsEnlarged() const noexcept { return focusOwns_; }

    // --- view state ---
    stack::ViewState viewState() const; // allocates: an export, not a frame query
    stack::Effects applyViewState(const stack::ViewState &, stack::TimeMs now); // normalises first; may allocate

  private:
    static constexpr std::uint32_t kNoRec = 0xffffffffu;
    static constexpr std::size_t kFocusOrder = 128; // a capacity: the chip, the chain and the modules of one row
    struct ItemRec {
        stack::DeviceId device = 0;
        std::uint64_t identity = 0;
        stack::ItemClass cls = stack::ItemClass::View;
        std::uint32_t gen = 0;
        bool available = true, enabled = true;
    };
    struct ModuleRec {
        stack::ModuleState state = stack::ModuleState::Absent;
        std::uint64_t instance = 0;
        stack::DeviceId device = 0;
        std::uint32_t gen = 0;
    };
    struct Rec { // one input track
        stack::TrackId id = 0, parent = 0;
        std::uint64_t identity = 0, revision = 0, textHash = 0;
        std::uint32_t gen = 0, depth = 0, parentRec = kNoRec;
        std::int32_t colour = -1;
        std::uint8_t roles = 0;
        bool valid = false, dup = false, group = false, main = false, ret = false, inView = false, reference = false;
        bool match = true, underFold = false; // the search, STACK's folds
        bool hasMixFunction = false;
        std::uint32_t laneFirst = 0;
        stack::DeviceId laneFirstDevice = 0; // the lane's first item follows its device
        std::string name, nameFold, haystack;
        std::vector<ItemRec> items;
        std::array<ModuleRec, stack::kMaxModules> modules{};
    };
    struct KindRec {
        std::uint32_t key = 0;
        std::string name, searchFold;
        stack::ItemClass widthClass = stack::ItemClass::View;
        bool keepsDormantColumn = false, dormantPreview = false;
    };
    struct Entry { // a row away from its rest height, or animating: its height and the gap below it
        std::size_t shown = 0;
        stack::TrackId id = 0;
        double hFrom = 0, hTo = 0, gFrom = 0, gTo = 0;
    };
    struct Jump { // an explicit anchor: a row's view top moving to a goal, then held
        bool active = false, moving = false, clamp = true;
        stack::TrackId row = 0;
        double from = 0, to = 0;
        stack::TimeMs start = 0;
        int duration = 0;
    };
    struct AnchorHold { // what holds still while heights change
        stack::TrackId row = 0;
        double viewTop = 0;
        double fraction = -1; // PointerRowFraction: the pointer's place in the row, else < 0
        bool clamp = true;    // false: the pointer's row is held even past the list's ends
    };
    struct LaneAnim {
        stack::TrackId row = 0;
        double from = 0, to = 0; // positions in items
        stack::TimeMs start = 0;
    };
    struct NavPress {
        bool active = false, dragging = false, inFrame = false, shift = false, onLine = false;
        double x = 0, y = 0, startScroll = 0;
        std::size_t line = 0;
    };
    struct Capture {
        bool active = false;
        std::uint32_t token = 0;
        stack::CaptureKind kind = stack::CaptureKind::Drag;
        stack::ItemRef target;
        double viewTop = 0;
        stack::LaneGeo lane;
        std::array<stack::CellGeo, stack::kMaxModules> cells{};
    };
    enum class Locus : std::uint8_t { Empty, Above, Row, Gap, Below };
    struct Located {
        Locus locus = Locus::Empty;
        std::size_t index = 0; // Row: the row; Gap: the row above the gap
        double top = 0;        // Row and Gap: the row's content top
        double height = 0;
    };
    struct Changes {
        bool any = false, text = false, structure = false, membership = false, groupName = false;
    };

    // inputs
    bool kindsDiffer(std::span<const stack::ModuleKindInput>);
    void adoptKinds(std::span<const stack::ModuleKindInput>);
    std::uint64_t textHashOf(const stack::TrackInput &, const stack::RowsInput &, std::size_t trackIndex) const;
    void buildHaystack(Rec &, const stack::TrackInput &, const stack::RowsInput &, std::size_t trackIndex);
    Changes updateRecord(Rec &, const stack::TrackInput &, const stack::RowsInput &, std::size_t trackIndex,
                         bool fresh);
    std::uint32_t resolveLaneFirst(const Rec &) const noexcept;
    void rebuildIndex();
    std::uint32_t recOf(stack::TrackId) const noexcept;
    const Rec *recFor(stack::TrackId) const noexcept;
    void scopeAdd(const Rec &, int sign) noexcept;
    void recountScope();
    void recountRolePartners() noexcept;
    bool rolePartnersStale() const noexcept;
    stack::TrackId rolePartnerFor(std::uint8_t roleBit, stack::TrackId notThis) const noexcept;
    bool inScope(const Rec &r) const noexcept { return r.valid && r.inView && !r.reference; }
    bool memberPlain(std::uint32_t rec) const noexcept;
    bool member(std::uint32_t rec) const noexcept;
    bool computeMatch(std::uint32_t rec) const noexcept;
    bool computeUnderFold(std::uint32_t rec) const noexcept;
    std::optional<std::size_t> shownOfRec(std::uint32_t rec) const noexcept;
    void rebuildShown();
    void editShown(std::uint32_t rec, bool memberNow);
    void afterShownChange(stack::TimeMs now, std::size_t oldFocusShown);
    void fixFocus(std::size_t oldFocusShown);
    void replan();
    void nextGen(std::uint32_t &gen) noexcept;

    // geometry core
    int base() const noexcept { return view_.baseHeight; }
    double g0() const noexcept;
    double pitch() const noexcept { return static_cast<double>(base()) + g0(); }
    double ease(double u) const noexcept;
    double animE() const noexcept;
    const Entry *entryAt(std::size_t shown) const noexcept;
    double heightAt(std::size_t shown) const noexcept;
    double targetHeightAt(std::size_t shown) const noexcept;
    double contentTop(std::size_t shown) const noexcept;
    double viewTop(std::size_t shown) const noexcept { return contentTop(shown) - scroll_; }
    Located locate(double contentY) const noexcept;
    stack::Span spanFor(double from, double to) const noexcept;
    double maxHeight() const noexcept;
    std::size_t buildTargets(std::array<Entry, stack::kMaxEntries> &out) const noexcept;
    void retarget(stack::TimeMs now);
    void finishAnimation();
    void snapEntries();
    void reindexEntries();
    void recomputeFrozenGaps();

    // anchors and scroll
    bool pointerOverRows() const noexcept;
    void anchorPointer();
    void anchorFirstVisible();
    void anchorRow(stack::TrackId, bool clamp);
    void startJump(stack::TrackId, double goal, int duration, bool clamp, stack::TimeMs now);
    double jumpTop(stack::TimeMs t) const noexcept;
    void releaseJumpIfDone();
    void solveScroll();
    void userScroll(double to, stack::TimeMs now);
    double revealGoal(std::size_t shown) const noexcept;
    void startRevealIfCut(stack::TimeMs now);

    // time
    stack::TimeMs normaliseTime(stack::TimeMs) noexcept;
    void rebase(stack::TimeMs delta) noexcept;
    stack::TimeMs animEnd() const noexcept;
    stack::TimeMs jumpEnd() const noexcept;
    stack::TimeMs landingDeadline() const noexcept;
    stack::TimeMs leaveDeadline() const noexcept;
    bool landingActive() const noexcept;
    stack::TimeMs promotionDeadline() const noexcept;
    stack::TimeMs laneDeadline() const noexcept;
    stack::TimeMs internalDeadline() const noexcept;
    stack::TimeMs advanceTo(stack::TimeMs); // runs what is due and answers the normalised time: every stamp uses it
    void processAt(stack::TimeMs);
    void onAnimEnd(stack::TimeMs);
    void onJumpArrive(stack::TimeMs);
    void onLeave(stack::TimeMs);
    void promote(stack::TimeMs);
    void setEnlarged(stack::TrackId, stack::TimeMs now);

    // interaction
    std::optional<std::size_t> pointerRowIndex() const noexcept;
    void rehit(stack::TimeMs now);
    void buttonPressed(stack::TimeMs now);  // rule 3: a moving reveal or jump stops where it is
    void buttonReleased(stack::TimeMs now); // the dwell starts again; a reveal held back runs
    stack::ItemRef chipRef(stack::TrackId) const noexcept;
    void focusByKey(std::size_t shown, const stack::ItemRef &, stack::TimeMs now, stack::Effects &);
    void focusByClick(std::size_t shown, const stack::ItemRef &, stack::TimeMs now, stack::Effects &);
    std::size_t focusOrder(std::size_t shown, std::array<stack::ItemRef, kFocusOrder> &out) const noexcept;
    void setLaneFirst(Rec &, std::uint32_t index, stack::TimeMs now, bool animate);
    void revealLaneItem(Rec &, std::uint32_t index, std::size_t shown, stack::TimeMs now);
    void stepItem(int dir, stack::TimeMs now, stack::Effects &);
    void doMaximise(std::size_t shown, stack::TimeMs now);
    void doRestore(stack::TimeMs now);
    void pressRow(const stack::Hit &, stack::Button, stack::Mods, int clicks, stack::TimeMs now, stack::Effects &);
    static stack::Effects mergeFlags(stack::Effects into, const stack::Effects &from) noexcept;
    void navDrag(double x, double y, stack::TimeMs now);
    void navClick(std::size_t line, bool shift, stack::TimeMs now, stack::Effects &);
    stack::Effects stepBaseNow(int steps, stack::TimeMs now);
    stack::Effects setBaseNow(int px, stack::TimeMs now, bool snap);
    void layoutChanged(stack::TimeMs now, stack::Effects &);
    void membershipChanged(stack::TimeMs now, bool toTop);
    void applySearchChange(stack::TimeMs now);
    void rebuildMatches(stack::TrackId keepCursorOn); // an O(shown) pass, counted; keepCursorOn: the match stays on this track (0: the first)
    void jumpToMatch(stack::TimeMs now);
    void clearSearch(stack::TimeMs now, stack::Effects &);
    bool setPinned(stack::TrackId, bool on);
    void pinsChanged(stack::TimeMs now, std::span<const stack::TrackId> touched, bool bulk);
    stack::Effects pinNow(const stack::RowRef &, stack::Mods, stack::TimeMs now);
    stack::Effects collisionNow(const stack::RowRef &);
    stack::Effects toggleFoldNow(const stack::RowRef &, stack::TimeMs now);
    void freezeCapturedRow(std::size_t shown, double top, double height);
    void finishCapture(stack::TimeMs now, stack::CaptureStatus, stack::TrackId matchCursor);
    void checkCapture(stack::TimeMs now, stack::Effects &, stack::TrackId matchCursor);
    void updateNavScroll() noexcept;
    std::uint64_t beginEvent() const noexcept;
    void endEvent(stack::Effects &, std::uint64_t epochBefore);
    void bump() noexcept { ++epoch_; }

    // queries
    stack::ItemRef cellRef(const Rec &, std::size_t kindIndex) const noexcept;
    bool isCapturedShown(std::size_t shown) const noexcept;
    stack::RowGeo rowGeoAt(std::size_t shown, double top, double height, double target, bool animating,
                           std::optional<std::size_t> pointerIdx) const noexcept;
    void walkRows(stack::Span, stack::detail::Sink<stack::RowGeo>) const;
    void layoutLane(std::size_t shown, double top, double height, stack::LaneGeo *geo,
                    stack::detail::Sink<stack::ItemGeo> emit) const;
    stack::LaneGeo laneAt(std::size_t shown, double top, double height) const noexcept;
    void walkItems(std::size_t shown, stack::detail::Sink<stack::ItemGeo>) const;
    stack::CellGeo cellAt(std::size_t shown, std::size_t kindIndex) const noexcept;
    void walkCells(std::size_t shown, stack::detail::Sink<stack::CellGeo>) const;
    stack::ChipGeo chipAt(std::size_t shown, double top, double height) const noexcept;
    stack::Hit hitRow(std::size_t shown, double x, double y, double top, double height) const noexcept;
    stack::Span frameSpan() const noexcept;
    stack::NavLine lineAt(std::size_t shown, const stack::Span &onScreen) const noexcept;
    void walkLines(stack::detail::Sink<stack::NavLine>) const;

    stack::Constants c_;
    // rows (records in the input's order), their index by id, the module kinds and the scope's counts
    std::vector<Rec> rows_, spare_;
    std::vector<std::pair<stack::TrackId, std::uint32_t>> byId_;
    std::vector<std::uint8_t> taken_;
    std::array<KindRec, stack::kMaxModules> kinds_{};
    std::size_t kindCount_ = 0;
    std::array<std::array<std::uint32_t, 3>, stack::kMaxModules> scope_{}; // per kind: rows Absent, Dormant, Engaged
    // the first two rows in scope with ★ Kick and with ★ Bass (records, kNoRec for none), so the role
    // partner is a read, not a pass; with the count of each, so a cached row leaving the scope costs
    // one recount only when a further one exists
    std::array<std::array<std::uint32_t, 2>, 2> rolePartners_{{{kNoRec, kNoRec}, {kNoRec, kNoRec}}};
    std::array<std::uint32_t, 2> roleCounts_{};
    std::uint64_t rowsGeneration_ = 0, providerRevision_ = 0;
    bool rowsSet_ = false;
    std::uint32_t gens_ = 0;
    std::size_t maxChain_ = 0;
    std::vector<std::uint32_t> shown_; // record indices, in order
    std::uint64_t shownVersion_ = 0, lastShownVersion_ = 0;
    std::string scratch_;
    std::vector<ItemRec> scratchItems_;
    // view state
    stack::WindowViewState view_;
    stack::SharedViewState shared_;
    std::string queryFold_;
    std::vector<std::uint32_t> matches_; // Jump mode: the shown records that match, in order
    std::size_t matchAt_ = 0;
    // geometry
    double listW_ = 0, listH_ = 0;
    stack::ProcessorPlan plan_, frozenPlan_;
    bool planFrozen_ = false;
    std::array<Entry, stack::kMaxEntries> entries_{};
    std::size_t entryCount_ = 0;
    stack::TimeMs animStart_ = 0;
    bool animActive_ = false;
    double scroll_ = 0;
    AnchorHold anchor_;
    Jump jump_;
    std::array<LaneAnim, stack::kMaxLaneAnims> laneAnims_{};
    double navScroll_ = 0;
    // interaction
    double px_ = 0, py_ = 0;
    bool pointerKnown_ = false, inside_ = false, buttonDown_ = false;
    stack::TrackId candidate_ = 0;           // the row under the pointer, waiting for its dwell
    stack::TimeMs enteredAt_ = 0, lastScrollAt_ = 0, captureEndAt_ = 0, leaveAt_ = stack::kNever;
    stack::TrackId enlarged_ = 0, maximised_ = 0;
    stack::TimeMs enlargedSince_ = 0;
    bool focusOwns_ = false; // a focus change owns the enlarged row until the pointer moves
    stack::TrackId focusRow_ = 0;
    stack::ItemRef focusItem_;
    bool keyboardFocus_ = false;
    stack::TrackId selection_ = 0;
    stack::TrackId landed_ = 0;
    stack::TimeMs landingEnd_ = 0;
    stack::TrackId openedFrom_ = 0;
    bool revealPending_ = false;
    NavPress navPress_;
    stack::TrackId pairPending_ = 0;
    stack::TrackId pinAnchor_ = 0;
    stack::TrackId laneWheelRow_ = 0;
    double laneWheelAcc_ = 0;
    // the capture
    Capture cap_;
    std::vector<stack::ItemGeo> frozenItems_;
    std::uint32_t nextToken_ = 1;
    std::uint32_t lastToken_ = 0;
    stack::CaptureStatus lastStatus_ = stack::CaptureStatus::Ended;
    // the clock and the counters
    stack::TimeMs now_ = 0;
    bool clockStarted_ = false;
    mutable stack::Stats stats_;
    std::uint64_t epoch_ = 0, realisedEpoch_ = 0;
    stack::Span lastRealised_;
    std::array<stack::RowRef, 2> lastRetained_{};
};
} // namespace adi::ui
