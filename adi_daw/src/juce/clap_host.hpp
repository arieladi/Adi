// SPDX-License-Identifier: GPL-3.0-or-later
//
// CLAP hosting, written against `clap/clap.h` directly. ADR-0052, ADR-0075.
//
// NO JUCE IN THIS FILE, and that is not an aesthetic choice — it is the
// headline property. `clap-juce-extensions` builds JUCE plugins *as* CLAP and
// its own README says "It does not support JUCE-based CLAP hosting", so there
// was never an add-a-format route. What there is instead is better than one:
// CLAP is a header-only MIT C API with no dependencies, so the host side
// compiles into `adi_core` and its tests run wherever the main suite runs —
// clang, gcc and MSVC on arm64 and x86_64, plus the hardened jobs. Not the
// i386/ILP32 job, which never builds the tree: it hand-compiles the blob
// reader to prove 32-bit size_t behaviour. VST3 hosting can only ever be
// exercised in the single CI job that has JUCE.
//
// THE PANEL'S FINDING, A THIRD TIME. `docs/DEVICE-CONTRACT-PANEL.md` said the
// plugin-shaped design was "named after the format that conforms to it worst".
// Against CLAP the contract in device_model.hpp needs no concessions at all:
//
//   * `clap_param_info` carries `min_value`, `max_value` and `default_value`
//     as PLAIN DOUBLES, and `get_value` returns a plain value. So
//     `ParamValue::hasReal` is true, the domain is Real, and an automation
//     lane stays meaningful with the plugin missing (SPEC 6.3.3). VST3
//     exposes a real value only as a display string.
//   * `clap_event_note_expression.value` is a double and its TUNING is
//     "relative tuning in semitones, from -120 to +120" — the same unit
//     `engine::Event` already carries, so there is NO conversion. VST3 needed
//     `norm = plain / 240 + 0.5` and a clamp (ADR-0057).
//   * CLAP names PRESSURE as its own expression id. VST3 has none, so MPE's
//     Z axis had to be mapped onto `kExpressionTypeID` by convention.
//
// Written from scratch on the director's instruction, so the host routes
// straight into `DeviceCore` rather than through somebody else's adapter.

#pragma once

#include "adi/blob.hpp"          // ExpressionDim
#include "juce/device_model.hpp"

#include <clap/clap.h>

#include <cstdint>
#include <string>
#include <vector>

namespace adi::device {

/// Which CLAP note expression carries an ADI dimension.
///
/// Every one of these is a NAMED CLAP id rather than a convention, which is
/// the difference from the VST3 mapping in `engine::note_expression.hpp`.
/// Returns -1 for a dimension CLAP does not name, so a caller cannot silently
/// send a Volume event for something else.
[[nodiscard]] constexpr std::int32_t clapExprFor(ExpressionDim d) noexcept {
    switch (d) {
        case ExpressionDim::Pitch:    return CLAP_NOTE_EXPRESSION_TUNING;
        case ExpressionDim::Pressure: return CLAP_NOTE_EXPRESSION_PRESSURE;
        case ExpressionDim::Timbre:   return CLAP_NOTE_EXPRESSION_BRIGHTNESS;
        case ExpressionDim::Gain:     return CLAP_NOTE_EXPRESSION_VOLUME;
        case ExpressionDim::Pan:      return CLAP_NOTE_EXPRESSION_PAN;
    }
    return -1;
}

/// CLAP's tuning is already in semitones, so this is the identity within the
/// declared range.
///
/// It exists anyway, for two reasons. It documents that the absence of a
/// conversion is a decision rather than an omission — the VST3 path has a real
/// one and somebody comparing them will ask. And it applies the SAME clamp,
/// because CLAP declares -120..+120 and an MPE zone configured to +/-127 would
/// otherwise hand a plugin a value outside the range its API promises.
inline constexpr double kClapTuningLimit = 120.0;

[[nodiscard]] constexpr double semitonesToClapTuning(double semitones) noexcept {
    if (semitones < -kClapTuningLimit) return -kClapTuningLimit;
    if (semitones >  kClapTuningLimit) return  kClapTuningLimit;
    return semitones;
}

/// A fixed-capacity `clap_input_events_t` we fill and hand to a plugin.
///
/// Same shape and same reasoning as `Vst3EventList`: capacity comes from
/// `reserve` at prepare, overflow is COUNTED rather than silently broken past
/// (ADR-0056, and the 2048-with-a-silent-break that JUCE does).
class ClapEventList {
public:
    void reserve(std::int32_t n);
    void clear() noexcept { events_.clear(); }

    [[nodiscard]] std::int64_t dropped() const noexcept { return dropped_; }
    [[nodiscard]] std::int32_t size() const noexcept {
        return static_cast<std::int32_t>(events_.size());
    }

    /// Translate one ADI event. False when it has no CLAP form, which is not
    /// an error — a ParamMod has its own event type and its own path.
    bool add(const engine::Event& e) noexcept;

    /// The struct a plugin is handed. Valid until the next `clear`.
    [[nodiscard]] const clap_input_events_t* inputEvents() const noexcept { return &in_; }

