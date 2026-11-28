// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session: a project, live. ADR-0122 -- roadmap step 6 opens here.
//
// Every part of this existed before this file and nothing joined them up. A
// `.adi` could be read into a `rows::Model` (ADR-0090's projection), a model
// could be planned and realised into a `Graph` (ADR-0077), a `GraphHost` could
// publish that graph to an audio thread and replace it (ADR-0089), a
// `DeviceHost` could hold plugin instances and answer their restart requests
// (ADR-0090), and a `DeviceBridge` could turn a driver callback into
// `BlockProcessor::process` (ADR-0049). What did not exist was the object that
// owns all of them for the lifetime of an open project and is the ONE thing a
// device callback talks to. This is it.
//
// THREE DECISIONS, each of which the pieces below it did not make:
//
//   1. **The rows place devices; the host only holds them.** `DeviceHost::add`
//      takes a track id and `chainSupplier()` places by it, which is right for
//      a probe and wrong for a project: a device's position is
//      `devices.chain_id` and `devices.ord`, and an edit moves it without
//      re-instantiating anything. So every instance is added UNPLACED and the
//      session answers `RealizeOptions::devicesFor` from the model, in row
//      order. A rebuild after `refresh` therefore follows the rows, and the
//      rows are the truth (ADR-0003).
//   2. **A plugin that will not load is a placeholder, never a gap.** ADR-0011
//      and SPEC §7.1: the device stays in the chain, bypassed, carrying its
//      `plugin_params` as answers and its `plugin_state` bytes untouched. The
//      chain's shape -- what feeds what, where the compensation goes -- is the
//      same on the machine that has the plugin and the one that does not.
//   3. **A format change is a REBUILD, not a reload.** `prepare` at a new rate
//      or block size builds a new graph at that size and publishes it; the same
//      instances are re-injected and re-prepared (ADR-0042 d5, honoured by
//      ADR-0090 d5's guard). `prepare` at the SAME format, on a graph that is
//      live, does nothing at all -- a driver that restarts without changing
//      anything must not cause a seam.
//
// THREADS. `load`, `refresh`, `rebuild`, `prepare`, `release` and `tick` are
// message-thread calls; `prepare` and `release` arrive from whatever thread
// opens and closes the device, which JUCE keeps off the audio thread. `process`
// is the audio thread and touches nothing the others write: it reads the
// published graph through `GraphHost`, whose handoff is ADR-0010's.
#pragma once

#include "adi/engine/host.hpp"
#include "adi/engine/session_runtime.hpp"
#include "adi/engine/gesture_preview.hpp"
#include "adi/applied_batches.hpp"
#include "adi/engine/gain_stage.hpp"
#include "adi/engine/main_loudness.hpp"
#include "adi/engine/live_midi_input.hpp"
#include "adi/engine/input_gain.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/engine/warp_arrangement_clock.hpp"
#include "adi/engine/clip_playback.hpp"
#include "adi/engine/midi_clips.hpp"
#include "adi/engine/mixer.hpp"
#include "adi/engine/cue_monitor.hpp"
#include "adi/engine/scope.hpp"
#include "adi/engine/summing.hpp"
#include "adi/engine/param_automation.hpp"
#include "adi/engine/process.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/engine/realize.hpp"
#include "adi/store_rows.hpp"
#include "juce/device_host.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace adi {
class Store;
}

namespace adi::audio { class ProgressiveDecoder; }
namespace adi::engine {

class AudioInputRecorder;
struct AudioRecordingSpec;
class MidiInputCapture;
struct RecordingTarget;

/// The format the session is asked to run at. `load` takes the expected one;
/// `prepare` replaces it with what the driver actually granted.
struct SessionSpec {
    std::int32_t channels = 2;
    double sampleRate = 48000.0;
    std::int32_t maxFrames = 512;
    /// `audio.processingPrecision` (ADR-0207 d2): Live's scheme by default.
    Precision precision = Precision::Float32;
    /// Live performance controls (saved loop/click). Offline export/analysis
    /// disables these so monitor choices never alter rendered material.
    bool performanceState = true;
    /// The app's Delay Compensation choice; its registry row is win-owned.
    bool delayCompensation = true;
    /// Optional app-owned decode service/cache. Must outlive this session and
    /// retired generations. Null uses the process service; never a callback API.
    audio::ProgressiveDecoder* progressiveDecoder = nullptr;
    /// Owner-selected performance clock, e.g. Link. A new prepared generation
    /// uses this constant tempo for ALL musical playback and clock consumers;
    /// authored project tempo rows remain intact. Not an offline export option.
    std::optional<double> externalTempoBpm = std::nullopt;
    /// Export quality is independent of monitoring/performance-state suppression.
    bool exporting = false;
    /// Processing without a realtime deadline, even at the normal quality.
    bool offline = false;
    /// An offline driver prepares clip pages itself between process calls.
    bool synchronousClipReads = false;
};

/// What a loader is asked for: the row, the identity it names, and the format
/// the session expects to run at. `ref` is null when the row has no
/// `plugin_ref_id`, and the session does not call the loader then.
struct DeviceRequest {
    const rows::Device* device = nullptr;
    const rows::PluginRef* ref = nullptr;
    double sampleRate = 48000.0;
    std::int32_t maxFrames = 512;
    std::int32_t channels = 2; ///< explicit prepared main-bus width, including offline loads
    bool exporting = false; ///< known before prepare, so the first block uses the intended bank
};

/// Turns a request into a live instance, or returns null with `error` set.
/// Injected for the same reason `DeviceChainFn` is: this file compiles on
/// every ABI and cannot construct a `Vst3Device`. A missing loader is the
/// headless case, and every row then becomes a placeholder.
using DeviceLoader = std::function<std::unique_ptr<device::DeviceInstance>(
    const DeviceRequest&, std::string& error)>;

/// The `param_ref` an automation lane on a device parameter carries, from the id
/// the plug-in spells (ADR-0165: lane refs stay decimal for every format, so a
/// CLAP id's hex becomes its decimal; every other format's id is its ref as it
/// stands). Empty for a CLAP id that is not hex. The UI's lookup of a
/// parameter's lane goes through this, as the session's own does.
std::optional<std::string> automationParamRef(const std::string& format, const std::string& id);

enum class OfflineTap { PreFader, PostFader };
struct OfflineOutput {
    NodeId node = kInvalidNode;
    std::int64_t latency = 0;
};
struct OfflineDeviceInfo {
    std::int64_t id = 0;
    device::DeviceIdentity identity;
};
struct OfflineSourceInfo {
    std::int64_t track = 0;
    std::vector<OfflineDeviceInfo> devices;  // actual loaded chain order / versions
    std::vector<std::int64_t> prefixDevices; // through the last MIDI consumer
};

class Session final : public BlockProcessor {
public:
    /// One device row the session resolved, placeholder or not.
    struct Entry {
        std::int64_t deviceId = 0;
        std::int64_t trackId = 0;      ///< the chain's track; 0 once retired
        std::string name;
        bool placeholder = false;      ///< a `MissingDevice` stands in (ADR-0011)
        bool parked = false;           ///< freeze retirement: restore a fresh instance on unfreeze
        bool retired = false;          ///< the row is gone; the instance is kept
        std::string error;             ///< why the loader refused, when it did
        std::size_t hostIndex = 0;     ///< into `devices()`
        /// Identity used to resolve this instance, independent of newer rows
        /// reusing the same device ID. Runtime receipts must match this too.
        std::optional<std::int64_t> resolvedPluginRef;
        std::string resolvedFormat, resolvedUid;
        std::shared_ptr<const void> runtimeIdentity;
        std::uint8_t route = 0;        ///< the RouteChoice last asked of the instance (ADR-0149)
    };

