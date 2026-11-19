// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/media/peaks.hpp"
#include <filesystem>
#include <map>
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
// Bounded explicit little-endian envelope; source identity and body digest are
// checked before section allocation. No native structs, JSON or recursive parse.
[[nodiscard]] std::vector<std::byte> encodeAnalysis(const SampleAnalysis&);
[[nodiscard]] AnalysisCacheResult decodeAnalysis(std::span<const std::byte>);
class AnalysisCache {
public:
    explicit AnalysisCache(std::filesystem::path folder):folder_(std::move(folder)){}
    // Serialized off-callback I/O. One <hash>.adia per sample in this folder.
    // Missing/invalid optional data can be rebuilt. Updating one section keeps
    // other supported/unknown sections; inconsistent identity is replaced.
    [[nodiscard]] AnalysisCacheResult read(const AnalysisIdentity&,std::stop_token={}) const;
    [[nodiscard]] AnalysisCacheProblem update(const AnalysisIdentity&,const std::string& tag,
                                             const AnalysisSection&,std::string& error,std::stop_token={});
    [[nodiscard]] AnalysisCacheProblem remove(const std::string& hash,std::string& error);
    [[nodiscard]] const std::filesystem::path& folder()const noexcept{return folder_;}
private:
    std::filesystem::path folder_;
};
[[nodiscard]] AnalysisCache& defaultAnalysisCache();
} // namespace adi::media
