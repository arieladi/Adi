// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/scope.hpp"

#include <algorithm>
#include <cmath>

namespace adi::engine {

ScopeTap::ScopeTap(double sampleRate, double seconds)
    : sampleRate_(sampleRate > 0.0 ? sampleRate : 48000.0),
      capacity_(std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(seconds * sampleRate_)))),
      l_(std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(capacity_))),
      r_(std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(capacity_))),
      stamp_(std::make_unique<std::atomic<std::int64_t>[]>(static_cast<std::size_t>(capacity_))) {}

void ScopeTap::write(const float* l, const float* r, std::int32_t frames, std::int64_t stamp,
                     bool advancing) noexcept {
    const std::int64_t w = written_.load(std::memory_order_relaxed);
    // A seqlock's order: say how far this write will reach BEFORE touching the
    // ring, so a reader that copies during it sees that it may have been lapped.
    pending_.store(w + frames, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::int32_t i = 0; i < frames; ++i) {
        const auto at = static_cast<std::size_t>((w + i) % capacity_);
        l_[at].store(l[i], std::memory_order_relaxed);
        r_[at].store(r != nullptr ? r[i] : l[i], std::memory_order_relaxed);
        stamp_[at].store(advancing ? stamp + i : stamp, std::memory_order_relaxed);
    }
    written_.store(w + frames, std::memory_order_release);
}

bool ScopeTap::read(std::span<float> l, std::span<float> r, std::int64_t& firstStamp) const {
    const auto n = static_cast<std::int64_t>(std::min(l.size(), r.size()));
    if (n == 0 || n > capacity_) return false;
    const std::int64_t end = written_.load(std::memory_order_acquire);
    if (end < n) return false;
    const std::int64_t start = end - n;
    firstStamp = stamp_[static_cast<std::size_t>(start % capacity_)].load(std::memory_order_relaxed);
    for (std::int64_t i = 0; i < n; ++i) {
        const auto at = static_cast<std::size_t>((start + i) % capacity_);
        l[static_cast<std::size_t>(i)] = l_[at].load(std::memory_order_relaxed);
        r[static_cast<std::size_t>(i)] = r_[at].load(std::memory_order_relaxed);
    }
    // Lapped while copying? `pending_` counts the write in progress too, so a
    // slot being overwritten right now is caught, not only finished writes.
    std::atomic_thread_fence(std::memory_order_acquire);
    return pending_.load(std::memory_order_relaxed) - start <= capacity_;
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
