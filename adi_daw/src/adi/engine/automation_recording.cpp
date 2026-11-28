// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/automation_recording.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace adi::engine {
AutomationRecorder::AutomationRecorder(AutomationTarget target, std::int32_t capacity,
                                       std::size_t maxPoints, AutomationCaptureMode mode)
    : target_(target), maxPoints_(maxPoints), mode_(mode) {
    if(target.data<=0 || target.lane<=0 || (target.clip && *target.clip<=0) || capacity<0 ||
        maxPoints==0 || maxPoints>(1u<<23))throw std::invalid_argument("invalid automation recorder bounds or target");
    if (mode != AutomationCaptureMode::Touch && mode != AutomationCaptureMode::Latch)
        throw std::invalid_argument("invalid automation capture mode");
    ring_.resize(static_cast<std::size_t>(capacity)+1);
}
void AutomationRecorder::invalidate() noexcept {
    if(open_){tainted_=true;droppedGesture_.store(serial_,std::memory_order_release);}
    dropped_.store(dropped_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
}
bool AutomationRecorder::offer(AutomationGesture kind, std::int64_t time, double value, bool hold) noexcept {
    const auto write=write_.load(std::memory_order_relaxed);const auto next=(write+1)%ring_.size();
    if(next==read_.load(std::memory_order_acquire)){invalidate();return false;}
    ring_[write]={kind,serial_,time,value,tainted_,hold};write_.store(next,std::memory_order_release);
    accepted_.store(accepted_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);return true;
}
bool AutomationRecorder::start(std::int64_t time, double value, bool hold) noexcept {
    if (serial_ == UINT64_MAX) {
        invalidate();
        return false;
    }
    ++serial_;
    open_ = true;
    tainted_ = false;
    held_ = hold;
    lastTime_ = time;
    lastValue_ = value;
    return offer(AutomationGesture::Begin, time, value, hold);
}
bool AutomationRecorder::finish(std::int64_t time,double value) noexcept {
    const bool valid=time>=lastTime_ && std::isfinite(value);
    if(!valid)invalidate();
    const bool queued=offer(AutomationGesture::End,valid?time:lastTime_,valid?value:lastValue_);
    open_=false;return valid && queued;
}
bool AutomationRecorder::setArmed(bool on,std::int64_t time) noexcept {
    if(time<0)return false;
    bool ok=true;
    if(!on && open_)ok=finish(std::max(time,lastTime_),lastValue_);
    armed_.store(on,std::memory_order_relaxed);return ok;
}
bool AutomationRecorder::setRecording(bool on,std::int64_t time) noexcept {
    if(time<0)return false;
    bool ok=true;
    if(!on && open_)ok=finish(std::max(time,lastTime_),lastValue_);
    recording_.store(on,std::memory_order_relaxed);return ok;
}
bool AutomationRecorder::splitAtTransportBoundary(std::int64_t oldTime, std::int64_t newTime) noexcept {
    if (oldTime < 0 || newTime < 0 || (open_ && oldTime < lastTime_)) {
        if (open_)
            invalidate();
        return false;
    }
    if (!open_)
        return true;
    const auto value = lastValue_;
    return finish(oldTime, value) && start(newTime, value, held_);
}
bool AutomationRecorder::push(AutomationGesture kind,std::int64_t time,double value) noexcept {
    if(!armed() || !recording()) {
        ignored_.store(ignored_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);return false;
    }
    if(time<0 || !std::isfinite(value) || (kind!=AutomationGesture::Begin &&
        kind!=AutomationGesture::Value && kind!=AutomationGesture::End)) {invalidate();return false;}
    const bool latch = mode_ == AutomationCaptureMode::Latch;
    const bool retouch = latch && open_ && !tainted_;
    if ((kind == AutomationGesture::Begin && !retouch) ||
        (!open_ && kind == AutomationGesture::Value)) {
        if(open_)(void)finish(lastTime_,lastValue_);
        return start(time, value);
    }
    if(!open_) {
        ignored_.store(ignored_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);return false;
    }
    if (kind == AutomationGesture::End && !latch)
        return finish(time, value);
    if(time<lastTime_){invalidate();return false;}
    lastTime_=time;lastValue_=value;
    held_ = kind == AutomationGesture::End;
    return offer(AutomationGesture::Value, time, value, held_);
}
void AutomationRecorder::discard() noexcept {
    if(pending_.gesture)++discarded_;
    pending_.points.clear();pending_.gesture=0;bad_=false;
}
std::size_t AutomationRecorder::drain(std::vector<RecordedAutomation>& out) {
    const auto before=out.size();auto read=read_.load(std::memory_order_relaxed);
    const auto end=write_.load(std::memory_order_acquire);
    while(read!=end) {
        const auto event=ring_[read];read=(read+1)%ring_.size();read_.store(read,std::memory_order_release);
        if(pending_.gesture!=event.gesture) {
            discard();pending_.gesture=event.gesture;pending_.target=target_;
            bad_=event.kind!=AutomationGesture::Begin;
        }
        bad_=bad_ || event.tainted;
        if(!bad_) {
            AutomationPoint point{};point.time=event.time;point.value=event.value;
            point.curve = static_cast<std::uint8_t>(event.hold ? Curve::Hold : Curve::Linear);
            if(!pending_.points.empty() && pending_.points.back().time==point.time)pending_.points.back()=point;
            else if(pending_.points.size()<maxPoints_)pending_.points.push_back(point);
            else bad_=true;
        }
        if(event.kind==AutomationGesture::End) {
            if(!bad_ && !pending_.points.empty()) {
                out.push_back(std::move(pending_));pending_={};bad_=false;
            } else discard();
        }
    }
    if(pending_.gesture && pending_.gesture==droppedGesture_.load(std::memory_order_acquire))discard();
    return out.size()-before;
}
AutomationRecorder::Stats AutomationRecorder::stats() const noexcept {
    return {accepted_.load(std::memory_order_relaxed),dropped_.load(std::memory_order_relaxed),
        ignored_.load(std::memory_order_relaxed),discarded_};
}
std::optional<automation::PreparedShape> prepareRecordedAutomation(
    Store &store, const RecordedAutomation &recorded, std::string &error,
    std::optional<automation::AutomationThinning> thinning) {
    if(!recorded.gesture){error="automation: missing recorded gesture identity";return std::nullopt;}
    return automation::prepareAutomationWrite(store, recorded.target.data, recorded.target.lane,
                                              recorded.target.clip, recorded.points, error, thinning);
}
} // namespace adi::engine
