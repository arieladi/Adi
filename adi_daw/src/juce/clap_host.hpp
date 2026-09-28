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
#include "adi/engine/mpe_output.hpp"
#include "juce/device_model.hpp"

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
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
// --- ADR-0099: note dialects --------------------------------------------------

/// What a plugin's notes are sent as.
///
/// CLAP lets a plugin DECLARE this, per note port: `supported_dialects` and a
/// `preferred_dialect`. A host that sends a dialect the plugin did not declare
/// is sending events the plugin is entitled to ignore -- a MIDI-only plugin
/// handed CLAP_EVENT_NOTE_ON plays nothing. So unlike VST3 (ADR-0097), where
/// `Auto` usually has nothing to go on, here it reads the answer.
enum class ClapDialect : std::uint8_t {
    Clap,      ///< CLAP_EVENT_NOTE_* and note expressions: doubles, by note id
    MidiMpe,   ///< raw MIDI with MPE: ADR-0097's MpeMidi route
    Midi,      ///< raw MIDI on one channel: ADR-0097's Plain route
    None,      ///< the plugin declares no note input
};

/// What the user asked for. An explicit choice the plugin does not declare
/// falls back to `Auto` -- never an undeclared dialect.
enum class ClapDialectChoice : std::uint8_t { Auto = 0, Clap = 1, MidiMpe = 2, Midi = 3 };

/// What `clap.note-ports` declared for the plugin's first input port.
struct ClapNotePorts {
    bool extension = false;       ///< the plugin implements clap.note-ports at all
    bool input = false;           ///< and declares at least one input port
    std::uint32_t supported = 0;  ///< CLAP_NOTE_DIALECT_* bits
    std::uint32_t preferred = 0;
};

/// The dialects this host speaks; `clap_host_note_ports.supported_dialects`.
/// MIDI 2.0 is not among them.
inline constexpr std::uint32_t kClapHostDialects =
    CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;

/// The dialect a plugin gets.
///   * no clap.note-ports at all -> Clap: the spec's preferred encoding, and
///     what this host sent every plugin before ADR-0099.
///   * the extension, but no input port -> None: it takes no notes.
///   * an explicit choice the plugin declares -> that.
///   * otherwise its preferred dialect when we speak it, else the first it
///     declares of Clap, MidiMpe, Midi -- else None (MIDI 2.0 only).
[[nodiscard]] ClapDialect resolveClapDialect(ClapDialectChoice choice,
                                             const ClapNotePorts& ports) noexcept;

/// The ADR-0097 route each dialect runs through `engine::MpeRouter`.
[[nodiscard]] constexpr engine::ExpressionRoute routeFor(ClapDialect d) noexcept {
    switch (d) {
        case ClapDialect::MidiMpe: return engine::ExpressionRoute::MpeMidi;
        case ClapDialect::Midi:    return engine::ExpressionRoute::Plain;
        case ClapDialect::Clap:
        case ClapDialect::None:    break;
    }
    return engine::ExpressionRoute::NoteExpression;
}

class ClapEventList {
public:
    void reserve(std::int32_t n);
    void clear() noexcept { events_.clear(); }

    [[nodiscard]] std::int64_t dropped() const noexcept { return dropped_; }

    /// Events refused because their frame fell outside the segment.
    ///
    /// COUNTED SEPARATELY FROM A CAPACITY DROP, and counted at all, which it
    /// was not. An event the scheduler assigned to this segment that lands
    /// outside it means the two sides disagree about the coordinate system --
    /// ADR-0078's bug. Returning false and saying nothing turned that into
    /// "some events went missing", which is the shape of defect this project
    /// keeps finding weeks late.
    [[nodiscard]] std::int64_t outOfRange() const noexcept { return outOfRange_; }
    [[nodiscard]] std::int32_t size() const noexcept {
        return static_cast<std::int32_t>(events_.size());
    }

