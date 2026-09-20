// SPDX-License-Identifier: GPL-3.0-or-later
//
// See clap_host.hpp for why this file has no JUCE in it.

#include "juce/clap_host.hpp"

#include <algorithm>
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

    Slot slot{};
    switch (in.type) {
        case engine::EventType::NoteOn:
        case engine::EventType::NoteOff: {
            slot.note = clap_event_note_t{};
            slot.note.header.size     = sizeof(clap_event_note_t);
            slot.note.header.time     = time;
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
            slot.expr.header.time     = time;
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

// ---------------------------------------------------------------------------
// ClapDevice
// ---------------------------------------------------------------------------

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
    if (plugin_ == nullptr || latencyExt_ == nullptr) return 0;

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
    injected_.assign(static_cast<std::size_t>((perNote > 0 ? perNote : 8) * 16),
                     engine::Event{});
    injectedUsed_ = 0;

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

    // THE GRAPH'S EVENTS, which is the path that makes MPE+ real. `io.events`
    // is the span the scheduler assigned to THIS segment, already sorted by
    // frame (NodeIo). Nothing else reaches the plugin, and until now nothing
    // read it at all -- the device sent only what pushEvent had queued.
    //
    // `io.blockOffset` is subtracted because Event::frame is block-relative
    // and the plugin is being handed one segment. ADR-0078's trap, one layer
    // up, and invisible in any test whose block has a single segment.
    events_.clear();
    for (const auto& e : io.events) events_.add(e, io.blockOffset, n);

    // Injected events, for callers with no graph. Also block-relative, so
    // they take the same subtraction, and they are consumed once.
    for (std::size_t i = 0; i < injectedUsed_; ++i)
        events_.add(injected_[i], io.blockOffset, n);
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
}

const void* ClapHostGlue::getExtension(const clap_host_t* h, const char* id) {
    if (h == nullptr || id == nullptr) return nullptr;
    auto* self = static_cast<ClapHostGlue*>(h->host_data);
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)     return &self->latencyExt_;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &self->portsExt_;
    // Returning nullptr for an unknown id is the contract, and a host that
    // lied here would have plugins calling into functions it does not
    // implement.
    return nullptr;
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

std::vector<std::string> ClapHost::defaultSearchPaths() {
    std::vector<std::string> out;

    // CLAP_PATH first, because a user who sets it means it. Colon-separated
    // on POSIX, semicolon on Windows -- the same convention as PATH itself.
    if (const char* env = std::getenv("CLAP_PATH")) {
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

    const char* home = std::getenv("HOME");
#if defined(__APPLE__)
    out.emplace_back("/Library/Audio/Plug-Ins/CLAP");
    if (home != nullptr) out.emplace_back(std::string(home) + "/Library/Audio/Plug-Ins/CLAP");
#elif defined(_WIN32)
    if (const char* pf = std::getenv("COMMONPROGRAMFILES"))
        out.emplace_back(std::string(pf) + "\\CLAP");
    if (const char* la = std::getenv("LOCALAPPDATA"))
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

void ClapHost::scan(const std::vector<std::string>& paths) {
    found_.clear();
    for (const auto& dir : paths) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            const std::string p = e.path().string();
            if (p.size() < 5 || p.compare(p.size() - 5, 5, ".clap") != 0) continue;

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
