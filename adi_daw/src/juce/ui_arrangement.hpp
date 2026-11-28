// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/arrangement_geometry.hpp"
#include "adi/ui/automation_mode.hpp"
#include "adi/ui/locator_edits.hpp"
#include "adi/ui/loop_edits.hpp"
#include "adi/ui/track_edits.hpp"
#include "adi/ui/waveform.hpp"
#include "adi/ui/track_tree.hpp"
#include "ui_look.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <vector>
namespace adi::engine { struct AudioRecordingPreview; }
namespace adi::automation { enum class Shape; }

namespace adi::ui {
class AdiRootComponent;
class ArrangementView;
struct LiveMidiTake;
inline constexpr float kPlaybackProblemMarkSize = 9.f;
void paintPlaybackProblemMark(juce::Graphics &, juce::Rectangle<float>);
// The Overview (Live 12 manual 6.1, ADR-0129): the whole arrangement in miniature,
// an outline around the part on screen. Drag sideways to scroll, vertically to zoom;
// a double-click zooms out to the whole arrangement.
class ArrangementOverview final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit ArrangementOverview(ArrangementView &);
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    // The span the overview draws, in ticks: the arrangement, or what is on screen if longer.
    std::int64_t span() const noexcept;
    // The outline around the visible part, in the overview's pixels.
    juce::Rectangle<float> outline() const noexcept;

  private:
    ArrangementView &view;
    juce::Point<float> last;
};
// The title bar's own editor for Rename: Enter commits, Esc cancels, Tab commits
// and moves on to the next track's title (Shift+Tab, the previous), as Live.
class RenameEditor final : public juce::TextEditor {
  public:
    bool keyPressed(const juce::KeyPress &) override;

