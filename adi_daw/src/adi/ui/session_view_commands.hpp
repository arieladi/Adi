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
// - `view`, `selection` and `rename` are the window's own state, never an op.
//
// Where Live's behaviour is not in its manual the answer is a named constant below, with its
// reading owed in the session window log.
#pragma once

#include "adi/ops.hpp"
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

struct Context {
    MainView view = MainView::Session;
    std::uint64_t incarnation = 0; // `engine::Session::launchIncarnation()`; 0: nothing to launch into
    float velocity = 1.0f;
    bool selectNextSceneOnLaunch = defaultSelectNextSceneOnLaunch;
    bool selectOnLaunch = defaultSelectOnLaunch;
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

struct Outcome {
    std::vector<OpRequest> edits;    // one batch: one step of Undo
    std::vector<OpRequest> launches; // one gesture: never in Undo
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

/// The rename itself, once the name is typed; nothing when it is unchanged.
[[nodiscard]] Outcome renameTo(const Model&, const RenameTarget&, std::string_view name);
/// Live 7.2 and 41.5: Tab while renaming moves to the next scene or track (Shift+Tab: back).
[[nodiscard]] std::optional<RenameTarget> nextRenameTarget(const Model&, const RenameTarget&, bool backward);

} // namespace adi::ui::session_view
