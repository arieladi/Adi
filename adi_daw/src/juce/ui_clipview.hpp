// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Clip View and its editors (STEP-7-PLAN 7.7; Live 12 manual ch. 8 "Clip View" and ch. 10
// "Editing MIDI"), approved as pictured on 16 November 2026 (ADR-0240).
//
// It reads the selected clip through `engine::readClipDetails` (N32), on the message thread, when
// the selection or the project's generation moves; it keeps that value until the next move and
// nothing else. Every change it makes is an op through the root's submitter.
#pragma once
#include "adi/engine/clip_details.hpp"
#include "adi/ui/arrangement_geometry.hpp"
#include "adi/ui/clip_edits.hpp"
#include "adi/ui/lane_shapes.hpp"
#include "adi/ui/note_edits.hpp"
#include "adi/ui/note_filters.hpp"
#include "adi/ui/note_geometry.hpp"
#include "adi/ui/scales.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "adi/ui/window_state.hpp"
#include "ui_arrangement.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <array>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui {
class AdiRootComponent;
class ClipViewPanel;

// Another of the selected MIDI clips, shown with the one being edited (manual 10.8).
struct ClipLayer {
    engine::ClipDetails details;
    juce::Colour colour;
};

// Live's MIDI Note Editor (manual 10.1): the time ruler along the top, the note ruler with its
// piano keys down the left, the key tracks with the clip's notes, and the Velocity lane below.
class NoteEditor final : public juce::Component {
  public:
    explicit NoteEditor(ClipViewPanel &);
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    // Editing (manual 10.4 to 10.6): a double-click adds a note or deletes the one under it; a
    // drag moves the selected notes, or their start or end edges; a drag in the Velocity lane
    // changes their velocities; a drag from empty space selects. Each gesture is one undo step.
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    // Delete, the arrows (with Shift: an octave, or the length), Ctrl/Cmd+A, Esc, and 0 to
    // deactivate (manual 10.5).
    bool keyPressed(const juce::KeyPress &) override;
    static constexpr int kKeysWidth = 44, kRulerHeight = 18, kVelocityHeight = 56;
    // The lane's height (manual 10.1: the divider between the key tracks and the lanes drags to
    // resize them): never lower than its header needs, and it leaves the key tracks four rows.
    static constexpr int kLaneFloor = 50, kKeysFloor = 48;
    int laneHeight() const noexcept { return laneHeight_; }
    void setLaneHeight(int height);
    // The divider's grip: a few pixels about the line above the lane.
    juce::Rectangle<int> laneDivider() const noexcept;
    // A batch from outside the editor's own gestures (the Pitch and Time panel): one undo step.
    bool apply(NoteEdit edit) { return commit(std::move(edit)); }
    // The trim tool (ADR-0241 d2, from Cubase's Trim tool; the header's switch): a stroke down
    // across notes cuts each one it crosses where the stroke is, on the grid; one undo step.
    bool trimMode() const noexcept { return trimMode_; }
    void setTrimMode(bool);
    // The lengths the stroke held now would leave, by note.
    const std::vector<std::pair<std::uint64_t, std::int64_t>> &trimmedLengths() const noexcept { return trimPreview_; }
    // The handles on two or more selected notes' velocities (ADR-0241 d2, from Cubase's controller
    // event editor): tilt from the left end or the right, scale, compress. A drag shows the result
    // while held; one undo step. None while a line tool is chosen or fewer than two are selected.
    juce::Rectangle<int> handleBox(LaneHandle) const noexcept;
    LaneHandle handleAt(juce::Point<int>) const noexcept;
    // The Velocity lane's line tools (ADR-0241 d2, from Cubase's Line tool): with a shape chosen, a
    // stroke across the lane gives every note under it the shape's value there, shown while held,
    // one undo step. Pointer is Live's lane: a marker is dragged. The chooser is in the lane's label.
    LaneShape laneShape() const noexcept { return shape_; }
    void setLaneShape(LaneShape);
    juce::Rectangle<int> shapeBox() const noexcept;
    juce::PopupMenu shapeMenu() const;
    bool chooseShape(int item);
    // The velocities the stroke held now would write, by note.
    const std::vector<std::pair<std::uint64_t, int>> &shapedVelocities() const noexcept { return shapePreview_; }
    // The Velocity lane as Live's (manual 10.5.12). A marker dragged takes the selected markers with
    // it; one that reaches the top or the bottom stays there while the others catch up, so a drag to
    // an end and back gives them all one value. Shift and a click adds a marker to the selection or
    // takes it out. Alt (Cmd on the Mac) and a vertical drag on a selected note changes velocity
    // from the notes. Digits typed with notes selected, then Enter, set their velocity. Ctrl/Cmd+up
    // and down change it by 10, with Shift by 1. The lane's header reads the selected notes' value,
    // or their range, or what is being typed. With Draw Mode on, a stroke in the lane gives the
    // notes of each grid step it crosses the value under the pointer: the selected ones only when
    // some are selected; with snapping bypassed, the one marker under the pointer.
    juce::String velocityReadout() const;
    // The editor's context menu (manual 10.5.2 and on: Live names these commands in the Edit menu
    // and the editor's own menu). A right-click on a note that is not selected selects it first.
    // Every entry is a command the editor already has; one that cannot act now is greyed.
    enum ContextItem {
        kCut = 1, kCopy, kPaste, kDuplicate, kDelete, kDeactivate, kSelectAll, kInvertSelection, kQuantize, kQuantizeSettings,
        kSplit, kJoin, kCrop, kDuplicateTime, kDeleteTime, kInsertTime, kDuplicateLoop, kFoldToNotes, kFoldToScale, kHighlightScale,
        kGridItems = 1000
    };
    juce::PopupMenu contextMenu() const;
    bool chooseContextItem(int item);
    // What a right-click does before its menu opens: a note under it that is not selected is selected.
    void contextClick(juce::Point<int> at);
    // MIDI Note Stretch (manual 10.5.6). With two or more notes selected, or time selected over
    // notes, a pair of markers hangs under the ruler at the selection's start and end. One dragged
    // stretches the notes in proportion, the other staying; dragged past the other it mirrors them.
    // A drag begun between the two moves a marker of its own: what lies before it and what lies
    // after it stretch to meet it, and nothing outside the pair moves. On the grid unless snapping
    // is bypassed; shown while held; one undo step.
    enum class StretchPart { None, Start, End, Middle };
    std::optional<std::pair<std::int64_t, std::int64_t>> stretchSpan() const;
    StretchPart stretchPartAt(juce::Point<int>) const;
    // The markers' own strip, between the ruler and the key tracks, as Live's marker row.
    static constexpr int kStretchHeight = 8;
    juce::Rectangle<int> stretchArea() const noexcept;
    // Getting about, as Live's (manual 8.7, 10.2). W fits the shown clips to the editor's width, H
    // the notes to its height; + and - zoom in time around the time selection, the insert marker
    // or the middle; Page Up and Down scroll an octave, with Shift a key track, with Ctrl/Cmd a
    // page sideways. A drag on the time ruler away from its handles scrolls sideways and zooms up
    // and down; a drag on the piano scrolls it and, sideways, zooms the key tracks; a double-click
    // on either zooms to the selection, or to the notes. Ctrl/Cmd+1 to 5 are the editing grid's
    // keys, on the editor's own grid: narrower, wider, triplets, snapping, fixed. While the song
    // plays with Follow on, the editor turns its page with the playhead; a scroll or an edit pauses it.
    void fitWidth();
    void fitHeight();
    void zoomTime(bool in);
    void gridKey(int key);
    // Note colours (ADR-0242 d1, N1; from Cubase's event colours): what the edited clip's notes
    // are coloured by. Clip is Live's, the clip's colour paler as the velocity falls, and the
    // default. Velocity runs from blue for the softest to red for the loudest; Pitch gives each of
    // the twelve pitches its colour in every octave; Channel each MIDI channel; Degree each degree
    // of the clip's scale, with the notes outside it dark grey. A deactivated note is grey in all.
    enum class NoteColours { Clip, Velocity, Pitch, Channel, Degree };
    static constexpr std::array<const char *, 5> kNoteColourNames = {"Clip", "Velocity", "Pitch", "Channel", "Degree"};
    NoteColours noteColours = NoteColours::Clip;
    juce::Colour noteFill(const engine::ClipNote &) const;
    // What the Status Bar reads while notes are selected (manual 3.2; as measured on Live): their
    // time in the song from the first one's start to the last one's end with its length, their
    // pitch, velocity and probability, each as a value or a range; or the editor's time selection.
    // Empty with neither. `statusStamp` changes whenever that text would: read each frame, it
    // allocates nothing.
    juce::String statusText(const std::vector<textproj::Meter> &meters) const;
    std::uint64_t statusStamp() const noexcept;
    juce::Rectangle<int> readoutBox() const noexcept;
    // Draw Mode (manual 10.4.1; B, and the header's switch): a drag across empty space draws a
    // note at each grid step on the key it began on, one undo step for the stroke; a click on a
    // note deletes it.
    bool drawMode() const noexcept { return drawMode_; }
    void setDrawMode(bool);
    // The grid steps the stroke held now would draw, as source ticks, and its key.
    std::vector<std::int64_t> drawnSteps() const;
    // Fold to Notes (manual 10.2; F): only the key tracks that hold notes. It follows the notes as
    // they change, and unfolds for a clip with none.
    bool folded() const noexcept { return folded_; }
    void setFold(bool);
    // Scale Mode in the editor (manual 10.6), while the clip holds a scale of its own: the scale's
    // keys are tinted on the piano. Highlight Scale (K; applied to every clip) tints their key
    // tracks too and marks the root. Fold to Scale (G) shows only the scale's key tracks and those
    // that hold notes; Fold to Notes, when on, has the say.
    bool scaleOn() const noexcept;
    bool scaleFolded() const noexcept { return scaleFolded_; }
    void setScaleFold(bool);
    bool highlightScale = true;
    // Z zooms to the selected notes, or to all of them; X goes back (manual 10.2).
    void zoomToNotes();
    void zoomBack();
    // Live's Duplicate on the selected notes (Ctrl/Cmd+D): copies right after them, selected.
    bool duplicate();
    // Whether a drag of notes, let go now, would copy them: Ctrl (Alt/Option on the Mac) is held.
    bool copying() const noexcept { return copying_; }
    // Moves the playhead: called on the window's frame.
    void frame();
    ArrangementPlayhead playhead;
    // Quantize (manual 10.5.11; Ctrl/Cmd+U): the selected notes' starts, or every note's, to the
    // editor's grid. Split and Chop (10.5.8; Ctrl/Cmd+E): with no note selected, every note under
    // the insert marker is cut there; with notes selected, each is cut at the grid lines crossing it.
    // Join (Ctrl/Cmd+J): the selected notes of each key become one. One undo step each.
    bool quantize();
    bool splitOrChop();
    bool join();
    // Time inside the clip (manual 10.7), one undo step each. Crop Clip (Ctrl/Cmd+Shift+J): the
    // notes outside the time selection go, or with none those outside the loop, and with the loop
    // off those outside the Start and End markers; a crossing note is cut at the edge, but kept
    // whole with the loop off, as measured on Live. With time selected: Duplicate Time
    // (Ctrl/Cmd+Shift+D), Delete Time (Ctrl/Cmd+Shift+Delete) and Insert Time (Ctrl/Cmd+I); the
    // clip's markers and loop stay. With the loop brace selected, Ctrl/Cmd+D is Duplicate Loop: the
    // loop twice as long, its notes copied, later notes moved on, and the editor zoomed out to show it.
    bool crop();
    enum class TimeCommand { Duplicate, Delete, Insert };
    bool timeCommand(TimeCommand);
    bool duplicateLoop();
    // The insert marker (manual 10.5.2): a click on empty space puts it there, on the grid; Paste
    // lands on it. In the song's time; none until a click, and then Paste lands at the clip's start.
    std::optional<std::int64_t> insertMarker() const noexcept { return insert_; }
    // The ruler's handles (manual 8.8, 8.9). The loop brace along its top: an end dragged changes
    // the loop's start or length, the bar dragged moves it. The clip's Start and End markers along
    // its foot, dragged. On the grid, shown while held, one undo step at the release. A press
    // selects the brace or the marker, and the arrows are then its own: for the brace, left and
    // right nudge it by the grid, up and down move it by its length, and with Ctrl/Cmd left and
    // right shorten or lengthen it and up and down double or halve it, as the Arrangement's brace;
    // for a marker, left and right move it by the grid, and with Alt/Option both markers together.
    enum class RulerPart { None, LoopStart, LoopEnd, Loop, StartMarker, EndMarker };
    RulerPart rulerPartAt(juce::Point<int>) const noexcept;
    RulerPart rulerSelection() const noexcept { return picked_; }
    // Where a marker stands in the ruler, as shown: the Start marker, or the End marker.
    int markerX(bool end) const noexcept;
    // The time selection (manual 10.5.2): from the insert marker to where a drag across empty space,
    // or Shift and the arrows, took its other end. In the song's time, the earlier end first; none
    // while it is a point. Enter swaps between it and the notes that begin inside it; Esc, a click
    // outside, or an arrow collapses it. Split cuts at its two edges; Paste lands on its start; Z
    // zooms to it.
    std::optional<std::pair<std::int64_t, std::int64_t>> timeSelection() const noexcept;
    void selectTime(std::int64_t from, std::int64_t to);
    bool toggleTimeAndNotes();
    // Every note that was not selected, and none that was (Ctrl/Cmd+Shift+A).
    void invertSelection();
    // Cut, Copy and Paste of notes (manual 10.5.4; Ctrl/Cmd+X, C, V): the selected notes of every
    // shown clip are copied with their places in the song; Paste puts them into the edited clip at
    // the insert marker, the earliest on it, selected; one undo step.
    bool copySelected();
    bool cutSelected();
    bool paste();
    // With several clips shown and Focus off, notes of the other clips can be selected too (a drag
    // from empty space, Ctrl/Cmd+A, Shift and a click), and a drag, the arrows, Delete, 0 and
    // Duplicate act on every selected note of every clip, as one undo step (manual 10.8).
    const std::vector<std::uint64_t> &selectedIn(std::int64_t clip) const noexcept;
    bool anySelected() const noexcept;
    // When another clip becomes the edited one, each clip keeps its own selected notes.
    void rehome(std::int64_t from, std::int64_t to);
    // A note of another shown clip as it is drawn while a drag is held.
    engine::ClipNote shownIn(const engine::ClipNote &, std::int64_t clip) const noexcept;
    // The selected notes, by id: this view's own, kept while the notes exist.
    const std::vector<std::uint64_t> &selected() const noexcept { return selected_; }
    bool isSelected(std::uint64_t id) const noexcept;
    void select(std::vector<std::uint64_t> ids);
    // Drops the selected notes the clip no longer holds: called when the clip is read again.
    void prune();
    // The note under a point, the one drawn last on top, and the part of it there.
    const engine::ClipNote *noteAt(juce::Point<int>, NotePart *part = nullptr) const noexcept;
    // A note as it is drawn: while a drag is held, where the drag would put it.
    engine::ClipNote shown(const engine::ClipNote &) const noexcept;
    // The editing grid's step, in ticks.
    std::int64_t gridTicks() const noexcept;
    // Time on the arrangement's own axis, with the editor's own zoom, scroll and grid.
    ArrangementGeometry time;
    KeyTracks keys;
    // The key tracks, and the Velocity lane under them, in this component.
    juce::Rectangle<int> notesArea() const noexcept;
    juce::Rectangle<int> velocityArea() const noexcept;
    juce::Rectangle<int> rulerArea() const noexcept;
    juce::Rectangle<int> keysArea() const noexcept;
    // The loop brace in the time ruler (manual 8.2.2): its left and right edge in this component;
    // none for a clip with no loop window.
    std::optional<juce::Range<int>> loopBrace() const noexcept;
    // Multi-clip editing (manual 10.8): while several clips are shown, a loop bar for each sits above
    // the time ruler in the clip's colour, the edited clip's first. A click on another clip's bar, or
    // on one of its notes, makes that clip the one being edited.
    static constexpr int kLoopBarHeight = 7;
    int barsHeight() const noexcept;
    juce::Rectangle<int> loopBar(std::size_t index) const noexcept;
    // The other clip whose loop bar or note is at a point, and that note; 0 for none.
    std::int64_t otherClipAt(juce::Point<int>, std::uint64_t *note = nullptr) const noexcept;
    // A note of any shown clip, placed through that clip's own position and start marker.
    std::optional<NoteBox> boxOf(const engine::ClipNote &, const engine::ClipDetails &) const noexcept;
    // A note's box in this component; none while Fold hides its key.
    std::optional<NoteBox> boxOf(const engine::ClipNote &) const noexcept;
    // Opens on a clip: its whole length across the editor, its notes in view (manual 10.2).
    void showClip();
    // Where this component draws text, for a picture that masks it (each platform's fonts differ).
    std::vector<juce::Rectangle<int>> textAreas() const;