    std::function<void()> onCommit, onCancel;
    std::function<void(bool forward)> onTab;
};
// The scrub area (Live 12 manual 6.1, p. 161): a click anywhere in it launches
// playback from that point. Holding to loop waits for the transport's loop.
// Its time signature markers (manual 6.5): a click on one selects it; its menu holds Insert Time
// Signature Change, and on a marker Edit Time Signature and Delete.
class ScrubArea final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit ScrubArea(ArrangementView &);
    void paint(juce::Graphics &) override;
    void mouseEnter(const juce::MouseEvent &) override;
    void mouseMove(const juce::MouseEvent &) override;
    void mouseExit(const juce::MouseEvent &) override;
    void mouseDown(const juce::MouseEvent &) override;
    // A marker dragged follows the mouse, to the nearest grid line while snapping, never before the
    // first grid line; let go, it moves there, one step (manual 6.5).
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    // While a marker is dragged: where it would land; else -1.
    std::int64_t movingTo() const noexcept { return dragged_ >= 0 ? movingTo_ : -1; }
    // Live's loop brace (manual 6.6) along the band's top, over the loop region, dark while the loop
    // is on: a press on it selects it, and the material in the loop with it (Select Loop); dragged by
    // an edge it moves that end, by its bar it moves whole, its length kept, each to the nearest grid
    // line while snapping; let go, the loop is set there, outside undo.
    enum class BracePart { None, Start, End, Bar };
    static constexpr float kBraceHeight = 8.f, kBraceEdge = 5.f;
    BracePart bracePart(juce::Point<float>) const;
    juce::Rectangle<float> braceBox(LoopRange) const;
    // While the brace is dragged: where it would be; else none.
    std::optional<LoopRange> braceMoving() const noexcept {
        return brace_ != BracePart::None ? std::optional<LoopRange>(braceTo_) : std::nullopt;
    }
    // Live's locators (manual 6.4) in the band under the brace, beside the time signature markers: a
    // triangle at each, its name after it. A click selects one and jumps there; a double-click while
    // stopped selects it and plays from it; a drag moves it, to the nearest grid line while snapping,
    // one step when let go. The locator drawn under `x`, by id, or 0.
    std::int64_t locatorAt(float x) const;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    // While a locator is dragged: where it would land; else -1.
    std::int64_t locatorMovingTo() const noexcept { return locatorDragged_ != 0 ? locatorTo_ : -1; }
    // Rename (Ctrl/Cmd+R) or Edit Info Text on a locator: a field over it, holding its name or its
    // info text, selected; Enter commits, one step; Esc cancels.
    bool beginLocatorEdit(std::int64_t id, bool info);
    std::int64_t locatorEditing() const noexcept { return locatorEditing_; }
    // The position of the time signature marker drawn under `x`, or -1.
    std::int64_t markerAt(float x) const;
    // Insert Time Signature Change at `pos` (inserting), or Edit Time Signature on the marker there:
    // a field over the marker, holding the signature in force there, selected; Enter commits, Esc
    // cancels. A value refused keeps the field open, and the Status Bar says why.
    bool beginMeterEdit(std::int64_t pos, bool inserting);
    std::int64_t meterEditing() const noexcept { return editing_; }
    // The scrub area's menu at `at` (a tick): Live's two commands on a fragmentary bar join it there.
    juce::PopupMenu menu(std::int64_t at = -1);
    RenameEditor editor;

  private:
    juce::Rectangle<float> markerBox(const textproj::Meter &) const;
    juce::Rectangle<float> locatorBox(const Locator &) const;
    void repairFragmentaryBar(std::int64_t at, bool complete);
    void endMeterEdit();
    // The locator being renamed or given info text, and which; the locator pressed, the ticks from
    // it to the press, where a drag would put it, and whether the mouse moved.
    std::int64_t locatorEditing_ = 0;
    bool editingInfo_ = false;
    std::int64_t locatorDragged_ = 0, locatorGrab_ = 0, locatorTo_ = -1;
    bool locatorMoved_ = false;
    ArrangementView &view;
    std::int64_t editing_ = -1;
    bool inserting_ = false;
    // The marker pressed, the ticks from its start to the press, and where a drag would put it.
    std::int64_t dragged_ = -1, grabbed_ = 0, movingTo_ = -1;
    // The brace's part pressed, the tick pressed, the brace then, and where a drag would put it.
    BracePart brace_ = BracePart::None;
    std::int64_t braceGrab_ = 0;
    LoopRange braceFrom_, braceTo_;
};
// The beat-time ruler (Live 12 manual 6.1): bars-beats-sixteenths; drag sideways
// scrolls, vertically zooms; a double-click zooms to the selection, or out to the
// whole arrangement when nothing is selected.
class TimelineRuler final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit TimelineRuler(ArrangementView &);
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    std::function<void(const juce::PopupMenu &)> onTempoMenu;
    RenameEditor tempoEditor;

  private:
    static constexpr int kTempoRowHeight = 12, kTempoLabelWidth = 64;
    void paintTempos(juce::Graphics &) const;
    void beginTempoEdit(std::int64_t);
    std::int64_t tempoPosition(const juce::MouseEvent &) const;
    ArrangementView &view;
    juce::Point<float> last;
    std::int64_t tempoEditing_ = 0;
    bool tempoMenuPressed_ = false;
};
// One row per track, on the RIGHT of the clips as in Live's Arrangement View (on the left,
// before the track controls, with the side panels swapped).
class TrackHeaderList final : public juce::Component,
                              public juce::SettableTooltipClient,
                              public juce::DragAndDropTarget,
                              public juce::FileDragAndDropTarget {
  public:
    explicit TrackHeaderList(ArrangementView &);
    void paint(juce::Graphics &) override;
    void resized() override;
    // Rename (Ctrl/Cmd+R, the Edit menu): edit this track's name in its title bar.
    bool beginRename(std::int64_t track, bool selectTrack = true);
    std::int64_t renaming() const noexcept { return renaming_; }
    RenameEditor editor;
    void mouseDown(const juce::MouseEvent &) override;
    // A double-click on a track's title brings up the Device View, as Live's.
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseMove(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDragEnter(const SourceDetails &details) override { itemDragMove(details); }
    void itemDragMove(const SourceDetails &) override;
    void itemDragExit(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;
    bool isInterestedInFileDrag(const juce::StringArray &) override;
    void fileDragEnter(const juce::StringArray &files, int x, int y) override { fileDragMove(files, x, y); }
    void fileDragMove(const juce::StringArray &, int, int) override;
    void fileDragExit(const juce::StringArray &) override;
    void filesDropped(const juce::StringArray &, int, int) override;

    bool keyPressed(const juce::KeyPress &) override;
    // Main and the Returns at the foot grow upwards. Other rows resize at their bottom edge.
    static constexpr int kSplitLine = 5;
    static constexpr bool onResizeEdge(int y, int height, bool anchored) noexcept {
        return y >= 0 && y < height && (anchored ? y < kSplitLine : y >= height - kSplitLine);
    }
    // Where tracks dragged by their titles would land with the mouse at `y` (Live: "dragging them
    // above or below other tracks", "into or out of the group"): a line at `y`, at the depth they
    // would stand, or the folded or empty group `into`, which they would go to the end of.
    struct Drop {
        TrackDrop to;
        int y = 0;
        std::uint32_t depth = 0;
        std::int64_t into = 0;
    };
    Drop dropAt(int y) const;
    // Where a dragged return would land: among the returns at the foot, before the one under the
    // mouse's upper half or after the one under its lower half; over a group's row or a row it
    // holds, into that group, after its returns (ADR-0247 d2); over the other tracks, before the
    // first; over Main or below, after the last.
    Drop returnDropAt(int y) const;
    // The drop shown while tracks are dragged; empty otherwise.
    std::optional<Drop> drop() const { return moving_ ? std::optional<Drop>(drop_) : std::nullopt; }

    // Where a row's colour and name begin, past its groups' bands; and its fold switch.
    int nameLeft(std::uint32_t depth) const noexcept;
    juce::Rectangle<float> foldSwitch(std::uint32_t depth, int top) const noexcept;
    // A MIDI track's Rec-Q and Play-Q boxes under its name (MR V.2 §9; N31), in a row tall enough for
    // them; empty where the row is not.
    juce::Rectangle<int> quantizeBox(std::uint32_t depth, int top, int height, bool record) const noexcept;
    // The box's menu: Free (Rec-Q) or Off (Play-Q), then the four grids, the track's ticked.
    juce::PopupMenu quantizeMenu(std::int64_t track, bool record);
    juce::Rectangle<int> automationChooserBox(std::uint32_t depth, int top, int height,
                                              bool device, bool midi = false) const noexcept;
    juce::PopupMenu automationChooserMenu(std::int64_t track, bool device);
    juce::Rectangle<int> automationLaneButtonBox(std::int64_t track, std::uint32_t depth, int top,
                                                 bool add) const noexcept;
    juce::Rectangle<int> automationLaneRemoveBox(std::uint32_t depth, int top, int height) const noexcept;
    // The bands drawn at most: deeper groups share the last one's place.
    static constexpr std::uint32_t kMaxBands = 16;

  private:
    bool commitRename();
    void endRename();
    ArrangementView &view;
    std::int64_t resizeId = 0, renaming_ = 0;
    int initialHeight = 0;
    bool resizeFromTop_ = false;
    // The track whose title was pressed, to move once the mouse has gone far enough; whether it
    // is moving; whether the press, on one of several selected tracks, selects it alone on release
    // without a drag (Live keeps the selection for the drag).
    std::int64_t moveId_ = 0;
    bool moving_ = false, movingReturn_ = false;
    std::int64_t selectOnUp_ = 0;
    // Whether the press was on a fold switch, which does all it does on the press: its release keeps
    // what the Status Bar says.
    bool foldPressed_ = false;
    bool automationLaneButtonPressed_ = false;
    Drop drop_;
};
// What a screen reader finds for one clip on screen (UI-ARCHITECTURE §6). The
// canvas paints every clip itself; JUCE gathers accessibility children only from
// child components, so each visible clip gets one of these: it paints nothing, the
// mouse passes through it, and it is recycled as the view scrolls (§10).
class ClipAccessible final : public juce::Component {
  public:
    explicit ClipAccessible(ArrangementView &);
    void bind(std::int64_t track, std::int64_t clip);
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    std::int64_t track = 0, clip = 0;

  private:
    ArrangementView &view;
};
// ONE component paints every clip and hit-tests them itself (ADR-0044).
class ArrangementCanvas final : public juce::Component,
                                public juce::FileDragAndDropTarget,
                                public juce::DragAndDropTarget,
                                public juce::SettableTooltipClient {
  public:
    explicit ArrangementCanvas(ArrangementView &);
    // The engine projection supplies sorted, unique clip IDs.
    void setPlaybackProblemIds(std::vector<std::int64_t> ids);
    [[nodiscard]] bool hasPlaybackProblem(std::int64_t id) const noexcept;
    bool hasInfoAt(juce::Point<int>) const;
    juce::String getTooltip() override;
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    bool keyPressed(const juce::KeyPress &) override;
    bool editMaterialSelection(bool deactivate);
    // Automation Mode (AU-01): each track's lane body, where its envelope is drawn and edited; the
    // breakpoint under a point, if any; the one chosen for Delete.
    struct LaneBody {
        const engine::TrackNode *track = nullptr;
        float top = 0, bottom = 0; // the part drawn, cut at the foot
        float scaleBottom = 0;     // the lane's own foot, which the values scale to
        AutomationTarget target;
    };
    std::vector<LaneBody> laneBodies() const;
    bool hasAutomationSelection() const;
    void clearAutomationSelection();
    std::optional<std::vector<automation::LaneSpec>> automationRangeLanes() const;
    std::optional<AutomationTarget> automationCommandTarget() const;
    void selectAutomationSpan(std::int64_t begin, std::int64_t end);
    struct AutomationPick {
        std::int64_t track = 0;
        std::uint64_t point = 0;
        AutomationTarget target;
        std::int64_t lane = 0, data = 0;
    };
    std::optional<AutomationPick> automationPointAt(juce::Point<float>) const;
    // The lane whose envelope line, as drawn, passes within a few pixels of a point (A14).
    std::optional<LaneBody> automationLineAt(juce::Point<float>) const;
    const std::optional<AutomationPick> &chosenAutomationPoint() const noexcept { return chosenPoint_; }
    void mouseMagnify(const juce::MouseEvent &, float) override;
    bool isInterestedInFileDrag(const juce::StringArray &) override;
    void fileDragEnter(const juce::StringArray &, int, int) override;
    void fileDragMove(const juce::StringArray &, int, int) override;
    void fileDragExit(const juce::StringArray &) override;
    // A sample dragged from the browser (D1) lands as a file dropped from the desktop does.
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDragEnter(const SourceDetails &) override;
    void itemDragMove(const SourceDetails &) override;
    void itemDragExit(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;
    void filesDropped(const juce::StringArray &, int, int) override;
    // The names drop goes to the same lane, at the insert marker for a sample.
    void previewSampleDrop(int y, std::int64_t tick, int files, bool existingTrackOnly);
    void previewDeviceDrop(int y, bool existingTrackOnly);
    void clearDropPreview();
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    // One accessible child per clip on screen; on a model or view change only.
    void realiseAccessible();
    // What the reader says for a clip: its track, where it starts, how long it is.
    juce::String describe(std::int64_t track, std::int64_t clip) const;
    const std::vector<std::unique_ptr<ClipAccessible>> &accessible() const noexcept {
        return accessible_;
    }
    juce::String lastAnnouncement;
    // The clip being moved by its bar (§6.7), and where it would land; 0 when none is.
    std::int64_t movingClip() const noexcept { return moving_; }
    // The track a clip being dragged would land on: its own until the mouse is over another lane
    // that can take it.
    std::int64_t movingTrack() const noexcept { return movingTrack_; }
    std::int64_t movingTo() const noexcept { return movingTo_; }
    // The clip whose right edge is being dragged (§6.7), and its length so far; 0 when none.
    std::int64_t resizingClip() const noexcept { return resizing_; }
    std::int64_t resizedLength() const noexcept;
    // A clip's bar, Live's clip title strip: where a drag moves the clip (§6.7), and the
    // width of its right edge, where a drag changes its length.
    static constexpr int kClipBarHeight = 11, kClipEdge = 4;
    void mouseMove(const juce::MouseEvent &) override;
    // Whether the clip's length is being changed from the keyboard: Enter with the insert
    // marker at its end, the arrows, then Enter to keep it or Esc to cancel (§41.4.1.3).
    bool keyResizing() const noexcept { return resizing_ != 0 && keyResize_; }
    // Live's fade handles on audio clips (Live 12 manual 6.8; E4): a square at each fade's peak sets
    // its length, a lower square its start, and a diamond its curve. They show over the clip under the mouse, on the
    // selected clips, and on every audio clip while F is held. A drag is one `clip.setFades` batch
    // for the dragged clip and every other selected audio clip.
    struct FadeHandle {
        std::int64_t clip = 0, track = 0;
        bool in = true, curve = false, start = false;
        friend bool operator==(const FadeHandle &, const FadeHandle &) = default;
    };
    std::optional<FadeHandle> fadeHandleAt(int x, int y) const;
    // Where a handle is drawn, for its clip; nothing when the handle is not shown.
    std::optional<juce::Point<float>> fadeHandlePoint(const FadeHandle &) const;
    // The handle being dragged, and the fade it would set; none when no handle is.
    std::optional<FadeHandle> fadeDragging() const noexcept { return fadeDrag_; }
    // The handle last clicked: Delete deletes its fade (manual 6.8).
    std::optional<FadeHandle> selectedFade() const noexcept { return selectedFade_; }
    // F held: every audio clip's handles show (manual 6.8, §42.25's momentary keys).
    bool showFades = false;
    bool keyStateChanged(bool isKeyDown) override;
    void focusLost(FocusChangeType) override;
    void mouseExit(const juce::MouseEvent &) override;
    // Rename on a selected clip (Ctrl/Cmd+R, the clip's menu; Live 12 manual 6.9, walk-B B14): a field
    // over its title bar holding its name; Enter (or Tab, or leaving it) commits one "Rename Clip"
    // step, Esc cancels. An empty name is a clip with no name, which shows its track's.
    bool beginClipRename(std::int64_t clip);
    std::int64_t renamingClip() const noexcept { return renamingClip_; }
    RenameEditor clipEditor;
    RenameEditor &automationValueEditor() noexcept { return automationValueEditor_; }
    const juce::String &automationValueReadout() const noexcept { return automationValueReadout_; }
    void resetAutomationValueInput();
    void cancelAutomationAddDrag();

  private:
    void clearReleasedFadeHandles();
    struct DropPreview {
        juce::Rectangle<int> bounds;
        bool device = false;
        friend bool operator==(const DropPreview &, const DropPreview &) = default;
    };
    void setDropPreview(DropPreview);
    std::pair<const engine::TrackNode *, juce::Rectangle<int>> dropLane(int y) const;
    std::int64_t audioDropTick(int x) const;
    DropPreview dropPreview_;
    std::vector<std::int64_t> playbackProblemIds_;
    bool commitClipRename();
    void endClipRename();
    std::int64_t renamingClip_ = 0;
    // A clip's body on screen (under its title strip), and the clip and track: every clip drawn.
    struct ClipBody {
        const engine::TrackNode *track = nullptr;
        const engine::ClipNode *clip = nullptr;
        float left = 0, right = 0, top = 0, bottom = 0;
    };
    std::vector<ClipBody> clipBodies() const;
    std::optional<juce::Point<float>> fadeHandlePoint(const FadeHandle &, const ClipBody &) const;
    bool fadeHandlesShown(const engine::ClipNode &) const;
    // A clip's fades as drawn: what the snapshot holds, or what a drag would set.
    void fadesOf(const engine::ClipNode &, engine::ClipFadeHandles &in, engine::ClipFadeHandles &out, std::optional<double> &inTension,
                 std::optional<double> &outTension) const;
    void paintFades(juce::Graphics &, const engine::ClipNode &, float left, float right, float top,
                    float bottom) const;
    std::optional<FadeHandle> fadeDrag_, selectedFade_;
    // Automation Mode: the breakpoint being dragged, where it is shown now, and the chosen one.
    void paintAutomation(juce::Graphics &) const;
    bool automationDown(const juce::MouseEvent &);
    bool beginAutomationStroke(const juce::MouseEvent &);
    void updateAutomationStroke(const juce::MouseEvent &);
    struct AutomationStroke {
        AutomationTarget target;
        std::int64_t track = 0, lastTick = 0, end = 0;
        std::uint64_t generation = 0;
        std::map<std::int64_t, double> values;
        float top = 0, bottom = 0, lastY = 0, valueY = 0;
        double lastValue = 0;
        bool stepped = false, cancelled = false;
    };
    std::optional<AutomationStroke> automationStroke_;
    bool automationSelectionDown(const juce::MouseEvent &);
    void rememberAutomationSelection(const std::vector<LaneBody> &, std::size_t destination, bool restart);
    bool automationLaneSelected(const LaneBody &) const;
    bool automationPickIsCurrent(const AutomationPick &) const;
    std::optional<Selection> automationSelection_;
    using AutomationLaneKey = std::pair<std::int64_t, AutomationTarget>;
    std::optional<AutomationLaneKey> automationSelectionAnchor_;
    std::vector<AutomationLaneKey> automationSelectionTargets_;
    enum class AutomationTransformHandle : std::uint8_t {
        TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left
    };
    struct AutomationTransformBounds {
        AutomationLaneKey target;
        std::int64_t laneId, dataId, begin, end;
        float top, bottom, visibleBottom;
    };
    struct AutomationTransformDrag {
        AutomationTransformBounds bounds;
        AutomationTransformHandle handle;
        juce::Point<float> lastPosition;
        juce::Point<float> distance;
        std::int64_t begin, end;
        std::array<double, 4> values{0.0, 1.0, 0.0, 1.0};
        std::uint64_t generation = 0;
        bool cancelled = false;
    };
    std::optional<AutomationTransformBounds> automationTransformBounds() const;
    juce::Rectangle<float> automationTransformBox(const AutomationTransformBounds &) const;
    std::array<juce::Point<float>, 8> automationTransformHandles(const AutomationTransformBounds &) const;
    std::optional<AutomationTransformHandle> automationTransformHandleAt(const AutomationTransformBounds &,
                                                                         juce::Point<float>) const;
    bool beginAutomationTransform(const juce::MouseEvent &);
    void updateAutomationTransform(const juce::MouseEvent &);
    void finishAutomationTransform(const juce::MouseEvent &);
    void paintAutomationTransform(juce::Graphics &) const;
    std::optional<juce::Point<float>> automationTransformHover_;
    std::optional<AutomationTransformDrag> automationTransformDrag_;
    struct AutomationValueAddress {
        AutomationTarget target;
        std::int64_t track=0,lane=0,data=0,tick=0;
        std::optional<std::uint64_t> point;
        double value=0;
        juce::Point<int> anchor;
    };
    std::optional<AutomationValueAddress> automationValueAt(juce::Point<float>,bool snapToGrid,
                                                           bool allowAdd) const;
    void automationContextMenu(const juce::MouseEvent &);
    bool draggingTakeSelection_ = false;
    bool draggingTimeRange_ = false;
    std::int64_t takeAnchor_ = 0;
    void simplifyAutomationEnvelope(const AutomationValueAddress &, std::int64_t begin,
                                    std::int64_t end, std::uint64_t generation);
    void insertAutomationShape(const AutomationValueAddress &, automation::Shape, std::int64_t begin,
                               std::int64_t end, std::uint64_t generation);
    void beginAutomationValueEdit(const AutomationValueAddress &);
    void commitAutomationValue();
    void endAutomationValueEdit(bool refocus=true);
    bool resolveAutomationValue(const AutomationValueAddress &,const TrackEnvelope &) const;
    void showAutomationValue(const AutomationTarget &,double,juce::Point<float>);
    void clearAutomationValueReadout();
    void paintAutomationValueReadout(juce::Graphics &) const;
    RenameEditor automationValueEditor_;
    std::optional<AutomationValueAddress> automationValueAddress_;
    struct KeyboardAutomationDraft {
        AutomationValueAddress address;
        std::int64_t tick;
        double value;
    };
    std::optional<AutomationLaneKey> keyboardAutomationLane_;
    std::optional<AutomationPick> keyboardAutomationFocus_;
    std::optional<KeyboardAutomationDraft> keyboardAutomationDraft_;
    bool keyboardValueEditing_ = false;
    std::optional<AutomationLaneKey> automationKeyboardLane() const;
    void focusAutomationKeyboardLane(const AutomationLaneKey &, std::int64_t tick,
                                     std::optional<AutomationPick> focused = {});
    bool navigateAutomationWithAlt(const juce::KeyPress &, const AutomationLaneKey &);
    bool automationKeyPressed(const juce::KeyPress &);
    juce::String automationValueReadout_;
    juce::GlyphArrangement automationValueGlyphs_;
    juce::Rectangle<float> automationValueReadoutBounds_;
    struct AutomationAddDrag {
        AutomationTarget target;
        std::int64_t track, tick;
        double value;
        std::uint64_t generation;
        bool cancelled = false;
    };
    struct AutomationDragPoint {
        std::uint64_t id;
        std::int64_t time;
        double value;
        std::uint8_t curve;
        float tension;
    };
    struct AutomationDragLane {
        std::int64_t track = 0, lane = 0, data = 0;
        AutomationTarget target;
        std::vector<AutomationDragPoint> points;
        std::vector<std::int64_t> pointTimes;
        std::vector<std::uint64_t> overwritten;
    };
    struct AutomationDrag {
        enum class Axis { Undecided, Horizontal, Vertical };
        std::vector<AutomationDragLane> lanes;
        juce::Point<float> origin, lastPointer, pendingShift;
        std::int64_t anchorTick = 0, anchorData = 0, timeDelta = 0;
        double horizontalTravel = 0, valueDelta = 0;
        Axis shiftAxis = Axis::Undecided;
        float height = 1;
        std::uint64_t generation = 0;
        bool verticalOnly = false, movesSelection = false, cancelled = false;
        std::optional<std::pair<std::int64_t, std::int64_t>> spliceBounds;
    };
    bool beginSingleAutomationDrag(const AutomationPick &, const juce::MouseEvent &);
    bool beginSelectedAutomationDrag(const AutomationPick &, const juce::MouseEvent &);
    void updateOverwrittenAutomationPoints(AutomationDrag &, const engine::Snapshot &);
    struct AutomationSegmentHit {
        LaneBody body;
        AutomationPick left, right;
        double leftValue = 0.0, rightValue = 0.0;
        std::uint8_t curve = 0;
        float tension = 0.f, distance = 0.f;
        std::size_t rightIndex = 0;
        bool held = false, diagonal = false;
    };
    std::optional<AutomationSegmentHit> automationSegmentAt(juce::Point<float>, float maxDistance) const;
    struct AutomationCurvePreview {
        std::uint64_t point;
        std::uint8_t curve;
        float tension;
    };
    std::optional<AutomationCurvePreview> automationCurvePreview(const LaneBody &, const TrackEnvelope &) const;
    bool beginAutomationSegmentDrag(const juce::MouseEvent &);
    void updateAutomationDrag(const juce::MouseEvent &);
    bool commitAutomationDrag(const AutomationDrag &);
    std::optional<AutomationDrag> automationDrag_;
    struct AutomationCurveDrag {
        AutomationPick left;
        std::uint64_t right = 0, generation = 0;
        float originY = 0, initialTension = 0, tension = 0, direction = 1;
        bool cancelled = false;
    };
    std::optional<AutomationCurveDrag> automationCurveDrag_;
    void commitAutomationCurve(const AutomationPick &left, std::uint64_t right, std::uint8_t shape, float tension);
    std::optional<AutomationPick> pointDrag_, chosenPoint_;
    enum class AutomationPointClick { None, Delete, Select };
    AutomationPointClick automationPointClick_ = AutomationPointClick::None;
    bool suppressAutomationDoubleClick_ = false, ignoringAutomationClick_ = false;
    void finishAutomationPointClick(const AutomationPick &);
    std::int64_t pointTick_ = 0;
    bool automationAddDragIsCurrent() const;
    void updateAutomationAddDrag(const juce::MouseEvent &);
    std::optional<AutomationAddDrag> automationAddDrag_;
    double pointValue_ = 0;
    std::int64_t fadeTicks_ = 0, hoverClip_ = 0;
    double fadeTension_ = 0, fadeTensionFrom_ = 0;
    int fadeFromY_ = 0;
    // What a screen reader hears after a key moves the selection.
    void announce(const juce::String &);
    // Live's left and right arrows (online §41.4.1.3): by the grid, or to a clip's edge with
    // Ctrl (Option on the Mac); with Shift they select.
    bool arrow(bool forward, const juce::ModifierKeys &);
    // The nearest clip edge on the selected track after `tick`, or before it; `tick` if none.
    std::int64_t clipEdge(std::int64_t tick, bool forward) const;
    enum class ResizeEdge { None, Left, Right };
    void beginResize(const engine::ClipNode &, std::int64_t track, ResizeEdge, bool stretch = false);
    void resizeTo(std::int64_t tick, std::int64_t minLength);
    std::optional<std::int64_t> commitResize();
    // Shift+click: the selection from its anchor to the point clicked, across tracks too (§6.9).
    void extendTo(int x, int y, bool snapToGrid);
    // Delete or 0 on a time selection, on every selected track, as one undo step.
    bool rangeEdit(bool deactivate);
    bool moveTimeSelection(std::int64_t target);
    struct RangeDrag {
        Selection from;
        std::int64_t to;
        bool cancelled = false;
    };
    std::optional<RangeDrag> rangeDrag_;
    std::int64_t moving_ = 0, movingFrom_ = 0, movingTo_ = 0, grabTicks_ = 0, movingSource_ = 0, movingTrack_ = 0;
    std::int64_t resizing_ = 0, resizeTrack_ = 0, resizeStart_ = 0, resizeFrom_ = 0, resizeLength_ = 0;
    std::optional<std::int64_t> resizeLimit_;
    ResizeEdge resizeEdge_ = ResizeEdge::None;
    bool keyResize_ = false, stretching_ = false;
    std::optional<std::pair<std::int64_t, std::int64_t>> clipTimeBounds_;
    // A plain press on the bar of one of several selected clips keeps them for the drag; released
    // without one, it selects that clip alone, from where it was pressed.
    bool selectOnUp_ = false;
    juce::Point<int> pressedAt_;
    // Ctrl/Cmd+press on a picked clip's bar: let go on release only if no drag came of it.
    std::int64_t toggleOnUp_ = 0;
    // The drag holds Ctrl (Option on the Mac): released, it copies the clips instead of moving them.
    bool copying_ = false;
    bool contextMenuPress_ = false;
    bool emptyTrackPress_ = false, emptyTrackClick_ = false;
    bool emptyTrackDoubleClicked_ = false;
    bool inEmptyTrackArea(juce::Point<int>) const;
    void addTrackInEmptyArea(juce::Point<int>);
    // The offset the drag gives every selected clip: the dragged clip's, stopped where the earliest
    // of them reaches the arrangement's start.
    std::int64_t movingDelta() const noexcept { return movingTo_ - movingFrom_; }
    // While several selected clips are dragged across tracks, the ones that change track, each
    // with the track it lands on (`clipsAcross`); the track a clip is drawn on while dragging.
    std::vector<std::pair<std::int64_t, std::int64_t>> movingAcross_;
    std::int64_t movingTargetOf(std::int64_t clip) const noexcept;
    // The clip's bar under a point and the edge closest to the pointer, if within reach.
    const engine::ClipNode *barAt(int x, int y, std::int64_t &track, ResizeEdge &edge) const;
    // A group's lane, row `at`: Live's overview of the clips of what it holds.
    void paintGroupLane(juce::Graphics &, std::size_t at, int y, int h, juce::Colour) const;
    // An audio clip's waveform in its body, from `left` to `right` and `top` to `bottom`, the clip
    // drawn at `pos`.
    void paintWaveform(juce::Graphics &, const engine::ClipNode &, std::int64_t pos, float left, float right, float top,
                       float bottom) const;
    // Draw the snapshot's note bars over the body, spreading the keys used by
    // the clip over its height (Live 12 manual 6.1).
    void paintNotes(juce::Graphics &, const engine::ClipNode &, std::int64_t pos, std::int64_t clipEnd,
                    float left, float right, float top, float bottom) const;
    void paintLiveMidiTake(juce::Graphics &, const LiveMidiTake &, const engine::TrackNode &,
                           std::size_t trackNumber, int y, int height) const;
    std::vector<std::unique_ptr<ClipAccessible>> accessible_;
    ArrangementView &view;
    juce::Point<float> last;
    bool panning = false;
};
// Bitwig's project view (Master Reference 10, Figure 32; the director, 28 October 2026), drawn as
// the Arrangement sheet's "Show" chooser over the track names: "Show", then the view chosen ("All
// groups", or the group's name, in the accent while a group is chosen, so a view that leaves tracks
// out always says so), and the arrow of its menu.
class ProjectViewButton final : public juce::Button {
  public:
    ProjectViewButton();
    void paintButton(juce::Graphics &, bool over, bool down) override;
    static constexpr const char *kAllGroups = "All groups";
};
// A button of the bar over the track names (the Arrangement sheet; language.png part B): a box of
// its own, with its label or its glyph; or a cell of a group, lit in the accent while on.
class BarButton final : public juce::Button {
  public:
    using Glyph = std::function<void(juce::Graphics &, juce::Rectangle<float>, juce::Colour)>;
    BarButton(const juce::String &name, const juce::String &tooltip, bool cell, Glyph = {});
    void paintButton(juce::Graphics &, bool over, bool down) override;
    const bool cell;

  private:
    Glyph glyph_;
};
// The bar over the track names and their controls, level with the beat ruler (the Arrangement
// sheet, arrangement.png; ADR-0244 d8; DF-W25), left to right: Flip Sides (the track names to the
// window's other side, ADR-0080); Show, the project view; the track row's sections, In/Out, Sends,
// Returns, Mixer and Track Delay, in one group; Live's Optimize Arrangement Height and Width (H, W)
// in another; Automation Mode; Lock Envelopes.
class NamesBar final : public juce::Component {
  public:
    NamesBar();
    void paint(juce::Graphics &) override;
    void resized() override;
    BarButton flip;
    ProjectViewButton show;
    BarButton inOut, sends, returns, mixer, delay;
    BarButton height, width;
    BarButton automation, lock;
    // Show's least width: "Show" and a few letters of the view.
    static constexpr int kShowMin = 80;

  private:
    // The two groups' boxes, drawn behind their cells.
    juce::Rectangle<float> sections_, sizes_;
};
// One pixel wide, above the canvas, transparent to the mouse (ADR-0050 d2): moving
// it repaints two thin strips, never the arrangement.
class ArrangementPlayhead final : public juce::Component {
  public:
    ArrangementPlayhead() { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics &g) override { g.fillAll(look::dropTarget); }
};
// The Overview is the root's child, not the view's: Live draws it across the track
// headers as well, up to the H and W buttons.
class ArrangementView final : public juce::Component {
  public:
    explicit ArrangementView(AdiRootComponent &);
    void resized() override;
    void paint(juce::Graphics &) override;
    // Under the bar over the track names, Live's Set Locator, Previous and Next Locator buttons (Live 12
    // manual 6.4): where paint draws each, and where a click finds it.
    enum class TopButton { Set, Previous, Next };
    juce::Rectangle<float> topButton(TopButton) const noexcept;
    void mouseDown(const juce::MouseEvent &) override;
    // The colour of the track at `position` down the rows: its own, an index into Live's palette
    // (ADR-0211), else the colour of its place, as Live's auto-assigned colours.
    juce::Colour colourFor(std::size_t position) const;
    void frame(bool changed) noexcept;
    // Arrangement Record opens the preview. The writer's captured extent and peaks replace the
    // initial bounds, and the preview stays visible while the take is being added to the project.
    struct LiveTake {
        std::int64_t start = 0;
        std::vector<std::int64_t> tracks;
        std::shared_ptr<const engine::AudioRecordingPreview> preview;
        std::uint64_t capturedFrames = 0;
        bool landing = false;
    };
    void beginLiveTake(std::int64_t start, std::vector<std::int64_t> tracks);
    void setRecordingPreview(std::shared_ptr<const engine::AudioRecordingPreview>, std::uint64_t capturedFrames,
                             bool landing) noexcept;
    const std::optional<LiveTake> &liveTake() const noexcept { return liveTake_; }
    void dragTrackHeight(std::int64_t track, int height, bool all);
    void changed();
    bool command(int);
    void fit(bool selection);
    void wheel(const juce::MouseEvent &, const juce::MouseWheelDetails &);
    // Live scrolls the lanes while a track, a clip or a selection is dragged to their edge: the mouse
    // at `y` within kScrollEdge of the lanes' top, or above it, scrolls up; near the foot, or the
    // lanes' bottom, down; with `sideways`, at `x` near the left or right edge, back or on. The
    // farther in, the faster. Points are the lanes' (the canvas's and the names'). True when the
    // view moved; a held mouse repeats the drag (beginDragAutoRepeat), so it keeps going.
    bool edgeScroll(int x, int y, bool sideways, bool upAndDown = true);
    static int edgeScrollStep(int position, int length) noexcept;
    static constexpr int kScrollEdge = 16, kAutoRepeatMs = 40;
    enum class SelectionPurpose { Edit, InsertMarker, ContextMenu };
    // Selects the clip or point. A context menu keeps an existing selection under the pointer.
    void select(int x, int y, bool snapToGrid = true, SelectionPurpose purpose = SelectionPurpose::Edit);
    // Whether a gesture with these keys snaps to the grid.
    bool snaps(const juce::ModifierKeys &) const noexcept;
    // Select one clip by identity: the accessible children's press action.
    void selectClip(std::int64_t track, std::int64_t clip);
    void dropMovedDevice(const juce::DragAndDropTarget::SourceDetails &);
    // Live's Select All (Ctrl/Cmd+A, manual §42.5): all the material, a time selection from the
    // earliest clip's start to the latest one's end across every track that holds clips; one clip
    // alone is selected as that clip. False, saying so, when there is nothing to select.
    bool selectAll();
    // Explicit positioning reveals the insert marker and locates the transport.
    void placeInsertMarker(std::int64_t tick, bool fromInsertMarker = false);
    // Update the selection and viewport for a command that already sends its own transport seek.
    void showInsertMarkerAt(std::int64_t tick);
    void locate();
    // Live's right-click menu for the track area and the headers.
    // The lanes' or a header's menu at `where`. `lane`: in Automation Mode, the envelope under the click, whose
    // Re-Enable Automation the lanes' menu ends with (AU-04).
    void contextMenu(juce::Point<int> where, bool header, const engine::AutomationLaneNode *lane = nullptr,
                     const std::function<void(juce::PopupMenu &)> &configure = {});
    void presentContextMenu(juce::Point<int> where, bool header, const engine::AutomationLaneNode *lane,
                            juce::PopupMenu menu);
    juce::PopupMenu contextMenuItems(bool header, const engine::AutomationLaneNode *lane = nullptr);
    // Headless tests receive the menu and its hit context without opening a native popup.
    std::function<void(juce::Point<int>,bool,const engine::AutomationLaneNode *,const juce::PopupMenu &)> onContextMenu;
    // In Automation Mode, the Volume lane of the track at `y` that the engine plays, else null; a pointer into
    // the root's current snapshot, for use at once.
    const engine::AutomationLaneNode *automationLaneAt(int y) const;
    // A track header's menu: Insert Audio and MIDI Track; with a track selected, Rename, Delete, Group
    // and Ungroup, then Live's clip colour assignment and colour chooser (manual 8.1.1.3, 18.3).
    juce::PopupMenu headerMenu(std::int64_t contextTrack = 0);
    bool toggleTakeLanes(std::int64_t track);
    bool insertTakeLane(std::int64_t track);
    bool toggleTakeAudition(std::int64_t track, std::int64_t lane);
    bool takeAuditioned(std::int64_t track, std::int64_t lane) const noexcept;
    struct TakeSelection {
        std::int64_t track, lane, start, end;
    };
    std::optional<TakeSelection> takeSelection;
    bool copyTakeSelectionToMain();

    // The colour chooser's pick: the colour at `index` of Live's palette on every selected track, one
    // undo step ("Recolour Track").
    bool recolour(std::int64_t index);
    // Live's Assign Track Color to Grouped Tracks and Clips: the selected group's colour, its own or
    // the one its place gives it, on every track and clip it holds in one undo step.
    bool assignGroupColour(std::int64_t group = 0);
    // The colour index a track shows: its own, or the one its place in the arrangement gives it.
    std::int64_t colourIndex(std::int64_t track) const;
    // The lanes' menu's grid (§6.10): the adaptive and fixed widths by name, the one in use
    // ticked, then Triplet Grid and Snap to Grid.
    void addGridItems(juce::PopupMenu &);
    // Live's Follow (§6.2): brings the playhead at `tick` on screen, by page (it jumps to the
    // left edge once it leaves the lanes) or by scrolling (it stays in the middle). True when
    // the view moved.
    bool follow(std::int64_t tick, bool page) noexcept;
    // Paused by an edit, a sideways scroll or a click on the ruler; a click in the lanes or
    // the scrub area, or playback starting or stopping, starts it again.
    bool followPaused = false;
    // A track to scroll into sight once the next frame's rows hold it (a newly inserted one), or 0.
    std::int64_t revealTrack = 0;
    // The selected time signature marker's position (Live 12 manual 6.5), or -1. A click in the
    // lanes or on a track's title lets it go; Delete and Ctrl/Cmd+R act on it while it is selected.
    std::int64_t selectedMeter = -1;
    // The loop brace, selected by a press on it (Live 12 manual 6.6): the arrows act on it while it
    // is (§42.8), and a click in the lanes, on a title or on a marker lets it go.
    bool loopSelected = false;
    // The selected locator's marker id (Live 12 manual 6.4), or 0: Set reads Del, Delete and Ctrl/Cmd+R
    // act on it and the arrows move it while it is selected; a click in the lanes or on a title lets
    // it go.
    std::int64_t selectedLocator = 0;
    // Live's Select Loop (manual 6.6, §42.8; a click on the brace): a time selection over the loop
    // across every track shown.
    bool selectLoop();
    const SnapshotReader &reader() const;
    std::map<std::int64_t, std::int64_t> auditionedTakes;

    // The audio clips' waveforms (N40): the document's cache, which the app and the tests give it;
    // without one, an audio clip is drawn as its colour and its name.
    WaveformCache *waveforms = nullptr;
    std::uint64_t waveformsSeen = 0;
    float waveformZoomScale() const noexcept;
    // Live 11's measures at 100 % (2026-10-27): the Overview with its margin, the beat
    // ruler with its numbers along its foot, the locator band, a dark gap, then the
    // lanes; the time ruler under them; a margin left of the time; the track header's
    // name column with the dark rule before it.
    static constexpr int kOverviewHeight = 20, kRulerHeight = 29, kScrubHeight = 24;
    static constexpr int kHeaderWidth = 95, kTimeRulerHeight = 18, kLaneGap = 7, kLeftMargin = 4;
    // One press of Alt/Option + or - on a track's height, in units (PROVISIONAL: Live's step
    // is to be measured).
    static constexpr int kHeightStep = 16;
    // The Overview's row and where the lanes begin in this window: kOverviewHeight and
    // kTopHeight with the Overview shown, without its row when it is hidden (§42.1).
    int overviewHeight() const noexcept;
    int topHeight() const noexcept { return kTopHeight - kOverviewHeight + overviewHeight(); }
    // Where the lanes begin: below the Overview, the ruler and the scrub area.
    static constexpr int kTopHeight = kOverviewHeight + kRulerHeight + kScrubHeight + kLaneGap;
    // The track names' column for a span of names and lanes: Live's width, at most a third.
    static int headerWidth(int span) noexcept { return std::min(kHeaderWidth, span / 3); }
    // With the track controls on the left (the swap, ADR-0080), the names lead: names, a
    // gap of `controls` units the root fills with the track controls, then the lanes.
    void setNamesFirst(bool first, int controls);
    bool namesFirst() const noexcept { return namesFirst_; }
    ArrangementGeometry geometry;
    AdiRootComponent &root;
    ArrangementOverview overview;
    TimelineRuler ruler;
    ScrubArea scrub;
    TrackHeaderList headers;
    ArrangementCanvas canvas;
    ArrangementPlayhead playhead;
    std::uint64_t canvasPaints = 0;
    // Bitwig's project view: its menu of the whole project and its groups (the bar's Show opens it),
    // and the view chosen there, which the lanes, the track controls and the Mixer View all follow.
    // What fills in the project's tracks as the view sees them, in place, for each new snapshot
    // and when the menu opens; applied at once. The snapshot's rows by default (rowsFromSnapshot);
    // tests give it a tree.
    using TrackRows = std::function<void(const SnapshotReader &, std::vector<TrackRow> &)>;
    void setTrackRows(TrackRows);
    const TrackTree &tree() const noexcept { return tree_; }
    // For a new snapshot: the tree and the rows' order again, then what the chosen view and the
    // folded groups leave out; a group that is gone gives way to the whole project. A new
    // generation with the same rows allocates nothing.
    void applyView();
    // Folding a group hides its children. Folding an ordinary track collapses its row.
    void toggleFold(std::int64_t track);
    // Alt+U (§42.16): every track unfolded.
    void unfoldAll();
    // Where a row's name column begins: past a line in its group's colour for each group it sits
    // in, as Live's; the Arrangement sheet's lines, 4 wide at a step of 5, the row itself as wide as
    // its group's (DF-W25).
    static constexpr int kGroupBand = 5, kGroupLine = 4;
    // Shows `group`, everything inside it and Main; 0 for the whole project. Kept per window; a
    // selected track left out gives the selection to the first track shown.
    void chooseView(std::int64_t group);
    // The button's menu: the project, then its groups as a tree, as Bitwig's, then the ★ Reference
    // track's toggle.
    juce::PopupMenu projectViewMenu();
    // A selected track left out gives the selection to the first track shown.
    void firstShownIfHidden();

  private:
    bool assignColourToClips(std::int64_t track, bool grouped);
    juce::Slider waveformZoom_;
    std::optional<LiveTake> liveTake_;
    bool hadLiveMidiTake_ = false;
    void refreshTree();
    TrackRows trackRows_;
    // What the chosen view and the folded groups leave out, from the tree as it is.
    void leaveOut();
    TrackTree tree_;
    // The menu's own look, Bitwig's; a menu keeps only a weak reference to it.
    std::unique_ptr<juce::LookAndFeel_V4> menuLook_;
    bool namesFirst_ = false;
    int controlsGap_ = 0;
};
} // namespace adi::ui
