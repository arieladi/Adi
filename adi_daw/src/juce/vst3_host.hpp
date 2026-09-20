// SPDX-License-Identifier: GPL-3.0-or-later
//
// VST3 hosting: the adapter that makes one plugin API look like the contract
// in device_model.hpp, and nothing else.
//
// ADR-0041 and ADR-0052: VST3 is the only format hosted today, CLAP is
// mandated next, and a remote AudioGridder device (ADR-0053) after that. All
// three go behind `DeviceInstance`. There is deliberately no Vst3Device with a
// chain, a parameter model or a suspension rule of its own -- those exist once,
// against `engine::Node`, and a plugin reaches them by being one.
//
// WHAT JUCE'S HOST PATH CAN AND CANNOT DO, checked against JUCE 9.0.2's source
// rather than assumed, because ADR-0054 makes one of these load-bearing:
//
//   * Discovery, instantiation, audio, parameters, opaque state, latency and
//     tail all work through `juce::AudioPluginInstance`, and this file uses it
//     for all of them.
//
//   * PER-NOTE EXPRESSION DOES NOT. `processBlock` takes a `juce::MidiBuffer`,
//     and `VST3Common.h`'s `toEventList` iterates exactly that. Three facts
//     from that file settle it: `createNoteOnEvent` sets `e.noteOn.noteId =
//     -1` (and so do noteOff and polyPressure), nothing anywhere constructs a
//     `kNoteExpressionValueEvent`, and an incoming one is converted to `{}`.
//     Since VST3 anchors note expression to a note's `noteId`, a hardcoded -1
//     means it cannot be addressed even if it could be sent. Velocity goes
//     through `normaliseMidiValue`, which is `value / 127.0f`.
//
//     That is exactly the failure ADR-0054 named -- "a single uint8_t in an
//     event struct undoes the mandate" -- sitting in the framework rather than
//     in our code, where every document would still have claimed compliance.
//
//   * THE ROUTE OUT EXISTS AND IS NOT A REWRITE. JUCE 9.0.2 hands a host the
//     raw interface: `AudioPluginInstance::getVST3Client()->getIComponentPtr()`
//     returns `Steinberg::Vst::IComponent*`, from which `IAudioProcessor` and
//     `INoteExpressionController` are a `queryInterface` away. So the event
//     path for instruments becomes ours while discovery, state and parameters
//     stay JUCE's. ADR-0057 records the decision; it is NOT built here, and
//     `supportsNoteExpression()` below reports false rather than letting
//     anything assume otherwise.

#pragma once

#include "juce/device_model.hpp"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace adi::device {

/// Owns the format manager, and owns the fact that only one format is on it.
///
/// ADR-0041 says every other host format is 0 and explicitly so. The CMake
/// definitions say it at compile time; this says it at run time, and
/// `formatNames()` exists so a test can assert it rather than trust the build.
class Vst3Host {
public:
    Vst3Host();

    /// Exactly one entry, "VST3". A second one here means something added a
    /// format behind the build flags.
    [[nodiscard]] std::vector<std::string> formatNames() const;

    /// Where the OS keeps VST3s. Read from JUCE rather than hardcoded.
    [[nodiscard]] juce::FileSearchPath defaultSearchPaths() const;

    /// Blocking scan. Message thread; a plugin's factory runs arbitrary code
    /// and some of them are slow.
    void scan(const juce::FileSearchPath& paths, juce::KnownPluginList& into);

    /// Instantiate, or return nullptr and set `error`.
    ///
    /// A null return is NOT a failure the caller may treat as "skip this
    /// device" -- ADR-0011 says the device stays in the chain as a bypassed
    /// placeholder. `makeDevice` below does that for you.
    std::unique_ptr<juce::AudioPluginInstance> instantiate(
        const juce::PluginDescription& desc, double sampleRate,
        int blockSize, std::string& error);

    /// The ADR-0011 path, and the one callers should use: a real device when
    /// the plugin loads, a `MissingDevice` carrying the same identity when it
    /// does not. Never null.
    std::unique_ptr<DeviceInstance> makeDevice(const juce::PluginDescription& desc,
                                               double sampleRate, int blockSize,
                                               std::string& error);

    [[nodiscard]] juce::AudioPluginFormatManager& formats() noexcept { return formats_; }

private:
    juce::AudioPluginFormatManager formats_;
};

