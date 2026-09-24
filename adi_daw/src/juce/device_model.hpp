// SPDX-License-Identifier: GPL-3.0-or-later
//
// The device contract, and it contains NO JUCE and NO VST3.
//
// That is the whole design, and it is the finding of docs/DEVICE-CONTRACT-PANEL.md
// turned into a type. The panel scored four designs and the one shaped like a
// plugin was, in its own words, "named after the format that conforms to it
// worst": every property it was proudest of — a real-valued automation lane
// surviving a parameter's range changing — is available to CLAP, to a Pure Data
// patch and to a native device, and NOT to VST3, whose API exposes a real value
// only as a display string.
//
// So the format in hand is the one that fits worst, and shaping the contract
// around it would make Pd, CLAP and a remote AudioGridder device each an
// exception (ADR-0052 decision 4, ADR-0053 decision 1). Hence: this file
// describes what a device IS, `vst3_host.*` describes how one particular API
// is made to look like that, and the graph never learns which it has.
//
// It lives under `src/juce/` because devices are that lane's responsibility and
// for no other reason — same as `device_core.hpp`, and for the same payoff: it
// compiles into `adi_core`, so its tests run on all seven ABIs rather than only
// in the one CI job that has JUCE.

#pragma once

#include "adi/engine/graph.hpp"
#include "adi/engine/mpe_output.hpp"
#include "adi/engine/param_edits.hpp"

#include <atomic>

#include <cstdint>
#include <string>
#include <vector>

namespace adi::device {

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

/// How a parameter's value is expressed. The spellings are
/// `automation_lanes.value_domain`'s, because a lane and the parameter it
/// drives disagreeing about the domain is a silent wrong number.
enum class ParamDomain : std::uint8_t { Normalized = 0, Real = 1, Enum = 2 };

const char* toString(ParamDomain) noexcept;

/// One parameter's value, carrying BOTH representations and saying which are
/// real.
///
/// This is the panel's resolution and the schema already had it:
/// `plugin_params.normalized_value` is `NOT NULL` and `real_value` is
/// nullable. The asymmetry is not an oversight, it is the VST3 edge:
///
///   * `normalized` is ALWAYS authoritative. Every format can produce it, it
///     is what gets sent back to the plugin, and it is what survives a
///     round trip.
///   * `real` is the readable one — 4800 Hz, -6 dB, 120 ms — and it is what
///     makes an automation lane still mean something when the plugin is
///     missing (SPEC 6.3.3) or has remapped its range between versions. CLAP,
///     Pd and native devices supply it. VST3 usually cannot, because
///     `getParamStringByValue` hands back "4.80 kHz" and parsing a localised
///     display string back into a number is a guess, not a conversion.
///
/// `hasReal` is therefore a fact about the plugin, not a flag someone forgot
/// to set, and code that treats a missing real value as zero is the bug this
/// struct exists to make impossible to write by accident.
struct ParamValue {
    double normalized = 0.0;
    double real       = 0.0;
    bool   hasReal    = false;

    [[nodiscard]] static ParamValue fromNormalized(double n) noexcept {
        ParamValue v; v.normalized = n; return v;
    }
    [[nodiscard]] static ParamValue withReal(double n, double r) noexcept {
        ParamValue v; v.normalized = n; v.real = r; v.hasReal = true; return v;
    }
};

/// What a device declares about one parameter.
///
/// `id` is TEXT and not an index, matching `plugin_params.param_id`. An index
/// is a binding, not an identity: VST3 hands out a `ParamID` that is stable for
/// one plugin version and a CLAP plugin may reorder, while a Pd patch's
/// parameter set can change while the project is CLOSED because the patch is a
/// text file someone edited. A string that the source owns is the only identity
/// that survives all three.
struct ParamDescriptor {
    std::string   id;
    std::string   name;
    std::string   unit;
    ParamDomain   domain = ParamDomain::Normalized;
    ParamValue    defaultValue;
    /// Only meaningful when `hasRealRange`; the real-unit bounds, so a lane
    /// written in Hz can be re-normalised if the plugin's range moves.
    double        minReal = 0.0;
    double        maxReal = 0.0;
    bool          hasRealRange = false;
    bool          automatable = true;
    std::uint32_t flags = 0;
};

/// Enough to find the plugin again, and to tell the user what is missing when
/// it cannot be found (ADR-0011). Mirrors `plugin_refs`.
struct DeviceIdentity {
    std::string format;   ///< 'vst3' | 'clap' | 'internal' | whatever was recorded
    std::string uid;
    std::string name;
    std::string vendor;
    std::string version;

