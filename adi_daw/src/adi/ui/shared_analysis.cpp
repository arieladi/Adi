// SPDX-License-Identifier: AGPL-3.0-or-later
#include "shared_analysis.hpp"
#include "adi/dsp/masking.hpp"
#include "adi/dsp/spectrum_average.hpp"
#include "adi/dsp/true_peak.hpp"
#include "adi/ui/focus_lag.hpp"
#include "adi/ui/focus_scope.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

namespace adi::ui {
std::optional<FocusLagPreview> focus_lag_detail::measureWide(std::span<const double> a,
                                                            std::span<const double> b,
                                                            std::size_t radius, double aEnergy) {
    const auto fftSize = std::bit_ceil(b.size());
    dsp::RealFft fft(static_cast<std::int32_t>(fftSize));
    if (!fft.valid()) return {};

    const auto support = a.size();
    const auto weight = static_cast<double>(support);
    const double bMean = std::accumulate(b.begin(), b.end(), 0.) / static_cast<double>(b.size());
    double aScale = 0., bScale = 0.;
    for (const auto sample : a) aScale = std::max(aScale, std::abs(sample));
    for (const auto sample : b) bScale = std::max(bScale, std::abs(sample - bMean));
    if (bScale < rmsFloor) return {};
    const double inverseB = 1. / bScale;
    const auto bValue = [bMean, inverseB](const double sample) { return (sample - bMean) * inverseB; };

    // One product gives every integer lag. A is the fixed interior,
    // so none of the requested offsets can wrap around the padded B.
    std::vector<float> frame(fftSize);
    std::vector<std::complex<float>> product(fftSize / 2 + 1), target(product.size());
    std::transform(a.begin(), a.end(), frame.begin(),
                   [aScale](const double sample) { return static_cast<float>(sample / aScale); });
    const auto aFloatMean = std::accumulate(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(support), 0.) / weight;
    if (!fft.forward(frame, product)) return {};
    std::transform(b.begin(), b.end(), frame.begin(),
                   [&](const double sample) { return static_cast<float>(bValue(sample)); });
    if (!fft.forward(frame, target)) return {};
    for (std::size_t k = 0; k < product.size(); ++k) product[k] = std::conj(product[k]) * target[k];
    if (!fft.inverse(product, frame)) return {};

    // Keep corrections across removals so an earlier large sample cannot
    // consume the variance of a quiet target window that no longer includes it.
    struct Moment {
        double sum = 0., correction = 0.;
        void add(double value) noexcept {
            const double next = sum + value;
            correction += std::abs(sum) >= std::abs(value) ? (sum - next) + value : (value - next) + sum;
            sum = next;
        }
        double value() const noexcept { return sum + correction; }
    };
    Moment sum, squares;
    for (std::size_t i = 0; i < support; ++i) {
        const auto sample = bValue(b[i]);
        sum.add(sample);
        squares.add(sample * sample);
    }
    const double normalisedA = aEnergy / (aScale * aScale);
    const double scaledFloor = rmsFloor / bScale;
    const double minimumEnergy = scaledFloor * scaledFloor * weight;
    std::vector<double> scores(2 * radius + 1);
    for (std::size_t offset = 0; offset < scores.size(); ++offset) {
        const double targetSum = sum.value();
        const double energy = squares.value() - targetSum * targetSum / weight;
        if (energy >= minimumEnergy) {
            const double cross = static_cast<double>(frame[offset]) - aFloatMean * targetSum;
            scores[offset] = std::clamp(cross / std::sqrt(normalisedA * energy), -1., 1.);
        }
        if (offset + 1 < scores.size()) {
            const auto old = bValue(b[offset]), next = bValue(b[offset + support]);
            sum.add(-old);
            sum.add(next);
            squares.add(-old * old);
            squares.add(next * next);
        }
    }
    // An ill-conditioned FFT may be unable to separate quiet candidates.
    // Confirm the proposed offset and current correlation with the raw data.
    const auto candidate = selectPeak(scores, radius, support);
    if (!candidate) return {};
    const auto selected = static_cast<std::size_t>(static_cast<std::int32_t>(radius) + candidate->lagSamples);
    const double rawFloor = rmsFloor * rmsFloor * weight;
    scores[selected] = correlationAt(a, b.subspan(selected, support), aEnergy, weight, rawFloor);
    scores[radius] = correlationAt(a, b.subspan(radius, support), aEnergy, weight, rawFloor);
    const auto confirmed = selectPeak(scores, radius, support);
    return confirmed && confirmed->lagSamples == candidate->lagSamples ? confirmed : std::nullopt;
}

namespace {
constexpr std::uint32_t allOutputs = SharedAnalysis::Spectrum | SharedAnalysis::Scope | SharedAnalysis::Meters |
                                     SharedAnalysis::Phase | SharedAnalysis::Spectrogram | SharedAnalysis::Average1s |
                                     SharedAnalysis::RealtimeMaximum | SharedAnalysis::CumulativeMaximum |
                                     SharedAnalysis::FilledSpectrum | SharedAnalysis::PeriodScope |
                                     SharedAnalysis::ColourWaveform |
                                     SharedAnalysis::Strip | SharedAnalysis::DeviceBlockAverage | SharedAnalysis::PhaseBands |
                                     SharedAnalysis::PhaseBandDetail |
                                     SharedAnalysis::CumulativeAverage | SharedAnalysis::RealtimeSigma;
constexpr int stripFftSize = 1024;
constexpr double stripRefreshSeconds = .05, stripSmoothingSeconds = .3;
constexpr std::size_t maxHistoryColumns = 128;
constexpr std::size_t pendingHistoryBytesPerDomain = 256 * 1024;
// PFFFT squares its unnormalised float bins. Leave headroom for that sum
// at the largest supported FFT as well as for the stereo and M/S projection.
constexpr float spectralSampleLimit = 1.e12f;
class PhaseBandPlan {
    static constexpr auto bandCount = SharedAnalysis::kPhaseBandCount;
    static constexpr auto unmapped = static_cast<std::uint8_t>(bandCount);
    using Bands = SharedAnalysis::PhaseBandCorrelations;
    using Spatial = SharedAnalysis::PhaseBandSpatial;
    struct Power { double left = 0., right = 0., cross = 0.; };
    static constexpr double leakageFloor = 1.e-12;

  public:
    PhaseBandPlan(int size, dsp::Window type)
        : fft_(size), window_(static_cast<std::size_t>(size)), scratch_(window_.size()),
          left_(window_.size() / 2 + 1), right_(left_.size()), binBands_(left_.size(), unmapped) {
        dsp::fillWindow(type, window_);
    }

    bool valid() const noexcept { return fft_.valid(); }

    void bindRate(double rate) noexcept {
        if (rate == rate_) return;
        rate_ = rate;
        resetWindow(0);
        hasMappedBins_ = false;
        std::fill(binBands_.begin(), binBands_.end(), unmapped);
        constexpr double lowHz = 20.;
        const double highHz = std::min(20000., rate * .5);
        const double low = dsp::erbRate(lowHz);
        const double extent = dsp::erbRate(highHz) - low;
        if (!(extent > 0.)) return;
        const double scale = static_cast<double>(bandCount) / extent;
        const double binHz = rate / static_cast<double>(fft_.size());
        for (std::size_t i = 1; i < binBands_.size(); ++i) {
            const double hz = static_cast<double>(i) * binHz;
            if (hz < lowHz || hz > highHz) continue;
            const auto band = static_cast<std::uint8_t>((dsp::erbRate(hz) - low) * scale);
            binBands_[i] = std::min(band, static_cast<std::uint8_t>(bandCount - 1));
            hasMappedBins_ = true;
        }
    }

    void project(SharedAnalysis::Result &picture, std::span<const float> left,
                 std::span<const float> right, std::size_t &transforms) noexcept {
        if (picture.windowSerial != windowSerial_) resetWindow(picture.windowSerial);
        picture.phaseBands = silent_;
        const bool hasLeft = picture.rms[0] > 0.f, hasRight = picture.rms[1] > 0.f;
        const bool stereo = hasLeft && hasRight;
        const bool detail = (picture.outputs & SharedAnalysis::PhaseBandDetail) != 0;
        if (!hasMappedBins_ || (!stereo && !(detail && (hasLeft || hasRight)))) return;
        if (!powersReady_) powersReady_ = measure(left, right, hasLeft, hasRight, transforms);
        if (!powersReady_) return;
        try {
            if (stereo) {
                if (!cached_) cached_ = std::make_shared<const Bands>(correlations());
                picture.phaseBands = cached_;
            }
            if (detail) {
                if (!cachedSpatial_) cachedSpatial_ = std::make_shared<const Spatial>(spatial());
                picture.phaseBandSpatial = cachedSpatial_;
            }
        } catch (...) { /* Broadband Phase stays available. */ }
    }

  private:
    void resetWindow(std::uint64_t serial) noexcept {
        windowSerial_ = serial;
        powersReady_ = false;
        cached_.reset();
        cachedSpatial_.reset();
    }
    bool forward(std::span<const float> input, std::span<std::complex<float>> output) noexcept {
        for (std::size_t i = 0; i < scratch_.size(); ++i) scratch_[i] = input[i] * window_[i];
        return fft_.forward(scratch_, output);
    }

    bool measure(std::span<const float> left, std::span<const float> right, bool hasLeft, bool hasRight,
                 std::size_t &transforms) noexcept {
        if (hasLeft) {
            if (!forward(left, left_)) return false;
            ++transforms;
        } else {
            std::ranges::fill(left_, std::complex<float>{});
        }
        if (hasRight) {
            if (!forward(right, right_)) return false;
            ++transforms;
        } else {
            std::ranges::fill(right_, std::complex<float>{});
        }
        powers_.fill({});
        totalLeft_ = totalRight_ = 0.;
        for (std::size_t i = 1; i < binBands_.size(); ++i) {
            if (binBands_[i] == unmapped) continue;
            const std::complex<double> l{left_[i]}, r{right_[i]};
            auto &band = powers_[binBands_[i]];
            // Interior bins represent a conjugate pair. Nyquist is real
            // and alone, so use half its energy on the same scale.
            const double weight = i + 1 == binBands_.size() ? .5 : 1.;
            const double lPower = std::norm(l) * weight, rPower = std::norm(r) * weight;
            band.left += lPower;
            band.right += rPower;
            band.cross += (l * std::conj(r)).real() * weight;
            totalLeft_ += lPower;
            totalRight_ += rPower;
        }
        return true;
    }
    Bands correlations() const noexcept {
        Bands result{};
        // Discard a band's numerical leakage relative to each channel. With
        // no energy on either side a correlation has no defined value.
        for (std::size_t i = 0; i < powers_.size(); ++i) {
            const auto &band = powers_[i];
            if (band.left > totalLeft_ * leakageFloor && band.right > totalRight_ * leakageFloor)
                result[i] = static_cast<float>(std::clamp(band.cross / std::sqrt(band.left * band.right), -1., 1.));
        }
        return result;
    }
    Spatial spatial() const noexcept {
        Spatial result{};
        const auto minimum = (totalLeft_ + totalRight_) * leakageFloor;
        for (std::size_t i = 0; i < powers_.size(); ++i) {
            const auto &band = powers_[i];
            const auto power = band.left + band.right;
            if (power <= minimum) continue;
            // Orthonormal M/S preserves the combined energy of the two lanes.
            result.width[i] = static_cast<float>(std::clamp(.5 - band.cross / power, 0., 1.));
            result.pan[i] = static_cast<float>(std::clamp((band.right - band.left) / power, -1., 1.));
        }
        return result;
    }

    dsp::RealFft fft_;
    std::vector<float> window_, scratch_;
    std::vector<std::complex<float>> left_, right_;
    std::vector<std::uint8_t> binBands_;
    std::array<Power, bandCount> powers_{};
    double totalLeft_ = 0., totalRight_ = 0.;
    bool hasMappedBins_ = false, powersReady_ = false;
    double rate_ = 0.;
    std::uint64_t windowSerial_ = 0;
    std::shared_ptr<const Bands> cached_;
    std::shared_ptr<const Spatial> cachedSpatial_;
    const std::shared_ptr<const Bands> silent_ = std::make_shared<const Bands>();
};

bool valid(const SharedAnalysis::Request &r) {
    if (r.track <= 0 || r.fftSize < 32 || r.fftSize > SharedAnalysis::kMaxFftSize || r.fftSize % 32 ||
        r.hopSize < 0 || r.hopSize > r.fftSize || !r.outputs || (r.outputs & ~allOutputs)) return false;
    if ((r.outputs & SharedAnalysis::ColourWaveform) && r.outputs != SharedAnalysis::ColourWaveform) return false;
    if ((r.outputs & SharedAnalysis::PhaseBandDetail) && !(r.outputs & SharedAnalysis::PhaseBands)) return false;
    if (r.heardSpectrogramGrid && (!(r.outputs & SharedAnalysis::Spectrogram) ||
        (r.outputs & ~(SharedAnalysis::Spectrogram | SharedAnalysis::Spectrum)))) return false;
    using Weighting = dsp::StereoEnvelopeMeter::Weighting;
    if (r.stereoMeter && (r.outputs != SharedAnalysis::Meters || r.stereoMeter->weighting < Weighting::Standard ||
                          r.stereoMeter->weighting > Weighting::K)) return false;
    if ((r.outputs & SharedAnalysis::Strip) &&
        (r.point != engine::ScopePoint::PostFader || r.fftSize != stripFftSize ||
         (r.hopSize != 0 && r.hopSize != stripFftSize / 4) ||
         r.channel != SharedAnalysis::Channel::Mid || r.window != dsp::Window::Hann)) return false;
    auto n = r.fftSize;
    for (const auto factor : {2, 3, 5}) while (n % factor == 0) n /= factor;
    const bool device = r.point == engine::ScopePoint::DeviceInput || r.point == engine::ScopePoint::DeviceOutput;
    return n == 1 && r.window >= dsp::Window::Hann && r.window <= dsp::Window::Blackman &&
           r.channel >= SharedAnalysis::Channel::Mid && r.channel <= SharedAnalysis::Channel::StereoPower &&
           r.point >= engine::ScopePoint::PostFader && r.point <= engine::ScopePoint::DeviceOutput &&
           (device ? r.device > 0 : r.device == 0);
}
SharedAnalysis::Request keyFor(SharedAnalysis::Request r) {
    r.hopSize = r.hopSize ? r.hopSize : r.fftSize / 4;
    r.outputs &= SharedAnalysis::ColourWaveform;
    if (r.outputs) {
        // This continuous domain is identified by the physical source. The
        // 32-sample metadata window only conveys the newest completed boundary.
        r.fftSize = r.hopSize = 32;
        r.channel = SharedAnalysis::Channel::Mid;
        r.window = dsp::Window::Hann;
    }
    if (r.stereoMeter && r.stereoMeter->kScale)
        r.stereoMeter->weighting = dsp::StereoEnvelopeMeter::Weighting::Standard;
    return r;
}
bool usable(const std::shared_ptr<const engine::ScopeTap> &tap, std::int32_t n) {
    return tap && std::isfinite(tap->sampleRate()) && tap->sampleRate() > 0 && tap->capacity() >= n;
}
bool spectralOutput(std::uint32_t outputs) noexcept {
    return (outputs & (SharedAnalysis::Spectrum | SharedAnalysis::Spectrogram |
                       SharedAnalysis::Average1s | SharedAnalysis::RealtimeMaximum |
                       SharedAnalysis::CumulativeMaximum | SharedAnalysis::FilledSpectrum | SharedAnalysis::Strip |
                       SharedAnalysis::CumulativeAverage | SharedAnalysis::RealtimeSigma)) != 0;
}
bool rawSpectralOutput(std::uint32_t outputs) noexcept {
    return (outputs & (SharedAnalysis::Spectrum | SharedAnalysis::Spectrogram)) != 0;
}
std::atomic<std::size_t> processHistoryBytes{0};
struct HistoryBudget {
    explicit HistoryBudget(std::size_t maximum) : limit(std::min(maximum, SharedAnalysis::kMaxHistoryBytes)) {}
    const std::size_t limit;
    std::atomic<std::size_t> bytes{0}, peak{0};
    bool canClaim(std::size_t size) const noexcept {
        return size <= limit && bytes.load(std::memory_order_acquire) <= limit - size &&
               processHistoryBytes.load(std::memory_order_acquire) <= SharedAnalysis::kMaxHistoryBytes - size;
    }
    bool claim(std::size_t size) noexcept {
        auto before = bytes.load(std::memory_order_relaxed);
        do { if (size > limit || before > limit - size) return false; }
        while (!bytes.compare_exchange_weak(before, before + size, std::memory_order_acq_rel));
        auto total = processHistoryBytes.load(std::memory_order_relaxed);
        do {
            if (size > SharedAnalysis::kMaxHistoryBytes || total > SharedAnalysis::kMaxHistoryBytes - size) {
                bytes.fetch_sub(size, std::memory_order_release); return false;
            }
        } while (!processHistoryBytes.compare_exchange_weak(total, total + size, std::memory_order_acq_rel));
        auto high = peak.load(std::memory_order_relaxed);
        while (high < before + size && !peak.compare_exchange_weak(high, before + size, std::memory_order_relaxed)) {}
        return true;
    }
    void release(std::size_t size) noexcept {
        bytes.fetch_sub(size, std::memory_order_release);
        processHistoryBytes.fetch_sub(size, std::memory_order_release);
    }
};
struct HistoryCharge {
    std::shared_ptr<HistoryBudget> budget;
    std::size_t bytes;
    HistoryCharge(std::shared_ptr<HistoryBudget> b, std::size_t n) noexcept : budget(std::move(b)), bytes(n) {}
    HistoryCharge(HistoryCharge &&) noexcept = default;
    HistoryCharge(const HistoryCharge &) = delete;
    ~HistoryCharge() { if (budget) budget->release(bytes); }
};
struct HistoryStorage {
    HistoryCharge charge;
    std::vector<float> bins;
    HistoryStorage(HistoryCharge c, std::vector<float> values) : charge(std::move(c)), bins(std::move(values)) {}
};
template <typename Bins>
std::shared_ptr<const std::vector<float>> historyBuffer(const std::shared_ptr<HistoryBudget> &budget,
                                                       Bins &&values) {
    const auto bytes = values.size() * sizeof(float);
    if (!budget->claim(bytes)) return {};
    HistoryCharge charge(budget, bytes);
    auto storage = std::make_shared<HistoryStorage>(std::move(charge), std::forward<Bins>(values));
    return {storage, &storage->bins};
}
// One immutable result handed from the analysis worker (or the owner, when it
// clears a binding) to owner-thread readers. std::atomic<std::shared_ptr> is
// C++20 but libc++ does not provide it, and the atomic_load/atomic_store free
// functions are deprecated in C++20, so a short lock guards the pointer copy
// instead: the same pattern as Domain::resultMutex. Neither side is the audio
// thread (see the header: no audio processor or callback lock is involved).
// A reader gets a complete result or none; the replaced result is released
// after the lock is dropped, so a large picture never frees under it.
template <typename T>
class PublicationSlot {
public:
    [[nodiscard]] std::shared_ptr<const T> load() const noexcept {
        const std::lock_guard lock(mutex_);
        return value_;
    }
    void store(std::shared_ptr<const T> next) noexcept {
        {
            const std::lock_guard lock(mutex_);
            value_.swap(next);
        }
        // `next` now holds the previous result and drops here, unlocked.
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const T> value_;
};
} // namespace
struct SharedAnalysis::CumulativeHistory {
    // Control and worker writes hold the domain lock. Readers own only immutable results.
    std::vector<float> bins;
    PublicationSlot<MaximumResult> publication;
    std::int64_t afterSequence = 0;
    std::uint64_t lastWindow = 0;
    bool binsChanged = false;

    explicit CumulativeHistory(std::int32_t fftSize) : bins(static_cast<std::size_t>(fftSize / 2 + 1)) {}
    void clear() noexcept {
        std::fill(bins.begin(), bins.end(), 0.f);
        lastWindow = 0;
        binsChanged = false;
        publication.store({});
    }
};
struct SharedAnalysis::FilledHistory {
    struct Bin {
        double realtimePower = 0., meanPower = 0.;
        float realtimePeak = 0.f, heldPeak = 0.f;
    };
    std::vector<Bin> bins;
    PublicationSlot<Result> publication;
    std::uint64_t windows = 0, lastWindow = 0;
    std::int64_t afterSequence = 0;

    explicit FilledHistory(std::int32_t fftSize) : bins(static_cast<std::size_t>(fftSize / 2 + 1)) {}
    void clear() noexcept {
        std::fill(bins.begin(), bins.end(), Bin{});
        windows = lastWindow = 0;
        publication.store({});
    }
    void process(std::span<const float> input, double powerStep, double peakStep, std::uint64_t serial) noexcept {
        const auto meanWeight = 1. / static_cast<double>(++windows);
        for (std::size_t i = 0; i < bins.size(); ++i) {
            auto &bin = bins[i];
            const auto magnitude = input[i];
            const auto power = static_cast<double>(magnitude) * magnitude;
            bin.realtimePower += (power - bin.realtimePower) * powerStep;
            bin.meanPower += (power - bin.meanPower) * meanWeight;
            const auto released = bin.realtimePeak + (magnitude - bin.realtimePeak) * peakStep;
            bin.realtimePeak = std::max(magnitude, static_cast<float>(released));
            bin.heldPeak = std::max(bin.heldPeak, magnitude);
        }
        lastWindow = serial;
    }
    void publish(const Result &window) noexcept {
        if (!lastWindow || lastWindow != window.windowSerial) return;
        if (const auto previous = publication.load(); previous && previous->windowSerial == lastWindow &&
            previous->outputs == window.outputs) return;
        try {
            auto curves = std::make_shared<FilledSpectrumResult>();
            curves->processedWindows = windows;
            for (auto &trace : curves->magnitudes) trace.resize(bins.size());
            auto &realAverage = curves->magnitudes[static_cast<std::size_t>(FilledCurve::RealtimeAverage)];
            auto &realPeak = curves->magnitudes[static_cast<std::size_t>(FilledCurve::RealtimeMaximum)];
            auto &held = curves->magnitudes[static_cast<std::size_t>(FilledCurve::HeldMaximum)];
            auto &average = curves->magnitudes[static_cast<std::size_t>(FilledCurve::Average)];
            for (std::size_t i = 0; i < bins.size(); ++i) {
                const auto &bin = bins[i];
                realAverage[i] = static_cast<float>(std::sqrt(bin.realtimePower));
                realPeak[i] = bin.realtimePeak;
                held[i] = bin.heldPeak;
                average[i] = static_cast<float>(std::sqrt(bin.meanPower));
            }
            auto picture = std::make_shared<Result>(window);
            picture->filledSpectrum = std::move(curves);
            publication.store(std::move(picture));
        } catch (...) { /* Keep the previous complete picture after allocation failure. */ }
    }
};
struct SharedAnalysis::CumulativeAverageHistory {
    // Sum and admission are owned by the worker and control under the domain lock.
    std::vector<double> sum;
    PublicationSlot<CumulativeAverageResult> publication;
    std::int64_t afterSequence = 0;
    std::uint64_t windows = 0;
    bool hasSignal = false;

