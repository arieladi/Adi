// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/audio_recording.hpp"
#include "adi/ops.hpp"
namespace adi::recording {
struct ClipIds {std::int64_t clip=0,media=0;std::string name;};
struct RecordingEdit {
    bool ok=false;
    std::string error,sourceProject;
    std::vector<OpRequest> requests;
    CommitOptions options;
private:
    struct Files;
    std::shared_ptr<Files> files_;
    const Store* owner_=nullptr;
    std::string client_;
    friend RecordingEdit prepareCommit(Store&,const engine::AudioRecordingResult&,std::span<const ClipIds>);
    friend CommitResult commit(Store&,RecordingEdit&,Actor);
};
// Owner-only bridge from a captured physical-input snapshot to a recoverable
// private take. Resolves this Store's source identity; does not change history.
// Then use prepareCommit/commit below, retaining the caller's document generation.
[[nodiscard]] engine::AudioRecordingResult captureMemory(Store&,const engine::AudioMemorySnapshot&,
    std::int64_t track,std::int64_t timelineStartSample,const std::filesystem::path& workFolder);
/// Owner/worker project connection. Prepare and commit through the same
/// still-open Store handle; another/reopened connection needs fresh preparation.
/// Match ids to captured tracks in order.
/// Retain the originating document generation on the caller; source identity
/// is also checked here. Actual sample boundaries must round-trip through the
/// current musical clock exactly. Does not change project rows or publish files.
[[nodiscard]] RecordingEdit prepareCommit(Store&,const engine::AudioRecordingResult&,std::span<const ClipIds>);
/// One undo step for every take in the capture. Failure rolls back project
/// rows and only newly published assets; private takes remain for retry.
[[nodiscard]] CommitResult commit(Store&,RecordingEdit&,Actor=Actor::User);
}
