// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/mix_view_provider.hpp"
#include "adi/ui/parameter_feed.hpp"
#include "adi/ui/stack_model.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <optional>
#include <vector>
namespace adi::ui {
class AdiRootComponent;
class StackTab;
// A realised row of StackModel. Geometry and target identity belong to the model;
// the row owns only the ordinary device panels currently realised inside it.
class StackRow final : public juce::Component, public juce::SettableTooltipClient {
  public:
    StackRow(StackTab &, std::int64_t track);
    ~StackRow() override;
    void paint(juce::Graphics &) override;
    void mouseEnter(const juce::MouseEvent &) override;
    void mouseExit(const juce::MouseEvent &) override;
    void mouseDown(const juce::MouseEvent &) override;
    std::int64_t track() const noexcept { return track_; }
    bool zoomed() const noexcept;
    // Compatibility queries for the legacy off drawings; empty in the actual-chain adapter.
    juce::Rectangle<int> eqArea() const noexcept;
    juce::Rectangle<int> sidechainArea() const noexcept;
    juce::Rectangle<int> limiterArea() const noexcept;
    const char *mode() const noexcept;
    static constexpr int kHeight = 100, kZoomedHeight = 190, kNameWidth = 128, kDrawerWidth = 128;

  private:
    friend class StackTab;
    struct State;
    void present(const stack::RowGeo &);
    StackTab &stack_;
    std::int64_t track_;
    std::unique_ptr<State> state_;
};
// The visible STACK surface is a projection of the pure StackModel. It reads the
// same parameter publication and uses the same gesture/undo authority as Device View.
class StackTab final : public juce::Component {
  public:
    explicit StackTab(AdiRootComponent &);
    ~StackTab() override;
    void suspendParameters(bool);
    void frame(const SnapshotReader &);
    // Deterministic component-test entry; normal frames use the window's monotonic clock.
    void frameAt(const SnapshotReader &, stack::TimeMs);
    void paint(juce::Graphics &) override;
    void resized() override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    void mouseMove(const juce::MouseEvent &) override;
    void mouseEnter(const juce::MouseEvent &) override;
    void mouseExit(const juce::MouseEvent &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void focusGained(FocusChangeType) override;
    void focusLost(FocusChangeType) override;
    void focusOfChildComponentChanged(FocusChangeType) override;
    void focus(std::int64_t track);
    std::int64_t focused() const noexcept;
    void hover(std::int64_t track);
    std::int64_t hovered() const noexcept;
    const std::vector<std::unique_ptr<StackRow>> &rows() const noexcept { return rows_; }
    juce::Rectangle<int> display() const noexcept;
    juce::Rectangle<int> listArea() const noexcept;
    const MixViewProvider &provider() const noexcept;
    juce::String scope() const;
    StackModel &model() noexcept;
    const StackModel &model() const noexcept;
    bool hasDeviceContext() const;
    bool hasToolbarFocus() const;
    bool deviceCommand(int command);
    // Ends a held edit exactly once before hiding/rebinding/removing its component.
    void cancelInteractions();
    AdiRootComponent &root;
    juce::TextEditor search;
    juce::ComboBox channelScope;
    juce::String emptyMessage() const;
    juce::Slider rowSize;
    juce::ToggleButton autoZoom{"Auto Zoom"}, pinsOnly{"Pinned"}, navigator{"Navigator"};
    static constexpr int kDisplayHeight = 56, kHeaderHeight = 30;

  private:
    friend class StackRow;
    struct State;
    std::unique_ptr<State> state_;
    std::vector<std::unique_ptr<StackRow>> rows_;
    void effects(const stack::Effects &, bool takeFocus = true);
    void layoutRows();
    void updateParameterDemand(std::vector<std::int64_t>);
    void refreshRows(const SnapshotReader &, stack::TimeMs);
    void syncScroll();
    int toolbarHeight() const noexcept;
    void focusTile(const stack::ItemRef &);
    stack::Hit presentedHit(double x, double y) const;
    void beginRename(stack::RowRef);
    void endRename(bool commit);
    bool beginInteraction(const stack::ItemRef &, stack::CaptureKind);
    void endInteraction();
    bool editParameter(const stack::ItemRef &, const std::string &, engine::ParamEventKind, double);
    bool validTarget(const stack::ItemRef &) const;
    const DevicePanelData *device(std::int64_t) const noexcept;
    juce::Point<float> modelPoint(const juce::MouseEvent &) const;
    bool fromList(const juce::MouseEvent &) const;
};
} // namespace adi::ui