    explicit CumulativeAverageHistory(std::int32_t fftSize) : sum(static_cast<std::size_t>(fftSize / 2 + 1)) {}
    void clear() noexcept {
        std::fill(sum.begin(), sum.end(), 0.);
        windows = 0;
        hasSignal = false;
        publication.store({});
    }
    void add(std::span<const float> magnitudes, bool nonzero) noexcept {
        ++windows;
        if (!nonzero) return;
        hasSignal = true;
        for (std::size_t i = 0; i < sum.size(); ++i)
            sum[i] += magnitudes[i];
    }
    void publish(const std::shared_ptr<const Result> &window) noexcept {
        if (!windows) return;
        if (const auto previous = publication.load(); previous && previous->processedWindows == windows &&
            previous->window->outputs == window->outputs) return;
        try {
            auto next = std::make_shared<CumulativeAverageResult>();
            next->window = window;
            next->processedWindows = windows;
            next->binsDbfs.resize(sum.size(), dsp::kDbFloor);
            if (hasSignal) {
                const auto weight = 1. / static_cast<double>(windows);
                for (std::size_t i = 0; i < sum.size(); ++i)
                    next->binsDbfs[i] = dsp::toDbfs(static_cast<float>(sum[i] * weight));
            }
            publication.store(std::move(next));
        } catch (...) { /* Keep the last complete interval publication. */ }
    }
};
namespace {
class StereoBalanceHistory {
    using Power = std::array<double, 2>;
    struct Bucket {
        Power whole{}, tail{};
        std::size_t frames = 0;
    };
    const std::size_t hop_, wholeHops_, tailFrames_;
    std::vector<Bucket> buckets_;
    std::size_t nextBucket_ = 0, used_ = 0;
    std::int64_t admission_ = 0, windowEnd_ = -1;

    static Power power(std::span<const float> left, std::span<const float> right) noexcept {
        Power result{};
        for (std::size_t i = 0; i < left.size(); ++i) {
            const double l = left[i], r = right[i];
            result[0] += l * l;
            result[1] += r * r;
        }
        return result;
    }
    void append(std::span<const float> left, std::span<const float> right, bool silent) noexcept {
        Bucket bucket;
        bucket.frames = left.size();
        if (!silent) {
            const auto length = std::min(tailFrames_, left.size());
            const auto prefix = left.size() - length;
            bucket.whole = power(left.first(prefix), right.first(prefix));
            bucket.tail = power(left.last(length), right.last(length));
            bucket.whole[0] += bucket.tail[0];
            bucket.whole[1] += bucket.tail[1];
        }
        buckets_[nextBucket_] = bucket;
        nextBucket_ = (nextBucket_ + 1) % buckets_.size();
        used_ = std::min(used_ + 1, buckets_.size());
    }

  public:
    struct Reading {
        std::array<float, 2> rms;
        bool fullSilence;
    };
    StereoBalanceHistory(std::size_t frames, std::size_t hop, std::int64_t admission)
        : hop_(hop), wholeHops_(frames / hop), tailFrames_(frames % hop),
          buckets_(wholeHops_ + static_cast<std::size_t>(tailFrames_ != 0)), admission_(admission) {}
    void reset() noexcept { nextBucket_ = used_ = 0; windowEnd_ = -1; }
    void startAt(std::int64_t admission) noexcept { admission_ = admission; reset(); }
    void rebaseSilentWindow(std::int64_t end) noexcept { windowEnd_ = end; }

    // Each completed FFT window ends on the hop grid. Keep only its newly heard
    // samples, including a possibly partial first hop at this history's admission.
    void advance(std::int64_t start, std::span<const float> left, std::span<const float> right, bool silent) noexcept {
        const auto end = start + static_cast<std::int64_t>(left.size());
        if (end <= std::max(admission_, windowEnd_)) return;
        if (windowEnd_ >= 0 && (start > windowEnd_ || end - windowEnd_ != static_cast<std::int64_t>(hop_))) reset();
        const auto first = std::max({start, admission_, windowEnd_});
        auto offset = static_cast<std::size_t>(first - start);
        auto length = (left.size() - offset) % hop_;
        if (length) {
            append(left.subspan(offset, length), right.subspan(offset, length), silent);
            offset += length;
        }
        while (offset < left.size()) {
            append(left.subspan(offset, hop_), right.subspan(offset, hop_), silent);
            offset += hop_;
        }
        windowEnd_ = end;
    }
    std::optional<Reading> reading(std::int64_t end) const noexcept {
        if (!used_ || end != windowEnd_) return {};
        Power sum{};
        std::size_t frames = 0, slot = nextBucket_;
        // Summing positive buckets avoids subtracting away quiet programme when
        // an earlier, much louder transient rolls out of the two-second interval.
        for (std::size_t i = 0; i < used_; ++i) {
            slot = slot ? slot - 1 : buckets_.size() - 1;
            const auto &bucket = buckets_[slot];
            const bool oldestTail = i == wholeHops_;
            const auto &p = oldestTail ? bucket.tail : bucket.whole;
            sum[0] += p[0];
            sum[1] += p[1];
            frames += oldestTail ? std::min(tailFrames_, bucket.frames) : bucket.frames;
        }
        const auto count = static_cast<double>(frames);
        const auto fullLength = wholeHops_ * hop_ + tailFrames_;
        return Reading{{static_cast<float>(std::sqrt(sum[0] / count)), static_cast<float>(std::sqrt(sum[1] / count))},
                       frames == fullLength && sum[0] == 0. && sum[1] == 0.};
    }
};

class SpectrumLevelHistory {
    using Stereo = std::array<double, 2>;
    static constexpr double silenceGuard = 1.e-15;
    const std::size_t warmup_;
    const double windowFrames_;
    std::vector<Stereo> powers_;
    std::int64_t admission_;
    std::size_t position_ = 0;
    std::uint64_t frames_ = 0;
    Stereo integrated_{}, rolling_{}, crest_{1., 1.}, crestSquared_{1., 1.};
    std::array<std::size_t, 2> rollingNonzero_{};
    std::array<float, 2> samplePeak_{};
    std::array<std::uint64_t, 2> sampleClips_{};

    static float magnitude(float sample) noexcept {
        return std::isfinite(sample) ? std::abs(sample) : 0.f;
    }

  public:
    std::optional<SharedAnalysis::SpectrumLevels> completed;

    SpectrumLevelHistory(double rate, std::int64_t admission)
        : warmup_(static_cast<std::size_t>(std::llround(rate * .125))),
          windowFrames_(static_cast<double>(std::max(1ll, std::llround(rate * .05)))),
          powers_(static_cast<std::size_t>(windowFrames_)), admission_(admission) {}

    void reset() noexcept {
        // Pre-reset ring entries become ineligible until overwritten.
        position_ = 0; frames_ = 0;
        integrated_ = {}; rolling_ = {}; crest_ = {1., 1.}; crestSquared_ = {1., 1.};
        rollingNonzero_ = {}; completed.reset();
        samplePeak_ = {}; sampleClips_ = {};
    }
    void startAt(std::int64_t admission) noexcept { admission_ = admission; reset(); }
    bool silent() const noexcept { return integrated_[0] == 0. && integrated_[1] == 0.; }

    void skipSilence(std::int64_t first, std::int64_t end) noexcept {
        const auto count = std::max(std::int64_t{0}, end - std::max(first, admission_));
        const auto length = static_cast<std::uint64_t>(powers_.size());
        // Fill logically without touching the ring. After a reset the first
        // traversal may still contain stale power values, so clear only the
        // prefix that becomes readable.
        if (frames_ < length && count > 0) {
            const auto fill = static_cast<std::size_t>(std::min(length - frames_, static_cast<std::uint64_t>(count)));
            std::fill_n(powers_.begin() + static_cast<std::ptrdiff_t>(position_), fill, Stereo{});
            position_ = (position_ + fill) % powers_.size();
        }
        frames_ += static_cast<std::uint64_t>(count);
    }

    void advance(std::int64_t first, std::span<const float> left, std::span<const float> right) noexcept {
        const auto begin = static_cast<std::size_t>(std::clamp(admission_ - first,
            std::int64_t{0}, static_cast<std::int64_t>(left.size())));
        for (std::size_t i = begin; i < left.size(); ++i) {
            const auto previous = frames_ < powers_.size() ? Stereo{} : powers_[position_];
            const std::array amplitude{magnitude(left[i]), magnitude(right[i])};
            auto &next = powers_[position_];
            ++frames_;
            const auto count = static_cast<double>(frames_);
            for (std::size_t c = 0; c < next.size(); ++c) {
                const float peak = amplitude[c];
                const double power = static_cast<double>(peak) * peak;
                next[c] = power;
                samplePeak_[c] = std::max(samplePeak_[c], peak);
                sampleClips_[c] += static_cast<std::uint64_t>(peak > 1.f);
                integrated_[c] += next[c];
                rollingNonzero_[c] -= static_cast<std::size_t>(previous[c] > 0.);
                rollingNonzero_[c] += static_cast<std::size_t>(next[c] > 0.);
                rolling_[c] = rollingNonzero_[c] ? std::max(0., (rolling_[c] - previous[c]) + next[c]) : 0.;
                // The silence guard moves a ratio above one toward one. A
                // squared comparison can reject non-records without square roots.
                if (frames_ > warmup_ && rolling_[c] * count > crestSquared_[c] * integrated_[c] * windowFrames_) {
                    const auto candidate = (std::sqrt(rolling_[c] / windowFrames_) + silenceGuard) /
                                           (std::sqrt(integrated_[c] / count) + silenceGuard);
                    if (candidate > crest_[c]) {
                        crest_[c] = candidate;
                        crestSquared_[c] = candidate * candidate;
                    }
                }
            }
            if (++position_ == powers_.size()) position_ = 0;
        }
    }

    void completeWindow() noexcept {
        if (frames_ == 0) return;
        completed = SharedAnalysis::SpectrumLevels{};
        completed->samplePeak = samplePeak_;
        completed->sampleClips = sampleClips_;
        for (std::size_t c = 0; c < integrated_.size(); ++c) {
            completed->integratedRms[c] = std::sqrt(integrated_[c] / static_cast<double>(frames_));
            completed->maxCrestDb[c] = 20. * std::log10(crest_[c]);
        }
    }
};

class SpectrumTruePeakHistory {
  public:
    SpectrumTruePeakHistory(double rate, std::int64_t admission) : admission_(admission) {
        for (auto &meter : meters_) meter.prepare(rate, 1);
    }
    void reset() noexcept {
        for (auto &meter : meters_) meter.reset();
        hasSamples_ = false;
    }
    void startAt(std::int64_t admission) noexcept { admission_ = admission; reset(); }

    void advance(std::int64_t first, std::span<const float> left, std::span<const float> right) noexcept {
        const auto begin = static_cast<std::size_t>(std::clamp(admission_ - first,
            std::int64_t{0}, static_cast<std::int64_t>(left.size())));
        if (begin == left.size()) return;
        const std::array input{left.data() + begin, right.data() + begin};
        const auto frames = static_cast<int>(left.size() - begin);
        for (std::size_t channel = 0; channel < meters_.size(); ++channel)
            meters_[channel].process(&input[channel], 1, frames);
        hasSamples_ = true;
    }
    std::optional<std::array<double, 2>> reading() const noexcept {
        if (!hasSamples_) return {};
        return std::array{meters_[0].peak(), meters_[1].peak()};
    }

  private:
    std::array<dsp::TruePeakMeter, 2> meters_;
    std::int64_t admission_;
    bool hasSamples_ = false;
};
} // namespace
struct SharedAnalysis::SpectrumBlockAverage {
    // Worker and owner mutations hold the domain mutex. Readers see immutable snapshots.
    const std::uint32_t depth;
    std::size_t subscribers = 1;
    std::int64_t afterSequence = 0;
    std::vector<double> sum, completed;
    std::shared_ptr<const Result> completedWindow;
    std::shared_ptr<const BlockAverageResult> publication;
    std::uint32_t windows = 0;
    bool quietGroup = true, settled = false;

    SpectrumBlockAverage(std::uint32_t blocks, std::int32_t fftSize)
        : depth(blocks), sum(static_cast<std::size_t>(fftSize / 2 + 1)), completed(sum.size()) {}

    void setPublication(std::mutex &resultMutex, std::shared_ptr<const BlockAverageResult> result) noexcept {
        const std::lock_guard lock(resultMutex);
        publication.swap(result);
    }

    void reset(std::mutex &resultMutex) noexcept {
        std::fill(sum.begin(), sum.end(), 0.);
        windows = 0;
        quietGroup = true;
        settled = false;
        completedWindow.reset();
        setPublication(resultMutex, {});
    }

    void advance(const std::shared_ptr<const Result> &window, std::span<const float> bins, bool nonzero) noexcept {
        if (window->firstSequence < afterSequence) return;
        if (nonzero) {
            settled = false;
            quietGroup = false;
            for (std::size_t bin = 0; bin < bins.size(); ++bin)
                sum[bin] += static_cast<double>(bins[bin]);
        }
        if (++windows == depth) {
            completed.swap(sum);
            std::fill(sum.begin(), sum.end(), 0.);
            completedWindow = window;
            settled = quietGroup;
            windows = 0;
            quietGroup = true;
        }
    }

