// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/audio_recording.hpp"
#include "adi/media/publish_file.hpp"
#include <string_view>

namespace adi::engine::recording_recovery {
// Versioned, immutable pre-capture intent. Target i owns only take-i.wav; no
// serialized path is ever resolved. First accepted placement is separate from
// this prepared phase because count-in can finish after begin() returns.
struct TakePlan {
    std::string recordingId, projectUuid;
    std::string sourceProject; // resolved SQLite path, metadata only while parsed
    std::uint32_t sampleRate = 0;
    audio::WavFormat format = audio::WavFormat::Float32;
    std::vector<RecordingTarget> targets;
    std::optional<AudioRecordingLoop> loop;
    std::vector<media::FileIdentity> identities; // exact target order
};
struct OwnedPlan {
    TakePlan plan;
    // Held by live capture AND inactive inspection. Never replace owner.lock;
    // its exclusive native lease is released only after the owned work ends.
    std::shared_ptr<media::ProjectFileLease> ownership;
    std::string warning; // publication succeeded; directory sync was advisory
};
struct LaunchTake {
    std::filesystem::path directory;
    std::string sourceProject,projectUuid,recordingId;
    std::vector<std::int64_t> tracks;
};

// Register before any audio is accepted. The process shares a single native
// session marker. A launch sweep only reads markers it can lease exclusively.
[[nodiscard]] std::string registerLaunchTake(const std::filesystem::path&,const TakePlan&);
void forgetLaunchTake(std::string_view recordingId) noexcept;
[[nodiscard]] std::vector<LaunchTake> findLaunchTakes();

// Owner/worker only, never the callback. Throw on invalid data or pre-publication
// failure. directory must be a canonical, privately reserved ordinary directory
// containing exactly the freshly created standard take-i.wav files. Their
// writers may be open. begin publishes take.json before capture is admitted.
[[nodiscard]] OwnedPlan begin(const AudioRecordingSpec&,
                             const std::filesystem::path& directory,
                             std::string_view projectUuid);
// The caller holds OwnedPlan::ownership throughout these calls. start.json is
// immutable: an identical repeat is allowed, a different ID/sample is refused.
// Missing start metadata throws: absence means capture never established its
// placement, and is not guessed from an arm-time transport position.
[[nodiscard]] std::int64_t readStart(const std::filesystem::path& directory,
                                   std::string_view recordingId);
// A nonempty return is an advisory directory durability warning, not failure.
// File data sync must succeed before publication. No physical-audio durability
// or bounded power-loss window is claimed by persisting this metadata alone.
[[nodiscard]] std::string writeStart(const std::filesystem::path& directory,
                                     std::string_view recordingId,
                                     std::int64_t startSample);
// Acquires the exclusive lifetime marker BEFORE reading any recovery metadata.
// Active captures, unknown fields/versions/files, links/reparse points and
// changed take identities refuse. Missing start.json is a valid unstarted plan.
// No WAV repair, deletion, project access or automatic discovery happens here.
[[nodiscard]] OwnedPlan openInactive(const std::filesystem::path& directory);
}
