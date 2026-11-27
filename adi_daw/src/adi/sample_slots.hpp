// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include "adi/engine/param_ops.hpp"
#include <filesystem>
#include <string_view>
namespace adi::audio {class DecodeCache;}
namespace adi {
struct PreparedSampleAssignment {
    bool ok=false;
    bool sourceChanged=false;
    std::string error,sourceFile;
    std::int64_t deviceId=0,mediaId=0;
    std::vector<OpRequest> requests;
    std::vector<engine::StateBlob> blobs;
    CommitOptions options;
};
// Off-thread preparation, including decode/SRC/derived profile. A single journal
// batch imports the file (deduplicated by hash) and replaces a declared slot.
// A supplied content hash binds an indexed selection to the imported bytes.
[[nodiscard]] PreparedSampleAssignment prepareSampleAssignment(Store&,std::int64_t device,
    std::int32_t slot,const std::filesystem::path&,double engineRate,std::int64_t importedUtc,
    audio::DecodeCache* =nullptr,std::string_view expectedHash={});
[[nodiscard]] PreparedSampleAssignment prepareSampleClear(Store&,std::int64_t device,std::int32_t slot);
// Prepare OneShot and its first sample together for a drop on a MIDI track.
[[nodiscard]] PreparedSampleAssignment prepareSampleInstrument(Store&, std::int64_t track,
    const std::filesystem::path&, double engineRate, std::int64_t importedUtc,
    audio::DecodeCache* = nullptr, std::int64_t retainedDeviceId = 0);
[[nodiscard]] CommitResult commitSampleAssignment(Store&,const PreparedSampleAssignment&,Actor=Actor::User);
}
