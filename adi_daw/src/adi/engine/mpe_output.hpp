// SPDX-License-Identifier: GPL-3.0-or-later
//
// MPE+ out through VST3: which of three routes a plugin gets, and what each
// route sends. ADR-0097. **No JUCE and no VST3 headers**, so every rule here
// is tested on all seven ABIs; `vst3_events.hpp` asserts the constants against
// the real SDK and fills in Steinberg's structs.
//
// WHY THERE ARE THREE ROUTES
//
// The engine carries per-note expression natively: a note id and a double per
// dimension (ADR-0054). A VST3 plugin can take it in one of three ways, and
// sending the wrong one is silent -- a well-formed event the plugin ignores,
// or worse, one it applies to every note:
//
//   NoteExpression  VST3's own model. Notes go out on channel 0 and expression
//                   as `kNoteExpressionValueEvent`, a double anchored to the
//                   note id. Nothing is quantised. A plugin that does not know
//                   the event type ignores it and still plays the notes.
//
//   MpeMidi         MPE 1.0 over MIDI. Each note gets a MEMBER CHANNEL of its
//                   own, and its expression becomes that channel's pitch bend,
//                   channel pressure and CC74. This is what the JUCE-built
//                   synths understand -- JUCE's VST3 client drops note-
//                   expression events, so without this route an MPE synth
//                   built on JUCE plays every note flat and still.
//
//   Plain           Standard MIDI. Notes on channel 0, pressure as poly
//                   aftertouch -- the one per-note expression MIDI 1.0 has --
//                   and the other dimensions dropped, COUNTED.
//
// BACKWARD COMPATIBILITY IS THE CHANNEL. `Event::channel` is transport, not
// identity (ADR-0054): an MPE controller sends each note on channel 2..16, and
// passing that through tells a plugin that listens on channel 1 -- a
// multitimbral sampler, anything channel-filtered -- that the notes are not
// for it. Only MpeMidi puts a note on a member channel, and it chooses the
// channel itself; the controller's channel never reaches a plugin.

#pragma once

#include "adi/blob.hpp"
#include "adi/engine/events.hpp"
#include "adi/engine/note_expression.hpp"

#include <string_view>
#include <array>
#include <cstdint>

namespace adi::engine {

enum class ExpressionRoute : std::uint8_t {
    NoteExpression = 0,
    MpeMidi        = 1,
    Plain          = 2,
};

/// What the user asked for. `Auto` resolves against what the plugin declared.
enum class RouteChoice : std::uint8_t {
    Auto           = 0,
    NoteExpression = 1,
    MpeMidi        = 2,
    Plain          = 3,
};

/// The names `device_expression_routes.route` stores (SPEC 7.5, ADR-0146)
/// and the plugin registry uses. `Auto` has none: it is the absence of a row.
[[nodiscard]] constexpr const char* routeChoiceName(RouteChoice c) noexcept {
    switch (c) {
        case RouteChoice::NoteExpression: return "note_expression";
        case RouteChoice::MpeMidi:        return "mpe_midi";
        case RouteChoice::Plain:          return "plain";
        case RouteChoice::Auto:           break;
    }
    return "";
}
/// The inverse. An unknown name is `Auto` with `known` false, so a newer
/// file's route this build does not know plays as the plugin declares.
[[nodiscard]] inline RouteChoice routeChoiceFromName(std::string_view name,
                                                    bool* known = nullptr) noexcept {
    RouteChoice c = RouteChoice::Auto;
    bool ok = true;
    if (name == "note_expression") c = RouteChoice::NoteExpression;
    else if (name == "mpe_midi")   c = RouteChoice::MpeMidi;
    else if (name == "plain")      c = RouteChoice::Plain;
    else ok = name.empty();
    if (known != nullptr) *known = ok;
    return c;
}

/// VST3's controller numbers for the two MIDI messages that are not CCs, from
/// `ivstmidicontrollers.h`. Asserted against the SDK in `vst3_events.hpp`.
inline constexpr std::uint16_t kCtrlAfterTouch = 128;
inline constexpr std::uint16_t kCtrlPitchBend  = 129;

/// MPE's timbre (Y) controller. The same number as the input parser's
/// `kTimbreCcMsb`; repeated so this header does not pull the parser in.
inline constexpr std::uint16_t kCtrlTimbre = 74;

/// "No parameter", VST3's `kNoParamId`. Asserted against the SDK.
inline constexpr std::uint32_t kNoParam = 0xFFFFFFFFu;

/// The member-channel bend range the MpeMidi route encodes against.
///
/// 48 semitones is what MPE 1.0 says a receiver assumes after a Configuration
/// Message, and this route SENDS one, so the assumption is made true rather
/// than hoped for. Encoding against any other range would need an RPN 0 on
/// every member channel and would still be wrong for a receiver that ignores
/// RPN 0 -- a transposition, not a subtle error.
inline constexpr double kMpeOutBendSemitones = 48.0;

/// MPE's neutral timbre, CC74 = 64. What a member channel is reset to before a
/// note that brings no timbre of its own, and what JUCE's `MPEInstrument`
/// assumes for a new note.
inline constexpr double kMpeNeutralTimbre = 64.0 / 127.0;

/// What a plugin declared it can take, read once on the message thread.
///
/// All of it comes from the plugin's EDIT CONTROLLER, and JUCE does not give a
/// host the controller: only `IComponent` is public. So it is reachable only
/// when the component answers for the controller itself (a single-component
/// plugin); for the rest -- which includes every JUCE-built plugin --
/// `controllerReachable` is false and `Auto` has nothing to go on. ADR-0097.
struct ExpressionCaps {
    bool controllerReachable = false;

