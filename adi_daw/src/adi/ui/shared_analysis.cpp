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
} // namespace
struct SharedAnalysis::Domain {
    explicit Domain(Request r) : request(r), spectrum(r.fftSize, r.window),
        ringLeft(static_cast<std::size_t>(r.fftSize)), ringRight(ringLeft.size()),
        left(ringLeft.size()), right(left.size()), scratch(left.size()),
        linear(static_cast<std::size_t>(r.fftSize / 2 + 1)), second(linear.size()),
        gain(dsp::coherentGain(r.window, r.fftSize)) {}
    Request request;
    dsp::Spectrum spectrum;
    std::vector<float> ringLeft, ringRight, left, right, scratch, linear, second;
    const double gain;
    std::size_t position = 0, filled = 0, sinceWindow = 0;
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
    }
    std::atomic<bool> active{true}, available{false};
    std::atomic<std::uint32_t> outputs{0};
    std::array<std::size_t, 5> outputUsers{}; // State mutex
    std::size_t consumers = 0;
    std::uint64_t sourceEpoch = 1, revision = 0, columnSerial = 0, gaps = 0, overruns = 0;
    std::vector<Column> columns;
    void changeOutputs(std::uint32_t bits, bool add) {
        std::uint32_t unionBits = 0;
        for (std::size_t i = 0; i < outputUsers.size(); ++i) {
            if (bits & (1u << i)) { if (add) ++outputUsers[i]; else --outputUsers[i]; }
            if (outputUsers[i]) unionBits |= 1u << i;
        }
        outputs.store(unionBits, std::memory_order_release);
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
    State(AcquireSourceTap a, ReleaseSourceTap r) : acquire(std::move(a)), release(std::move(r)), owner(std::this_thread::get_id()) {}
    void ownerOnly() const { assert(owner == std::this_thread::get_id()); }
    void drop(const std::shared_ptr<Domain> &domain, std::uint32_t outputs) noexcept {
        ownerOnly();
        bool detach = false;
        const auto key = domain->request.source();
        {
            std::lock_guard lock(mutex);
            if (closed || !domain->consumers) return;
            domain->changeOutputs(outputs, false);
            --domain->consumers; --consumers;
            if (domain->consumers) return;
            domain->active.store(false, std::memory_order_release);
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
                                remaining->bind(at->second.value, true); ++remaining->sourceEpoch;
                            }
                        }
                    } catch (...) { /* retain the earlier, safely larger history on failure */ }
                }
            }
        }
        if (detach && release) { try { release(key); } catch (...) {} }
    }
    std::size_t pump() {
        // This lock is never reachable from audio. Shutdown holds it before releasing owner
        // callbacks; a queued process-worker request can therefore never outlive its Session.
        std::lock_guard run(runMutex);
        std::array<std::shared_ptr<Domain>, kMaxDomains> jobs;
        std::size_t count = 0, done = 0, windows = 0, samples = 0, reads = 0;
        {
            std::lock_guard lock(mutex);
            if (closed) return 0;
            for (const auto &[key, domain] : domains) { (void)key; jobs[count++] = domain; }
        }
        workerPasses.fetch_add(1, std::memory_order_relaxed);
        const auto start = count ? nextJob++ % count : 0;
        for (std::size_t j = 0; j < count && windows < kMaxWindowsPerFrame && samples < kMaxSamplesPerFrame && reads < kMaxReadsPerFrame; ++j) {
            auto &d = *jobs[(start + j) % count];
            std::lock_guard lock(d.mutex);
            if (!d.active.load(std::memory_order_acquire) || !d.available.load(std::memory_order_acquire)) continue;
            std::shared_ptr<Result> latest;
            bool discontinuity = false;
            const auto n = d.left.size(), hop = static_cast<std::size_t>(d.request.hopSize);
            while (windows < kMaxWindowsPerFrame && samples < kMaxSamplesPerFrame && reads < kMaxReadsPerFrame) {
                // Read only to the next hop boundary; independent cursors do not destroy each
                // other's history and every transform has a known, contiguous sample origin.
                const auto need = d.filled < n ? n - d.filled : hop - d.sinceWindow;
                const auto length = std::min(need, kMaxSamplesPerFrame - samples);
                ++reads;
                auto read = d.tap->read(d.cursor, std::span<float>(d.left.data(), length),
                                       std::span<float>(d.right.data(), length));
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
                    d.ringLeft[d.position] = d.left[static_cast<std::size_t>(i)];
                    d.ringRight[d.position] = d.right[static_cast<std::size_t>(i)];
                    d.position = (d.position + 1) % n;
                }
                const bool wasFull = d.filled == n;
                d.filled = std::min(n, d.filled + static_cast<std::size_t>(read.frames));
                if (wasFull) d.sinceWindow += static_cast<std::size_t>(read.frames);
                if (d.filled != n || (wasFull && d.sinceWindow < hop)) continue;
                d.sinceWindow = 0; ++windows;
                latest = std::make_shared<Result>();
                latest->request = d.request;
                latest->outputs = d.outputs.load(std::memory_order_acquire);
                latest->request.outputs = latest->outputs;
                latest->sourceEpoch = d.sourceEpoch; latest->generation = d.tap->generation();
                latest->revision = ++d.revision; latest->sampleRate = d.tap->sampleRate();
                latest->coherentGain = d.gain; latest->hopSize = static_cast<std::int32_t>(hop);
                latest->firstSequence = d.cursor.next - static_cast<std::int64_t>(n);
                latest->advancing = read.advancing;
                latest->firstStamp = read.firstStamp + (read.advancing ? read.frames - static_cast<std::int64_t>(n) : 0);
                latest->gapCount = d.gaps; latest->overrunCount = d.overruns;
                latest->discontinuity = discontinuity;
                double sumL = 0, sumR = 0, cross = 0;
                bool nonzero = false;
                for (std::size_t i = 0; i < n; ++i) {
                    const auto k = (d.position + i) % n;
                    const float rawL = d.ringLeft[k], rawR = d.ringRight[k];
                    latest->nonFiniteSamples += static_cast<std::size_t>(!std::isfinite(rawL)) +
                                                static_cast<std::size_t>(!std::isfinite(rawR));
                    const float l = d.left[i] = std::isfinite(rawL) ? rawL : 0.f;
                    const float r = d.right[i] = std::isfinite(rawR) ? rawR : 0.f;
                    sumL += static_cast<double>(l) * l; sumR += static_cast<double>(r) * r;
                    cross += static_cast<double>(l) * r;
                    latest->peak[0] = std::max(latest->peak[0], std::abs(l));
                    latest->peak[1] = std::max(latest->peak[1], std::abs(r));
                    switch (d.request.channel) {
                    case Channel::Mid: d.scratch[i] = (l + r) * .5f; break;
                    case Channel::Side: d.scratch[i] = (l - r) * .5f; break;
                    case Channel::Left: case Channel::StereoPower: d.scratch[i] = l; break;
                    case Channel::Right: d.scratch[i] = r; break;
                    }
                    nonzero = nonzero || d.scratch[i] != 0 || (d.request.channel == Channel::StereoPower && r != 0);
                }
                latest->rms = {static_cast<float>(std::sqrt(sumL / static_cast<double>(n))),
                               static_cast<float>(std::sqrt(sumR / static_cast<double>(n)))};
                latest->correlation = sumL > 0 && sumR > 0 ? std::clamp(cross / std::sqrt(sumL * sumR), -1., 1.) : 0.;
                if (latest->outputs & (Output::Scope | Output::Phase)) {
                    latest->scopeLeft = d.left; latest->scopeRight = d.right;
                }
                if (latest->outputs & (Output::Spectrum | Output::Spectrogram)) {
                    if (nonzero) { d.spectrum.analyse(d.scratch, d.linear); ++done; }
                    else std::fill(d.linear.begin(), d.linear.end(), 0.f);
                    if (nonzero && d.request.channel == Channel::StereoPower) {
                        d.spectrum.analyse(d.right, d.second); ++done;
                        for (std::size_t i = 0; i < d.linear.size(); ++i)
                            d.linear[i] = std::sqrt((d.linear[i] * d.linear[i] + d.second[i] * d.second[i]) * .5f);
                    }
                    latest->binsDbfs.resize(d.linear.size());
                    std::transform(d.linear.begin(), d.linear.end(), latest->binsDbfs.begin(), dsp::toDbfs);
                    if (latest->outputs & Output::Spectrogram) {
                        if (d.columns.size() == 128) d.columns.erase(d.columns.begin());
                        d.columns.push_back({++d.columnSerial, latest->firstStamp, latest->binsDbfs});
                    } else d.columns.clear();
                } else d.columns.clear();
            }
            if (latest) {
                latest->pendingFrames = std::max<std::int64_t>(0, d.tap->written() - d.cursor.next);
                latest->budgetLimited = latest->pendingFrames > 0 &&
                    (windows == kMaxWindowsPerFrame || samples == kMaxSamplesPerFrame || reads == kMaxReadsPerFrame);
                latest->columns = d.columns;
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
    mutable std::mutex mutex;
    std::mutex runMutex;
    std::map<Request, std::shared_ptr<Domain>> domains;
    std::map<engine::ScopeSource, Tap> taps;
    std::size_t consumers = 0, nextJob = 0;
    bool closed = false;
    std::atomic<bool> queued{false};
    std::atomic<std::uint64_t> transforms{0}, publications{0}, frameRequests{0}, workerPasses{0};
    std::atomic<std::uint64_t> cursorReads{0}, analysisWindows{0};
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
        { std::lock_guard lock(mutex); pending.emplace_back(state); }
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
SharedAnalysis::SharedAnalysis(AcquireSourceTap a, ReleaseSourceTap r, Mode mode)
    : state_(std::make_shared<State>(std::move(a), std::move(r))), mode_(mode) {}
SharedAnalysis::SharedAnalysis(AcquireTap a, ReleaseTap r, Mode mode)
    : SharedAnalysis([a = std::move(a)](engine::ScopeSource source, double) {
          return a && !source.device ? a(source.track, source.point) : nullptr;
      }, [r = std::move(r)](engine::ScopeSource source) { if (r) r(source.track, source.point); }, mode) {}
SharedAnalysis::~SharedAnalysis() { shutdown(); }
void SharedAnalysis::shutdown() noexcept {
    state_->ownerOnly();
    std::lock_guard run(state_->runMutex);
    std::map<engine::ScopeSource, State::Tap> taps;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) return;
        state_->closed = true;
        for (auto &[key, domain] : state_->domains) { (void)key; domain->active.store(false, std::memory_order_release); }
        state_->domains.clear(); taps.swap(state_->taps); state_->consumers = 0;
    }
    for (const auto &[key, tap] : taps) if (tap.attached && state_->release) { try { state_->release(key); } catch (...) {} }
}
SharedAnalysis::Lease SharedAnalysis::acquire(const Request &request) {
    state_->ownerOnly();
    if (!valid(request)) return {};
    const auto key = keyFor(request);
    std::lock_guard lock(state_->mutex);
    if (state_->closed) return {};
    if (const auto found = state_->domains.find(key); found != state_->domains.end()) {
        found->second->changeOutputs(request.outputs, true); ++found->second->consumers; ++state_->consumers;
        return Lease(state_, found->second, request.outputs);
    }
    if (state_->domains.size() >= kMaxDomains) return {};
    auto domain = std::make_shared<Domain>(key);
    if (!domain->spectrum.valid()) return {};
    auto [tap, inserted] = state_->taps.try_emplace(key.source());
    auto &t = tap->second;
    // Four windows give a consumer time to catch up, negotiated against actual rate. This
    // is display history, not a forced ten-second allocation for every selected device.
    const double rate = t.value ? t.value->sampleRate() : 44100.;
    const auto seconds = std::max(.25, 4. * key.fftSize / rate);
    if (inserted || seconds > t.seconds || !t.value) {
        t.seconds = std::max(t.seconds, seconds);
        t.attached = t.attached || static_cast<bool>(state_->acquire);
        auto next = state_->acquire ? state_->acquire(key.source(), t.seconds) : nullptr;
        if (next && std::isfinite(next->sampleRate()) && next->sampleRate() > 0 &&
            next->capacity() < key.fftSize && state_->acquire) {
            t.seconds = std::max(t.seconds, 4. * key.fftSize / next->sampleRate());
            next = state_->acquire(key.source(), t.seconds);
        }
        const bool replaced = next != t.value;
        t.value = std::move(next); t.attached = t.attached || t.value != nullptr;
        if (replaced || inserted) t.freshSequence = t.value ? t.value->written() : 0;
        if (replaced) for (auto &[otherKey, other] : state_->domains) if (otherKey.source() == key.source()) {
            std::lock_guard domainLock(other->mutex); other->bind(t.value, true); ++other->sourceEpoch;
        }
    }
    ++t.domains; domain->bind(t.value, true);
    if (t.value && !inserted)
        domain->cursor.next = std::max({t.value->oldest(), t.value->written() - key.fftSize, t.freshSequence});
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
        auto next = state_->acquire ? state_->acquire(key, tap.seconds) : nullptr;
        const bool replaced = next != tap.value;
        tap.value = std::move(next); tap.attached = tap.attached || tap.value != nullptr;
        if (!replaced && !forceEpoch) continue;
        tap.freshSequence = tap.value ? tap.value->written() : 0;
        for (auto &[request, domain] : state_->domains) if (request.source() == key) {
            std::lock_guard domainLock(domain->mutex);
            domain->bind(tap.value, true); ++domain->sourceEpoch;
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
            if (state_->closed || std::none_of(state_->domains.begin(), state_->domains.end(), [](const auto &entry) {
                return entry.second->available.load(std::memory_order_acquire);
            })) return;
        }
        Scheduler::instance().request(state_);
    } catch (...) { state_->queued.store(false, std::memory_order_release); }
}
std::size_t SharedAnalysis::pump() { return state_->pump(); }
SharedAnalysis::Stats SharedAnalysis::stats() const {
    std::lock_guard lock(state_->mutex);
    return {state_->consumers, state_->domains.size(), state_->taps.size(),
        state_->transforms.load(std::memory_order_relaxed), state_->publications.load(std::memory_order_relaxed),
        state_->frameRequests.load(std::memory_order_relaxed), state_->workerPasses.load(std::memory_order_relaxed),
        state_->cursorReads.load(std::memory_order_relaxed), state_->analysisWindows.load(std::memory_order_relaxed)};
}
} // namespace adi::ui
