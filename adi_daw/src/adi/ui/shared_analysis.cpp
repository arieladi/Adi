// SPDX-License-Identifier: AGPL-3.0-or-later
#include "shared_analysis.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace adi::ui {
namespace {
struct TapKey {
    std::int64_t track;
    engine::ScopePoint point;
    auto operator<=>(const TapKey &) const = default;
};
bool valid(const SharedAnalysis::Request &r) {
    if (r.track <= 0 || r.fftSize < 32 || r.fftSize > SharedAnalysis::kMaxFftSize || r.fftSize % 32 != 0)
        return false;
    auto n = r.fftSize;
    for (const auto factor : {2, 3, 5})
        while (n % factor == 0) n /= factor;
    return n == 1 && (r.window == dsp::Window::Hann || r.window == dsp::Window::Rectangular) &&
           (r.channel == SharedAnalysis::Channel::Mid || r.channel == SharedAnalysis::Channel::Side ||
            r.channel == SharedAnalysis::Channel::Left || r.channel == SharedAnalysis::Channel::Right) &&
           (r.point == engine::ScopePoint::PostFader || r.point == engine::ScopePoint::PreFader ||
            r.point == engine::ScopePoint::ChainInput);
}
bool usable(const std::shared_ptr<const engine::ScopeTap> &tap, std::int32_t size) {
    return tap && std::isfinite(tap->sampleRate()) && tap->sampleRate() > 0 &&
           tap->capacity() >= size &&
           static_cast<double>(tap->capacity()) + 1.0 >= tap->sampleRate() * SharedAnalysis::kTapSeconds;
}
} // namespace
struct SharedAnalysis::Domain {
    explicit Domain(Request r) : request(r), spectrum(r.fftSize, r.window),
        left(static_cast<std::size_t>(r.fftSize)), right(left.size()),
        linear(static_cast<std::size_t>(r.fftSize / 2 + 1)) {}
    Request request;
    dsp::Spectrum spectrum;
    std::vector<float> left, right, linear;
    std::mutex mutex;
    std::shared_ptr<const engine::ScopeTap> tap;
    // Some supported libc++ releases have no atomic<shared_ptr> specialization. Only the
    // UI and analysis worker touch this publication; the audio callback never takes this lock.
    mutable std::mutex resultMutex;
    std::shared_ptr<const Result> result;
    std::shared_ptr<const Result> readResult() const {
        std::lock_guard lock(resultMutex);
        return result;
    }
    void publishResult(std::shared_ptr<const Result> next) {
        std::lock_guard lock(resultMutex);
        result.swap(next); // next releases the old allocation after the guard is destroyed
    }
    std::atomic<bool> active{true}, available{false};
    std::size_t consumers = 0; // State mutex
    std::int64_t lastWritten = 0; // Domain mutex
    std::uint64_t sourceEpoch = 1, revision = 0; // Domain mutex
};
struct SharedAnalysis::State {
    struct Tap {
        std::shared_ptr<const engine::ScopeTap> value;
        std::size_t domains = 0;
        bool attached = false;
        std::int64_t attachmentWritten = 0;
    };
    State(AcquireTap a, ReleaseTap r) : acquire(std::move(a)), release(std::move(r)), owner(std::this_thread::get_id()) {}
    void ownerOnly() const { assert(owner == std::this_thread::get_id()); }
    void drop(const std::shared_ptr<Domain> &domain) noexcept {
        ownerOnly();
        bool detach = false;
        const TapKey key{domain->request.track, domain->request.point};
        {
            std::lock_guard lock(mutex);
            if (closed || !domain->consumers) return;
            --domain->consumers;
            --consumers;
            if (domain->consumers) return;
            domain->active.store(false, std::memory_order_release);
            domains.erase(domain->request);
            const auto at = taps.find(key);
            if (at != taps.end() && --at->second.domains == 0) {
                detach = at->second.attached;
                taps.erase(at);
            }
        }
        if (detach && release) {
            try { release(key.track, key.point); } catch (...) { /* destructor must not throw */ }
        }
        changed.notify_all();
    }
    std::size_t pump() {
        std::array<std::shared_ptr<Domain>, kMaxDomains> jobs;
        std::size_t count = 0, done = 0;
        {
            std::lock_guard lock(mutex);
            if (closed) return 0;
            for (const auto &[key, domain] : domains) {
                (void)key;
                jobs[count++] = domain;
            }
        }
        for (std::size_t index = 0; index < count; ++index) {
            auto &domain = *jobs[index];
            std::lock_guard lock(domain.mutex);
            if (!domain.active.load(std::memory_order_acquire) || !domain.available.load(std::memory_order_acquire)) continue;
            const auto written = domain.tap->written();
            if (written == domain.lastWritten || written < domain.request.fftSize) continue;
            std::int64_t stamp = 0;
            if (!domain.tap->read(domain.left, domain.right, stamp)) continue;
            auto out = std::make_shared<Result>();
            out->request = domain.request;
            out->sourceEpoch = domain.sourceEpoch;
            out->revision = ++domain.revision;
            out->firstStamp = stamp;
            out->sampleRate = domain.tap->sampleRate();
            double sumLeft = 0, sumRight = 0;
            for (std::size_t i = 0; i < domain.left.size(); ++i) {
                const float l = domain.left[i], r = domain.right[i];
                out->peak[0] = std::max(out->peak[0], std::abs(l));
                out->peak[1] = std::max(out->peak[1], std::abs(r));
                sumLeft += static_cast<double>(l) * l;
                sumRight += static_cast<double>(r) * r;
                switch (domain.request.channel) {
                case Channel::Mid: domain.left[i] = (l + r) * .5f; break;
                case Channel::Side: domain.left[i] = (l - r) * .5f; break;
                case Channel::Left: break;
                case Channel::Right: domain.left[i] = r; break;
                }
            }
            const auto size = static_cast<double>(domain.left.size());
            out->rms = {static_cast<float>(std::sqrt(sumLeft / size)), static_cast<float>(std::sqrt(sumRight / size))};
            domain.spectrum.analyse(domain.left, domain.linear);
            out->binsDbfs.resize(domain.linear.size());
            std::transform(domain.linear.begin(), domain.linear.end(), out->binsDbfs.begin(), dsp::toDbfs);
            domain.lastWritten = written;
            domain.publishResult(std::move(out));
            transforms.fetch_add(1, std::memory_order_relaxed);
            publications.fetch_add(1, std::memory_order_relaxed);
            ++done;
        }
        return done;
    }
    AcquireTap acquire;
    ReleaseTap release;
    std::thread::id owner;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<Request, std::shared_ptr<Domain>> domains;
    std::map<TapKey, Tap> taps;
    std::size_t consumers = 0;
    bool closed = false;
    std::atomic<std::uint64_t> transforms{0}, publications{0};
};
struct SharedAnalysis::Worker {
    explicit Worker(const std::shared_ptr<State> &state) : thread([state](std::stop_token stop) {
        std::unique_lock lock(state->mutex);
        while (!stop.stop_requested() && !state->closed) {
            state->changed.wait(lock, [&] { return stop.stop_requested() || state->closed || !state->domains.empty(); });
            if (stop.stop_requested() || state->closed) break;
            lock.unlock();
            state->pump();
            lock.lock();
            state->changed.wait_for(lock, std::chrono::milliseconds(33), [&] {
                return stop.stop_requested() || state->closed || state->domains.empty();
            });
        }
    }) {}
    std::jthread thread;
};
SharedAnalysis::Lease::Lease(std::weak_ptr<State> state, std::shared_ptr<Domain> domain)
    : state_(std::move(state)), domain_(std::move(domain)) {}
