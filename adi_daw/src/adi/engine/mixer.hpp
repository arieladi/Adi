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

/// One track's strip as the model has it, sanitised: what its StripNode
/// plays, and what every Post send from it plays at its first stage
/// (SendNode). One value, applied to both by `StripNode::configure`, so a
/// strip and its sends never disagree about the track -- and a parameter
/// the strip learns to play is a field here, played by both.
struct StripConfig {
    double volumeDb = 0.0;
    double pan = 0.0;
    PanLaw law = PanLaw::Live;
    bool unmuted = true;   ///< the track's mute; its solo is not here (`Hearing`)
    /// The lanes the strip reads (ADR-0164), or null; `keepAlive` holds them,
    /// and the realiser keeps it with every graph that plays the node, so a
    /// retired graph never reads a freed lane.
    const StripLanes* lanes = nullptr;
    std::shared_ptr<void> keepAlive;
    std::int64_t delaySamples = 0;    ///< the track delay (ADR-0172), at `delayRate`
    std::int64_t delayRate = 48000;   ///< the project's rate
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

    /// Message thread: all of the above from one track's configuration, with
    /// `audible` the gate to play it at (its mute, and its solo or not).
    void configure(const StripConfig& config, bool audible, const Transport* transport) noexcept {
        setTransport(transport);
        setStatic(config.volumeDb, config.pan, config.law, audible);
        setDelay(config.delaySamples, config.delayRate);
        setLanes(config.lanes, config.lanes != nullptr ? config.keepAlive : std::shared_ptr<void>{});
    }

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
/// (ADR-0092).
///
/// Two gain stages, each a strip's -- the same law, the same five
/// milliseconds, the same width handover -- run in turn: what its source
/// plays at the tap (`setSource`), then the send's own level, pan and switch
/// (`set`). A Post send leaves where a Pre send does, after the effects, and
/// plays its source's strip in the first stage -- fader, pan, mute, lanes,
/// track delay, the same ramps -- but not its solo, which is the second
/// stage's switch (`hearing`). So a track soloed away is silent at its strip
/// while a soloed return still hears it, with no node anywhere that a send's
/// coming or going changes under a playing graph. At 0 dB, centred and on,
/// the second stage is exact: a Post send is bit for bit what its strip
/// plays without the solo, at either precision.
class SendNode final : public Node {
public:
    SendNode() noexcept { source_.setStatic(0.0, 0.0, PanLaw::Live, true); }

    /// Message thread. A level at or below kSilenceDb is silent, and so is
    /// one that is not a number; one past kSendTopDb, +inf included, plays at
    /// kSendTopDb (the caller names both). `on` is Enable Send, with solo as
    /// it reaches this send (`Hearing::send`).
    void set(double levelDb, double pan, PanLaw law, bool on) noexcept {
        const double db = std::isnan(levelDb) ? -std::numeric_limits<double>::infinity()
                                              : std::min(levelDb, kSendTopDb);
        gain_.setStatic(db, pan, law, on);
    }

    /// Message thread: what the source plays at the send's tap, before the
    /// send's own gain, played at its mute (`unmuted`) and never its solo.
    /// For a Post send, its strip's configuration, the same value the strip
    /// is given; for a Pre send, unity, its mute and its mute lane (the sends
    /// plan's preMute, on until Live is measured). The same transport every
    /// time, as every strip is given it, so a send turned from Pre to Post
    /// never changes it under a graph.
    void setSource(const StripConfig& source, const Transport* transport) noexcept {
        source_.configure(source, source.unmuted, transport);
        gain_.setTransport(transport);
    }

    [[nodiscard]] std::shared_ptr<void> sourceLifetime() const override { return source_.sourceLifetime(); }
    /// The source's track delay, as its strip reports it: so the edge after
    /// this node waits as the edge after the strip does.
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return source_.latencySamples(); }

