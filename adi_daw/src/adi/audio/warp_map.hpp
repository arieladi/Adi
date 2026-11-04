// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/blob.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
namespace adi {
/// AWRP v1, 16 bytes: source-window frame followed by source musical tick.
struct WarpMarker {
    std::int64_t sourceFrame=0,tick=0;
    bool operator==(const WarpMarker&)const=default;
};
static_assert(sizeof(WarpMarker)==16 && offsetof(WarpMarker,sourceFrame)==0 && offsetof(WarpMarker,tick)==8);
template<>struct StreamTraits<WarpMarker>{static constexpr std::array<std::size_t,1> released={16};};
namespace audio {
enum class WarpMapError {None,Realtime,Header,Count,Bounds,Order,Coverage};
struct WarpMapRead {
    WarpMapError error=WarpMapError::None;
    bool futureLayout=false;
    std::vector<WarpMarker> markers;
    [[nodiscard]] explicit operator bool()const noexcept{return error==WarpMapError::None;}
};
struct WarpMapWrite {
    WarpMapError error=WarpMapError::None;
    std::vector<std::byte> bytes;
    [[nodiscard]] explicit operator bool()const noexcept{return error==WarpMapError::None;}
};
/// Off-callback. Canonical maps cover 0..sourceFrames with an origin at tick
/// zero and strictly increasing frame/tick coordinates. Future record tails
/// are skipped when reading; keep their original BLOB for a later save/edit.
[[nodiscard]] WarpMapRead readWarpMap(std::span<const std::byte>,std::uint64_t sourceFrames);
[[nodiscard]] WarpMapWrite writeWarpMap(std::span<const WarpMarker>,std::uint64_t sourceFrames);
}
}
