// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/media/peaks.hpp"
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
namespace adi::media {
// Optional, rebuildable sample analysis only. User Warp/clip settings belong
// in the persistent library and must never enter this cache or its cleanup.
struct AnalysisIdentity {
    std::string hash;
    std::uint32_t sampleRate=0,channels=0;
    std::uint64_t frames=0;
    friend bool operator==(const AnalysisIdentity&,const AnalysisIdentity&)=default;
};
struct AnalysisSection {
    std::uint32_t version=1,configuration=0;
    std::vector<std::byte> bytes;
    friend bool operator==(const AnalysisSection&,const AnalysisSection&)=default;
};
struct SampleAnalysis {
    AnalysisIdentity identity;
    // Four printable ASCII characters, e.g. APKS. Unknown sections survive
    // ordinary updates. At most 32 sections / 32 MiB total, never recursive.
    std::map<std::string,AnalysisSection> sections;
    friend bool operator==(const SampleAnalysis&,const SampleAnalysis&)=default;
};
enum class AnalysisCacheProblem {None,Missing,Realtime,Cancelled,Invalid,Io,Resource};
struct AnalysisCacheResult {
    AnalysisCacheProblem problem=AnalysisCacheProblem::None;
    std::string error;
    std::optional<SampleAnalysis> data;
    explicit operator bool()const noexcept{return problem==AnalysisCacheProblem::None && data.has_value();}
};
struct AnalysisTrimResult {
    AnalysisCacheProblem problem=AnalysisCacheProblem::None;
    // Logical bytes of validated, directly owned cache files. Shared Cleanup
    // includes PCM. Unrecognised entries are neither charged nor removed. Counts remain valid on partial
    // cancellation/failure; bytesBefore is complete only with inventoryComplete.
    // Skipped entries have an I/O refusal, distinct from ignored foreign/invalid
    // entries. Unreadable entries are uncharged and prevent proving budgetMet.
    std::uint64_t bytesBefore=0,bytesAfter=0,filesRemoved=0,ignoredEntries=0;
    std::uint64_t skippedEntries=0;
    std::uint64_t retainedEntries=0; // Cleanup protection (current hashes or active PCM leases).
    bool inventoryComplete=false,budgetMet=false;
    explicit operator bool()const noexcept{return problem==AnalysisCacheProblem::None && budgetMet;}
};
// Bounded explicit little-endian envelope; source identity and body digest are
// checked before section allocation. No native structs, JSON or recursive parse.
[[nodiscard]] std::vector<std::byte> encodeAnalysis(const SampleAnalysis&);
[[nodiscard]] AnalysisCacheResult decodeAnalysis(std::span<const std::byte>);

struct AnalysisCacheLimits {
    std::uint64_t maximumBytes = 0, minimumFreeBytes = 0;
    friend bool operator==(const AnalysisCacheLimits&, const AnalysisCacheLimits&) = default;
};

class AnalysisCache {
public:
    using FreeSpace = std::function<std::optional<std::uint64_t>(const std::filesystem::path&)>;
    explicit AnalysisCache(std::filesystem::path folder, FreeSpace available = {})
        : AnalysisCache(std::move(folder), std::move(available), std::nullopt) {}
    // Serialized off-callback I/O. One <hash>.adia per sample in this folder.
    // Missing/invalid optional data can be rebuilt. Updating one section keeps
    // other supported/unknown sections; inconsistent identity is replaced.
    [[nodiscard]] AnalysisCacheResult read(const AnalysisIdentity&,std::stop_token={}) const;
    // Indexed content already carries a hash but may not have media dimensions.
    [[nodiscard]] AnalysisCacheResult readByHash(const std::string&,std::stop_token={}) const;
    [[nodiscard]] AnalysisCacheProblem update(const AnalysisIdentity&,const std::string& tag,
                                             const AnalysisSection&,std::string& error,std::stop_token={});
    [[nodiscard]] AnalysisCacheProblem remove(const std::string& hash,std::string& error);
    // Owner-worker maintenance, never a callback. Evict validated direct-child
    // <hash>.adia files by oldest last-write/access stamp to a caller's budget.
    // Successful reads refresh that stamp best-effort. No recursive cleanup,
    // source/library deletion, global settings policy or physical-free-space
    // guarantee. This process's reads/updates finish before eviction; returned
    // analysis owns its bytes. External writers need separate coordination.
    // Per-entry I/O refusals are skipped, with the first path/error retained;
    // inventory still finishes before any deletion. None are counted reclaimed.
    [[nodiscard]] AnalysisTrimResult trimToBytes(std::uint64_t maximumBytes,std::string& error,
                                               std::stop_token={});
    // Apply an optional-cache policy on the owner worker's next publication or
    // successful read. Setting the policy does not open or enumerate the cache.
    void setAutomaticLimits(AnalysisCacheLimits);
    [[nodiscard]] const std::filesystem::path& folder()const noexcept{return folder_;}
private:
    friend class audio::DecodeCache;
    friend AnalysisCache& defaultAnalysisCache();
    friend AnalysisTrimResult cleanupMediaCache(AnalysisCache&, audio::DecodeCache&, std::span<const std::string>,
                                                std::string&, std::stop_token);
    AnalysisCache(std::filesystem::path folder, FreeSpace available, std::optional<AnalysisCacheLimits> appStartLimits)
        : folder_(std::move(folder)), available_(std::move(available)), followsAppLimits_(appStartLimits.has_value()),
          appliedLimits_(appStartLimits) {}
    [[nodiscard]] AnalysisCacheResult readChecked(const std::string&,const AnalysisIdentity*,std::stop_token) const;
    [[nodiscard]] AnalysisTrimResult trimLocked(AnalysisCacheLimits, bool automatic,
                                                std::string&, std::stop_token,
                                                audio::DecodeCache* = nullptr, std::string_view incomingHash = {},
                                                bool checkReachable = false,
                                                std::optional<std::span<const std::string>> current = {}) const;
    [[nodiscard]] std::optional<std::uint64_t> freeSpace() const;
    // True defers an application trim until the caller releases ADIA's lock.
    bool maintainLocked(std::size_t publishedBytes, std::stop_token) const noexcept;
    void maintainApplication(std::stop_token) const noexcept;
    void maintainForDecoder(audio::DecodeCache&, std::uint64_t incoming, std::string_view hash,
                            bool checkReachable, std::stop_token = {}) const;
    [[nodiscard]] AnalysisTrimResult cleanupWithDecoder(audio::DecodeCache&, std::span<const std::string>,
                                                         std::string&, std::stop_token) const;
    std::filesystem::path folder_;
    FreeSpace available_;
    bool followsAppLimits_ = false;
    std::optional<AnalysisCacheLimits> automaticLimits_;
    mutable std::optional<AnalysisCacheLimits> appliedLimits_;
    mutable std::optional<std::uint64_t> estimatedBytes_;
};
[[nodiscard]] AnalysisCache& defaultAnalysisCache();
// Called with validated application preferences. A later owner-worker access
// performs maintenance without delaying the settings save.
void setDefaultAnalysisCacheLimits(AnalysisCacheLimits);
[[nodiscard]] AnalysisCacheLimits defaultAnalysisCacheLimits();
// Owner-worker Cleanup of the two subfolders under one selected cache root.
// Current project hashes and active PCM leases protect their matching records.
[[nodiscard]] AnalysisTrimResult cleanupMediaCache(AnalysisCache&, audio::DecodeCache&,
    std::span<const std::string> currentHashes, std::string&, std::stop_token = {});
} // namespace adi::media
