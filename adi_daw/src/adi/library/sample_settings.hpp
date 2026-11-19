// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace adi::library {
// Authored per-sample defaults, never rebuildable analysis/cache data. Placement,
// track/media/clip ids, device automation and project selection are not saved.
struct SampleSettings {
    std::uint32_t sampleRate=0,channels=0;
    std::uint64_t frames=0,sourceStart=0,sourceLength=0;
    bool warpEnabled=false,reverse=false,loopEnabled=false,muted=false;
    std::string warpMode="none";
    // Original AWRP bytes, including readable future record tails/version.
    std::optional<std::vector<std::byte>> warpMarkers;
    double transposeSemis=0,formantShift=0,gainDb=0;
    std::int64_t channelMode=0,contentOffsetTicks=0,loopStartTicks=0;
    std::optional<std::int64_t> loopLengthTicks,endMarkerTicks;
    std::int64_t fadeInTicks=0,fadeOutTicks=0,fadeInCurve=1,fadeOutCurve=1;
    std::optional<double> fadeInTension,fadeOutTension;
    std::optional<std::int64_t> contentOffsetNs,loopBeginNs,loopEndNs,sourceEndNs;
    std::array<std::optional<std::int64_t>,4> fadeClockNs{},fadeClockTicks{};
    std::optional<std::int64_t> signatureNumerator,signatureDenominator;
    friend bool operator==(const SampleSettings&,const SampleSettings&)=default;
};
enum class SampleSettingsProblem {None,Missing,Realtime,Invalid,Unsupported,Resource,Database};
struct SampleSettingsResult {
    SampleSettingsProblem problem=SampleSettingsProblem::None;
    std::string error;
    std::optional<SampleSettings> data;
    explicit operator bool()const noexcept{return problem==SampleSettingsProblem::None && data.has_value();}
};
inline constexpr std::size_t kMaxSampleWarpBytes=32*1024*1024;
// Owner-worker only, no SQL/I/O. Exact JSON types/fields; no lossy number/string
// coercion. The native AWRP parser validates coverage/order against sourceLength.
// Unsupported version/fields refuse editing; a library keeps its original JSON.
[[nodiscard]] SampleSettingsProblem validateSampleSettings(const SampleSettings&,std::string& error);
[[nodiscard]] nlohmann::json encodeSampleSettings(const SampleSettings&);
[[nodiscard]] SampleSettingsResult decodeSampleSettings(const nlohmann::json&);
} // namespace adi::library
