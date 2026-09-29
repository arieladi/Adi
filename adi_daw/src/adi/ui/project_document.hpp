// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "adi/ui/op_submitter.hpp"
#include "adi/ui/window_state.hpp"
namespace adi::ui {
using media::pathFromUtf8;
// Message-thread project lifetime. The callback is detached before destruction.
// Only process() accesses Transport while the driver is running.
class ProjectDocument final : public engine::BlockProcessor {
  public:
    static std::unique_ptr<ProjectDocument> open(const std::filesystem::path &, bool create,
                                                 engine::DeviceLoader, std::string &error);
    bool save(std::string &error);
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
    std::uint64_t realisedGeneration_ = 0;
    std::string error_;
};
} // namespace adi::ui