SharedAnalysis::Lease::~Lease() { reset(); }
SharedAnalysis::Lease::Lease(Lease &&other) noexcept : state_(std::move(other.state_)), domain_(std::move(other.domain_)) {}
SharedAnalysis::Lease &SharedAnalysis::Lease::operator=(Lease &&other) noexcept {
    if (this != &other) { reset(); state_ = std::move(other.state_); domain_ = std::move(other.domain_); }
    return *this;
}
void SharedAnalysis::Lease::reset() noexcept {
    if (domain_) if (auto state = state_.lock()) state->drop(domain_);
    domain_.reset(); state_.reset();
}
SharedAnalysis::Lease::operator bool() const noexcept {
    return domain_ && !state_.expired() && domain_->active.load(std::memory_order_acquire);
}
bool SharedAnalysis::Lease::available() const noexcept {
    return static_cast<bool>(*this) && domain_->available.load(std::memory_order_acquire);
}
std::shared_ptr<const SharedAnalysis::Result> SharedAnalysis::Lease::read() const noexcept {
    return available() ? domain_->readResult() : nullptr;
}
SharedAnalysis::SharedAnalysis(AcquireTap acquire, ReleaseTap release, Mode mode)
    : state_(std::make_shared<State>(std::move(acquire), std::move(release))) {
    if (mode == Mode::Worker) worker_ = std::make_unique<Worker>(state_);
}
SharedAnalysis::~SharedAnalysis() { shutdown(); }
void SharedAnalysis::shutdown() noexcept {
    state_->ownerOnly();
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) return;
        state_->closed = true;
        for (auto &[key, domain] : state_->domains) {
            (void)key;
            domain->active.store(false, std::memory_order_release);
        }
    }
    if (worker_) worker_->thread.request_stop();
    state_->changed.notify_all();
    worker_.reset(); // joins before callbacks or their owner can go away
    for (const auto &[key, tap] : state_->taps)
        if (tap.attached && state_->release) {
            try { state_->release(key.track, key.point); } catch (...) { /* destructor must not throw */ }
        }
    std::lock_guard lock(state_->mutex);
    state_->domains.clear();
    state_->taps.clear();
    state_->consumers = 0;
}
SharedAnalysis::Lease SharedAnalysis::acquire(const Request &request) {
    state_->ownerOnly();
    if (!valid(request)) return {};
    std::lock_guard lock(state_->mutex);
    if (state_->closed) return {};
    auto found = state_->domains.find(request);
    if (found != state_->domains.end()) {
        ++found->second->consumers;
        ++state_->consumers;
        return Lease(state_, found->second);
    }
    if (state_->domains.size() >= kMaxDomains) return {};
    auto domain = std::make_shared<Domain>(request);
    if (!domain->spectrum.valid()) return {};
    const TapKey key{request.track, request.point};
    auto [tap, inserted] = state_->taps.try_emplace(key);
    if (inserted && state_->acquire) {
        tap->second.value = state_->acquire(key.track, key.point);
        tap->second.attached = tap->second.value != nullptr;
        tap->second.attachmentWritten = tap->second.value ? tap->second.value->written() : 0;
    }
    ++tap->second.domains;
    domain->tap = tap->second.value;
    // Session retains the tap's history while detached. First renewed demand must wait for
    // audio written after attachment; compatible consumers of an existing domain share its
    // current result, and an additional configuration of an already active tap may read it.
    domain->lastWritten = tap->second.attachmentWritten;
    domain->available.store(usable(domain->tap, request.fftSize), std::memory_order_release);
    domain->consumers = 1;
    state_->domains.emplace(request, domain);
    ++state_->consumers;
    state_->changed.notify_all();
    return Lease(state_, std::move(domain));
}
void SharedAnalysis::refreshSources(bool forceEpoch) {
    state_->ownerOnly();
    std::lock_guard lock(state_->mutex);
    if (state_->closed) return;
    for (auto &[key, tap] : state_->taps) {
        auto next = state_->acquire ? state_->acquire(key.track, key.point) : nullptr;
        const bool replaced = next != tap.value;
        tap.value = std::move(next);
        tap.attached = tap.attached || tap.value != nullptr;
        if (replaced || forceEpoch)
            tap.attachmentWritten = tap.value ? tap.value->written() : 0;
        for (auto &[request, domain] : state_->domains) {
            if (request.track != key.track || request.point != key.point) continue;
            std::lock_guard domainLock(domain->mutex);
            if (!replaced && !forceEpoch) continue;
            domain->tap = tap.value;
            domain->available.store(usable(domain->tap, request.fftSize), std::memory_order_release);
            ++domain->sourceEpoch;
            domain->lastWritten = tap.attachmentWritten;
            domain->publishResult({});
        }
    }
    state_->changed.notify_all();
}
std::size_t SharedAnalysis::pump() { return state_->pump(); }
SharedAnalysis::Stats SharedAnalysis::stats() const {
    std::lock_guard lock(state_->mutex);
    return {state_->consumers, state_->domains.size(), state_->taps.size(),
            state_->transforms.load(std::memory_order_relaxed), state_->publications.load(std::memory_order_relaxed)};
}
} // namespace adi::ui
