// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <cstddef>
#include <string_view>
namespace adi::detail {
inline constexpr std::size_t kRackUndoLimitBytes = 4 * 1024 * 1024;
inline constexpr std::string_view kRackUndoLimitMessage = "lossy_undo would exceed the 4 MiB exact rack undo limit";
inline bool isRackUndoPacket(const Payload& inverse) {
    // New rack inverses use the typed image with only its legacy identity fields.
    return inverse.is_object() && inverse.size() == 3 && inverse.contains("contents") &&
           inverse.contains("id") && inverse.contains("chain");
}
// Owner-thread causal row image for track.delete's paired track.create inverse.
// The journal owns the transaction. Audio and shared plug-in blobs stay pooled.
Payload captureTrack(SQLite::Database&,std::int64_t);
// Read-only causal source rows for render fingerprints. Includes restrictive
// FK dependants, with no delete/restore safety relaxation. Freeze metadata and
// purely visual/controller layout rows are excluded to avoid self-reference.
Payload captureTrackSource(SQLite::Database &, std::int64_t);
// Same causal image authority for devices, including nested chains, polymorphic
// key connections and their owned taps. No parallel row-copy implementation.
Payload captureDevice(SQLite::Database&, std::int64_t, std::size_t inverseLimit = 0);
void deleteDevice(SQLite::Database&, std::int64_t, const Payload* expected = nullptr);
void restoreDevice(SQLite::Database&,std::int64_t,const Payload&);
/// Causal image rooted at a device chain, including all nested devices.
Payload captureChain(SQLite::Database&, std::int64_t, std::size_t inverseLimit = 0);
void deleteChain(SQLite::Database&, std::int64_t, const Payload* expected = nullptr);
void restoreChain(SQLite::Database&, std::int64_t, const Payload&);
void deleteTrack(SQLite::Database&,std::int64_t);
void restoreTrack(SQLite::Database&,std::int64_t,const Payload&);
/// Routing rows and their polymorphic owners, in the same order as ids.
std::vector<Payload> captureRouting(SQLite::Database&, std::span<const std::int64_t> ids);
void deleteRouting(SQLite::Database&, std::span<const std::int64_t> ids);
void restoreRouting(SQLite::Database&, const Payload& route, const Payload& packet);
}