    void prepare(double sampleRate, std::int32_t maxFrames) override {
        source_.prepare(sampleRate, maxFrames);
        gain_.prepare(sampleRate, maxFrames);
    }
    void process(const NodeIo& io) noexcept override;
    /// Asleep, a send lands a level moved meanwhile, as a strip does.
    void silenceTaps(const NodeIo& io) noexcept override {
        source_.silenceTaps(io);
        gain_.silenceTaps(io);
    }

    /// Message thread: the next block starts at the level, without a ramp --
    /// for a send back in the plan, which no graph has played meanwhile.
    void restart() noexcept {
        source_.restart();
        gain_.restart();
    }
    [[nodiscard]] bool processesDouble() const noexcept override { return true; }
    /// Zero, like a strip's: a send holds nothing back.
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    /// A track's notes do not travel down its sends.
    [[nodiscard]] EventFlow eventFlow() const noexcept override { return EventFlow::Stop; }
    [[nodiscard]] const char* name() const noexcept override { return "send"; }

private:
    StripNode source_;   ///< what the source plays at the tap
    StripNode gain_;     ///< the send's own level, pan and switch
};

/// Live's feedback loop (ADR-0203 d3): a return's send into itself. What the
/// return sends itself, after the send's own node, is written to a ring and
/// played back into the return's junction `D` samples later, beside whatever
/// else feeds it. D is the graph's largest block, so a block is always
/// written before it is read, whatever size the callbacks are; Live's figure
/// is AWAITING 7(f). The session owns one per routing row, as it owns a send.
///
/// Two nodes and the ring between them, outside the graph's order. The writer
/// ends the send: a sink, no edge leaves it. The reader starts the return: a
/// source, no edge enters it, so it is at level 0 and runs, every segment of
/// it, before the writer in every block -- one audio thread runs the levels in
/// order (Graph::process). The reader keeps the loop's clock: at the start of
/// a block it marks where the block starts, and both nodes address the ring
/// from there, so they agree however a block is split.
///
/// NOTHING STALE OR UNWRITTEN IS EVER PLAYED. The reader plays a position only
/// if it has been written and not played already (a restart counts all it
/// held as played); anything else is silence. The ring holds two of the
/// largest block, so nothing it may play has been overwritten. So a D that grows leaves a gap
/// rather than a repeat, and one that shrinks skips rather than plays garbage.
/// A loop starts empty when it comes back into play after time out of it --
/// told at the swap, against the graph that really played last
/// (`Node::enterPlay`) -- when the sample rate changes, since what it holds
/// would play at the wrong pitch, and when the device stops (`release`). A writer that missed blocks zeroes
/// what it missed before it writes again.
///
/// Every position and the ring itself are the audio thread's. A graph's
/// `prepare` runs while the previous graph may still be playing the same
/// nodes, so preparing writes only atomics: the D and rate the next block
/// takes up.
///
/// The ring is double at both precisions, so a precision change keeps the
/// echoes in flight exactly. Its shape -- channels, and capacity for two of
/// the largest block -- is fixed when it is made, on the message thread, and
/// never reallocated: a session-owned node is prepared while the previous
/// graph may still be playing it. A block larger than the shape holds needs a
/// new loop (`fits`).
class FeedbackLoop {
public:
    /// The smallest largest-block a loop is made for, so that every buffer
    /// size an audio device offers changes D without making a new loop.
    static constexpr std::int32_t kMinBlock = 8192;

    /// Message thread: allocates the ring, for `channels` and blocks up to
    /// max(maxFrames, kMinBlock).
    FeedbackLoop(std::int32_t channels, std::int32_t maxFrames);

    /// Whether this loop's ring can play a graph of this shape.
    [[nodiscard]] bool fits(std::int32_t channels, std::int32_t maxFrames) const noexcept;

    /// The node that ends the send: the ring's writer.
    [[nodiscard]] Node& writer() noexcept { return writer_; }
    /// The node that starts the return: the ring's reader, a feedback source.
    [[nodiscard]] Node& reader() noexcept { return reader_; }

