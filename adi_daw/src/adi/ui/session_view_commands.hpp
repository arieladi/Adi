// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Session View's commands and keys, as Live 12 has them (manual 7, 16 and 41.15), free of
// drawing (TM-SV-05).
//
// A command reads the model and says what to do; it does nothing itself. What it returns is data
// the window hands on:
// - `edits` go to `OpSubmitter::submit` as ONE batch, so ONE step of Undo, as Live's edits are
//   (Insert Scene, Delete, Rename, Add/Remove Stop Button, Deactivate, moving scenes);
// - `launches` go to `engine::dispatchLaunchOperations` as ONE gesture: launching, stopping, Back
//   to Arrangement and Cancel Scene Launch are performance, outside Undo, as in Live;
// - `capture` asks for Capture and Insert Scene, which the engine prepares and commits itself
//   (`engine::prepareCaptureAndInsertScene`, then `commitCaptureAndInsertScene`: one Undo step);
// - `copies` are clip copies the store side places among `edits` (session_view_edits.hpp), the
//   same batch;
// - `view`, `selection`, `rename` and `clipboard` are the window's own state, never an op. A
//   selection naming a scene or clip an edit makes is applied after the rebuild that edit brings.
//
// Where Live's behaviour is not in its manual the answer is a named constant below, with its
// reading owed in the session window log.
#pragma once

#include "adi/ops.hpp"
#include "adi/textproj.hpp"
#include "adi/ui/session_view_model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi::ui::session_view {

/// Live 7.2: the scene below a launched scene is selected next unless the option is Off.
inline constexpr bool defaultSelectNextSceneOnLaunch = true;
/// Live 7.4.1: launching a clip or a scene selects it, by default.
inline constexpr bool defaultSelectOnLaunch = true;
/// Live 41.15: Page Up/Down moves eight scenes at a time.
inline constexpr std::size_t scenesPerPage = 8;
/// READING OWED: whether Live refuses to delete the last remaining scene. ADI keeps one.
inline constexpr bool keepOneScene = true;
/// READING OWED: whether a group slot's launch stops the held tracks that have no clip in that
/// scene, as a scene launch's Stop buttons do. ADI stops them where the empty slot has its Stop.
inline constexpr bool groupLaunchStopsEmptyHeldTracks = true;
/// READING OWED: Enter with several slots selected. ADI launches the focused slot only.
inline constexpr bool enterLaunchesFocusOnly = true;
/// TM-SV-05 names F3 beside Live's Tab for the view switch, as F4 opens the Mix View (ADR-0209 d9).
inline constexpr bool f3TogglesSessionView = true;
/// READING OWED: Duplicate on clips puts the copies the selection's height below them; where a slot
/// there is taken or missing, ADI inserts that many scenes below the selection for them, so no
/// clip is overwritten.
inline constexpr bool duplicateInsertsScenes = true;
/// READING OWED: a paste that reaches past the last scene adds the scenes it needs at the end.
inline constexpr bool pasteAddsScenes = true;
/// READING OWED: Create Follow Action Chain (16.7.2: "all selected clips play in a loop", the
/// selection need not be contiguous). ADI sets each selected clip of a track to Jump to the next
/// selected clip's scene at 100 %, the last back to the first, and keeps each clip's Follow time.
inline constexpr bool chainKeepsFollowTime = true;
/// READING OWED: the length of the clip Insert MIDI Clip makes in a slot. ADI: one bar.
inline constexpr std::int64_t newSlotClipBars = 1;
/// READING OWED: a duplicated or pasted scene keeps its name as it was.
inline constexpr bool sceneCopyKeepsName = true;

enum class MainView : std::uint8_t { Arrangement, Session };
[[nodiscard]] constexpr MainView toggled(MainView v) noexcept {
    return v == MainView::Arrangement ? MainView::Session : MainView::Arrangement;
}

