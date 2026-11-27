// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/library/analysis_worker.hpp"
#include "adi/library/sample_analysis.hpp"
#include <memory>
#include <span>
#include <string_view>

namespace adi::library {
class Index;

struct SampleAnalysisLookup {
    // Pass the current content hash when known. Otherwise use the same key as
    // the dispatched job, including a revision when a listing detects edits.
    std::string_view key, hash;
};

// Off-callback home for the measurements of a published library listing.
// Owns its worker and shares immutable results with both query callers. It
// borrows the owner's caches and index, which must outlive it.
class SampleAnalysisCatalog {
public:
    SampleAnalysisCatalog(media::AnalysisCache&, audio::DecodeCache&, const Index* annotations = nullptr,
                          std::optional<audio::WarpTimingSettings> timing = {});
    ~SampleAnalysisCatalog();
    SampleAnalysisCatalog(const SampleAnalysisCatalog&) = delete;
    SampleAnalysisCatalog& operator=(const SampleAnalysisCatalog&) = delete;

    // Publish pending jobs and displayed priority together. Resume before the
    // first scan; later view changes may reprioritize the same pending work.
    void replaceFiles(std::vector<AnalysisJob>&&, std::span<const std::string> shownKeys = {});
    AnalysisWorker& worker() noexcept { return worker_; }

    // Output order matches the input's word-query results. Null means that the
    // worker has yet to publish a measurement for the current file identity.
    void read(std::span<const SampleAnalysisLookup>,
              std::vector<std::shared_ptr<const SampleAnalysis>>&) const;

private:
    struct State;
    std::unique_ptr<State> state_;
    AnalysisWorker worker_;
};
} // namespace adi::library
