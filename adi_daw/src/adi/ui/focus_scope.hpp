// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "shared_analysis.hpp"
#include "adi/dsp/true_peak.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/ui/meters.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace adi::ui {
struct FocusScopeRange {
    std::int64_t first = 0;
    std::size_t frames = 0;
    double sampleRate = 0.;
    bool operator==(const FocusScopeRange &) const = default;
};
struct FocusScopeBar {
    std::int64_t firstTick = 0, nextTick = 0;
    FocusScopeRange range;
};
enum class FocusScopeWindow { Beat = 1, Bar, Sixteenth, TwoBars, FourBars, Milliseconds85, Milliseconds170, Milliseconds340 };

inline constexpr int focusScopeWindowMilliseconds(FocusScopeWindow window) noexcept {
    switch (window) {
    case FocusScopeWindow::Milliseconds85: return 85;
    case FocusScopeWindow::Milliseconds170: return 170;
    case FocusScopeWindow::Milliseconds340: return 340;
    default: return 0;
    }
}

// Use the Arrangement's signature boundaries and the driver's nearest-sample
// convention. Keep absolute edges so consecutive windows never gain a rounding gap.
inline std::optional<FocusScopeBar> focusScopeWindowAtTick(std::int64_t tick, double sampleRate,
                                                          const engine::TempoMap &tempo,
                                                          const std::vector<textproj::Meter> &meters,
                                                          FocusScopeWindow window) noexcept {
    if (tick < 0 || !std::isfinite(sampleRate) || sampleRate <= 0. || tempo.events.empty()) return {};
    const auto &meter = meterAt(tick, meters);
    const auto barTicks = meter.barTicks();
    if (meter.start_ticks < 0 || meter.start_ticks > tick || barTicks <= 0) return {};
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    auto windowTicks = barTicks;
    switch (window) {
    case FocusScopeWindow::Beat: windowTicks = textproj::kWhole / meter.denominator; break;
    case FocusScopeWindow::Sixteenth: windowTicks = textproj::kWhole / 16; break;
    case FocusScopeWindow::TwoBars:
    case FocusScopeWindow::FourBars: {
        const int bars = window == FocusScopeWindow::TwoBars ? 2 : 4;
        if (barTicks > limit / bars) return {};
        windowTicks = bars * barTicks;
        break;
    }
    case FocusScopeWindow::Bar: break;
    case FocusScopeWindow::Milliseconds85:
    case FocusScopeWindow::Milliseconds170:
    case FocusScopeWindow::Milliseconds340: return {};
    }
    if (windowTicks <= 0) return {};
    const auto intoSignature = tick - meter.start_ticks;
    const auto fromGridStart = window == FocusScopeWindow::Sixteenth ? intoSignature % barTicks : intoSignature;
    const auto intoWindow = fromGridStart % windowTicks;
    const auto firstTick = tick - intoWindow;
    // A sixteenth restarts at every bar. At an irregular bar end its final
    // capture may be shorter than a full note.
    if (window == FocusScopeWindow::Sixteenth)
        windowTicks = std::min(windowTicks, barTicks - (fromGridStart - intoWindow));
    const auto nextMeter = std::upper_bound(meters.begin(), meters.end(), tick,
        [](std::int64_t position, const textproj::Meter &signature) { return position < signature.start_ticks; });
    if (nextMeter != meters.end()) windowTicks = std::min(windowTicks, nextMeter->start_ticks - firstTick);
    if (windowTicks > limit - firstTick) return {};
    const auto nextTick = firstTick + windowTicks;

    const auto sampleAtTick = [&](std::int64_t edge) -> std::optional<std::int64_t> {
        const auto sample = std::round(static_cast<long double>(tempo.ticksToSeconds(edge)) * sampleRate);
        const auto sampleLimit = std::ldexp(1.L, std::numeric_limits<std::int64_t>::digits);
        if (!(sample >= 0. && sample < sampleLimit)) return {};
        return static_cast<std::int64_t>(sample);
    };
    const auto start = sampleAtTick(firstTick);
    const auto finish = sampleAtTick(nextTick);
    if (!start || !finish || *finish <= *start || !std::in_range<std::size_t>(*finish - *start)) return {};
    return FocusScopeBar{firstTick, nextTick, {*start, static_cast<std::size_t>(*finish - *start), sampleRate}};
}
inline std::optional<FocusScopeBar> focusScopeBarAtTick(std::int64_t tick, double sampleRate,
                                                       const engine::TempoMap &tempo,
                                                       const std::vector<textproj::Meter> &meters) noexcept {
    return focusScopeWindowAtTick(tick, sampleRate, tempo, meters, FocusScopeWindow::Bar);
}
inline std::optional<FocusScopeBar> focusScopeWindowAtSample(std::int64_t heard, double sampleRate,
                                                            const engine::TempoMap &tempo,
                                                            const std::vector<textproj::Meter> &meters,
                                                            FocusScopeWindow window) noexcept {
    if (heard < 0 || !std::isfinite(sampleRate) || sampleRate <= 0.) return {};
    const auto seconds = static_cast<double>(heard) / sampleRate;
    if (!std::isfinite(seconds)) return {};
    const auto next = focusScopeWindowAtTick(tempo.secondsToTicks(seconds), sampleRate, tempo, meters, window);
    // A grid edge rounded down to a sample can precede its theoretical tick.
    // A capture starting on that sample belongs to the next half-open window.
    if (next && heard >= next->range.first + static_cast<std::int64_t>(next->range.frames))
        return focusScopeWindowAtTick(next->nextTick, sampleRate, tempo, meters, window);
    return next;
}
struct FocusSumReading {
    std::array<double, 2> peak{}, rms{};
};
struct FocusSumColumn {
    std::array<double, 2> low{}, high{};
};
class FocusScopePhase {
  public:
    void add(double a, double b) noexcept {
        aPower_ += a * a;
        bPower_ += b * b;
        cross_ += a * b;
    }
    [[nodiscard]] std::optional<double> reading() const noexcept {
        if (aPower_ <= 0. || bPower_ <= 0.) return {};
        return std::clamp(cross_ / std::sqrt(aPower_ * bPower_), -1., 1.);
    }

