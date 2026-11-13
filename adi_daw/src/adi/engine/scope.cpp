// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/scope.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace adi::engine {

namespace {
std::atomic<std::uint64_t> nextTapGeneration{0};
double tapRate(double value) { return std::isfinite(value) && value > 0 ? value : 48000.0; }
std::int64_t tapFrames(double rate, double seconds) {
    const double frames = std::ceil(rate * seconds);
    if (!std::isfinite(seconds) || seconds <= 0 || !std::isfinite(frames) || frames > (1 << 24))
        throw std::invalid_argument("scope history exceeds its finite prepared capacity");
    return (std::max)(std::int64_t{1}, static_cast<std::int64_t>(frames));
}
}
ScopeTap::ScopeTap(double sampleRate, double seconds)
    : sampleRate_(tapRate(sampleRate)), capacity_(tapFrames(sampleRate_, seconds)),
      generation_(nextTapGeneration.fetch_add(1, std::memory_order_relaxed) + 1),
      l_(std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(capacity_))),
      r_(std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(capacity_))),
      spanCapacity_(std::clamp(capacity_ / 32 + 1, std::int64_t{64}, std::int64_t{65536})),
      spans_(std::make_unique<StampSpan[]>(static_cast<std::size_t>(spanCapacity_))) {}

void ScopeTap::write(const float* l, const float* r, std::int32_t frames, std::int64_t stamp,
                     bool advancing) noexcept { store(l, r, frames, stamp, advancing); }
void ScopeTap::write(const double* l, const double* r, std::int32_t frames, std::int64_t stamp,
                     bool advancing) noexcept { store(l, r, frames, stamp, advancing); }

template <typename T>
void ScopeTap::store(const T* l, const T* r, std::int32_t frames, std::int64_t stamp,
                     bool advancing) noexcept {
    if (frames <= 0) return;
    const std::int64_t w = written_.load(std::memory_order_relaxed);
    pending_.store(w + frames, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::int32_t i = 0; i < frames; ++i) {
        const auto at = static_cast<std::size_t>((w + i) % capacity_);
        const float left = l != nullptr ? static_cast<float>(l[i]) : 0.0f;
        l_[at].store(left, std::memory_order_relaxed);
        r_[at].store(r != nullptr ? static_cast<float>(r[i]) : left, std::memory_order_relaxed);
    }
    // Timestamp records describe contiguous heard time, not UI frames or event segments.
    // One-sample segments with linear stamps extend one record instead of exhausting a side ring.
    const bool contiguous = lastSpan_ >= 0 && lastEnd_ == w && lastAdvancing_ == advancing &&
        stamp == lastStamp_ + (advancing ? w - lastBegin_ : 0);
    if (!contiguous) {
        ++lastSpan_;
        lastBegin_ = w; lastStamp_ = stamp; lastAdvancing_ = advancing;
        auto& span = spans_[static_cast<std::size_t>(lastSpan_ % spanCapacity_)];
        const auto version = span.version.load();
        span.version.store(version + 1);
        span.begin.store(w); span.end.store(w + frames); span.stamp.store(stamp); span.advancing.store(advancing);
        span.version.store(version + 2);
        spanCount_.store(lastSpan_ + 1, std::memory_order_release);
    } else {
        spans_[static_cast<std::size_t>(lastSpan_ % spanCapacity_)].end.store(w + frames);
    }
    lastEnd_ = w + frames;
    written_.store(w + frames, std::memory_order_release);
}