    /// Translate one ADI event.
    ///
    /// `blockOffset` is where this SEGMENT starts inside the block, and it is
    /// subtracted because the two sides use different origins:
    /// `engine::Event::frame` is block-relative (events.hpp says so, and it
    /// must be — a segment-relative frame would change every time the
    /// scheduler split differently), while a plugin handed one segment wants
    /// offsets inside that segment. Same trap as ADR-0078, one layer up, and
    /// it does not show in a block with a single segment.
    ///
    /// False when the event has no CLAP form, which is not an error, or when
    /// it lands outside this segment, which is the caller passing an event
    /// the graph did not assign here.
    bool add(const engine::Event& e, std::int32_t blockOffset = 0,
             std::int32_t segmentFrames = 0) noexcept;

    /// One output of `engine::MpeRouter`, in the plugin's dialect (ADR-0099):
    /// CLAP note and note-expression events, or CLAP_EVENT_MIDI bytes. The
    /// same offset rule and counting as `add`, which now uses this for notes.
    bool addOut(const engine::MpeOut& o, ClapDialect dialect, std::int32_t blockOffset = 0,
                std::int32_t segmentFrames = 0) noexcept;

    /// Stable, by time. CLAP requires a plugin's input events in time order,
    /// and a list built from several sources -- the route, the graph's
    /// parameters, the queued ones -- is not, until this runs. Insertion
    /// sort: short, nearly sorted, and allocation-free.
    void sortByTime() noexcept;

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
        clap_event_midi_t            midi;
    };

    std::vector<Slot> events_;
    std::int32_t cap_ = 0;
    std::int64_t dropped_ = 0;
    std::int64_t outOfRange_ = 0;
    clap_input_events_t in_{this, &ClapEventList::sizeCb, &ClapEventList::getCb};
};

/// One CLAP plugin behind the format-agnostic contract.
///
/// Deliberately the same shape as `Vst3Device`: both are `DeviceInstance`, and
/// `DeviceNode` wraps either without knowing which. That is ADR-0052 decision
/// 4 holding — hybrid tracks, modulation, suspension and delay compensation
/// exist once, against `engine::Node`.
class ClapHostGlue;

class ClapDevice final : public DeviceInstance {
public:
    /// Takes an already-created plugin. Ownership: `destroy()` is called on
    /// release. The `clap_host_t` handed to `create_plugin` must outlive this.
    ClapDevice(const clap_plugin_t* plugin, DeviceIdentity id);
    ~ClapDevice() override;

    [[nodiscard]] const DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return plugin_ != nullptr; }

    /// ADR-0091: an instrument consumes the note stream.
    [[nodiscard]] engine::EventFlow eventFlow() const noexcept override {
        return instrument_ ? engine::EventFlow::Consume : engine::EventFlow::Through;
    }

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

    /// Re-read the list on `CLAP_PARAM_RESCAN_ALL`, WITHOUT renumbering.
    /// Indices are held outside this object (`ParamEdit::paramIndex`,
    /// `ParamOps`' mirror), so the list is reconciled by `clap_id`: a known
    /// id keeps its index, a vanished one is kept and marked `missing`
    /// (ADR-0177 d4), a new one is appended. ADR-0179.
    void rescanParamsPreservingIndices();

    /// ADR-0142. The host this plugin was created against, for its
    /// state-signal count. `ClapHost::makeDevice` sets it; a device built
    /// by hand without one reports 0 and never signals.
    void adoptGlue(std::unique_ptr<ClapHostGlue> glue) noexcept {
        glue_ = std::move(glue);
    }
    /// This device's own host glue, or null for a device built without one.
    /// Every counter on it belongs to THIS plugin (ADR-0179); before that,
    /// they were shared by every plugin on the host and no signal could be
    /// traced to the device that sent it.
    [[nodiscard]] ClapHostGlue* glue() const noexcept { return glue_.get(); }
    [[nodiscard]] std::uint64_t stateEpoch() const noexcept override;

    /// ADR-0179. These read THIS plugin's own glue, so `DeviceHost` can
    /// register three sources per DEVICE through the contract instead of
    /// three per HOST. Before the per-instance glue they could only have
    /// been host-wide, which is why `watchClapGlue` existed: one plugin's
    /// port rescan rebuilt the graph for every plugin, and one plugin's
    /// state signal made every CLAP device save and hash its chunk.
    [[nodiscard]] std::uint64_t shapeEpoch() const noexcept override;
    [[nodiscard]] std::uint64_t restartEpoch() const noexcept override;

    /// MESSAGE THREAD. Runs this plugin's deferred `on_main_thread` work and
    /// answers any `params.request_flush()` it asked for (ADR-0179).
    void pumpMainThread() override;