    /// The plugin takes per-note expression: its `INoteExpressionController`
    /// lists Tuning, or its physical-UI mapping names a type for X.
    bool noteExpression = false;

    /// Which note-expression type each MPE axis becomes. The defaults are the
    /// host convention (note_expression.hpp); a plugin that answers
    /// `INoteExpressionPhysicalUIMapping` overrides them -- that interface is
    /// VST3's own bridge for MPE controllers: X, Y and pressure, by name.
    std::uint32_t pitchType    = static_cast<std::uint32_t>(Vst3NoteExprType::Tuning);
    std::uint32_t timbreType   = static_cast<std::uint32_t>(Vst3NoteExprType::Brightness);
    std::uint32_t pressureType = static_cast<std::uint32_t>(Vst3NoteExprType::Expression);

    /// `IMidiMapping`, per channel: the parameter each MPE message lands on.
    std::array<std::uint32_t, 16> bendParam{};
    std::array<std::uint32_t, 16> pressureParam{};
    std::array<std::uint32_t, 16> timbreParam{};

    /// CC101, CC100 and CC6 on channel 0: where the MPE Configuration Message
    /// would land.
    std::array<std::uint32_t, 3> rpnParam{};

    ExpressionCaps() noexcept {
        bendParam.fill(kNoParam);
        pressureParam.fill(kNoParam);
        timbreParam.fill(kNoParam);
        rpnParam.fill(kNoParam);
    }

    /// Channels whose pitch bend lands on a parameter of its OWN. A plugin
    /// that maps bend on one channel, or maps every channel onto one
    /// parameter, has a global bend and is not an MPE receiver.
    [[nodiscard]] int perChannelBend() const noexcept;
};

/// Which route a plugin gets.
///
/// An explicit choice always wins. `Auto`:
///   * controller unreachable  -> NoteExpression. VST3's native model, and
///     the one that costs a plugin that does not support it nothing: it
///     ignores the events and plays the notes on channel 0.
///   * note expression declared -> NoteExpression.
///   * bend mapped per channel on two or more channels -> MpeMidi.
///   * otherwise -> Plain.
[[nodiscard]] ExpressionRoute resolveRoute(RouteChoice choice,
                                           const ExpressionCaps& caps) noexcept;

/// Semitones to a 14-bit pitch-bend word, UNROUNDED.
///
/// The exact inverse of `bendToSemitones`: 8192 steps below centre and 8191
/// above, so -range reaches 0 and +range reaches 16383. Dividing by 8192 both
/// ways would leave the top word unreachable and push every upward bend flat.
/// Clamped to 0..16383.
[[nodiscard]] double semitonesToBendExact(double semitones, double range) noexcept;

/// The same, rounded to the word that goes on a MIDI wire.
[[nodiscard]] std::uint16_t semitonesToBendWord(double semitones, double range) noexcept;

/// One thing to send, SDK-free. `vst3_events` turns it into an `SV::Event`,
/// or -- for a `Control` with a mapped parameter -- a parameter change.
struct MpeOut {
    enum class Kind : std::uint8_t {
        NoteOn,
        NoteOff,
        /// `kNoteExpressionValueEvent`: `exprType`, `noteId`, `value`.
        Expression,
        /// `kPolyPressureEvent`: `channel`, `key`, `noteId`, `value`.
        PolyPressure,
        /// A channel message: `ctrl` is 0..127 for a CC, or kCtrlAfterTouch /
        /// kCtrlPitchBend. With `paramId` it goes out as a parameter change
        /// carrying `value`; without, as a `kLegacyMIDICCOutEvent` carrying
        /// `word` -- the one input JUCE's VST3 client turns back into MIDI.
        Control,
    };

