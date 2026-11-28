// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Session View's model, free of drawing (TM-SV-05; Live 12 manual chapter 7).
//
// The grid of tracks by scenes, each slot's state, the scene column with its tempo and signature,
// the Track Status of each column and what is selected. It holds no store and draws nothing: the
// window paints from it, and the look of what it paints is agreed from a picture first (ADR-0243).
//
// TWO HALVES, TWO THREADS OF CHANGE.
// - `rebuild` takes the authored project (tracks, scenes, slots and the slot clips' facts), after a
//   load and after every committed edit, undo and redo. It allocates, on the message thread.
// - `applyStatus` and `applyRecording` take the engine's own publications (the launcher's completed
//   `LaunchTrackStatus` frame and the slot recorder's state) every frame. They write only into the
//   storage `rebuild` sized and never allocate, so a frame of the window allocates nothing.
//
// Selection is session state (ADR-0021): it lives here, by identity, so a rebuild keeps it on the
// same track and scene. It is never an op and never undone.
#pragma once

#include "adi/engine/launch_data.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/slot_recording.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace adi::rows {
struct Model;
}

namespace adi::ui::session_view {

/// What a column of the grid is. Clip tracks hold slots; a group's slots stand for the clips of the
/// tracks it holds (manual 7.1); a return has no slots. Main is the scene column, not a column here.
enum class ColumnKind : std::uint8_t { Clips, Group, Return };

/// A slot's state as the window shows it, most urgent first: recording, then a queued change, then
/// playing, then a clip at rest, then empty.
enum class SlotState : std::uint8_t {
    Empty, Stopped, Playing, QueuedStart, QueuedStop, QueuedRecord, Recording
};
enum class SlotQueue : std::uint8_t { None, Start, Stop, Record };

/// The authored project the model is built from, in display order. Owner side; allocates.
struct ModelInput {
    struct Track {
        std::int64_t id = 0;
        ColumnKind kind = ColumnKind::Clips;
        std::int64_t parent = 0; // the group that holds it, or 0
        std::string name;
        std::optional<std::int64_t> color;
        bool armed = false;
        bool midi = false; // a MIDI or instrument track: it takes MIDI clips, an audio track audio
    };
    struct SlotRow {
        std::int64_t track = 0, scene = 0;
        std::int64_t clip = 0; // 0: empty
        bool hasStop = engine::launch::defaultEmptySlotStop;
    };
    struct Clip {
        std::int64_t id = 0;
        std::string name;
        std::optional<std::int64_t> color;
        bool deactivated = false;
        engine::LaunchMode mode = engine::LaunchMode::Trigger;
        bool follow = false;
        bool midi = false;
        engine::FollowSettings followSettings{}; // its `enabled` is `follow`
    };
    std::vector<Track> tracks;                   // display order: a group, then what it holds
    std::vector<engine::launch::Scene> scenes;    // display order (the scenes' `order`)
    std::vector<SlotRow> slots;                   // sparse: no row is an empty slot with Stop
    std::vector<Clip> clips;                      // the facts of the slot clips
};

/// The model input of a project: its tracks in the Arrangement's order (each group followed by what
/// it holds, the returns after, Main left out), its scenes, slots and slot clips.
[[nodiscard]] ModelInput modelInput(const rows::Model&, const engine::launch::LaunchData&);

struct Column {
    std::int64_t track = 0;
    ColumnKind kind = ColumnKind::Clips;
    std::int64_t parent = 0;
    std::string name;
    std::optional<std::int64_t> color;
    bool armed = false;
    bool midi = false;
};

/// What a slot holds; changes only with the project.
struct SlotFacts {
    std::int64_t clip = 0; // 0 in an empty slot and in every group slot
    std::string name;
    std::optional<std::int64_t> color; // a group slot: the left-most held clip's (manual 7.1)
    bool hasStop = engine::launch::defaultEmptySlotStop;
    bool deactivated = false;
    bool follow = false; // Follow Actions on: Live stripes the launch button (manual 16)
    engine::LaunchMode mode = engine::LaunchMode::Trigger;
    bool groupOccupied = false; // a group slot whose held tracks hold a clip in this scene
    bool midiClip = false;
    engine::FollowSettings followSettings{}; // the clip's whole Follow Action, kept by a toggle
};

/// What a slot is doing; changes every frame.
struct SlotLive {
    bool playing = false, recording = false;
    SlotQueue queued = SlotQueue::None;
    bool operator==(const SlotLive&) const = default;
};

[[nodiscard]] SlotState stateOf(const SlotFacts&, const SlotLive&) noexcept;

struct SceneFacts {
    std::int64_t id = 0, order = 0, number = 0; // number: the place from 1, as Live shows it
    std::string name;
    std::optional<std::int64_t> color;
    double tempoBpm = engine::launch::defaultDormantSceneTempo;
    bool tempoEnabled = false;
    std::int64_t signatureNumerator = engine::launch::defaultDormantSceneSignature;
    std::int64_t signatureDenominator = engine::launch::defaultDormantSceneSignature;
    bool signatureEnabled = false;
    bool follow = false;
    std::int32_t occupied = 0; // clip tracks holding a clip in this scene
    engine::FollowSettings followSettings{.linked = false};
};

struct SceneLive {
    bool triggered = false; // the scene's launch is queued
    std::int32_t playing = 0; // clip tracks playing this scene's slot
    bool operator==(const SceneLive&) const = default;
};

/// A column's Track Status (manual 7.3) and its Clip Stop button.
struct TrackLive {
    engine::LaunchTrackSource source = engine::LaunchTrackSource::Arrangement;
    std::int64_t playingClip = 0, playingScene = 0;
    engine::LaunchQueuedAction queued = engine::LaunchQueuedAction::None;
    std::uint64_t completedLoops = 0;
    std::int64_t loopLengthTicks = 0, remainingSamples = 0;
    bool stopQueued = false; // the Clip Stop button blinks
    bool operator==(const TrackLive&) const = default;
};

enum class Focus : std::uint8_t { None, Slot, Scene, Track };

/// One item picked with Ctrl+click: a slot (track and scene), a scene (track 0) or a track title
/// (scene 0).
struct Pick {
    std::int64_t track = 0, scene = 0;
    auto operator<=>(const Pick&) const = default;
};

/// What is selected, by identity. A slot selection is the rectangle from the anchor to the focus in
/// display order; a scene selection the run of scenes between them; a track selection the run of
/// track titles. Anchor equal to focus: one item. With `picks` (Ctrl+click, manual 7.1 and 7.2:
/// "nonadjacent clips ... by Ctrl-clicking") the selection is exactly the picked items, of the
/// focus's kind, and the focus is the keyboard's place among them.
struct Selection {
    Focus focus = Focus::None;
    std::int64_t track = 0, scene = 0;
    std::int64_t anchorTrack = 0, anchorScene = 0;
    std::vector<Pick> picks; // sorted, distinct; empty: the rectangle
    bool operator==(const Selection&) const = default;
};

class Model {
  public:
    /// Owner side. Takes a new project; keeps the selection where its track and scene still stand.
    void rebuild(ModelInput);

