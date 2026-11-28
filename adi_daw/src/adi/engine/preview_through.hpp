// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// MX-40 (ADR-0254 d16): the Browser's preview heard through a track. A source
// at the track's junction, beside its input monitor (N65's seam), so the
// preview passes the track's devices and its mixer path; nothing is written
// and nothing records it (it is not the hardware input the recorder reads).
// While this node plays the preview, the cue monitor leaves it off Cue Out:
// one player, heard in one place.
#include "adi/engine/graph.hpp"
#include "adi/engine/sample_preview.hpp"
#include <algorithm>
#include <array>
#include <atomic>
namespace adi::engine {
class PreviewThroughTrack final : public Node {
  public:
    explicit PreviewThroughTrack(SamplePreview &player) noexcept : player_(player) {}
    [[nodiscard]] bool processesDouble() const noexcept override { return true; }
    [[nodiscard]] const char *name() const noexcept override { return "preview through a track"; }
    void process(const NodeIo &io) noexcept override {
        if (io.frames <= 0 || io.channels < 2) return;   // a stereo pair or nothing: Cue Out keeps it
        const auto offset = static_cast<std::size_t>(std::max(0, io.blockOffset));
        AudioIo pair;
        pair.frames = io.frames;
        pair.numOut = 2;
        std::array<float *, 2> narrow{};
        std::array<double *, 2> wide{};
        for (std::size_t c = 0; c < 2; ++c) {
            if (io.wide()) {
                wide[c] = io.out64[c] + offset;
                std::fill_n(wide[c], io.frames, 0.0);
            } else {
                narrow[c] = io.out[c] + offset;
                std::fill_n(narrow[c], io.frames, 0.0f);
            }
        }
        for (std::int32_t c = 2; c < io.channels; ++c) {
            if (io.wide()) std::fill_n(io.out64[c] + offset, io.frames, 0.0);
            else std::fill_n(io.out[c] + offset, io.frames, 0.0f);
        }
        if (io.wide()) pair.out64 = wide.data();
        else pair.out = narrow.data();
        // The node renders only in a live session (Session refuses the route
        // for an export), so the player is advanced here and nowhere else.
        player_.mix(pair, io.sampleRate, 0, 1.0);
        mixed_.store(true, std::memory_order_release);
    }
    /// Audio thread, once per callback after the graph ran: whether this node
    /// played the preview, and clear it for the next callback.
    [[nodiscard]] bool takeMixed() noexcept { return mixed_.exchange(false, std::memory_order_acq_rel); }

  private:
    SamplePreview &player_;
    std::atomic<bool> mixed_{false};
};
} // namespace adi::engine