bool ScopeTap::loadSpan(std::int64_t record, Stamp& result) const noexcept {
    const auto& span = spans_[static_cast<std::size_t>(record % spanCapacity_)];
    // All descriptor atomics share SC order. A stable even version encloses one coherent
    // descriptor, including while its slot is overwritten; audio never retries or waits.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto before = span.version.load();
        if (!before || (before & 1U)) continue;
        Stamp value{span.begin.load(), span.end.load(), span.stamp.load(), span.advancing.load()};
        if (span.version.load() == before) { result = value; return true; }
    }
    return false;
}
bool ScopeTap::spanAt(std::int64_t sequence, Stamp& result) const noexcept {
    const auto end = spanCount_.load(std::memory_order_acquire);
    const auto begin = (std::max)(std::int64_t{0}, end - spanCapacity_);
    for (auto record = end; record > begin;) {
        Stamp span;
        if (!loadSpan(--record, span)) continue;
        if (span.begin <= sequence && sequence < span.end) { result = span; return true; }
        if (span.end <= sequence) break;
    }
    return false;
}
std::int64_t ScopeTap::oldest() const noexcept {
    const auto end = written();
    auto first = (std::max)(std::int64_t{0}, end - capacity_);
    const auto count = spanCount_.load(std::memory_order_acquire);
    if (count > 0) {
        Stamp span;
        if (loadSpan((std::max)(std::int64_t{0}, count - spanCapacity_), span))
            first = (std::max)(first, span.begin);
    }
    return first;
}
ScopeTap::ReadResult ScopeTap::read(Cursor& cursor, std::span<float> l, std::span<float> r) const noexcept {
    ReadResult result;
    if (l.empty() || r.empty()) { result.status = ReadStatus::Invalid; return result; }
    if (cursor.generation != generation_) {
        cursor = cursorAtOldest(); result.status = ReadStatus::GenerationChanged; return result;
    }
    const auto end = written();
    const auto first = oldest();
    if (cursor.next < first || cursor.next < 0) {
        cursor = {generation_, first}; result.status = ReadStatus::Overrun; return result;
    }
    if (cursor.next >= end) return result;
    Stamp span;
    if (!spanAt(cursor.next, span)) {
        cursor = cursorAtOldest(); result.status = ReadStatus::Overrun; return result;
    }
    const auto count = (std::min)({static_cast<std::int64_t>((std::min)(l.size(), r.size())),
                                   end - cursor.next, span.end - cursor.next});
    if (count <= 0) return result;
    const auto start = cursor.next;
    const auto stamp = span.stamp + (span.advancing ? start - span.begin : 0);
    for (std::int64_t i = 0; i < count; ++i) {
        const auto at = static_cast<std::size_t>((start + i) % capacity_);
        l[static_cast<std::size_t>(i)] = l_[at].load(std::memory_order_relaxed);
        r[static_cast<std::size_t>(i)] = r_[at].load(std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (pending_.load(std::memory_order_relaxed) - start > capacity_) {
        cursor = cursorAtOldest(); result.status = ReadStatus::Overrun; return result;
    }
    result.status = cursor.hasExpected && (cursor.expectedStamp != stamp || cursor.expectedAdvancing != span.advancing)
        ? ReadStatus::Discontinuity : ReadStatus::Ready;
    result.frames = static_cast<std::int32_t>(count);
    result.firstStamp = stamp; result.firstSequence = start; result.advancing = span.advancing;
    cursor.next += count; cursor.expectedStamp = stamp + (span.advancing ? count : 0);
    cursor.expectedAdvancing = span.advancing; cursor.hasExpected = true;
    return result;
}
bool ScopeTap::read(std::span<float> l, std::span<float> r, std::int64_t& firstStamp) const {
    const auto count = static_cast<std::int64_t>((std::min)(l.size(), r.size()));
    const auto end = written();
    if (count <= 0 || count > capacity_ || end < count) return false;
    Cursor cursor{generation_, end - count};
    const auto result = read(cursor, l, r);
    if (result.status != ReadStatus::Ready || result.frames != count) return false;
    firstStamp = result.firstStamp;
    return true;
}

double correlation(std::span<const float> a, std::span<const float> b) noexcept {
    const std::size_t n = std::min(a.size(), b.size());
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    return (aa > 0.0 && bb > 0.0) ? ab / std::sqrt(aa * bb) : 0.0;
}

Offset bestOffset(std::span<const float> a, std::span<const float> b, std::int32_t maxLag) noexcept {
    const auto n = static_cast<std::int64_t>(std::min(a.size(), b.size()));
    Offset best;
    best.correlation = -2.0;
    for (std::int32_t lag = -maxLag; lag <= maxLag; ++lag) {
        // b late by `lag`: a[i] against b[i + lag].
        const std::int64_t from = std::max<std::int64_t>(0, -lag);
        const std::int64_t to = std::min<std::int64_t>(n, n - lag);
        if (to - from < 16) continue;
        const auto len = static_cast<std::size_t>(to - from);
        const double c = correlation(a.subspan(static_cast<std::size_t>(from), len),
                                     b.subspan(static_cast<std::size_t>(from + lag), len));
        if (c > best.correlation) {
            best.correlation = c;
            best.samples = lag;
        }
    }
    if (best.correlation < -1.0) best.correlation = 0.0;
    return best;
}

}  // namespace adi::engine
