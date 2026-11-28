// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <stop_token>
#include <string>
#include <vector>

namespace adi::audio {
class DecodeCache;
class WarpRenderCache;
enum class WarpMode { Beats,Tones,Texture,RePitch,Complex,ComplexPro };
[[nodiscard]] const char* warpModeName(WarpMode)noexcept;
[[nodiscard]] std::optional<WarpMode> parseWarpMode(std::string_view)noexcept;
// Unwarped audio uses the transposed source clock at the existing sample-rate converter.
[[nodiscard]] double pitchedSampleRate(double sampleRate,double semitones)noexcept;
enum class TransientLoop { Off,Forward,PingPong };
struct WarpAnchor {
    std::uint64_t sourceFrame=0,outputFrame=0;
    bool operator==(const WarpAnchor&)const=default;
};
struct WarpRenderPlan {
    std::filesystem::path source,workFolder;
    std::uint64_t sourceStart=0,sourceFrames=0;
    std::uint32_t outputRate=48000;
    std::vector<WarpAnchor> anchors; // relative to source window / prepared output
    WarpMode mode=WarpMode::Beats;
    double transposeSemis=0,formantScale=1;
    double grainSeconds=0.065,fluctuation=0.25,transientEnvelope=1;
    TransientLoop transientLoop=TransientLoop::Forward;
    std::uint64_t seed=0;
    std::vector<std::uint64_t> transients;
};
enum class WarpError { None,Realtime,InvalidPlan,UnsupportedMode,Cancelled,Io,Process };
/// Owns a private prepared PCM file, never a project/media-pool file. Keep it
/// alive while a reader uses file. Destroy off the callback, after closing it.
struct PreparedWarp {
    const std::filesystem::path file;
    const std::uint32_t rate;
    const std::uint16_t channels;
    const std::uint64_t frames;
    ~PreparedWarp();
    PreparedWarp(const PreparedWarp&)=delete;
    PreparedWarp& operator=(const PreparedWarp&)=delete;
private:
    struct CachedFile {};
    PreparedWarp(std::filesystem::path,std::uint32_t,std::uint16_t,std::uint64_t);
    PreparedWarp(std::filesystem::path,std::uint32_t,std::uint16_t,std::uint64_t,CachedFile);
    const std::filesystem::path directory_;
    DecodeCache* cache_ = nullptr;
    friend struct WarpPreparation;
    friend class WarpRenderCache;
};
struct WarpResult {
    WarpError error=WarpError::None;
    std::string detail;
    std::shared_ptr<const PreparedWarp> prepared;
    [[nodiscard]] explicit operator bool()const noexcept{return error==WarpError::None && prepared!=nullptr;}
};
/// Worker/owner-thread only. Cooperative cancellation between bounded blocks;
/// private output is removed on failure/cancellation. No Store or live Session.
/// Output PCM is aligned offline, so its eventual paged reader adds no live
/// stretcher latency. A future live stretcher must declare its own PDC delay.
[[nodiscard]] WarpResult prepareWarp(const WarpRenderPlan&,std::stop_token={});
struct WarpPcmReader {
    void* context = nullptr;
    void (*read)(void*, std::uint64_t fileFrame, float* interleaved, std::uint32_t frames) = nullptr;
};
inline constexpr double kArrangementTonesGrainSeconds = 0.03;
/// Admit the exact-unity, same-rate offline subset with less than one complete
/// source window needed per page. Tones uses the default Arrangement grain.
[[nodiscard]] std::optional<std::uint32_t> reversedUnityR3PageFrames(const WarpRenderPlan&,
    std::uint32_t sourceRate,std::uint16_t channels) noexcept;
/// Worker only. Render a page from an admitted plan in playback-reverse order.
void renderReversedUnityR3Page(const WarpPcmReader&,std::uint64_t sourceStart,std::uint64_t sourceFrames,
                               std::uint32_t rate,WarpMode,std::uint16_t channels,
                               std::uint64_t first,std::span<float> output);


/// A shared worker result. An empty result means the render is pending.
/// Request and inspect from owner or disk workers only.
class WarpTask {
public:
    [[nodiscard]] std::shared_ptr<const WarpResult> result() const noexcept;
private:
    // The worker writes the result once, then publishes it to other threads.
    std::shared_ptr<const WarpResult> result_;
    std::atomic<bool> ready_{false};
    friend class WarpRenderCache;
};
[[nodiscard]] std::shared_ptr<const WarpTask> requestWarp(WarpRenderPlan plan);
} // namespace adi::audio