    void publish(std::mutex &resultMutex) noexcept {
        if (!completedWindow) return;
        try {
            auto next = std::make_shared<BlockAverageResult>();
            next->window = completedWindow;
            next->binsDbfs.resize(completed.size());
            std::transform(completed.begin(), completed.end(), next->binsDbfs.begin(), [this](double sumValue) {
                return dsp::toDbfs(static_cast<float>(sumValue / depth));
            });
            setPublication(resultMutex, std::move(next));
        } catch (...) { /* Raw publication stays usable. */ }
        completedWindow.reset();
    }
};
struct SharedAnalysis::Domain {
    explicit Domain(Request r) : request(r),
        ringLeft(static_cast<std::size_t>(r.fftSize)), ringRight(ringLeft.size()),
        left(ringLeft.size()), right(left.size()), gain([&] {
            // Use prepared scratch for exactly coherentGain's float-window/double-sum
            // law. Its standalone helper allocates inside noexcept; construction here
            // keeps recoverable allocation failures outside that boundary.
            dsp::fillWindow(r.window, left);
            double sum = 0;
            for (const float value : left) sum += static_cast<double>(value);
            std::fill(left.begin(), left.end(), 0.f);
            return sum / static_cast<double>(r.fftSize);
        }()) {
        if (request.stereoMeter) meter = std::make_unique<dsp::StereoEnvelopeMeter>();
        if (request.outputs & ColourWaveform) colour = std::make_unique<dsp::ColourWaveformCapture>();
    }
    Request request;
    std::unique_ptr<dsp::Spectrum> spectrum;
    std::unique_ptr<dsp::LinearMagnitudeAverage> average;
    struct MaximumHistory {
        std::vector<float> bins;
        float decayStep = 0.f, magnitudeFloor = 0.f;
        std::int64_t afterSequence = 0;
        std::uint64_t lastWindow = 0;
    };
    std::unique_ptr<MaximumHistory> maximum;
    std::atomic<bool> neutralMaximum{true};
    struct SigmaHistory {
        struct Moments { double mean = 0., variance = 0.; };
        std::vector<Moments> bins;
        double step = 0.;
        std::int64_t afterSequence = 0;
        std::uint64_t lastWindow = 0;
    };
    std::unique_ptr<SigmaHistory> sigma;
    std::atomic<bool> neutralSigma{true};
    std::vector<std::shared_ptr<CumulativeHistory>> cumulativeHistories;
    std::vector<std::shared_ptr<FilledHistory>> filledHistories;
    struct PeriodHistory {
        dsp::PeriodScopeCapture capture;
        std::array<float, dsp::kPeriodScopeSize> window{};
        std::shared_ptr<const dsp::PeriodScopeReading> publication;
        std::int64_t afterSequence = 0;
        std::uint64_t publicationWindow = 0;
        bool complete = false;
    };
    std::unique_ptr<PeriodHistory> period;
    std::atomic<bool> periodReady{false};
    std::atomic<bool> cumulativeReady{true};
    std::vector<std::shared_ptr<CumulativeAverageHistory>> cumulativeAverages;
    std::atomic<bool> cumulativeAverageQuiet{false};
    std::unique_ptr<StereoBalanceHistory> balanceHistory;
    std::atomic<bool> balanceActive{false};
    std::size_t spectrumLevelUsers = 0;
    std::unique_ptr<SpectrumLevelHistory> spectrumLevelHistory;
    std::atomic<bool> spectrumLevelsActive{false}, spectrumLevelsParkedSettled{true};
    std::size_t spectrumTruePeakUsers = 0;
    std::unique_ptr<SpectrumTruePeakHistory> spectrumTruePeakHistory;
    std::atomic<bool> spectrumTruePeakActive{false};
    std::atomic<bool> balanceParkedSettled{true};
    std::size_t balanceUsers = 0;
    std::vector<std::shared_ptr<SpectrumBlockAverage>> blockAverages;
    std::atomic<bool> blockAveragesQuiet{true};
    std::unique_ptr<dsp::StereoEnvelopeMeter> meter;
    std::unique_ptr<dsp::ColourWaveformCapture> colour;
    bool colourPrepared = false;
    bool meterPrepared = false;
    std::atomic<bool> neutralMeter{false};
    bool averageConfigured = false;
    bool averageExactlyZero = true;
    std::atomic<bool> averageParkedReady{false};
    AverageStatus averageStatus = AverageStatus::Unavailable;
    std::uint64_t averageEpoch = 0, averageWindows = 0, averageWindow = 0, averageBinsWindow = 0;
    std::int64_t averageAfterSequence = 0;
    std::shared_ptr<const std::vector<float>> averageBins;
    std::vector<float> ringLeft, ringRight, left, right, scratch, linear, second;
    std::unique_ptr<PhaseBandPlan> phaseBandPlan;
    const double gain;
    std::size_t position = 0, filled = 0, sinceWindow = 0;
    std::uint64_t windowSerial = 0, spectralWindow = 0, historyWindow = 0, transportRevision = 0;
    std::atomic<std::uint64_t> droppedHistory{0};
    std::int64_t firstStamp = 0, firstSequence = 0;
    bool advancing = false, hasWindow = false, discontinuous = false;
    struct PendingColumn {
        std::uint64_t serial;
        std::int64_t stamp, sequence;
        std::vector<float> bins;
    };
    std::deque<PendingColumn> pendingHistory;
    std::size_t retiredForPending = 0;
    std::atomic<std::int64_t> consumedSequence{0}, completedWindowEnd{-1};
    // A settled reader can outlive the tap's latest quiet certificate.
    // State::mutex serializes the publisher and tap replacement.
    mutable std::atomic<std::int64_t> observedQuietBegin{0}, observedQuietEnd{0};
    std::atomic<bool> dirty{true}, historyBlocked{false};
    engine::ScopeTap::Cursor cursor;
    std::mutex mutex;
    std::shared_ptr<const engine::ScopeTap> tap;
    mutable std::mutex resultMutex;
    std::shared_ptr<const Result> result;
    std::shared_ptr<const AverageResult> averageResult;
    std::shared_ptr<const MaximumResult> maximumResult;
    std::shared_ptr<const RealtimeSigmaResult> sigmaResult;
    std::shared_ptr<const StereoMeterResult> meterResult;
    std::shared_ptr<const StripResult> stripResult;
    struct StripHistory {
        std::array<float, kStripSpectrumPoints> magnitudes{};
        std::int64_t period = 0, previousEnd = -1;
    };
    std::unique_ptr<StripHistory> strip;
    std::atomic<std::int64_t> stripNextEnd{0};
    std::atomic<bool> neutralStrip{false};
    std::shared_ptr<const Result> readResult() const { std::lock_guard lock(resultMutex); return result; }
    std::shared_ptr<const AverageResult> readAverageResult() const { std::lock_guard lock(resultMutex); return averageResult; }
    std::shared_ptr<const MaximumResult> readMaximumResult() const { std::lock_guard lock(resultMutex); return maximumResult; }
    std::shared_ptr<const RealtimeSigmaResult> readSigmaResult() const { std::lock_guard lock(resultMutex); return sigmaResult; }
    std::shared_ptr<const StereoMeterResult> readMeterResult() const { std::lock_guard lock(resultMutex); return meterResult; }
    std::shared_ptr<const StripResult> readStripResult() const { std::lock_guard lock(resultMutex); return stripResult; }
    void publishStrip(std::shared_ptr<const StripResult> next) {
        std::lock_guard lock(resultMutex);
        stripResult.swap(next);
    }
    void resetStripProjection() noexcept {
        strip->previousEnd = -1;
        neutralStrip.store(false, std::memory_order_release);
        publishStrip({});
    }
    void startStripAt(std::int64_t sequence) noexcept {
        resetStripProjection();
        strip->period = usable(tap, request.fftSize) ? static_cast<std::int64_t>(
            std::clamp(std::round(tap->sampleRate() * stripRefreshSeconds), 1.,
                       static_cast<double>(std::numeric_limits<std::int32_t>::max()))) : 0;
        stripNextEnd.store(sequence + std::max<std::int64_t>(request.fftSize, strip->period), std::memory_order_release);
    }
    std::int64_t stripReadEnd(std::int64_t end) noexcept {
        const auto due = stripNextEnd.load(std::memory_order_relaxed);
        const auto period = strip->period;
        if (!period || end < due) return cursor.next;
        const auto target = end - (end - due) % period;
        if (target <= cursor.next) {
            stripNextEnd.store(target + period, std::memory_order_release);
            return cursor.next;
        }
        const auto first = target - request.fftSize;
        if (first > cursor.next) {
            // Preserve the tap's continuity check when older samples are not part of
            // the window. A seek still differs in stamp, revision, or advancing state.
            if (cursor.hasExpected && cursor.expectedAdvancing) cursor.expectedStamp += first - cursor.next;
            cursor.next = first;
            position = filled = sinceWindow = 0;
        }
        return target;
    }
    void projectStrip(std::int64_t end) {
        const auto due = stripNextEnd.load(std::memory_order_relaxed);
        const auto elapsed = strip->previousEnd < 0 ? 0 : end - strip->previousEnd;
        const auto quiet = tap->lastParkedSilence();
        const bool parked = quiet.end >= end && quiet.begin <= firstSequence;
        const auto weight = elapsed == 0 || parked ? 1.f : static_cast<float>(
            -std::expm1(-static_cast<double>(elapsed) / (stripSmoothingSeconds * tap->sampleRate())));
        auto next = std::make_shared<StripResult>();
        next->generation = tap->generation(); next->sourceEpoch = sourceEpoch;
        next->transportRevision = transportRevision; next->firstStamp = firstStamp;
        next->sampleRate = tap->sampleRate(); next->endSequence = end;
        for (std::size_t i = 0; i < kStripSpectrumPoints; ++i) {
            const auto start = linear.data() + i * linear.size() / kStripSpectrumPoints;
            const auto stop = linear.data() + (i + 1) * linear.size() / kStripSpectrumPoints;
            auto &magnitude = strip->magnitudes[i];
            const auto input = *std::max_element(start, stop);
            magnitude = weight == 1.f ? input : std::lerp(magnitude, input, weight);
            next->spectrumDbfs[i] = dsp::toDbfs(magnitude);
        }
        for (std::size_t i = 0; i < kStripScopeColumns; ++i) {
            const auto first = i * left.size() / kStripScopeColumns;
            const auto last = (i + 1) * left.size() / kStripScopeColumns;
            const auto l = std::minmax_element(left.data() + first, left.data() + last);
            const auto r = std::minmax_element(right.data() + first, right.data() + last);
            next->scope[i] = {{*l.first, *r.first}, {*l.second, *r.second}};
        }
        strip->previousEnd = end;
        neutralStrip.store(parked, std::memory_order_release);
        stripNextEnd.store(end + (strip->period - (end - due) % strip->period), std::memory_order_release);
        publishStrip(std::move(next));
    }
    void publishMeter(std::shared_ptr<const StereoMeterResult> next) {
        std::lock_guard lock(resultMutex);
        meterResult.swap(next);
    }
    void publishResult(std::shared_ptr<const Result> next, std::shared_ptr<const AverageResult> nextAverage = {},
                       std::shared_ptr<const MaximumResult> nextMaximum = {},
                       std::shared_ptr<const RealtimeSigmaResult> nextSigma = {}) {
        {
            std::lock_guard lock(resultMutex);
            result.swap(next); averageResult.swap(nextAverage); maximumResult.swap(nextMaximum);
            sigmaResult.swap(nextSigma);
        }
    }
    void updateAvailability() noexcept {
        const auto bits = outputs.load(std::memory_order_acquire);
        const auto unavailable = (averageConfigured ? 0u : std::uint32_t{Average1s}) |
                                 (maximum ? 0u : std::uint32_t{RealtimeMaximum}) |
                                 (sigma ? 0u : std::uint32_t{RealtimeSigma});
        const bool onlyUnavailable = bits != 0 && (bits & ~unavailable) == 0;
        available.store(usable(tap, request.fftSize) && !onlyUnavailable && (!meter || meterPrepared) &&
                            (!colour || colourPrepared),
                        std::memory_order_release);
    }
    void resetAverageForGap() noexcept {
        if (!averageConfigured) return;
        average->reset(); ++averageEpoch; averageWindows = 0;
        averageWindow = windowSerial; averageBinsWindow = 0;
        averageBins.reset(); averageStatus = AverageStatus::Waiting;
        averageExactlyZero = true;
        averageParkedReady.store(false, std::memory_order_release);
    }
    // Owner-only under the domain lock: no preparation/retry on the worker.
    void activateAverage(std::int64_t sequence) noexcept {
        if (!(outputs.load(std::memory_order_acquire) & Average1s)) return;
        averageConfigured = false; averageStatus = AverageStatus::Unavailable;
        averageExactlyZero = true;
        averageParkedReady.store(false, std::memory_order_release);
        averageAfterSequence = sequence; averageWindows = 0; averageWindow = windowSerial;
        averageBinsWindow = 0; averageBins.reset(); ++averageEpoch;
        try {
            if (usable(tap, request.fftSize)) {
                const dsp::LinearMagnitudeAverage::Settings settings{
                    static_cast<std::size_t>(request.fftSize / 2 + 1), tap->sampleRate(), request.hopSize, 1.};
                if (!average) {
                    auto candidate = std::make_unique<dsp::LinearMagnitudeAverage>();
                    if (candidate->prepare(settings)) average = std::move(candidate);
                } else if (average->settings() == settings) average->reset();
                else if (!average->prepare(settings)) { updateAvailability(); return; }
                averageConfigured = average && average->settings() == settings;
                if (averageConfigured) averageStatus = AverageStatus::Waiting;
            }
        } catch (...) { /* explicit unavailable average; retain the working Raw path */ }
        updateAvailability();
    }
    void resetMaximum() noexcept {
        if (maximum) {
            std::fill(maximum->bins.begin(), maximum->bins.end(), 0.f);
            maximum->lastWindow = 0;
        }
        // A requested history must publish one new quiet window before the
        // parked-silence fast path can skip more samples.
        neutralMaximum.store(!maximum, std::memory_order_release);
    }
    // The worker sees only fully prepared history. It never allocates storage
    // for an inactive mode or changes the mode's hop rate.
    void activateMaximum(std::int64_t sequence) noexcept {
        if (!(outputs.load(std::memory_order_acquire) & RealtimeMaximum)) return;
        try {
            if (!usable(tap, request.fftSize)) maximum.reset();
            else {
                if (!maximum) {
                    auto next = std::make_unique<MaximumHistory>();
                    next->bins.resize(linear.size());
                    maximum = std::move(next);
                }
                const double hopSeconds = request.hopSize / tap->sampleRate();
                maximum->decayStep = static_cast<float>(-std::expm1(std::log(.1) * hopSeconds));
                maximum->magnitudeFloor = std::pow(10.f, dsp::kDbFloor / 20.f);
                maximum->afterSequence = sequence;
            }
        } catch (...) { maximum.reset(); }
        resetMaximum();
        updateAvailability();
    }
    void resetSigma() noexcept {
        if (sigma) {
            std::fill(sigma->bins.begin(), sigma->bins.end(), SigmaHistory::Moments{});
            sigma->lastWindow = 0;
        }
        neutralSigma.store(!sigma, std::memory_order_release);
    }
    void activateSigma(std::int64_t sequence) noexcept {
        if (!(outputs.load(std::memory_order_acquire) & RealtimeSigma)) return;
        try {
            if (!usable(tap, request.fftSize)) sigma.reset();
            else {
                if (!sigma) {
                    auto next = std::make_unique<SigmaHistory>();
                    next->bins.resize(linear.size());
                    sigma = std::move(next);
                }
                // Both moments use the half-second coefficient for the one-second Sigma trace.
                sigma->step = -std::expm1(std::log(.1) * (request.hopSize / (.5 * tap->sampleRate())));
                sigma->afterSequence = sequence;
            }
        } catch (...) { sigma.reset(); }
        resetSigma();
        updateAvailability();
    }
    void advanceSigma(bool nonzero) noexcept {
        bool quiet = !nonzero;
        if (nonzero || !neutralSigma.load(std::memory_order_relaxed)) {
            constexpr double nearZero = 1.e-15;
            for (std::size_t i = 0; i < sigma->bins.size(); ++i) {
                auto &bin = sigma->bins[i];
                const double magnitude = linear[i];
                bin.mean += sigma->step * (magnitude - bin.mean);
                const auto residual = magnitude - bin.mean;
                bin.variance += sigma->step * (residual * residual - bin.variance);
                quiet = quiet && bin.mean < nearZero && bin.variance < nearZero * nearZero;
            }
            // Effects this far below the display floor cannot affect a reading.
            if (quiet) std::fill(sigma->bins.begin(), sigma->bins.end(), SigmaHistory::Moments{});
            neutralSigma.store(quiet, std::memory_order_release);
        }
        sigma->lastWindow = windowSerial;
    }
    void addCumulative(const std::shared_ptr<CumulativeHistory> &history, std::int64_t sequence) {
        if (!history) return;
        cumulativeHistories.push_back(history);
        history->afterSequence = sequence;
        cumulativeReady.store(false, std::memory_order_release);
    }
    void dropCumulative(const std::shared_ptr<CumulativeHistory> &history) noexcept {
        if (!history) return;
        std::erase(cumulativeHistories, history);
        cumulativeReady.store(std::ranges::all_of(cumulativeHistories, [](const auto &item) {
            return item->lastWindow != 0;
        }), std::memory_order_release);
    }
    void addCumulativeAverage(const std::shared_ptr<CumulativeAverageHistory> &history, std::int64_t sequence) {
        if (!history) return;
        cumulativeAverages.push_back(history);
        history->afterSequence = sequence;
        cumulativeAverageQuiet.store(false, std::memory_order_release);
    }
    void refreshCumulativeAverageQuiet(bool caughtUp = true) noexcept {
        cumulativeAverageQuiet.store(caughtUp && !cumulativeAverages.empty() &&
            std::ranges::all_of(cumulativeAverages, [](const auto &history) {
                return !history->hasSignal && history->publication.load();
            }), std::memory_order_release);
    }
    void refreshBlockAveragesQuiet() noexcept {
        blockAveragesQuiet.store(std::ranges::all_of(blockAverages, [](const auto &entry) {
            return entry->settled;
        }), std::memory_order_release);
    }
    void prepareBalance() noexcept {
        balanceHistory.reset();
        constexpr auto maximumSupportedRate = 768000.;
        const auto rate = tap ? tap->sampleRate() : 0.;
        if (balanceUsers && rate > 0. && rate <= maximumSupportedRate) {
            const auto frames = static_cast<std::size_t>(std::max(1ll, std::llround(rate * 2.)));
            try {
                balanceHistory = std::make_unique<StereoBalanceHistory>(frames, static_cast<std::size_t>(request.hopSize),
                                                                       tap->written());
            } catch (...) { /* The Spectrum and current-window correlation can still publish. */ }
        }
        balanceActive.store(static_cast<bool>(balanceHistory), std::memory_order_release);
        balanceParkedSettled.store(!balanceHistory, std::memory_order_release);
    }
    void changeBalanceDemand(bool enabled) noexcept {
        if (enabled) {
            if (++balanceUsers == 1) prepareBalance();
        } else if (--balanceUsers == 0) {
            prepareBalance();
        }
    }
    void prepareSpectrumLevels() noexcept {
        spectrumLevelHistory.reset();
        const auto rate = tap ? tap->sampleRate() : 0.;
        if (spectrumLevelUsers && rate > 0. && rate <= 768000.) {
            try { spectrumLevelHistory = std::make_unique<SpectrumLevelHistory>(rate, tap->written()); }
            catch (...) { /* Level statistics are optional. */ }
        }
        spectrumLevelsActive.store(static_cast<bool>(spectrumLevelHistory), std::memory_order_release);
        spectrumLevelsParkedSettled.store(!spectrumLevelHistory, std::memory_order_release);
        if (spectrumTruePeakUsers) prepareSpectrumTruePeak();
        dirty.store(true, std::memory_order_release);
    }
    void changeSpectrumLevelDemand(bool enabled) noexcept {
        if (enabled) {
            if (++spectrumLevelUsers == 1) prepareSpectrumLevels();
        } else if (--spectrumLevelUsers == 0) {
            prepareSpectrumLevels();
        }
    }
    void prepareSpectrumTruePeak() noexcept {
        spectrumTruePeakHistory.reset();
        if (spectrumLevelHistory && spectrumLevelHistory->completed)
            spectrumLevelHistory->completed->truePeak.reset();
        if (spectrumTruePeakUsers && spectrumLevelHistory && tap) {
            try {
                spectrumTruePeakHistory = std::make_unique<SpectrumTruePeakHistory>(tap->sampleRate(), tap->written());
                spectrumLevelsParkedSettled.store(false, std::memory_order_release);
            } catch (...) { /* Sample statistics remain available. */ }
        }
        spectrumTruePeakActive.store(static_cast<bool>(spectrumTruePeakHistory), std::memory_order_release);
    }
    void changeSpectrumTruePeakDemand(bool enabled) noexcept {
        if (enabled) {
            if (++spectrumTruePeakUsers == 1) {
                catchUpParkedSilence();
                prepareSpectrumTruePeak();
            }
        } else if (--spectrumTruePeakUsers == 0) {
            prepareSpectrumTruePeak();
        }
    }
    std::shared_ptr<SpectrumBlockAverage> admitBlockAverage(
        std::uint32_t depth, std::int64_t sequence) {
        if (depth <= 1) return {};
        const auto sameDepth = [depth](const auto &entry) { return entry->depth == depth; };
        if (const auto found = std::ranges::find_if(blockAverages, sameDepth); found != blockAverages.end()) {
            ++(*found)->subscribers;
            return *found;
        }
        auto entry = std::make_shared<SpectrumBlockAverage>(depth, request.fftSize);
        entry->afterSequence = sequence;
        blockAverages.push_back(entry);
        blockAveragesQuiet.store(false, std::memory_order_release);
        return entry;
    }
    void dropBlockAverage(const std::shared_ptr<SpectrumBlockAverage> &entry) noexcept {
        if (entry && --entry->subscribers == 0) {
            std::erase(blockAverages, entry);
            refreshBlockAveragesQuiet();
        }
    }
    void resetWindow(bool epoch, bool resetFilled = true) {
        position = filled = sinceWindow = 0;
        completedWindowEnd.store(-1, std::memory_order_release);
        hasWindow = false; spectralWindow = historyWindow = 0;
        pendingHistory.clear(); retiredForPending = 0;
        historyBlocked.store(false, std::memory_order_release);
        columns.clear();
        resetPeriod();
        if (epoch) {
            ++sourceEpoch;
            if (balanceHistory) {
                balanceHistory->reset();
                balanceParkedSettled.store(false, std::memory_order_release);
            }
            if (spectrumLevelHistory) {
                spectrumLevelHistory->reset();
                spectrumLevelsParkedSettled.store(false, std::memory_order_release);
            }
            if (spectrumTruePeakHistory) spectrumTruePeakHistory->reset();
            resetAverageForGap();
            // A discontinuity clears the history but cannot admit older
            // samples from the same tap. A replacement tap uses startAt().
            resetMaximum();
            resetSigma();
            for (const auto &history : cumulativeHistories) history->clear();
            for (const auto &history : cumulativeAverages) history->clear();
            cumulativeAverageQuiet.store(false, std::memory_order_release);
            for (const auto &entry : blockAverages) entry->reset(resultMutex);
            refreshBlockAveragesQuiet();
            cumulativeReady.store(cumulativeHistories.empty(), std::memory_order_release);
            for (const auto &history : filledHistories) {
                if (resetFilled) history->clear();
                else history->publication.store({});
            }
        }
        publishResult({});
    }
    void bind(std::shared_ptr<const engine::ScopeTap> next, bool fresh) {
        observedQuietBegin.store(0);
        observedQuietEnd.store(0);
        tap = std::move(next);
        cursor = tap ? tap->cursorAtEnd() : engine::ScopeTap::Cursor{};
        if (tap && !fresh) cursor.next = std::max(tap->oldest(), tap->written() - request.fftSize);
        if (phaseBandPlan) phaseBandPlan->bindRate(tap ? tap->sampleRate() : 0.);
        if (balanceUsers) prepareBalance();
        if (spectrumLevelUsers) prepareSpectrumLevels();
        if (meter) {
            neutralMeter.store(false, std::memory_order_release);
            publishMeter({});
            const auto profile = *request.stereoMeter;
            const double releaseSeconds = profile.kScale ? .6 : .3;
            const double integrationSeconds = profile.kScale ? .6 : 1.;
            constexpr double correlationSeconds = .5;
            meterPrepared = usable(tap, request.fftSize) &&
                meter->prepare({tap->sampleRate(), releaseSeconds, integrationSeconds, correlationSeconds, 1.e-6,
                                profile.midSide, profile.weighting, profile.linkedDynamic});
        }
        if (colour) colourPrepared = usable(tap, request.fftSize) && colour->prepare(tap->sampleRate());
        updateAvailability();
        resetWindow(false);
        consumedSequence.store(cursor.next, std::memory_order_release);
        dirty.store(true, std::memory_order_release);
    }
    void startAt(std::int64_t sequence) {
        if (tap) {
            const auto oldest = tap->oldest();
            const auto end = tap->written();
            cursor.next = std::clamp(sequence, oldest, end);
        }
        consumedSequence.store(cursor.next, std::memory_order_release);
        if (balanceHistory) {
            balanceHistory->startAt(cursor.next);
            balanceParkedSettled.store(false, std::memory_order_release);
        }
        if (spectrumLevelHistory) {
            spectrumLevelHistory->startAt(cursor.next);
            spectrumLevelsParkedSettled.store(false, std::memory_order_release);
        }
        if (spectrumTruePeakHistory) spectrumTruePeakHistory->startAt(cursor.next);
        activateAverage(sequence);
        activateMaximum(sequence);
        activatePeriod(sequence);
        activateSigma(sequence);
        if (outputs.load(std::memory_order_acquire) & Strip) startStripAt(sequence);
        for (const auto &blockAverage : blockAverages) {
            blockAverage->afterSequence = sequence;
            blockAverage->reset(resultMutex);
        }
        refreshBlockAveragesQuiet();
        for (const auto &history : cumulativeHistories) {
            history->afterSequence = sequence;
            history->clear();
        }
        for (const auto &history : filledHistories) {
            history->afterSequence = sequence;
            history->clear();
        }
        for (const auto &history : cumulativeAverages) {
            history->afterSequence = sequence;
            history->clear();
        }
        cumulativeAverageQuiet.store(false, std::memory_order_release);
        cumulativeReady.store(cumulativeHistories.empty(), std::memory_order_release);
    }
    bool ensureSpectrum(std::uint32_t bits) {
        if (!spectralOutput(bits)) return true;
        auto nextStrip = (bits & Strip) && !strip ? std::make_unique<StripHistory>() : nullptr;
        if (!spectrum) {
            auto plan = std::make_unique<dsp::Spectrum>(request.fftSize, request.window);
            if (!plan->valid()) return false;
            std::vector<float> time(left.size()), bins(static_cast<std::size_t>(request.fftSize / 2 + 1));
            std::vector<float> stereo(request.channel == Channel::StereoPower ? bins.size() : 0);
            spectrum = std::move(plan); scratch = std::move(time); linear = std::move(bins); second = std::move(stereo);
        }
        if ((bits & Spectrogram) && columns.capacity() < maxHistoryColumns) columns.reserve(maxHistoryColumns);
        if (nextStrip) strip = std::move(nextStrip);
        return true;
    }
    bool ensureOutputPlans(std::uint32_t bits) {
        if (!(bits & PhaseBands) || phaseBandPlan) return ensureSpectrum(bits);
        auto candidate = std::make_unique<PhaseBandPlan>(request.fftSize, request.window);
        if (!candidate->valid() || !ensureSpectrum(bits)) return false;
        candidate->bindRate(tap ? tap->sampleRate() : 0.);
        phaseBandPlan = std::move(candidate);
        return true;
    }
    bool prepareOutputs(std::uint32_t bits) {
        auto capture = (bits & PeriodScope) && !period ? std::make_unique<PeriodHistory>() : nullptr;
        if (!ensureOutputPlans(bits)) return false;
        if (capture) period = std::move(capture);
        return true;
    }
    void resetPeriod() noexcept {
        if (!period) return;
        period->capture.reset();
        period->publication.reset();
        period->publicationWindow = 0;
        period->complete = false;
        periodReady.store(false, std::memory_order_release);
    }
    void activatePeriod(std::int64_t sequence) noexcept {
        if (!period) return;
        resetPeriod();
        period->afterSequence = sequence;
    }
    void feedPeriod(std::span<const float> leftSamples, std::span<const float> rightSamples, std::int64_t first) noexcept {
        if (!period || !(outputs.load(std::memory_order_acquire) & PeriodScope)) return;
        const auto skip = std::max(std::int64_t{0}, period->afterSequence - first);
        if (skip >= static_cast<std::int64_t>(leftSamples.size())) return;
        const auto offset = static_cast<std::size_t>(skip);
        period->capture.feed(leftSamples.subspan(offset), rightSamples.subspan(offset), tap->sampleRate());
    }
    void publishPeriod(Result &latest) noexcept {
        if (!period || !(latest.outputs & PeriodScope)) return;
        if (!period->complete) {
            period->publication.reset();
            latest.periodScope.reset();
            periodReady.store(false, std::memory_order_release);
            return;
        }
        if (period->publicationWindow != latest.windowSerial || !period->publication) try {
            period->publication = std::make_shared<const dsp::PeriodScopeReading>(dsp::measurePeriodScope(period->window));
            period->publicationWindow = latest.windowSerial;
            periodReady.store(true, std::memory_order_release);
        } catch (...) {
            periodReady.store(false, std::memory_order_release);
            return;
        }
        latest.periodScope = period->publication;
    }
    void releaseUnused(std::uint32_t bits) {
        if (!bits && balanceHistory) {
            balanceHistory.reset();
            balanceActive.store(false, std::memory_order_release);
            balanceParkedSettled.store(true, std::memory_order_release);
        }
        if (!bits && spectrumLevelHistory) {
            spectrumLevelHistory.reset();
            spectrumLevelsActive.store(false, std::memory_order_release);
            spectrumLevelsParkedSettled.store(true, std::memory_order_release);
        }
        if (!bits && spectrumTruePeakHistory) {
            spectrumTruePeakHistory.reset();
            spectrumTruePeakActive.store(false, std::memory_order_release);
        }
        if (!(bits & PhaseBands)) phaseBandPlan.reset();
        if (!(bits & Strip) && strip) {
            strip.reset();
            publishStrip({});
        }
        if (!(bits & Average1s)) {
            average.reset(); averageBins.reset(); averageConfigured = false;
            averageStatus = AverageStatus::Unavailable;
            averageParkedReady.store(false, std::memory_order_release);
            std::shared_ptr<const AverageResult> retired;
            { std::lock_guard lock(resultMutex); averageResult.swap(retired); }
        }
        if (!(bits & RealtimeMaximum) && maximum) {
            maximum.reset();
            neutralMaximum.store(true, std::memory_order_release);
            std::shared_ptr<const MaximumResult> retired;
            { std::lock_guard lock(resultMutex); maximumResult.swap(retired); }
        }
        if (!(bits & RealtimeSigma) && sigma) {
            sigma.reset();
            neutralSigma.store(true, std::memory_order_release);
            std::shared_ptr<const RealtimeSigmaResult> retired;
            { std::lock_guard lock(resultMutex); sigmaResult.swap(retired); }
        }
        if (!(bits & Output::Spectrogram)) {
            columns.clear(); pendingHistory.clear(); retiredForPending = 0;
            historyWindow = 0; historyBlocked.store(false, std::memory_order_release);
        }
        if (!(bits & CumulativeMaximum)) {
            std::vector<std::shared_ptr<CumulativeHistory>>().swap(cumulativeHistories);
            cumulativeReady.store(true, std::memory_order_release);
        }
        if (!(bits & FilledSpectrum))
            std::vector<std::shared_ptr<FilledHistory>>().swap(filledHistories);
        if (!(bits & PeriodScope)) {
            period.reset();
            periodReady.store(false, std::memory_order_release);
        }
        if (!(bits & CumulativeAverage) && !cumulativeAverages.empty())
            std::vector<std::shared_ptr<CumulativeAverageHistory>>().swap(cumulativeAverages);
        if (!(bits & DeviceBlockAverage) && !blockAverages.empty()) {
            std::vector<std::shared_ptr<SpectrumBlockAverage>>().swap(blockAverages);
            blockAveragesQuiet.store(true, std::memory_order_release);
        }
        if (!spectralOutput(bits)) {
            spectrum.reset(); spectralWindow = 0;
            std::vector<float>().swap(scratch); std::vector<float>().swap(linear); std::vector<float>().swap(second);
        }
        updateAvailability();
    }
    bool canParkAverage(std::uint32_t bits) const noexcept {
        // Other histories can require their own processing of the same quiet interval.
        constexpr std::uint32_t sharedOutputs = Average1s | Spectrum;
        return (bits & Average1s) && !(bits & ~sharedOutputs) && averageParkedReady.load(std::memory_order_acquire);
    }
    bool canParkCumulativeAverage(std::uint32_t bits) const noexcept {
        // Raw Scope shares the same zero window and has no history to age.
        constexpr std::uint32_t sharedOutputs = CumulativeAverage | Spectrum | Scope;
        return (bits & CumulativeAverage) && !(bits & ~sharedOutputs) &&
               cumulativeAverageQuiet.load(std::memory_order_acquire);
    }
    bool parkedWindowSettled(engine::ScopeTap::ParkedSilence quiet, std::uint32_t bits) const noexcept {
        const auto windowEnd = completedWindowEnd.load(std::memory_order_acquire);
        if ((bits & PeriodScope) && (!periodReady.load(std::memory_order_acquire) ||
            static_cast<double>(windowEnd - quiet.begin) < tap->sampleRate() * .04)) return false;
        return windowEnd <= quiet.end && windowEnd - quiet.begin >= request.fftSize &&
               balanceParkedSettled.load(std::memory_order_acquire) &&
               spectrumLevelsParkedSettled.load(std::memory_order_acquire) &&
               (!(bits & Strip) || neutralStrip.load(std::memory_order_acquire)) &&
               (!(bits & DeviceBlockAverage) || blockAveragesQuiet.load(std::memory_order_acquire)) &&
               (!(bits & CumulativeAverage) || canParkCumulativeAverage(bits)) &&
               !(bits & FilledSpectrum) && (!(bits & Average1s) || canParkAverage(bits)) &&
               (!(bits & RealtimeMaximum) || neutralMaximum.load(std::memory_order_acquire)) &&
               (!(bits & RealtimeSigma) || neutralSigma.load(std::memory_order_acquire));
    }
    bool settledParkedSilence(engine::ScopeTap::ParkedSilence quiet) const noexcept {
        const auto bits = outputs.load(std::memory_order_acquire);
        return parkedWindowSettled(quiet, bits) &&
               (!(bits & CumulativeMaximum) || cumulativeReady.load(std::memory_order_acquire));
    }
    bool keepsParkedHop(std::uint32_t bits) const noexcept {
        constexpr std::uint32_t windowed = Spectrum | Scope | Phase | Spectrogram | PhaseBands |
                                           PhaseBandDetail | RealtimeMaximum | RealtimeSigma;
        return !(bits & ~windowed) || canParkAverage(bits) || canParkCumulativeAverage(bits) || spectrumLevelHistory;
    }
    void skipCertifiedSilenceTo(std::int64_t next) noexcept {
        if (next <= cursor.next) return;
        const auto bits = outputs.load(std::memory_order_relaxed);
        const bool averageParked = canParkAverage(bits);
        const bool cumulativeParked = canParkCumulativeAverage(bits);
        if (spectrumLevelHistory) spectrumLevelHistory->skipSilence(cursor.next, next);
        if (keepsParkedHop(bits)) {
            // The retained history is exactly zero. Omitted samples still
            // belong to the audible sequence and retain the original hop phase.
            const auto frames = next - cursor.next + static_cast<std::int64_t>(sinceWindow);
            const auto omitted = static_cast<std::uint64_t>(frames / request.hopSize);
            sinceWindow = static_cast<std::size_t>(frames % request.hopSize);
            if (omitted > 0) {
                const auto complete = next - static_cast<std::int64_t>(sinceWindow);
                windowSerial += omitted;
                if (averageParked) {
                    averageWindows += omitted;
                    averageWindow = windowSerial;
                    if (averageBins) averageBinsWindow = averageWindow;
                }
                if (cumulativeParked) {
                    // Every active interval already has an eligible zero window.
                    // Keep its immutable picture and count each omitted full hop.
                    for (const auto &history : cumulativeAverages) history->windows += omitted;
                }
                if (maximum && maximum->lastWindow) maximum->lastWindow = windowSerial;
                if (sigma && sigma->lastWindow) sigma->lastWindow = windowSerial;
                spectralWindow = windowSerial;
                // Keep the existing raster parked. A newly requested sibling may reuse
                // this window, but must not append one of the omitted quiet columns.
                if (historyWindow) historyWindow = windowSerial;
                firstSequence = complete - request.fftSize;
                completedWindowEnd.store(complete, std::memory_order_release);
            }
        }
        cursor.next = next;
        // The existing FFT ring and the full balance history contain only zeros.
        // Preserve the partial-hop phase when the cursor moves over omitted silence.
        if (balanceHistory)
            balanceHistory->rebaseSilentWindow(next - static_cast<std::int64_t>(sinceWindow));
    }
    void advanceObservedParkedAverage(std::int64_t written) noexcept {
        const auto bits = outputs.load(std::memory_order_relaxed);
        if (!canParkAverage(bits) && !canParkCumulativeAverage(bits)) return;
        const auto begin = observedQuietBegin.load();
        const auto end = std::min(observedQuietEnd.load(), written);
        // Publication writes begin before end. A mixed old end and new begin
        // either fail the range test or the second begin read.
        if (begin <= cursor.next && cursor.next < end && begin == observedQuietBegin.load() &&
            settledParkedSilence({begin, end}))
            skipCertifiedSilenceTo(end);
    }
    void catchUpParkedSilence() noexcept {
        if (!tap) return;
        const auto written = tap->written();
        advanceObservedParkedAverage(written);
        const auto quiet = tap->lastParkedSilence();
        if (quiet.end > cursor.next && settledParkedSilence(quiet))
            skipCertifiedSilenceTo(std::min(quiet.end, written));
        consumedSequence.store(cursor.next, std::memory_order_release);
    }
    void skipParkedBeforeCumulative(engine::ScopeTap::ParkedSilence quiet) noexcept {
        const auto bits = outputs.load(std::memory_order_acquire);
        if (!(bits & CumulativeMaximum) || cumulativeReady.load(std::memory_order_acquire) ||
            cursor.next >= quiet.end || !parkedWindowSettled(quiet, bits)) return;
        // The retained window and skipped range are both certified silent.
        // A reset or new view needs post-admission audio, not earlier zeros.
        auto first = quiet.end;
        for (const auto &history : cumulativeHistories)
            if (!history->lastWindow) first = std::min(first, history->afterSequence);
        // If that boundary has itself rolled out, only certified zero
        // samples may be omitted. A later nonquiet span uses the normal cursor.
        const auto next = std::min(quiet.end, std::max(first, tap->oldest()));
        skipCertifiedSilenceTo(next);
    }
    std::size_t historyColumnBytes() const noexcept {
        return (static_cast<std::size_t>(request.fftSize) / 2 + 1) * sizeof(float);
    }
    bool needsWork(const HistoryBudget &budget, bool observeQuiet = false) const noexcept {
        if (!active.load(std::memory_order_acquire) || !available.load(std::memory_order_acquire) || !tap) return false;
        const auto end = tap->written();
        const auto bits = outputs.load(std::memory_order_acquire);
        if (bits == Strip && end < stripNextEnd.load(std::memory_order_acquire))
            return false;
        if (dirty.load(std::memory_order_acquire) ||
            (historyBlocked.load(std::memory_order_acquire) && budget.canClaim(historyColumnBytes()))) return true;
        const auto consumed = consumedSequence.load(std::memory_order_acquire);
        if (end == consumed) return false;
        if (colour) return true; // real silence also advances the waveform
        const auto quiet = tap->lastParkedSilence();
        if (meter) return !(neutralMeter.load(std::memory_order_acquire) && quiet.begin < consumed && quiet.end >= end);
        const bool parked = quiet.end >= end && settledParkedSilence(quiet);
        if (observeQuiet && parked && (canParkAverage(bits) || canParkCumulativeAverage(bits)) &&
            end > observedQuietEnd.load()) {
            // Only preserve committed samples from the verified run. Do not
            // infer anything about the gap if the next run replaces it.
            observedQuietBegin.store(quiet.begin);
            observedQuietEnd.store(end);
        }
        return !parked;
    }
    std::atomic<bool> active{true}, available{false};
    std::atomic<std::uint32_t> outputs{0};
    std::array<std::size_t, std::bit_width(allOutputs)> outputUsers{}; // State mutex
    std::size_t consumers = 0;
    std::uint64_t sourceEpoch = 1, revision = 0, columnSerial = 0, gaps = 0, overruns = 0;
    std::vector<Column> columns;
    void changeOutputs(std::uint32_t bits, bool add) {
        const auto previous = outputs.load(std::memory_order_relaxed);
        std::uint32_t unionBits = 0;
        for (std::size_t i = 0; i < outputUsers.size(); ++i) {
            if (bits & (1u << i)) { if (add) ++outputUsers[i]; else --outputUsers[i]; }
            if (outputUsers[i]) unionBits |= 1u << i;
        }
        outputs.store(unionBits, std::memory_order_release);
        // Strip owns a separate publication. Without it, the remaining readers
        // can keep the completed window and any existing pending projection.
        const bool onlyStripRemoved = !add && unionBits != 0 && (previous ^ unionBits) == Strip;
        if (previous != unionBits && !onlyStripRemoved) dirty.store(true, std::memory_order_release);
    }
    void appendHistory(Column column) {
        if (columns.size() == maxHistoryColumns) columns.erase(columns.begin());
        columns.push_back(std::move(column));
    }
    void recordHistory(const std::shared_ptr<HistoryBudget> &budget, const std::vector<float> &bins) {
        historyWindow = windowSerial;
        ++columnSerial;
        if (pendingHistory.empty())
            if (auto storage = historyBuffer(budget, bins)) {
                appendHistory({columnSerial, firstStamp, std::move(storage), firstSequence});
                return;
            }
        // Stage up to a worker pass where practical, with a bounded scratch
        // budget. Immutable backing remains charged until its last reader.
        const auto bytes = historyColumnBytes();
        const auto capacity = std::min({kMaxWindowsPerFrame, pendingHistoryBytesPerDomain / bytes,
                                       std::max<std::size_t>(1, budget->limit / bytes)});
        if (pendingHistory.size() == capacity) {
            auto &oldest = pendingHistory.front();
            oldest.serial = columnSerial;
            oldest.stamp = firstStamp;
            oldest.sequence = firstSequence;
            std::copy(bins.begin(), bins.end(), oldest.bins.begin());
            std::rotate(pendingHistory.begin(), pendingHistory.begin() + 1, pendingHistory.end());
            droppedHistory.fetch_add(1, std::memory_order_relaxed);
        } else pendingHistory.push_back({columnSerial, firstStamp, firstSequence, bins});
        historyBlocked.store(true, std::memory_order_release);
    }
    bool flushPendingHistory(const std::shared_ptr<HistoryBudget> &budget) {
        if (pendingHistory.empty()) return false;
        bool changed = false;
        while (!pendingHistory.empty() && budget->canClaim(historyColumnBytes())) {
            auto &column = pendingHistory.front();
            auto storage = historyBuffer(budget, std::move(column.bins));
            if (!storage) break;
            appendHistory({column.serial, column.stamp, std::move(storage), column.sequence});
            pendingHistory.pop_front();
            if (retiredForPending) --retiredForPending;
            changed = true;
        }
        if (pendingHistory.empty()) {
            retiredForPending = 0;
            historyBlocked.store(false, std::memory_order_release);
        }
        return changed;
    }
    bool retireForPending(const HistoryBudget &budget) {
        if (pendingHistory.size() <= retiredForPending || budget.canClaim(historyColumnBytes())) return false;
        // Retire only the slots requested by this pending batch. Older
        // publications may hold those bins until the next paint or an unfreeze.
        const auto count = std::min(columns.size(), pendingHistory.size() - retiredForPending);
        if (!count) return false;
        columns.erase(columns.begin(), columns.begin() + static_cast<std::ptrdiff_t>(count));
        retiredForPending += count;
        droppedHistory.fetch_add(count, std::memory_order_relaxed);
        return true;
    }
    void setHistory(Result &publication) const {
        publication.historySerial = columnSerial;
        publication.historyLimited = historyBlocked.load(std::memory_order_acquire);
        publication.droppedHistoryColumns = droppedHistory.load(std::memory_order_relaxed);
        publication.columns = columns; // immutable bin buffers are shared, never deep-copied
    }
    std::shared_ptr<Result> analyse(const std::shared_ptr<HistoryBudget> &budget, std::size_t &transforms, bool newWindow = true) {
        if (!hasWindow) return {};
        const auto bits = outputs.load(std::memory_order_acquire);
        const auto n = left.size();
        const bool averageHopEligible = (bits & Average1s) && averageConfigured && newWindow && averageWindow != windowSerial &&
            firstSequence > averageAfterSequence - static_cast<std::int64_t>(n);
        const bool maximumHopEligible = maximum && newWindow && firstSequence >= maximum->afterSequence;
        const bool sigmaHopEligible = sigma && (bits & RealtimeSigma) && newWindow && firstSequence >= sigma->afterSequence;
        const bool cumulativeHop = (bits & CumulativeMaximum) && newWindow &&
            std::ranges::any_of(cumulativeHistories, [this](const auto &history) {
                return firstSequence >= history->afterSequence;
            });
        const bool cumulativeAverageHop = (bits & CumulativeAverage) && newWindow &&
            std::ranges::any_of(cumulativeAverages, [this](const auto &history) {
                return firstSequence >= history->afterSequence;
            });
        const auto windowEnd = firstSequence + static_cast<std::int64_t>(n);
        const bool stripHop = (bits & Strip) && newWindow && strip->period &&
                              windowEnd >= stripNextEnd.load(std::memory_order_relaxed);
        const bool filledHop = (bits & FilledSpectrum) && newWindow &&
            std::ranges::any_of(filledHistories, [this](const auto &history) { return firstSequence >= history->afterSequence; });
        const bool needsSpectrum = rawSpectralOutput(bits) || averageHopEligible || maximumHopEligible || sigmaHopEligible ||
                                   cumulativeHop || cumulativeAverageHop || stripHop || filledHop;
        if (needsSpectrum && !spectrum) return {};
        auto latest = std::make_shared<Result>();
        latest->request = request; latest->outputs = bits; latest->request.outputs = bits;
        latest->sourceEpoch = sourceEpoch; latest->generation = tap->generation();
        latest->revision = ++revision; latest->sampleRate = tap->sampleRate();
        latest->windowSerial = windowSerial;
        latest->transportRevision = transportRevision;
        latest->coherentGain = gain; latest->hopSize = request.hopSize;
        latest->firstSequence = firstSequence; latest->advancing = advancing; latest->firstStamp = firstStamp;
        latest->gapCount = gaps; latest->overrunCount = overruns; latest->discontinuity = discontinuous;
        if (!newWindow) if (const auto previous = readResult()) {
            latest->nonFiniteSamples = previous->nonFiniteSamples;
            if (balanceHistory) latest->balanceRms2s = previous->balanceRms2s;
        }
        double sumL = 0, sumR = 0, cross = 0;
        const bool needsMeters = (bits & Output::Meters) != 0;
        std::size_t clippedSamples = 0;
        bool nonzero = false, clippedLeft = false, clippedRight = false;
        for (std::size_t i = 0; i < n; ++i) {
            const auto k = (position + i) % n;
            const float rawL = newWindow ? ringLeft[k] : left[i], rawR = newWindow ? ringRight[k] : right[i];
            const bool finiteL = std::isfinite(rawL), finiteR = std::isfinite(rawR);
            latest->nonFiniteSamples += static_cast<std::size_t>(!finiteL) + static_cast<std::size_t>(!finiteR);
            const float l = left[i] = finiteL ? std::clamp(rawL, -spectralSampleLimit, spectralSampleLimit) : 0.f;
            const float r = right[i] = finiteR ? std::clamp(rawR, -spectralSampleLimit, spectralSampleLimit) : 0.f;
            clippedLeft = clippedLeft || (finiteL && rawL != l);
            clippedRight = clippedRight || (finiteR && rawR != r);
            sumL += static_cast<double>(l) * l; sumR += static_cast<double>(r) * r;
            cross += static_cast<double>(l) * r;
            const float magnitudeL = std::abs(l), magnitudeR = std::abs(r);
            latest->peak[0] = std::max(latest->peak[0], magnitudeL);
            latest->peak[1] = std::max(latest->peak[1], magnitudeR);
            if (needsMeters)
                clippedSamples += static_cast<std::size_t>(magnitudeL >= 1.f) + static_cast<std::size_t>(magnitudeR >= 1.f);
            if (needsSpectrum) {
                switch (request.channel) {
                case Channel::Mid: scratch[i] = (l + r) * .5f; break;
                case Channel::Side: scratch[i] = (l - r) * .5f; break;
                case Channel::Left: case Channel::StereoPower: scratch[i] = l; break;
                case Channel::Right: scratch[i] = r; break;
                }
                nonzero = nonzero || scratch[i] != 0 || (request.channel == Channel::StereoPower && r != 0);
            }
        }
        latest->clippedSamples = clippedSamples;
        latest->rms = {static_cast<float>(std::sqrt(sumL / static_cast<double>(n))),
                       static_cast<float>(std::sqrt(sumR / static_cast<double>(n)))};
        latest->correlation = sumL > 0 && sumR > 0 ? std::clamp(cross / std::sqrt(sumL * sumR), -1., 1.) : 0.;
        if (balanceHistory && newWindow) {
            balanceHistory->advance(firstSequence, left, right, sumL == 0. && sumR == 0.);
            const auto reading = balanceHistory->reading(windowEnd);
            if (reading) latest->balanceRms2s = reading->rms;
            balanceParkedSettled.store(reading && reading->fullSilence, std::memory_order_release);
        }
        if (spectrumLevelHistory) {
            if (newWindow) {
                spectrumLevelHistory->completeWindow();
                if (spectrumTruePeakHistory && spectrumLevelHistory->completed)
                    spectrumLevelHistory->completed->truePeak = spectrumTruePeakHistory->reading();
                spectrumLevelsParkedSettled.store(spectrumLevelHistory->completed && spectrumLevelHistory->silent(),
                                                  std::memory_order_release);
            }
            latest->spectrumLevels = spectrumLevelHistory->completed;
        }
        if (needsSpectrum) {
            if (spectralWindow != windowSerial) {
                const bool stereoPower = request.channel == Channel::StereoPower;
                // Sanitized float squares are accumulated in double above. Reuse
                // their exact-zero result; no threshold or additional sample scan.
                if (nonzero && (!stereoPower || sumL > 0)) { spectrum->analyse(scratch, linear); ++transforms; }
                else std::fill(linear.begin(), linear.end(), 0.f);
                if (nonzero && stereoPower) {
                    if (sumR > 0) { spectrum->analyse(right, second); ++transforms; }
                    else std::fill(second.begin(), second.end(), 0.f);
                    for (std::size_t i = 0; i < linear.size(); ++i)
                        linear[i] = std::sqrt((linear[i] * linear[i] + second[i] * second[i]) * .5f);
                }
                spectralWindow = windowSerial;
            }
            if (stripHop) projectStrip(windowEnd);
            if (averageHopEligible) {
                const bool clipped = (request.channel != Channel::Right && clippedLeft) ||
                                     (request.channel != Channel::Left && clippedRight);
                averageWindow = windowSerial;
                if (!clipped && average->processChecked(linear)) {
                    if (averageExactlyZero && nonzero) {
                        averageExactlyZero = false;
                        averageParkedReady.store(false, std::memory_order_release);
                    }
                    ++averageWindows;
                    averageStatus = AverageStatus::Ready;
                }
                else {
                    // Refuse broken spectral input; an explicit average-only
                    // gap clears once, and subsequent valid hops recover from zero.
                    if (averageStatus != AverageStatus::InvalidInput) {
                        average->reset(); averageWindows = 0; ++averageEpoch; averageBins.reset(); averageBinsWindow = 0;
                        averageExactlyZero = true;
                        averageParkedReady.store(false, std::memory_order_release);
                    }
                    averageStatus = AverageStatus::InvalidInput;
                }
            }
            if (maximumHopEligible) {
                bool quiet = true;
                for (std::size_t i = 0; i < linear.size(); ++i) {
                    const float input = linear[i];
                    const float previous = maximum->bins[i];
                    const float level = std::max(input, previous + (input - previous) * maximum->decayStep);
                    maximum->bins[i] = level > maximum->magnitudeFloor ? level : 0.f;
                    quiet = quiet && maximum->bins[i] == 0.f;
                }
                maximum->lastWindow = windowSerial;
                neutralMaximum.store(quiet, std::memory_order_release);
            }
            if (sigmaHopEligible) advanceSigma(nonzero);
            if (cumulativeHop) {
                bool ready = true;
                for (const auto &history : cumulativeHistories) {
                    if (firstSequence < history->afterSequence) {
                        ready = false;
                        continue;
                    }
                    if (nonzero) {
                        bool changed = history->binsChanged;
                        for (std::size_t i = 0; i < linear.size(); ++i) {
                            const auto held = history->bins[i];
                            const auto peak = std::max(held, linear[i]);
                            history->bins[i] = peak;
                            changed = peak > held || changed;
                        }
                        history->binsChanged = changed;
                    }
                    history->lastWindow = windowSerial;
                }
                // Once every observer has an eligible window, further certified
                // parked silence cannot change any held bin.
                cumulativeReady.store(ready, std::memory_order_release);
            }
            if (filledHop) {
                constexpr double averagingSeconds = .194;
                const double interval = request.hopSize / (latest->sampleRate * averagingSeconds);
                const double powerStep = -std::expm1(std::log(.0001) * interval);
                const double peakStep = -std::expm1(std::log(.1) * interval);
                for (const auto &history : filledHistories)
                    if (firstSequence >= history->afterSequence)
                        history->process(linear, powerStep, peakStep, windowSerial);
            }
            if (cumulativeAverageHop) {
                for (const auto &history : cumulativeAverages)
                    if (firstSequence >= history->afterSequence) history->add(linear, nonzero);
            }
            if (rawSpectralOutput(bits)) {
                latest->binsDbfs.resize(linear.size());
                std::transform(linear.begin(), linear.end(), latest->binsDbfs.begin(), dsp::toDbfs);
            }
            if ((bits & DeviceBlockAverage) && newWindow) {
                for (const auto &blockAverage : blockAverages) blockAverage->advance(latest, linear, nonzero);
                refreshBlockAveragesQuiet();
            }
            if ((bits & Output::Spectrogram) && historyWindow != windowSerial)
                recordHistory(budget, latest->binsDbfs);
        }
        return latest;
    }
    std::shared_ptr<const AverageResult> makeAverageResult(std::shared_ptr<const Result> window) noexcept {
        if (!(window->outputs & Average1s)) return {};
        try {
            auto publication = std::make_shared<AverageResult>();
            publication->window = std::move(window);
            publication->status = averageConfigured ? averageStatus : AverageStatus::Unavailable;
            publication->epoch = averageEpoch; publication->processedWindows = averageWindows;
            publication->windowSerial = averageWindow;
            if (publication->status == AverageStatus::Ready) {
                if (!averageBins || averageBinsWindow != averageWindow) {
                    const auto values = average->magnitudes();
                    auto projected = std::make_shared<std::vector<float>>(values.size());
                    for (std::size_t i = 0; i < values.size(); ++i)
                        (*projected)[i] = dsp::toDbfs(static_cast<float>(values[i]));
                    averageBins = std::move(projected); averageBinsWindow = averageWindow;
                }
                publication->binsDbfs = averageBins;
            }
            return publication;
        } catch (...) { return {}; } // Raw still publishes; retry only at a genuine publication event.
    }
    std::shared_ptr<const MaximumResult> makeMaximumResult(const std::shared_ptr<const Result> &window) noexcept {
        if (!maximum || maximum->lastWindow == 0 || !(window->outputs & RealtimeMaximum)) return {};
        if (auto previous = readMaximumResult(); previous && previous->window->windowSerial == maximum->lastWindow &&
            previous->window->outputs == window->outputs)
            return previous;
        try {
            auto publication = std::make_shared<MaximumResult>();
            publication->window = window;
            publication->binsDbfs.resize(maximum->bins.size());
            std::transform(maximum->bins.begin(), maximum->bins.end(), publication->binsDbfs.begin(), dsp::toDbfs);
            return publication;
        } catch (...) { return {}; }
    }
    std::shared_ptr<const RealtimeSigmaResult> makeSigmaResult(const std::shared_ptr<const Result> &window) noexcept {
        if (!sigma || !sigma->lastWindow || !(window->outputs & RealtimeSigma)) return {};
        const auto previous = readSigmaResult();
        const bool unchanged = previous && previous->window->windowSerial == sigma->lastWindow;
        if (unchanged && previous->window->outputs == window->outputs) return previous;
        try {
            auto next = std::make_shared<RealtimeSigmaResult>();
            next->window = window;
            if (unchanged) next->binsDbfs = previous->binsDbfs;
            else {
                next->binsDbfs.resize(sigma->bins.size(), dsp::kDbFloor);
                if (!neutralSigma.load(std::memory_order_relaxed)) {
                    std::ranges::transform(sigma->bins, next->binsDbfs.begin(), [](const auto &bin) {
                        return dsp::toDbfs(static_cast<float>(std::sqrt(std::max(0., bin.variance))));
                    });
                }
            }
            return next;
        } catch (...) { return {}; }
    }
    void publishCumulative(const std::shared_ptr<const Result> &window) noexcept {
        if (!(window->outputs & CumulativeMaximum)) return;
        for (const auto &history : cumulativeHistories) {
            if (!history->lastWindow) continue;
            const auto previous = history->publication.load();
            if (previous && !history->binsChanged && previous->window->outputs == window->outputs)
                continue;
            try {
                auto publication = std::make_shared<MaximumResult>();
                publication->window = window;
                publication->binsDbfs.resize(history->bins.size());
                std::transform(history->bins.begin(), history->bins.end(), publication->binsDbfs.begin(), dsp::toDbfs);
                history->publication.store(std::move(publication));
                history->binsChanged = false;
            } catch (...) { /* The previous immutable picture remains available. */ }
        }
    }
    std::shared_ptr<Result> refreshHistoryPublication() {
        const auto previous = readResult();
        if (!previous) return {};
        auto latest = std::make_shared<Result>(*previous);
        latest->revision = ++revision;
        return latest;
    }
    bool pumpColour(std::span<float> leftSamples, std::span<float> rightSamples,
                    std::size_t &samples, std::size_t &reads) {
        const auto end = tap->written();
        bool changedEpoch = false;
        const auto invalidate = [&] {
            ++sourceEpoch;
            ++gaps;
            changedEpoch = true;
            publishResult({});
        };
        while (cursor.next < end && reads < kMaxReadsPerFrame && samples < kMaxSamplesPerFrame) {
            const auto count = std::min({static_cast<std::size_t>(end - cursor.next), leftSamples.size(),
                                         kMaxSamplesPerFrame - samples});
            const auto previousRevision = cursor.expectedRevision;
            const bool wasAdvancing = cursor.expectedAdvancing;
            ++reads;
            const auto read = tap->read(cursor, leftSamples.first(count), rightSamples.first(count));
            using Status = engine::ScopeTap::ReadStatus;
            if (read.status == Status::Overrun || read.status == Status::GenerationChanged) {
                if (read.status == Status::Overrun) ++overruns;
                colour->reset();
                invalidate();
                continue;
            }
            // Physical time continues through loop wraps and a simple Play/Stop.
            if (read.status == Status::Discontinuity && read.transportRevision != previousRevision &&
                (read.advancing == wasAdvancing || read.transportRevision != previousRevision + 1)) {
                colour->reset();
                invalidate();
            }
            if (!read.frames) break;
            const auto frames = static_cast<std::size_t>(read.frames);
            samples += frames;
            if (!colour->feed(leftSamples.first(frames), rightSamples.first(frames), read.firstSequence)) invalidate();
        }
        consumedSequence.store(cursor.next, std::memory_order_release);
        if (!colour->serial() || !active.load(std::memory_order_acquire)) return false;
        const auto prior = readResult();
        if (prior && prior->sourceEpoch == sourceEpoch && prior->windowSerial == colour->serial()) return false;
        try {
            auto latest = std::make_shared<Result>();
            latest->request = request;
            latest->outputs = ColourWaveform;
            latest->generation = tap->generation();
            latest->sourceEpoch = sourceEpoch;
            latest->revision = ++revision;
            latest->windowSerial = colour->serial();
            latest->sampleRate = tap->sampleRate();
            latest->hopSize = request.hopSize;
            latest->transportRevision = cursor.expectedRevision;
            latest->advancing = cursor.expectedAdvancing;
            latest->gapCount = gaps;
            latest->overrunCount = overruns;
            latest->discontinuity = changedEpoch;
            latest->colourWaveform = std::make_shared<const dsp::ColourWaveformReading>(colour->reading());
            latest->firstSequence = latest->colourWaveform->endSequence - request.fftSize;
            latest->firstStamp = latest->advancing
                ? cursor.expectedStamp - (cursor.next - latest->firstSequence) : cursor.expectedStamp;
            latest->pendingFrames = std::max(std::int64_t{0}, tap->written() - cursor.next);
            latest->budgetLimited = latest->pendingFrames > 0 && (samples == kMaxSamplesPerFrame || reads == kMaxReadsPerFrame);
            publishResult(std::move(latest));
            return true;
        } catch (...) {
            dirty.store(true, std::memory_order_release);
            return false;
        }
    }
    void resetMeter() {
        meter->reset();
        neutralMeter.store(false, std::memory_order_release);
        ++sourceEpoch;
        publishMeter({});
    }
    bool feedMeter(std::span<const float> l, std::span<const float> r) {
        if (meter->process(l, r)) return true;
        // Discard the invalid part and restart at the valid suffix. The suffix is
        // defined by samples, regardless of their audio callback/read boundaries.
        resetMeter();
        auto first = l.size();
        while (first > 0 && std::isfinite(l[first - 1]) && std::isfinite(r[first - 1])) --first;
        return first < l.size() && meter->process(l.subspan(first), r.subspan(first));
    }
    bool pumpMeter(std::span<float> l, std::span<float> r, std::size_t &samples, std::size_t &reads) {
        const auto end = tap->written();
        if (neutralMeter.load(std::memory_order_relaxed)) {
            // A fully empty reading has never accumulated a nonzero peak. The
            // certified parked zeros keep its history exactly neutral through wrap.
            // Read the first block of a new certificate to observe its epoch.
            const auto quiet = tap->lastParkedSilence();
            if (quiet.begin < cursor.next && quiet.end > cursor.next)
                cursor.next = std::min(end, quiet.end);
        }
        bool advanced = false;
        while (cursor.next < end && reads < kMaxReadsPerFrame && samples < kMaxSamplesPerFrame) {
            const auto count = std::min({static_cast<std::size_t>(end - cursor.next), l.size(),
                                         kMaxSamplesPerFrame - samples});
            const auto previousRevision = cursor.expectedRevision;
            const bool wasAdvancing = cursor.expectedAdvancing;
            ++reads;
            const auto read = tap->read(cursor, l.first(count), r.first(count));
            using Status = engine::ScopeTap::ReadStatus;
            if (read.status == Status::Overrun || read.status == Status::GenerationChanged) {
                resetMeter();
                advanced = false;
                continue;
            }
            // A loop changes the heard stamp, while the source keeps streaming.
            // Play/Stop also bumps Transport's revision once. A seek bumps it
            // without flipping playback, or may be hidden in a larger jump.
            if (read.status == Status::Discontinuity && read.transportRevision != previousRevision &&
                (read.advancing == wasAdvancing || read.transportRevision != previousRevision + 1)) {
                resetMeter();
                advanced = false;
            }
            if (!read.frames) break;
            const auto frames = static_cast<std::size_t>(read.frames);
            samples += frames;
            advanced = feedMeter(l.first(frames), r.first(frames));
        }
        consumedSequence.store(cursor.next, std::memory_order_release);
        if (!advanced || !active.load(std::memory_order_acquire)) return false;
        const auto reading = meter->reading();
        neutralMeter.store(reading == dsp::StereoEnvelopeMeter::Reading{}, std::memory_order_release);
        if (const auto prior = readMeterResult(); prior && prior->reading == reading) return false;
        publishMeter(std::make_shared<StereoMeterResult>(StereoMeterResult{
            reading, tap->sampleRate(), tap->generation(), sourceEpoch}));
        return true;
    }
};
struct SharedAnalysis::CaptureDomain {
    explicit CaptureDomain(CaptureRequest r) : request(r) {}
    CaptureRequest request;
    dsp::TriggeredScope scope;
    std::shared_ptr<const engine::ScopeTap> tap;
    engine::ScopeTap::Cursor cursor;
    std::atomic<std::int64_t> consumed{0};
    std::atomic<bool> active{true}, available{false};
    PublicationSlot<CaptureResult> result;
    std::mutex mutex;
    std::size_t consumers = 0;
    std::uint64_t epoch = 0, revision = 0, gaps = 0, overruns = 0;

