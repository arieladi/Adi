// SPDX-License-Identifier: GPL-3.0-or-later
//
// See clap_host.hpp for why this file has no JUCE in it.

#include "juce/clap_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
// Dynamic loading is the one genuinely platform-specific thing in a CLAP
// host, and it is three calls. Split here rather than behind a wrapper
// library: a wrapper for `LoadLibrary` versus `dlopen` is more code than
// the thing it wraps.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace adi::device {

// ---------------------------------------------------------------------------
// ClapEventList
// ---------------------------------------------------------------------------

void ClapEventList::reserve(std::int32_t n) {
    events_.clear();
    events_.reserve(static_cast<std::size_t>(n));
    cap_ = n;
    // Re-point the callback struct: `reserve` may reallocate, and `in_.ctx`
    // is a pointer to THIS rather than to the storage, so it stays valid.
    in_.ctx = this;
}

std::uint32_t ClapEventList::sizeCb(const clap_input_events_t* list) {
    const auto* self = static_cast<const ClapEventList*>(list->ctx);
    return static_cast<std::uint32_t>(self->events_.size());
}

const clap_event_header_t* ClapEventList::getCb(const clap_input_events_t* list,
                                                std::uint32_t index) {
    const auto* self = static_cast<const ClapEventList*>(list->ctx);
    if (index >= self->events_.size()) return nullptr;
    return &self->events_[index].hdr;
}

const clap_event_header_t* ClapEventList::at(std::int32_t i) const noexcept {
    if (i < 0 || i >= static_cast<std::int32_t>(events_.size())) return nullptr;
    return &events_[static_cast<std::size_t>(i)].hdr;
}

bool ClapEventList::add(const engine::Event& in, std::int32_t blockOffset,
                        std::int32_t segmentFrames) noexcept {
    if (cap_ > 0 && static_cast<std::int32_t>(events_.size()) >= cap_) {
        ++dropped_;
        return false;
    }

    // BLOCK-relative in, SEGMENT-relative out. See the header: this is
    // ADR-0078's trap one layer up, and a block with one segment cannot
    // show it because the offset is zero.
    const std::int32_t t = in.frame - blockOffset;
    if (t < 0) { ++outOfRange_; return false; }
    if (segmentFrames > 0 && t >= segmentFrames) { ++outOfRange_; return false; }
    const auto time = static_cast<std::uint32_t>(t);

    // Notes and expressions: the Clap dialect's rule, in ONE place (ADR-0099).
    // Channel 0 whatever the controller used -- ADR-0054's channel is
    // transport, and a CLAP plugin filtering on channel 0 would otherwise not
    // hear an MPE controller's notes. Expression is addressed by note id.
    if (engine::isNoteStream(in.type)) {
        engine::MpeOut o;
        if (!engine::noteExpressionOut(in, engine::ExpressionCaps{}, o)) return false;
        return addOut(o, ClapDialect::Clap, blockOffset, segmentFrames);
    }

    Slot slot{};
    switch (in.type) {
        case engine::EventType::NoteOn:
        case engine::EventType::NoteOff:
        case engine::EventType::NoteExpression:
            return false;                 // handled above

        case engine::EventType::ParamValue:
        case engine::EventType::ParamMod: {
            slot.param = clap_event_param_value_t{};
            slot.param.header.size     = sizeof(clap_event_param_value_t);
            slot.param.header.time     = time;
            slot.param.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            // CLAP CARRIES MODULATION NATIVELY, and this is the line that says
            // so. ADR-0046 and ADR-0052: a modulation offset leaves the user's
            // stored value alone. On VST3 the host has to resolve the two into
            // one number and shadow the user's setting; here they are separate
            // event types and the plugin does it properly.
            slot.param.header.type = static_cast<std::uint16_t>(
                (in.type == engine::EventType::ParamMod) ? CLAP_EVENT_PARAM_MOD
                                                         : CLAP_EVENT_PARAM_VALUE);
            slot.param.header.flags = 0;
            slot.param.param_id  = static_cast<clap_id>(in.paramId);
            slot.param.cookie    = nullptr;
            slot.param.note_id   = (in.noteId == 0)
                                     ? -1
                                     : static_cast<std::int32_t>(in.noteId % 0x7FFFFFFFu);
            slot.param.port_index = -1;
            slot.param.channel    = -1;
            slot.param.key        = -1;
            slot.param.value      = in.value;
            break;
        }
    }

    events_.push_back(slot);
    return true;
}

namespace {

std::int32_t clapNoteId(std::uint64_t id) noexcept {
    return id == 0 ? -1 : static_cast<std::int32_t>(id % 0x7FFFFFFFu);
}

std::uint8_t unit7(double v) noexcept {
    const double c = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    return static_cast<std::uint8_t>(std::lround(c * 127.0));
}

}  // namespace

ClapDialect resolveClapDialect(ClapDialectChoice choice, const ClapNotePorts& p) noexcept {
    if (!p.extension) return ClapDialect::Clap;
    if (!p.input) return ClapDialect::None;
    const auto has = [&p](std::uint32_t d) { return (p.supported & d) != 0; };
    switch (choice) {
        case ClapDialectChoice::Clap:    if (has(CLAP_NOTE_DIALECT_CLAP)) return ClapDialect::Clap; break;
        case ClapDialectChoice::MidiMpe: if (has(CLAP_NOTE_DIALECT_MIDI_MPE)) return ClapDialect::MidiMpe; break;
        case ClapDialectChoice::Midi:    if (has(CLAP_NOTE_DIALECT_MIDI)) return ClapDialect::Midi; break;
        case ClapDialectChoice::Auto:    break;
    }
    if (p.preferred == CLAP_NOTE_DIALECT_CLAP && has(CLAP_NOTE_DIALECT_CLAP)) return ClapDialect::Clap;
    if (p.preferred == CLAP_NOTE_DIALECT_MIDI_MPE && has(CLAP_NOTE_DIALECT_MIDI_MPE))
        return ClapDialect::MidiMpe;
    if (p.preferred == CLAP_NOTE_DIALECT_MIDI && has(CLAP_NOTE_DIALECT_MIDI)) return ClapDialect::Midi;
    if (has(CLAP_NOTE_DIALECT_CLAP)) return ClapDialect::Clap;
    if (has(CLAP_NOTE_DIALECT_MIDI_MPE)) return ClapDialect::MidiMpe;
    if (has(CLAP_NOTE_DIALECT_MIDI)) return ClapDialect::Midi;
    return ClapDialect::None;
}

