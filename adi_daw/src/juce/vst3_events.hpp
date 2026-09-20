// SPDX-License-Identifier: GPL-3.0-or-later
//
// The VST3 side of the event path: our constants checked against Steinberg's,
// and the `IEventList` a plugin is handed.
//
// `adi/engine/note_expression.hpp` does the conversion and has no SDK headers
// in it, so it is tested on all seven ABIs. The risk that split creates is
// that our copy of Steinberg's type ids drifts from theirs — silently, because
// a wrong-but-valid type id produces a well-formed event carrying the wrong
// meaning, and the note gets brighter instead of sharper.
//
// So every constant is asserted here, at compile time, in the one build that
// has the SDK. Drift is a build failure in the JUCE job rather than a bug
// report from somebody with a Continuum.

#pragma once

#include "adi/engine/events.hpp"
#include "adi/engine/note_expression.hpp"

#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>

#include <cstdint>
#include <vector>

namespace adi::device {

namespace SV = Steinberg::Vst;

// --- our copy of Steinberg's enum, checked against Steinberg's --------------

static_assert(static_cast<std::uint32_t>(engine::Vst3NoteExprType::Volume)
              == SV::kVolumeTypeID, "kVolumeTypeID drifted");
static_assert(static_cast<std::uint32_t>(engine::Vst3NoteExprType::Pan)
              == SV::kPanTypeID, "kPanTypeID drifted");
static_assert(static_cast<std::uint32_t>(engine::Vst3NoteExprType::Tuning)
              == SV::kTuningTypeID, "kTuningTypeID drifted");
static_assert(static_cast<std::uint32_t>(engine::Vst3NoteExprType::Vibrato)
              == SV::kVibratoTypeID, "kVibratoTypeID drifted");
static_assert(static_cast<std::uint32_t>(engine::Vst3NoteExprType::Expression)
              == SV::kExpressionTypeID, "kExpressionTypeID drifted");
static_assert(static_cast<std::uint32_t>(engine::Vst3NoteExprType::Brightness)
              == SV::kBrightnessTypeID, "kBrightnessTypeID drifted");

/// `NoteExpressionValue` must be a double, or the mandate dies at the last
/// hop. ADR-0054 names this exact failure and it would be invisible: a float
/// still holds a 14-bit value, so nothing breaks until somebody chains two
/// conversions.
static_assert(sizeof(SV::NoteExpressionValue) == 8,
              "VST3's NoteExpressionValue is not a double -- ADR-0054");

/// And VST3's own note id is signed 32-bit, while ours is unsigned 64.
/// -1 means "not set" on their side, which is what JUCE hardcodes and what
/// makes per-note expression unaddressable through its path (ADR-0057).
static_assert(sizeof(SV::NoteOnEvent::noteId) == 4, "VST3 noteId is int32");

/// Narrow our 64-bit note id into VST3's int32, never producing their
/// "unset" sentinel.
///
/// A session can outlive 2^31 notes only in principle, but wrapping onto -1
/// would turn one note's expression into global expression, silently. The
/// modulo keeps it positive and the id stays unique for any plausible
/// session; the important part is that -1 is unreachable.
[[nodiscard]] inline Steinberg::int32 toVst3NoteId(std::uint64_t id) noexcept {
    if (id == 0) return -1;                       // genuinely unassigned
    return static_cast<Steinberg::int32>(id % 0x7FFFFFFFu);
}

/// A fixed-capacity `IEventList` we fill and hand to a plugin.
///
/// Capacity comes from `reserve` at prepare, not from a constant. Steinberg's
/// Host Checker states no more than 2048 events at once, and JUCE hardcodes
/// exactly that and `break`s past it, silently. ADR-0056's arithmetic says
/// 2048 is polyphony 16 at 500 Hz across three dimensions — so the limit is
/// reached by the instrument ADR-0054 was written for, not by an edge case.
/// Overflow is COUNTED here.
class Vst3EventList final : public SV::IEventList {
public:
    void reserve(std::int32_t n) { events_.reserve(static_cast<std::size_t>(n)); cap_ = n; }
    void clear() noexcept { events_.clear(); }

    [[nodiscard]] std::int64_t dropped() const noexcept { return dropped_; }
    void resetDropped() noexcept { dropped_ = 0; }

    /// Translate one ADI event. Returns false when it has no VST3 form, which
    /// is not an error: a ParamMod has its own path.
    bool add(const engine::Event& e) noexcept;

    // --- IEventList ---------------------------------------------------------
    Steinberg::int32 PLUGIN_API getEventCount() override {
        return static_cast<Steinberg::int32>(events_.size());
    }
    Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index, SV::Event& e) override;
    Steinberg::tresult PLUGIN_API addEvent(SV::Event& e) override;

    // --- FUnknown: this object is owned by us and outlives every call into
    // the plugin, so reference counting is deliberately a no-op rather than
    // a partial implementation somebody might trust.
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID id, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1000; }
    Steinberg::uint32 PLUGIN_API release() override { return 1000; }

private:
    std::vector<SV::Event> events_;
    std::int32_t cap_ = 0;
    std::int64_t dropped_ = 0;
};

}  // namespace adi::device
