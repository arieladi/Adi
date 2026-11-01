// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/mix_view_provider.hpp"
#include "ui_stack.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
namespace adi::ui {
class AdiRootComponent;
class SnapshotReader;
// The Mix View's own window or the centre: two overlapping boxes, lit while it is in a window.
class FloatButton final : public juce::Button {
  public:
    FloatButton() : juce::Button("Mix View in a Window") {}
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
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
    // At the band's right end: into a window of its own, onto any display, and back.
    FloatButton floating;
  private:
    // The slots the engine has not written yet come from the stub (window log, N16); declared
    // before STACK, which reads through it.
    StubMixViewProvider stub_;

  public:
    // STACK: ADR-0204 d1's one window, every channel's ADI-Q, sidechain slot and ADI-L.
    StackTab stack;

  private:
    AdiRootComponent &root_;
    int shownTab_ = -1;
};
} // namespace adi::ui
