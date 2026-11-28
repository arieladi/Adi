// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/audio/warp.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace adi::clip_edit { struct ClipPitchEdits; }
namespace adi::audio { class PitchComparePcm; }

namespace adi::media {

struct ClipPitchRenderPlan {
    std::filesystem::path source, workFolder;
    std::shared_ptr<const clip_edit::ClipPitchEdits> edits;
    std::uint64_t sourceStart = 0, sourceFrames = 0;
    bool reverse = false;
    // The caller projects this recipe using the original clip clock. The
    // pitch worker substitutes its staged source window before executing it.
    std::optional<audio::WarpRenderPlan> warp;
    // Effective frame rate for unwarped comparison. Zero uses the WAV rate.
    std::uint32_t playbackRate = 0;
};

enum class ClipPitchRenderProblem { None, Realtime, Cancelled, Source, Edits, Unsupported, Resource, Io, Dsp };

/// Owns worker-prepared PCM. Close downstream readers before releasing the
/// last reference, and keep all acquisition and retirement off the callback.
struct PreparedClipPitch {
    const std::filesystem::path file;
    const std::uint32_t rate;
    const std::uint16_t channels;
    const std::uint64_t frames;
    ~PreparedClipPitch();
    PreparedClipPitch(const PreparedClipPitch&) = delete;
    PreparedClipPitch& operator=(const PreparedClipPitch&) = delete;

private:
    PreparedClipPitch(std::filesystem::path, std::uint32_t, std::uint16_t, std::uint64_t);
    PreparedClipPitch(std::filesystem::path, std::shared_ptr<const audio::PreparedWarp>);
    const std::filesystem::path stageDirectory_, stageFile_;
    std::shared_ptr<const audio::PreparedWarp> warp_;
    friend struct ClipPitchPreparation;
};

struct ClipPitchRenderResult {
    ClipPitchRenderProblem problem = ClipPitchRenderProblem::None;
    std::optional<std::string> detail;
    std::shared_ptr<const PreparedClipPitch> prepared;
    explicit operator bool() const noexcept { return problem == ClipPitchRenderProblem::None && prepared != nullptr; }
};

class AnalysisCache;

/// Rebuilds optional analysis on a miss. Overrides allow independent worker
/// caches in tests. A null decode selects the process decoder and a null analysis
/// disables analysis persistence. A failed render leaves no prepared PCM.
[[nodiscard]] ClipPitchRenderResult prepareClipPitchRender(const ClipPitchRenderPlan&, std::stop_token = {});
[[nodiscard]] ClipPitchRenderResult prepareClipPitchRender(const ClipPitchRenderPlan&, std::stop_token,
                                                           audio::DecodeCache*, AnalysisCache*);

struct ClipPitchCompareResult {
    ClipPitchRenderProblem problem = ClipPitchRenderProblem::None;
    std::optional<std::string> detail;
    std::shared_ptr<const audio::PitchComparePcm> prepared;
    explicit operator bool() const noexcept { return problem == ClipPitchRenderProblem::None && prepared != nullptr; }
};

/// Opt-in worker comparison, from a single verified source capture. Both
/// sides have the same trim, direction and optional project Warp clock. The
/// original bypasses all segment edits. Unwarped comparison uses playbackRate
/// or the native WAV rate, and converts both sides to outputRate when supplied.
/// Warped comparison uses the captured plan's clock. A supplied outputRate
/// must equal that clock's rate.
[[nodiscard]] ClipPitchCompareResult prepareClipPitchCompare(const ClipPitchRenderPlan&, std::stop_token = {},
                                                             std::uint32_t outputRate = 0);
[[nodiscard]] ClipPitchCompareResult prepareClipPitchCompare(const ClipPitchRenderPlan&, std::stop_token,
                                                             audio::DecodeCache*, AnalysisCache*,
                                                             std::uint32_t outputRate = 0);

class ClipPitchRenderTask {
public:
    ~ClipPitchRenderTask();
    ClipPitchRenderTask(const ClipPitchRenderTask&) = delete;
    ClipPitchRenderTask& operator=(const ClipPitchRenderTask&) = delete;
    // Null while pending. The result remains valid while the task is retained.
    [[nodiscard]] const ClipPitchRenderResult* result() const noexcept;

private:
    struct State;
    explicit ClipPitchRenderTask(std::shared_ptr<State>);
    const std::shared_ptr<State> state_;
    friend class ClipPitchRenderWorker;
};

/// Enqueues immutable preparation on a worker. Refuses the audio callback.
[[nodiscard]] std::shared_ptr<const ClipPitchRenderTask> requestClipPitchRender(const ClipPitchRenderPlan&);

} // namespace adi::media
