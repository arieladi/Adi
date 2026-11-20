// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/link_session.hpp"
#include "adi/audio/io_audit.hpp"
#ifdef LINK_AUDIO
#error "ADI LinkSession is musical sync only; Link Audio must remain disabled"
#endif
#include <ableton/Link.hpp>
#ifdef LINK_AUDIO
#error "ADI LinkSession must not select the Link Audio SDK controller"
#endif
#include <type_traits>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
namespace adi::engine {
namespace {
// Link4's audio header selects a different API controller before Link.hpp.
// Pin the adapter to the musical-only controller as well as rejecting its macro.
using MusicalLinkController=ableton::link::SessionController<ableton::link::PeerCountCallback,
    ableton::link::TempoCallback,ableton::link::StartStopStateCallback,ableton::Link::Clock,
    ableton::link::platform::Random,ableton::link::platform::IoContext>;
static_assert(std::is_same_v<ableton::link::ApiController<ableton::Link::Clock>,MusicalLinkController>);
bool validTempo(double tempo)noexcept{return std::isfinite(tempo) && tempo>=20 && tempo<=999;}
bool valid(const LinkBuffer& buffer,const LinkCommand& command)noexcept {
    constexpr std::int64_t limit=std::int64_t{1}<<50;
    if(buffer.callbackMicros < -limit || buffer.callbackMicros>limit || buffer.outputLatencyMicros<0 || buffer.outputLatencyMicros>10000000 ||
       !std::isfinite(buffer.quantum) || buffer.quantum<1.0/1024 || buffer.quantum>256 ||
       (buffer.queryLaunch && (!std::isfinite(buffer.launchBeat) || std::abs(buffer.launchBeat)>1e9)) ||
       (command.tempo!=0 && !validTempo(command.tempo)) ||
       (command.requestBeat && (!std::isfinite(command.beat) || std::abs(command.beat)>1e9)))return false;
    switch(command.transport){case LinkTransport::Keep:case LinkTransport::Play:case LinkTransport::Stop:return true;}
    return false;
}
}
// Link 4.1.1 serves the audio thread its own last commit only while that commit
// is younger than its 1 s local-modification grace period, and it stamps the
// grace period ZERO while networking is disabled. With networking off it
// therefore reads its IO thread's copy whenever no audio commit is pending; but
// that IO thread clears the pending flag when it finishes the batch it took
// BEFORE a newer commit arrived, so the next capture can return the state from
// before the newest commit (old tempo, old transport) until the IO thread
// catches up. The ubuntu arm64 leg caught that on main 5acebf6b. With
// networking off this participant is the only writer, so the audio thread keeps
// its own last commit for the same grace period. Networking on keeps the SDK's
// own timestamped path untouched, where peers may legitimately change state.
constexpr std::chrono::microseconds kOwnCommitGrace{1000000};
struct LinkSession::Impl {
    explicit Impl(double tempo):sdk(tempo){}
    ableton::Link sdk;
    std::atomic<bool> startStopSync{false};
    // Owner configuration epoch, bumped only when the owner turns networking
    // on; that retires the audio-held commit below, so it never outlives the
    // networking-off state. Calls that change nothing the SDK reports keep it.
    std::atomic<std::uint64_t> epoch{0};
    // Audio thread only.
    std::optional<ableton::Link::SessionState> committed;
    std::chrono::microseconds committedAt{0};
    std::uint64_t committedEpoch=0;
};
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
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
    // Only turning networking on changes what the SDK reports: Link resets its
    // session state when discovery starts and peers may then write. Turning it
    // off needs no epoch (a callback that sees Link enabled already drops the
    // held commit, and off->on->off between two callbacks bumps here), and an
    // unchanged value does nothing in the SDK, so neither retires the commit.
    // Every enable(true) bumps, not only a change seen here: two owner threads
    // could otherwise both read "already on" around the one that turned it on.
    // A bump while on costs nothing, since the held commit is only used while off.
    impl_->sdk.enable(enabled);
    if(enabled)impl_->epoch.fetch_add(1,std::memory_order_release);
    return true;
}
bool LinkSession::enableStartStopSync(bool enabled) {
    if(audio::onAudioThread)return false;
    // Link 4.1.1 only flips its sync flag here; timeline and transport stay, so
    // the audio thread's held commit stays too.
    impl_->sdk.enableStartStopSync(enabled);impl_->startStopSync.store(enabled,std::memory_order_release);return true;
}
bool LinkSession::enabled()const noexcept{return impl_->sdk.isEnabled();}
std::int64_t LinkSession::clockMicros()const noexcept{return impl_->sdk.clock().micros().count();}
LinkFrame LinkSession::process(const LinkBuffer& buffer,const LinkCommand& command)noexcept {
    if(!valid(buffer,command))return {};
    const auto audible=std::chrono::microseconds(buffer.callbackMicros+buffer.outputLatencyMicros);
    // Epoch before enabled: the owner changes the SDK first, then the epoch.
    const auto epoch=impl_->epoch.load(std::memory_order_acquire);
    const bool enabled=impl_->sdk.isEnabled();
    const auto now=impl_->sdk.clock().micros();
    const bool ownCommit=!enabled && impl_->committed && impl_->committedEpoch==epoch &&
        now>=impl_->committedAt && now-impl_->committedAt<kOwnCommitGrace;
    if(!ownCommit)impl_->committed.reset();
    auto state=ownCommit?*impl_->committed:impl_->sdk.captureAudioSessionState();
    if(command.tempo!=0)state.setTempo(command.tempo,audible);
    if(command.transport!=LinkTransport::Keep)state.setIsPlaying(command.transport==LinkTransport::Play,audible);
    if(command.requestBeat)state.requestBeatAtTime(command.beat,audible,buffer.quantum);
    LinkFrame frame;frame.valid=true;frame.capturedMicros=buffer.callbackMicros;frame.enabled=enabled;frame.startStopSync=impl_->startStopSync.load(std::memory_order_acquire);frame.peers=impl_->sdk.numPeers();
    frame.tempo=state.tempo();frame.beat=state.beatAtTime(audible,buffer.quantum);frame.phase=state.phaseAtTime(audible,buffer.quantum);frame.requestedPlaying=state.isPlaying();frame.audibleMicros=audible.count();frame.transportChangeMicros=state.timeForIsPlaying().count();
    if(command.requestBeat){frame.hasLaunchTime=true;frame.launchMicros=state.timeAtBeat(command.beat,buffer.quantum).count();}
    else if(buffer.queryLaunch && frame.requestedPlaying) {
        auto launch=state;
        launch.requestBeatAtStartPlayingTime(buffer.launchBeat,buffer.quantum);
        frame.hasLaunchTime=true;
        frame.launchMicros=launch.timeAtBeat(buffer.launchBeat,buffer.quantum).count();
    }
    if(command.tempo!=0 || command.transport!=LinkTransport::Keep || command.requestBeat) {
        impl_->sdk.commitAudioSessionState(state);
        if(!enabled){impl_->committed=state;impl_->committedAt=now;impl_->committedEpoch=epoch;}
    }
    return frame;
}
} // namespace adi::engine
