// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/link_playback.hpp"
#include "adi/engine/audio_io_slice.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/textproj.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace adi::engine {
namespace {
constexpr double ppq=static_cast<double>(textproj::kPPQ);
constexpr std::int64_t maximumAge=2000000;
constexpr std::int64_t clockLimit=std::int64_t{1}<<50;
void silence(const AudioIo& io)noexcept {
    SilenceProcessor silent;
    if(io.frames>0 && io.numOut>=0 && io.numOut<=AudioIoSlice::maximumChannels)
        silent.process(io);
}
bool validFrame(const LinkFrame& frame,double quantum)noexcept {
    return frame.valid && std::isfinite(frame.tempo) && frame.tempo>=20 && frame.tempo<=999 &&
        std::isfinite(frame.beat) && std::abs(frame.beat)<=1e9 && std::isfinite(quantum) &&
        quantum>=1.0/1024 && quantum<=256 && frame.audibleMicros>=-clockLimit && frame.audibleMicros<=clockLimit &&
        frame.transportChangeMicros>=-clockLimit && frame.transportChangeMicros<=clockLimit &&
        (!frame.capturedMicros || (*frame.capturedMicros>=-clockLimit && *frame.capturedMicros<=clockLimit)) &&
        (!frame.hasLaunchTime || (frame.launchMicros>=-clockLimit && frame.launchMicros<=clockLimit));
}
bool equalTempo(std::optional<double> a,std::optional<double> b)noexcept {
    return a.has_value()==b.has_value() && (!a || std::abs(*a-*b)<1e-8);
}
bool validCommand(const LinkCommand& command)noexcept {
    if(command.tempo!=0 && (!std::isfinite(command.tempo) || command.tempo<20 || command.tempo>999))return false;
    if(command.requestBeat && (!std::isfinite(command.beat) || std::abs(command.beat)>1e9))return false;
    return command.transport==LinkTransport::Keep || command.transport==LinkTransport::Play || command.transport==LinkTransport::Stop;
}
std::int32_t offsetAt(std::int64_t time,std::int64_t origin,double rate,std::int32_t frames)noexcept {
    const double offset=static_cast<double>(time-origin)*rate/1000000.0;
    if(offset<=0)return 0;
    if(offset>=static_cast<double>(frames))return frames;
    return static_cast<std::int32_t>(std::llround(offset));
}
}
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
LinkPlayback::LinkPlayback(Session& session,LinkSession& link)noexcept:session_(session),link_(link){}
bool LinkPlayback::setLaunchTick(std::int64_t tick)noexcept {
    if(tick<0 || static_cast<double>(tick)>1e9*ppq)return false;
    launchTick_=tick;return true;
}
LinkPlayback::Result LinkPlayback::process(const AudioIo& io,const LinkBuffer& input,const LinkCommand& command)noexcept {
    auto block=session_.beginRender();
    AudioIoSlice whole(io,0,io.frames);
    if(!block.valid() || !whole.valid() || io.frames<=0 || io.frames>block.maxFrames() ||
       block.sampleRate()<=0){silence(io);return {};}
    LinkBuffer buffer=input;
    const auto graphLatency=static_cast<std::int64_t>(std::llround(
        static_cast<double>(block.latencySamples())*1000000.0/block.sampleRate()));
    if(buffer.outputLatencyMicros<0 || buffer.outputLatencyMicros>10000000-graphLatency){silence(io);return {};}
    buffer.outputLatencyMicros+=graphLatency;
    buffer.queryLaunch=true;buffer.launchBeat=static_cast<double>(launchTick_)/ppq;
    return consume(block,io,link_.process(buffer,command),buffer.quantum,command);
}
LinkPlayback::Result LinkPlayback::processCaptured(const AudioIo& io,const LinkFrame& frame,
                                                  double quantum,const LinkCommand& command)noexcept {
    auto block=session_.beginRender();return consume(block,io,frame,quantum,command);
}
LinkPlayback::Result LinkPlayback::consume(Session::RenderBlock& block,const AudioIo& io,
                                          const LinkFrame& frame,double quantum,const LinkCommand& command)noexcept {
    Result result;result.frame=frame;
    AudioIoSlice whole(io,0,io.frames);
    if(!block.valid() || !whole.valid() || io.frames<=0 || io.frames>block.maxFrames() ||
       !validFrame(frame,quantum) || !validCommand(command)){silence(io);return result;}
    result.valid=true;
    const double rate=block.sampleRate();
    auto& transport=session_.transport();
    const bool continuous=haveEnd_ && expectedSample_==transport.position() && expectedRevision_==transport.revision();
    if(!continuous) {
        performedBeat_=block.beatAtSample(transport.sampleAt(0));
        if(haveEnd_ && anchored_)beatOffset_=frame.beat-performedBeat_;
        if(haveEnd_ && frame.enabled) {
            repairTick_=block.tick();
            requested_.fetch_add(1,std::memory_order_acq_rel);pending_={};
        }
    } else if(lastGeneration_!=block.generation()) {
        performedBeat_+=block.beatAtSample(transport.sampleAt(0))-endBeat_;
    }
    if(block.externalTempoBpm() && std::abs(block.bpmAtTick(block.tick())-*block.externalTempoBpm())>1e-8) {
        if(transport.looping())repairTick_=block.loopBeginTick().value_or(block.tickAtSample(transport.loopBegin()));
        else if(!repairTick_)repairTick_=block.tick();
    }
    const auto request=[&](std::optional<double> tempo,bool reset) {
        // A slow owner must be able to finish a drift correction while audio
        // keeps running. Only a real Link tempo/mode change supersedes it.
        const bool inFlight=pending_ && prepared_.load(std::memory_order_acquire)!=pending_.serial;
        const bool changedIntent=pendingLinkTempo_!=frame.tempo || pending_.tempo.has_value()!=tempo.has_value() || pending_.reset!=reset ||
            (!reset && repairTick_ && pending_.boundaryTick!=*repairTick_);
        const auto captured=frame.capturedMicros.value_or(frame.audibleMicros);
        if(pending_ && !inFlight && !changedIntent && !reset && tempo &&
           captured>=pending_.capturedMicros && captured-pending_.capturedMicros<250000)
            return; // bound drift-preparation churn without delaying peer tempo changes
        if(!pending_ || prepared_.load(std::memory_order_acquire)==pending_.serial ||
           (!inFlight && !equalTempo(pending_.tempo,tempo)) || changedIntent ||
           captured-pending_.capturedMicros>maximumAge) {
            auto boundary=repairTick_.value_or(block.tick());
            if(reset && pendingStart_) {
                const double late=std::max(0.0,static_cast<double>(frame.audibleMicros-launchMicros_)/1000000.0);
                const double projected=static_cast<double>(launchTick_)+late*frame.tempo*ppq/60.0;
                if(projected>=static_cast<double>(std::numeric_limits<std::int64_t>::max())){result.valid=false;return;}
                boundary=static_cast<std::int64_t>(std::llround(projected));
            }
            pending_={requested_.fetch_add(1,std::memory_order_acq_rel)+1,block.generation(),
                      captured,boundary,tempo,reset};
            pendingLinkTempo_=frame.tempo;
        }
        result.preparation=pending_;result.waiting=true;
    };
    if(!frame.enabled) {
        if(linked_){requested_.fetch_add(1,std::memory_order_acq_rel);pending_={};}
        pendingStart_=false;pendingStop_=false;anchored_=false;haveChange_=false;linked_=false;repairTick_.reset();
        if(block.externalTempoBpm())request(std::nullopt,!transport.playing());
        if(command.transport!=LinkTransport::Keep)transport.play(command.transport==LinkTransport::Play);
        (void)block.process(io);haveEnd_=false;return result;
    }
    if(!linked_){requested_.fetch_add(1,std::memory_order_acq_rel);pending_={};linked_=true;haveChange_=false;anchored_=false;}
    const bool explicitTransport=command.transport!=LinkTransport::Keep;
    const bool follow=explicitTransport || frame.startStopSync;
    if(follow && (!haveChange_ || explicitTransport || frame.transportChangeMicros!=lastChange_)) {
        haveChange_=true;lastChange_=frame.transportChangeMicros;
        const bool play=explicitTransport?command.transport==LinkTransport::Play:frame.requestedPlaying;
        // A newer Play supersedes a scheduled Stop even when playback is
        // already running. Idempotence must not retain the older intention.
        if(play && transport.playing()){pendingStart_=false;pendingStop_=false;}
        if(play && !transport.playing()) {
            pendingLocal_=explicitTransport;
            pendingStart_=true;pendingStop_=false;
            launchMicros_=frame.hasLaunchTime?frame.launchMicros:frame.transportChangeMicros;
        } else if(!play) {
            pendingLocal_=explicitTransport;
            pendingStart_=false;pendingStop_=true;stopMicros_=frame.transportChangeMicros;
        }
    }
    if(!frame.startStopSync && !pendingLocal_) {
        pendingStart_=false;pendingStop_=false;
    }
    if(pendingStart_ && frame.hasLaunchTime && frame.transportChangeMicros==lastChange_)
        launchMicros_=frame.launchMicros;
    double target=frame.tempo;
    if(transport.playing() && !pendingStart_) {
        const double nativeBeat=performedBeat_;
        if(!anchored_) {
            beatOffset_=std::round((frame.beat-nativeBeat)/quantum)*quantum;
            anchored_=true;
        }
        result.phaseErrorFrames=(frame.beat-beatOffset_-nativeBeat)*60.0/frame.tempo*rate;
        if(std::abs(result.phaseErrorFrames)>2)
            target=std::clamp(frame.tempo*(1+std::clamp(result.phaseErrorFrames/(2*rate),-.005,.005)),20.0,999.0);
    }
    const bool clockReady=block.externalTempoBpm() &&
        std::abs(*block.externalTempoBpm()-target)<1e-8 &&
        std::abs(block.bpmAtTick(block.tick())-*block.externalTempoBpm())<1e-8 &&
        (!repairTick_ || std::abs(block.bpmAtTick(*repairTick_)-target)<1e-8) &&
        (!pendingStart_ || block.constantExternalTempo());
    if(!clockReady)request(target,pendingStart_);
    else if(pending_ && prepared_.load(std::memory_order_acquire)==pending_.serial){pending_={};repairTick_.reset();}
    if(!result.valid){silence(io);return result;}

    std::int32_t cursor=0;
    const auto render=[&](std::int32_t end) {
        if(end<=cursor)return;
        if(transport.playing()) {
            if(transport.looping()) {
                const auto frames=end-cursor;
                const bool wraps=transport.contiguousFrames(0,frames)<frames ||
                    transport.sampleAt(frames)==transport.loopBegin();
                const auto begin=block.loopBeginTick().value_or(block.tickAtSample(transport.loopBegin()));
                if(wraps && block.externalTempoBpm() &&
                   std::abs(block.bpmAtTick(begin)-*block.externalTempoBpm())>1e-8)
                    repairTick_=begin;
            }
            const auto frames=static_cast<std::int64_t>(end-cursor);
            const auto first=transport.contiguousFrames(0,frames);
            const auto sample=transport.sampleAt(0);
            if(sample<=std::numeric_limits<std::int64_t>::max()-first)
                performedBeat_+=block.beatAtSample(sample+first)-block.beatAtSample(sample);
            const auto remaining=frames-first;
            if(remaining>0 && transport.looping()) {
                const auto begin=transport.loopBegin(),length=transport.loopEnd()-begin;
                if(length>0) {
                    const auto cycles=remaining/length,tail=remaining%length;
                    const auto beginBeat=block.beatAtSample(begin);
                    performedBeat_+=static_cast<double>(cycles)*(block.beatAtSample(transport.loopEnd())-beginBeat);
                    performedBeat_+=block.beatAtSample(begin+tail)-beginBeat;
                }
            }
        }
        AudioIoSlice slice(io,cursor,end-cursor);
        if(slice.valid())(void)block.process(slice.io());
        cursor=end;
    };
    if(pendingStart_ && clockReady) {
        const auto offset=offsetAt(launchMicros_,frame.audibleMicros,rate,io.frames);
        if(offset<io.frames) {
            render(offset);
            const double lateSeconds=std::max(0.0,static_cast<double>(frame.audibleMicros-launchMicros_)/1000000.0);
            const double startTick=static_cast<double>(launchTick_)+lateSeconds*frame.tempo*ppq/60.0;
            if(startTick<static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
                const auto tick=static_cast<std::int64_t>(std::llround(startTick));
                Transport launch=transport;launch.locate(block.sampleAtTick(tick));launch.play();
                if(block.sourcesReady(launch,io.frames-offset)) {
                    transport.locate(launch.position());transport.play();
                    performedBeat_=startTick/ppq;
                    beatOffset_=frame.beat+(static_cast<double>(offset)/rate)*frame.tempo/60.0-startTick/ppq;
                    anchored_=true;pendingStart_=false;result.started=true;result.startOffset=offset;
                    requested_.fetch_add(1,std::memory_order_acq_rel);pending_={};
                    result.preparation={};result.waiting=false;
                } else request(target,true);
            }
        }
    }
    if(pendingStop_) {
        const auto offset=offsetAt(stopMicros_,frame.audibleMicros,rate,io.frames);
        if(offset<io.frames) {
            render(offset);transport.play(false);pendingStop_=false;
            anchored_=false;result.stopped=true;result.stopOffset=offset;
        }
    }
    render(io.frames);
    expectedSample_=transport.position();expectedRevision_=transport.revision();
    endBeat_=block.beatAtSample(transport.sampleAt(0));lastGeneration_=block.generation();haveEnd_=true;
    result.waiting=result.waiting || pendingStart_;
    return result;
}
bool LinkPlayback::prepare(const Store& store,const Preparation& packet,std::int64_t nowMicros) {
    if(audio::onAudioThread || !packet || nowMicros<-clockLimit || nowMicros>clockLimit ||
       packet.capturedMicros<-clockLimit || packet.capturedMicros>clockLimit || nowMicros<packet.capturedMicros ||
       nowMicros-packet.capturedMicros>maximumAge || requested_.load(std::memory_order_acquire)!=packet.serial ||
       prepared_.load(std::memory_order_acquire)==packet.serial)return false;
    const auto prepareStarted=link_.clockMicros();
    const bool realClock=std::abs(nowMicros-prepareStarted)<maximumAge;
    const auto guard=[&] {
        const auto now=link_.clockMicros();
        // Controlled captures may use a synthetic clock. The explicit caller
        // timestamp always bounds admission; real captures also age while building.
        return requested_.load(std::memory_order_acquire)==packet.serial &&
            (!realClock || (now>=packet.capturedMicros && now-packet.capturedMicros<=maximumAge));
    };
    const bool ok=packet.reset?session_.setExternalTempo(store,packet.tempo,guard,packet.boundaryTick):
        session_.setExternalTempoAt(store,packet.tempo,packet.boundaryTick,guard);
    if(ok)prepared_.store(packet.serial,std::memory_order_release);
    return ok;
}
} // namespace adi::engine