  private:
    enum class Drag { None, Move, Start, End, Velocity, Band, Draw, Shape, Trim, Handle, Ruler, LaneDraw, Zoom, Keys, Stretch, LaneSize };
    int laneHeight_ = kVelocityHeight, laneFrom_ = 0;
    // A stretch held: which marker, the pair as it was and the place grabbed, where the marker is
    // now (all in the song's ticks), and the notes it carries.
    StretchPart stretchPart_ = StretchPart::None;
    std::int64_t stretchA_ = 0, stretchB_ = 0, stretchP_ = 0, stretchNow_ = 0;
    std::vector<std::uint64_t> stretchIds_;
    std::vector<std::uint64_t> stretchTargets() const;
    std::int64_t stretchMap(std::int64_t songTick) const noexcept;
    // A drag on the time ruler or the piano: where the pointer last was, and the zoom's count.
    juce::Point<int> viewLast_;
    int zoomFrom_ = 0;
    // A velocity drag: the value the pointer last stood for, and whether it began on a note.
    int velocityLast_ = 0;
    juce::Point<int> laneLast_;
    bool velocityByNote_ = false;
    juce::String typedVelocity_;
    void beginVelocityDrag(bool byNote);
    void velocityDrag();
    void laneDrawStroke(const juce::ModifierKeys &);
    RulerPart picked_ = RulerPart::None, rulerDrag_ = RulerPart::None;
    // While a ruler handle is held: the loop's start and length, or the marker, in the clip's ticks.
    std::optional<std::pair<std::int64_t, std::int64_t>> loopPreview_;
    std::optional<std::int64_t> markerPreview_;
    void rulerDrag(const juce::ModifierKeys &);
    bool rulerKey(const juce::KeyPress &);
    void shapeStroke();
    void trimStroke();
    void handleDrag();
    juce::Rectangle<int> selectedVelocityFrame() const noexcept;
    bool commit(NoteEdit);
    using Build = std::function<NoteEdit(const engine::ClipDetails &, const std::vector<std::uint64_t> &)>;
    NoteEdit across(const Build &) const;
    engine::ClipNote dragged(const engine::ClipNote &) const noexcept;
    std::vector<std::uint64_t> &otherIds(std::int64_t clip);
    bool snapping(const juce::ModifierKeys &) const noexcept;
    ClipViewPanel &panel;
    bool fitPending_ = false;
    mutable std::vector<juce::Rectangle<int>> labels_;
    std::vector<std::uint64_t> selected_, bandBase_;
    std::vector<std::pair<std::int64_t, std::vector<std::uint64_t>>> otherSelected_;
    std::optional<std::int64_t> insert_, head_;
    // The insert marker, or with `extend` the time selection's moving end, put at `at` and kept in view.
    void placeMarker(std::int64_t at, bool extend);
    // Alt and an arrow (manual 10.5.2, 41.12): with no note selected, left and right take the marker
    // to the next note edge and up and down select the note nearest it; with one, up and down go to
    // the previous and next note in time, left and right to those on the same key. Shift extends.
    bool walk(bool sideways, bool forward, bool extend);
    Drag drag_ = Drag::None;
    bool copying_ = false, folded_ = false, drawMode_ = false, scaleFolded_ = false;
    // The key tracks shown, after either fold or none.
    void refold();
    int drawKey_ = 0;
    LaneShape shape_ = LaneShape::Pointer;
    LaneHandle handle_ = LaneHandle::None;
    bool trimMode_ = false;
    std::vector<std::pair<std::uint64_t, std::int64_t>> trimPreview_;
    std::vector<std::pair<std::uint64_t, int>> shapePreview_;
    juce::Point<int> from_, to_;
    std::int64_t dragTicks_ = 0;
    int dragKeys_ = 0;
    std::uint64_t anchor_ = 0;
};