enum class Command : std::uint8_t {
    ToggleView,          // Tab (with Use Tab to Move Focus off), F3
    LaunchSelected,      // Enter
    ReleaseSelected,     // Enter let go: the release edge Gate and Repeat clips need
    StopSelectedTracks,  // Ctrl+Enter: Stop Clips in Track with Slot Selection
    StopAll,             // the Main track's Stop All Clips button
    BackToArrangement,   // the Back to Arrangement button
    CancelSceneLaunch,   // the Main track's context menu
    SelectUp, SelectDown, SelectLeft, SelectRight,     // arrows
    ExtendUp, ExtendDown, ExtendLeft, ExtendRight,     // Shift+arrows
    PageUp, PageDown,    // eight scenes at a time
    FirstTrack,          // Home
    LastTrack,           // End; from the last track, the current scene
    TrackTitle,          // Esc: Jump to Highlighted Track Title Bar
    SelectAll,           // Ctrl+A
    InsertScene,         // Ctrl+I
    CaptureAndInsertScene, // Ctrl+Shift+I
    AddRemoveStopButton, // Ctrl+E
    Rename,              // Ctrl+R
    Delete,              // Delete, Backspace
    Deactivate,          // 0
    MoveScenesUp, MoveScenesDown, // Ctrl+Up/Down: Move Nonadjacent Scenes Without Collapsing
    InsertMidiClip,      // Ctrl+Shift+M
    ToggleFollowActions, // Shift+Enter: Toggle Follow Actions for Selected Clips
    CreateFollowActionChain, // Ctrl+Shift+Enter
    Duplicate,           // Ctrl+D
    Copy, Cut, Paste,    // Ctrl+C, Ctrl+X, Ctrl+V (41.5)
};

enum class Key : std::uint8_t {
    Tab, F3, Enter, Escape, Up, Down, Left, Right, PageUp, PageDown, Home, End, Delete, Backspace, Character
};
/// A key as the window reads it. `command` is Ctrl on Windows and Cmd on a Mac.
struct KeyChord {
    Key key = Key::Character;
    char32_t character = 0; // lower case, for Key::Character
    bool command = false, shift = false, alt = false;
};
struct KeyOptions {
    bool useTabToMoveFocus = false; // Live 41.26.1: on, Tab moves focus and no longer switches views
};
/// Live's key for a command in `view`; in the Arrangement only the view switch is the Session's.
[[nodiscard]] std::optional<Command> commandForKey(const KeyChord&, MainView, const KeyOptions& = {}) noexcept;
/// Letting a key go: Enter's release edge in the Session View.
[[nodiscard]] std::optional<Command> commandForKeyRelease(const KeyChord&, MainView) noexcept;

/// What Copy and Cut put on the Session View's clipboard: clips by their place from the
/// selection's top-left corner, or whole scenes. A clip is named by its identity; a Cut's clips
/// are gone once the Cut commits, so the window takes their images first (`snapshotClipboard` in
/// session_view_edits.hpp) and a paste copies from those.
struct Clipboard {
    enum class Kind : std::uint8_t { None, Clips, Scenes };
    struct Clip {
        std::size_t column = 0, row = 0; // from the top-left corner, in display places
        std::int64_t clip = 0;
        bool midi = false;
    };
    struct Slot {
        std::int64_t track = 0, clip = 0; // clip 0: an empty slot
        bool midi = false, hasStop = true;
    };
    struct Scene {
        SceneFacts facts;
        std::vector<Slot> slots; // the clip tracks' slots that differ from an empty one with Stop
    };
    Kind kind = Kind::None;
    std::vector<Clip> clips;
    std::vector<Scene> scenes;
    Payload images = Payload::object(); // clip identity as text -> {image, launch}; see above
};

struct Context {
    MainView view = MainView::Session;
    std::uint64_t incarnation = 0; // `engine::Session::launchIncarnation()`; 0: nothing to launch into
    float velocity = 1.0f;
    bool selectNextSceneOnLaunch = defaultSelectNextSceneOnLaunch;
    bool selectOnLaunch = defaultSelectOnLaunch;
    std::int64_t nextClipId = 0; // the first free clip identity (Insert MIDI Clip); 0: unknown
    std::int64_t barTicks = 4 * textproj::kPPQ; // one bar of the project's meter where the clip goes
    const Clipboard* clipboard = nullptr; // what Paste pastes
};

/// Capture and Insert Scene below `belowScene`, or at the end when none is selected.
struct CaptureRequest {
    std::optional<std::int64_t> belowScene;
    bool operator==(const CaptureRequest&) const = default;
};

