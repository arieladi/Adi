// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/audio_recording.hpp"
#include "adi/ops.hpp"
namespace adi::recording {
struct ClipIds {std::int64_t clip=0,media=0;std::string name;};
/// Explicit owner-allocated identities. Each pass gets a new take lane; an
/// occupied lane ID refuses rather than inventing automatic lane-reuse policy.
struct LoopPassIds {std::int64_t lane=0,clip=0;std::string name;};
struct LoopTrackIds {std::int64_t media=0;std::vector<LoopPassIds> passes;};
struct RecordingEdit {
    bool ok=false;
    std::string error,sourceProject;
    std::vector<OpRequest> requests;
    CommitOptions options;
private:
    struct Files;
    struct PreparedSource;
    std::vector<std::size_t> groupEnds_;
    static std::vector<PreparedSource> prepareSources(Store&,const engine::AudioRecordingResult&,
        std::span<const ClipIds>,RecordingEdit&);
    std::shared_ptr<Files> files_;
    const Store* owner_=nullptr;
    std::string client_;
    friend RecordingEdit prepareCommit(Store&,const engine::AudioRecordingResult&,std::span<const ClipIds>);
    friend RecordingEdit prepareLoopCommit(Store&,const engine::AudioRecordingResult&,std::span<const LoopTrackIds>,std::span<const std::optional<Payload>>);
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
/// Opted-in loop captures are refused, including a single partial pass: their
/// take-lane/main-copy publication and per-pass history are a separate contract.
[[nodiscard]] RecordingEdit prepareCommit(Store&,const engine::AudioRecordingResult&,std::span<const ClipIds>);
/// Complete pass log to new take lanes and cumulative main-lane copies. The
/// last partial pass replaces only its captured interval; earlier audible
/// material outside that interval survives. One source is imported per unique
/// captured content, and all passes commit atomically with one Undo per pass.
/// Does not write project rows during preparation. Existing take lanes are
/// unaffected. Optional selection metadata is supplied once per pass and stored
/// on that pass's first request. No automatic reuse, punch trimming, quantize
/// or UI policy. Material recording leaves authored automation unchanged.
[[nodiscard]] RecordingEdit prepareLoopCommit(Store&,const engine::AudioRecordingResult&,std::span<const LoopTrackIds>,
    std::span<const std::optional<Payload>> selectionsBefore={});
/// Linear captures have one Undo. Loop captures have one per pass. Failure rolls
/// back project rows, cleans only identity-verified publications where supported,
/// and leaves private takes for retry.
[[nodiscard]] CommitResult commit(Store&,RecordingEdit&,Actor=Actor::User);
}
