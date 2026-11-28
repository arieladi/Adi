// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_analysis.hpp"
#include "ui_colour_waveform.hpp"
#include "ui_filled_spectrum.hpp"
#include "ui_look.hpp"
#include "ui_third_octave.hpp"
#include "adi/engine/main_loudness.hpp"
#include <algorithm>
#include <cmath>

namespace adi::ui {
using SpectrumScale = SharedSpectrumView::SpectrumScale;

float SpectrumAxis::x(double hz) const noexcept {
    return static_cast<float>(std::log(hz / lowHz) / logWidth);
}

float SpectrumAxis::y(float value) const noexcept {
    if (!std::isfinite(value)) return 1.f;
    return std::clamp((static_cast<float>(topDb) - value) / static_cast<float>(topDb - bottomDb), 0.f, 1.f);
}

const SpectrumAxis &spectrumAxis(SpectrumScale scale) {
    static const SpectrumAxis wide{20., 20000., std::log(1000.), 0, -96, 24, "20", "20k"};
    static const SpectrumAxis detail{20., 20000., std::log(1000.), -18, -78, 6, "20", "20k"};
    static const SpectrumAxis extended{10., 30000., std::log(3000.), 0, -100, 10, "10", "30k"};
    static const SpectrumAxis device{10., 24000., std::log(2400.), 0, -120, 12, nullptr, nullptr};
    if (scale == SpectrumScale::Extended) return extended;
    if (scale == SpectrumScale::Device) return device;
    if (scale == SpectrumScale::Detail) return detail;
    return wide;
}

void populateSpectrumScaleMenu(juce::ComboBox &box) {
    box.addItemList({"Scale: Wide", "Scale: Detail", "Scale: Extended", "Scale: Device"}, 1);
    box.setSelectedId(1, juce::dontSendNotification);
    box.setTitle("Spectrum display scale");
    box.setTooltip("Detail uses 20 Hz to 20 kHz and -18 to -78 dBFS. "
                   "Extended uses 10 Hz to 30 kHz and 0 to -100 dBFS. "
                   "Device uses 10 Hz to 24 kHz and 0 to -120 dBFS.");
}

SpectrumScale selectedSpectrumScale(const juce::ComboBox &box) noexcept {
    const int id = box.getSelectedId();
    return id >= 1 && id <= static_cast<int>(SpectrumScale::Device) + 1
        ? static_cast<SpectrumScale>(id - 1) : SpectrumScale::Wide;
}

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
std::size_t currentThemeIndex() noexcept {
    const auto theme = std::ranges::find(kThemes, look::themeId());
    return theme == kThemes.end() ? 0 : static_cast<std::size_t>(theme - kThemes.begin());
}
// Release shared swatches during GUI shutdown, before JUCE's image leak counters run.
struct PalettePreviewCache final : private juce::DeletedAtShutdown {
    ~PalettePreviewCache() override { clearSingletonInstance(); }
    std::array<juce::Image, kThemes.size()> themed;
    std::array<juce::Image, palettes::count> stock;
    JUCE_DECLARE_SINGLETON_INLINE(PalettePreviewCache, false)
};
const juce::Image &palettePreview(std::size_t index) {
    // On the message thread, share themed default previews and fixed stock swatches across panes.
    auto &cache = *PalettePreviewCache::getInstance();
    auto &image = index == palettes::defaultIndex ? cache.themed[currentThemeIndex()] : cache.stock[index];
    if (image.isNull()) {
        constexpr int width = 96, height = 8;
        image = juce::Image(juce::Image::RGB, width, height, false);
        juce::Image::BitmapData pixels(image, juce::Image::BitmapData::writeOnly);
        for (int x = 0; x < width; ++x) {
            const auto colour = analysisPaletteColour(index, static_cast<std::uint8_t>(x * 255 / (width - 1)));
            for (int y = 0; y < height; ++y) pixels.setPixelColour(x, y, colour);
        }
    }
    return image;
}
} // namespace
juce::Colour analysisPaletteColour(std::size_t palette, std::uint8_t sample, bool reverse) noexcept {
    if (palette >= palettes::count) palette = palettes::defaultIndex;
    if (palette != palettes::defaultIndex) return juce::Colour(0xff000000u | palettes::rgb(palette, sample, reverse));
    // Keep the same quantized interpolation in each shared theme.
    static const auto original = [] {
        std::array<std::array<juce::Colour, 256>, kThemes.size()> colors;
        for (std::size_t table = 0; table < colors.size(); ++table) {
            const auto &theme = themeFor(kThemes[table]);
            const juce::Colour background(theme.display), value(theme.value), accent(theme.accent);
            for (std::size_t i = 0; i < colors[table].size(); ++i) {
                const auto strength = static_cast<float>(i) / 255.f;
                colors[table][i] = background.interpolatedWith(value, strength)
                    .interpolatedWith(accent, std::max(0.f, (strength - .7f) / .3f));
            }
        }
        return colors;
    }();
    return original[currentThemeIndex()][reverse ? 255u - sample : sample];
}
struct SharedSpectrumView::ScaledSpectrum {
    // Store unit coordinates so resizing and repainting never rewalk the bins.
    double rate = 0.;
    int size = 0;
    SpectrumScale scale = SpectrumScale::Wide;
    std::size_t count = 0, firstBin = 0;
    std::vector<float> x;
    juce::Path line, fill;

    void setFormat(SpectrumScale nextScale, double nextRate, int nextSize, std::size_t nextCount) {
        if (nextScale == scale && nextRate == rate && nextSize == size && nextCount == count) return;
        x.clear();
        if (std::isfinite(nextRate) && nextRate > 0 && nextSize > 0) {
            const auto &axis = spectrumAxis(nextScale);
            const auto hzPerBin = nextRate / nextSize;
            const auto lastHz = std::min(axis.highHz, nextRate * .5);
            for (std::size_t i = 1; i < nextCount; ++i) {
                const auto hz = hzPerBin * static_cast<double>(i);
                if (hz > lastHz) break;
                if (hz < axis.lowHz) continue;
                if (x.empty()) firstBin = i;
                x.push_back(axis.x(hz));
            }
        }
        scale = nextScale; rate = nextRate; size = nextSize; count = nextCount;
    }

    void update(std::span<const float> bins, std::span<const float> offsets) {
        line.clear();
        fill.clear();
        if (x.size() < 2) return;
        const auto &axis = spectrumAxis(scale);
        const bool filled = scale != SpectrumScale::Device;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const auto bin = firstBin + i;
            const auto y = axis.y(bins[bin] + (offsets.empty() ? 0.f : offsets[bin]));
            if (i == 0) {
                line.startNewSubPath(x[i], y);
                if (filled) fill.startNewSubPath(x[i], y);
            } else {
                line.lineTo(x[i], y);
                if (filled) fill.lineTo(x[i], y);
            }
        }
        if (filled) {
            fill.lineTo(x.back(), 1.f);
            fill.lineTo(x.front(), 1.f);
            fill.closeSubPath();
        }
    }

    void paint(juce::Graphics &g, juce::Rectangle<float> plot) const {
        if (line.isEmpty() || plot.getWidth() <= 1.f || plot.getHeight() <= 1.f) return;
        const auto transform = juce::AffineTransform::scale(plot.getWidth() - 1.f, plot.getHeight() - 1.f)
            .translated(plot.getX(), plot.getY());
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(plot.toNearestInt());
        if (!fill.isEmpty()) {
            g.setColour(look::value.withAlpha(.28f));
            g.fillPath(fill, transform);
        }
        g.setColour(look::value);
        g.strokePath(line, juce::PathStrokeType(look::line), transform);
    }
};

struct SharedSpectrumView::PhaseTrail {
    static constexpr std::size_t pairsPerFrame = 512;
    static constexpr double initialGain = .25, minimumGain = .02;
    struct Trace {
        std::array<juce::Point<float>, pairsPerFrame> points;
        std::size_t count = 0;
    };
    std::array<Trace, 4> history;
    std::size_t next = 0, count = 0;
    double gain = initialGain;

    void clear() noexcept {
        next = count = 0;
        gain = initialGain;
    }

    void push(const SharedAnalysis::Result &window, double elapsedSeconds) noexcept {
        const auto paired = std::min(window.scopeLeft.size(), window.scopeRight.size());
        if (paired < 2) { clear(); return; }
        auto &trace = history[next];
        trace.count = std::min(paired, pairsPerFrame);
        const auto first = paired - trace.count;
        float peak = 0.f;
        for (std::size_t i = 0; i < trace.count; ++i) {
            const float left = window.scopeLeft[first + i], right = window.scopeRight[first + i];
            // The channels come from one tap sample. Rotate by 45 degrees for
            // a vertical mid axis and a horizontal side axis.
            trace.points[i] = {(right - left) * .5f, -(left + right) * .5f};
            peak = std::max({peak, std::abs(left), std::abs(right)});
        }
        gain = std::max({minimumGain, static_cast<double>(peak), gain * std::exp(-elapsedSeconds / 7.1)});
        // A certified parked source may publish no more frames after silence.
        // Retire its display trail without reopening the gain interval.
        if (peak == 0.f && window.peak == std::array<float, 2>{}) {
            next = count = 0;
            return;
        }
        next = (next + 1) % history.size();
        count = std::min(count + 1, history.size());
    }

