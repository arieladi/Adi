// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/midi_clips.hpp"
#include "adi/blob.hpp"
#include "adi/engine/midi_controllers.hpp"
#include "adi/engine/snapshot.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
namespace adi::engine {
namespace {
constexpr std::size_t maxActive = 128;
constexpr std::size_t maxOccurrences = 1000000;
struct Occurrence { std::int64_t on=0, off=0; Event start, stop; double tuning=0; };
struct Active { bool used=false, pending=false, releasing=false; std::uint64_t occurrence=0; Occurrence note; };
struct ControlState {
    double requestedValue = 0, confirmedValue = 0;
    std::uint32_t serial = 0, confirmedSeq = 0;
    bool requestedValid = false, pending = false, retry = false, held = false, releaseDesired = false,
         confirmedKnown = false;
};
struct TrackState {
    std::array<ControlState, 2048> controls{};
    const std::uint32_t controlPrefix = allocateControllerSourcePrefix();
    std::uint16_t controlCursor = 0;
    std::atomic<std::uint32_t> droppedControls{0};
    std::array<Active,maxActive> active{};
    std::uint64_t generation=0, revision=0;
    std::int64_t expected=-1;
    std::uint64_t prefix=0;
    std::uint32_t serial=0;
    std::atomic<std::uint32_t> dropped{0};
};
struct Scheduled { std::int64_t time; std::size_t note; bool on; };
class Source final : public Node {
public:
    Transport& transport;
    std::shared_ptr<TrackState> state;
    std::uint64_t generation;
    std::vector<Occurrence> notes;
    std::vector<Scheduled> events;
    std::shared_ptr<const MidiControllerProgram> controls;
    std::uint32_t controllerSourcePrefix() const noexcept override {
        return state->controlPrefix;
    }
    static bool pedal(std::uint16_t address) noexcept {
        const auto cc = address % 128;
        return cc == 64 || cc == 66 || cc == 69;
    }
    bool control(EventList &out, std::int32_t frame, std::uint16_t address, double value,
                 bool force = false) noexcept {
        auto &c = state->controls[address];
        if (!force && c.requestedValid && !c.retry && c.requestedValue == value)
            return true;
        Event e;
        e.type = EventType::Controller;
        e.frame = frame;
        e.channel = static_cast<std::uint8_t>(address / 128);
        e.paramId = address % 128;
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
        if (pedal(address)) {
            if (value > 0)
                c.held = true;
            c.releaseDesired = value == 0;
        }
        return true;
    }
    void controllerDelivered(const Event &e) noexcept override {
        if (e.type != EventType::Controller || e.channel > 15 || e.paramId > 127 ||
            (e.noteId >> 32) != state->controlPrefix)
            return;
        auto &c = state->controls[e.channel * 128 + e.paramId];
        const auto seq = static_cast<std::uint32_t>(e.noteId);
        const auto delta = seq - c.confirmedSeq;
        if (!c.confirmedKnown || (delta != 0 && delta < 0x80000000u)) {
            c.confirmedKnown = true;
            c.confirmedSeq = seq;
            c.confirmedValue = e.value;
        }
        if (seq == c.serial) {
            c.pending = false;
            if (e.value == 0 && !c.retry)
                c.held = false;
        }
    }
    void controllerRejected(const Event &e) noexcept override {
        if (e.type != EventType::Controller || e.channel > 15 || e.paramId > 127 ||
            (e.noteId >> 32) != state->controlPrefix)
            return;
        auto &c = state->controls[e.channel * 128 + e.paramId];
        if (static_cast<std::uint32_t>(e.noteId) != c.serial)
            return;
        c.pending = false;
        c.retry = true;
        if (pedal(static_cast<std::uint16_t>(e.channel * 128 + e.paramId)) && e.value == 0) {
            c.held = true;
            c.releaseDesired = true;
        }
    }
    bool releasePedals(EventList &out, std::int32_t frame, bool force = false, bool changedOnly = false,
                       std::int64_t time = 0) noexcept {
        bool ok = true;
        if (changedOnly) {
            if (!controls)
                return true;
            for (std::size_t i = 0; i < controls->size(); ++i) {
                const auto address = controls->address(i);
                if (pedal(address) && controls->changesOwnerAt(i, time)) {
                    auto &c = state->controls[address];
                    if (c.held || c.retry)
                        ok = control(out, frame, address, 0, force) && ok;
                    c.requestedValid = false;
                }
            }
        } else
            for (std::uint16_t channel = 0; channel < 16; ++channel)
                for (auto cc : {64, 66, 69}) {
                    const auto address = static_cast<std::uint16_t>(channel * 128 + cc);
                    auto &c = state->controls[address];
                    if (c.held || c.retry)
                        ok = control(out, frame, address, 0, force) && ok;
                }
        return ok;
    }
    void invalidateControls() noexcept {
        if (controls)
            for (std::size_t i = 0; i < controls->size(); ++i)
                state->controls[controls->address(i)].requestedValid = false;
    }
    void evaluateControls(EventList &out, std::int32_t frame, std::int64_t time) noexcept {
        if (!controls || !controls->size())
            return;
        // Keep room for note-ons and all owned note-offs. Rotate the first
        // address so a small output list cannot starve later controllers.
        const auto reserve =
            std::min(out.capacity(),
                     std::max(3, std::min(out.capacity() / 2, static_cast<std::int32_t>(maxActive * 2))));
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
    void start(const Occurrence& n,EventList& out,std::int32_t frame) noexcept {
        // Reserve enough room for every currently owned off, even when an
        // adversarial clip floods one callback. Refused ons cannot hang.
        std::size_t active=0;for(const auto& a:state->active)active+=a.used?1u:0u;
        if(out.capacity()-out.size()<static_cast<std::int32_t>(active)+3){++state->dropped;return;}
        if(state->serial==std::numeric_limits<std::uint32_t>::max()){++state->dropped;return;}
        for(auto& a:state->active)if(!a.used) {
            auto e=n.start;e.frame=frame;e.noteId=state->prefix | ++state->serial;
            if(!out.push(e))return;
            a.note=n;a.used=true;a.pending=false;a.releasing=false;a.occurrence=n.start.noteId;
            a.note.start.noteId=e.noteId;a.note.stop.noteId=e.noteId;
            if(n.tuning!=0) {
                e.type=EventType::NoteExpression;e.dim=static_cast<std::uint16_t>(ExpressionDim::Pitch);
                e.value=n.tuning/100.0;out.push(e);
            }
            return;
        }
        ++state->dropped;
    }
    void chase(EventList& out,std::int32_t frame,std::int64_t time) noexcept {
        for(const auto& n:notes) {
            if(n.on>=time)break;
            if(n.off>time)start(n,out,frame);
        }
    }
    void noteOffDelivered(const Event& e) noexcept override {
        for(auto& a:state->active)if(a.used && a.note.start.noteId==e.noteId)a.used=false;
    }
    void noteOffRejected(const Event& e) noexcept override {
        for(auto& a:state->active)if(a.used && a.note.start.noteId==e.noteId)a.pending=false;
    }
    void sourceEvents(EventList& out,std::int32_t frames) noexcept override {
        if (frames <= 0)
            return;
        // A rejected zero must remain owned even after its lane disappears.
        for (std::uint16_t channel = 0; channel < 16; ++channel)
            for (auto cc : {64, 66, 69}) {
                const auto address = static_cast<std::uint16_t>(channel * 128 + cc);
                const auto &c = state->controls[address];
                if (c.releaseDesired && c.retry && !control(out, 0, address, 0))
                    return;
            }
        const auto first=transport.sampleAt(0);
        bool discontinuity=state->generation!=generation || state->revision!=transport.revision() || state->expected!=first;
        const bool newGeneration = state->generation != generation;
        if(!transport.playing()) {
            if (releasePedals(out, 0, newGeneration) && flush(out, 0, newGeneration))
                state->generation = generation;
            state->expected=-1;return;
        }
        if(discontinuity) {
            if (!releasePedals(out, 0, newGeneration) || !flush(out, 0, newGeneration))
                return;
            invalidateControls();
            state->generation = generation;
            state->revision = transport.revision();
        }
        for (auto &a : state->active)
            if (a.used && ((a.releasing && !a.pending) || a.note.off <= first))
                if (!stop(a, out, 0))
                    return;
        std::int32_t frame=0;
        while(frame<frames) {
            const auto time=transport.sampleAt(frame);
            const auto count=transport.contiguousFrames(frame,frames-frame);
            if (frame > 0) {
                if (!releasePedals(out, frame) || !flush(out, frame))
                    return;
                invalidateControls();
                discontinuity = true;
            }
            if (discontinuity) {
                evaluateControls(out, frame, time);
                chase(out, frame, time);
            }
            const auto end=time+std::min(count,std::numeric_limits<std::int64_t>::max()-time);
            auto it=std::lower_bound(events.begin(),events.end(),time,[](const auto& e,std::int64_t t){return e.time<t;});
            auto cursor = time;
            while (cursor < end) {
                const auto boundary = controls ? controls->nextBoundary(cursor, end) : end;
                auto due = std::min(it == events.end() ? end : it->time, boundary);
                if (controls && controls->size()) {
                    const auto remainder = cursor % 64;
                    const auto step = remainder ? 64 - remainder : 0;
                    if (step <= end - cursor)
                        due = std::min(due, cursor + step);
                }
                if (due >= end)
                    break;
                const auto at = frame + static_cast<std::int32_t>(due - time);
                if (due == boundary && !(discontinuity && due == time) &&
                    !releasePedals(out, at, false, true, due))
                    return;
                // Releases precede the new controller state; note-ons follow it.
                for (; it != events.end() && it->time == due && !it->on; ++it)
                    for (auto &active : state->active)
                        if (active.used && !active.pending &&
                            active.occurrence == notes[it->note].start.noteId)
                            stop(active, out, at);
                evaluateControls(out, at, due);
                for (; it != events.end() && it->time == due; ++it)
                    if (it->on)
                        start(notes[it->note], out, at);
                cursor = due + 1;
            }
            frame += static_cast<std::int32_t>(count);
            discontinuity = false;
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
}
struct MidiClipState {
    std::map<std::int64_t,std::shared_ptr<TrackState>> tracks; // message thread only
    std::uint64_t generation=0, trackIdentity=0;
};
std::shared_ptr<MidiClipState> makeMidiClipState(){return std::make_shared<MidiClipState>();}
struct MidiClips::Impl {
    std::map<std::int64_t,std::unique_ptr<Source>> tracks;
    std::vector<std::string> problems;
};
MidiClips::MidiClips(const rows::Model& model,Transport& transport,double rate,std::shared_ptr<MidiClipState> state)
    :impl_(std::make_shared<Impl>()) {
    if(!std::isfinite(rate)||rate<44100||rate>768000)throw std::runtime_error("MIDI session rate outside 44.1-768 kHz");
    const auto generation=++state->generation;
    // Include empty tracks: deleting or muting the last clip must still flush
    // the notes owned by the preceding published source generation.
    for(const auto& track:model.tracks) {
        auto& s=state->tracks[track.id];if(!s) {s=std::make_shared<TrackState>();s->prefix=(++state->trackIdentity)<<32;}
        impl_->tracks.emplace(track.id,std::make_unique<Source>(transport,s,generation));
    }
    MidiControllerPrograms controls(model, rate);
    impl_->problems.insert(impl_->problems.end(), controls.problems().begin(), controls.problems().end());
    for (auto &[id, source] : impl_->tracks)
        source->controls = controls.forTrack(id);
    TempoMap tempo;for(const auto& e:model.tempo)tempo.events.push_back({e.posTicks,e.bpm,static_cast<int>(e.curve)});
    if(tempo.events.empty())tempo.events.push_back({0,120,0});
    if(std::any_of(tempo.events.begin(),tempo.events.end(),[](const auto& e){return e.curve!=0;}))
        impl_->problems.push_back("MIDI tempo ramps unsupported; timing uses step tempos");
    std::uint64_t identity=1;
    for(const auto& clip:model.clips) {
        if(clip.kind!="midi" || clip.muted)continue;
        const auto found=impl_->tracks.find(clip.trackId);if(found==impl_->tracks.end())continue;
        auto& source=*found->second;
        const auto before=source.notes.size();
        try {
            if(clip.aliasOf)throw std::runtime_error("MIDI alias unsupported; silent");
            const double startSeconds=clip.timeBase==0?tempo.ticksToSeconds(clip.posTicks.value_or(0)):static_cast<double>(clip.posNs.value_or(0))*1e-9;
            const auto origin=clip.timeBase==0?clip.posTicks.value_or(0):tempo.secondsToTicks(startSeconds);
            const auto length=clip.timeBase==0?clip.lengthTicks.value_or(0):tempo.secondsToTicks(startSeconds+static_cast<double>(clip.lengthNs.value_or(0))*1e-9)-origin;
            constexpr auto tickLimit=std::int64_t{1}<<50;
            if(origin<0||origin>tickLimit||length<=0||length>tickLimit||clip.contentOffsetTicks<0||clip.contentOffsetTicks>tickLimit)throw std::runtime_error("invalid MIDI clip time; silent");
            const auto loop=clip.loopLenTicks.value_or(0);
            if(clip.loopEnabled && (loop<=0||loop>tickLimit||clip.loopStartTicks<0||clip.loopStartTicks>tickLimit))throw std::runtime_error("invalid MIDI clip loop; silent");
            if (!clip.loopEnabled && clip.endMarkerTicks &&
                (*clip.endMarkerTicks <= clip.contentOffsetTicks || *clip.endMarkerTicks > tickLimit))
                throw std::runtime_error("invalid MIDI clip End marker; silent");
            auto at=[&](std::int64_t local){return checkedSample(startSeconds+tempo.ticksToSeconds(origin+local)-tempo.ticksToSeconds(origin),rate);};
            const auto clipEnd=clip.timeBase==0?at(length):checkedSample(startSeconds+static_cast<double>(clip.lengthNs.value_or(0))*1e-9,rate);
            bool unsupported=false;
            for(const auto& note:model.notes) {
                if(note.clipId!=clip.id || (note.flags&1u)!=0)continue;
                // ANOT starts are signed. A note crossing source zero is
                // chased by add()'s visible-start clamp; an earlier finished
                // note is skipped. Keep the same bounded arithmetic domain.
                if(note.startTicks < -tickLimit||note.startTicks>tickLimit||note.durTicks<=0||note.durTicks>tickLimit||note.key<0||note.key>127||note.channel<0||note.channel>15||note.velOn<1||note.velOn>127||note.velOff<0||note.velOff>127||!std::isfinite(note.tuningCents))throw std::runtime_error("invalid MIDI note; clip silent");
                unsupported=unsupported || note.probability!=10000 || (note.flags&12u)!=0;
                auto add=[&](std::int64_t on,std::int64_t off){
                    on=std::max<std::int64_t>(on,0);
                    if(off<=on || on>length)return;
                    // A linear clip endpoint must not pass through whole ticks:
                    // that truncation can cross the final sample's rounding tie.
                    Occurrence n;n.on=at(on);n.off=off>length?clipEnd:std::min(at(off),clipEnd);
                    if(n.off<=n.on)return;
                    n.start.type=EventType::NoteOn;n.start.noteId=identity++;n.start.channel=static_cast<std::uint8_t>(note.channel);n.start.dim=static_cast<std::uint16_t>(note.key);n.start.value=static_cast<double>(note.velOn)/127;
                    n.stop=n.start;n.stop.type=EventType::NoteOff;n.stop.value=static_cast<double>(note.velOff)/127;n.tuning=note.tuningCents;
                    if(source.notes.size()>=maxOccurrences)throw std::runtime_error("MIDI occurrence budget exceeded; clip silent");
                    source.notes.push_back(n);
                };
                if (!clip.loopEnabled) {
                    auto off = note.startTicks + note.durTicks;
                    if (clip.endMarkerTicks) off = std::min(off, *clip.endMarkerTicks);
                    add(note.startTicks-clip.contentOffsetTicks, off-clip.contentOffsetTicks);
                    continue;
                }
                // Prefix before the loop plays once. Notes crossing a repeat
                // boundary are released there, and chased at the next origin.
                const auto lb=clip.loopStartTicks, le=lb+loop, offset=clip.contentOffsetTicks;
                if(offset<lb)add(note.startTicks-offset,std::min(note.startTicks+note.durTicks,le)-offset);
                if(note.startTicks>=le||note.startTicks+note.durTicks<=lb)continue;
                const auto firstRepeat=offset<lb?le-offset:-(offset-lb)%loop;
                for(auto base=firstRepeat;base<length;base+=loop) {
                    add(base+std::max(note.startTicks,lb)-lb,base+std::min(note.startTicks+note.durTicks,le)-lb);
                    if(static_cast<std::uint64_t>((length-base)/loop)>maxOccurrences)throw std::runtime_error("MIDI repeat budget exceeded; clip silent");
                }
            }
            if(unsupported)impl_->problems.push_back("clips#"+std::to_string(clip.id)+": probability, ties and stored expression curves not rendered; notes play independently at full probability");
        } catch(const std::exception& e) {
            source.notes.resize(before);impl_->problems.push_back("clips#"+std::to_string(clip.id)+": "+e.what());
        }
    }
    for(auto& [id,source]:impl_->tracks) {
        (void)id;source->owner=impl_;
        std::stable_sort(source->notes.begin(),source->notes.end(),[](const auto& a,const auto& b){return a.on<b.on;});
        for(std::size_t i=0;i<source->notes.size();++i) {
            source->events.push_back({source->notes[i].on,i,true});source->events.push_back({source->notes[i].off,i,false});
        }
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
const std::vector<std::string>& MidiClips::problems() const noexcept{return impl_->problems;}
}