    /// What a user is shown when the plugin did not load. Never empty: a
    /// placeholder saying nothing is the failure SPEC 7.1 is about.
    [[nodiscard]] std::string describe() const;
};

// ---------------------------------------------------------------------------
// The format boundary
// ---------------------------------------------------------------------------

/// One loaded device, whatever it actually is.
///
/// Every plugin API in this project implements this and nothing above it knows
/// the difference. The set of virtuals is deliberately small: anything a
/// particular format cannot do is answered conservatively here rather than
/// leaking a capability query upward.
class DeviceInstance {
public:
    virtual ~DeviceInstance() = default;
    DeviceInstance(const DeviceInstance&) = delete;
    DeviceInstance& operator=(const DeviceInstance&) = delete;

    [[nodiscard]] virtual const DeviceIdentity& identity() const noexcept = 0;

    /// False for a device that could not be instantiated. Such a device is
    /// STILL a device and still in the chain — ADR-0011 — it simply passes
    /// audio through and holds its bytes.
    [[nodiscard]] virtual bool loaded() const noexcept = 0;

    // Names commented out, matching `engine::Node::prepare`: MSVC /W4 raises
    // C4100 on an unreferenced parameter in a defaulted body, and the
    // -Werror build turns that into a hard failure that the default local
    // build never shows.
    virtual void prepare(double /*sampleRate*/, std::int32_t /*maxFrames*/) {}
    virtual void release() {}

    /// Audio thread. Same contract as `engine::Node::process`.
    virtual void process(const engine::NodeIo& io) noexcept = 0;

    // --- what the scheduler asks (ADR-0043, ADR-0058) ----------------------

    /// Conservative by default: never suspended. A plugin reporting no tail
    /// and then producing one is common enough that `devices.always_process`
    /// exists for it.
    [[nodiscard]] virtual std::int64_t tailSamples() const noexcept {
        return engine::kInfiniteTail;
    }
    /// 0 by default, the OPPOSITE default from the tail. See
    /// `engine::Node::latencySamples`.
    [[nodiscard]] virtual std::int32_t latencySamples() const noexcept { return 0; }

    /// Whether this device turns notes into audio, and so stops the note
    /// stream (ADR-0091). Through by default, the harmless failure: see
    /// `engine::Node::eventFlow`, including its rule that this is read on the
    /// audio thread and must return a value decided at construction.
    [[nodiscard]] virtual engine::EventFlow eventFlow() const noexcept {
        return engine::EventFlow::Through;
    }

    /// A counter that increases every time this device reports that its
    /// latency moved. ADR-0082's coalescer polls it; ADR-0066 says the
    /// report must do nothing else.
    ///
    /// ON THE CONTRACT rather than on the format types, and that is the
    /// point. `DeviceHost` registered its sources with a `dynamic_cast` to
    /// `Vst3Device` in its first version, which is a format-agnostic host
    /// asking what format it has — ADR-0052 decision 4's exact failure. The
    /// compiler caught it, because the cast needs a JUCE header in a file
    /// that has none.
    ///
    /// **0 means "this device does not report here."** A CLAP device returns
    /// 0 and reports through `ClapHostGlue` instead, because
    /// `clap_host_latency.changed` is a HOST callback — the notification
    /// belongs to the host object, not to the plugin (ADR-0084).
    [[nodiscard]] virtual std::uint64_t latencyEpoch() const noexcept { return 0; }