  private:
    double aPower_ = 0., bPower_ = 0., cross_ = 0.;
};
inline constexpr bool focusScopeIsFolded(FocusScopeChannels channels) noexcept {
    return channels == FocusScopeChannels::Mid || channels == FocusScopeChannels::Side;
}
inline double focusScopeFoldSample(FocusScopeChannels fold, double left, double right) noexcept {
    return (fold == FocusScopeChannels::Side ? left - right : left + right) * .5;
}
inline FocusScopeLayerAdjustment focusScopeBoundedAdjustment(FocusScopeLayerAdjustment adjustment) noexcept {
    constexpr auto limit = static_cast<int>(SharedAnalysis::kMaxFftSize);
    adjustment.offsetSamples = std::clamp(adjustment.offsetSamples, -limit, limit);
    return adjustment;
}
inline float focusScopeDisplayedSample(std::span<const float> samples, std::size_t first,
                                       std::size_t frames, std::size_t at,
                                       FocusScopeLayerAdjustment adjustment) noexcept {
    if (at >= frames || first > samples.size() || frames > samples.size() - first) return 0.f;
    const auto offset = focusScopeBoundedAdjustment(adjustment).offsetSamples;
    if (offset > 0) {
        const auto shift = static_cast<std::size_t>(offset);
        if (at < shift) return 0.f;
        at -= shift;
    } else if (offset < 0) {
        const auto shift = static_cast<std::size_t>(-offset);
        if (shift >= frames - at) return 0.f;
        at += shift;
    }
    const float sample = samples[first + at];
    return adjustment.inverted ? -sample : sample;
}
struct FocusScopeBarSource {
    engine::ScopeSource source;
    std::uint64_t generation = 0; // ScopeTap::generation() or SharedAnalysis::Result::generation
    bool operator==(const FocusScopeBarSource &) const = default;
};
struct FocusScopeBarBlock {
    FocusScopeBarSource source;
    std::int64_t firstStamp = 0;
    std::uint64_t transportRevision = 0;
    double sampleRate = 0.;
    bool advancing = false;
    std::span<const float> left, right;
    bool inverted = false;
};
inline FocusSumColumn focusEmptyExtrema(bool mono = false) noexcept {
    constexpr auto inf = std::numeric_limits<double>::infinity();
    return mono ? FocusSumColumn{{inf, 0.}, {-inf, 0.}} : FocusSumColumn{{inf, inf}, {-inf, -inf}};
}

template <class ReadDifference>
inline void focusIncludeDifference(FocusSumColumn &extrema, std::size_t count, FocusScopeChannels fold,
                                   const ReadDifference &read, std::array<double, 2> *energy = nullptr) noexcept {
    const auto include = [&](std::size_t channel, double sample) {
        extrema.low[channel] = std::min(extrema.low[channel], sample);
        extrema.high[channel] = std::max(extrema.high[channel], sample);
        if (energy) (*energy)[channel] += sample * sample;
    };
    const bool mono = focusScopeIsFolded(fold);
    for (std::size_t i = 0; i < count; ++i) {
        const auto [left, right] = read(i);
        if (mono) include(0, focusScopeFoldSample(fold, left, right));
        else {
            include(0, left);
            include(1, right);
        }
    }
}
inline std::array<double, 2> focusReadDifferenceRms(const std::array<double, 2> &energy, std::size_t frames) noexcept {
    if (frames == 0) return {};
    const auto count = static_cast<double>(frames);
    return {std::sqrt(energy[0] / count), std::sqrt(energy[1] / count)};
}
inline std::array<double, 2> focusBarSample(const FocusScopeBarBlock &block, std::size_t i) noexcept {
    if (block.inverted) return {-static_cast<double>(block.left[i]), -static_cast<double>(block.right[i])};
    return {block.left[i], block.right[i]};
}
struct FocusScopePoint {
    float x = 0.f, y = 0.f;
    bool operator==(const FocusScopePoint &) const = default;
};

