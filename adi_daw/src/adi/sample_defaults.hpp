// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/library/sample_settings.hpp"
#include "adi/ops.hpp"
namespace adi::library {class Index;}
namespace adi::media {struct MediaResult;}
namespace adi::clip_edit {
struct SampleDefaultsCapture {
    bool ok=false;
    std::string error,hash;
    std::optional<library::SampleSettings> settings;
};
// Serialized Store owner, off callback. Reuses native clip-copy source clocks;
// no project mutation. referenceRate is the heard/session rate for clock pins.
[[nodiscard]] SampleDefaultsCapture captureSampleDefaults(Store&,std::int64_t clipId,double referenceRate);
[[nodiscard]] library::SampleSettingsProblem saveSampleDefaults(Store&,std::int64_t clipId,double referenceRate,
                                                               library::Index&,std::string& error);
// Resolve the captured or reused media row against authored settings. Absence
// returns an empty result; an incompatible or unreadable default is an error.
[[nodiscard]] std::optional<library::SampleSettings> loadSampleDefaults(Store &, library::Index &,
                                                                        const media::MediaResult &, const OpRequest &);
// Material preparation helper: modifies only the caller's uncommitted image.
// Existing media id/clip id/track/placement survive; authored defaults supply
// the source window, playback controls and initial intrinsic source/loop span.
void applySampleDefaults(Store&,Payload& image,const library::SampleSettings&,double referenceRate);

// Prepare an Arrangement clip with defaults in a plain file drop's batch.
// The media and track can be new rows supplied by preceding requests.
struct DefaultedAudioClip {
    OpRequest request;
    std::int64_t lengthTicks;
};
[[nodiscard]] DefaultedAudioClip prepareDefaultedAudioClip(Store &, std::int64_t id, std::int64_t track,
                                                          std::int64_t mediaId, const std::string &name,
                                                          std::int64_t start, const library::SampleSettings &,
                                                          double referenceRate);
} // namespace adi::clip_edit
