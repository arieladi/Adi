// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/shared_analysis.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <vector>

namespace adi::ui {
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
    void watchAncestors();
    void unwatchAncestors() noexcept;
    void componentVisibilityChanged(juce::Component &) override;
    void componentParentHierarchyChanged(juce::Component &) override;
    void componentMovedOrResized(juce::Component &, bool, bool) override;
    void componentBeingDeleted(juce::Component &) override;
    std::shared_ptr<SharedAnalysis> broker_;
    std::int64_t track_ = 0;
    SharedAnalysis::Lease lease_;
    std::shared_ptr<const SharedAnalysis::Result> shown_;
    std::vector<juce::Component::SafePointer<juce::Component>> ancestors_;
    Status status_ = Status::Detached;
    bool suspended_ = false;
    bool frozen_ = false;
    Mode mode_ = Mode::Spectrum;
    engine::ScopePoint point_ = engine::ScopePoint::PostFader;
    std::int64_t device_ = 0;
    SharedAnalysis::Channel channel_ = SharedAnalysis::Channel::Mid;
    juce::Image spectrogram_;
    std::uint64_t rasterRevision_ = 0, rasterEpoch_ = 0, rasterColumn_ = 0;
    std::uint64_t clearedRevision_ = 0, clearedEpoch_ = 0, clearedColumn_ = 0;
};

// A full analyzer and the compact Info renderer use SharedSpectrumView above. Controls are
// view state, never audio parameters. Main is explicit default; Selected follows channel
// selection rather than retaining a stale track after a project/selection replacement.
class AnalysisPane final : public juce::Component {
  public:
    enum class Source { Main, Selected };
    AnalysisPane();
    void frame(std::shared_ptr<SharedAnalysis>, std::int64_t main, std::int64_t selected);
    void resized() override;
    void setMode(SharedSpectrumView::Mode);
    SharedSpectrumView::Mode mode() const noexcept { return view.mode(); }
    void setSource(Source);
    Source source() const noexcept { return source_; }
    void setPoint(engine::ScopePoint);
    engine::ScopePoint point() const noexcept { return view.point(); }
    void suspend(bool value) { view.suspend(value); }
    std::function<void(std::int64_t)> onExpand;
    SharedSpectrumView view;
    juce::ComboBox modeBox, sourceBox, pointBox, channelBox;
    juce::TextButton freezeButton{"Freeze"}, resetButton{"Reset"}, expandButton{"Window"};
  private:
    Source source_ = Source::Main;
};
} // namespace adi::ui
