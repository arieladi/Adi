// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/process.hpp"
#include "adi/media/peaks.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace adi::engine {

// One application-owned one-shot audition. The owner requests files and reads
// the waveform/status. One audio driver may mix at a time. Stop the driver
// before destroying the player. Decoding and waveform scans are worker-only.
class SamplePreview {
  public:
    enum class Phase { Idle, Loading, Playing, Finished, Failed };
    struct Waveform {
        std::filesystem::path path;
        std::uint32_t sampleRate = 0;
        std::uint64_t frames = 0, framesPerBin = 1;
        std::vector<media::PeakBin> bins;
    };
    struct Status {
        Phase phase = Phase::Idle;
        double position = 0;
        std::filesystem::path playingPath;
        std::shared_ptr<const Waveform> waveform;
        std::string error;
    };

    SamplePreview();
    ~SamplePreview();
    SamplePreview(const SamplePreview &) = delete;
    SamplePreview &operator=(const SamplePreview &) = delete;

    // Owner thread. An empty selection clears its waveform. Playback and
    // selection are independent, so a manual audition need not change a list.
    void select(std::filesystem::path);
    void play(std::filesystem::path, double fraction = 0.);
    void stop();
    [[nodiscard]] Status status() const;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] static bool canOutput(const AudioIo &, int firstChannel) noexcept;
    // Audio thread. Adds to a complete physical stereo pair, never to inputs or a graph
    // tap. No allocation, locks, decoding or filesystem access here.
    void mix(const AudioIo &, double deviceRate, int firstChannel, double gain) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace adi::engine