    void bind(std::shared_ptr<const engine::ScopeTap> next, std::int64_t first) noexcept {
        tap = std::move(next);
        cursor = tap ? tap->cursorAtEnd() : engine::ScopeTap::Cursor{};
        if (tap) cursor.next = std::clamp(first, tap->oldest(), tap->written());
        consumed.store(cursor.next);
        result.store({});
        ++epoch;
        bool ready = false;
        if (tap) {
            auto settings = request.settings;
            settings.sampleRate = tap->sampleRate();
            if (scope.settings() && *scope.settings() == settings) {
                scope.reset();
                ready = true;
            } else {
                ready = scope.prepare(settings);
            }
        }
        available.store(ready);
    }
    bool needsWork() const noexcept {
        return active.load() && available.load() && tap && tap->written() != consumed.load();
    }
    bool process(std::span<float> left, std::span<float> right, std::size_t &reads, std::size_t &samples) {
        std::lock_guard lock(mutex);
        if (!needsWork()) return false;
        const auto end = tap->written();
        while (active.load(std::memory_order_relaxed) && cursor.next < end &&
               reads < kMaxReadsPerFrame && samples < kMaxSamplesPerFrame) {
            const auto length = std::min({left.size(), kMaxSamplesPerFrame - samples,
                                          static_cast<std::size_t>(end - cursor.next)});
            ++reads;
            const auto read = tap->read(cursor, left.first(length), right.first(length));
            using Status = engine::ScopeTap::ReadStatus;
            if (read.status == Status::Overrun || read.status == Status::GenerationChanged ||
                read.status == Status::Discontinuity) {
                ++gaps;
                if (read.status == Status::Overrun) ++overruns;
                scope.reset();
                ++epoch;
                result.store({});
                if (read.status != Status::Discontinuity) continue;
            }
            if (!read.frames) break;
            auto lane = (request.channel == Channel::Left ? left : right).first(static_cast<std::size_t>(read.frames));
            for (float &sample : lane) if (!std::isfinite(sample)) sample = 0;
            samples += lane.size();
            if (!scope.processTrustedFinite(lane, read.firstSequence)) {
                available.store(false);
                result.store({});
                return false;
            }
        }
        consumed.store(cursor.next);
        if (!active.load(std::memory_order_acquire)) return false;
        const auto current = scope.current();
        const auto previous = scope.previous();
        const auto old = result.load();
        const auto pending = std::max<std::int64_t>(0, tap->written() - cursor.next);
        if (old && old->currentSerial == current.serial && old->current.size() == current.columns.size() &&
            old->pendingFrames == pending) return false;
        if (current.columns.empty() && previous.columns.empty()) return false;
        // Only the last two valid sweeps at the common consumed-sample boundary
        // are visible. Never publish storage still owned by the trigger machine.
        try {
            auto next = std::make_shared<CaptureResult>();
            next->current.assign(current.columns.begin(), current.columns.end());
            next->previous.assign(previous.columns.begin(), previous.columns.end());
            next->currentSerial = current.serial;
            next->previousSerial = previous.serial;
            next->currentTriggerSequence = current.triggerSequence;
            next->previousTriggerSequence = previous.triggerSequence;
            next->generation = tap->generation();
            next->sourceEpoch = epoch;
            next->transportRevision = cursor.expectedRevision;
            next->sampleRate = tap->sampleRate();
            next->gapCount = gaps;
            next->overrunCount = overruns;
            next->pendingFrames = pending;
            if (!active.load(std::memory_order_acquire)) return false;
            next->revision = ++revision;
            result.store(std::move(next));
            return true;
        } catch (const std::bad_alloc &) {
            return false; // keep the last good picture until later input
        }
    }
};
namespace {
struct DifferenceResidualPlans {
    static constexpr auto planSlots = static_cast<std::size_t>(
        std::bit_width(static_cast<unsigned>(SharedAnalysis::kMaxFftSize)));
    std::array<std::unique_ptr<dsp::Spectrum>, planSlots> plans;
    std::vector<float> rightMagnitudes;

