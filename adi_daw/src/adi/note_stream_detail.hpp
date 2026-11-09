// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/note_diff.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adi::notes::detail {

inline constexpr std::size_t notePrefixBytes = 40;
inline constexpr std::size_t streamHeaderBytes = 16;

/// Lossless ANOT envelope. Header bytes and complete Record bytes are their
/// sole authoritative representations. Header count is patched during encode
/// from records.size(); version/stride/flags and trailer stay byte-exact.
/// This type is internal: externally captured snapshots hold it const.
struct Stream {
    std::array<std::byte, streamHeaderBytes> header{};
    std::vector<Record> records;
    std::vector<std::byte> trailer;
};

/// These are decoded views, never additional writable fields in Stream.
[[nodiscard]] std::uint16_t recordStride(const Stream&) noexcept;
[[nodiscard]] std::uint16_t streamVersion(const Stream&) noexcept;
[[nodiscard]] std::uint32_t streamFlags(const Stream&) noexcept;

/// Strict MI-01/P0 policy: musical ANOT, version>=1, supported stride,
/// header flags limited to SortedByTime and truthful ordering, unique nonzero
/// uint64 IDs, complete known-field validation, checked endpoints/size products.
/// Existing note_transforms admission is intentionally not changed/shared here.
[[nodiscard]] std::optional<Stream> decodeNotes(std::span<const std::byte>, std::string& error);
/// Revalidates the header policy and complete records, but deliberately ignores
/// the original header count: additions/removals change records.size(), and the
/// encoder writes that checked count into the otherwise preserved header.
[[nodiscard]] std::optional<std::vector<std::byte>> encodeNotes(const Stream&, std::string& error);
/// Native version1/stride40/SortedByTime/count0; not an absence marker.
[[nodiscard]] Stream emptyNotes();
[[nodiscard]] std::optional<std::string> hashNotes(std::span<const std::byte>, std::string& error);

/// One deterministic O(N log N) or better transform. Validates exact old
/// bytes, matching strides and identities, duplicate IDs across every section,
/// and all input/output extents before encoding. Preserves original order
/// unless SortedByTime is advertised AND an addition/start change requires a
/// stable start-only sort. Non-time changes and deletion alone never sort.
struct StreamPlan {
    bool ok = false;
    std::string error;
    bool changed = false;
    /// The complete final ANOT. No record is synthesized into a wider stride.
    std::vector<std::byte> bytes;
    /// Effective literal diff; byte-identical changes are removed. The order
    /// of unaffected/equal-time records is determined by the stream, not this.
    Diff effects;
};
[[nodiscard]] StreamPlan planRecords(const Stream&, const Diff&);

/// Values supplied solely by owner capture, under its one coherent snapshot.
/// Each row is a complete typed SQLite row; unknown columns and BLOBs survive.
/// Neither this bridge nor its consumers contain a database reference.
struct CapturedNotes {
    Payload clipRow;
    std::optional<Payload> streamRow;
    /// All note_expression rows for this clip, ordered by signed SQL row ID.
    /// Orphan/unrelated identities stay captured but are never repaired/edited.
    std::vector<Payload> expressionRows;
    /// Present only for absent canonical stream plus explicit caller ID.
    /// Complete owner-captured default row with native empty ANOT data.
    std::optional<Payload> creationRow;
    std::string sourceFile;
    /// nullopt means an explicitly captured empty undo head, NOT no guard.
    std::optional<std::int64_t> head;
};

/// Cached immutable projections are established by makeSnapshot once and
/// cannot contradict externally mutable record fields (there are none).
struct SnapshotData {
    CapturedNotes captured;
    Stream baseline;
    std::string from;
    std::int64_t clip = 0;
    std::optional<std::int64_t> streamId;
    std::optional<std::int64_t> creationId;
};

/// Validates MIDI ownership, canonical row/create shape and record bytes,
/// computes the whole-byte hash, and freezes all capture values. For absence,
/// baseline is emptyNotes(), but from remains "absent" and streamRow absent.
/// This function is pure; defaults and row acquisition happened before entry.
[[nodiscard]] Snapshot makeSnapshot(CapturedNotes);

struct SnapshotAccess {
    [[nodiscard]] static const SnapshotData* get(const Snapshot& value) noexcept {
        return value.data_.get();
    }
    /// Internal construction after validation; always moves into const storage.
    [[nodiscard]] static Snapshot seal(SnapshotData);
};

/// Fixed bit-preserving mapping, shared by capture, forward removal and inverse
/// guards. For example UINT64_MAX maps to SQL INTEGER -1, without signed casts.
[[nodiscard]] inline std::int64_t expressionSqlId(std::uint64_t id) noexcept {
    return std::bit_cast<std::int64_t>(id);
}
[[nodiscard]] inline std::uint64_t expressionNoteId(std::int64_t id) noexcept {
    return std::bit_cast<std::uint64_t>(id);
}

/// Complete row-set lookup, including an empty result, from immutable capture.
/// Each removed note's ID comes from its authoritative Record bytes. The
/// forward RemovedPacket always carries the entire matching set; checking
/// only rows that happen to exist cannot guard an expected empty set.
[[nodiscard]] std::vector<Payload> expressionSet(const SnapshotData&, std::uint64_t noteId);

/// Pure packet builder implemented by the Store/op worker, reused by public
/// prepareDiff. Produces the closed forward grammar from the validated plan
/// plus captured rows; performs NO Store/SQL/default/ID/file access. Requires
/// plan.ok && plan.changed; false sets error and leaves output unused.
/// Existing stream: from hash + actual stream_id, no create field. Absence:
/// from="absent", literal captured creation-row ID, and full create object.
/// Removed records include expressionSet(...), including empty arrays.
[[nodiscard]] bool makeForwardRequest(const SnapshotData&, const StreamPlan&,
    OpRequest& output, std::string& error);

/// Strict nested wire helpers, reusable by integration/wrappers. Integer-array
/// bytes are accepted only when every element is integral and in [0,255].
/// The record packet is closed: exactly {"bytes": Bytes}; prefix validation
/// happens here, and the planner additionally checks target stride.
[[nodiscard]] Payload recordPacket(const Record&);
[[nodiscard]] std::optional<Record> parseRecordPacket(const Payload&, std::string& error);
/// Exact integer parsing; fractional values and unsigned values above INT64_MAX
/// refuse before narrowing. Zero/negative SQL keys remain valid signed values.
[[nodiscard]] std::optional<std::int64_t> signedValue(const Payload&, std::string& error);
/// Exact nonzero uint64 parsing; signed-negative and zero values refuse.
[[nodiscard]] std::optional<std::uint64_t> noteIdValue(const Payload&, std::string& error);

} // namespace adi::notes::detail