    struct Stats {
        std::int64_t loaded = 0;         ///< instances the loader produced
        std::int64_t placeholders = 0;   ///< rows kept as `MissingDevice`
        std::int64_t skipped = 0;        ///< racks and their nested chains (ADR-0060)
        std::int64_t retired = 0;        ///< rows that left the project after `load`
        std::int64_t statesLoaded = 0;   ///< `loadState` calls that succeeded
        std::int64_t paramsApplied = 0;  ///< `setParam` calls made from `plugin_params`
        std::int64_t paramsMatched = 0;  ///< rows a loaded chunk already agreed with (ADR-0142)
        std::int64_t routesApplied = 0;  ///< recorded routes a device took (ADR-0149)
        std::int64_t routesRefused = 0;  ///< recorded routes a device could not take
        std::int64_t rebuilds = 0;       ///< graphs this session asked for
        std::int64_t prepares = 0;       ///< `prepare` calls, including no-ops
        std::int64_t formatChanges = 0;  ///< prepares that changed rate or size
    };

    Session();
    ~Session() override;

    // --- message thread -----------------------------------------------------

    /// Read the project, resolve every device row, build the first graph at
    /// `spec`. False with `error()` set when no graph could be built; the
    /// devices resolved so far are kept, so a caller can still report them.
    bool load(const Store& store, DeviceLoader loader, SessionSpec spec);

    struct LoadProgress {
        std::size_t completedDevices = 0, totalDevices = 0;
        std::string_view nextDevice;
    };
    /// Stage a candidate on the owner thread. The caller may yield between
    /// device steps or destroy the candidate to cancel. It becomes playable
    /// only after finishLoad publishes its first graph.
    bool beginLoad(const Store&, DeviceLoader, SessionSpec);
    [[nodiscard]] LoadProgress loadProgress() const noexcept;
    bool loadNextDevice(const Store&);
    bool finishLoad(const Store&);

    /// Re-read the project after an edit. Rows that are new are resolved
    /// through the loader; rows that are gone are RETIRED -- left out of every
    /// chain but kept alive, because a graph the audio thread may still be
    /// rendering holds their nodes. Then rebuild.
    bool refresh(const Store& store);
    /// Owner-thread audition, exclusive per track. Zero restores the main lane.
    bool auditionTakeLane(const Store&, std::int64_t track, std::int64_t lane);
    /// Apply a single material edit admitted by the document's current view.
    /// Empty means the ordinary refresh path must handle the edit.
    std::optional<bool> refreshLocalEdit(const Store&, const ProjectView&);

    /// Plan, realise, prepare and publish a new graph from the current model.
    bool rebuild();

    /// SC-318: asks the loader again for every placeholder's plug-in, as a
    /// plug-in scan ends. One that now loads takes the placeholder's place --
    /// its stored state and parameter rows restored, as at open -- and the
    /// placeholder is retired; one still missing stays as it is, its reason
    /// updated. Rebuilds when any woke. Returns how many woke.
    std::size_t retryPlaceholders(const Store& store);
    // nullopt means the latest transaction needs ordinary full synchronisation.
    std::optional<bool> refreshDeviceKeys(const Store&, const std::shared_ptr<const std::vector<std::uint8_t>>& image);

    [[nodiscard]] std::shared_ptr<const SessionEndpoint> endpoint() const noexcept { return endpoint_; }
    [[nodiscard]] DeviceRuntimeToken deviceRuntimeToken(std::int64_t deviceId) const;
    [[nodiscard]] device::DeviceInstance* resolveDevice(const DeviceRuntimeToken&) noexcept;
    void retainRuntimeClient(std::shared_ptr<SessionRuntimeClient>);
    [[nodiscard]] bool presentationReady(const AppliedSource&,std::uint64_t sequence) const noexcept;