// Live's Sample Editor (manual 8.1.3): the clip's audio on the time ruler.
class SampleEditor final : public juce::Component {
  public:
    explicit SampleEditor(ClipViewPanel &);
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    static constexpr int kRulerHeight = 18;
    ArrangementGeometry time;
    juce::Rectangle<int> waveArea() const noexcept;
    void showClip();
    void frame();
    ArrangementPlayhead playhead;

  private:
    ClipViewPanel &panel;
    bool fitPending_ = false;
};

// The Clip View: the clip's title bar with its activator, name and colour; the Main panel with the
// clip's properties; and the editor for its kind (manual 8.1).
class ClipViewPanel final : public juce::Component {
  public:
    explicit ClipViewPanel(AdiRootComponent &);
    // Follows the selected clip and the project; called on the window's frame.
    void frame(const SnapshotReader &);
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;

    static constexpr int kPanelWidth = 156, kToolsWidth = 196, kTitleHeight = 17, kHeaderHeight = 17;
    // The panels' whole width: the Main panel, and for a MIDI clip the Pitch and Time panel.
    int panelWidth() const noexcept { return midi() ? kPanelWidth + kToolsWidth : kPanelWidth; }
    // The Pitch and Time panel (manual 8.5), for a MIDI clip: the notes' range, Transpose, and the
    // utilities. Each acts on the selected notes, or on all of them when none is selected, and is
    // one undo step.
    // After Live's, ADI's additions from Cubase's Key Editor (ADR-0241 d2): Fixed Lengths, Delete
    // Overlaps on one key or on all, and chord editing: a chord of the chosen type on each selected
    // note, inversions and drop voicings of the selected chords.
    enum class Tool {
        FitToScale, Invert, Reverse, Double, Halve, Legato,
        FixedLength, OverlapsMono, OverlapsPoly, Chord, InvertUp, InvertDown, Drop2, Drop3,
        VelocityAdd, VelocityScale, VelocityLimit, DeleteDoubles, RestrictPolyphony, Quantize, AddInterval
    };
    static constexpr Tool kTools[] = {Tool::FitToScale, Tool::Invert,       Tool::Reverse,      Tool::Double,     Tool::Halve,
                                      Tool::Legato,     Tool::FixedLength,  Tool::OverlapsMono, Tool::OverlapsPoly, Tool::Chord,
                                      Tool::InvertUp,   Tool::InvertDown,   Tool::Drop2,        Tool::Drop3,
                                      Tool::VelocityAdd, Tool::VelocityScale, Tool::VelocityLimit, Tool::DeleteDoubles,
                                      Tool::RestrictPolyphony, Tool::Quantize, Tool::AddInterval};
    // The tools panel's three tabs. Pitch and Time is Live's panel (manual 10.5.9): the range,
    // Transpose, Fit to Scale, Invert, Reverse, the Interval size with Add Interval, the Stretch
    // control with x2 and /2, the Duration chooser with Set Length, and Legato. Functions and
    // Chords hold ADI's additions: the velocity and note functions with the Quantize settings, and
    // the chord and overlap tools.
    static constexpr int kToolTabs = 3;
    // Add Interval: the size, in semitones, or in the scale's degrees while the clip's Scale Mode is on.
    int intervalSize = 7;
    // The Stretch control (manual 10.5.9.5): a factor in per cent, typed or dragged; it stretches the
    // selected notes, or all, from the first one's start, and reads 100 again.
    bool stretch(int percent);
    // The Duration chooser beside Set Length (manual 10.5.9.6): the editing grid's step, or a fixed length.
    struct Duration {
        const char *name;
        std::int64_t ticks; // 0: the editing grid's step
    };
    static constexpr std::array<Duration, 7> kDurations = {{{"Grid", 0},
                                                           {"1/32", textproj::kPPQ / 8},
                                                           {"1/16", textproj::kPPQ / 4},
                                                           {"1/8", textproj::kPPQ / 2},
                                                           {"1/4", textproj::kPPQ},
                                                           {"1/2", 2 * textproj::kPPQ},
                                                           {"1 Bar", textproj::kWhole}}};
    std::size_t duration() const noexcept { return duration_; }
    juce::PopupMenu durationMenu() const;
    bool chooseDuration(int item);
    std::int64_t durationTicks() const noexcept;
    juce::Rectangle<int> durationBox() const noexcept;
    // The Quantize settings (manual 10.5.11), at the foot of the Functions tab; Ctrl/Cmd+U and the
    // Quantize button use them, Ctrl/Cmd+Shift+U shows them. The value is the editing grid or a
    // fixed one; Start and End say which ends move; the amount is how far towards the grid. Swing
    // is ADI's addition (ADR-0242 d1, from Cubase's quantize panel; the engine's op carries it).
    struct QuantizeValue {
        const char *name;
        std::int64_t ticks; // 0: the editing grid
    };
    static constexpr std::array<QuantizeValue, 8> kQuantizeValues = {{{"Grid", 0},
                                                                     {"1/4", textproj::kPPQ},
                                                                     {"1/8", textproj::kPPQ / 2},
                                                                     {"1/8T", textproj::kPPQ / 3},
                                                                     {"1/16", textproj::kPPQ / 4},
                                                                     {"1/16T", textproj::kPPQ / 6},
                                                                     {"1/32", textproj::kPPQ / 8},
                                                                     {"1/32T", textproj::kPPQ / 12}}};
    std::size_t quantizeValue() const noexcept { return quantizeValue_; }
    juce::PopupMenu quantizeMenu() const;
    bool chooseQuantize(int item);
    std::int64_t quantizeGrid() const noexcept;
    QuantizeSettings quantizeSettings() const noexcept;
    juce::Rectangle<int> quantizeChooserBox() const noexcept;
    juce::Rectangle<int> quantizeStartBox() const noexcept;
    juce::Rectangle<int> quantizeEndBox() const noexcept;
    int quantizeAmount = 100, quantizeSwing = 0;
    bool quantizeStarts = true, quantizeEnds = false;
    // The tools panel has two tabs: Live's Pitch and Time, with the chord tools, and Functions, ADI's
    // additions from Cubase's MIDI functions (ADR-0242 d1): velocities added to, compressed or
    // expanded, and limited; Delete Doubles; Restrict Polyphony. Their amounts are fields on the tab.
    int toolsTab() const noexcept { return toolsTab_; }
    void setToolsTab(int tab);
    juce::Rectangle<int> tabBox(int tab) const noexcept;
    static int tabOf(Tool) noexcept;
    int velocityAmount = 10, velocityPercent = 50, velocityLow = 1, velocityHigh = 127, voices = 4;
    // The chord type Insert Chord uses, chosen from its field's menu; items from 1.
    juce::Rectangle<int> chordBox() const noexcept;
    const ChordType &chordType() const noexcept { return kChordTypes[chord_]; }
    juce::PopupMenu chordMenu() const;
    bool chooseChord(int item);
    juce::Rectangle<int> toolsArea() const noexcept;
    juce::Rectangle<int> rangeBox() const noexcept;
    juce::Rectangle<int> toolBox(Tool) const noexcept;
    static const char *toolName(Tool) noexcept;
    bool runTool(Tool);
    bool transpose(int semitones);
    // The Scale field's two choosers (manual 8.2.5): the root notes, or the scales' names; and one
    // of their items chosen, from 1.
    juce::PopupMenu scaleMenu(bool roots) const;
    bool chooseScale(bool roots, int item);
    // The Main panel's fields (manual 8.2). A drag on one changes it by bars, beats or sixteenths,
    // as the part of the field grabbed; a click opens it for typing; Start, End, Position and
    // Length have Live's Set button, which takes the arrangement's insert marker. Each change is
    // one of the clip's own ops (N33), one undo step.
    enum class Field {
        None, Start, End, Position, Length, Signature, Scale, Transpose,
        VelocityAmount, VelocityPercent, VelocityLow, VelocityHigh, Voices,
        RandomAmount, RampStart, RampEnd, FilterA, FilterB, FilterC, QuantizeAmount, QuantizeSwing, Interval, Stretch, Name
    };
    // The title bar's context menu (manual 8.1.1): Rename, which opens the name for typing, and the
    // colour, one of the palette's fourteen. With several clips shown both act on all of them
    // (manual 10.8); one undo step. Item 1 is Rename; items 100 on are the colours.
    static constexpr int kTitleColours = 14;
    juce::PopupMenu titleMenu() const;
    bool chooseTitleItem(int item);
    bool rename(const juce::String &name);
    bool recolour(int colour);
    // Find and Select Notes (manual 10.5.3). The header's Find switch (Ctrl/Cmd+Shift+F in the
    // editor) shows a bar of note filters under the header: a chooser of the eight filters, the
    // chosen filter's controls, Invert and Select. A control changed applies the filter and selects
    // what every applied filter finds; an applied filter has a dot in the chooser; a plain click in
    // the editor lets all of them go. The number fields are the chosen filter's: Time counts whole
    // beats from the clip's Start (start, length, repeat), Duration counts sixteenths.
    // The editor's settings that are kept with the window (the grid, Highlight Scale, the notes'
    // colours, the Quantize settings): taken from the window's state as it opens, and written back
    // whenever one of them changes.
    void applyState(const WindowState &);
    // The Status Bar's Clip View Selector (manual 8.7): a drag in it scrolls the shown editor by
    // that share of the clip's length, and zooms it by the pixels moved up or down; and the time
    // the editor shows, in the song's ticks, for the selector's frame.
    void selectorDrag(double clipShare, int downBy);
    std::pair<std::int64_t, std::int64_t> shownSpan() const noexcept;
    void keepSettings();
    bool filtersShown() const noexcept { return filtersShown_; }
    void showFilters(bool);
    void clearFilters();
    NoteFilter filter() const noexcept { return filter_; }
    juce::PopupMenu filterMenu() const;
    bool chooseFilter(int item);
    bool applyFilter();
    bool toggleFilterInvert();
    // The chosen filter's switches: Pitch has twelve, Condition three, Count and Scale one each.
    int filterToggleCount() const noexcept;
    bool filterToggle(int index) const noexcept;
    bool clickFilterToggle(int index);
    juce::Rectangle<int> filterArea() const noexcept;
    juce::Rectangle<int> findBox() const noexcept;
    // The header's chooser of what the notes are coloured by.
    juce::Rectangle<int> coloursBox() const noexcept;
    // The editing grid's chooser, on the header's grid readout (manual 10.2.1): the adaptive
    // widths, the fixed ones, triplets and snapping. Items 1 to 5 are the adaptive widths, widest
    // first; 10 on, the fixed ones from 8 Bars to 1/32; 30 Triplet Grid; 31 Snap to Grid.
    juce::Rectangle<int> gridBox() const noexcept;
    juce::PopupMenu gridMenu(int idOffset = 0) const;
    bool chooseGrid(int item);
    juce::PopupMenu coloursMenu() const;
    bool chooseColours(int item);
    juce::Rectangle<int> filterChooserBox() const noexcept;
    juce::Rectangle<int> filterToggleBox(int index) const noexcept;
    juce::Rectangle<int> filterInvertBox() const noexcept;
    juce::Rectangle<int> filterSelectBox() const noexcept;
    NoteFilters filters;
    int filterTimeStart = 0, filterTimeLength = 1, filterTimeRepeat = 0, filterDurationMin = 1, filterDurationMax = 4;
    // Live's Velocity Controls (manual 10.5.12), in a strip under the editor's lanes: Randomize
    // with its amount, and the Ramp's start and end values. Randomize raises or lowers each
    // selected note's velocity (every note's when none is selected) by up to the amount; typing
    // the amount and Enter randomizes too. A Ramp value set, typed or dragged, spreads the notes'
    // velocities evenly from the start value at the first note to the end value at the last.
    static constexpr int kStripHeight = 18;
    juce::Rectangle<int> velocityStripArea() const noexcept;
    juce::Rectangle<int> randomizeBox() const noexcept;
    bool randomize();
    bool ramp();
    int randomAmount = 20, rampStart = 100, rampEnd = 100;
    // One random value from -amount to +amount; a test puts its own in.
    std::function<int(int)> randomOffset;
    juce::Rectangle<int> fieldBox(Field) const noexcept;
    juce::Rectangle<int> setBox(Field) const noexcept;
    juce::Rectangle<int> loopBox() const noexcept;
    Field fieldAt(juce::Point<int>) const noexcept;
    // A value typed into a field: a position as 1.1.1, a length as 2.0.0, a signature as 3/4.
    bool typeField(Field, const juce::String &);
    // A field set to a time, in the clip's own ticks.
    bool setField(Field, std::int64_t ticks);
    // The field's Set button.
    bool setFieldHere(Field);
    bool toggleLoop();
    // The field open for typing, and its editor.
    Field editing() const noexcept { return editing_; }
    juce::TextEditor &fieldEditor() noexcept { return editor_; }
    // The clip shown: 0 for none. It stays while nothing else is selected, as Live's does.
    std::int64_t clip() const noexcept { return clip_; }
    const engine::ClipDetails *details() const noexcept { return details_ ? &*details_ : nullptr; }
    // Why the clip, or its notes, cannot be shown; empty when they can.
    const std::string &problem() const noexcept { return problem_; }
    bool midi() const noexcept { return details_ && details_->kind == "midi"; }
    bool audio() const noexcept { return details_ && details_->kind == "audio"; }
    // Multi-clip editing (manual 10.8): the other selected MIDI clips, up to eight in all, drawn in
    // the same editor, each in its own colour. Editing applies to the clip being edited; `activate`
    // makes another of them that clip. Focus (N, and the header's switch) greys the others.
    static constexpr std::size_t kMaxClips = 8;
    const std::vector<ClipLayer> &others() const noexcept { return others_; }
    bool activate(std::int64_t clip);
    bool focus() const noexcept { return focus_; }
    void setFocus(bool);
    juce::Rectangle<int> focusBox() const noexcept;
    // The header's Scale switch (Fold to Scale, G) and Highlight switch (Highlight Scale, K); and
    // the Main panel's Scale Mode switch, beside the Scale field's name: on, the clip holds its own scale.
    juce::Rectangle<int> scaleFoldBox() const noexcept;
    juce::Rectangle<int> highlightBox() const noexcept;
    juce::Rectangle<int> scaleModeBox() const noexcept;
    bool toggleScaleMode();
    // The clip in the frame's snapshot: its audio source and its track.
    const engine::ClipNode *node() const noexcept { return node_; }
    juce::Colour colour() const noexcept { return colour_; }
    const textproj::Meter &meter() const noexcept { return meter_; }
    juce::Rectangle<int> activatorArea() const noexcept;
    juce::Rectangle<int> titleArea() const noexcept;
    juce::Rectangle<int> mainArea() const noexcept;
    juce::Rectangle<int> headerArea() const noexcept;
    // The Note Editor's Fold switch, in the header (manual 10.2).
    juce::Rectangle<int> foldBox() const noexcept;
    // Its Draw Mode switch beside it (manual 10.4.1).
    juce::Rectangle<int> drawBox() const noexcept;
    // The trim tool's switch (ADR-0241 d2).
    juce::Rectangle<int> trimBox() const noexcept;
    juce::Rectangle<int> editorArea() const noexcept;
    // The Main panel's readings, as drawn (manual 8.2): Start, End, Position, Length, Signature.
    struct Readings {
        juce::String start, end, position, length, signature, scale, grid, transpose, range, title;
        bool loop = false;
        // With several clips shown (manual 10.8): some loop and some do not.
        bool loopMixed = false;
    };
    Readings readings() const;
    // Reads the clip again now: after an edit of this view's, before the next frame.
    void reread();
    // The notes last cut or copied in this window's Clip View, each with its start counted from the
    // first of them; they stay while other clips are opened.
    std::vector<engine::ClipNote> noteClipboard;
    // Where each of them came from, and which clips were shown, and which was being edited, when
    // they were copied: while that is unchanged, Paste puts each note back into its own clip
    // (manual 10.8); after another clip is chosen, all go into the edited one.
    std::vector<std::int64_t> noteClipboardClips, noteClipboardShown;
    std::int64_t noteClipboardEdited = 0;
    // The shown clips' ids, the edited one first.
    std::vector<std::int64_t> shownClips() const;
    // Every place this panel and its editor draw text, in the panel's coordinates.
    std::vector<juce::Rectangle<int>> textAreas() const;