bool ClapEventList::addOut(const engine::MpeOut& o, ClapDialect dialect,
                           std::int32_t blockOffset, std::int32_t segmentFrames) noexcept {
    using K = engine::MpeOut::Kind;
    if (dialect == ClapDialect::None) return false;
    if (cap_ > 0 && static_cast<std::int32_t>(events_.size()) >= cap_) {
        ++dropped_;
        return false;
    }
    const std::int32_t t = o.frame - blockOffset;
    if (t < 0) { ++outOfRange_; return false; }
    if (segmentFrames > 0 && t >= segmentFrames) { ++outOfRange_; return false; }
    const auto time = static_cast<std::uint32_t>(t);

    Slot slot{};
    if (dialect == ClapDialect::Clap) {
        switch (o.kind) {
            case K::NoteOn:
            case K::NoteOff:
                slot.note = clap_event_note_t{};
                slot.note.header.size     = sizeof(clap_event_note_t);
                slot.note.header.time     = time;
                slot.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                slot.note.header.type     = static_cast<std::uint16_t>(
                    o.kind == K::NoteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF);
                slot.note.header.flags    = 0;
                // A REAL note id, not -1: CLAP anchors note expression to it.
                slot.note.note_id    = clapNoteId(o.noteId);
                slot.note.port_index = 0;
                slot.note.channel    = static_cast<std::int16_t>(o.channel & 0x0F);
                slot.note.key        = o.key;
                slot.note.velocity   = o.value;       // a double, 0..1
                break;
            case K::Expression: {
                const std::int32_t id = clapExprFor(static_cast<ExpressionDim>(o.dim));
                if (id < 0) return false;
                slot.expr = clap_event_note_expression_t{};
                slot.expr.header.size     = sizeof(clap_event_note_expression_t);
                slot.expr.header.time     = time;
                slot.expr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                slot.expr.header.type     = CLAP_EVENT_NOTE_EXPRESSION;
                slot.expr.header.flags    = 0;
                slot.expr.expression_id   = id;
                slot.expr.note_id    = clapNoteId(o.noteId);
                slot.expr.port_index = 0;
                slot.expr.channel    = static_cast<std::int16_t>(o.channel & 0x0F);
                // The note's own key when the route is tracking it, -1 (any)
                // when not: a plugin that matches on (channel, key) and not on
                // the id must still find ONE note on the one channel.
                slot.expr.key        = o.key;
                // NO CONVERSION for tuning: CLAP's TUNING is semitones, which
                // is what the engine carries. The plain value, not VST3's.
                slot.expr.value = (id == CLAP_NOTE_EXPRESSION_TUNING)
                                    ? semitonesToClapTuning(o.plain)
                                    : std::clamp(o.plain, 0.0, 1.0);
                break;
            }
            case K::PolyPressure:
            case K::Control:
                return false;                  // not on the Clap dialect's route
        }
    } else {
        // Raw MIDI. The route already chose channels and words (ADR-0097);
        // this only lays out the bytes.
        const auto ch = static_cast<std::uint8_t>(o.channel & 0x0F);
        const auto key = static_cast<std::uint8_t>(o.key & 0x7F);
        std::uint8_t b0 = 0, b1 = 0, b2 = 0;
        switch (o.kind) {
            case K::NoteOn: {
                // Velocity 0 is a note-off in MIDI 1.0, so a note-on never sends it.
                const std::uint8_t v = unit7(o.value);
                b0 = static_cast<std::uint8_t>(0x90 | ch); b1 = key; b2 = v == 0 ? 1 : v;
                break;
            }
            case K::NoteOff:
                b0 = static_cast<std::uint8_t>(0x80 | ch); b1 = key; b2 = unit7(o.value);
                break;
            case K::PolyPressure:
                b0 = static_cast<std::uint8_t>(0xA0 | ch); b1 = key; b2 = unit7(o.value);
                break;
            case K::Control:
                if (o.ctrl == engine::kCtrlPitchBend) {
                    b0 = static_cast<std::uint8_t>(0xE0 | ch);
                    b1 = static_cast<std::uint8_t>(o.word & 0x7F);
                    b2 = static_cast<std::uint8_t>((o.word >> 7) & 0x7F);
                } else if (o.ctrl == engine::kCtrlAfterTouch) {
                    b0 = static_cast<std::uint8_t>(0xD0 | ch);
                    b1 = static_cast<std::uint8_t>(o.word & 0x7F);
                } else if (o.ctrl < 128) {
                    b0 = static_cast<std::uint8_t>(0xB0 | ch);
                    b1 = static_cast<std::uint8_t>(o.ctrl);
                    b2 = static_cast<std::uint8_t>(o.word & 0x7F);
                } else {
                    return false;
                }
                break;
            case K::Expression:
                return false;                  // MIDI routes never produce one
        }
        slot.midi = clap_event_midi_t{};
        slot.midi.header.size     = sizeof(clap_event_midi_t);
        slot.midi.header.time     = time;
        slot.midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        slot.midi.header.type     = CLAP_EVENT_MIDI;
        slot.midi.header.flags    = 0;
        slot.midi.port_index = 0;
        slot.midi.data[0] = b0;
        slot.midi.data[1] = b1;
        slot.midi.data[2] = b2;
    }
    events_.push_back(slot);
    return true;
}

void ClapEventList::sortByTime() noexcept {
    for (std::size_t i = 1; i < events_.size(); ++i) {
        const Slot key = events_[i];
        std::size_t j = i;
        while (j > 0 && events_[j - 1].hdr.time > key.hdr.time) {
            events_[j] = events_[j - 1];
            --j;
        }
        events_[j] = key;
    }
}

// ---------------------------------------------------------------------------
// ClapDevice
// ---------------------------------------------------------------------------

void ClapDevice::readNotePorts() {
    notePorts_ = ClapNotePorts{};
    // Checked field by field: mac found plugins that return an extension
    // struct with null function pointers inside.
    if (notePortsExt_ == nullptr || notePortsExt_->count == nullptr || notePortsExt_->get == nullptr)
        return;
    notePorts_.extension = true;
    if (notePortsExt_->count(plugin_, true) == 0) return;
    clap_note_port_info_t info{};
    if (!notePortsExt_->get(plugin_, 0, true, &info)) return;
    notePorts_.input = true;
    notePorts_.supported = info.supported_dialects;
    notePorts_.preferred = info.preferred_dialect;
}

void ClapDevice::applyDialect(ClapDialectChoice choice) {
    appliedDialect_ = static_cast<std::uint8_t>(choice);
    dialect_ = resolveClapDialect(choice, notePorts_);
    router_.configure(routeFor(dialect_), engine::ExpressionCaps{});
    dialectInUse_.store(static_cast<std::uint8_t>(dialect_), std::memory_order_release);
}

bool ClapDevice::pushEvent(const engine::Event& e) noexcept {
    if (injectedUsed_ >= injected_.size()) { ++pendingDropped_; return false; }
    injected_[injectedUsed_++] = e;
    return true;
}

std::string ClapDevice::paramIdToText(clap_id id) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(id));
    return buf;
}

bool ClapDevice::paramIdFromText(const std::string& s, clap_id& out) noexcept {
    if (s.size() != 8) return false;
    unsigned v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
        else return false;              // uppercase is NOT accepted: one
                                        // spelling per id, or two rows collide
    }
    out = static_cast<clap_id>(v);
    return true;
}

ClapDevice::ClapDevice(const clap_plugin_t* plugin, DeviceIdentity id)
    : plugin_(plugin), id_(std::move(id)) {
    if (plugin_ == nullptr) return;
    paramsExt_ = static_cast<const clap_plugin_params_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
    stateExt_ = static_cast<const clap_plugin_state_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_STATE));
    tailExt_ = static_cast<const clap_plugin_tail_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_TAIL));
    latencyExt_ = static_cast<const clap_plugin_latency_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_LATENCY));
    notePortsExt_ = static_cast<const clap_plugin_note_ports_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_NOTE_PORTS));
    // ADR-0099: the plugin is not active yet, which is when the spec allows
    // the note-port scan.
    readNotePorts();
    applyDialect(ClapDialectChoice::Auto);
    // The output-event sink exists from construction, not from prepare: a
    // parameter set before activation is flushed (ADR-0123), and flush hands
    // the plugin this sink.
    outEvents_.ctx = this;
    outEvents_.try_push = &ClapDevice::outPush;

    // Read once, here, on the message thread. `desc` is required by the spec
    // and checked anyway: mac found plugins that return a non-null extension
    // struct with null function pointers inside, and a required field is the
    // same promise.
    if (plugin_->desc != nullptr)
        for (const char* const* f = plugin_->desc->features; f != nullptr && *f != nullptr; ++f)
            if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) instrument_ = true;

    rescanParams();
}

