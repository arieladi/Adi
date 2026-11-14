// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "volume.hpp"
#include "sample_settings.hpp"
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
struct ScanResult {
    bool ok = false;
    std::uint64_t files = 0, unchanged = 0, queued = 0;
    std::string error;
    bool cancelled = false;
};
struct ScanProgress {
    enum class Phase { Discovering, Publishing };
    Phase phase = Phase::Discovering;
    std::uint64_t visited = 0, files = 0;
};
using ScanObserver = std::function<void(const ScanProgress &)>;
struct WorkerStats { std::uint64_t hashed = 0, failed = 0; bool lowPriority = false; };
// Off-callback, application-scoped persistent library, one indexing owner per database.
// File-index rows are rebuildable; authored annotations/sample defaults are user
// data. Never delete this database as part of decoding/analysis-cache cleanup.
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
    // Cooperative stop between filesystem/SQL operations, including waiting
    // for the database mutex. Cancellation preserves the last complete index
    // and queues no hashes. A stop after commit is a completed scan.
    // Observer runs synchronously during discovery and before publication,
    // always outside the database mutex. It may inspect entries/request stop;
    // do not recursively scan or mutate the index from it. Return reports the
    // terminal state; no fallible observer is called after a durable commit.
    ScanResult scan(const Volume &, const std::filesystem::path &relativeFolder = {},
                    std::stop_token stop = {}, ScanObserver observer = {});
    std::vector<Entry> entries() const;
    void start();
    void cancel(); // Cooperative stop + join. Pending rows remain restartable.
    void waitIdle(); // Call only with worker started; paused work is not complete.
    WorkerStats stats() const;
    // Authored per-source defaults live in this persistent data database,
    // independent of file presence, scans, decoding and analysis-cache cleanup.
    // Off-callback; returned values own all data. No .adi operation is emitted.
    [[nodiscard]] SampleSettingsResult sampleSettings(const std::string& hash) const;
    [[nodiscard]] SampleSettingsProblem saveSampleSettings(const std::string& hash,const SampleSettings&,std::string& error);
    [[nodiscard]] SampleSettingsProblem clearSampleSettings(const std::string& hash,std::string& error);
    std::string exportJson() const;
    bool importJson(const std::string&, std::string& error);
private:
    struct Impl;
    explicit Index(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
