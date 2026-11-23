// SPDX-License-Identifier: GPL-3.0-or-later
//
// See mpe_input.hpp. ADR-0054: no MIDI byte survives this file.

#include "adi/engine/mpe_input.hpp"

#include <limits>

namespace adi::engine {

MpeZone MpeZone::lower(std::uint8_t memberCount) noexcept {
    MpeZone z;
    z.active = true;
    z.masterChan = 1;
    z.firstMember = 2;
    const int last = 1 + static_cast<int>(memberCount);
    z.lastMember = static_cast<std::uint8_t>(last > 16 ? 16 : last);
    return z;
}

// ---------------------------------------------------------------------------
// The conversions
// ---------------------------------------------------------------------------

double bendToSemitones(std::uint16_t word14, double rangeSemitones) noexcept {
    // Centre is 8192, so there are 8192 steps below it and 8191 above. Using
    // 8192 for both makes full-up reach only 8191/8192 of the range -- about
    // 0.7 cents short at ±48 semitones, which is inaudible on its own and
    // wrong in a way that compounds when a value is round-tripped.
    const double v = static_cast<double>(word14);
    if (v >= 8192.0) return ((v - 8192.0) / 8191.0) * rangeSemitones;
    return ((v - 8192.0) / 8192.0) * rangeSemitones;
}

double cc14ToUnit(std::uint16_t word14) noexcept {
    return static_cast<double>(word14) / 16383.0;
}

double cc7ToUnit(std::uint8_t v) noexcept {
    return static_cast<double>(v) / 127.0;
}

// ---------------------------------------------------------------------------

void MpeParser::reset() noexcept {
    for (auto& c : chans_) c = ChannelState{};
    // nextNoteId_ is NOT reset. A note id must never be reused within a
    // session: expression already queued against an old id would otherwise
    // land on a new note, which is an audible wrong-note rather than a
    // dropped one.
}

void MpeParser::setZone(const MpeZone& z) noexcept {
    if(zone_.active!=z.active || zone_.masterChan!=z.masterChan ||
       zone_.firstMember!=z.firstMember || zone_.lastMember!=z.lastMember) {
        for(auto& state:chans_) {
            state.idle={};
            state.timbreMsb=-1;
        }
    }
    zone_=z;
}

std::size_t MpeParser::activeNoteIndex(const ChannelState& state) noexcept {
    for(std::size_t i=state.heldCount;i>0;--i)
        if(state.held[i-1].id && state.held[i-1].release<0)return i-1;
    return state.heldCount;
}

std::uint64_t MpeParser::activeNoteOn(std::uint8_t chan1) const noexcept {
    if (chan1 < 1 || chan1 > 16) return 0;
    const auto& state=chans_[chan1-1];
    const auto index=activeNoteIndex(state);
    return index<state.heldCount?state.held[index].id:0;
}

bool MpeParser::emit(Sink sink, void* ctx, const Event& e) noexcept {
    if (sink == nullptr) return false;
    if (!sink(ctx, e)) { ++rejected_; return false; }
    return true;
}

bool MpeParser::emitExpression(std::uint8_t channel,std::uint64_t noteId,ExpressionDim dim,
                               double value,Sink sink,void* context) noexcept {
    Event event;
    event.type=EventType::NoteExpression;event.frame=frame_;event.channel=channel;
    event.noteId=noteId;event.dim=static_cast<std::uint16_t>(dim);event.value=value;
    return emit(sink,context,event);
}

int MpeParser::sendInitial(HeldNote& note,std::uint8_t channel,Sink sink,void* context) noexcept {
    int sent=0;
    if(note.initial.bend!=kNoExpression) {
        if(!emitExpression(channel,note.id,ExpressionDim::Pitch,
                           bendToSemitones(note.initial.bend,zone_.memberBendSemitones),
                           sink,context))return 0;
        note.initial.bend=kNoExpression;
        ++sent;
    }
    if(note.initial.timbre!=kNoExpression) {
        const auto word=note.initial.timbre;
        const auto value=word&kSevenBitTimbre?cc7ToUnit(static_cast<std::uint8_t>(word)):
                                              cc14ToUnit(word);
        if(emitExpression(channel,note.id,ExpressionDim::Timbre,value,sink,context)) {
            note.initial.timbre=kNoExpression;
            ++sent;
        }
    }
    return sent;
}

bool MpeParser::releaseNote(const HeldNote& note,std::uint8_t channel,std::uint8_t velocity,
                            Sink sink,void* context) noexcept {
    Event event;
    event.type=EventType::NoteOff;event.frame=frame_;event.channel=channel;
    event.noteId=note.id;event.dim=note.key;event.value=cc7ToUnit(velocity);
    return emit(sink,context,event);
}
void MpeParser::forgetNote(ChannelState& state,std::size_t at) noexcept {
    --state.heldCount;
    for(;at<state.heldCount;++at)state.held[at]=state.held[at+1];
}

int MpeParser::feed(const std::uint8_t* bytes, std::int32_t n,
                    Sink sink, void* ctx) noexcept {
    if (bytes == nullptr || n < 2) return 0;

    const std::uint8_t status = bytes[0];
    if (status < 0x80) return 0;              // not a status byte
    if (status >= 0xF0) return 0;             // system messages are not ours

    const std::uint8_t type  = static_cast<std::uint8_t>(status & 0xF0);
    const std::int32_t needed=(type==0xD0 || type==0xC0)?2:3;
    if(n<needed)return 0;
    for(std::int32_t index=1;index<needed;++index)if(bytes[index]>=0x80)return 0;
    const std::uint8_t chan1 = static_cast<std::uint8_t>((status & 0x0F) + 1);
    ChannelState& cs = chan(chan1);
    const bool member = zone_.isMember(chan1);

    int emitted = 0;
    // A refused initial value stays with the accepted note. Retry at this
    // message's frame, since the note's original block may have completed.
    for(std::size_t at=0;at<cs.heldCount;) {
        auto& note=cs.held[at];
        if(note.release>=0 && releaseNote(note,chan1,static_cast<std::uint8_t>(note.release),sink,ctx)) {
            ++emitted;forgetNote(cs,at);
        } else {
            if(note.release<0 && note.id)emitted+=sendInitial(note,chan1,sink,ctx);
            ++at;
        }
    }

    switch (type) {
        case 0xA0: // Polyphonic aftertouch has no key-addressed engine route.
        case 0xC0: // Program change does not participate in note ownership.
            ++unsupported_;
            return emitted;
        case 0x90:
        case 0x80: {                           // note on, and note off
            if (n < 3) return 0;
            const std::uint8_t key = bytes[1];
            const std::uint8_t vel = bytes[2];

            // A note-on with velocity 0 IS a note-off. Every controller in
            // existence sends them, and treating one as a note-on leaves a
            // voice sounding forever -- the classic stuck note.
            const bool isOff = (type == 0x80) || (vel == 0);

            if (isOff) {
                // The oldest matching start owns this release, independently
                // of current zone membership and other keys held here.
                for(std::size_t at=0;at<cs.heldCount;++at) {
                    auto& held=cs.held[at];
                    if(held.key!=key || held.release>=0)continue;
                    if(!held.id)forgetNote(cs,at);
                    else if(releaseNote(held,chan1,vel,sink,ctx)) {++emitted;forgetNote(cs,at);}
                    else held.release=vel;
                    if(!activeNoteOn(chan1) && cs.idle.timbre==kNoExpression)cs.timbreMsb=-1;
                    return emitted;
                }
                if(cs.dropped[key])--cs.dropped[key];
                else if(releaseNote({0,key,-1},chan1,vel,sink,ctx))++emitted;
                return emitted;
            }
            {
                const auto initial=cs.idle;
                cs.idle={};
                if(cs.heldCount==cs.held.size() || cs.dropped[key] || nextNoteId_==0) {
                    if(cs.dropped[key]!=std::numeric_limits<std::uint32_t>::max())++cs.dropped[key];
                    ++droppedStarts_;
                    if(!activeNoteOn(chan1))cs.timbreMsb=-1;
                    return emitted;
                }
                Event e;
                e.type    = EventType::NoteOn;
                e.frame   = frame_;
                e.channel = chan1;
                // Minted here and never derived from the channel afterwards.
                // ADR-0054: the channel is transport, the id is identity.
                e.noteId  = nextNoteId_++;
                e.dim     = key;               // the key, for a note event
                e.value   = cc7ToUnit(vel);    // velocity as a DOUBLE, 0..1
                ++notesStarted_;
                const bool sent=emit(sink,ctx,e);
                auto& held=cs.held[cs.heldCount++];
                held={sent?e.noteId:0,key,-1};
                if(sent) {
                    held.initial=initial;
                    emitted+=1+sendInitial(held,chan1,sink,ctx);
                } else if(!activeNoteOn(chan1))cs.timbreMsb=-1;
            }
            return emitted;
        }

        case 0xE0: {                           // pitch bend
            if (n < 3) return 0;
            const auto word = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(bytes[2] & 0x7F) << 7) |
                static_cast<std::uint16_t>(bytes[1] & 0x7F));
            const auto at=activeNoteIndex(cs);
            if(member && at==cs.heldCount) {
                cs.idle.bend=word;
                return emitted;
            }
            const auto value=bendToSemitones(word,member?zone_.memberBendSemitones:
                                                         zone_.masterBendSemitones);
            if(emitExpression(chan1,member?cs.held[at].id:0,ExpressionDim::Pitch,value,sink,ctx)) {
                if(member)cs.held[at].initial.bend=kNoExpression;
                ++emitted;
            }
            return emitted;
        }

