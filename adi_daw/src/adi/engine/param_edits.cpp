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

void ParamEditCapture::track(const Key& key, Parameter& parameter) {
    if (parameter.tracked) return;
    const bool ordered = maintenance_.empty() || maintenance_.back().key < key;
    maintenance_.push_back({key, &parameter});
    maintenanceOrdered_ = maintenanceOrdered_ && ordered;
    parameter.tracked = true;
}

void ParamEditCapture::seed(std::int64_t deviceId, std::int32_t paramIndex, double value) {
    auto& p = parameters_[{deviceId, paramIndex}];
    p.last = value;
    p.known = true;
}
void ParamEditCapture::expectEcho(std::int64_t deviceId, std::int32_t paramIndex,
                                double value, std::int64_t nowMs) {
    const Key key{deviceId, paramIndex};
    auto& p = parameters_[key];
    track(key, p);
    p.guarded = true;
    p.echo = value;
    p.armedMs = nowMs;
    p.echoes.clear();
}
void ParamEditCapture::expectAdditionalEcho(std::int64_t deviceId, std::int32_t paramIndex,
                                          double value, std::int64_t nowMs) {
    const Key key{deviceId, paramIndex};
    auto& p = parameters_[key];
    track(key, p);
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
void ParamEditCapture::discardGesture(std::int64_t deviceId, std::int32_t paramIndex) {
    const auto found = parameters_.find({deviceId,paramIndex});
    if (found == parameters_.end()) return;
    auto& p = found->second;
    p.open = false; p.hasValue = false; p.implicit = false;
}
void ParamEditCapture::cancel(std::int64_t deviceId, std::int32_t paramIndex) {
    discardGesture(deviceId, paramIndex);
    cancelEcho(deviceId, paramIndex);
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
                                             std::vector<ParamActivity>& activity,bool absorb,bool finishOpen) {
    return drainImpl(nowMs,out,absorb,&activity,finishOpen);
}
void ParamEditCapture::clearLastTouch() noexcept {
    touches_.clear();
    const auto read = read_.load(std::memory_order_relaxed);
    const auto write = write_.load(std::memory_order_acquire);
    priorAttachmentEvents_ = write >= read ? write - read : ring_.size() - read + write;
}
std::size_t ParamEditCapture::drainImpl(std::int64_t nowMs, std::vector<ParamEdit>& out,
                                        bool absorb,std::vector<ParamActivity>* activity,bool finishOpen) {
    const auto start = out.size();
    // Expire before processing queued values, even when the queue is empty.
    for (const auto& entry : maintenance_) {
        auto& p = *entry.parameter;
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
    // ADR-0154: only explicit gestures contribute touches during absorption.
    const auto touch = [&](std::int32_t index) {
        if (priorAttachmentEvents_ != 0 || index < 0) return;
        const ParameterTouch observation{index, ++touchSequence_};
        if (!touches_.empty() && touches_.back().index == index)
            touches_.back() = observation;
        else
            touches_.push_back(observation);
    };
    const auto activityStart = activity ? activity->size() : 0;
    bool promotedActivity = false;
    auto r = read_.load(std::memory_order_relaxed);
    // Snapshot the producer boundary: a busy producer cannot prolong this drain.
    const auto end = write_.load(std::memory_order_acquire);
    const auto pending=end>=r?end-r:ring_.size()-r+end;
    if (pending == 0 && maintenance_.empty()) return 0;
    // The published prefix cannot be overwritten while read_ stays fixed.
    // Allocate every newly discovered parameter (including Debug iterator
    // proxies) before consuming even a known-key prefix.
    for(auto cursor=r;cursor!=end;cursor=cursor+1==ring_.size()?0:cursor+1) {
        const auto& event=ring_[cursor].event;
        parameters_.try_emplace({event.deviceId,event.paramIndex});
    }
    // Reserve before advancing any consumer state. Once processing begins,
    // publishing the corresponding owned observations cannot allocate.
    const auto tracked = maintenance_.size();
    const auto needed = tracked + pending;
    if (needed > maintenance_.capacity()) {
        const auto growth = std::min(maintenance_.max_size(), maintenance_.capacity() * 2);
        maintenance_.reserve(std::max(needed, growth));
    }
    // A new implicit completion needs a Value in this prefix. It and the
    // already tracked gestures bound the extra synthetic observations.
    if(activity)activity->reserve(activity->size()+pending*2+tracked);
    out.reserve(out.size()+pending+tracked);
    const auto touchLimit = std::max(ring_.size() - 1, std::size_t{128});
    // Room for one whole producer prefix lets us trim the old tail once
    // after the drain rather than shifting history for each reported touch.
    if (pending != 0 && touches_.capacity() == 0) touches_.reserve(touchLimit * 2);
    while (r != end) {
        const auto slot=ring_[r];
        const auto event=slot.event;
        const Key key{event.deviceId, event.paramIndex};
        auto& p = parameters_.at(key);
        p.promoteEarlierActivity = false;
        ParamActivity observed;
        observed.event=event;observed.fence=slot.fence;observed.afterNavigation=slot.afterNavigation;
        observed.before=p.known?p.last:event.value;observed.after=event.value;
        observed.implicit=p.implicit;
        switch (event.kind) {
        case ParamEventKind::Begin: {
            observed.kind=ParamActivityKind::Begin;
            touch(event.paramIndex);
            const bool promote=p.open && p.implicit;
            observed.implicit=promote && absorb && activity;
            promotedActivity=promotedActivity || observed.implicit;
            if (promote) {
                p.implicit = false;
                observed.before=p.before;
                observed.after=p.after;
            } else if (p.open) {
                ++stats_.strayBegins;
            } else {
                track(key, p);
                p.open = true;
                p.implicit = false;
                p.hasValue = false;
            }
            break;
        }
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
            if (!absorb || (p.open && !p.implicit)) touch(event.paramIndex);
            if (!p.open) {
                track(key, p);
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
        if (priorAttachmentEvents_ != 0) --priorAttachmentEvents_;
        r = r + 1 == ring_.size() ? 0 : r + 1;
        read_.store(r, std::memory_order_release);
    }
    if (touches_.size() > touchLimit)
        touches_.erase(touches_.begin(), touches_.begin() + static_cast<std::ptrdiff_t>(touches_.size() - touchLimit));
    if (activity && promotedActivity) {
        // ParamOps gives implicit observations to the state snapshot. A
        // promoted gesture owns its earlier values in the same prefix, back
        // to the preceding End for this parameter.
        for (auto i=activity->size();i>activityStart;--i) {
            auto& observation=(*activity)[i-1];
            auto& p=parameters_.at({observation.event.deviceId,observation.event.paramIndex});
            if (observation.kind==ParamActivityKind::Begin) {
                p.promoteEarlierActivity=observation.implicit;
                observation.implicit=false;
            } else if (observation.kind==ParamActivityKind::End) {
                p.promoteEarlierActivity=false;
            } else if (observation.kind==ParamActivityKind::Value && p.promoteEarlierActivity) {
                observation.implicit=false;
            }
        }
    }
    if (!maintenanceOrdered_) {
        std::sort(maintenance_.begin(), maintenance_.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        maintenanceOrdered_ = true;
    }
    auto retained = maintenance_.begin();
    for (const auto& entry : maintenance_) {
        const auto& key = entry.key;
        auto& p = *entry.parameter;
        if (p.open) {
            if (absorb && p.implicit) {
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
            } else if (finishOpen || (p.implicit && elapsed(nowMs, p.lastValueMs) >= static_cast<std::uint64_t>(quietMs_))) {
                if (activity) {
                    const auto before = p.hasValue ? p.before : p.last;
                    const auto after = p.hasValue ? p.after : p.last;
                    activity->push_back({ParamActivityKind::QuietEnd,
                        {key.first,key.second,ParamEventKind::End,0},before,after,p.implicit,false,nullptr});
                }
                close(key, p, out);
            }
        }
        p.tracked = p.guarded || !p.echoes.empty() || p.open;
        if (p.tracked) *retained++ = entry;
    }
    maintenance_.erase(retained, maintenance_.end());
    return out.size() - start;
}

const ParamEditCapture::Stats& ParamEditCapture::stats() const noexcept {
    stats_.pushed = pushed_.load(std::memory_order_relaxed);
    stats_.dropped = dropped_.load(std::memory_order_relaxed);
    return stats_;
}
} // namespace adi::engine