// Scope columns arrive in increasing x order, with a possible vertical span at
// each column. Omit only points on a straight horizontal run, at any scale.
class FocusScopePathReducer {
  public:
    template <class Emit>
    void add(FocusScopePoint point, const Emit &emit) {
        if (!started_) {
            started_ = true;
            anchor_ = tail_ = point;
            emit(point, true);
        } else if (point != tail_) {
            if (anchor_.y != tail_.y || tail_.y != point.y) finish(emit);
            tail_ = point;
        }
    }

    template <class Emit>
    void finish(const Emit &emit) {
        if (anchor_ == tail_) return;
        anchor_ = tail_;
        emit(anchor_, false);
    }

  private:
    FocusScopePoint anchor_, tail_;
    bool started_ = false;
};

inline std::optional<std::int64_t> focusWindowEnd(std::int64_t first, std::int64_t length) noexcept {
    if (length <= 0 || first > std::numeric_limits<std::int64_t>::max() - length) return {};
    return first + length;
}

inline std::optional<FocusScopeRange> focusScopeTimedWindow(std::int64_t first, double rate, int milliseconds) noexcept {
    if (first < 0 || !std::isfinite(rate) || rate <= 0. || milliseconds <= 0) return {};
    const auto rounded = std::round(static_cast<long double>(rate) * milliseconds / 1000.L);
    const auto limit = std::ldexp(1.L, std::numeric_limits<std::int64_t>::digits);
    if (!(rounded >= 1.L && rounded < limit)) return {};
    const auto frames = static_cast<std::int64_t>(rounded);
    if (!std::in_range<std::size_t>(frames) || !focusWindowEnd(first, frames)) return {};
    return FocusScopeRange{first, static_cast<std::size_t>(frames), rate};
}

template <typename Visit>
void forEachFocusScopeGridLine(const FocusScopeRange &range, const engine::TempoMap &tempo,
                               const std::vector<textproj::Meter> &meters, Visit &&visit) {
    if (!std::in_range<std::int64_t>(range.frames)) return;
    const auto end = focusWindowEnd(range.first, static_cast<std::int64_t>(range.frames));
    if (!end || *end <= 0) return;
    constexpr auto beatWindow = FocusScopeWindow::Beat;
    auto beat = focusScopeWindowAtSample(std::max<std::int64_t>(0, range.first), range.sampleRate, tempo, meters,
                                         beatWindow);
    // Project signatures allow up to 255 beats per bar. Cover the Scope's
    // four-bar range, with one extra interval for a preview starting mid-beat.
    constexpr int maximumLines = 4 * 255 + 1;
    for (int line = 0; beat && beat->range.first < *end && line < maximumLines; ++line) {
        if (beat->range.first >= range.first) {
            const auto &meter = meterAt(beat->firstTick, meters);
            const bool bar = (beat->firstTick - meter.start_ticks) % meter.barTicks() == 0;
            visit(beat->range.first, bar);
        }
        if (beat->range.first + static_cast<std::int64_t>(beat->range.frames) >= *end) break;
        beat = focusScopeWindowAtTick(beat->nextTick, range.sampleRate, tempo, meters, beatWindow);
    }
}
// A worker may prepare storage before reading the bar and append only its
// contiguous slices. The source identities and revision describe heard audio;
// this collector neither acquires taps nor advances the transport.
// Measure only the half-open captured interval, with zero extension at its
// edges. The sample maximum is supplied by the same signed sum as the plot.
class FocusCapturedTruePeak {
  public:
    FocusCapturedTruePeak(double rate, std::size_t frames)
        : frames_(std::isfinite(rate) && rate > 0. ? frames : 0), factor_(factorFor(rate)) {
        if (frames_ && factor_ > 1) meter_.prepare(std::max(rate, 1.), 2);
    }
    [[nodiscard]] static int factorFor(double rate) noexcept {
        // Such small synthetic rates already saturate at the maximum factor.
        return dsp::TruePeakMeter::factorFor(std::max(rate, 1.));
    }
    void append(double left, double right) noexcept {
        if (received_ == frames_) return;
        ++received_;
        if (factor_ == 1) return;
        left_[static_cast<std::size_t>(buffered_)] = left;
        right_[static_cast<std::size_t>(buffered_)] = right;
        if (++buffered_ == batch) flushSamples();
    }
    void flushSamples() noexcept {
        process(left_.data(), right_.data(), buffered_);
        buffered_ = 0;
    }
    [[nodiscard]] std::optional<double> finish(double samplePeak) noexcept {
        if (!frames_ || received_ != frames_) return {};
        if (!result_) {
            if (factor_ > 1) {
                flushSamples();
                // Exactly N phase groups after the eight-group delay. On captures
                // shorter than eight, part of this pad completes the warmup.
                constexpr std::array<double, dsp::TruePeakMeter::kDelay> zero{};
                process(zero.data(), zero.data(), dsp::TruePeakMeter::kDelay);
            }
            result_ = std::max(samplePeak, meter_.peak());
        }
        return result_;
    }

