// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mixer strip in the graph: a track's volume, pan and mute, and solo
// across the project (ADR-0163).
//
// ADR-0077 shaped the junction for the strip and left two things open: where
// the gain attaches, and the pan law and the global nature of solo. This is
// the answer.
//
//   * WHERE. A StripNode is the last node of every track's chain, after its
//     devices, as Live's mixer section follows the device chain. Whatever the
//     track feeds -- its group, the master -- takes the strip's output.
//   * WHO OWNS IT. The session, not the graph. Every edit publishes a new graph
//     (ADR-0092), a fader move included, and a strip that died with its graph
//     would jump to its new gain at every edit instead of ramping to it. One
//     strip per track id survives every rebuild, as device nodes do (ADR-0122),
//     and only one graph renders a callback, so only one audio thread runs it.
//   * HOW A CHANGE ARRIVES. The message thread sets a target; the audio thread
//     ramps to it over five milliseconds. A mute is a ramp to zero, so it does
//     not click.

#pragma once

#include "adi/engine/graph.hpp"
#include "adi/engine/plan.hpp"
#include "adi/store_rows.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace adi::engine {

/// `mixer_strip.pan_law` (SPEC §6.9, ADR-0163). Any other value reads as Live
/// and is named in the session's problems.
enum class PanLaw : std::int64_t {
    /// Live 12's Stereo Pan: turning towards a side raises that channel and
    /// lowers the other (manual §18). Constant power with the centre at unity,
    /// so a hard side is +3 dB. The default, for parity (ADR-0108); the +3 dB
    /// waits on a side-by-side measurement (docs/AWAITING.md).
    Live = 0,
    /// Constant power, -3 dB at the centre and unity at a hard side: Cubase's
    /// Equal Power.
    EqualPower = 1,
    /// A balance control: the near channel stays at unity and the far one falls
    /// linearly to silence.
    Balance = 2,
    /// Linear, -6 dB at the centre.
    Linear = 3,
};

struct StereoGain {
    float left = 1.0f;
    float right = 1.0f;
};

/// The gains a stereo signal takes at `pan` (-1 hard left, +1 hard right;
/// clamped) under `law`. Pure; the audio thread never calls it.
[[nodiscard]] StereoGain panGains(double pan, PanLaw law) noexcept;

/// A fader's linear gain. At or below kSilenceDb, and for -inf, it is 0.
inline constexpr double kSilenceDb = -150.0;
[[nodiscard]] double faderGain(double volumeDb) noexcept;

class StripNode final : public Node {
public:
    /// Message thread. `left` and `right` are the gains channels 0 and 1 take;
    /// `other` is what any further channel takes -- the fader alone, since a
    /// pan has no meaning past two channels.
    void setTarget(float left, float right, float other) noexcept;

    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void process(const NodeIo& io) noexcept override;

    /// Zero, like MixNode's: a strip holds nothing back.
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "strip"; }

    /// The gains now set as the target. For tests.
    [[nodiscard]] float targetLeft() const noexcept { return targetL_.load(std::memory_order_relaxed); }
    [[nodiscard]] float targetRight() const noexcept { return targetR_.load(std::memory_order_relaxed); }

    static constexpr double kRampSeconds = 0.005;

private:
    // Written by the message thread, read at the top of each segment.
    std::atomic<float> targetL_{1.0f}, targetR_{1.0f}, targetOther_{1.0f};

    // Audio thread only. A strip begins AT its target, not ramping up to it
    // from silence: a project that opens must not fade in.
    bool started_ = false;
    float cur_[3] = {1.0f, 1.0f, 1.0f};
    float step_[3] = {0.0f, 0.0f, 0.0f};
    float rampTo_[3] = {1.0f, 1.0f, 1.0f};
    std::int32_t remaining_ = 0;
    std::int32_t rampFrames_ = 240;
};

/// Which tracks are heard (ADR-0163 d4). A muted track is silent. When any
/// track is soloed, a track is heard only if it is soloed, solo-defeated,
/// feeds a soloed track (a group's children), or is fed by one (its group, the
/// master) -- over main routes; a sidechain key keeps nothing audible.
[[nodiscard]] std::map<std::int64_t, bool> audibleTracks(const rows::Model&, const GraphPlan&);

/// One strip per track id, kept for the life of the session. Message thread.
class MixerStrips {
public:
    /// Recompute every target from the model: fader, pan, law, mute and solo.
    /// Creates strips for new tracks; a departed track's strip is kept, silent,
    /// until the session ends, as a departed device is (ADR-0122), since a
    /// retired graph may still name it.
    void sync(const rows::Model&);

    /// The strip for a track the plan realises, or null.
    [[nodiscard]] StripNode* stripFor(std::int64_t trackId) noexcept;

    /// Rows the strip could not honour, named: an unknown pan law, a volume or
    /// pan that is not finite.
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }

private:
    std::map<std::int64_t, std::unique_ptr<StripNode>> strips_;
    std::vector<std::string> problems_;
};

}  // namespace adi::engine