    /// Every frame: the launcher's completed status frame. Rows of tracks that are not columns are
    /// ignored. `pendingScene` is the scene whose launch is queued, where the caller knows it.
    /// Allocation-free. True when anything the window shows changed.
    bool applyStatus(std::span<const engine::LaunchTrackStatus>, const engine::LaunchStatusHeader&,
                     std::optional<std::int64_t> pendingScene = std::nullopt) noexcept;
    /// Every frame: the slot recorder's state and the slots it records into. Allocation-free.
    bool applyRecording(engine::launch::SlotRecordingState,
                        std::span<const engine::launch::SlotKey> targets) noexcept;

    [[nodiscard]] std::size_t columnCount() const noexcept { return columns_.size(); }
    [[nodiscard]] std::size_t sceneCount() const noexcept { return scenes_.size(); }
    [[nodiscard]] const Column& column(std::size_t c) const noexcept { return columns_[c]; }
    [[nodiscard]] const SceneFacts& scene(std::size_t r) const noexcept { return scenes_[r]; }
    [[nodiscard]] const SceneLive& sceneLive(std::size_t r) const noexcept { return sceneLive_[r]; }
    [[nodiscard]] const SlotFacts& slot(std::size_t c, std::size_t r) const noexcept { return facts_[at(c, r)]; }
    [[nodiscard]] const SlotLive& slotLive(std::size_t c, std::size_t r) const noexcept { return live_[at(c, r)]; }
    [[nodiscard]] SlotState slotState(std::size_t c, std::size_t r) const noexcept {
        return stateOf(slot(c, r), slotLive(c, r));
    }
    [[nodiscard]] const TrackLive& trackLive(std::size_t c) const noexcept { return trackLive_[c]; }
    /// Live's Back to Arrangement button lights while what plays differs from the Arrangement.
    [[nodiscard]] bool backToArrangementLit() const noexcept { return backToArrangement_; }
    [[nodiscard]] bool transportPlaying() const noexcept { return transportPlaying_; }
    [[nodiscard]] std::optional<std::int64_t> pendingScene() const noexcept { return pendingScene_; }
    /// The status incarnation the last frame came from; a launch op is stamped with the Session's own.
    [[nodiscard]] std::uint64_t statusIncarnation() const noexcept { return statusIncarnation_; }

