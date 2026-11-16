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
#include "adi/ui/note_geometry.hpp"
#include "adi/ui/scales.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "ui_arrangement.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include <string>
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
    enum class Drag { None, Move, Start, End, Velocity, Band, Draw, Shape, Trim, Handle, Ruler };
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
    bool copying_ = false, folded_ = false, drawMode_ = false;
    int drawKey_ = 0;
    LaneShape shape_ = LaneShape::Pointer;
    LaneHandle handle_ = LaneHandle::None;
    bool trimMode_ = false;
    std::vector<std::pair<std::uint64_t, std::int64_t>> trimPreview_;
    std::vector<std::pair<std::uint64_t, int>> shapePreview_;
    juce::Point<int> from_, to_;
    std::int64_t dragTicks_ = 0;
    int dragKeys_ = 0, dragVelocity_ = 0;
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

    static constexpr int kPanelWidth = 156, kToolsWidth = 150, kTitleHeight = 17, kHeaderHeight = 17;
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
        VelocityAdd, VelocityScale, VelocityLimit, DeleteDoubles, RestrictPolyphony
    };
    static constexpr Tool kTools[] = {Tool::FitToScale, Tool::Invert,       Tool::Reverse,      Tool::Double,     Tool::Halve,
                                      Tool::Legato,     Tool::FixedLength,  Tool::OverlapsMono, Tool::OverlapsPoly, Tool::Chord,
                                      Tool::InvertUp,   Tool::InvertDown,   Tool::Drop2,        Tool::Drop3,
                                      Tool::VelocityAdd, Tool::VelocityScale, Tool::VelocityLimit, Tool::DeleteDoubles,
                                      Tool::RestrictPolyphony};
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
        VelocityAmount, VelocityPercent, VelocityLow, VelocityHigh, Voices
    };
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
        juce::String start, end, position, length, signature, scale, grid, transpose, range;
        bool loop = false;
    };
    Readings readings() const;
    // Reads the clip again now: after an edit of this view's, before the next frame.
    void reread();
    // The notes last cut or copied in this window's Clip View, each with its start counted from the
    // first of them; they stay while other clips are opened.
    std::vector<engine::ClipNote> noteClipboard;
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
    std::int64_t clip_ = 0, wanted_ = 0;
    std::vector<std::int64_t> shown_;
    std::vector<ClipLayer> others_;
    bool focus_ = false;
    std::uint64_t generation_ = 0;
    bool read_ = false;
    std::optional<engine::ClipDetails> details_;
    std::string problem_;
    const engine::ClipNode *node_ = nullptr;
    juce::Colour colour_;
    textproj::Meter meter_;
    std::vector<textproj::Meter> clipMeters_;
    mutable std::vector<juce::Rectangle<int>> labels_;
    Field pressed_ = Field::None, editing_ = Field::None;
    bool dragged_ = false;
    int pressPart_ = 0, dragSteps_ = 0;
    std::size_t chord_ = 0;
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