  private:
    static constexpr int batch = 64;
    void process(const double *left, const double *right, int frames) noexcept {
        const auto prime = std::min(warmup_, frames);
        if (prime > 0) {
            const double *channels[]{left, right};
            meter_.process(channels, 2, prime);
            warmup_ -= prime;
            if (!warmup_) meter_.resetPeak();
        }
        if (prime < frames) {
            const double *channels[]{left + prime, right + prime};
            meter_.process(channels, 2, frames - prime);
        }
    }
    std::size_t frames_ = 0, received_ = 0;
    int factor_ = 1, buffered_ = 0, warmup_ = dsp::TruePeakMeter::kDelay;
    dsp::TruePeakMeter meter_;
    std::array<double, batch> left_{}, right_{};
    std::optional<double> result_;
};

class FocusScopeBarProjection {
  public:
    struct MonoColumn {
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();

        void include(double sample) noexcept {
            low = std::min(low, sample);
            high = std::max(high, sample);
        }
    };

    bool prepare(FocusScopeRange range, std::span<const FocusScopeBarSource> sources,
                 std::size_t requestedColumns, std::uint64_t transportRevision,
                 FocusScopeChannels mono = FocusScopeChannels::Stereo,
                 std::optional<SharedAnalysis::ScopeDifference> difference = {}, bool measurePhase = false,
                 bool measureSumTruePeak = false,
                 std::span<const std::optional<FocusScopeChannels>> layerChannels = {}) {
        const auto columns = std::min({requestedColumns, range.frames,
                                       static_cast<std::size_t>(SharedAnalysis::kMaxFftSize)});
        if (sources.empty() || columns == 0 || !transportRevision || range.first < 0 ||
            !std::isfinite(range.sampleRate) || range.sampleRate <= 0. ||
            !std::in_range<std::int64_t>(range.frames) ||
            !focusWindowEnd(range.first, static_cast<std::int64_t>(range.frames)) ||
            sources.size() >= bins_.max_size() / columns)
            return false;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto &source = sources[i];
            if (source.source.track <= 0 || !source.generation ||
                std::ranges::any_of(sources.first(i), [&](const auto &earlier) { return earlier.source == source.source; }))
                return false;
        }