    /// For tests: the header of event `i`, or nullptr.
    [[nodiscard]] const clap_event_header_t* at(std::int32_t i) const noexcept;

private:
    static std::uint32_t sizeCb(const clap_input_events_t* list);
    static const clap_event_header_t* getCb(const clap_input_events_t* list,
                                            std::uint32_t index);

    /// One slot per event, big enough for the largest we emit. A union rather
    /// than separate vectors so `get()` can hand back a stable pointer into
    /// contiguous storage, which is what CLAP's contract wants.
    union Slot {
        clap_event_header_t          hdr;
        clap_event_note_t            note;
        clap_event_note_expression_t expr;
        clap_event_param_value_t     param;
    };

    std::vector<Slot> events_;
    std::int32_t cap_ = 0;
    std::int64_t dropped_ = 0;
    clap_input_events_t in_{this, &ClapEventList::sizeCb, &ClapEventList::getCb};
};

/// One CLAP plugin behind the format-agnostic contract.
///
/// Deliberately the same shape as `Vst3Device`: both are `DeviceInstance`, and
/// `DeviceNode` wraps either without knowing which. That is ADR-0052 decision
/// 4 holding — hybrid tracks, modulation, suspension and delay compensation
/// exist once, against `engine::Node`.
class ClapDevice final : public DeviceInstance {
public:
    /// Takes an already-created plugin. Ownership: `destroy()` is called on
    /// release. The `clap_host_t` handed to `create_plugin` must outlive this.
    ClapDevice(const clap_plugin_t* plugin, DeviceIdentity id);
    ~ClapDevice() override;

    [[nodiscard]] const DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return plugin_ != nullptr; }

    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void release() override;
    void process(const engine::NodeIo& io) noexcept override;

    [[nodiscard]] std::int64_t tailSamples() const noexcept override;
    [[nodiscard]] std::int32_t latencySamples() const noexcept override;

    [[nodiscard]] std::int32_t paramCount() const noexcept override {
        return static_cast<std::int32_t>(params_.size());
    }
    [[nodiscard]] const ParamDescriptor* paramAt(std::int32_t i) const noexcept override;
    [[nodiscard]] ParamValue getParam(const std::string& paramId) const noexcept override;
    bool setParam(const std::string& paramId, const ParamValue& v) override;

    [[nodiscard]] std::vector<std::string> stateRoles() const override;
    [[nodiscard]] std::vector<std::uint8_t> saveState(const std::string& role) const override;
    bool loadState(const std::string& role, const std::vector<std::uint8_t>& b) override;

    /// Read the plugin's parameter list. Called at construction and again when
    /// the plugin asks for a rescan.
    void rescanParams();

    /// `clap_id` is a uint32; `plugin_params.param_id` is TEXT. The conversion
    /// is fixed-width hex so it sorts stably and cannot collide with a Pd
    /// symbol or a VST3 id in the same column.
    [[nodiscard]] static std::string paramIdToText(clap_id id);
    [[nodiscard]] static bool paramIdFromText(const std::string& s, clap_id& out) noexcept;

private:
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* paramsExt_ = nullptr;
    const clap_plugin_state_t* stateExt_ = nullptr;
    const clap_plugin_tail_t* tailExt_ = nullptr;
    const clap_plugin_latency_t* latencyExt_ = nullptr;

    DeviceIdentity id_;
    std::vector<ParamDescriptor> params_;
    std::vector<clap_id> paramIds_;
    bool activated_ = false;
    double sampleRate_ = 0.0;
    std::int32_t maxFrames_ = 0;

    /// A CLAP parameter is set by an EVENT, not a setter, so `setParam`
    /// records intent and the next process block carries it. One slot is
    /// enough today because nothing drives more than one at a time yet; when
    /// the graph does, this becomes the same pooled queue the VST3 path has.
    clap_id pendingId_ = 0;
    double pendingReal_ = 0.0;
    bool hasPending_ = false;
};

/// The `clap_host_t` a plugin is given, and the callbacks behind it.
///
/// Every callback here runs on whichever thread the PLUGIN chose, and the
/// rule is the same one ADR-0066 made for VST3's `restartComponent`: REPORT
/// AND RETURN. Recomputing a schedule on a plugin's thread, while it waits,
/// is the bug that ADR exists to prevent.
class ClapHostGlue {
public:
    ClapHostGlue();

    [[nodiscard]] const clap_host_t* host() noexcept { return &host_; }

    [[nodiscard]] std::uint64_t restartRequests() const noexcept { return restarts_; }
    [[nodiscard]] std::uint64_t processRequests() const noexcept { return processes_; }
    [[nodiscard]] std::uint64_t callbackRequests() const noexcept { return callbacks_; }

private:
    static const void* getExtension(const clap_host_t*, const char* id);
    static void requestRestart(const clap_host_t*);
    static void requestProcess(const clap_host_t*);
    static void requestCallback(const clap_host_t*);

    clap_host_t host_{};
    std::uint64_t restarts_ = 0, processes_ = 0, callbacks_ = 0;
};

}  // namespace adi::device
