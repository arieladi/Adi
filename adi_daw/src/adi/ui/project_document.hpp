// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/automation_lanes.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/device_copy.hpp"
#include "adi/device_presets.hpp"
#include "adi/sample_slots.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/param_ops.hpp"
#include "adi/gesture_producers.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/mix_functions.hpp"
#include "adi/mix/limiter/ReductionMailbox.h"
#include "adi/media/collect_export.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "adi/material_commands.hpp"
#include "adi/ui/arm_edits.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/ui/external_input_meter.hpp"
#include "adi/ui/import_choices.hpp"
#include "adi/ui/live_midi_take.hpp"
#include "adi/ui/op_submitter.hpp"
#include "adi/ui/parameter_feed.hpp"
#include "adi/ui/recording_edits.hpp"
#include "adi/ui/session_state.hpp"
#include "adi/ui/shared_analysis.hpp"
#include "adi/ui/track_edits.hpp"
#include "adi/ui/waveform.hpp"
#include "adi/ui/window_state.hpp"
#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <utility>
namespace adi::recording { struct RecordingEdit; }
namespace adi::engine::recording_recovery { struct LaunchTake; }
namespace adi::ui {
using media::pathFromUtf8;
// The history's first entry in a project made by New; never undone (Live 5.4.2).
inline constexpr const char *kNewProjectLabel = "New project";
// Message-thread project lifetime. The callback is detached before destruction.
// Only process() accesses Transport while the driver is running.
//
// Every op is committed as it happens (ADR-0003, ADR-0203 d13), and the store
// closes into one file. Ctrl/Cmd+S names the project once and then takes snapshots.
struct WaveformSource; // project_document.cpp
class ProjectDocument final : public engine::BlockProcessor {
  public:
    ~ProjectDocument() override;
    // New (`create`) captures the registry's recording defaults for its four tracks in the history's
    // root (`Store::createNewProject`'s four-argument form, #397, N31) when `defaults` is given.
    static std::unique_ptr<ProjectDocument> open(const std::filesystem::path &, bool create,
                                                 engine::DeviceLoader, std::string &error,
                                                 const RecordingDefaults *defaults = nullptr);
    // A candidate remains private until its graph and view agree. Advance and
    // destroy this job on the document owner, between message loop turns.
    class OpenJob {
      public:
        enum class Phase { preparing, loadingDevices, buildingGraph, ready, failed };
        ~OpenJob();
        Phase phase() const noexcept { return phase_; }
        void advance();
        std::size_t completedDevices() const noexcept { return completedDevices_; }
        std::size_t totalDevices() const noexcept { return totalDevices_; }
        const std::string &error() const noexcept { return error_; }
        std::unique_ptr<ProjectDocument> takeDocument();

      private:
        friend class ProjectDocument;
        OpenJob(std::filesystem::path, bool, engine::DeviceLoader, const RecordingDefaults *);
        void prepare();
        void fail(std::string reason);

