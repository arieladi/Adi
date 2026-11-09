// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adi::notes {
namespace detail { struct SnapshotData; struct SnapshotAccess; }

/// Complete record bytes, including any future tail. There is no second,
/// separately mutable decoded representation of a Record.
struct Record {
    std::vector<std::byte> bytes;
    bool operator==(const Record&) const = default;
};

/// Wide input fields are checked before encoding/narrowing to the ANOT prefix.
/// start may be negative; duration must be positive; id must be nonzero.
struct NoteValues {
    std::int64_t start = 0;
    std::int64_t duration = 0;
    std::uint64_t id = 0;
    int key = 0;
    int velocity = 0;
    int releaseVelocity = 0;
    int channel = 0;
    int probability = 10000;
    std::uint32_t flags = 0;
    double cents = 0;
    bool operator==(const NoteValues&) const = default;
};

struct Change {
    Record oldRecord;
    Record newRecord;
    bool operator==(const Change&) const = default;
};

struct Diff {
    std::vector<Record> added;
    std::vector<Record> removed;
    std::vector<Change> changed;
    bool operator==(const Diff&) const = default;
};

/// Native prefix is 40 bytes. A wider new record requires an explicit tail.
/// withValues retains the supplied record's exact opaque tail. Both helpers
/// validate all known values, reserved bytes and checked start+duration.
[[nodiscard]] std::optional<Record> makeRecord(const NoteValues&,
    std::span<const std::byte> tail, std::string& error);
[[nodiscard]] std::optional<Record> withValues(const Record&,
    const NoteValues&, std::string& error);
[[nodiscard]] std::optional<NoteValues> values(const Record&, std::string& error);

/// Owned immutable capture; copies share only const value storage, never Store,
/// SQLite, a borrowed BLOB, a read transaction or an owner-thread reference.
/// Accessors return views owned by this Snapshot and remain valid while its
/// immutable backing value is retained. Failed/default snapshots expose empty
/// views; callers must inspect ok before treating clip()'s value as meaningful.
class Snapshot {
public:
    bool ok = false;
    std::string error;
    [[nodiscard]] std::int64_t clip() const noexcept;
    [[nodiscard]] bool hasStream() const noexcept;
    /// The existing canonical row ID, absent when no canonical row exists.
    [[nodiscard]] std::optional<std::int64_t> streamId() const noexcept;
    /// The caller-owned ID captured for creation, absent for existing streams
    /// or a readable absent snapshot without a creation row.
    [[nodiscard]] std::optional<std::int64_t> creationId() const noexcept;
    [[nodiscard]] std::span<const Record> records() const noexcept;
    /// Full original-blob BLAKE3, or exactly "absent" for a missing stream.
    [[nodiscard]] const std::string& from() const noexcept;
    [[nodiscard]] const std::string& sourceFile() const noexcept;
private:
    std::shared_ptr<const detail::SnapshotData> data_;
    friend struct detail::SnapshotAccess;
};

/// Owner connection: one coherent deferred read capture (reuse an active
/// caller transaction). Creation defaults are captured once off-project.
/// Existing stream ID is authoritative; a supplied ID must be absent or equal.
/// For an absent stream, no supplied ID still permits a readable snapshot.
[[nodiscard]] Snapshot captureNotes(Store&, std::int64_t clip,
    std::optional<std::int64_t> newStreamId = std::nullopt);

struct PreparedDiff {
    bool ok = false;
    std::string error;
    std::string sourceFile;
    /// Zero requests for a semantic no-op; otherwise one note.applyDiff.
    std::vector<OpRequest> requests;
    CommitOptions options;
    /// Literal validated effects, with byte-identical changes omitted. This is
    /// preparation metadata, not an independent mutation input or row packet.
    /// If requests are subsequently changed, this preview is no longer valid.
    Diff effects;
};

/// Pure owned-value preparation: no Store/SQL/settings/filesystem access,
/// ID allocation, ambient selection reads or project writes. An absent empty
/// diff is a no-op even without a creation ID. Nonempty creation requires the
/// owner's previously captured full creation row.
[[nodiscard]] PreparedDiff prepareDiff(const Snapshot&, const Diff&);

/// Owner connection: check source-file identity, stamp actor, then commit via
/// OpJournal with the captured expected head (including a real null head).
[[nodiscard]] CommitResult commitDiff(Store&, const PreparedDiff&, Actor = Actor::User);

} // namespace adi::notes
