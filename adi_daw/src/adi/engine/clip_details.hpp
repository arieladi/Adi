// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/blob.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adi { class Store; }
namespace adi::engine {
namespace detail {
/// Intrinsic validity for displaying a native ANOT record. Editing may also
/// require a nonzero, unambiguous identity.
[[nodiscard]] bool displayableNote(const NoteRecord&) noexcept;
}

struct ClipNote {
    std::uint64_t id = 0; // exact ANOT note_id, not a display index
    std::int64_t startTicks = 0, durationTicks = 0;
    std::uint8_t key = 0, velocity = 0, releaseVelocity = 0, channel = 0;
    std::uint16_t flags = 0, probability = 10000;
    float tuningCents = 0.0f;
    bool active = true;
};
struct ClipSignature {
    std::int64_t numerator = 4, denominator = 4;
    bool local = false; // otherwise inherited at the arrangement start
};
struct ClipScale {
    std::int64_t root = 0, mask = 2741;
    std::string id = "major";
    std::optional<std::int64_t> tuningId;
    std::vector<std::int64_t> degrees;
    bool local = false;
};
struct NoteExpressionCurve {
    std::int64_t clipId=0;
    std::uint64_t noteId=0;
    std::int64_t dimension=0;
    std::uint16_t recordSize=0;
    std::vector<ExpressionPoint> points;
};
struct NoteExpressionsRead {
    std::vector<NoteExpressionCurve> curves;
    std::vector<std::string> problems;
};

/// Owner-thread projection of the stored AEXP streams, optionally for one clip.
/// Invalid streams report their row and leave the other curves available.
[[nodiscard]] NoteExpressionsRead readNoteExpressions(const Store&,std::optional<std::int64_t> clipId={});

/// Owner-thread value snapshot for Clip View; no store/stream handles escape.
struct ClipDetails {
    std::int64_t id = 0, trackId = 0, timeBase = 0;
    std::optional<std::int64_t> laneId, color, posTicks, posNs, lengthTicks, lengthNs, aliasOf;
    std::string kind, name;
    bool muted = false, loopEnabled = false;
    double gainDb = 0.0;
    std::optional<double> transposeSemis;
    bool pitchEnabled = false;
    std::int64_t contentOffsetTicks = 0, loopStartTicks = 0;
    std::optional<std::int64_t> loopLengthTicks;
    std::int64_t fadeInTicks = 0, fadeOutTicks = 0, fadeInCurve = 1, fadeOutCurve = 1;
    std::int64_t startMarkerTicks = 0; // stored content_offset_ticks
    std::optional<std::int64_t> endMarkerTicks;
    bool endMarkerLocal = false;
    ClipSignature signature;
    ClipScale scale;
    std::vector<ClipNote> notes;
    std::string notesProblem, propertiesProblem;
    std::uint16_t notesRecordSize = 0;
    std::vector<std::byte> encodedNotes;
    NoteExpressionsRead noteExpressions;
};
struct ClipDetailsRead {
    std::optional<ClipDetails> clip;
    std::string error; // missing clip/SQL error: no partial result
};

/// Call on the store's owner/message thread after the journal moves, outside
/// its mutation transaction. Includes muted notes and their original identity.
/// The expression editor can request this clip's AEXP streams with its notes.
[[nodiscard]] ClipDetailsRead readClipDetails(const Store&,std::int64_t clipId,
                                              bool includeNotes=true,bool includeExpression=false);
} // namespace adi::engine
