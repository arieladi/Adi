// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "volume.hpp"
#include "adi/media/blake3.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>
namespace adi::library {
struct Entry {
    std::string volumeId, relativePath, hash, error;
    std::uintmax_t size = 0;
    std::int64_t modifiedNs = 0;
    bool present = true;
};
struct ScanResult { bool ok = false; std::uint64_t files = 0, unchanged = 0, queued = 0; std::string error; };
struct WorkerStats { std::uint64_t hashed = 0, failed = 0; bool lowPriority = false; };
// Off-callback, application-scoped cache, one indexing owner per database.
// Public operations are serialized; hashing runs outside the database lock.
// JSON metadata is keyed only by BLAKE3, never location. No .adi is accepted.
class Index {
public:
    static constexpr int applicationId = 0x4144494c; // ADIL, distinct from ADI project/registry
    static constexpr int version = 1;
    using Hasher = std::function<media::HashResult(const std::filesystem::path&, std::stop_token)>;
    static std::unique_ptr<Index> open(const std::filesystem::path&, std::string& error,
                                       bool autoStart = true, Hasher testHasher = {});
    ~Index();
    Index(const Index&) = delete;
    Index& operator=(const Index&) = delete;
    // Metadata pass returns before hashes; relativeFolder cannot escape root.
    // Incomplete scans fail without marking unseen files missing.
    ScanResult scan(const Volume&, const std::filesystem::path& relativeFolder = {});
    std::vector<Entry> entries() const;
    void start();
    void cancel(); // Cooperative stop + join. Pending rows remain restartable.
    void waitIdle(); // Call only with worker started; paused work is not complete.
    WorkerStats stats() const;
    std::string exportJson() const;
    bool importJson(const std::string&, std::string& error);
private:
    struct Impl;
    explicit Index(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
