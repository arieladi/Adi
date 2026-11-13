// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/shared_analysis.hpp"
#include "adi/ui/analysis_palettes.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <limits>
#include <optional>
#include <vector>

namespace adi::engine { class MainLoudness; }
namespace adi::ui {
// Presentation of the existing persistent Main service, not another meter.
// Values have independent measurement intervals; this is not a sample-synchronous
// publication. The owner callback is read only and owns no audio/session pointer.
struct MainMeterSnapshot {
    std::int64_t track = 0;
    double sampleRate = 0;
    std::uint64_t formatSequence = 0;
    float momentaryLufs = -std::numeric_limits<float>::infinity();
    float shortTermLufs = -std::numeric_limits<float>::infinity();
    float integratedLufs = -std::numeric_limits<float>::infinity();
    float truePeakDb = -std::numeric_limits<float>::infinity();
    bool operator==(const MainMeterSnapshot &) const = default;
};
struct MainMeterSource {
    std::int64_t track = 0;
    std::function<std::optional<MainMeterSnapshot>()> read;
};
// Owner-thread projection of one existing service; validates format adoption
// around independent value reads without requesting audio work or a reset.
std::optional<MainMeterSnapshot> readMainMeterSnapshot(const engine::MainLoudness &,
                                                     std::int64_t track, double sampleRate) noexcept;
// Immutable display lookup; sample is the existing quantized 0..255 intensity.
juce::Colour analysisPaletteColour(std::size_t palette, std::uint8_t sample, bool reverse = false) noexcept;
// The same calibrated publication in the boxed Info area and the full Mix View. This component
// owns no timer, FFT or worker. Its owner calls frame on the window's clock. Ancestor visibility
// listeners release demand even when the hidden window's clock has stopped.
class SharedSpectrumView final : public juce::Component,
                                 public juce::SettableTooltipClient,
                                 private juce::ComponentListener {
  public:
    SharedSpectrumView();
    ~SharedSpectrumView() override;
    void present(std::shared_ptr<SharedAnalysis>, std::int64_t track);
    enum class Mode { Spectrum, Scope, Meters, Phase, Spectrogram };
    void setMode(Mode);
    Mode mode() const noexcept { return mode_; }
    void setSource(engine::ScopePoint, std::int64_t device = 0);
    engine::ScopePoint point() const noexcept { return point_; }
    void setChannel(SharedAnalysis::Channel);
    SharedAnalysis::Channel channel() const noexcept { return channel_; }
    void setPalette(std::string_view id, bool reverse = false);
    std::string_view palette() const noexcept { return palettes::id(paletteIndex_); }
    bool paletteReversed() const noexcept { return paletteReversed_; }
    // Weak owner token: closing/replacing the document revokes future reads.
    // Main-only eligibility is checked before invoking the callback.
    void setMainMeterSource(const std::shared_ptr<const MainMeterSource> &);
    std::optional<MainMeterSnapshot> mainMeterSnapshot() const noexcept { return mainMeter_; }
    juce::String mainMeterText() const;
    void freeze(bool);
    void clear();
    bool frozen() const noexcept { return frozen_; }
    void frame() noexcept;
    // Native minimisation can stop VBlank without changing Component visibility. The owning
    // window calls this in its minimisation callback, then clears it on restore.
    void suspend(bool) noexcept;
    void paint(juce::Graphics &) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    void resized() override;
    void moved() override;
    enum class Status { Detached, Unavailable, Waiting, Ready };
    Status status() const noexcept { return status_; }
    bool subscribed() const noexcept { return static_cast<bool>(lease_); }
    std::shared_ptr<const SharedAnalysis::Result> snapshot() const noexcept { return shown_; }
    std::int64_t track() const noexcept { return track_; }
  private:
    bool eligible() const noexcept;
    void updateDemand() noexcept;
    void updateMainMeter() noexcept;
    void watchAncestors();
    void unwatchAncestors() noexcept;
    void componentVisibilityChanged(juce::Component &) override;
    void componentParentHierarchyChanged(juce::Component &) override;
    void componentMovedOrResized(juce::Component &, bool, bool) override;
    void componentBeingDeleted(juce::Component &) override;
    struct Raster;
    void updateRaster();
    std::shared_ptr<SharedAnalysis> broker_;
    std::int64_t track_ = 0;
    SharedAnalysis::Lease lease_;
    std::shared_ptr<const SharedAnalysis::Result> shown_;
    std::weak_ptr<const MainMeterSource> mainMeterSource_;
    std::optional<MainMeterSnapshot> mainMeter_;
    std::vector<juce::Component::SafePointer<juce::Component>> ancestors_;
    Status status_ = Status::Detached;
    bool suspended_ = false;
    bool frozen_ = false;
    Mode mode_ = Mode::Spectrum;
    engine::ScopePoint point_ = engine::ScopePoint::PostFader;
    std::int64_t device_ = 0;
    SharedAnalysis::Channel channel_ = SharedAnalysis::Channel::Mid;
    std::unique_ptr<Raster> raster_;
    std::size_t paletteIndex_ = palettes::defaultIndex;
    bool paletteReversed_ = false;
    std::uint64_t clearedRevision_ = 0, clearedEpoch_ = 0, clearedColumn_ = 0;
};

// A full analyzer and the compact Info renderer use SharedSpectrumView above. Controls are
// view state, never audio parameters. Main is explicit default; Selected follows channel
// selection rather than retaining a stale track after a project/selection replacement.
class AnalysisPane final : public juce::Component {
  public:
    enum class Source { Main, Selected };
    AnalysisPane();
    // A detached pane forwards app commands through its owning main window.
    // Guard the focused pane's ancestry, independently of that window's tree.
    static bool anyControlsFocused() noexcept;
    void frame(std::shared_ptr<SharedAnalysis>, std::int64_t main, std::int64_t selected);
    void resized() override;
    void setMode(SharedSpectrumView::Mode);
    SharedSpectrumView::Mode mode() const noexcept { return view.mode(); }
    void setSource(Source);
    Source source() const noexcept { return source_; }
    void setPoint(engine::ScopePoint);
    engine::ScopePoint point() const noexcept { return view.point(); }
    void suspend(bool value) { view.suspend(value); }
    void setPalette(std::string_view id, bool reverse = false);
    void restorePaletteControls(std::string_view id, bool reverse);
    std::string_view requestedPalette() const noexcept;
    std::function<void()> onPaletteChange;
    std::function<void(std::int64_t)> onExpand;
    SharedSpectrumView view;
    juce::ComboBox modeBox, sourceBox, pointBox, channelBox, paletteBox;
    juce::ToggleButton reverseButton{"Reverse"};
    juce::TextButton freezeButton{"Freeze"}, resetButton{"Reset"}, expandButton{"Window"};
  private:
    Source source_ = Source::Main;
};

// Optional compact projection of the same broker: identical source/configuration
// lets these output modes share one domain, cursor and spectral transform.
class AnalysisDock final : public juce::Component {
  public:
    AnalysisDock();
    void restoreControls(int source, int point, int channel, int width);
    void configure(int source, int point, int channel, int width);
    int source() const noexcept { return sourceBox.getSelectedId() - 1; }
    int point() const noexcept { return pointBox.getSelectedId() - 1; }
    int channel() const noexcept { return channelBox.getSelectedId() - 1; }
    int requestedWidth() const noexcept { return widthBox.getSelectedId() > 0 ? widthBox.getSelectedId() : width_; }
    void frame(std::shared_ptr<SharedAnalysis>, std::int64_t main, std::int64_t selected);
    void suspend(bool) noexcept;
    void resized() override;
    void paint(juce::Graphics &) override;
    std::function<void()> onChange;
    std::function<void(std::int64_t)> onExpand;
    SharedSpectrumView meters, phase, spectrum;
    juce::ComboBox sourceBox, pointBox, channelBox, widthBox;
    juce::TextButton freezeButton{"Freeze"}, resetButton{"Reset"}, expandButton{"Window"};
    static constexpr int kControlsHeight = 75;
  private:
    void changed();
    int width_ = 300;
};
} // namespace adi::ui
