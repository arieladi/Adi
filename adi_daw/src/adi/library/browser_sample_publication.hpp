// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/library/browser_sample_query.hpp"
#include "adi/library/sample_analysis_catalog.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace adi::library {

struct BrowserIndexedSample {
    std::filesystem::path path;
    std::string hash;
    std::uintmax_t size = 0;
    std::int64_t modifiedNs = 0; // Unix epoch, as in Index::Entry.
    // Supply only when byte verification proved hash for this exact native
    // revision. The persisted Index size and modified time cannot prove that.
    std::optional<media::FileRevision> verifiedRevision = {};
};

namespace browser_publication_detail {
inline std::optional<std::int64_t> indexedStamp(const media::FileRevision& revision) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if (revision.identity.scheme == media::FileIdentityScheme::Posix) {
        constexpr std::int64_t nanosPerSecond = 1'000'000'000;
        const auto [seconds, nanos] = revision.modified;
        if (seconds < minimum / nanosPerSecond || seconds > maximum / nanosPerSecond ||
            nanos < 0 || nanos >= nanosPerSecond || seconds * nanosPerSecond > maximum - nanos) return {};
        return seconds * nanosPerSecond + nanos;
    }
    // Windows FileRevision stores 100 ns ticks from 1601 in its first field.
    constexpr std::int64_t unixEpochTicks = 116'444'736'000'000'000;
    if (revision.modified[0] < 0) return {};
    const auto sinceUnix = revision.modified[0] - unixEpochTicks;
    if (sinceUnix < minimum / 100 || sinceUnix > maximum / 100) return {};
    return sinceUnix * 100;
}

