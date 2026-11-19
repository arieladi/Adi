// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/automation_recording.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace adi::engine {
AutomationRecorder::AutomationRecorder(AutomationTarget target,std::int32_t capacity,std::size_t maxPoints)
    : target_(target),maxPoints_(maxPoints) {
    if(target.data<=0 || target.lane<=0 || (target.clip && *target.clip<=0) || capacity<0 ||
        maxPoints==0 || maxPoints>(1u<<23))throw std::invalid_argument("invalid automation recorder bounds or target");
    ring_.resize(static_cast<std::size_t>(capacity)+1);
}
void AutomationRecorder::invalidate() noexcept {
    if(open_){tainted_=true;droppedGesture_.store(serial_,std::memory_order_release);}
    dropped_.store(dropped_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
}
bool AutomationRecorder::offer(AutomationGesture kind,std::int64_t time,double value) noexcept {
    const auto write=write_.load(std::memory_order_relaxed);const auto next=(write+1)%ring_.size();
    if(next==read_.load(std::memory_order_acquire)){invalidate();return false;}
    ring_[write]={kind,serial_,time,value,tainted_};write_.store(next,std::memory_order_release);
    accepted_.store(accepted_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);return true;
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
bool AutomationRecorder::push(AutomationGesture kind,std::int64_t time,double value) noexcept {
    if(!armed() || !recording()) {
        ignored_.store(ignored_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);return false;
    }
    if(time<0 || !std::isfinite(value) || (kind!=AutomationGesture::Begin &&
        kind!=AutomationGesture::Value && kind!=AutomationGesture::End)) {invalidate();return false;}
    if(kind==AutomationGesture::Begin || (!open_ && kind==AutomationGesture::Value)) {
        if(open_)(void)finish(lastTime_,lastValue_);
        if(serial_==UINT64_MAX){invalidate();return false;}
        ++serial_;open_=true;tainted_=false;lastTime_=time;lastValue_=value;
        return offer(AutomationGesture::Begin,time,value);
    }
    if(!open_) {
        ignored_.store(ignored_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);return false;
    }
    if(kind==AutomationGesture::End)return finish(time,value);
    if(time<lastTime_){invalidate();return false;}
    lastTime_=time;lastValue_=value;return offer(kind,time,value);
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
            AutomationPoint point{};point.time=event.time;point.value=event.value;point.curve=1;
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
std::optional<automation::PreparedShape> prepareRecordedAutomation(Store& store,const RecordedAutomation& recorded,std::string& error) {
    if(!recorded.gesture){error="automation: missing recorded gesture identity";return std::nullopt;}
    return automation::prepareAutomationWrite(store,recorded.target.data,recorded.target.lane,recorded.target.clip,recorded.points,error);
}
} // namespace adi::engine
