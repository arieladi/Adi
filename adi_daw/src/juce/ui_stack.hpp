// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/mix_view_provider.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <vector>
namespace adi::ui {
class AdiRootComponent;
class StackTab;
// One channel of STACK (ADR-0209 d11; FabFilter Pro-Q's instance list): its colour and name, then
// its three slots, ADI-Q, the sidechain slot and ADI-L, each drawn dimmed, a flat line, while off.
// The row under the mouse grows: ADI-Q's graph to edit, and the two other slots as drawers beside
// it.
class StackRow final : public juce::Component, public juce::SettableTooltipClient {
  public:
    StackRow(StackTab &, std::int64_t track);
    void paint(juce::Graphics &) override;
    void mouseEnter(const juce::MouseEvent &) override;
    void mouseExit(const juce::MouseEvent &) override;
    void mouseDown(const juce::MouseEvent &) override;
    std::int64_t track() const noexcept { return track_; }
    bool zoomed() const noexcept;
    // Where each slot is drawn, in the row's coordinates: ADI-Q's graph, then the two drawers.
    juce::Rectangle<int> eqArea() const noexcept;
    juce::Rectangle<int> sidechainArea() const noexcept;
    juce::Rectangle<int> limiterArea() const noexcept;
    // ADI-L's mode (ADR-0204 d1): full on Main's row, light on every other channel's.
    const char *mode() const noexcept;
    static constexpr int kHeight = 30, kZoomedHeight = 168, kNameWidth = 176, kDrawerWidth = 128;

  private:
    StackTab &stack_;
    std::int64_t track_;
};
// The Mix View's STACK tab, ADR-0204 d1's one window (ADR-0209 d11): every channel the window's
// project view shows (its group, or the whole project), in the arrangement's order, under one
// display where every engaged ADI-Q's curve is drawn in its track's colour. The slots are read
// through a MixViewProvider: the stub until the engine writes them (window log, N16).
class StackTab final : public juce::Component {
  public:
    StackTab(AdiRootComponent &, const MixViewProvider &);
    // Rows follow the shown channels; a frame that changes none of them builds nothing.
    void frame(const SnapshotReader &);
    void paint(juce::Graphics &) override;
    void resized() override;
    // A channel shown selected: from a click, or the mixer's Q or L.
    void focus(std::int64_t track);
    std::int64_t focused() const noexcept { return focused_; }
    // The row under the mouse, zoomed; 0 for none.
    void hover(std::int64_t track);
    std::int64_t hovered() const noexcept { return hovered_; }
    const std::vector<std::unique_ptr<StackRow>> &rows() const noexcept { return rows_; }
    // The shared display above the rows.
    juce::Rectangle<int> display() const noexcept;
    void setProvider(const MixViewProvider &p) noexcept { provider_ = &p; }
    const MixViewProvider &provider() const noexcept { return *provider_; }
    // What the rows show: "Project", or the name of the group the project view shows.
    juce::String scope() const;
    AdiRootComponent &root;
    static constexpr int kDisplayHeight = 120, kHeaderHeight = 22;

  private:
    void layoutRows();
    const MixViewProvider *provider_;
    std::vector<std::unique_ptr<StackRow>> rows_;
    std::vector<std::int64_t> wanted_; // the shown channels, refilled in place each frame
    std::uint64_t shown_ = 0;          // the snapshot generation the rows were built from
    std::int64_t focused_ = 0, hovered_ = 0;
    juce::Viewport scroll_;
    juce::Component list_;
};
} // namespace adi::ui
