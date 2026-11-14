// SPDX-License-Identifier: AGPL-3.0-or-later
#include "shared_analysis.hpp"
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

namespace adi::ui {
namespace {
constexpr std::uint32_t allOutputs = 31;
bool valid(const SharedAnalysis::Request &r) {
    if (r.track <= 0 || r.fftSize < 32 || r.fftSize > SharedAnalysis::kMaxFftSize || r.fftSize % 32 ||
        r.hopSize < 0 || r.hopSize > r.fftSize || !r.outputs || (r.outputs & ~allOutputs)) return false;
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
    return (outputs & (SharedAnalysis::Spectrum | SharedAnalysis::Spectrogram)) != 0;
}
std::atomic<std::size_t> processHistoryBytes{0};
struct HistoryBudget {
    explicit HistoryBudget(std::size_t maximum) : limit(std::min(maximum, SharedAnalysis::kMaxHistoryBytes)) {}
    const std::size_t limit;
    std::atomic<std::size_t> bytes{0}, peak{0};
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
    HistoryStorage(HistoryCharge c, const std::vector<float> &values) : charge(std::move(c)), bins(values) {}
};
std::shared_ptr<const std::vector<float>> historyBuffer(const std::shared_ptr<HistoryBudget> &budget,
                                                       const std::vector<float> &values) {
    const auto bytes = values.size() * sizeof(float);
    if (!budget->claim(bytes)) return {};
    HistoryCharge charge(budget, bytes);
    auto storage = std::make_shared<HistoryStorage>(std::move(charge), values);
    return {storage, &storage->bins};
}
} // namespace
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
        }()) {}
    Request request;
    std::unique_ptr<dsp::Spectrum> spectrum;
    std::vector<float> ringLeft, ringRight, left, right, scratch, linear, second;
    const double gain;
    std::size_t position = 0, filled = 0, sinceWindow = 0;
    std::uint64_t windowSerial = 0, spectralWindow = 0, historyWindow = 0;
    std::atomic<std::uint64_t> droppedHistory{0};
    std::int64_t firstStamp = 0, firstSequence = 0;
    bool advancing = false, hasWindow = false, discontinuous = false;
    bool pendingHistory = false, retiredForPending = false;
    std::int64_t pendingStamp = 0;
    std::vector<float> pendingColumn;
    std::atomic<std::int64_t> consumedSequence{0};
    std::atomic<bool> dirty{true}, historyBlocked{false};
    engine::ScopeTap::Cursor cursor;
    std::mutex mutex;
    std::shared_ptr<const engine::ScopeTap> tap;
    mutable std::mutex resultMutex;
    std::shared_ptr<const Result> result;
    std::shared_ptr<const Result> readResult() const { std::lock_guard lock(resultMutex); return result; }
    void publishResult(std::shared_ptr<const Result> next) {
        { std::lock_guard lock(resultMutex); result.swap(next); }
    }
    void resetWindow(bool epoch) {
        position = filled = sinceWindow = 0;
        hasWindow = pendingHistory = retiredForPending = false; spectralWindow = historyWindow = 0;
        historyBlocked.store(false, std::memory_order_release);
        columns.clear();
        if (epoch) ++sourceEpoch;
        publishResult({});
    }
    void bind(std::shared_ptr<const engine::ScopeTap> next, bool fresh) {
        tap = std::move(next);
        cursor = tap ? tap->cursorAtEnd() : engine::ScopeTap::Cursor{};
        if (tap && !fresh) cursor.next = std::max(tap->oldest(), tap->written() - request.fftSize);
        available.store(usable(tap, request.fftSize), std::memory_order_release);
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
        if ((bits & Output::Spectrogram) && pendingColumn.size() != linear.size()) pendingColumn.resize(linear.size());
        return true;
    }
    void releaseUnused(std::uint32_t bits) {
        if (!(bits & Output::Spectrogram)) {
            columns.clear(); pendingHistory = false; historyWindow = 0; historyBlocked.store(false, std::memory_order_release);
            std::vector<float>().swap(pendingColumn);
        }
        if (!spectralOutput(bits)) {
            spectrum.reset(); spectralWindow = 0;
            std::vector<float>().swap(scratch); std::vector<float>().swap(linear); std::vector<float>().swap(second);
        }
    }
    bool needsWork() const noexcept {
        return active.load(std::memory_order_acquire) && available.load(std::memory_order_acquire) && tap &&
               (dirty.load(std::memory_order_acquire) || historyBlocked.load(std::memory_order_acquire) ||
                tap->written() != consumedSequence.load(std::memory_order_acquire));
    }
    std::atomic<bool> active{true}, available{false};
    std::atomic<std::uint32_t> outputs{0};
    std::array<std::size_t, 5> outputUsers{}; // State mutex
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
    bool appendPendingHistory(const std::shared_ptr<HistoryBudget> &budget) {
        if (!pendingHistory) return false;
        auto storage = historyBuffer(budget, pendingColumn);
        if (!storage) {
            // Retire at most one owned column for this hop. Publishing the reduced
            // snapshot lets its old backing be released; frozen readers remain charged.
            if (!retiredForPending && !columns.empty()) {
                columns.erase(columns.begin()); retiredForPending = true;
                droppedHistory.fetch_add(1, std::memory_order_relaxed);
            }
            historyBlocked.store(true, std::memory_order_release); return false;
        }
        if (columns.size() == 128) columns.erase(columns.begin());
        columns.push_back({columnSerial, pendingStamp, std::move(storage)});
        pendingHistory = retiredForPending = false; historyBlocked.store(false, std::memory_order_release);
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
        if (spectralOutput(bits) && !spectrum) return {};
        auto latest = std::make_shared<Result>();
        latest->request = request; latest->outputs = bits; latest->request.outputs = bits;
        latest->sourceEpoch = sourceEpoch; latest->generation = tap->generation();
        latest->revision = ++revision; latest->sampleRate = tap->sampleRate();
        latest->windowSerial = windowSerial;
        latest->coherentGain = gain; latest->hopSize = request.hopSize;
        latest->firstSequence = firstSequence; latest->advancing = advancing; latest->firstStamp = firstStamp;
        latest->gapCount = gaps; latest->overrunCount = overruns; latest->discontinuity = discontinuous;
        if (!newWindow) if (const auto previous = readResult()) latest->nonFiniteSamples = previous->nonFiniteSamples;
        const auto n = left.size();
        double sumL = 0, sumR = 0, cross = 0;
        bool nonzero = false;
        for (std::size_t i = 0; i < n; ++i) {
            const auto k = (position + i) % n;
            const float rawL = newWindow ? ringLeft[k] : left[i], rawR = newWindow ? ringRight[k] : right[i];
            latest->nonFiniteSamples += static_cast<std::size_t>(!std::isfinite(rawL)) +
                                        static_cast<std::size_t>(!std::isfinite(rawR));
            const float l = left[i] = std::isfinite(rawL) ? rawL : 0.f;
            const float r = right[i] = std::isfinite(rawR) ? rawR : 0.f;
            sumL += static_cast<double>(l) * l; sumR += static_cast<double>(r) * r;
            cross += static_cast<double>(l) * r;
            latest->peak[0] = std::max(latest->peak[0], std::abs(l));
            latest->peak[1] = std::max(latest->peak[1], std::abs(r));
            if (spectralOutput(bits)) {
                switch (request.channel) {
                case Channel::Mid: scratch[i] = (l + r) * .5f; break;
                case Channel::Side: scratch[i] = (l - r) * .5f; break;
                case Channel::Left: case Channel::StereoPower: scratch[i] = l; break;
                case Channel::Right: scratch[i] = r; break;
                }
                nonzero = nonzero || scratch[i] != 0 || (request.channel == Channel::StereoPower && r != 0);
            }
        }
        latest->rms = {static_cast<float>(std::sqrt(sumL / static_cast<double>(n))),
                       static_cast<float>(std::sqrt(sumR / static_cast<double>(n)))};
        latest->correlation = sumL > 0 && sumR > 0 ? std::clamp(cross / std::sqrt(sumL * sumR), -1., 1.) : 0.;
        if (bits & (Output::Scope | Output::Phase)) { latest->scopeLeft = left; latest->scopeRight = right; }
        if (spectralOutput(bits)) {
            if (spectralWindow != windowSerial) {
                if (nonzero) { spectrum->analyse(scratch, linear); ++transforms; }
                else std::fill(linear.begin(), linear.end(), 0.f);
                if (nonzero && request.channel == Channel::StereoPower) {
                    spectrum->analyse(right, second); ++transforms;
                    for (std::size_t i = 0; i < linear.size(); ++i)
                        linear[i] = std::sqrt((linear[i] * linear[i] + second[i] * second[i]) * .5f);
                }
                spectralWindow = windowSerial;
            }
            latest->binsDbfs.resize(linear.size());
            std::transform(linear.begin(), linear.end(), latest->binsDbfs.begin(), dsp::toDbfs);
            if (bits & Output::Spectrogram) {
                if (historyWindow != windowSerial) {
                    if (pendingHistory) droppedHistory.fetch_add(1, std::memory_order_relaxed);
                    historyWindow = windowSerial; ++columnSerial; pendingStamp = firstStamp;
                    pendingColumn = latest->binsDbfs; pendingHistory = true; retiredForPending = false;
                }
                (void)appendPendingHistory(budget);
            }
        }
        return latest;
    }
    std::shared_ptr<Result> retryHistory(const std::shared_ptr<HistoryBudget> &budget) {
        if (!pendingHistory || !appendPendingHistory(budget)) return {};
        const auto previous = readResult();
        if (!previous) return {};
        auto latest = std::make_shared<Result>(*previous);
        latest->revision = ++revision;
        return latest;
    }
};
struct SharedAnalysis::State {
    struct Tap {
        std::shared_ptr<const engine::ScopeTap> value;
        std::size_t domains = 0;
        bool attached = false; // acquired source demand, including a pending/null tap
        double seconds = .25;
        std::int64_t freshSequence = 0;
    };
    State(AcquireSourceTap a, ReleaseSourceTap r, std::size_t limit)
        : acquire(std::move(a)), release(std::move(r)), owner(std::this_thread::get_id()), historyBudget(std::make_shared<HistoryBudget>(limit)) {}
    void ownerOnly() const { assert(owner == std::this_thread::get_id()); }
    static double historySeconds(std::int32_t size, const std::shared_ptr<const engine::ScopeTap> &tap) {
        return tap && std::isfinite(tap->sampleRate()) && tap->sampleRate() > 0 ?
            std::max(.25, 4. * size / tap->sampleRate()) : .25;
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
            for (auto &[request, domain] : domains) if (request.source() == key) {
                std::lock_guard domainLock(domain->mutex);
                domain->bind(tap.value, true); domain->startAt(tap.freshSequence); ++domain->sourceEpoch;
            }
        }
    }
    void drop(const std::shared_ptr<Domain> &domain, std::uint32_t outputs) noexcept {
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
                const double rate = at->second.value ? at->second.value->sampleRate() : 44100.;
                double seconds = .25;
                for (const auto &[request, remaining] : domains) {
                    (void)remaining;
                    if (request.source() == key) seconds = std::max(seconds, 4. * request.fftSize / rate);
                }
                if (seconds < at->second.seconds) {
                    try {
                        auto next = acquire(key, seconds);
                        at->second.seconds = seconds;
                        if (next != at->second.value) {
                            at->second.value = std::move(next);
                            at->second.attached = at->second.attached || at->second.value != nullptr;
                            at->second.freshSequence = at->second.value ? at->second.value->written() : 0;
                            for (auto &[request, remaining] : domains) if (request.source() == key) {
                                std::lock_guard domainLock(remaining->mutex);
                                remaining->bind(at->second.value, true); remaining->startAt(at->second.freshSequence); ++remaining->sourceEpoch;
                            }
                        }
                    } catch (...) { /* retain the earlier, safely larger history on failure */ }
                }
            }
        }
        if (detach && release) { try { release(key); } catch (...) {} }
    }
    bool workPending() const {
        return std::any_of(domains.begin(), domains.end(), [](const auto &entry) { return entry.second->needsWork(); });
    }
    std::size_t pump() {
        // Never audio-reachable. Shutdown waits before releasing owner callbacks.
        std::lock_guard run(runMutex);
        std::array<std::shared_ptr<Domain>, kMaxDomains> jobs;
        std::size_t count = 0, done = 0, windows = 0, samples = 0, reads = 0;
        {
            std::lock_guard lock(mutex);
            if (closed || !workPending()) return 0;
            for (const auto &[key, domain] : domains) { (void)key; jobs[count++] = domain; }
        }
        // One bounded read scratch pair per pass, independent of domain/output type.
        std::array<float, 4096> readLeft, readRight;
        workerPasses.fetch_add(1, std::memory_order_relaxed);
        const auto start = count ? nextJob++ % count : 0;
        for (std::size_t j = 0; j < count && windows < kMaxWindowsPerFrame && samples < kMaxSamplesPerFrame && reads < kMaxReadsPerFrame; ++j) {
            auto &d = *jobs[(start + j) % count];
            std::lock_guard lock(d.mutex);
            if (!d.needsWork()) continue;
            const bool changed = d.dirty.exchange(false, std::memory_order_acq_rel);
            std::shared_ptr<Result> latest;
            bool discontinuity = false;
            const auto n = d.left.size(), hop = static_cast<std::size_t>(d.request.hopSize);
            while (d.tap->written() != d.cursor.next && windows < kMaxWindowsPerFrame && samples < kMaxSamplesPerFrame && reads < kMaxReadsPerFrame) {
                const auto need = d.filled < n ? n - d.filled : hop - d.sinceWindow;
                const auto length = std::min({need, kMaxSamplesPerFrame - samples, readLeft.size()});
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
                d.firstStamp = read.firstStamp + (read.advancing ? read.frames - static_cast<std::int64_t>(n) : 0);
                d.discontinuous = discontinuity;
                latest = d.analyse(historyBudget, done);
            }
            d.consumedSequence.store(d.cursor.next, std::memory_order_release);
            // New output demand can reuse the retained completed window even at idle.
            // A partial hop never replaces that completed window's raw sample pair.
            if (!latest && changed && d.hasWindow) latest = d.analyse(historyBudget, done, false);
            if (!latest && d.historyBlocked.load(std::memory_order_acquire)) latest = d.retryHistory(historyBudget);
            if (latest) {
                // Only the final publication needs the shallow history snapshot;
                // intermediate hops update the domain, not duplicate metadata.
                d.setHistory(*latest);
                latest->pendingFrames = std::max<std::int64_t>(0, d.tap->written() - d.cursor.next);
                latest->budgetLimited = latest->pendingFrames > 0 &&
                    (windows == kMaxWindowsPerFrame || samples == kMaxSamplesPerFrame || reads == kMaxReadsPerFrame);
                if (d.active.load(std::memory_order_acquire)) {
                    d.publishResult(std::move(latest)); publications.fetch_add(1, std::memory_order_relaxed);
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
    std::map<engine::ScopeSource, Tap> taps;
    std::size_t consumers = 0, nextJob = 0;
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
SharedAnalysis::Lease::Lease(std::weak_ptr<State> state, std::shared_ptr<Domain> domain, std::uint32_t outputs)
    : state_(std::move(state)), domain_(std::move(domain)), outputs_(outputs) {}
SharedAnalysis::Lease::~Lease() { reset(); }
SharedAnalysis::Lease::Lease(Lease &&other) noexcept
    : state_(std::move(other.state_)), domain_(std::move(other.domain_)), outputs_(std::exchange(other.outputs_, 0)) {}
SharedAnalysis::Lease &SharedAnalysis::Lease::operator=(Lease &&other) noexcept {
    if (this != &other) { reset(); state_ = std::move(other.state_); domain_ = std::move(other.domain_); outputs_ = std::exchange(other.outputs_, 0); }
    return *this;
}
void SharedAnalysis::Lease::reset() noexcept {
    if (domain_) if (auto state = state_.lock()) state->drop(domain_, outputs_);
    domain_.reset(); state_.reset(); outputs_ = 0;
}
SharedAnalysis::Lease::operator bool() const noexcept { return domain_ && !state_.expired() && domain_->active.load(std::memory_order_acquire); }
bool SharedAnalysis::Lease::available() const noexcept { return static_cast<bool>(*this) && domain_->available.load(std::memory_order_acquire); }
std::shared_ptr<const SharedAnalysis::Result> SharedAnalysis::Lease::read() const noexcept {
    auto result = available() ? domain_->readResult() : nullptr;
    return result && (result->outputs & outputs_) == outputs_ ? result : nullptr;
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
    if (const auto found = state_->domains.find(key); found != state_->domains.end()) {
        std::lock_guard domainLock(found->second->mutex);
        if (!found->second->ensureSpectrum(request.outputs)) return {};
        found->second->changeOutputs(request.outputs, true); ++found->second->consumers; ++state_->consumers;
        return Lease(state_, found->second, request.outputs);
    }
    if (state_->domains.size() >= kMaxDomains) return {};
    auto domain = std::make_shared<Domain>(key);
    if (!domain->ensureSpectrum(request.outputs)) return {};
    auto [tap, inserted] = state_->taps.try_emplace(key.source());
    auto &t = tap->second;
    auto maximumSize = key.fftSize;
    for (const auto &[other, ignored] : state_->domains) {
        (void)ignored;
        if (other.source() == key.source()) maximumSize = std::max(maximumSize, other.fftSize);
    }
    try { state_->resolveTap(key.source(), t, maximumSize, false); }
    catch (...) {
        if (inserted) {
            const bool detach = t.attached;
            state_->taps.erase(tap);
            lock.unlock();
            if (detach && state_->release) { try { state_->release(key.source()); } catch (...) {} }
        }
        throw;
    }
    ++t.domains; domain->bind(t.value, true);
    if (t.value) domain->startAt(inserted ? t.freshSequence :
        std::max({t.value->oldest(), t.value->written() - key.fftSize, t.freshSequence}));
    domain->consumers = 1;
    domain->changeOutputs(request.outputs, true);
    state_->domains.emplace(key, domain); ++state_->consumers;
    return Lease(state_, std::move(domain), request.outputs);
}
void SharedAnalysis::refreshSources(bool forceEpoch) {
    state_->ownerOnly();
    std::lock_guard lock(state_->mutex);
    if (state_->closed) return;
    for (auto &[key, tap] : state_->taps) {
        const auto previous = tap.value;
        std::int32_t maximumSize = 32;
        for (const auto &[request, ignored] : state_->domains) {
            (void)ignored;
            if (request.source() == key) maximumSize = std::max(maximumSize, request.fftSize);
        }
        state_->resolveTap(key, tap, maximumSize, true);
        if (!forceEpoch || previous != tap.value) continue;
        tap.freshSequence = tap.value ? tap.value->written() : 0;
        for (auto &[request, domain] : state_->domains) if (request.source() == key) {
            std::lock_guard domainLock(domain->mutex);
            domain->bind(tap.value, true); domain->startAt(tap.freshSequence); ++domain->sourceEpoch;
        }
    }
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
    Stats result{state_->consumers, state_->domains.size(), state_->taps.size(),
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
