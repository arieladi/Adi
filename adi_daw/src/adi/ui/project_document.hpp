// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
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
    bool parameterGesture(std::int64_t, const std::string &, engine::ParamEventKind, double);
    // Auto Gain Stage (ADR-0195 d4): one offline command, one undo step.
    bool autoGainStage(bool lufs);
    // A device edit from the strip (enable, remove, panel), as one op.
    bool deviceAction(const std::string &op, Payload payload);

  private:
    ProjectDocument(std::unique_ptr<Store>, std::string name);
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
    std::string error_;
};
} // namespace adi::ui
