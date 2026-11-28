// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/media/peaks.hpp"
#include "adi/ops.hpp"
#include "juce/pd_samples.hpp"
#include <array>
#include <span>
namespace adi::device {
inline constexpr char kPresetSampleStateHint[] = "adi.device-preset/embedded-sample";
inline constexpr std::array<std::size_t, 5> kColorCabSizes{64, 128, 256, 512, 1024};
inline constexpr std::size_t kColorCabDefaultSize = kColorCabSizes[2];
inline constexpr std::size_t builtinSampleMaxFrames=8u*1024u*1024u;
/// Whether a source of this many frames and channels fits a built-in's sample
/// state: its float backup holds at most builtinSampleMaxFrames values, which
/// both restore and the host's 40 MiB state limit accept. A stereo source
/// therefore holds at most 4M frames (DEV-21).
[[nodiscard]] constexpr bool builtinSampleStateFits(std::uint64_t frames,unsigned channels) noexcept {
    return channels>0 && frames>0 && frames<=builtinSampleMaxFrames/channels;
}
struct BuiltinSampleInfo {
    std::string uid,mediaHash;
    std::int32_t slot=0;
    bool empty=true;
    std::size_t colorCabSize=0;
};
[[nodiscard]] Payload emptyBuiltinSample(std::string_view uid,std::int32_t slot);
// Off-thread only. Serialized source backup allows re-preparation at another
// engine rate without the original file. No media bytes enter an op payload.
[[nodiscard]] Payload captureBuiltinSample(std::string_view uid,std::int32_t slot,
    const std::filesystem::path& source,const std::filesystem::path& playable,double rate,
    std::size_t colorCabSize=kColorCabDefaultSize);
[[nodiscard]] BuiltinSampleInfo inspectBuiltinSample(const Payload&);
// Rebuild from the embedded source without reopening the original file.
[[nodiscard]] Payload resizeColorCabSample(Payload, std::size_t size, double rate);
// Preview the original-rate backup in a portable, fileless sample preset off the owner thread.
[[nodiscard]] media::PeakResult buildBuiltinSamplePeaks(const Payload&, std::stop_token);
[[nodiscard]] std::unique_ptr<PdSampleBuffer> restoreBuiltinSample(const Payload&,double rate);
}