        std::array<std::size_t, 2> differenceIndexes{};
        std::vector<FocusSumColumn> differenceBins;
        if (difference) {
            const auto find = [&](engine::ScopeSource source) {
                return std::ranges::find(sources, source, &FocusScopeBarSource::source);
            };
            const auto a = find(difference->a), b = find(difference->b);
            if (a != sources.end() && b != sources.end() && a != b) {
                differenceIndexes = {static_cast<std::size_t>(a - sources.begin()),
                                     static_cast<std::size_t>(b - sources.begin())};
                differenceBins.assign(columns, focusEmptyExtrema(focusScopeIsFolded(difference->fold)));
            } else {
                difference.reset();
            }
        }
        std::vector<FocusSumColumn> bins((sources.size() + 1) * columns, focusEmptyExtrema());
        std::vector<MonoColumn> monoBins;
        std::vector<FoldedLane> layerFolds;
        const auto foldFor = [](FocusScopeChannels view) {
            return focusScopeIsFolded(view) ? view : FocusScopeChannels::Stereo;
        };
        const auto sumFold = foldFor(mono);
        const auto overrides = layerChannels.first(std::min(layerChannels.size(), sources.size()));
        if (std::ranges::any_of(overrides, [&](auto view) { return view && foldFor(*view) != sumFold; })) {
            layerFolds.resize(sources.size() + 1);
            std::size_t folded = 0;
            for (std::size_t i = 0; i < layerFolds.size(); ++i) {
                const auto view = i == sources.size() || i >= overrides.size() ? sumFold :
                    foldFor(overrides[i].value_or(sumFold));
                if (focusScopeIsFolded(view)) layerFolds[i] = {view, folded++ * columns};
            }
            monoBins.resize(folded * columns);
        } else if (focusScopeIsFolded(sumFold)) {
            monoBins.resize((sources.size() + 1) * columns);
        }
        std::vector<FocusScopeBarSource> identities(sources.begin(), sources.end());
        auto meter = measureSumTruePeak && FocusCapturedTruePeak::factorFor(range.sampleRate) > 1
            ? std::make_unique<FocusCapturedTruePeak>(range.sampleRate, range.frames) : nullptr;
        bins_ = std::move(bins);
        monoBins_ = std::move(monoBins);
        layerFolds_ = std::move(layerFolds);
        mono_ = sumFold;
        differenceBins_ = std::move(differenceBins);
        difference_ = difference;
        differenceIndexes_ = differenceIndexes;
        sources_ = std::move(identities);
        range_ = range;
        transportRevision_ = transportRevision;
        columns_ = columns;
        framesPerColumn_ = range.frames / columns;
        remainder_ = range.frames % columns;
        frames_ = column_ = endOfColumn_ = carry_ = visibleColumns_ = 0;
        peak_ = energy_ = differenceEnergy_ = {};
        sumPhase_ = midPairPhase_ = {};
        measuresPhase_ = measurePhase;
        measuresSumTruePeak_ = measureSumTruePeak;
        sumPeakMeter_ = std::move(meter);
        sumTruePeak_.reset();
        extendColumn();
        return true;
    }

    // The caller slices the last block at the half-open bar edge. An invalid
    // block leaves the accumulated prefix intact, so it can be retried exactly.
    bool append(std::span<const FocusScopeBarBlock> blocks) noexcept {
        if (bins_.empty() || blocks.empty() || blocks.size() != sources_.size()) return false;
        const auto count = blocks.front().left.size();
        if (!count || count > range_.frames - frames_) return false;
        const auto stamp = range_.first + static_cast<std::int64_t>(frames_);
        const auto finite = [](float sample) { return std::isfinite(sample); };
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const auto &block = blocks[i];
            if (block.source != sources_[i] || block.firstStamp != stamp || !block.advancing ||
                block.transportRevision != transportRevision_ || block.sampleRate != range_.sampleRate ||
                block.left.size() != count || block.right.size() != count ||
                !std::ranges::all_of(block.left, finite) || !std::ranges::all_of(block.right, finite))
                return false;
        }

        const auto project = [this, blocks, count]<bool TruePeak>() {
            const auto fold = [this, blocks, count]<FocusScopeChannels Fold>() {
                if (layerFolds_.empty()) appendValid<Fold, TruePeak, false>(blocks, count);
                else appendValid<Fold, TruePeak, true>(blocks, count);
            };
            switch (mono_) {
            case FocusScopeChannels::Mid: fold.template operator()<FocusScopeChannels::Mid>(); break;
            case FocusScopeChannels::Side: fold.template operator()<FocusScopeChannels::Side>(); break;
            default: fold.template operator()<FocusScopeChannels::Stereo>(); break;
            }
        };
        if (sumPeakMeter_) {
            project.operator()<true>();
            sumPeakMeter_->flushSamples();
        } else {
            project.operator()<false>();
        }
        if (measuresSumTruePeak_ && complete()) {
            const auto samplePeak = std::max(peak_[0], peak_[1]);
            sumTruePeak_ = sumPeakMeter_ ? sumPeakMeter_->finish(samplePeak) : std::optional{samplePeak};
            sumPeakMeter_.reset();
        }
        return true;
    }

    [[nodiscard]] std::size_t frames() const noexcept { return bins_.empty() ? 0 : frames_; }
    [[nodiscard]] bool complete() const noexcept { return range_.frames != 0 && frames() == range_.frames; }
    [[nodiscard]] std::span<const FocusSumColumn> memberColumns(std::size_t member) const noexcept {
        return member < sources_.size() ? lane(member) : std::span<const FocusSumColumn>{};
    }
    [[nodiscard]] std::span<const MonoColumn> memberMonoColumns(std::size_t member,
                                                               FocusScopeChannels expected) const noexcept {
        return member < sources_.size() ? monoLane(member, expected) : std::span<const MonoColumn>{};
    }
    [[nodiscard]] std::span<const MonoColumn> sumMonoColumns(FocusScopeChannels expected) const noexcept {
        return monoLane(sources_.size(), expected);
    }
    // Used only for an unfinished projection. Published results are immutable.
    void retainMono(FocusScopeChannels requested) noexcept {
        if (requested == mono_ || (monoBins_.empty() && layerFolds_.empty())) return;
        mono_ = FocusScopeChannels::Stereo;
        std::vector<MonoColumn>{}.swap(monoBins_);
        std::vector<FoldedLane>{}.swap(layerFolds_);
    }
    [[nodiscard]] std::span<const FocusSumColumn> differenceColumns(
        const SharedAnalysis::ScopeDifference &expected) const noexcept {
        return difference_ == expected ? std::span(differenceBins_).first(visibleColumns_)
                                       : std::span<const FocusSumColumn>{};
    }
    [[nodiscard]] std::optional<std::array<double, 2>> differenceRms(
        const SharedAnalysis::ScopeDifference &expected) const noexcept {
        if (expected.spectrum || difference_ != expected || !frames_) return {};
        return focusReadDifferenceRms(differenceEnergy_, frames_);
    }
    void discardDifference() noexcept {
        difference_.reset();
        differenceEnergy_ = {};
        std::vector<FocusSumColumn>{}.swap(differenceBins_);
    }
    [[nodiscard]] std::span<const FocusSumColumn> sumColumns() const noexcept { return lane(sources_.size()); }
    void retainPhase(bool requested) noexcept { measuresPhase_ = measuresPhase_ && requested; }
    void retainSumTruePeak(bool requested) noexcept {
        if (requested || !measuresSumTruePeak_) return;
        measuresSumTruePeak_ = false;
        sumTruePeak_.reset();
        sumPeakMeter_.reset();
    }
    [[nodiscard]] bool filtersSumPeak() const noexcept { return sumPeakMeter_ != nullptr; }
    [[nodiscard]] std::optional<double> sumTruePeak() const noexcept { return sumTruePeak_; }
    [[nodiscard]] std::optional<double> phase(bool betweenMembers) const noexcept {
        if (!measuresPhase_) return {};
        return betweenMembers ? midPairPhase_.reading() : sumPhase_.reading();
    }
    [[nodiscard]] std::optional<FocusSumReading> sumReading() const noexcept {
        if (!frames()) return {};
        const auto count = static_cast<double>(frames_);
        return FocusSumReading{peak_, {std::sqrt(energy_[0] / count), std::sqrt(energy_[1] / count)}};
    }

  private:
    struct FoldedLane {
        FocusScopeChannels view = FocusScopeChannels::Stereo;
        std::size_t offset = 0;
    };

    template <FocusScopeChannels Fold, bool TruePeak, bool Mixed>
    void appendValid(std::span<const FocusScopeBarBlock> blocks, std::size_t count) noexcept {
        const auto remember = [](FocusSumColumn &column, const std::array<double, 2> &sample) {
            for (std::size_t ch = 0; ch < sample.size(); ++ch) {
                column.low[ch] = std::min(column.low[ch], sample[ch]);
                column.high[ch] = std::max(column.high[ch], sample[ch]);
            }
        };
        const auto midAt = [](const FocusScopeBarBlock &block, std::size_t index) {
            const auto mid = (static_cast<double>(block.left[index]) + block.right[index]) * .5;
            return block.inverted ? -mid : mid;
        };
        const bool measurePair = measuresPhase_ && blocks.size() > 1;
        for (std::size_t begin = 0; begin < count;) {
            const auto added = std::min(count - begin, endOfColumn_ - frames_);
            const auto end = begin + added;
            auto &sum = bins_[sources_.size() * columns_ + column_];
            for (auto at = begin; at < end; ++at) {
                std::array<double, 2> value{};
                for (std::size_t member = 0; member < blocks.size(); ++member) {
                    const auto sample = focusBarSample(blocks[member], at);
                    const auto index = member * columns_ + column_;
                    remember(bins_[index], sample);
                    // Combine corresponding samples before binning to preserve cancellation.
                    if constexpr (Mixed) {
                        const auto lane = layerFolds_[member];
                        if (focusScopeIsFolded(lane.view))
                            monoBins_[lane.offset + column_].include(focusScopeFoldSample(lane.view, sample[0], sample[1]));
                    } else if constexpr (focusScopeIsFolded(Fold)) {
                        monoBins_[index].include(focusScopeFoldSample(Fold, sample[0], sample[1]));
                    }
                    value[0] += sample[0];
                    value[1] += sample[1];
                }
                remember(sum, value);
                if constexpr (TruePeak) sumPeakMeter_->append(value[0], value[1]);
                if (measuresPhase_) {
                    sumPhase_.add(value[0], value[1]);
                    if (measurePair) midPairPhase_.add(midAt(blocks[0], at), midAt(blocks[1], at));
                }
                if constexpr (focusScopeIsFolded(Fold)) {
                    const auto offset = Mixed ? layerFolds_.back().offset : blocks.size() * columns_;
                    monoBins_[offset + column_].include(focusScopeFoldSample(Fold, value[0], value[1]));
                }
                for (std::size_t ch = 0; ch < value.size(); ++ch) {
                    peak_[ch] = std::max(peak_[ch], std::abs(value[ch]));
                    energy_[ch] += value[ch] * value[ch];
                }
            }
            if (difference_) {
                const auto &a = blocks[differenceIndexes_[0]], &b = blocks[differenceIndexes_[1]];
                focusIncludeDifference(differenceBins_[column_], added, difference_->fold, [&](std::size_t i) {
                    const auto first = focusBarSample(a, begin + i), second = focusBarSample(b, begin + i);
                    return std::array{first[0] - second[0], first[1] - second[1]};
                }, difference_->spectrum ? nullptr : &differenceEnergy_);
            }
            frames_ += added;
            begin = end;
            visibleColumns_ = column_ + 1;
            if (frames_ == endOfColumn_ && ++column_ < columns_) extendColumn();
        }
    }

    void extendColumn() noexcept {
        // Same floor(c * frames / columns) edges as the short Scope projection,
        // without a potentially overflowing product on a long bar.
        endOfColumn_ += framesPerColumn_;
        carry_ += remainder_;
        if (carry_ >= columns_) {
            carry_ -= columns_;
            ++endOfColumn_;
        }
    }
    std::span<const FocusSumColumn> lane(std::size_t index) const noexcept {
        if (bins_.empty() || !visibleColumns_) return {};
        return std::span(bins_).subspan(index * columns_, visibleColumns_);
    }

    [[nodiscard]] std::span<const MonoColumn> monoLane(std::size_t index,
                                                       FocusScopeChannels expected) const noexcept {
        if (!focusScopeIsFolded(expected) || monoBins_.empty() || !visibleColumns_) return {};
        if (!layerFolds_.empty()) {
            const auto lane = layerFolds_[index];
            if (lane.view != expected) return {};
            return std::span(monoBins_).subspan(lane.offset, visibleColumns_);
        }
        if (expected != mono_) return {};
        return std::span(monoBins_).subspan(index * columns_, visibleColumns_);
    }
    FocusScopeRange range_;
    std::vector<FocusScopeBarSource> sources_;
    std::vector<FocusSumColumn> bins_;
    std::vector<MonoColumn> monoBins_;
    std::vector<FoldedLane> layerFolds_;
    FocusScopeChannels mono_ = FocusScopeChannels::Stereo;
    std::vector<FocusSumColumn> differenceBins_;
    std::optional<SharedAnalysis::ScopeDifference> difference_;
    std::array<double, 2> differenceEnergy_{};
    std::array<std::size_t, 2> differenceIndexes_{};
    std::uint64_t transportRevision_ = 0;
    std::size_t columns_ = 0, framesPerColumn_ = 0, remainder_ = 0;
    std::size_t frames_ = 0, column_ = 0, endOfColumn_ = 0, carry_ = 0, visibleColumns_ = 0;
    std::array<double, 2> peak_{}, energy_{};
    std::unique_ptr<FocusCapturedTruePeak> sumPeakMeter_;
    std::optional<double> sumTruePeak_;
    bool measuresSumTruePeak_ = false;
    FocusScopePhase sumPhase_, midPairPhase_;
    bool measuresPhase_ = false;
};