ClapDevice::~ClapDevice() {
    // Guard each CALL, never the whole body: an early return here would
    // skip destroy() and leak the plugin. The first version of this did
    // exactly that, by being pasted in from prepare().
    if (plugin_ == nullptr) return;
    if (activated_) {
        if (plugin_->stop_processing != nullptr) plugin_->stop_processing(plugin_);
        if (plugin_->deactivate != nullptr) plugin_->deactivate(plugin_);
    }
    if (plugin_->destroy != nullptr) plugin_->destroy(plugin_);
}

void ClapDevice::rescanParams() {
    params_.clear();
    paramIds_.clear();
    if (plugin_ == nullptr || paramsExt_ == nullptr ||
        paramsExt_->count == nullptr || paramsExt_->get_info == nullptr) return;

    const std::uint32_t n = paramsExt_->count(plugin_);
    params_.reserve(n);
    paramIds_.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        clap_param_info_t info{};
        if (!paramsExt_->get_info(plugin_, i, &info)) continue;

        ParamDescriptor d;
        d.id   = paramIdToText(info.id);
        d.name = info.name;
        // THE PANEL'S FINDING, INVERTED. CLAP gives min, max and default as
        // PLAIN DOUBLES, so the domain is Real and `hasReal` is true -- which
        // is what keeps an automation lane meaningful when the plugin is
        // missing (SPEC 6.3.3). VST3 could only ever give a display string.
        d.domain       = ParamDomain::Real;
        d.minReal      = info.min_value;
        d.maxReal      = info.max_value;
        d.hasRealRange = true;
        d.automatable  = (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
        d.flags        = static_cast<std::uint32_t>(info.flags);

        // Normalised alongside the real value, because the contract carries
        // both and a zero-width range would otherwise divide by zero.
        const double span = info.max_value - info.min_value;
        const double norm = (span > 0.0) ? (info.default_value - info.min_value) / span : 0.0;
        d.defaultValue = ParamValue::withReal(norm, info.default_value);

        params_.push_back(std::move(d));
        paramIds_.push_back(info.id);
    }
}

const ParamDescriptor* ClapDevice::paramAt(std::int32_t i) const noexcept {
    if (i < 0 || i >= static_cast<std::int32_t>(params_.size())) return nullptr;
    return &params_[static_cast<std::size_t>(i)];
}

ParamValue ClapDevice::getParam(const std::string& paramId) const noexcept {
    if (plugin_ == nullptr || paramsExt_ == nullptr ||
        paramsExt_->get_value == nullptr) return {};
    for (std::size_t i = 0; i < params_.size(); ++i) {
        if (params_[i].id != paramId) continue;
        double plain = 0.0;
        if (!paramsExt_->get_value(plugin_, paramIds_[i], &plain))
            return params_[i].defaultValue;
        const double span = params_[i].maxReal - params_[i].minReal;
        const double norm = (span > 0.0) ? (plain - params_[i].minReal) / span : 0.0;
        return ParamValue::withReal(norm, plain);
    }
    return {};
}

bool ClapDevice::setParam(const std::string& paramId, const ParamValue& v) {
    // A CLAP parameter is set by sending a CLAP_EVENT_PARAM_VALUE, not by a
    // setter -- values are sample-accurate and arrive with the block. While
    // the plugin is ACTIVE this records intent and the event goes in on the
    // next process: a flush() would place the change at the block boundary
    // and lose the offset ADR-0042 exists to preserve, and params.h makes
    // flush the audio thread's while active anyway.
    //
    // NOT ACTIVE is the other half of that same annotation --
    // `[active ? audio-thread : main-thread]` -- and it is the half a project
    // load lives in: the session applies the parameter mirror BEFORE the
    // first prepare (ADR-0122 d5). Queueing there dropped every value into a
    // queue that did not exist yet (linux's audit, C5; ADR-0123). So: not
    // active, flush now on this thread, which is the main thread.
    for (std::size_t i = 0; i < params_.size(); ++i) {
        if (params_[i].id != paramId) continue;
        const double span = params_[i].maxReal - params_[i].minReal;
        // CLAP parameter events carry the PLAIN value, not a normalised one.
        // Sending 0.42 to a 20 Hz..20 kHz cutoff would set it to 0.42 Hz --
        // a valid number in the wrong unit, which is the failure mode this
        // whole contract exists to keep visible (ADR-0057, ADR-0075).
        const double plain = v.hasReal ? v.real : params_[i].minReal + v.normalized * span;
        if (!activated_) {
            if (paramsExt_ == nullptr || paramsExt_->flush == nullptr) return false;
            ClapEventList one;
            one.reserve(1);
            engine::Event e;
            e.type = engine::EventType::ParamValue;
            e.paramId = paramIds_[i];
            e.value = plain;
            one.add(e);
            paramsExt_->flush(plugin_, one.inputEvents(), &outEvents_);
            return true;
        }
        if (pendingUsed_ >= pending_.size()) { ++pendingDropped_; return false; }
        pending_[pendingUsed_].id = paramIds_[i];
        pending_[pendingUsed_].value = plain;
        ++pendingUsed_;
        return true;
    }
    return false;
}

std::vector<std::string> ClapDevice::stateRoles() const {
    if (plugin_ == nullptr || stateExt_ == nullptr ||
        stateExt_->save == nullptr || stateExt_->load == nullptr) return {};
    // One opaque stream. CLAP has a single state blob, unlike VST3's component
    // plus controller -- so 'chunk' and nothing else, and plugin_state still
    // admits the other roles because the FORMAT must represent a file another
    // implementation wrote (SPEC 7).
    return {"chunk"};
}

namespace {

/// A `clap_ostream_t` that appends into a vector.
struct VecOut {
    clap_ostream_t os{};
    std::vector<std::uint8_t>* v = nullptr;

    static std::int64_t write(const clap_ostream_t* s, const void* buf, std::uint64_t n) {
        auto* self = static_cast<VecOut*>(s->ctx);
        const auto* p = static_cast<const std::uint8_t*>(buf);
        self->v->insert(self->v->end(), p, p + n);
        return static_cast<std::int64_t>(n);
    }
    explicit VecOut(std::vector<std::uint8_t>& out) : v(&out) {
        os.ctx = this;
        os.write = &VecOut::write;
    }
};

/// A `clap_istream_t` over a vector.
struct VecIn {
    clap_istream_t is{};
    const std::vector<std::uint8_t>* v = nullptr;
    std::size_t pos = 0;

    static std::int64_t read(const clap_istream_t* s, void* buf, std::uint64_t n) {
        auto* self = static_cast<VecIn*>(s->ctx);
        const std::size_t left = self->v->size() - self->pos;
        const std::size_t take = static_cast<std::size_t>(n) < left
                                   ? static_cast<std::size_t>(n) : left;
        std::memcpy(buf, self->v->data() + self->pos, take);
        self->pos += take;
        return static_cast<std::int64_t>(take);
    }
    explicit VecIn(const std::vector<std::uint8_t>& in) : v(&in) {
        is.ctx = this;
        is.read = &VecIn::read;
    }
};

}  // namespace

std::vector<std::uint8_t> ClapDevice::saveState(const std::string& role) const {
    if (plugin_ == nullptr || stateExt_ == nullptr || stateExt_->save == nullptr ||
        role != "chunk") return {};
    std::vector<std::uint8_t> out;
    VecOut sink(out);
    if (!stateExt_->save(plugin_, &sink.os)) return {};
    return out;
}

