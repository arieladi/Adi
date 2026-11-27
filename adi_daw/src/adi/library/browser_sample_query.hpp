// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/io_audit.hpp"
#include "adi/library/sample_query.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
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

    struct Results {
        // Offsets into the original Browser listing, ordered by the query.
        std::vector<SampleQueryMatch> matches;

    private:
        friend class BrowserSampleQuery;
        std::vector<const File*> filenames, tagged, combined;
        std::vector<const SampleAnalysis*> measured;
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
        const auto rowOf = [&](const File* file) { return static_cast<std::size_t>(file - files.data()); };
        out.measured.clear();
        out.measured.reserve(candidates.size());
        for (const auto* file : candidates) out.measured.push_back(analysis(rowOf(file)));
        querySamples(out.measured, samples, out.matches);
        for (auto& match : out.matches) match.row = rowOf(candidates[match.row]);
    }

private:
    std::shared_ptr<const BrowserSnapshot> browser_;
    std::vector<std::shared_ptr<const SampleAnalysis>> analyses_;
    std::unique_ptr<const BrowserSnapshot> tagIndex_;
    std::vector<std::size_t> taggedRows_;
};

} // namespace adi::library