    /// D, in samples, as the last graph to prepare the reader set it.
    [[nodiscard]] std::int32_t delay() const noexcept { return pendingDelay_.load(std::memory_order_relaxed); }
    /// Samples the writer could not write as they came: not finite, written
    /// as silence, or past the ceiling, written at it.
    [[nodiscard]] std::uint64_t guarded() const noexcept { return guarded_.load(std::memory_order_relaxed); }

    /// Below this a sample is written as silence: -600 dB, far under any
    /// converter. Without it a loop that decays never reaches silence, at
    /// either width: any gain between -6 dB and unity rounds a subnormal back
    /// to itself, so the loop would settle there for good, its return never
    /// asleep and its chain doing subnormal arithmetic.
    static constexpr double kFloor = 1e-30;

    /// A loop that runs away is held here, 2^20 (about +120 dBFS): far past
    /// any converter, and far enough under a float's top that no gain stage
    /// after it reaches infinity at either width. An infinity into a device
    /// would turn its state, and the master after it, to NaN for good; the
    /// writer's flush cannot reach that. ADI's guard: Live's is not measured
    /// (AWAITING 7(f)).
    static constexpr double kCeiling = 1048576.0;

private:
    class Writer final : public Node {
    public:
        explicit Writer(FeedbackLoop& loop) : loop_(loop) {}
        void process(const NodeIo& io) noexcept override { loop_.write(io); }
        /// Never asleep: it writes silence when the send is silent, so the
        /// ring has no gaps while the loop is in the graph.
        [[nodiscard]] std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
        /// Its input is never compensated: the send's own latency, the
        /// return's track delay, does not lengthen the loop.
        [[nodiscard]] bool closesFeedback() const noexcept override { return true; }
        [[nodiscard]] bool processesDouble() const noexcept override { return true; }
        [[nodiscard]] EventFlow eventFlow() const noexcept override { return EventFlow::Stop; }
        [[nodiscard]] const char* name() const noexcept override { return "feedback.write"; }

    private:
        FeedbackLoop& loop_;
    };
    class Reader final : public Node {
    public:
        explicit Reader(FeedbackLoop& loop) : loop_(loop) {}
        void prepare(double sampleRate, std::int32_t maxFrames) override { loop_.prepareFormat(sampleRate, maxFrames); }
        /// The device stopped: the loop starts empty when it plays again, as a
        /// plug-in's delay starts empty when it is activated again.
        void release() override { loop_.restart_.store(true, std::memory_order_release); }
        void process(const NodeIo& io) noexcept override { loop_.read(io); }
        void enterPlay() noexcept override { loop_.empty(); }
        /// Never asleep: it keeps the loop's clock. Its silence is measured
        /// silent, so the return it feeds still sleeps when the loop is empty.
        [[nodiscard]] std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
        [[nodiscard]] bool closesFeedback() const noexcept override { return true; }
        [[nodiscard]] bool processesDouble() const noexcept override { return true; }
        [[nodiscard]] EventFlow eventFlow() const noexcept override { return EventFlow::Stop; }
        [[nodiscard]] const char* name() const noexcept override { return "feedback.read"; }

    private:
        FeedbackLoop& loop_;
    };

    void prepareFormat(double sampleRate, std::int32_t maxFrames) noexcept;
    void empty() noexcept { writtenEnd_ = readEnd_ = clock_; }   // audio thread
    void read(const NodeIo& io) noexcept;    // audio thread
    void write(const NodeIo& io) noexcept;   // audio thread
    [[nodiscard]] double* lane(std::int32_t c) noexcept {
        return ring_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(capacity_);
    }
    /// Where position `q` (never negative) lives in a lane.
    [[nodiscard]] std::size_t slot(std::int64_t q) const noexcept {
        return static_cast<std::size_t>(q % capacity_);
    }