private:
    /// What the plugin declares RIGHT NOW, asked rather than remembered.
    /// Used by `prepare` to decide whether reactivating is necessary at all.
    [[nodiscard]] std::vector<std::int32_t> declaredChannels(bool isInput) const;
    [[nodiscard]] bool layoutMatches() const;

    static bool outPush(const clap_output_events_t*, const clap_event_header_t*);
    /// Our index for a plugin's `clap_id`, or -1. Audio thread: a linear
    /// scan over the parameter list, no allocation.
    [[nodiscard]] std::int32_t indexOfParam(clap_id id) const noexcept;
    /// True when this event addresses a parameter the plugin no longer
    /// declares (ADR-0177 d4). Audio thread; a linear scan, no allocation.
    [[nodiscard]] bool addressesAMissingParam(const engine::Event& e) const noexcept;
    /// A device-addressed ParamValue/ParamMod converted from the wire's
    /// NORMALIZED unit to the PLAIN value CLAP expects (ADR-0196). Anything
    /// else is returned unchanged. Audio thread; no allocation.
    [[nodiscard]] engine::Event toPlainUnits(const engine::Event& e) const noexcept;

public:

    /// Events for the NEXT block, in block-relative frames. Called from the
    /// graph before `process`, or by whatever produced them.
    ///
    /// This is the zero-truncation path. `engine::Event` carries a double and
    /// CLAP's note expression takes a double in the same unit, so nothing in
    /// between rounds, scales or clamps except at CLAP's own declared range.
    /// Frames are BLOCK-relative, matching `engine::Event`. Held until the
    /// next `process`, which converts them along with the graph's own
    /// `io.events` and clears them.
    ///
    /// For callers with no graph — the probe, and tests. A node inside a
    /// graph does not need this: `io.events` already carries exactly the
    /// events the scheduler assigned to the segment being processed.
    bool pushEvent(const engine::Event& e) noexcept;

    /// `start_processing` calls that returned false (ADR-0123). Such a plugin
    /// is active but not processing, and `process` passes audio through
    /// rather than calling it: plugin.h makes `process` legal only while
    /// processing, and a start that failed says the plugin is not.
    [[nodiscard]] std::int64_t startFailures() const noexcept { return startFailures_; }

    /// Events refused because a queue was full. Non-zero means the capacity
    /// derived at prepare was too small for what arrived (ADR-0056).
    [[nodiscard]] std::int64_t eventsDropped() const noexcept {
        return events_.dropped() + pendingDropped_;
    }

    /// Events whose frame fell outside the segment they were handed with.
    /// Non-zero means a coordinate-system disagreement, not a busy block.
    [[nodiscard]] std::int64_t eventsOutOfRange() const noexcept {
        return events_.outOfRange();
    }

    /// How many blocks have been processed, and the plugin's last answer.
    [[nodiscard]] std::int64_t steadyTime() const noexcept { return steadyTime_; }

    // --- ADR-0099: note dialects -------------------------------------------

    /// Ask for a dialect. Any thread; applied at the start of the next process
    /// call, after every sounding note is ended in the dialect it began in.
    void setNoteDialect(ClapDialectChoice c) noexcept {
        requestedDialect_.store(static_cast<std::uint8_t>(c), std::memory_order_release);
    }
    /// The dialect in use now. Any thread.
    [[nodiscard]] ClapDialect noteDialect() const noexcept {
        return static_cast<ClapDialect>(dialectInUse_.load(std::memory_order_acquire));
    }
    /// What the plugin declared. Read at construction and at every activation
    /// -- the spec allows the scan only while the plugin is deactivated.
    [[nodiscard]] const ClapNotePorts& notePorts() const noexcept { return notePorts_; }
    /// The router, for its counters. Audio thread, or while not processing.
    [[nodiscard]] const engine::MpeRouter& noteRouter() const noexcept { return router_; }

    /// `clap_id` is a uint32; `plugin_params.param_id` is TEXT. The conversion
    /// is fixed-width hex so it sorts stably and cannot collide with a Pd
    /// symbol or a VST3 id in the same column.
    /// The raw plugin, for a host that needs an extension this class does
    /// not wrap yet. Null when nothing loaded.
    [[nodiscard]] const void* rawPlugin() const noexcept { return plugin_; }

    [[nodiscard]] static std::string paramIdToText(clap_id id);
    [[nodiscard]] static bool paramIdFromText(const std::string& s, clap_id& out) noexcept;

