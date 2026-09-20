// SPDX-License-Identifier: GPL-3.0-or-later

#include "juce/vst3_events.hpp"

#include <cstring>

namespace adi::device {

bool Vst3EventList::add(const engine::Event& in) noexcept {
    if (cap_ > 0 && static_cast<std::int32_t>(events_.size()) >= cap_) {
        // Counted, not a silent break. JUCE's own path stops at 2048 and says
        // nothing, which is how ten notes of MPE+ lose their packets with
        // every test still green (ADR-0057).
        ++dropped_;
        return false;
    }

    SV::Event e{};
    e.busIndex = 0;
    e.sampleOffset = in.frame;
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

}  // namespace adi::device