    // --- automation override (ADR-0162) -------------------------------------
    // Live 12 25.4: changing an automated control while not recording turns
    // that lane off and the manual value stands. Here a refresh that finds an
    // automated strip value changed -- by a user, an op or the agent -- marks
    // its lane overridden. Session state: not an op, not saved.

    /// True while any lane is overridden: Live's Re-Enable Automation button lit.
    [[nodiscard]] bool automationOverridden() const noexcept { return !overridden_.empty(); }
    [[nodiscard]] const std::set<std::int64_t>& overriddenLanes() const noexcept { return overridden_; }
    /// Follow every overridden lane again, from the playhead at once. Rebuilds.
    bool reenableAutomation();
    /// One lane only: the parameter's context menu in Live. False if that lane
    /// was not overridden.
    bool reenableAutomation(std::int64_t laneId);
    /// Message thread: a live UI value overrides its device lane before the
    /// gesture commits, so playback cannot fight the drag. No op/history row;
    /// reenableAutomation resumes it. True for an absent/already-overridden
    /// lane too; false if rebuilding the requested binding failed (error()).
    bool overrideDeviceAutomation(std::int64_t deviceId, const std::string& paramId);
    /// Host bypass is automatable for loaded audio effects. The message
    /// thread reads the published state and falls back to the stored bit
    /// until an active binding has been sampled by the graph.
    [[nodiscard]] bool deviceActivatorAvailable(std::int64_t deviceId) const noexcept;
    [[nodiscard]] std::optional<bool> deviceActivatorPlaying(std::int64_t deviceId) const noexcept;
    bool overrideDeviceActivator(std::int64_t deviceId);

    // Document-owner recording policy. Queue matching gate changes to each
    // recorder's producer separately; these methods never touch its SPSC ring.
    // Turning a gate off prevents new writes but keeps a pending write suspended
    // until its captured data is committed/refreshed and explicitly finished.
    void setAutomationWriteState(bool armed, bool recording) noexcept;
    /// SC-333: a take is being recorded, so the transport a hosted plug-in is handed says it records
    /// while it plays. Set by the document as its recording starts and ends; read on the audio thread.
    void setRecordingForDevices(bool on) noexcept { recordingForDevices_.store(on, std::memory_order_release); }
    struct DriverClock {
        std::int64_t sample = 0, streamedFrames = 0, loopBegin = 0, loopEnd = 0;
        std::uint64_t revision = 0;
        bool playing = false;
    };
    bool readDriverClock(DriverClock&) const noexcept; // bounded owner observation

    [[nodiscard]] bool automationArmed() const noexcept { return automationArmed_; }
    [[nodiscard]] bool automationRecording() const noexcept { return automationRecording_; }
    [[nodiscard]] const std::set<std::int64_t>& writingAutomationLanes() const noexcept { return writingAutomation_; }
    // As with reenableAutomation(), a rebuild failure leaves the requested
    // policy installed for an owner-side rebuild() retry; false is not readiness.
    bool beginAutomationWrite(std::int64_t laneId);
    // After commit + refresh, follow=true resumes the newly recorded lane.
    // On recording failure, follow=false retains the manual value as a normal
    // override until Re-Enable Automation. This is an explicit caller choice.
    bool finishAutomationWrite(std::int64_t laneId, bool follow = true);

    /// ADR-0165: the device lanes this rebuild plays, per device. Message
    /// thread, for tests and the UI: null until the first rebuild.
    [[nodiscard]] const DeviceAutomationBinding* deviceAutomation() const noexcept {
        return deviceAutomation_.get();
    }

    /// What pushes into each track's junction -- a clip reader, a test tone.
    /// Effective from the next rebuild. The nodes are the caller's and must
    /// outlive every graph that holds them, which means: until the session is
    /// destroyed or a later rebuild without them has been collected.
    void setSourcesFor(SourceFn fn);

    /// Owner-side physical input selection for audio monitoring. Channels
    /// are resolved driver indices, as in AudioRecordingSpec. An empty set
    /// disconnects monitoring inputs. Track Arm and Monitor Mode come from
    /// the published project generation.
    bool setAudioMonitorInputs(std::span<const RecordingTarget>);

    /// Drain plugin callbacks, answer restart requests, free retired graphs.
    /// Returns true when a latency change was applied.
    bool tick(std::int64_t nowMs);

    // --- BlockProcessor -----------------------------------------------------

