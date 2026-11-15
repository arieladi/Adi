// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/mix_view_provider.hpp"
#include "ui_stack.hpp"
#include "ui_analysis.hpp"
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
// [REF] (ADR-0209 d9): down auditions the ★ Reference, up lets go; a tap under 200 ms latches it.
class ReferenceButton final : public juce::Button {
  public:
    explicit ReferenceButton(AdiRootComponent &r) : juce::Button("REF"), root_(r) {}
    void paintButton(juce::Graphics &, bool over, bool down) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;

  private:
    AdiRootComponent &root_;
};
// [Reference Track] (ADR-0209 §4, d11): an eye and the words, lit while the track shows.
class EyeButton final : public juce::Button {
  public:
    EyeButton() : juce::Button("Reference Track") {}
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
// The Mix View presents actual channel chains and optional native functions (ADR-0236/0237).
// F4 or the control bar's Mix button opens it, docked in the centre
// in place of the arrangement and its track controls; the browser, the strip down the edge and the
// Detail View stay. STACK and the shared analyzer views use the same document/runtime sources.
// Which tab is shown is this window's view state (`WindowState::mixViewTab`, kept in `ui_view`),
// never the panel's own.
class MixViewPanel final : public juce::Component {
  public:
    enum class Tab { Overview, Stack, ScopePhase, Spectrogram, Cascade, Analysis };
    static constexpr int kTabCount = 6;
    static constexpr std::array<const char *, kTabCount> kTabNames{"OVERVIEW", "STACK", "SCOPE & PHASE",
                                                                   "SPECTROGRAM", "CTZ CASCADE", "ANALYSIS"};
    // The tabs' widths, Live's 17 pt fields on its dark band.
    static constexpr std::array<int, kTabCount> kTabWidths{72, 52, 100, 100, 92, 72};
    static constexpr int kHeaderHeight = 25;
    // The ★ Reference strip under the tabs (ADR-0209 d11): the track against the arrangement's
    // length, [REF] and [Reference Track].
    static constexpr int kStripHeight = 30;
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
    ReferenceButton ref;
    EyeButton eye;
    // What the strip says of the ★ Reference track: its name, or that none is yet.
    juce::String referenceText() const;
    // The strip's own rectangle, and where its timeline runs.
    juce::Rectangle<int> strip() const noexcept;
    juce::Rectangle<int> timeline() const noexcept;
    // STACK follows each channel's actual authored devices and optional native functions.
    StackTab stack;
    SharedSpectrumView spectrum;
    AnalysisPane analysis;
    void suspendAnalysis(bool value) { spectrum.suspend(value); analysis.suspend(value); }

  private:
    AdiRootComponent &root_;
    int shownTab_ = -1;
};
} // namespace adi::ui