inline std::optional<FocusScopeRange> focusScopeMember(const SharedAnalysis::Result &source) noexcept {
    const auto frames = source.scopeLeft.size();
    if (!source.advancing || !source.transportRevision || !std::isfinite(source.sampleRate) ||
        source.sampleRate <= 0. || !(source.outputs & SharedAnalysis::Scope) || source.nonFiniteSamples ||
        frames != source.scopeRight.size() || frames > static_cast<std::size_t>(SharedAnalysis::kMaxFftSize) ||
        !focusWindowEnd(source.firstStamp, static_cast<std::int64_t>(frames))) return {};
    return FocusScopeRange{source.firstStamp, frames, source.sampleRate};
}

// The displayed interval is the intersection of the member windows in the
// same audible transport epoch. A parked stamp is constant across callbacks
// and cannot identify a simultaneous multi-source window.
inline std::optional<FocusScopeRange>
focusScopeRange(std::span<const SharedAnalysis::Result *const> sources) noexcept {
    if (sources.size() < 2 || !sources.front()) return {};
    const auto &primary = *sources.front();
    auto first = std::numeric_limits<std::int64_t>::min();
    auto last = std::numeric_limits<std::int64_t>::max();
    for (const auto *source : sources) {
        if (!source || source->transportRevision != primary.transportRevision ||
            source->sampleRate != primary.sampleRate) return {};
        const auto window = focusScopeMember(*source);
        if (!window) return {};
        first = std::max(first, window->first);
        last = std::min(last, window->first + static_cast<std::int64_t>(window->frames));
    }
    if (first >= last) return {};
    return FocusScopeRange{first, static_cast<std::size_t>(last - first), primary.sampleRate};
}

