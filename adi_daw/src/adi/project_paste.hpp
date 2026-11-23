// SPDX-License-Identifier: AGPL-3.0-or-later
//
// PD-15: cross-project track paste — the engine half (ADR-0068, ADR-0111).
//
// Copies one track with its mixer strip, top-level device chains and devices,
// clips (audio and MIDI), automation lanes/data, and referenced media from a
// second Store (typically opened read-only) into the active writable Store as
// one undoable OpJournal transaction.
//
// Rules (ADR-0068 d3-d5, PLAN PD-15):
//   * Every destination row id is allocated through RowIds on `dest`.
//   * Devices are captured through readDeviceCopy() (device_copy.hpp).
//   * Media travels by BLAKE3 hash through media::publishFile(); a hash
//     already present in `dest` is reused and its file is not copied again.
//   * State blobs travel by BLAKE3 hash through OpTransactionInputs; an
//     existing hash in `dest` is reused.
//   * The source Store is never written; a read-only destination is refused.

#pragma once

#include "adi/ops.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
    std::int64_t trackId = 0;

    std::map<std::int64_t, std::int64_t> chainIds;   ///< source chain id -> dest chain id
    std::map<std::int64_t, std::int64_t> deviceIds;  ///< source device id -> dest device id
    std::map<std::int64_t, std::int64_t> clipIds;    ///< source clip id -> dest clip id
    std::map<std::int64_t, std::int64_t> mediaIds;   ///< source media id -> dest media id
    std::map<std::int64_t, std::int64_t> laneIds;    ///< source lane id -> dest lane id

    std::size_t copiedMediaCount = 0;
    std::size_t reusedMediaCount = 0;
    std::size_t copiedStateBlobCount = 0;
    std::size_t reusedStateBlobCount = 0;
    std::vector<std::filesystem::path> publishedMediaPaths;
};

/// Paste `sourceTrackId` from `source` into `dest` as one undoable transaction.
[[nodiscard]] ProjectPasteResult pasteTrackFromProject(
    Store& source,
    std::int64_t sourceTrackId,
    Store& dest,
    Actor actor = Actor::User,
    std::string_view actorDetail = {});

}  // namespace adi
