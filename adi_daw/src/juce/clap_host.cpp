// SPDX-License-Identifier: GPL-3.0-or-later
//
// See clap_host.hpp for why this file has no JUCE in it.

#include "juce/clap_host.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

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

bool ClapEventList::add(const engine::Event& in) noexcept {
    if (cap_ > 0 && static_cast<std::int32_t>(events_.size()) >= cap_) {
        ++dropped_;
        return false;
    }

    Slot slot{};
    switch (in.type) {
        case engine::EventType::NoteOn:
        case engine::EventType::NoteOff: {
            slot.note = clap_event_note_t{};
            slot.note.header.size     = sizeof(clap_event_note_t);
            slot.note.header.time     = static_cast<std::uint32_t>(in.frame);
            slot.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            // CLAP's event constants are an unnamed int enum and `type` is a
            // uint16_t, so the assignment narrows. MSVC /W4 raises C4244 and
            // -Werror makes it fatal; the cast says the narrowing is intended
            // rather than leaving a warning the Windows job stops on.
            slot.note.header.type     = static_cast<std::uint16_t>(
                (in.type == engine::EventType::NoteOn) ? CLAP_EVENT_NOTE_ON
                                                       : CLAP_EVENT_NOTE_OFF);
            slot.note.header.flags    = 0;
            // A REAL note id, not -1. CLAP anchors note expression to it in
            // exactly the way VST3 does -- the difference is that JUCE's VST3
            // host hardcodes -1 and we are not going through anyone's host.
            slot.note.note_id    = (in.noteId == 0)
                                     ? -1
                                     : static_cast<std::int32_t>(in.noteId % 0x7FFFFFFFu);
            slot.note.port_index = 0;
            slot.note.channel    = static_cast<std::int16_t>(in.channel > 0 ? in.channel - 1 : 0);
            slot.note.key        = static_cast<std::int16_t>(in.dim);
            slot.note.velocity   = in.value;          // a double, 0..1
            break;
        }

        case engine::EventType::NoteExpression: {
            const std::int32_t id = clapExprFor(static_cast<ExpressionDim>(in.dim));
            if (id < 0) return false;
            slot.expr = clap_event_note_expression_t{};
            slot.expr.header.size     = sizeof(clap_event_note_expression_t);
            slot.expr.header.time     = static_cast<std::uint32_t>(in.frame);
            slot.expr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            slot.expr.header.type     = CLAP_EVENT_NOTE_EXPRESSION;
            slot.expr.header.flags    = 0;
            slot.expr.expression_id   = id;
            slot.expr.note_id    = (in.noteId == 0)
                                     ? -1
                                     : static_cast<std::int32_t>(in.noteId % 0x7FFFFFFFu);
            slot.expr.port_index = 0;
            slot.expr.channel    = static_cast<std::int16_t>(in.channel > 0 ? in.channel - 1 : 0);
            slot.expr.key        = -1;                // wildcard: the note_id decides
            // NO CONVERSION for tuning. CLAP's TUNING is "relative tuning in
            // semitones, from -120 to +120", which is the unit engine::Event
            // already carries. The clamp is still applied, because an MPE zone
            // configured beyond +/-120 would otherwise exceed the range CLAP's
            // API declares. VST3 needed norm = plain/240 + 0.5 here.
            slot.expr.value = (id == CLAP_NOTE_EXPRESSION_TUNING)
                                ? semitonesToClapTuning(in.value)
                                : std::clamp(in.value, 0.0, 1.0);
            break;
        }

        case engine::EventType::ParamValue:
        case engine::EventType::ParamMod: {
            slot.param = clap_event_param_value_t{};
            slot.param.header.size     = sizeof(clap_event_param_value_t);
            slot.param.header.time     = static_cast<std::uint32_t>(in.frame);
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

// ---------------------------------------------------------------------------
// ClapDevice
// ---------------------------------------------------------------------------

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
    rescanParams();
}

ClapDevice::~ClapDevice() {
    if (plugin_ == nullptr) return;
    if (activated_) { plugin_->stop_processing(plugin_); plugin_->deactivate(plugin_); }
    plugin_->destroy(plugin_);
}

void ClapDevice::rescanParams() {
    params_.clear();
    paramIds_.clear();
    if (plugin_ == nullptr || paramsExt_ == nullptr) return;

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
    if (plugin_ == nullptr || paramsExt_ == nullptr) return {};
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
    // setter -- values are sample-accurate and arrive with the block. So this
    // records intent; the event goes in on the next process. Deliberately not
    // faked with a flush(), which would place the change at the block
    // boundary and lose the offset ADR-0042 exists to preserve.
    for (std::size_t i = 0; i < params_.size(); ++i) {
        if (params_[i].id != paramId) continue;
        if (pendingUsed_ >= pending_.size()) { ++pendingDropped_; return false; }
        const double span = params_[i].maxReal - params_[i].minReal;
        // CLAP parameter events carry the PLAIN value, not a normalised one.
        // Sending 0.42 to a 20 Hz..20 kHz cutoff would set it to 0.42 Hz --
        // a valid number in the wrong unit, which is the failure mode this
        // whole contract exists to keep visible (ADR-0057, ADR-0075).
        pending_[pendingUsed_].id = paramIds_[i];
        pending_[pendingUsed_].value =
            v.hasReal ? v.real : params_[i].minReal + v.normalized * span;
        ++pendingUsed_;
        return true;
    }
    return false;
}

std::vector<std::string> ClapDevice::stateRoles() const {
    if (plugin_ == nullptr || stateExt_ == nullptr) return {};
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
    if (plugin_ == nullptr || stateExt_ == nullptr || role != "chunk") return {};
    std::vector<std::uint8_t> out;
    VecOut sink(out);
    if (!stateExt_->save(plugin_, &sink.os)) return {};
    return out;
}

bool ClapDevice::loadState(const std::string& role, const std::vector<std::uint8_t>& b) {
    if (plugin_ == nullptr || stateExt_ == nullptr || role != "chunk") return false;
    VecIn src(b);
    return stateExt_->load(plugin_, &src.is);
}

std::int64_t ClapDevice::tailSamples() const noexcept {
    if (plugin_ == nullptr || tailExt_ == nullptr) return engine::kInfiniteTail;
    const std::uint32_t t = tailExt_->get(plugin_);
    // CLAP spells an unbounded tail UINT32_MAX; ours is INT64_MAX. Mapped
    // rather than cast, or "never suspend" becomes 4294967295 samples --
    // twenty-four hours, which is wrong in a way nobody would ever see.
    if (t == UINT32_MAX) return engine::kInfiniteTail;
    return static_cast<std::int64_t>(t);
}

std::int32_t ClapDevice::latencySamples() const noexcept {
    if (plugin_ == nullptr || latencyExt_ == nullptr) return 0;
    const std::uint32_t l = latencyExt_->get(plugin_);
    // Clamped into int32. A plugin reporting a latency larger than that is
    // reporting nonsense, and a negative compensation moves audio EARLIER
    // than its source.
    if (l > 0x7FFFFFFFu) return 0x7FFFFFFF;
    return static_cast<std::int32_t>(l);
}

void ClapDevice::prepare(double sampleRate, std::int32_t maxFrames) {
    if (plugin_ == nullptr) return;
    if (activated_) { plugin_->stop_processing(plugin_); plugin_->deactivate(plugin_); }
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    // ADR-0049: the GRANTED size, as both the min and the max. A plugin told
    // a larger maximum than it will get allocates more than it needs; one
    // told a smaller maximum overruns when the driver hands over a full block.
    activated_ = plugin_->activate(plugin_, sampleRate,
                                   static_cast<std::uint32_t>(maxFrames),
                                   static_cast<std::uint32_t>(maxFrames));
    if (activated_) plugin_->start_processing(plugin_);

    // Everything the audio thread touches, allocated here and never again
    // (ADR-0010). Sized from the GRANTED block size (ADR-0049).
    const auto n = static_cast<std::size_t>(maxFrames) * static_cast<std::size_t>(channels_);
    inScratch_.assign(n, 0.0f);
    outScratch_.assign(n, 0.0f);
    inPtrs_.resize(static_cast<std::size_t>(channels_));
    outPtrs_.resize(static_cast<std::size_t>(channels_));
    for (std::int32_t c = 0; c < channels_; ++c) {
        const auto off = static_cast<std::size_t>(c) * static_cast<std::size_t>(maxFrames);
        inPtrs_[static_cast<std::size_t>(c)]  = inScratch_.data() + off;
        outPtrs_[static_cast<std::size_t>(c)] = outScratch_.data() + off;
    }

    // ADR-0056's arithmetic, applied to this device rather than re-derived:
    // 500 Hz MPE+ across the block, three dimensions per note, at the
    // polyphony the graph is configured for -- plus room for note on/off.
    const double frames = static_cast<double>(maxFrames);
    const double updateFrames = (sampleRate > 0.0) ? (frames / sampleRate) * 500.0 : 1.0;
    const auto perNote = static_cast<std::int32_t>(updateFrames * 3.0) + 2;
    events_.reserve((perNote > 0 ? perNote : 8) * 16);

    pending_.assign(256, PendingParam{});
    pendingUsed_ = 0;

    outEvents_.ctx = this;
    outEvents_.try_push = &ClapDevice::outPush;
}

void ClapDevice::release() {
    if (plugin_ == nullptr || !activated_) return;
    plugin_->stop_processing(plugin_);
    plugin_->deactivate(plugin_);
    activated_ = false;
}

bool ClapDevice::outPush(const clap_output_events_t*, const clap_event_header_t*) {
    // A plugin may report its own parameter changes and note ends back to us.
    // Accepted and discarded for now: routing them into the op log is
    // ADR-0038's business and needs the message thread, which is not this
    // one. Returning false would tell the plugin we are broken.
    return true;
}

void ClapDevice::process(const engine::NodeIo& io) noexcept {
    if (plugin_ == nullptr || !activated_ || io.out == nullptr) { passThrough(io); return; }

    const std::int32_t n  = io.frames < maxFrames_ ? io.frames : maxFrames_;
    const std::int32_t ch = io.channels < channels_ ? io.channels : channels_;
    if (n <= 0 || ch <= 0) { passThrough(io); return; }

    // `io.in` and `io.out` address the BLOCK; `io.frames` is this segment's
    // length and `io.blockOffset` is where it starts. The plugin's own buffers
    // are segment-sized and always start at 0, so the offset applies on the
    // graph's side of every copy and nowhere else (ADR-0042).
    const std::int32_t off = io.blockOffset;
    for (std::int32_t c = 0; c < ch; ++c) {
        float* dst = inPtrs_[static_cast<std::size_t>(c)];
        const float* src = (io.in != nullptr) ? io.in[c] + off : nullptr;
        if (src != nullptr) for (std::int32_t i = 0; i < n; ++i) dst[i] = src[i];
        else                for (std::int32_t i = 0; i < n; ++i) dst[i] = 0.0f;
    }

    // Queued parameter changes go in as EVENTS at offset 0. CLAP parameters
    // are sample-accurate and this is the floor of that, not the ceiling:
    // once the graph drives them per segment they carry a real offset and
    // nothing here changes (ADR-0042).
    for (std::size_t i = 0; i < pendingUsed_; ++i) {
        engine::Event e;
        e.type = engine::EventType::ParamValue;
        e.paramId = static_cast<std::uint32_t>(pending_[i].id);
        e.value = pending_[i].value;
        e.frame = 0;
        events_.add(e);
    }
    pendingUsed_ = 0;

    clap_audio_buffer_t inBus{};
    inBus.data32 = inPtrs_.data();
    inBus.data64 = nullptr;
    inBus.channel_count = static_cast<std::uint32_t>(ch);
    inBus.latency = 0;
    inBus.constant_mask = 0;

    clap_audio_buffer_t outBus = inBus;
    outBus.data32 = outPtrs_.data();

    clap_process_t pd{};
    pd.steady_time = steadyTime_;
    pd.frames_count = static_cast<std::uint32_t>(n);
    pd.transport = nullptr;              // free-running; ADR-0050 owns the clock
    pd.audio_inputs = &inBus;
    pd.audio_outputs = &outBus;
    pd.audio_inputs_count = 1;
    pd.audio_outputs_count = 1;
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
        if (c < ch) {
            const float* src = outPtrs_[static_cast<std::size_t>(c)];
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
}

const void* ClapHostGlue::getExtension(const clap_host_t*, const char*) {
    // Nothing offered yet. Returning nullptr for an unknown id is the
    // contract, and a host that lied here would have plugins calling into
    // functions it does not implement.
    return nullptr;
}

void ClapHostGlue::requestRestart(const clap_host_t* h) {
    // REPORT AND RETURN. This is ADR-0066's rule arriving from the other
    // format: the plugin may call this from any thread, and rebuilding a
    // graph on that thread while it waits is the bug that ADR exists to stop.
    ++static_cast<ClapHostGlue*>(h->host_data)->restarts_;
}
void ClapHostGlue::requestProcess(const clap_host_t* h) {
    ++static_cast<ClapHostGlue*>(h->host_data)->processes_;
}
void ClapHostGlue::requestCallback(const clap_host_t* h) {
    ++static_cast<ClapHostGlue*>(h->host_data)->callbacks_;
}

}  // namespace adi::device