    void paint(juce::Graphics &g, juce::Rectangle<float> plot) const {
        const auto centre = plot.getCentre();
        const auto radius = std::min(plot.getWidth(), plot.getHeight()) * .5f - 1.f;
        if (radius < 8.f) return;
        g.setColour(juce::Colour(51, 66, 61));
        g.drawVerticalLine(juce::roundToInt(centre.x), centre.y - radius, centre.y + radius);
        g.drawHorizontalLine(juce::roundToInt(centre.y), centre.x - radius, centre.x + radius);
        if (radius >= 24.f) {
            g.setFont(look::font(8.f));
            g.setColour(look::textDim);
            const auto label = [&](const char *text, float x, float y) {
                g.drawText(text, juce::Rectangle<float>(x - 8.f, y - 5.f, 16.f, 10.f), juce::Justification::centred);
            };
            label("M", centre.x, centre.y - radius + 6.f);
            label("L", centre.x - radius * .66f, centre.y - radius + 14.f);
            label("R", centre.x + radius * .66f, centre.y - radius + 14.f);
            label("+S", centre.x - radius + 9.f, centre.y - 7.f);
            label("-S", centre.x + radius - 9.f, centre.y - 7.f);
        }
        const auto factor = radius * .92f / static_cast<float>(gain);
        const auto first = (next + history.size() - count) % history.size();
        constexpr std::array opacity{.52f, .312f, .1872f, .11232f};
        for (std::size_t frame = 0; frame < count; ++frame) {
            const auto &trace = history[(first + frame) % history.size()];
            const std::size_t step = trace.count >= 8 ? 4 : 1;
            juce::Path path;
            for (std::size_t i = 0; i < trace.count; i += step) {
                const auto &point = trace.points[i];
                const float x = centre.x + std::clamp(point.x * factor, -radius, radius);
                const float y = centre.y + std::clamp(point.y * factor, -radius, radius);
                if (i == 0) path.startNewSubPath(x, y);
                else path.lineTo(x, y);
            }
            g.setColour(juce::Colour(107, 240, 184).withAlpha(opacity[count - 1 - frame]));
            g.strokePath(path, juce::PathStrokeType(1.f));
        }
    }
};

struct SharedSpectrumView::BandDisplay {
    std::array<float, third_octave::centres.size()> levels{}, peaks{};
    bool ready = false;

    void clear() noexcept { ready = false; }

    void push(const SharedAnalysis::Result &window, double elapsedSeconds) noexcept {
        if (window.request.fftSize <= 0 || window.binsDbfs.empty()) { clear(); return; }
        const auto binHz = window.sampleRate / window.request.fftSize;
        for (std::size_t i = 0; i < levels.size(); ++i) {
            const auto hz = third_octave::centres[i];
            const auto tilt = spectrumDisplayOffset(SpectrumSlope::Tilt45, hz);
            levels[i] = third_octave::bandLevel(window.binsDbfs, binHz, hz) + static_cast<float>(tilt);
            peaks[i] = ready ? third_octave::holdLevel(peaks[i], levels[i], elapsedSeconds) : levels[i];
        }
        ready = true;
    }
};

