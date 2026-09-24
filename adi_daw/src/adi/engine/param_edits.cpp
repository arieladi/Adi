// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/param_edits.hpp"

#include <cmath>
#include <stdexcept>

namespace adi::engine {
namespace {
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<std::int64_t>::is_always_lock_free);

std::size_t slots(std::int32_t capacity) {
    if (capacity < 0) throw std::invalid_argument("negative parameter ring capacity");
    return static_cast<std::size_t>(capacity) + 1;
}
// Unsigned subtraction avoids signed overflow even across the full clock range.
std::uint64_t elapsed(std::int64_t now, std::int64_t then) noexcept {
    return now >= then ? static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(then) : 0;
}
} // namespace

ParamEditCapture::ParamEditCapture(std::int32_t capacity) : ring_(slots(capacity)) {}

bool ParamEditCapture::push(const ParamEvent& event) noexcept {
    const auto w = write_.load(std::memory_order_relaxed);
    const auto next = w + 1 == ring_.size() ? 0 : w + 1;
    if (next == read_.load(std::memory_order_acquire)) {
        dropped_.store(dropped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return false;
    }
    ring_[w] = event;
    write_.store(next, std::memory_order_release);
    pushed_.store(pushed_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return true;
}

void ParamEditCapture::seed(std::int64_t deviceId, std::int32_t paramIndex, double value) {
    auto& p = parameters_[{deviceId, paramIndex}];
    p.last = value;
    p.known = true;
}
void ParamEditCapture::expectEcho(std::int64_t deviceId, std::int32_t paramIndex,
                                double value, std::int64_t nowMs) {
    auto& p = parameters_[{deviceId, paramIndex}];
    p.guarded = true;
    p.echo = value;
    p.armedMs = nowMs;
}
void ParamEditCapture::setQuietMs(std::int64_t ms) {
    if (ms < 0) throw std::invalid_argument("negative quiet interval");
    quietMs_ = ms;
}
void ParamEditCapture::setEchoTtlMs(std::int64_t ms) {
    if (ms < 0) throw std::invalid_argument("negative echo lifetime");
    echoTtlMs_ = ms;
}
void ParamEditCapture::setEchoTolerance(double tol) {
    if (!std::isfinite(tol) || tol < 0.0) throw std::invalid_argument("invalid echo tolerance");
    echoTolerance_ = tol;
}

void ParamEditCapture::close(const Key& key, Parameter& p, std::vector<ParamEdit>& out) {
    if (p.hasValue) {
        out.push_back({key.first, key.second, p.before, p.after, p.implicit});
        p.last = p.after;
        p.known = true;
        ++stats_.edits;
        if (p.implicit) ++stats_.implicitEdits;
    }
    p.open = false;
    p.hasValue = false;
}

std::size_t ParamEditCapture::drain(std::int64_t nowMs, std::vector<ParamEdit>& out) {
    return drainImpl(nowMs, out, false);
}

std::size_t ParamEditCapture::drainAbsorbing(std::int64_t nowMs, std::vector<ParamEdit>& out) {
    return drainImpl(nowMs, out, true);
}

std::size_t ParamEditCapture::drainImpl(std::int64_t nowMs, std::vector<ParamEdit>& out,
                                        bool absorb) {
    const auto start = out.size();
    // Expire before processing queued values, even when the queue is empty.
    for (auto& [key, p] : parameters_) {
        if (p.guarded && elapsed(nowMs, p.armedMs) > static_cast<std::uint64_t>(echoTtlMs_)) {
            p.guarded = false;
            ++stats_.guardsExpired;
        }
    }
    auto r = read_.load(std::memory_order_relaxed);
    // Snapshot the producer boundary: a busy producer cannot prolong this drain.
    const auto end = write_.load(std::memory_order_acquire);
    while (r != end) {
        const auto event = ring_[r];
        const Key key{event.deviceId, event.paramIndex};
        auto& p = parameters_[key];
        switch (event.kind) {
        case ParamEventKind::Begin:
            if (p.open) {
                ++stats_.strayBegins;
            } else {
                p.open = true;
                p.implicit = false;
                p.hasValue = false;
            }
            break;
        case ParamEventKind::End:
            if (p.open) close(key, p, out);
            else ++stats_.strayEnds;
            break;
        case ParamEventKind::Value:
            if (p.guarded && !p.open && std::abs(event.value - p.echo) <= echoTolerance_) {
                p.last = p.echo;
                p.known = true;
                p.guarded = false;
                ++stats_.echoesSwallowed;
                break;
            }
            if (!p.open) {
                p.open = true;
                p.implicit = true;
                p.hasValue = false;
            }
            if (!p.hasValue) {
                p.before = p.known ? p.last : event.value;
                if (!p.known) ++stats_.unseeded;
            }
            p.after = event.value;
            p.hasValue = true;
            p.lastValueMs = nowMs;
            break;
        }
        r = r + 1 == ring_.size() ? 0 : r + 1;
        read_.store(r, std::memory_order_release);
    }
    for (auto& [key, p] : parameters_) {
        if (!p.open || !p.implicit) continue;
        if (absorb) {
            // Folded into the snapshot: the value is where the plugin put it,
            // and no edit says so.
            if (p.hasValue) {
                p.last = p.after;
                p.known = true;
                ++stats_.absorbed;
            }
            p.open = false;
            p.hasValue = false;
        } else if (elapsed(nowMs, p.lastValueMs) >= static_cast<std::uint64_t>(quietMs_)) {
            close(key, p, out);
        }
    }
    return out.size() - start;
}

const ParamEditCapture::Stats& ParamEditCapture::stats() const noexcept {
    stats_.pushed = pushed_.load(std::memory_order_relaxed);
    stats_.dropped = dropped_.load(std::memory_order_relaxed);
    return stats_;
}
} // namespace adi::engine
