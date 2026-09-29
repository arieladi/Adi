// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "adi/ui/op_submitter.hpp"
#include "adi/ui/window_state.hpp"
#include "parameter_feed.hpp"
namespace adi::ui {
using media::pathFromUtf8;
// Message-thread project lifetime. The callback is detached before destruction.
// Only process() accesses Transport while the driver is running.
class ProjectDocument final : public engine::BlockProcessor {
  public:
    static std::unique_ptr<ProjectDocument> open(const std::filesystem::path &, bool create,
                                                 engine::DeviceLoader, std::string &error);
    bool addAudioTrack(std::int64_t &id);
    bool dropAudio(const std::filesystem::path &, std::int64_t track, std::int64_t tick);
    bool save(std::string &error);
    ParameterFeed &parameterFeed() noexcept { return feed_; }
    bool parameterGesture(std::int64_t, const std::string &, engine::ParamEventKind, double);
    bool deviceAction(const std::string &op, Payload payload);
    bool synchronise();
    void tick(std::int64_t milliseconds);
    void prepare(double rate, std::int32_t frames) override;
    void release() override;
    void process(const engine::AudioIo &) noexcept override;
    Store &store() noexcept { return *store_; }
    engine::ProjectView &view() noexcept { return view_; }
    OpSubmitter &ops() noexcept { return ops_; }
    ViewStateStore &views() noexcept { return views_; }
    TransportMailbox &mailbox() noexcept { return mailbox_; }
    engine::Session &session() noexcept { return session_; }
    const std::string &error() const noexcept { return error_; }

  private:
    explicit ProjectDocument(std::unique_ptr<Store>);
    std::unique_ptr<Store> store_;
    engine::ProjectView view_;
    OpSubmitter ops_;
    ViewStateStore views_;
    engine::Session session_;
    TransportMailbox mailbox_;
    engine::ParamOps parameterOps_; // destroyed before Session and its instances
    ParameterFeed feed_;
    std::uint64_t realisedGeneration_ = 0;
    std::string error_;
};
} // namespace adi::ui