    Kind kind = Kind::NoteOn;
    std::uint8_t channel = 0;       ///< 0-based, as VST3 numbers them
    /// The note's key. For an `Expression`, the key of the note it addresses
    /// when the router is tracking that note, else -1 -- CLAP's address is
    /// (port, channel, key, note_id) and a plugin that matches on key alone
    /// must still find ONE note (ADR-0099).
    std::int16_t key = 0;
    std::uint16_t ctrl = 0;
    /// The wire value for a legacy event: 7 bits, or 14 for pitch bend.
    std::uint16_t word = 0;
    /// BLOCK-relative, exactly as the event that caused it (ADR-0081).
    std::int32_t frame = 0;
    std::uint32_t exprType = 0;
    std::uint32_t paramId = kNoParam;
    std::uint64_t noteId = 0;
    /// Velocity 0..1, an expression's normalised value, poly pressure, or a
    /// Control's parameter value. A double: the route decides what is
    /// quantised, and only the MIDI wire is.
    double value = 0.0;
    /// For an `Expression`: the dimension, and the engine's own value --
    /// semitones for pitch, 0..1 otherwise. `value` is VST3's normalised form;
    /// CLAP takes semitones directly, and converting back from VST3's would
    /// round-trip a double through a scale it never needed (ADR-0099).
    std::uint16_t dim = 0;
    double plain = 0.0;
};

/// A fixed-capacity list over storage someone else owns. Overflow is COUNTED,
/// for EventList's reason.
class MpeOutList {
public:
    MpeOutList() = default;
    MpeOutList(MpeOut* storage, std::int32_t capacity) noexcept
        : data_(storage), capacity_(capacity) {}

    [[nodiscard]] std::int32_t size() const noexcept { return size_; }
    [[nodiscard]] std::int64_t dropped() const noexcept { return dropped_; }
    [[nodiscard]] const MpeOut* begin() const noexcept { return data_; }
    [[nodiscard]] const MpeOut* end() const noexcept { return data_ + size_; }
    [[nodiscard]] MpeOut at(std::int32_t i) const noexcept {
        if (data_ == nullptr || i < 0 || i >= size_) return MpeOut{};
        return data_[i];
    }

    bool push(const MpeOut& o) noexcept {
        if (data_ == nullptr || size_ >= capacity_) { ++dropped_; return false; }
        data_[size_++] = o;
        return true;
    }
    void clear() noexcept { size_ = 0; }

private:
    MpeOut* data_ = nullptr;
    std::int32_t capacity_ = 0;
    std::int32_t size_ = 0;
    std::int64_t dropped_ = 0;
};

/// Translate one event the NoteExpression way, with no note tracking: channel
/// 0, the note id kept, expression as its type from `caps`. The route's own
/// rule, exposed for `Vst3EventList::add`, which translates single events.
/// False when the event has no VST3 event form (a parameter event).
bool noteExpressionOut(const Event& in, const ExpressionCaps& caps, MpeOut& out) noexcept;

/// Engine events in, what the plugin should receive out.
///
/// STATEFUL, because MpeMidi is: a member channel is allocated at note-on and
/// released at note-off, and every expression value in between goes to the
/// channel its note was given. Allocation-free and non-throwing; it runs on
/// the audio thread.
class MpeRouter {
public:
    /// Notes tracked at once. A note-on beyond it is refused and counted:
    /// sending it untracked would leave its note-off nowhere to go -- a stuck
    /// note, which is worse than a missing one.
    static constexpr int kMaxNotes = 128;

