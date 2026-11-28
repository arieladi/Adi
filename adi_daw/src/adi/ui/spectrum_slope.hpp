// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace adi::ui {
enum class SpectrumSlope : std::uint8_t { Flat, Tilt45 };
inline bool validSpectrumSlopeId(std::string_view id) noexcept { return id == "flat" || id == "tilt45"; }
inline SpectrumSlope spectrumSlopeFromId(std::string_view id) noexcept {
    return id == "tilt45" ? SpectrumSlope::Tilt45 : SpectrumSlope::Flat;
}
inline std::string_view spectrumSlopeId(SpectrumSlope slope) noexcept {
    return slope == SpectrumSlope::Tilt45 ? "tilt45" : "flat";
}
// Display correction only. DC/invalid frequencies have no logarithmic tilt;
// raw coherent-sine bins, statistics and comparison calibration stay unchanged.
inline double spectrumDisplayOffset(SpectrumSlope slope, double frequencyHz) noexcept {
    if (slope != SpectrumSlope::Tilt45 || !std::isfinite(frequencyHz) || frequencyHz <= 0) return 0;
    const auto octaves = frequencyHz < std::numeric_limits<double>::min() * 1000.
        ? std::log2(frequencyHz) - std::log2(1000.) : std::log2(frequencyHz / 1000.);
    return 4.5 * octaves;
}
// Owner-prepared, bounded display cache. No work occurs in paint/audio. Flat
// requires no storage; unchanged metadata returns before allocation or logarithms.
class SpectrumSlopeCache {
  public:
    bool prepare(SpectrumSlope slope, double rate, std::int32_t fftSize, std::size_t bins) {
        if (slope != SpectrumSlope::Tilt45 || !std::isfinite(rate) || rate <= 0 ||
            fftSize < 32 || fftSize > 65536 || bins != static_cast<std::size_t>(fftSize / 2 + 1)) {
            const bool changed = size_ != 0; reset(); return changed;
        }
        if (rate_ == rate && fftSize_ == fftSize && size_ == bins) return false;
        auto next = std::make_unique<float[]>(bins);
        for (std::size_t i = 1; i < bins; ++i)
            next[i] = static_cast<float>(spectrumDisplayOffset(slope, rate * (static_cast<double>(i) / fftSize)));
        offsets_ = std::move(next); size_ = bins; rate_ = rate; fftSize_ = fftSize;
        return true;
    }
    void reset() noexcept { offsets_.reset(); size_ = 0; rate_ = 0; fftSize_ = 0; }
    std::span<const float> offsets() const noexcept { return {offsets_.get(), size_}; }
  private:
    std::unique_ptr<float[]> offsets_;
    std::size_t size_ = 0;
    double rate_ = 0;
    std::int32_t fftSize_ = 0;
};


struct SpectrumSmoothingChoice {
    std::string_view label;
    double octaves;
    double upperCorrection;
};
inline constexpr std::array<SpectrumSmoothingChoice, 16> kSpectrumSmoothingChoices{{
    {"Off", 0., 0.}, {"1/48 oct", 1. / 48, 1.}, {"1/24 oct", 1. / 24, 1.}, {"1/12 oct", 1. / 12, 1.},
    {"1/9 oct", 1. / 9, 1.}, {"1/6 oct", 1. / 6, 1.}, {"1/5 oct", 1. / 5, 1.}, {"1/4 oct", 1. / 4, 1.},
    {"1/3 oct", 1. / 3, 1.01}, {"1/2 oct", 1. / 2, 1.04}, {"2/3 oct", 2. / 3, 1.06}, {"3/4 oct", 3. / 4, 1.09},
    {"1 oct", 1., 1.13}, {"1.5 oct", 1.5, 1.19}, {"2 oct", 2., 1.22}, {"2.5 oct", 2.5, 1.27},
}};

// A display projection prepared and evaluated by its owner. The returned
// points belong to this object and may be overwritten by the next projection.
class SpectrumSmoothing {
  public:
    struct Point {
        double frequencyHz;
        float db;
    };

