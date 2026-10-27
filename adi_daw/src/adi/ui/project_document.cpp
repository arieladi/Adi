// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_document.hpp"
namespace adi::ui {
ProjectDocument::ProjectDocument(std::unique_ptr<Store> s, std::string name)
    : store_(std::move(s)), name_(std::move(name)), ops_(*store_, view_), views_(*store_) {}
std::unique_ptr<ProjectDocument> ProjectDocument::open(const std::filesystem::path &path,
                                                       bool create, engine::DeviceLoader loader,
                                                       std::string &error) {
    try {
        StoreError result{};
        auto store = create ? Store::create(path, result) : Store::open(path, result);
        if (!store) {
            error = "Cannot open project: " + std::string(toString(result));
            return {};
        }
        const auto stem = path.stem().u8string();
        auto document = std::unique_ptr<ProjectDocument>(new ProjectDocument(
            std::move(store), std::string(reinterpret_cast<const char *>(stem.data()), stem.size())));
        document->view_.refresh(*document->store_);
        document->ops_.protectRoot(kNewProjectLabel);
        if (create) {
            // A new project gets its Main track through the op every track is made
            // with: step 7 writes no rows of its own (STEP-7-PLAN §3.0). Store::create
            // writes neither the project row nor the Main track, and a bootstrap that
            // is not an undo step is the engine lane's (collab/window.md).
            OpRequest main;
            main.opType = "track.create";
            main.payload = {{"id", 1}, {"kind", "master"}, {"name", "Main"}};
            main.label = kNewProjectLabel;
            if (const auto made = document->ops_.submit(std::move(main)); !made) {
                error = made.error;
                return {};
            }
        }
        engine::SessionSpec spec;
        spec.sampleRate = document->view_.current()->sampleRate;
        if (!document->session_.load(*document->store_, std::move(loader), spec)) {
            error = document->session_.error();
            return {};
        }
        document->realisedGeneration_ = document->view_.generation();
        error.clear();
        return document;
    } catch (const std::exception &e) {
        error = e.what();
        return {};
    }
}
History::SnapshotResult ProjectDocument::takeSnapshot(const std::string &name,
                                                      std::int64_t nowUtc, int utcOffsetMinutes) {
    if (store_->readOnly()) {
        History::SnapshotResult refused;
        refused.error = "Project is read-only";
        return refused;
    }
    History history(*store_);
    return history.takeSnapshot(name, nowUtc, utcOffsetMinutes);
}
std::string ProjectDocument::snapshotName(std::int64_t nowUtc, int utcOffsetMinutes) const {
    return History::defaultSnapshotName(name_, nowUtc, utcOffsetMinutes);
}
bool ProjectDocument::synchronise() {
    if (realisedGeneration_ == view_.generation())
        return true;
    if (!session_.refresh(*store_)) {
        error_ = session_.error();
        return false;
    }
    realisedGeneration_ = view_.generation();
    error_.clear();
    return true;
}
void ProjectDocument::tick(std::int64_t milliseconds) {
    synchronise();
    session_.tick(milliseconds);
}
void ProjectDocument::prepare(double rate, std::int32_t frames) {
    session_.prepare(rate, frames);
    mailbox_.setSampleRate(rate);
}
void ProjectDocument::release() { session_.release(); }
void ProjectDocument::process(const engine::AudioIo &io) noexcept {
    mailbox_.drain(session_.transport());
    session_.process(io);
    mailbox_.publish(session_.transport());
}
} // namespace adi::ui