    /// One driver-scoped physical callback. Every query and logical slice uses
    /// the same published graph and its immutable clock, even if the owner
    /// publishes another generation between slices. Acquiring another handle
    /// expires the older one. The Session must outlive all its driver handles.
    class RenderBlock final {
    public:
        RenderBlock(const RenderBlock&) = delete;
        RenderBlock& operator=(const RenderBlock&) = delete;
        [[nodiscard]] bool valid() const noexcept { return graph_.valid(); }
        [[nodiscard]] std::uint64_t generation() const noexcept { return graph_.generation(); }
        [[nodiscard]] double sampleRate() const noexcept;
        [[nodiscard]] std::int32_t maxFrames() const noexcept;
        [[nodiscard]] Precision precision() const noexcept;
        [[nodiscard]] std::int32_t latencySamples() const noexcept { return graph_.latencySamples(); }
        [[nodiscard]] std::optional<double> externalTempoBpm() const noexcept;
        [[nodiscard]] bool constantExternalTempo() const noexcept;
        [[nodiscard]] std::optional<std::int64_t> loopBeginTick() const noexcept;
        [[nodiscard]] std::optional<std::int64_t> loopEndTick() const noexcept;
        [[nodiscard]] std::int64_t tick() const noexcept;
        [[nodiscard]] std::int64_t tickAtSample(std::int64_t) const noexcept;
        [[nodiscard]] double beatAtSample(std::int64_t) const noexcept;
        [[nodiscard]] std::int64_t sampleAtTick(std::int64_t) const noexcept;
        [[nodiscard]] double bpmAtTick(std::int64_t) const noexcept;
        /// Driver only, immediately before rendering this target. Posts bounded
        /// read-ahead and checks mandatory pages without waiting or doing I/O.
        [[nodiscard]] bool sourcesReadyAt(std::int64_t sample, std::int32_t frames,
                                          bool allowFailedSources = false) const noexcept;
        [[nodiscard]] bool sourcesReady(const Transport& requested, std::int32_t frames,
                                        bool allowFailedSources = false) const noexcept;
        /// Driver command between slices. Saved song start overrides the
        /// insert marker; only a record start uses the saved count-in. Prepare
        /// the recorder on its owner first. Explicit locate/play bypasses this
        /// default start policy. Offline sessions ignore saved performance state.
        bool startPlayback(std::int64_t insertMarkerTick, bool recording = false) noexcept;
        [[nodiscard]] CountInPosition countInPosition() const noexcept;
        bool process(const AudioIo&, bool captureRecording = true) noexcept;
    private:
        friend class Session;
        explicit RenderBlock(Session&) noexcept;
        // CallbackScope precedes acquisition and outlives the graph handle.
        audio::CallbackScope callback_;
        Session& session_;
        GraphHost::RenderBlock graph_;
    };
    [[nodiscard]] RenderBlock beginRender() noexcept { return RenderBlock(*this); }

    /// Owner only. Rebuild a performance clock without changing authored tempo
    /// or replacing device instances. Null restores authored project timing.
    /// The owner guard admits the fully primed candidate before shared bindings
    /// are changed; false leaves the existing source generation installed.
    bool setExternalTempo(const Store&, std::optional<double> bpm,
                          const std::function<bool()>& publishGuard = {}, std::int64_t primeTick = 0);
    /// Preserve performed time before the musical boundary and replace its
    /// future. Up to 4096 boundaries are retained in memory, never in the file.
    bool setExternalTempoAt(const Store&, std::optional<double> bpm, std::int64_t tick,
                            const std::function<bool()>& publishGuard = {});

    void prepare(double sampleRate, std::int32_t maxFrames) override;
    /// Configure device-owned workers as part of the same graph preparation as the device format.
    void prepareWithGraphWorkers(double sampleRate, std::int32_t maxFrames,
                                 std::uint32_t workers, std::int32_t minimumFrames);
    void process(const AudioIo& io) noexcept override;
    void release() override;

    /// Track input from one serialized MIDI producer. Submit in ascending
    /// physical stream samples (AudioIo::streamTimeSamples), preserving wire
    /// order at equal samples. Use zero-based channels for all events. Note
    /// on/off IDs must be nonzero and unique across ports. An unaddressed
    /// expression event may use ID zero.
    /// Returns false for invalid events or a full queue. Rebind the producer
    /// when the driver restarts its sample clock.
    bool enqueueMidiInput(std::int64_t streamSample, const Event&, std::int64_t inputSample = -1,
                          std::uint64_t port = 0) noexcept;
    /// Discard queued events and release held live notes on the next callback.
    /// Call when the producer disconnects or replaces its port configuration.
    void resetMidiInput() noexcept;
    /// Owner only. Start a new capture with the listed armed MIDI tracks.
    bool startMidiInputCapture(std::span<const std::int64_t> tracks, std::string& error, bool overdub = false);
    struct MidiRecordMonitor {
        std::int32_t instrumentArrival = 0;
        std::vector<MidiMonitorRange> blockedRanges;
    };
    /// Owner only. Timing and Auto suppression from the published live input route.
    [[nodiscard]] std::optional<MidiRecordMonitor> midiRecordMonitor(std::int64_t track) const;
    [[nodiscard]] MidiInputCapture& midiInputCapture() noexcept { return *midiInputCapture_; }
    // With the audio callback detached, finish without waiting for another slice.
    void finishMidiInputCapture() noexcept;

    [[nodiscard]] bool midiHistoryAvailable() const noexcept;
    bool readMidiHistories(std::vector<MidiMemorySnapshot>&, std::string& error) const;
    void consumeMidiHistories(std::span<const MidiMemorySnapshot>) noexcept;

    // Owner only. Note Editor audition bypasses Track monitoring and capture.
    bool previewMidiNote(std::int64_t track, int key, int velocity, bool held) noexcept;
    void stopMidiPreview() noexcept;
    [[nodiscard]] std::array<std::uint64_t, 2> midiKeysSounding(std::int64_t track) const noexcept;

    /// Owner only. Configures the next graph and rebuilds when loaded; the
    /// prior graph stays live on failure. Zero workers restores serial.
    bool setParallelRendering(std::uint32_t workers,std::int32_t minimumFrames,
                              std::uint32_t minimumNodes,RenderWorkers::Initializer initialize=nullptr,
                              void* context=nullptr);
    /// The Processing Precision switch (ADR-0207 d2). Message thread. A new
    /// precision is a new graph, built and published as a format change is
    /// (decision 3), and swapped in as any graph is: the handover carries
    /// every compensated edge's history across, widened or narrowed, and
    /// there is no crossfade (the swap is faded in only when
    /// `graph().setFadeFrames` is non-zero, 0 by default). The same precision
    /// does nothing. False only when a graph was due and could not be built.
    bool setPrecision(Precision p);

    /// Message thread. Publish a new alignment policy without reloading
    /// devices. Track Delay is suspended with it; stored values are retained.
    bool setDelayCompensation(bool on);

