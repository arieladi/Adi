// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "shared_analysis.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/ui/meters.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
};
struct FocusScopeBar {
    std::int64_t firstTick = 0, nextTick = 0;
    FocusScopeRange range;
};

// Use the Arrangement's signature boundaries and the driver's nearest-sample
// convention. Keep absolute edges so consecutive bars never gain a rounding gap.
inline std::optional<FocusScopeBar> focusScopeBarAtTick(std::int64_t tick, double sampleRate,
                                                        const engine::TempoMap &tempo,
                                                        const std::vector<textproj::Meter> &meters) noexcept {
    if (tick < 0 || !std::isfinite(sampleRate) || sampleRate <= 0. || tempo.events.empty()) return {};
    const auto &meter = meterAt(tick, meters);
    const auto barTicks = meter.barTicks();
    if (meter.start_ticks < 0 || meter.start_ticks > tick || barTicks <= 0) return {};
    const auto firstTick = tick - (tick - meter.start_ticks) % barTicks;
    const auto nextMeter = std::upper_bound(meters.begin(), meters.end(), tick,
        [](std::int64_t position, const textproj::Meter &signature) { return position < signature.start_ticks; });
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (nextMeter == meters.end() && barTicks > limit - firstTick) return {};
    const auto untilNextMeter = nextMeter != meters.end() ? nextMeter->start_ticks - firstTick : barTicks;
    const auto nextTick = firstTick + std::min(barTicks, untilNextMeter);

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
struct FocusSumReading {
    std::array<double, 2> peak{}, rms{};
};
struct FocusSumColumn {
    std::array<double, 2> low{}, high{};
};
inline double focusScopeMidSample(double left, double right) noexcept { return (left + right) * .5; }
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
};
inline FocusSumColumn focusEmptyExtrema(bool mid = false) noexcept {
    constexpr auto inf = std::numeric_limits<double>::infinity();
    return mid ? FocusSumColumn{{inf, 0.}, {-inf, 0.}} : FocusSumColumn{{inf, inf}, {-inf, -inf}};
}