private:
    /// The plugin's parameter list as it declares it right now.
    void readParamsInto(std::vector<ParamDescriptor>& out,
                        std::vector<clap_id>& ids) const;
public:

private:
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* paramsExt_ = nullptr;
    const clap_plugin_state_t* stateExt_ = nullptr;
    /// ADR-0179: OWNED, one per plugin instance. `host_data` on the
    /// `clap_host_t` inside it is what makes every callback attributable to
    /// this device -- which is the whole of ADR-0123 item 6 ("C4").
    ///
    /// Owned by the device rather than by `ClapHost` so that the glue cannot
    /// outlive the plugin it describes: they are destroyed together, in this
    /// object's destructor, in that order.
    std::unique_ptr<ClapHostGlue> glue_;

    /// The value of `glue_->portChanges()` at this device's last activation.
    /// `prepare` reactivates when it has moved -- ADR-0179's legal
    /// replacement for reading the port layout off an active plugin.
    std::uint64_t shapeSeen_ = 0;

    /// `glue_->flushRequests()` already answered. Message thread only.
    std::uint64_t flushSeen_ = 0;
    /// `glue_->paramListRescans()` already acted on. Message thread only.
    std::uint64_t listRescanSeen_ = 0;
    const clap_plugin_tail_t* tailExt_ = nullptr;
    const clap_plugin_latency_t* latencyExt_ = nullptr;

    /// CLAP_PLUGIN_FEATURE_INSTRUMENT, read once from the descriptor at
    /// construction. `eventFlow` is on the audio thread and must not walk a
    /// string array per block (ADR-0091).
    bool instrument_ = false;

    DeviceIdentity id_;
    std::vector<ParamDescriptor> params_;
    std::vector<clap_id> paramIds_;
    bool activated_ = false;
    bool processing_ = false;          ///< start_processing succeeded (ADR-0123)
    std::int64_t startFailures_ = 0;
    double sampleRate_ = 0.0;
    std::int32_t maxFrames_ = 0;

    /// A CLAP parameter is set by an EVENT, not a setter, so `setParam`
    /// queues one and the next process block carries it. Pooled at prepare
    /// and never allocated on the audio thread (ADR-0010); an overflow is
    /// counted, like every other queue in this project.
    struct PendingParam { clap_id id; double value; };
    std::vector<PendingParam> pending_;
    std::size_t pendingUsed_ = 0;
    std::int64_t pendingDropped_ = 0;

    /// Audio, allocated at prepare. CLAP wants an array of channel pointers
    /// per bus; the graph hands separate in and out pointers (ADR-0045's port
    /// pair), so the bridging happens here.
    /// THE PLUGIN'S OWN BUS LAYOUT, queried rather than assumed.
    ///
    /// The first version hardcoded one input bus and one output bus. Every
    /// plugin tested disagrees: Pro-Q 3 has TWO inputs (main + sidechain),
    /// Vital has ZERO, Surge XT has THREE outputs. A plugin indexes
    /// `audio_inputs[i]` up to the count it declared, so passing 1 when it
    /// declares 2 reads past the end of the host's array -- which is how
    /// Pro-Q 3 crashed inside its own `process`.
    ///
    /// It appeared to work standalone because the object next to it on the
    /// stack was readable. That is what undefined behaviour looks like when
    /// it is being polite.
    struct Bus {
        std::int32_t channels = 0;
        std::vector<float> storage;      ///< channels * maxFrames
        std::vector<float*> ptrs;
    };
    std::vector<Bus> inBuses_, outBuses_;
    std::vector<clap_audio_buffer_t> inBufs_, outBufs_;
    ClapEventList       events_;     ///< rebuilt per process call

    // ADR-0099. The route's output is sized at prepare and reused.
    void readNotePorts();
    void applyDialect(ClapDialectChoice choice);
    const clap_plugin_note_ports_t* notePortsExt_ = nullptr;
    ClapNotePorts notePorts_{};
    ClapDialect dialect_ = ClapDialect::Clap;        ///< audio thread, and prepare
    std::atomic<std::uint8_t> requestedDialect_{0};  ///< ClapDialectChoice
    std::uint8_t appliedDialect_ = 0;                ///< audio thread, and prepare
    std::atomic<std::uint8_t> dialectInUse_{0};      ///< ClapDialect
    engine::MpeRouter router_;
    std::vector<engine::MpeOut> routedStore_;
    engine::MpeOutList routed_;
    std::vector<engine::Event> injected_;   ///< from pushEvent, block-relative
    std::size_t injectedUsed_ = 0;
    clap_output_events_t outEvents_{};
    std::int64_t steadyTime_ = 0;
    std::int32_t channels_ = 2;
};