    // --- inspection (message thread) ---------------------------------------

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] const rows::Model& model() const noexcept { return model_; }
    bool validateRouting(const rows::Model& candidate,std::string& error); // owner, no SDK prepare
    /// Owner-side runtime notifications for this loaded Store connection.
    /// No Store/SQLite pointer survives in the revocable source token.
    [[nodiscard]] const AppliedSource& appliedNotifications() const noexcept { return appliedSource_; }
    /// Last successful model/instance refresh in this notification domain.
    [[nodiscard]] std::uint64_t realisedAppliedSequence() const noexcept { return appliedSequence_; }
    /// Invalidated before Session-owned devices are destroyed. Session-bound
    /// ParamOps may outlive the Session without dereferencing an old instance.
    [[nodiscard]] std::weak_ptr<const void> lifetimeToken() const noexcept { return lifetime_; }
    [[nodiscard]] const SessionSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    /// Resolve a direct wet stem endpoint on an isolated offline session.
    /// PostFader is before the parent's console encoder; PreFader is after
    /// this track's effects (not dry). No graph mutation or extra DSP node.
    /// Use currentGraph()->copyNodeOutput after each process, with no intervening
    /// graph mutation. The result is invalidated by a rebuild or latency change.
    /// Empty for missing/non-audio tracks, unsupported taps or no prepared graph.
    [[nodiscard]] std::optional<OfflineOutput> offlineOutput(std::int64_t trackId, OfflineTap);
    /// Owner-only cache capture. PreFx is after the instrument, before audio FX.
    /// Returned identities come from the loaded devices AFTER state restoration.
    [[nodiscard]] std::optional<OfflineOutput> offlineTapOutput(std::int64_t trackId, Tap);
    [[nodiscard]] std::optional<OfflineSourceInfo> offlineSourceInfo(std::int64_t trackId);

    /// Everything the model read, the plan found and the session decided,
    /// in that order. A project with problems still plays where it can.
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept {
        return problems_;
    }
    /// Problems that still invalidate a render when missing clips play
    /// silently and unavailable plug-ins are bypassed as in live playback.
    [[nodiscard]] std::vector<std::string> exportBlockingProblems();

    /// ADR-0174: the group summing this graph plays. Message thread.
    [[nodiscard]] const GroupSumming& summing() const noexcept { return summing_; }
    /// ADR-0163, ADR-0203 d3: the strips, sends and loops it plays. Message thread.
    [[nodiscard]] const MixerStrips& strips() const noexcept { return strips_; }
    /// Ephemeral monitoring choice for the live stereo device pair.
    void setMainAudition(MainAudition mode) noexcept { cueMonitor_.setMainAudition(mode); }
    // The application owns the single player. Attach before starting the
    // driver, and keep it alive until the driver has stopped.
    void setSamplePreview(SamplePreview *player) noexcept { cueMonitor_.setSamplePreview(player); }
    /// Main's post-output live LUFS/true peak. Independent relaxed scalar reads;
    /// requestReset is an off-thread command consumed at the next callback.
    [[nodiscard]] const MainLoudness& mainLoudness() const noexcept { return mainLoudness_; }

    /// ADR-0175/0195: a track/point tap holding `seconds` of audio. The default
    /// is after the fader; spectrum.post is PreFader, spectrum.pre ChainInput.
    /// Message thread. A second call for the same key returns the same tap
    /// with its original requested history while the endpoint is unchanged. Reacquire
    /// after graph/format changes, or use ScopeLease to follow generations. Retired
    /// taps remain safe snapshots and are reclaimed only after callback detachment.
    /// Null for a track with no strip in the graph.
    [[nodiscard]] std::shared_ptr<const ScopeTap> openScope(std::int64_t trackId, double seconds = 10.0,
                                                         ScopePoint point = ScopePoint::PostFader);
    void closeScope(std::int64_t trackId, ScopePoint point = ScopePoint::PostFader);
    /// Scoped visual demand. Compatible logical endpoints share their physical capture and the
    /// longest requested history. Off-thread tap snapshots follow replacement generations.
    [[nodiscard]] ScopeLease requestScope(const ScopeRequest&);
    /// Owner collection and latest deferred-plan publication after audio acknowledgement;
    /// never releases a tap used by a stalled physical callback.
    std::size_t collectScopeTaps();
    struct ScopeStats {
        std::size_t consumers = 0, sources = 0, retired = 0, deferredPlans = 0;
        bool awaitingAudio = false;
    };
    [[nodiscard]] ScopeStats scopeStats() const;

    /// ADR-0203 d3: how long a return's send into itself takes to come back,
    /// in samples, in the graph now playing -- the loop's block, D, plus the
    /// latency of the return's chain from its junction to the send's tap.
    /// Its own track delay is not in it: that is reported latency, played by
    /// compensation elsewhere, and the loop is not compensated. -1 for a row
    /// that is not a loop playing. Message thread.
    [[nodiscard]] std::int32_t feedbackTrip(std::int64_t routingId);

    /// Message thread: isolated offline analysis, then one undoable transaction.
    [[nodiscard]] GainStageResult autoGainStage(Store& store, const GainStageOptions& options = {});

