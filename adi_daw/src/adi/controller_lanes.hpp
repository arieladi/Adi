// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/controller_data.hpp"
#include "adi/ops.hpp"
#include <filesystem>
namespace adi::controllers {
struct Address {
    std::int64_t id = 0, clipId = 0;
    int channel = 0, controller = 0;
};
struct ReadResult {
    bool ok = false;
    std::vector<Lane> lanes;
    std::string error;
};
// Owner-only reads, one SQLite snapshot; no new IDs or ambient edit settings.
ReadResult readLanes(const Store &, std::optional<std::int64_t> clip = {});
OpRequest writeRequest(Address, std::span<const Point>);
OpRequest deleteRequest(Address, std::span<const std::uint64_t> pointIds);
struct PreparedWrite {
    bool ok = false;
    Address address;
    std::vector<Point> points;
    OpRequest request;
    std::optional<std::int64_t> expectedHead;
    std::filesystem::path sourcePath;
    std::string sourceClient, error;
};
// A zero point ID asks this owner-side producer for a fresh explicit ID.
// Preparation reserves/writes nothing; commit refuses a changed history head
// or another document. Keep the caller's document generation across async work.
PreparedWrite prepareWrite(Store &, std::int64_t clip, int channel, int controller, std::span<const Point>);
CommitResult commitPrepared(Store &, const PreparedWrite &);
std::span<const OpDescriptor> laneOps();
} // namespace adi::controllers
