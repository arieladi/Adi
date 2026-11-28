// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/source_hash_memo.hpp"
#include "adi/audio/io_audit.hpp"
#include <utility>

namespace adi::media {
namespace fs = std::filesystem;

std::optional<SourceHashMemo::Version> SourceHashMemo::version(const fs::path& source) {
    std::error_code error;
    const auto identity = readFileIdentity(source, error);
    if (!identity) return {};
    const auto bytes = fs::file_size(source, error);
    if (error) return {};
    const auto modified = fs::last_write_time(source, error);
    if (error || readFileIdentity(source, error) != identity) return {};
    return Version{*identity, {bytes, modified}};
}

SourceHash SourceHashMemo::verify(const fs::path& source, std::stop_token stop) noexcept {
    try {
        if (audio::onAudioThread) {
            audio::fileIoPoint();
            return {{{}, HashError::resource}};
        }
        if (stop.stop_requested()) return {{{}, HashError::cancelled}};
        const auto before = version(source);
        if (before) {
            std::lock_guard lock(mutex_);
            const auto cached = hashes_.find(before->identity);
            if (stop.stop_requested()) return {{{}, HashError::cancelled}};
            if (cached != hashes_.end() && cached->second.stamp == before->stamp)
                return {{cached->second.hex, HashError::none}, true};
        }

        auto digest = blake3File(source, stop);
        if (stop.stop_requested()) return {{{}, HashError::cancelled}};
        if (!digest || !before) return {std::move(digest)};
        if (version(source) != before) return {{{}, HashError::read}};
        std::lock_guard lock(mutex_);
        // Keep memory bounded on sessions browsing very large media libraries.
        // At capacity the memo still refreshes entries already admitted.
        constexpr std::size_t maxEntries = 4096;
        if (hashes_.size() < maxEntries || hashes_.contains(before->identity))
            hashes_.insert_or_assign(before->identity, Entry{before->stamp, digest.hex});
        return {std::move(digest)};
    } catch (...) {
        return {{{}, HashError::resource}};
    }
}

} // namespace adi::media
