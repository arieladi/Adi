// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include "juce/pd_samples.hpp"
#include <span>
namespace adi::device {
inline constexpr std::size_t builtinSampleMaxFrames=8u*1024u*1024u;
struct BuiltinSampleInfo {
    std::string uid,mediaHash;
    std::int32_t slot=0;
    bool empty=true;
};
[[nodiscard]] Payload emptyBuiltinSample(std::string_view uid,std::int32_t slot);
// Off-thread only. Serialized source backup allows re-preparation at another
// engine rate without the original file. No media bytes enter an op payload.
[[nodiscard]] Payload captureBuiltinSample(std::string_view uid,std::int32_t slot,
    const std::filesystem::path& source,const std::filesystem::path& playable,double rate);
[[nodiscard]] BuiltinSampleInfo inspectBuiltinSample(const Payload&);
[[nodiscard]] std::unique_ptr<PdSampleBuffer> restoreBuiltinSample(const Payload&,double rate);
}
