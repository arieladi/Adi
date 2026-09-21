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
#include "adi/engine/mpe_output.hpp"
#include "adi/engine/note_expression.hpp"

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>

#include <cstdint>
#include <memory>
#include <utility>
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

// --- ADR-0097's constants, checked the same way ----------------------------

static_assert(engine::kCtrlAfterTouch == SV::kAfterTouch, "kAfterTouch drifted");
static_assert(engine::kCtrlPitchBend == SV::kPitchBend, "kPitchBend drifted");
static_assert(engine::kNoParam == SV::kNoParamId, "kNoParamId drifted");

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

    /// Translate one ADI event.
    ///
    /// `blockOffset` is subtracted and `segmentFrames` bounds the result, for
    /// the reason ADR-0081 gives for CLAP: `engine::Event::frame` is
    /// BLOCK-relative and a plugin handed one segment wants offsets inside
    /// that segment. The trap is identical in both formats and invisible in
    /// any block with a single segment.
    ///
    /// False when the event has no VST3 form — a ParamMod has its own path —
    /// or when it falls outside the segment, which is COUNTED rather than
    /// silently dropped (ADR-0081: a refusal that looks like absence is worse
    /// than a wrong value).
    bool add(const engine::Event& e, std::int32_t blockOffset = 0,
             std::int32_t segmentFrames = 0) noexcept;

    /// Translate one output of `engine::MpeRouter` (ADR-0097). The same
    /// offset rule and the same counting as `add`, which is now this applied
    /// to `engine::noteExpressionOut` -- one translation, not two.
    ///
    /// False, and not counted, for a `Control` that carries a parameter id:
    /// that is a parameter change, and the caller sends it through
    /// `Vst3ParamChanges` in the same process call (ADR-0073).
    bool addOut(const engine::MpeOut& o, std::int32_t blockOffset = 0,
                std::int32_t segmentFrames = 0) noexcept;

    /// Events refused for landing outside their segment. Separate from
    /// `dropped()`: a capacity drop means a busy block, an out-of-range means
    /// the two sides disagree about coordinates.
    [[nodiscard]] std::int64_t outOfRange() const noexcept { return outOfRange_; }

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
    std::int64_t outOfRange_ = 0;
};

/// One parameter's points within a block. ADR-0073.
///
/// A queue per parameter that actually changed, which is what VST3 means by
/// "changes": a plugin is handed only the parameters somebody touched, each
/// with its points in sample order.
///
/// SAMPLE-ACCURATE BY CONSTRUCTION, and that is the point rather than a
/// bonus. ADR-0042 splits a block at every event boundary so automation does
/// not step at 85 ms; a parameter path that could only place a value at offset
/// 0 would undo that for every plugin parameter, which is most of the
/// automation in a real project.
class Vst3ParamQueue final : public SV::IParamValueQueue {
public:
    /// Points this queue holds, allocated here and never in `addPoint`.
    ///
    /// Until ADR-0097 there was no limit and `addPoint` inserted into a vector
    /// that grew on the audio thread (ADR-0010). One point per block hid it;
    /// MPE over IMidiMapping puts a 500 Hz stream into one queue per member
    /// channel, and the first busy block would have allocated.
    void reserve(std::int32_t points) {
        points_.reserve(static_cast<std::size_t>(points > 0 ? points : 0));
        cap_ = points > 0 ? points : 0;
    }
    void reset(SV::ParamID id) noexcept { id_ = id; points_.clear(); }
    /// Points refused because the queue was full. Counted, like every limit.
    [[nodiscard]] std::int64_t dropped() const noexcept { return dropped_; }
    [[nodiscard]] SV::ParamID id() const noexcept { return id_; }

    SV::ParamID PLUGIN_API getParameterId() override { return id_; }
    Steinberg::int32 PLUGIN_API getPointCount() override {
        return static_cast<Steinberg::int32>(points_.size());
    }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index,
                                           Steinberg::int32& sampleOffset,
                                           SV::ParamValue& value) override;
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset,
                                           SV::ParamValue value,
                                           Steinberg::int32& index) override;

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1000; }
    Steinberg::uint32 PLUGIN_API release() override { return 1000; }

private:
    SV::ParamID id_ = 0;
    std::vector<std::pair<Steinberg::int32, SV::ParamValue>> points_;
    std::int32_t cap_ = 0;
    std::int64_t dropped_ = 0;
};

/// Every parameter that changed this block. ADR-0073.
///
/// Ours rather than JUCE's, and not by preference: `processAudio` flushes
/// JUCE's `cachedParamValues` into its own `inputParameterChanges` inside the
/// same call that converts the MidiBuffer, so a host that takes the event path
/// takes the parameter path with it. See ADR-0073.
///
/// Queues are POOLED, never allocated per block. `set()` runs from the
/// message thread and `getParameterData` from the audio thread, but only one
/// of them at a time: the pointer the plugin reads is published before
/// `process` and not touched during it.
class Vst3ParamChanges final : public SV::IParameterChanges {
public:
    /// Pool size. A block in which more than `n` distinct parameters changed
    /// drops the rest and counts them, rather than allocating on the audio
    /// thread (ADR-0010). Each queue holds `pointsPerQueue` points, the same.
    void reserve(std::int32_t n, std::int32_t pointsPerQueue = 64);
    void clear() noexcept;

    /// Queue a value for `id` at `sampleOffset` within the block. Returns
    /// false when the pool is exhausted.
    bool set(SV::ParamID id, double normalized, std::int32_t sampleOffset) noexcept;

    /// Parameters refused for want of a queue, plus points refused for want of
    /// room in one.
    [[nodiscard]] std::int64_t dropped() const noexcept;

    Steinberg::int32 PLUGIN_API getParameterCount() override {
        return static_cast<Steinberg::int32>(used_);
    }
    SV::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override;
    SV::IParamValueQueue* PLUGIN_API addParameterData(const SV::ParamID& id,
                                                      Steinberg::int32& index) override;

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1000; }
    Steinberg::uint32 PLUGIN_API release() override { return 1000; }

private:
    std::vector<std::unique_ptr<Vst3ParamQueue>> pool_;
    std::size_t used_ = 0;
    std::int64_t dropped_ = 0;
};

}  // namespace adi::device
