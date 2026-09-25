// SPDX-License-Identifier: GPL-3.0-or-later
//
// Auto gain: an effect's output held at its input's loudness — ADR-0166.
//
// A saturator, tape or tube stage gets louder as it is driven, and louder
// sounds better to everyone, so the drive knob is judged by its level, not its
// tone. Auto gain takes the level out of the judgement: it measures the effect's
// input and output the same way and scales the output by the ratio.
//
// THREE CHOICES, each against the simpler thing:
//
//   1. **Loudness, not RMS.** Both sides are K-weighted (ITU-R BS.1770: a
//      +4 dB shelf above ~1.7 kHz and a high-pass at ~38 Hz) before they are
//      measured. Saturation adds upper harmonics; plain RMS barely sees them,
//      the ear does, and an RMS match leaves the driven signal sounding louder.
//
//   2. **The same slow window on both sides, never the ratio's own.** Each side
//      is a one-pole mean square with a 1 s time constant, started from zero
//      together, so their ratio is the gain over the same second of audio from
//      the first block on. A fast window would follow the envelope and undo
//      what a compressor did; a smoothed RATIO would lag behind level changes
//      that both sides share.
//
//   3. **Silence and muting freeze the measurement, not just the gain.** When
//      either side falls silent -- a clip's end, or an effect that mutes while
//      its input plays on (a gate, a kill switch) -- there is no level change
//      to match. Holding only the GAIN is not enough: the silent side's window
//      keeps draining while the other's does not, and when the sound returns
//      the ratio is tens of dB wrong and the first note comes out as a burst.
//      So every 32 frames the chunk itself is judged: it is live when both
//      sides are above -80 dB and within 48 dB of each other (twice what the
//      gain can correct). Only live chunks enter the 1 s windows; anything else
//      leaves them, and the gain, exactly where they were. The judgement is the
//      chunk's own energy, not a meter's: a meter lags a mute by as long as it
//      takes to decay, and the window drains for all of that time. A chunk
//      judged dead that was not -- a delay's output out of phase with its
//      input for 0.7 ms -- only skips one update.
//
// The gain is limited to ±24 dB, is recomputed every 32 frames and ramps
// linearly between, like the mixer strip (ADR-0163). Zero latency: the
// measurement runs alongside the audio, never ahead of it. No allocation.

#pragma once

#include "adi/dsp/biquad.hpp"

namespace adi::dsp {

class AutoGain {
public:
    static constexpr int kMaxChannels = 2;     // measured; more are scaled alike
    static constexpr int kControlFrames = 32;
    static constexpr double kMaxDb = 24.0;
    static constexpr double kWindowSeconds = 1.0;
    static constexpr double kGateDb = -80.0;
    static constexpr double kLiveSpanDb = 48.0;

    void prepare(double sampleRate) noexcept;

    /// Forget the measurement: unity gain, both windows empty.
    void reset() noexcept;

    /// Off ramps to unity and keeps measuring, so turning it back on is right
    /// at once instead of a second later.
    void setEnabled(bool on) noexcept { enabled_ = on; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    /// Audio thread. `dry` is the effect's input, as it was before the effect
    /// ran; `wet` is its output, scaled in place.
    void process(const float* const* dry, float* const* wet, int channels,
                 int frames) noexcept;

    /// The gain being applied, in dB (the end of the last ramp).
    [[nodiscard]] double gainDb() const noexcept;

    /// The K-weighting both sides pass through, for the sample rate.
    [[nodiscard]] static Biquad kShelf(double fs) noexcept;
    [[nodiscard]] static Biquad kHighPass(double fs) noexcept;

private:
    struct Stage {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    };
    struct Side {
        Stage shelf[kMaxChannels];
        Stage hp[kMaxChannels];
        double meanSquare = 0.0;   // the 1 s window, fed only live chunks
        double energy[kControlFrames] = {};
    };
    /// K-weights `count` frames into side.energy; returns their mean.
    double weigh(Side& side, const float* const* x, int channels, int from,
                 int count) noexcept;
    void feed(Side& side, int count) const noexcept;

    double sr_ = 48000.0;
    Biquad shelf_{};
    Biquad hp_{};
    double k_ = 0.0;           // one-pole coefficient for the window
    Side dry_{};
    Side wet_{};
    double gain_ = 1.0;
    bool enabled_ = true;
};

}  // namespace adi::dsp