    AdiRootComponent &root;
    NoteEditor notes;
    SampleEditor sample;

  private:
    void read(const SnapshotReader &);
    friend class NoteEditor;
    bool commit(TrackEdit);
    std::int64_t fieldTicks(Field) const noexcept;
    std::int64_t draggedTicks() const noexcept;
    std::pair<std::int64_t, std::int64_t> draggedSignature() const noexcept;
    void openEditor(Field);
    void closeEditor();
    void showScaleMenu(bool roots);
    // A field of the Functions tab: the number it holds and its range; null for any other field.
    int *plainField(Field, int *low = nullptr, int *high = nullptr) noexcept;
    // What follows a number field's change: a Ramp value ramps; the Randomize amount typed randomizes.
    void numberChanged(Field, bool typed);
    // An edit of the Main panel's that belongs to every shown clip (manual 10.8: loop, signature and
    // scale apply to all the selected clips): `build` makes it for one clip; one batch, one undo
    // step. The edited clip's refusal refuses it; another clip that cannot take it is passed over.
    bool commitForAll(const std::function<TrackEdit(const engine::ClipDetails &)> &build);
    bool filtersShown_ = false;
    NoteFilter filter_ = NoteFilter::Pitch;
    // How many number fields the chosen filter has, and their names.
    int filterFieldCount() const noexcept;
    const char *filterFieldName(int slot) const noexcept;
    const char *filterToggleName(int index) const noexcept;
    std::mt19937 random_{std::random_device{}()};
    std::int64_t clip_ = 0, wanted_ = 0;
    std::vector<std::int64_t> shown_;
    std::vector<ClipLayer> others_;
    bool focus_ = false;
    std::uint64_t generation_ = 0, statusStamp_ = 0;
    bool read_ = false;
    std::optional<engine::ClipDetails> details_;
    std::string problem_;
    const engine::ClipNode *node_ = nullptr;
    // The edited clip's track's name, from this frame's snapshot: what an unnamed clip's title reads.
    std::string_view trackName_;
    juce::Colour colour_;
    textproj::Meter meter_;
    std::vector<textproj::Meter> clipMeters_;
    mutable std::vector<juce::Rectangle<int>> labels_;
    Field pressed_ = Field::None, editing_ = Field::None;
    bool dragged_ = false;
    int pressPart_ = 0, dragSteps_ = 0;
    std::size_t chord_ = 0, quantizeValue_ = 0, duration_ = 0;
    int toolsTab_ = 0;
    juce::Point<int> pressAt_;
    juce::TextEditor editor_;
};

// The Clip View's top edge: dragging it grows the view, and dragging it to the window's foot
// closes it (manual 8).
class ClipViewResizer final : public juce::Component {
  public:
    explicit ClipViewResizer(AdiRootComponent &);
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;

  private:
    AdiRootComponent &root;
    int start = 0;
    bool closing = false;
};
} // namespace adi::ui
