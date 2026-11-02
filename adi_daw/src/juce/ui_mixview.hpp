// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ui_channel_eq.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
namespace adi::ui {
class AdiRootComponent;
class SnapshotReader;
// The Mix View (ADR-0209, ruled 1 November 2026; UI-ARCHITECTURE §12): every channel's ADI-Q, ADI-L
// and sidechain slot in one view. F4 or the control bar's Mix button opens it, docked in the centre
// in place of the arrangement and its track controls; the browser, the strip down the edge and the
// Detail View stay. Its tabs in the draft's order and names (d13). P1 builds STACK, ADR-0204 d1's
// one window (d11); the other four are drawn empty until their turn after the first-window gate.
// Which tab is shown is this window's view state (`WindowState::mixViewTab`, kept in `ui_view`),
// never the panel's own.
class MixViewPanel final : public juce::Component {
  public:
    enum class Tab { Overview, Stack, ScopePhase, Spectrogram, Cascade };
    static constexpr int kTabCount = 5;
    static constexpr std::array<const char *, kTabCount> kTabNames{"OVERVIEW", "STACK", "SCOPE & PHASE",
                                                                   "SPECTROGRAM", "CTZ CASCADE"};
    // The tabs' widths, Live's 17 pt fields on its dark band.
    static constexpr std::array<int, kTabCount> kTabWidths{72, 52, 100, 100, 92};
    static constexpr int kHeaderHeight = 25;
    explicit MixViewPanel(AdiRootComponent &);
    ~MixViewPanel() override;
    // The window's frame: the shown tab follows the view state, and STACK its rows.
    void frame(const SnapshotReader &);
    void paint(juce::Graphics &) override;
    void resized() override;
    Tab tab() const noexcept;
    // What the tabs sit above, in the panel's coordinates.
    juce::Rectangle<int> body() const noexcept;
    std::array<std::unique_ptr<juce::TextButton>, kTabCount> tabs;
    // STACK: every channel, its ADI-Q and its ADI-L, present and off until the engine's slots
    // arrive (window log, N16). For now the Channel EQ window's rows, which the STACK item
    // reshapes into Pro-Q's instance list.
    ChannelEqView stack;

  private:
    AdiRootComponent &root_;
    int shownTab_ = -1;
};
} // namespace adi::ui
