// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/blob.hpp"
#include "adi/controller_data.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/recording_recovery_plan.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/note_diff.hpp"
#include "adi/ops.hpp"
#include <stop_token>
namespace adi::recording {
struct ClipIds {
    std::int64_t clip=0,media=0;
    std::string name;
    std::string trackName{}; // optional displayed track name for the filename
};
struct MidiExpressionLane {
    std::uint64_t noteId=0;
    ExpressionDim dimension=ExpressionDim::Pitch;
    std::span<const ExpressionPoint> points; // relative to the note's start
};
struct MidiControllerLane {
    int channel = 0, controller = 0;
    std::span<const controllers::Point> points; // in time order, relative to the clip's start
};
struct MidiLinearTake {
    std::int64_t track=0,clip=0,start=0,length=0;
    std::string name;
    std::span<const notes::NoteValues> notes; // relative to the clip's start
    std::span<const MidiExpressionLane> expression;
    bool overdub = false;
    std::span<const MidiControllerLane> controllers{};
};
struct MidiLinearLanding {
    std::int64_t clip = 0, sourceStart = 0;
    std::vector<std::uint64_t> noteIds;
    bool inTakeLane = false;
};
struct RecordingEdit;
enum class StagedRefresh { ready, restage, refused };
/// Owned capture and verified file staging. Safe to move between workers and
/// the document owner. Dropping it retains the original recoverable take.
struct StagedTake {
    bool ok = false;
    std::string error;
private:
    struct Data;
    std::shared_ptr<const Data> data_;
    friend struct RecordingEdit;
    friend StagedTake stageTake(const engine::AudioRecordingResult&,const std::filesystem::path&,std::stop_token);
    friend RecordingEdit prepareStagedCommit(Store&,const StagedTake&,std::span<const ClipIds>,
        std::span<const MidiLinearTake>,std::int64_t);
};
/// File worker only: stage and hash the closed native WAVs on the destination
/// filesystem. No access to the document or its SQLite connection.
[[nodiscard]] StagedTake stageTake(const engine::AudioRecordingResult&,const std::filesystem::path& projectFolder,
                                   std::stop_token = {});
/// Explicit owner-allocated identities. Lane IDs reserve fresh fallbacks when
/// no existing take lane is eligible; clip IDs are always new. All supplied
/// identities must be unoccupied even when a fallback is ultimately unused.
struct LoopPassIds {std::int64_t lane=0,clip=0;std::string name;};
struct LoopTrackIds {
    std::int64_t media=0;
    std::vector<LoopPassIds> passes;
    std::string trackName{}; // optional displayed track name for the filename
};
struct MidiLoopPlacement {
    std::int64_t track=0,start=0,length=0;
    std::int64_t mainClip=0,mainStream=0;
    std::string name;
};
struct MidiTakePass {
    LoopPassIds take;
    std::int64_t stream=0,length=0;
    // Recorder-owned note identities. Coordinates are relative to loop start.
    // The main image is the recorder's cumulative audible result for this pass.
    std::span<const notes::NoteValues> takeNotes,mainNotes;
};
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
// A file worker may validate with a private read-only Store and repair its
// staging copies. Only the editable document owner can place the result.
[[nodiscard]] RecoveryTake prepareInterrupted(Store&,const std::filesystem::path& recordingRoot,
    const std::filesystem::path& directory,std::string_view expectedRecording,std::stop_token);
// Startup worker API for PD-05. Skips live sessions, incomplete takes and
// receipts already placed in the matching project. Holding Alt can skip this
// call entirely. The caller presents the candidates and revalidates on reopen.
[[nodiscard]] std::vector<engine::recording_recovery::LaunchTake> discoverInterrupted();
struct RecordingEdit {
    bool ok=false;
    bool audioOutsidePunch=false; // valid latency placement with no frames inside the punch
    std::vector<std::int64_t> placedAudioClips; // linear clip IDs prepared from the placeable tracks
    std::string error,sourceProject;
    std::vector<OpRequest> requests;
    CommitOptions options;
    std::vector<MidiLinearLanding> midiLandings;
    // Test support, off-callback: hooks for each project-side AtomicOnly
    // publication at commit. Null hooks use the real OS.
    media::PublishHooks publicationHooks{};
private:
    struct Files;
    struct PreparedSource;
    std::vector<std::size_t> groupEnds_;
    static std::vector<PreparedSource> prepareSources(Store&,const engine::AudioRecordingResult&,
        std::span<const ClipIds>,RecordingEdit&,std::span<const std::size_t> sourceIndices = {},
        const StagedTake* = nullptr);
    static RecordingEdit prepareLinear(Store&,const engine::AudioRecordingResult*,std::span<const ClipIds>,
        std::span<const MidiLinearTake>,double,std::int64_t,const StagedTake* = nullptr);
    std::shared_ptr<Files> files_;
    const Store* owner_=nullptr;
    std::string client_;
    friend RecordingEdit prepareStagedCommit(Store&,const StagedTake&,std::span<const ClipIds>,
        std::span<const MidiLinearTake>,std::int64_t);
    friend bool publishStagedTake(RecordingEdit&,std::stop_token);
    friend std::string cleanupStagedTake(RecordingEdit&);
    friend StagedRefresh refreshStagedCommit(Store&,const StagedTake&,RecordingEdit&,std::span<const ClipIds>,
        std::span<const MidiLinearTake>,std::int64_t);
    friend RecordingEdit prepareCommit(Store&,const engine::AudioRecordingResult&,std::span<const ClipIds>,
        std::span<const MidiLinearTake>,std::int64_t);
    friend RecordingEdit prepareMidiLinearCommit(Store&,std::span<const MidiLinearTake>,double);
    friend RecordingEdit prepareLoopCommit(Store&,const engine::AudioRecordingResult&,std::span<const LoopTrackIds>,std::span<const std::optional<Payload>>);
    friend RecordingEdit prepareMidiLoopCommit(Store&,const MidiLoopPlacement&,std::span<const MidiTakePass>);
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
/// Subtract each track's captured signed latency offset from the timeline start,
/// using the caller's offset for tracks without one. A correction that reaches before song zero
/// starts the clip at zero and hides the frames before it, so the rest stays in time.
[[nodiscard]] RecordingEdit prepareCommit(Store&,const engine::AudioRecordingResult&,std::span<const ClipIds>,
    std::int64_t placementOffsetSamples=0);
/// Publish a linear audio and MIDI take together in one placement and Undo.
[[nodiscard]] RecordingEdit prepareCommit(Store&,const engine::AudioRecordingResult&,std::span<const ClipIds>,
    std::span<const MidiLinearTake>,std::int64_t placementOffsetSamples=0);
/// Prepare a linear take on each destination's Arrangement main lane. The
/// caller assigns distinct clip IDs and closes held notes at the take's end.
/// Empty notes are allowed for a take with a positive recorded length.
[[nodiscard]] RecordingEdit prepareMidiLinearCommit(Store&,std::span<const MidiLinearTake>,double sampleRate);
/// Document owner: assemble current project rows from the staged media proof.
/// No full file reads or publication. The resulting Store binding stays on the
/// owner thread even while its prepared files are handed to a worker.
[[nodiscard]] RecordingEdit prepareStagedCommit(Store&,const StagedTake&,std::span<const ClipIds>,
    std::int64_t placementOffsetSamples=0);
[[nodiscard]] RecordingEdit prepareStagedCommit(Store&,const StagedTake&,std::span<const ClipIds>,
    std::span<const MidiLinearTake>,std::int64_t placementOffsetSamples=0);
/// File worker: publish verified files atomically. The owner calls commit
/// after success, or reads edit.error. No SQLite access on the worker.
[[nodiscard]] bool publishStagedTake(RecordingEdit&,std::stop_token = {});
/// Owner, before commit: when the Undo head moved during publication, rebuild
/// against current rows if the already-published media binding is unchanged.
[[nodiscard]] StagedRefresh refreshStagedCommit(Store&,const StagedTake&,RecordingEdit&,std::span<const ClipIds>,
    std::int64_t placementOffsetSamples=0);
[[nodiscard]] StagedRefresh refreshStagedCommit(Store&,const StagedTake&,RecordingEdit&,std::span<const ClipIds>,
    std::span<const MidiLinearTake>,std::int64_t placementOffsetSamples=0);
/// File worker, after a successful staged commit: remove private recovery
/// files no longer needed by history. Empty means there is no cleanup warning.
[[nodiscard]] std::string cleanupStagedTake(RecordingEdit&);
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
/// Owner preparation for a new MIDI loop recording. Creates a real take lane
/// and MIDI clip for every completed pass, including a final partial pass.
/// The recorder supplies each pass's main image, including notes held across
/// wraps. The take image closes any overhanging note at its pass boundary.
[[nodiscard]] RecordingEdit prepareMidiLoopCommit(Store&,const MidiLoopPlacement&,std::span<const MidiTakePass>);
/// Prepare the recorder's explicit quantize diff after the captured take has
/// committed. The caller commits it with notes::commitDiff as its own Undo.
[[nodiscard]] notes::PreparedDiff prepareRecordQuantize(Store&,std::int64_t clip,const notes::Diff&);
struct MidiRecordQuantize {
    std::int64_t clip = 0;
    std::int64_t sourceStart = 0, timelineStart = 0, length = 0;
    std::int64_t grid = 0;
    std::span<const std::uint64_t> noteIds;
};
// Quantize only the newly recorded identities, sharing one Undo across all armed tracks.
[[nodiscard]] notes::PreparedDiff prepareRecordQuantize(Store&,std::span<const MidiRecordQuantize>);
/// Linear and recovered captures have one Undo. Ordinary loop captures have
/// one per pass. Failure rolls
/// back project rows, cleans only identity-verified publications where supported,
/// and leaves private takes for retry.
[[nodiscard]] CommitResult commit(Store&,RecordingEdit&,Actor=Actor::User);
}