struct SharedSpectrumView::Raster {
    static constexpr int width = 128, height = 256;
    static constexpr std::uint16_t gap = 256;
    std::array<std::uint16_t, static_cast<std::size_t>(width * height)> intensity{};
    std::array<juce::Colour, 256> colors{};
    juce::Image image;
    std::uint64_t revision = 0, epoch = 0, column = 0, sweepOrigin = 0;
    ThemeId theme = ThemeId::Dark;
    bool paletteDirty = true;
    Raster() { intensity.fill(gap); }
    void setPalette(std::size_t index, bool reverse) noexcept {
        theme = look::themeId();
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
    void paintSweep(juce::Graphics &g, juce::Rectangle<float> plot) const {
        if (!sweepOrigin || column < sweepOrigin) {
            g.drawImage(image, plot);
            return;
        }
        const auto head = (column - sweepOrigin) % width;
        const auto rotation = static_cast<float>((head + 1) % width) * (plot.getWidth() / width);
        const auto clip = plot.toNearestInt();
        if (clip.isEmpty()) return;
        if (rotation == 0.f) {
            g.drawImage(image, plot);
        } else {
            // Rotate the scrolling raster at draw time. Both fragments use the
            // same scale and the same published pixels as Scroll.
            const int split = std::clamp(juce::roundToInt(plot.getX() + rotation), clip.getX(), clip.getRight());
            auto right = clip;
            const auto left = right.removeFromLeft(split - clip.getX());
            const auto draw = [&](juce::Rectangle<int> limit, float offset) {
                juce::Graphics::ScopedSaveState state(g);
                if (g.reduceClipRegion(limit)) g.drawImage(image, plot.translated(offset, 0.f));
            };
            draw(left, rotation - plot.getWidth());
            draw(right, rotation);
        }
        const auto boundary = plot.getX() + static_cast<float>(head + 1) * (plot.getWidth() / width);
        const auto x = std::clamp(juce::roundToInt(boundary), clip.getX(), clip.getRight() - 1);
        g.setColour(look::textOnBand.withAlpha(.55f));
        g.drawVerticalLine(x, plot.getY(), plot.getBottom());
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
namespace {
bool isMaximumSpectrum(SharedSpectrumView::Mode mode) noexcept {
    return mode == SharedSpectrumView::Mode::SpectrumMaximum || mode == SharedSpectrumView::Mode::SpectrumCumulativeMaximum;
}
juce::Rectangle<int> filledSpectrumPlot(juce::Rectangle<int> bounds, bool compact) {
    auto plot = bounds.reduced(5).withTrimmedTop(compact ? 0 : 16);
    if (plot.getWidth() >= 120) plot = plot.withTrimmedRight(30);
    if (plot.getHeight() >= 44) plot = plot.withTrimmedBottom(14);
    return plot;
}
void describeAnalysisView(SharedSpectrumView &view, SharedSpectrumView::Mode mode) {
    if (mode == SharedSpectrumView::Mode::ClassicScope) {
        view.setTitle("Classic Scope");
        view.setTooltip("Classic Scope shows the waveform using the selected channel and trigger.");
        return;
    }
    if (mode == SharedSpectrumView::Mode::SpectrumMaximum) {
        view.setTitle("Realtime Maximum");
        view.setTooltip("Each spectrum bin rises immediately and falls by up to 20 dB per second of source audio.");
        return;
    }
    if (mode == SharedSpectrumView::Mode::SpectrumCumulativeMaximum) {
        view.setTitle("Cumulative Maximum");
        view.setTooltip("Each spectrum bin holds its highest level in this view since the analysis interval began or was cleared.");
        return;
    }
    if (mode == SharedSpectrumView::Mode::FilledSpectrum) {
        view.setTitle("Filled Spectrum");
        view.setTooltip("Frequency, note and level under the pointer. The darker fill holds the highest level in this view.");
        return;
    }
    if (mode == SharedSpectrumView::Mode::Phase) {
        view.setTitle("Phase");
        view.setTooltip("The stereo trace draws mono vertically and opposing channels horizontally. "
                        "The correlation reading is below the trace.");
        return;
    }
    if (mode == SharedSpectrumView::Mode::ThirdOctaveBands) {
        view.setTitle("Third-Octave Bands");
        view.setTooltip("Thirty third-octave bands with held peaks and a scope locked to one cycle when a pitch is detected.");
        return;
    }
    if (mode == SharedSpectrumView::Mode::ColourWaveform) {
        view.setTitle("Colour Waveform");
        view.setTooltip("Two seconds of audio scroll towards the left. Blue, amber and pale show low, middle and high bands.");
        return;
    }
    view.setTitle("Channel spectrum");
    view.setTooltip("The channel after its mixer. Full and boxed views share one Hann-window spectrum. "
               "Sine-calibrated dBFS; sample peak and RMS refer to the same contiguous window. "
               "Phase is L/R correlation, not a loudness measurement. Meters on Main/Post Mixer "
               "also show the existing programme loudness before cue/metronome: momentary, "
               "short-term and integrated LUFS, plus the held aggregate Main true peak. "
               "Freeze/Reset affect this view only, never the shared Main measurement.");
}
}
SharedSpectrumView::SharedSpectrumView(bool compact) : compact_(compact) {
    (void)analysisPaletteColour(palettes::defaultIndex, 0); // no lazy LUT initialization in paint
    describeAnalysisView(*this, mode_);
    setInterceptsMouseClicks(false, false);
}
void SharedSpectrumView::setMode(Mode mode) {
    if (mode_ == mode) return;
    if (mode == Mode::Phase) phaseTrail_ = std::make_unique<PhaseTrail>();
    else if (mode_ == Mode::Phase) phaseTrail_.reset();
    if (mode == Mode::ThirdOctaveBands) bandDisplay_ = std::make_unique<BandDisplay>();
    else if (mode_ == Mode::ThirdOctaveBands) bandDisplay_.reset();
    if (mode == Mode::SpectrumAverage || mode_ == Mode::SpectrumAverage ||
        isMaximumSpectrum(mode) || isMaximumSpectrum(mode_) ||
        mode == Mode::FilledSpectrum || mode_ == Mode::FilledSpectrum ||
        mode == Mode::ThirdOctaveBands || mode_ == Mode::ThirdOctaveBands ||
        mode == Mode::ColourWaveform || mode_ == Mode::ColourWaveform) frozen_ = false;
    lease_.reset(); resetPicture(); mainMeter_.reset(); raster_.reset(); clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    sourcePosition_ = {}; historyClearPosition_ = {}; freshRequired_ = false;
    if (mode == Mode::ClassicScope && !classicPanel_) {
        classicPanel_ = std::make_unique<ClassicScopePanel>();
        classicPanel_->setBounds(getLocalBounds());
        classicPanel_->onChange = [this](const ClassicScopeControls &controls) { setClassicControls(controls); };
        classicPanel_->onFreeze = [this](bool held) {
            freeze(held);
            if (onClassicFreeze) onClassicFreeze(held);
        };
        addChildComponent(*classicPanel_);
    }
    if (mode_ == Mode::ClassicScope || mode == Mode::ClassicScope ||
        isMaximumSpectrum(mode_) || isMaximumSpectrum(mode) ||
        mode_ == Mode::FilledSpectrum || mode == Mode::FilledSpectrum || mode_ == Mode::Phase || mode == Mode::Phase ||
        mode_ == Mode::ThirdOctaveBands || mode == Mode::ThirdOctaveBands ||
        mode_ == Mode::ColourWaveform || mode == Mode::ColourWaveform)
        describeAnalysisView(*this, mode);
    mode_ = mode;
    if (classicPanel_) {
        resetClassic(false);
        classicPanel_->setVisible(mode_ == Mode::ClassicScope);
        setInterceptsMouseClicks(false, mode_ == Mode::ClassicScope && !compact_);
    }
    updateDemand();
    repaint();
}
void SharedSpectrumView::setSource(engine::ScopePoint point, std::int64_t device) {
    if (point_ == point && device_ == device) return;
    lease_.reset(); resetPicture(); mainMeter_.reset(); raster_.reset(); clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    sourcePosition_ = {}; historyClearPosition_ = {}; freshRequired_ = false;
    point_ = point; device_ = device; resetClassic(false); updateDemand(); repaint();
}
void SharedSpectrumView::setChannel(SharedAnalysis::Channel channel) {
    if (channel_ == channel) return;
    lease_.reset(); resetPicture(); mainMeter_.reset(); raster_.reset(); clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    sourcePosition_ = {}; historyClearPosition_ = {}; freshRequired_ = false;
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
void SharedSpectrumView::setHistoryMotion(HistoryMotion motion) {
    if (historyMotion_ == motion) return;
    historyMotion_ = motion;
    if (mode_ == Mode::Spectrogram && eligible()) repaint();
}
void SharedSpectrumView::setSpectrumSlope(SpectrumSlope slope) {
    slope = spectrumSlopeFromId(spectrumSlopeId(slope));
    if (slope == SpectrumSlope::Flat) spectrumProjection_.reset();
    if (spectrumSlope_ == slope) return;
    spectrumSlope_ = slope;
    try { updateSpectrumProjection(); } catch (...) { spectrumProjection_.reset(); }
    if (spectrumScale_ != SpectrumScale::Wide) updateScaledSpectrum();
    repaint();
}
void SharedSpectrumView::setFilledSpectrumCurve(SharedAnalysis::FilledCurve curve) {
    if (curve == filledCurve_ || curve < SharedAnalysis::FilledCurve::RealtimeAverage ||
        curve > SharedAnalysis::FilledCurve::Average) return;
    filledCurve_ = curve;
    if (mode_ == Mode::FilledSpectrum) repaint();
}
void SharedSpectrumView::setSpectrumScale(SpectrumScale scale) {
    if (spectrumScale_ == scale) return;
    spectrumScale_ = scale;
    if (!spectrumMode() || !eligible()) return;
    updateScaledSpectrum();
    repaint();
}
void SharedSpectrumView::updateScaledSpectrum() noexcept {
    if (spectrumScale_ == SpectrumScale::Wide || !shown_) {
        scaledSpectrum_.reset();
        return;
    }
    const auto bins = spectrumBins();
    const bool tilted = spectrumSlope_ == SpectrumSlope::Tilt45;
    const auto offsets = tilted ? spectrumProjection_.offsets() : std::span<const float>{};
    if (bins.empty() || (tilted && offsets.size() != bins.size())) {
        scaledSpectrum_.reset();
        return;
    }
    try {
        if (!scaledSpectrum_) scaledSpectrum_ = std::make_unique<ScaledSpectrum>();
        scaledSpectrum_->setFormat(spectrumScale_, shown_->sampleRate, shown_->request.fftSize, bins.size());
        scaledSpectrum_->update(bins, offsets);
    } catch (...) {
        scaledSpectrum_.reset();
    }
}
void SharedSpectrumView::updateSpectrumProjection() {
    if (!spectrumMode() || !shown_) return;
    spectrumProjection_.prepare(spectrumSlope_, shown_->sampleRate, shown_->request.fftSize, spectrumBins().size());
}
std::span<const float> SharedSpectrumView::spectrumBins() const noexcept {
    if (mode_ == Mode::SpectrumAverage)
        return averageBins_ ? std::span<const float>(*averageBins_) : std::span<const float>{};
    if (isMaximumSpectrum(mode_))
        return maximumShown_ ? std::span<const float>(maximumShown_->binsDbfs) : std::span<const float>{};
    return mode_ == Mode::Spectrum && shown_ ? std::span<const float>(shown_->binsDbfs) : std::span<const float>{};
}
void SharedSpectrumView::resetPicture() noexcept {
    shown_.reset();
    averageBins_.reset();
    maximumShown_.reset();
    scaledSpectrum_.reset();
    averageStatus_ = SharedAnalysis::AverageStatus::Waiting;
    filledPointer_ = {-1, -1};
    filledDisplaySlot_ = -1.;
    if (phaseTrail_) phaseTrail_->clear();
    if (bandDisplay_) bandDisplay_->clear();
}
SharedSpectrumView::Status SharedSpectrumView::pictureStatus() const noexcept {
    if (averageStatus_ == SharedAnalysis::AverageStatus::InvalidInput ||
        averageStatus_ == SharedAnalysis::AverageStatus::Unavailable) return Status::Unavailable;
    return shown_ ? Status::Ready : Status::Waiting;
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
void SharedSpectrumView::setClassicControls(const ClassicScopeControls &controls) {
    if (controls == classicControls_) return;
    auto previous = classicControls_;
    previous.sync = controls.sync;
    const bool newProfile = controls != previous;
    classicControls_ = controls;
    if (mode_ != Mode::ClassicScope) return;
    if (newProfile) {
        // Keep the physical tap during a knob drag. The new profile starts
        // its own capture before the previous sweep drops its lease.
        auto previousLease = std::move(captureLease_);
        resetClassic(false);
        updateDemand();
        previousLease.reset();
    }
    presentClassic();
}
void SharedSpectrumView::freeze(bool value) {
    if (frozen_ == value) return;
    frozen_ = value;
    if (mode_ == Mode::ClassicScope) {
        if (!value) resetClassic(true);
        updateDemand();
        presentClassic();
        return;
    }
    if (value) lease_.reset();
    else {
        freshRequired_ = true; sourcePosition_ = {};
        clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
        historyClearPosition_ = {};
        resetPicture(); mainMeter_.reset(); raster_.reset(); updateDemand();
    }
    repaint();
}
void SharedSpectrumView::clear() {
    if (mode_ == Mode::ClassicScope) {
        captureShown_.reset();
        captureFresh_ = true;
        capturePosition_ = captureLease_.sourcePosition();
        updateDemand();
        presentClassic();
        return;
    }
    // Local interval only: clearing one view never seeks the tap, discards another view's
    // publication or resets audio/loudness state. Wait for a subsequent worker window.
    if (shown_) {
        clearedRevision_ = shown_->windowSerial; clearedEpoch_ = shown_->sourceEpoch;
        clearedColumn_ = shown_->historySerial;
    }
    if (mode_ == Mode::SpectrumCumulativeMaximum) {
        lease_.restartCumulativeMaximum();
    } else if (mode_ == Mode::FilledSpectrum) {
        lease_.restartFilledSpectrum();
    } else {
        armFreshBoundary();
    }
    if (mode_ == Mode::Spectrogram || mode_ == Mode::ColourWaveform) historyClearPosition_ = sourcePosition_;
    resetPicture(); mainMeter_.reset(); raster_.reset();
    status_ = lease_.available() ? Status::Waiting : Status::Detached; repaint();
}
SharedSpectrumView::~SharedSpectrumView() {
    unwatchAncestors();
    lease_.reset();
}
void SharedSpectrumView::present(std::shared_ptr<SharedAnalysis> broker, std::int64_t track) {
    if (broker_ != broker || track_ != track) {
        lease_.reset();
        resetPicture();
        mainMeter_.reset();
        raster_.reset();
        broker_ = std::move(broker);
        track_ = track;
        clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
        sourcePosition_ = {}; historyClearPosition_ = {}; freshRequired_ = false;
        status_ = Status::Detached;
        resetClassic(false);
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
    if (mode_ == Mode::ClassicScope) {
        updateClassicDemand();
        return;
    }
    const auto previous = status_;
    const bool hadMainMeter = mainMeter_.has_value();
    if (!eligible()) {
        if (shown_ || lease_) freshRequired_ = true;
        lease_.reset();
        sourcePosition_ = {};
        resetPicture();
        raster_.reset();
        mainMeter_.reset();
        status_ = Status::Detached;
    } else if (!broker_->active()) {
        lease_.reset(); resetPicture(); raster_.reset(); mainMeter_.reset();
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
            case Mode::Meters:
            case Mode::ChannelReadout: request.outputs = SharedAnalysis::Meters; break;
            case Mode::Phase: request.outputs = SharedAnalysis::Phase; break;
            case Mode::Spectrogram: request.outputs = SharedAnalysis::Spectrogram; break;
            case Mode::SpectrumAverage: request.outputs = SharedAnalysis::Average1s; break;
            case Mode::SpectrumMaximum: request.outputs = SharedAnalysis::RealtimeMaximum; break;
            case Mode::SpectrumCumulativeMaximum: request.outputs = SharedAnalysis::CumulativeMaximum; break;
            case Mode::FilledSpectrum:
                request.fftSize = 4096;
                request.outputs = SharedAnalysis::FilledSpectrum;
                break;
            case Mode::ThirdOctaveBands:
                request.fftSize = 4096;
                request.channel = SharedAnalysis::Channel::Mid;
                request.outputs = SharedAnalysis::Spectrum | SharedAnalysis::PeriodScope;
                break;
            case Mode::ColourWaveform:
                request.channel = SharedAnalysis::Channel::Mid;
                request.outputs = SharedAnalysis::ColourWaveform;
                break;
            case Mode::ClassicScope: break; // Uses the continuous capture lease above.
            }
            try {
                lease_ = broker_->acquire(request);
                sourcePosition_ = lease_.sourcePosition();
                clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
                historyClearPosition_ = mode_ == Mode::ColourWaveform && freshRequired_
                    ? sourcePosition_ : SharedAnalysis::Lease::SourcePosition{};
            }
            catch (...) { lease_.reset(); }
        }
        if (!lease_ || !lease_.available()) {
            if (shown_ || sourcePosition_.generation) freshRequired_ = true;
            sourcePosition_ = {};
            resetPicture();
            raster_.reset();
            mainMeter_.reset();
            status_ = Status::Unavailable;
        } else {
            if (!sourcePosition_.generation) sourcePosition_ = lease_.sourcePosition();
            status_ = pictureStatus();
        }
    }
    if (previous != status_ || (hadMainMeter && !mainMeter_)) repaint();
}
void SharedSpectrumView::armFreshBoundary() noexcept {
    freshRequired_ = true;
    sourcePosition_ = lease_.sourcePosition();
}
void SharedSpectrumView::resetClassic(bool fresh) noexcept {
    captureLease_.reset();
    captureShown_.reset();
    capturePosition_ = {};
    captureFresh_ = fresh;
    presentClassic();
}
void SharedSpectrumView::presentClassic() noexcept {
    if (classicPanel_) classicPanel_->present(classicControls_, frozen_, captureShown_, capturePosition_.written);
}
void SharedSpectrumView::updateClassicDemand() noexcept {
    if (!eligible()) {
        if (captureLease_) captureFresh_ = true;
        captureLease_.reset();
        if (!frozen_ && captureShown_) resetClassic(true);
        status_ = Status::Detached;
        return;
    }
    if (!broker_->active()) {
        resetClassic(true);
        status_ = Status::Unavailable;
        return;
    }
    if (frozen_) {
        captureLease_.reset();
        status_ = captureShown_ ? Status::Ready : Status::Waiting;
        return;
    }
    if (!captureLease_) {
        try {
            const auto lane = classicControls_.right ? SharedAnalysis::Channel::Right : SharedAnalysis::Channel::Left;
            captureLease_ = broker_->acquireCapture({{track_, point_, device_}, lane, classicControls_.settings()});
        } catch (...) {
            status_ = Status::Unavailable;
            return;
        }
    }
    if (!captureLease_.available()) {
        if (capturePosition_.generation) captureFresh_ = true;
        capturePosition_ = {};
        if (captureShown_) {
            captureShown_.reset();
            presentClassic();
        }
        status_ = Status::Unavailable;
        return;
    }
    // The tap's activation fence is cheap and owner-only. Re-read it only to
    // detect a replaced tap, never to chase ordinary audio writes.
    const auto position = captureLease_.sourcePosition();
    if (capturePosition_.generation != position.generation) {
        const bool replacement = capturePosition_.generation != 0;
        capturePosition_ = position;
        if (replacement) captureFresh_ = true;
        if (!captureFresh_) capturePosition_.written = 0;
        if (captureShown_) {
            captureShown_.reset();
            presentClassic();
        }
    }
    status_ = captureShown_ ? Status::Ready : Status::Waiting;
}
void SharedSpectrumView::classicFrame() noexcept {
    if (!captureLease_) return;
    broker_->requestFrame();
    updateClassicDemand(); // owner resolution can replace a tap before its next publication
    if (!captureLease_.available()) return;
    auto next = captureLease_.read();
    if (next && next->generation != capturePosition_.generation) next.reset();
    const auto hasNewColumn = [this](const auto &columns) {
        return !columns.empty() && columns.back().firstSequence >= capturePosition_.written;
    };
    if (next && captureFresh_ && !hasNewColumn(next->current) && !hasNewColumn(next->previous))
        next.reset();
    if (next == captureShown_) return;
    captureShown_ = std::move(next);
    status_ = captureShown_ ? Status::Ready : Status::Waiting;
    presentClassic();
}
void SharedSpectrumView::frame() noexcept {
    updateDemand();
    if (mode_ == Mode::ClassicScope) {
        classicFrame();
        return;
    }
    const bool updateFilled = mode_ != Mode::FilledSpectrum || updateFilledDisplay();
    if (raster_ && raster_->theme != look::themeId()) {
        raster_->setPalette(paletteIndex_, paletteReversed_);
        try { raster_->recolor(); } catch (...) { raster_.reset(); }
        repaint();
    }
    if (!lease_) return;
    broker_->requestFrame();
    updateDemand(); // owner resolution may have changed pending availability/generation
    if (!lease_.available()) return;
    const bool averaging = mode_ == Mode::SpectrumAverage;
    const bool maximumMode = isMaximumSpectrum(mode_);
    const auto average = averaging ? lease_.readAverage() : nullptr;
    const auto maximum = maximumMode ? lease_.readMaximum() : nullptr;
    const auto averageStatus = average ? average->status : SharedAnalysis::AverageStatus::Waiting;
    std::shared_ptr<const SharedAnalysis::Result> next;
    if (averaging) {
        if (averageStatus == SharedAnalysis::AverageStatus::Ready && average->binsDbfs)
            next = average->window;
    } else if (maximumMode) {
        if (maximum) next = maximum->window;
    } else {
        next = lease_.read();
    }
    // Invalidation can replace a live source before its next publication. Fence
    // at that activation, rather than accepting buffered replacement-source data.
    if (!next && shown_) armFreshBoundary();
    if (next && next->generation != sourcePosition_.generation) {
        armFreshBoundary();
        next.reset();
    }
    // The cumulative lease owns its admission fence. A held picture can
    // remain unchanged across later hops after the fence has been satisfied.
    if (next && (mode_ == Mode::SpectrumCumulativeMaximum || mode_ == Mode::FilledSpectrum))
        freshRequired_ = false;
    if (next && freshRequired_) {
        const auto size = static_cast<std::int64_t>(next->request.fftSize);
        if (!sourcePosition_.generation || size <= 0 || next->firstSequence < 0 ||
            next->firstSequence > std::numeric_limits<std::int64_t>::max() - size ||
            next->firstSequence + size <= sourcePosition_.written) next.reset();
        else freshRequired_ = false;
    }
    if (next && next->sourceEpoch == clearedEpoch_ && next->windowSerial <= clearedRevision_) next.reset();
    if (next && shown_ && next->sourceEpoch == shown_->sourceEpoch && !updateFilled) return;
    // A recreated domain may restart these counters. A discarded publication
    // cannot leave a raster that later aliases the new domain's first result.
    if (!next) raster_.reset();
    // An explicit source/transport epoch may invalidate a result without dropping the lease.
    const auto nextBins = next && average ? average->binsDbfs : nullptr;
    const auto nextMaximum = next ? maximum : nullptr;
    const bool pictureChanged = next != shown_ || nextBins != averageBins_ || nextMaximum != maximumShown_ ||
                                averageStatus != averageStatus_;
    if (pictureChanged) {
        if (phaseTrail_ || bandDisplay_) {
            const bool sameSource = next && shown_ && next->generation == shown_->generation &&
                next->sourceEpoch == shown_->sourceEpoch;
            if (!sameSource) {
                if (phaseTrail_) phaseTrail_->clear();
                if (bandDisplay_) bandDisplay_->clear();
            }
            if (next && (!sameSource || next->windowSerial != shown_->windowSerial)) {
                const auto elapsed = sameSource && next->firstSequence > shown_->firstSequence && next->sampleRate > 0.
                    ? static_cast<double>(next->firstSequence - shown_->firstSequence) / next->sampleRate : 0.;
                if (phaseTrail_) phaseTrail_->push(*next, elapsed);
                if (bandDisplay_) bandDisplay_->push(*next, elapsed);
            }
        }
        shown_ = std::move(next);
        averageBins_ = nextBins;
        maximumShown_ = nextMaximum;
        averageStatus_ = averageStatus;
        mainMeter_.reset();
        status_ = pictureStatus();
        repaint();
    }
    updateMainMeter();
    try { updateRaster(); } catch (...) { raster_.reset(); }
    try { updateSpectrumProjection(); } catch (...) { spectrumProjection_.reset(); }
    if (spectrumScale_ != SpectrumScale::Wide && pictureChanged) updateScaledSpectrum();
}
bool SharedSpectrumView::updateFilledDisplay() noexcept {
    const auto slot = std::floor(juce::Time::getMillisecondCounterHiRes() * (filled_spectrum::framesPerSecond / 1000.));
    if (slot == filledDisplaySlot_) return false;
    filledDisplaySlot_ = slot;
    juce::Point<int> pointer{-1, -1};
    if (const auto *peer = getPeer(); isShowing() && peer && peer->getNativeHandle()) {
        const auto position = getMouseXYRelative();
        if (filledSpectrumPlot(getLocalBounds(), compact_).contains(position)) pointer = position;
    }
    if (pointer != filledPointer_) {
        filledPointer_ = pointer;
        repaint();
    }
    return true;
}
void SharedSpectrumView::updateRaster() {
    if (mode_ != Mode::Spectrogram || !shown_) return;
    const bool needsColumns = !raster_ || raster_->image.isNull() ||
        raster_->revision != shown_->revision || raster_->epoch != shown_->sourceEpoch;
    if (needsColumns) {
        const auto &columns = shown_->columns;
        // Published history is ordered by serial, including across gaps.
        auto firstVisible = shown_->sourceEpoch == clearedEpoch_
            ? std::upper_bound(columns.begin(), columns.end(), clearedColumn_,
                               [](auto serial, const auto &entry) { return serial < entry.serial; })
            : columns.begin();
        const bool afterClear = historyClearPosition_.generation != 0 &&
            historyClearPosition_.generation == shown_->generation;
        if (afterClear) {
            // A publication seen after Clear can contain columns completed
            // before it. Use each column's first physical window, including
            // a group whose last window crossed the Clear boundary.
            const auto lastOldStart = historyClearPosition_.written - shown_->request.fftSize;
            firstVisible = std::upper_bound(firstVisible, columns.end(), lastOldStart,
                                           [](auto start, const auto &entry) { return start < entry.firstSequence; });
            if (firstVisible == columns.end()) { raster_.reset(); return; }
        }
        const auto skipped = static_cast<std::size_t>(firstVisible - columns.begin());
        const auto count = columns.size() - skipped;
        const auto topHz = std::min(20000., shown_->sampleRate * .5);
        if ((!count && !shown_->historySerial) || topHz <= 20) return;
        if (!raster_) {
            raster_ = std::make_unique<Raster>();
            raster_->setPalette(paletteIndex_, paletteReversed_);
        }
        auto &raster = *raster_;
        auto firstColumn = skipped;
        const bool newSweep = raster.image.isNull() || raster.epoch != shown_->sourceEpoch ||
            shown_->historySerial < raster.column;
        if (newSweep) {
            if (afterClear)
                raster.sweepOrigin = firstVisible->serial;
            else if (clearedEpoch_ == shown_->sourceEpoch && clearedColumn_ < shown_->historySerial)
                raster.sweepOrigin = clearedColumn_ + 1;
            else
                raster.sweepOrigin = count ? columns[skipped].serial : shown_->historySerial;
        }
        if (newSweep || shown_->historySerial - raster.column >= Raster::width) {
            // History scrolls and changes individual pixels on the message thread.
            // Keep those edits in memory so shifts and clears never read back a GPU surface.
            if (raster.image.isNull())
                raster.image = juce::Image(juce::Image::RGB, Raster::width, Raster::height, false,
                                           juce::SoftwareImageType{});
            raster.intensity.fill(Raster::gap); raster.image.clear(raster.image.getBounds(), look::display);
        } else {
            while (firstColumn < columns.size() && columns[firstColumn].serial <= raster.column) ++firstColumn;
            const auto added = static_cast<int>(shown_->historySerial - raster.column);
            if (added > 0) raster.shift(added);
            if (firstColumn && columns[firstColumn - 1].serial == raster.column) --firstColumn;
        }
        if (shown_->droppedHistoryColumns) {
            std::array<bool, Raster::width> retained{};
            for (auto column = skipped; column < columns.size(); ++column) {
                const auto serial = columns[column].serial;
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
        for (auto column = firstColumn; column < columns.size(); ++column) for (int row = 0; row < Raster::height; ++row) {
            const auto &entry = columns[column];
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
    if (raster_->paletteDirty) raster_->recolor();
}
void SharedSpectrumView::updateMainMeter() noexcept {
    std::optional<MainMeterSnapshot> next;
    if ((mode_ == Mode::Meters || mode_ == Mode::ChannelReadout) &&
        point_ == engine::ScopePoint::PostFader && device_ == 0 && shown_ &&
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
void SharedSpectrumView::resized() {
    if (classicPanel_) classicPanel_->setBounds(getLocalBounds());
    updateDemand();
}
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
    resetClassic(true);
    if (shown_ || lease_) freshRequired_ = true;
    sourcePosition_ = {};
    lease_.reset(); resetPicture(); mainMeter_.reset(); raster_.reset(); status_ = Status::Detached;
    unwatchAncestors();
}
void SharedSpectrumView::paintChannelReadout(juce::Graphics &g) const {
    g.setColour(look::text);
    g.setFont(look::font(look::fontCaption));
    const auto row = getLocalBounds();
    if (!shown_) {
        g.drawText(status_ == Status::Unavailable ? "Unavailable" : "Waiting for audio", row.reduced(look::border, 0),
                   juce::Justification::centredLeft, true);
        return;
    }
    const auto peak = std::max(shown_->peak[0], shown_->peak[1]);
    const double left = shown_->rms[0], right = shown_->rms[1];
    const double energy = left * left + right * right;
    const double side = energy > 0 ? .5 - left * right * shown_->correlation / energy : 0;
    const std::array values{
        peak > 0 ? juce::String(20. * std::log10(peak), 1) : juce::String("-inf"),
        energy > 0 && peak > 0 ? juce::String(20. * std::log10(peak / std::sqrt(.5 * energy)), 1) : juce::String("-"),
        juce::String(100. * std::clamp(side, 0., 1.), 0),
        juce::String(shown_->correlation, 2),
        juce::String(static_cast<juce::int64>(shown_->clippedSamples)),
        mainMeter_ ? meterNumber(mainMeter_->momentaryLufs) : juce::String("-")
    };
    for (std::size_t i = 0; i < values.size(); ++i) {
        const int x = static_cast<int>(i) * row.getWidth() / static_cast<int>(values.size());
        const int end = static_cast<int>(i + 1) * row.getWidth() / static_cast<int>(values.size());
        g.drawText(values[i], row.withX(x).withWidth(end - x).reduced(look::border, 0), juce::Justification::centredLeft, true);
    }
}
juce::Rectangle<float> paintSpectrumGrid(juce::Graphics &g, juce::Rectangle<float> area, SpectrumScale scale) {
    const auto &axis = spectrumAxis(scale);
    const auto scaleWidth = look::fontCaption * 3.f;
    const auto stepHeight = area.getHeight() / static_cast<float>((axis.topDb - axis.bottomDb) / axis.divisionDb);
    const bool labels = area.getWidth() >= scaleWidth * 4.f && stepHeight > look::fontCaption;
    const bool topRuler = scale == SpectrumScale::Device;
    const auto plot = labels ? (topRuler ? area.withTrimmedLeft(scaleWidth).withTrimmedTop(look::fontCaption)
                                         : area.withTrimmedRight(scaleWidth).withTrimmedBottom(look::fontCaption))
                             : area;
    if (plot.getWidth() <= 1.f || plot.getHeight() <= 1.f) return {};
    const auto xAt = [&](double hz) { return plot.getX() + (plot.getWidth() - 1.f) * axis.x(hz); };
    g.setFont(look::font(look::fontCaption));
    for (int levelDb = axis.topDb; levelDb >= axis.bottomDb; levelDb -= axis.divisionDb) {
        const auto y = plot.getY() + (plot.getHeight() - 1.f) * axis.y(static_cast<float>(levelDb));
        g.setColour(look::outline);
        g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
        if (labels) {
            g.setColour(look::textOnBand);
            const auto x = topRuler ? area.getX() : plot.getRight();
            g.drawText(juce::String(levelDb), juce::Rectangle<float>(x, y - look::fontCaption * .5f,
                       scaleWidth, look::fontCaption),
                       topRuler ? juce::Justification::centredLeft : juce::Justification::centredRight);
        }
    }
    const auto tick = [&](double hz, const char *label) {
        const auto x = xAt(hz);
        g.setColour(look::outline);
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
        if (labels && label) {
            const auto left = std::clamp(x - scaleWidth * .5f, plot.getX(), plot.getRight() - scaleWidth);
            g.setColour(look::textOnBand);
            const auto rulerY = topRuler ? area.getY() : plot.getBottom();
            g.drawText(label, juce::Rectangle<float>(left, rulerY, scaleWidth, look::fontCaption),
                       juce::Justification::centred);
        }
    };
    tick(axis.lowHz, axis.lowLabel);
    tick(100., "100");
    tick(1000., "1k");
    tick(10000., (topRuler || xAt(axis.highHz) - xAt(10000.) >= scaleWidth) ? "10k" : nullptr);
    tick(axis.highHz, axis.highLabel);
    return plot;
}

void SharedSpectrumView::paintFilledSpectrum(juce::Graphics &g) const {
    using namespace filled_spectrum;
    g.fillAll(look::display);
    const auto bounds = getLocalBounds();
    if (!compact_) {
        const auto title = bounds.reduced(5).withHeight(14);
        g.setFont(look::font(10.f, true));
        g.setColour(look::textOnBand);
        g.drawText(frozen_ ? "Filled Spectrum / Frozen" : "Filled Spectrum", title, juce::Justification::centredLeft);
    }
    const auto axes = filledSpectrumPlot(bounds, compact_);
    if (axes.getWidth() < 2 || axes.getHeight() < 2) return;
    const auto plot = axes.toFloat();
    const auto xAt = [&](double hz) {
        return plot.getX() + (plot.getWidth() - 1.f) * static_cast<float>(std::log(hz / lowHz) / logWidth());
    };
    const auto yAt = [&](float levelDb) {
        return plot.getY() + (plot.getHeight() - 1.f) * std::clamp((topDb - levelDb) / (topDb - bottomDb), 0.f, 1.f);
    };
    const auto gradient = [&](juce::Colour top, juce::Colour bottom) {
        return juce::ColourGradient(top, 0.f, plot.getY(), bottom, 0.f, plot.getBottom(), false);
    };
    g.setGradientFill(gradient(juce::Colour(38, 38, 38), juce::Colour(18, 18, 18)));
    g.fillRect(plot);
    g.setFont(look::font(9.f));
    const auto labelColour = look::textOnBand;
    const bool rightRuler = bounds.getWidth() >= 130;
    const bool bottomRuler = bounds.getHeight() >= 70;
    const bool denseScale = axes.getHeight() >= 136;
    for (const int tick : {-13, -18, -24, -30, -36, -42, -48, -54, -60, -66, -72, -78, -80}) {
        const bool endpoint = tick == -13 || tick == -80;
        if (!denseScale && !endpoint && tick % 12 != 0) continue;
        const auto y = juce::roundToInt(yAt(static_cast<float>(tick)));
        g.setColour(juce::Colour(74, 74, 74));
        g.drawHorizontalLine(y, plot.getX(), plot.getRight());
        if (rightRuler && (endpoint || (y - axes.getY() >= 10 && axes.getBottom() - y >= 10))) {
            g.setColour(labelColour);
            g.drawText(juce::String(tick), axes.withX(axes.getRight()).withWidth(28).withY(y - 5).withHeight(10),
                       juce::Justification::centredRight);
        }
    }
    struct FrequencyTick { double hz; const char *label; };
    constexpr std::array frequencies{FrequencyTick{10., "10"}, FrequencyTick{20., nullptr}, FrequencyTick{50., nullptr},
        FrequencyTick{100., "100"}, FrequencyTick{200., nullptr}, FrequencyTick{500., nullptr}, FrequencyTick{1000., "1k"},
        FrequencyTick{2000., nullptr}, FrequencyTick{5000., nullptr}, FrequencyTick{10000., "10k"}, FrequencyTick{20000., "20k"}};
    for (const auto &tick : frequencies) {
        const auto x = juce::roundToInt(xAt(tick.hz));
        g.setColour(tick.label ? juce::Colour(74, 74, 74) : juce::Colour(56, 56, 56));
        g.drawVerticalLine(x, plot.getY(), plot.getBottom());
        if (bottomRuler && tick.label && (tick.hz != 10000. || axes.getRight() - x >= 31)) {
            const auto tickWidth = std::min(30, axes.getWidth());
            const auto labelX = std::clamp(x - tickWidth / 2, axes.getX(), axes.getRight() - tickWidth);
            g.setColour(labelColour);
            g.drawText(tick.label, juce::Rectangle<int>{labelX, axes.getBottom(), tickWidth, 14},
                       juce::Justification::centred);
        }
    }
    if (shown_ && shown_->filledSpectrum && shown_->request.fftSize > 0) {
        juce::Graphics::ScopedSaveState clip(g);
        if (!g.reduceClipRegion(axes)) return;
        const auto binHz = shown_->sampleRate / shown_->request.fftSize;
        const auto &spectra = shown_->filledSpectrum->magnitudes;
        const auto frequencyStep = std::exp(logWidth() / static_cast<double>(axes.getWidth()));
        const auto drawTrace = [&](SharedAnalysis::FilledCurve curve, juce::Colour edge, bool held) {
            const auto &bins = spectra[static_cast<std::size_t>(curve)];
            if (bins.empty()) return;
            juce::Path trace;
            auto low = lowHz;
            float lastY = plot.getBottom();
            for (int column = 0; column < axes.getWidth(); ++column) {
                const auto upper = std::min(highHz, low * frequencyStep);
                const auto magnitude = columnMagnitude(bins, binHz, low, upper);
                const auto offset = spectrumDisplayOffset(SpectrumSlope::Tilt45, std::sqrt(low * upper));
                const auto levelDb = dsp::toDbfs(magnitude) + static_cast<float>(offset);
                lastY = yAt(levelDb);
                const auto x = plot.getX() + static_cast<float>(column) + .5f;
                if (column == 0) trace.startNewSubPath(plot.getX(), lastY);
                trace.lineTo(x, lastY);
                low = upper;
            }
            trace.lineTo(plot.getRight(), lastY);
            auto fill = trace;
            fill.lineTo(plot.getRight(), plot.getBottom());
            fill.lineTo(plot.getX(), plot.getBottom());
            fill.closeSubPath();
            if (held) g.setColour(juce::Colour(60, 74, 29));
            else g.setGradientFill(gradient(juce::Colour(135, 170, 59), juce::Colour(89, 111, 40)));
            g.fillPath(fill);
            g.setColour(edge);
            g.strokePath(trace, juce::PathStrokeType(held ? 1.f : 1.4f));
        };
        drawTrace(SharedAnalysis::FilledCurve::HeldMaximum, juce::Colour(95, 119, 41), true);
        drawTrace(filledCurve_, juce::Colour(159, 202, 67), false);
    } else if (!compact_) {
        g.setColour(juce::Colour(178, 178, 178));
        const char *message = status_ == Status::Unavailable ? "Analysis source unavailable"
                            : status_ == Status::Waiting ? "Waiting for audio" : "Analysis inactive";
        g.drawFittedText(message, axes.reduced(6), juce::Justification::centred, 2);
    }
    if (axes.contains(filledPointer_) && axes.getHeight() >= 15) {
        const auto hover = cursorAt(static_cast<double>(filledPointer_.x - axes.getX()) / (axes.getWidth() - 1),
                                    static_cast<double>(filledPointer_.y - axes.getY()) / (axes.getHeight() - 1));
        g.setColour(juce::Colour(216, 216, 216).withAlpha(.35f));
        g.drawVerticalLine(filledPointer_.x, plot.getY(), plot.getBottom());
        g.drawHorizontalLine(filledPointer_.y, plot.getX(), plot.getRight());
        const auto tooltipWidth = std::min(172, axes.getWidth());
        const auto tooltipX = std::clamp(filledPointer_.x + 5, axes.getX(), axes.getRight() - tooltipWidth);
        const auto tooltipY = std::min(filledPointer_.y + 7, axes.getBottom() - 15);
        const juce::Rectangle<int> tooltip{tooltipX, tooltipY, tooltipWidth, 15};
        g.setColour(juce::Colour(27, 27, 27).withAlpha(.9f));
        g.fillRect(tooltip);
        g.setColour(juce::Colour(237, 237, 237));
        const auto hz = hover.hz < 1000. ? juce::String(hover.hz, 1) + " Hz" : juce::String(hover.hz / 1000., 2) + " kHz";
        g.drawText(hz + "  " + hover.note + juce::String(hover.octave) + (hover.cents >= 0 ? " +" : " ") +
                       juce::String(hover.cents) + "c  " + juce::String(hover.levelDb, 1) + " dB",
                   tooltip.reduced(3, 0), juce::Justification::centredLeft);
    }
}

void SharedSpectrumView::paintThirdOctaveBands(juce::Graphics &g) const {
    using namespace third_octave;
    g.fillAll(look::display);
    const auto body = getLocalBounds().reduced(5);
    g.setFont(look::font(10.f, true));
    g.setColour(look::textOnBand);
    g.drawText(frozen_ ? "Third-Octave Bands / Frozen" : "Third-Octave Bands", body.withHeight(14),
               juce::Justification::centredLeft);
    const auto axes = body.withTrimmedTop(16).toFloat();
    if (axes.getWidth() < 30.f || axes.getHeight() < 34.f) return;
    g.setColour(juce::Colour(26, 28, 27));
    g.fillRect(axes);
    const auto reserve = axes.getWidth() >= 144.f ? std::min(110.f, axes.getWidth() / 3.f) : 0.f;
    const auto gap = reserve > 0.f ? 8.f : 0.f;
    const auto barsW = std::min(17.f * static_cast<float>(centres.size()), axes.getWidth() - reserve - gap);
    const auto columnW = barsW / static_cast<float>(centres.size());
    const auto segmentWidth = std::max(1.f, columnW - 3.f);
    const auto barsBottom = axes.getBottom() - 17.f;
    const auto segments = static_cast<int>((axes.getHeight() - 19.f) / 4.f);
    const auto segmentFor = [segments](float value) {
        const auto fraction = std::clamp((value - filled_spectrum::bottomDb) /
            (filled_spectrum::topDb - filled_spectrum::bottomDb), 0.f, 1.f);
        return juce::roundToInt(fraction * static_cast<float>(segments));
    };
    const bool measured = bandDisplay_ && bandDisplay_->ready;
    g.setFont(look::font(8.f));
    for (std::size_t band = 0; band < centres.size(); ++band) {
        const int lit = measured ? segmentFor(bandDisplay_->levels[band]) : 0;
        const int held = measured ? segmentFor(bandDisplay_->peaks[band]) - 1 : -1;
        const auto cellX = axes.getX() + static_cast<float>(band) * columnW;
        const auto barX = cellX + (columnW - segmentWidth) * .5f;
        for (int segment = 0; segment < segments; ++segment) {
            const auto y = barsBottom - static_cast<float>(segment + 1) * 4.f;
            g.setColour(segment == held ? juce::Colour(233, 230, 62) :
                segment < lit ? juce::Colour(138, 218, 206) : juce::Colour(37, 69, 50));
            g.fillRect(barX, y, segmentWidth, 3.f);
        }
        if (columnW >= 7.f || band % 6 == 0 || band + 1 == centres.size()) {
            const auto row = band % 2 ? 8.f : 0.f;
            const auto labelWidth = std::min(columnW * 2.f, barsW);
            const auto labelX = std::clamp(cellX + columnW * .5f - labelWidth * .5f,
                                           axes.getX(), axes.getX() + barsW - labelWidth);
            g.setColour(juce::Colour(157, 157, 157));
            g.drawText(labels[band], juce::Rectangle<float>(labelX, axes.getBottom() - 17.f + row, labelWidth, 9.f),
                       juce::Justification::centred);
        }
    }
    const auto scope = axes.withTrimmedLeft(barsW + gap);
    if (scope.getWidth() < 30.f) return;
    const auto midY = scope.getCentreY();
    const auto verticalScale = scope.getHeight() * .42f;
    g.setColour(juce::Colour(69, 69, 69));
    for (const float fraction : {.25f, .5f, .75f})
        g.drawVerticalLine(juce::roundToInt(scope.getX() + scope.getWidth() * fraction), scope.getY(), scope.getBottom());
    for (const float amplitude : {-.5f, .5f})
        g.drawHorizontalLine(juce::roundToInt(midY - amplitude * verticalScale), scope.getX(), scope.getRight());
    g.setColour(juce::Colour(122, 122, 122));
    g.drawHorizontalLine(juce::roundToInt(midY), scope.getX(), scope.getRight());
    if (!shown_ || !shown_->periodScope) return;
    const auto &period = *shown_->periodScope;
    const auto lastSample = static_cast<double>(period.samples.size() - 1);
    const bool locked = period.lag > 0. && period.start >= 0. && period.start + period.lag <= lastSample;
    const double start = locked ? period.start : 0., length = locked ? period.lag : lastSample;
    const auto points = std::min(juce::roundToInt(scope.getWidth()) - 1, static_cast<int>(lastSample));
    juce::Path trace;
    for (int step = 0; step <= points; ++step) {
        const auto fraction = static_cast<double>(step) / points;
        const auto at = std::min(lastSample, start + length * fraction);
        const auto before = static_cast<std::size_t>(at);
        const auto after = std::min(before + 1, period.samples.size() - 1);
        const auto amplitude = std::lerp(period.samples[before], period.samples[after],
                                         static_cast<float>(at - static_cast<double>(before)));
        const auto x = scope.getX() + (scope.getWidth() - 1.f) * static_cast<float>(fraction);
        const auto y = midY - verticalScale * std::clamp(amplitude, -1.f, 1.f);
        if (step == 0) trace.startNewSubPath(x, y);
        else trace.lineTo(x, y);
    }
    auto fill = trace;
    fill.lineTo(scope.getRight() - 1.f, midY);
    fill.lineTo(scope.getX(), midY);
    fill.closeSubPath();
    g.setColour(juce::Colour(135, 170, 59).withAlpha(.3f));
    g.fillPath(fill);
    g.setColour(juce::Colour(159, 202, 67));
    g.strokePath(trace, juce::PathStrokeType(1.f));
    g.setFont(look::font(9.f, true));
    g.setColour(juce::Colour(237, 237, 237));
    const auto readout = scope.withTrimmedLeft(4.f).withTrimmedTop(2.f).withHeight(12.f);
    if (locked) {
        const auto hz = dsp::kPeriodScopeSampleRate / period.lag;
        const auto note = filled_spectrum::noteAt(hz);
        g.drawText(juce::String(hz, 1) + " Hz  " + note.name + juce::String(note.octave) +
                       (note.cents >= 0 ? " +" : " ") + juce::String(note.cents) + "c",
                   readout, juce::Justification::centredLeft);
    } else {
        g.drawText("No pitch", readout, juce::Justification::centredLeft);
    }
}

void SharedSpectrumView::paintColourWaveform(juce::Graphics &g) const {
    using namespace colour_waveform;
    g.fillAll(look::display);
    const auto body = getLocalBounds().reduced(5);
    g.setFont(look::font(10.f, true));
    g.setColour(look::textOnBand);
    g.drawText(frozen_ ? "Colour Waveform / Frozen" : "Colour Waveform", body.withHeight(14),
               juce::Justification::centredLeft);
    const auto plot = body.withTrimmedTop(16).withTrimmedBottom(14).toFloat();
    if (plot.getWidth() < 2.f || plot.getHeight() < 2.f) return;
    const auto midY = plot.getCentreY();
    const auto verticalScale = plot.getHeight() * .36f;
    const auto xAt = [&](double slot) { return plot.getX() + columnX(slot, plot.getWidth()); };
    g.setColour(juce::Colour(26, 28, 27));
    g.fillRect(plot);
    g.setColour(juce::Colour(50, 54, 52));
    for (const float fraction : {-.75f, -.5f, -.25f, .25f, .5f, .75f})
        g.drawHorizontalLine(juce::roundToInt(midY - fraction * verticalScale), plot.getX(), plot.getRight());
    g.setColour(juce::Colour(71, 77, 74));
    g.drawHorizontalLine(juce::roundToInt(midY), plot.getX(), plot.getRight());
    g.setFont(look::font(9.f));
    constexpr std::array ticks{"-2 s", "-1.5 s", "-1 s", "-0.5 s", "Now"};
    for (std::size_t index = 0; index < ticks.size(); ++index) {
        const auto slot = static_cast<double>(index) * dsp::kColourWaveformColumnsPerSecond * .5;
        const auto x = std::min(xAt(slot), plot.getRight() - 1.f);
        g.setColour(juce::Colour(71, 77, 74));
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
        if (plot.getWidth() >= 160.f || index % 2 == 0) {
            const auto width = std::min(40.f, plot.getWidth());
            const auto labelX = std::clamp(x - width * .5f, plot.getX(), plot.getRight() - width);
            g.setColour(look::textOnBand);
            g.drawText(ticks[index], juce::Rectangle<float>(labelX, plot.getBottom(), width, 14.f),
                       juce::Justification::centred);
        }
    }
    if (!shown_ || !shown_->colourWaveform) return;
    const auto &waveform = *shown_->colourWaveform;
    const auto first = historyClearPosition_.generation == shown_->generation && historyClearPosition_.generation != 0
        ? firstVisibleColumn(waveform, historyClearPosition_.written, shown_->sampleRate) : 0;
    if (first >= waveform.size) return;
    const auto blankSlots = static_cast<double>(dsp::kColourWaveformColumns - waveform.size);
    std::array<std::array<float, 3>, dsp::kColourWaveformColumns> edges;
    for (auto column = first; column < waveform.size; ++column) {
        const auto &record = waveform.columns[column];
        edges[column] = bandEdges(record.envelope, record.bands);
    }
    // Back-to-front cumulative fills meet without gaps and leave exactly
    // each band's share of the height visible on both sides of the centre.
    const std::array colours{juce::Colour(46, 115, 219), juce::Colour(230, 152, 41), juce::Colour(224, 230, 238)};
    const auto firstX = xAt(blankSlots + static_cast<double>(first));
    juce::Graphics::ScopedSaveState clip(g);
    if (!g.reduceClipRegion(plot.toNearestInt())) return;
    for (std::size_t band = colours.size(); band-- > 0;) {
        const auto yAt = [&](std::size_t col) { return verticalScale * edges[col][band]; };
        juce::Path shape;
        shape.startNewSubPath(firstX, midY - yAt(first));
        for (auto col = first; col < waveform.size; ++col)
            shape.lineTo(xAt(blankSlots + static_cast<double>(col) + .5), midY - yAt(col));
        shape.lineTo(plot.getRight(), midY - yAt(waveform.size - 1));
        shape.lineTo(plot.getRight(), midY + yAt(waveform.size - 1));
        for (auto col = waveform.size; col-- > first;)
            shape.lineTo(xAt(blankSlots + static_cast<double>(col) + .5), midY + yAt(col));
        shape.lineTo(firstX, midY + yAt(first));
        shape.closeSubPath();
        g.setColour(colours[band]);
        g.fillPath(shape);
    }
}

void SharedSpectrumView::paint(juce::Graphics &g) {
    if (mode_ == Mode::ChannelReadout) { paintChannelReadout(g); return; }
    if (mode_ == Mode::FilledSpectrum) { paintFilledSpectrum(g); return; }
    if (mode_ == Mode::ThirdOctaveBands) { paintThirdOctaveBands(g); return; }
    if (mode_ == Mode::ColourWaveform) { paintColourWaveform(g); return; }
    g.fillAll(look::display);
    if (mode_ == Mode::ClassicScope) return;
    const auto bounds = getLocalBounds().toFloat();
    if (compact_) {
        if (bounds.isEmpty()) return;
        if (mode_ == Mode::Spectrogram) {
            if (raster_ && !raster_->image.isNull()) {
                g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
                g.drawImage(raster_->image, bounds);
            }
        } else if (spectrumMode()) {
            const auto grid = paintSpectrumGrid(g, bounds, spectrumScale_);
            if (shown_ && scaledSpectrum_) scaledSpectrum_->paint(g, grid);
        }
        return;
    }
    const auto area = bounds.reduced(5.f);
    if (area.isEmpty()) return;
    g.setColour(look::textOnBand);
    g.setFont(look::font(10.f, true));
    const char *names[]{"Spectrum", "Scope", "Sample peak / RMS", "Phase / L-R correlation", "Spectrogram",
                        "Channel analysis", "Spectrum / 1 s average", "Classic Scope", "Realtime Maximum", "Cumulative Maximum"};
    const char *points[]{"Post Mixer", "Post FX", "Pre FX", "Device In", "Device Out"};
    g.drawText(juce::String(names[static_cast<int>(mode_)]) + " / " + points[static_cast<int>(point_)] +
                   (frozen_ ? " / Frozen" : ""), area.withHeight(14.f), juce::Justification::centredLeft, true);
    const bool mainMeter = mode_ == Mode::Meters && mainMeter_.has_value();
    const auto meterHeight = mainMeter ? 38.f : 0.f;
    auto plot = area.withTrimmedTop(16.f).withTrimmedBottom(29.f + meterHeight).withTrimmedRight(18.f);
    if (plot.getHeight() <= 4 || plot.getWidth() <= 4) return;
    g.setFont(look::font(9.f));
    const bool scaled = spectrumMode() && spectrumScale_ != SpectrumScale::Wide;
    const auto scaledPlot = scaled ? paintSpectrumGrid(g, plot, spectrumScale_) : juce::Rectangle<float>{};
    if ((spectrumMode() && !scaled) || mode_ == Mode::Meters) for (int step = 0; step <= 4; ++step) {
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
    } else if (mode_ == Mode::Phase && phaseTrail_) {
        phaseTrail_->paint(g, plot);
    } else if (mode_ == Mode::Spectrogram && raster_ && !raster_->image.isNull()) {
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        if (historyMotion_ == HistoryMotion::Sweep) raster_->paintSweep(g, plot);
        else g.drawImage(raster_->image, plot);
    }
    const double topHz = std::min(20000.0, shown_->sampleRate * .5);
    if (topHz <= 20) return;
    const auto xFor = [&](double hz) {
        return plot.getX() + plot.getWidth() * static_cast<float>(std::log(hz / 20.0) / std::log(topHz / 20.0));
    };
    if (spectrumMode() && !scaled) for (const double hz : {100., 1000., 10000.}) {
        if (hz > topHz) continue;
        const auto x = xFor(hz);
        g.setColour(look::outline);
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
    }
    juce::Path curve;
    bool started = false;
    const auto offsets = spectrumProjection_.offsets();
    const bool tilted = spectrumSlope_ == SpectrumSlope::Tilt45;
    const auto bins = spectrumBins();
    const bool projectionReady = !tilted || offsets.size() == bins.size();
    if (spectrumMode() && !scaled && projectionReady) for (std::size_t i = 1; i < bins.size(); ++i) {
        const double hz = static_cast<double>(i) * shown_->sampleRate / shown_->request.fftSize;
        if (hz < 20 || hz > topHz) continue;
        auto value = std::isfinite(bins[i]) ? bins[i] : floorDb;
        if (tilted) value += offsets[i];
        const auto x = xFor(hz), y = heightFor(value, plot);
        if (!started) { curve.startNewSubPath(x, y); started = true; }
        else curve.lineTo(x, y);
    }
    if (scaled) {
        if (scaledSpectrum_) scaledSpectrum_->paint(g, scaledPlot);
    } else {
        g.setColour(look::value);
        g.strokePath(curve, juce::PathStrokeType(1.4f));
    }
    if (spectrumMode() && tilted) {
        g.setFont(look::font(9.f));
        g.drawText(projectionReady ? "Display +4.5 dB/oct @ 1 kHz" : "Spectrum display unavailable",
                   (spectrumScale_ == SpectrumScale::Device ? area : plot).withHeight(12.f),
                   juce::Justification::topRight, true);
    }
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
    modeBox.addItem("Spectrum 1 s average", static_cast<int>(SharedSpectrumView::Mode::SpectrumAverage) + 1);
    modeBox.addItem("Realtime Maximum", static_cast<int>(SharedSpectrumView::Mode::SpectrumMaximum) + 1);
    modeBox.addItem("Cumulative Maximum", static_cast<int>(SharedSpectrumView::Mode::SpectrumCumulativeMaximum) + 1);
    modeBox.addItem("Classic Scope", static_cast<int>(SharedSpectrumView::Mode::ClassicScope) + 1);
    modeBox.addItem("Filled Spectrum", static_cast<int>(SharedSpectrumView::Mode::FilledSpectrum) + 1);
    modeBox.addItem("Third-Octave Bands", static_cast<int>(SharedSpectrumView::Mode::ThirdOctaveBands) + 1);
    modeBox.addItem("Colour Waveform", static_cast<int>(SharedSpectrumView::Mode::ColourWaveform) + 1);
    sourceBox.addItemList({"Main", "Selected"}, 1);
    pointBox.addItemList({"Post Mixer", "Post FX", "Pre FX"}, 1);
    channelBox.addItemList({"Mid", "Side", "Left", "Right", "Stereo Power"}, 1);
    for (auto *box : {&modeBox, &sourceBox, &pointBox, &channelBox}) {
        box->setSelectedId(1, juce::dontSendNotification); addAndMakeVisible(*box);
    }
    modeBox.setTitle("Analysis mode"); sourceBox.setTitle("Analysis channel source");
    pointBox.setTitle("Analysis tap point"); channelBox.setTitle("Spectrum channel");
    paletteBox.setTitle("Spectrogram palette"); reverseButton.setTitle("Reverse spectrogram palette");
    historyBox.addItemList({"History: Scroll", "History: Sweep"}, 1);
    historyBox.setTitle("Spectrogram history motion");
    historyBox.setTooltip("Scroll keeps new columns at the right. Sweep writes in place and wraps at the edge.");
    historyBox.onChange = [this] {
        using Motion = SharedSpectrumView::HistoryMotion;
        setHistoryMotion(historyBox.getSelectedId() == 2 ? Motion::Sweep : Motion::Scroll);
    };
    setHistoryMotion(SharedSpectrumView::HistoryMotion::Scroll);
    addAndMakeVisible(historyBox);
    slopeBox.addItemList({"Slope: Flat", "+4.5 dB/oct @ 1k"}, 1);
    slopeBox.setTitle("Spectrum display slope");
    slopeBox.setTooltip("Display tilt around 1 kHz only. Raw sine-calibrated dBFS, peak/RMS and comparison bins are unchanged.");
    slopeBox.onChange = [this] {
        if (onSpectrumSlopeChange) onSpectrumSlopeChange();
        else setSpectrumSlope(requestedSpectrumSlope());
    };
    restoreSlopeControls(SpectrumSlope::Flat); addAndMakeVisible(slopeBox);
    populateSpectrumScaleMenu(scaleBox);
    scaleBox.onChange = [this] { setSpectrumScale(selectedSpectrumScale(scaleBox)); };
    addAndMakeVisible(scaleBox);
    palettePreviewTheme_ = look::themeId();
    for (std::size_t i = 0; i < palettes::count; ++i) {
        const auto name = palettes::id(i);
        paletteBox.getRootMenu()->addItem(static_cast<int>(i) + 1,
            i == 0 ? juce::String("ADI (default)") : juce::String::fromUTF8(name.data(), static_cast<int>(name.size())),
            true, false, palettePreview(i));
    }
    const auto paletteChanged = [this] {
        if (onPaletteChange) onPaletteChange();
        else setPalette(requestedPalette(), reverseButton.getToggleState());
    };
    paletteBox.onChange = paletteChanged; reverseButton.onClick = paletteChanged;
    addAndMakeVisible(paletteBox); addAndMakeVisible(reverseButton);
    setPalette("adi");
    modeBox.onChange = [this] {
        if (const auto id = modeBox.getSelectedId(); id > 0) setMode(static_cast<SharedSpectrumView::Mode>(id - 1));
    };
    sourceBox.onChange = [this] { source_ = sourceBox.getSelectedId() == 2 ? Source::Selected : Source::Main; };
    pointBox.onChange = [this] { setPoint(static_cast<engine::ScopePoint>(pointBox.getSelectedId() - 1)); };
    channelBox.onChange = [this] { view.setChannel(static_cast<SharedAnalysis::Channel>(channelBox.getSelectedId() - 1)); };
    freezeButton.setClickingTogglesState(true);
    freezeButton.onClick = [this] { view.freeze(freezeButton.getToggleState()); };
    view.onClassicFreeze = [this](bool held) { freezeButton.setToggleState(held, juce::dontSendNotification); };
    freezeButton.setTooltip("Freeze this picture and release its live analysis demand. Resume waits for fresh audio.");
    resetButton.onClick = [this] { view.clear(); };
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
    freezeButton.setToggleState(view.frozen(), juce::dontSendNotification);
    const bool spectral = mode == SharedSpectrumView::Mode::Spectrum || mode == SharedSpectrumView::Mode::SpectrumAverage ||
                          isMaximumSpectrum(mode);
    const bool filled = mode == SharedSpectrumView::Mode::FilledSpectrum;
    resetButton.setTooltip(mode == SharedSpectrumView::Mode::SpectrumCumulativeMaximum || filled
        ? "Clear this view and restart its held maximum."
        : "Clear this view's display interval; other analyzer views and audio are unchanged.");
    channelBox.setEnabled(spectral || filled || mode == SharedSpectrumView::Mode::Spectrogram);
    const bool spectrogram = mode == SharedSpectrumView::Mode::Spectrogram;
    historyBox.setVisible(spectrogram);
    paletteBox.setVisible(spectrogram);
    paletteBox.setEnabled(spectrogram);
    reverseButton.setVisible(spectrogram);
    reverseButton.setEnabled(spectrogram);
    slopeBox.setVisible(spectral);
    slopeBox.setEnabled(spectral);
    scaleBox.setVisible(spectral);
    const bool classic = mode == SharedSpectrumView::Mode::ClassicScope;
    channelBox.setVisible(!classic && mode != SharedSpectrumView::Mode::ThirdOctaveBands &&
                          mode != SharedSpectrumView::Mode::ColourWaveform);
    freezeButton.setVisible(!classic);
    resized();
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
SpectrumSlope AnalysisPane::requestedSpectrumSlope() const noexcept {
    return slopeBox.getSelectedId() == 2 ? SpectrumSlope::Tilt45 : SpectrumSlope::Flat;
}
void AnalysisPane::restoreSlopeControls(SpectrumSlope slope) {
    slopeBox.setSelectedId(slope == SpectrumSlope::Tilt45 ? 2 : 1, juce::dontSendNotification);
}
void AnalysisPane::setSpectrumSlope(SpectrumSlope slope) {
    view.setSpectrumSlope(slope); restoreSlopeControls(view.spectrumSlope());
}
void AnalysisPane::setSpectrumScale(SharedSpectrumView::SpectrumScale scale) {
    view.setSpectrumScale(scale);
    scaleBox.setSelectedId(static_cast<int>(scale) + 1, juce::dontSendNotification);
}
void AnalysisPane::setHistoryMotion(SharedSpectrumView::HistoryMotion motion) {
    view.setHistoryMotion(motion);
    historyBox.setSelectedId(motion == SharedSpectrumView::HistoryMotion::Sweep ? 2 : 1, juce::dontSendNotification);
}
void AnalysisPane::frame(std::shared_ptr<SharedAnalysis> broker, std::int64_t main, std::int64_t selected) {
    if (const auto theme = look::themeId(); theme != palettePreviewTheme_) {
        palettePreviewTheme_ = theme;
        for (juce::PopupMenu::MenuItemIterator item(*paletteBox.getRootMenu()); item.next();) {
            if (item.getItem().itemID != static_cast<int>(palettes::defaultIndex) + 1) continue;
            if (auto *preview = dynamic_cast<juce::DrawableImage *>(item.getItem().image.get()))
                preview->setImage(palettePreview(palettes::defaultIndex));
            break;
        }
    }
    view.present(std::move(broker), source_ == Source::Selected ? selected : main); view.frame();
}
void AnalysisPane::usePageControls() {
    modeBox.setVisible(false);
    sourceBox.setVisible(false);
    resized();
}
void AnalysisPane::resized() {
    const std::array<juce::Component *, 12> controls{&modeBox, &sourceBox, &pointBox, &channelBox, &paletteBox, &slopeBox,
        &scaleBox, &historyBox, &reverseButton, &freezeButton, &resetButton, &expandButton};
    const bool pageControls = !modeBox.isVisible();
    const int columns = std::clamp(getWidth() / (pageControls ? look::mixPageWidth : 96), 1, 4);
    const int count = static_cast<int>(std::ranges::count_if(controls, [](const auto *control) { return control->isVisible(); }));
    const int rows = std::max(1, (count + columns - 1) / columns);
    const int height = std::min(pageControls ? look::fieldHeight : 25, std::max(0, getHeight() / rows));
    int i = 0;
    for (auto *control : controls) {
        if (!control->isVisible()) continue;
        const int column = i % columns, row = i / columns;
        const int left = column * getWidth() / columns, right = (column + 1) * getWidth() / columns;
        control->setBounds(juce::Rectangle<int>(left, row * height, right - left, height).reduced(pageControls ? look::border : 1));
        ++i;
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
