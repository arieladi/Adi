// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/wav_file.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/track_recording.hpp"
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui {
inline constexpr std::string_view kResamplingInput = "resampling";

// An enabled external input. Its reference stores physical channel numbers,
// and its channels are indices in the current driver's packed input buffer.
struct ExternalAudioInput {
    std::string ref, label;
    std::vector<std::uint32_t> channels;
};

inline const ExternalAudioInput *resolveExternalAudioInput(std::span<const ExternalAudioInput> inputs,
                                                          const std::optional<std::string> &ref) noexcept {
    const std::string_view wanted = ref ? std::string_view(*ref) : "ext:stereo:1";
    const auto found = std::find_if(inputs.begin(), inputs.end(),
                                    [wanted](const ExternalAudioInput &input) { return input.ref == wanted; });
    if (found != inputs.end())
        return &*found;
    return ref || inputs.empty() ? nullptr : &inputs.front();
}

inline audio::WavFormat recordingWavFormat(int bitDepth) noexcept {
    switch (bitDepth) {
    case 16: return audio::WavFormat::Pcm16;
    case 24: return audio::WavFormat::Pcm24;
    default: return audio::WavFormat::Float32;
    }
}

// A track's recording options (#397, N31; MR V.2 §9; Live 12 manual 18.8): Rec-Q, the quantize new
// recordings get ([1/16] or [Free]), and Play-Q, the live quantization of what is played, on a MIDI
// track's header; Keep Monitoring Latency in Recorded Audio, from a right-click on the In or Auto
// monitor button. Each change is one op, one undo step, from the snapshot the command lands on.
//
// Rec-Q takes effect when a take lands, in its own Undo step. Play-Q delays monitored live MIDI
// while the transport runs, with the enabled state highlighted to show the added latency.
inline constexpr std::array<const char *, 4> kQuantizeGrids{"1/4", "1/8", "1/16", "1/32"};
// What Rec-Q shows: its grid, or Free when off. Play-Q: its grid, or Off.
inline std::string recordQuantizeText(const TrackRecordingOptions &o) { return o.recordEnabled ? o.recordGrid : "Free"; }
inline std::string playQuantizeText(const TrackRecordingOptions &o) { return o.playEnabled ? o.playGrid : "Off"; }
// Rec-Q or Play-Q set to `choice`: "off" or a grid. The same value is no edit.
inline TrackEdit setQuantize(const engine::TrackNode &track, bool record, std::string_view choice) {
    TrackEdit edit;
    const auto &o = track.recording;
    const bool on = choice != "off";
    const auto &grid = record ? o.recordGrid : o.playGrid;
    const bool enabled = record ? o.recordEnabled : o.playEnabled;
    if (enabled == on && (!on || grid == choice))
        return edit;
    Payload payload{{"id", track.id}, {"on", on}};
    if (on)
        payload["grid"] = std::string(choice);
    edit.ops.push_back(track_edits_detail::op(record ? "track.setRecordQuantize" : "track.setPlayQuantize", std::move(payload),
                                              record ? "Change Record Quantization" : "Change Live Quantization"));
    return edit;
}
inline TrackEdit setKeepMonitoringLatency(const engine::TrackNode &track, bool on) {
    TrackEdit edit;
    if (track.recording.keepMonitoringLatency != on)
        edit.ops.push_back(track_edits_detail::op("track.setKeepMonitoringLatency", {{"id", track.id}, {"on", on}},
                                                  "Keep Monitoring Latency"));
    return edit;
}
} // namespace adi::ui
