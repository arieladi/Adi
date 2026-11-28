// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/midi_clips.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/engine/curves.hpp"
#include "adi/engine/timeline_clips.hpp"
#include "adi/blob.hpp"
#include "adi/engine/midi_controllers.hpp"
#include "adi/engine/warp_arrangement_clock.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>
namespace adi::engine {
namespace {
constexpr std::size_t maxActive = 128;
constexpr std::size_t maxOccurrences = 1000000;
constexpr std::size_t maxExpressionSamples = 1000000;
constexpr auto expressionDimensions = kStandardExpressionDimensionCount;
struct ExpressionSample {std::int64_t time;double value;};
struct ExpressionRange {std::size_t first=0,last=0;};
using ExpressionCurves = std::array<const NoteExpressionCurve*,expressionDimensions>;
struct ClipClock {
    std::int64_t id=0,timeBase=0,offset=0,loopStart=0;
    std::optional<std::int64_t> posTicks,posNs,lengthTicks,lengthNs,loopLength,endMarker,alias;
    bool looping=false,muted=false;
    friend bool operator==(const ClipClock&,const ClipClock&)=default;
};
ClipClock clipClock(const rows::Clip& clip) {
    return {clip.id,clip.timeBase,clip.contentOffsetTicks,clip.loopStartTicks,
            clip.posTicks,clip.posNs,clip.lengthTicks,clip.lengthNs,clip.loopLenTicks,
            clip.endMarkerTicks,clip.aliasOf,clip.loopEnabled,clip.muted};
}
struct NoteIdentity {
    std::int64_t clip=0,repeat=0;
    std::uint64_t note=0;
    friend bool operator==(const NoteIdentity&,const NoteIdentity&)=default;
    friend bool operator<(const NoteIdentity& a,const NoteIdentity& b) noexcept {
        return std::tie(a.clip,a.note,a.repeat)<std::tie(b.clip,b.note,b.repeat);
    }
};
struct AuthoredNote {
    ClipClock clip;
    std::int64_t start=0,duration=0,key=0,channel=0,velocity=0,release=0,probability=0;
    std::uint16_t flags=0;
    double tuning=0,rate=0;
    std::uint64_t route=0;
    std::uint64_t expression=0;
    friend bool operator==(const AuthoredNote&,const AuthoredNote&)=default;
};
struct RouteVersion {
    std::vector<Node*> path;
    std::uint64_t generation=0;
};
struct ControllerIdentity {
    std::vector<ClipClock> clips;
    std::vector<controllers::Lane> lanes;
    double rate=0;
    std::shared_ptr<const RouteVersion> route;
    friend bool operator==(const ControllerIdentity&,const ControllerIdentity&)=default;
};
struct ControllerVersion {
    ControllerIdentity identity;
    std::uint64_t generation=0;
};
struct Occurrence {
    std::int64_t on=0,off=0;
    Event start,stop;
    double tuning=0;
    NoteIdentity identity;
    AuthoredNote authored;
    std::array<ExpressionRange,expressionDimensions> expression;
    std::uint64_t expressionTiming=0;
};
struct Active {
    bool used=false,pending=false,releasing=false,sounding=false,retryOn=false;
    std::uint64_t occurrence=0,generation=0;
    std::uint64_t retainedOn=0,retainedOff=0;
    Occurrence note;
    std::array<std::size_t,expressionDimensions> expressionAt{};
};
struct ControlState {
    double requestedValue = 0, confirmedValue = 0;
    std::uint32_t serial = 0, confirmedSeq = 0;
    std::uint32_t retainedSerial=0;
    std::uint64_t retainedGeneration=0;
    bool requestedValid = false, pending = false, retry = false, held = false, releaseDesired = false,
         confirmedKnown = false;
};
struct TrackState {
    std::array<ControlState, kControllerAddressCount> controls{};
    const std::uint32_t controlPrefix = allocateControllerSourcePrefix();
    std::uint16_t controlCursor = 0;
    std::atomic<std::uint32_t> droppedControls{0};
    std::array<Active,maxActive> active{};
    std::uint64_t generation=0, revision=0,controllerGeneration=0;
    std::atomic<std::uint64_t> renderedControllerGeneration{0};
    std::int64_t expected=-1;
    std::uint64_t prefix=0;
    std::uint32_t serial=0;
    std::atomic<std::uint32_t> dropped{0};
    std::uint32_t noteOnsets = 0;
    std::atomic<std::uint64_t> noteActivity{0}; // onset count and held flag, published together
};
struct Scheduled { std::int64_t time; std::size_t note; bool on; };
class Source final : public Node {
public:
    Transport& transport;
    std::shared_ptr<TrackState> state;
    std::uint64_t generation;
    std::uint64_t controllerGeneration=0;
    std::uint64_t retainedTransportRevision=0;
    std::shared_ptr<const ControllerVersion> controllerVersion;
    std::vector<Occurrence> notes;
    // Built once on the owner; at most 128 bounded binary searches per swap.
    std::vector<std::size_t> byIdentity;
    std::vector<Scheduled> events;
    std::vector<ExpressionSample> expressionSamples;
    std::vector<MidiMonitorRange> monitorRanges;
    bool monitorInputOnly = false;
    bool meterNotes = false;
    std::shared_ptr<const MidiControllerProgram> controls;
    std::uint16_t pitchChase = 0;
    bool retainsQueuedSourceEvents()const noexcept override{return true;}
    const Occurrence* replacement(const Active& active)const noexcept {
        if(!active.note.identity.note || byIdentity.empty())return nullptr;
        const auto* end=byIdentity.data()+byIdentity.size();
        const auto* found=std::lower_bound(byIdentity.data(),end,active.note.identity,
            [&](std::size_t index,const NoteIdentity& key){return notes[index].identity<key;});
        if(found==end)return nullptr;
        const auto& next=notes[*found];
        return next.identity==active.note.identity && next.authored==active.note.authored?&next:nullptr;
    }
    bool canRetainPendingEvent(const Event& event)const noexcept override {
        if(!transport.playing() || state->revision!=transport.revision() || state->expected!=transport.sampleAt(0))return false;
        if(event.type==EventType::Controller) {
            if((event.noteId>>32)!=state->controlPrefix || event.channel>15 ||
               !controllers::validController(event.paramId) ||
               state->controllerGeneration!=controllerGeneration)return false;
            const auto& controlState=state->controls[controllerAddress(event.channel, static_cast<int>(event.paramId))];
            const auto serial=static_cast<std::uint32_t>(event.noteId);
            // Earlier curve samples can still be queued after a newer value
            // was issued, and another route may have acknowledged them first.
            return serial && controlState.serial-serial<0x80000000u;
        }
        if(event.type!=EventType::NoteOn && event.type!=EventType::NoteOff && event.type!=EventType::NoteExpression)return false;
        for(const auto& active:state->active)if(active.used && active.note.start.noteId==event.noteId) {
            const auto* next=replacement(active);
            if(!next)return false;
            if(event.type==EventType::NoteExpression && event.paramId==kNativeNoteExpressionUnits)
                return next->expressionTiming==active.note.expressionTiming;
            return event.type==EventType::NoteOff?next->off==active.note.off:next->on==active.note.on;
        }
        return false;
    }
    void pendingEventRetained(const Event& event)noexcept override {
        retainedTransportRevision=transport.revision();
        if(event.type==EventType::Controller && event.channel<=15 && controllers::validController(event.paramId) &&
           (event.noteId>>32)==state->controlPrefix) {
            auto& controlState=state->controls[controllerAddress(event.channel, static_cast<int>(event.paramId))];
            const auto serial=static_cast<std::uint32_t>(event.noteId);
            if(serial==controlState.serial){controlState.retainedGeneration=generation;controlState.retainedSerial=serial;}
        } else for(auto& active:state->active)if(active.used && active.note.start.noteId==event.noteId) {
            if(event.type==EventType::NoteOn)active.retainedOn=generation;
            if(event.type==EventType::NoteOff)active.retainedOff=generation;
        }
    }
    bool retainedEventsStillValid(const Event& event)const noexcept override {
        if(retainedTransportRevision!=transport.revision())return false;
        if(event.type==EventType::NoteOn || event.type==EventType::NoteExpression) {
            // A copied onset is playable only after its voice was adopted.
            // If its paired release could not transfer (capacity or changed
            // clock), adoptNotes force-stops the old generation instead.
            for(const auto& active:state->active)
                if(active.used && active.generation==generation && active.note.start.noteId==event.noteId)return true;
            return false;
        }
        return true;
    }
    std::uint32_t controllerSourcePrefix() const noexcept override {
        return state->controlPrefix;
    }
    static constexpr std::array<int, 4> releaseControllers{64, 66, 69, controllers::kPitchBend};
    static bool managesRelease(std::uint16_t address) noexcept {
        return std::ranges::find(releaseControllers, controllerNumber(address)) != releaseControllers.end();
    }
    static double neutralValue(std::uint16_t address) noexcept {
        return address >= kCcAddressCount ? .5 : 0.;
    }
    bool control(EventList &out, std::int32_t frame, std::uint16_t address, double value,
                 bool force = false) noexcept {
        auto &c = state->controls[address];
        if (!force && c.requestedValid && !c.retry && c.requestedValue == value)
            return true;
        Event e;
        e.type = EventType::Controller;
        e.frame = frame;
        e.channel = controllerChannel(address);
        e.paramId = static_cast<std::uint32_t>(controllerNumber(address));
        e.value = value;
        auto serial = c.serial + 1;
        if (!serial)
            ++serial;
        e.noteId = (std::uint64_t(state->controlPrefix) << 32) | serial;
        if (!out.push(e)) {
            ++state->droppedControls;
            return false;
        }
        c.serial = serial;
        c.requestedValue = value;
        c.requestedValid = true;
        c.pending = true;
        c.retry = false;
        if (managesRelease(address)) {
            const bool neutral = value == neutralValue(address);
            if (!neutral)
                c.held = true;
            c.releaseDesired = neutral;
        }
        return true;
    }
    void controllerDelivered(const Event &e) noexcept override {
        if (e.type != EventType::Controller || e.channel > 15 || !controllers::validController(e.paramId) ||
            (e.noteId >> 32) != state->controlPrefix)
            return;
        const auto address = controllerAddress(e.channel, static_cast<int>(e.paramId));
        auto &c = state->controls[address];
        const auto seq = static_cast<std::uint32_t>(e.noteId);
        const auto delta = seq - c.confirmedSeq;
        if (!c.confirmedKnown || (delta != 0 && delta < 0x80000000u)) {
            c.confirmedKnown = true;
            c.confirmedSeq = seq;
            c.confirmedValue = e.value;
        }
        if (seq == c.serial) {
            c.pending = false;
            if (e.value == neutralValue(address) && !c.retry)
                c.held = false;
        }
    }
    void controllerRejected(const Event &e) noexcept override {
        if (e.type != EventType::Controller || e.channel > 15 || !controllers::validController(e.paramId) ||
            (e.noteId >> 32) != state->controlPrefix)
            return;
        const auto address = controllerAddress(e.channel, static_cast<int>(e.paramId));
        auto &c = state->controls[address];
        if (static_cast<std::uint32_t>(e.noteId) != c.serial)
            return;
        c.pending = false;
        c.retry = true;
        if (managesRelease(address) && e.value == neutralValue(address)) {
            c.held = true;
            c.releaseDesired = true;
        }
    }
    bool releaseControls(EventList &out, std::int32_t frame, bool force = false, bool changedOnly = false,
                         std::int64_t time = 0) noexcept {
        bool ok = true;
        if (changedOnly) {
            if (!controls)
                return true;
            for (std::size_t i = 0; i < controls->size(); ++i) {
                const auto address = controls->address(i);
                if (managesRelease(address) && controls->changesOwnerAt(i, time)) {
                    auto &c = state->controls[address];
                    if (c.held || c.retry)
                        ok = control(out, frame, address, neutralValue(address), force) && ok;
                    c.requestedValid = false;
                }
            }
        } else
            for (std::uint16_t channel = 0; channel < 16; ++channel)
                for (const auto controller : releaseControllers) {
                    const auto address = controllerAddress(channel, controller);
                    auto &c = state->controls[address];
                    if (c.held || c.retry)
                        ok = control(out, frame, address, neutralValue(address), force) && ok;
                }
        return ok;
    }
    void invalidateControls() noexcept {
        if (controls)
            for (std::size_t i = 0; i < controls->size(); ++i)
                state->controls[controls->address(i)].requestedValid = false;
    }
    static std::int32_t controllerReserve(const EventList& out) noexcept {
        // Keep room for note-ons and all owned note-offs.
        return std::min(out.capacity(),
                        std::max(3, std::min(out.capacity() / 2, static_cast<std::int32_t>(maxActive * 2))));
    }
    void evaluateControls(EventList &out, std::int32_t frame, std::int64_t time) noexcept {
        if (!controls || !controls->size())
            return;
        // Rotate the first address so a small output list cannot starve later controllers.
        const auto reserve = controllerReserve(out);
        const auto size = controls->size();
        const auto start = static_cast<std::size_t>(state->controlCursor) % size;
        for (std::size_t j = 0; j < size; ++j) {
            const auto i = (start + j) % size;
            double value = 0;
            if (!controls->valueAt(i, time, value))
                continue;
            const auto address = controls->address(i);
            auto &c = state->controls[address];
            if (c.requestedValid && !c.retry && c.requestedValue == value)
                continue;
            if (out.capacity() - out.size() <= reserve) {
                ++state->droppedControls;
                state->controlCursor = static_cast<std::uint16_t>(i);
                return;
            }
            control(out, frame, address, value);
        }
        state->controlCursor = static_cast<std::uint16_t>((start + 1) % size);
    }
    void chasePitchAtStart(EventList& out, std::int64_t sample, bool regularChase) noexcept {
        const auto channels = std::exchange(pitchChase, std::uint16_t{0});
        if (!channels || !controls) return;
        const auto reserve = controllerReserve(out);
        // Addresses are sorted. The last sixteen possible lanes are pitch wheels.
        for (auto i = controls->size(); i > 0;) {
            const auto address = controls->address(--i);
            if (address < kCcAddressCount) break;
            if (!(channels & (1u << controllerChannel(address))) || controls->changesOwnerAt(i, sample))
                continue; // An owner change already performs its own release and chase.
            double value = 0;
            if (!controls->valueAt(i, sample, value)) continue;
            state->controls[address].requestedValid = false;
            if (regularChase) continue;
            if (out.capacity() - out.size() > reserve)
                control(out, 0, address, value);
            else
                ++state->droppedControls; // Retry on the normal controller cadence.
        }
    }
    std::weak_ptr<void> owner;
    Source(Transport& t,std::shared_ptr<TrackState> s,std::uint64_t g):transport(t),state(std::move(s)),generation(g){}
    std::shared_ptr<void> sourceLifetime() const override { return owner.lock(); }
    // ADR-0092: new at every rebuild, the same track's notes all the same.
    std::uint64_t historyName() const noexcept override { return 2; }
    std::int64_t tailSamples() const noexcept override { return 0; }
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override {
        if(io.wide()) { for(std::int32_t c=0;c<io.channels;++c) std::fill_n(io.out64[c]+io.blockOffset,io.frames,0.0); return; }
        for(std::int32_t c=0;c<io.channels;++c) std::fill_n(io.out[c]+io.blockOffset,io.frames,0.0f);
    }
    bool stop(Active& a,EventList& out,std::int32_t frame,bool force=false) noexcept {
        if(a.pending && !force)return true;
        a.releasing=true;
        auto e=a.note.stop;e.frame=frame;
        if(!out.push(e))return false; // retain ownership and retry; never forget an off
        a.pending=true;return true;
    }
    bool flush(EventList& out,std::int32_t frame,bool force=false) noexcept {
        bool ok=true;
        for(auto& a:state->active)if(a.used)ok=stop(a,out,frame,force)&&ok;
        return ok;
    }
    void chaseExpression(Active& active,std::int64_t time) noexcept {
        for(std::size_t dimension=0;dimension<expressionDimensions;++dimension) {
            const auto range=active.note.expression[dimension];
            if(range.first==range.last) {active.expressionAt[dimension]=range.last;continue;}
            const auto* begin=expressionSamples.data();
            const auto* next=std::upper_bound(begin+range.first,begin+range.last,time,
                [](const auto sample,const auto& point){return sample<point.time;});
            active.expressionAt[dimension]=next==begin+range.first?range.first:
                static_cast<std::size_t>(next-begin-1);
        }
    }
    std::int64_t nextExpression(std::int64_t first,std::int64_t limit) const noexcept {
        for(const auto& active:state->active)if(active.used && !active.releasing && active.generation==generation) {
            for(std::size_t d=0;d<expressionDimensions;++d) {
                const auto at=active.expressionAt[d];
                if(at<active.note.expression[d].last)
                    limit=std::min(limit,std::max(first,expressionSamples.data()[at].time));
            }
        }
        return limit;
    }
    bool emitExpression(EventList& out,std::int32_t frame,std::int64_t time) noexcept {
        const auto reserve=std::count_if(state->active.begin(),state->active.end(),[](const auto& active){return active.used;});
        for(auto& active:state->active)if(active.used && !active.releasing && active.generation==generation) {
            for(std::size_t d=0;d<expressionDimensions;++d) {
                auto& at=active.expressionAt[d];
                const auto last=active.note.expression[d].last;
                if(at>=last || expressionSamples.data()[at].time>time)continue;
                if(out.capacity()-out.size()<=reserve)return false;
                // A Locate, resend or delayed generation adopts the latest
                // value at its actual position, using this voice's runtime ID.
                while(at+1<last && expressionSamples.data()[at+1].time<=time)++at;
                auto event=active.note.start;
                event.type=EventType::NoteExpression;event.frame=frame;
                event.dim=static_cast<std::uint16_t>(d);event.paramId=kNativeNoteExpressionUnits;
                event.value=expressionSamples.data()[at++].value;
                out.push(event);
            }
        }
        return true;
    }
    void start(const Occurrence& n,EventList& out,std::int32_t frame,std::int64_t time) noexcept {
        // A generation chase must not retrigger a voice adopted by identity.
        for(const auto& a:state->active)
            if(a.used && !a.releasing && a.generation==generation &&
               a.occurrence==n.start.noteId)return;
        // Reserve enough room for every currently owned off, even when an
        // adversarial clip floods one callback. Refused ons cannot hang.
        std::size_t active=0;for(const auto& a:state->active)active+=a.used?1u:0u;
        if(out.capacity()-out.size()<static_cast<std::int32_t>(active)+3){++state->dropped;return;}
        if(state->serial==std::numeric_limits<std::uint32_t>::max()){++state->dropped;return;}
        for(auto& a:state->active)if(!a.used) {
            auto e=n.start;e.frame=frame;e.noteId=state->prefix | ++state->serial;
            if(!out.push(e))return;
            if (meterNotes) ++state->noteOnsets;
            a.note=n;a.used=true;a.pending=false;a.releasing=false;a.sounding=false;a.retryOn=false;a.occurrence=n.start.noteId;
            a.retainedOn=0;a.retainedOff=0;
            a.generation=generation;
            a.note.start.noteId=e.noteId;a.note.stop.noteId=e.noteId;
            chaseExpression(a,time);
            if(n.tuning!=0 && n.expression[0].first==n.expression[0].last) {
                e.type=EventType::NoteExpression;e.dim=static_cast<std::uint16_t>(ExpressionDim::Pitch);
                e.value=n.tuning/100.0;out.push(e);
            }
            return;
        }
        ++state->dropped;
    }
    void chase(EventList& out,std::int32_t frame,std::int64_t time) noexcept {
        for(std::size_t i=0;i<notes.size();++i) {
            const auto& n=notes[i];
            if(n.on>=time)break;
            if(n.off>time)start(n,out,frame,time);
        }
    }
    void noteOffDelivered(const Event& e) noexcept override {
        // Another branch may have refused this release earlier in the same
        // callback. Its retry ownership survives a different branch's success.
        for(auto& a:state->active)if(a.used && a.pending && a.note.start.noteId==e.noteId)a.used=false;
    }
    void noteOffRejected(const Event& e) noexcept override {
        for(auto& a:state->active)if(a.used && a.note.start.noteId==e.noteId)a.pending=false;
    }
    void noteOnDelivered(const Event& e) noexcept override {
        for(auto& a:state->active)if(a.used && a.note.start.noteId==e.noteId){a.sounding=true;a.retryOn=false;}
    }
    void noteOnRejected(const Event& e) noexcept override {
        for(auto& a:state->active)if(a.used && a.note.start.noteId==e.noteId && !a.sounding)a.retryOn=true;
    }
    bool adoptNotes(EventList& out,std::int64_t first) noexcept {
        for(auto& active:state->active) {
            if(!active.used)continue;
            // A verified retained queue is as real as an acknowledged voice.
            // Unheard ons from discarded routes are re-chased instead.
            if(!active.sounding && active.retainedOn!=generation){active.used=false;continue;}
            const auto* next=replacement(active);
            const bool retainedRelease=active.pending && active.retainedOff==generation;
            // A release due at the first frame still belongs to the adopted
            // voice. The normal release can run alongside the retained onset.
            if(next && next->on<=first &&
               ((!active.releasing && !active.pending && next->off>=first) || retainedRelease)) {
                const auto runtimeId=active.note.start.noteId;
                active.note=*next;
                active.note.start.noteId=runtimeId;active.note.stop.noteId=runtimeId;
                active.occurrence=next->start.noteId;active.generation=generation;
                if(active.note.authored.expression)chaseExpression(active,first);
            } else if(!stop(active,out,0,true))return false;
        }
        return true;
    }
    void publishNoteActivity(bool held) noexcept {
        const auto reading = (std::uint64_t{state->noteOnsets} << 1) | static_cast<std::uint64_t>(held);
        if (state->noteActivity.load(std::memory_order_relaxed) != reading)
            state->noteActivity.store(reading, std::memory_order_relaxed);
    }
    void release() override {
        if (meterNotes) publishNoteActivity(false);
    }
    void sourceEvents(EventList& out, std::int32_t frames) noexcept override {
        scheduleEvents(out, frames);
        if (meterNotes && frames > 0)
            publishNoteActivity(!notes.empty() && transport.playing() && std::any_of(state->active.begin(), state->active.end(),
                [](const Active& note) { return note.used && !note.releasing; }));
    }
    void scheduleEvents(EventList& out,std::int32_t frames) noexcept {
        if (frames <= 0)
            return;
        // A rejected neutral value must remain owned even after its lane disappears.
        for (std::uint16_t channel = 0; channel < 16; ++channel)
            for (const auto controller : releaseControllers) {
                const auto address = controllerAddress(channel, controller);
                const auto &c = state->controls[address];
                if (c.releaseDesired && c.retry && !control(out, 0, address, neutralValue(address)))
                    return;
            }
        const auto first=transport.sampleAt(0);
        const bool newGeneration = state->generation != generation;
        const bool moved=state->revision!=transport.revision() || state->expected!=first;
        bool discontinuity=newGeneration || moved;
        bool controlsReset=false;
        bool controlChase=false;
        bool expressionBlocked=false;
        if(!transport.playing()) {
            pitchChase = 0;
            if (releaseControls(out, 0, newGeneration) && flush(out, 0, newGeneration))
                state->generation = generation;
            state->expected=-1;return;
        }
        if(discontinuity) {
            const bool changedControls=state->controllerGeneration!=controllerGeneration;
            if(moved || changedControls) {
                if(!releaseControls(out,0,newGeneration))return;
                invalidateControls();
                controlsReset=true;
                controlChase=true;
            }
            if(moved) {
                if(!flush(out,0,newGeneration))return;
            } else if(!adoptNotes(out,first))return;
            state->generation = generation;
            state->revision = transport.revision();
            state->controllerGeneration=controllerGeneration;
            state->renderedControllerGeneration.store(controllerGeneration,std::memory_order_release);
            if(newGeneration)for(auto& controlState:state->controls)
                if(controlState.pending && (controlState.retainedGeneration!=generation ||
                    controlState.retainedSerial!=controlState.serial)){controlState.pending=false;controlState.retry=true;}
            for(const auto& controlState:state->controls)controlChase=controlChase || controlState.retry;
        }
        for (auto &a : state->active)
            if (a.used && ((a.releasing && !a.pending) || a.note.off <= first))
                if (!stop(a, out, 0))
                    return;
        chasePitchAtStart(out, first, controlChase);
        for(auto& active:state->active)if(active.used && !active.sounding && active.retryOn && !active.releasing) {
            std::size_t owned=0;for(const auto& note:state->active)owned+=note.used?1u:0u;
            if(out.capacity()-out.size()<static_cast<std::int32_t>(owned)+3)return;
            auto event=active.note.start;event.frame=0;
            if(!out.push(event))return;
            active.retryOn=false;
            chaseExpression(active,first);
            if(active.note.tuning!=0 && active.note.expression[0].first==active.note.expression[0].last) {
                event.type=EventType::NoteExpression;event.dim=static_cast<std::uint16_t>(ExpressionDim::Pitch);
                event.value=active.note.tuning/100.0;out.push(event);
            }
        }
        std::int32_t frame=0;
        while(frame<frames) {
            const auto time=transport.sampleAt(frame);
            const auto count=transport.contiguousFrames(frame,frames-frame);
            if (frame > 0) {
                if (!releaseControls(out, frame) || !flush(out, frame))
                    return;
                invalidateControls();
                discontinuity = true;
                controlsReset=true;controlChase=true;
            }
            if (discontinuity) {
                if(controlChase)evaluateControls(out, frame, time);
                chase(out, frame, time);
            }
            const auto end=time+std::min(count,std::numeric_limits<std::int64_t>::max()-time);
            const auto* eventBegin=events.data();
            const auto* eventEnd=events.empty()?eventBegin:eventBegin+events.size();
            const auto* it=events.empty()?eventBegin:
                std::lower_bound(eventBegin,eventEnd,time,[](const auto& e,std::int64_t t){return e.time<t;});
            auto cursor = time;
            while (cursor < end) {
                const auto boundary = controls ? controls->nextBoundary(cursor, end) : end;
                auto due = std::min(it == eventEnd ? end : it->time, boundary);
                if (controls && controls->size()) {
                    const auto remainder = cursor % 64;
                    const auto step = remainder ? 64 - remainder : 0;
                    if (step <= end - cursor)
                        due = std::min(due, cursor + step);
                }
                const auto controllerTime=due;
                if(!expressionSamples.empty() && !expressionBlocked)due=nextExpression(cursor,due);
                if (due >= end)
                    break;
                const auto at = frame + static_cast<std::int32_t>(due - time);
                if (due == boundary && !(controlsReset && due == time) &&
                    !releaseControls(out, at, false, true, due))
                    return;
                // Releases precede the new controller state; note-ons follow it.
                for (; it != eventEnd && it->time == due && !it->on; ++it)
                    for (auto &active : state->active)
                        if (active.used && !active.pending &&
                            active.generation==generation &&
                            active.occurrence == notes[it->note].start.noteId)
                            stop(active, out, at);
                if(due==controllerTime)evaluateControls(out,at,due);
                for (; it != eventEnd && it->time == due; ++it)
                    if (it->on)
                        start(notes[it->note],out,at,due);
                if(!expressionSamples.empty() && !expressionBlocked)expressionBlocked=!emitExpression(out,at,due);
                cursor = due + 1;
            }
            frame += static_cast<std::int32_t>(count);
            discontinuity = false;
            controlsReset=false;
        }
        const auto last=transport.sampleAt(frames-1);
        state->expected=last==std::numeric_limits<std::int64_t>::max()?last:last+1;
    }
};
std::int64_t checkedSample(double seconds,double rate) {
    const double n=seconds*rate;
    if(!std::isfinite(n)||n<0||n>static_cast<double>(std::int64_t{1}<<50))throw std::runtime_error("MIDI time outside supported range");
    return static_cast<std::int64_t>(std::llround(std::nextafter(n,std::numeric_limits<double>::infinity())));
}
void mixExpressionKey(std::uint64_t& key,std::uint64_t value) noexcept {
    key=(key^value)*1099511628211ULL;
}
double expressionValueAt(const std::vector<ExpressionPoint>& points,double tick) noexcept {
    const auto next=std::upper_bound(points.begin(),points.end(),tick,
        [](double at,const auto& point){return at<static_cast<double>(point.time_ticks);});
    if(next==points.begin())return next->value;
    const auto& before=*(next-1);
    if(next==points.end())return before.value;
    const auto x=(tick-static_cast<double>(before.time_ticks))/
        (static_cast<double>(next->time_ticks)-static_cast<double>(before.time_ticks));
    return curveValue(before.value,next->value,x,before.curve,before.tension);
}
template <typename SecondsAt>
void prepareExpression(Occurrence& note,const ExpressionCurves& curves,std::vector<ExpressionSample>& samples,
                       const WarpArrangementClock& tempo,double rate,double originSeconds,const SecondsAt& secondsAt,
                       std::int64_t sourceStart,std::int64_t visibleStart) {
    constexpr auto tickLimit=std::int64_t{1}<<50;
    constexpr double ppq=5765760.0;
    const auto step=static_cast<std::int64_t>(std::floor(rate/500.0));
    const auto sampleAt=[&](std::int64_t tick) {
        const double time=secondsAt(sourceStart+tick);
        return checkedSample(std::max(0.0,time),rate);
    };
    const auto noteTickAt=[&](std::int64_t sample) {
        const double seconds=static_cast<double>(sample)/rate-originSeconds;
        const auto tick=tempo.secondsToTicks(seconds);
        const auto fraction=(seconds-tempo.ticksToSeconds(tick))*tempo.bpmAt(tick)*ppq/60.0;
        return static_cast<double>(tick-sourceStart)+fraction;
    };
    std::uint64_t identity=14695981039346656037ULL,timing=identity;
    for(std::size_t dimension=0;dimension<expressionDimensions;++dimension) {
        const auto* curve=curves[dimension];
        if(!curve || curve->points.empty())continue;
        const auto& points=curve->points;
        mixExpressionKey(identity,dimension);
        mixExpressionKey(identity,points.size());
        for(const auto& point:points) {
            if(point.time_ticks < -tickLimit || point.time_ticks > tickLimit)
                throw std::runtime_error("expression time outside supported musical range");
            mixExpressionKey(identity,static_cast<std::uint64_t>(point.time_ticks));
            mixExpressionKey(identity,std::bit_cast<std::uint32_t>(point.value));
            mixExpressionKey(identity,std::bit_cast<std::uint32_t>(point.tension));
            mixExpressionKey(identity,point.curve);
        }
        const auto first=samples.size();
        const double tuning=dimension==0?note.tuning/100.0:0.0;
        const auto append=[&](std::int64_t when,double value) {
            value+=tuning;
            if(samples.size()>first) {
                if(samples.back().time==when) {samples.back().value=value;return;}
                if(samples.back().value==value)return;
            }
            if(samples.size()==maxExpressionSamples)throw std::runtime_error("expression schedule budget exceeded");
            samples.push_back({when,value});
        };
        append(note.on,expressionValueAt(points,static_cast<double>(visibleStart-sourceStart)));
        for(std::size_t i=0;i<points.size();++i) {
            const auto& point=points[i];
            const auto left=sampleAt(point.time_ticks);
            if(left>=note.off)break;
            if(left>=note.on && sourceStart+point.time_ticks>=visibleStart)append(left,point.value);
            if(i+1==points.size() || point.curve==0 || point.value==points[i+1].value)continue;
            const auto right=std::min(note.off,sampleAt(points[i+1].time_ticks));
            const auto firstGrid=std::max(note.on,left)/step+1;
            if(right/step-firstGrid>static_cast<std::int64_t>(maxExpressionSamples))
                throw std::runtime_error("expression schedule budget exceeded");
            for(auto sample=firstGrid*step;sample<right;sample+=step)
                append(sample,expressionValueAt(points,noteTickAt(sample)));
        }
        note.expression[dimension]={first,samples.size()};
        mixExpressionKey(timing,dimension);
        for(std::size_t i=first;i<samples.size();++i) {
            mixExpressionKey(timing,static_cast<std::uint64_t>(samples[i].time));
            mixExpressionKey(timing,std::bit_cast<std::uint64_t>(samples[i].value));
        }
    }
    if(timing!=14695981039346656037ULL) {
        note.authored.expression=identity;
        note.expressionTiming=timing;
    }
}
}
struct MidiClipState {
    struct OwnedTrack {
        std::shared_ptr<TrackState> audio;
        std::vector<std::shared_ptr<const ControllerVersion>> controllers;
        std::vector<std::shared_ptr<const RouteVersion>> routes;
    };
    std::map<std::int64_t,OwnedTrack> tracks; // message thread only
    std::uint64_t generation=0,trackIdentity=0,controllerIdentity=0,routeIdentity=0;
};
std::shared_ptr<MidiClipState> makeMidiClipState(){return std::make_shared<MidiClipState>();}
struct MidiClips::Impl {
    std::map<std::int64_t,std::shared_ptr<Source>> tracks;
    std::vector<std::string> problems;
    std::unordered_set<std::int64_t> unavailableClips;
    std::weak_ptr<MidiClipState> state;
    const Transport* transport = nullptr;
    double rate = 0;
    bool honorMonitoring = false;
};
MidiClips::MidiClips(const rows::Model& model,Transport& transport,double rate,std::shared_ptr<MidiClipState> state,
                     const DeviceChainFn& eventPathFor)
    :MidiClips(model,transport,rate,std::move(state),eventPathFor,NoteExpressionsRead{}) {}
MidiClips::MidiClips(const rows::Model& model,Transport& transport,double rate,std::shared_ptr<MidiClipState> state,
                     const DeviceChainFn& eventPathFor,const NoteExpressionsRead& expressions,bool honorMonitoring)
    :MidiClips(model,transport,rate,std::move(state),eventPathFor,expressions,honorMonitoring,nullptr,{}) {}
MidiClips::MidiClips(const rows::Model& model,Transport& transport,double rate,std::shared_ptr<MidiClipState> state,
                     const DeviceChainFn& eventPathFor,const NoteExpressionsRead& expressions,
                     const MidiClips* previous,const std::set<std::int64_t>& changedTracks)
    :MidiClips(model,transport,rate,std::move(state),eventPathFor,expressions,
               previous && previous->impl_->honorMonitoring,previous,changedTracks) {}
MidiClips::MidiClips(const rows::Model& model,Transport& transport,double rate,std::shared_ptr<MidiClipState> state,
                     const DeviceChainFn& eventPathFor,const NoteExpressionsRead& expressions,bool honorMonitoring,
                     const MidiClips* previous,const std::set<std::int64_t>& changedTracks)
    :impl_(std::make_shared<Impl>()) {
    if(!std::isfinite(rate)||rate<44100||rate>768000)throw std::runtime_error("MIDI session rate outside 44.1-768 kHz");
    impl_->state=state;
    impl_->transport=&transport;
    impl_->rate=rate;
    impl_->honorMonitoring=honorMonitoring;
    // The controller compiler has a project-wide budget. Preserve its order
    // and any old projection diagnostics by rebuilding the complete sources
    // when either needs to be reconsidered.
    const bool reuse = previous && previous->impl_->state.lock() == state &&
                       previous->impl_->transport == &transport && previous->impl_->rate == rate &&
                       previous->impl_->problems.empty() && model.controllers.empty();
    const auto generation=++state->generation;
    // Include empty tracks: deleting or muting the last clip must still flush
    // the notes owned by the preceding published source generation.
    for(const auto& track:model.tracks) {
        const bool meterNotes = track.kind == "midi" || track.kind == "instrument";
        const bool monitorInputOnly = honorMonitoring && track.monitorMode == 1 && meterNotes;
        if (reuse && !changedTracks.contains(track.id)) {
            const auto old = previous->impl_->tracks.find(track.id);
            if (old != previous->impl_->tracks.end() && old->second->meterNotes == meterNotes &&
                old->second->monitorInputOnly == monitorInputOnly) {
                impl_->tracks.emplace(track.id, old->second);
                continue;
            }
        }
        auto& s=state->tracks[track.id].audio;if(!s) {s=std::make_shared<TrackState>();s->prefix=(++state->trackIdentity)<<32;}
        auto source = std::make_shared<Source>(transport, s, generation);
        // The realizer retains each source independently. A replacement graph
        // can share an unchanged track without keeping old changed tracks alive.
        source->owner = source;
        source->meterNotes = meterNotes;
        source->monitorInputOnly = monitorInputOnly;
        impl_->tracks.emplace(track.id, std::move(source));
    }
    MidiControllerPrograms controls(model, rate);
    impl_->problems.insert(impl_->problems.end(), controls.problems().begin(), controls.problems().end());
    // Exact owner-side comparison, never a hash collision or callback walk of
    // the authored streams. A clock-only rebuild retains each controller's
    // delivery/retry state, including sustain already held by the instrument.
    for(auto& [id,source]:impl_->tracks) {
        if (source->generation != generation)
            continue;
        ControllerIdentity identity;identity.rate=rate;
        if (!source->monitorInputOnly && !model.controllers.empty()) {
            source->controls = controls.forTrack(id);
            for(const auto& clip:timelineClips(model)) {
                if(clip.trackId!=id)continue;
                bool used=false;
                for(const auto& lane:model.controllers)if(lane.clipId==clip.id) {
                    identity.lanes.push_back(lane);used=true;
                }
                if(used)identity.clips.push_back(clipClock(clip));
            }
            std::sort(identity.clips.begin(),identity.clips.end(),[](const auto& a,const auto& b){return a.id<b.id;});
            std::sort(identity.lanes.begin(),identity.lanes.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        }
        auto& versions=state->tracks[id].controllers;
        // The last graph may have been acquired without being rendered, then
        // collected. Keep the controller identity the audio state actually
        // adopted even after that graph dies; the callback publishes a token
        // only, never copies or releases a shared_ptr.
        const auto rendered=source->state->renderedControllerGeneration.load(std::memory_order_acquire);
        std::erase_if(versions,[&](const auto& version){return version.use_count()==1 && version->generation!=rendered;});
        // Controller versions retain the route actually adopted by the audio
        // state, even if its graph was collected before another was rendered.
        // Compare actual nodes: an undone parked row can have a fresh instance.
        auto& routes=state->tracks[id].routes;
        std::erase_if(routes,[](const auto& route){return route.use_count()==1;});
        const auto path=eventPathFor?eventPathFor(id):std::vector<Node*>{};
        for(const auto& route:routes)if(route->path==path){identity.route=route;break;}
        if(!identity.route) {
            identity.route=std::make_shared<RouteVersion>(RouteVersion{path,++state->routeIdentity});
            routes.push_back(identity.route);
        }
        for(const auto& version:versions)if(version->identity==identity) {
            source->controllerVersion=version;break;
        }
        if(!source->controllerVersion) {
            source->controllerVersion=std::make_shared<ControllerVersion>(ControllerVersion{std::move(identity),++state->controllerIdentity});
            versions.push_back(source->controllerVersion);
        }
        source->controllerGeneration=source->controllerVersion->generation;
    }
    const WarpArrangementClock tempo(model.tempo,rate);
    std::uint64_t identity=1;
    std::map<std::int64_t, std::vector<const rows::Note*>> notesByClip;
    for (const auto& clip : timelineClips(model)) {
        const auto source = impl_->tracks.find(clip.trackId);
        if (clip.kind == "midi" && !clip.muted && source != impl_->tracks.end() && source->second->generation == generation)
            notesByClip.try_emplace(clip.id);
    }
    std::map<std::pair<std::int64_t,std::uint64_t>,std::size_t> identities;
    for (const auto& note : model.notes) {
        const auto notes = notesByClip.find(note.clipId);
        if (notes == notesByClip.end())
            continue;
        notes->second.push_back(&note);
        if (note.noteId)
            ++identities[{note.clipId, note.noteId}];
    }
    std::map<std::pair<std::int64_t,std::uint64_t>,ExpressionCurves> noteCurves;
    impl_->problems.insert(impl_->problems.end(),expressions.problems.begin(),expressions.problems.end());
    for(const auto& curve:expressions.curves) {
        const auto key=std::pair{curve.clipId,curve.noteId};
        const auto count=identities.find(key);
        if(count==identities.end() || count->second!=1 || curve.dimension<0 ||
           static_cast<std::uint64_t>(curve.dimension)>=expressionDimensions)continue;
        noteCurves[key][static_cast<std::size_t>(curve.dimension)]=&curve;
    }
    for(const auto& clip:timelineClips(model)) {
        if(clip.kind!="midi" || clip.muted)continue;
        const auto found=impl_->tracks.find(clip.trackId);
        if(found==impl_->tracks.end() || found->second->generation!=generation)continue;
        const auto notes = notesByClip.find(clip.id);
        if (notes == notesByClip.end())
            continue;
        auto& source=*found->second;
        const auto before=source.notes.size();
        const auto expressionBefore=source.expressionSamples.size();
        try {
            if(clip.aliasOf)throw std::runtime_error("MIDI alias unsupported; silent");
            auto origin = clip.posTicks.value_or(0);
            auto length = clip.lengthTicks.value_or(0);
            double startSeconds = tempo.ticksToSeconds(origin), endSeconds = startSeconds;
            if (clip.timeBase == 1) {
                const auto begin = clip.posNs.value_or(-1);
                const auto extent = clip.lengthNs.value_or(0);
                if (begin < 0 || extent <= 0 || begin > INT64_MAX - extent)
                    throw std::runtime_error("invalid linear MIDI clip time; silent");
                const auto end = begin + extent;
                startSeconds = static_cast<double>(begin) * 1e-9;
                endSeconds = static_cast<double>(end) * 1e-9;
                origin = tempo.storedNanosecondsToTicks(begin);
                length = tempo.storedNanosecondsToTicks(end) - origin;
            }
            constexpr auto tickLimit=std::int64_t{1}<<50;
            if(origin<0||origin>tickLimit||length<=0||length>tickLimit||clip.contentOffsetTicks < -tickLimit||clip.contentOffsetTicks>tickLimit)throw std::runtime_error("invalid MIDI clip time; silent");
            const auto loop=clip.loopLenTicks.value_or(0);
            if(clip.loopEnabled && (loop<=0||loop>tickLimit||clip.loopStartTicks<0||clip.loopStartTicks>tickLimit))throw std::runtime_error("invalid MIDI clip loop; silent");
            if (!clip.loopEnabled && clip.endMarkerTicks &&
                (*clip.endMarkerTicks <= clip.contentOffsetTicks || *clip.endMarkerTicks > tickLimit))
                throw std::runtime_error("invalid MIDI clip End marker; silent");
            const auto tempoAtOrigin = tempo.ticksToSeconds(origin);
            // One conversion keeps note edges and curve knots on the same
            // side of the sample rounding boundary.
            const auto secondsAt = [&](std::int64_t tick) {
                return startSeconds + tempo.ticksToSeconds(tick) - tempoAtOrigin;
            };
            const auto at = [&](std::int64_t local) { return checkedSample(secondsAt(origin + local),rate); };
            const auto clipEnd=clip.timeBase==0?at(length):checkedSample(endSeconds,rate);
            const auto clipStart = at(0);
            if (clipEnd > clipStart)
                source.monitorRanges.push_back({clipStart, clipEnd});
            // Keep the source with its previous driver state. An empty note
            // and controller program sends releases on the first new block.
            if (source.monitorInputOnly)
                continue;
            bool unsupported=false;
            std::string expressionProblem;
            const double tempoOrigin=startSeconds-tempoAtOrigin;
            for(const auto* record : notes->second) {
                const auto& note = *record;
                if((note.flags&1u)!=0)continue;
                // ANOT starts are signed. A note crossing source zero is
                // chased by add()'s visible-start clamp; an earlier finished
                // note is skipped. Keep the same bounded arithmetic domain.
                if(note.startTicks < -tickLimit||note.startTicks>tickLimit||note.durTicks<=0||note.durTicks>tickLimit||note.key<0||note.key>127||note.channel<0||note.channel>15||note.velOn<1||note.velOn>127||note.velOff<0||note.velOff>127||!std::isfinite(note.tuningCents))throw std::runtime_error("invalid MIDI note; clip silent");
                unsupported=unsupported || note.probability!=10000 || (note.flags&12u)!=0;
                auto add=[&](std::int64_t on,std::int64_t off,std::int64_t authoredStart){
                    const auto repeat=on;
                    on=std::max<std::int64_t>(on,0);
                    if(off<=on || on>length)return;
                    // A linear clip endpoint must not pass through whole ticks:
                    // that truncation can cross the final sample's rounding tie.
                    Occurrence n;n.on=at(on);n.off=off>length?clipEnd:std::min(at(off),clipEnd);
                    if(n.off<=n.on)return;
                    n.start.type=EventType::NoteOn;n.start.noteId=identity++;n.start.channel=static_cast<std::uint8_t>(note.channel);n.start.dim=static_cast<std::uint16_t>(note.key);n.start.value=static_cast<double>(note.velOn)/127;
                    n.stop=n.start;n.stop.type=EventType::NoteOff;n.stop.value=static_cast<double>(note.velOff)/127;n.tuning=note.tuningCents;
                    // Zero/duplicate legacy identities remain playable but can
                    // never authorize adoption of another stored note's voice.
                    const auto stable=note.noteId && identities.at({note.clipId,note.noteId})==1?note.noteId:0;
                    n.identity={clip.id,repeat,stable};
                    n.authored={clipClock(clip),note.startTicks,note.durTicks,note.key,note.channel,
                                note.velOn,note.velOff,note.probability,note.flags,note.tuningCents,rate,
                                source.controllerVersion->identity.route->generation,0};
                    if(const auto foundCurve=noteCurves.find({clip.id,stable});foundCurve!=noteCurves.end()) {
                        const auto sampleCount=source.expressionSamples.size();
                        try {prepareExpression(n,foundCurve->second,source.expressionSamples,tempo,rate,tempoOrigin,
                                               secondsAt,origin+authoredStart,origin+on);}
                        catch(const std::exception& error) {
                            source.expressionSamples.resize(sampleCount);n.expression={};
                            expressionProblem=error.what();
                        }
                    }
                    if(source.notes.size()>=maxOccurrences)throw std::runtime_error("MIDI occurrence budget exceeded; clip silent");
                    source.notes.push_back(n);
                };
                if (!clip.loopEnabled) {
                    auto off = note.startTicks + note.durTicks;
                    if (clip.endMarkerTicks) off = std::min(off, *clip.endMarkerTicks);
                    const auto start=note.startTicks-clip.contentOffsetTicks;
                    add(start,off-clip.contentOffsetTicks,start);
                    continue;
                }
                // Prefix before the loop plays once. Notes crossing a repeat
                // boundary are released there, and chased at the next origin.
                const auto lb=clip.loopStartTicks, le=lb+loop, offset=clip.contentOffsetTicks;
                if(offset<lb)add(note.startTicks-offset,std::min(note.startTicks+note.durTicks,le)-offset,note.startTicks-offset);
                if(note.startTicks>=le||note.startTicks+note.durTicks<=lb)continue;
                const auto firstRepeat=offset<lb?le-offset:-(offset-lb)%loop;
                for(auto base=firstRepeat;base<length;base+=loop) {
                    add(base+std::max(note.startTicks,lb)-lb,base+std::min(note.startTicks+note.durTicks,le)-lb,
                        base+note.startTicks-lb);
                    if(static_cast<std::uint64_t>((length-base)/loop)>maxOccurrences)throw std::runtime_error("MIDI repeat budget exceeded; clip silent");
                }
            }
            if(unsupported)impl_->problems.push_back("clips#"+std::to_string(clip.id)+": probability and ties not rendered; notes play independently at full probability");
            if(!expressionProblem.empty())impl_->problems.push_back("clips#"+std::to_string(clip.id)+": "+expressionProblem);
        } catch(const std::exception& e) {
            source.notes.resize(before);source.expressionSamples.resize(expressionBefore);
            impl_->unavailableClips.insert(clip.id);
            impl_->problems.push_back("clips#"+std::to_string(clip.id)+": "+e.what());
        }
    }
    for(auto& [id,source]:impl_->tracks) {
        (void)id;
        if (source->generation != generation)
            continue;
        auto& ranges = source->monitorRanges;
        std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
        auto merged = ranges.begin();
        for (const auto range : ranges) {
            if (merged == ranges.begin() || (merged - 1)->end < range.start)
                *merged++ = range;
            else
                (merged - 1)->end = std::max((merged - 1)->end, range.end);
        }
        ranges.erase(merged, ranges.end());
        std::stable_sort(source->notes.begin(),source->notes.end(),[](const auto& a,const auto& b){return a.on<b.on;});
        for(std::size_t i=0;i<source->notes.size();++i) {
            source->events.push_back({source->notes[i].on,i,true});source->events.push_back({source->notes[i].off,i,false});
            if(source->notes[i].identity.note)source->byIdentity.push_back(i);
        }
        std::sort(source->byIdentity.begin(),source->byIdentity.end(),[&notes=source->notes](std::size_t a,std::size_t b){return notes[a].identity<notes[b].identity;});
        std::stable_sort(source->events.begin(),source->events.end(),[](const auto& a,const auto& b){return a.time==b.time?a.on<b.on:a.time<b.time;});
    }
}
MidiClips::~MidiClips()=default;
std::uint32_t MidiClips::droppedNotes() const noexcept {
    std::uint32_t count=0;for(const auto& [id,source]:impl_->tracks){(void)id;count+=source->state->dropped.load(std::memory_order_relaxed);}return count;
}
std::uint64_t MidiClips::droppedControllers() const noexcept {
    std::uint64_t count = 0;
    for (const auto &[id, source] : impl_->tracks) {
        (void)id;
        count += source->state->droppedControls.load(std::memory_order_relaxed);
    }
    return count;
}
std::vector<Node*> MidiClips::sourcesFor(std::int64_t id){const auto i=impl_->tracks.find(id);return i==impl_->tracks.end()?std::vector<Node*>{}:std::vector<Node*>{i->second.get()};}
std::shared_ptr<std::uint16_t> MidiClips::pitchChaseFor(std::int64_t track) const noexcept {
    if (const auto source = impl_->tracks.find(track); source != impl_->tracks.end())
        return {impl_, &source->second->pitchChase};
    return {};
}
std::span<const MidiMonitorRange> MidiClips::monitorRanges(std::int64_t track) const noexcept {
    if (const auto source = impl_->tracks.find(track); source != impl_->tracks.end())
        return source->second->monitorRanges;
    return {};
}
MidiNoteActivity MidiClips::noteActivity(std::int64_t track) const noexcept {
    const auto source = impl_->tracks.find(track);
    if (source == impl_->tracks.end() || !source->second->meterNotes)
        return {};
    const auto reading = source->second->state->noteActivity.load(std::memory_order_relaxed);
    return {static_cast<std::uint32_t>(reading >> 1), (reading & 1u) != 0};
}
const std::vector<std::string>& MidiClips::problems() const noexcept{return impl_->problems;}
bool MidiClips::clipUnavailable(std::int64_t clipId) const noexcept{return impl_->unavailableClips.contains(clipId);}
}
