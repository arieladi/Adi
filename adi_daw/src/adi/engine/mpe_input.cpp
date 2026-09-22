// SPDX-License-Identifier: GPL-3.0-or-later
//
// See mpe_input.hpp. ADR-0054: no MIDI byte survives this file.

#include "adi/engine/mpe_input.hpp"

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

std::uint64_t MpeParser::activeNoteOn(std::uint8_t chan1) const noexcept {
    if (chan1 < 1 || chan1 > 16) return 0;
    return chans_[chan1 - 1].noteId;
}

bool MpeParser::emit(Sink sink, void* ctx, const Event& e) noexcept {
    if (sink == nullptr) return false;
    if (!sink(ctx, e)) { ++rejected_; return false; }
    return true;
}

int MpeParser::feed(const std::uint8_t* bytes, std::int32_t n,
                    Sink sink, void* ctx) noexcept {
    if (bytes == nullptr || n < 2) return 0;

    const std::uint8_t status = bytes[0];
    if (status < 0x80) return 0;              // not a status byte
    if (status >= 0xF0) return 0;             // system messages are not ours

    const std::uint8_t type  = static_cast<std::uint8_t>(status & 0xF0);
    const std::uint8_t chan1 = static_cast<std::uint8_t>((status & 0x0F) + 1);
    ChannelState& cs = chan(chan1);
    const bool member = zone_.isMember(chan1);

    int emitted = 0;

    switch (type) {
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
                Event e;
                e.type    = EventType::NoteOff;
                e.frame   = frame_;
                e.channel = chan1;
                // Anchored to the note that is actually sounding on this
                // channel. A note-off for a key we are not holding gets no
                // id rather than a wrong one.
                e.noteId  = (cs.key == key && cs.noteId != 0) ? cs.noteId : 0;
                e.dim     = key;
                e.value   = cc7ToUnit(vel);
                if (cs.key == key) { cs.noteId = 0; cs.key = 0; }
                if (emit(sink, ctx, e)) ++emitted;
                return emitted;
            }
            {
                Event e;
                e.type    = EventType::NoteOn;
                e.frame   = frame_;
                e.channel = chan1;
                // Minted here and never derived from the channel afterwards.
                // ADR-0054: the channel is transport, the id is identity.
                e.noteId  = nextNoteId_++;
                e.dim     = key;               // the key, for a note event
                e.value   = cc7ToUnit(vel);    // velocity as a DOUBLE, 0..1
                cs.noteId = e.noteId;
                cs.key    = key;
                ++notesStarted_;
                if (emit(sink, ctx, e)) ++emitted;
            }
            return emitted;
        }

        case 0xE0: {                           // pitch bend
            if (n < 3) return 0;
            const auto word = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(bytes[2] & 0x7F) << 7) |
                static_cast<std::uint16_t>(bytes[1] & 0x7F));
            Event e;
            e.type    = EventType::NoteExpression;
            e.frame   = frame_;
            e.channel = chan1;
            e.dim     = static_cast<std::uint16_t>(ExpressionDim::Pitch);
            // A member channel's bend is THIS NOTE's pitch, in semitones, at
            // the member range -- which defaults to +/-48, not +/-2. Getting
            // that wrong is a transposition, not a subtle error.
            e.value   = bendToSemitones(word, member ? zone_.memberBendSemitones
                                                     : zone_.masterBendSemitones);
            e.noteId  = member ? cs.noteId : 0;
            if (emit(sink, ctx, e)) ++emitted;
            return emitted;
        }

        case 0xD0: {                           // channel pressure
            Event e;
            e.type    = EventType::NoteExpression;
            e.frame   = frame_;
            e.channel = chan1;
            e.dim     = static_cast<std::uint16_t>(ExpressionDim::Pressure);
            e.value   = cc7ToUnit(bytes[1]);
            e.noteId  = member ? cs.noteId : 0;
            if (emit(sink, ctx, e)) ++emitted;
            return emitted;
        }

        case 0xB0: {                           // control change
            if (n < 3) return 0;
            const std::uint8_t cc  = static_cast<std::uint8_t>(bytes[1] & 0x7F);
            const std::uint8_t val = static_cast<std::uint8_t>(bytes[2] & 0x7F);
            cs.cc[cc] = val;
            cs.ccSeen[cc] = true;

            // An LSB completing a high-resolution pair. This is the branch the
            // whole mandate rests on: taking the MSB alone would hand
            // downstream a 7-bit value that LOOKS like a double, and the
            // Continuum's extra bits would vanish with nothing failing.
            if (cc >= 32 && isHighResMsb(static_cast<std::uint8_t>(cc - 32))) {
                const std::uint8_t msbCc = static_cast<std::uint8_t>(cc - 32);
                if (!cs.ccSeen[msbCc]) return 0;   // no MSB yet: not 14-bit
                const auto word = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(cs.cc[msbCc]) << 7) |
                    static_cast<std::uint16_t>(val));
                if (msbCc != kTimbreCcMsb) return 0;   // only Y is mapped today
                Event e;
                e.type    = EventType::NoteExpression;
                e.frame   = frame_;
                e.channel = chan1;
                e.dim     = static_cast<std::uint16_t>(ExpressionDim::Timbre);
                e.value   = cc14ToUnit(word);
                e.noteId  = member ? cs.noteId : 0;
                if (emit(sink, ctx, e)) ++emitted;
                return emitted;
            }

            // A bare MSB with no LSB following is a legitimate 7-bit CC, and
            // it is emitted as one. Waiting for an LSB that never comes would
            // drop every ordinary controller.
            if (cc == kTimbreCcMsb) {
                Event e;
                e.type    = EventType::NoteExpression;
                e.frame   = frame_;
                e.channel = chan1;
                e.dim     = static_cast<std::uint16_t>(ExpressionDim::Timbre);
                e.value   = cc7ToUnit(val);
                e.noteId  = member ? cs.noteId : 0;
                if (emit(sink, ctx, e)) ++emitted;
                return emitted;
            }
            return 0;
        }

        default:
            return 0;
    }
}

}  // namespace adi::engine
