// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/render/queue.hpp"
namespace adi::render {
struct FreezeCaptureSpec {
    std::vector<std::int64_t> tracks;
    std::int64_t endSample = 0; // exclusive, from sample zero, includes explicit tail
    engine::SessionSpec session;
    audio::WavFormat format = audio::WavFormat::Float32;
    std::uint64_t documentGeneration = 0; // owner identity captured when the job starts
    bool wholeGroups = false;
    bool acknowledgeChildAudioRoutes = false;
};
struct FrozenStreamCapture {
    std::int64_t track = 0;
    std::filesystem::path preFx, postFx;
    std::int64_t preFxLatency = 0, postFxLatency = 0;
    engine::OfflineSourceInfo source;
    std::string fingerprint; // from the immutable backup, before any progress callback
    bool wholeGroup = false;
    std::vector<engine::OfflineSourceInfo> contributors{};
};
class FreezeAudioCapture {
  public:
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const std::string &error() const noexcept {
        return error_;
    }
    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] std::uint64_t sourceGeneration() const noexcept;
    [[nodiscard]] std::stop_token stopToken() const noexcept;
    [[nodiscard]] const std::vector<FrozenStreamCapture> &streams() const noexcept;
    [[nodiscard]] const std::filesystem::path &directory() const noexcept;
    [[nodiscard]] const std::string &sourceFile() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> sourceHead() const noexcept;

  private:
    struct Data;
    std::shared_ptr<Data> data_;
    std::string error_;
    bool cancelled_ = false;
    friend FreezeAudioCapture captureFreezeAudio(Store &, FreezeCaptureSpec, engine::DeviceLoader, Control);
};
// Document owner/worker only, not the audio driver. One immutable SQLite backup
// (including WAL), one loaded Session, one processing pass for all tap streams.
// Private sources remain owned by this value for publication/recovery; no
// project operation is committed and no public destination is replaced.
[[nodiscard]] FreezeAudioCapture captureFreezeAudio(Store &, FreezeCaptureSpec, engine::DeviceLoader = {},
                                                    Control = {});
} // namespace adi::render
