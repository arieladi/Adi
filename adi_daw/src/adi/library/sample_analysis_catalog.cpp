// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_analysis_catalog.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/library/index.hpp"
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace adi::library {
namespace {
struct TextHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
};

template<class T>
using Table = std::unordered_map<std::string, T, TextHash, std::equal_to<>>;
using Keys = std::unordered_set<std::string, TextHash, std::equal_to<>>;
}

struct SampleAnalysisCatalog::State {
    State(media::AnalysisCache& analysis, audio::DecodeCache& decode, const Index* annotations,
          std::optional<audio::WarpTimingSettings> analysisTiming)
        : service(analysis, decode), index(annotations), timing(analysisTiming) {}

    bool measure(const AnalysisJob& job, std::stop_token stop) {
        auto result = service.analyze(job.source, job.hash, stop, timing);
        if (!result || stop.stop_requested()) return false;
        if (index) result.analysis = applySampleOverrides(std::move(*result.analysis), index->analysisOverrides(result.hash));

        auto value = std::make_shared<const SampleAnalysis>(std::move(*result.analysis));
        std::lock_guard lock(mutex);
        const bool activeHash = hashesWanted.contains(result.hash);
        const bool activeKey = job.hash.empty() && keysWanted.contains(job.key);
        if (activeHash || activeKey) byHash.insert_or_assign(result.hash, std::move(value));
        if (activeKey) hashForKey.insert_or_assign(job.key, std::move(result.hash));
        return true;
    }

    SampleAnalysisService service;
    const Index* index;
    std::optional<audio::WarpTimingSettings> timing;
    std::mutex publicationMutex;
    mutable std::mutex mutex;
    Keys hashesWanted, keysWanted;
    Table<std::shared_ptr<const SampleAnalysis>> byHash;
    Table<std::string> hashForKey;
};

SampleAnalysisCatalog::SampleAnalysisCatalog(media::AnalysisCache& analysis, audio::DecodeCache& decode,
                                             const Index* annotations, std::optional<audio::WarpTimingSettings> timing)
    : state_(std::make_unique<State>(analysis, decode, annotations, timing)),
      worker_([this](const AnalysisJob& job, std::stop_token stop) { return state_->measure(job, stop); }) {}
SampleAnalysisCatalog::~SampleAnalysisCatalog() = default;

void SampleAnalysisCatalog::replaceFiles(std::vector<AnalysisJob>&& jobs, std::span<const std::string> shownKeys) {
    if (audio::onAudioThread) return;
    Keys hashes, keys;
    for (const auto& job : jobs) {
        if (!job.hash.empty()) hashes.insert(job.hash);
        else if (!job.key.empty()) keys.insert(job.key);
    }
    std::lock_guard publication(state_->publicationMutex);
    {
        std::lock_guard lock(state_->mutex);
        state_->hashesWanted.swap(hashes);
        state_->keysWanted.swap(keys);
        std::erase_if(state_->hashForKey, [&](const auto& pair) { return !state_->keysWanted.contains(pair.first); });
        for (const auto& [key, hash] : state_->hashForKey) state_->hashesWanted.insert(hash);
        std::erase_if(state_->byHash, [&](const auto& pair) { return !state_->hashesWanted.contains(pair.first); });
    }
    worker_.replaceJobs(std::move(jobs), shownKeys);
}

void SampleAnalysisCatalog::read(std::span<const SampleAnalysisLookup> files,
                                 std::vector<std::shared_ptr<const SampleAnalysis>>& output) const {
    if (audio::onAudioThread) return;
    std::lock_guard lock(state_->mutex);
    output.clear();
    output.reserve(files.size());
    for (const auto& file : files) {
        auto hash = file.hash;
        if (hash.empty()) {
            const auto found = state_->hashForKey.find(file.key);
            if (found != state_->hashForKey.end()) hash = found->second;
        }
        const auto found = state_->byHash.find(hash);
        output.push_back(found == state_->byHash.end() ? nullptr : found->second);
    }
}
} // namespace adi::library
