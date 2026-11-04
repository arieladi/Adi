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
    friend RecordingEdit prepareCommit(Store&,const engine::AudioRecordingResult&,std::span<const ClipIds>);
    friend CommitResult commit(Store&,RecordingEdit&,Actor);
};
/// Owner/worker project connection. Match ids to captured tracks in order.
/// Retain the originating document generation on the caller; source identity
/// is also checked here. Actual sample boundaries must round-trip through the
/// current musical clock exactly. Does not change project rows or publish files.
[[nodiscard]] RecordingEdit prepareCommit(Store&,const engine::AudioRecordingResult&,std::span<const ClipIds>);
/// One undo step for every take in the capture. Failure rolls back project
/// rows and only newly published assets; private takes remain for retry.
[[nodiscard]] CommitResult commit(Store&,RecordingEdit&,Actor=Actor::User);
}