bool ClapDevice::loadState(const std::string& role, const std::vector<std::uint8_t>& b) {
    if (plugin_ == nullptr || stateExt_ == nullptr || stateExt_->load == nullptr ||
        role != "chunk") return false;
    VecIn src(b);
    return stateExt_->load(plugin_, &src.is);
}

std::int64_t ClapDevice::tailSamples() const noexcept {
    // `tailExt_ != nullptr` IS NOT ENOUGH, and this cost a segfault against a
    // real plugin. FabFilter Pro-Q 3's CLAP returns a non-null
    // clap_plugin_tail_t whose `get` is NULL, so checking the struct and
    // calling the member crashes the host.
    //
    // A plugin returning an extension it does not implement is sloppy, and a
    // host that dies because of it is worse: ADR-0011's whole posture is that
    // a misbehaving plugin must not take the project down with it. So every
    // extension function pointer is checked, not just the struct.
    //
    // The fake plugin in the tests fills in every pointer, which is exactly
    // why it could never have found this.
    if (plugin_ == nullptr || tailExt_ == nullptr || tailExt_->get == nullptr)
        return engine::kInfiniteTail;
    // NOT BEFORE ACTIVATE. Same rule as the latency below and the same
    // reason -- a tail is a number of samples and the plugin does not know
    // the sample rate yet. The conservative answer here is infinite, which
    // is ADR-0055's default: never suspend something we cannot ask about.
    if (!activated_) return engine::kInfiniteTail;
    const std::uint32_t t = tailExt_->get(plugin_);
    // CLAP spells an unbounded tail UINT32_MAX; ours is INT64_MAX. Mapped
    // rather than cast, or "never suspend" becomes 4294967295 samples --
    // twenty-four hours, which is wrong in a way nobody would ever see.
    if (t == UINT32_MAX) return engine::kInfiniteTail;
    return static_cast<std::int64_t>(t);
}

std::int32_t ClapDevice::latencySamples() const noexcept {
    // See tailSamples(): a non-null extension struct may still have a null
    // function pointer.
    if (plugin_ == nullptr || latencyExt_ == nullptr || latencyExt_->get == nullptr)
        return 0;

    // NOT BEFORE ACTIVATE, and this was found by a real plugin telling us
    // off. Surge XT 1.3.4 prints, from inside clap_plugin_latency.get:
    //
    //   "It is wrong to query the latency before the plugin is activated,
    //    because if the plugin dosen't know the sample rate, it can't know
    //    the number of samples of latency."
    //
    // It is right, and `ext/latency.h` says so in its annotation:
    // `[main-thread & (being-activated | active)]`. Nothing here enforced
    // it, and `Node::latencySamples()` is precisely the kind of thing a
    // compensation pass calls whenever it likes -- ADR-0058 computes at
    // prepare, but a node can be asked before its device has been.
    //
    // 0 is the right answer, and it is ADR-0058's default for the same
    // reason: a latency we cannot ask about must not move audio.
    if (!activated_) return 0;
    const std::uint32_t l = latencyExt_->get(plugin_);
    // Clamped into int32. A plugin reporting a latency larger than that is
    // reporting nonsense, and a negative compensation moves audio EARLIER
    // than its source.
    if (l > 0x7FFFFFFFu) return 0x7FFFFFFF;
    return static_cast<std::int32_t>(l);
}

void ClapDevice::prepare(double sampleRate, std::int32_t maxFrames) {
    // Every one of these is checked, not just the plugin pointer. A plugin
    // may return an incomplete vtable, and a host that dies because of it is
    // ADR-0011's failure: a misbehaving plugin must not take the project
    // down. Restored after a bisecting `cp` quietly reverted them -- which
    // is its own lesson about restoring from a snapshot taken mid-edit.
    if (plugin_ == nullptr || plugin_->activate == nullptr) return;

    // ALREADY RUNNING AND NOTHING CHANGED: DO NOTHING.
    //
    // `Graph::prepare` calls `prepare` on every node, and ADR-0089 prepares a
    // whole new graph on every rebuild -- so without this, one plugin
    // rescanning its ports deactivates and reactivates EVERY plugin in the
    // project. That is not a theoretical cost. Measured on Pro-Q 3 in linear
    // phase: a rebuild silenced the master for 5120 samples, 106.7 ms,
    // because reactivating threw away the FIR's input history.
    //
    // It also nearly cost the right diagnosis. The first measurement had a
    // dry path whose compensation ring the rebuild had emptied, and a ring
    // is the obvious culprit; feeding ONLY the wet path -- no ring to lose --
    // produced exactly the same 5120-sample hole.
    //
    // The declared BUS LAYOUT is re-read rather than assumed, because a port
    // rescan is the one thing that must not take this path: re-reading is a
    // few `get_extension` calls and no state is lost by asking.
    if (activated_ && sampleRate == sampleRate_ && maxFrames == maxFrames_ &&
        layoutMatches()) {
        return;
    }

    if (activated_) {
        if (processing_ && plugin_->stop_processing != nullptr) plugin_->stop_processing(plugin_);
        processing_ = false;
        if (plugin_->deactivate != nullptr) plugin_->deactivate(plugin_);
    }
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    // ADR-0099: deactivated now, so the note ports may be scanned -- they can
    // change between activations -- and the dialect resolved against them.
    // The route forgets its notes, which a reactivated plugin has too, and
    // MpeMidi's Configuration Message is sent again.
    readNotePorts();
    applyDialect(static_cast<ClapDialectChoice>(requestedDialect_.load(std::memory_order_acquire)));
    // The MAX is the granted size (ADR-0049): a plugin told a larger maximum
    // than it will get allocates more than it needs; one told a smaller
    // maximum overruns when the driver hands over a full block. The MIN is
    // 1, not the granted size: plugin.h promises every process call's
    // frame count lies inside [min, max], and sub-block splitting (ADR-0042
    // d2) hands a plugin segments as short as one frame. Passing the block
    // size as the minimum was a promise the graph breaks on every split
    // (linux's audit, C3; ADR-0123).
    activated_ = plugin_->activate(plugin_, sampleRate, 1u,
                                   static_cast<std::uint32_t>(maxFrames));
    // `start_processing` RETURNS whether it worked, and `process` is legal
    // only while processing (plugin.h). A start that failed leaves the plugin
    // active and silent: `process` passes audio through, and the refusal is
    // counted where a host can see it (C2; ADR-0123).
    processing_ = activated_ && plugin_->start_processing != nullptr &&
                  plugin_->start_processing(plugin_);
    if (activated_ && !processing_) ++startFailures_;

    // Everything the audio thread touches, allocated here and never again
    // (ADR-0010). Sized from the GRANTED block size (ADR-0049).
    // ASK THE PLUGIN WHAT IT WANTS. See the header: hardcoding one bus each
    // way crashed Pro-Q 3 inside its own process, because it declares two
    // inputs and indexed the second one past the end of our array.
    inBuses_.clear();
    outBuses_.clear();
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS));

    auto addBus = [&](std::vector<Bus>& into, std::int32_t chans) {
        Bus b;
        b.channels = chans > 0 ? chans : 0;
        b.storage.assign(static_cast<std::size_t>(b.channels) *
                         static_cast<std::size_t>(maxFrames), 0.0f);
        b.ptrs.resize(static_cast<std::size_t>(b.channels));
        for (std::int32_t c = 0; c < b.channels; ++c)
            b.ptrs[static_cast<std::size_t>(c)] =
                b.storage.data() + static_cast<std::size_t>(c) *
                                   static_cast<std::size_t>(maxFrames);
        into.push_back(std::move(b));
    };

    if (ports != nullptr && ports->count != nullptr && ports->get != nullptr) {
        for (bool isInput : {true, false}) {
            const std::uint32_t n = ports->count(plugin_, isInput);
            for (std::uint32_t i = 0; i < n; ++i) {
                clap_audio_port_info_t info{};
                const std::int32_t chans =
                    ports->get(plugin_, i, isInput, &info)
                        ? static_cast<std::int32_t>(info.channel_count) : channels_;
                addBus(isInput ? inBuses_ : outBuses_, chans);
            }
        }
    } else {
        // No extension: CLAP's default is one stereo bus each way.
        addBus(inBuses_, channels_);
        addBus(outBuses_, channels_);
    }
    // A plugin with no output bus cannot be rendered; give it one so the
    // graph gets silence rather than a null dereference.
    if (outBuses_.empty()) addBus(outBuses_, channels_);

    inBufs_.assign(inBuses_.size(), clap_audio_buffer_t{});
    outBufs_.assign(outBuses_.size(), clap_audio_buffer_t{});

    // ADR-0056's arithmetic, applied to this device rather than re-derived:
    // 500 Hz MPE+ across the block, three dimensions per note, at the
    // polyphony the graph is configured for -- plus room for note on/off.
    const double frames = static_cast<double>(maxFrames);
    const double updateFrames = (sampleRate > 0.0) ? (frames / sampleRate) * 500.0 : 1.0;
    const auto perNote = static_cast<std::int32_t>(updateFrames * 3.0) + 2;
    events_.reserve((perNote > 0 ? perNote : 8) * 16);
    // The route can turn one note-on into four outputs (three channel resets
    // and the note), plus the Configuration Message once.
    const std::int32_t routedCap = (perNote > 0 ? perNote : 8) * 16 + 16 * 4 + 8;
    routedStore_.assign(static_cast<std::size_t>(routedCap), engine::MpeOut{});
    routed_ = engine::MpeOutList(routedStore_.data(), routedCap);

    pending_.assign(256, PendingParam{});
    pendingUsed_ = 0;
    injected_.assign(static_cast<std::size_t>((perNote > 0 ? perNote : 8) * 16),
                     engine::Event{});
    injectedUsed_ = 0;

    outEvents_.ctx = this;
    outEvents_.try_push = &ClapDevice::outPush;
}