        std::filesystem::path path_;
        bool create_;
        engine::DeviceLoader loader_;
        std::optional<RecordingDefaults> defaults_;
        std::unique_ptr<ProjectDocument> document_;
        Phase phase_ = Phase::preparing;
        std::size_t completedDevices_ = 0;
        std::size_t totalDevices_ = 0;
        std::string error_;
    };
    static std::unique_ptr<OpenJob> startOpen(const std::filesystem::path &, bool create, engine::DeviceLoader,
                                              const RecordingDefaults *defaults = nullptr);
    struct HistoryReload {
        std::unique_ptr<ProjectDocument> document;
        std::string error;
        bool replayBaseChanged = false;
    };
    // The caller must stop audio before calling and replace this document if the replay base changes.
    // If the target cannot be opened, try to restore and open the previous head.
    HistoryReload reopenAtStoredHistory(HistoryRestoreTarget, std::int64_t nowUtc);
    // Take Snapshot (ADR-0203 d6, d13; ADR-0128): history metadata, not an op,
    // so it neither refreshes the view nor enters the undo list.
    History::SnapshotResult takeSnapshot(const std::string &name, std::int64_t nowUtc,
                                         int utcOffsetMinutes);
    // The app supplies local civil time alongside UTC microseconds. The
    // document updates the daily version after committed history changes.
    using DailyHistoryClock = std::function<std::pair<std::int64_t, int>()>;
    void setDailyHistoryClock(DailyHistoryClock clock) { dailyHistoryClock_ = std::move(clock); }
    void flushDailyHistory() { updateDailyHistory(true); }
    // What the Take Snapshot prompt is pre-filled with: the project's name and the time.
    std::string snapshotName(std::int64_t nowUtc, int utcOffsetMinutes) const;
    const std::string &name() const noexcept { return name_; }
    // Save As (Live 12 manual 5.4.1, 41.3; ADR-0203 d6; N3): the store copies the project into a
    // new `.adi` and carries on in it (`Store::saveAs`, #380), called with no live statement held
    // (SPEC §3.7); the name, the view and the session follow, and the caller retitles the window.
    // Existing destinations require explicit replacement confirmation. False means nothing moved; once the store has
    // switched the save stands and is true, a warning the store reports, or a session that
    // failed to re-read the new file, in `warning()`.
    bool saveAs(const std::filesystem::path &, bool firstSave = false,
                StoreFileCollision = StoreFileCollision::Fail,
                const media::FileIdentity *expectedTarget = nullptr);
    const std::string &warning() const noexcept { return warning_; }
    // Live's Save a Copy (manual 5.4.1): the project as it stands into a new `.adi`, the open one
    // left as it is (`Store::saveCopy`, #380, N30). An existing file is refused.
    bool saveCopy(const std::filesystem::path &, bool keepUndoHistory = false);
    enum class SaveKind { as, copy, firstSave };
    struct SaveProgress {
        enum class Phase { waitingForRecording, copying, relocating, finishing, succeeded, cancelled, failed };
        Phase phase;
        int pagesDone = 0;
        int pagesTotal = 0;
        bool switched = false;
    };
    // File-menu saves progress between owner turns. Only Save As switches the live document.
    bool startSave(const std::filesystem::path &, SaveKind, StoreFileCollision = StoreFileCollision::Fail,
                   const media::FileIdentity *expectedTarget = nullptr, bool keepUndoHistory = false);
    std::optional<SaveProgress> saveProgress() const;
    void advanceSave();
    void cancelSave();
    void clearSave(); // after a terminal result has been shown
    // Live's Collect All and Save (manual 5.7): every outside media file the project and its
    // history use is copied into `audio/` beside it and the project points at the copies
    // (`Store::collectAllAndSave`, #411). Live's dialog chooses which locations to collect; the
    // store collects them all. Not an undo step; the view and the session read the project again.
    bool collectAllAndSave();
    // ADI's Collect and Export (MR 1.6, 11.3): a portable `.zip` of the project and every file it
    // uses (`media::collectExport`, #411, behind `Store::collectAndExport`), of the project as committed,
    // the open one left as it is. Prepared here on the message thread: the destination is refused
    // when it exists, and the job names the project's file. Run anywhere: `runCollectAndExport` reads
    // the file on a short read-only connection of its own (SPEC §3.8), so the app runs it on a worker
    // it owns and the window keeps working; a project replaced meanwhile does not wait for it.
    struct ExportJob {
        std::filesystem::path project, zip;
        std::string stateWarning;
        bool keepUndoHistory = false;
    };
    bool prepareCollectAndExport(const std::filesystem::path &zip, ExportJob &out, bool keepUndoHistory = false);
    static StoreFileResult runCollectAndExport(const ExportJob &, const media::ArchiveControl & = {});
    // Finish pending device edits and capture even plug-ins that never report changed state.
    // Call before closing the document or reading the file for an offline render.
    // Save, Export and Quit keep stored state for an unreadable plug-in and
    // mark it stale. A hard storage failure is reported separately in error().
    bool capturePluginState(std::optional<std::int64_t> deviceId = {});
    const std::string &pluginStateWarning() const noexcept { return pluginStateWarning_; }
    // Structural edits require the device's live state before they proceed.
    bool requirePluginState();
    bool synchronise();
    // SC-318: a plug-in scan has ended; every placeholder whose plug-in now loads wakes in its place.
    // Returns how many woke (the view then re-reads the session).
    std::size_t wakeMissingPlugins();
    void tick(std::int64_t milliseconds);
    // Drain and publish queued plug-in edits before a new surface begins observing touches.
    void pollParameterEdits();
    // Commits every parameter edit still held in the capture, each as the undo step it was going to
    // be, without waiting for its quiet interval: what Undo, Redo and the Undo History do first, so the
    // step they undo is the one the user sees last (DF-W29). A held drag (Begin with no End) stays.
    bool settleParameters();
    // Installed by the audio device owner before attaching its callback. Evaluated on the
    // message thread when the driver supplies the granted size to prepare.
    void configureGraphWorkers(std::function<std::uint32_t(std::int32_t)> count,
                               std::int32_t minimumFrames);
    void prepare(double rate, std::int32_t frames) override;
    void release() override;
    void process(const engine::AudioIo &) noexcept override;
    Store &store() noexcept { return *store_; }
    engine::ProjectView &view() noexcept { return view_; }
    OpSubmitter &ops() noexcept { return ops_; }
    ViewStateStore &views() noexcept { return views_; }
    SessionState &sessionState() noexcept { return sessionState_; }
    TransportMailbox &mailbox() noexcept { return mailbox_; }
    engine::Session &session() noexcept { return session_; }
    std::int64_t retainedDeviceId() const noexcept;
    const std::string &error() const noexcept { return error_; }
    // The parameter feed every window and the control API drain (ADR-0180 d5).
    ParameterFeed &parameterFeed() noexcept { return feed_; }
    void setParameterDemand(const void *consumer, const std::vector<std::int64_t> &devices);
    std::shared_ptr<const ParameterPublication> automationParameters(std::int64_t track);
    const std::shared_ptr<SharedAnalysis> &analysis() const noexcept { return analysis_; }
    // Visible surfaces hold one owner-thread lease per presented runtime.
    // Reads are nonconsuming and expose the same current frame to every reader.
    [[nodiscard]] mix::limiter::ReductionMailbox::Lease observeLimiterReduction(
        std::int64_t id, const engine::DeviceRuntimeToken& expected);
    // A control in the device strip: Begin, Values and End on one parameter, in the plug-in's
    // normalised unit (N7; ADR-0124 d4, ADR-0181 d5; MR 9.1). Every value reaches the plug-in at
    // once; End commits the gesture as ONE undo step, its first-touch opener in front, through
    // the engine's own window producer (`ParamOps::uiEvent`). A value nothing brackets closes on
    // the drain's quiet timer, in `tick()`. False when THIS event was refused (a device that is
    // not loaded, a missing or read-only parameter, a value the plug-in did not take), with
    // `error()` saying which. An older gesture the producer's own drain completed on the way to
    // judging the value is committed by this call; a device, parameter or value the producer
    // refuses outright drains nothing, and the older gesture commits on the timer's next frame.
    bool parameterGesture(std::int64_t id, const std::string &param, engine::ParamEventKind,
                          double normalized);
    // Explicitly configurable descriptors use the same gesture/undo path,
    // but stay non-automatable and never override an automation lane.
    // Retained multi-track surfaces edit the exact runtime they presented. A stale row ID
    // must never redirect a gesture to a replacement instance.
    bool parameterGestureExpected(std::int64_t id, const std::string &param, engine::ParamEventKind,
                                  double normalized, const engine::DeviceRuntimeToken &, std::uint64_t instance);
    bool parameterPairGestureExpected(std::int64_t id, const engine::ParameterPair &,
                                      engine::ParameterPairEvent, const engine::DeviceRuntimeToken &,
                                      std::uint64_t instance, std::shared_ptr<const gesture::OwnerToken> &owner);
    bool deviceTargetCurrent(std::int64_t id, const engine::DeviceRuntimeToken &, std::uint64_t instance);
    // Live's Re-Enable Automation (manual 25.4; MR 6.1; N26): every lane a manual change overrode
    // this run follows its envelope again, from the playhead at once, and the snapshot's
    // `automationOverridden` and each lane's `overridden` follow in the same call. Not an op and
    // not an undo step: the override is session state (ADR-0162), and so is its end. True with
    // nothing overridden too; false with `error()` when the session's rebuild failed.
    bool reenableAutomation();
    // Live's Options > Delay Compensation (manual 18.7; MX-04): the session lines every track up with
    // the slowest chain, or compensates nothing and suspends Track Delay, keeping its stored values.
    // The application's setting, not the project's: not an op and not an undo step. False with
    // `error()` when the session refused it.
    bool setDelayCompensation(bool on);
    // The parameter's context menu in Live (manual 25.4): one overridden lane follows its envelope
    // again; the others stay overridden. Session state as above, no undo step. False with `error()`
    // when the lane was not overridden or the rebuild failed.
    bool reenableAutomation(std::int64_t laneId);
    // Live's Automation Arm (manual 25.1): whether a control moved while the Arrangement Record
    // button is on becomes automation. The session's switch is on in a new Set.
    void setAutomationArmed(bool armed);
    bool automationArmed() const noexcept { return session_.automationArmed(); }
    // Arm and monitoring as the session's rows hold them (MX-09): the snapshot carries neither yet.
    RecordStates recordStates() const;
    // Arrangement Record (MX-09; Live 12 manual 21.1): armed audio tracks record their selected input.
    // Armed MIDI tracks record the Track MIDI input.
    // The message thread prepares the recorder before notifying the driver. From stopped, the driver
    // starts at the song start or the insert marker after the saved Count-In. While playing, it records
    // from the playhead. False with `error()` saying why.
    // `inputs` is the device's input channel count, `insertTick` where a stopped transport starts.
    // The driver's round-trip latency and signed driver error are supplied in samples at the recording's rate.
    // A nonempty `recordFolder` keeps the recoverable capture there until publication into the song.
    bool startRecording(std::int64_t insertTick, std::int32_t inputs, bool playing,
                        std::int64_t deviceLatencySamples = 0, const std::string &recordFolder = {},
                        audio::WavFormat format = audio::WavFormat::Float32, bool overdub = false);
    // The application owns launch discovery. Headless and transient document
    // hosts do not create entries in the application's recording registry.
    void enableRecordingRecovery() noexcept { recordingRecoveryEnabled_ = true; }
    bool startTakeRecovery(const engine::recording_recovery::LaunchTake &);
    // The application replaces these routes when the device or its channel config changes.
    // An empty list means the device has no enabled inputs. The view lasts until the next set.
    void setExternalAudioInputs(std::vector<ExternalAudioInput> inputs);
    std::span<const ExternalAudioInput> externalAudioInputs() const noexcept;
    std::shared_ptr<ExternalInputMeter> externalInputMeter() const noexcept { return externalInputMeter_; }
    // Hosts without a route list can still provide a contiguous input count for monitoring.
    void setInputChannels(std::int32_t inputs);
    // Stops the recorder (the transport is the caller's); the takes land on a later `tick()`.
    void stopRecording();
    // Lands a running or stopping take now, with the driver detached (a project replaced, Quit): call it
    // after the audio callback is gone and before the document goes. False: the take was still being
    // written after ten seconds and goes with the document.
    bool settleRecording();
    // A take recording or landing lands in this file now, Record stopped first: what Save As does before
    // the file switches. False when the recorder's worker did not finish in ten seconds.
    bool landTakeHere();
    bool recording() const noexcept { return recording_; }
    bool capturingAudioTake() const noexcept { return recording_ && recordingAudio_; }
    bool landingTake() const noexcept { return landing_; }
    std::span<const LiveMidiTake> liveMidiTakes() const noexcept;
    // What the last take did, once: the clips it added (one undo step for all of them), or nothing and
    // the reason in `error()`. Empty until a take lands.
    struct TakeLanded {
        bool ok = false;
        std::vector<std::int64_t> clips;
        std::string message;
    };
    [[nodiscard]] bool canCaptureMidi() const noexcept;
    TakeLanded captureRecentMidi(std::int64_t positionTick, bool playing);
    std::optional<TakeLanded> takeLanded();
    // A mixer drag heard while it moves (MX-23, SRF-02; Live 12 manual 18.1): a fader, a pan or a send
    // knob, on its track or, when that track is among the selected, on every selected track by the same
    // move. Begin captures the values where the drag starts; each Value is heard at once, with no op;
    // End commits ONE transaction of the final values, one undo step named `label`, or nothing when
    // nothing changed; Cancel (Esc) puts every value back. The engine's gesture session does the work
    // (`gesture::relativeScalarProducer`); `coordinate` is the control's own value (a fader's dB, a
    // pan's -1..1). False with `error()` saying why; a drag already running is ended first.
    // With no `coordinate` the drag begins where the rows stand and is heard from its first move, so a
    // click that does not move writes nothing.
    bool beginMixerDrag(const gesture::RelativeScalarSelection &, std::optional<double> coordinate,
                        const std::string &label);
    bool mixerDrag(double coordinate);
    bool endMixerDrag();
    void cancelMixerDrag();
    bool mixerDragging() const noexcept { return mixerDrag_ != nullptr; }
    // Auto Gain Stage (ADR-0195 d4): one offline command, one undo step.
    bool autoGainStage(bool lufs);
    // One track render at a time. The window thread owns plug-in life cycles
    // and the final edit, while the worker reads and renders its own snapshot.
    void configureFreezeTail(double seconds) noexcept;
    void configureTrackRenderDispatch(std::function<void(std::function<void()>)> post);
    bool freezeTrack(std::int64_t track);
    bool unfreezeTrack(std::int64_t track);
    bool flattenTrack(std::int64_t track);
    bool bounceTrackToNewTrack(std::int64_t track);
    bool bounceTrackInPlace(std::int64_t track);
    bool trackRenderRunning() const noexcept { return trackRender_ != nullptr; }
    std::optional<std::string> takeTrackRenderStatus();
    // A device edit from the strip (enable, remove, panel), as one op.
    bool deviceAction(const std::string &op, Payload payload);
    // Live's device clipboard (manual 23.2.1; N18, #394). Copy captures live state first and uses
    // stored state for a device whose plug-in is not loaded; the clipboard is the app's, not the
    // document's. Paste puts a new device, with the next free id and the copy's stored state, in
    // front of `before` in its chain, or at the end of `track`'s own chain: one undo step. The
    // engine's refusals (a rack with nested chains, a copy from another project) come back in
    // `error()`.
    bool copyDevice(std::int64_t id, DeviceCopy &out);
    bool pasteDevice(const DeviceCopy &copy, std::int64_t track, std::optional<std::int64_t> before);
    bool copyDevices(std::span<const DevicePanelData>, std::vector<DeviceCopy> &out, bool cut = false);
    bool removeDevices(std::span<const DevicePanelData>);
    bool pasteDevices(std::span<const DeviceCopy>, std::int64_t track, std::optional<std::int64_t> before);
    // The file the engine names a device copy's project by (`DeviceCopy::sourceFile`): after Save As
    // the app moves a held copy over to the new file, the same project under a new name.
    std::string deviceCopyIdentity() const;
    // A built-in device from the browser (D4; N15, #395): `uid` from `builtinCatalog()` onto
    // `track`, in front of `before` or at the end of its chain. The plug-in reference and the
    // chain are made in the same batch when the project has neither: one undo step, "Insert <name>".
    // A drop below the tracks passes track 0 and creates and selects the appropriate track.
    // The catalogue's refusals (an unavailable device, a missing track) come back in `error()`.
    bool insertBuiltin(const std::string &uid, std::int64_t track, std::optional<std::int64_t> before,
                       const RecordingDefaults *defaults = nullptr, bool exclusiveArm = false);
    bool insertPlugin(const rows::PluginRef &, std::int64_t track,
                      std::optional<std::int64_t> before = {}, const PluginInsertOptions& = {});
    // A plug-in the user loads from the Browser or the Stack's menu, as Live loads one (walk-A A26,
    // DF-W52): an instrument goes onto a MIDI track only (it takes notes and no audio, so it would
    // silence any other track) and takes the place of the track's instrument, in the same undo step.
    // An effect loads as `insertPlugin` places it.
    bool loadPlugin(const rows::PluginRef &, std::int64_t track, std::optional<std::int64_t> before = {},
                    const RecordingDefaults *defaults = nullptr, bool exclusiveArm = false);
    bool saveDevicePreset(const DevicePanelData &, const std::filesystem::path &library,
                          const std::string &name, std::filesystem::path &saved);
    bool saveDefaultDevicePreset(const DevicePanelData &, const std::filesystem::path &library);
    // The current User Library, read when a new device is loaded. An empty path has no defaults.
    std::function<std::filesystem::path()> devicePresetLibrary;
    std::int64_t loadDevicePreset(preset::Preset, std::int64_t track, std::optional<std::int64_t> before,
                                  const std::optional<rows::PluginRef> &installed = {});
    struct HotSwapTargetInfo {
        preset::Identity device;
        std::string subtype;
    };
    std::optional<HotSwapTargetInfo> hotSwapTarget(const DevicePanelData &);
    std::int64_t hotSwapBuiltin(const DevicePanelData &, const std::string &uid);
    std::int64_t hotSwapPlugin(const DevicePanelData &, const rows::PluginRef &);
    std::int64_t hotSwapPreset(const DevicePanelData &, preset::Preset,
                               const std::optional<rows::PluginRef> &installed = {});
    bool hotSwapSample(const DevicePanelData &, std::int32_t slot, const std::filesystem::path &,
                       std::int64_t nowUtc);
    // SC-325 (ADR-0134 d7): the expression route remembered for a plug-in (format, uid), the default of a
    // new instance `loadPlugin` makes; the project then keeps it on the device row. Unset: Auto.
    std::function<std::optional<engine::RouteChoice>(const std::string &, const std::string &)> pluginRouteDefault;
    bool addMixFunction(mix::Function, std::int64_t track);
    std::vector<DeviceKeyChoice> deviceKeyChoices(const DevicePanelData&);
    bool setDeviceKeyChoice(const DevicePanelData&,const DeviceKeyChoice&);
    bool setDeviceKey(const DevicePanelData&,std::optional<std::int64_t> source,const std::string& tap);
    bool moveMixFunction(const DevicePanelData &expected, std::int64_t track,
                         std::optional<std::int64_t> before);
    bool moveDevice(const DevicePanelData &expected, std::int64_t track,
                    std::optional<std::int64_t> before);
    bool moveDevices(std::span<const DevicePanelData>, std::int64_t track, std::optional<std::int64_t> before);
    // A built-in device's sample slot (D5; N28, #414; ENGINE-SAMPLE-SLOTS.md): `file` imported by its
    // content and prepared for the engine's rate, then the slot replaced, in one undo step; Clear
    // empties it. The live device takes the new state at once (`ParamOps::appliedState`). The
    // engine's refusals (a device this build cannot run, an undeclared slot, a file that is not
    // audio) come back in `error()`. Preparation decodes on the calling thread.
    // Drop a sample on a MIDI track: OneShot and its filled sample slot are one Undo step.
    bool loadSampleInstrument(std::int64_t track, const std::filesystem::path &file, std::int64_t nowUtc);
    bool assignSample(std::int64_t device, std::int32_t slot, const std::filesystem::path &file, std::int64_t nowUtc);
    bool clearSample(std::int64_t device, std::int32_t slot);
    // The parameter glue's counters (for the tests: a sample's state reached the live device).
    const engine::ParamOps::Stats &parameterStats() const noexcept { return parameterOps_.stats(); }
    std::vector<engine::PendingParameterGesture> pendingParameterGestures() const {
        return parameterOps_.pendingGestures();
    }
    // An audio file dropped on the arrangement (#308, N5): reads the file and the media pool and
    // captures the media row without writing anything; the window adds the clip and commits the
    // batch as one undo step. `nowUtc` in microseconds, as the history's own times. False with
    // `error()` saying why, a file that is not audio included.
    bool prepareImport(const std::string &utf8Path, std::int64_t nowUtc, AudioImport &out,
                       const std::filesystem::path &defaultsIndex = {});
    // An explicit Library write, available even for a read-only project.
    bool saveSampleDefaults(std::int64_t clip, const std::filesystem::path &defaultsIndex);
    struct ClipRelinkTarget {
        std::int64_t clipId{}, mediaId{};
        std::string hash, originalName;
    };
    std::optional<ClipRelinkTarget> clipRelinkTarget(std::int64_t clip);
    bool relinkClipMedia(const ClipRelinkTarget &, const std::filesystem::path &);
    bool relinkMediaFile(std::int64_t mediaId, const std::filesystem::path &);
    bool relinkMediaFile(std::int64_t mediaId, std::string_view expectedHash, const std::filesystem::path &);
    // An audio file imported with the prompt's choices (E5; Live 12 manual 9.2; ADR-0132 d6): Warp,
    // Create Fades on Clip Edges, the file copied into the project. The engine's material import
    // (`clip_edit::prepareImportAudio`, #393) reads, hashes and analyses the file and stages its copy
    // without writing the project, so it runs off the message thread: `materialJob` captures what it
    // needs here (the project's file, the ids, the rate), `prepareMaterial` runs anywhere on a
    // connection of its own, and `commitMaterial` lands it here as one undo step, or refuses as
    // stale when the project changed meanwhile, leaving nothing behind.
    struct MaterialJob {
        std::filesystem::path project, source;
        std::filesystem::path defaultsIndex;
        std::int64_t track = 0, clipId = 0, mediaId = 0, atTicks = 0, importedUtc = 0;
        ImportChoices choices;
        double rate = 48000.0;
        edit_plan::EditPolicy placementPolicy;
    };
    MaterialJob materialJob(const std::string &utf8Path, std::int64_t track, std::int64_t atTicks,
                            const ImportChoices &, std::int64_t nowUtc, edit_plan::EditPolicy = {}) const;
    static clip_edit::MaterialEdit prepareMaterial(const MaterialJob &, std::stop_token = {});
    // The new clip's id when it landed; 0 with `error()` saying why when it did not.
    std::int64_t commitMaterial(clip_edit::MaterialEdit &, const MaterialJob &);
    struct MaterialBatchJob {
        std::filesystem::path project, defaultsIndex;
        clip_edit::ImportBatch batch;
        double rate = 48000.0;
    };
    MaterialBatchJob materialBatchJob(const std::vector<std::pair<std::string, std::string>> &files,
                                      std::int64_t track, std::int64_t atTicks, bool separate, const ImportChoices &,
                                      const RecordingDefaults &, std::int64_t nowUtc) const;
    static clip_edit::MaterialEdit prepareMaterialBatch(MaterialBatchJob, std::stop_token = {});
    bool commitMaterialBatch(clip_edit::MaterialEdit &, const MaterialBatchJob &);
    // The audio clips' waveforms (N40, #404): peaks built on a worker of their own, which reads the
    // project through its own read-only store (`Store::open(path, error, true)`), never this one.
    WaveformCache &waveforms() noexcept { return waveforms_; }

