// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_analysis.hpp"
#include "ui_look.hpp"
#include "adi/engine/main_loudness.hpp"
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
juce::String meterNumber(float value) {
    return std::isfinite(value) ? juce::String(value, 1) : juce::String("--");
}
} // namespace
juce::Colour analysisPaletteColour(std::size_t palette, std::uint8_t sample, bool reverse) noexcept {
    if (palette >= palettes::count) palette = palettes::defaultIndex;
    if (palette != palettes::defaultIndex) return juce::Colour(0xff000000u | palettes::rgb(palette, sample, reverse));
    // Preserve the exact original 256-node JUCE color law and integer pixel
    // indexing. This initialization occurs on owner-side view construction.
    static const auto original = [] {
        std::array<juce::Colour, 256> colors;
        for (std::size_t i = 0; i < colors.size(); ++i) {
            const auto strength = static_cast<float>(i) / 255.f;
            colors[i] = look::display.interpolatedWith(look::value, strength)
                .interpolatedWith(look::accent, std::max(0.f, (strength - .7f) / .3f));
        }
        return colors;
    }();
    return original[reverse ? 255u - sample : sample];
}
struct SharedSpectrumView::Raster {
    static constexpr int width = 128, height = 256;
    static constexpr std::uint16_t gap = 256;
    std::array<std::uint16_t, static_cast<std::size_t>(width * height)> intensity{};
    std::array<juce::Colour, 256> colors{};
    juce::Image image;
    std::uint64_t revision = 0, epoch = 0, column = 0;
    bool paletteDirty = true;
    Raster() { intensity.fill(gap); }
    void setPalette(std::size_t index, bool reverse) noexcept {
        for (std::size_t i = 0; i < colors.size(); ++i)
            colors[i] = analysisPaletteColour(index, static_cast<std::uint8_t>(i), reverse);
        paletteDirty = true;
    }
    void clearColumns(int first, int count) {
        for (int y = 0; y < height; ++y) {
            const auto start = static_cast<std::size_t>(y * width + first);
            std::fill_n(intensity.data() + start, count, gap);
        }
        image.clear({first, 0, count, height}, look::display);
    }
    void shift(int added) {
        for (int y = 0; y < height; ++y) {
            auto *row = intensity.data() + static_cast<std::size_t>(y * width);
            std::copy_n(row + added, width - added, row);
        }
        image.moveImageSection(0, 0, added, 0, width - added, height);
        clearColumns(width - added, added);
    }
    void recolor() {
        if (image.isNull()) return;
        juce::Image::BitmapData pixels(image, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const auto value = intensity[static_cast<std::size_t>(y * width + x)];
            pixels.setPixelColour(x, y, value == gap ? look::display : colors[value]);
        }
        paletteDirty = false;
    }
};
std::optional<MainMeterSnapshot> readMainMeterSnapshot(const engine::MainLoudness &meter,
                                                     std::int64_t track, double sampleRate) noexcept {
    const auto expected = meter.preparedFormatSequence();
    const auto &readout = meter.readout();
    const auto before = readout.formatSequence.load(std::memory_order_acquire);
    if (track <= 0 || !std::isfinite(sampleRate) || sampleRate <= 0 || expected == 0 || before != expected) return {};
    MainMeterSnapshot result;
    result.track = track;
    result.sampleRate = sampleRate;
    result.formatSequence = before;
    result.momentaryLufs = readout.momentaryLufs.load(std::memory_order_relaxed);
    result.shortTermLufs = readout.shortTermLufs.load(std::memory_order_relaxed);
    result.integratedLufs = readout.integratedLufs.load(std::memory_order_relaxed);
    result.truePeakDb = readout.truePeakDb.load(std::memory_order_relaxed);
    if (readout.formatSequence.load(std::memory_order_acquire) != before) return {};
    return result;
}
SharedSpectrumView::SharedSpectrumView() {
    (void)analysisPaletteColour(palettes::defaultIndex, 0); // no lazy LUT initialization in paint
    setTitle("Channel spectrum");
    setTooltip("The channel after its mixer. Full and boxed views share one Hann-window spectrum. "
               "Sine-calibrated dBFS; sample peak and RMS refer to the same contiguous window. "
               "Phase is L/R correlation, not a loudness measurement. Meters on Main/Post Mixer "
               "also show the existing programme loudness before cue/metronome: momentary, "
               "short-term and integrated LUFS, plus the held aggregate Main true peak. "
               "Freeze/Reset affect this view only, never the shared Main measurement.");
    setInterceptsMouseClicks(false, false);
}
void SharedSpectrumView::setMode(Mode mode) {
    if (mode_ == mode) return;
    lease_.reset(); shown_.reset(); mainMeter_.reset(); raster_.reset(); clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    sourcePosition_ = {}; freshRequired_ = false;
    mode_ = mode; updateDemand(); repaint();
}
void SharedSpectrumView::setSource(engine::ScopePoint point, std::int64_t device) {
    if (point_ == point && device_ == device) return;
    lease_.reset(); shown_.reset(); mainMeter_.reset(); raster_.reset(); clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    sourcePosition_ = {}; freshRequired_ = false;
    point_ = point; device_ = device; updateDemand(); repaint();
}
void SharedSpectrumView::setChannel(SharedAnalysis::Channel channel) {
    if (channel_ == channel) return;
    lease_.reset(); shown_.reset(); mainMeter_.reset(); raster_.reset(); clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    sourcePosition_ = {}; freshRequired_ = false;
    channel_ = channel; updateDemand(); repaint();
}
void SharedSpectrumView::setPalette(std::string_view id, bool reverse) {
    if (palette() == id && paletteReversed_ == reverse) return;
    const auto index = palettes::find(id);
    if (paletteIndex_ == index && paletteReversed_ == reverse) return;
    paletteIndex_ = index; paletteReversed_ = reverse;
    if (raster_) raster_->setPalette(index, reverse);
    if (eligible()) {
        try { updateRaster(); } catch (...) { raster_.reset(); }
    }
    repaint();
}
void SharedSpectrumView::setMainMeterSource(const std::shared_ptr<const MainMeterSource> &source) {
    if (!mainMeterSource_.owner_before(source) && !source.owner_before(mainMeterSource_)) return;
    mainMeterSource_ = source;
    mainMeter_.reset();
    repaint();
}
juce::String SharedSpectrumView::mainMeterText() const {
    if (!mainMeter_) return {};
    return "Main aggregate programme / pre-cue\nM " + meterNumber(mainMeter_->momentaryLufs) +
           "  S " + meterNumber(mainMeter_->shortTermLufs) + "  I " + meterNumber(mainMeter_->integratedLufs) +
           " LUFS\nTP (held) " + meterNumber(mainMeter_->truePeakDb) + " dBTP";
}
void SharedSpectrumView::freeze(bool value) {
    if (frozen_ == value) return;
    frozen_ = value;
    if (value) lease_.reset();
    else {
        freshRequired_ = true; sourcePosition_ = {};
        clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
        shown_.reset(); mainMeter_.reset(); raster_.reset(); updateDemand();
    }
    repaint();
}
void SharedSpectrumView::clear() {
    // Local interval only: clearing one view never seeks the tap, discards another view's
    // publication or resets audio/loudness state. Wait for a subsequent worker window.
    if (shown_) {
        clearedRevision_ = shown_->windowSerial; clearedEpoch_ = shown_->sourceEpoch;
        clearedColumn_ = shown_->historySerial;
    }
    armFreshBoundary();
    shown_.reset(); mainMeter_.reset(); raster_.reset();
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
        mainMeter_.reset();
        raster_.reset();
        broker_ = std::move(broker);
        track_ = track;
        clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
        sourcePosition_ = {}; freshRequired_ = false;
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
    const bool hadMainMeter = mainMeter_.has_value();
    if (!eligible()) {
        if (shown_ || lease_) freshRequired_ = true;
        lease_.reset();
        sourcePosition_ = {};
        shown_.reset();
        raster_.reset();
        mainMeter_.reset();
        status_ = Status::Detached;
    } else if (!broker_->active()) {
        lease_.reset(); shown_.reset(); raster_.reset(); mainMeter_.reset();
        sourcePosition_ = {}; freshRequired_ = true;
        status_ = Status::Unavailable;
    } else if (frozen_) {
        lease_.reset();
        if (mainMeterSource_.expired()) mainMeter_.reset();
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
            try {
                lease_ = broker_->acquire(request);
                sourcePosition_ = lease_.sourcePosition();
                clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
            }
            catch (...) { lease_.reset(); }
        }
        if (!lease_ || !lease_.available()) {
            if (shown_ || sourcePosition_.generation) freshRequired_ = true;
            sourcePosition_ = {};
            shown_.reset();
            raster_.reset();
            mainMeter_.reset();
            status_ = Status::Unavailable;
        } else {
            if (!sourcePosition_.generation) sourcePosition_ = lease_.sourcePosition();
            status_ = shown_ ? Status::Ready : Status::Waiting;
        }
    }
    if (previous != status_ || (hadMainMeter && !mainMeter_)) repaint();
}
void SharedSpectrumView::armFreshBoundary() noexcept {
    freshRequired_ = true;
    sourcePosition_ = lease_.sourcePosition();
}
void SharedSpectrumView::frame() noexcept {
    updateDemand();
    if (!lease_) return;
    broker_->requestFrame();
    updateDemand(); // owner resolution may have changed pending availability/generation
    if (!lease_.available()) return;
    auto next = lease_.read();
    // Invalidation can replace a live source before its next publication. Fence
    // at that activation, rather than accepting buffered replacement-source data.
    if (!next && shown_) armFreshBoundary();
    if (next && next->generation != sourcePosition_.generation) {
        armFreshBoundary();
        next.reset();
    }
    if (next && freshRequired_) {
        const auto size = static_cast<std::int64_t>(next->request.fftSize);
        if (!sourcePosition_.generation || size <= 0 || next->firstSequence < 0 ||
            next->firstSequence > std::numeric_limits<std::int64_t>::max() - size ||
            next->firstSequence + size <= sourcePosition_.written) next.reset();
        else freshRequired_ = false;
    }
    if (next && next->sourceEpoch == clearedEpoch_ && next->windowSerial <= clearedRevision_) next.reset();
    // A recreated domain may restart these counters. A discarded publication
    // cannot leave a raster that later aliases the new domain's first result.
    if (!next) raster_.reset();
    // An explicit source/transport epoch may invalidate a result without dropping the lease.
    if (next != shown_) {
        shown_ = std::move(next);
        mainMeter_.reset();
        status_ = shown_ ? Status::Ready : Status::Waiting;
        repaint();
    }
    updateMainMeter();
    try { updateRaster(); } catch (...) { raster_.reset(); }
}
void SharedSpectrumView::updateRaster() {
    if (mode_ != Mode::Spectrogram || !shown_) return;
    const auto skipped = shown_->sourceEpoch == clearedEpoch_ ? static_cast<std::size_t>(std::count_if(
        shown_->columns.begin(), shown_->columns.end(), [this](const auto &entry) { return entry.serial <= clearedColumn_; })) : 0;
    const auto count = shown_->columns.size() - skipped;
    const auto topHz = std::min(20000., shown_->sampleRate * .5);
    if ((!count && !shown_->historySerial) || topHz <= 20) return;
    if (!raster_) {
        raster_ = std::make_unique<Raster>(); raster_->setPalette(paletteIndex_, paletteReversed_);
    }
    auto &raster = *raster_;
    if (raster.image.isNull() || raster.revision != shown_->revision || raster.epoch != shown_->sourceEpoch) {
        auto firstColumn = skipped;
        const bool reset = raster.image.isNull() || raster.epoch != shown_->sourceEpoch ||
            shown_->historySerial < raster.column || shown_->historySerial - raster.column >= Raster::width;
        if (reset) {
            if (raster.image.isNull()) raster.image = juce::Image(juce::Image::RGB, Raster::width, Raster::height, true);
            raster.intensity.fill(Raster::gap); raster.image.clear(raster.image.getBounds(), look::display);
        } else {
            while (firstColumn < shown_->columns.size() && shown_->columns[firstColumn].serial <= raster.column) ++firstColumn;
            const auto added = static_cast<int>(shown_->historySerial - raster.column);
            if (added > 0) raster.shift(added);
            if (firstColumn && shown_->columns[firstColumn - 1].serial == raster.column) --firstColumn;
        }
        if (shown_->droppedHistoryColumns) {
            std::array<bool, Raster::width> retained{};
            for (auto column = skipped; column < shown_->columns.size(); ++column) {
                const auto serial = shown_->columns[column].serial;
                if (serial <= shown_->historySerial && shown_->historySerial - serial < Raster::width)
                    retained[127 - static_cast<std::size_t>(shown_->historySerial - serial)] = true;
            }
            for (std::size_t x = 0; x < retained.size(); ++x)
                if (!retained[x]) raster.clearColumns(static_cast<int>(x), 1);
        }
        std::array<std::size_t, Raster::height> firstBin{}, lastBin{};
        for (int row = 0; row < Raster::height; ++row) {
            const auto low = 20. * std::pow(topHz / 20., static_cast<double>(row) / Raster::height);
            const auto high = 20. * std::pow(topHz / 20., static_cast<double>(row + 1) / Raster::height);
            firstBin[static_cast<std::size_t>(row)] = std::max<std::size_t>(1, static_cast<std::size_t>(low * shown_->request.fftSize / shown_->sampleRate));
            lastBin[static_cast<std::size_t>(row)] = std::max(firstBin[static_cast<std::size_t>(row)] + 1,
                static_cast<std::size_t>(high * shown_->request.fftSize / shown_->sampleRate) + 1);
        }
        juce::Image::BitmapData pixels(raster.image, juce::Image::BitmapData::writeOnly);
        for (auto column = firstColumn; column < shown_->columns.size(); ++column) for (int row = 0; row < Raster::height; ++row) {
            const auto &entry = shown_->columns[column];
            if (!entry.binsDbfs || entry.serial > shown_->historySerial || shown_->historySerial - entry.serial >= Raster::width) continue;
            const auto &bins = *entry.binsDbfs;
            float peak = floorDb;
            for (auto i = firstBin[static_cast<std::size_t>(row)]; i < std::min(bins.size(), lastBin[static_cast<std::size_t>(row)]); ++i)
                peak = std::max(peak, bins[i]);
            const auto strength = std::clamp((peak - floorDb) / -floorDb, 0.f, 1.f);
            const auto value = static_cast<std::uint16_t>(strength * 255.f);
            const int x = 127 - static_cast<int>(shown_->historySerial - entry.serial), y = Raster::height - row - 1;
            raster.intensity[static_cast<std::size_t>(y * Raster::width + x)] = value;
            pixels.setPixelColour(x, y, raster.colors[value]);
        }
        raster.revision = shown_->revision; raster.epoch = shown_->sourceEpoch; raster.column = shown_->historySerial;
    }
    if (raster.paletteDirty) raster.recolor();
}
void SharedSpectrumView::updateMainMeter() noexcept {
    std::optional<MainMeterSnapshot> next;
    if (mode_ == Mode::Meters && point_ == engine::ScopePoint::PostFader && device_ == 0 && shown_ &&
        !frozen_ && eligible()) {
        if (const auto source = mainMeterSource_.lock(); source && source->track == track_ && source->read) {
            try {
                next = source->read();
                const auto validLevel = [](float value) { return std::isfinite(value) || value == -std::numeric_limits<float>::infinity(); };
                if (next && (next->track != track_ || !std::isfinite(next->sampleRate) ||
                    next->sampleRate != shown_->sampleRate || next->formatSequence == 0 || !validLevel(next->momentaryLufs) ||
                    !validLevel(next->shortTermLufs) || !validLevel(next->integratedLufs) || !validLevel(next->truePeakDb)))
                    next.reset();
            } catch (...) { next.reset(); }
        }
    }
    if (mainMeter_ != next) { mainMeter_ = next; repaint(); }
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
    if (shown_ || lease_) freshRequired_ = true;
    sourcePosition_ = {};
    lease_.reset(); shown_.reset(); mainMeter_.reset(); raster_.reset(); status_ = Status::Detached;
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
    const bool mainMeter = mode_ == Mode::Meters && mainMeter_.has_value();
    const auto meterHeight = mainMeter ? 38.f : 0.f;
    auto plot = area.withTrimmedTop(16.f).withTrimmedBottom(29.f + meterHeight).withTrimmedRight(18.f);
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
    } else if (mode_ == Mode::Spectrogram && raster_ && !raster_->image.isNull()) {
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        g.drawImage(raster_->image, plot);
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
                   (shown_->budgetLimited ? " / Catching up" : "") +
                   (shown_->historyLimited ? " / History memory limit" : "") +
                   (shown_->droppedHistoryColumns ? " / History gaps " + juce::String(static_cast<juce::int64>(shown_->droppedHistoryColumns)) : ""),
               mainMeter ? footer.withTrimmedTop(12.f).withHeight(12.f) : footer.withTrimmedTop(12.f),
               juce::Justification::centredLeft, true);
    if (mainMeter) {
        const auto lines = juce::StringArray::fromLines(mainMeterText());
        auto strip = area.withTrimmedTop(area.getHeight() - meterHeight);
        g.setColour(look::textOnBand);
        g.setFont(look::font(9.f));
        for (const auto &line : lines)
            g.drawText(line, strip.removeFromTop(12.f), juce::Justification::centredLeft, true);
    }
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
    paletteBox.setTitle("Spectrogram palette"); reverseButton.setTitle("Reverse spectrogram palette");
    static const auto previews = [] {
        std::array<juce::Image, palettes::count> images;
        for (std::size_t i = 0; i < images.size(); ++i) {
            images[i] = juce::Image(juce::Image::RGB, 96, 8, false);
            juce::Image::BitmapData pixels(images[i], juce::Image::BitmapData::writeOnly);
            for (int x = 0; x < 96; ++x) for (int y = 0; y < 8; ++y)
                pixels.setPixelColour(x, y, analysisPaletteColour(i, static_cast<std::uint8_t>(x * 255 / 95)));
        }
        return images;
    }();
    for (std::size_t i = 0; i < palettes::count; ++i) {
        const auto name = palettes::id(i);
        paletteBox.getRootMenu()->addItem(static_cast<int>(i) + 1,
            i == 0 ? juce::String("ADI (default)") : juce::String::fromUTF8(name.data(), static_cast<int>(name.size())),
            true, false, previews[i]);
    }
    const auto paletteChanged = [this] {
        if (onPaletteChange) onPaletteChange();
        else setPalette(requestedPalette(), reverseButton.getToggleState());
    };
    paletteBox.onChange = paletteChanged; reverseButton.onClick = paletteChanged;
    addAndMakeVisible(paletteBox); addAndMakeVisible(reverseButton);
    setPalette("adi");
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
    setMode(view.mode());
}
bool AnalysisPane::anyControlsFocused() noexcept {
    auto *focused = juce::Component::getCurrentlyFocusedComponent();
    return focused && focused->isShowing() &&
        (dynamic_cast<AnalysisPane *>(focused) != nullptr || focused->findParentComponentOfClass<AnalysisPane>() != nullptr);
}
void AnalysisPane::setMode(SharedSpectrumView::Mode mode) {
    view.setMode(mode); modeBox.setSelectedId(static_cast<int>(mode) + 1, juce::dontSendNotification);
    channelBox.setEnabled(mode == SharedSpectrumView::Mode::Spectrum || mode == SharedSpectrumView::Mode::Spectrogram);
    paletteBox.setEnabled(mode == SharedSpectrumView::Mode::Spectrogram);
    reverseButton.setEnabled(mode == SharedSpectrumView::Mode::Spectrogram);
}
void AnalysisPane::setSource(Source source) {
    source_ = source; sourceBox.setSelectedId(source == Source::Selected ? 2 : 1, juce::dontSendNotification);
}
void AnalysisPane::setPoint(engine::ScopePoint point) {
    view.setSource(point); pointBox.setSelectedId(static_cast<int>(point) + 1, juce::dontSendNotification);
}
std::string_view AnalysisPane::requestedPalette() const noexcept {
    const int selected = paletteBox.getSelectedId();
    return palettes::id(selected > 0 ? static_cast<std::size_t>(selected - 1) : 0);
}
void AnalysisPane::restorePaletteControls(std::string_view id, bool reverse) {
    paletteBox.setSelectedId(static_cast<int>(palettes::find(id)) + 1, juce::dontSendNotification);
    reverseButton.setToggleState(reverse, juce::dontSendNotification);
}
void AnalysisPane::setPalette(std::string_view id, bool reverse) {
    view.setPalette(id, reverse); restorePaletteControls(view.palette(), reverse);
}
void AnalysisPane::frame(std::shared_ptr<SharedAnalysis> broker, std::int64_t main, std::int64_t selected) {
    view.present(std::move(broker), source_ == Source::Selected ? selected : main); view.frame();
}
void AnalysisPane::resized() {
    const std::array<juce::Component *, 9> controls{&modeBox, &sourceBox, &pointBox, &channelBox, &paletteBox,
        &reverseButton, &freezeButton, &resetButton, &expandButton};
    const int columns = std::clamp(getWidth() / 96, 1, 4);
    const int rows = (static_cast<int>(controls.size()) + columns - 1) / columns;
    const int height = std::min(25, std::max(0, getHeight() / rows));
    for (std::size_t i = 0; i < controls.size(); ++i) {
        const int column = static_cast<int>(i) % columns, row = static_cast<int>(i) / columns;
        const int left = column * getWidth() / columns, right = (column + 1) * getWidth() / columns;
        controls[i]->setBounds(juce::Rectangle<int>(left, row * height, right - left, height).reduced(1));
    }
    view.setBounds(getLocalBounds().withTrimmedTop(rows * height));
}