    dsp::Spectrum *plan(std::int32_t fftSize) {
        auto &entry = plans[static_cast<std::size_t>(std::countr_zero(static_cast<unsigned>(fftSize)))];
        if (!entry) {
            auto prepared = std::make_unique<dsp::Spectrum>(fftSize);
            if (!prepared->valid()) return nullptr;
            entry = std::move(prepared);
        }
        return entry.get();
    }
};

class DifferenceResidualFrame {
  public:
    void clear() noexcept { source_.reset(); }

    void prepare(const FocusScopeRange &range, std::span<const FocusScopeBarSource> sources,
                 const std::optional<SharedAnalysis::ScopeDifference> &difference) noexcept {
        clear();
        if (!difference || !difference->spectrum || range.frames < 32) return;
        const auto find = [&](engine::ScopeSource source) {
            return std::ranges::find(sources, source, &FocusScopeBarSource::source);
        };
        const auto a = find(difference->a), b = find(difference->b);
        if (a == sources.end() || b == sources.end() || a == b) return;

        constexpr auto maximum = static_cast<std::size_t>(SharedAnalysis::kMaxFftSize);
        // Aim for about 6 Hz per bin while limiting the work to one supported
        // power-of-two frame at the end of the existing bar.
        const auto wanted = static_cast<std::size_t>(std::clamp(std::ceil(range.sampleRate / 6.), 32.,
                                                                static_cast<double>(maximum)));
        const auto size = std::min(std::bit_floor(std::min(range.frames, maximum)), std::bit_ceil(wanted));
        try {
            samples_[0].resize(size);
            if (difference->fold == FocusScopeChannels::Stereo) samples_[1].resize(size);
        } catch (...) {
            return; // Keep the waveform available if spectrum storage fails.
        }
        pair_ = {static_cast<std::size_t>(a - sources.begin()), static_cast<std::size_t>(b - sources.begin())};
        first_ = range.frames - size;
        occupied_ = {};
        source_ = difference;
    }

    void append(std::span<const FocusScopeBarBlock> blocks, std::size_t first, std::size_t count) noexcept {
        if (!source_ || first + count <= first_) return;
        const auto begin = first < first_ ? first_ - first : 0;
        for (std::size_t i = begin; i < count; ++i) {
            const auto a = focusBarSample(blocks[pair_[0]], i), b = focusBarSample(blocks[pair_[1]], i);
            const auto left = a[0] - b[0], right = a[1] - b[1];
            const auto at = first + i - first_;
            switch (source_->fold) {
            case FocusScopeChannels::Stereo: store(1, at, right); store(0, at, left); break;
            case FocusScopeChannels::Left: store(0, at, left); break;
            case FocusScopeChannels::Right: store(0, at, right); break;
            case FocusScopeChannels::Mid: case FocusScopeChannels::Side:
                store(0, at, focusScopeFoldSample(source_->fold, left, right));
                break;
            }
        }
    }

    [[nodiscard]] bool needsTransform() const noexcept {
        return source_ && (occupied_[0] || occupied_[1]);
    }

    std::optional<SharedAnalysis::ScopeDifferenceSpectrum> analyse(DifferenceResidualPlans &cache,
                                                                    std::size_t &transforms) const noexcept {
        if (!source_) return {};
        SharedAnalysis::ScopeDifferenceSpectrum result{*source_, static_cast<std::int32_t>(samples_[0].size()), {}};
        if (!needsTransform()) return result;
        try {
            auto *const plan = cache.plan(result.fftSize);
            if (!plan) return {};
            const auto bins = static_cast<std::size_t>(plan->bins());
            result.binsDbfs.resize(bins);
            auto &primary = result.binsDbfs;
            if (occupied_[0]) { plan->analyse(samples_[0], primary); ++transforms; }
            if (source_->fold == FocusScopeChannels::Stereo) {
                auto &other = cache.rightMagnitudes;
                other.resize(bins);
                if (occupied_[1]) { plan->analyse(samples_[1], other); ++transforms; }
                else std::ranges::fill(other, 0.f);
                for (std::size_t i = 0; i < bins; ++i) {
                    const auto l = static_cast<double>(primary[i]), r = static_cast<double>(other[i]);
                    primary[i] = static_cast<float>(std::sqrt((l * l + r * r) * .5));
                }
            }
            if (!std::ranges::all_of(primary, [](float value) { return std::isfinite(value); })) return {};
            std::ranges::transform(primary, primary.begin(), dsp::toDbfs);
        } catch (...) {
            return {};
        }
        return result;
    }

  private:
    void store(std::size_t channel, std::size_t at, double value) noexcept {
        // Subtract and fold in double before the same PFFFT input guard as
        // the other spectra. Finite float operands can overflow their difference.
        constexpr auto limit = static_cast<double>(spectralSampleLimit);
        const auto sample = static_cast<float>(std::clamp(value, -limit, limit));
        samples_[channel][at] = sample;
        occupied_[channel] = occupied_[channel] || sample != 0.f;
    }

    std::optional<SharedAnalysis::ScopeDifference> source_;
    std::array<std::vector<float>, 2> samples_;
    std::array<std::size_t, 2> pair_{};
    std::array<bool, 2> occupied_{};
    std::size_t first_ = 0;
};
} // namespace

struct SharedAnalysis::BarScopeDomain {
    static constexpr std::size_t chunkFrames = 256;
    static constexpr std::array<float, chunkFrames> silence{};
    struct Lane {
        engine::ScopeSource source;
        std::shared_ptr<const engine::ScopeTap> tap;
        std::uint64_t binding = 0;
        engine::ScopeTap::Cursor cursor;
        engine::ScopeTap::ReadResult chunk;
        std::int64_t chunkEnd = 0;
        std::array<float, chunkFrames> left, right;
        FocusScopeLayerAdjustment adjustment;
        std::optional<FocusScopeChannels> channelView;
    };

    BarScopeDomain(std::span<const engine::ScopeSource> sources, std::size_t width)
        : lanes(sources.size()), blocks(sources.size()), identities(sources.size()), columns(width) {
        for (std::size_t i = 0; i < lanes.size(); ++i) lanes[i].source = sources[i];
    }

    void invalidateProjection() noexcept {
        projection.reset();
        residualFrame.clear();
        result.store({});
        if (status.load() != BarScopeStatus::Idle) status.store(BarScopeStatus::Missed);
        waiting.store(0);
    }

    void invalidate() noexcept {
        invalidateProjection();
        for (auto &lane : lanes) {
            lane.cursor = lane.tap ? lane.tap->cursorAtEnd() : engine::ScopeTap::Cursor{};
            lane.chunk = {};
        }
    }

    void setLayerAdjustments(std::span<const FocusScopeLayerAdjustment> adjustments) noexcept {
        const std::lock_guard lock(mutex);
        if (!active.load()) return;
        bool changed = false;
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            const auto next = i < adjustments.size() ? focusScopeBoundedAdjustment(adjustments[i])
                                                     : FocusScopeLayerAdjustment{};
            changed = changed || lanes[i].adjustment != next;
            lanes[i].adjustment = next;
        }
        // The lane cursor and staged suffix may supply the next full bar.
        if (changed) invalidateProjection();
    }

    void setLayerChannelViews(std::span<const std::optional<FocusScopeChannels>> views) noexcept {
        const std::lock_guard lock(mutex);
        if (!active.load() || (views.empty() && !hasLayerChannelViews)) return;
        bool changed = false;
        hasLayerChannelViews = false;
        const auto inherited = monoRequested.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            auto next = i < views.size() ? views[i] : std::nullopt;
            if (next && !focusScopeIsFolded(*next)) next = FocusScopeChannels::Stereo;
            changed = changed || lanes[i].channelView.value_or(inherited) != next.value_or(inherited);
            hasLayerChannelViews = hasLayerChannelViews || next.has_value();
            lanes[i].channelView = next;
        }
        if (changed) invalidateProjection();
    }

    bool prepareProjection(FocusScopeChannels fold, bool phase, bool sumTruePeak) {
        if (!hasLayerChannelViews)
            return projection->prepare(range, identities, columns, revision, fold, difference, phase, sumTruePeak);
        std::array<std::optional<FocusScopeChannels>, kMaxDomains> views;
        for (std::size_t i = 0; i < lanes.size(); ++i) views[i] = lanes[i].channelView;
        return projection->prepare(range, identities, columns, revision, fold, difference, phase, sumTruePeak,
                                   std::span(views).first(lanes.size()));
    }

