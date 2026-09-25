// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mixer strip in the graph: a track's volume, pan and mute, and solo
// across the project (ADR-0163), and the automation that moves them
// (ADR-0164).
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
//   * HOW A CHANGE ARRIVES. The message thread sets the model's values; the
//     audio thread turns them into gains and ramps to them over five
//     milliseconds. A mute is a ramp to zero, so it does not click.
//   * AUTOMATION (ADR-0164). A strip evaluates its own lanes -- volume, pan,
//     mute -- at the playhead, every 32 samples, through the same ramp. No event
//     has to travel to it, and a lane is followed whether the transport is
//     playing or parked. A lane the user has overridden (ADR-0162) is simply
//     not bound, and the model's value stands.

#pragma once

#include "adi/engine/automation.hpp"
#include "adi/engine/graph.hpp"
#include "adi/engine/plan.hpp"
#include "adi/engine/transport.hpp"
#include "adi/store_rows.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
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
/// clamped) under `law`. Pure and allocation-free.
[[nodiscard]] StereoGain panGains(double pan, PanLaw law) noexcept;

/// A fader's linear gain. At or below kSilenceDb, and for -inf, it is 0.
inline constexpr double kSilenceDb = -150.0;
[[nodiscard]] double faderGain(double volumeDb) noexcept;

/// The lanes that drive one strip, resolved against one compiled
/// AutomationProgram (ADR-0164). A null lane means the model's static value.
struct StripLanes {
    const AutomationLaneProgram* volume = nullptr;   ///< dB
    const AutomationLaneProgram* pan = nullptr;      ///< -1 .. +1
    const AutomationLaneProgram* mute = nullptr;     ///< 0.5 or more is muted
};

class StripNode final : public Node {
public:
    /// Message thread: the model's values. `audible` is mute and solo together.
    void setStatic(double volumeDb, double pan, PanLaw law, bool audible) noexcept;

    /// Message thread, before the graph that plays them is published: the lanes
    /// now driving this strip (null for none), and what keeps them alive. The
    /// realiser retains `keepAlive` with the graph (sourceLifetime), so a lane a
    /// retired graph may still read is never freed under it.
    void setLanes(const StripLanes* lanes, std::shared_ptr<void> keepAlive) noexcept;

    /// Where a lane is read. Driver-thread state, read at the block's origin as
    /// every source reads it (Transport's contract).
    void setTransport(const Transport* transport) noexcept { transport_ = transport; }

    [[nodiscard]] std::shared_ptr<void> sourceLifetime() const override { return keepAlive_; }

    /// Message thread: the track delay (ADR-0172), `mixer_strip.delay_samples`,
    /// in samples at the PROJECT's rate. Positive plays the track late,
    /// negative plays it early.
    ///
    /// It is played as latency, and the sign is the point. A strip that says
    /// it is D samples EARLIER than it is (latency -D) is delayed by D at the
    /// next sum by delay compensation, as if everything else were late
    /// (ADR-0058). One that says it is D samples later (latency +D) has every
    /// other path delayed by D, which is the only way to play a track early in
    /// real time. No new delay line, and a change is an ordinary latency
    /// change: retapped within the headroom, rebuilt past it (ADR-0079).
    void setDelay(std::int64_t projectSamples, std::int64_t projectRate) noexcept;

    /// Minus the delay, at the rate passed to `prepare`: see setDelay.
    [[nodiscard]] std::int32_t latencySamples() const noexcept override;

    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void process(const NodeIo& io) noexcept override;

    /// Zero, like MixNode's: a strip holds nothing back.
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "strip"; }

    static constexpr double kRampSeconds = 0.005;
    /// How often a bound lane is read: 1.5 kHz at 48 kHz, 3x ADR-0054's floor.
    static constexpr std::int32_t kControlFrames = 32;
    /// A track delay is limited to one second either way (ADR-0172).
    static constexpr double kMaxDelaySeconds = 1.0;

private:
    std::atomic<std::int64_t> delayProject_{0};
    std::atomic<std::int64_t> delayRate_{48000};
    double sampleRate_ = 48000.0;       ///< message thread, set by prepare
    struct Gains {
        float g[3] = {1.0f, 1.0f, 1.0f};
    };
    [[nodiscard]] Gains desired(const StripLanes* lanes, std::int32_t frame) noexcept;
    void retarget(const Gains& t) noexcept;

    // The model's values, written by the message thread.
    std::atomic<float> volumeDb_{0.0f}, pan_{0.0f};
    std::atomic<int> law_{0};
    std::atomic<bool> audible_{true};
    std::atomic<const StripLanes*> lanes_{nullptr};
    std::shared_ptr<void> keepAlive_;   ///< message thread only
    const Transport* transport_ = nullptr;

    // Audio thread only. The last inputs, so gains are recomputed only when one
    // changes; and the ramp. A strip begins AT its target, not ramping up to it
    // from silence: a project that opens must not fade in.
    double lastVol_ = 0.0, lastPan_ = 0.0;
    int lastLaw_ = 0;
    bool lastHeard_ = true;
    Gains lastGains_{};
    bool haveLast_ = false;
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

/// A strip's automatable parameters (ADR-0164), as `automation_lanes.param_ref`
/// names them for a track (SPEC §6.9).
enum class StripParam { Volume, Pan, Mute };

/// One compiled program's lanes, bound to strips. Immutable once built; the
/// strips and the graphs that play them share it.
struct StripAutomation {
    std::shared_ptr<const AutomationProgram> program;
    std::map<std::int64_t, StripLanes> byTrack;   ///< lanes not overridden
    /// Every strip lane in the program, overridden or not: what a user edit to
    /// that track's parameter would override.
    std::map<std::pair<std::int64_t, StripParam>, std::int64_t> laneFor;
    std::vector<std::string> problems;
};

/// Bind a program's track lanes to strips. A lane in `overridden` is known but
/// not bound. Named, never dropped: a lane for a parameter the strip does not
/// have, a volume or pan lane not in real units, a second lane for one
/// parameter, and every device lane (not played yet).
[[nodiscard]] std::shared_ptr<StripAutomation> bindStripAutomation(
    std::shared_ptr<const AutomationProgram> program, const std::set<std::int64_t>& overridden);

/// One strip per track id, kept for the life of the session. Message thread.
class MixerStrips {
public:
    /// Recompute every strip from the model -- fader, pan, law, mute and solo --
    /// and bind its lanes. Creates strips for new tracks; a departed track's
    /// strip is kept, silent, until the session ends, as a departed device is
    /// (ADR-0122), since a retired graph may still name it.
    void sync(const rows::Model&, const Transport*, const std::shared_ptr<StripAutomation>&);

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
