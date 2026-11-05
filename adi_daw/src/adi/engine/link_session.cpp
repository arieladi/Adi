// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/link_session.hpp"
#include "adi/audio/io_audit.hpp"
#include <ableton/Link.hpp>
#include <atomic>
#include <cmath>
#include <utility>
namespace adi::engine {
namespace {
bool validTempo(double tempo)noexcept{return std::isfinite(tempo) && tempo>=20 && tempo<=999;}
bool valid(const LinkBuffer& buffer,const LinkCommand& command)noexcept {
    constexpr std::int64_t limit=std::int64_t{1}<<50;
    if(buffer.callbackMicros < -limit || buffer.callbackMicros>limit || buffer.outputLatencyMicros<0 || buffer.outputLatencyMicros>10000000 ||
       !std::isfinite(buffer.quantum) || buffer.quantum<1.0/1024 || buffer.quantum>256 ||
       (command.tempo!=0 && !validTempo(command.tempo)) ||
       (command.requestBeat && (!std::isfinite(command.beat) || std::abs(command.beat)>1e9)))return false;
    switch(command.transport){case LinkTransport::Keep:case LinkTransport::Play:case LinkTransport::Stop:return true;}
    return false;
}
}
struct LinkSession::Impl {
    explicit Impl(double tempo):sdk(tempo){}
    ableton::Link sdk;
    std::atomic<bool> startStopSync{false};
};
static_assert(std::atomic<bool>::is_always_lock_free);
LinkSession::LinkSession(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
LinkSession::~LinkSession()=default;
std::unique_ptr<LinkSession> LinkSession::create(double initialTempo,std::string& error) {
    if(audio::onAudioThread)return {};
    if(!validTempo(initialTempo)){error="Link tempo must be finite and within 20..999 BPM";return {};}
    try {auto impl=std::make_unique<Impl>(initialTempo);auto result=std::unique_ptr<LinkSession>(new LinkSession(std::move(impl)));error.clear();return result;}
    catch(const std::exception& e){error=e.what();return {};}
}
bool LinkSession::enable(bool enabled) {
    if(audio::onAudioThread)return false;
    impl_->sdk.enable(enabled);return true;
}
bool LinkSession::enableStartStopSync(bool enabled) {
    if(audio::onAudioThread)return false;
    impl_->sdk.enableStartStopSync(enabled);impl_->startStopSync.store(enabled,std::memory_order_release);return true;
}
bool LinkSession::enabled()const noexcept{return impl_->sdk.isEnabled();}
std::int64_t LinkSession::clockMicros()const noexcept{return impl_->sdk.clock().micros().count();}
LinkFrame LinkSession::process(const LinkBuffer& buffer,const LinkCommand& command)noexcept {
    if(!valid(buffer,command))return {};
    const auto audible=std::chrono::microseconds(buffer.callbackMicros+buffer.outputLatencyMicros);
    auto state=impl_->sdk.captureAudioSessionState();
    if(command.tempo!=0)state.setTempo(command.tempo,audible);
    if(command.transport!=LinkTransport::Keep)state.setIsPlaying(command.transport==LinkTransport::Play,audible);
    if(command.requestBeat)state.requestBeatAtTime(command.beat,audible,buffer.quantum);
    LinkFrame frame;frame.valid=true;frame.enabled=impl_->sdk.isEnabled();frame.startStopSync=impl_->startStopSync.load(std::memory_order_acquire);frame.peers=impl_->sdk.numPeers();
    frame.tempo=state.tempo();frame.beat=state.beatAtTime(audible,buffer.quantum);frame.phase=state.phaseAtTime(audible,buffer.quantum);frame.requestedPlaying=state.isPlaying();frame.audibleMicros=audible.count();frame.transportChangeMicros=state.timeForIsPlaying().count();
    if(command.requestBeat){frame.hasLaunchTime=true;frame.launchMicros=state.timeAtBeat(command.beat,buffer.quantum).count();}
    if(command.tempo!=0 || command.transport!=LinkTransport::Keep || command.requestBeat)impl_->sdk.commitAudioSessionState(state);
    return frame;
}
} // namespace adi::engine