    // Driver thread only, commands between callbacks; see transport.hpp.
    [[nodiscard]] Transport& transport() noexcept { return transport_; }
    /// Owner-thread recording control. Channels address the driver's current
    /// AudioIo inputs; per-device channel configuration remains the adapter's.
    // Owner only: copy a publishable Arrangement loop and its clock into the recorder request.
    bool configureArrangementLoopRecording(AudioRecordingSpec&, std::string& error) const;
    /// The published recording start is at or beyond Loop End. An active
    /// roll passes its driver position; a stopped start uses the song start
    /// override or insert marker, like RenderBlock::startPlayback.
    [[nodiscard]] bool recordingStartsAfterLoop(std::int64_t insertTick,
                                                std::optional<std::int64_t> playingSample) const noexcept;
    bool startRecording(const AudioRecordingSpec&,std::string& error);
    [[nodiscard]] AudioInputRecorder& recording() noexcept;
    // Selection arming is a live session choice. Explicit arms stay in the project.
    bool setSelectionArmedTrack(std::int64_t track);
    [[nodiscard]] bool recordArmed(const rows::Track& track) const noexcept {
        return track.recordArmed || (selectionArmedTrack_ != 0 && track.id == selectionArmedTrack_);
    }
    // Message/offline driver inspection. Pointer valid until the next rebuild.
    [[nodiscard]] ClipPlayback* clips() noexcept { if(auto* value=gestureClips(gestureRuntime_))return value;return clips_.get(); }
    [[nodiscard]] MidiClips* midiClips() noexcept { if(auto* value=gestureMidi(gestureRuntime_))return value;return midi_.get(); }
    [[nodiscard]] bool clipPlaybackUnavailable(std::int64_t clipId) noexcept {
        const auto* playback = clips();
        const auto* midi = midiClips();
        return (playback && playback->clipUnavailable(clipId)) || (midi && midi->clipUnavailable(clipId));
    }

    [[nodiscard]] device::DeviceHost& devices() noexcept { return devices_; }
    [[nodiscard]] GraphHost& graph() noexcept { return graph_; }

    [[nodiscard]] std::size_t entryCount() const noexcept { return entries_.size(); }
    [[nodiscard]] const Entry& entryAt(std::size_t i) const { return entries_[i]; }
    [[nodiscard]] const Entry* entryFor(std::int64_t deviceId) const noexcept;

    /// The live node and instance behind a device row; null for a row the
    /// session did not resolve (a rack) or never saw.
    [[nodiscard]] device::DeviceNode* nodeFor(std::int64_t deviceId) noexcept;
    [[nodiscard]] device::DeviceInstance* instanceFor(std::int64_t deviceId) noexcept;

    /// A track's chain as the rows order it: every chain on the track by
    /// `ord`, every device in it by `ord`, skipping what was not resolved.
    /// This is what `RealizeOptions::devicesFor` answers with.
    [[nodiscard]] std::vector<device::DeviceNode*> chainFor(std::int64_t trackId);
    /// Every node of a track's chain as the graph is given it: the buss half,
    /// the devices with the input gain after the instrument, the strip, the
    /// channel half (ADR-0122, ADR-0163, ADR-0174).
    [[nodiscard]] std::vector<Node*> trackChain(std::int64_t trackId);
    /// Where on that chain `tap` is (ADR-0203 d3): the node whose output it
    /// is, or null for the junction.
    [[nodiscard]] Node* tapFor(std::int64_t trackId, Tap tap);

private:
    friend struct GestureRuntime;
    friend std::shared_ptr<const GestureControls> nativeGestureControls(Session&);
    friend class gesture::EnginePresentation;
    struct TimingView;
    struct PendingLoad;
    bool prepareLoad(const Store&, DeviceLoader, SessionSpec, bool staged);
    bool checkLoadRevision(const Store&);
    SourceFn playbackSources(ClipPlayback*, MidiClips*, std::shared_ptr<const TimingView>);
    struct InputMonitors;
    void attach(std::shared_ptr<const TimingView>);
    void prepareInputMonitors(TimingView&, const rows::Model&, ClipPlayback&);
    bool readPlayback(const Store &);
    // The tempo this Session plays a model at: performed history (with its
    // authored resume), else the external tempo, else the model's map.
    [[nodiscard]] std::vector<rows::TempoEvent> playbackTempo(const rows::Model&) const;
    // Every node a track's events reach, which is MIDI's controller route identity.
    [[nodiscard]] DeviceChainFn eventPathFor(const rows::Model&, const TimingView&);
    [[nodiscard]] std::vector<Node*> graphTrackChain(std::int64_t, const TimingView&);
    [[nodiscard]] const device::DeviceInstance* resolvedAutomationDevice(
        std::int64_t id, const rows::Model&, bool requireAudioEffect = false) const noexcept;
    [[nodiscard]] std::shared_ptr<DeviceAutomationBinding> bindResolvedDeviceAutomation(
        std::shared_ptr<const AutomationProgram>, const std::set<std::int64_t>& suspended, const rows::Model&);
    bool parkedDevice(const rows::Device &) const;
    void resolveNewRows(const Store& store);
    void resolveOne(const Store& store, const rows::Device& row,
                    std::unique_ptr<device::DeviceInstance> preloaded = nullptr);
    void retireDepartedRows();
    void syncFlags();
    void syncRoute(Entry& e, device::DeviceInstance& inst);
    [[nodiscard]] const rows::PluginRef* refFor(const rows::Device& row) const noexcept;
    [[nodiscard]] const rows::DeviceChain* chainOf(std::int64_t chainId) const noexcept;
    [[nodiscard]] std::unique_ptr<device::DeviceInstance> makePlaceholder(
        const Store& store, const rows::Device& row, const rows::PluginRef* ref);
    void restoreState(const Store& store, const rows::Device& row,
                      device::DeviceInstance& inst);