/// One loaded `.clap` bundle.
///
/// A CLAP plugin is a shared library exporting one symbol, `clap_entry`, and
/// that is the whole of the loading protocol — no SDK, no registry, no
/// framework. ADR-0075 listed this as unbuilt; it is about eighty lines.
///
/// RAII because the entry point must be `deinit`'d and the library closed
/// AFTER every plugin from it is destroyed. Getting that order wrong calls a
/// destructor through a function pointer in unmapped memory, which is a crash
/// with a stack trace pointing at nothing.
class ClapLibrary {
public:
    ClapLibrary() = default;
    ~ClapLibrary();
    ClapLibrary(const ClapLibrary&) = delete;
    ClapLibrary& operator=(const ClapLibrary&) = delete;

    /// `path` is the bundle (macOS) or the library (elsewhere). Returns false
    /// and sets `error` rather than throwing, because a plugin that will not
    /// load is ADR-0011's ordinary case and not an exception.
    bool open(const std::string& path, std::string& error);
    void close();

    [[nodiscard]] bool isOpen() const noexcept { return entry_ != nullptr; }
    [[nodiscard]] std::uint32_t pluginCount() const noexcept;
    [[nodiscard]] const clap_plugin_descriptor_t* descriptorAt(std::uint32_t i) const noexcept;

    /// Create by id. The returned plugin is `init`'d and ready to activate;
    /// null when the factory refused, which a caller turns into ADR-0011's
    /// placeholder rather than an error.
    [[nodiscard]] const clap_plugin_t* create(const clap_host_t* host,
                                              const char* pluginId) const;

private:
    void* lib_ = nullptr;
    const clap_plugin_entry_t* entry_ = nullptr;
    const clap_plugin_factory_t* factory_ = nullptr;
    std::string path_;
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

    // ACQUIRE, because these are written from whatever thread the plugin
    // picked. See the counters below.
    [[nodiscard]] std::uint64_t restartRequests() const noexcept {
        return restarts_.load(std::memory_order_acquire);
    }

    // --- ADR-0084: the cause of a restart, which CLAP does distinguish -----

    /// A latency change. THIS is the coalescer's cheap path (ADR-0082): a
    /// plugin signals `clap_host_latency.changed()` and only then asks for a
    /// restart, so the specific notification arrives BEFORE the generic one.
    [[nodiscard]] std::uint64_t latencyChanges() const noexcept {
        return latencyChanges_.load(std::memory_order_acquire);
    }

    /// A port-layout change that alters SHAPE. Not a tap move -- the graph
    /// is wired differently and has to be rebuilt.
    [[nodiscard]] std::uint64_t portChanges() const noexcept {
        return portChanges_.load(std::memory_order_acquire);
    }

