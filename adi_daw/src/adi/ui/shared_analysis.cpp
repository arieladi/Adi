// SPDX-License-Identifier: AGPL-3.0-or-later
#include "shared_analysis.hpp"
#include "adi/dsp/spectrum_average.hpp"
#include "adi/ui/focus_scope.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

namespace adi::ui {
namespace {
constexpr std::uint32_t allOutputs = SharedAnalysis::Spectrum | SharedAnalysis::Scope | SharedAnalysis::Meters |
                                     SharedAnalysis::Phase | SharedAnalysis::Spectrogram | SharedAnalysis::Average1s |
                                     SharedAnalysis::RealtimeMaximum | SharedAnalysis::CumulativeMaximum;
constexpr std::size_t maxHistoryColumns = 128;
constexpr std::size_t pendingHistoryBytesPerDomain = 256 * 1024;
// PFFFT squares its unnormalised float bins. Leave headroom for that sum
// at the largest supported FFT as well as for the stereo and M/S projection.
constexpr float spectralSampleLimit = 1.e12f;
bool valid(const SharedAnalysis::Request &r) {
    if (r.track <= 0 || r.fftSize < 32 || r.fftSize > SharedAnalysis::kMaxFftSize || r.fftSize % 32 ||
        r.hopSize < 0 || r.hopSize > r.fftSize || !r.outputs || (r.outputs & ~allOutputs)) return false;
    using Weighting = dsp::StereoEnvelopeMeter::Weighting;
    if (r.stereoMeter && (r.outputs != SharedAnalysis::Meters || r.stereoMeter->weighting < Weighting::Standard ||
                          r.stereoMeter->weighting > Weighting::K)) return false;
    auto n = r.fftSize;
    for (const auto factor : {2, 3, 5}) while (n % factor == 0) n /= factor;
    const bool device = r.point == engine::ScopePoint::DeviceInput || r.point == engine::ScopePoint::DeviceOutput;
    return n == 1 && (r.window == dsp::Window::Hann || r.window == dsp::Window::Rectangular) &&
           r.channel >= SharedAnalysis::Channel::Mid && r.channel <= SharedAnalysis::Channel::StereoPower &&
           r.point >= engine::ScopePoint::PostFader && r.point <= engine::ScopePoint::DeviceOutput &&
           (device ? r.device > 0 : r.device == 0);
}
SharedAnalysis::Request keyFor(SharedAnalysis::Request r) {
    r.hopSize = r.hopSize ? r.hopSize : r.fftSize / 4;
    r.outputs = 0;
    return r;
}
bool usable(const std::shared_ptr<const engine::ScopeTap> &tap, std::int32_t n) {
    return tap && std::isfinite(tap->sampleRate()) && tap->sampleRate() > 0 && tap->capacity() >= n;
}
bool spectralOutput(std::uint32_t outputs) noexcept {
    return (outputs & (SharedAnalysis::Spectrum | SharedAnalysis::Spectrogram |
                       SharedAnalysis::Average1s | SharedAnalysis::RealtimeMaximum |
                       SharedAnalysis::CumulativeMaximum)) != 0;
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
} // namespace
struct SharedAnalysis::CumulativeHistory {
    // Control and worker writes hold the domain lock. Readers own only immutable results.
    std::vector<float> bins;
    std::atomic<std::shared_ptr<const MaximumResult>> publication;
    std::int64_t afterSequence = 0;
    std::uint64_t lastWindow = 0;

