// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/snapshot_reader.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <vector>
namespace adi::ui {
class AdiRootComponent;
class ChannelEqView;
// One channel in the window: its colour and name, its ADI-Q and its ADI-L, each present
// and off until switched on (ADR-0204 d1).
class ChannelEqRow final : public juce::Component, public juce::SettableTooltipClient {
  public:
    ChannelEqRow(ChannelEqView &, std::int64_t track);
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    std::int64_t track() const noexcept { return track_; }
    juce::TextButton eq{"Off"}, limiter{"Off"};
    static constexpr int kHeight = 21;

  private:
    ChannelEqView &view_;
    std::int64_t track_;
};
// ADI's channel EQ window (ADR-0204 d1): every channel's ADI-Q in one view, FabFilter's
// "show all instances" made native so it spans the project, and the ADI-L limiters beside
// them so the Loudness Drive's cascade is seen in one place. The display draws each engaged
// EQ's curve in its track's colour, on EQ Eight's grid. The slots are present and off until
// the engine's channel slots and the clones lane's plug-ins arrive (window log, N16).
class ChannelEqView final : public juce::Component {
  public:
    explicit ChannelEqView(AdiRootComponent &);
    ~ChannelEqView() override;
    // The window's frame: rows follow the tracks, in the snapshot's order.
    void frame(const SnapshotReader &);
    void paint(juce::Graphics &) override;
    void resized() override;
    // Shows a channel's row selected: from the mixer's Q or L switch, or a click.
    void focus(std::int64_t track);
    std::int64_t focused() const noexcept { return focused_; }
    const std::vector<std::unique_ptr<ChannelEqRow>> &rows() const noexcept { return rows_; }
    juce::Rectangle<int> display() const noexcept;
    AdiRootComponent &root;
    static constexpr int kDisplayHeight = 170, kHeaderHeight = 22;

  private:
    std::vector<std::unique_ptr<ChannelEqRow>> rows_;
    std::uint64_t shown_ = 0; // the snapshot generation the rows were built from
    std::int64_t focused_ = 0;
    juce::Viewport scroll_;
    juce::Component list_;
};
} // namespace adi::ui
