// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/io_audit.hpp"
#include "adi/library/sample_query.hpp"
#include "adi/library/sample_swap.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace adi::library {

// One immutable sample-search publication shared by the Browser and floating
// search. BrowserSnapshot is the panel-free BrowserLibrarySnapshot; the
// adapter can be built independently of the Browser chain. The owner supplies
// catalog records in listing order and refreshes the publication as analysis
// becomes available. Build it on the publication worker. Missing records still
// participate in filename matches.
template <class BrowserSnapshot>
class BrowserSampleQuery {
public:
    using Listing = std::remove_cvref_t<decltype(std::declval<const BrowserSnapshot&>().listing())>;
    using File = typename decltype(Listing::files)::value_type;

private:
    struct WordMatches {
        std::vector<const File*> filenames, tagged, combined;
    };
    struct Candidate {
        const File* file;
        const std::shared_ptr<const SampleAnalysis>* record;
    };

public:
    struct Results {
        // Offsets into the original Browser listing, ordered by the query.
        std::vector<SampleQueryMatch> matches;

    private:
        friend class BrowserSampleQuery;
        WordMatches words;
        std::vector<const SampleAnalysis*> measured;
    };

    struct CombinedMatch {
        const File* file = nullptr;
        std::shared_ptr<const SampleAnalysis> analysis;
        double score = 0.0;
    };

    struct CombinedResults {
        // File pointers borrow the two publications. Analysis may be retained
        // separately as the reference for a later, refreshed publication.
        std::vector<CombinedMatch> matches;

    private:
        friend class BrowserSampleQuery;
        WordMatches sharedWords, projectWords;
        std::vector<Candidate> joined;
        std::vector<const SampleAnalysis*> measured;
        std::vector<SampleQueryMatch> ranked;
    };

    BrowserSampleQuery(std::shared_ptr<const BrowserSnapshot> browser,
                       std::vector<std::shared_ptr<const SampleAnalysis>> analyses)
        : browser_(std::move(browser)), analyses_(std::move(analyses)) {
        if (!browser_) return;
        const auto& files = browser_->listing().files;
        analyses_.resize(files.size());
        Listing tagged;
        for (std::size_t row = 0; row < files.size(); ++row) {
            if (!files[row].isSample() || !analyses_[row]) continue;
            std::string searchable;
            for (const auto& tag : analyses_[row]->tags) {
                if (tag.empty()) continue;
                if (searchable.empty()) searchable = files[row].name;
                searchable.push_back('\n');
                searchable.append(tag);
            }
            if (searchable.empty()) continue;
            tagged.files.push_back(files[row]);
            tagged.files.back().name = std::move(searchable);
            taggedRows_.push_back(row);
        }
        if (!taggedRows_.empty()) {
            // Use the existing word postings and Place membership for the
            // tagged subset. No per-query scan or second word parser is needed.
            tagged.places = browser_->listing().places;
            tagIndex_ = std::make_unique<const BrowserSnapshot>(std::move(tagged));
        }
    }

    [[nodiscard]] const BrowserSnapshot* browser() const noexcept { return browser_.get(); }
    [[nodiscard]] const SampleAnalysis* analysis(std::size_t row) const noexcept {
        return row < analyses_.size() ? analyses_[row].get() : nullptr;
    }

    // WordQuery is the existing LibraryQuery. Terms may come from either the
    // name or its tags, with the same Unicode folding and word boundaries as
    // ordinary filename search.
    // Call off the audio callback with separate result storage per caller.
    template <class WordQuery>
    void query(const WordQuery& words, const std::string* volume, const SampleQuery& samples,
               Results& out) const {
        if (audio::onAudioThread) return;
        out.matches.clear();
        if (!browser_ || samples.limit == 0) return;

        const auto candidates = wordMatches(words, volume, out.words);
        out.measured.clear();
        out.measured.reserve(candidates.size());
        for (const auto* file : candidates) out.measured.push_back(analysis(rowOf(file)));
        querySamples(out.measured, samples, out.matches);
        for (auto& match : out.matches) match.row = rowOf(candidates[match.row]);
    }

