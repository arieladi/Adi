// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/audio/stem_manifest.hpp"
#include "adi/ops.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
namespace adi::media {
struct StemImportTrack {
    std::int64_t track = 0, clip = 0, media = 0;
    std::optional<int> paletteIndex; // caller maps NI RGB to Live's 0..69 palette
};
struct StemImportOptions {
    std::int64_t group = 0, containerMedia = 0, positionTicks = 0, index = 0;
    std::optional<std::int64_t> parent;
    std::string groupName;
    std::vector<StemImportTrack> stems;
    std::uint64_t documentGeneration = 0; // caller increments on document replacement
    // Enabled NI mastering DSP is not ADI's Main processing. Default refusal;
    // the caller may explicitly choose raw decoded stems, retaining the original
    // container/manifest in the pool. The premixed master is never doubled in.
    bool allowRawMastering = false;
    std::stop_token stop;
    std::function<void()> service;
};
class StemImport {
  public:
    StemImport() = default;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const std::string &error() const noexcept {
        return error_;
    }
    [[nodiscard]] const audio::StemManifest &manifest() const noexcept {
        return manifest_;
    }
    [[nodiscard]] std::int64_t containerMedia() const noexcept {
        return containerMedia_;
    }

  private:
    struct Data;
    std::shared_ptr<Data> data_;
    std::string error_;
    audio::StemManifest manifest_;
    std::int64_t containerMedia_ = 0;
    friend StemImport prepareStemImport(Store &, const std::filesystem::path &, const StemImportOptions &);
    friend CommitResult commitStemImport(Store &, StemImport &, std::uint64_t, Actor, const CommitOptions &);
};
// Worker/document-owner thread only, never the callback. A complete immutable
// private copy is decoded before native requests are captured. Commit checks
// project identity, document generation and the captured journal head.
[[nodiscard]] StemImport prepareStemImport(Store &, const std::filesystem::path &, const StemImportOptions &);
// One undo step: group, one audio track/clip per stem, and owned pool assets.
// Failing publication/SQL preserves the private sources for retry/recovery;
// destructing the prepared job removes only its own private staging directory.
// Extra beforeCommit runs after publication, within the same SQL transaction.
[[nodiscard]] CommitResult commitStemImport(Store &, StemImport &, std::uint64_t documentGeneration,
                                            Actor = Actor::Import, const CommitOptions &extra = {});
} // namespace adi::media
