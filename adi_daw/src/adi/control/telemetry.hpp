// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The telemetry board: what the audio engine already publishes, as one
// immutable frame a control caller can read (docs/CONTROL-MCP.md §4.1, §7.3).
//
// The audio thread is not touched. It already stores, in relaxed atomics, the
// sample peak and RMS of each strip for the current callback (StripMeter) and
// Main's LUFS and held true peak (MainLoudnessReadout); the UI reads the same
// atomics every frame. `sample()` performs those relaxed loads on the message
// thread at the UI clock, and the board swaps in the result. ADR-0050 §4's
// short list of audio-to-UI paths does not grow (MX-30 rejects a fourth).
//
// Both sides of the board are ordinary threads -- the sampler on the message
// thread, readers on network threads -- so it is a mutex around one pointer
// copy. Lock-freedom is what the AUDIO boundary needs, and it has it.
//
// Honesty is part of the contract: per-strip values are the latest callback
// only (not held, so a poll cannot prove a strip never clipped), the four
// loudness readouts are independent reads (not a coherent set), and every
// Mix Radar input that does not exist yet is listed as unavailable with the
// milestone that brings it.

#pragma once

#include "adi/control/jsonrpc.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace adi::engine {
struct StripMeter;
struct MainLoudnessReadout;
}  // namespace adi::engine

namespace adi::control {

struct StripReading {
    std::int64_t track = 0;
    float peak = 0.0f;   ///< linear sample peak, post-fader, latest callback
    float rms = 0.0f;    ///< linear RMS over all channels, latest callback
};

struct LoudnessReading {
    float momentaryLufs = 0.0f;   ///< -inf: silence, or not warmed up
    float shortTermLufs = 0.0f;
    float integratedLufs = 0.0f;
    float truePeakDb = 0.0f;      ///< held until reset
};

struct TransportReading {
    bool playing = false;
    std::int64_t positionSamples = 0;
    double sampleRate = 0.0;
};

struct TelemetryFrame {
    std::uint64_t sequence = 0;
    std::int64_t capturedMs = 0;   ///< steady clock
    std::optional<TransportReading> transport;
    std::vector<StripReading> strips;
    std::optional<LoudnessReading> main;
};

/// Where the sampler reads. The app host (CTL-09) builds it from
/// Session::strips() and Session::mainLoudness() when the graph changes; the
/// pointers must outlive the sampling, which the session's strips do (a
/// departed track's strip is kept, silent, until the session ends).
struct MeterTaps {
    std::vector<std::pair<std::int64_t, const engine::StripMeter*>> strips;
    const engine::MainLoudnessReadout* main = nullptr;
    std::function<TransportReading()> transport;
};

/// Message thread, at the UI clock: relaxed loads only, then one allocation.
[[nodiscard]] std::shared_ptr<const TelemetryFrame> sample(const MeterTaps& taps,
                                                           std::uint64_t sequence,
                                                           std::int64_t nowMs);

class TelemetryBoard {
public:
    void publish(std::shared_ptr<const TelemetryFrame> frame);
    [[nodiscard]] std::shared_ptr<const TelemetryFrame> latest() const;

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const TelemetryFrame> frame_;
};

/// The resource's JSON (docs/CONTROL-MCP.md §7.3). `frame` null with `live`
/// false is the headless answer: no audio engine runs here.
[[nodiscard]] Json telemetryJson(const TelemetryFrame* frame, std::int64_t nowMs, bool live);

}  // namespace adi::control
