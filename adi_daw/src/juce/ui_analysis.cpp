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
               "Sine-calibrated dBFS; sample peak and RMS refer to the same contiguous window. "
               "Phase is L/R correlation, not a loudness measurement.");
    setInterceptsMouseClicks(false, false);
}
void SharedSpectrumView::setMode(Mode mode) {
    if (mode_ == mode) return;
    lease_.reset(); shown_.reset(); spectrogram_ = {}; clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    mode_ = mode; updateDemand(); repaint();
}
void SharedSpectrumView::setSource(engine::ScopePoint point, std::int64_t device) {
    if (point_ == point && device_ == device) return;
    lease_.reset(); shown_.reset(); spectrogram_ = {}; clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    point_ = point; device_ = device; updateDemand(); repaint();
}
void SharedSpectrumView::setChannel(SharedAnalysis::Channel channel) {
    if (channel_ == channel) return;
    lease_.reset(); shown_.reset(); spectrogram_ = {}; clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    channel_ = channel; updateDemand(); repaint();
}
void SharedSpectrumView::freeze(bool value) {
    if (frozen_ == value) return;
    frozen_ = value;
    if (value) lease_.reset();
    else { shown_.reset(); clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0; updateDemand(); }
    repaint();
}
void SharedSpectrumView::clear() {
    // Local interval only: clearing one view never seeks the tap, discards another view's
    // publication or resets audio/loudness state. Wait for a subsequent worker window.
    if (shown_) {
        clearedRevision_ = shown_->revision; clearedEpoch_ = shown_->sourceEpoch;
        clearedColumn_ = shown_->columns.empty() ? 0 : shown_->columns.back().serial;
    }
    shown_.reset(); spectrogram_ = {};
    status_ = lease_.available() ? Status::Waiting : Status::Detached; repaint();
}
SharedSpectrumView::~SharedSpectrumView() {
    unwatchAncestors();
    lease_.reset();
}
void SharedSpectrumView::present(std::shared_ptr<SharedAnalysis> broker, std::int64_t track) {
    if (broker_ != broker || track_ != track) {
        lease_.reset();
        shown_.reset();
        spectrogram_ = {};
        broker_ = std::move(broker);
        track_ = track;
        clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
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
    } else if (frozen_) {
        lease_.reset();
    } else {
        if (!lease_) {
            SharedAnalysis::Request request{track_, point_, 1024, channel_, dsp::Window::Hann};
            request.device = device_;
            switch (mode_) {
            case Mode::Spectrum: request.outputs = SharedAnalysis::Spectrum; break;
            case Mode::Scope: request.outputs = SharedAnalysis::Scope; break;
            case Mode::Meters: request.outputs = SharedAnalysis::Meters; break;
            case Mode::Phase: request.outputs = SharedAnalysis::Phase; break;
            case Mode::Spectrogram: request.outputs = SharedAnalysis::Spectrogram; break;
            }
            try { lease_ = broker_->acquire(request); }
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
    if (!lease_) return;
    broker_->requestFrame();
    updateDemand(); // owner resolution may have changed pending availability/generation
    if (!lease_.available()) return;
    auto next = lease_.read();
    if (next && next->sourceEpoch == clearedEpoch_ && next->revision <= clearedRevision_) next.reset();
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
    const char *names[]{"Spectrum", "Scope", "Sample peak / RMS", "Phase / L-R correlation", "Spectrogram"};
    const char *points[]{"Post Mixer", "Post FX", "Pre FX", "Device In", "Device Out"};
    g.drawText(juce::String(names[static_cast<int>(mode_)]) + " / " + points[static_cast<int>(point_)] +
                   (frozen_ ? " / Frozen" : ""), area.withHeight(14.f), juce::Justification::centredLeft, true);
    auto plot = area.withTrimmedTop(16.f).withTrimmedBottom(29.f).withTrimmedRight(18.f);
    if (plot.getHeight() <= 4 || plot.getWidth() <= 4) return;
    g.setFont(look::font(9.f));
    if (mode_ == Mode::Spectrum || mode_ == Mode::Meters) for (int step = 0; step <= 4; ++step) {
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
    if (mode_ == Mode::Scope) {
        const auto size = shown_->scopeLeft.size();
        if (size > 1) for (int channel = 0; channel < 2; ++channel) {
            const auto &samples = channel ? shown_->scopeRight : shown_->scopeLeft;
            juce::Path wave;
            // Min/max per raster column keeps narrow peaks instead of skipping samples.
            const auto columns = std::max(1, juce::roundToInt(plot.getWidth()));
            for (int x = 0; x < columns; ++x) {
                const auto first = static_cast<std::size_t>(x) * size / static_cast<std::size_t>(columns);
                const auto end = std::max(first + 1, static_cast<std::size_t>(x + 1) * size / static_cast<std::size_t>(columns));
                float lo = 1, hi = -1;
                for (auto i = first; i < std::min(size, end); ++i) { lo = std::min(lo, samples[i]); hi = std::max(hi, samples[i]); }
                const auto px = plot.getX() + static_cast<float>(x);
                const auto y = [&](float v) { return plot.getCentreY() - std::clamp(v, -1.f, 1.f) * plot.getHeight() * .47f; };
                wave.startNewSubPath(px, y(lo)); wave.lineTo(px, y(hi));
            }
            g.setColour((channel ? look::accent : look::value).withAlpha(.8f));
            g.strokePath(wave, juce::PathStrokeType(1.f));
        }
    } else if (mode_ == Mode::Meters) {
        for (int channel = 0; channel < 2; ++channel) {
            const auto band = plot.withWidth(plot.getWidth() * .35f).withX(plot.getX() + plot.getWidth() * (.1f + .45f * static_cast<float>(channel)));
            const auto y = heightFor(db(shown_->rms[static_cast<std::size_t>(channel)]), band);
            g.setColour(look::value.withAlpha(.65f)); g.fillRect(band.withTop(y));
            g.setColour(look::accent);
            g.fillRect(band.withY(heightFor(db(shown_->peak[static_cast<std::size_t>(channel)]), band)).withHeight(2.f));
        }
    } else if (mode_ == Mode::Phase) {
        const auto radius = std::min(plot.getWidth(), plot.getHeight()) * .46f;
        const auto centre = plot.getCentre();
        g.setColour(look::outline); g.drawEllipse(centre.x - radius, centre.y - radius, radius * 2, radius * 2, 1.f);
        g.drawVerticalLine(juce::roundToInt(centre.x), centre.y - radius, centre.y + radius);
        g.drawHorizontalLine(juce::roundToInt(centre.y), centre.x - radius, centre.x + radius);
        g.setColour(look::value.withAlpha(.55f));
        const auto size = std::min(shown_->scopeLeft.size(), shown_->scopeRight.size());
        const auto step = std::max<std::size_t>(1, size / 1024);
        for (std::size_t i = 0; i < size; i += step) {
            const float l = shown_->scopeLeft[i], r = shown_->scopeRight[i];
            g.fillRect(centre.x + std::clamp((l - r) * .5f, -1.f, 1.f) * radius,
                       centre.y - std::clamp((l + r) * .5f, -1.f, 1.f) * radius, 1.4f, 1.4f);
        }
    } else if (mode_ == Mode::Spectrogram) {
        const auto skipped = shown_->sourceEpoch == clearedEpoch_ ? static_cast<std::size_t>(std::count_if(
            shown_->columns.begin(), shown_->columns.end(), [this](const auto &column) { return column.serial <= clearedColumn_; })) : 0;
        const auto count = shown_->columns.size() - skipped;
        const auto topHz = std::min(20000., shown_->sampleRate * .5);
        // Each display column represents exactly one hop, with max aggregation across the
        // logarithmic row's FFT bins. No renderer-dependent resampling of analysis time.
        if (count && topHz > 20) {
            constexpr int rows = 256;
            if (spectrogram_.isNull() || rasterRevision_ != shown_->revision || rasterEpoch_ != shown_->sourceEpoch) {
                auto firstColumn = skipped;
                const bool reset = spectrogram_.isNull() || rasterEpoch_ != shown_->sourceEpoch ||
                    rasterColumn_ < shown_->columns.front().serial - 1;
                if (reset) {
                    spectrogram_ = juce::Image(juce::Image::RGB, 128, rows, true);
                    spectrogram_.clear(spectrogram_.getBounds(), look::display);
                } else {
                    while (firstColumn < shown_->columns.size() && shown_->columns[firstColumn].serial <= rasterColumn_) ++firstColumn;
                    const auto added = static_cast<int>(shown_->columns.size() - firstColumn);
                    if (added > 0 && added < 128) spectrogram_.moveImageSection(0, 0, added, 0, 128 - added, rows);
                }
                std::array<std::size_t, rows> firstBin{}, lastBin{};
                std::array<juce::Colour, 256> palette;
                for (int row = 0; row < rows; ++row) {
                    const auto low = 20. * std::pow(topHz / 20., static_cast<double>(row) / rows);
                    const auto high = 20. * std::pow(topHz / 20., static_cast<double>(row + 1) / rows);
                    firstBin[static_cast<std::size_t>(row)] = std::max<std::size_t>(1, static_cast<std::size_t>(low * shown_->request.fftSize / shown_->sampleRate));
                    lastBin[static_cast<std::size_t>(row)] = std::max(firstBin[static_cast<std::size_t>(row)] + 1,
                        static_cast<std::size_t>(high * shown_->request.fftSize / shown_->sampleRate) + 1);
                    const auto strength = static_cast<float>(row) / 255.f;
                    palette[static_cast<std::size_t>(row)] = look::display.interpolatedWith(look::value, strength)
                        .interpolatedWith(look::accent, std::max(0.f, (strength - .7f) / .3f));
                }
                juce::Image::BitmapData pixels(spectrogram_, juce::Image::BitmapData::writeOnly);
                for (auto column = firstColumn; column < shown_->columns.size(); ++column) for (int row = 0; row < rows; ++row) {
                    const auto &bins = shown_->columns[column].binsDbfs;
                    float peak = floorDb;
                    for (auto i = firstBin[static_cast<std::size_t>(row)]; i < std::min(bins.size(), lastBin[static_cast<std::size_t>(row)]); ++i)
                        peak = std::max(peak, bins[i]);
                    const auto strength = std::clamp((peak - floorDb) / -floorDb, 0.f, 1.f);
                    pixels.setPixelColour(static_cast<int>(128 - shown_->columns.size() + column), rows - row - 1,
                        palette[static_cast<std::size_t>(strength * 255.f)]);
                }
                rasterRevision_ = shown_->revision; rasterEpoch_ = shown_->sourceEpoch;
                rasterColumn_ = shown_->columns.back().serial;
            }
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            g.drawImage(spectrogram_, plot);
        }
    }
    const double topHz = std::min(20000.0, shown_->sampleRate * .5);
    if (topHz <= 20) return;
    const auto xFor = [&](double hz) {
        return plot.getX() + plot.getWidth() * static_cast<float>(std::log(hz / 20.0) / std::log(topHz / 20.0));
    };
    if (mode_ == Mode::Spectrum) for (const double hz : {100., 1000., 10000.}) {
        if (hz > topHz) continue;
        const auto x = xFor(hz);
        g.setColour(look::outline);
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
    }
    juce::Path curve;
    bool started = false;
    if (mode_ == Mode::Spectrum) for (std::size_t i = 1; i < shown_->binsDbfs.size(); ++i) {
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
    g.drawText((mode_ == Mode::Phase ? "Correlation " + juce::String(shown_->correlation, 3) :
                   "RMS  L " + level(shown_->rms[0]) + "  R " + level(shown_->rms[1])) +
                   (shown_->gapCount ? " / Gap " + juce::String(static_cast<juce::int64>(shown_->gapCount)) : "") +
                   (shown_->nonFiniteSamples ? " / Invalid samples" : "") +
                   (shown_->budgetLimited ? " / Catching up" : ""),
               footer.withTrimmedTop(12.f), juce::Justification::centredLeft, true);
}

AnalysisPane::AnalysisPane() {
    setTitle("Analysis");
    modeBox.addItemList({"Spectrum", "Scope", "Meters", "Phase", "Spectrogram"}, 1);
    sourceBox.addItemList({"Main", "Selected"}, 1);
    pointBox.addItemList({"Post Mixer", "Post FX", "Pre FX"}, 1);
    channelBox.addItemList({"Mid", "Side", "Left", "Right", "Stereo Power"}, 1);
    for (auto *box : {&modeBox, &sourceBox, &pointBox, &channelBox}) {
        box->setSelectedId(1, juce::dontSendNotification); addAndMakeVisible(*box);
    }
    modeBox.setTitle("Analysis mode"); sourceBox.setTitle("Analysis channel source");
    pointBox.setTitle("Analysis tap point"); channelBox.setTitle("Spectrum channel");
    modeBox.onChange = [this] { setMode(static_cast<SharedSpectrumView::Mode>(modeBox.getSelectedId() - 1)); };
    sourceBox.onChange = [this] { source_ = sourceBox.getSelectedId() == 2 ? Source::Selected : Source::Main; };
    pointBox.onChange = [this] { setPoint(static_cast<engine::ScopePoint>(pointBox.getSelectedId() - 1)); };
    channelBox.onChange = [this] { view.setChannel(static_cast<SharedAnalysis::Channel>(channelBox.getSelectedId() - 1)); };
    freezeButton.setClickingTogglesState(true);
    freezeButton.onClick = [this] { view.freeze(freezeButton.getToggleState()); };
    freezeButton.setTooltip("Freeze this picture and release its live analysis demand. Resume waits for fresh audio.");
    resetButton.onClick = [this] { view.clear(); };
    resetButton.setTooltip("Clear this view's display interval; other analyzer views and audio are unchanged.");
    expandButton.onClick = [this] { if (onExpand) onExpand(view.track()); };
    addAndMakeVisible(freezeButton); addAndMakeVisible(resetButton); addAndMakeVisible(expandButton); addAndMakeVisible(view);
}
void AnalysisPane::setMode(SharedSpectrumView::Mode mode) {
    view.setMode(mode); modeBox.setSelectedId(static_cast<int>(mode) + 1, juce::dontSendNotification);
    channelBox.setEnabled(mode == SharedSpectrumView::Mode::Spectrum || mode == SharedSpectrumView::Mode::Spectrogram);
}
void AnalysisPane::setSource(Source source) {
    source_ = source; sourceBox.setSelectedId(source == Source::Selected ? 2 : 1, juce::dontSendNotification);
}
void AnalysisPane::setPoint(engine::ScopePoint point) {
    view.setSource(point); pointBox.setSelectedId(static_cast<int>(point) + 1, juce::dontSendNotification);
}
void AnalysisPane::frame(std::shared_ptr<SharedAnalysis> broker, std::int64_t main, std::int64_t selected) {
    view.present(std::move(broker), source_ == Source::Selected ? selected : main); view.frame();
}
void AnalysisPane::resized() {
    auto area = getLocalBounds();
    auto controls = area.removeFromTop(25).reduced(2);
    const int width = std::max(38, (controls.getWidth() - 180) / 4);
    for (auto *box : {&modeBox, &sourceBox, &pointBox, &channelBox}) box->setBounds(controls.removeFromLeft(width).reduced(1, 0));
    freezeButton.setBounds(controls.removeFromLeft(60).reduced(1, 0));
    resetButton.setBounds(controls.removeFromLeft(60).reduced(1, 0));
    expandButton.setBounds(controls.reduced(1, 0));
    view.setBounds(area);
}
} // namespace adi::ui