    explicit CumulativeHistory(std::int32_t fftSize) : bins(static_cast<std::size_t>(fftSize / 2 + 1)) {}
    void clear() noexcept {
        std::fill(bins.begin(), bins.end(), 0.f);
        lastWindow = 0;
        publication.store({});
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
    std::vector<std::shared_ptr<CumulativeHistory>> cumulativeHistories;
    std::atomic<bool> cumulativeReady{true};
    std::unique_ptr<dsp::StereoEnvelopeMeter> meter;
    bool meterPrepared = false;
    std::atomic<bool> neutralMeter{false};
    bool averageConfigured = false;
    AverageStatus averageStatus = AverageStatus::Unavailable;
    std::uint64_t averageEpoch = 0, averageWindows = 0, averageWindow = 0, averageBinsWindow = 0;
    std::int64_t averageAfterSequence = 0;
    std::shared_ptr<const std::vector<float>> averageBins;
    std::vector<float> ringLeft, ringRight, left, right, scratch, linear, second;
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
    std::atomic<bool> dirty{true}, historyBlocked{false};
    engine::ScopeTap::Cursor cursor;
    std::mutex mutex;
    std::shared_ptr<const engine::ScopeTap> tap;
    mutable std::mutex resultMutex;
    std::shared_ptr<const Result> result;
    std::shared_ptr<const AverageResult> averageResult;
    std::shared_ptr<const MaximumResult> maximumResult;
    std::shared_ptr<const StereoMeterResult> meterResult;
    std::shared_ptr<const Result> readResult() const { std::lock_guard lock(resultMutex); return result; }
    std::shared_ptr<const AverageResult> readAverageResult() const { std::lock_guard lock(resultMutex); return averageResult; }
    std::shared_ptr<const MaximumResult> readMaximumResult() const { std::lock_guard lock(resultMutex); return maximumResult; }
    std::shared_ptr<const StereoMeterResult> readMeterResult() const { std::lock_guard lock(resultMutex); return meterResult; }
    void publishMeter(std::shared_ptr<const StereoMeterResult> next) {
        std::lock_guard lock(resultMutex);
        meterResult.swap(next);
    }
    void publishResult(std::shared_ptr<const Result> next, std::shared_ptr<const AverageResult> nextAverage = {},
                       std::shared_ptr<const MaximumResult> nextMaximum = {}) {
        {
            std::lock_guard lock(resultMutex);
            result.swap(next); averageResult.swap(nextAverage); maximumResult.swap(nextMaximum);
        }
    }
    void updateAvailability() noexcept {
        const auto bits = outputs.load(std::memory_order_acquire);
        const auto unavailable = (averageConfigured ? 0u : std::uint32_t{Average1s}) |
                                 (maximum ? 0u : std::uint32_t{RealtimeMaximum});
        const bool onlyUnavailable = bits != 0 && (bits & ~unavailable) == 0;
        available.store(usable(tap, request.fftSize) && !onlyUnavailable && (!meter || meterPrepared),
                        std::memory_order_release);
    }
    void resetAverageForGap() noexcept {
        if (!averageConfigured) return;
        average->reset(); ++averageEpoch; averageWindows = 0;
        averageWindow = windowSerial; averageBinsWindow = 0;
        averageBins.reset(); averageStatus = AverageStatus::Waiting;
    }
    // Owner-only under the domain lock: no preparation/retry on the worker.
    void activateAverage(std::int64_t sequence) noexcept {
        if (!(outputs.load(std::memory_order_acquire) & Average1s)) return;
        averageConfigured = false; averageStatus = AverageStatus::Unavailable;
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
    void resetWindow(bool epoch) {
        position = filled = sinceWindow = 0;
        completedWindowEnd.store(-1, std::memory_order_release);
        hasWindow = false; spectralWindow = historyWindow = 0;
        pendingHistory.clear(); retiredForPending = 0;
        historyBlocked.store(false, std::memory_order_release);
        columns.clear();
        if (epoch) {
            ++sourceEpoch;
            resetAverageForGap();
            // A discontinuity clears the history but cannot admit older
            // samples from the same tap. A replacement tap uses startAt().
            resetMaximum();
            for (const auto &history : cumulativeHistories) history->clear();
            cumulativeReady.store(cumulativeHistories.empty(), std::memory_order_release);
        }
        publishResult({});
    }
    void bind(std::shared_ptr<const engine::ScopeTap> next, bool fresh) {
        tap = std::move(next);
        cursor = tap ? tap->cursorAtEnd() : engine::ScopeTap::Cursor{};
        if (tap && !fresh) cursor.next = std::max(tap->oldest(), tap->written() - request.fftSize);
        if (meter) {
            neutralMeter.store(false, std::memory_order_release);
            publishMeter({});
            const auto profile = *request.stereoMeter;
            meterPrepared = usable(tap, request.fftSize) &&
                meter->prepare({tap->sampleRate(), .6, .5, .5, 1.e-6,
                                profile.midSide, profile.weighting, profile.linkedDynamic});
        }
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
        activateAverage(sequence);
        activateMaximum(sequence);
        for (const auto &history : cumulativeHistories) {
            history->afterSequence = sequence;
            history->clear();
        }
        cumulativeReady.store(cumulativeHistories.empty(), std::memory_order_release);
    }
    bool ensureSpectrum(std::uint32_t bits) {
        if (!spectralOutput(bits)) return true;
        if (!spectrum) {
            auto plan = std::make_unique<dsp::Spectrum>(request.fftSize, request.window);
            if (!plan->valid()) return false;
            std::vector<float> time(left.size()), bins(static_cast<std::size_t>(request.fftSize / 2 + 1));
            std::vector<float> stereo(request.channel == Channel::StereoPower ? bins.size() : 0);
            spectrum = std::move(plan); scratch = std::move(time); linear = std::move(bins); second = std::move(stereo);
        }
        if ((bits & Spectrogram) && columns.capacity() < maxHistoryColumns) columns.reserve(maxHistoryColumns);
        return true;
    }
    void releaseUnused(std::uint32_t bits) {
        if (!(bits & Average1s)) {
            average.reset(); averageBins.reset(); averageConfigured = false;
            averageStatus = AverageStatus::Unavailable;
            std::shared_ptr<const AverageResult> retired;
            { std::lock_guard lock(resultMutex); averageResult.swap(retired); }
        }
        if (!(bits & RealtimeMaximum) && maximum) {
            maximum.reset();
            neutralMaximum.store(true, std::memory_order_release);
            std::shared_ptr<const MaximumResult> retired;
            { std::lock_guard lock(resultMutex); maximumResult.swap(retired); }
        }
        if (!(bits & Output::Spectrogram)) {
            columns.clear(); pendingHistory.clear(); retiredForPending = 0;
            historyWindow = 0; historyBlocked.store(false, std::memory_order_release);
        }
        if (!(bits & CumulativeMaximum)) {
            std::vector<std::shared_ptr<CumulativeHistory>>().swap(cumulativeHistories);
            cumulativeReady.store(true, std::memory_order_release);
        }
        if (!spectralOutput(bits)) {
            spectrum.reset(); spectralWindow = 0;
            std::vector<float>().swap(scratch); std::vector<float>().swap(linear); std::vector<float>().swap(second);
        }
        updateAvailability();
    }
    bool parkedWindowSettled(engine::ScopeTap::ParkedSilence quiet, std::uint32_t bits) const noexcept {
        const auto windowEnd = completedWindowEnd.load(std::memory_order_acquire);
        return windowEnd <= quiet.end && windowEnd - quiet.begin >= request.fftSize &&
               !(bits & Average1s) && (!(bits & RealtimeMaximum) || neutralMaximum.load(std::memory_order_acquire));
    }
    bool settledParkedSilence(engine::ScopeTap::ParkedSilence quiet) const noexcept {
        const auto bits = outputs.load(std::memory_order_acquire);
        return parkedWindowSettled(quiet, bits) &&
               (!(bits & CumulativeMaximum) || cumulativeReady.load(std::memory_order_acquire));
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
        cursor.next = std::max(cursor.next, next);
    }
    std::size_t historyColumnBytes() const noexcept {
        return (static_cast<std::size_t>(request.fftSize) / 2 + 1) * sizeof(float);
    }
    bool needsWork(const HistoryBudget &budget) const noexcept {
        if (!active.load(std::memory_order_acquire) || !available.load(std::memory_order_acquire) || !tap) return false;
        if (dirty.load(std::memory_order_acquire) ||
            (historyBlocked.load(std::memory_order_acquire) && budget.canClaim(historyColumnBytes()))) return true;
        const auto end = tap->written();
        const auto consumed = consumedSequence.load(std::memory_order_acquire);
        if (end == consumed) return false;
        const auto quiet = tap->lastParkedSilence();
        if (meter) return !(neutralMeter.load(std::memory_order_acquire) && quiet.begin < consumed && quiet.end >= end);
        return !(quiet.end >= end && settledParkedSilence(quiet));
    }
    std::atomic<bool> active{true}, available{false};
    std::atomic<std::uint32_t> outputs{0};
    std::array<std::size_t, 8> outputUsers{}; // State mutex
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
        if (previous != unionBits) dirty.store(true, std::memory_order_release);
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
        const bool cumulativeHop = (bits & CumulativeMaximum) && newWindow &&
            std::ranges::any_of(cumulativeHistories, [this](const auto &history) {
                return firstSequence >= history->afterSequence;
            });
        const bool needsSpectrum = rawSpectralOutput(bits) || averageHopEligible || maximumHopEligible || cumulativeHop;
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
        if (!newWindow) if (const auto previous = readResult()) latest->nonFiniteSamples = previous->nonFiniteSamples;
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
            if (averageHopEligible) {
                const bool clipped = (request.channel != Channel::Right && clippedLeft) ||
                                     (request.channel != Channel::Left && clippedRight);
                averageWindow = windowSerial;
                if (!clipped && average->processChecked(linear)) { ++averageWindows; averageStatus = AverageStatus::Ready; }
                else {
                    // Refuse broken spectral input; an explicit average-only
                    // gap clears once, and subsequent valid hops recover from zero.
                    if (averageStatus != AverageStatus::InvalidInput) {
                        average->reset(); averageWindows = 0; ++averageEpoch; averageBins.reset(); averageBinsWindow = 0;
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
            if (cumulativeHop) {
                bool ready = true;
                for (const auto &history : cumulativeHistories) {
                    if (firstSequence < history->afterSequence) {
                        ready = false;
                        continue;
                    }
                    if (nonzero) {
                        for (std::size_t i = 0; i < linear.size(); ++i)
                            history->bins[i] = std::max(history->bins[i], linear[i]);
                    }
                    history->lastWindow = windowSerial;
                }
                // Once every observer has an eligible window, further certified
                // parked silence cannot change any held bin.
                cumulativeReady.store(ready, std::memory_order_release);
            }
            if (rawSpectralOutput(bits)) {
                latest->binsDbfs.resize(linear.size());
                std::transform(linear.begin(), linear.end(), latest->binsDbfs.begin(), dsp::toDbfs);
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
    void publishCumulative(const std::shared_ptr<const Result> &window) noexcept {
        if (!(window->outputs & CumulativeMaximum)) return;
        for (const auto &history : cumulativeHistories) {
            if (!history->lastWindow) continue;
            const auto previous = history->publication.load();
            if (previous && previous->window->windowSerial == history->lastWindow &&
                previous->window->outputs == window->outputs) continue;
            try {
                auto publication = std::make_shared<MaximumResult>();
                publication->window = window;
                publication->binsDbfs.resize(history->bins.size());
                std::transform(history->bins.begin(), history->bins.end(), publication->binsDbfs.begin(), dsp::toDbfs);
                history->publication.store(std::move(publication));
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
    std::atomic<std::shared_ptr<const CaptureResult>> result;
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
struct SharedAnalysis::BarScopeDomain {
    static constexpr std::size_t chunkFrames = 256;
    struct Lane {
        engine::ScopeSource source;
        std::shared_ptr<const engine::ScopeTap> tap;
        std::uint64_t binding = 0;
        engine::ScopeTap::Cursor cursor;
        engine::ScopeTap::ReadResult chunk;
        std::int64_t chunkEnd = 0;
        std::array<float, chunkFrames> left, right;
    };

    BarScopeDomain(std::span<const engine::ScopeSource> sources, std::size_t width)
        : lanes(sources.size()), blocks(sources.size()), identities(sources.size()), columns(width) {
        for (std::size_t i = 0; i < lanes.size(); ++i) lanes[i].source = sources[i];
    }

    void invalidate() noexcept {
        projection.reset();
        result.store({});
        if (status.load() != BarScopeStatus::Idle) status.store(BarScopeStatus::Missed);
        for (auto &lane : lanes) {
            lane.cursor = lane.tap ? lane.tap->cursorAtEnd() : engine::ScopeTap::Cursor{};
            lane.chunk = {};
        }
        waiting.store(0);
    }

    bool request(const FocusScopeRange &next, std::uint64_t expected) noexcept {
        if (next.first < 0 || !std::in_range<std::int64_t>(next.frames)) return false;
        const auto finish = focusWindowEnd(next.first, static_cast<std::int64_t>(next.frames));
        if (!finish) return false;
        std::lock_guard lock(mutex);
        if (!active.load() || !(next.sampleRate > 0.) || rate.load() != next.sampleRate) return false;
        if (status.load() == BarScopeStatus::Pending && range.first == next.first && range.frames == next.frames &&
            expectedRevision == expected) return true;
        range = next;
        end = *finish;
        expectedRevision = revision = expected;
        projection.reset();
        result.store({});
        waiting.store(0);
        status.store(BarScopeStatus::Pending);
        return true;
    }

    void setMidRequested(bool enabled) noexcept {
        includeMid.store(enabled, std::memory_order_relaxed);
    }

    // On the owner this runs under State::mutex, which also protects tap rebinding.
    bool needsWork() const noexcept {
        if (!active.load() || status.load() != BarScopeStatus::Pending || !rate.load()) return false;
        const auto stalled = waiting.load();
        return !stalled || lanes[stalled - 1].tap->written() != waitThrough.load();
    }

    bool prepareLane(std::size_t index, std::int64_t first, std::size_t &reads, std::size_t readLimit,
                     std::size_t &samples) noexcept {
        auto &lane = lanes[index];
        using Status = engine::ScopeTap::ReadStatus;
        while ((!lane.chunk.frames || lane.chunkEnd <= first) && active.load()) {
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
            if (read.status != Status::Ready || !read.advancing || !read.transportRevision || !finish) {
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

    bool process(std::size_t &reads, std::size_t &samples) noexcept {
        std::lock_guard lock(mutex);
        if (!needsWork()) return false;
        waiting.store(0);
        const auto readLimit = std::min(kMaxReadsPerFrame, reads + kMaxReadsPerFrame / 2);
        try {
            while (active.load()) {
                const auto first = range.first + static_cast<std::int64_t>(projection ? projection->frames() : 0);
                auto finish = end;
                for (std::size_t i = 0; i < lanes.size(); ++i) {
                    if (!prepareLane(i, first, reads, readLimit, samples)) return false;
                    finish = std::min(finish, lanes[i].chunkEnd);
                }
                const bool wantsMid = includeMid.load(std::memory_order_relaxed);
                if (!projection) {
                    for (std::size_t i = 0; i < lanes.size(); ++i)
                        identities[i] = {lanes[i].source, lanes[i].tap->generation()};
                    projection = std::make_shared<FocusScopeBarProjection>();
                    if (!projection->prepare(range, identities, columns, revision, wantsMid)) {
                        invalidate();
                        return false;
                    }
                } else if (!wantsMid) {
                    projection->discardMid();
                }
                const auto count = static_cast<std::size_t>(finish - first);
                for (std::size_t i = 0; i < lanes.size(); ++i) {
                    const auto &lane = lanes[i];
                    const auto start = static_cast<std::size_t>(first - lane.chunk.firstStamp);
                    blocks[i] = {identities[i], first, revision, range.sampleRate, true,
                        std::span(lane.left).subspan(start, count), std::span(lane.right).subspan(start, count)};
                }
                if (!projection->append(blocks)) { invalidate(); return false; }
                if (!projection->complete()) continue;
                if (!active.load()) return false;
                result.store(std::make_shared<BarScopeResult>(BarScopeResult{range.first, range.frames,
                    range.sampleRate, revision, std::move(projection)}));
                status.store(BarScopeStatus::Ready);
                return true;
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
    std::atomic<std::shared_ptr<const BarScopeResult>> result;
    std::atomic<std::size_t> waiting{0};
    std::atomic<std::int64_t> waitThrough{0};
    FocusScopeRange range;
    std::uint64_t expectedRevision = 0, revision = 0;
    std::int64_t end = 0;
    std::atomic<bool> includeMid{false};
    std::shared_ptr<FocusScopeBarProjection> projection;
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
            ++tap.binding;
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
              const std::shared_ptr<CumulativeHistory> &cumulative) noexcept {
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
                domain->dropCumulative(cumulative);
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
        return std::any_of(domains.begin(), domains.end(), [this](const auto &entry) { return entry.second->needsWork(*historyBudget); }) ||
               std::any_of(captures.begin(), captures.end(), [](const auto &capture) { return capture->needsWork(); }) ||
               std::ranges::any_of(bars, [](const auto &bar) { return bar->needsWork(); });
    }
    std::size_t pump() {
        // Never audio-reachable. Shutdown waits before releasing owner callbacks.
        std::lock_guard run(runMutex);
        std::unique_lock stateLock(mutex);
        if (closed || !workPending()) return 0;
        using Job = std::variant<std::shared_ptr<Domain>, std::shared_ptr<CaptureDomain>, std::shared_ptr<BarScopeDomain>>;
        std::array<Job, kMaxDomains> jobs;
        std::size_t count = 0, done = 0, windows = 0, samples = 0, reads = 0;
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
                if ((*bar)->process(reads, samples)) publications.fetch_add(1, std::memory_order_relaxed);
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
            std::shared_ptr<Result> latest;
            // Make space released since the preceding publication available to
            // its staged columns before this pass can offer new ones.
            const bool historyFlushed = d.flushPendingHistory(historyBudget);
            bool discontinuity = false;
            const auto n = d.left.size(), hop = static_cast<std::size_t>(d.request.hopSize);
            const auto end = d.tap->written();
            const auto quiet = d.tap->lastParkedSilence();
            d.skipParkedBeforeCumulative(quiet);
            // Resume at the end of a previously parked range. Its omitted samples
            // are identical to those in the retained window. The regular cursor
            // still detects a seek, playback start, or another stamp at the join.
            if (quiet.end < end && quiet.end > d.cursor.next && d.settledParkedSilence(quiet))
                d.cursor.next = quiet.end;
            while (d.cursor.next < end && windows < kMaxWindowsPerFrame && samples < kMaxSamplesPerFrame && reads < kMaxReadsPerFrame) {
                if (quiet.end >= end && d.settledParkedSilence(quiet)) break;
                const auto need = d.filled < n ? n - d.filled : hop - d.sinceWindow;
                const auto length = std::min({need, kMaxSamplesPerFrame - samples, readLeft.size(),
                                               static_cast<std::size_t>(end - d.cursor.next)});
                ++reads;
                auto read = d.tap->read(d.cursor, std::span<float>(readLeft.data(), length),
                                       std::span<float>(readRight.data(), length));
                if (read.status == engine::ScopeTap::ReadStatus::Overrun ||
                    read.status == engine::ScopeTap::ReadStatus::GenerationChanged) {
                    ++d.gaps;
                    if (read.status == engine::ScopeTap::ReadStatus::Overrun) ++d.overruns;
                    d.resetWindow(true); discontinuity = true; latest.reset(); continue;
                }
                if (read.status == engine::ScopeTap::ReadStatus::Discontinuity) {
                    ++d.gaps; d.resetWindow(true); discontinuity = true; latest.reset();
                }
                if (!read.frames) break;
                samples += static_cast<std::size_t>(read.frames);
                for (std::int32_t i = 0; i < read.frames; ++i) {
                    d.ringLeft[d.position] = readLeft[static_cast<std::size_t>(i)];
                    d.ringRight[d.position] = readRight[static_cast<std::size_t>(i)];
                    d.position = (d.position + 1) % n;
                }
                const bool wasFull = d.filled == n;
                d.filled = std::min(n, d.filled + static_cast<std::size_t>(read.frames));
                if (wasFull) d.sinceWindow += static_cast<std::size_t>(read.frames);
                if (d.filled != n || (wasFull && d.sinceWindow < hop)) continue;
                d.sinceWindow = 0; ++windows; ++d.windowSerial; d.hasWindow = true;
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
                    auto averagePublication = d.makeAverageResult(latest);
                    auto maximumPublication = d.makeMaximumResult(latest);
                    d.publishCumulative(latest);
                    d.publishResult(std::move(latest), std::move(averagePublication), std::move(maximumPublication));
                    publications.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        transforms.fetch_add(done, std::memory_order_relaxed);
        cursorReads.fetch_add(reads, std::memory_order_relaxed);
        analysisWindows.fetch_add(windows, std::memory_order_relaxed);
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
    std::map<engine::ScopeSource, Tap> taps;
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
                             std::shared_ptr<CumulativeHistory> cumulative)
    : state_(std::move(state)), domain_(std::move(domain)), cumulative_(std::move(cumulative)), outputs_(outputs) {}
SharedAnalysis::Lease::~Lease() { reset(); }
SharedAnalysis::Lease::Lease(Lease &&other) noexcept
    : state_(std::move(other.state_)), domain_(std::move(other.domain_)), cumulative_(std::move(other.cumulative_)),
      outputs_(std::exchange(other.outputs_, 0)) {}
SharedAnalysis::Lease &SharedAnalysis::Lease::operator=(Lease &&other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        domain_ = std::move(other.domain_);
        cumulative_ = std::move(other.cumulative_);
        outputs_ = std::exchange(other.outputs_, 0);
    }
    return *this;
}
void SharedAnalysis::Lease::reset() noexcept {
    if (domain_) if (auto state = state_.lock()) state->drop(domain_, outputs_, cumulative_);
    cumulative_.reset(); domain_.reset(); state_.reset(); outputs_ = 0;
}
SharedAnalysis::Lease::operator bool() const noexcept { return domain_ && !state_.expired() && domain_->active.load(std::memory_order_acquire); }
bool SharedAnalysis::Lease::available() const noexcept { return static_cast<bool>(*this) && domain_->available.load(std::memory_order_acquire); }
std::shared_ptr<const SharedAnalysis::Result> SharedAnalysis::Lease::read() const noexcept {
    auto result = available() ? domain_->readResult() : nullptr;
    return result && (result->outputs & outputs_) == outputs_ ? result : nullptr;
}
std::shared_ptr<const SharedAnalysis::AverageResult> SharedAnalysis::Lease::readAverage() const noexcept {
    const auto publication = (outputs_ & Average1s) && available() ? domain_->readAverageResult() : nullptr;
    return publication && publication->window && (publication->window->outputs & outputs_) == outputs_ ? publication : nullptr;
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
std::shared_ptr<const SharedAnalysis::StereoMeterResult> SharedAnalysis::Lease::readStereoMeter() const noexcept {
    return available() && domain_->meter ? domain_->readMeterResult() : nullptr;
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
bool SharedAnalysis::BarScopeLease::request(const FocusScopeRange &range, std::uint64_t expected) noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return false;
    state->ownerOnly();
    return domain_->request(range, expected);
}
void SharedAnalysis::BarScopeLease::setMidRequested(bool enabled) noexcept {
    const auto state = state_.lock();
    if (!state || !domain_) return;
    state->ownerOnly();
    domain_->setMidRequested(enabled);
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
    state_->ownerOnly();
    if (!valid(request)) return {};
    const auto key = keyFor(request);
    std::unique_lock lock(state_->mutex);
    if (state_->closed) return {};
    const auto found = state_->domains.find(key);
    if (found == state_->domains.end() && state_->jobCount() >= kMaxDomains) return {};
    auto cumulative = (request.outputs & CumulativeMaximum) ?
        std::make_shared<CumulativeHistory>(key.fftSize) : nullptr;
    if (found != state_->domains.end()) {
        std::lock_guard domainLock(found->second->mutex);
        if (cumulative) found->second->cumulativeHistories.reserve(found->second->cumulativeHistories.size() + 1);
        if (!found->second->ensureSpectrum(request.outputs)) return {};
        const auto newlyRequested = request.outputs & ~found->second->outputs.load(std::memory_order_acquire);
        const auto sequence = found->second->tap ? found->second->tap->written() : 0;
        found->second->addCumulative(cumulative, sequence);
        found->second->changeOutputs(request.outputs, true);
        if (newlyRequested & Average1s) found->second->activateAverage(sequence);
        if (newlyRequested & RealtimeMaximum) found->second->activateMaximum(sequence);
        found->second->updateAvailability();
        ++found->second->consumers; ++state_->consumers;
        return Lease(state_, found->second, request.outputs, std::move(cumulative));
    }
    auto domain = std::make_shared<Domain>(key);
    if (!domain->ensureSpectrum(request.outputs)) return {};
    domain->addCumulative(cumulative, 0);
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
        key.stereoMeter ? t.value->written() : std::max({t.value->oldest(), t.value->written() - key.fftSize, t.freshSequence}));
    domain->consumers = 1;
    domain->changeOutputs(request.outputs, true);
    const auto admission = inserted || t.value != previousTap ? t.freshSequence : (t.value ? t.value->written() : 0);
    if (request.outputs & Average1s) domain->activateAverage(admission);
    if (request.outputs & RealtimeMaximum) domain->activateMaximum(admission);
    if (cumulative) cumulative->afterSequence = admission;
    state_->domains.emplace(key, domain); ++state_->consumers;
    return Lease(state_, std::move(domain), request.outputs, std::move(cumulative));
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
    if (sources.size() < 2 || sources.size() > kMaxDomains || !columns ||
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
        ++tap.binding;
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
            if (state_->closed || !state_->workPending()) return;
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
        result.spectralPlans += static_cast<std::size_t>(domain->spectrum != nullptr);
        result.droppedHistoryColumns += domain->droppedHistory.load(std::memory_order_relaxed);
    }
    result.historyBytes = state_->historyBudget->bytes.load(std::memory_order_acquire);
    result.historyPeakBytes = state_->historyBudget->peak.load(std::memory_order_relaxed);
    return result;
}
} // namespace adi::ui