inline bool validHash(std::string_view hash) {
    return hash.size() == 64 && std::ranges::all_of(hash, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline std::string revisionKey(const std::filesystem::path& path, const media::FileRevision& revision) {
    auto result = media::pathUtf8(path);
    result.reserve(result.size() + 128);
    result.push_back('\0'); // A filesystem path cannot contain a null byte.
    const auto append = [&](auto value) {
        std::array<char, 32> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        result.append(buffer.data(), converted.ptr);
        result.push_back(':');
    };
    append(static_cast<unsigned int>(revision.identity.scheme));
    append(revision.identity.volume);
    append(revision.identity.file);
    append(revision.identity.fileHigh);
    append(revision.size);
    for (auto time : revision.modified) append(time);
    for (auto time : revision.changed) append(time);
    return result;
}
}

// One publication owner per catalog. Call from a single, non-audio owner
// worker. Immutable query snapshots may be shared with both search callers.
template <class BrowserSnapshot>
class BrowserSamplePublication {
public:
    using Query = BrowserSampleQuery<BrowserSnapshot>;

    explicit BrowserSamplePublication(SampleAnalysisCatalog& catalog) : catalog_(catalog) {}
    BrowserSamplePublication(const BrowserSamplePublication&) = delete;
    BrowserSamplePublication& operator=(const BrowserSamplePublication&) = delete;

    // A successful replacement invalidates refreshes and priority changes
    // carrying a prior generation. All observations and keys are prepared
    // before the queue replacement. Cancellation before that commit preserves
    // the prior generation. The catalog keeps its current pause state.
    bool replace(std::uint64_t generation, std::shared_ptr<const BrowserSnapshot> listing,
                 std::span<const BrowserIndexedSample> indexed = {}, std::span<const std::size_t> shown = {},
                 std::stop_token stop = {}) {
        if (audio::onAudioThread || stop.stop_requested() || (generation_ && generation <= *generation_)) return false;
        const auto files = filesFor(listing);
        struct Observation { const BrowserIndexedSample* source; std::optional<std::size_t> previous; };
        std::unordered_map<std::filesystem::path, Observation> observations;
        observations.reserve(indexed.size());
        for (const auto& observation : indexed) {
            if (stop.stop_requested()) return false;
            const auto [slot, added] = observations.try_emplace(observation.path, Observation{&observation, {}});
            if (!added) slot->second.source = nullptr;
        }
        const auto oldFiles = filesFor(listing_);
        for (std::size_t i = 0; i < oldFiles.size(); ++i) {
            if (rows_[i].hash.empty() && !attached_[i]) continue;
            auto slot = observations.try_emplace(oldFiles[i].path, Observation{nullptr, {}}).first;
            if (!slot->second.previous) slot->second.previous = i;
        }

        std::vector<Row> rows(files.size());
        std::vector<AnalysisJob> jobs;
        jobs.reserve(files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (stop.stop_requested()) return false;
            const auto& file = files[i];
            if (!file.isSample() || !file.path.is_absolute()) continue;
            auto& row = rows[i];
            std::error_code error;
            row.revision = media::readFileRevision(file.path, error);
            if (!row.revision) continue;
            row.key = browser_publication_detail::revisionKey(file.path, *row.revision);
            if (const auto indexedRow = observations.find(file.path); indexedRow != observations.end()) {
                const auto& [source, previous] = indexedRow->second;
                if (previous) row.hash = previousVerifiedHash(*previous, *row.revision);
                if (row.hash.empty() && source && source->verifiedRevision == row.revision &&
                    source->size == row.revision->size &&
                    browser_publication_detail::indexedStamp(*row.revision) == source->modifiedNs &&
                    browser_publication_detail::validHash(source->hash))
                    row.hash = source->hash;
            }
            // Without proof for this native revision the existing worker key
            // makes the analysis service hash the current bytes off the owner.
            jobs.push_back({file.path, row.hash, row.key});
        }

        std::vector<SampleAnalysisLookup> lookups;
        lookups.reserve(rows.size());
        for (const auto& row : rows) lookups.push_back({row.key, row.hash});
        auto shownKeys = keysFor(rows, shown);
        std::vector<Measurement> attached(rows.size()), seen(rows.size());
        if (stop.stop_requested()) return false;
        catalog_.replaceFiles(std::move(jobs), shownKeys);
        rows_.swap(rows);
        lookups_.swap(lookups);
        attached_.swap(attached);
        seen_.swap(seen);
        listing_ = std::move(listing);
        generation_ = generation;
        published_.reset();
        changed_ = true;
        return true;
    }

    bool prioritize(std::uint64_t generation, std::span<const std::size_t> shown) {
        if (audio::onAudioThread || generation_ != generation) return false;
        catalog_.worker().prioritize(keysFor(rows_, shown));
        return true;
    }

    // Validate new records when attaching them. A changed or missing source
    // remains a filename result until the next listing. A returned snapshot
    // keeps its measurements even if files later change or the owner rescans.
    [[nodiscard]] std::shared_ptr<const Query> snapshot(std::uint64_t generation, std::stop_token stop = {}) {
        if (audio::onAudioThread || stop.stop_requested() || generation_ != generation) return {};
        catalog_.read(lookups_, scratch_);
        const auto files = filesFor(listing_);
        for (std::size_t row = 0; row < scratch_.size(); ++row) {
            if (stop.stop_requested()) return {};
            auto& observed = scratch_[row];
            if (observed == seen_[row]) continue;
            seen_[row] = observed;
            if (observed) {
                std::error_code error;
                if (media::readFileRevision(files[row].path, error) != rows_[row].revision) observed.reset();
            }
            if (observed != attached_[row]) {
                attached_[row] = std::move(observed);
                changed_ = true;
            }
        }
        if (stop.stop_requested()) return {};
        if (changed_) {
            auto next = std::make_shared<const Query>(listing_, attached_);
            if (stop.stop_requested()) return {};
            published_ = std::move(next);
            changed_ = false;
        }
        return published_;
    }

private:
    struct Row {
        std::string key, hash;
        std::optional<media::FileRevision> revision;
    };
    using Measurement = std::shared_ptr<const SampleAnalysis>;
    using File = typename Query::File;

    static std::span<const File> filesFor(const std::shared_ptr<const BrowserSnapshot>& listing) noexcept {
        return listing ? std::span<const File>{listing->listing().files} : std::span<const File>{};
    }

    static std::vector<std::string> keysFor(std::span<const Row> rows, std::span<const std::size_t> shown) {
        std::vector<std::string> keys;
        keys.reserve(shown.size());
        for (const auto row : shown)
            if (row < rows.size() && !rows[row].key.empty()) keys.push_back(rows[row].key);
        return keys;
    }

    std::string_view previousVerifiedHash(std::size_t previous, const media::FileRevision& revision) const {
        const auto& earlier = rows_[previous];
        if (earlier.revision != revision) return {};
        if (!earlier.hash.empty()) return earlier.hash;
        if (attached_[previous]) return attached_[previous]->contentHash;
        return {};
    }

    SampleAnalysisCatalog& catalog_;
    std::optional<std::uint64_t> generation_;
    std::shared_ptr<const BrowserSnapshot> listing_;
    std::vector<Row> rows_;
    std::vector<SampleAnalysisLookup> lookups_; // Borrow immutable strings in rows_.
    std::vector<Measurement> attached_, seen_, scratch_;
    std::shared_ptr<const Query> published_;
    bool changed_ = false;
};

} // namespace adi::library
