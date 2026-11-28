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
// Prepare OneShot and its first sample together for a drop on a MIDI track.
[[nodiscard]] PreparedSampleAssignment prepareSampleInstrument(Store&, std::int64_t track,
    const std::filesystem::path&, double engineRate, std::int64_t importedUtc,
    audio::DecodeCache* = nullptr, std::int64_t retainedDeviceId = 0);
// SC-482: Simpler's Reverse and Crop for OneShot's sample. Each makes a changed
// copy of the slot's sample (reversed; or cut to the region between Start and
// End), writes it as a new file under the project's Samples/Processed/Reverse
// or Samples/Processed/Crop, and prepares its assignment: one Undo step that
// imports the copy and replaces the slot, the original's media row untouched.
// Crop also moves Start to 0 % and End to 100 %, so the same region plays.
// Off the audio thread; nothing is journalled until the commit.
[[nodiscard]] PreparedSampleAssignment prepareSampleReverse(Store&, std::int64_t device,
    std::int32_t slot, double engineRate, std::int64_t importedUtc);
[[nodiscard]] PreparedSampleAssignment prepareSampleCrop(Store&, std::int64_t device,
    std::int32_t slot, double engineRate, std::int64_t importedUtc);
[[nodiscard]] CommitResult commitSampleAssignment(Store&,const PreparedSampleAssignment&,Actor=Actor::User);
}
