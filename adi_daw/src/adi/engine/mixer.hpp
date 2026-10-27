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
#include "adi/engine/scope.hpp"
#include "adi/engine/plan.hpp"
#include "adi/engine/transport.hpp"
#include "adi/store_rows.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
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

/// The same gains unrounded, for a graph at 64 (ADR-0207 d2).
struct StereoGainWide {
    double left = 1.0;
    double right = 1.0;
};

/// The gains a stereo signal takes at `pan` (-1 hard left, +1 hard right;
/// clamped) under `law`. Pure and allocation-free. Each is `panGainsWide`'s
/// value rounded once to float, which is how it was always computed.
[[nodiscard]] StereoGain panGains(double pan, PanLaw law) noexcept;
[[nodiscard]] StereoGainWide panGainsWide(double pan, PanLaw law) noexcept;

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
    void silenceTaps(const NodeIo& io) noexcept override;

    /// Message thread: the next block starts AT the target, as a new strip
    /// does, rather than ramping from whatever this one last played.
    void restart() noexcept { restart_.store(true, std::memory_order_release); }

    /// ADR-0207 d2: at 64 the gains and their ramps are double too.
    [[nodiscard]] bool processesDouble() const noexcept override { return true; }

    /// Message thread: the scope tap this strip's output feeds (ADR-0175), or
    /// null. The tap outlives every graph: the session keeps it.
    void setTap(ScopeTap* tap) noexcept { tap_.store(tap, std::memory_order_release); }
    void setPreFaderTap(ScopeTap* tap) noexcept { preFaderTap_.store(tap, std::memory_order_release); }

    /// Zero, like MixNode's: a strip holds nothing back.
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "strip"; }

    static constexpr double kRampSeconds = 0.005;
    /// How often a bound lane is read: 1.5 kHz at 48 kHz, 3x ADR-0054's floor.
    static constexpr std::int32_t kControlFrames = 32;
    /// A track delay is limited to one second either way (ADR-0172).
    static constexpr double kMaxDelaySeconds = 1.0;

private:
    template <typename T>
    void processGains(const NodeIo& io, const T* const* in, T* const* out) noexcept;
    template <typename T>
    void writeTap(std::atomic<ScopeTap*>& slot, const NodeIo& io,
                  const T* const* audio) noexcept;

    std::atomic<ScopeTap*> tap_{nullptr};
    std::atomic<ScopeTap*> preFaderTap_{nullptr};
    std::atomic<std::int64_t> delayProject_{0};
    std::atomic<std::int64_t> delayRate_{48000};
    /// Set by prepare, which every graph that places this strip calls on the
    /// message thread while the graph before may still be playing it; so an
    /// atomic, as `rampFrames_` is, and never a plain write under a reader.
    std::atomic<double> sampleRate_{48000.0};
    /// Left, right, and every other channel (the fader alone). `g` is what a
    /// graph at 32 plays, rounded exactly as it always was; `w` the same
    /// values unrounded, for a graph at 64.
    struct Gains {
        float g[3] = {1.0f, 1.0f, 1.0f};
        double w[3] = {1.0, 1.0, 1.0};
    };
    [[nodiscard]] Gains desired(const StripLanes* lanes, std::int32_t frame) noexcept;

    /// The five-millisecond ramp toward a target, at one width. A strip runs
    /// one of the two in any graph. The strip outlives every graph, so at a
    /// precision switch (a new graph) the ramp about to run TAKES OVER from
    /// the one that just ran (`handover`): its own state is whatever it held
    /// when that width last played, and resuming it would replay an old gain.
    template <typename T>
    struct Ramp {
        T cur[3] = {1, 1, 1};
        T step[3] = {0, 0, 0};
        T to[3] = {1, 1, 1};
        std::int32_t remaining = 0;
        bool started = false;
        void retarget(const T (&t)[3], std::int32_t frames) noexcept;
        /// At the target now, no ramp: for a strip asleep, whose input is silent.
        void land(const T (&t)[3]) noexcept;
    };
    template <typename T>
    [[nodiscard]] Ramp<T>& ramp() noexcept;
    /// Audio thread: called with the width about to run; when it is not the
    /// width that ran last, carries the gain that was playing across.
    template <typename T>
    void handover() noexcept;

    // The model's values, written by the message thread.
    std::atomic<float> volumeDb_{0.0f}, pan_{0.0f};
    std::atomic<int> law_{0};
    std::atomic<bool> audible_{true};
    std::atomic<bool> restart_{false};
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
    Ramp<float> ramp32_;
    Ramp<double> ramp64_;
    std::int8_t lastWidth_ = 0;   ///< audio thread: 32 or 64 once a ramp has run, 0 before
    std::atomic<std::int32_t> rampFrames_{240};
};

