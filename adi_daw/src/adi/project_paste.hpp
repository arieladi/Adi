// SPDX-License-Identifier: AGPL-3.0-or-later
//
// PD-15: cross-project track paste — the engine half (ADR-0068, ADR-0111).
//
// Copies a track — or a group track with its whole descendant subtree — with
// its mixer strip, split-pan settings, recording options, group summing, return
// send tap, take lanes, top-level device chains and devices, clips (audio and
// MIDI, preserving take-lane assignments and internal alias links), sends,
// internal sidechain routes (with routing_key_taps), internal user Main routes,
// automation lanes/data, and referenced media from a second Store (typically
// opened read-only) into the active writable Store as one undoable OpJournal
// transaction.
//
// Rules (ADR-0068 d3-d5, PLAN PD-15):
//   * Every destination row id is allocated through RowIds on `dest`.
//   * Devices are captured through readDeviceCopy() (device_copy.hpp).
//   * Referenced media stays in place (ADR-0068 d5): files are NOT copied to
//     `dest`'s folder; `dest` records the resolved absolute path hint and
//     relinks by BLAKE3 content hash if the file moves later. Existing media in
//     `dest` with the same non-empty valid BLAKE3 hash is reused. Empty or
//     invalid hashes are never deduplicated and are refused.
//   * State blobs travel by BLAKE3 hash through OpTransactionInputs; an
//     existing hash in `dest` is reused.
//   * Any source attribute that cannot be mapped across projects (e.g. a send
//     targeting a return absent in `dest`, an external user Main or sidechain
//     route, a VCA route or mixer_strip.vca_group_id, an external alias link,
//     or a non-default mixer_strip.pan_law with no op in ops_catalog) is
//     reported in `warnings` rather than dropped silently.
//   * The source Store is never written; a read-only destination is refused.

#pragma once

#include "adi/ops.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace adi {

class Store;

struct ProjectPasteResult {
    bool ok = false;
    std::string error;
    std::int64_t txnId = 0;
    std::int64_t trackId = 0;  ///< Destination id of the root pasted track

    std::map<std::int64_t, std::int64_t> trackIds;     ///< source track id -> dest track id
    std::map<std::int64_t, std::int64_t> takeLaneIds;  ///< source take lane id -> dest take lane id
    std::map<std::int64_t, std::int64_t> chainIds;     ///< source chain id -> dest chain id
    std::map<std::int64_t, std::int64_t> deviceIds;    ///< source device id -> dest device id
    std::map<std::int64_t, std::int64_t> clipIds;      ///< source clip id -> dest clip id
    std::map<std::int64_t, std::int64_t> mediaIds;     ///< source media id -> dest media id
    std::map<std::int64_t, std::int64_t> laneIds;      ///< source automation lane id -> dest id
    std::map<std::int64_t, std::int64_t> routingIds;   ///< source send routing id -> dest id

    std::size_t referencedMediaCount = 0;  ///< New media_files rows referencing source files in place
    std::size_t reusedMediaCount = 0;      ///< Media rows deduplicated by non-empty BLAKE3 hash
    std::size_t copiedStateBlobCount = 0;
    std::size_t reusedStateBlobCount = 0;
    std::vector<std::string> warnings;     ///< Explicit diagnostics for unmappable cross-project items
};

/// Paste `sourceTrackId` (and all descendant tracks if it is a group) from
/// `source` into `dest` as one undoable transaction.
[[nodiscard]] ProjectPasteResult pasteTrackFromProject(
    Store& source,
    std::int64_t sourceTrackId,
    Store& dest,
    Actor actor = Actor::User,
    std::string_view actorDetail = {});

}  // namespace adi
