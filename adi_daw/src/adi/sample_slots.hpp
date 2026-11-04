// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include "adi/engine/param_ops.hpp"
#include <filesystem>
namespace adi::audio {class DecodeCache;}
namespace adi {
struct PreparedSampleAssignment {
    bool ok=false;
    std::string error,sourceFile;
    std::int64_t deviceId=0,mediaId=0;
    std::vector<OpRequest> requests;
    std::vector<engine::StateBlob> blobs;
    CommitOptions options;
};
// Off-thread preparation, including decode/SRC/derived profile. A single journal
// batch imports the file (deduplicated by hash) and replaces a declared slot.
[[nodiscard]] PreparedSampleAssignment prepareSampleAssignment(Store&,std::int64_t device,
    std::int32_t slot,const std::filesystem::path&,double engineRate,std::int64_t importedUtc,
    audio::DecodeCache* =nullptr);
[[nodiscard]] PreparedSampleAssignment prepareSampleClear(Store&,std::int64_t device,std::int32_t slot);
[[nodiscard]] CommitResult commitSampleAssignment(Store&,const PreparedSampleAssignment&,Actor=Actor::User);
}