AnalysisDock::AnalysisDock() {
    setTitle("Mix View analyzer dock");
    meters.setMode(SharedSpectrumView::Mode::Meters);
    phase.setMode(SharedSpectrumView::Mode::Phase);
    spectrum.setMode(SharedSpectrumView::Mode::Spectrum);
    for (auto *view : {&meters, &phase, &spectrum}) addAndMakeVisible(*view);
    sourceBox.addItemList({"Main", "Selected"}, 1);
    pointBox.addItemList({"Post Mixer", "Post FX", "Pre FX"}, 1);
    channelBox.addItemList({"Mid", "Side", "Left", "Right", "Stereo Power"}, 1);
    for (const int width : {240, 300, 360, 420}) widthBox.addItem(juce::String(width) + " px", width);
    sourceBox.setTitle("Dock channel source"); pointBox.setTitle("Dock tap point");
    channelBox.setTitle("Dock spectrum channel"); widthBox.setTitle("Dock width");
    for (auto *box : {&sourceBox, &pointBox, &channelBox, &widthBox}) {
        addAndMakeVisible(*box);
        box->onChange = [this] { changed(); };
    }
    freezeButton.setClickingTogglesState(true);
    freezeButton.onClick = [this] {
        const bool value = freezeButton.getToggleState();
        for (auto *view : {&meters, &phase, &spectrum}) view->freeze(value);
    };
    resetButton.onClick = [this] { for (auto *view : {&meters, &phase, &spectrum}) view->clear(); };
    expandButton.onClick = [this] { if (meters.track() > 0 && onExpand) onExpand(meters.track()); };
    freezeButton.setTooltip("Freeze these pictures and release this dock's live demand only.");
    resetButton.setTooltip("Clear this dock's pictures; shared Main loudness and other views are unchanged.");
    for (auto *button : {&freezeButton, &resetButton, &expandButton}) addAndMakeVisible(*button);
    configure(0, 0, 0, 300);
}
void AnalysisDock::restoreControls(int source, int point, int channel, int width) {
    sourceBox.setSelectedId(std::clamp(source, 0, 1) + 1, juce::dontSendNotification);
    pointBox.setSelectedId(std::clamp(point, 0, 2) + 1, juce::dontSendNotification);
    channelBox.setSelectedId(std::clamp(channel, 0, 4) + 1, juce::dontSendNotification);
    width_ = std::clamp(width, 240, 420);
    widthBox.setSelectedId(width_, juce::dontSendNotification);
    if (widthBox.getSelectedId() == 0) widthBox.setText(juce::String(width_) + " px", juce::dontSendNotification);
}
void AnalysisDock::configure(int source, int point, int channel, int width) {
    restoreControls(source, point, channel, width);
    for (auto *view : {&meters, &phase, &spectrum}) {
        view->setSource(static_cast<engine::ScopePoint>(pointBox.getSelectedId() - 1));
        view->setChannel(static_cast<SharedAnalysis::Channel>(channelBox.getSelectedId() - 1));
    }
}
void AnalysisDock::changed() {
    const juce::Component::SafePointer<AnalysisDock> alive(this);
    if (onChange) onChange();
    else configure(source(), point(), channel(), requestedWidth());
    if (alive) repaint();
}
void AnalysisDock::frame(std::shared_ptr<SharedAnalysis> broker, std::int64_t main, std::int64_t selected) {
    const auto track = source() == 1 ? selected : main;
    expandButton.setEnabled(track > 0);
    for (auto *view : {&meters, &phase, &spectrum}) view->present(broker, track);
    for (auto *view : {&meters, &phase, &spectrum}) view->frame();
}
void AnalysisDock::suspend(bool value) noexcept {
    for (auto *view : {&meters, &phase, &spectrum}) view->suspend(value);
}
void AnalysisDock::resized() {
    auto area = getLocalBounds().reduced(3);
    auto row = area.removeFromTop(24);
    sourceBox.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(1)); pointBox.setBounds(row.reduced(1));
    row = area.removeFromTop(24);
    channelBox.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(1)); widthBox.setBounds(row.reduced(1));
    row = area.removeFromTop(24);
    const int third = row.getWidth() / 3;
    freezeButton.setBounds(row.removeFromLeft(third).reduced(1));
    resetButton.setBounds(row.removeFromLeft(third).reduced(1)); expandButton.setBounds(row.reduced(1));
    // Main aggregate readouts need more space than the two other projections.
    const int unit = std::max(0, area.getHeight() - 54) / 7;
    area.removeFromTop(18); meters.setBounds(area.removeFromTop(unit * 3));
    area.removeFromTop(18); phase.setBounds(area.removeFromTop(unit * 2));
    area.removeFromTop(18); spectrum.setBounds(area);
}
void AnalysisDock::paint(juce::Graphics &g) {
    g.setColour(look::panel); g.fillRect(getLocalBounds());
    g.setColour(look::textDim); g.setFont(look::font(11.f, true));
    for (const auto &entry : {std::pair{&meters, "METERS"}, {&phase, "PHASE"}, {&spectrum, "SPECTRUM"}})
        g.drawText(entry.second, 5, entry.first->getY() - 18, std::max(0, getWidth() - 10), 18,
                   juce::Justification::centredLeft);
}
} // namespace adi::ui