/// What a rename edits: a clip, a scene or a track.
struct RenameTarget {
    Focus what = Focus::None;
    std::int64_t id = 0; // the clip, scene or track
    bool operator==(const RenameTarget&) const = default;
};

/// A clip copied into a slot. Copying needs the clip's rows, so the store side
/// (`prepareEdit` in session_view_edits.hpp) turns it into the copy (`clip.duplicate`), its slot
/// (`session.setSlotClip`) and its launch settings, in place before `edits[before]`.
struct SlotCopy {
    std::int64_t clip = 0;              // the source
    std::int64_t track = 0, scene = 0;  // the slot it goes to, on a track that takes its kind
    std::optional<bool> stop;           // a copied scene keeps its slot's Stop button
    std::size_t before = 0;
    bool operator==(const SlotCopy&) const = default;
};

struct Outcome {
    std::vector<OpRequest> edits;    // one batch: one step of Undo
    std::vector<SlotCopy> copies;    // with `edits`, the same batch (prepareEdit)
    std::string label;               // the step's name in Undo where it holds copies
    std::vector<OpRequest> launches; // one gesture: never in Undo
    std::optional<Clipboard> clipboard; // Copy and Cut: the window's clipboard takes it
    std::optional<MainView> view;
    std::optional<Selection> selection;
    std::optional<CaptureRequest> capture;
    std::optional<RenameTarget> rename;          // open the name editor on it
    std::optional<engine::launch::SlotKey> recordInto; // Session Record into an armed track's empty slot
    bool passToShell = false; // the command is the window's general one here (a track's Delete, ...)
    std::string refusal;
};

/// Runs a command against the model as it stands.
[[nodiscard]] Outcome run(Command, const Model&, const Context&);

/// Mouse gestures on the grid, the same paths the keys take.
[[nodiscard]] Outcome launchSlot(const Model&, std::size_t column, std::size_t row, const Context&);
[[nodiscard]] Outcome releaseSlot(const Model&, std::size_t column, std::size_t row, const Context&);
[[nodiscard]] Outcome launchScene(const Model&, std::size_t row, const Context&);
[[nodiscard]] Outcome stopColumn(const Model&, std::size_t column, const Context&);

/// A click that selects (manual 7.1, 7.2, 41.5): plainly one item; with Shift the run or rectangle
/// from the anchor; with Ctrl (Cmd) the item picked into or out of the selection.
enum class Target : std::uint8_t { Slot, Scene, Track };
struct Click {
    Target what = Target::Slot;
    std::size_t column = 0, row = 0; // a scene uses `row`, a track title `column`
    bool command = false, shift = false;
};
[[nodiscard]] Outcome click(const Model&, const Click&);
/// The rubber band from an empty slot (7.4): the rectangle from where it began to where it is.
[[nodiscard]] Outcome rubberBand(const Model&, std::size_t fromColumn, std::size_t fromRow, std::size_t toColumn,
                                 std::size_t toRow);
/// Dropping the selected clips, grabbed at one slot, on another (7.4): a move, or with Ctrl (Win) or
/// Alt (Mac) held a copy (41.15, Copy Clips). Every clip must land on a track of its kind.
[[nodiscard]] Outcome dropSlots(const Model&, std::size_t grabColumn, std::size_t grabRow, std::size_t column,
                                std::size_t row, bool copy);
/// Dropping the selected scenes before the scene at `beforeRow` (the scene count: at the end).
/// Nonadjacent scenes collapse together when dropped (7.2).
[[nodiscard]] Outcome dropScenes(const Model&, std::size_t beforeRow);
/// The context menu's colour palette (7.1, 7.2): the selected clips or tracks take the colour.
/// A scene's colour waits for its op (see the session window log).
[[nodiscard]] Outcome recolour(const Model&, std::int64_t colour);

/// The rename itself, once the name is typed; nothing when it is unchanged.
[[nodiscard]] Outcome renameTo(const Model&, const RenameTarget&, std::string_view name);
/// Live 7.2 and 41.5: Tab while renaming moves to the next scene or track (Shift+Tab: back).
[[nodiscard]] std::optional<RenameTarget> nextRenameTarget(const Model&, const RenameTarget&, bool backward);

} // namespace adi::ui::session_view
