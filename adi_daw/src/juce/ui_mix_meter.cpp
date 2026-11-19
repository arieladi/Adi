// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mix_meter.hpp"
#include "ui_look.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace adi::ui {
namespace {
using Reading = dsp::StereoEnvelopeMeter::Reading;
using Weighting = dsp::StereoEnvelopeMeter::Weighting;
using Audition = MixOutputMeter::Audition;
constexpr int filterListenItem = 6;
constexpr Audition filterAudition(Weighting weighting) noexcept {
    switch (weighting) {
    case Weighting::A: return Audition::FilterA;
    case Weighting::C: return Audition::FilterC;
    case Weighting::K: return Audition::FilterK;
    case Weighting::Standard: return Audition::FilterFlat;
    }
    return Audition::FilterFlat;
}
double db(double amplitude) {
    return amplitude > 0. ? 20. * std::log10(amplitude) : -std::numeric_limits<double>::infinity();
}
juce::String number(double value) {
    return std::isfinite(value) ? juce::String(value, 1) : juce::String("-inf");
}
void text(juce::Graphics &g, juce::Rectangle<int> area, const juce::String &value, juce::Colour color,
          juce::Justification alignment = juce::Justification::centred) {
    g.setColour(color);
    g.drawFittedText(value, area, alignment, 1);
}
void stereoRail(juce::Graphics &g, juce::Rectangle<int> row, const char *title, const char *left,
                const char *right, std::optional<double> value) {
    auto labels = row.removeFromTop(look::mixMeterRailLabelHeight);
    text(g, labels, title, look::text);
    text(g, labels, left, look::textDim, juce::Justification::centredLeft);
    text(g, labels, right, look::textDim, juce::Justification::centredRight);
    const int hairline = juce::roundToInt(look::hairline);
    const int markerInset = juce::roundToInt(look::radiusSmall);
    const auto line = row.withSizeKeepingCentre(row.getWidth(), juce::roundToInt(look::radiusThumb));
    g.setColour(look::chooser);
    g.fillRect(line);
    g.setColour(look::textDim);
    g.fillRect(line.getCentreX(), line.getY() - hairline, hairline, line.getHeight() + 2 * hairline);
    if (value) {
        const auto x = line.getX() + juce::roundToInt(static_cast<double>(line.getWidth() - 1) *
                                                      (.5 + .5 * std::clamp(*value, -1., 1.)));
        g.setColour(look::value);
        g.fillRect(x - hairline, line.getY() - markerInset, line.getHeight(), line.getHeight() + 2 * markerInset);
    }
}
} // namespace

