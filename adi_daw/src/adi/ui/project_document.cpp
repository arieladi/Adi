// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_document.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
namespace adi::ui {
ProjectDocument::ProjectDocument(std::unique_ptr<Store> s)
    : store_(std::move(s)), ops_(*store_, view_), views_(*store_) {}
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
        if (create) {
            // The bootstrap row precedes edits. Store creates schema, not this row.
            SQLite::Transaction transaction(store->db());
            SQLite::Statement row(store->db(), "INSERT INTO project(id,name) VALUES(1,?)");
            const auto name = path.stem().u8string();
            row.bind(1, std::string(reinterpret_cast<const char *>(name.data()), name.size()));
            row.exec();
            store->db().exec(
                "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'master','Master',0); "
                "INSERT INTO mixer_strip(track_id) VALUES(1)");
            transaction.commit();
        }
        auto document = std::unique_ptr<ProjectDocument>(new ProjectDocument(std::move(store)));
        document->view_.refresh(*document->store_);
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
bool ProjectDocument::save(std::string &error) {
    if (store_->readOnly()) {
        error = "Project is read-only";
        return false;
    }
    try {
        // SQLite reports BUSY in column zero rather than throwing. Never say saved
        // when an external reader prevented checkpointing the complete journal.
        SQLite::Statement checkpoint(store_->db(), "PRAGMA wal_checkpoint(TRUNCATE)");
        if (!checkpoint.executeStep() || checkpoint.getColumn(0).getInt() != 0) {
            error = "Project checkpoint is busy; try Save again";
            return false;
        }
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
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
