// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// MX-39 (ADR-0203 d8, ADR-0166 d8, ADR-0254 d23): auto gain-match for any
// device, and Match Output Gain.
//
// Match: the host's matcher (dsp::AutoGain, the one ADR-0166 built) holds the
// device's output at its input's loudness, so the device switched on and off
// compares at equal loudness. Fixed: Match Output Gain has committed the
// matcher's gain at one moment as a plain fixed gain, and the matcher stops.
// Off: nothing is touched, so a device whose flag is off is bit-identical to
// one with no matcher at all.
//
// The measured input is the device's own input as it is handed to the device.
// It is not delayed by the device's latency: the matcher's windows are a second
// long, so a latency of a few milliseconds moves the measurement by that much
// of a second and no more.
//
// Message thread: set, prepare. Audio thread: apply. No allocation, no lock.
#include "adi/dsp/auto_gain.hpp"
#include "adi/engine/graph.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
namespace adi::engine {
class OutputMatch {
  public:
    enum class Mode : int { Off, Match, Fixed };
    static constexpr int kMaxChannels = 16;   ///< channels past this pass unmatched

    /// Message thread. `fixedDb` is the committed gain, used in Fixed only.
    void set(Mode mode, double fixedDb = 0.0) noexcept {
        const double db = std::isfinite(fixedDb) ? std::clamp(fixedDb, -dsp::AutoGain::kMaxDb, dsp::AutoGain::kMaxDb) : 0.0;
        fixedDb_.store(db, std::memory_order_relaxed);
        fixed_.store(std::pow(10.0, db / 20.0), std::memory_order_relaxed);
        mode_.store(mode, std::memory_order_release);
    }
    [[nodiscard]] Mode mode() const noexcept { return mode_.load(std::memory_order_acquire); }
    [[nodiscard]] double fixedDb() const noexcept { return fixedDb_.load(std::memory_order_relaxed); }
    /// The matcher's gain as the last block applied it, in dB: what Match
    /// Output Gain commits. 0 before the matcher has run.
    [[nodiscard]] double matchedDb() const noexcept { return matchedDb_.load(std::memory_order_relaxed); }
    /// Message thread: the rate the audio thread prepares the matcher at.
    void prepare(double sampleRate) noexcept {
        rate_.store(sampleRate > 0.0 ? sampleRate : 48000.0, std::memory_order_release);
    }

    /// Audio thread, after the device wrote `io.out` (or `io.out64`).
    void apply(const NodeIo& io) noexcept {
        const Mode mode = mode_.load(std::memory_order_acquire);
        if (mode != running_) {
            // A fresh match measures from its own first block, as ADR-0166's
            // windows start together from zero.
            if (mode == Mode::Match) matcher_.reset();
            running_ = mode;
        }
        if (mode == Mode::Off || io.frames <= 0 || io.channels <= 0) return;
        const double rate = rate_.load(std::memory_order_acquire);
        if (rate != preparedRate_) {
            matcher_.prepare(rate);
            preparedRate_ = rate;
        }
        if (io.wide() && io.out64 != nullptr) run(io.in64, io.out64, io, mode);
        else if (io.out != nullptr) run(io.in, io.out, io, mode);
    }

  private:
    template <class T>
    void run(const T* const* in, T* const* out, const NodeIo& io, Mode mode) noexcept {
        const int channels = std::min(io.channels, kMaxChannels);
        const auto offset = static_cast<std::size_t>(std::max(0, io.blockOffset));
        std::array<const T*, kMaxChannels> dry{};
        std::array<T*, kMaxChannels> wet{};
        bool measured = in != nullptr;
        for (int c = 0; c < channels; ++c) {
            const auto at = static_cast<std::size_t>(c);
            wet[at] = out[c] != nullptr ? out[c] + offset : nullptr;
            dry[at] = in != nullptr && in[c] != nullptr ? in[c] + offset : nullptr;
            if (wet[at] == nullptr) return;   // nothing written to scale
            measured = measured && dry[at] != nullptr;
        }
        if (mode == Mode::Match && measured) {
            matcher_.process(dry.data(), wet.data(), channels, io.frames);
            matchedDb_.store(matcher_.gainDb(), std::memory_order_relaxed);
            return;
        }
        // Fixed, or a block with no input to measure: the gain held.
        const double gain = mode == Mode::Fixed ? fixed_.load(std::memory_order_relaxed)
                                                : std::pow(10.0, matcher_.gainDb() / 20.0);
        for (int c = 0; c < channels; ++c) {
            T* w = wet[static_cast<std::size_t>(c)];
            for (int i = 0; i < io.frames; ++i) w[i] = static_cast<T>(static_cast<double>(w[i]) * gain);
        }
    }

    dsp::AutoGain matcher_;
    std::atomic<Mode> mode_{Mode::Off};
    std::atomic<double> fixed_{1.0}, fixedDb_{0.0}, matchedDb_{0.0}, rate_{48000.0};
    Mode running_ = Mode::Off;      // audio thread
    double preparedRate_ = 0.0;     // audio thread
};
} // namespace adi::engine