std::vector<std::int32_t> ClapDevice::declaredChannels(bool isInput) const {
    std::vector<std::int32_t> out;
    if (plugin_ == nullptr) return out;
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS));
    if (ports == nullptr || ports->count == nullptr || ports->get == nullptr) {
        // No extension: CLAP's default is one stereo bus each way. Stated in
        // ONE place, so `prepare` and this cannot disagree about what a
        // plugin that declares nothing has.
        out.push_back(channels_);
        return out;
    }
    const std::uint32_t n = ports->count(plugin_, isInput);
    for (std::uint32_t i = 0; i < n; ++i) {
        clap_audio_port_info_t info{};
        out.push_back(ports->get(plugin_, i, isInput, &info)
                          ? static_cast<std::int32_t>(info.channel_count)
                          : channels_);
    }
    return out;
}

bool ClapDevice::layoutMatches() const {
    const std::vector<std::int32_t> in = declaredChannels(true);
    const std::vector<std::int32_t> outC = declaredChannels(false);

    if (in.size() != inBuses_.size()) return false;
    for (std::size_t i = 0; i < in.size(); ++i)
        if (in[i] != inBuses_[i].channels) return false;

    // `prepare` gives a plugin declaring no output bus one anyway, so a
    // plugin that really has none matches a single allocated bus rather than
    // zero. Anything else would reactivate such a plugin on every rebuild.
    if (outC.empty()) return outBuses_.size() == 1;

    if (outC.size() != outBuses_.size()) return false;
    for (std::size_t i = 0; i < outC.size(); ++i)
        if (outC[i] != outBuses_[i].channels) return false;
    return true;
}

void ClapDevice::release() {
    if (plugin_ == nullptr || !activated_) return;
    // The THIRD call site, and the one the guard audit found by arithmetic --
    // "stop_processing: guarded=2 calls=3" -- which I read and did not act
    // on. Counting is not checking. And stop is legal only while processing
    // (plugin.h), so a plugin whose start failed is not stopped (ADR-0123).
    if (processing_ && plugin_->stop_processing != nullptr) plugin_->stop_processing(plugin_);
    processing_ = false;
    if (plugin_->deactivate != nullptr) plugin_->deactivate(plugin_);
    activated_ = false;
}

std::int32_t ClapDevice::indexOfParam(clap_id id) const noexcept {
    for (std::size_t i = 0; i < paramIds_.size(); ++i)
        if (paramIds_[i] == id) return static_cast<std::int32_t>(i);
    return -1;
}

bool ClapDevice::outPush(const clap_output_events_t* list, const clap_event_header_t* h) {
    // A plugin reports its own parameter edits here, on the audio thread,
    // as output events of `process`: a gesture bracket and the values inside
    // it. They go into the device's ring (ADR-0110 d4: a lock-free queue to
    // the message thread, where the op is made; ADR-0124) and nowhere else.
    // Note ends and anything else are still accepted and dropped. Returning
    // false would tell the plugin we are broken.
    if (list == nullptr || h == nullptr || h->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;
    auto* self = static_cast<ClapDevice*>(list->ctx);
    if (self == nullptr) return true;
    if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
        const auto* g = reinterpret_cast<const clap_event_param_gesture_t*>(h);
        const std::int32_t i = self->indexOfParam(g->param_id);
        if (i >= 0)
            self->broadcastParam(i, h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN
                                        ? engine::ParamEventKind::Begin
                                        : engine::ParamEventKind::End, 0.0);
    } else if (h->type == CLAP_EVENT_PARAM_VALUE) {
        const auto* v = reinterpret_cast<const clap_event_param_value_t*>(h);
        const std::int32_t i = self->indexOfParam(v->param_id);
        if (i >= 0) {
            // CLAP values are PLAIN; the wire unit is normalized (ADR-0124 d2).
            const ParamDescriptor& d = self->params_[static_cast<std::size_t>(i)];
            const double span = d.maxReal - d.minReal;
            const double norm = span > 0.0 ? (v->value - d.minReal) / span : 0.0;
            self->broadcastParam(i, engine::ParamEventKind::Value, norm);
        }
    }
    return true;
}