/// A send's highest level. Live's top is not read yet (AWAITING 7(a)); this is
/// a guard, above any host's, so that a level no host offers -- +inf, or one
/// whose gain a float cannot hold -- plays as a level and not as infinity.
inline constexpr double kSendTopDb = 24.0;

/// A send (ADR-0203 d3): its level, its pan and its switch, between the tap on
/// its source track and the junction it feeds. The session owns one per
/// routing row and keeps it across rebuilds, as it keeps a strip, so a level
/// move ramps rather than jumps and both edges around it keep their history
/// (ADR-0092). The gain stage IS a strip's -- the same law, the same five
/// milliseconds, the same width handover -- so a send at 0 dB, centred and on,
/// is bit for bit its input at either precision.
class SendNode final : public Node {
public:
    /// Message thread. A level at or below kSilenceDb is silent, and so is
    /// one that is not a number; one past kSendTopDb, +inf included, plays at
    /// kSendTopDb (the caller names both). `on` is Enable Send.
    void set(double levelDb, double pan, PanLaw law, bool on) noexcept {
        const double db = std::isnan(levelDb) ? -std::numeric_limits<double>::infinity()
                                              : std::min(levelDb, kSendTopDb);
        gain_.setStatic(db, pan, law, on);
    }

    /// Message thread, for a Pre send: the source's mute lane (ADR-0164),
    /// which silences a send that leaves before the strip as the strip's own
    /// mute silences a Post one; null for none. `keepAlive` holds the lane's
    /// program, and the realiser keeps it with every graph that plays this
    /// node (sourceLifetime), so a retired graph never reads a freed lane.
    void setMuteLane(const AutomationLaneProgram* mute, std::shared_ptr<void> keepAlive,
                     const Transport* transport) noexcept;

    [[nodiscard]] std::shared_ptr<void> sourceLifetime() const override { return gain_.sourceLifetime(); }

    void prepare(double sampleRate, std::int32_t maxFrames) override { gain_.prepare(sampleRate, maxFrames); }
    void process(const NodeIo& io) noexcept override { gain_.process(io); }
    /// Asleep, a send lands a level moved meanwhile, as a strip does.
    void silenceTaps(const NodeIo& io) noexcept override { gain_.silenceTaps(io); }

    /// Message thread: the next block starts at the level, without a ramp --
    /// for a send back in the plan, which no graph has played meanwhile.
    void restart() noexcept { gain_.restart(); }
    [[nodiscard]] bool processesDouble() const noexcept override { return true; }
    /// Zero, like a strip's: a send holds nothing back.
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    /// A track's notes do not travel down its sends.
    [[nodiscard]] EventFlow eventFlow() const noexcept override { return EventFlow::Stop; }
    [[nodiscard]] const char* name() const noexcept override { return "send"; }

private:
    StripNode gain_;
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

    /// The node a planned send plays through (ADR-0203 d3), by its routing
    /// row, or null. One per row, kept for the session as a strip is, so a
    /// level move ramps and the send's edges keep their history.
    [[nodiscard]] SendNode* sendFor(std::int64_t routingId) noexcept;

    /// Rows the strip could not honour, named: an unknown pan law, a volume or
    /// pan that is not finite.
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }

private:
    std::map<std::int64_t, std::unique_ptr<StripNode>> strips_;
    std::map<std::int64_t, std::unique_ptr<SendNode>> sends_;
    std::set<std::int64_t> liveSends_;   ///< the rows planned at the last sync
    std::vector<std::string> problems_;
};

}  // namespace adi::engine