    bool request(const FocusScopeRange &next, std::uint64_t expected, std::uint64_t minimum) noexcept {
        if (next.first < 0 || !std::in_range<std::int64_t>(next.frames) || (expected && expected < minimum)) return false;
        const auto finish = focusWindowEnd(next.first, static_cast<std::int64_t>(next.frames));
        if (!finish) return false;
        std::lock_guard lock(mutex);
        if (!active.load() || !(next.sampleRate > 0.) || rate.load() != next.sampleRate) return false;
        const auto firstRevision = std::max(expected, minimum);
        if (status.load() == BarScopeStatus::Pending && range.first == next.first && range.frames == next.frames &&
            expectedRevision == expected && minimumRevision == firstRevision) return true;
        range = next;
        end = *finish;
        expectedRevision = revision = expected;
        minimumRevision = firstRevision;
        projection.reset();
        residualFrame.clear();
        result.store({});
        waiting.store(0);
        status.store(BarScopeStatus::Pending);
        return true;
    }

    void setMonoRequested(FocusScopeChannels fold) noexcept {
        const auto next = focusScopeIsFolded(fold) ? fold : FocusScopeChannels::Stereo;
        if (!hasLayerChannelViews) {
            monoRequested.store(next, std::memory_order_relaxed);
            return;
        }
        const std::lock_guard lock(mutex);
        if (monoRequested.exchange(next, std::memory_order_relaxed) != next) invalidateProjection();
    }

    void setDifferenceRequested(std::optional<ScopeDifference> requested) noexcept {
        bool changeSpectrum = false;
        std::uint64_t version = 0;
        {
            const std::lock_guard lock(differenceMutex);
            if (requested == requestedDifference) return;
            changeSpectrum = (requested && requested->spectrum) || (requestedDifference && requestedDifference->spectrum);
            requestedDifference = requested;
            version = differenceVersion.fetch_add(1, std::memory_order_release) + 1;
        }
        if (!changeSpectrum) return;
        // An old spectrum is no longer a valid completed bar for this request.
        // Leave an in-flight bar to drop its comparison at the next worker pass.
        const std::lock_guard lock(mutex);
        if (status.load() == BarScopeStatus::Ready && seenDifferenceVersion != version)
            invalidateProjection();
    }

    void pollDifferenceRequest() noexcept {
        if (seenDifferenceVersion == differenceVersion.load(std::memory_order_acquire)) return;
        {
            const std::lock_guard lock(differenceMutex);
            difference = requestedDifference;
            seenDifferenceVersion = differenceVersion.load(std::memory_order_relaxed);
        }
        if (projection) projection->discardDifference();
        residualFrame.clear();
    }

    // On the owner this runs under State::mutex, which also protects tap rebinding.
    bool needsWork() const noexcept {
        if (!active.load() || status.load() != BarScopeStatus::Pending || rate.load() == 0.) return false;
        const auto stalled = waiting.load();
        return !stalled || lanes[stalled - 1].tap->written() != waitThrough.load();
    }

    bool prepareLane(std::size_t index, std::int64_t first, std::size_t &reads, std::size_t readLimit,
                     std::size_t &samples) noexcept {
        auto &lane = lanes[index];
        using Status = engine::ScopeTap::ReadStatus;
        const auto earlierEpoch = [&](std::uint64_t seen) { return !projection && seen < minimumRevision; };
        while ((!lane.chunk.frames || lane.chunkEnd <= first || earlierEpoch(lane.chunk.transportRevision)) && active.load()) {
            if (reads >= readLimit || samples >= kMaxSamplesPerFrame) return false;
            const auto written = lane.tap->written();
            const auto wait = [&] { waitThrough.store(written); waiting.store(index + 1); };
            if (lane.cursor.next >= written) { wait(); return false; }
            const auto length = std::min(chunkFrames, kMaxSamplesPerFrame - samples);
            auto cursor = lane.cursor;
            ++reads;
            const auto read = lane.tap->read(cursor, std::span(lane.left).first(length), std::span(lane.right).first(length));
            if (read.status == Status::NotReady) { wait(); return false; }
            // A late overrun can be reported after copying the attempted span.
            samples += read.frames ? static_cast<std::size_t>(read.frames) : length;
            const auto finish = focusWindowEnd(read.firstStamp, read.frames);
            const bool older = earlierEpoch(read.transportRevision);
            // The tap reports a seek as a discontinuity even when the read
            // begins at the new window's first sample. Keep that boundary.
            const bool newEpoch = !projection && lane.cursor.hasExpected &&
                                  lane.cursor.expectedRevision < minimumRevision && !older;
            const bool usable = read.status == Status::Ready || (read.status == Status::Discontinuity && (older || newEpoch));
            if (!usable || !finish || (!older && (!read.advancing || !read.transportRevision))) {
                invalidate();
                return false;
            }
            lane.cursor = cursor;
            lane.chunk = read;
            lane.chunkEnd = *finish;
        }
        if (!active.load()) return false;
        if (lane.chunk.firstStamp > first || (revision && revision != lane.chunk.transportRevision)) {
            invalidate();
            return false;
        }
        revision = lane.chunk.transportRevision;
        return true;
    }

    // Each display interval pulls from the same raw bar but at a shifted
    // stamp. A source outside those edges contributes zeros to every fold.
    bool prepareDisplayBlocks(std::int64_t first, std::size_t &reads, std::size_t readLimit,
                              std::size_t &samples, std::size_t &count) noexcept {
        const auto remaining = end - first;
        count = static_cast<std::size_t>(remaining);
        std::size_t padded = 0;
        bool allSourcesReady = true;
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            const auto &lane = lanes[i];
            const auto offset = static_cast<std::int64_t>(lane.adjustment.offsetSamples);
            const auto relative = first - range.first;
            const bool padding = relative < offset || remaining <= -offset;
            const auto source = padding ? range.first : first - offset;
            // Observe the real source epoch before publishing padding. A lane
            // waiting for a future bar must let its peers drain their rings
            // too, or the later lanes overrun before that bar begins.
            if ((!padding || !projection) && !prepareLane(i, source, reads, readLimit, samples)) {
                if (status.load() != BarScopeStatus::Pending) return false;
                allSourcesReady = false;
                continue;
            }
            std::span<const float> left, right;
            if (padding) {
                const auto available = relative < offset ? std::min(offset - relative, remaining) : remaining;
                const auto zeros = std::min(chunkFrames, static_cast<std::size_t>(available));
                left = right = std::span(silence).first(zeros);
                ++padded;
            } else {
                const auto start = static_cast<std::size_t>(source - lane.chunk.firstStamp);
                const auto length = static_cast<std::size_t>(std::min(lane.chunkEnd, end) - source);
                left = std::span(lane.left).subspan(start, length);
                right = std::span(lane.right).subspan(start, length);
            }
            count = std::min(count, left.size());
            blocks[i] = {identities[i], first, 0, range.sampleRate, true, left, right, lane.adjustment.inverted};
        }
        if (!allSourcesReady) return false;
        if (padded) {
            count = std::min(count, (kMaxSamplesPerFrame - samples) / padded);
            samples += count * padded;
        }
        return count != 0;
    }

    bool publish(DifferenceResidualPlans &cache, std::size_t &windows, std::size_t &transforms) {
        if (!active.load()) return false;
        if (residualFrame.needsTransform()) {
            if (windows >= kMaxWindowsPerFrame) return false;
            ++windows;
        }
        const auto version = seenDifferenceVersion;
        auto residual = residualFrame.analyse(cache, transforms);
        pollDifferenceRequest();
        if (seenDifferenceVersion != version) residual.reset();
        if (!active.load()) return false;
        projection->retainSumTruePeak(sumTruePeakRequested.load(std::memory_order_relaxed));
        result.store(std::make_shared<BarScopeResult>(BarScopeResult{range.first, range.frames,
            range.sampleRate, revision, std::move(projection), std::move(residual)}));
        status.store(BarScopeStatus::Ready);
        return true;
    }

    bool process(std::size_t &reads, std::size_t &samples, DifferenceResidualPlans &cache,
                 std::size_t &windows, std::size_t &transforms, std::size_t &peakPhaseBudget) noexcept {
        std::lock_guard lock(mutex);
        if (!needsWork()) return false;
        waiting.store(0);
        const auto readLimit = std::min(kMaxReadsPerFrame, reads + kMaxReadsPerFrame / 2);
        try {
            while (active.load()) {
                pollDifferenceRequest();
                if (projection && projection->complete()) return publish(cache, windows, transforms);
                const auto captured = projection ? projection->frames() : 0;
                const auto first = range.first + static_cast<std::int64_t>(captured);
                std::size_t count = 0;
                if (!prepareDisplayBlocks(first, reads, readLimit, samples, count)) return false;
                const auto fold = monoRequested.load(std::memory_order_relaxed);
                const auto phase = phaseRequested.load(std::memory_order_relaxed);
                const auto sumTruePeak = sumTruePeakRequested.load(std::memory_order_relaxed);
                if (!projection) {
                    for (std::size_t i = 0; i < lanes.size(); ++i)
                        identities[i] = {lanes[i].source, lanes[i].tap->generation()};
                    projection = std::make_shared<FocusScopeBarProjection>();
                    if (!prepareProjection(fold, phase, sumTruePeak)) {
                        invalidate();
                        return false;
                    }
                    residualFrame.prepare(range, identities, difference);
                } else {
                    projection->retainMono(fold);
                    projection->retainPhase(phase);
                    projection->retainSumTruePeak(sumTruePeak);
                }
                if (projection->filtersSumPeak()) {
                    // Charge both lanes for every FIR phase, including the
                    // fixed tail. The budget belongs to the whole worker pass.
                    const auto phaseGroups = static_cast<std::size_t>(2 * FocusCapturedTruePeak::factorFor(range.sampleRate));
                    const auto tail = static_cast<std::size_t>(dsp::TruePeakMeter::kDelay);
                    const auto groupsLeft = peakPhaseBudget / phaseGroups;
                    if (groupsLeft <= tail) return false;
                    count = std::min(count, groupsLeft - tail);
                    peakPhaseBudget -= (count + (captured + count == range.frames ? tail : 0)) * phaseGroups;
                }
                for (std::size_t i = 0; i < lanes.size(); ++i) {
                    auto &block = blocks[i];
                    block.source = identities[i];
                    block.transportRevision = revision;
                    block.left = block.left.first(count);
                    block.right = block.right.first(count);
                }
                if (!projection->append(blocks)) { invalidate(); return false; }
                residualFrame.append(blocks, captured, count);
            }
        } catch (...) {
            invalidate();
        }
        return false;
    }

    std::vector<Lane> lanes;
    std::vector<FocusScopeBarBlock> blocks;
    std::vector<FocusScopeBarSource> identities;
    const std::size_t columns;
    std::mutex mutex;
    std::atomic<bool> active{true};
    std::atomic<double> rate{0.};
    std::atomic<BarScopeStatus> status{BarScopeStatus::Idle};
    PublicationSlot<BarScopeResult> result;
    std::atomic<std::size_t> waiting{0};
    std::atomic<std::int64_t> waitThrough{0};
    FocusScopeRange range;
    std::uint64_t expectedRevision = 0, minimumRevision = 0, revision = 0;
    std::int64_t end = 0;
    std::atomic<FocusScopeChannels> monoRequested{FocusScopeChannels::Stereo};
    bool hasLayerChannelViews = false;
    std::atomic<bool> phaseRequested{false};
    std::atomic<bool> sumTruePeakRequested{false};
    std::mutex differenceMutex; // held only to copy the request, never while processing samples
    std::optional<ScopeDifference> requestedDifference, difference;
    std::atomic<std::uint64_t> differenceVersion{0};
    std::uint64_t seenDifferenceVersion = 0;
    std::shared_ptr<FocusScopeBarProjection> projection;
    DifferenceResidualFrame residualFrame;
};

struct SharedAnalysis::FocusLagJob {
    FocusLagRequest request;
    std::array<std::uint64_t, 2> bindings{};
    FocusLagStatus status = FocusLagStatus::Pending; // guarded by State::mutex
    std::optional<FocusLagPreview> preview{};
};