    /// ADR-0142 (ADR-0110 d1): a CAPTURE BOUNDARY. Moves when the plugin says
    /// its state changed in a way its parameter broadcasts do not carry -- a
    /// preset picked in its browser, a sample dropped on it: VST3's
    /// `restartComponent` and `setDirty`, CLAP's `params.rescan` and
    /// `state.mark_dirty`. The parameter-op glue compares it at every drain
    /// and, when it moved, snapshots the chunk as a `device.loadState` op.
    /// Our own `setParam` and `loadState` do not move it.
    ///
    /// 0, never moving, is "this device never signals". A CLAP device
    /// reports its HOST's count, shared by every plugin that host serves
    /// until each has a `clap_host_t` of its own (ADR-0123 C4); the glue's
    /// hash comparison is what tells which plugin actually changed.
    [[nodiscard]] virtual std::uint64_t stateEpoch() const noexcept { return 0; }

    /// ADR-0146, ADR-0149: play per-note expression on this route. Message
    /// thread; the format applies it where it is safe. False: this device
    /// cannot take a chosen route (a CLAP's dialect follows its note ports,
    /// ADR-0099), and the session reports that, keeping the project's row.
    virtual bool setExpressionRoute(engine::RouteChoice) { return false; }

    // --- parameters --------------------------------------------------------

    [[nodiscard]] virtual std::int32_t paramCount() const noexcept { return 0; }
    [[nodiscard]] virtual const ParamDescriptor* paramAt(std::int32_t) const noexcept {
        return nullptr;
    }
    /// By `param_id`, not index. Returns nullptr when the device does not have
    /// it — which is the ordinary case for a project saved against a newer
    /// version of the plugin, not an error.
    [[nodiscard]] virtual const ParamDescriptor* findParam(
        const std::string& paramId) const noexcept;

    [[nodiscard]] virtual ParamValue getParam(const std::string& paramId) const noexcept;

    /// Message thread. The op log is the source of truth (ADR-0038); this
    /// pushes a value the log has already recorded into the live plugin.
    virtual bool setParam(const std::string& paramId, const ParamValue& v);

    // --- opaque state (ADR-0038) -------------------------------------------

    /// The stream roles this device produces: 'component', 'controller', ...
    /// Empty for a device with no opaque state.
    [[nodiscard]] virtual std::vector<std::string> stateRoles() const { return {}; }
    [[nodiscard]] virtual std::vector<std::uint8_t> saveState(const std::string&) const {
        return {};
    }
    virtual bool loadState(const std::string&, const std::vector<std::uint8_t>&) {
        return false;
    }

    /// Where this device's own parameter broadcasts go (ADR-0110, ADR-0124).
    /// Set by the glue on the message thread; read by whichever thread the
    /// format broadcasts on. The capture behind it is single-producer, which
    /// is why it is per device. Null unhooks; the capture must outlive the
    /// unhooking, because a push may be in flight.
    void setParamSink(engine::ParamEditCapture* capture, std::int64_t deviceId) noexcept;
    [[nodiscard]] bool hasParamSink() const noexcept {
        return sink_.load(std::memory_order_acquire) != nullptr;
    }

protected:
    DeviceInstance() = default;

    /// A format's broadcast, on the format's thread: pushed into the sink if
    /// one is set, dropped otherwise. `normalized` is the wire unit; a
    /// gesture begin or end carries no value.
    void broadcastParam(std::int32_t index, engine::ParamEventKind kind,
                        double normalized) noexcept;

private:
    std::atomic<engine::ParamEditCapture*> sink_{nullptr};
    std::atomic<std::int64_t> sinkDevice_{0};
};

// ---------------------------------------------------------------------------
// The missing plugin (ADR-0011, SPEC 7.1)
// ---------------------------------------------------------------------------

/// A device that could not be instantiated, and is therefore STILL A DEVICE.
///
/// The rule it implements has three parts and all three are easy to get wrong
/// in a way nobody notices until a session is opened on another machine:
///
///   1. **It is never dropped.** Dropping it silently rewires the signal path —
///      a chain of four with the second missing becomes a chain of three, and
///      everything downstream now receives something it has never received.
///   2. **Its state survives byte for byte.** The bytes are opaque to us, so
///      "preserve" means exactly that: hand back what was handed in, on a
///      machine that has the plugin. Re-encoding, truncating or normalising
///      them is the same as losing them.
///   3. **The identity is surfaced.** "A plugin is missing" is not actionable;
///      "Valhalla VintageVerb 2.1.0 by Valhalla DSP" is.
///
/// It passes audio through unchanged rather than silencing, because a bypassed
/// insert is what the user would have chosen and silence is not recoverable by
/// listening.
class MissingDevice final : public DeviceInstance {
public:
    explicit MissingDevice(DeviceIdentity id) : id_(std::move(id)) {}