  private:
    enum class TrackRenderKind { Freeze, BounceToNew, BounceInPlace };
    struct TrackRenderWork;
    bool startTrackRender(TrackRenderKind, std::int64_t track);
    void pollTrackRender();
    void stopTrackRender();
    enum class PresetLoad { Insert, InPlace, Default };
    std::optional<preset::Preset> captureDevicePreset(const DevicePanelData &);
    bool finishInsert(PreparedBuiltinInsert, const preset::Identity &);
    std::int64_t finishDevicePreset(PreparedBuiltinInsert, preset::Preset, PresetLoad = PresetLoad::Insert);
    void publishDeviceEdit(std::optional<std::int64_t> newTrack);
    std::optional<HotSwapTargetInfo> hotSwapTargetAfterCapture(const DevicePanelData &);
    std::vector<DeviceMoveSource> deviceRange(std::span<const DevicePanelData>, bool mutation);
    bool editDeviceRange(std::span<const DevicePanelData>, std::vector<DeviceCopy> *clipboard, bool remove);
    ProjectDocument(std::unique_ptr<Store>, std::string name);
    // The clock the capture's timers run on: steady milliseconds, the same for the timer's drain
    // and for a gesture, so a gesture's quiet timer and its End agree.
    static std::int64_t captureNowMs() noexcept;
    bool capturePluginStateImpl(std::optional<std::int64_t> deviceId, bool bestEffort);
    bool markStalePluginStates(std::span<const std::int64_t> stale, bool complete);
    // What the drain and a gesture both do with the requests the capture appended: the state
    // blobs first (ADR-0142), then ONE transaction, then the session follows. Nothing appended
    // is nothing done. False with `error()` set when the store refused.
    bool commitCaptured(std::vector<OpRequest> &requests);
    bool drainParameterEdits(int gestureLimit, std::int64_t quietDelayMs = 0);
    // Why the producer refused a gesture, for the Status Bar: the device, the parameter or the
    // value, in that order.
    std::string refusalReason(std::int64_t id, const std::string &param);
    // The snapshot follows the session's overridden lanes (ADR-0162, N26): the session finds an
    // override when it re-reads the rows, after the submitter's refresh, or a live gesture sets one
    // with no op at all, and Re-Enable clears them. The view is rebuilt only when the two disagree,
    // so a frame that changes nothing builds nothing; the rows did not move, so the session has
    // nothing to re-read after it.
    void followOverrides();
    void refreshSessionSources();
    struct AutomationPass {
        automation::LaneSpec target;
        std::optional<std::int64_t> suspendedLane;
        std::optional<engine::DeviceRuntimeToken> device;
        std::shared_ptr<const gesture::OwnerToken> owner;
        std::vector<std::vector<AutomationPoint>> spans;
        engine::Session::DriverClock last;
        bool touched = false, nativeReleased = false;
        void append(std::int64_t sample,double value);
        void advance(const engine::Session::DriverClock &);
        void value(const engine::Session::DriverClock &,double value);
        void close(const engine::Session::DriverClock &);
    };
    void recordMixerValues(std::span<const OpRequest>);
    void advanceAutomationWrites();
    bool closeAutomationWrites(std::optional<std::int64_t> deviceId = {});
    bool commitAutomation(std::vector<AutomationPass>,std::vector<OpRequest> requests={});
    std::optional<AutomationPass> prepareParameterAutomation(std::int64_t,const device::ParamDescriptor&,
                                                            double before,double after);
    void suspendParameterAutomation(AutomationPass&);
    bool observeNativeAutomation(std::int64_t,const device::ParamDescriptor&,engine::ParamEventKind,double before,double after,
                                 const std::shared_ptr<const gesture::OwnerToken>&);
    bool finishNativeAutomation(bool stop=false, std::optional<std::int64_t> deviceId = {});
    bool releaseParameterPreview(const std::shared_ptr<const gesture::OwnerToken>&);
    bool commitParameterAutomation(std::int64_t,const std::string&);
    std::vector<AutomationPass> mixerWrites_, nativeWrites_;
    std::map<std::pair<std::int64_t,std::string>,AutomationPass> parameterWrites_;
    [[nodiscard]] device::DeviceInstance* limiterReductionTarget(
        std::int64_t id, const engine::DeviceRuntimeToken& expected);
    void refreshLimiterReductionSources();
    // The session's monitoring inputs for the tracks as they stand, given when they changed.
    void syncMonitorInputs();
    std::span<const std::uint32_t> channelsForInput(const std::optional<std::string> &ref,
                                                   std::int32_t legacyCount) const noexcept;
    std::optional<std::vector<ExternalAudioInput>> externalAudioInputs_;
    std::shared_ptr<ExternalInputMeter> externalInputMeter_ = std::make_shared<ExternalInputMeter>();
    std::int32_t inputChannels_ = 0;
    std::vector<std::pair<std::int64_t, std::vector<std::uint32_t>>> monitorInputs_;
    void persistReportedLatencies();
    void refreshReportedLatenciesForSave();
    void updateDailyHistory(bool closing = false);
    bool followSaveAs(const StoreFileResult &, bool keptTake);
    struct SaveJob;
    std::unique_ptr<Store> store_;
    std::string name_;
    DailyHistorySnapshots dailyHistory_;
    AppliedSource dailyHistorySource_;
    std::uint64_t dailyHistorySequence_ = 0;
    DailyHistoryClock dailyHistoryClock_;
    engine::ProjectView view_;
    OpSubmitter ops_;
    ViewStateStore views_;
    SessionState sessionState_;
    engine::Session session_;
    engine::DeviceLoader renderLoader_;
    std::unique_ptr<TrackRenderWork> trackRender_;
    std::optional<std::string> trackRenderStatus_;
    std::function<void(std::function<void()>)> postTrackRenderHost_;
    double freezeTailSeconds_ = 4.0;
    engine::ParamOps parameterOps_; // destroyed before Session and its instances
    struct SurfaceGesture {
        std::uint64_t instance = 0;
        std::shared_ptr<const gesture::OwnerToken> owner;
    };
    std::map<std::pair<std::int64_t, std::string>, SurfaceGesture> surfaceGestures_;
    std::function<std::uint32_t(std::int32_t)> graphWorkerCount_;
    std::int32_t graphWorkerMinimumFrames_ = 1;
    double engineRate_ = 48000.0;   // the rate `prepare` last set, for a sample's preparation
    std::int32_t engineFrames_ = 512; // the block `prepare` last set, for the recorder
    // Arrangement Record (MX-09): capture, staged audio files, then one owner commit.
    struct TakePublication;
    struct RecoveryWork;
    struct MidiRecording;
    void pollRecording(bool deviceStopped);
    void landTake();
    TakeLanded commitMidiTake(MidiRecording &, const std::string &audioProblem, const std::string &audioLocation);
    void finishMidiTake(const MidiRecording &, const recording::RecordingEdit &, const SubmitResult &, TakeLanded &);
    void pollTakePublication();
    bool waitForTake(bool forceDetached);
    // Discards a take that is not kept; what the Status Bar adds when its files stay (DF-E44), else empty.
    std::string discardTake(engine::AudioRecordingResult &);
    bool recording_ = false, landing_ = false;
    bool recordingRecoveryEnabled_ = false;
    std::unique_ptr<RecoveryWork> recoveryWork_;
    std::unique_ptr<MidiRecording> midiRecording_;
    std::optional<engine::AudioRecordingResult> audioTake_;
    bool recordingAudio_ = false;
    bool cancelTake_ = false;
    // Both recorders must be installed before either observes the next rolling callback.
    std::atomic<bool> captureReady_{false};
    std::atomic<bool> deviceStopped_{true}; // `release` without a `prepare` since
    std::optional<TakeLanded> landed_;
    std::unique_ptr<TakePublication> takePublication_;
    // The mixer drag's gesture (MX-23): the coordinator of this store's gestures, and the open session.
    std::shared_ptr<gesture::Coordinator> coordinator_;
    std::unique_ptr<gesture::Session> mixerDrag_;
    ParameterFeed feed_{parameterOps_};
    // Owner-only. Refresh the pointer even when latency fields still match so
    // an old load of controls is never held after its display changes.
    std::shared_ptr<const ParameterPublication> reportedLatencyFeed_;
    std::uint64_t reportedLatencySequence_ = 0;
    double reportedLatencyRate_ = 0;
    struct AnalysisSource { engine::ScopeLease lease; double seconds = 0; };
    std::map<engine::ScopeSource, AnalysisSource> analysisSources_;
    std::shared_ptr<SharedAnalysis> analysis_;
    TransportMailbox mailbox_;
    std::uint64_t realisedGeneration_ = 0;
    std::string error_, warning_, pluginStateWarning_;
    // Last, so its worker stops before anything else goes; it touches nothing of the above.
    // Where the document's file is now, for the waveform worker: Save As moves it.
    std::shared_ptr<WaveformSource> waveformSource_;
    WaveformCache waveforms_;
    struct ReductionSource {
        engine::DeviceRuntimeToken expected;
        std::weak_ptr<mix::limiter::ReductionMailbox> mailbox;
    };
    std::map<std::int64_t, ReductionSource> limiterReductionSources_;
    std::int64_t limiterReductionPublication_ = 0;
    std::unique_ptr<SaveJob> saveJob_;
};
} // namespace adi::ui
