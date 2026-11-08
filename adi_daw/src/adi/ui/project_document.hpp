// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/ui/op_submitter.hpp"
#include "adi/ui/parameter_feed.hpp"
#include "adi/ui/session_state.hpp"
#include "adi/ui/window_state.hpp"
namespace adi::ui {
using media::pathFromUtf8;
// The history's first entry in a project made by New; never undone (Live 5.4.2).
inline constexpr const char *kNewProjectLabel = "New project";
// Message-thread project lifetime. The callback is detached before destruction.
// Only process() accesses Transport while the driver is running.
//
// There is no Save: every op is committed as it happens (ADR-0003, ADR-0203 d13),
// and the store is closed into one file when the document goes. Ctrl/Cmd+S takes
// a named snapshot instead, a point in the history that copies nothing.
class ProjectDocument final : public engine::BlockProcessor {
  public:
    static std::unique_ptr<ProjectDocument> open(const std::filesystem::path &, bool create,
                                                 engine::DeviceLoader, std::string &error);
    // Take Snapshot (ADR-0203 d6, d13; ADR-0128): history metadata, not an op,
    // so it neither refreshes the view nor enters the undo list.
    History::SnapshotResult takeSnapshot(const std::string &name, std::int64_t nowUtc,
                                         int utcOffsetMinutes);
    // What the Take Snapshot prompt is pre-filled with: the project's name and the time.
    std::string snapshotName(std::int64_t nowUtc, int utcOffsetMinutes) const;
    const std::string &name() const noexcept { return name_; }
    // Save As (Live 12 manual 5.4.1, 41.3; ADR-0203 d6; N3): the store copies the project into a
    // new `.adi` and carries on in it (`Store::saveAs`, #380), called with no live statement held
    // (SPEC §3.7); the name, the view and the session follow, and the caller retitles the window.
    // The store never overwrites, so an existing destination is refused and `error()` says why,
    // where Live asks first and then replaces. False means nothing moved; once the store has
    // switched the save stands and is true, a warning the store reports, or a session that
    // failed to re-read the new file, in `warning()`.
    bool saveAs(const std::filesystem::path &);
    const std::string &warning() const noexcept { return warning_; }
    bool synchronise();
    void tick(std::int64_t milliseconds);
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
    const std::string &error() const noexcept { return error_; }
    // The parameter feed every window and the control API drain (ADR-0180 d5).
    ParameterFeed &parameterFeed() noexcept { return feed_; }
    // A control in the device strip: Begin, Values and End on one parameter, in the plug-in's
    // normalised unit (N7; ADR-0124 d4, ADR-0181 d5; MR 9.1). Every value reaches the plug-in at
    // once; End commits the gesture as ONE undo step, its first-touch opener in front, through
    // the engine's own window producer (`ParamOps::uiEvent`). A value nothing brackets closes on
    // the drain's quiet timer, in `tick()`. False when THIS event was refused (a device that is
    // not loaded, a missing or non-automatable parameter, a value the plug-in did not take), with
    // `error()` saying which. An older gesture the producer's own drain completed on the way to
    // judging the value is committed by this call; a device, parameter or value the producer
    // refuses outright drains nothing, and the older gesture commits on the timer's next frame.
    bool parameterGesture(std::int64_t id, const std::string &param, engine::ParamEventKind,
                          double normalized);
    // Live's Re-Enable Automation (manual 25.4; MR 6.1; N26): every lane a manual change overrode
    // this run follows its envelope again, from the playhead at once, and the snapshot's
    // `automationOverridden` and each lane's `overridden` follow in the same call. Not an op and
    // not an undo step: the override is session state (ADR-0162), and so is its end. True with
    // nothing overridden too; false with `error()` when the session's rebuild failed.
    bool reenableAutomation();
    // Live's Automation Arm (manual 25.1): whether a control moved while the Arrangement Record
    // button is on becomes automation. The session's switch, on in a new Set as Live's. The window
    // does not record yet, so the session is never told that recording is on.
    void setAutomationArmed(bool armed) noexcept;
    bool automationArmed() const noexcept { return session_.automationArmed(); }
    // Auto Gain Stage (ADR-0195 d4): one offline command, one undo step.
    bool autoGainStage(bool lufs);
    // A device edit from the strip (enable, remove, panel), as one op.
    bool deviceAction(const std::string &op, Payload payload);
    // An audio file dropped on the arrangement (#308, N5): reads the file and the media pool and
    // captures the media row without writing anything; the window adds the clip and commits the
    // batch as one undo step. `nowUtc` in microseconds, as the history's own times. False with
    // `error()` saying why, a file that is not audio included.
    bool prepareImport(const std::string &utf8Path, std::int64_t nowUtc, AudioImport &out);

  private:
    ProjectDocument(std::unique_ptr<Store>, std::string name);
    // The clock the capture's timers run on: steady milliseconds, the same for the timer's drain
    // and for a gesture, so a gesture's quiet timer and its End agree.
    static std::int64_t captureNowMs() noexcept;
    // What the drain and a gesture both do with the requests the capture appended: the state
    // blobs first (ADR-0142), then ONE transaction, then the session follows. Nothing appended
    // is nothing done. False with `error()` set when the store refused.
    bool commitCaptured(std::vector<OpRequest> &requests);
    // Why the producer refused a gesture, for the Status Bar: the device, the parameter or the
    // value, in that order.
    std::string refusalReason(std::int64_t id, const std::string &param);
    // The snapshot follows the session's overridden lanes (ADR-0162, N26): the session finds an
    // override when it re-reads the rows, after the submitter's refresh, or a live gesture sets one
    // with no op at all, and Re-Enable clears them. The view is rebuilt only when the two disagree,
    // so a frame that changes nothing builds nothing; the rows did not move, so the session has
    // nothing to re-read after it.
    void followOverrides();
    std::unique_ptr<Store> store_;
    std::string name_;
    engine::ProjectView view_;
    OpSubmitter ops_;
    ViewStateStore views_;
    SessionState sessionState_;
    engine::Session session_;
    engine::ParamOps parameterOps_; // destroyed before Session and its instances
    ParameterFeed feed_;
    TransportMailbox mailbox_;
    std::uint64_t realisedGeneration_ = 0;
    std::string error_, warning_;
};
} // namespace adi::ui