    [[nodiscard]] const DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return false; }

    void process(const engine::NodeIo& io) noexcept override;

    /// A pass-through adds no delay and holds nothing back. Declaring a tail
    /// here would keep a whole chain awake for a device that is doing nothing.
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return 0; }

    /// The recorded parameter values, read back from `plugin_params`. They are
    /// why SPEC 6.3.3 prefers a 'real' automation domain: with the plugin
    /// absent, `normalized` 0.42 means nothing and "4800 Hz" still does.
    void addParam(ParamDescriptor d, ParamValue v);

    [[nodiscard]] std::int32_t paramCount() const noexcept override {
        return static_cast<std::int32_t>(params_.size());
    }
    [[nodiscard]] const ParamDescriptor* paramAt(std::int32_t i) const noexcept override;
    [[nodiscard]] ParamValue getParam(const std::string& paramId) const noexcept override;
    bool setParam(const std::string& paramId, const ParamValue& v) override;

    /// Held, not interpreted. This is rule 2 above.
    void keepState(const std::string& role, std::vector<std::uint8_t> bytes);
    [[nodiscard]] std::vector<std::string> stateRoles() const override;
    [[nodiscard]] std::vector<std::uint8_t> saveState(const std::string& role) const override;
    bool loadState(const std::string& role, const std::vector<std::uint8_t>& b) override;

private:
    DeviceIdentity id_;
    std::vector<ParamDescriptor> params_;
    std::vector<ParamValue> values_;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> state_;
};

// ---------------------------------------------------------------------------
// The node the graph actually sees
// ---------------------------------------------------------------------------

/// Wraps ONE `DeviceInstance` as an `engine::Node`.
///
/// This type is the reason there is no `Vst3Device` with a chain behind it.
/// Hybrid tracks, modulation, suspension and delay compensation are implemented
/// once, against `Node`, and a plugin reaches them by being one — rather than
/// each of those growing a second implementation that diverges on the third bug
/// (ADR-0052 decision 4).
///
/// It forwards the three declarations rather than answering them itself,
/// EXCEPT where the project overrides:
///
///   * `always_process` (`devices.always_process`) forces `kInfiniteTail`,
///     which is ADR-0043's escape hatch for a plugin that reports no tail and
///     then produces one.
///   * a bypassed device passes audio through and declares no tail and no
///     latency, because it is not running.
class DeviceNode final : public engine::Node {
public:
    explicit DeviceNode(DeviceInstance& inst) : inst_(&inst) {}

    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void release() override;
    void process(const engine::NodeIo& io) noexcept override;

    [[nodiscard]] std::int64_t tailSamples() const noexcept override;
    [[nodiscard]] std::int32_t latencySamples() const noexcept override;
    [[nodiscard]] bool alwaysProcess() const noexcept override { return always_; }
    [[nodiscard]] engine::EventFlow eventFlow() const noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "device"; }

    /// `devices.always_process`. Message thread.
    void setAlwaysProcess(bool v) noexcept { always_ = v; }

    /// `devices.enabled` = 0. A bypassed device stays in the graph so the
    /// topology — and therefore every other node's compensation — does not
    /// change when someone clicks bypass.
    void setBypassed(bool v) noexcept { bypassed_ = v; }
    [[nodiscard]] bool bypassed() const noexcept { return bypassed_; }

    [[nodiscard]] DeviceInstance& instance() const noexcept { return *inst_; }

private:
    DeviceInstance* inst_ = nullptr;
    bool always_ = false;
    bool bypassed_ = false;
};

/// Copy `io.in` to `io.out` for one segment, or zero the output when there is
/// no input. Shared by `MissingDevice` and by a bypassed `DeviceNode` — the two
/// places where "do nothing" has to mean something specific.
void passThrough(const engine::NodeIo& io) noexcept;

}  // namespace adi::device