void ClapDevice::process(const engine::NodeIo& io) noexcept {
    if (plugin_ == nullptr || !activated_ || !processing_ || plugin_->process == nullptr ||
        io.out == nullptr) { passThrough(io); return; }

    const std::int32_t n  = io.frames < maxFrames_ ? io.frames : maxFrames_;
    const std::int32_t ch = io.channels < channels_ ? io.channels : channels_;
    if (n <= 0 || ch <= 0) { passThrough(io); return; }

    // `io.in` and `io.out` address the BLOCK; `io.frames` is this segment's
    // length and `io.blockOffset` is where it starts. The plugin's own buffers
    // are segment-sized and always start at 0, so the offset applies on the
    // graph's side of every copy and nowhere else (ADR-0042).
    const std::int32_t off = io.blockOffset;
    // Bus 0 is the main input; every other declared bus is fed silence.
    // A sidechain bus the graph is not driving must still be VALID memory,
    // because the plugin will read it.
    for (std::size_t b = 0; b < inBuses_.size(); ++b) {
        Bus& bus = inBuses_[b];
        for (std::int32_t c = 0; c < bus.channels; ++c) {
            float* dst = bus.ptrs[static_cast<std::size_t>(c)];
            const float* src = (b == 0 && io.in != nullptr && c < ch)
                                 ? io.in[c] + off : nullptr;
            if (src != nullptr) for (std::int32_t i = 0; i < n; ++i) dst[i] = src[i];
            else                for (std::int32_t i = 0; i < n; ++i) dst[i] = 0.0f;
        }
    }

    // THE GRAPH'S EVENTS, which is the path that makes MPE+ real. `io.events`
    // is the span the scheduler assigned to THIS segment, already sorted by
    // frame (NodeIo). Nothing else reaches the plugin, and until now nothing
    // read it at all -- the device sent only what pushEvent had queued.
    //
    // `io.blockOffset` is subtracted because Event::frame is block-relative
    // and the plugin is being handed one segment. ADR-0078's trap, one layer
    // up, and invisible in any test whose block has a single segment.
    events_.clear();

    // ADR-0099: a dialect change asked for since the last call. Every sounding
    // note is ended IN THE DIALECT IT BEGAN IN -- a MIDI note-on is not ended
    // by a CLAP note-off -- before the new one starts.
    routed_.clear();
    const auto want = requestedDialect_.load(std::memory_order_acquire);
    if (want != appliedDialect_) {
        const ClapDialect next = resolveClapDialect(static_cast<ClapDialectChoice>(want), notePorts_);
        router_.switchTo(routeFor(next), io.blockOffset, routed_);
        for (const engine::MpeOut& o : routed_) events_.addOut(o, dialect_, io.blockOffset, n);
        routed_.clear();
        dialect_ = next;
        appliedDialect_ = want;
        dialectInUse_.store(static_cast<std::uint8_t>(dialect_), std::memory_order_release);
    }

    // The note stream, through the route the dialect implies. The graph's
    // events and the injected ones (callers with no graph), both
    // block-relative, both consumed once. A plugin with no note input port
    // is sent no notes: it declared it takes none.
    if (dialect_ != ClapDialect::None) {
        router_.route(io.events.first, io.events.count, io.blockOffset, routed_);
        router_.route(injected_.data(), static_cast<std::int32_t>(injectedUsed_),
                      io.blockOffset, routed_);
        for (const engine::MpeOut& o : routed_) events_.addOut(o, dialect_, io.blockOffset, n);
    }
    // Parameters address the plugin and are never a note stream (ADR-0091).
    for (const auto& e : io.events)
        if (!engine::isNoteStream(e.type)) events_.add(e, io.blockOffset, n);
    for (std::size_t i = 0; i < injectedUsed_; ++i)
        if (!engine::isNoteStream(injected_[i].type)) events_.add(injected_[i], io.blockOffset, n);
    injectedUsed_ = 0;

    // Queued parameter changes, at the start of this segment. CLAP parameters
    // are sample-accurate and this is the floor of that, not the ceiling:
    // once the graph drives them per segment they carry a real offset and
    // nothing here changes (ADR-0042).
    for (std::size_t i = 0; i < pendingUsed_; ++i) {
        engine::Event e;
        e.type = engine::EventType::ParamValue;
        e.paramId = static_cast<std::uint32_t>(pending_[i].id);
        e.value = pending_[i].value;
        e.frame = io.blockOffset;      // segment start, once the offset goes
        events_.add(e, io.blockOffset, n);
    }
    pendingUsed_ = 0;

    // CLAP requires input events in time order, and the list above was built
    // from three sources. Queued parameters went in last at the segment's
    // start, AFTER notes later in it, until ADR-0099 sorted the list.
    events_.sortByTime();

    for (std::size_t b = 0; b < inBuses_.size(); ++b) {
        inBufs_[b].data32 = inBuses_[b].ptrs.data();
        inBufs_[b].data64 = nullptr;
        inBufs_[b].channel_count = static_cast<std::uint32_t>(inBuses_[b].channels);
        inBufs_[b].latency = 0;
        inBufs_[b].constant_mask = 0;
    }
    for (std::size_t b = 0; b < outBuses_.size(); ++b) {
        outBufs_[b].data32 = outBuses_[b].ptrs.data();
        outBufs_[b].data64 = nullptr;
        outBufs_[b].channel_count = static_cast<std::uint32_t>(outBuses_[b].channels);
        outBufs_[b].latency = 0;
        outBufs_[b].constant_mask = 0;
    }

    clap_process_t pd{};
    pd.steady_time = steadyTime_;
    pd.frames_count = static_cast<std::uint32_t>(n);
    pd.transport = nullptr;              // free-running; ADR-0050 owns the clock
    pd.audio_inputs = inBufs_.empty() ? nullptr : inBufs_.data();
    pd.audio_outputs = outBufs_.data();
    pd.audio_inputs_count = static_cast<std::uint32_t>(inBufs_.size());
    pd.audio_outputs_count = static_cast<std::uint32_t>(outBufs_.size());
    pd.in_events = events_.inputEvents();
    pd.out_events = &outEvents_;

    const clap_process_status st = plugin_->process(plugin_, &pd);

    // CLAP_PROCESS_ERROR means the plugin did not write the output, so
    // copying it out would hand the graph uninitialised memory -- which on a
    // first block is whatever was there and on a later one is last block,
    // repeated. Silence is the only safe answer.
    if (st == CLAP_PROCESS_ERROR) {
        for (std::int32_t c = 0; c < io.channels; ++c)
            if (io.out[c] != nullptr)
                for (std::int32_t i = 0; i < io.frames; ++i) io.out[c][off + i] = 0.0f;
        events_.clear();
        return;
    }

    for (std::int32_t c = 0; c < io.channels; ++c) {
        float* out = io.out[c];
        if (out == nullptr) continue;
        out += off;
        if (!outBuses_.empty() && c < outBuses_[0].channels) {
            const float* src = outBuses_[0].ptrs[static_cast<std::size_t>(c)];
            for (std::int32_t i = 0; i < n; ++i) out[i] = src[i];
        } else {
            for (std::int32_t i = 0; i < n; ++i) out[i] = 0.0f;
        }
        // A segment shorter than the buffer leaves the tail untouched, and
        // the tail is last block's audio.
        for (std::int32_t i = n; i < io.frames; ++i) out[i] = 0.0f;
    }

    events_.clear();
    steadyTime_ += n;
}

// ---------------------------------------------------------------------------
// ClapLibrary -- loading a .clap bundle (ADR-0075 listed this as unbuilt)
// ---------------------------------------------------------------------------

namespace {

/// Where the loadable object lives inside what the user points us at.
///
/// On macOS a .clap is a BUNDLE -- a directory -- and the library is at
/// Contents/MacOS/<name>. Passing the directory to dlopen fails with a
/// message about a file that is not a Mach-O, which reads like a corrupt
/// plugin rather than like the wrong path.
std::string resolveLoadPath(const std::string& path) {
    // Only macOS wraps a .clap in a bundle. On Windows and Linux the .clap
    // IS the loadable file, so it is handed through unchanged -- which is
    // what the #if below leaves happening.
#if defined(__APPLE__)
    const std::string suffix = ".clap";
    if (path.size() > suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
        std::size_t slash = path.find_last_of('/');
        std::string leaf = (slash == std::string::npos) ? path : path.substr(slash + 1);
        leaf = leaf.substr(0, leaf.size() - suffix.size());
        return path + "/Contents/MacOS/" + leaf;
    }
#endif
    return path;
}

}  // namespace

ClapLibrary::~ClapLibrary() { close(); }