MixOutputMeter::MixOutputMeter() {
    setTitle("Main output meter");
    setOpaque(true);
    listenBox.setTitle("Audition Main output");
    listenBox.addItemList({"Listen: Stereo", "Solo Left", "Solo Right", "Solo Mid", "Solo Side"}, 1);
    listenBox.addItem("Filter Listen", filterListenItem);
    listenBox.setSelectedId(1, juce::dontSendNotification);
    listenBox.onChange = [this] {
        const int item = listenBox.getSelectedId();
        if (!audition_ || !eligible() || item < 1 || item > filterListenItem) {
            const int current = engine::isFilterListen(listening_) ? filterListenItem : static_cast<int>(listening_) + 1;
            listenBox.setSelectedId(current, juce::dontSendNotification);
            return;
        }
        chooseAudition(item == filterListenItem ? filterAudition(profile_.weighting) : static_cast<Audition>(item - 1));
    };
    addChildComponent(listenBox);
    scaleBox.setTitle("Meter scale");
    scaleBox.addItemList({"dBFS", "K-12", "K-14", "K-20"}, 1);
    scaleBox.setSelectedId(1, juce::dontSendNotification);
    scaleBox.onChange = [this] { repaint(plot_); };
    weightingBox.setTitle("Meter weighting");
    weightingBox.addItemList({"Flat", "A", "C", "K"}, 1);
    weightingBox.setSelectedId(1, juce::dontSendNotification);
    weightingBox.onChange = [this] {
        static constexpr std::array weights{Weighting::Standard, Weighting::A, Weighting::C, Weighting::K};
        const auto index = static_cast<unsigned>(weightingBox.getSelectedId() - 1);
        if (const auto next = index < weights.size() ? weights[index] : Weighting::Standard; next != profile_.weighting) {
            profile_.weighting = next;
            resetProfile();
            if (engine::isFilterListen(listening_)) chooseAudition(filterAudition(next));
        }
    };
    channelBox.setTitle("Meter channels");
    channelBox.addItemList({"Stereo", "M/S"}, 1);
    channelBox.setSelectedId(1, juce::dontSendNotification);
    channelBox.onChange = [this] {
        if (const bool next = channelBox.getSelectedId() == 2; next != profile_.midSide) {
            profile_.midSide = next;
            resetProfile();
        }
    };
    floatButton.setTitle("Floating dynamic range bars");
    floatButton.setToggleState(true, juce::dontSendNotification);
    floatButton.onClick = [this] { repaint(plot_); };
    resetButton.setTitle("Reset Main meter interval");
    resetButton.onClick = [this] { resetInterval(); };
    for (juce::Component *child :
         std::array<juce::Component *, 5>{&scaleBox, &weightingBox, &channelBox, &floatButton, &resetButton})
        addAndMakeVisible(child);
}
MixOutputMeter::~MixOutputMeter() {
    stopWatching();
    stopAudition();
}
void MixOutputMeter::chooseAudition(Audition mode) {
    if (mode != listening_ && audition_ && eligible()) {
        listening_ = mode;
        audition_(mode);
    }
}
void MixOutputMeter::stopAudition() {
    if (std::exchange(listening_, Audition::Stereo) == Audition::Stereo) return;
    listenBox.setSelectedId(1, juce::dontSendNotification);
    if (audition_) audition_(Audition::Stereo);
}
void MixOutputMeter::setAuditionSink(std::function<void(Audition)> sink) {
    const juce::Component::SafePointer<MixOutputMeter> alive(this);
    stopAudition();
    if (!alive) return;
    audition_ = std::move(sink);
    listenBox.setVisible(static_cast<bool>(audition_));
    resized();
}
bool MixOutputMeter::eligible() const noexcept {
    if (suspended_ || plot_.isEmpty() || !isShowing()) return false;
    if (const auto *peer = getPeer(); peer && peer->isMinimised()) return false;
    auto visible = plot_;
    for (auto *parent = getParentComponent(); parent; parent = parent->getParentComponent()) {
        visible = visible.getIntersection(getLocalArea(parent, parent->getLocalBounds()));
        if (visible.isEmpty()) return false;
    }
    return true;
}
void MixOutputMeter::release() {
    lease_.reset();
    const bool hadReading = std::exchange(wasAvailable_, false) || shown_ || main_.has_value();
    shown_.reset();
    main_.reset();
    if (hadReading) repaint(plot_);
    stopAudition();
}
void MixOutputMeter::resetProfile() {
    lease_.reset();
    wasAvailable_ = false;
    shown_.reset();
    main_.reset();
    repaint(plot_);
}
void MixOutputMeter::resetInterval() {
    if (eligible() && lease_.restartStereoMeter()) {
        shown_.reset();
        main_.reset();
        repaint(plot_);
    }
}
void MixOutputMeter::stopWatching() {
    for (auto &other : ancestors_)
        if (other != nullptr) other->removeComponentListener(this);
    ancestors_.clear();
}
void MixOutputMeter::parentHierarchyChanged() {
    stopWatching();
    for (auto *other = getParentComponent(); other; other = other->getParentComponent()) {
        ancestors_.emplace_back(other);
        other->addComponentListener(this);
    }
    visibilityChanged();
}
void MixOutputMeter::visibilityChanged() {
    if (!eligible()) release();
}
void MixOutputMeter::componentVisibilityChanged(juce::Component &) { visibilityChanged(); }
void MixOutputMeter::componentBeingDeleted(juce::Component &) { release(); }
void MixOutputMeter::suspend(bool value) {
    suspended_ = value;
    if (suspended_) release();
}
double MixOutputMeter::offsetDb() const noexcept {
    switch (scaleBox.getSelectedId()) {
    case 2:
        return 12.;
    case 3:
        return 14.;
    case 4:
        return 20.;
    default:
        return 0.;
    }
}
void MixOutputMeter::frame(const std::shared_ptr<SharedAnalysis> &broker, std::int64_t track,
                           const std::shared_ptr<const MainMeterSource> &source) {
    if (!eligible() || !broker || !broker->active() || track <= 0) { release(); return; }
    if (track_ != track || broker_.lock() != broker) {
        const juce::Component::SafePointer<MixOutputMeter> alive(this);
        stopAudition();
        if (!alive) return;
        track_ = track;
        broker_ = broker;
        resetProfile();
    }
    if (!lease_) {
        try { lease_ = broker->acquireStereoMeter(track, profile_); } catch (...) { return; }
    }
    broker->requestFrame();
    const bool available = lease_.available();
    if (std::exchange(wasAvailable_, available) != available) repaint(plot_);
    const auto publication = lease_.readStereoMeter();
    const bool sameService = !lastMainSource_.owner_before(source) && !source.owner_before(lastMainSource_);
    const bool changed = shown_ != publication;
    if (!changed && sameService)
        return;
    shown_ = publication;
    lastMainSource_ = source;
    std::optional<MainMeterSnapshot> next;
    if (publication && source && source->track == track && source->read) {
        try {
            next = source->read();
        } catch (...) {
            next.reset();
        }
        if (next && (next->track != track || next->sampleRate != publication->sampleRate ||
                     !next->formatSequence || std::isnan(next->truePeakDb) || std::isnan(next->momentaryLufs)))
            next.reset();
    }
    if (changed || main_ != next) repaint(plot_);
    main_ = next;
}
void MixOutputMeter::resized() {
    auto area = getLocalBounds().reduced(look::border);
    const int hairline = juce::roundToInt(look::hairline);
    const int controlHeight = look::mixMeterControlHeight;
    auto channels = area.removeFromBottom(std::min(controlHeight, area.getHeight() / 5));
    if (listenBox.isVisible()) listenBox.setBounds(channels.removeFromRight(channels.getWidth() / 2).reduced(hairline, 0));
    channelBox.setBounds(channels.reduced(hairline, 0));
    auto choices = area.removeFromBottom(std::min(controlHeight, area.getHeight() / 4));
    scaleBox.setBounds(choices.removeFromLeft(choices.getWidth() / 2).reduced(hairline, 0));
    weightingBox.setBounds(choices.reduced(hairline, 0));
    auto controls = area.removeFromBottom(std::min(controlHeight, area.getHeight() / 4));
    floatButton.setBounds(controls.removeFromLeft(controls.getWidth() / 2));
    resetButton.setBounds(controls.reduced(hairline, 0));
    plot_ = area;
    if (plot_.isEmpty()) release();
}
void MixOutputMeter::paint(juce::Graphics &g) {
    g.fillAll(look::panel);
    paintReadings(g, plot_);
}
void MixOutputMeter::mouseUp(const juce::MouseEvent &event) {
    if (event.mods.isLeftButtonDown() && !event.mods.isPopupMenu() && event.mouseWasClicked() &&
        plot_.contains(event.getMouseDownPosition()) && plot_.contains(event.getPosition()))
        resetInterval();
}
void MixOutputMeter::paintReadings(juce::Graphics &g, juce::Rectangle<int> area) {
    g.setColour(look::display);
    g.fillRect(area);
    area.reduce(look::border, look::border);
    if (area.isEmpty())
        return;
    const Reading *r = shown_ ? &shown_->reading : nullptr;
    g.setFont(look::font(look::fontLabel, true));
    text(g, area.removeFromTop(look::mixMeterTitleHeight), "Main output", look::text);
    g.setFont(look::font(look::fontTiny));
    stereoRail(g, area.removeFromTop(look::mixMeterRailHeight), "Balance", "L", "R", r ? std::optional(r->balance) : std::nullopt);
    stereoRail(g, area.removeFromTop(look::mixMeterRailHeight), "Correlation", "-1", "+1",
               r ? std::optional(r->signCoincidence) : std::nullopt);
    const auto values = [&](const char *label, double a, double b) {
        auto line = area.removeFromTop(look::mixMeterReadingHeight);
        text(g, line, label, look::textDim);
        text(g, line, r ? number(a) : "-", look::text, juce::Justification::centredLeft);
        text(g, line, r ? number(b) : "-", look::text, juce::Justification::centredRight);
    };
    const auto offset = offsetDb();
    if (r) {
        values("Peak", db(r->maximumPeak[0]) + offset, db(r->maximumPeak[1]) + offset);
        values("Dynamic", r->dynamicDb[0], r->dynamicDb[1]);
        values("RMS", db(r->sineReferencedRms[0]) + offset, db(r->sineReferencedRms[1]) + offset);
    } else {
        values("Peak", 0, 0);
        values("Dynamic", 0, 0);
        values("RMS", 0, 0);
    }
    if (area.getHeight() < look::mixMeterMinimumBarHeight)
        return;
    if (main_ && area.getHeight() >= look::mixMeterLoudnessMinimumHeight) {
        auto line = area.removeFromBottom(look::fieldHeight);
        text(g, line, "TP " + number(main_->truePeakDb) + "   M " + number(main_->momentaryLufs) + " LUFS",
             look::textDim);
    }
    auto names = area.removeFromBottom(look::mixMeterChannelLabelHeight);
    const bool ms = channelBox.getSelectedId() == 2;
    text(g, names, ms ? "MID" : "LEFT", look::textDim, juce::Justification::centredLeft);
    text(g, names, ms ? "SIDE" : "RIGHT", look::textDim, juce::Justification::centredRight);
    const int hairline = juce::roundToInt(look::hairline);
    const int heavyLine = juce::roundToInt(look::heavyLine);
    const int top = area.getY() + look::border, height = std::max(hairline, area.getHeight() - 2 * look::border);
    const auto yDb = [&](double value) {
        return top + juce::roundToInt(height * std::clamp(-value / 48., 0., 1.));
    };
    const auto left = area.getX() + area.getWidth() / 5, right = area.getRight() - area.getWidth() / 5;
    const int barWidth = std::clamp(area.getWidth() / 12, look::border, look::mixMeterMaxBarWidth);
    const int gap = std::max(juce::roundToInt(look::radiusThumb), area.getWidth() / 40);
    const int labelHeight = juce::roundToInt(look::fontSmall);
    for (int tick = 0; tick >= -48; tick -= height > look::mixMeterDenseScaleHeight ? 3 : 6) {
        const auto y = yDb(tick);
        const auto label = juce::String(tick + static_cast<int>(offset));
        text(g, {area.getX(), y - labelHeight / 2, left - area.getX() - heavyLine, labelHeight}, label, look::textDim,
             juce::Justification::centredRight);
        text(g, {right + heavyLine, y - labelHeight / 2, area.getRight() - right - heavyLine, labelHeight}, label, look::textDim,
             juce::Justification::centredLeft);
    }
    const int amber = yDb(-9.), red = yDb(-3.);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        const auto x = ch ? right - barWidth : left;
        const auto dynX = ch ? x - gap - barWidth : x + barWidth + gap;
        g.setColour(look::chooser);
        g.fillRect(x, top, barWidth, height);
        g.fillRect(dynX, top, barWidth, height);
        if (!r)
            continue;
        const auto peakY = yDb(db(r->peak[ch])), rmsY = yDb(db(r->sineReferencedRms[ch]));
        const double anchor = floatButton.getToggleState() ? db(r->floatingPeak[ch]) : 0.;
        const auto dynTop = yDb(anchor), dynBottom = yDb(anchor - r->dynamicDb[ch]);
        for (auto y = top; y < top + height; y += look::mixMeterSegmentHeight + hairline) {
            const int segment = std::min(look::mixMeterSegmentHeight, top + height - y);
            if (y >= std::min(rmsY, peakY)) {
                const auto colour = y < red ? look::record : y < amber ? look::handle : look::meterLevel;
                g.setColour(y >= rmsY ? colour : colour.darker());
                g.fillRect(x, y, barWidth, segment);
            }
            if (y >= dynTop && y < dynBottom) {
                g.setColour(look::value);
                g.fillRect(dynX, y, barWidth, segment);
            }
        }
        if (r->peakHold[ch] > 0) {
            g.setColour(look::meterPeak);
            g.fillRect(x - hairline, yDb(db(r->peakHold[ch])), barWidth + 2 * hairline, heavyLine);
        }
        if (r->rmsHold[ch] > 0) {
            g.setColour(look::accent);
            g.fillRect(x - hairline, yDb(db(r->rmsHold[ch])), barWidth + 2 * hairline, hairline);
        }
    }
    if (!r)
        text(g, area, wasAvailable_ ? "Waiting" : "Unavailable", look::text);
}
} // namespace adi::ui
