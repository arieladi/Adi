// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/render/freeze_capture.hpp"
namespace adi::freeze {
struct FreezeFiles;
class FreezeEdit {
  public:
    bool ready() const noexcept;
    const std::string &error() const noexcept {
        return error_;
    }

  private:
    std::shared_ptr<FreezeFiles> files_;
    std::string error_;
    friend FreezeEdit prepareFreeze(Store &, render::FreezeAudioCapture, std::uint64_t, std::int64_t,
                                    std::int64_t);
    friend CommitResult commitFreeze(Store &, FreezeEdit &, std::uint64_t, CommitOptions);
};
// Whole ordinary tracks, from zero through ALL material plus the requested
// tail. No incoming audio/sidechain keys. Caller retains document generation.
// Explicit media ID allocation; native media operations and one undo step.
FreezeEdit prepareFreeze(Store &, render::FreezeAudioCapture, std::uint64_t currentGeneration,
                         std::int64_t firstMediaId, std::int64_t importedUtc = 0);
CommitResult commitFreeze(Store &, FreezeEdit &, std::uint64_t currentGeneration, CommitOptions = {});
// Removes a cache without deleting its media or any original device/state.
// Fingerprints must match unless explicitly acknowledging externally changed
// source state. Installed identity comparison is an owner-supplied check.
CommitResult
unfreeze(Store &, std::span<const std::int64_t> tracks, bool acknowledgeChanged = false,
         std::function<bool(std::int64_t, const Payload &identities, std::string &)> installed = {});
} // namespace adi::freeze