// The two windows already passed the same stamp and transport checks as Sum.
inline void focusIncludeDifference(FocusSumColumn &extrema, std::span<const float> leftA,
                                   std::span<const float> rightA, std::span<const float> leftB,
                                   std::span<const float> rightB, bool mid) noexcept {
    const auto include = [&](std::size_t channel, double sample) {
        extrema.low[channel] = std::min(extrema.low[channel], sample);
        extrema.high[channel] = std::max(extrema.high[channel], sample);
    };
    for (std::size_t i = 0; i < leftA.size(); ++i) {
        const double left = static_cast<double>(leftA[i]) - leftB[i];
        const double right = static_cast<double>(rightA[i]) - rightB[i];
        if (mid) include(0, focusScopeMidSample(left, right));
        else {
            include(0, left);
            include(1, right);
        }
    }
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
// A worker may prepare storage before reading the bar and append only its
// contiguous slices. The source identities and revision describe heard audio;
// this collector neither acquires taps nor advances the transport.
class FocusScopeBarProjection {
  public:
    struct MidColumn {
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();

        void include(double sample) noexcept {
            low = std::min(low, sample);
            high = std::max(high, sample);
        }
    };

    bool prepare(FocusScopeRange range, std::span<const FocusScopeBarSource> sources,
                 std::size_t requestedColumns, std::uint64_t transportRevision, bool includeMid = false,
                 std::optional<SharedAnalysis::ScopeDifference> difference = {}) {
        const auto columns = std::min({requestedColumns, range.frames,
                                       static_cast<std::size_t>(SharedAnalysis::kMaxFftSize)});
        if (sources.size() < 2 || columns == 0 || !transportRevision || range.first < 0 ||
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
                differenceBins.assign(columns, focusEmptyExtrema(difference->mid));
            } else {
                difference.reset();
            }
        }
        std::vector<FocusSumColumn> bins((sources.size() + 1) * columns, focusEmptyExtrema());
        std::vector<MidColumn> midBins;
        if (includeMid) midBins.resize((sources.size() + 1) * columns);
        std::vector<FocusScopeBarSource> identities(sources.begin(), sources.end());
        bins_ = std::move(bins);
        midBins_ = std::move(midBins);
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
        peak_ = energy_ = {};
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

        if (midBins_.empty()) appendValid<false>(blocks, count);
        else appendValid<true>(blocks, count);
        return true;
    }

    [[nodiscard]] std::size_t frames() const noexcept { return bins_.empty() ? 0 : frames_; }
    [[nodiscard]] bool complete() const noexcept { return range_.frames != 0 && frames() == range_.frames; }
    [[nodiscard]] std::span<const FocusSumColumn> memberColumns(std::size_t member) const noexcept {
        return member < sources_.size() ? lane(member) : std::span<const FocusSumColumn>{};
    }
    [[nodiscard]] std::span<const MidColumn> memberMidColumns(std::size_t member) const noexcept {
        return member < sources_.size() ? midLane(member) : std::span<const MidColumn>{};
    }
    [[nodiscard]] std::span<const MidColumn> sumMidColumns() const noexcept { return midLane(sources_.size()); }
    // Used only for an unfinished projection. Published results are immutable.
    void discardMid() noexcept { std::vector<MidColumn>{}.swap(midBins_); }
    [[nodiscard]] std::span<const FocusSumColumn> differenceColumns(
        const SharedAnalysis::ScopeDifference &expected) const noexcept {
        return difference_ == expected ? std::span(differenceBins_).first(visibleColumns_)
                                       : std::span<const FocusSumColumn>{};
    }
    void discardDifference() noexcept {
        difference_.reset();
        std::vector<FocusSumColumn>{}.swap(differenceBins_);
    }
    [[nodiscard]] std::span<const FocusSumColumn> sumColumns() const noexcept { return lane(sources_.size()); }
    [[nodiscard]] std::optional<FocusSumReading> sumReading() const noexcept {
        if (!frames()) return {};
        const auto count = static_cast<double>(frames_);
        return FocusSumReading{peak_, {std::sqrt(energy_[0] / count), std::sqrt(energy_[1] / count)}};
    }

  private:
    template <bool IncludeMid>
    void appendValid(std::span<const FocusScopeBarBlock> blocks, std::size_t count) noexcept {
        const auto remember = [](FocusSumColumn &column, const std::array<double, 2> &sample) {
            for (std::size_t ch = 0; ch < sample.size(); ++ch) {
                column.low[ch] = std::min(column.low[ch], sample[ch]);
                column.high[ch] = std::max(column.high[ch], sample[ch]);
            }
        };
        for (std::size_t begin = 0; begin < count;) {
            const auto added = std::min(count - begin, endOfColumn_ - frames_);
            const auto end = begin + added;
            auto &sum = bins_[sources_.size() * columns_ + column_];
            for (auto at = begin; at < end; ++at) {
                std::array<double, 2> value{};
                for (std::size_t member = 0; member < blocks.size(); ++member) {
                    const std::array<double, 2> sample{blocks[member].left[at], blocks[member].right[at]};
                    const auto index = member * columns_ + column_;
                    remember(bins_[index], sample);
                    // Combine corresponding samples before binning to preserve cancellation.
                    if constexpr (IncludeMid)
                        midBins_[index].include(focusScopeMidSample(sample[0], sample[1]));
                    value[0] += sample[0];
                    value[1] += sample[1];
                }
                remember(sum, value);
                if constexpr (IncludeMid)
                    midBins_[blocks.size() * columns_ + column_].include(focusScopeMidSample(value[0], value[1]));
                for (std::size_t ch = 0; ch < value.size(); ++ch) {
                    peak_[ch] = std::max(peak_[ch], std::abs(value[ch]));
                    energy_[ch] += value[ch] * value[ch];
                }
            }
            if (difference_) {
                const auto &a = blocks[differenceIndexes_[0]], &b = blocks[differenceIndexes_[1]];
                focusIncludeDifference(differenceBins_[column_], a.left.subspan(begin, added),
                                       a.right.subspan(begin, added), b.left.subspan(begin, added),
                                       b.right.subspan(begin, added), difference_->mid);
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

    [[nodiscard]] std::span<const MidColumn> midLane(std::size_t index) const noexcept {
        if (midBins_.empty() || !visibleColumns_) return {};
        return std::span(midBins_).subspan(index * columns_, visibleColumns_);
    }
    FocusScopeRange range_;
    std::vector<FocusScopeBarSource> sources_;
    std::vector<FocusSumColumn> bins_;
    std::vector<MidColumn> midBins_;
    std::vector<FocusSumColumn> differenceBins_;
    std::optional<SharedAnalysis::ScopeDifference> difference_;
    std::array<std::size_t, 2> differenceIndexes_{};
    std::uint64_t transportRevision_ = 0;
    std::size_t columns_ = 0, framesPerColumn_ = 0, remainder_ = 0;
    std::size_t frames_ = 0, column_ = 0, endOfColumn_ = 0, carry_ = 0, visibleColumns_ = 0;
    std::array<double, 2> peak_{}, energy_{};
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
                                                      std::size_t columns, bool mid, PublishColumn &&publish) {
    const std::array<const SharedAnalysis::Result *, 2> pair{&a, &b};
    const auto range = focusScopeRange(pair);
    if (!range || !columns) return {};
    columns = std::min(columns, range->frames);
    const auto startA = static_cast<std::size_t>(range->first - a.firstStamp);
    const auto startB = static_cast<std::size_t>(range->first - b.firstStamp);
    for (std::size_t column = 0; column < columns; ++column) {
        const auto begin = column * range->frames / columns;
        const auto length = (column + 1) * range->frames / columns - begin;
        auto extrema = focusEmptyExtrema(mid);
        focusIncludeDifference(extrema, std::span(a.scopeLeft).subspan(startA + begin, length),
                               std::span(a.scopeRight).subspan(startA + begin, length),
                               std::span(b.scopeLeft).subspan(startB + begin, length),
                               std::span(b.scopeRight).subspan(startB + begin, length), mid);
        publish(column, extrema);
    }
    return range;
}

// The supplied range must be accepted for these sources. Measure the signed
// stereo sum and optionally emit its display extrema during the same scan.
template <bool IncludeMid = false, class PublishColumn>
FocusSumReading focusProjectSum(std::span<const SharedAnalysis::Result *const> sources,
                                const FocusScopeRange &range, std::size_t columns,
                                PublishColumn &&publishColumn) {
    FocusSumReading reading;
    std::array<double, 2> energy{};
    const auto scan = [&](std::size_t first, std::size_t last, const auto &consume) {
        for (auto frame = first; frame < last; ++frame) {
            double left = 0., right = 0.;
            for (const auto *source : sources) {
                const auto i = static_cast<std::size_t>(range.first - source->firstStamp) + frame;
                left += source->scopeLeft[i];
                right += source->scopeRight[i];
            }
            reading.peak[0] = std::max(reading.peak[0], std::abs(left));
            reading.peak[1] = std::max(reading.peak[1], std::abs(right));
            energy[0] += left * left;
            energy[1] += right * right;
            consume(left, right);
        }
    };
    columns = std::min(columns, range.frames);
    if (columns == 0) {
        scan(0, range.frames, [](double, double) {});
    }
    for (std::size_t column = 0; column < columns; ++column) {
        [[maybe_unused]] constexpr auto inf = std::numeric_limits<double>::infinity();
        using Extrema = std::conditional_t<IncludeMid, FocusScopeBarProjection::MidColumn, FocusSumColumn>;
        Extrema extrema;
        if constexpr (!IncludeMid) extrema = {{inf, inf}, {-inf, -inf}};
        scan(column * range.frames / columns, (column + 1) * range.frames / columns,
             [&](double left, double right) {
                 if constexpr (IncludeMid) {
                     extrema.include(focusScopeMidSample(left, right));
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
