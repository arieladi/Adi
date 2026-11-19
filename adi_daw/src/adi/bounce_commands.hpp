// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/material_commands.hpp"
namespace adi::clip_edit {
struct TrackBounceOptions {
    render::Job job; // explicit range/format/precision; source and paths are captured
    std::int64_t sourceTrack = 0, newTrack = 0, clipId = 0, mediaId = 0;
    // Needed only to retain a user-selected explicit Main destination.
    std::int64_t routingId = 0;
    std::string name; // empty: source name + " (Bounce)"
    bool muteOriginal = true;
    std::uint64_t documentGeneration = 0;
};
struct InPlaceBounceOptions {
    render::Job job; // whole track from sample zero, with explicit tail/end
    std::int64_t sourceTrack = 0, clipId = 0, mediaId = 0;
    std::string name;
    std::uint64_t documentGeneration = 0;
};
class TrackBounce {
  public:
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const std::string &error() const noexcept {
        return error_;
    }

  private:
    struct Data;
    std::shared_ptr<Data> data_;
    std::string error_;
    friend TrackBounce prepareBounceToNewTrack(Store &, const TrackBounceOptions &, engine::DeviceLoader,
                                               render::Control);
    friend TrackBounce prepareBounceInPlace(Store &, const InPlaceBounceOptions &, engine::DeviceLoader,
                                            render::Control);
    friend CommitResult commitTrackBounce(Store &, TrackBounce &, std::uint64_t, Actor);
};
// Direct post-fader wet render, placed Warp off on a plain audio sibling.
// The source's parent, color and explicit Main destination are captured; the
// source may be muted in the same undo step. Returns, sends and sidechain
// connections are not copied: this command places the selected direct output.
[[nodiscard]] TrackBounce prepareBounceToNewTrack(Store &, const TrackBounceOptions &,
                                                  engine::DeviceLoader = {}, render::Control = {});
// Whole-chain in-place replacement: only regular tracks without incoming
// main/send audio. All their clips must fit the explicit [0,end) range. Effects
// are parked bypassed, baked input gain resets to zero; fader/pan stay live.
[[nodiscard]] TrackBounce prepareBounceInPlace(Store &, const InPlaceBounceOptions &,
                                               engine::DeviceLoader = {}, render::Control = {});
[[nodiscard]] CommitResult commitTrackBounce(Store &, TrackBounce &, std::uint64_t documentGeneration,
                                             Actor = Actor::User);
} // namespace adi::clip_edit