    /// A restart asked for with no preceding notification, so the cause is
    /// unknown. Escalates to a rebuild: a needless rebuild costs a graph
    /// swap, a missed port change plays the wrong channel count.
    [[nodiscard]] std::uint64_t unexplainedRestarts() const noexcept {
        return unexplained_.load(std::memory_order_acquire);
    }

    /// `clap_host_note_ports.rescan(CLAP_NOTE_PORTS_RESCAN_ALL)` calls: the
    /// plugin's note ports, and so possibly its dialects, changed (ADR-0099).
    /// A device re-reads them at its next activation.
    [[nodiscard]] std::uint64_t noteRescans() const noexcept {
        return noteRescans_.load(std::memory_order_acquire);
    }

    /// ADR-0142: `clap_host_params.rescan` with VALUES or ALL, and
    /// `clap_host_state.mark_dirty` -- a plugin saying its state changed
    /// outside its parameter events. THIS plugin's, since ADR-0179 gave each
    /// instance its own glue; `ClapDevice` reports it as its `stateEpoch`.
    /// It was shared by every plugin on the host until then, which made one
    /// plugin's rescan re-save and re-hash every other plugin's chunk.
    [[nodiscard]] std::uint64_t stateSignals() const noexcept {
        return stateSignals_.load(std::memory_order_acquire);
    }
    /// Signals that arrived inside our own `loadState`, and were not counted.
    [[nodiscard]] std::uint64_t mutedStateSignals() const noexcept {
        return mutedStateSignals_.load(std::memory_order_acquire);
    }
    /// `clap_host_params.request_flush` calls. Counted, not yet answered:
    /// before ADR-0142 a plugin could not ask at all.
    [[nodiscard]] std::uint64_t flushRequests() const noexcept {
        return flushRequests_.load(std::memory_order_acquire);
    }
    /// `clap_host_params.rescan(CLAP_PARAM_RESCAN_ALL)` calls: the parameter
    /// LIST itself may have changed. Separate from `stateSignals()` because
    /// the responses differ -- a state signal means re-save the chunk, this
    /// means re-read the declarations (ADR-0179).
    [[nodiscard]] std::uint64_t paramListRescans() const noexcept {
        return paramListRescans_.load(std::memory_order_acquire);
    }

    /// Parameters the plugin asked the host to drop references to, with the
    /// flags it asked under (ADR-0179). Drained by the device on the main
    /// thread; `clear` itself only records, because the response belongs to
    /// the device that owns the parameter list, not to the glue.
    struct ClearRequest {
        clap_id id = 0;
        clap_param_clear_flags flags = 0;
    };
    [[nodiscard]] std::vector<ClearRequest> takeClearRequests();
    /// MAIN THREAD. Our own `loadState` brackets itself with these, so the
    /// rescan a plugin answers a load with is not taken for the user's.
    void muteStateSignals() noexcept { ++muteDepth_; }
    void unmuteStateSignals() noexcept { if (muteDepth_ > 0) --muteDepth_; }

    /// True when a plugin deferred work to the main thread and nothing has
    /// run it yet.
    [[nodiscard]] bool mainThreadWorkPending() const noexcept {
        return callbacks_.load(std::memory_order_acquire) != dispatched_;
    }

    /// MESSAGE THREAD. Runs THIS plugin's deferred work. Without it a CLAP
    /// plugin that defers work never runs it, and nothing reports that -- the
    /// plugin simply does less than it was written to do.
    void dispatchMainThread();

