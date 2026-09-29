// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_document.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/wav_file.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <chrono>
#include <limits>
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
bool ProjectDocument::addAudioTrack(std::int64_t &id) {
    try {
        id = store_->db().execAndGet("SELECT COALESCE(MAX(id),0)+1 FROM tracks").getInt64();
        OpRequest request;
        request.opType = "track.create";
        request.payload = {
            {"id", id}, {"kind", "audio"}, {"name", "Audio " + std::to_string(id)}, {"index", id}};
        auto result = ops_.submit(request);
        error_ = result.error;
        return result.ok && synchronise();
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
}
bool ProjectDocument::dropAudio(const std::filesystem::path &source, std::int64_t track,
                                std::int64_t tick) {
    try {
        const auto current = view_.current(); // Re-read on landing, never retain a drag's snapshot.
        const auto *target = current->findTrack(track);
        if (!target || target->kind != "audio" || tick < 0) {
            error_ = "Drop needs an audio track and nonnegative position";
            return false;
        }
        audio::DecodeCache cache(media::projectFolder(*store_) / ".adi-decode");
        const auto playable = cache.playable(source);
        audio::WavReader wav(playable);
        if (wav.frames() == 0 ||
            wav.frames() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            error_ = "Invalid audio length";
            return false;
        }
        auto mediaId =
            store_->db().execAndGet("SELECT COALESCE(MAX(id),0)+1 FROM media_files").getInt64();
        auto clipId = store_->db().execAndGet("SELECT COALESCE(MAX(id),0)+1 FROM clips").getInt64();
        OpRequest import;
        const auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        auto prepared = media::prepareImport(*store_, source, mediaId, time, import);
        if (!prepared.ok) {
            error_ = prepared.error;
            return false;
        }
        mediaId = prepared.mediaId;
        std::vector<OpRequest> requests;
        if (!prepared.existing) {
            auto &row = import.payload["row"];
            row["sample_rate"] = wav.sampleRate();
            row["channels"] = wav.channels();
            row["frames"] = wav.frames();
            row["duration_ns"] = static_cast<std::int64_t>(static_cast<double>(wav.frames()) /
                                                           wav.sampleRate() * 1e9);
            // Encoding remains the original container; decoded PCM precision is not original bit
            // depth.
            row["format"] = media::pathUtf8(source.extension());
            requests.push_back(std::move(import));
        }
        const auto end =
            current->tempo->secondsToTicks(current->tempo->ticksToSeconds(tick) +
                                           static_cast<double>(wav.frames()) / wav.sampleRate());
        OpRequest clip;
        clip.opType = "clip.create";
        clip.payload = {{"id", clipId},    {"track", track},
                        {"kind", "audio"}, {"name", media::pathUtf8(source.stem())},
                        {"pos", tick},     {"length", std::max<std::int64_t>(1, end - tick)}};
        requests.push_back(std::move(clip));
        OpRequest audio;
        audio.opType = "clip.attachAudio";
        audio.payload = {{"id", clipId}, {"media", mediaId}, {"frames", wav.frames()}};
        requests.push_back(std::move(audio));
        auto result = ops_.submit(requests);
        error_ = result.error;
        return result.ok && synchronise();
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
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