struct SharedAnalysis::State {
    struct Tap {
        std::shared_ptr<const engine::ScopeTap> value;
        std::size_t domains = 0;
        bool attached = false; // acquired source demand, including a pending/null tap
        double seconds = .25;
        std::int64_t freshSequence = 0;
        std::uint64_t binding = 0;
    };
    State(AcquireSourceTap a, ReleaseSourceTap r, std::size_t limit)
        : acquire(std::move(a)), release(std::move(r)), owner(std::this_thread::get_id()), historyBudget(std::make_shared<HistoryBudget>(limit)) {}
    void ownerOnly() const { assert(owner == std::this_thread::get_id()); }
    static double historySeconds(std::int32_t size, const std::shared_ptr<const engine::ScopeTap> &tap) {
        return tap && std::isfinite(tap->sampleRate()) && tap->sampleRate() > 0 ?
            std::max(.25, 4. * size / tap->sampleRate()) : .25;
    }
    void bindSource(engine::ScopeSource key, const Tap &tap) {
        for (auto &[request, domain] : domains) if (request.source() == key) {
            std::lock_guard domainLock(domain->mutex);
            domain->bind(tap.value, true);
            domain->startAt(tap.freshSequence);
            ++domain->sourceEpoch;
        }
        for (const auto &capture : captures) if (capture->request.source == key) {
            std::lock_guard domainLock(capture->mutex);
            capture->bind(tap.value, tap.freshSequence);
        }
    }
    std::int32_t maximumFft(engine::ScopeSource key, std::int32_t size = 0) const {
        for (const auto &[request, domain] : domains) {
            (void)domain;
            if (request.source() == key) size = std::max(size, request.fftSize);
        }
        return size;
    }
    void resolveTap(engine::ScopeSource key, Tap &tap, std::int32_t size, bool refresh) {
        if (!acquire) return;
        const auto seconds = historySeconds(size, tap.value);
        if (!refresh && tap.attached && tap.value && seconds == tap.seconds) return;
        tap.attached = true;
        auto next = acquire(key, seconds);
        const auto discovered = next;
        const auto firstSequence = next ? next->written() : 0;
        auto actualSeconds = historySeconds(size, next);
        // Initial rate discovery uses only the minimum lease. Correct its demand
        // before binding a cursor, and never reset an unchanged physical ring.
        if (next && actualSeconds != seconds) next = acquire(key, actualSeconds);
        tap.seconds = actualSeconds;
        const bool replaced = next != tap.value;
        tap.value = std::move(next);
        if (replaced) {
            tap.freshSequence = tap.value == discovered ? firstSequence : (tap.value ? tap.value->written() : 0);
            tap.binding = ++nextTapBinding;
            bindSource(key, tap);
            if (!refresh) refreshBarSources();
        }
    }
    void bindBarSources(BarScopeDomain &bar) noexcept {
        const auto bindingChanged = [this](const auto &lane) {
            const auto &tap = taps.at(lane.source);
            return tap.value != lane.tap || tap.binding != lane.binding;
        };
        if (!std::ranges::any_of(bar.lanes, bindingChanged)) return;
        std::lock_guard lock(bar.mutex);
        for (auto &lane : bar.lanes) {
            const auto &tap = taps.at(lane.source);
            lane.tap = tap.value;
            lane.binding = tap.binding;
        }
        bar.invalidate();
        const auto rate = bar.lanes.front().tap ? bar.lanes.front().tap->sampleRate() : 0.;
        const bool coherent = std::ranges::all_of(bar.lanes, [rate](const auto &lane) {
            return lane.tap && lane.tap->sampleRate() == rate;
        });
        bar.rate.store(coherent ? rate : 0.);
    }
    void refreshBarSources() noexcept {
        for (const auto &bar : bars) bindBarSources(*bar);
    }
    std::size_t jobCount() const noexcept { return domains.size() + captures.size() + bars.size(); }
    const Tap *lagTap(const FocusScopeBarSource &source) const noexcept {
        const auto found = taps.find(source.source);
        if (found == taps.end() || !found->second.domains || !found->second.value ||
            !focus_lag_detail::validSource(source, found->second.value->generation())) return nullptr;
        return &found->second;
    }
    bool lagBindingsCurrent(const FocusLagJob &job) const noexcept {
        if (closed) return false;
        for (std::size_t i = 0; i < job.bindings.size(); ++i) {
            const auto *tap = lagTap(job.request.sources[i]);
            if (!tap || tap->binding != job.bindings[i]) return false;
        }
        return true;
    }
    void runFocusLag(const std::shared_ptr<FocusLagJob> &job) noexcept {
        std::array<std::shared_ptr<const engine::ScopeTap>, 2> sources;
        {
            std::lock_guard lock(mutex);
            if (focusLag != job) return;
            if (!lagBindingsCurrent(*job)) {
                job->status = FocusLagStatus::Refused;
                focusLag.reset();
                return;
            }
            for (std::size_t i = 0; i < sources.size(); ++i) sources[i] = lagTap(job->request.sources[i])->value;
        }
        const auto preview = focusFindLagOnTaps(job->request, sources);
        std::lock_guard lock(mutex);
        if (focusLag != job) return; // owner cancelled while processing
        if (preview && lagBindingsCurrent(*job)) {
            job->preview = preview;
            job->status = FocusLagStatus::Ready;
        } else {
            job->status = FocusLagStatus::Refused;
        }
        focusLag.reset();
    }
    // A failed first attach may already hold an external lease. Drop our
    // bookkeeping before calling the owner, which may query the broker.
    void abandonFirstAttach(engine::ScopeSource key, std::unique_lock<std::mutex> &lock) noexcept {
        const auto at = taps.find(key);
        const bool detach = at->second.attached;
        taps.erase(at);
        lock.unlock();
        if (detach && release) { try { release(key); } catch (...) {} }
    }
    void drop(const std::shared_ptr<Domain> &domain, std::uint32_t outputs,
              const std::shared_ptr<CumulativeHistory> &cumulative,
              const std::shared_ptr<SpectrumBlockAverage> &blockAverage,
              const std::shared_ptr<CumulativeAverageHistory> &cumulativeAverage,
              const std::shared_ptr<FilledHistory> &filled, bool balanceEnabled,
              bool spectrumLevelsEnabled, bool spectrumTruePeakEnabled) noexcept {
        ownerOnly();
        bool detach = false;
        const auto key = domain->request.source();
        {
            std::lock_guard lock(mutex);
            if (closed || !domain->consumers) return;
            domain->changeOutputs(outputs, false);
            --domain->consumers; --consumers;
            if (domain->consumers) {
                std::lock_guard domainLock(domain->mutex);
                if (balanceEnabled) domain->changeBalanceDemand(false);
                if (spectrumTruePeakEnabled) domain->changeSpectrumTruePeakDemand(false);
                if (spectrumLevelsEnabled) domain->changeSpectrumLevelDemand(false);
                domain->dropCumulative(cumulative);
                if (filled) std::erase(domain->filledHistories, filled);
                domain->dropBlockAverage(blockAverage);
                if (cumulativeAverage) {
                    std::erase(domain->cumulativeAverages, cumulativeAverage);
                    domain->refreshCumulativeAverageQuiet();
                }
                domain->releaseUnused(domain->outputs.load(std::memory_order_acquire));
                return;
            }
            domain->active.store(false, std::memory_order_release);
            { std::lock_guard domainLock(domain->mutex); domain->releaseUnused(0); domain->publishResult({}); }
            domains.erase(domain->request);
            const auto at = taps.find(key);
            if (at != taps.end() && --at->second.domains == 0) { detach = at->second.attached; taps.erase(at); }
            else if (at != taps.end() && acquire) {
                // History is the maximum of current consumers, not the maximum ever opened.
                // Renegotiate only on a real decrease; an external longer lease can retain
                // the same ring, in which case no publication/epoch needs invalidation.
                const auto size = maximumFft(key);
                if (historySeconds(size, at->second.value) < at->second.seconds) {
                    try { resolveTap(key, at->second, size, false); }
                    catch (...) { /* retain the earlier history */ }
                }
            }
        }
        if (detach && release) { try { release(key); } catch (...) {} }
    }
    void dropCapture(const std::shared_ptr<CaptureDomain> &domain) noexcept {
        ownerOnly();
        const auto key = domain->request.source;
        bool detach = false;
        {
            std::lock_guard lock(mutex);
            if (closed || !domain->consumers) return;
            --consumers;
            if (--domain->consumers) return;
            domain->active.store(false);
            {
                std::lock_guard domainLock(domain->mutex);
                domain->result.store({});
            }
            std::erase(captures, domain);
            const auto at = taps.find(key);
            if (at != taps.end() && --at->second.domains == 0) { detach = at->second.attached; taps.erase(at); }
        }
        if (detach && release) { try { release(key); } catch (...) {} }
    }
    void dropBarScope(const std::shared_ptr<BarScopeDomain> &bar) noexcept {
        ownerOnly();
        std::array<engine::ScopeSource, kMaxDomains> detach;
        std::size_t detached = 0;
        {
            std::lock_guard lock(mutex);
            if (closed || !bar->active.exchange(false)) return;
            --consumers;
            barMembers -= bar->lanes.size();
            {
                std::lock_guard groupLock(bar->mutex);
                bar->projection.reset();
                bar->result.store({});
            }
            std::erase(bars, bar);
            for (const auto &lane : bar->lanes) {
                const auto at = taps.find(lane.source);
                if (--at->second.domains) continue;
                if (at->second.attached) detach[detached++] = lane.source;
                taps.erase(at);
            }
        }
        if (release) for (std::size_t i = 0; i < detached; ++i) {
            try { release(detach[i]); } catch (...) {}
        }
    }
    bool workPending() const {
        bool pending = false;
        for (const auto &[key, domain] : domains) {
            (void)key;
            // Sample a parked average even if an unrelated reader needs a pass.
            if (!pending || domain->averageParkedReady.load(std::memory_order_relaxed) ||
                domain->cumulativeAverageQuiet.load(std::memory_order_relaxed))
                pending = domain->needsWork(*historyBudget, true) || pending;
        }
        return pending || std::any_of(captures.begin(), captures.end(), [](const auto &capture) { return capture->needsWork(); }) ||
               std::ranges::any_of(bars, [](const auto &bar) { return bar->needsWork(); });
    }
    std::size_t pump() {
        // Never audio-reachable. Shutdown waits before releasing owner callbacks.
        std::lock_guard run(runMutex);
        std::unique_lock stateLock(mutex);
        const auto lag = focusLag;
        if (closed || (!workPending() && !lag)) return 0;
        using Job = std::variant<std::shared_ptr<Domain>, std::shared_ptr<CaptureDomain>, std::shared_ptr<BarScopeDomain>>;
        std::array<Job, kMaxDomains> jobs;
        std::size_t count = 0, done = 0, windows = 0, samples = 0, reads = 0;
        std::size_t scopePeakPhaseBudget = 32 * 1024;
        for (const auto &[key, domain] : domains) { (void)key; jobs[count++] = domain; }
        for (const auto &capture : captures) jobs[count++] = capture;
        for (const auto &bar : bars) jobs[count++] = bar;
        stateLock.unlock();
        // One bounded read scratch pair per pass, independent of domain/output type.
        std::array<float, 4096> readLeft, readRight;
        workerPasses.fetch_add(1, std::memory_order_relaxed);
        const auto start = count ? nextJob++ % count : 0;
        for (std::size_t j = 0; j < count && windows < kMaxWindowsPerFrame && samples < kMaxSamplesPerFrame && reads < kMaxReadsPerFrame; ++j) {
            const auto &job = jobs[(start + j) % count];
            if (const auto *bar = std::get_if<std::shared_ptr<BarScopeDomain>>(&job)) {
                if ((*bar)->process(reads, samples, residualPlans, windows, done, scopePeakPhaseBudget))
                    publications.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (const auto *scopeJob = std::get_if<std::shared_ptr<CaptureDomain>>(&job)) {
                if ((*scopeJob)->process(readLeft, readRight, reads, samples))
                    publications.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            auto &d = *std::get<std::shared_ptr<Domain>>(job);
            std::lock_guard lock(d.mutex);
            if (!d.needsWork(*historyBudget)) continue;
            const bool changed = d.dirty.exchange(false, std::memory_order_acq_rel);
            if (d.meter) {
                if (d.pumpMeter(readLeft, readRight, samples, reads)) publications.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (d.colour) {
                if (d.pumpColour(readLeft, readRight, samples, reads)) publications.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::shared_ptr<Result> latest;
            // Make space released since the preceding publication available to
            // its staged columns before this pass can offer new ones.
            const bool historyFlushed = d.flushPendingHistory(historyBudget);
            bool discontinuity = false;
            const auto n = d.left.size(), hop = static_cast<std::size_t>(d.request.hopSize);
            const auto end = d.tap->written();
            const auto quiet = d.tap->lastParkedSilence();
            const auto beforeQuiet = d.cursor.next;
            const auto bits = d.outputs.load(std::memory_order_acquire);
            const bool stripOnly = bits == Strip;
            d.advanceObservedParkedAverage(end);
            d.skipParkedBeforeCumulative(quiet);
            // Resume at the end of a previously parked range. Its omitted samples
            // are identical to those in the retained window. The regular cursor
            // still detects a seek, playback start, or another stamp at the join.
            if (quiet.end < end && quiet.end > d.cursor.next && d.settledParkedSilence(quiet))
                d.skipCertifiedSilenceTo(quiet.end);
            if (d.period && d.cursor.next > beforeQuiet)
                d.period->capture.skipSilence(d.cursor.next - beforeQuiet, d.tap->sampleRate());
            const auto readEnd = stripOnly ? d.stripReadEnd(end) : end;
            while (d.cursor.next < readEnd && windows < kMaxWindowsPerFrame && samples < kMaxSamplesPerFrame && reads < kMaxReadsPerFrame) {
                if (quiet.end >= end && d.settledParkedSilence(quiet)) break;
                const auto remaining = static_cast<std::size_t>(readEnd - d.cursor.next);
                const auto need = d.filled < n ? n - d.filled : stripOnly ? remaining : hop - d.sinceWindow;
                const auto length = std::min({need, kMaxSamplesPerFrame - samples, readLeft.size(),
                                               remaining});
                const auto lastTransportRevision = d.cursor.expectedRevision;
                const bool previouslyAdvancing = d.cursor.expectedAdvancing;
                ++reads;
                auto read = d.tap->read(d.cursor, std::span<float>(readLeft.data(), length),
                                       std::span<float>(readRight.data(), length));
                if (read.status == engine::ScopeTap::ReadStatus::Overrun ||
                    read.status == engine::ScopeTap::ReadStatus::GenerationChanged) {
                    ++d.gaps;
                    if (read.status == engine::ScopeTap::ReadStatus::Overrun) ++d.overruns;
                    d.resetWindow(true); discontinuity = true; latest.reset();
                    if (bits & Strip) d.resetStripProjection();
                    continue;
                }
                if (read.status == engine::ScopeTap::ReadStatus::Discontinuity) {
                    // Stop and a loop wrap keep the filled spectrum's history (analysers queue).
                    const bool stopped = previouslyAdvancing && !read.advancing &&
                        read.transportRevision == lastTransportRevision + 1;
                    const bool looped = previouslyAdvancing && read.advancing &&
                        read.transportRevision == lastTransportRevision;
                    ++d.gaps; d.resetWindow(true, !stopped && !looped); discontinuity = true; latest.reset();
                    if (bits & Strip) d.resetStripProjection();
                }
                if (!read.frames) break;
                const auto frames = static_cast<std::size_t>(read.frames);
                samples += frames;
                if (d.spectrumLevelHistory) {
                    const auto left = std::span(readLeft).first(frames), right = std::span(readRight).first(frames);
                    d.spectrumLevelHistory->advance(read.firstSequence, left, right);
                    if (d.spectrumTruePeakHistory) d.spectrumTruePeakHistory->advance(read.firstSequence, left, right);
                }
                d.feedPeriod(std::span<const float>(readLeft).first(frames),
                             std::span<const float>(readRight).first(frames), read.firstSequence);
                std::size_t first = 0;
                if (d.filled == 0 && d.request.heardSpectrogramGrid) {
                    if (!read.advancing) continue;
                    const auto stride = static_cast<std::int64_t>(hop);
                    const auto residue = read.firstStamp % stride;
                    first = static_cast<std::size_t>(residue <= 0 ? -residue : stride - residue);
                    if (first >= frames) continue;
                }
                for (auto i = first; i < frames; ++i) {
                    d.ringLeft[d.position] = readLeft[i];
                    d.ringRight[d.position] = readRight[i];
                    d.position = (d.position + 1) % n;
                }
                const bool wasFull = d.filled == n;
                const auto accepted = frames - first;
                d.filled = std::min(n, d.filled + accepted);
                if (wasFull && !stripOnly) d.sinceWindow += accepted;
                if (d.filled != n) continue;
                if (stripOnly) {
                    if (d.cursor.next < readEnd || d.cursor.next < d.stripNextEnd.load(std::memory_order_relaxed)) continue;
                } else if (wasFull && d.sinceWindow < hop) continue;
                d.sinceWindow = 0; ++windows; ++d.windowSerial; d.hasWindow = true;
                if (d.period) d.period->complete = d.period->capture.copy(d.period->window);
                d.firstSequence = d.cursor.next - static_cast<std::int64_t>(n);
                d.advancing = read.advancing;
                d.transportRevision = read.transportRevision;
                d.firstStamp = read.firstStamp + (read.advancing ? read.frames - static_cast<std::int64_t>(n) : 0);
                d.discontinuous = discontinuity;
                latest = d.analyse(historyBudget, done);
                if (latest) d.completedWindowEnd.store(d.cursor.next, std::memory_order_release);
            }
            d.consumedSequence.store(d.cursor.next, std::memory_order_release);
            // New output demand can reuse the retained completed window even at idle.
            // A partial hop never replaces that completed window's raw sample pair.
            if (!latest && changed && d.hasWindow) latest = d.analyse(historyBudget, done, false);
            const bool historyRetired = d.retireForPending(*historyBudget);
            if (!latest && (historyFlushed || historyRetired)) latest = d.refreshHistoryPublication();
            if (latest) {
                // Band phase serves the visible picture only. Intermediate
                // overlap windows do not need another stereo transform.
                if (d.phaseBandPlan && (latest->outputs & d.outputs.load(std::memory_order_acquire) & PhaseBands)) {
                    d.phaseBandPlan->project(*latest, d.left, d.right, done);
                }
                // Intermediate hops advance the retained audio window without
                // copying a stereo raw pair that this pass will never publish.
                if ((latest->outputs & (Output::Scope | Output::Phase)) && latest->scopeLeft.empty()) {
                    latest->scopeLeft = d.left;
                    latest->scopeRight = d.right;
                }
                // Only the final publication needs the shallow history snapshot;
                // intermediate hops update the domain, not duplicate metadata.
                d.setHistory(*latest);
                latest->pendingFrames = quiet.end >= end && d.settledParkedSilence(quiet)
                    ? 0 : std::max<std::int64_t>(0, d.tap->written() - d.cursor.next);
                latest->budgetLimited = latest->pendingFrames > 0 &&
                    (windows == kMaxWindowsPerFrame || samples == kMaxSamplesPerFrame || reads == kMaxReadsPerFrame);
                if (d.active.load(std::memory_order_acquire)) {
                    d.publishPeriod(*latest);
                    auto averagePublication = d.makeAverageResult(latest);
                    const bool canPark = d.averageExactlyZero && averagePublication &&
                        averagePublication->status == AverageStatus::Ready;
                    auto maximumPublication = d.makeMaximumResult(latest);
                    d.publishCumulative(latest);
                    if (latest->outputs & FilledSpectrum)
                        for (const auto &history : d.filledHistories) history->publish(*latest);
                    if (latest->outputs & CumulativeAverage) {
                        for (const auto &history : d.cumulativeAverages) history->publish(latest);
                        d.refreshCumulativeAverageQuiet(!latest->budgetLimited);
                    }
                    for (const auto &blockAverage : d.blockAverages) blockAverage->publish(d.resultMutex);
                    auto sigmaPublication = d.makeSigmaResult(latest);
                    d.publishResult(std::move(latest), std::move(averagePublication), std::move(maximumPublication),
                                    std::move(sigmaPublication));
                    d.averageParkedReady.store(canPark, std::memory_order_release);
                    publications.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        transforms.fetch_add(done, std::memory_order_relaxed);
        cursorReads.fetch_add(reads, std::memory_order_relaxed);
        analysisWindows.fetch_add(windows, std::memory_order_relaxed);
        if (lag) runFocusLag(lag);
        return done;
    }
    AcquireSourceTap acquire;
    ReleaseSourceTap release;
    std::thread::id owner;
    std::shared_ptr<HistoryBudget> historyBudget;
    mutable std::mutex mutex;
    std::mutex runMutex;
    std::map<Request, std::shared_ptr<Domain>> domains;
    std::vector<std::shared_ptr<CaptureDomain>> captures;
    std::vector<std::shared_ptr<BarScopeDomain>> bars;
    DifferenceResidualPlans residualPlans; // serialized by the existing worker's runMutex
    std::map<engine::ScopeSource, Tap> taps;
    std::shared_ptr<FocusLagJob> focusLag;
    std::uint64_t nextTapBinding = 0;
    std::size_t consumers = 0, nextJob = 0, barMembers = 0;
    bool closed = false;
    std::atomic<bool> queued{false};
    std::atomic<std::uint64_t> transforms{0}, publications{0}, frameRequests{0}, workerPasses{0};
    std::atomic<std::uint64_t> cursorReads{0}, analysisWindows{0};
    std::atomic<std::uint64_t> successfulEnqueues{0};
};
struct SharedAnalysis::Scheduler {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::weak_ptr<State>> pending;
    std::jthread thread;
    Scheduler() : thread([this](std::stop_token stop) {
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            changed.wait(lock, [&] { return stop.stop_requested() || !pending.empty(); });
            if (stop.stop_requested()) break;
            auto next = pending.front().lock(); pending.pop_front();
            lock.unlock();
            if (next) {
                // Clear before work: at most one further frame can queue while this bounded
                // pass runs. No timer or self-rescheduling loop exists, including on backlog.
                next->queued.store(false, std::memory_order_release);
                try { next->pump(); } catch (...) { /* a failed UI analysis pass cannot stop audio */ }
            }
            lock.lock();
        }
    }) {}
    ~Scheduler() { thread.request_stop(); changed.notify_all(); }
    static Scheduler &instance() { static Scheduler scheduler; return scheduler; }
    void request(const std::shared_ptr<State> &state) {
        if (state->queued.exchange(true, std::memory_order_acq_rel)) return;
        {
            std::lock_guard lock(mutex);
            pending.emplace_back(state);
            state->successfulEnqueues.fetch_add(1, std::memory_order_relaxed);
        }
        changed.notify_one();
    }
};
SharedAnalysis::Lease::Lease(std::weak_ptr<State> state, std::shared_ptr<Domain> domain, std::uint32_t outputs,
                             std::shared_ptr<CumulativeHistory> cumulative,
                             std::shared_ptr<SpectrumBlockAverage> blockAverage,
                             std::shared_ptr<CumulativeAverageHistory> cumulativeAverage,
                             std::shared_ptr<FilledHistory> filled)
    : state_(std::move(state)), domain_(std::move(domain)), cumulative_(std::move(cumulative)),
      spectrumBlockAverage_(std::move(blockAverage)), cumulativeAverage_(std::move(cumulativeAverage)),
      filled_(std::move(filled)), outputs_(outputs) {}
SharedAnalysis::Lease::~Lease() { reset(); }
SharedAnalysis::Lease::Lease(Lease &&other) noexcept
    : state_(std::move(other.state_)), domain_(std::move(other.domain_)), cumulative_(std::move(other.cumulative_)),
      spectrumBlockAverage_(std::move(other.spectrumBlockAverage_)), cumulativeAverage_(std::move(other.cumulativeAverage_)),
      filled_(std::move(other.filled_)),
      outputs_(std::exchange(other.outputs_, 0)), balanceEnabled_(std::exchange(other.balanceEnabled_, false)),
      spectrumLevelsEnabled_(std::exchange(other.spectrumLevelsEnabled_, false)),
      spectrumTruePeakEnabled_(std::exchange(other.spectrumTruePeakEnabled_, false)) {}
SharedAnalysis::Lease &SharedAnalysis::Lease::operator=(Lease &&other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        domain_ = std::move(other.domain_);
        cumulative_ = std::move(other.cumulative_);
        spectrumBlockAverage_ = std::move(other.spectrumBlockAverage_);
        cumulativeAverage_ = std::move(other.cumulativeAverage_);
        filled_ = std::move(other.filled_);
        outputs_ = std::exchange(other.outputs_, 0);
        balanceEnabled_ = std::exchange(other.balanceEnabled_, false);
        spectrumLevelsEnabled_ = std::exchange(other.spectrumLevelsEnabled_, false);
        spectrumTruePeakEnabled_ = std::exchange(other.spectrumTruePeakEnabled_, false);
    }
    return *this;
}
void SharedAnalysis::Lease::reset() noexcept {
    if (domain_) if (auto state = state_.lock())
        state->drop(domain_, outputs_, cumulative_, spectrumBlockAverage_, cumulativeAverage_, filled_, balanceEnabled_,
                    spectrumLevelsEnabled_, spectrumTruePeakEnabled_);
    balanceEnabled_ = false;
    spectrumLevelsEnabled_ = false;
    spectrumTruePeakEnabled_ = false;
    filled_.reset();
    cumulativeAverage_.reset();
    spectrumBlockAverage_.reset();
    cumulative_.reset(); domain_.reset(); state_.reset(); outputs_ = 0;
}
SharedAnalysis::Lease::operator bool() const noexcept { return domain_ && !state_.expired() && domain_->active.load(std::memory_order_acquire); }
bool SharedAnalysis::Lease::available() const noexcept { return static_cast<bool>(*this) && domain_->available.load(std::memory_order_acquire); }
std::shared_ptr<const SharedAnalysis::Result> SharedAnalysis::Lease::read() const noexcept {
    auto result = available() ? (filled_ ? filled_->publication.load() : domain_->readResult()) : nullptr;
    return result && (result->outputs & outputs_) == outputs_ ? result : nullptr;
}
std::shared_ptr<const SharedAnalysis::AverageResult> SharedAnalysis::Lease::readAverage() const noexcept {
    const auto publication = (outputs_ & Average1s) && available() ? domain_->readAverageResult() : nullptr;
    return publication && publication->window && (publication->window->outputs & outputs_) == outputs_ ? publication : nullptr;
}
std::shared_ptr<const SharedAnalysis::BlockAverageResult> SharedAnalysis::Lease::readBlockAverage() const noexcept {
    if (!spectrumBlockAverage_ || !available()) return {};
    const std::lock_guard lock(domain_->resultMutex);
    return spectrumBlockAverage_->publication;
}
std::shared_ptr<const SharedAnalysis::MaximumResult> SharedAnalysis::Lease::readMaximum() const noexcept {
    return readMaximum((outputs_ & RealtimeMaximum) ? RealtimeMaximum : CumulativeMaximum);
}
std::shared_ptr<const SharedAnalysis::MaximumResult> SharedAnalysis::Lease::readMaximum(Output mode) const noexcept {
    if (!(outputs_ & mode) || !available()) return {};
    std::shared_ptr<const MaximumResult> publication;
    if (mode == RealtimeMaximum) publication = domain_->readMaximumResult();
    else if (mode == CumulativeMaximum && cumulative_) publication = cumulative_->publication.load();
    return publication && publication->window && (publication->window->outputs & outputs_) == outputs_ ? publication : nullptr;
}
bool SharedAnalysis::Lease::restartCumulativeMaximum() noexcept {
    const auto state = state_.lock();
    if (!state || !cumulative_ || !available()) return false;
    state->ownerOnly();
    std::lock_guard lock(domain_->mutex);
    if (!domain_->tap) return false;
    cumulative_->afterSequence = domain_->tap->written();
    cumulative_->clear();
    domain_->cumulativeReady.store(false, std::memory_order_release);
    return true;
}
std::shared_ptr<const SharedAnalysis::CumulativeAverageResult> SharedAnalysis::Lease::readCumulativeAverage() const noexcept {
    if (!cumulativeAverage_ || !available()) return {};
    const auto publication = cumulativeAverage_->publication.load();
    return publication && (publication->window->outputs & outputs_) == outputs_ ? publication : nullptr;
}
bool SharedAnalysis::Lease::restartCumulativeAverage() noexcept {
    const auto state = state_.lock();
    if (!state || !cumulativeAverage_ || !available()) return false;
    state->ownerOnly();
    const std::lock_guard lock(domain_->mutex);
    if (!domain_->tap) return false;
    domain_->catchUpParkedSilence();
    cumulativeAverage_->afterSequence = domain_->tap->written();
    cumulativeAverage_->clear();
    domain_->cumulativeAverageQuiet.store(false, std::memory_order_release);
    return true;
}
std::shared_ptr<const SharedAnalysis::RealtimeSigmaResult> SharedAnalysis::Lease::readRealtimeSigma() const noexcept {
    const auto publication = (outputs_ & RealtimeSigma) && available() ? domain_->readSigmaResult() : nullptr;
    return publication && (publication->window->outputs & outputs_) == outputs_ ? publication : nullptr;
}
void SharedAnalysis::Lease::setStereoBalanceEnabled(bool enabled) noexcept {
    if (enabled == balanceEnabled_ || !domain_ || !spectralOutput(outputs_)) return;
    const auto state = state_.lock();
    if (!state || !domain_->active.load(std::memory_order_acquire)) return;
    state->ownerOnly();
    const std::lock_guard lock(domain_->mutex);
    domain_->changeBalanceDemand(enabled);
    balanceEnabled_ = enabled;
}
void SharedAnalysis::Lease::setSpectrumLevelsEnabled(bool enabled) noexcept {
    if (enabled == spectrumLevelsEnabled_ || !domain_ || !spectralOutput(outputs_)) return;
    const auto state = state_.lock();
    if (!state || !domain_->active.load(std::memory_order_acquire)) return;
    state->ownerOnly();
    const std::lock_guard lock(domain_->mutex);
    if (enabled) domain_->catchUpParkedSilence();
    else if (spectrumTruePeakEnabled_) {
        domain_->changeSpectrumTruePeakDemand(false);
        spectrumTruePeakEnabled_ = false;
    }
    domain_->changeSpectrumLevelDemand(enabled);
    spectrumLevelsEnabled_ = enabled;
}
void SharedAnalysis::Lease::setSpectrumTruePeakEnabled(bool enabled) noexcept {
    if (enabled == spectrumTruePeakEnabled_ || !spectrumLevelsEnabled_ || !domain_) return;
    const auto state = state_.lock();
    if (!state || !domain_->active.load(std::memory_order_acquire)) return;
    state->ownerOnly();
    const std::lock_guard lock(domain_->mutex);
    domain_->changeSpectrumTruePeakDemand(enabled);
    spectrumTruePeakEnabled_ = enabled;
}
bool SharedAnalysis::Lease::restartSpectrumLevels() noexcept {
    const auto state = state_.lock();
    if (!spectrumLevelsEnabled_ || !state || !available()) return false;
    state->ownerOnly();
    const std::lock_guard lock(domain_->mutex);
    if (!domain_->tap || !domain_->spectrumLevelHistory || domain_->spectrumLevelUsers != 1) return false;
    // The expanded Spectrum owns this interval. Keep any normal Spectrum
    // readers and the last immutable window on the existing source timeline.
    domain_->catchUpParkedSilence();
    const auto admission = domain_->tap->written();
    domain_->spectrumLevelHistory->startAt(admission);
    if (domain_->spectrumTruePeakHistory) domain_->spectrumTruePeakHistory->startAt(admission);
    domain_->spectrumLevelsParkedSettled.store(false, std::memory_order_release);
    return true;
}
std::shared_ptr<const SharedAnalysis::StereoMeterResult> SharedAnalysis::Lease::readStereoMeter() const noexcept {
    return available() && domain_->meter ? domain_->readMeterResult() : nullptr;
}
bool SharedAnalysis::Lease::restartFilledSpectrum() noexcept {
    const auto state = state_.lock();
    if (!state || !filled_ || !available()) return false;
    state->ownerOnly();
    std::lock_guard lock(domain_->mutex);
    if (!domain_->tap) return false;
    filled_->afterSequence = domain_->tap->written();
    filled_->clear();
    return true;
}
std::shared_ptr<const SharedAnalysis::StripResult> SharedAnalysis::Lease::readStrip() const noexcept {
    return (outputs_ & Strip) && available() ? domain_->readStripResult() : nullptr;
}
bool SharedAnalysis::Lease::restartStereoMeter() noexcept {
    const auto state = state_.lock();
    if (!state || !available() || !domain_->meter) return false;
    state->ownerOnly();
    std::lock_guard lock(domain_->mutex);
    domain_->cursor = domain_->tap->cursorAtEnd();
    domain_->consumedSequence.store(domain_->cursor.next, std::memory_order_release);
    domain_->resetMeter();
    return true;
}
SharedAnalysis::Lease::SourcePosition SharedAnalysis::Lease::sourcePosition() const noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return {};
    state->ownerOnly();
    std::lock_guard lock(domain_->mutex);
    if (!domain_->active.load(std::memory_order_acquire) ||
        !usable(domain_->tap, domain_->request.fftSize)) return {};
    return {domain_->tap->generation(), domain_->tap->written()};
}
double SharedAnalysis::Lease::preparedSampleRate() const noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return 0.;
    state->ownerOnly();
    return domain_->active.load(std::memory_order_acquire) && domain_->tap ? domain_->tap->sampleRate() : 0.;
}
SharedAnalysis::CaptureLease::CaptureLease(std::weak_ptr<State> state, std::shared_ptr<CaptureDomain> domain)
    : state_(std::move(state)), domain_(std::move(domain)) {}
SharedAnalysis::CaptureLease::~CaptureLease() { reset(); }
SharedAnalysis::CaptureLease::CaptureLease(CaptureLease &&) noexcept = default;
SharedAnalysis::CaptureLease &SharedAnalysis::CaptureLease::operator=(CaptureLease &&other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        domain_ = std::move(other.domain_);
    }
    return *this;
}
void SharedAnalysis::CaptureLease::reset() noexcept {
    if (domain_) if (const auto state = state_.lock()) state->dropCapture(domain_);
    domain_.reset();
    state_.reset();
}
SharedAnalysis::CaptureLease::operator bool() const noexcept {
    return domain_ && !state_.expired() && domain_->active.load(std::memory_order_acquire);
}
bool SharedAnalysis::CaptureLease::available() const noexcept {
    return static_cast<bool>(*this) && domain_->available.load(std::memory_order_acquire);
}
std::shared_ptr<const SharedAnalysis::CaptureResult> SharedAnalysis::CaptureLease::read() const noexcept {
    return available() ? domain_->result.load() : nullptr;
}
SharedAnalysis::Lease::SourcePosition SharedAnalysis::CaptureLease::sourcePosition() const noexcept {
    const auto state = state_.lock();
    if (!state || !available()) return {};
    state->ownerOnly();
    // Physical source binding changes exclusively on the owner. Do not wait for
    // worker-side capture or immutable publication to take the freshness fence.
    return {domain_->tap->generation(), domain_->tap->written()};
}
SharedAnalysis::BarScopeLease::BarScopeLease(std::weak_ptr<State> state, std::shared_ptr<BarScopeDomain> domain)
    : state_(std::move(state)), domain_(std::move(domain)) {}
SharedAnalysis::BarScopeLease::~BarScopeLease() { reset(); }
SharedAnalysis::BarScopeLease::BarScopeLease(BarScopeLease &&) noexcept = default;
SharedAnalysis::BarScopeLease &SharedAnalysis::BarScopeLease::operator=(BarScopeLease &&other) noexcept {
    if (this != &other) { reset(); state_ = std::move(other.state_); domain_ = std::move(other.domain_); }
    return *this;
}
SharedAnalysis::BarScopeLease::operator bool() const noexcept { return domain_ && domain_->active.load(); }
bool SharedAnalysis::BarScopeLease::request(const FocusScopeRange &range, std::uint64_t expected,
                                          std::uint64_t minimum) noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return false;
    state->ownerOnly();
    return domain_->request(range, expected, minimum);
}
void SharedAnalysis::BarScopeLease::setMonoRequested(FocusScopeChannels fold) noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return;
    state->ownerOnly();
    domain_->setMonoRequested(fold);
}
void SharedAnalysis::BarScopeLease::setPhaseRequested(bool value) noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return;
    state->ownerOnly();
    domain_->phaseRequested.store(value, std::memory_order_relaxed);
}
void SharedAnalysis::BarScopeLease::setSumTruePeakRequested(bool value) noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return;
    state->ownerOnly();
    domain_->sumTruePeakRequested.store(value, std::memory_order_relaxed);
}
void SharedAnalysis::BarScopeLease::setDifferenceRequested(std::optional<ScopeDifference> requested) noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return;
    state->ownerOnly();
    domain_->setDifferenceRequested(requested);
}
void SharedAnalysis::BarScopeLease::setLayerAdjustments(std::span<const FocusScopeLayerAdjustment> adjustments) noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return;
    state->ownerOnly();
    domain_->setLayerAdjustments(adjustments);
}
void SharedAnalysis::BarScopeLease::setLayerChannelViews(std::span<const std::optional<FocusScopeChannels>> views) noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return;
    state->ownerOnly();
    domain_->setLayerChannelViews(views);
}
double SharedAnalysis::BarScopeLease::sampleRate() const noexcept { return *this ? domain_->rate.load() : 0.; }
SharedAnalysis::BarScopeStatus SharedAnalysis::BarScopeLease::status() const noexcept {
    return *this ? domain_->status.load() : BarScopeStatus::Idle;
}
std::shared_ptr<const SharedAnalysis::BarScopeResult> SharedAnalysis::BarScopeLease::read() const noexcept {
    return status() == BarScopeStatus::Ready ? domain_->result.load() : nullptr;
}
void SharedAnalysis::BarScopeLease::reset() noexcept {
    if (domain_) if (const auto state = state_.lock()) state->dropBarScope(domain_);
    domain_.reset();
    state_.reset();
}

