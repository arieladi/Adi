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
#include "adi/ui/note_geometry.hpp"
#include "adi/ui/snapshot_reader.hpp"
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
    static constexpr int kKeysWidth = 44, kRulerHeight = 18, kVelocityHeight = 56;
    // Time on the arrangement's own axis, with the editor's own zoom, scroll and grid.
    ArrangementGeometry time;
    KeyTracks keys;
    // The key tracks, and the Velocity lane under them, in this component.
    juce::Rectangle<int> notesArea() const noexcept;
    juce::Rectangle<int> velocityArea() const noexcept;
    juce::Rectangle<int> rulerArea() const noexcept;
    juce::Rectangle<int> keysArea() const noexcept;
    // A note's box in this component; none while Fold hides its key.
    std::optional<NoteBox> boxOf(const engine::ClipNote &) const noexcept;
    // Opens on a clip: its whole length across the editor, its notes in view (manual 10.2).
    void showClip();
    // Where this component draws text, for a picture that masks it (each platform's fonts differ).
    std::vector<juce::Rectangle<int>> textAreas() const;

  private:
    ClipViewPanel &panel;
    bool fitPending_ = false;
    mutable std::vector<juce::Rectangle<int>> labels_;
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

    static constexpr int kPanelWidth = 156, kTitleHeight = 17, kHeaderHeight = 17;
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
    juce::Rectangle<int> editorArea() const noexcept;
    // The Main panel's readings, as drawn (manual 8.2): Start, End, Position, Length, Signature.
    struct Readings {
        juce::String start, end, position, length, signature, scale, grid;
        bool loop = false;
    };
    Readings readings() const;
    // Every place this panel and its editor draw text, in the panel's coordinates.
    std::vector<juce::Rectangle<int>> textAreas() const;

    AdiRootComponent &root;
    NoteEditor notes;
    SampleEditor sample;

  private:
    void read(const SnapshotReader &);
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