bool ClapLibrary::open(const std::string& path, std::string& error) {
    close();
    path_ = path;
    const std::string load = resolveLoadPath(path);

#if defined(_WIN32)
    lib_ = static_cast<void*>(::LoadLibraryA(load.c_str()));
    if (lib_ == nullptr) {
        error = "LoadLibrary failed, error " + std::to_string(::GetLastError());
        return false;
    }
    entry_ = reinterpret_cast<const clap_plugin_entry_t*>(
        ::GetProcAddress(static_cast<HMODULE>(lib_), "clap_entry"));
#else
    lib_ = dlopen(load.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (lib_ == nullptr) {
        const char* e = dlerror();
        error = "dlopen failed: " + std::string(e != nullptr ? e : "unknown");
        return false;
    }
    // ONE exported symbol, and that is the whole protocol.
    entry_ = static_cast<const clap_plugin_entry_t*>(dlsym(lib_, "clap_entry"));
#endif
    if (entry_ == nullptr) {
        error = "no clap_entry symbol -- not a CLAP plugin";
        close();
        return false;
    }
    if (!clap_version_is_compatible(entry_->clap_version)) {
        error = "built against an incompatible CLAP version";
        entry_ = nullptr;
        close();
        return false;
    }
    // init() takes the bundle path, NOT the library inside it: a plugin
    // finds its own resources relative to what it is handed, and handing it
    // Contents/MacOS means its factory presets are two directories away.
    if (!entry_->init(path_.c_str())) {
        error = "clap_entry->init refused";
        entry_ = nullptr;
        close();
        return false;
    }

    factory_ = static_cast<const clap_plugin_factory_t*>(
        entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (factory_ == nullptr) {
        error = "no plugin factory";
        entry_->deinit();
        entry_ = nullptr;
        close();
        return false;
    }
    return true;
}

void ClapLibrary::close() {
    // deinit BEFORE dlclose, and both after every plugin is destroyed. The
    // other order calls a destructor through a pointer into unmapped memory.
    if (entry_ != nullptr) { entry_->deinit(); entry_ = nullptr; }
    factory_ = nullptr;
    if (lib_ != nullptr) {
#if defined(_WIN32)
        ::FreeLibrary(static_cast<HMODULE>(lib_));
#else
        dlclose(lib_);
#endif
        lib_ = nullptr;
    }
}

std::uint32_t ClapLibrary::pluginCount() const noexcept {
    return factory_ != nullptr ? factory_->get_plugin_count(factory_) : 0;
}

const clap_plugin_descriptor_t* ClapLibrary::descriptorAt(std::uint32_t i) const noexcept {
    if (factory_ == nullptr || i >= pluginCount()) return nullptr;
    return factory_->get_plugin_descriptor(factory_, i);
}

const clap_plugin_t* ClapLibrary::create(const clap_host_t* host,
                                         const char* pluginId) const {
    if (factory_ == nullptr || host == nullptr || pluginId == nullptr) return nullptr;
    const clap_plugin_t* p = factory_->create_plugin(factory_, host, pluginId);
    if (p == nullptr) return nullptr;
    if (!p->init(p)) { p->destroy(p); return nullptr; }
    return p;
}

// ---------------------------------------------------------------------------
// ClapHostGlue
// ---------------------------------------------------------------------------

ClapHostGlue::ClapHostGlue() {
    host_.clap_version = CLAP_VERSION;
    host_.host_data = this;
    host_.name = "adi_daw";
    host_.vendor = "adi";
    host_.url = "https://github.com/arieladi/Adi";
    host_.version = "0.1.0";
    host_.get_extension = &ClapHostGlue::getExtension;
    host_.request_restart = &ClapHostGlue::requestRestart;
    host_.request_process = &ClapHostGlue::requestProcess;
    host_.request_callback = &ClapHostGlue::requestCallback;

    // THE EXTENSIONS WE OFFER. Until now getExtension returned nullptr for
    // everything, so a plugin could not tell us WHY it wanted a restart --
    // only that it did. CLAP distinguishes the causes and we were not
    // listening (ADR-0084).
    latencyExt_.changed = &ClapHostGlue::latencyChanged;
    portsExt_.rescan    = &ClapHostGlue::portsRescan;
    // Both functions, not one. audio-ports.h declares the host struct with
    // `is_rescan_flag_supported` beside `rescan`, and a plugin asks the first
    // before calling the second; a null there was a crash waiting for the
    // first plugin polite enough to ask (linux's audit, C1; ADR-0123).
    portsExt_.is_rescan_flag_supported = &ClapHostGlue::portsRescanSupported;
    // ADR-0099: which dialects we speak, and word that a plugin's note ports
    // changed.
    notePortsExt_.supported_dialects = &ClapHostGlue::noteDialects;
    notePortsExt_.rescan             = &ClapHostGlue::noteRescan;
}

const void* ClapHostGlue::getExtension(const clap_host_t* h, const char* id) {
    if (h == nullptr || id == nullptr) return nullptr;
    auto* self = static_cast<ClapHostGlue*>(h->host_data);
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)     return &self->latencyExt_;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &self->portsExt_;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)  return &self->notePortsExt_;
    // Returning nullptr for an unknown id is the contract, and a host that
    // lied here would have plugins calling into functions it does not
    // implement.
    return nullptr;
}

bool ClapHostGlue::portsRescanSupported(const clap_host_t*, std::uint32_t flag) {
    // Every rescan the header defines is answered the same way -- the
    // coalescer sees a shape change and a new graph is built (ADR-0090) --
    // so every defined flag is supported. The [!active] ones oblige the
    // PLUGIN to be deactivated when it asks; that obligation is the plugin's
    // (see ADR-0123 on the per-instance host this still needs).
    constexpr std::uint32_t known = CLAP_AUDIO_PORTS_RESCAN_NAMES |
                                    CLAP_AUDIO_PORTS_RESCAN_FLAGS |
                                    CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT |
                                    CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE |
                                    CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR |
                                    CLAP_AUDIO_PORTS_RESCAN_LIST;
    return flag != 0 && (flag & ~known) == 0;
}

void ClapHostGlue::latencyChanged(const clap_host_t* h) {
    // THE CHEAP PATH. A latency change is a tap move (ADR-0079), not a
    // rebuild. Report and return -- the coalescer on the message thread
    // debounces and calls retapLatency (ADR-0082).
    static_cast<ClapHostGlue*>(h->host_data)
        ->latencyChanges_.fetch_add(1, std::memory_order_release);
}

void ClapHostGlue::portsRescan(const clap_host_t* h, std::uint32_t flags) {
    // THE EXPENSIVE PATH, and only for the flags that actually change shape.
    // NAMES and FLAGS are cosmetic; CHANNEL_COUNT, PORT_TYPE, IN_PLACE_PAIR
    // and LIST change what the graph is wired to, and no tap move fixes that.
    constexpr std::uint32_t kShape =
        CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT | CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE |
        CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR | CLAP_AUDIO_PORTS_RESCAN_LIST;
    if ((flags & kShape) != 0)
        static_cast<ClapHostGlue*>(h->host_data)
            ->portChanges_.fetch_add(1, std::memory_order_release);
}

std::uint32_t ClapHostGlue::noteDialects(const clap_host_t*) { return kClapHostDialects; }

void ClapHostGlue::noteRescan(const clap_host_t* h, std::uint32_t flags) {
    // RESCAN_ALL means the ports -- and so the dialects -- may have changed;
    // it is legal only while the plugin is inactive, and a device re-reads
    // them when it next activates. NAMES is cosmetic.
    if ((flags & CLAP_NOTE_PORTS_RESCAN_ALL) != 0)
        static_cast<ClapHostGlue*>(h->host_data)
            ->noteRescans_.fetch_add(1, std::memory_order_release);
}