    /// The plugin this glue serves. Called once, by `ClapHost::makeDevice`,
    /// between `create` and handing the glue to the `ClapDevice` that owns it.
    void registerPlugin(const clap_plugin_t* p);
    /// Forget it. `~ClapDevice` calls this before `plugin_->destroy`, so a
    /// glue that outlives its plugin by any margin holds nullptr and not a
    /// dangling pointer (ADR-0179).
    void unregisterPlugin(const clap_plugin_t* p);
    [[nodiscard]] std::uint64_t processRequests() const noexcept {
        return processes_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t callbackRequests() const noexcept {
        return callbacks_.load(std::memory_order_acquire);
    }

private:
    static const void* getExtension(const clap_host_t*, const char* id);
    static void latencyChanged(const clap_host_t*);
    static void portsRescan(const clap_host_t*, std::uint32_t flags);
    static bool portsRescanSupported(const clap_host_t*, std::uint32_t flag);
    static std::uint32_t noteDialects(const clap_host_t*);
    static void noteRescan(const clap_host_t*, std::uint32_t flags);
    static void paramsRescan(const clap_host_t*, clap_param_rescan_flags flags);
    static void paramsClear(const clap_host_t*, clap_id, clap_param_clear_flags);
    static void paramsRequestFlush(const clap_host_t*);
    static void stateMarkDirty(const clap_host_t*);
    void stateSignal() noexcept;
    static void requestRestart(const clap_host_t*);
    static void requestProcess(const clap_host_t*);
    static void requestCallback(const clap_host_t*);

    clap_host_t host_{};
    clap_host_params_t      hostParamsExt_{};   ///< ADR-0142
    clap_host_state_t       hostStateExt_{};    ///< ADR-0142
    std::atomic<std::uint64_t> stateSignals_{0};
    std::atomic<std::uint64_t> mutedStateSignals_{0};
    std::atomic<std::uint64_t> flushRequests_{0};
    std::atomic<std::uint64_t> paramListRescans_{0};
    /// MAIN THREAD only: params.h annotates `clear` `[main-thread]`, so this
    /// needs no lock and no atomic.
    std::vector<ClearRequest> clears_;
    int muteDepth_ = 0;                          ///< main thread only
    clap_host_latency_t     latencyExt_{};
    clap_host_audio_ports_t portsExt_{};
    clap_host_note_ports_t  notePortsExt_{};
    std::atomic<std::uint64_t> noteRescans_{0};
    std::atomic<std::uint64_t> latencyChanges_{0};
    std::atomic<std::uint64_t> portChanges_{0};
    std::atomic<std::uint64_t> unexplained_{0};
    std::uint64_t dispatched_ = 0;
    /// THE plugin this glue serves, not a list. ADR-0179.
    ///
    /// This was a `std::vector<const clap_plugin_t*>` when one glue served
    /// every plugin on a host, and that vector was a use-after-free:
    /// `makeDevice` pushed the raw pointer, `~ClapDevice` called
    /// `plugin_->destroy`, nothing ever called `unregisterPlugin`, and
    /// `dispatchMainThread` then called `on_main_thread` through freed
    /// memory. Reproduced under AddressSanitizer with a real ADI Airwindows
    /// suite: load, destroy, one timer tick.
    ///
    /// It is a single pointer rather than a vector with one element because
    /// a container invites the broadcast loop back, and the broadcast loop
    /// is the bug. One glue, one plugin, and `unregisterPlugin` has nothing
    /// left to forget to call.
    ///
    /// What actually closes the bug is OWNERSHIP, not the unregister call:
    /// `ClapDevice` holds its glue by value-semantics `unique_ptr`, so the
    /// two die together and no dispatch can reach a destroyed plugin. The
    /// reproduction that used to report a heap-use-after-free now runs clean
    /// (`adi_clap_probe`'s sibling under ASan, a real ADI Airwindows suite).
    const clap_plugin_t* plugin_ = nullptr;
    // ATOMIC, because `requestRestart` says in its own comment that the plugin
    // may call it from any thread -- and a plain `++` from an arbitrary thread,
    // read from the message thread, is a data race whatever the width. On the
    // 32-bit CI job a 64-bit non-atomic read can also tear, so the counter
    // could be observed as a value it never held.
    std::atomic<std::uint64_t> restarts_{0}, processes_{0}, callbacks_{0};
};

/// One plugin a scan found, before it is instantiated.
///
/// Deliberately the same shape as `juce::PluginDescription` is used for VST3:
/// enough to put a row in a browser and to instantiate later, and nothing
/// that requires the plugin to be loaded.
struct ClapPluginRef {
    std::string bundlePath;   ///< the .clap, which is what `ClapLibrary::open` wants
    std::string id;           ///< `clap_plugin_descriptor.id`, stable and reverse-DNS
    std::string name;
    std::string vendor;
    std::string version;
    std::string features;     ///< joined `clap_plugin_descriptor.features`

