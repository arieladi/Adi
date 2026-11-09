// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gesture_session.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace adi::gesture {
/// Concrete public adapters for existing catalogue operations; they run through
/// the same evaluator/admission/inverse path, not test-only virtual producers.
enum class ScalarKind {
    TrackVolume, TrackPan, TrackPanLeft, TrackPanRight, TrackInputGain,
    RoutingGain, RoutingPan, GroupSummingDrive
};
struct ScalarTarget { ScalarKind kind; std::int64_t id; };
class ScalarInput final : public Input {
public:
    explicit ScalarInput(double absolute) : value(absolute) {}
    const double value;
};
/// Maps respectively to mixer.setVolume/Pan/PanLeft/PanRight/InputGain,
/// routing.setGain/Pan, group.setSummingDrive. Capture exact typed row/presence
/// and target definition; validate finite/range rules and unchanged topology.
/// No plug-in parameter path here: ParamOps owns that capture/runtime protocol.
/// Existing mixer.setWidth has no playback consumer and is UnsupportedPreview;
/// this item does not invent a new width DSP implementation.
std::unique_ptr<Producer> scalarProducer(ScalarTarget);

enum class TrackSwitchKind { Activation, Solo };
struct TrackSwitchTarget {
    TrackSwitchKind kind;
    std::int64_t clicked;
    std::vector<std::int64_t> selected;
    bool exclusiveSolo;
    bool additiveSolo;
};
enum class TrackSwitchValue { ToggleFromOrigin, On, Off };
class TrackSwitchInput final : public Input {
public:
    explicit TrackSwitchInput(TrackSwitchValue choice) : value(choice) {}
    const TrackSwitchValue value;
};
/// Concrete momentary-latch/track-switch adapter. Activation emits track.setMute
/// with muted=!active; Solo emits track.setSolo. ToggleFromOrigin always derives
/// from the one captured clicked-track value, never toggles a previous preview.
/// Capture clicked/selected literal IDs and explicit solo policy, without SQL
/// or setting reads by the caller. Match current track_edits behavior: multiple
/// selected tracks act together only when clicked is in that set; clicked Main
/// refuses and Main is never affected. Exclusive non-additive Solo-on also
/// clears every other eligible track's solo. Complete selected/affected-set and
/// exact typed current-row guards cover all resulting effects atomically.
/// Window may commit a tap or cancel a hold; no undo-recommit or quiet-time merge.
std::unique_ptr<Producer> trackSwitchProducer(TrackSwitchTarget);
enum class TrackBooleanKind { Mute, Solo };
class TrackBooleanInput final : public Input {
public:
    explicit TrackBooleanInput(std::vector<std::pair<std::int64_t, bool>> values)
        : rows(std::move(values)) {}
    const std::vector<std::pair<std::int64_t, bool>> rows;
};
/// Literal already-selected batch; no selection/exclusive policy inference.
std::unique_ptr<Producer> trackBooleanProducer(TrackBooleanKind, std::vector<std::int64_t> targets);

class ClipLoopInput final : public Input {
public:
    ClipLoopInput(bool enabled, std::int64_t begin, std::int64_t duration)
        : on(enabled), startTicks(begin), lengthTicks(duration) {}
    const bool on;
    const std::int64_t startTicks, lengthTicks;
};
/// Emits clip.setLoop with explicit on/start/length using the existing handler.
/// Captures exact clip identity/native definition and origin loop properties.
/// Native ticks are explicit, no grid/UI/default clock inference. Does not
/// move/copy/place material or bypass ED-03's pending producer boundary.
std::unique_ptr<Producer> clipLoopProducer(std::int64_t clipId);

class TempoInput final : public Input {
public:
    explicit TempoInput(double value) : bpm(value) {}
    const double bpm;
};
/// Existing event at explicit position: preserve its captured id, position,
/// curve and tension; only bpm varies. Emit remove+insert using the SAME two
/// existing tempo opcodes. HI-01 extends insert with optional literal id/tension
/// and captures both in remove inverse; old packets lacking fields retain old
/// behavior. Require exact canonical five-column typed row, refuse extended or
/// unsupported stored types. Exact row/set guards bind current pre-commit state;
/// inverse restores that complete state, including nonzero tension and sparse ID.
/// This is the producer for Tap Tempo's existing-event-in-force target. Caller
/// supplies the captured event position, not a repeatedly sampled playhead.
std::unique_ptr<Producer> tempoProducer(std::int64_t positionTicks);

class TempoInsertInput final : public Input {
public:
    TempoInsertInput(double value, std::int64_t shape) : bpm(value), curve(shape) {}
    const double bpm;
    const std::int64_t curve;
};
/// Complementary absent-position insertion, with explicit pos/bpm/curve.
/// It does not substitute for tempoProducer's required existing-row behavior.
std::unique_ptr<Producer> tempoInsertProducer(std::int64_t positionTicks);

struct AutomationPointPosition {
    std::uint64_t id;
    std::int64_t time;
    double value;
};
class AutomationMoveInput final : public Input {
public:
    explicit AutomationMoveInput(std::vector<AutomationPointPosition> absolute)
        : points(std::move(absolute)) {}
    const std::vector<AutomationPointPosition> points;
};
/// Explicit EDIT of existing automation points, delegating automation.movePoints
/// (not AU-03 recording). Captures data/lane/clip address, original stream and
/// selected literal IDs; every input must address exactly that ID set, no
/// duplicates. Stream codec remains existing authority; no new points/IDs or
/// implicit writes from scalar gestures. Foreign same-stream changes refuse
/// conservatively; unrelated streams may rebase.
std::unique_ptr<Producer> automationMoveProducer(std::int64_t dataId,
    std::int64_t laneId, std::optional<std::int64_t> clipId,
    std::vector<std::uint64_t> pointIds);

} // namespace adi::gesture