SharedAnalysis::FocusLagLease::FocusLagLease(std::weak_ptr<State> state, std::shared_ptr<FocusLagJob> job)
    : state_(std::move(state)), job_(std::move(job)) {}
SharedAnalysis::FocusLagLease::~FocusLagLease() { reset(); }
SharedAnalysis::FocusLagLease::FocusLagLease(FocusLagLease &&) noexcept = default;
SharedAnalysis::FocusLagLease &SharedAnalysis::FocusLagLease::operator=(FocusLagLease &&other) noexcept {
    if (this != &other) { reset(); state_ = std::move(other.state_); job_ = std::move(other.job_); }
    return *this;
}
SharedAnalysis::FocusLagLease::operator bool() const noexcept { return job_ && !state_.expired(); }
SharedAnalysis::FocusLagStatus SharedAnalysis::FocusLagLease::status() const noexcept {
    const auto state = state_.lock();
    if (!state || !job_) return FocusLagStatus::Idle;
    state->ownerOnly();
    std::lock_guard lock(state->mutex);
    return state->lagBindingsCurrent(*job_) ? job_->status : FocusLagStatus::Refused;
}
std::shared_ptr<const FocusLagPreview> SharedAnalysis::FocusLagLease::read() const noexcept {
    const auto state = state_.lock();
    if (!state || !job_) return {};
    state->ownerOnly();
    std::lock_guard lock(state->mutex);
    if (!job_->preview || !state->lagBindingsCurrent(*job_)) return {};
    // The alias keeps only the completed tiny job alive, never a physical tap.
    return {job_, &*job_->preview};
}
void SharedAnalysis::FocusLagLease::reset() noexcept {
    if (job_) if (const auto state = state_.lock()) {
        state->ownerOnly();
        std::lock_guard lock(state->mutex);
        if (state->focusLag == job_) state->focusLag.reset();
    }
    job_.reset();
    state_.reset();
}

SharedAnalysis::FocusLagLease SharedAnalysis::requestFocusLag(const FocusLagRequest &request) {
    state_->ownerOnly();
    if (!focus_lag_detail::requestedEnd(request)) return {};
    std::shared_ptr<FocusLagJob> job;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed || state_->focusLag) return {};
        const auto *main = state_->lagTap(request.sources[0]);
        const auto *target = state_->lagTap(request.sources[1]);
        if (!main || !target || main->value == target->value || main->value->sampleRate() != target->value->sampleRate() ||
            !focus_lag_detail::validRate(request, main->value->sampleRate())) return {};
        try { job = std::make_shared<FocusLagJob>(FocusLagJob{request, {main->binding, target->binding}}); }
        catch (const std::bad_alloc &) { return {}; }
        state_->focusLag = job;
    }
    if (mode_ == Mode::Worker) {
        try { Scheduler::instance().request(state_); }
        catch (...) {
            state_->queued.store(false, std::memory_order_release);
            std::lock_guard lock(state_->mutex);
            if (state_->focusLag == job) {
                job->status = FocusLagStatus::Refused;
                state_->focusLag.reset();
            }
        }
    }
    return FocusLagLease(state_, std::move(job));
}

SharedAnalysis::SharedAnalysis(AcquireSourceTap a, ReleaseSourceTap r, Mode mode, std::size_t historyByteLimit)
    : state_(std::make_shared<State>(std::move(a), std::move(r), historyByteLimit)), mode_(mode) {}
SharedAnalysis::SharedAnalysis(AcquireTap a, ReleaseTap r, Mode mode, std::size_t historyByteLimit)
    : SharedAnalysis([a = std::move(a)](engine::ScopeSource source, double) {
          return a && !source.device ? a(source.track, source.point) : nullptr;
      }, [r = std::move(r)](engine::ScopeSource source) { if (r) r(source.track, source.point); }, mode, historyByteLimit) {}
SharedAnalysis::~SharedAnalysis() { shutdown(); }
bool SharedAnalysis::active() const noexcept {
    state_->ownerOnly();
    return !state_->closed;
}
void SharedAnalysis::shutdown() noexcept {
    state_->ownerOnly();
    std::lock_guard run(state_->runMutex);
    std::map<engine::ScopeSource, State::Tap> taps;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) return;
        state_->closed = true;
        state_->focusLag.reset();
        for (auto &[key, domain] : state_->domains) {
            (void)key; domain->active.store(false, std::memory_order_release);
            domain->releaseUnused(0); domain->publishResult({});
        }
        for (const auto &capture : state_->captures) {
            capture->active.store(false);
            capture->result.store({});
        }
        for (const auto &bar : state_->bars) {
            bar->active.store(false);
            bar->projection.reset();
            bar->result.store({});
        }
        state_->captures.clear();
        state_->bars.clear();
        state_->barMembers = 0;
        state_->domains.clear(); taps.swap(state_->taps); state_->consumers = 0;
    }
    for (const auto &[key, tap] : taps) if (tap.attached && state_->release) { try { state_->release(key); } catch (...) {} }
}
SharedAnalysis::Lease SharedAnalysis::acquire(const Request &request) {
    return acquire(request, 1);
}
SharedAnalysis::Lease SharedAnalysis::acquire(const Request &request, std::uint32_t averageBlocks) {
    state_->ownerOnly();
    if (!valid(request) || ((request.outputs & DeviceBlockAverage) != 0) != (averageBlocks > 1)) return {};
    const auto key = keyFor(request);
    std::unique_lock lock(state_->mutex);
    if (state_->closed) return {};
    const auto found = state_->domains.find(key);
    if (found == state_->domains.end() && state_->jobCount() >= kMaxDomains) return {};
    auto cumulative = (request.outputs & CumulativeMaximum) ?
        std::make_shared<CumulativeHistory>(key.fftSize) : nullptr;
    auto filled = (request.outputs & FilledSpectrum) ? std::make_shared<FilledHistory>(key.fftSize) : nullptr;
    auto cumulativeAverage = (request.outputs & CumulativeAverage) ?
        std::make_shared<CumulativeAverageHistory>(key.fftSize) : nullptr;
    if (found != state_->domains.end()) {
        std::lock_guard domainLock(found->second->mutex);
        if (cumulative) found->second->cumulativeHistories.reserve(found->second->cumulativeHistories.size() + 1);
        if (cumulativeAverage) found->second->cumulativeAverages.reserve(found->second->cumulativeAverages.size() + 1);
        if (filled) found->second->filledHistories.reserve(found->second->filledHistories.size() + 1);
        if (!found->second->prepareOutputs(request.outputs)) return {};
        const auto previousOutputs = found->second->outputs.load(std::memory_order_acquire);
        const auto newlyRequested = request.outputs & ~previousOutputs;
        if ((newlyRequested || cumulativeAverage) && found->second->keepsParkedHop(previousOutputs))
            found->second->catchUpParkedSilence();
        const auto sequence = found->second->tap ? found->second->tap->written() : 0;
        auto blockAverage = found->second->admitBlockAverage(averageBlocks, sequence);
        found->second->addCumulative(cumulative, sequence);
        if (filled) {
            filled->afterSequence = sequence;
            found->second->filledHistories.push_back(filled);
        }
        found->second->addCumulativeAverage(cumulativeAverage, sequence);
        found->second->changeOutputs(request.outputs, true);
        if (newlyRequested & Average1s) found->second->activateAverage(sequence);
        if (newlyRequested & RealtimeMaximum) found->second->activateMaximum(sequence);
        if (newlyRequested & PeriodScope) found->second->activatePeriod(sequence);
        if (newlyRequested & RealtimeSigma) found->second->activateSigma(sequence);
        if (newlyRequested & Strip) found->second->startStripAt(sequence);
        found->second->updateAvailability();
        ++found->second->consumers; ++state_->consumers;
        return Lease(state_, found->second, request.outputs, std::move(cumulative), std::move(blockAverage),
                     std::move(cumulativeAverage), std::move(filled));
    }
    auto domain = std::make_shared<Domain>(key);
    if (!domain->prepareOutputs(request.outputs)) return {};
    auto blockAverage = domain->admitBlockAverage(averageBlocks, 0);
    domain->addCumulative(cumulative, 0);
    if (filled) domain->filledHistories.push_back(filled);
    domain->addCumulativeAverage(cumulativeAverage, 0);
    auto [tap, inserted] = state_->taps.try_emplace(key.source());
    auto &t = tap->second;
    const auto previousTap = t.value;
    const auto maximumSize = state_->maximumFft(key.source(), key.fftSize);
    try { state_->resolveTap(key.source(), t, maximumSize, false); }
    catch (...) {
        if (inserted) state_->abandonFirstAttach(key.source(), lock);
        throw;
    }
    ++t.domains; domain->bind(t.value, true);
    if (t.value) domain->startAt(inserted ? t.freshSequence :
        (key.stereoMeter || (key.outputs & ColourWaveform)) ? t.value->written() :
        std::max({t.value->oldest(), t.value->written() - key.fftSize, t.freshSequence}));
    domain->consumers = 1;
    domain->changeOutputs(request.outputs, true);
    const auto admission = inserted || t.value != previousTap ? t.freshSequence : (t.value ? t.value->written() : 0);
    if (request.outputs & Average1s) domain->activateAverage(admission);
    if (request.outputs & RealtimeMaximum) domain->activateMaximum(admission);
    if (request.outputs & PeriodScope) domain->activatePeriod(admission);
    if (request.outputs & RealtimeSigma) domain->activateSigma(admission);
    if (request.outputs & Strip) domain->startStripAt(admission);
    if (cumulative) cumulative->afterSequence = admission;
    if (filled) filled->afterSequence = admission;
    if (blockAverage) blockAverage->afterSequence = admission;
    if (cumulativeAverage) cumulativeAverage->afterSequence = admission;
    state_->domains.emplace(key, domain); ++state_->consumers;
    return Lease(state_, std::move(domain), request.outputs, std::move(cumulative), std::move(blockAverage),
                 std::move(cumulativeAverage), std::move(filled));
}

SharedAnalysis::Lease SharedAnalysis::acquireDeviceSpectrum(const DeviceSpectrumRequest &device) {
    if (device.fftSize < 32 || device.fftSize > kMaxFftSize ||
        !std::has_single_bit(static_cast<std::uint32_t>(device.fftSize)) ||
        device.averageBlocks < 1 || device.averageBlocks > DeviceSpectrumRequest::kMaximumAverageBlocks)
        return {};
    Request request{device.track, engine::ScopePoint::DeviceOutput, device.fftSize};
    request.device = device.device;
    switch (device.channel) {
    case DeviceSpectrumRequest::Channel::Left: request.channel = Channel::Left; break;
    case DeviceSpectrumRequest::Channel::Right: request.channel = Channel::Right; break;
    case DeviceSpectrumRequest::Channel::Stereo: request.channel = Channel::StereoPower; break;
    default: return {};
    }
    if (device.maximum) request.outputs |= CumulativeMaximum;
    if (device.averageBlocks > 1) request.outputs |= DeviceBlockAverage;
    return acquire(request, device.averageBlocks);
}
SharedAnalysis::Lease SharedAnalysis::acquireStrip(std::int64_t track) {
    Request request{track, engine::ScopePoint::PostFader, stripFftSize};
    request.outputs = Strip;
    return acquire(request);
}
SharedAnalysis::Lease SharedAnalysis::acquireStereoMeter(std::int64_t track, StereoMeterProfile profile) {
    Request request{track, engine::ScopePoint::PostFader, 32};
    request.outputs = Meters;
    request.stereoMeter = profile;
    return acquire(request);
}
SharedAnalysis::CaptureLease SharedAnalysis::acquireCapture(const CaptureRequest &request) {
    state_->ownerOnly();
    CaptureRequest key = request;
    key.settings.sampleRate = 48000.; // identity is independent of tap discovery
    const auto point = key.source.point;
    const bool device = point == engine::ScopePoint::DeviceInput || point == engine::ScopePoint::DeviceOutput;
    if (key.source.track <= 0 || point < engine::ScopePoint::PostFader || point > engine::ScopePoint::DeviceOutput ||
        (device ? key.source.device <= 0 : key.source.device != 0) ||
        (key.channel != Channel::Left && key.channel != Channel::Right) ||
        !dsp::TriggeredScope::validSettings(key.settings)) return {};
    std::unique_lock lock(state_->mutex);
    if (state_->closed) return {};
    for (const auto &domain : state_->captures) if (domain->request == key) {
        ++domain->consumers;
        ++state_->consumers;
        return CaptureLease(state_, domain);
    }
    if (state_->jobCount() >= kMaxDomains) return {};
    auto domain = std::make_shared<CaptureDomain>(key);
    // Reserve before the source callback. Once acquired, installing the
    // reader is non-throwing and existing readers retain their own cursors.
    if (state_->captures.size() == state_->captures.capacity())
        state_->captures.reserve(std::min(kMaxDomains, 2 * state_->captures.size() + 1));
    auto [at, inserted] = state_->taps.try_emplace(key.source);
    auto &tap = at->second;
    const auto previousTap = tap.value;
    try { state_->resolveTap(key.source, tap, state_->maximumFft(key.source), false); }
    catch (...) {
        if (inserted) state_->abandonFirstAttach(key.source, lock);
        throw;
    }
    const auto first = inserted || previousTap != tap.value ? tap.freshSequence : (tap.value ? tap.value->written() : 0);
    domain->bind(tap.value, first);
    domain->consumers = 1;
    state_->captures.push_back(domain);
    ++tap.domains;
    ++state_->consumers;
    return CaptureLease(state_, std::move(domain));
}
SharedAnalysis::BarScopeLease SharedAnalysis::acquireBarScope(std::span<const engine::ScopeSource> sources,
                                                            std::size_t columns) {
    state_->ownerOnly();
    if (sources.empty() || sources.size() > kMaxDomains || !columns ||
        columns > dsp::TriggeredScope::kMaximumColumns) return {};
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto &source = sources[i];
        const bool device = source.point == engine::ScopePoint::DeviceInput || source.point == engine::ScopePoint::DeviceOutput;
        if (source.track <= 0 || source.point < engine::ScopePoint::PostFader || source.point > engine::ScopePoint::DeviceOutput ||
            (device ? source.device <= 0 : source.device != 0) ||
            std::ranges::find(sources.first(i), source) != sources.first(i).end()) return {};
    }
    std::unique_lock lock(state_->mutex);
    if (state_->closed || state_->jobCount() >= kMaxDomains || sources.size() > kMaxDomains - state_->barMembers)
        return {};
    auto bar = std::make_shared<BarScopeDomain>(sources, columns);
    if (state_->bars.size() == state_->bars.capacity())
        state_->bars.reserve(std::min(kMaxDomains, 2 * state_->bars.size() + 1));
    std::array<engine::ScopeSource, kMaxDomains> fresh;
    std::size_t created = 0;
    try {
        for (const auto &source : sources) {
            auto [at, inserted] = state_->taps.try_emplace(source);
            if (inserted) fresh[created++] = source;
            state_->resolveTap(source, at->second, state_->maximumFft(source), false);
        }
    } catch (...) {
        std::size_t detached = 0;
        for (std::size_t i = 0; i < created; ++i) {
            const auto at = state_->taps.find(fresh[i]);
            if (at->second.attached) fresh[detached++] = fresh[i];
            state_->taps.erase(at);
        }
        lock.unlock();
        if (state_->release) for (std::size_t i = 0; i < detached; ++i) {
            try { state_->release(fresh[i]); } catch (...) {}
        }
        throw;
    }
    for (const auto &source : sources) ++state_->taps.at(source).domains;
    state_->bindBarSources(*bar);
    state_->bars.push_back(bar);
    ++state_->consumers;
    state_->barMembers += sources.size();
    return BarScopeLease(state_, std::move(bar));
}
void SharedAnalysis::refreshSources(bool forceEpoch) {
    state_->ownerOnly();
    std::lock_guard lock(state_->mutex);
    if (state_->closed) return;
    for (auto &[key, tap] : state_->taps) {
        const auto previous = tap.value;
        state_->resolveTap(key, tap, state_->maximumFft(key), true);
        if (!forceEpoch || previous != tap.value) continue;
        tap.freshSequence = tap.value ? tap.value->written() : 0;
        tap.binding = ++state_->nextTapBinding;
        state_->bindSource(key, tap);
    }
    state_->refreshBarSources();
}
void SharedAnalysis::requestFrame() noexcept {
    state_->ownerOnly();
    if (mode_ == Mode::Manual) return;
    { std::lock_guard lock(state_->mutex); if (state_->closed || !state_->consumers) return; }
    state_->frameRequests.fetch_add(1, std::memory_order_relaxed);
    try {
        // A negotiated physical tap can change generation through another logical endpoint
        // or lease without a project edit. Resolve current leases on the owner, never on
        // the analysis worker/audio thread, before scheduling this visible frame's work.
        refreshSources();
        {
            std::lock_guard lock(state_->mutex);
            if (state_->closed || (!state_->workPending() && !state_->focusLag)) return;
        }
        Scheduler::instance().request(state_);
    } catch (...) { state_->queued.store(false, std::memory_order_release); }
}
std::size_t SharedAnalysis::pump() { return state_->pump(); }
SharedAnalysis::Stats SharedAnalysis::stats() const {
    std::lock_guard lock(state_->mutex);
    Stats result{state_->consumers, state_->jobCount(), state_->taps.size(),
        state_->transforms.load(std::memory_order_relaxed), state_->publications.load(std::memory_order_relaxed),
        state_->frameRequests.load(std::memory_order_relaxed), state_->workerPasses.load(std::memory_order_relaxed),
        state_->cursorReads.load(std::memory_order_relaxed), state_->analysisWindows.load(std::memory_order_relaxed)};
    result.successfulEnqueues = state_->successfulEnqueues.load(std::memory_order_relaxed);
    for (const auto &[key, domain] : state_->domains) {
        (void)key;
        result.spectralPlans += static_cast<std::size_t>(domain->spectrum != nullptr) +
                                static_cast<std::size_t>(domain->phaseBandPlan != nullptr);
        result.stereoBalanceHistories += static_cast<std::size_t>(domain->balanceActive.load(std::memory_order_acquire));
        result.spectrumLevelHistories += static_cast<std::size_t>(domain->spectrumLevelsActive.load(std::memory_order_acquire));
        result.spectrumTruePeakHistories += static_cast<std::size_t>(domain->spectrumTruePeakActive.load(std::memory_order_acquire));
        result.droppedHistoryColumns += domain->droppedHistory.load(std::memory_order_relaxed);
    }
    result.historyBytes = state_->historyBudget->bytes.load(std::memory_order_acquire);
    result.historyPeakBytes = state_->historyBudget->peak.load(std::memory_order_relaxed);
    return result;
}
} // namespace adi::ui