        case 0xD0: {                           // channel pressure
            Event e;
            e.type    = EventType::NoteExpression;
            e.frame   = frame_;
            e.channel = chan1;
            e.dim     = static_cast<std::uint16_t>(ExpressionDim::Pressure);
            e.value   = cc7ToUnit(bytes[1]);
            e.noteId  = member ? activeNoteOn(chan1) : 0;
            if (emit(sink, ctx, e)) ++emitted;
            return emitted;
        }

        case 0xB0: {                           // control change
            if (n < 3) return 0;
            const std::uint8_t cc  = static_cast<std::uint8_t>(bytes[1] & 0x7F);
            const std::uint8_t val = static_cast<std::uint8_t>(bytes[2] & 0x7F);

            // Only member-channel CC74/106 is MPE timbre. Ordinary controls,
            // including master-channel pedals and plain MIDI timbre, retain
            // their address rather than disappearing or becoming expression.
            if(!member || (cc!=kTimbreCcMsb && cc!=kTimbreCcMsb+32)) {
                Event e;e.type=EventType::Controller;e.frame=frame_;
                e.channel=static_cast<std::uint8_t>(chan1-1);e.paramId=cc;e.value=cc7ToUnit(val);
                if(emit(sink,ctx,e))++emitted;
                return emitted;
            }

            // An MSB is useful on its own. Its LSB can refine it to 14 bits.
            const bool sevenBit=cc==kTimbreCcMsb;
            if(sevenBit)cs.timbreMsb=val;
            else if(cs.timbreMsb<0)return emitted;
            const auto word=static_cast<std::uint16_t>(sevenBit?kSevenBitTimbre|val:
                                             (static_cast<unsigned>(cs.timbreMsb)<<7)|val);
            const auto at=activeNoteIndex(cs);
            if(at==cs.heldCount) {
                cs.idle.timbre=word;
                return emitted;
            }
            auto& note=cs.held[at];
            if(emitExpression(chan1,note.id,ExpressionDim::Timbre,
                              sevenBit?cc7ToUnit(val):cc14ToUnit(word),sink,ctx)) {
                note.initial.timbre=kNoExpression;
                ++emitted;
            }
            return emitted;
        }

        default:
            return 0;
    }
}

}  // namespace adi::engine
