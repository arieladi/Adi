// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_analysis.hpp"
#include "ui_colour_waveform.hpp"
#include "ui_filled_spectrum.hpp"
#include "ui_spectrogram_presentation.hpp"
#include "ui_look.hpp"
#include "ui_third_octave.hpp"
#include "adi/engine/main_loudness.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/ui/note_geometry.hpp"
#include <algorithm>
#include <charconv>
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

bool spectrumScaleHasFill(SpectrumScale scale) noexcept {
    return scale == SpectrumScale::Detail || scale == SpectrumScale::Extended;
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
constexpr float spectrogramTopDb = 24.f;
constexpr std::array spectrogramGammaValues{.5f, .75f, 1.f, 1.5f, 2.f};
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
bool sameSpectrumTimeline(const SharedAnalysis::Result &a, const SharedAnalysis::Result &b) noexcept {
    return a.sampleRate == b.sampleRate && a.request.fftSize == b.request.fftSize &&
           a.transportRevision == b.transportRevision && a.advancing == b.advancing;
}
bool spectrumFitsPrimary(const SharedAnalysis::Result *primary, const SharedAnalysis::Result &pin) noexcept {
    return primary && sameSpectrumTimeline(*primary, pin) && (!pin.advancing || pin.firstStamp <= primary->firstStamp);
}
std::int32_t overviewFftForRate(double rate) noexcept {
    // Stay near the 4,096 sample / 48 kHz window. Real FFT sizes are multiples
    // of 32 whose remaining factors are drawn from 2, 3 and 5.
    const auto target = 4096. * rate / 48000.;
    std::int32_t nearest = 32;
    for (std::int32_t twos = 32; twos <= SharedAnalysis::kMaxFftSize; twos *= 2)
        for (auto threes = twos; threes <= SharedAnalysis::kMaxFftSize; threes *= 3)
            for (auto size = threes; size <= SharedAnalysis::kMaxFftSize; size *= 5)
                if (std::abs(size - target) < std::abs(nearest - target)) nearest = size;
    return nearest;
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
    static constexpr std::size_t extendedCellCount = 1024;
    struct LogCell {
        float x = 0.f;
        std::size_t first = 0, end = 0;
        float fraction = 0.f;
    };

    // Store unit coordinates so resizing and repainting never rewalk the bins.
    double rate = 0.;
    int size = 0;
    SpectrumScale scale = SpectrumScale::Wide;
    std::size_t count = 0, firstBin = 0;
    std::vector<float> x;
    std::vector<LogCell> cells;
    juce::Path line, fill;

    void prepareLogCells(const SpectrumAxis &axis, double hzPerBin) {
        const auto cellFor = [](float unit) {
            const auto cell = static_cast<std::size_t>(std::clamp(unit, 0.f, 1.f) * extendedCellCount);
            return std::min(cell, extendedCellCount - 1);
        };
        const auto firstCell = cellFor(x.front()), lastCell = cellFor(x.back());
        const auto lastBin = firstBin + x.size() - 1;
        cells.reserve(lastCell - firstCell + 1);
        std::size_t bin = 0;
        for (auto cell = firstCell; cell <= lastCell; ++cell) {
            const auto centre = (static_cast<double>(cell) + .5) / static_cast<double>(extendedCellCount);
            const auto position = cell == firstCell ? x.front() :
                                  cell == lastCell ? x.back() : static_cast<float>(centre);
            const auto begin = bin;
            while (bin < x.size() && cellFor(x[bin]) == cell) ++bin;
            if (bin != begin) {
                cells.push_back({position, firstBin + begin, firstBin + bin, 0.f});
            } else {
                const auto hz = axis.lowHz * std::exp(centre * axis.logWidth);
                const auto fractional = std::clamp(hz / hzPerBin, static_cast<double>(firstBin), static_cast<double>(lastBin));
                const auto lower = std::min(static_cast<std::size_t>(fractional), lastBin - 1);
                cells.push_back({position, lower, lower, static_cast<float>(fractional - static_cast<double>(lower))});
            }
        }
    }

    void setFormat(SpectrumScale nextScale, double nextRate, int nextSize, std::size_t nextCount) {
        if (nextScale == scale && nextRate == rate && nextSize == size && nextCount == count) return;
        x.clear();
        cells.clear();
        if (std::isfinite(nextRate) && nextRate > 0 && nextSize > 0) {
            auto axis = spectrumAxis(nextScale);
            const auto hzPerBin = nextRate / nextSize;
            const auto lastHz = std::min(axis.highHz, nextRate * .5);
            if (nextScale == SpectrumScale::Wide && lastHz > axis.lowHz)
                axis.logWidth = std::log(lastHz / axis.lowHz);
            for (std::size_t i = 1; i < nextCount; ++i) {
                const auto hz = hzPerBin * static_cast<double>(i);
                if (hz > lastHz) break;
                if (hz < axis.lowHz) continue;
                if (x.empty()) firstBin = i;
                x.push_back(axis.x(hz));
            }
            if (nextScale == SpectrumScale::Extended && x.size() > extendedCellCount)
                prepareLogCells(axis, hzPerBin);
        }
        scale = nextScale; rate = nextRate; size = nextSize; count = nextCount;
    }

    void buildPaths(const SpectrumAxis &axis, bool fillTrace, float left, float right, auto &&enumerate) {
        line.clear();
        fill.clear();
        const bool filled = fillTrace && spectrumScaleHasFill(scale);
        bool first = true;
        const auto append = [&](float position, float value) {
            const auto y = axis.y(value);
            if (first) {
                first = false;
                line.startNewSubPath(position, y);
                if (filled) fill.startNewSubPath(position, y);
            } else {
                line.lineTo(position, y);
                if (filled) fill.lineTo(position, y);
            }
        };
        enumerate(append);
        if (filled) {
            fill.lineTo(right, 1.f);
            fill.lineTo(left, 1.f);
            fill.closeSubPath();
        }
    }

    void update(std::span<const float> bins, std::span<const float> offsets, bool fillTrace) {
        if (x.size() < 2) {
            line.clear();
            fill.clear();
            return;
        }
        const auto &axis = spectrumAxis(scale);
        const auto valueAt = [&](std::size_t bin) { return bins[bin] + (offsets.empty() ? 0.f : offsets[bin]); };
        buildPaths(axis, fillTrace, x.front(), x.back(), [&](const auto &append) {
            if (cells.empty()) {
                for (std::size_t i = 0; i < x.size(); ++i) append(x[i], valueAt(firstBin + i));
                return;
            }
            const auto availableValue = [&](std::size_t bin) {
                const auto value = valueAt(bin);
                return std::isfinite(value) ? value : static_cast<float>(axis.bottomDb);
            };
            for (const auto &cell : cells) {
                auto value = availableValue(cell.first);
                if (cell.first == cell.end) {
                    value = std::lerp(value, availableValue(cell.first + 1), cell.fraction);
                } else {
                    for (auto bin = cell.first + 1; bin < cell.end; ++bin)
                        value = std::max(value, availableValue(bin));
                }
                append(cell.x, value);
            }
        });
    }

    void updateSmoothed(SpectrumScale nextScale, const SpectrumAxis &axis,
                        std::span<const SpectrumSmoothing::Point> points, SpectrumSlope slope, bool fillTrace) {
        if (scale != nextScale) count = 0; // cached raw-bin geometry belongs to the previous scale
        scale = nextScale;
        buildPaths(axis, fillTrace, axis.x(points.front().frequencyHz), axis.x(points.back().frequencyHz),
                   [&](const auto &append) {
            for (const auto &point : points)
                append(axis.x(point.frequencyHz), point.db + static_cast<float>(spectrumDisplayOffset(slope, point.frequencyHz)));
        });
    }

    void paint(juce::Graphics &g, juce::Rectangle<float> plot, juce::Colour colour = look::value,
               bool primary = false) const {
        if (line.isEmpty() || plot.getWidth() <= 1.f || plot.getHeight() <= 1.f) return;
        const float inset = scale == SpectrumScale::Wide ? 0.f : 1.f;
        const auto transform = juce::AffineTransform::scale(plot.getWidth() - inset, plot.getHeight() - inset)
            .translated(plot.getX(), plot.getY());
        juce::Graphics::ScopedSaveState state(g);
        const bool widePrimary = primary && scale == SpectrumScale::Wide;
        if (!widePrimary) g.reduceClipRegion(plot.toNearestInt());
        if (!fill.isEmpty()) {
            g.setColour(colour.withAlpha(spectrumFillOpacity));
            g.fillPath(fill, transform);
        }
        g.setColour(colour);
        g.strokePath(line, juce::PathStrokeType(widePrimary ? 1.4f : look::line), transform);
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

struct SharedSpectrumView::SpectrumOverlay {
    struct Frame {
        std::shared_ptr<const SharedAnalysis::Result> window;
        std::shared_ptr<const std::vector<float>> projectedBins;
        bool operator==(const Frame &) const = default;
    };
    SpectrumOverlayTrack source;
    SharedAnalysis::Lease lease;
    SharedAnalysis::Lease::SourcePosition admission;
    Frame picture, waiting;
    std::unique_ptr<ScaledSpectrum> plot;

    void clearPicture() noexcept { picture = {}; waiting = {}; plot.reset(); }
};

namespace {
using SpectrogramScale = SharedSpectrumView::SpectrogramScale;

double spectrogramPosition(SpectrogramScale scale, double hz) noexcept {
    switch (scale) {
    case SpectrogramScale::Linear: return hz;
    case SpectrogramScale::Log: return std::log(hz);
    case SpectrogramScale::Mel: return std::log1p(hz / 700.);
    case SpectrogramScale::Bark: return 26.81 * hz / (1960. + hz) - .53;
    case SpectrogramScale::Erb: return std::log1p(.00437 * hz);
    }
    return hz;
}

double spectrogramFrequency(SpectrogramScale scale, double position) noexcept {
    switch (scale) {
    case SpectrogramScale::Linear: return position;
    case SpectrogramScale::Log: return std::exp(position);
    case SpectrogramScale::Mel: return 700. * std::expm1(position);
    case SpectrogramScale::Bark:
        return 1960. * (position + .53) / (26.81 - (position + .53));
    case SpectrogramScale::Erb: return std::expm1(position) / .00437;
    }
    return position;
}
} // namespace

void spectrogram::configureScaleBox(juce::ComboBox &box) {
    box.addItemList({"Linear", "Log", "Mel", "Bark", "ERB"}, 1);
    box.setSelectedId(static_cast<int>(SpectrogramScale::Log) + 1, juce::dontSendNotification);
}

double spectrogram::frequencyAt(SpectrogramScale scale, double lowHz, double topHz, double fraction) noexcept {
    if (scale == SpectrogramScale::Log) return lowHz * std::pow(topHz / lowHz, fraction);
    return spectrogramFrequency(scale, std::lerp(spectrogramPosition(scale, lowHz),
                                                 spectrogramPosition(scale, topHz), fraction));
}

void spectrogram::paintSweep(juce::Graphics &g, const juce::Image &image, juce::Rectangle<float> plot,
                            int rotation, bool drawGuide) {
    const auto clip = plot.toNearestInt();
    if (clip.isEmpty()) return;
    const auto columnWidth = plot.getWidth() / static_cast<float>(image.getWidth());
    if (rotation == 0) {
        g.drawImage(image, plot);
    } else {
        // Rotate the scrolling raster at paint time and keep its sampling phase on both sides.
        const auto offset = static_cast<float>(rotation) * columnWidth;
        const int split = std::clamp(juce::roundToInt(plot.getX() + offset), clip.getX(), clip.getRight());
        auto right = clip;
        const auto left = right.removeFromLeft(split - clip.getX());
        const auto draw = [&](juce::Rectangle<int> region, float x) {
            juce::Graphics::ScopedSaveState state(g);
            if (g.reduceClipRegion(region)) g.drawImage(image, plot.translated(x, 0.f));
        };
        draw(left, offset - plot.getWidth());
        draw(right, offset);
    }
    if (!drawGuide) return;
    const auto boundary = rotation == 0 ? plot.getRight() : plot.getX() + static_cast<float>(rotation) * columnWidth;
    const auto x = std::clamp(juce::roundToInt(boundary), clip.getX(), clip.getRight() - 1);
    g.setColour(look::textOnBand.withAlpha(.55f));
    g.drawVerticalLine(x, plot.getY(), plot.getBottom());
}

struct SharedSpectrumView::Raster {
    static constexpr int width = 128, height = 256;
    static constexpr std::uint16_t gap = 256;
    static constexpr double lowestHz = 20.;
    std::array<std::uint16_t, static_cast<std::size_t>(width * height)> intensity{};
    std::array<juce::Colour, 256> colors{};
    std::array<std::size_t, height> firstBin{}, lastBin{};
    SpectrogramScale scale = SpectrogramScale::Log;
    SpectrogramLevels levels;
    double sampleRate = 0., topHz = 0., lowPosition = 0., highPosition = 0.;
    std::int32_t fftSize = 0;
    juce::Image image;
    std::uint64_t revision = 0, epoch = 0, column = 0, sweepOrigin = 0;
    ThemeId theme = ThemeId::Dark;
    float gamma = 1.f;
    bool paletteDirty = true;
    Raster() { intensity.fill(gap); }
    bool matchesFrequencyMap(SpectrogramScale next, double rate, std::int32_t size) const noexcept {
        return scale == next && sampleRate == rate && fftSize == size;
    }
    double frequencyAt(double fraction) const noexcept {
        // Keep the endpoint computation of the default grid.
        return scale == SpectrogramScale::Log ? lowestHz * std::pow(topHz / lowestHz, fraction) :
            spectrogramFrequency(scale, std::lerp(lowPosition, highPosition, fraction));
    }
    void prepareFrequencyMap(SpectrogramScale next, double rate, std::int32_t size, double upperHz) noexcept {
        scale = next;
        sampleRate = rate;
        fftSize = size;
        topHz = upperHz;
        lowPosition = spectrogramPosition(scale, lowestHz);
        highPosition = spectrogramPosition(scale, topHz);
        const auto edgeBin = [&](int row) {
            return static_cast<std::size_t>(frequencyAt(static_cast<double>(row) / height) * size / rate);
        };
        auto low = edgeBin(0);
        for (int row = 0; row < height; ++row) {
            const auto y = static_cast<std::size_t>(row);
            const auto high = edgeBin(row + 1);
            firstBin[y] = std::max<std::size_t>(1, low);
            lastBin[y] = std::max(firstBin[y] + 1, high + 1);
            low = high;
        }
    }
    void setPalette(std::size_t index, bool reverse, float exponent) noexcept {
        theme = look::themeId();
        gamma = exponent;
        for (std::size_t i = 0; i < colors.size(); ++i) {
            const double strength = static_cast<double>(i) / 255.;
            const auto value = exponent == 1.f ? static_cast<std::uint8_t>(i) :
                static_cast<std::uint8_t>(255. * std::pow(strength, static_cast<double>(exponent)));
            colors[i] = analysisPaletteColour(index, value, reverse);
        }
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
    int sweepRotation() const noexcept {
        return sweepOrigin && column >= sweepOrigin ? static_cast<int>((column - sweepOrigin + 1) % width) : 0;
    }
    float rowPeak(std::span<const float> bins, std::size_t row) const noexcept {
        float peak = dsp::kDbFloor;
        for (auto i = firstBin[row]; i < std::min(bins.size(), lastBin[row]); ++i)
            peak = std::max(peak, bins[i]);
        return peak;
    }
    void paintSweep(juce::Graphics &g, juce::Rectangle<float> plot, bool drawGuide = true) const {
        if (!sweepOrigin || column < sweepOrigin) {
            g.drawImage(image, plot);
            return;
        }
        spectrogram::paintSweep(g, image, plot, sweepRotation(), drawGuide);
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
juce::Rectangle<int> filledSpectrumPlot(juce::Rectangle<int> bounds) {
    auto plot = bounds.reduced(5).withTrimmedTop(16);
    if (plot.getWidth() >= 120) plot = plot.withTrimmedRight(30);
    if (plot.getHeight() >= 44) plot = plot.withTrimmedBottom(14);
    return plot;
}
bool usesProjectedSpectrumBins(SharedSpectrumView::Mode mode) noexcept {
    return mode == SharedSpectrumView::Mode::SpectrumAverage || mode == SharedSpectrumView::Mode::SpectrumCumulativeAverage ||
           mode == SharedSpectrumView::Mode::SpectrumRealtimeSigma;
}
void describeAnalysisView(SharedSpectrumView &view, SharedSpectrumView::Mode mode) {
    if (mode == SharedSpectrumView::Mode::Phase) {
        view.setTitle("Phase");
        view.setTooltip("Shows stereo phase and left/right correlation.");
        return;
    }
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
    if (mode == SharedSpectrumView::Mode::SpectrumCumulativeAverage) {
        view.setTitle("Cumulative Average");
        view.setTooltip("Shows the average magnitude of each frequency since the interval began or was cleared.");
        return;
    }
    if (mode == SharedSpectrumView::Mode::SpectrumRealtimeSigma) {
        view.setTitle("Realtime Sigma");
        view.setTooltip("Shows the recent variation in each frequency's magnitude as a standard deviation.");
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
    std::optional<SpectrumLeases> retired;
    if (usesOverviewSpectrum() && supportsSpectrumOverlays(mode) && lease_)
        takeSpectrumLeases(retired.emplace());
    else if (supportsSpectrumOverlays(mode_))
        releaseSpectrumOverlays();
    if (usesProjectedSpectrumBins(mode) || usesProjectedSpectrumBins(mode_) || isMaximumSpectrum(mode) || isMaximumSpectrum(mode_) ||
        mode == Mode::FilledSpectrum || mode_ == Mode::FilledSpectrum ||
        mode == Mode::ThirdOctaveBands || mode_ == Mode::ThirdOctaveBands ||
        mode == Mode::ColourWaveform || mode_ == Mode::ColourWaveform)
        frozen_ = false;
    lease_.reset(); resetPicture(); mainMeter_.reset(); raster_.reset(); clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    sourcePosition_ = {}; historyClearPosition_ = {}; freshRequired_ = retired.has_value();
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
    if (mode_ == Mode::Phase || mode == Mode::Phase || mode_ == Mode::ClassicScope || mode == Mode::ClassicScope ||
        isMaximumSpectrum(mode_) || isMaximumSpectrum(mode) ||
        mode_ == Mode::SpectrumCumulativeAverage || mode == Mode::SpectrumCumulativeAverage ||
        mode_ == Mode::SpectrumRealtimeSigma || mode == Mode::SpectrumRealtimeSigma ||
        mode_ == Mode::FilledSpectrum || mode == Mode::FilledSpectrum ||
        mode_ == Mode::ThirdOctaveBands || mode == Mode::ThirdOctaveBands ||
        mode_ == Mode::ColourWaveform || mode == Mode::ColourWaveform)
        describeAnalysisView(*this, mode);
    mode_ = mode;
    if (!spectrumMode()) {
        activeSpectrumFftSize_ = 1024;
        activeSpectrumOverlap_ = 75;
    }
    if (classicPanel_) {
        resetClassic(false);
        classicPanel_->setVisible(mode_ == Mode::ClassicScope);
    }
    updateSpectrumCursorInput();
    updateDemand();
    if (retired && lease_) acquireSpectrumOverlays();
    repaint();
}
void SharedSpectrumView::enablePhaseBands() {
    if (phaseBandsEnabled_) return;
    phaseBandsEnabled_ = true;
    if (mode_ != Mode::Phase) return;
    lease_.reset();
    sourcePosition_ = {};
    freshRequired_ = true;
    resetPicture();
    updateDemand();
    repaint();
}
void SharedSpectrumView::setPhaseBandDisplay(PhaseBandDisplay display) {
    if (phaseBandDisplay_ == display) return;
    const bool wasSpatial = phaseBandDisplay_ != PhaseBandDisplay::Correlation;
    const bool wantsSpatial = display != PhaseBandDisplay::Correlation;
    phaseBandDisplay_ = display;
    if (mode_ != Mode::Phase || !phaseBandsEnabled_) return;
    if (wasSpatial != wantsSpatial && lease_ && eligible()) {
        try {
            // Admit the new output on the same prepared domain before releasing
            // the former output. Width and Pan themselves share one projection.
            auto next = broker_->acquire(phaseRequest());
            if (next) {
                phaseProjectionPending_ = wantsSpatial && static_cast<bool>(shown_);
                lease_ = std::move(next);
            }
        } catch (...) { /* Keep the usable vectorscope if spatial admission fails. */ }
    }
    repaint();
}
void SharedSpectrumView::setStereoBalanceEnabled(bool value) noexcept {
    if (stereoBalanceEnabled_ == value) return;
    stereoBalanceEnabled_ = value;
    lease_.setStereoBalanceEnabled(value && supportsSpectrumOverlays(mode_));
}
void SharedSpectrumView::setSpectrumLevelsEnabled(bool value) noexcept {
    if (spectrumLevelsEnabled_ == value) return;
    spectrumLevelsEnabled_ = value;
    lease_.setSpectrumLevelsEnabled(value && supportsSpectrumOverlays(mode_));
}
void SharedSpectrumView::setSpectrumTruePeakEnabled(bool value) noexcept {
    if (spectrumTruePeakEnabled_ == value) return;
    spectrumTruePeakEnabled_ = value;
    lease_.setSpectrumTruePeakEnabled(value && spectrumLevelsEnabled_ && supportsSpectrumOverlays(mode_));
}
void SharedSpectrumView::setSource(engine::ScopePoint point, std::int64_t device) {
    if (point_ == point && device_ == device) return;
    releaseSpectrumOverlays();
    lease_.reset(); resetPicture(); mainMeter_.reset(); raster_.reset(); clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    sourcePosition_ = {}; historyClearPosition_ = {}; freshRequired_ = false;
    point_ = point; device_ = device; resetClassic(false); updateDemand(); repaint();
}
void SharedSpectrumView::setChannel(SharedAnalysis::Channel channel) {
    if (channel_ == channel) return;
    channel_ = channel;
    if (mode_ == Mode::Phase && phaseBandsEnabled_) return;
    releaseSpectrumOverlays();
    lease_.reset(); resetPicture(); mainMeter_.reset(); raster_.reset(); clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
    sourcePosition_ = {}; historyClearPosition_ = {}; freshRequired_ = false;
    updateDemand(); repaint();
}
void SharedSpectrumView::setSpectrumOverlayTracks(std::span<const SpectrumOverlayTrack> tracks,
                                                   SpectrumOverlayProfile profile) {
    overlayProfile_ = profile;
    if (std::ranges::equal(spectrumOverlays_, tracks, std::ranges::equal_to{}, &SpectrumOverlay::source)) return;

    std::vector<SpectrumOverlay> next;
    next.reserve(std::min(tracks.size(), kSpectrumOverlayTracks));
    for (const auto &source : tracks) {
        if (source.track <= 0 || std::ranges::any_of(next, [&](const auto &overlay) {
                return overlay.source.track == source.track;
            })) continue;
        const auto existing = std::ranges::find_if(spectrumOverlays_, [&](const auto &overlay) {
            return overlay.source.track == source.track;
        });
        if (existing == spectrumOverlays_.end()) next.emplace_back();
        else next.push_back(std::move(*existing));
        next.back().source = source;
        if (next.size() == kSpectrumOverlayTracks) break;
    }
    spectrumOverlays_.swap(next);
    if (supportsSpectrumOverlays(mode_) && eligible()) repaint();
}
bool SharedSpectrumView::usesOverviewSpectrum() const noexcept {
    return supportsSpectrumOverlays(mode_) && device_ == 0 && !spectrumOverlays_.empty() &&
           overlayProfile_ == SpectrumOverlayProfile::Overview;
}
SharedAnalysis::Request SharedSpectrumView::spectrumRequest(std::int64_t track) const noexcept {
    const auto size = spectrumMode() ? activeSpectrumFftSize_ : 1024;
    SharedAnalysis::Request request{track, point_, size, channel_, dsp::Window::Hann};
    if (spectrumMode() && activeSpectrumOverlap_ != 75)
        request.hopSize = (size * (100 - activeSpectrumOverlap_) + 50) / 100;
    request.device = device_;
    if (mode_ == Mode::SpectrumAverage) request.outputs = SharedAnalysis::Average1s;
    else if (mode_ == Mode::SpectrumCumulativeAverage) request.outputs = SharedAnalysis::CumulativeAverage;
    else if (mode_ == Mode::SpectrumRealtimeSigma) request.outputs = SharedAnalysis::RealtimeSigma;
    return request;
}
SharedAnalysis::Request SharedSpectrumView::phaseRequest() const noexcept {
    auto request = spectrumRequest(track_);
    request.outputs = SharedAnalysis::Phase;
    if (phaseBandsEnabled_) {
        request.outputs |= SharedAnalysis::PhaseBands;
        if (phaseBandDisplay_ != PhaseBandDisplay::Correlation) request.outputs |= SharedAnalysis::PhaseBandDetail;
        request.fftSize = 4096;
        request.hopSize = request.fftSize / 2;
        request.channel = SharedAnalysis::Channel::StereoPower;
    }
    return request;
}
void SharedSpectrumView::acquireSpectrumOverlays() noexcept {
    if (!supportsSpectrumOverlays(mode_) || device_ != 0) return;
    for (auto &overlay : spectrumOverlays_) {
        if (overlay.source.track == track_) {
            overlay.lease.reset();
            overlay.admission = {};
            overlay.clearPicture();
        } else if (!overlay.lease) {
            try {
                overlay.lease = broker_->acquire(spectrumRequest(overlay.source.track));
                overlay.admission = overlay.lease.sourcePosition();
            } catch (...) {
                overlay.lease.reset();
            }
        }
    }
}
void SharedSpectrumView::takeSpectrumLeases(SpectrumLeases &leases) noexcept {
    leases.front() = std::move(lease_);
    for (std::size_t i = 0; i < spectrumOverlays_.size(); ++i) {
        auto &overlay = spectrumOverlays_[i];
        leases[i + 1] = std::move(overlay.lease);
        overlay.admission = {};
        overlay.clearPicture();
    }
}
void SharedSpectrumView::releaseSpectrumOverlays(bool keepPictures) noexcept {
    for (auto &overlay : spectrumOverlays_) {
        overlay.lease.reset();
        overlay.admission = {};
        overlay.waiting = {};
        if (!keepPictures) overlay.clearPicture();
    }
}
bool SharedSpectrumView::spectrumOverlayMatchesPrimary(const SpectrumOverlay &overlay) const noexcept {
    return overlay.picture.window && spectrumFitsPrimary(shown_.get(), *overlay.picture.window);
}
void SharedSpectrumView::updateSpectrumOverlayPlot(SpectrumOverlay &overlay) noexcept {
    if (!spectrumOverlayMatchesPrimary(overlay)) {
        overlay.plot.reset();
        return;
    }
    const auto &picture = *overlay.picture.window;
    const auto &bins = overlay.picture.projectedBins ? *overlay.picture.projectedBins : picture.binsDbfs;
    const auto offsets = spectrumSlope_ == SpectrumSlope::Tilt45 ? spectrumProjection_.offsets() :
        std::span<const float>{};
    if (spectrumSlope_ == SpectrumSlope::Tilt45 && offsets.size() != bins.size()) {
        overlay.plot.reset();
        return;
    }
    try {
        if (!overlay.plot) overlay.plot = std::make_unique<ScaledSpectrum>();
        updateSpectrumPlot(*overlay.plot, picture, bins, offsets, false);
    } catch (...) {
        overlay.plot.reset();
    }
}
void SharedSpectrumView::updateSpectrumOverlays(bool primaryChanged) noexcept {
    if (!supportsSpectrumOverlays(mode_) || spectrumOverlays_.empty()) return;
    bool redraw = false;
    for (std::size_t i = 0; i < spectrumOverlays_.size(); ++i) {
        auto &overlay = spectrumOverlays_[i];
        SpectrumOverlay::Frame next;
        if (mode_ == Mode::SpectrumAverage) {
            const auto average = overlay.lease.readAverage();
            if (average && average->status == SharedAnalysis::AverageStatus::Ready && average->binsDbfs)
                next = {average->window, average->binsDbfs};
        } else if (mode_ == Mode::SpectrumCumulativeAverage) {
            const auto average = overlay.lease.readCumulativeAverage();
            if (average)
                next = {average->window, std::shared_ptr<const std::vector<float>>(average, &average->binsDbfs)};
        } else if (mode_ == Mode::SpectrumRealtimeSigma) {
            const auto sigma = overlay.lease.readRealtimeSigma();
            if (sigma) next = {sigma->window, std::shared_ptr<const std::vector<float>>(sigma, &sigma->binsDbfs)};
        } else {
            next.window = overlay.lease.read();
        }
        const auto &window = next.window;
        if (!window || window->generation != overlay.admission.generation || !overlay.admission.generation) {
            if (window || overlay.picture.window || !overlay.admission.generation)
                overlay.admission = overlay.lease.sourcePosition();
            redraw = redraw || static_cast<bool>(overlay.plot);
            overlay.clearPicture();
            continue;
        }
        const auto size = static_cast<std::int64_t>(window->request.fftSize);
        if (size <= 0 || window->firstSequence < 0 ||
            window->firstSequence > std::numeric_limits<std::int64_t>::max() - size ||
            window->firstSequence + size <= overlay.admission.written) continue;
        // Keep one waiting picture even when the worker replaces its latest
        // publication. A persistently leading pin still advances as heard.
        auto &waiting = overlay.waiting;
        if (waiting.window && (waiting.window->sourceEpoch != window->sourceEpoch ||
                               !sameSpectrumTimeline(*waiting.window, *window)))
            waiting = {};
        if (spectrumFitsPrimary(shown_.get(), *window)) {
            waiting = {};
        } else if (waiting.window && spectrumFitsPrimary(shown_.get(), *waiting.window)) {
            std::swap(next, waiting);
        } else {
            if (!waiting.window && window->advancing) waiting = std::move(next);
            next = {};
        }

        // The primary and first seven additional curves follow the normal clock.
        // Extra paths adopt new data at 15 Hz while the worker retains its hop rate.
        const auto &prior = overlay.picture.window;
        bool due = window && next != overlay.picture;
        if (due && i >= 7 && prior && prior->generation == window->generation &&
            prior->sourceEpoch == window->sourceEpoch && prior->transportRevision == window->transportRevision &&
            prior->advancing == window->advancing && prior->sampleRate == window->sampleRate) {
            const auto before = window->advancing ? prior->firstStamp : prior->firstSequence;
            const auto after = window->advancing ? window->firstStamp : window->firstSequence;
            if (after >= before)
                due = static_cast<double>(static_cast<std::uint64_t>(after) - static_cast<std::uint64_t>(before))
                    >= window->sampleRate / 15.;
        }
        if (due) overlay.picture = std::move(next);
        if (due || (primaryChanged && spectrumOverlayMatchesPrimary(overlay) != static_cast<bool>(overlay.plot))) {
            updateSpectrumOverlayPlot(overlay);
            redraw = true;
        }
    }
    if (redraw && !primaryChanged) repaint();
}
void SharedSpectrumView::paintSpectrumOverlays(juce::Graphics &g, juce::Rectangle<float> plot) const {
    if (spectrumOverlays_.empty() || plot.isEmpty()) return;
    for (const auto &overlay : spectrumOverlays_)
        if (overlay.plot) overlay.plot->paint(g, plot, overlay.source.colour);

    // Keep the labels within two short rows on smaller windows.
    const auto rowHeight = static_cast<float>(look::fieldHeight);
    auto legend = plot.reduced(static_cast<float>(look::gap), 0.f)
                      .withHeight(std::min(2.f * rowHeight, plot.getHeight() * .25f));
    const auto columns = std::max(1, static_cast<int>(legend.getWidth() / static_cast<float>(look::mixPageWidth)));
    const auto rows = static_cast<int>(legend.getHeight() / rowHeight);
    if (!rows) return;
    const auto capacity = static_cast<std::size_t>(columns * rows);
    const bool overflow = spectrumOverlays_.size() > capacity;
    const auto count = std::min(capacity, spectrumOverlays_.size());
    const auto width = legend.getWidth() / static_cast<float>(columns);
    g.setFont(look::font(look::fontCaption));
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = static_cast<int>(i);
        auto cell = juce::Rectangle<float>(legend.getX() + width * static_cast<float>(index % columns),
            legend.getY() + rowHeight * static_cast<float>(index / columns), width, rowHeight)
            .reduced(static_cast<float>(look::border), 0.f);
        g.setColour(look::display.withAlpha(.85f));
        g.fillRect(cell);
        g.setColour(look::textOnBand);
        if (overflow && i + 1 == count) {
            const auto remaining = static_cast<int>(spectrumOverlays_.size() - i);
            g.drawText("+" + juce::String(remaining), cell, juce::Justification::centredLeft, true);
            break;
        }
        const auto &overlay = spectrumOverlays_[i];
        auto key = cell.removeFromLeft(rowHeight).reduced(rowHeight * .25f);
        g.setColour(overlay.source.colour);
        g.fillRect(key.withSizeKeepingCentre(key.getWidth(), look::line));
        g.setColour(look::textOnBand);
        g.drawText(overlay.source.name, cell, juce::Justification::centredLeft, true);
    }
}
void SharedSpectrumView::setPalette(std::string_view id, bool reverse) {
    if (palette() == id && paletteReversed_ == reverse) return;
    const auto index = palettes::find(id);
    if (paletteIndex_ == index && paletteReversed_ == reverse) return;
    paletteIndex_ = index; paletteReversed_ = reverse;
    if (raster_) raster_->setPalette(index, reverse, spectrogramGamma_);
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
void SharedSpectrumView::setSpectrumBlockSize(std::int32_t size) {
    if (requestedSpectrumFftSize_ == size || std::ranges::find(spectrumBlockSizes, size) == spectrumBlockSizes.end()) return;
    requestedSpectrumFftSize_ = size;
    if (spectrumMode() && !frozen_) updateDemand();
}

void SharedSpectrumView::setSpectrumOverlap(int percent) {
    if (requestedSpectrumOverlap_ == percent || std::ranges::find(spectrumOverlaps, percent) == spectrumOverlaps.end()) return;
    requestedSpectrumOverlap_ = percent;
    if (spectrumMode() && !frozen_) updateDemand();
}

void SharedSpectrumView::setSpectrumSmoothing(std::size_t choice) {
    if (choice == spectrumSmoothingChoice_ || choice >= kSpectrumSmoothingChoices.size()) return;
    spectrumSmoothingChoice_ = choice;
    if (choice == 0) spectrumSmoothing_.reset();
    if (!spectrumMode() || !eligible()) return;
    updateScaledSpectrum();
    for (auto &overlay : spectrumOverlays_) updateSpectrumOverlayPlot(overlay);
    repaint();
}

void SharedSpectrumView::setSpectrumSlope(SpectrumSlope slope) {
    slope = spectrumSlopeFromId(spectrumSlopeId(slope));
    if (slope == SpectrumSlope::Flat) spectrumProjection_.reset();
    if (spectrumSlope_ == slope) return;
    spectrumSlope_ = slope;
    try { updateSpectrumProjection(); } catch (...) { spectrumProjection_.reset(); }
    updateScaledSpectrum();
    for (auto &overlay : spectrumOverlays_) updateSpectrumOverlayPlot(overlay);
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
    updateSpectrumCursorInput();
    if (!spectrumMode() || !eligible()) return;
    updateScaledSpectrum();
    for (auto &overlay : spectrumOverlays_) updateSpectrumOverlayPlot(overlay);
    repaint();
}
void SharedSpectrumView::updateSpectrumPlot(ScaledSpectrum &plot, const SharedAnalysis::Result &picture,
                                            std::span<const float> bins, std::span<const float> offsets,
                                            bool fillTrace) {
    if (spectrumSmoothingChoice_ != 0 && bins.size() >= 2 && picture.request.fftSize > 0) {
        auto axis = spectrumAxis(spectrumScale_);
        const auto nyquist = picture.sampleRate * .5;
        const auto lastHz = std::min(axis.highHz, nyquist);
        if (spectrumScale_ == SpectrumScale::Wide && lastHz > axis.lowHz)
            axis.logWidth = std::log(lastHz / axis.lowHz);
        if (!spectrumSmoothing_) spectrumSmoothing_ = std::make_unique<SpectrumSmoothing>();
        if (spectrumSmoothing_->prepare(spectrumSmoothingChoice_, axis.lowHz, lastHz,
                                        picture.sampleRate, picture.request.fftSize, bins.size())) {
            const auto points = spectrumSmoothing_->project(bins);
            if (points.size() >= 2) {
                plot.updateSmoothed(spectrumScale_, axis, points, spectrumSlope_, fillTrace);
                return;
            }
        }
    }
    plot.setFormat(spectrumScale_, picture.sampleRate, picture.request.fftSize, bins.size());
    plot.update(bins, offsets, fillTrace);
}

void SharedSpectrumView::updateScaledSpectrum() noexcept {
    if (!spectrumMode() || !shown_) {
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
        updateSpectrumPlot(*scaledSpectrum_, *shown_, bins, offsets, true);
    } catch (...) {
        scaledSpectrum_.reset();
    }
}
void SharedSpectrumView::updateSpectrumProjection() {
    if (!spectrumMode() || !shown_) return;
    spectrumProjection_.prepare(spectrumSlope_, shown_->sampleRate, shown_->request.fftSize, spectrumBins().size());
}
std::span<const float> SharedSpectrumView::spectrumBins() const noexcept {
    if (usesProjectedSpectrumBins(mode_))
        return projectedBins_ ? std::span<const float>(*projectedBins_) : std::span<const float>{};
    if (isMaximumSpectrum(mode_))
        return maximumShown_ ? std::span<const float>(maximumShown_->binsDbfs) : std::span<const float>{};
    return mode_ == Mode::Spectrum && shown_ ? std::span<const float>(shown_->binsDbfs) : std::span<const float>{};
}
void SharedSpectrumView::resetPicture() noexcept {
    phaseProjectionPending_ = false;
    shown_.reset();
    projectedBins_.reset();
    maximumShown_.reset();
    scaledSpectrum_.reset();
    spectrumSmoothing_.reset();
    averageStatus_ = SharedAnalysis::AverageStatus::Waiting;
    filledPointer_ = {-1, -1};
    filledDisplaySlot_ = -1.;
    if (phaseTrail_) phaseTrail_->clear();
    if (bandDisplay_) bandDisplay_->clear();
    spectrumCursorPosition_.reset();
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
    const auto peak = mainMeter_->truePeakDb;
    const auto integrated = mainMeter_->integratedLufs;
    const auto plr = std::isfinite(peak) && std::isfinite(integrated)
                         ? meterNumber(peak - integrated) : juce::String("--");
    return "Main aggregate programme / pre-cue\nM " + meterNumber(mainMeter_->momentaryLufs) +
           "  S " + meterNumber(mainMeter_->shortTermLufs) + "  I " + meterNumber(integrated) +
           " LUFS\nTP (held) " + meterNumber(peak) + " dBTP  PLR " + plr + " dB";
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
    releaseSpectrumOverlays(value);
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
    } else if (mode_ == Mode::SpectrumCumulativeAverage) {
        lease_.restartCumulativeAverage();
    } else {
        armFreshBoundary();
    }
    if (spectrumLevelsEnabled_) lease_.restartSpectrumLevels();
    if (mode_ == Mode::Spectrogram || mode_ == Mode::ColourWaveform) historyClearPosition_ = sourcePosition_;
    for (auto &overlay : spectrumOverlays_) {
        if (mode_ == Mode::SpectrumCumulativeAverage) overlay.lease.restartCumulativeAverage();
        overlay.admission = overlay.lease.sourcePosition();
        overlay.clearPicture();
    }
    resetPicture(); mainMeter_.reset(); raster_.reset();
    status_ = lease_.available() ? Status::Waiting : Status::Detached; repaint();
}
SharedSpectrumView::~SharedSpectrumView() {
    unwatchAncestors();
    lease_.reset();
}
void SharedSpectrumView::present(std::shared_ptr<SharedAnalysis> broker, std::int64_t track) {
    if (broker_ != broker || track_ != track) {
        if (broker_ != broker) releaseSpectrumOverlays();
        else for (auto &overlay : spectrumOverlays_)
            if (overlay.source.track == track) {
                overlay.lease.reset();
                overlay.admission = {};
                overlay.clearPicture();
            }
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
        releaseSpectrumOverlays();
        if (shown_ || lease_) freshRequired_ = true;
        lease_.reset();
        sourcePosition_ = {};
        resetPicture();
        raster_.reset();
        mainMeter_.reset();
        status_ = Status::Detached;
    } else if (!broker_->active()) {
        releaseSpectrumOverlays();
        lease_.reset(); resetPicture(); raster_.reset(); mainMeter_.reset();
        sourcePosition_ = {}; freshRequired_ = true;
        status_ = Status::Unavailable;
    } else if (frozen_) {
        lease_.reset();
        if (mainMeterSource_.expired()) mainMeter_.reset();
    } else {
        const bool overview = requestedSpectrumFftSize_ == 0 && usesOverviewSpectrum();
        const auto rate = overview ? lease_.preparedSampleRate() : 0.;
        if (rate > 0. && overviewRate_ != rate) {
            overviewFftSize_ = overviewFftForRate(rate);
            overviewRate_ = rate;
        }
        auto size = overview ? (overviewRate_ > 0. ? overviewFftSize_ : activeSpectrumFftSize_) : 1024;
        const auto spectral = spectrumMode();
        if (spectral && requestedSpectrumFftSize_ > 0) size = requestedSpectrumFftSize_;
        const auto overlap = spectral ? requestedSpectrumOverlap_ : 75;
        std::optional<SpectrumLeases> retired;
        if (activeSpectrumFftSize_ != size || activeSpectrumOverlap_ != overlap) {
            takeSpectrumLeases(retired.emplace());
            activeSpectrumFftSize_ = size;
            activeSpectrumOverlap_ = overlap;
            resetPicture();
            freshRequired_ = true;
        }
        if (!lease_) {
            auto request = spectrumRequest(track_);
            switch (mode_) {
            case Mode::Spectrum:
            case Mode::SpectrumAverage:
            case Mode::SpectrumCumulativeAverage:
            case Mode::SpectrumRealtimeSigma: break;
            case Mode::Scope: request.outputs = SharedAnalysis::Scope; break;
            case Mode::Meters:
            case Mode::ChannelReadout: request.outputs = SharedAnalysis::Meters; break;
            case Mode::Phase: request = phaseRequest(); break;
            case Mode::Spectrogram: request.outputs = SharedAnalysis::Spectrogram; break;
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
                if (supportsSpectrumOverlays(mode_)) {
                    if (spectrumLevelsEnabled_) {
                        lease_.setSpectrumLevelsEnabled(true);
                        if (spectrumTruePeakEnabled_) lease_.setSpectrumTruePeakEnabled(true);
                    }
                    if (stereoBalanceEnabled_) lease_.setStereoBalanceEnabled(true);
                }
                sourcePosition_ = lease_.sourcePosition();
                clearedRevision_ = clearedEpoch_ = clearedColumn_ = 0;
                historyClearPosition_ = mode_ == Mode::ColourWaveform && freshRequired_
                    ? sourcePosition_ : SharedAnalysis::Lease::SourcePosition{};
            }
            catch (...) { lease_.reset(); }
        }
        // The replacement requests attach before the former requests release
        // their sources. Keep existing physical taps when their capacity fits.
        if (retired && lease_) acquireSpectrumOverlays();
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
    if (raster_ && (raster_->theme != look::themeId() || (raster_->gamma != spectrogramGamma_ && eligible()))) {
        raster_->setPalette(paletteIndex_, paletteReversed_, spectrogramGamma_);
        try { raster_->recolor(); } catch (...) { raster_.reset(); }
        repaint();
    }
    if (!lease_) return;
    acquireSpectrumOverlays();
    broker_->requestFrame();
    updateDemand(); // owner resolution may have changed pending availability/generation
    if (!lease_.available()) return;
    const bool averaging = mode_ == Mode::SpectrumAverage;
    const bool maximumMode = isMaximumSpectrum(mode_);
    const auto average = averaging ? lease_.readAverage() : nullptr;
    const auto cumulative = mode_ == Mode::SpectrumCumulativeAverage ? lease_.readCumulativeAverage() : nullptr;
    const auto sigma = mode_ == Mode::SpectrumRealtimeSigma ? lease_.readRealtimeSigma() : nullptr;
    const auto maximum = maximumMode ? lease_.readMaximum() : nullptr;
    const auto averageStatus = average ? average->status : SharedAnalysis::AverageStatus::Waiting;
    std::shared_ptr<const SharedAnalysis::Result> next;
    if (averaging) {
        if (averageStatus == SharedAnalysis::AverageStatus::Ready && average->binsDbfs)
            next = average->window;
    } else if (mode_ == Mode::SpectrumCumulativeAverage) {
        if (cumulative) next = cumulative->window;
    } else if (mode_ == Mode::SpectrumRealtimeSigma) {
        if (sigma) next = sigma->window;
    } else if (maximumMode) {
        if (maximum) next = maximum->window;
    } else {
        next = lease_.read();
    }
    // A new rail projection can be pending while the same source's vector is
    // still usable. A replacement tap must take the normal freshness path.
    if (!next && phaseProjectionPending_ && shown_ && lease_.sourcePosition().generation == shown_->generation)
        return;
    phaseProjectionPending_ = false;
    // Invalidation can replace a live source before its next publication. Fence
    // at that activation, rather than accepting buffered replacement-source data.
    if (!next && shown_) armFreshBoundary();
    if (next && next->generation != sourcePosition_.generation) {
        armFreshBoundary();
        next.reset();
    }
    // The cumulative lease owns its admission fence. A held picture can
    // remain unchanged across later hops after the fence has been satisfied.
    if (next && (mode_ == Mode::SpectrumCumulativeMaximum || mode_ == Mode::SpectrumCumulativeAverage ||
                 mode_ == Mode::FilledSpectrum))
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
    auto nextBins = next && average ? average->binsDbfs : nullptr;
    if (next && cumulative)
        nextBins = std::shared_ptr<const std::vector<float>>(cumulative, &cumulative->binsDbfs);
    if (next && sigma)
        nextBins = std::shared_ptr<const std::vector<float>>(sigma, &sigma->binsDbfs);
    const auto nextMaximum = next ? maximum : nullptr;
    const bool pictureChanged = next != shown_ || nextBins != projectedBins_ || nextMaximum != maximumShown_ ||
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
        projectedBins_ = nextBins;
        maximumShown_ = nextMaximum;
        averageStatus_ = averageStatus;
        mainMeter_.reset();
        status_ = pictureStatus();
        repaint();
    }
    updateMainMeter();
    try { updateRaster(); } catch (...) { raster_.reset(); }
    try { updateSpectrumProjection(); } catch (...) { spectrumProjection_.reset(); }
    if (pictureChanged && spectrumMode()) updateScaledSpectrum();
    updateSpectrumOverlays(pictureChanged);
}
void SharedSpectrumView::setSpectrogramScale(SpectrogramScale scale) {
    if (spectrogramScale_ == scale) return;
    spectrogramScale_ = scale;
    if (mode_ != Mode::Spectrogram || !shown_) return;
    try { updateRaster(); } catch (...) { raster_.reset(); }
    repaint();
}

void SharedSpectrumView::setSpectrogramLevels(SpectrogramLevels levels) {
    if (levels == spectrogramLevels_ || !std::isfinite(levels.floor) || !std::isfinite(levels.ceiling) ||
        levels.floor < dsp::kDbFloor || levels.ceiling > spectrogramTopDb || levels.ceiling - levels.floor < 1.f) return;
    spectrogramLevels_ = levels;
    if (mode_ != Mode::Spectrogram || !shown_) return;
    try { updateRaster(); } catch (...) { raster_.reset(); }
    repaint();
}
bool SharedSpectrumView::updateFilledDisplay() noexcept {
    const auto slot = std::floor(juce::Time::getMillisecondCounterHiRes() * (filled_spectrum::framesPerSecond / 1000.));
    if (slot == filledDisplaySlot_) return false;
    filledDisplaySlot_ = slot;
    juce::Point<int> pointer{-1, -1};
    if (const auto *peer = getPeer(); isShowing() && peer && peer->getNativeHandle()) {
        const auto position = getMouseXYRelative();
        if (filledSpectrumPlot(getLocalBounds()).contains(position)) pointer = position;
    }
    if (pointer != filledPointer_) {
        filledPointer_ = pointer;
        repaint();
    }
    return true;
}

void SharedSpectrumView::setSpectrogramGamma(float gamma) {
    if (gamma == spectrogramGamma_ || !std::isfinite(gamma) ||
        gamma < spectrogramGammaValues.front() || gamma > spectrogramGammaValues.back()) return;
    spectrogramGamma_ = gamma;
    if (!raster_ || !eligible()) return;
    raster_->setPalette(paletteIndex_, paletteReversed_, spectrogramGamma_);
    try { raster_->recolor(); } catch (...) { raster_.reset(); }
    repaint();
}

void SharedSpectrumView::setSpectrogramRuler(bool visible) {
    if (spectrogramRuler_ == visible) return;
    spectrogramRuler_ = visible;
    if (mode_ == Mode::Spectrogram) repaint();
}

bool SharedSpectrumView::hasSpectrogramImage() const noexcept {
    return mode_ == Mode::Spectrogram && raster_ && raster_->image.isValid();
}

juce::Image SharedSpectrumView::spectrogramImage() const {
    if (!hasSpectrogramImage()) return {};
    if (historyMotion_ == HistoryMotion::Scroll) return raster_->image.createCopy();
    juce::Image image(juce::Image::RGB, Raster::width, Raster::height, true, juce::SoftwareImageType{});
    juce::Graphics graphics(image);
    raster_->paintSweep(graphics, image.getBounds().toFloat(), false);
    return image;
}

void SharedSpectrumView::updateRaster() {
    if (mode_ != Mode::Spectrogram || !shown_) return;
    const auto rate = shown_->sampleRate;
    const auto fftSize = shown_->request.fftSize;
    const bool frequencyChanged = !raster_ || !raster_->matchesFrequencyMap(spectrogramScale_, rate, fftSize);
    const bool levelsChanged = raster_ && raster_->levels != spectrogramLevels_;
    const bool needsColumns = frequencyChanged || levelsChanged || raster_->image.isNull() ||
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
        const auto topHz = std::min(20000., rate * .5);
        if ((!count && !shown_->historySerial) || !std::isfinite(rate) || topHz <= 20 || fftSize <= 0) return;
        if (!raster_) {
            raster_ = std::make_unique<Raster>();
            raster_->setPalette(paletteIndex_, paletteReversed_, spectrogramGamma_);
        }
        auto &raster = *raster_;
        if (frequencyChanged) raster.prepareFrequencyMap(spectrogramScale_, rate, fftSize, topHz);
        raster.levels = spectrogramLevels_;
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
        if (newSweep || frequencyChanged || levelsChanged || shown_->historySerial - raster.column >= Raster::width) {
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
                    retained[Raster::width - 1 - static_cast<std::size_t>(shown_->historySerial - serial)] = true;
            }
            for (std::size_t x = 0; x < retained.size(); ++x)
                if (!retained[x]) raster.clearColumns(static_cast<int>(x), 1);
        }
        juce::Image::BitmapData pixels(raster.image, juce::Image::BitmapData::writeOnly);
        const auto levelSpan = raster.levels.ceiling - raster.levels.floor;
        for (auto column = firstColumn; column < columns.size(); ++column) {
            const auto &entry = columns[column];
            if (!entry.binsDbfs || entry.serial > shown_->historySerial || shown_->historySerial - entry.serial >= Raster::width) continue;
            const auto &bins = *entry.binsDbfs;
            const int x = Raster::width - 1 - static_cast<int>(shown_->historySerial - entry.serial);
            for (int row = 0; row < Raster::height; ++row) {
                const auto peak = raster.rowPeak(bins, static_cast<std::size_t>(row));
                const auto strength = std::clamp((peak - raster.levels.floor) / levelSpan, 0.f, 1.f);
                const auto value = static_cast<std::uint16_t>(strength * 255.f);
                const int y = Raster::height - row - 1;
                raster.intensity[static_cast<std::size_t>(y * Raster::width + x)] = value;
                pixels.setPixelColour(x, y, raster.colors[value]);
            }
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
    spectrumCursorPosition_.reset();
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
    releaseSpectrumOverlays();
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
namespace {
juce::Rectangle<float> analysisArea(juce::Rectangle<int> bounds) noexcept {
    return bounds.toFloat().reduced(5.f);
}
juce::Rectangle<float> analysisPlot(juce::Rectangle<float> area, float meterHeight = 0.f) noexcept {
    return area.withTrimmedTop(16.f).withTrimmedBottom(29.f + meterHeight).withTrimmedRight(18.f);
}
} // namespace

juce::Rectangle<float> spectrumGridPlot(juce::Rectangle<float> area, SpectrumScale scale) noexcept {
    const auto &axis = spectrumAxis(scale);
    const auto scaleWidth = look::fontCaption * 3.f;
    const auto stepHeight = area.getHeight() / static_cast<float>((axis.topDb - axis.bottomDb) / axis.divisionDb);
    if (area.getWidth() < scaleWidth * 4.f || stepHeight <= look::fontCaption) return area;
    if (scale == SpectrumScale::Device) return area.withTrimmedLeft(scaleWidth).withTrimmedTop(look::fontCaption);
    return area.withTrimmedRight(scaleWidth).withTrimmedBottom(look::fontCaption);
}
namespace {
juce::Rectangle<float> spectrumCursorPlot(juce::Rectangle<int> bounds, SpectrumScale scale) noexcept {
    const auto plot = analysisPlot(analysisArea(bounds));
    return scale == SpectrumScale::Wide ? plot : spectrumGridPlot(plot, scale);
}
} // namespace

juce::Rectangle<float> paintSpectrumGrid(juce::Graphics &g, juce::Rectangle<float> area, SpectrumScale scale) {
    const auto &axis = spectrumAxis(scale);
    const auto plot = spectrumGridPlot(area, scale);
    const auto scaleWidth = look::fontCaption * 3.f;
    const bool labels = plot != area;
    const bool topRuler = scale == SpectrumScale::Device;
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
    const auto title = bounds.reduced(5).withHeight(14);
    g.setFont(look::font(10.f, true));
    g.setColour(look::textOnBand);
    g.drawText(frozen_ ? "Filled Spectrum / Frozen" : "Filled Spectrum", title, juce::Justification::centredLeft);
    const auto axes = filledSpectrumPlot(bounds);
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

void SharedSpectrumView::enableSpectrumCursor() {
    spectrumCursorEnabled_ = true;
    updateSpectrumCursorInput();
}
bool SharedSpectrumView::spectrumCursorActive() const noexcept {
    return spectrumCursorEnabled_ && (spectrumMode() || mode_ == Mode::Spectrogram);
}
juce::Rectangle<float> spectrogram::rulerPlot(juce::Rectangle<float> base, bool visible) noexcept {
    const auto gutter = 4.f * look::fontCaption + static_cast<float>(look::gap);
    return visible && base.getWidth() >= 2.f * gutter ? base.withTrimmedLeft(gutter) : base;
}
juce::Rectangle<float> SharedSpectrumView::spectrogramPlot(juce::Rectangle<float> base) const noexcept {
    return spectrogram::rulerPlot(base, spectrogramRuler_);
}
juce::Rectangle<float> SharedSpectrumView::spectrumCursorBounds() const noexcept {
    if (mode_ == Mode::Spectrogram) return spectrogramPlot(analysisPlot(analysisArea(getLocalBounds())));
    return spectrumCursorPlot(getLocalBounds(), spectrumScale_);
}
void SharedSpectrumView::updateSpectrumCursorInput() {
    const bool active = spectrumCursorActive();
    if (!active) spectrumCursorPosition_.reset();
    setInterceptsMouseClicks(active, mode_ == Mode::ClassicScope && !compact_);
    if (active) setTooltip({});
    else if (getTooltip().isEmpty()) describeAnalysisView(*this, mode_);
}
juce::String SharedSpectrumView::SpectrumCursorReading::text() const {
    // Follow the DAW's middle-C spelling while also naming pitches above the MIDI keyboard.
    const auto note = juce::roundToInt(69. + 12. * std::log2(frequency / 440.));
    auto reading = juce::String(frequency, 1) + " Hz   " + pitchClassName(note) + juce::String(note / 12 - 2) +
        "   " + juce::String(level, 1) + " dBFS";
    if (time) {
        const auto milliseconds = 1000. * (static_cast<double>(time->sample) / time->sampleRate);
        reading << (time->heard ? "   Heard " : "   Capture ") << static_cast<juce::int64>(time->sample)
                << " samples   " << juce::String(milliseconds, 1) << " ms";
    }
    return reading;
}
std::optional<SharedSpectrumView::SpectrumCursorReading>
SharedSpectrumView::spectrumCursorReading(juce::Point<float> point) const noexcept {
    if (!spectrumCursorActive() || !shown_ || !std::isfinite(shown_->sampleRate)) return std::nullopt;
    const auto plot = spectrumCursorBounds();
    if (plot.getWidth() <= 1.f || plot.getHeight() <= 1.f || !plot.contains(point)) return std::nullopt;
    if (mode_ == Mode::Spectrogram) return spectrogramCursorReading(point, plot);
    if (!scaledSpectrum_) return std::nullopt;
    const auto &axis = spectrumAxis(spectrumScale_);
    const bool wide = spectrumScale_ == SpectrumScale::Wide;
    const auto nyquist = shown_->sampleRate * .5;
    const auto highHz = wide ? std::min(axis.highHz, nyquist) : axis.highHz;
    if (highHz <= axis.lowHz) return std::nullopt;
    const auto logWidth = wide ? std::log(highHz / axis.lowHz) : axis.logWidth;
    // Use the same pixel inset and frequency span as the plotted trace.
    const float inset = wide ? 0.f : 1.f;
    const auto x = std::min(1.f, (point.x - plot.getX()) / (plot.getWidth() - inset));
    const auto y = std::min(1.f, (point.y - plot.getY()) / (plot.getHeight() - inset));
    const auto frequency = std::min(highHz, axis.lowHz * std::exp(static_cast<double>(x) * logWidth));
    if (frequency > nyquist) return std::nullopt;
    const auto db = static_cast<float>(axis.topDb) - y * static_cast<float>(axis.topDb - axis.bottomDb);
    return SpectrumCursorReading{frequency, db, std::nullopt};
}
std::optional<SharedSpectrumView::SpectrumCursorReading>
SharedSpectrumView::spectrogramCursorReading(juce::Point<float> point, juce::Rectangle<float> plot) const noexcept {
    if (!raster_ || raster_->image.isNull() || raster_->epoch != shown_->sourceEpoch ||
        raster_->revision != shown_->revision || raster_->sampleRate <= 0.) return std::nullopt;
    const auto visibleX = std::min(static_cast<int>((point.x - plot.getX()) / plot.getWidth() * Raster::width),
                                   Raster::width - 1);
    const auto rotation = historyMotion_ == HistoryMotion::Sweep ? raster_->sweepRotation() : 0;
    const auto x = (visibleX + Raster::width - rotation) % Raster::width;
    const auto downward = (static_cast<double>(point.y) - plot.getY()) / plot.getHeight();
    const auto y = std::min(static_cast<int>(downward * Raster::height), Raster::height - 1);
    if (raster_->intensity[static_cast<std::size_t>(y * Raster::width + x)] == Raster::gap) return std::nullopt;
    const auto behind = static_cast<std::uint64_t>(Raster::width - 1 - x);
    if (behind >= raster_->column) return std::nullopt;
    const auto serial = raster_->column - behind;
    const auto &columns = shown_->columns;
    const auto found = std::lower_bound(columns.begin(), columns.end(), serial,
                                         [](const auto &column, auto at) { return column.serial < at; });
    if (found == columns.end() || found->serial != serial || !found->binsDbfs) return std::nullopt;
    const auto frequency = raster_->frequencyAt(1. - downward);
    const auto peak = raster_->rowPeak(*found->binsDbfs, static_cast<std::size_t>(Raster::height - 1 - y));
    // Monitoring can fill several columns at one stopped transport position.
    // The capture sequence still records where those samples entered the tap.
    const auto sample = shown_->advancing ? found->firstStamp : found->firstSequence;
    return SpectrumCursorReading{frequency, peak, SpectrumCursorReading::Time{sample, raster_->sampleRate, shown_->advancing}};
}
void SharedSpectrumView::mouseEnter(const juce::MouseEvent &event) { mouseMove(event); }
void SharedSpectrumView::mouseMove(const juce::MouseEvent &event) {
    if (spectrumCursorPosition_ && *spectrumCursorPosition_ == event.position) return;
    const bool inside = spectrumCursorActive() && spectrumCursorBounds().contains(event.position);
    const auto next = inside ? std::optional{event.position} : std::nullopt;
    if (spectrumCursorPosition_ == next) return;
    spectrumCursorPosition_ = next;
    if (shown_) repaint();
}
void SharedSpectrumView::mouseExit(const juce::MouseEvent &) {
    if (!spectrumCursorPosition_) return;
    spectrumCursorPosition_.reset();
    repaint();
}

void SharedSpectrumView::paintPhaseBands(juce::Graphics &g, juce::Rectangle<float> bands) const {
    const juce::Graphics::ScopedSaveState bandStyle(g);
    const auto rowHeight = static_cast<float>(look::fieldHeight);
    const auto caption = bands.removeFromTop(std::min(rowHeight, bands.getHeight() * .5f));
    const auto mode = phaseBandDisplay_;
    const auto *spatial = shown_->phaseBandSpatial.get();
    const SharedAnalysis::PhaseBandCorrelations *values = shown_->phaseBands.get();
    const char *title = "By band";
    if (mode == PhaseBandDisplay::Width) {
        title = "Band width (side energy)";
        values = spatial ? &spatial->width : nullptr;
    } else if (mode == PhaseBandDisplay::Pan) {
        title = "Band pan (L - / R +)";
        values = spatial ? &spatial->pan : nullptr;
    }
    g.setFont(look::font(look::fontCaption));
    g.setColour(look::textOnBand);
    g.drawText(title, caption, juce::Justification::centredLeft);

    const bool widthDisplay = mode == PhaseBandDisplay::Width;
    const auto width = bands.getWidth() / static_cast<float>(SharedAnalysis::kPhaseBandCount);
    const auto baseline = widthDisplay ? bands.getBottom() : bands.getCentreY();
    const auto gap = std::min(width * .25f, look::line);
    const auto height = bands.getHeight();
    const auto neutralHeight = std::min(height, look::line);
    const auto neutralY = baseline - neutralHeight * (widthDisplay ? 1.f : .5f);
    for (std::size_t band = 0; band < SharedAnalysis::kPhaseBandCount; ++band) {
        const auto bar = bands.removeFromLeft(width).withTrimmedRight(gap);
        g.setColour(look::outline);
        g.fillRect(bar);
        g.setColour(look::display);
        g.fillRect(bar.withY(neutralY).withHeight(neutralHeight));
        if (!values || !(*values)[band]) continue;
        const auto value = std::clamp(*(*values)[band], widthDisplay ? 0.f : -1.f, 1.f);
        const auto length = std::abs(value) * height * (widthDisplay ? 1.f : .5f);
        if (length < neutralHeight) {
            g.setColour(look::textOnBand);
            g.fillRect(bar.withY(neutralY).withHeight(neutralHeight));
        } else {
            if (widthDisplay) g.setColour(look::accent);
            else if (value > 0.f) g.setColour(look::meterLevel);
            else g.setColour(mode == PhaseBandDisplay::Pan ? look::value : look::record);
            g.fillRect(bar.withY(value > 0.f ? baseline - length : baseline).withHeight(length));
        }
    }
}

void spectrogram::paintRuler(juce::Graphics &g, juce::Rectangle<float> base, juce::Rectangle<float> plot,
                            SpectrogramScale scale, double lowHz, double topHz) {
    const auto labelHeight = look::fontCaption;
    const auto left = plot.getX();
    if (left <= base.getX() || plot.getHeight() < labelHeight || topHz <= lowHz) return;
    const auto lowPosition = spectrogramPosition(scale, lowHz);
    const auto span = spectrogramPosition(scale, topHz) - lowPosition;
    if (!(span > 0.)) return;
    const auto clip = plot.toNearestInt();
    const auto tickWidth = labelHeight * .65f;
    g.setColour(look::outline);
    g.drawVerticalLine(juce::roundToInt(left), plot.getY(), plot.getBottom());
    g.setFont(look::font(labelHeight));
    float labelEnd = plot.getY();
    double lastHz = std::numeric_limits<double>::infinity();
    for (const double hz : {topHz, 10000., 1000., 100., lowHz}) {
        if (hz >= lastHz || hz < lowHz) continue;
        lastHz = hz;
        const auto fraction = (spectrogramPosition(scale, hz) - lowPosition) / span;
        const auto y = plot.getBottom() - static_cast<float>(fraction) * plot.getHeight();
        const auto pixel = std::clamp(juce::roundToInt(y), clip.getY(), clip.getBottom() - 1);
        g.setColour(look::textOnBand);
        g.drawHorizontalLine(pixel, left - tickWidth, left);
        const auto labelTop = std::clamp(y - labelHeight * .5f, plot.getY(), plot.getBottom() - labelHeight);
        if (labelTop < labelEnd) continue;
        const auto label = hz >= 1000. ? juce::String(hz / 1000., std::fmod(hz, 1000.) == 0. ? 0 : 1) + "k"
                                       : juce::String(juce::roundToInt(hz));
        const auto labelWidth = left - base.getX() - tickWidth - static_cast<float>(look::border);
        g.drawText(label, juce::Rectangle<float>(base.getX(), labelTop, labelWidth, labelHeight),
                   juce::Justification::centredRight, true);
        labelEnd = labelTop + labelHeight;
    }
}

void SharedSpectrumView::paintSpectrogramRuler(juce::Graphics &g, juce::Rectangle<float> base,
                                              juce::Rectangle<float> plot) const {
    if (raster_) spectrogram::paintRuler(g, base, plot, raster_->scale, Raster::lowestHz, raster_->topHz);
}

void SharedSpectrumView::paint(juce::Graphics &g) {
    if (mode_ == Mode::ChannelReadout) { paintChannelReadout(g); return; }
    if (mode_ == Mode::FilledSpectrum) { paintFilledSpectrum(g); return; }
    if (mode_ == Mode::ThirdOctaveBands) { paintThirdOctaveBands(g); return; }
    if (mode_ == Mode::ColourWaveform) { paintColourWaveform(g); return; }
    g.fillAll(look::display);
    if (mode_ == Mode::ClassicScope) return;
    const auto area = analysisArea(getLocalBounds());
    if (area.isEmpty()) return;
    g.setColour(look::textOnBand);
    g.setFont(look::font(10.f, true));
    if (compact_) {
        const auto heading = mode_ == Mode::Spectrogram ? "Spectrogram" : "Spectrum";
        g.drawText(heading, area.withHeight(14.f), juce::Justification::centredLeft);
        const auto plot = area.withTrimmedTop(16.f);
        if (mode_ == Mode::Spectrogram) {
            if (raster_ && !raster_->image.isNull()) {
                g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
                g.drawImage(raster_->image, plot);
            }
        } else if (spectrumMode()) {
            const auto grid = paintSpectrumGrid(g, plot, spectrumScale_);
            if (shown_ && scaledSpectrum_) scaledSpectrum_->paint(g, grid);
        }
        return;
    }
    static constexpr std::array names{"Spectrum", "Scope", "Sample peak / RMS", "Phase / L-R correlation", "Spectrogram",
                                      "Channel analysis", "Spectrum / 1 s average", "Classic Scope", "Realtime Maximum",
                                      "Cumulative Maximum"};
    const char *points[]{"Post Mixer", "Post FX", "Pre FX", "Device In", "Device Out"};
    const auto modeIndex = static_cast<std::size_t>(mode_);
    const auto title = modeIndex < names.size() ? juce::String(names[modeIndex]) : getTitle();
    g.drawText(title + " / " + points[static_cast<int>(point_)] +
                   (frozen_ ? " / Frozen" : ""), area.withHeight(14.f), juce::Justification::centredLeft, true);
    const bool mainMeter = mode_ == Mode::Meters && mainMeter_.has_value();
    const auto meterHeight = mainMeter ? 38.f : 0.f;
    const auto basePlot = analysisPlot(area, meterHeight);
    const auto plot = mode_ == Mode::Spectrogram ? spectrogramPlot(basePlot) : basePlot;
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
    } else if (mode_ == Mode::Phase) {
        auto vectorArea = plot;
        if (phaseBandsEnabled_) {
            const auto rowHeight = static_cast<float>(look::fieldHeight);
            paintPhaseBands(g, vectorArea.removeFromBottom(std::min(2.f * rowHeight, plot.getHeight() * .4f)));
        }
        // The stereo picture of the analysers queue; the point cloud below remains for a view without its trail.
        if (phaseTrail_) phaseTrail_->paint(g, vectorArea);
        else {
        const auto radius = std::min(vectorArea.getWidth(), vectorArea.getHeight()) * .46f;
        const auto centre = vectorArea.getCentre();
        g.setColour(look::outline); g.drawEllipse(centre.x - radius, centre.y - radius, radius * 2, radius * 2, 1.f);
        g.drawVerticalLine(juce::roundToInt(centre.x), centre.y - radius, centre.y + radius);
        g.drawHorizontalLine(juce::roundToInt(centre.y), centre.x - radius, centre.x + radius);
        g.setColour(look::value.withAlpha(.55f));
        const auto size = std::min(shown_->scopeLeft.size(), shown_->scopeRight.size());
        // Keep the same paint budget without sampling only one waveform phase.
        const auto first = size - std::min<std::size_t>(size, 1024);
        for (auto i = first; i < size; ++i) {
            const float l = shown_->scopeLeft[i], r = shown_->scopeRight[i];
            g.fillRect(centre.x + std::clamp((l - r) * .5f, -1.f, 1.f) * radius,
                       centre.y - std::clamp((l + r) * .5f, -1.f, 1.f) * radius, 1.4f, 1.4f);
        }
        }
    } else if (mode_ == Mode::Spectrogram && raster_ && !raster_->image.isNull()) {
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        if (historyMotion_ == HistoryMotion::Sweep) raster_->paintSweep(g, plot);
        else g.drawImage(raster_->image, plot);
        paintSpectrogramRuler(g, basePlot, plot);
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
    const auto offsets = spectrumProjection_.offsets();
    const bool tilted = spectrumSlope_ == SpectrumSlope::Tilt45;
    const auto bins = spectrumBins();
    const bool projectionReady = !tilted || offsets.size() == bins.size();
    const auto spectrumPlot = scaled ? scaledPlot : plot;
    if (scaledSpectrum_) scaledSpectrum_->paint(g, spectrumPlot, look::value, true);
    if (supportsSpectrumOverlays(mode_)) paintSpectrumOverlays(g, spectrumPlot);
    if (spectrumCursorPosition_) {
        const auto pointer = *spectrumCursorPosition_;
        if (const auto reading = spectrumCursorReading(pointer)) {
            g.setColour(look::gridLine);
            g.drawVerticalLine(juce::roundToInt(pointer.x), spectrumPlot.getY(), spectrumPlot.getBottom());
            g.drawHorizontalLine(juce::roundToInt(pointer.y), spectrumPlot.getX(), spectrumPlot.getRight());
            g.setColour(look::textOnBand);
            g.setFont(look::font(look::fontCaption));
            const auto textRow = spectrumPlot.withTrimmedTop(spectrumPlot.getHeight() - look::fontCaption)
                                           .reduced(static_cast<float>(look::border), 0.f);
            g.drawText(reading->text(), textRow, juce::Justification::centredRight, true);
        }
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
    modeBox.addItem("Cumulative Average", static_cast<int>(SharedSpectrumView::Mode::SpectrumCumulativeAverage) + 1);
    modeBox.addItem("Realtime Sigma", static_cast<int>(SharedSpectrumView::Mode::SpectrumRealtimeSigma) + 1);
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
    spectrumBlockBox_.setTitle("Spectrum block size");
    spectrumBlockBox_.setTooltip("Set the analysis block in samples for this Spectrum and its pinned channels.");
    for (std::size_t i = 0; i < SharedSpectrumView::spectrumBlockSizes.size(); ++i) {
        const auto size = SharedSpectrumView::spectrumBlockSizes[i];
        spectrumBlockBox_.addItem(size == 0 ? juce::String("Block: Auto") : "Block: " + juce::String(size),
                                  static_cast<int>(i) + 1);
    }
    spectrumBlockBox_.setSelectedId(1, juce::dontSendNotification);
    spectrumBlockBox_.onChange = [this] {
        const auto index = spectrumBlockBox_.getSelectedItemIndex();
        if (index >= 0 && index < static_cast<int>(SharedSpectrumView::spectrumBlockSizes.size()))
            view.setSpectrumBlockSize(SharedSpectrumView::spectrumBlockSizes[static_cast<std::size_t>(index)]);
    };
    addChildComponent(spectrumBlockBox_);
    spectrumOverlapBox_.setTitle("Spectrum overlap");
    spectrumOverlapBox_.setTooltip("Set how much consecutive analysis blocks overlap.");
    for (std::size_t i = 0; i < SharedSpectrumView::spectrumOverlaps.size(); ++i)
        spectrumOverlapBox_.addItem("Overlap: " + juce::String(SharedSpectrumView::spectrumOverlaps[i]) + "%",
                                    static_cast<int>(i) + 1);
    spectrumOverlapBox_.setSelectedId(3, juce::dontSendNotification);
    spectrumOverlapBox_.onChange = [this] {
        const auto index = spectrumOverlapBox_.getSelectedItemIndex();
        if (index >= 0 && index < static_cast<int>(SharedSpectrumView::spectrumOverlaps.size()))
            view.setSpectrumOverlap(SharedSpectrumView::spectrumOverlaps[static_cast<std::size_t>(index)]);
    };
    addChildComponent(spectrumOverlapBox_);
    spectrumSmoothingBox_.setTitle("Spectrum smoothing");
    spectrumSmoothingBox_.setTooltip("Smooth the displayed traces over a fraction of an octave or more.");
    for (std::size_t i = 0; i < kSpectrumSmoothingChoices.size(); ++i)
        spectrumSmoothingBox_.addItem("Smooth: " + juce::String(kSpectrumSmoothingChoices[i].label.data()),
                                      static_cast<int>(i) + 1);
    spectrumSmoothingBox_.setSelectedId(1, juce::dontSendNotification);
    spectrumSmoothingBox_.onChange = [this] {
        const auto index = spectrumSmoothingBox_.getSelectedItemIndex();
        if (index >= 0) view.setSpectrumSmoothing(static_cast<std::size_t>(index));
    };
    addChildComponent(spectrumSmoothingBox_);
    spectrogramScaleBox.setTitle("Spectrogram frequency scale");
    spectrogram::configureScaleBox(spectrogramScaleBox);
    spectrogramScaleBox.onChange = [this] {
        const auto id = spectrogramScaleBox.getSelectedId();
        if (id > 0) view.setSpectrogramScale(static_cast<SharedSpectrumView::SpectrogramScale>(id - 1));
    };
    addAndMakeVisible(spectrogramScaleBox);
    const auto configureLevel = [this](juce::Slider &control, const juce::String &label, double low, double high,
                                       double initial) {
        control.setTitle("Spectrogram " + label.toLowerCase());
        control.setSliderStyle(juce::Slider::LinearBar);
        control.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 0, 0);
        control.setSliderSnapsToMousePosition(false);
        control.setRange(low, high, 1.);
        control.setDoubleClickReturnValue(true, initial);
        control.textFromValueFunction = [label](double value) {
            return label + " " + juce::String(juce::roundToInt(value)) + " dB";
        };
        control.valueFromTextFunction = [&control, label](const juce::String &text) {
            const auto valueText = text.trim();
            const auto bare = valueText.startsWithIgnoreCase(label) ? valueText.substring(label.length()).trim() : valueText;
            const auto numeric = bare.trimCharactersAtStart("+");
            const auto bytes = numeric.endsWithIgnoreCase("dB") ? numeric.dropLastCharacters(2).trim().toStdString()
                                                                 : numeric.toStdString();
            const auto *last = bytes.data() + bytes.size();
            int value = 0;
            const auto [end, error] = std::from_chars(bytes.data(), last, value);
            return error == std::errc{} && end == last ? static_cast<double>(value) : control.getValue();
        };
        control.setValue(initial, juce::dontSendNotification);
        control.updateText();
        addAndMakeVisible(control);
    };
    const auto levels = view.spectrogramLevels();
    configureLevel(spectrogramFloor_, "Floor", dsp::kDbFloor, spectrogramTopDb - 1.f, levels.floor);
    configureLevel(spectrogramCeiling_, "Ceiling", dsp::kDbFloor + 1.f, spectrogramTopDb, levels.ceiling);
    spectrogramFloor_.onValueChange = [this] {
        auto next = view.spectrogramLevels();
        next.floor = std::min(static_cast<float>(spectrogramFloor_.getValue()), next.ceiling - 1.f);
        spectrogramFloor_.setValue(next.floor, juce::dontSendNotification);
        view.setSpectrogramLevels(next);
    };
    spectrogramCeiling_.onValueChange = [this] {
        auto next = view.spectrogramLevels();
        next.ceiling = std::max(static_cast<float>(spectrogramCeiling_.getValue()), next.floor + 1.f);
        spectrogramCeiling_.setValue(next.ceiling, juce::dontSendNotification);
        view.setSpectrogramLevels(next);
    };
    spectrogramGammaBox_.setTitle("Spectrogram gamma");
    spectrogramGammaBox_.setTooltip("Reveal quiet detail below 1.00 or darken the middle of the colour scale above 1.00.");
    for (std::size_t i = 0; i < spectrogramGammaValues.size(); ++i) {
        const int id = static_cast<int>(i) + 1;
        const auto gamma = spectrogramGammaValues[i];
        spectrogramGammaBox_.addItem("Gamma " + juce::String(gamma, 2), id);
        if (gamma == view.spectrogramGamma()) spectrogramGammaBox_.setSelectedId(id, juce::dontSendNotification);
    }
    spectrogramGammaBox_.onChange = [this] {
        const int id = spectrogramGammaBox_.getSelectedId();
        if (id > 0) view.setSpectrogramGamma(spectrogramGammaValues[static_cast<std::size_t>(id - 1)]);
    };
    addAndMakeVisible(spectrogramGammaBox_);
    spectrogramRulerButton_.setTitle("Spectrogram frequency ruler");
    spectrogramRulerButton_.setTooltip("Label the vertical frequency scale beside the history.");
    spectrogramRulerButton_.onClick = [this] { view.setSpectrogramRuler(spectrogramRulerButton_.getToggleState()); };
    addAndMakeVisible(spectrogramRulerButton_);
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
    resetButton.onClick = [this] {
        view.clear();
        spectrogramPngButton_.setEnabled(false);
    };
    spectrogramPngButton_.setTitle("Export Spectrogram PNG");
    spectrogramPngButton_.setTooltip("Save the displayed spectrogram history as a PNG image.");
    spectrogramPngButton_.onClick = [this] { exportSpectrogramPng(); };
    addAndMakeVisible(spectrogramPngButton_);
    expandButton.onClick = [this] { if (onExpand) onExpand(view.track()); };
    addAndMakeVisible(freezeButton); addAndMakeVisible(resetButton); addAndMakeVisible(expandButton); addAndMakeVisible(view);
    view.enableSpectrumCursor();
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
    const bool spectral = SharedSpectrumView::supportsSpectrumOverlays(mode) || isMaximumSpectrum(mode);
    const bool filled = mode == SharedSpectrumView::Mode::FilledSpectrum;
    resetButton.setTooltip(mode == SharedSpectrumView::Mode::SpectrumCumulativeMaximum || filled
        ? "Clear this view and restart its held maximum."
        : mode == SharedSpectrumView::Mode::SpectrumCumulativeAverage
        ? "Clear this view and restart its cumulative average."
        : "Clear this view's display interval; other analyzer views and audio are unchanged.");
    channelBox.setEnabled(spectral || filled || mode == SharedSpectrumView::Mode::Spectrogram);
    const bool usesSpectrumSettings = spectral && !modeBox.isVisible();
    spectrumBlockBox_.setVisible(usesSpectrumSettings);
    spectrumOverlapBox_.setVisible(usesSpectrumSettings);
    spectrumSmoothingBox_.setVisible(usesSpectrumSettings);
    const bool spectrogram = mode == SharedSpectrumView::Mode::Spectrogram;
    historyBox.setVisible(spectrogram);
    spectrogramScaleBox.setVisible(spectrogram);
    spectrogramFloor_.setVisible(spectrogram);
    spectrogramCeiling_.setVisible(spectrogram);
    spectrogramGammaBox_.setVisible(spectrogram);
    spectrogramRulerButton_.setVisible(spectrogram);
    paletteBox.setVisible(spectrogram);
    paletteBox.setEnabled(spectrogram);
    reverseButton.setVisible(spectrogram);
    reverseButton.setEnabled(spectrogram);
    spectrogramPngButton_.setVisible(spectrogram);
    spectrogramPngButton_.setEnabled(view.hasSpectrogramImage());
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

namespace {
juce::Result writeSpectrogramPng(const juce::Image &image, const juce::File &target,
                                 const std::filesystem::path &path, const media::ReplaceHooks &replace) {
    const juce::TemporaryFile staging(target);
    {
        auto output = staging.getFile().createOutputStream();
        if (!output || !juce::PNGImageFormat{}.writeImageToStream(image, *output))
            return juce::Result::fail("Could not write the PNG image.");
        output->flush();
        if (output->getStatus().failed()) return output->getStatus();
    }
    const auto source = media::pathFromUtf8(staging.getFile().getFullPathName().toStdString());
    const auto result = replace.expectedTarget ? media::replaceFile(source, path, replace)
        : media::publishFile(source, path, {}, media::PublishPolicy::AtomicOnly);
    if (result.status == media::PublishStatus::published)
        return result.warning.empty() ? juce::Result::ok() : juce::Result::fail(juce::String(result.warning));
    return juce::Result::fail(result.error.empty() ? "Could not save the PNG image." : juce::String(result.error));
}
}

void AnalysisPane::exportSpectrogramPng() {
    if (!spectrogramChooser_) exportSpectrogramImage(view.spectrogramImage(), *this, "Spectrogram.png");
}

void AnalysisPane::exportSpectrogramImage(juce::Image snapshot, juce::Component &anchor, const juce::String &fileName) {
    if (spectrogramChooser_ || !snapshot.isValid()) return;
    const auto initial = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(fileName);
    spectrogramChooser_ = std::make_unique<juce::FileChooser>("Export Spectrogram PNG", initial, "*.png", true, false, &anchor);
    const auto owner = juce::Component::SafePointer<AnalysisPane>(this);
    const auto show = [parent = juce::Component::SafePointer<juce::Component>(&anchor)](const juce::String &message) {
        if (auto *component = parent.getComponent())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export Spectrogram PNG",
                                                   message, {}, component);
    };
    const int chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                             juce::FileBrowserComponent::warnAboutOverwriting;
    spectrogramChooser_->launchAsync(chooserFlags, [owner, image = std::move(snapshot), show](const juce::FileChooser &chosen) {
        juce::MessageManager::callAsync([owner] {
            if (auto *pane = owner.getComponent()) pane->spectrogramChooser_.reset();
        });
        const auto selected = chosen.getResult();
        if (selected == juce::File{}) return;
        const auto target = selected.hasFileExtension("png") ? selected
            : selected.getParentDirectory().getChildFile(selected.getFileName() + ".png");
        const auto path = media::pathFromUtf8(target.getFullPathName().toStdString());
        media::ReplaceHooks replace;
        if (target.exists()) {
            if (target != selected) {
                show(target.getFileName() + " already exists. Select that file to replace it.");
                return;
            }
            std::error_code error;
            replace.expectedTarget = media::readFileIdentity(path, error);
            if (!replace.expectedTarget) {
                show("Could not open the selected destination for replacement.");
                return;
            }
        }
        if (!juce::Thread::launch(juce::Thread::Priority::low, [image, target, path, replace = std::move(replace), show] {
            const auto result = writeSpectrogramPng(image, target, path, replace);
            if (result.failed()) juce::MessageManager::callAsync([show, result] { show(result.getErrorMessage()); });
        })) show("Could not start the PNG export.");
    });
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
    if (spectrogramPngButton_.isVisible()) spectrogramPngButton_.setEnabled(view.hasSpectrogramImage());
}
void AnalysisPane::usePageControls() {
    modeBox.setVisible(false);
    sourceBox.setVisible(false);
    setMode(view.mode());
}
void AnalysisPane::resized() {
    const std::array<juce::Component *, 21> controls{&modeBox, &sourceBox, &pointBox, &channelBox, &paletteBox, &slopeBox,
        &scaleBox, &spectrumBlockBox_, &spectrumOverlapBox_, &spectrumSmoothingBox_, &spectrogramScaleBox,
        &spectrogramFloor_, &spectrogramCeiling_, &spectrogramGammaBox_, &spectrogramRulerButton_,
        &historyBox, &reverseButton, &freezeButton, &resetButton, &expandButton, &spectrogramPngButton_};
    const bool pageControls = !modeBox.isVisible();
    const int maxColumns = spectrumOverlapBox_.isVisible() ? 5 : 4;
    const int columns = std::clamp(getWidth() / (pageControls ? look::mixPageWidth : 96), 1, maxColumns);
    const int count = static_cast<int>(std::ranges::count_if(controls, [](const auto *control) { return control->isVisible(); }));
    const int rows = std::max(1, (count + columns - 1) / columns);
    const bool compactFields = pageControls || view.mode() == SharedSpectrumView::Mode::Spectrogram;
    const int height = std::min(compactFields ? look::fieldHeight : 25, std::max(0, getHeight() / rows));
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