/// Identity as the format records it, so a missing plugin can still be named.
DeviceIdentity identityOf(const juce::PluginDescription& desc);

/// One VST3, behind the format-agnostic contract.
class Vst3Device final : public DeviceInstance,
                         private juce::AudioProcessorListener {
public:
    Vst3Device(std::unique_ptr<juce::AudioPluginInstance> inst, DeviceIdentity id);
    ~Vst3Device() override;

    [[nodiscard]] const DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return inst_ != nullptr; }

    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void release() override;
    void process(const engine::NodeIo& io) noexcept override;

    // --- the two declarations, mapped straight through ----------------------

    /// VST3's `kInfiniteTail` is `INT64_MAX` and so is `engine::kInfiniteTail`;
    /// they are the same constant on purpose, so this is a mapping and not a
    /// translation. JUCE reports the tail in SECONDS, so the conversion is
    /// here, and the infinite case is a branch rather than an arithmetic
    /// accident -- multiplying an infinity by a sample rate does not give you
    /// INT64_MAX, it gives you undefined behaviour.
    [[nodiscard]] std::int64_t tailSamples() const noexcept override;

    /// Whatever `getLatencySamples()` says, and nothing added. ADR-0058
    /// decision 4: the device buffer is NOT included here, or every
    /// compensated track is wrong by up to 4096 samples.
    [[nodiscard]] std::int32_t latencySamples() const noexcept override;

    // --- parameters ---------------------------------------------------------

    [[nodiscard]] std::int32_t paramCount() const noexcept override {
        return static_cast<std::int32_t>(params_.size());
    }
    [[nodiscard]] const ParamDescriptor* paramAt(std::int32_t i) const noexcept override;
    [[nodiscard]] ParamValue getParam(const std::string& paramId) const noexcept override;
    bool setParam(const std::string& paramId, const ParamValue& v) override;

    // --- state (ADR-0038) ---------------------------------------------------

    [[nodiscard]] std::vector<std::string> stateRoles() const override;
    [[nodiscard]] std::vector<std::uint8_t> saveState(const std::string& role) const override;
    bool loadState(const std::string& role, const std::vector<std::uint8_t>& b) override;

    // --- ADR-0066: a latency change is REPORTED and nothing else ------------

    /// Bumped whenever the plugin reports a new latency. The message thread
    /// polls or is woken, coalesces, recomputes the schedule OFF-THREAD and
    /// republishes by atomic pointer swap (ADR-0019's machinery).
    ///
    /// This class must NEVER call back into the graph from the notification.
    /// Recomputing on the thread the report arrives on is the bug ADR-0066
    /// exists to prevent: a Pro-Q 3 switching to linear phase reports several
    /// times in milliseconds, and each report would build a schedule nobody
    /// uses while holding up whatever thread Steinberg chose to call us on.
    [[nodiscard]] std::uint64_t latencyEpoch() const noexcept {
        return latencyEpoch_.load(std::memory_order_acquire);
    }

    /// Set by the same notification. A change of parameters, programs or
    /// anything else the plugin declares dirty (SPEC 7.3's boundary list).
    [[nodiscard]] std::uint64_t stateEpoch() const noexcept {
        return stateEpoch_.load(std::memory_order_acquire);
    }

    /// False, today, and see the header. Reported rather than assumed so that
    /// nothing silently sends 7-bit values while claiming ADR-0054.
    [[nodiscard]] static bool supportsNoteExpression() noexcept { return false; }

    /// The raw interface JUCE exposes, and the route to note expression
    /// without reimplementing discovery, state and parameters. Null when the
    /// instance is not actually a VST3 -- which cannot happen while VST3 is
    /// the only format registered, and is checked rather than assumed.
    [[nodiscard]] void* rawComponent() const noexcept;

private:
    // juce::AudioProcessorListener
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& d) override;
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}

    void readParameters();

    std::unique_ptr<juce::AudioPluginInstance> inst_;
    DeviceIdentity id_;
    std::vector<ParamDescriptor> params_;
    std::vector<juce::AudioProcessorParameter*> handles_;

    juce::AudioBuffer<float> scratch_;
    juce::MidiBuffer midi_;

    std::atomic<std::uint64_t> latencyEpoch_{0};
    std::atomic<std::uint64_t> stateEpoch_{0};
    std::int32_t maxFrames_ = 0;
    std::int32_t channels_ = 2;
};

}  // namespace adi::device
