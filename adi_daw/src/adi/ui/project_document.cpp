// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_document.hpp"
#include <chrono>
namespace adi::ui {
ProjectDocument::ProjectDocument(std::unique_ptr<Store> s, std::string name)
    : store_(std::move(s)), name_(std::move(name)), ops_(*store_, view_), views_(*store_),
      sessionState_(*store_) {}
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
        document->parameterOps_.attachSession(document->session_);
        document->feed_.publish(document->session_);
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
    for (std::size_t i = 0; i < session_.entryCount(); ++i) {
        const auto &entry = session_.entryAt(i);
        if (entry.retired)
            parameterOps_.detach(entry.deviceId);
    }
    parameterOps_.attachSession(session_);
    error_.clear();
    return true;
}
void ProjectDocument::tick(std::int64_t milliseconds) {
    synchronise();
    session_.tick(milliseconds);
    std::vector<OpRequest> requests;
    const auto captureNow = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    parameterOps_.drain(captureNow, requests);
    if (!requests.empty() && parameterOps_.writeBlobs(*store_, error_)) {
        const auto result = ops_.submit(requests);
        if (!result.ok)
            error_ = result.error;
        else
            synchronise();
    }
    feed_.publish(session_);
}
bool ProjectDocument::autoGainStage(bool lufs) {
    engine::GainStageOptions options;
    options.measure =
        lufs ? engine::GainStageMeasure::IntegratedLufs : engine::GainStageMeasure::GatedRms;
    const auto result = session_.autoGainStage(*store_, options);
    error_ = result.commit.error;
    if (!result.commit.ok)
        return false;
    view_.refresh(*store_);
    return synchronise();
}
bool ProjectDocument::deviceAction(const std::string &op, Payload payload) {
    OpRequest r;
    r.opType = op;
    r.payload = std::move(payload);
    const auto result = ops_.submit(r);
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    feed_.publish(session_);
    return true;
}
bool ProjectDocument::parameterGesture(std::int64_t id, const std::string &,
                                       engine::ParamEventKind, double) {
    const auto *entry = session_.entryFor(id);
    if (!entry || entry->retired)
        return false; // re-read on landing
    // A drag in the strip is an edit and goes through the capture, so a first touch
    // emits its opener and undo lands on a value (ADR-0124, ADR-0181 d5). ParamOps
    // has a producer per plug-in and none yet for the UI: that is the engine lane's
    // (collab/window.md, N7). Until then the strip shows values and edits nothing.
    error_ = "Editing a parameter from the strip waits for the engine's UI producer.";
    return false;
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
