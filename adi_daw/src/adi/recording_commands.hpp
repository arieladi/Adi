// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/recording_recovery_plan.hpp"
#include "adi/ops.hpp"
namespace adi::recording {
struct ClipIds {std::int64_t clip=0,media=0;std::string name;};
/// Explicit owner-allocated identities. Lane IDs reserve fresh fallbacks when
/// no existing take lane is eligible; clip IDs are always new. All supplied
/// identities must be unoccupied even when a fallback is ultimately unused.
struct LoopPassIds {std::int64_t lane=0,clip=0;std::string name;};
struct LoopTrackIds {std::int64_t media=0;std::vector<LoopPassIds> passes;};
enum class RecoveryStatus {Ready, AlreadyPlaced, Refused};
struct RecoveryTake {
    RecoveryStatus status=RecoveryStatus::Refused;
    engine::AudioRecordingResult capture;
    std::string error;
};
// Document owner. Sets the project's stable identity in the recorder spec.
// The normal Session::startRecording path then persists the private take plan.
[[nodiscard]] bool configureRecovery(Store&,engine::AudioRecordingSpec&,std::string& error);
// Called only for a directory identified by the launch/session owner. The
// directory must be directly inside the known recording root. A live recorder,
// another project, a read-only store, and missing destinations are refused.
// A Ready take is prepared by the existing linear or loop Stop command.
[[nodiscard]] RecoveryTake recoverInterrupted(Store&,const std::filesystem::path& recordingRoot,
    const std::filesystem::path& directory);
// Startup worker API for PD-05. Skips live sessions, incomplete takes and
// receipts already placed in the matching project. Holding Alt can skip this
// call entirely. The caller presents the candidates and revalidates on reopen.
[[nodiscard]] std::vector<engine::recording_recovery::LaunchTake> discoverInterrupted();
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
/// Complete pass log to eligible existing or new take lanes and main-lane copies.
/// A pre-existing same-track take lane is eligible when every clip provably ends
/// at/before this pass's start. First visible (ord,id) wins; each is used once.
/// Unknown extents refuse reuse, not the recording. New fallback lanes form a
/// chronological group above existing lanes, whose relative order is retained. The
/// last partial pass replaces only its captured interval; earlier audible
/// material outside that interval survives. One source is imported per unique
/// captured content, and all passes commit atomically with one Undo per pass.
/// Does not write project rows during preparation. Existing lane identities,
/// properties and clips are retained; only the new take is added to a reused lane.
/// Optional selection metadata is supplied once per pass on its first request.
/// No punch trimming, quantize or UI policy. Material recording leaves authored
/// automation unchanged. First-visible reuse is ADI's deterministic policy;
/// exact reference-DAW selection among several eligible lanes remains unmeasured.
[[nodiscard]] RecordingEdit prepareLoopCommit(Store&,const engine::AudioRecordingResult&,std::span<const LoopTrackIds>,
    std::span<const std::optional<Payload>> selectionsBefore={});
/// Linear and recovered captures have one Undo. Ordinary loop captures have
/// one per pass. Failure rolls
/// back project rows, cleans only identity-verified publications where supported,
/// and leaves private takes for retry.
[[nodiscard]] CommitResult commit(Store&,RecordingEdit&,Actor=Actor::User);
}
