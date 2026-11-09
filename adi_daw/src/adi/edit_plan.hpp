// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/clip_commands.hpp"
#include "adi/edit_placement.hpp"

namespace adi::settings {class AppSettings;}
namespace adi::edit {class TimeMap;class RowEdits;}
namespace adi::clip_edit {class CopyIds;}

namespace adi::edit_plan {

enum class Domains : std::uint32_t {
    Clips=1, Automation=2, Markers=4, MusicalMaps=8, Loop=16
};
constexpr Domains operator|(Domains a,Domains b) noexcept {
    return static_cast<Domains>(static_cast<std::uint32_t>(a)|static_cast<std::uint32_t>(b));
}
struct EditScope {
    PlacementClock clock=PlacementClock::Ticks;
    std::vector<PlacementLane> targets;
    PlacementSpan bounds;
    Domains domains=Domains::Clips|Domains::Automation;
};
enum class Preset { Live, NonLive };
enum class Ripple { Off, SelectedTracks, AllTracks };
/// Owned settings snapshot. automationFollowsClips=true means Lock is OFF.
struct EditPolicy {
    bool deleteOverlaps=true;
    bool automationFollowsClips=true;
    bool mutedClipsAsDeleted=false;
    Preset preset=Preset::Live;
    Ripple ripple=Ripple::Off;
};
struct Options {double referenceRate=48000;EditPolicy policy;};
[[nodiscard]] EditPolicy capturePolicy(const settings::AppSettings&);
[[nodiscard]] EditScope materialScope(std::span<const std::int64_t> tracks,
                                     PlacementClock,PlacementSpan);
struct Move {
    std::int64_t id=0;
    PlacementLane to;
    clip_edit::TimePoint position;
};
struct ResizeEnd {std::int64_t id=0;clip_edit::TimePoint end;};
/// Ordinary material editing currently admits main lanes. Scope bounds admit
/// source/destination extents; only actual incoming spans clear destinations.
[[nodiscard]] clip_edit::PreparedEdit prepareMove(Store&,const EditScope&,
    std::span<const Move>,const Options&);
/// Keeps the native start fixed; left trim and Crop are separate intents.
[[nodiscard]] clip_edit::PreparedEdit prepareResizeEnd(Store&,const EditScope&,
    ResizeEnd,const Options&);

enum class EffectKind { Removed, Trimmed, Split, Created, Moved, Resized };
struct MaterialState {
    std::int64_t id=0;
    std::string name;
    PlacementLane lane;
    /// Display projection in Description.scope.clock; very short physical
    /// material can project to an empty interval in musical ticks.
    PlacementSpan span;
    PlacementClock nativeClock=PlacementClock::Ticks;
    PlacementSpan nativeSpan;
    PlacementSpan physicalSpan; // exact normalized nanoseconds
};
struct Effect {
    EffectKind kind=EffectKind::Created;
    std::optional<MaterialState> before;
    std::vector<MaterialState> after;
};
struct AppendedOperation {std::string opType,label;Payload payload;};
struct Description {
    bool ok=false;
    std::string error;
    EditScope scope;
    EditPolicy policy;
    std::vector<Effect> effects;
    /// Additional requests remain explicit; they are not guessed to be part
    /// of the captured placement lineage.
    std::vector<AppendedOperation> appended;
};
[[nodiscard]] Description describe(const clip_edit::PreparedEdit&);

namespace detail {
// Engine adapters only: complete owned images, already at their destination.
// Caller holds the same read snapshot and reserves incoming child/root IDs in
// the shared allocator. Media/track prefix ownership is a later integration.
struct PlacementBatch {
    std::vector<Payload> incoming;
    std::vector<std::int64_t> relocated;
    std::vector<ClearInterval> clear;
    std::vector<std::int64_t> sourceTracks;
    EffectKind incomingKind=EffectKind::Created;
};
[[nodiscard]] clip_edit::PreparedEdit prepareCaptured(Store&,const EditScope&,
    PlacementBatch,const Options&,clip_edit::CopyIds&,const char* label);
// Reused ED-01 image arithmetic. Freeze before changing any visible boundary;
// trimFrozenImage performs only value/time-map arithmetic and preserves native
// outer boundaries supplied by the owner.
void freezeImage(Store&,Payload&,const edit::TimeMap&,double);
void trimFrozenImage(Payload&,std::int64_t nativeBegin,std::int64_t nativeEnd,
                     const edit::TimeMap&,double);
void resizeFrozenEnd(Payload&,std::int64_t nativeEnd,const edit::TimeMap&,double);
void retainImageDetails(edit::RowEdits&,const Payload&);
}
} // namespace adi::edit_plan