    // All's sample stream. Both listings contain resolved source paths, so
    // the existing path index identifies duplicates without filesystem I/O.
    // A matching project name replaces its shared duplicate in place; other
    // project matches follow in project order. The joined stream is ranked
    // once, so limits and ties work across publications.
    template <class WordQuery>
    void queryCombined(const WordQuery& words, const BrowserSampleQuery* project, const SampleQuery& samples,
                       CombinedResults& out) const {
        if (audio::onAudioThread) return;
        out.matches.clear();
        if (samples.limit == 0) return;

        const auto shared = wordMatches(words, nullptr, out.sharedWords);
        const auto projectFiles = project ? project->wordMatches(words, nullptr, out.projectWords)
                                          : std::span<const File* const>{};
        out.joined.clear();
        out.joined.reserve(shared.size() + projectFiles.size());
        for (const auto* file : shared) out.joined.push_back({file, &recordFor(file)});
        if (!projectFiles.empty()) {
            const WordQuery anyName("");
            for (const auto* file : projectFiles) {
                const auto* record = &project->recordFor(file);
                const auto* duplicate = browser_ ? browser_->matchingFileAtPath(file->path, anyName) : nullptr;
                if (duplicate) {
                    if (!*record) record = &recordFor(duplicate);
                    const auto found = std::lower_bound(shared.begin(), shared.end(), duplicate, std::less<>{});
                    if (found != shared.end() && *found == duplicate) {
                        out.joined[static_cast<std::size_t>(found - shared.begin())] = {file, record};
                        continue;
                    }
                }
                out.joined.push_back({file, record});
            }
        }

        if (samples.rank != SampleQueryRank::LibraryOrder && samples.reference && !samples.reference->contentHash.empty()) {
            // A retained reference may come from the prior listing, with a
            // different object address even when it has the same bytes.
            std::erase_if(out.joined, [&](const auto& candidate) {
                return *candidate.record && (*candidate.record)->contentHash == samples.reference->contentHash;
            });
        }
        out.measured.clear();
        out.measured.reserve(out.joined.size());
        for (const auto& item : out.joined) out.measured.push_back(item.record->get());
        querySamples(out.measured, samples, out.ranked);
        out.matches.reserve(out.ranked.size());
        for (const auto& match : out.ranked) {
            const auto& item = out.joined[match.row];
            out.matches.push_back({item.file, *item.record, match.score});
        }
    }

    // Retain the first occurrence of each ranked content hash. Use all ranked
    // matches when alias files should never reduce the number of alternatives.
    static void makeSwapCandidates(std::span<const CombinedMatch> matches, std::vector<SimilarSampleCandidate>& out) {
        if (audio::onAudioThread) return;
        out.clear();
        std::unordered_set<std::string_view> seen;
        for (const auto& match : matches) {
            if (!match.file || match.file->path.empty() || !match.analysis || match.analysis->contentHash.empty()) continue;
            if (seen.insert(match.analysis->contentHash).second)
                out.push_back({match.file->path, match.analysis->contentHash});
        }
    }

private:
    [[nodiscard]] std::size_t rowOf(const File* file) const noexcept {
        return static_cast<std::size_t>(file - browser_->listing().files.data());
    }

    [[nodiscard]] const std::shared_ptr<const SampleAnalysis>& recordFor(const File* file) const noexcept {
        return analyses_[rowOf(file)];
    }

    template <class WordQuery>
    [[nodiscard]] std::span<const File* const> wordMatches(const WordQuery& words, const std::string* volume,
                                                           WordMatches& out) const {
        if (!browser_) return {};
        const auto& files = browser_->listing().files;
        out.filenames.clear();
        browser_->appendSamples(words, volume, out.filenames);
        std::span<const File* const> candidates = out.filenames;

        if (!words.empty() && tagIndex_) {
            out.tagged.clear();
            tagIndex_->appendSamples(words, volume, out.tagged);
            if (!out.tagged.empty()) {
                const auto* tagFiles = tagIndex_->listing().files.data();
                for (auto& file : out.tagged)
                    file = &files[taggedRows_[static_cast<std::size_t>(file - tagFiles)]];
                out.combined.clear();
                std::set_union(out.filenames.begin(), out.filenames.end(), out.tagged.begin(), out.tagged.end(),
                               std::back_inserter(out.combined), std::less<>{});
                candidates = out.combined;
            }
        }
        return candidates;
    }

    std::shared_ptr<const BrowserSnapshot> browser_;
    std::vector<std::shared_ptr<const SampleAnalysis>> analyses_;
    std::unique_ptr<const BrowserSnapshot> tagIndex_;
    std::vector<std::size_t> taggedRows_;
};

} // namespace adi::library
