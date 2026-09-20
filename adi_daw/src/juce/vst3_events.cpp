// SPDX-License-Identifier: GPL-3.0-or-later

#include "juce/vst3_events.hpp"

#include <cstring>

namespace adi::device {

bool Vst3EventList::add(const engine::Event& in, std::int32_t blockOffset,
                        std::int32_t segmentFrames) noexcept {
    if (cap_ > 0 && static_cast<std::int32_t>(events_.size()) >= cap_) {
        // Counted, not a silent break. JUCE's own path stops at 2048 and says
        // nothing, which is how ten notes of MPE+ lose their packets with
        // every test still green (ADR-0057).
        ++dropped_;
        return false;
    }

    // BLOCK-relative in, SEGMENT-relative out (ADR-0081).
    const std::int32_t t = in.frame - blockOffset;
    if (t < 0) { ++outOfRange_; return false; }
    if (segmentFrames > 0 && t >= segmentFrames) { ++outOfRange_; return false; }

    SV::Event e{};
    e.busIndex = 0;
    e.sampleOffset = t;
    e.ppqPosition = 0.0;
    e.flags = SV::Event::kIsLive;

    switch (in.type) {
        case engine::EventType::NoteOn:
            e.type = SV::Event::kNoteOnEvent;
            e.noteOn.channel  = static_cast<Steinberg::int16>(in.channel > 0 ? in.channel - 1 : 0);
            e.noteOn.pitch    = static_cast<Steinberg::int16>(in.dim);
            e.noteOn.tuning   = 0.0f;
            e.noteOn.velocity = static_cast<float>(in.value);
            e.noteOn.length   = 0;
            // THE LINE JUCE HARDCODES TO -1. Without a real id here, VST3 has
            // nothing to anchor note expression to, and every per-note value
            // becomes a channel-wide one.
            e.noteOn.noteId   = toVst3NoteId(in.noteId);
            break;

        case engine::EventType::NoteOff:
            e.type = SV::Event::kNoteOffEvent;
            e.noteOff.channel  = static_cast<Steinberg::int16>(in.channel > 0 ? in.channel - 1 : 0);
            e.noteOff.pitch    = static_cast<Steinberg::int16>(in.dim);
            e.noteOff.velocity = static_cast<float>(in.value);
            e.noteOff.tuning   = 0.0f;
            e.noteOff.noteId   = toVst3NoteId(in.noteId);
            break;

        case engine::EventType::NoteExpression: {
            engine::Vst3NoteExprType t{};
            double norm = 0.0;
            if (!engine::toVst3NoteExpression(
                    static_cast<ExpressionDim>(in.dim), in.value, t, norm))
                return false;
            e.type = SV::Event::kNoteExpressionValueEvent;
            e.noteExpressionValue.typeId = static_cast<SV::NoteExpressionTypeID>(t);
            e.noteExpressionValue.noteId = toVst3NoteId(in.noteId);
            // A double, all the way to the plugin. This is the last hop of
            // ADR-0054 and the one JUCE never reaches at all.
            e.noteExpressionValue.value = norm;
            break;
        }

        // A parameter change is not an event in VST3 -- it travels in
        // IParameterChanges alongside the event list. Returning false rather
        // than inventing an event is what stops a caller assuming it was sent.
        case engine::EventType::ParamValue:
        case engine::EventType::ParamMod:
            return false;
    }

    events_.push_back(e);
    return true;
}

Steinberg::tresult PLUGIN_API Vst3EventList::getEvent(Steinberg::int32 index, SV::Event& e) {
    if (index < 0 || index >= static_cast<Steinberg::int32>(events_.size()))
        return Steinberg::kInvalidArgument;
    e = events_[static_cast<std::size_t>(index)];
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Vst3EventList::addEvent(SV::Event& e) {
    if (cap_ > 0 && static_cast<std::int32_t>(events_.size()) >= cap_) {
        ++dropped_;
        return Steinberg::kResultFalse;
    }
    events_.push_back(e);
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Vst3EventList::queryInterface(const Steinberg::TUID id, void** obj) {
    if (obj == nullptr) return Steinberg::kInvalidArgument;
    if (std::memcmp(id, SV::IEventList::iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(id, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0) {
        *obj = static_cast<SV::IEventList*>(this);
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}


// ---------------------------------------------------------------------------
// Parameter changes (ADR-0073)
// ---------------------------------------------------------------------------

Steinberg::tresult PLUGIN_API Vst3ParamQueue::getPoint(Steinberg::int32 index,
                                                       Steinberg::int32& sampleOffset,
                                                       SV::ParamValue& value) {
    if (index < 0 || index >= static_cast<Steinberg::int32>(points_.size()))
        return Steinberg::kInvalidArgument;
    const auto& p = points_[static_cast<std::size_t>(index)];
    sampleOffset = p.first;
    value = p.second;
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Vst3ParamQueue::addPoint(Steinberg::int32 sampleOffset,
                                                       SV::ParamValue value,
                                                       Steinberg::int32& index) {
    // Clamped, because the SDK does not define behaviour outside 0..1 and a
    // plugin handed 1.4 is entitled to do anything at all.
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;
    if (sampleOffset < 0) sampleOffset = 0;

    // Kept in sample order. VST3 says points are a queue and plugins read
    // them in order; an out-of-order point makes a ramp jump backwards
    // mid-block, which sounds like a click rather than like a bug.
    auto at = points_.end();
    while (at != points_.begin() && (at - 1)->first > sampleOffset) --at;
    at = points_.insert(at, {sampleOffset, value});
    index = static_cast<Steinberg::int32>(at - points_.begin());
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Vst3ParamQueue::queryInterface(const Steinberg::TUID id,
                                                             void** obj) {
    if (obj == nullptr) return Steinberg::kInvalidArgument;
    if (std::memcmp(id, SV::IParamValueQueue::iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(id, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0) {
        *obj = static_cast<SV::IParamValueQueue*>(this);
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

// ---------------------------------------------------------------------------

void Vst3ParamChanges::reserve(std::int32_t n) {
    // Allocated at prepare, never in process. ADR-0010.
    pool_.clear();
    pool_.reserve(static_cast<std::size_t>(n));
    for (std::int32_t i = 0; i < n; ++i)
        pool_.push_back(std::make_unique<Vst3ParamQueue>());
    used_ = 0;
}

void Vst3ParamChanges::clear() noexcept { used_ = 0; }

SV::IParamValueQueue* PLUGIN_API Vst3ParamChanges::getParameterData(Steinberg::int32 index) {
    if (index < 0 || index >= static_cast<Steinberg::int32>(used_)) return nullptr;
    return pool_[static_cast<std::size_t>(index)].get();
}

SV::IParamValueQueue* PLUGIN_API Vst3ParamChanges::addParameterData(const SV::ParamID& id,
                                                                    Steinberg::int32& index) {
    // A plugin may call this during process to report its own changes back.
    // Existing queue first: two queues for one ParamID is malformed, and some
    // plugins will read only the first.
    for (std::size_t i = 0; i < used_; ++i) {
        if (pool_[i]->id() == id) {
            index = static_cast<Steinberg::int32>(i);
            return pool_[i].get();
        }
    }
    if (used_ >= pool_.size()) { ++dropped_; index = 0; return nullptr; }
    auto* q = pool_[used_].get();
    q->reset(id);
    index = static_cast<Steinberg::int32>(used_);
    ++used_;
    return q;
}

bool Vst3ParamChanges::set(SV::ParamID id, double normalized,
                           std::int32_t sampleOffset) noexcept {
    Steinberg::int32 index = 0;
    auto* q = addParameterData(id, index);
    if (q == nullptr) return false;     // pool exhausted; already counted
    Steinberg::int32 pointIndex = 0;
    q->addPoint(sampleOffset, normalized, pointIndex);
    return true;
}

Steinberg::tresult PLUGIN_API Vst3ParamChanges::queryInterface(const Steinberg::TUID id,
                                                               void** obj) {
    if (obj == nullptr) return Steinberg::kInvalidArgument;
    if (std::memcmp(id, SV::IParameterChanges::iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(id, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0) {
        *obj = static_cast<SV::IParameterChanges*>(this);
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

}  // namespace adi::device
