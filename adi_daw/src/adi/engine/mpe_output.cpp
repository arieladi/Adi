// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/mpe_output.hpp"

#include <cmath>

namespace adi::engine {

namespace {

constexpr double clampUnit(double v) noexcept {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

std::int16_t clampKey(std::uint16_t dim) noexcept {
    return static_cast<std::int16_t>(dim > 127 ? 127 : dim);
}

constexpr std::uint32_t typeId(Vst3NoteExprType t) noexcept {
    return static_cast<std::uint32_t>(t);
}

}  // namespace

int ExpressionCaps::perChannelBend() const noexcept {
    int distinct = 0;
    for (std::size_t c = 0; c < bendParam.size(); ++c) {
        if (bendParam[c] == kNoParam) continue;
        bool seen = false;
        for (std::size_t k = 0; k < c; ++k)
            if (bendParam[k] == bendParam[c]) seen = true;
        if (!seen) ++distinct;
    }
    return distinct;
}

ExpressionRoute resolveRoute(RouteChoice choice, const ExpressionCaps& caps) noexcept {
    switch (choice) {
        case RouteChoice::NoteExpression: return ExpressionRoute::NoteExpression;
        case RouteChoice::MpeMidi:        return ExpressionRoute::MpeMidi;
        case RouteChoice::Plain:          return ExpressionRoute::Plain;
        case RouteChoice::Auto:           break;
    }
    if (!caps.controllerReachable) return ExpressionRoute::NoteExpression;
    if (caps.noteExpression) return ExpressionRoute::NoteExpression;
    if (caps.perChannelBend() >= 2) return ExpressionRoute::MpeMidi;
    return ExpressionRoute::Plain;
}

double semitonesToBendExact(double semitones, double range) noexcept {
    if (!(range > 0.0) || std::isnan(semitones)) return 8192.0;
    // 8191 steps above centre, 8192 below: bendToSemitones, inverted.
    const double x = semitones >= 0.0 ? 8192.0 + semitones / range * 8191.0
                                      : 8192.0 + semitones / range * 8192.0;
    return x < 0.0 ? 0.0 : (x > 16383.0 ? 16383.0 : x);
}

std::uint16_t semitonesToBendWord(double semitones, double range) noexcept {
    return static_cast<std::uint16_t>(std::lround(semitonesToBendExact(semitones, range)));
}

bool noteExpressionOut(const Event& in, const ExpressionCaps& caps, MpeOut& out) noexcept {
    out = MpeOut{};
    out.frame = in.frame;
    out.noteId = in.noteId;

    switch (in.type) {
        case EventType::NoteOn:
        case EventType::NoteOff:
            out.kind = in.type == EventType::NoteOn ? MpeOut::Kind::NoteOn : MpeOut::Kind::NoteOff;
            // CHANNEL 0, whatever the controller used. ADR-0054: the channel
            // is transport, and a plugin filtering on channel 1 would
            // otherwise never hear notes an MPE controller sent on 2..16.
            out.channel = 0;
            out.key = clampKey(in.dim);
            out.value = clampUnit(in.value);
            return true;

        case EventType::NoteExpression: {
            out.kind = MpeOut::Kind::Expression;
            out.key = -1;                       // unknown here; the router fills it
            out.dim = in.dim;
            out.plain = in.value;
            switch (static_cast<ExpressionDim>(in.dim)) {
                case ExpressionDim::Pitch:
                    out.exprType = caps.pitchType;
                    // Tuning is in semitones over VST3's ±120. A plugin that
                    // mapped MPE's X to a type of its own gets VST3's physical
                    // X position instead: 0..1, centre 0.5, across MPE's ±48.
                    out.value = caps.pitchType == typeId(Vst3NoteExprType::Tuning)
                                    ? semitonesToVst3Tuning(in.value)
                                    : clampUnit(0.5 + in.value / (2.0 * kMpeOutBendSemitones));
                    return true;
                case ExpressionDim::Timbre:
                    out.exprType = caps.timbreType;
                    break;
                case ExpressionDim::Pressure:
                    out.exprType = caps.pressureType;
                    break;
                case ExpressionDim::Gain:
                    out.exprType = typeId(Vst3NoteExprType::Volume);
                    break;
                case ExpressionDim::Pan:
                    out.exprType = typeId(Vst3NoteExprType::Pan);
                    break;
                default:
                    return false;             // plugin-defined: no VST3 type to name
            }
            out.value = clampUnit(in.value);
            return true;
        }

        case EventType::ParamValue:
        case EventType::ParamMod:
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------

void MpeRouter::configure(ExpressionRoute route, const ExpressionCaps& caps,
                          int memberChannels) noexcept {
    route_ = route;
    caps_ = caps;
    members_ = memberChannels < 1 ? 1 : (memberChannels > 15 ? 15 : memberChannels);
    reset();
}

void MpeRouter::reset() noexcept {
    notes_.fill(Note{});
    active_.fill(0);
    lastOn_.fill(0);
    lastOff_.fill(0);
    clock_ = 0;
    mcmPending_ = route_ == ExpressionRoute::MpeMidi;
}

void MpeRouter::switchTo(ExpressionRoute route, std::int32_t frame, MpeOutList& out) noexcept {
    if (route == route_) return;
    for (Note& nt : notes_) {
        if (!nt.used) continue;
        MpeOut o;
        o.kind = MpeOut::Kind::NoteOff;
        o.channel = nt.channel;             // where it STARTED, not where the new route would put it
        o.key = nt.key;
        o.noteId = nt.id;
        o.frame = frame;
        out.push(o);
    }
    route_ = route;
    reset();
}

int MpeRouter::liveNotes() const noexcept {
    int n = 0;
    for (const Note& nt : notes_) if (nt.used) ++n;
    return n;
}

int MpeRouter::channelOf(std::uint64_t noteId) const noexcept {
    const Note* nt = find(noteId);
    return nt == nullptr ? -1 : nt->channel;
}

std::int64_t MpeRouter::dropped(ExpressionDim d) const noexcept {
    const auto i = static_cast<std::size_t>(d);
    return i < dropped_.size() ? dropped_[i] : droppedOther_;
}

MpeRouter::Note* MpeRouter::find(std::uint64_t id) noexcept {
    for (Note& nt : notes_) if (nt.used && nt.id == id) return &nt;
    return nullptr;
}

const MpeRouter::Note* MpeRouter::find(std::uint64_t id) const noexcept {
    for (const Note& nt : notes_) if (nt.used && nt.id == id) return &nt;
    return nullptr;
}

int MpeRouter::allocate() noexcept {
    // A free member channel, the one released LONGEST ago: a channel freed a
    // moment ago may still be ringing out its release, and a new note there
    // would reset the bend under it. Never-used channels have lastOff_ 0 and
    // go first, lowest number first.
    int best = -1;
    for (int c = 1; c <= members_; ++c) {
        const auto k = static_cast<std::size_t>(c);
        if (active_[k] != 0) continue;
        if (best < 0 || lastOff_[k] < lastOff_[static_cast<std::size_t>(best)]) best = c;
    }
    if (best >= 0) return best;

    // Every member channel is sounding. MPE 1.0's answer is to SHARE the
    // channel whose note began longest ago -- both notes then follow its
    // expression. Ending that note to make room would cut off something the
    // player is still holding, which changes the music; sharing only blurs it.
    int oldest = 1;
    for (int c = 2; c <= members_; ++c)
        if (lastOn_[static_cast<std::size_t>(c)] < lastOn_[static_cast<std::size_t>(oldest)])
            oldest = c;
    ++shared_;
    return oldest;
}

void MpeRouter::control(std::uint8_t chan, std::uint16_t ctrl, double v, std::int32_t frame,
                        std::uint64_t noteId, MpeOutList& out) noexcept {
    MpeOut o;
    o.kind = MpeOut::Kind::Control;
    o.channel = chan;
    o.ctrl = ctrl;
    o.frame = frame;
    o.noteId = noteId;
    const auto c = static_cast<std::size_t>(chan & 0x0F);

    if (ctrl == kCtrlPitchBend) {
        // `v` is semitones. The parameter gets the UNROUNDED word over 16383
        // -- the scale JUCE's own host uses, which its client decodes back to
        // the same word -- and the wire gets the rounded one.
        const double x = semitonesToBendExact(v, kMpeOutBendSemitones);
        o.word = static_cast<std::uint16_t>(std::lround(x));
        o.value = x / 16383.0;
        o.paramId = caps_.bendParam[c];
    } else {
        const double u = clampUnit(v);
        o.word = static_cast<std::uint16_t>(std::lround(u * 127.0));
        o.value = u;
        if (ctrl == kCtrlAfterTouch) o.paramId = caps_.pressureParam[c];
        else if (ctrl == kCtrlTimbre) o.paramId = caps_.timbreParam[c];
        else if (chan == 0 && ctrl == 101) o.paramId = caps_.rpnParam[0];
        else if (chan == 0 && ctrl == 100) o.paramId = caps_.rpnParam[1];
        else if (chan == 0 && ctrl == 6)   o.paramId = caps_.rpnParam[2];
    }
    out.push(o);
}

void MpeRouter::sendMcm(std::int32_t frame, MpeOutList& out) noexcept {
    // MPE 1.0's Configuration Message: RPN 6 on the master channel, value =
    // the member count. It is what makes "a member channel bends ±48" true on
    // the receiving end rather than assumed -- see kMpeOutBendSemitones.
    control(0, 101, 0.0, frame, 0, out);
    control(0, 100, 6.0 / 127.0, frame, 0, out);
    control(0, 6, static_cast<double>(members_) / 127.0, frame, 0, out);
}

void MpeRouter::route(const Event* events, std::int32_t n, std::int32_t segmentStart,
                      MpeOutList& out) noexcept {
    if (mcmPending_) {
        sendMcm(segmentStart, out);
        mcmPending_ = false;
    }
    if (events == nullptr) return;
    for (std::int32_t i = 0; i < n; ++i) {
        switch (events[i].type) {
            case EventType::NoteOn:         noteOn(events, n, i, out); break;
            case EventType::NoteOff:        noteOff(events[i], out); break;
            case EventType::NoteExpression: expression(events[i], out); break;
            case EventType::ParamValue:
            case EventType::ParamMod:       break;   // addressed, not a note stream (ADR-0091)
        }
    }
}

void MpeRouter::noteOn(const Event* events, std::int32_t n, std::int32_t i,
                       MpeOutList& out) noexcept {
    const Event& e = events[i];

    Note* slot = nullptr;
    for (Note& nt : notes_) if (!nt.used) { slot = &nt; break; }
    if (slot == nullptr) { ++tableFull_; return; }

    const std::int16_t key = clampKey(e.dim);
    std::uint8_t chan = 0;

    if (route_ == ExpressionRoute::MpeMidi) {
        chan = static_cast<std::uint8_t>(allocate());

        // THE CHANNEL IS RESET BEFORE THE NOTE. The previous note on this
        // member channel may have ended bent a fifth up; without a reset the
        // new one starts a fifth sharp. The note's own starting values are
        // used where the stream carries them at the same instant -- MPE sends
        // a note's initial bend and timbre before its note-on -- and MPE's
        // neutral values where it does not.
        double pitch = 0.0, pressure = 0.0, timbre = kMpeNeutralTimbre;
        for (std::int32_t j = i + 1; j < n && events[j].frame == e.frame; ++j) {
            const Event& x = events[j];
            if (x.type != EventType::NoteExpression || x.noteId != e.noteId || e.noteId == 0)
                continue;
            switch (static_cast<ExpressionDim>(x.dim)) {
                case ExpressionDim::Pitch:    pitch = x.value; break;
                case ExpressionDim::Pressure: pressure = x.value; break;
                case ExpressionDim::Timbre:   timbre = x.value; break;
                default: break;
            }
        }
        control(chan, kCtrlPitchBend, pitch, e.frame, e.noteId, out);
        control(chan, kCtrlTimbre, timbre, e.frame, e.noteId, out);
        control(chan, kCtrlAfterTouch, pressure, e.frame, e.noteId, out);
    } else {
        for (const Note& nt : notes_)
            if (nt.used && nt.key == key && nt.channel == 0) { ++collisions_; break; }
    }

    MpeOut o;
    o.kind = MpeOut::Kind::NoteOn;
    o.channel = chan;
    o.key = key;
    o.noteId = e.noteId;
    o.value = clampUnit(e.value);
    o.frame = e.frame;
    out.push(o);

    const auto c = static_cast<std::size_t>(chan);
    ++active_[c];
    lastOn_[c] = ++clock_;
    slot->used = true;
    slot->id = e.noteId;
    slot->key = key;
    slot->channel = chan;
}

void MpeRouter::noteOff(const Event& e, MpeOutList& out) noexcept {
    Note* nt = nullptr;
    if (e.noteId != 0) {
        nt = find(e.noteId);
    } else {
        // An unassigned id is legal for a note with no expression (events.hpp),
        // so it is matched by key among the other unassigned notes.
        const std::int16_t key = clampKey(e.dim);
        for (Note& k : notes_) if (k.used && k.id == 0 && k.key == key) { nt = &k; break; }
    }
    if (nt == nullptr) { ++unknown_; return; }

    MpeOut o;
    o.kind = MpeOut::Kind::NoteOff;
    o.channel = nt->channel;
    o.key = nt->key;
    o.noteId = nt->id;
    o.value = clampUnit(e.value);
    o.frame = e.frame;
    out.push(o);

    const auto c = static_cast<std::size_t>(nt->channel);
    if (active_[c] > 0) --active_[c];
    if (active_[c] == 0) lastOff_[c] = ++clock_;
    nt->used = false;
}

void MpeRouter::expression(const Event& e, MpeOutList& out) noexcept {
    const auto d = static_cast<ExpressionDim>(e.dim);
    const auto di = static_cast<std::size_t>(e.dim);
    auto drop = [&]() noexcept {
        if (di < dropped_.size()) ++dropped_[di]; else ++droppedOther_;
    };

    if (route_ == ExpressionRoute::NoteExpression) {
        // Anchored by note id, so no channel or key is needed and the note
        // need not have been seen: a plugin ignores an id it does not know.
        MpeOut o;
        if (!noteExpressionOut(e, caps_, o)) { drop(); return; }
        if (const Note* nt = e.noteId != 0 ? find(e.noteId) : nullptr) o.key = nt->key;
        out.push(o);
        return;
    }

    const Note* nt = e.noteId != 0 ? find(e.noteId) : nullptr;
    if (nt == nullptr) { ++unknown_; return; }

    if (route_ == ExpressionRoute::MpeMidi) {
        switch (d) {
            case ExpressionDim::Pitch:
                control(nt->channel, kCtrlPitchBend, e.value, e.frame, e.noteId, out); return;
            case ExpressionDim::Pressure:
                control(nt->channel, kCtrlAfterTouch, e.value, e.frame, e.noteId, out); return;
            case ExpressionDim::Timbre:
                control(nt->channel, kCtrlTimbre, e.value, e.frame, e.noteId, out); return;
            default:
                drop(); return;
        }
    }

    // Plain: poly aftertouch is the only per-note expression MIDI 1.0 has.
    if (d != ExpressionDim::Pressure) { drop(); return; }
    MpeOut o;
    o.kind = MpeOut::Kind::PolyPressure;
    o.channel = 0;
    o.key = nt->key;
    o.noteId = nt->id;
    o.value = clampUnit(e.value);
    o.frame = e.frame;
    out.push(o);
}

}  // namespace adi::engine
