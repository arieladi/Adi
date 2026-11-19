// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/param_edits.hpp"
#include "adi/gesture_navigation_detail.hpp"

#include <cmath>
#include <algorithm>
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

bool ParamEditCapture::push(const ParamEvent& event) noexcept { return push(event,nullptr); }
void ParamEditCapture::retainFence(std::shared_ptr<gesture::detail::ProducerFence> fence) {
    if(fence)fenceOwners_.push_back(std::move(fence));
}
bool ParamEditCapture::push(const ParamEvent& event,gesture::detail::ProducerFence* fence) noexcept {
    if(producerActive_.test_and_set(std::memory_order_acquire)) {
        if(fence)fence->captureFault();
        dropped_.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    struct ReleaseProducer {
        std::atomic_flag& flag;
        ~ReleaseProducer(){flag.clear(std::memory_order_release);}
    } release{producerActive_};
    auto reservation=gesture::detail::ProducerFence::Reservation::BeforeNavigation;
    if(fence) {
        reservation=fence->reserve();
        if(reservation==gesture::detail::ProducerFence::Reservation::RejectedClosed) {
            dropped_.fetch_add(1,std::memory_order_relaxed);
            return false;
        }
    }
    const auto w = write_.load(std::memory_order_relaxed);
    const auto next = w + 1 == ring_.size() ? 0 : w + 1;
    if (next == read_.load(std::memory_order_acquire)) {
        if(fence)fence->publicationFailed();
        dropped_.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    ring_[w] = {event,fence,reservation==gesture::detail::ProducerFence::Reservation::AfterNavigation};
    write_.store(next, std::memory_order_release);
    pushed_.fetch_add(1,std::memory_order_relaxed);
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
    p.echoes.clear();
}
void ParamEditCapture::expectAdditionalEcho(std::int64_t deviceId, std::int32_t paramIndex,
                                          double value, std::int64_t nowMs) {
    auto& p = parameters_[{deviceId,paramIndex}];
    if (p.echoes.empty() && p.guarded && elapsed(nowMs,p.armedMs) <= static_cast<std::uint64_t>(echoTtlMs_))
        p.echoes.push_back({p.echo,p.armedMs});
    std::erase_if(p.echoes,[&](const Parameter::Echo& echo) {
        return elapsed(nowMs,echo.armedMs) > static_cast<std::uint64_t>(echoTtlMs_);
    });
    constexpr std::size_t bound = 256;
    if (p.echoes.size() == bound) p.echoes.erase(p.echoes.begin());
    p.echoes.push_back({value,nowMs});
    p.guarded = true; p.echo = value; p.armedMs = nowMs;
}
void ParamEditCapture::cancelEcho(std::int64_t deviceId, std::int32_t paramIndex) {
    const auto found = parameters_.find({deviceId,paramIndex});
    if (found == parameters_.end()) return;
    found->second.guarded = false; found->second.echoes.clear();
}
void ParamEditCapture::cancel(std::int64_t deviceId, std::int32_t paramIndex) {
    const auto found = parameters_.find({deviceId,paramIndex});
    if (found == parameters_.end()) return;
    auto& p = found->second;
    p.open = false; p.hasValue = false; p.implicit = false;
    p.guarded = false; p.echoes.clear();
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

std::size_t ParamEditCapture::drainLifecycle(std::int64_t nowMs,std::vector<ParamEdit>& out,
                                             std::vector<ParamActivity>& activity,bool absorb) {
    return drainImpl(nowMs,out,absorb,&activity);
}
std::size_t ParamEditCapture::drainImpl(std::int64_t nowMs, std::vector<ParamEdit>& out,
                                        bool absorb,std::vector<ParamActivity>* activity) {
    const auto start = out.size();
    // Expire before processing queued values, even when the queue is empty.
    for (auto& [key, p] : parameters_) {
        if (!p.echoes.empty()) {
            std::erase_if(p.echoes,[&](const Parameter::Echo& echo) {
                return elapsed(nowMs,echo.armedMs) > static_cast<std::uint64_t>(echoTtlMs_);
            });
            if (p.echoes.empty() && p.guarded) {
                p.guarded = false;
                ++stats_.guardsExpired;
            }
        }
        if (p.guarded && elapsed(nowMs, p.armedMs) > static_cast<std::uint64_t>(echoTtlMs_)) {
            p.guarded = false;
            ++stats_.guardsExpired;
        }
    }
    // ADR-0154: this drain's last touch, and whether it was a gesture's.
    std::int32_t touched = -1;
    bool touchedExplicit = false;
    auto r = read_.load(std::memory_order_relaxed);
    // Snapshot the producer boundary: a busy producer cannot prolong this drain.
    const auto end = write_.load(std::memory_order_acquire);
    const auto pending=end>=r?end-r:ring_.size()-r+end;
    // The published prefix cannot be overwritten while read_ stays fixed.
    // Allocate every newly discovered parameter (including Debug iterator
    // proxies) before consuming even a known-key prefix.
    for(auto cursor=r;cursor!=end;cursor=cursor+1==ring_.size()?0:cursor+1) {
        const auto& event=ring_[cursor].event;
        parameters_.try_emplace({event.deviceId,event.paramIndex});
    }
    // Reserve before advancing any consumer state. Once processing begins,
    // publishing the corresponding owned observations cannot allocate.
    if(activity)activity->reserve(activity->size()+pending*2+parameters_.size());
    out.reserve(out.size()+pending+parameters_.size());
    while (r != end) {
        const auto slot=ring_[r];
        const auto event=slot.event;
        const Key key{event.deviceId, event.paramIndex};
        auto& p = parameters_.at(key);
        ParamActivity observed;
        observed.event=event;observed.fence=slot.fence;observed.afterNavigation=slot.afterNavigation;
        observed.before=p.known?p.last:event.value;observed.after=event.value;
        observed.implicit=p.implicit;
        switch (event.kind) {
        case ParamEventKind::Begin:
            observed.kind=ParamActivityKind::Begin;
            touched = event.paramIndex;
            touchedExplicit = true;
            if (p.open) {
                ++stats_.strayBegins;
            } else {
                p.open = true;
                p.implicit = false;
                p.hasValue = false;
            }
            break;
        case ParamEventKind::End:
            observed.kind=p.open?ParamActivityKind::End:ParamActivityKind::Ignored;
            observed.before=p.hasValue?p.before:observed.before;
            observed.after=p.hasValue?p.after:observed.before;
            if (p.open) close(key, p, out);
            else ++stats_.strayEnds;
            break;
        case ParamEventKind::Value:
            observed.kind=ParamActivityKind::Value;
            if (p.guarded && !p.open) {
                auto expected = std::find_if(p.echoes.begin(),p.echoes.end(),[&](const Parameter::Echo& echo) {
                    return std::abs(event.value-echo.value) <= echoTolerance_;
                });
                const bool matched = p.echoes.empty() ? std::abs(event.value-p.echo) <= echoTolerance_
                                                      : expected != p.echoes.end();
                if (matched) {
                    p.last = p.echoes.empty() ? p.echo : expected->value;
                    p.known = true;
                    if (!p.echoes.empty()) p.echoes.erase(expected);
                    p.guarded = !p.echoes.empty();
                    ++stats_.echoesSwallowed;
                    observed.kind=ParamActivityKind::Echo;
                    break;
                }
            }
            touched = event.paramIndex;
            touchedExplicit = p.open && !p.implicit;
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
            observed.before=p.before;observed.after=p.after;observed.implicit=p.implicit;
            break;
        }
        if(activity)activity->push_back(std::move(observed));
        r = r + 1 == ring_.size() ? 0 : r + 1;
        read_.store(r, std::memory_order_release);
    }
    // A preset's broadcasts are the plug-in moving itself, not the user
    // touching a knob: in an absorbing drain only a gesture counts.
    if (touched >= 0 && (!absorb || touchedExplicit)) lastTouched_ = touched;
    for (auto& [key, p] : parameters_) {
        if (!p.open || !p.implicit) continue;
        if (absorb) {
            if(activity)activity->push_back({ParamActivityKind::Absorbed,{key.first,key.second,ParamEventKind::End,0},p.before,p.after,true,false,nullptr});
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
            if(activity)activity->push_back({ParamActivityKind::QuietEnd,{key.first,key.second,ParamEventKind::End,0},p.before,p.after,true,false,nullptr});
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