    // Declared so that destruction runs graphs FIRST: a retired graph holds
    // raw pointers to device nodes, and the devices must still exist while
    // it is freed. (Nothing dereferences them today; the order is the rule.)
    Transport transport_;
    std::unique_ptr<LiveMidiInput> midiInput_;
    std::unique_ptr<MidiInputCapture> midiInputCapture_;
    std::map<std::size_t, std::shared_ptr<LiveMidiInput::Voices>> midiInputVoices_;
    struct MidiHardwareNode;
    std::uint64_t midiHardwareNodeSerial_ = 0; // owner, identifies a prepared output node
    const MidiHardwareSink* midiHardwareSink_ = nullptr; // driver, during graph processing
    std::int64_t midiHardwareStreamSample_ = 0;
    std::uint64_t midiHardwareRouting_ = 0;
    std::map<std::int64_t, std::shared_ptr<LiveMidiInput::History>> midiHistories_;
    std::int64_t lastMidiHistoryId_ = 0; // owner only
    // Device nodes persist across graph swaps and Undo. Their audition voices
    // retain any pending releases even if the device temporarily leaves the graph.
    std::map<const Node*, LiveMidiInput::PreviewVoice> midiPreviewVoices_;
    std::map<std::int64_t, std::array<std::int32_t, 2>> monitorInputs_;
    std::unique_ptr<AudioInputRecorder> recording_;
    // Borrowed from the recorder for one rendered slice. Graph workers join
    // before the driver clears it or acquires the next recorder snapshot.
    std::span<const std::int64_t> recordingAudioTracks_;
    std::string recordingProject_;
    // Message-thread construction/publication; audio reads one immutable view
    // per block. No model_ or spec_ access from process while refresh rebuilds.
    struct TimingView final : GraphTransportState {
        TimingView(TempoMap map, double rate)
            : tempo(std::move(map)), tempoClock(tempo, rate), sampleRate(rate) {}

