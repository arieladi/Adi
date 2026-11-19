// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/mix_view_provider.hpp"
#include "adi/ui/density.hpp"
#include "adi/ui/theme.hpp"
#include "ui_stack.hpp"
#include "ui_analysis.hpp"
#include "ui_mix_channels.hpp"
#include "ui_mix_meter.hpp"
#include "ui_mix_analysers.hpp"
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
// The overview uses the same clips, signatures and transport commands as the Arrangement.
class MixSongStrip final : public juce::Component {
  public:
    explicit MixSongStrip(AdiRootComponent&);
    void frame(const SnapshotReader&, std::int64_t tick);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void cancelSeek() noexcept { seeking_ = false; }
    std::int64_t tickAt(float x) const noexcept;
  private:
    void drawSong(juce::Graphics&, const SnapshotReader&);
    int headX(std::int64_t tick) const noexcept;
    void moveHead(int);
    AdiRootComponent& root_;
    juce::Image map_;
    const void* snapshot_ = nullptr;
    ThemeId theme_ = ThemeId::Dark;
    DensityId density_ = DensityId::Compact;
    std::uint64_t generation_{};
    std::int64_t span_{1}, previewTick_{};
    int head_{-1};
    bool seeking_{};
};

// Docked Mix View occupies the workspace between the Control Bar and Status Bar.
// Which tab is shown is this window's view state (`WindowState::mixViewTab`, kept in `ui_view`),
// never the panel's own.
class MixViewPanel final : public juce::Component {
  public:
    enum class Tab { Overview, Stack, ScopePhase, Spectrogram, Cascade, Analysis };
    static constexpr int kTabCount = 6;
    static constexpr std::array<const char *, kTabCount> kTabNames{"Overview", "Stack", "Scope & Phase",
                                                                   "Spectrogram", "", "Analysers"};
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
    MixSongStrip song;
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
    MixChannelBrowser channels;
    MixOutputMeter mainMeter;
    std::weak_ptr<const MainMeterSource> mainMeterSource;
    SharedSpectrumView spectrum;
    MixAnalysers analysis;
    AnalysisDock analyzerDock;
    juce::ToggleButton analyzerToggle{"Analyzer dock"};
    bool setAnalyzerDock(bool);
    bool setAnalyzerWidth(int);
    void adoptViewState(); // owner constructor body, after WindowState exists
    bool hasAnalyzerControlFocus() const;
    bool hasChannelControlFocus() const { return channels.isVisible() && channels.hasKeyboardFocus(true); }
    bool focusChannelSearch();
    bool analyzerDockShown() const noexcept { return analyzerDock.isVisible(); }
    static constexpr int kStackMinimumWidth = 320, kDockMinimumWidth = 240, kDockGap = 6;
    static constexpr int kDockMinimumHeight = 420, kHeaderSingleRowWidth = 632;
    void suspendAnalysis(bool value) {
        spectrum.suspend(value);
        analysis.suspend(value);
        analyzerDock.suspend(value);
        channels.preview.suspend(value);
        mainMeter.suspend(value);
    }

  private:
    AdiRootComponent &root_;
    int shownTab_ = -1;
    int headerHeight_ = kHeaderHeight;
    bool layoutPending_ = false, appliedDock_ = false, ownerReady_ = false;
    int appliedWidth_ = 300;
    juce::Rectangle<int> appliedBody_, appliedStrip_;
    bool persistAnalyzerSettings();
};
} // namespace adi::ui