    /// Before prepare, on the message thread. Forgets every note.
    void configure(ExpressionRoute route, const ExpressionCaps& caps,
                   int memberChannels = 15) noexcept;

    /// At prepare: forget every note, and send MpeMidi's Configuration
    /// Message again -- a re-activated plugin has forgotten it.
    void reset() noexcept;

    /// Change route while running. Every sounding note is ended FIRST, on the
    /// channel it was started on: a note begun on member channel 5 and ended
    /// on channel 0 under the new route is a note that never ends.
    void switchTo(ExpressionRoute route, std::int32_t frame, MpeOutList& out) noexcept;

    /// Translate `n` events, sorted by frame, of the segment starting at
    /// `segmentStart` (block-relative). Appends to `out`.
    void route(const Event* events, std::int32_t n, std::int32_t segmentStart,
               MpeOutList& out) noexcept;

    [[nodiscard]] ExpressionRoute current() const noexcept { return route_; }
    [[nodiscard]] int liveNotes() const noexcept;
    /// The member channel a sounding note was given, or -1.
    [[nodiscard]] int channelOf(std::uint64_t noteId) const noexcept;

    // --- what happened, so tests assert rather than trust -------------------

    /// Expression values with nowhere to go on this route, by dimension.
    [[nodiscard]] std::int64_t dropped(ExpressionDim d) const noexcept;
    /// MpeMidi note-ons that had to SHARE a member channel because all of
    /// them were sounding. MPE 1.0's degradation: the two notes share that
    /// channel's expression. Nothing is cut off to make room.
    [[nodiscard]] std::int64_t sharedChannels() const noexcept { return shared_; }
    /// Plain/NoteExpression note-ons whose key was already sounding on
    /// channel 0 -- two MPE fingers on one key, collapsed onto one channel.
    [[nodiscard]] std::int64_t keyCollisions() const noexcept { return collisions_; }
    /// Note-offs and expression for a note id that is not sounding.
    [[nodiscard]] std::int64_t unknownNotes() const noexcept { return unknown_; }
    /// Note-ons refused because `kMaxNotes` were already sounding.
    [[nodiscard]] std::int64_t tableFull() const noexcept { return tableFull_; }

private:
    struct Note {
        /// A flag, not `id == 0`: an unassigned id is legal for a note that
        /// carries no expression (events.hpp), so 0 cannot mean "free".
        bool used = false;
        std::uint64_t id = 0;
        std::int16_t key = 0;
        std::uint8_t channel = 0;
    };

    void noteOn(const Event* events, std::int32_t n, std::int32_t i, MpeOutList& out) noexcept;
    void noteOff(const Event& e, MpeOutList& out) noexcept;
    void expression(const Event& e, MpeOutList& out) noexcept;
    void sendMcm(std::int32_t frame, MpeOutList& out) noexcept;
    void control(std::uint8_t chan, std::uint16_t ctrl, double unit, std::int32_t frame,
                 std::uint64_t noteId, MpeOutList& out) noexcept;
    int allocate() noexcept;
    Note* find(std::uint64_t id) noexcept;
    [[nodiscard]] const Note* find(std::uint64_t id) const noexcept;

    ExpressionRoute route_ = ExpressionRoute::NoteExpression;
    ExpressionCaps caps_{};
    int members_ = 15;
    bool mcmPending_ = false;

    std::array<Note, kMaxNotes> notes_{};
    std::array<int, 16> active_{};             ///< notes sounding per channel
    std::array<std::uint64_t, 16> lastOn_{};   ///< clock of the last note-on
    std::array<std::uint64_t, 16> lastOff_{};  ///< clock of the last release
    std::uint64_t clock_ = 0;

    std::array<std::int64_t, 5> dropped_{};
    std::int64_t droppedOther_ = 0;
    std::int64_t shared_ = 0;
    std::int64_t collisions_ = 0;
    std::int64_t unknown_ = 0;
    std::int64_t tableFull_ = 0;
};

}  // namespace adi::engine
