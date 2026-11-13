// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_analysis.hpp"
#include "ui_look.hpp"
#include <algorithm>
#include <cmath>

namespace adi::ui {
namespace {
constexpr float floorDb = -96.f;
float db(float value) {
    return std::isfinite(value) && value > 0 ? std::max(floorDb, 20.f * std::log10(value)) : floorDb;
}
juce::String level(float value) {
    return std::isfinite(value) && value > 0 ? juce::String(db(value), 1) : juce::String("--");
}
float heightFor(float value, juce::Rectangle<float> area) {
    return area.getY() + area.getHeight() * std::clamp(value / floorDb, 0.f, 1.f);
}
} // namespace
SharedSpectrumView::SharedSpectrumView() {
    setTitle("Channel spectrum");
    setTooltip("The channel after its mixer. Full and boxed views share one Hann-window spectrum. "
               "A full-scale sine reads 0 dBFS; L/R peak and RMS refer to the same newest window.");
    setInterceptsMouseClicks(false, false);
}
SharedSpectrumView::~SharedSpectrumView() {
    unwatchAncestors();
    lease_.reset();
}
void SharedSpectrumView::present(std::shared_ptr<SharedAnalysis> broker, std::int64_t track) {
    if (broker_ != broker || track_ != track) {
        lease_.reset();
        shown_.reset();
        broker_ = std::move(broker);
        track_ = track;
        status_ = Status::Detached;
        repaint();
    }
    updateDemand();
}
bool SharedSpectrumView::eligible() const noexcept {
    if (suspended_ || !broker_ || track_ <= 0 || !isShowing() || getLocalBounds().isEmpty()) return false;
    if (auto *peer = getPeer(); peer != nullptr && peer->isMinimised()) return false;
    auto visible = getLocalBounds();
    for (auto *parent = getParentComponent(); parent != nullptr; parent = parent->getParentComponent()) {
        visible = visible.getIntersection(getLocalArea(parent, parent->getLocalBounds()));
        if (visible.isEmpty()) return false;
    }
    return true;
}
void SharedSpectrumView::updateDemand() noexcept {
    const auto previous = status_;
    if (!eligible()) {
        lease_.reset();
        shown_.reset();
        status_ = Status::Detached;
    } else {
        if (!lease_) {
            try { lease_ = broker_->acquire({track_, engine::ScopePoint::PostFader, 1024,
                                             SharedAnalysis::Channel::Mid, dsp::Window::Hann}); }
            catch (...) { lease_.reset(); }
        }
        if (!lease_ || !lease_.available()) {
            shown_.reset();
            status_ = Status::Unavailable;
        } else {
            status_ = shown_ ? Status::Ready : Status::Waiting;
        }
    }
    if (previous != status_) repaint();
}
void SharedSpectrumView::frame() noexcept {
    updateDemand();
    if (!lease_ || !lease_.available()) return;
    auto next = lease_.read();
    // An explicit source/transport epoch may invalidate a result without dropping the lease.
    if (next != shown_) {
        shown_ = std::move(next);
        status_ = shown_ ? Status::Ready : Status::Waiting;
        repaint();
    }
}
void SharedSpectrumView::suspend(bool value) noexcept {
    if (suspended_ != value) { suspended_ = value; updateDemand(); }
}
void SharedSpectrumView::visibilityChanged() { updateDemand(); }
void SharedSpectrumView::parentHierarchyChanged() { watchAncestors(); updateDemand(); }
void SharedSpectrumView::resized() { updateDemand(); }
void SharedSpectrumView::moved() { updateDemand(); }
void SharedSpectrumView::unwatchAncestors() noexcept {
    for (auto &parent : ancestors_)
        if (parent != nullptr) parent->removeComponentListener(this);
    ancestors_.clear();
}
void SharedSpectrumView::watchAncestors() {
    unwatchAncestors();
    for (auto *parent = getParentComponent(); parent != nullptr; parent = parent->getParentComponent()) {
        ancestors_.emplace_back(parent);
        parent->addComponentListener(this);
    }
}
void SharedSpectrumView::componentVisibilityChanged(juce::Component &) { updateDemand(); }
void SharedSpectrumView::componentParentHierarchyChanged(juce::Component &) { watchAncestors(); updateDemand(); }
void SharedSpectrumView::componentMovedOrResized(juce::Component &, bool, bool) { updateDemand(); }
void SharedSpectrumView::componentBeingDeleted(juce::Component &) {
    lease_.reset(); shown_.reset(); status_ = Status::Detached;
    unwatchAncestors();
}
void SharedSpectrumView::paint(juce::Graphics &g) {
    g.fillAll(look::display);
    const auto area = getLocalBounds().toFloat().reduced(5.f);
    if (area.isEmpty()) return;
    g.setColour(look::textOnBand);
    g.setFont(look::font(10.f, true));
    g.drawText("Spectrum / Post Mixer / dBFS", area.withHeight(14.f), juce::Justification::centredLeft, true);
    auto plot = area.withTrimmedTop(16.f).withTrimmedBottom(29.f).withTrimmedRight(18.f);
    if (plot.getHeight() <= 4 || plot.getWidth() <= 4) return;
    g.setFont(look::font(9.f));
    for (int step = 0; step <= 4; ++step) {
        const float value = static_cast<float>(step) * -24.f;
        const float y = heightFor(value, plot);
        g.setColour(look::outline);
        g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
        if (plot.getWidth() >= 250) {
            g.setColour(look::textOnBand.withAlpha(.7f));
            g.drawText(juce::String(static_cast<int>(value)), juce::Rectangle<float>(plot.getX() + 2, y + 1, 28.f, 11.f),
                       juce::Justification::centredLeft);
        }
    }
    if (!shown_) {
        g.setColour(look::textOnBand);
        const char *message = status_ == Status::Unavailable ? "Analysis source unavailable"
                            : status_ == Status::Waiting ? "Waiting for audio" : "Analysis inactive";
        g.drawFittedText(message, plot.toNearestInt().reduced(6), juce::Justification::centred, 2);
        return;
    }
    const double topHz = std::min(20000.0, shown_->sampleRate * .5);
    if (topHz <= 20) return;
    const auto xFor = [&](double hz) {
        return plot.getX() + plot.getWidth() * static_cast<float>(std::log(hz / 20.0) / std::log(topHz / 20.0));
    };
    for (const double hz : {100., 1000., 10000.}) {
        if (hz > topHz) continue;
        const auto x = xFor(hz);
        g.setColour(look::outline);
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
    }
    juce::Path curve;
    bool started = false;
    for (std::size_t i = 1; i < shown_->binsDbfs.size(); ++i) {
        const double hz = static_cast<double>(i) * shown_->sampleRate / shown_->request.fftSize;
        if (hz < 20 || hz > topHz) continue;
        const auto value = std::isfinite(shown_->binsDbfs[i]) ? shown_->binsDbfs[i] : floorDb;
        const auto x = xFor(hz), y = heightFor(value, plot);
        if (!started) { curve.startNewSubPath(x, y); started = true; }
        else curve.lineTo(x, y);
    }
    g.setColour(look::value);
    g.strokePath(curve, juce::PathStrokeType(1.4f));
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const float x = plot.getRight() + 5.f + static_cast<float>(channel) * 6.f;
        g.setColour(look::outline);
        g.fillRect(x, plot.getY(), 4.f, plot.getHeight());
        const auto y = heightFor(db(shown_->rms[channel]), plot);
        g.setColour(look::value.withAlpha(.6f));
        g.fillRect(x, y, 4.f, plot.getBottom() - y);
        g.setColour(look::accent);
        g.fillRect(x, heightFor(db(shown_->peak[channel]), plot), 4.f, 1.5f);
    }
    g.setColour(look::textOnBand);
    g.setFont(look::font(10.f));
    const auto footer = area.withTop(plot.getBottom() + 3.f);
    g.drawText("Peak L " + level(shown_->peak[0]) + "  R " + level(shown_->peak[1]),
               footer.withHeight(12.f), juce::Justification::centredLeft, true);
    g.drawText("RMS  L " + level(shown_->rms[0]) + "  R " + level(shown_->rms[1]),
               footer.withTrimmedTop(12.f), juce::Justification::centredLeft, true);
}
} // namespace adi::ui