        std::shared_ptr<const GestureControls> generationControls;
        std::shared_ptr<GestureAudio> gestureAudio;
        std::uint64_t gestureGeneration=0;
        Session* session=nullptr; // retained Session owns every published graph
        mutable TransportInfo blockInfo{}; // the single audio driver's scratch
        TransportSettings settings;
        // Allocated on the owner for exact count-in and punch boundary splits.
        // Input supports 64 physical channels and output covers the full graph.
        mutable std::vector<const float *> inputSlice;
        mutable std::vector<float *> outputSlice;
        mutable std::vector<double *> outputSlice64;
        std::int64_t loopBegin=0,loopEnd=0;
    std::int64_t punchOutSample=0; // brace end without the one-frame minimum for loops
        TempoMap tempo;
        WarpArrangementClock tempoClock;
        std::vector<rows::TimeSignature> signatures;
        std::vector<std::pair<std::int64_t, std::int64_t>> deviceClockChanges; ///< sample, tick
        std::vector<LiveMidiInput::Route> midiInputRoutes;
        std::map<std::int64_t, std::shared_ptr<MidiHardwareNode>> midiHardware;
        std::uint64_t midiHardwareRouting = 0;
        std::vector<std::shared_ptr<LiveMidiInput::History>> midiHistories;
        std::vector<LiveMidiInput::PreviewRoute> midiPreviewRoutes;
        std::shared_ptr<const StripLaneBindings> stripLanes;
        ClipPlayback::Readiness sourceReadiness;
        ClipPlayback::Readiness delayedSourceReadiness;
        std::uint64_t tempoRevision=0;
        bool authoredPerformanceTempo=false;
        std::shared_ptr<InputMonitors> inputMonitors;
        double sampleRate = 48000.0;
        double lastTickSeconds = 0.0;
        SessionSpec spec;
        [[nodiscard]] std::int64_t sampleForTick(std::int64_t tick) const noexcept;
        void setBraceSamples(std::int64_t begin, std::int64_t end) noexcept;
        void prepareDeviceClockChanges();
        void applyLoop() const noexcept;
        [[nodiscard]] bool projectLoop(Transport&) const noexcept;
        [[nodiscard]] bool readyToAdopt(const GraphTransportState*) const noexcept override;
        [[nodiscard]] TransportInfo at(const Transport& transport) const noexcept;
        [[nodiscard]] double quartersAt(std::int64_t sample) const noexcept;
        [[nodiscard]] DeviceTransport deviceAt(std::int64_t sample) const noexcept;
        [[nodiscard]] const TransportInfo* beginBlock()const noexcept override;
        [[nodiscard]] Node* inputMonitorSource(std::int64_t track) const noexcept;
    };
    void prepareSourceReadiness(TimingView&, const ClipPlayback&);
    void prepareMidiHardware(TimingView&, const rows::Model&, const TimingView* previous);
    void prepareMidiInput(TimingView&, const rows::Model&, const MidiClips&);
    void prepareMidiPreview(TimingView&, const rows::Model&);
    [[nodiscard]] const LiveMidiInput::PreviewRoute* midiPreviewRoute(std::int64_t track) const noexcept;
    void clearMidiPreviewDevice() noexcept;
    MainLoudness mainLoudness_;
    CueSettings cueSettings_;
    CueMonitor cueMonitor_;
    TransportSettings transportSettings_; // message thread, copied into TimingView
    std::vector<rows::TempoEvent> performedTempo_; // owner-only performance history
    std::optional<std::int64_t> authoredResumeAt_; // future authored map resumes here
    std::optional<std::int64_t> primeExternalAt_; // owner-only candidate readiness anchor
    bool primeExternalFollowDriver_ = false;
    const std::function<bool()>* externalPublishGuard_ = nullptr; // synchronous owner call only
    void publishDriverClock() noexcept; // single audio driver only
    void primeWhilePlaying(ClipPlayback&, const TimingView&); // owner, an unpublished candidate only; stopped too (B12)
    std::atomic<std::uint64_t> driverClockSequence_{0}, driverRevision_{0};
    std::atomic<std::int64_t> driverSample_{0}, driverStreamed_{0}, driverLoopBegin_{0}, driverLoopEnd_{0};
    std::atomic<bool> driverPlaying_{false};
    std::atomic<bool> recordingForDevices_{false}; // SC-333
    bool loopApplied_=false,loopOn_=false;
    std::int64_t loopBegin_=0,loopEnd_=0;
    std::int64_t loopStartTick_=0,loopEndTick_=0;
    // Driver-thread click state; routing moves to Cue Out when N23 arrives.
    std::int64_t clickLastSample_=-2,clickLastBeat_=-1;
    bool clickWasPlaying_=false;
    double clickPhase_=0,clickEnvelope_=0,clickFrequency_=880;
    struct ClickCaptureEdge {
        std::int64_t streamFrame;
        bool capturing;
    };
    // Keep recent capture edges on the undelayed stream clock. The audio
    // callback reads them back on the same compensated clock as the click.
    std::array<ClickCaptureEdge, 64> clickCaptureEdges_{};
    std::size_t clickCaptureEdgeCount_ = 0;
    std::uint64_t clickCaptureRevision_ = 0;
    double countInBeatSamples_ = 24000;
    std::int64_t countInBeatsPerBar_ = 4;
    std::int32_t countInBars_ = 0, countInDivisionsPerBeat_ = 4;
    // DF-E6 (a): a count-in delayed by the render latency still owes its last
    // clicks (at most one beat) when the roll starts. They play on, on the
    // count-in grid, until the compensated program is heard; the transport
    // revision of that roll guards them against stop, locate and restart.
    bool countInTail_ = false;
    std::uint64_t countInTailRevision_ = 0;
    std::int64_t countInTailTotal_ = 0, countInTailLag_ = 0;
    [[nodiscard]] std::int64_t countInLag(std::int32_t latency) const noexcept;
    [[nodiscard]] bool countInTailActive() const noexcept;
    void triggerClick(bool accent, MetronomeSound sound) noexcept;
    double nextClickSample(double rate, double decay, MetronomeSound sound) noexcept;
    double countInClick(std::int64_t heard, double rate, double decay, MetronomeSound sound) noexcept;
    void renderCountIn(const TimingView &, const AudioIo &) noexcept;
    void noteClickCapture(bool capturing) noexcept;
    [[nodiscard]] bool recordingClickPending() const noexcept;
    void renderMetronome(const TimingView&, const AudioIo&, bool rollingClick) noexcept;
    std::shared_ptr<const ClipProject> clipProject_;
    // Published graphs retain their own old source generations until reclamation.
    std::shared_ptr<ClipPlayback> clips_;
    std::shared_ptr<MidiClipState> midiState_ = makeMidiClipState();
    std::shared_ptr<MidiClips> midi_;
    // ADR-0163: one strip per track, for the whole session. Declared before
    // the graphs so it outlives every graph that names one of its strips.
    MixerStrips strips_;
    std::map<std::int64_t, std::unique_ptr<InputGainNode>> inputGains_;
    GroupSumming summing_;   ///< ADR-0174: native analog group summing
    struct ScopeRuntime;
    std::unique_ptr<ScopeRuntime> scopeRuntime_;
    void attachTaps();
    void beginScopeRead() noexcept;
    // ADR-0164: this rebuild's program, bound to strips. Kept here as well as
    // by every strip and graph that plays it.
    std::shared_ptr<StripAutomation> stripAutomation_;
    // ADR-0165: this rebuild's device lanes, one EventSource per device, set on
    // each device's node before the graph that plays them is published.
    std::shared_ptr<DeviceAutomationBinding> deviceAutomation_;
    std::vector<std::string> deviceSourceProblems_;
    std::vector<std::string> placeholderAutomationProblems_;
    void bindDeviceSources();
    std::set<std::int64_t> overridden_;   ///< ADR-0162: lane ids
    std::set<std::int64_t> writingAutomation_;
    bool automationArmed_ = true, automationRecording_ = false;
    std::shared_ptr<GestureAudio> gestureAudio_;
    std::shared_ptr<GestureRuntime> gestureRuntime_;
    device::DeviceHost devices_;
    GraphHost graph_;
    rows::Model model_;
    AppliedSource appliedSource_;
    void observeKeyRoutes();
    void refreshProblems();
    [[nodiscard]] std::vector<std::string> collectProblems(bool includeUnavailable);
    std::uint64_t appliedSequence_ = 0;
    std::shared_ptr<const void> lifetime_ = std::make_shared<const int>(0);
    std::shared_ptr<SessionEndpoint> endpoint_;
    std::vector<std::shared_ptr<SessionRuntimeClient>> runtimeClients_;
    bool prepareFormat(double sampleRate, std::int32_t maxFrames, bool workerPolicyChanged);
    std::uint64_t runtimeEpoch_ = 0;
    rows::Model playbackModel_;
    std::map<std::int64_t, std::int64_t> auditionedTakeLanes_;
    std::set<std::int64_t> frozenGroupChildren_;
    struct FrozenMediaFile {
        std::string row;
        std::filesystem::path file;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};
    };
    std::map<std::int64_t, FrozenMediaFile> verifiedFrozenMedia_;
    std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> frozenSources_; // dry, wet synthetic tracks
    DeviceLoader loader_;
    SourceFn sources_;
    SessionSpec spec_;
    std::int64_t selectionArmedTrack_ = 0;
    Stats stats_;
    std::vector<Entry> entries_;
    std::unordered_map<std::int64_t, std::size_t> byId_;   ///< device id -> entries_
    std::vector<std::string> problems_;
    std::vector<std::string> sessionProblems_;   ///< ours, re-appended after each read
    std::string error_;
    std::unique_ptr<PendingLoad> pendingLoad_;
    std::atomic<bool> playbackRunning_{false}; // driver sample; owner never reads Transport
    bool loaded_ = false;
    bool presentationPending_=false; ///< graph already published; owner tick retries auxiliary Cue scene
    bool live_ = false;      ///< a graph is prepared and published for a device
};

}  // namespace adi::engine