    bool prepare(std::size_t choice, double lowHz, double topHz, double rate, int fftSize, std::size_t bins) {
        if (choice == 0 || choice >= kSpectrumSmoothingChoices.size() || !std::isfinite(rate) || rate <= 0. ||
            !std::isfinite(lowHz) || !std::isfinite(topHz) || lowHz <= 0. || topHz <= lowHz || topHz > rate * .5 ||
            fftSize < 32 || fftSize > 65536 || bins != static_cast<std::size_t>(fftSize / 2 + 1)) {
            reset();
            return false;
        }
        const Input input{choice, lowHz, topHz, rate, fftSize, bins};
        if (input_ == input) return true;

        const auto &setting = kSpectrumSmoothingChoices[choice];
        const double logLow = std::log2(lowHz);
        const double logSpan = std::log2(topHz) - logLow;
        const double required = std::ceil(16. * logSpan / std::sqrt(setting.octaves) + 1.);
        constexpr std::size_t maxPoints = 8192;
        if (required < 2. || required > static_cast<double>(maxPoints)) {
            reset();
            return false;
        }
        const auto count = static_cast<std::size_t>(required);
        const auto lowerRatio = std::exp2(-1.5 * setting.octaves);
        SpectrumSmoothing draft;
        draft.input_ = input;
        draft.points_.reserve(count);
        draft.plan_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(count - 1);
            const double frequency = i == 0 ? lowHz : i + 1 == count ? topHz : std::exp2(logLow + t * logSpan);
            const double centre = (frequency / rate) * static_cast<double>(fftSize);
            const double lower = centre * lowerRatio;
            const double upper = centre + (centre - lower) * setting.upperCorrection;
            draft.points_.push_back({frequency, kDbFloor});
            draft.preparePoint(lower, upper);
        }
        if (!draft.directWeights_.empty() || !draft.momentRanges_.empty()) draft.magnitudes_.resize(bins);
        if (!draft.momentRanges_.empty()) {
            const auto blocks = (bins + kBlock - 1) / kBlock;
            draft.prefix_.resize(blocks * (kBlock + 1));
        }
        *this = std::move(draft);
        return true;
    }

    std::span<const Point> project(std::span<const float> rawDbfs) {
        if (points_.empty() || rawDbfs.size() != input_.bins) return {};
        double tail = 0.;
        if (!magnitudes_.empty()) {
            for (std::size_t i = 0; i < rawDbfs.size(); ++i)
                magnitudes_[i] = std::isfinite(rawDbfs[i]) ? std::exp(safeDb(rawDbfs[i]) * (std::numbers::ln10 / 20.)) : 0.;
            const std::size_t n = magnitudes_.size();
            for (std::size_t i = n - 8; i < n - 4; ++i) tail -= magnitudes_[i];
            for (std::size_t i = n - 4; i < n; ++i) tail += 2. * magnitudes_[i];
            tail *= .25;
            updatePrefix();
        }
        for (std::size_t i = 0; i < points_.size(); ++i) {
            const auto &point = plan_[i];
            if (!point.usable) continue;
            double db = 0.;
            if (point.broadMix > 0.) db = broad(point, tail);
            if (point.broadMix < 1.) db = std::lerp(narrow(point, rawDbfs), db, point.broadMix);
            points_[i].db = static_cast<float>(db);
        }
        return points_;
    }

    void reset() noexcept { *this = SpectrumSmoothing{}; }

  private:
    static constexpr float kDbFloor = -300.f;
    static constexpr std::size_t kNarrowSteps = 12;
    static constexpr std::size_t kPieces = 16;
    static constexpr std::size_t kBlock = 64;
    static constexpr std::size_t kDirect = 160;
    using Moment = std::array<double, 6>;

    struct Input {
        std::size_t choice{};
        double low{}, top{}, rate{};
        int fftSize{};
        std::size_t bins{};
        bool operator==(const Input &) const = default;
    };
    struct NarrowSample {
        std::size_t bin{};
        double position{};
        double weight{};
    };
    struct MomentRange {
        std::size_t first{}, end{};
        Moment coefficients{};
    };
    struct Window {
        std::array<NarrowSample, kNarrowSteps> narrow{};
        std::size_t narrowCount{};
        std::size_t firstBin{}, endBin{}, firstTerm{}, endTerm{};
        double lower{}, width{}, weight{}, tailWeight{}, broadMix{};
        bool usable{}, useMoments{};
    };

    static double safeDb(float db) noexcept {
        return std::isfinite(db) ? std::clamp(static_cast<double>(db), -6000., 6000.) : kDbFloor;
    }

    static double weight(double portion) noexcept {
        const double s = std::sin(std::numbers::pi * std::min(portion, 1. - portion));
        return s * s;
    }

    static Moment powers(double value) noexcept {
        Moment result{1.};
        for (std::size_t i = 1; i < result.size(); ++i) result[i] = result[i - 1] * value;
        return result;
    }

    static const auto &blockPowers() noexcept {
        static const auto coefficients = [] {
            std::array<Moment, kBlock> result{};
            for (std::size_t i = 0; i < result.size(); ++i)
                result[i] = powers(static_cast<double>(i) / static_cast<double>(kBlock));
            return result;
        }();
        return coefficients;
    }

    static const auto &unitPrefix() noexcept {
        static const auto moments = [] {
            std::array<Moment, kBlock + 1> result{};
            const auto &power = blockPowers();
            for (std::size_t i = 0; i < kBlock; ++i)
                for (std::size_t j = 0; j < power[i].size(); ++j)
                    result[i + 1][j] = result[i][j] + power[i][j];
            return result;
        }();
        return moments;
    }

    static const auto &hannPieces() noexcept {
        static const auto coefficients = [] {
            std::array<Moment, kPieces / 2> result{};
            constexpr double step = 1. / static_cast<double>(kPieces);
            const auto slope = [](double t) { return step * std::numbers::pi * std::sin(2. * std::numbers::pi * t); };
            const auto curvature = [](double t) {
                return 2. * std::numbers::pi * std::numbers::pi * step * step * std::cos(2. * std::numbers::pi * t);
            };
            for (std::size_t i = 0; i < result.size(); ++i) {
                const double low = static_cast<double>(i) * step;
                const double high = low + step;
                const double c0 = weight(low), c1 = slope(low), c2 = .5 * curvature(low);
                const double value = weight(high) - c0 - c1 - c2;
                const double derivative = slope(high) - c1 - 2. * c2;
                const double second = curvature(high) - 2. * c2;
                result[i] = {c0, c1, c2, 10. * value - 4. * derivative + .5 * second,
                             -15. * value + 7. * derivative - second, 6. * value - 3. * derivative + .5 * second};
            }
            return result;
        }();
        return coefficients;
    }

    // Express one Hann segment in block-local coordinates. Close-to-zero
    // output remains accurate because the moments never span a large origin.
    static Moment shiftPolynomial(const Moment &coefficients, double origin, double slope) noexcept {
        constexpr std::array<Moment, 6> choose{{
            {1.}, {1., 1.}, {1., 2., 1.}, {1., 3., 3., 1.}, {1., 4., 6., 4., 1.}, {1., 5., 10., 10., 5., 1.},
        }};
        const auto originPower = powers(origin);
        const auto slopePower = powers(slope);
        Moment result{};
        for (std::size_t j = 0; j < result.size(); ++j) {
            for (std::size_t i = j; i < result.size(); ++i)
                result[j] += coefficients[i] * choose[i][j] * originPower[i - j];
            result[j] *= slopePower[j];
        }
        return result;
    }

    static double dot(const Moment &coefficients, const Moment &first, const Moment &end) noexcept {
        double total = 0.;
        for (std::size_t i = 0; i < coefficients.size(); ++i)
            total += coefficients[i] * (end[i] - first[i]);
        return total;
    }

    static double tailWeight(std::size_t first, std::size_t end, double low, double width) noexcept {
        if (first >= end) return 0.;
        const double count = static_cast<double>(end - first);
        const double centre = static_cast<double>(first) + .5 * (count - 1.);
        const double step = std::numbers::pi / width;
        const double cosine = std::cos(2. * (centre - low) * step);
        return std::max(0., .5 * (count - cosine * std::sin(count * step) / std::sin(step)));
    }

    void preparePoint(double lower, double upper) {
        auto &point = plan_.emplace_back();
        const std::size_t first = static_cast<std::size_t>(std::ceil(lower));
        const double width = upper - lower;
        if (first >= input_.bins || width <= 0.) return;
        point.usable = true;
        point.broadMix = std::clamp((width - 5.) / 5., 0., 1.);
        if (point.broadMix < 1.) {
            for (std::size_t j = 1; j < kNarrowSteps; ++j) {
                const double fraction = static_cast<double>(j) / static_cast<double>(kNarrowSteps);
                const double x = lower + width * fraction;
                const auto bin = static_cast<std::size_t>(x);
                if (bin >= input_.bins) break;
                point.narrow[point.narrowCount++] = {bin, x - static_cast<double>(bin), weight(fraction)};
            }
        }
        if (point.broadMix == 0.) return;
        point.lower = lower;
        point.width = width;
        point.firstBin = first;
        const std::size_t end = static_cast<std::size_t>(std::floor(upper)) + 1;
        point.endBin = std::min(end, input_.bins);
        point.tailWeight = tailWeight(point.endBin, end, lower, width);
        point.weight = point.tailWeight;
        point.useMoments = point.endBin - point.firstBin >= kDirect;
        if (point.useMoments) {
            prepareMoments(point);
            return;
        }
        point.firstTerm = directWeights_.size();
        for (std::size_t i = first; i < point.endBin; ++i) {
            const double w = weight((static_cast<double>(i) - lower) / width);
            directWeights_.push_back(w);
            point.weight += w;
        }
        point.endTerm = directWeights_.size();
    }

    void prepareMoments(Window &point) {
        const auto &shapes = hannPieces();
        const auto &uniform = unitPrefix();
        const double step = point.width / static_cast<double>(kPieces);
        point.firstTerm = momentRanges_.size();
        std::size_t lo = point.firstBin;
        for (std::size_t piece = 0; piece < kPieces && lo < point.endBin; ++piece) {
            const bool mirror = piece >= kPieces / 2;
            const double edge = point.lower + step * static_cast<double>(piece);
            const auto segmentEnd = static_cast<std::size_t>(std::ceil(edge + step));
            const std::size_t hi = piece + 1 == kPieces ? point.endBin : std::min(segmentEnd, point.endBin);
            const auto &shape = shapes[mirror ? kPieces - 1 - piece : piece];
            while (lo < hi) {
                const std::size_t block = lo / kBlock;
                const std::size_t origin = block * kBlock;
                const std::size_t end = std::min(hi, origin + kBlock);
                const double shift = (static_cast<double>(origin) - edge) / step;
                const double scale = static_cast<double>(kBlock) / step;
                const auto polynomial = shiftPolynomial(shape, mirror ? 1. - shift : shift, mirror ? -scale : scale);
                const std::size_t prefix = block * (kBlock + 1);
                momentRanges_.push_back({prefix + lo - origin, prefix + end - origin, polynomial});
                point.weight += dot(polynomial, uniform[lo - origin], uniform[end - origin]);
                lo = end;
            }
        }
        point.endTerm = momentRanges_.size();
    }

    void updatePrefix() noexcept {
        if (prefix_.empty()) return;
        const auto &power = blockPowers();
        for (std::size_t origin = 0; origin < magnitudes_.size(); origin += kBlock) {
            const auto base = (origin / kBlock) * (kBlock + 1);
            const auto count = std::min(kBlock, magnitudes_.size() - origin);
            for (std::size_t i = 0; i < count; ++i)
                for (std::size_t degree = 0; degree < power[i].size(); ++degree)
                    prefix_[base + i + 1][degree] = prefix_[base + i][degree] + magnitudes_[origin + i] * power[i][degree];
        }
    }

    static double limited(double a, double b) noexcept {
        if ((a > 0.) != (b > 0.)) return 0.;
        return std::copysign(2. * std::min({std::abs(a), std::abs(b), std::abs(a + b) * .25}), a);
    }

    static double narrow(const Window &point, std::span<const float> rawDbfs) noexcept {
        double values = 0., weights = 1.e-300;
        for (std::size_t i = 0; i < point.narrowCount; ++i) {
            const auto &sample = point.narrow[i];
            const std::size_t bin = sample.bin;
            const double a = safeDb(rawDbfs[bin == 0 ? 0 : bin - 1]);
            const double b = safeDb(rawDbfs[bin]);
            const double c = safeDb(rawDbfs[std::min(bin + 1, rawDbfs.size() - 1)]);
            const double d = safeDb(rawDbfs[std::min(bin + 2, rawDbfs.size() - 1)]);
            const double p = b - a, q = c - b, r = d - c;
            const double left = .5 * (limited(p, q) + .3 * p + .5 * q);
            const double right = .5 * (limited(q, r) + .5 * q + .3 * r);
            const double t = sample.position;
            const double value = b + t * (left + t * ((3. * q - 2. * left - right) + t * (left + right - 2. * q)));
            values += value * sample.weight;
            weights += sample.weight;
        }
        return point.narrowCount == 0 ? kDbFloor : values / weights;
    }

    double broad(const Window &point, double tail) const noexcept {
        double values = 0.;
        double weights = point.weight;
        const double tailValue = point.tailWeight * tail;
        if (point.useMoments) {
            for (std::size_t i = point.firstTerm; i < point.endTerm; ++i) {
                const auto &range = momentRanges_[i];
                values += dot(range.coefficients, prefix_[range.first], prefix_[range.end]);
            }
            // A negative extrapolation can nearly cancel the measured bins.
            // Use exact weights there to avoid magnifying approximation error.
            if (weights <= 0. || (tailValue < 0. && std::abs(values + tailValue) < .3 * (std::abs(values) - tailValue))) {
                values = 0.;
                weights = point.tailWeight;
                for (std::size_t i = point.firstBin; i < point.endBin; ++i) {
                    const double w = weight((static_cast<double>(i) - point.lower) / point.width);
                    values += magnitudes_[i] * w;
                    weights += w;
                }
            }
        } else {
            for (std::size_t i = point.firstTerm; i < point.endTerm; ++i)
                values += directWeights_[i] * magnitudes_[point.firstBin + i - point.firstTerm];
        }
        if (weights <= 0.) return kDbFloor;
        return 20. * std::log10(std::abs((values + tailValue) / weights) + 1.e-15);
    }

    Input input_{};
    std::vector<Point> points_;
    std::vector<Window> plan_;
    std::vector<double> directWeights_, magnitudes_;
    std::vector<MomentRange> momentRanges_;
    std::vector<Moment> prefix_;
};
} // namespace adi::ui
