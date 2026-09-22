// SPDX-License-Identifier: GPL-3.0-or-later
//
// The input parser. ADR-0054: **no MIDI byte survives it.**
//
// Bytes go in because bytes are what the wire carries. `engine::Event` comes
// out, and every expression value in it is a `double`. 7-bit and 14-bit are
// encodings of a control surface, not properties of the music, and the moment
// one of them reaches an engine type the mandate is undone silently — the
// Continuum's extra bits simply stop arriving and nothing fails.
//
// This file is the first half of that pipeline. The second half is the VST3
// event path (ADR-0057), which cannot go through JUCE's `MidiBuffer` for the
// same reason this parser exists.
//
// WHAT IT HANDLES
//
//   MPE            member channels carry one note each; per-note pitch bend,
//                  channel pressure and CC74 are that note's X, Y and Z.
//   MPE+           the Continuum's higher resolution: 14-bit CCs, sent as an
//                  MSB on CC n and an LSB on CC n+32, at up to 500 Hz.
//   plain MIDI     a note on channel 1 with no zone configured still works;
//                  it simply has no per-note expression.
//
// WHAT IT DELIBERATELY DOES NOT DO
//
//   Reconstruct MPE member allocation from the channel afterwards. ADR-0054
//   says a reader MUST NOT, and the reason is here rather than downstream:
//   the channel is transport. Identity is `Event::noteId`, minted here, and
//   everything after this file anchors to that.

#pragma once

#include "adi/blob.hpp"
#include "adi/engine/events.hpp"

#include <array>
#include <cstdint>

namespace adi::engine {

/// One MPE zone, as MPE 1.0 defines it.
///
/// A zone is a master channel plus a contiguous block of member channels. The
/// lower zone's master is channel 1 and its members run upward; the upper
/// zone's master is channel 16 and its members run downward.
struct MpeZone {
    bool         active      = false;
    std::uint8_t masterChan  = 1;    ///< 1-based, as the wire numbers them
    std::uint8_t firstMember = 2;    ///< 1-based
    std::uint8_t lastMember  = 16;   ///< 1-based, inclusive

    /// Per-note bend range in SEMITONES. MPE's default is ±48, not ±2, and
    /// getting this wrong is a transposition rather than a subtle error.
    double memberBendSemitones = 48.0;
    /// The master channel's own bend is an ordinary global bend.
    double masterBendSemitones = 2.0;

    [[nodiscard]] bool isMember(std::uint8_t chan1) const noexcept {
        return active && chan1 >= firstMember && chan1 <= lastMember;
    }

    /// MPE 1.0's lower zone: master 1, members 2..(1+n).
    [[nodiscard]] static MpeZone lower(std::uint8_t memberCount = 15) noexcept;
};

/// Bytes in, `Event`s out.
///
/// Not allocating and not throwing: this runs wherever MIDI arrives, which may
/// be a realtime callback. Events are handed to a caller-supplied sink rather
/// than collected, so the parser owns no buffer and imposes no capacity.
class MpeParser {
public:
    /// What a parsed event is delivered to. Returning false means the sink is
    /// full; the parser counts it and carries on rather than blocking.
    using Sink = bool (*)(void* ctx, const Event& e);

    MpeParser() = default;

    void setZone(const MpeZone& z) noexcept { zone_ = z; }
    [[nodiscard]] const MpeZone& zone() const noexcept { return zone_; }

    /// Where in the current block subsequent events land. The caller sets this
    /// from the driver's timestamp before feeding the bytes of that packet.
    void setFrame(std::int32_t frame) noexcept { frame_ = frame; }

    /// Feed one complete MIDI message. Running status is NOT handled here --
    /// a driver hands over complete messages, and a parser that also did
    /// byte-stream reassembly would be two things.
    ///
    /// Returns the number of events emitted.
    int feed(const std::uint8_t* bytes, std::int32_t n, Sink sink, void* ctx) noexcept;

    /// Reset note tracking. Called when the transport stops or a device is
    /// reopened; a note left active would anchor later expression to a note
    /// that is no longer sounding.
    void reset() noexcept;

    // --- what happened, so tests assert rather than trust -------------------

    [[nodiscard]] std::int64_t notesStarted() const noexcept { return notesStarted_; }
    [[nodiscard]] std::int64_t sinkRejections() const noexcept { return rejected_; }

    /// The note currently sounding on a member channel, or 0 for none.
    [[nodiscard]] std::uint64_t activeNoteOn(std::uint8_t chan1) const noexcept;

private:
    struct ChannelState {
        std::uint64_t noteId = 0;
        std::uint8_t  key    = 0;
        /// Last value seen for every controller, so an LSB can find its MSB.
        /// A 14-bit CC whose MSB has not arrived is NOT promoted to 14 bits:
        /// pretending otherwise invents precision the wire did not carry.
        std::array<std::uint8_t, 128> cc{};
        std::array<bool, 128>         ccSeen{};
    };

    bool emit(Sink sink, void* ctx, const Event& e) noexcept;
    ChannelState& chan(std::uint8_t chan1) noexcept {
        return chans_[chan1 >= 1 && chan1 <= 16 ? chan1 - 1 : 0];
    }

    MpeZone zone_{};
    std::array<ChannelState, 16> chans_{};
    std::int32_t  frame_ = 0;
    std::uint64_t nextNoteId_ = 1;   ///< 0 means "unassigned" (events.hpp)
    std::int64_t  notesStarted_ = 0;
    std::int64_t  rejected_ = 0;
};

// --- the conversions, exposed so they can be tested directly ---------------
//
// Each is a pure function and each is somewhere the mandate could be lost.

/// A 14-bit pitch-bend word (0..16383, centre 8192) to semitones.
///
/// The asymmetry is real and is not a rounding bug: centre is 8192, so there
/// are 8192 steps below and 8191 above. Dividing by 8192 in both directions
/// makes full-up fall short of the range; dividing by the correct count in
/// each direction is what makes -range and +range both reachable.
[[nodiscard]] double bendToSemitones(std::uint16_t word14, double rangeSemitones) noexcept;

/// A 14-bit controller value (0..16383) to 0..1.
[[nodiscard]] double cc14ToUnit(std::uint16_t word14) noexcept;

/// A 7-bit value (0..127) to 0..1.
[[nodiscard]] double cc7ToUnit(std::uint8_t v) noexcept;

/// MPE's timbre (Y) axis.
inline constexpr std::uint8_t kTimbreCcMsb = 74;

/// Is this controller the MSB half of a high-resolution pair, whose LSB
/// arrives on `cc + 32`?
///
/// MIDI 1.0 §4.2 defines the pairing for **CC 0..31 only**, with 32..63 as
/// their LSBs. CC74 is outside that range, and treating it as a pair is the
/// **MPE+ extension** — 74 pairs with 106, which is 74+32, so the arithmetic
/// is the same but the authority is Haken's rather than the MIDI spec's.
///
/// NOT VERIFIED AGAINST HARDWARE. I have not had a Continuum in front of me
/// and have not read Haken's document; this is the convention the arithmetic
/// and the usual descriptions imply. It is isolated in one predicate so that
/// confirming or correcting it is a one-line change rather than a hunt, and
/// so that nothing downstream has quietly assumed it. If it is wrong, CC74
/// stays 7-bit and the Y axis loses resolution the mandate asked for — which
/// is exactly the class of silent failure ADR-0054 exists to prevent, so it
/// is flagged here rather than left to be discovered.
[[nodiscard]] constexpr bool isHighResMsb(std::uint8_t cc) noexcept {
    return cc < 32 || cc == kTimbreCcMsb;
}

}  // namespace adi::engine