    /// Allocation-free lookups by identity; nullopt where the track or scene is not shown.
    [[nodiscard]] std::optional<std::size_t> columnOf(std::int64_t track) const noexcept;
    [[nodiscard]] std::optional<std::size_t> rowOf(std::int64_t scene) const noexcept;
    /// The clip columns a group column holds at any depth, in display order; empty for others.
    [[nodiscard]] std::span<const std::size_t> heldColumns(std::size_t c) const noexcept;

    [[nodiscard]] const Selection& selection() const noexcept { return selection_; }
    /// Sets the selection; one the model cannot show (an unknown track or scene, or a pick of one)
    /// selects nothing. On the message thread: it may allocate.
    void select(const Selection&);
    /// Whether a slot, a scene or a track title is inside the selection. Allocation-free.
    [[nodiscard]] bool slotSelected(std::size_t c, std::size_t r) const noexcept;
    [[nodiscard]] bool sceneSelected(std::size_t r) const noexcept;
    [[nodiscard]] bool trackSelected(std::size_t c) const noexcept;
    /// The selection's rectangle in display indices: columns [c0, c1], rows [r0, r1].
    struct Range {
        std::size_t c0 = 0, c1 = 0, r0 = 0, r1 = 0;
    };
    [[nodiscard]] std::optional<Range> selectedRange() const noexcept; // with picks: their box
    /// The selected items in display order (columns, then rows); the commands' reading.
    [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> selectedSlots() const;
    [[nodiscard]] std::vector<std::size_t> selectedScenes() const;
    [[nodiscard]] std::vector<std::size_t> selectedTracks() const;

    /// Counters a window compares to know what to present again.
    [[nodiscard]] std::uint64_t projectRevision() const noexcept { return projectRevision_; }
    [[nodiscard]] std::uint64_t liveRevision() const noexcept { return liveRevision_; }
    [[nodiscard]] std::uint64_t selectionRevision() const noexcept { return selectionRevision_; }

  private:
    [[nodiscard]] std::size_t at(std::size_t c, std::size_t r) const noexcept { return c * scenes_.size() + r; }
    bool publish() noexcept;
    void resolveSelection();
    [[nodiscard]] bool stands(Focus, std::int64_t track, std::int64_t scene) const noexcept;

    std::vector<Column> columns_;
    std::vector<SceneFacts> scenes_;
    std::vector<SlotFacts> facts_;
    std::vector<std::pair<std::int64_t, std::size_t>> columnIndex_, sceneIndex_; // sorted by identity
    std::vector<std::size_t> held_;                                    // groups' held clip columns
    std::vector<std::pair<std::size_t, std::size_t>> heldRange_;       // per column: [begin, end) in held_
    // The live state, double-buffered so a frame compares before it publishes. `next*` is scratch.
    std::vector<SlotLive> live_, nextLive_;
    std::vector<TrackLive> trackLive_, nextTrackLive_;
    std::vector<SceneLive> sceneLive_, nextSceneLive_;
    // The last engine frame per column, kept so a recording change recomputes without a new frame.
    std::vector<engine::LaunchTrackStatus> columnStatus_;
    std::vector<std::uint8_t> columnHasStatus_;
    engine::launch::SlotRecordingState recordingState_ = engine::launch::SlotRecordingState::Idle;
    std::vector<engine::launch::SlotKey> recording_; // capacity: one per slot
    bool backToArrangement_ = false, transportPlaying_ = false;
    std::optional<std::int64_t> pendingScene_;
    std::uint64_t statusIncarnation_ = 0;
    Selection selection_;
    std::vector<std::uint8_t> pickedSlots_, pickedScenes_, pickedTracks_; // the picks, by place
    std::uint64_t projectRevision_ = 0, liveRevision_ = 0, selectionRevision_ = 0;
};

} // namespace adi::ui::session_view
