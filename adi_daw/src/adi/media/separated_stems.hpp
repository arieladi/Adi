// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace adi::media {
struct SeparatedStem {
    std::string file;        // one UTF-8 basename beneath the closed result folder
    std::string name;        // authored display name, not an inferred model order
    std::string hash;        // exact returned WAV's BLAKE3
};
struct SeparationResult {
    std::int64_t sourceMedia = 0, sourceBegin = 0, frames = 0;
    std::string sourceHash;
    std::string service, model, version, request;
    std::optional<std::string> weightsHash; // mandatory for service "builtin"
    std::filesystem::path folder;
    std::vector<SeparatedStem> stems;
};
struct SeparatedStemIds {
    std::int64_t track = 0, clip = 0, media = 0;
    std::optional<int> paletteIndex;
};
struct SeparationImportOptions {
    std::int64_t group = 0, index = 0, positionTicks = 0, createdUtc = 0;
    std::optional<std::int64_t> parent;
    std::string name;
    std::vector<SeparatedStemIds> stems;
    std::uint64_t documentGeneration = 0;
    std::uint64_t maximumBytes = std::uint64_t{8} << 30;
    std::optional<Payload> selectionBefore;
    std::stop_token stop;
    std::function<void()> checkpoint; // owner service point; must not reenter Store
};
class PreparedSeparation {
public:
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept {return error_;}
private:
    struct Data;
    std::shared_ptr<Data> data_;
    std::string error_;
    friend PreparedSeparation prepareSeparatedStems(Store&,const SeparationResult&,const SeparationImportOptions&);
    friend CommitResult commitSeparatedStems(Store&,PreparedSeparation&,std::uint64_t,Actor,const CommitOptions&);
};

/// Owner-worker result gateway, NOT a model runner. The producer must be
/// finished/closed; request/result provenance is explicit, never inferred.
/// Captures immutable validated WAV copies and native requests without project
/// SQL writes. N is resource-bounded to 1..1024, not musically limited to four.
/// Source is an authenticated registered mono/stereo media window. Every stem
/// retains that rate/frame count; channels may be mono/stereo. Nonfinite audio,
/// path escapes/symlinks, mismatched hashes/shapes and stale documents refuse.
PreparedSeparation prepareSeparatedStems(Store&,const SeparationResult&,const SeparationImportOptions&);

/// One native transaction: owned media, group, N tracks/clips and model/request
/// audit row. Same still-open Store/client, generation and captured head only.
/// Caller veto/head policies remain enforced. Undo/redo retains pool audio.
/// SQL failure may leave unreferenced content-addressed pool files for GC/retry;
/// never deletes a published file another importer could already be using.
CommitResult commitSeparatedStems(Store&,PreparedSeparation&,std::uint64_t documentGeneration,
                                  Actor = Actor::Import,const CommitOptions& extra = {});
} // namespace adi::media
