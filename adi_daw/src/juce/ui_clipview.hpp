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
#include "adi/ui/note_edits.hpp"
#include "adi/ui/note_geometry.hpp"
#include "adi/ui/scales.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "ui_arrangement.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include <string>
#include <vector>
namespace adi::ui {
class AdiRootComponent;
class ClipViewPanel;

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
    // A note's box in this component; none while Fold hides its key.
    std::optional<NoteBox> boxOf(const engine::ClipNote &) const noexcept;
    // Opens on a clip: its whole length across the editor, its notes in view (manual 10.2).
    void showClip();
    // Where this component draws text, for a picture that masks it (each platform's fonts differ).
    std::vector<juce::Rectangle<int>> textAreas() const;

  private:
    enum class Drag { None, Move, Start, End, Velocity, Band, Draw };
    bool commit(NoteEdit);
    bool snapping(const juce::ModifierKeys &) const noexcept;
    ClipViewPanel &panel;
    bool fitPending_ = false;
    mutable std::vector<juce::Rectangle<int>> labels_;
    std::vector<std::uint64_t> selected_, bandBase_;
    Drag drag_ = Drag::None;
    bool copying_ = false, folded_ = false, drawMode_ = false;
    int drawKey_ = 0;
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
        FixedLength, OverlapsMono, OverlapsPoly, Chord, InvertUp, InvertDown, Drop2, Drop3
    };
    static constexpr Tool kTools[] = {Tool::FitToScale, Tool::Invert,       Tool::Reverse,      Tool::Double,     Tool::Halve,
                                      Tool::Legato,     Tool::FixedLength,  Tool::OverlapsMono, Tool::OverlapsPoly, Tool::Chord,
                                      Tool::InvertUp,   Tool::InvertDown,   Tool::Drop2,        Tool::Drop3};
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
    enum class Field { None, Start, End, Position, Length, Signature, Scale, Transpose };
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
    juce::Rectangle<int> editorArea() const noexcept;
    // The Main panel's readings, as drawn (manual 8.2): Start, End, Position, Length, Signature.
    struct Readings {
        juce::String start, end, position, length, signature, scale, grid, transpose, range;
        bool loop = false;
    };
    Readings readings() const;
    // Reads the clip again now: after an edit of this view's, before the next frame.
    void reread();
    // Every place this panel and its editor draw text, in the panel's coordinates.
    std::vector<juce::Rectangle<int>> textAreas() const;

    AdiRootComponent &root;
    NoteEditor notes;
    SampleEditor sample;

  private:
    void read(const SnapshotReader &);
    bool commit(TrackEdit);
    std::int64_t fieldTicks(Field) const noexcept;
    std::int64_t draggedTicks() const noexcept;
    std::pair<std::int64_t, std::int64_t> draggedSignature() const noexcept;
    void openEditor(Field);
    void closeEditor();
    void showScaleMenu(bool roots);
    std::int64_t clip_ = 0, wanted_ = 0;
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
