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
};
} // namespace adi::ui