void ClapHostGlue::registerPlugin(const clap_plugin_t* p) {
    if (p == nullptr) return;
    for (const auto* q : plugins_) if (q == p) return;
    plugins_.push_back(p);
}

void ClapHostGlue::unregisterPlugin(const clap_plugin_t* p) {
    for (std::size_t i = 0; i < plugins_.size(); ++i) {
        if (plugins_[i] == p) { plugins_.erase(plugins_.begin() + static_cast<long>(i)); return; }
    }
}

void ClapHostGlue::dispatchMainThread() {
    const std::uint64_t want = callbacks_.load(std::memory_order_acquire);
    if (want == dispatched_) return;
    dispatched_ = want;
    // Every registered plugin, not just the one that asked: request_callback
    // carries no identity, so there is no way to know which. Calling
    // on_main_thread on a plugin that did not ask is explicitly allowed and
    // costs a no-op; not calling the one that did is silent work never done.
    for (const auto* p : plugins_)
        if (p != nullptr && p->on_main_thread != nullptr) p->on_main_thread(p);
}

void ClapHostGlue::requestRestart(const clap_host_t* h) {
    // REPORT AND RETURN. This is ADR-0066's rule arriving from the other
    // format: the plugin may call this from any thread, and rebuilding a
    // graph on that thread while it waits is the bug that ADR exists to stop.
    auto* self = static_cast<ClapHostGlue*>(h->host_data);
    const std::uint64_t n = self->restarts_.fetch_add(1, std::memory_order_acq_rel) + 1;

    // A restart with NO preceding notification has an unknown cause, and
    // ADR-0084 escalates it to a rebuild. The conservative answer differs
    // per question and this is the one that cannot corrupt: a needless
    // rebuild costs a graph swap, a missed port change plays the wrong
    // channel count.
    const std::uint64_t explained =
        self->latencyChanges_.load(std::memory_order_acquire) +
        self->portChanges_.load(std::memory_order_acquire);
    if (n > explained)
        self->unexplained_.fetch_add(1, std::memory_order_release);
}
void ClapHostGlue::requestProcess(const clap_host_t* h) {
    static_cast<ClapHostGlue*>(h->host_data)->processes_.fetch_add(
        1, std::memory_order_release);
}
void ClapHostGlue::requestCallback(const clap_host_t* h) {
    static_cast<ClapHostGlue*>(h->host_data)->callbacks_.fetch_add(
        1, std::memory_order_release);
}


// ---------------------------------------------------------------------------
// ClapHost -- the sibling of Vst3Host
// ---------------------------------------------------------------------------

ClapHost::ClapHost() = default;

namespace {

/// `std::getenv`, with MSVC's C4996 silenced at ONE site instead of four.
///
/// MSVC deprecates it because the returned pointer is invalidated by a later
/// `getenv`/`putenv` on another thread. We read a handful of variables once, at
/// startup, on the message thread, and copy each into a `std::string`
/// immediately -- so the hazard it warns about cannot arise. The suggested
/// replacement, `_dupenv_s`, is Windows-only and would put an `#ifdef` at every
/// call site rather than in this one function.
const char* envOr(const char* name) noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

}  // namespace

std::vector<std::string> ClapHost::defaultSearchPaths() {
    std::vector<std::string> out;

    // CLAP_PATH first, because a user who sets it means it. Colon-separated
    // on POSIX, semicolon on Windows -- the same convention as PATH itself.
    if (const char* env = envOr("CLAP_PATH")) {
#if defined(_WIN32)
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::string s(env), cur;
        for (char c : s) {
            if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
    }

    // DECLARED ONLY WHERE IT IS USED. On Windows nothing reads HOME, and an
    // initialised-but-unreferenced local is C4189 -- fatal under -Werror, and
    // invisible on a platform that does read it.
#if !defined(_WIN32)
    const char* home = envOr("HOME");
#endif
#if defined(__APPLE__)
    out.emplace_back("/Library/Audio/Plug-Ins/CLAP");
    if (home != nullptr) out.emplace_back(std::string(home) + "/Library/Audio/Plug-Ins/CLAP");
#elif defined(_WIN32)
    if (const char* pf = envOr("COMMONPROGRAMFILES"))
        out.emplace_back(std::string(pf) + "\\CLAP");
    if (const char* la = envOr("LOCALAPPDATA"))
        out.emplace_back(std::string(la) + "\\Programs\\Common\\CLAP");
#else
    out.emplace_back("/usr/lib/clap");
    out.emplace_back("/usr/local/lib/clap");
    if (home != nullptr) out.emplace_back(std::string(home) + "/.clap");
#endif
    return out;
}

ClapLibrary* ClapHost::libraryFor(const std::string& path, std::string& error) {
    for (auto& [p, lib] : libs_)
        if (p == path) return lib.get();

    auto lib = std::make_unique<ClapLibrary>();
    if (!lib->open(path, error)) return nullptr;
    libs_.emplace_back(path, std::move(lib));
    return libs_.back().second.get();
}

std::vector<std::string> ClapHost::findBundles(const std::vector<std::string>& paths) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    for (const auto& dir : paths) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        // skip_permission_denied: one unreadable vendor folder must not end
        // the walk for everything after it.
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            if (ext != ".clap") continue;
            out.push_back(it->path().string());
            std::error_code dirEc;
            if (it->is_directory(dirEc)) it.disable_recursion_pending();   // a macOS bundle
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void ClapHost::scan(const std::vector<std::string>& paths) {
    found_.clear();
    for (const std::string& p : findBundles(paths)) {
        std::string err;
        ClapLibrary* lib = libraryFor(p, err);
        if (lib == nullptr) continue;      // not ours to report; a browser shows what loaded

        const std::uint32_t n = lib->pluginCount();
        for (std::uint32_t i = 0; i < n; ++i) {
            const clap_plugin_descriptor_t* d = lib->descriptorAt(i);
            if (d == nullptr || d->id == nullptr) continue;
            ClapPluginRef r;
            r.bundlePath = p;
            r.id      = d->id;
            r.name    = d->name    != nullptr ? d->name    : "";
            r.vendor  = d->vendor  != nullptr ? d->vendor  : "";
            r.version = d->version != nullptr ? d->version : "";
            for (const char* const* f = d->features; f != nullptr && *f != nullptr; ++f) {
                if (!r.features.empty()) r.features += ',';
                r.features += *f;
                if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0)
                    r.isInstrument = true;
            }
            found_.push_back(std::move(r));
        }
    }
}

std::unique_ptr<DeviceInstance> ClapHost::makeDevice(const ClapPluginRef& ref,
                                                     double sampleRate,
                                                     std::int32_t blockSize,
                                                     std::string& error) {
    DeviceIdentity id;
    id.format  = "clap";
    id.uid     = ref.id;
    id.name    = ref.name;
    id.vendor  = ref.vendor;
    id.version = ref.version;

    ClapLibrary* lib = libraryFor(ref.bundlePath, error);
    const clap_plugin_t* p = (lib != nullptr) ? lib->create(glue_.host(), ref.id.c_str())
                                              : nullptr;
    if (p == nullptr) {
        // ADR-0011 / SPEC 7.1. Not nullptr, not an exception, not a skip:
        // the device stays in the chain as a bypassed placeholder carrying
        // the identity, so the signal path is unchanged and the user is told
        // what is missing rather than that "a plugin" is.
        if (error.empty()) error = "the factory refused to create " + ref.id;
        return std::make_unique<MissingDevice>(id);
    }
    glue_.registerPlugin(p);
    auto dev = std::make_unique<ClapDevice>(p, id);
    dev->prepare(sampleRate, blockSize);
    return dev;
}

}  // namespace adi::device