    /// True when the descriptor declares CLAP_PLUGIN_FEATURE_INSTRUMENT.
    /// A browser needs it, and so does anything asserting "a note should
    /// make sound" -- an effect handed no input is correctly silent.
    bool isInstrument = false;
};

/// The sibling of `Vst3Host`: scan, enumerate, instantiate.
///
/// LIFETIME, which is the sharp edge and is not obvious. A `ClapDevice` holds
/// a `clap_plugin_t*` that lives inside a `ClapLibrary`'s loaded image. The
/// library must therefore outlive every device made from it — `dlclose` while
/// a plugin is alive unmaps the code its destructor is about to run.
///
/// So the host OWNS the libraries, keyed by path, and never closes one while
/// it is open. A caller keeps the host alive for as long as any device it
/// made. That is stated here because the compiler cannot say it.
///
/// No JUCE: CLAP needs none, so this scans, loads and instantiates on every
/// ABI the suite runs on. `Vst3Host` cannot.
class ClapHost {
public:
    ClapHost();

    /// Where CLAP plugins live, per the format's own convention, plus
    /// `CLAP_PATH` when it is set. Read rather than hardcoded to one OS.
    [[nodiscard]] static std::vector<std::string> defaultSearchPaths();

    /// Every `.clap` under `paths`, searched RECURSIVELY -- CLAP's entry.h:
    /// "Each directory should be recursively searched". Sorted, without
    /// duplicates (CLAP_PATH may repeat a default). A `.clap` that is a
    /// directory is a macOS bundle: it is a candidate, and it is not entered.
    /// Pure filesystem; nothing is loaded.
    ///
    /// One level was searched until ADR-0098, which is every CLAP on Windows
    /// whose installer uses a vendor folder -- Surge XT's does,
    /// `CLAP\Surge Synth Team\Surge XT.clap` -- found by nothing.
    [[nodiscard]] static std::vector<std::string> findBundles(
        const std::vector<std::string>& paths);

    /// `findBundles`, then read each bundle's factory. Opening a bundle runs
    /// its `init`, so a scan is not free and is not a loop to put on a timer.
    void scan(const std::vector<std::string>& paths);

    [[nodiscard]] const std::vector<ClapPluginRef>& plugins() const noexcept {
        return found_;
    }

    /// ADR-0011: never null. A plugin that cannot be instantiated becomes a
    /// `MissingDevice` carrying the same identity, so the chain keeps its
    /// shape and the user is told what is missing.
    std::unique_ptr<DeviceInstance> makeDevice(const ClapPluginRef& ref,
                                               double sampleRate,
                                               std::int32_t blockSize,
                                               std::string& error);

    /// The glue every plugin from this host reports through. Registered once
    /// with `DeviceHost::watchClapGlue`, because `clap_host_latency.changed`
    /// is a HOST callback and belongs to the host object (ADR-0084).
    /// GONE with ADR-0179, deliberately, and not replaced by a forwarder.
    ///
    /// There is no host-wide glue any more: each `ClapDevice` owns its own,
    /// and a host-level one would answer every counter with a zero it could
    /// never leave -- a reading that looks like "nothing happened" and means
    /// "nobody is asking me". Ask the device: `ClapDevice::glue()`.
    ///
    /// [[nodiscard]] ClapHostGlue& glue() noexcept;

    [[nodiscard]] std::size_t libraryCount() const noexcept { return libs_.size(); }

private:
    ClapLibrary* libraryFor(const std::string& path, std::string& error);


    std::vector<ClapPluginRef> found_;
    std::vector<std::pair<std::string, std::unique_ptr<ClapLibrary>>> libs_;
};

}  // namespace adi::device