template <class PublishColumn>
std::optional<FocusScopeRange> focusProjectDifference(const SharedAnalysis::Result &a,
                                                      const SharedAnalysis::Result &b,
                                                      std::size_t columns, FocusScopeChannels fold,
                                                      PublishColumn &&publish,
                                                      FocusScopeLayerAdjustment adjustA = {},
                                                      FocusScopeLayerAdjustment adjustB = {},
                                                      std::array<double, 2> *energy = nullptr) {
    if (energy) *energy = {};
    const std::array<const SharedAnalysis::Result *, 2> pair{&a, &b};
    const auto range = focusScopeRange(pair);
    if (!range || !columns) return {};
    columns = std::min(columns, range->frames);
    const auto startA = static_cast<std::size_t>(range->first - a.firstStamp);
    const auto startB = static_cast<std::size_t>(range->first - b.firstStamp);
    for (std::size_t column = 0; column < columns; ++column) {
        const auto begin = column * range->frames / columns;
        const auto length = (column + 1) * range->frames / columns - begin;
        auto extrema = focusEmptyExtrema(focusScopeIsFolded(fold));
        if (adjustA == FocusScopeLayerAdjustment{} && adjustB == FocusScopeLayerAdjustment{}) {
            focusIncludeDifference(extrema, length, fold, [&](std::size_t i) {
                return std::array{static_cast<double>(a.scopeLeft[startA + begin + i]) - b.scopeLeft[startB + begin + i],
                                  static_cast<double>(a.scopeRight[startA + begin + i]) - b.scopeRight[startB + begin + i]};
            }, energy);
        } else {
            focusIncludeDifference(extrema, length, fold, [&](std::size_t i) {
                const auto subtract = [&](auto &first, auto &second) {
                    return static_cast<double>(focusScopeDisplayedSample(first, startA, range->frames, begin + i, adjustA))
                         - focusScopeDisplayedSample(second, startB, range->frames, begin + i, adjustB);
                };
                return std::array{subtract(a.scopeLeft, b.scopeLeft), subtract(a.scopeRight, b.scopeRight)};
            }, energy);
        }
        publish(column, extrema);
    }
    return range;
}

