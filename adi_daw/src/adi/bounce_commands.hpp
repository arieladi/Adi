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
    // Post-FX wet audio with the source's live mixer and sends reapplied.
    bool copyMixerSettings = false;
};
struct InPlaceBounceOptions {
    render::Job job; // whole track from sample zero, with explicit tail/end
    std::int64_t sourceTrack = 0, clipId = 0, mediaId = 0;
    std::string name;
    std::uint64_t documentGeneration = 0;
    bool replaceDevices = false; // remove the baked chain and convert the original track to audio
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
// Wet render placed Warp off on an audio sibling with the source's parent,
// color and explicit Main destination. copyMixerSettings renders post-FX,
// pre-fader, including verified frozen wet audio, and copies live mixer and
// send settings. The default renders post-fader without copying those settings.
// The source may be muted in the same undo step.
[[nodiscard]] TrackBounce prepareBounceToNewTrack(Store &, const TrackBounceOptions &,
                                                  engine::DeviceLoader = {}, render::Control = {});
// Whole-chain in-place replacement: only regular tracks without incoming
// main/send audio. All their clips must fit the explicit [0,end) range. Effects
// are parked bypassed by default. replaceDevices removes baked devices and
// converts the track to audio. Baked input gain resets to zero; fader/pan stay live.
[[nodiscard]] TrackBounce prepareBounceInPlace(Store &, const InPlaceBounceOptions &,
                                               engine::DeviceLoader = {}, render::Control = {});
[[nodiscard]] CommitResult commitTrackBounce(Store &, TrackBounce &, std::uint64_t documentGeneration,
                                             Actor = Actor::User);
} // namespace adi::clip_edit