    std::int32_t channels_;
    std::int32_t capacity_;
    std::vector<double> ring_;   ///< channels_ lanes of capacity_ frames
    Writer writer_{*this};
    Reader reader_{*this};
    std::atomic<std::int32_t> pendingDelay_;
    std::atomic<double> rate_{0.0};          ///< the rate last prepared at
    std::atomic<bool> restart_{false};       ///< a new rate, a stop: empty at the next block
    std::atomic<std::uint64_t> guarded_{0};
    // The audio thread's alone. Positions count frames from the loop's start.
    std::int32_t delay_ = 0;            ///< D for the block being played
    std::int64_t clock_ = 0;            ///< where the next block starts
    std::int64_t blockStart_ = 0;       ///< where this block started
    std::int64_t writtenEnd_ = 0;       ///< one past the last position written
    std::int64_t readEnd_ = 0;          ///< one past the last position played, or emptied
};

/// How solo reaches across sends (ADR-0203 d3). Fixed until the format has a
/// home for them (win's question 7); a test may change them.
struct HearingFlags {
    /// Live's Solo in Place: a soloed track's returns, and what they feed, stay heard.
    bool soloInPlace = true;
    /// Another track's solo silences a Pre send with its source, as it does
    /// a Post one (the sends plan's preFollowsSolo, on until Live is measured).
    bool preFollowsSolo = true;
    /// With Solo in Place, every return stays heard, not only those a heard
    /// track sends to (the sends plan's keepAllReturns, off until Live is
    /// measured).
    bool keepAllReturns = false;
};

/// Who is heard (ADR-0163 d4, ADR-0203 d3). A muted track is silent. When any
/// track is soloed, a track is heard only if it is soloed, solo-defeated,
/// feeds a soloed track (a group's children), or is fed by one (its group,
/// the master) -- over main routes; a sidechain key keeps nothing audible --
/// or, with Solo in Place, is a soloed track's return or what that feeds. A
/// soloed track hears what is sent to it: a sender soloed away stays silent
/// in its own output while its send plays, and a group whose send it hears
/// has its children play into it alone, its own output silent.
struct Hearing {
    /// By track id: its own output, mute included.
    std::map<std::int64_t, bool> track;
    /// By routing row: whether solo lets the send through -- into what a
    /// soloed track hears, between two heard tracks, or Pre when Pre sends do
    /// not follow solo. A muted source is silent in all its sends at the
    /// send's own first stage, as at its strip, so the mute is not here.
    std::map<std::int64_t, bool> send;
};
[[nodiscard]] Hearing hearing(const rows::Model&, const GraphPlan&, const HearingFlags& = {});

/// Which tracks are heard: `hearing(...).track`.
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

    /// The loop a return's send into itself closes (ADR-0203 d3), by its
    /// routing row, or null. Kept for the session, so its echoes cross every
    /// rebuild.
    [[nodiscard]] FeedbackLoop* loopFor(std::int64_t routingId) noexcept;
    [[nodiscard]] const FeedbackLoop* loopFor(std::int64_t routingId) const noexcept {
        return const_cast<MixerStrips*>(this)->loopFor(routingId);
    }

    /// The shape of the graphs the next sync's loops play in: the session's
    /// channels and largest block. A loop that cannot hold it is replaced, at
    /// that sync, by one that can.
    void setFormat(std::int32_t channels, std::int32_t maxFrames) noexcept {
        channels_ = channels;
        maxFrames_ = maxFrames;
    }

    /// Rows the strip could not honour, named: an unknown pan law, a volume or
    /// pan that is not finite.
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }

private:
    std::map<std::int64_t, std::unique_ptr<StripNode>> strips_;
    std::map<std::int64_t, std::unique_ptr<SendNode>> sends_;
    std::set<std::int64_t> liveSends_;   ///< the rows planned at the last sync
    std::map<std::int64_t, std::unique_ptr<FeedbackLoop>> loops_;
    /// Loops replaced by a larger shape, kept until the session ends: a
    /// retired graph may still name their nodes (ADR-0122's rule).
    std::vector<std::unique_ptr<FeedbackLoop>> retiredLoops_;
    std::int32_t channels_ = 2;
    std::int32_t maxFrames_ = 512;
    std::vector<std::string> problems_;
};

}  // namespace adi::engine