// Use the same shifted signed samples as the plot. Two members use Mid;
// one member compares the two stereo lanes in its retained raw window.
inline std::optional<double> focusScopePhase(const SharedAnalysis::Result &a,
                                              const SharedAnalysis::Result *b,
                                              const FocusScopeRange &range,
                                              FocusScopeLayerAdjustment adjustmentA,
                                              FocusScopeLayerAdjustment adjustmentB = {}) noexcept {
    const auto startA = static_cast<std::size_t>(range.first - a.firstStamp);
    const auto startB = b ? static_cast<std::size_t>(range.first - b->firstStamp) : 0;
    FocusScopePhase phase;
    const auto at = [&](const auto &samples, std::size_t start, std::size_t i, FocusScopeLayerAdjustment adjustment) {
        return static_cast<double>(focusScopeDisplayedSample(samples, start, range.frames, i, adjustment));
    };
    if (b) {
        for (std::size_t i = 0; i < range.frames; ++i) {
            const double leftA = at(a.scopeLeft, startA, i, adjustmentA);
            const double rightA = at(a.scopeRight, startA, i, adjustmentA);
            const double leftB = at(b->scopeLeft, startB, i, adjustmentB);
            const double rightB = at(b->scopeRight, startB, i, adjustmentB);
            phase.add((leftA + rightA) * .5, (leftB + rightB) * .5);
        }
    } else {
        for (std::size_t i = 0; i < range.frames; ++i)
            phase.add(at(a.scopeLeft, startA, i, adjustmentA), at(a.scopeRight, startA, i, adjustmentA));
    }
    return phase.reading();
}

// The supplied range must be accepted for these sources. Measure the signed
// stereo sum and optionally emit its display extrema during the same scan.
template <FocusScopeChannels Fold = FocusScopeChannels::Stereo, class PublishColumn>
FocusSumReading focusProjectSum(std::span<const SharedAnalysis::Result *const> sources,
                                const FocusScopeRange &range, std::size_t columns,
                                PublishColumn &&publishColumn,
                                std::span<const FocusScopeLayerAdjustment> adjustments = {},
                                FocusScopePhase *phase = nullptr) {
    FocusSumReading reading;
    std::array<double, 2> energy{};
    const auto scan = [&](std::size_t first, std::size_t last, const auto &consume) {
        for (auto frame = first; frame < last; ++frame) {
            double left = 0., right = 0.;
            for (std::size_t member = 0; member < sources.size(); ++member) {
                const auto &source = *sources[member];
                const auto start = static_cast<std::size_t>(range.first - source.firstStamp);
                if (member < adjustments.size() && adjustments[member] != FocusScopeLayerAdjustment{}) {
                    left += focusScopeDisplayedSample(source.scopeLeft, start, range.frames, frame, adjustments[member]);
                    right += focusScopeDisplayedSample(source.scopeRight, start, range.frames, frame, adjustments[member]);
                } else {
                    left += source.scopeLeft[start + frame];
                    right += source.scopeRight[start + frame];
                }
            }
            reading.peak[0] = std::max(reading.peak[0], std::abs(left));
            reading.peak[1] = std::max(reading.peak[1], std::abs(right));
            energy[0] += left * left;
            energy[1] += right * right;
            if (phase) phase->add(left, right);
            consume(left, right);
        }
    };
    columns = std::min(columns, range.frames);
    if (columns == 0) {
        scan(0, range.frames, [](double, double) {});
    }
    for (std::size_t column = 0; column < columns; ++column) {
        constexpr bool mono = focusScopeIsFolded(Fold);
        using Extrema = std::conditional_t<mono, FocusScopeBarProjection::MonoColumn, FocusSumColumn>;
        Extrema extrema;
        if constexpr (!mono) extrema = focusEmptyExtrema();
        scan(column * range.frames / columns, (column + 1) * range.frames / columns,
             [&](double left, double right) {
                 if constexpr (focusScopeIsFolded(Fold)) {
                     extrema.include(focusScopeFoldSample(Fold, left, right));
                 } else {
                     extrema.low[0] = std::min(extrema.low[0], left);
                     extrema.high[0] = std::max(extrema.high[0], left);
                     extrema.low[1] = std::min(extrema.low[1], right);
                     extrema.high[1] = std::max(extrema.high[1], right);
                 }
             });
        publishColumn(column, extrema);
    }
    for (std::size_t ch = 0; ch < reading.rms.size(); ++ch)
        reading.rms[ch] = std::sqrt(energy[ch] / static_cast<double>(range.frames));
    return reading;
}

inline FocusSumReading focusReadSum(std::span<const SharedAnalysis::Result *const> sources,
                                   const FocusScopeRange &range) noexcept {
    return focusProjectSum(sources, range, 0, [](std::size_t, const FocusSumColumn &) {});
}
} // namespace adi::ui
