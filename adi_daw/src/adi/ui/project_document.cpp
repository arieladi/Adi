// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_document.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/dsp/oneshot.hpp"
#include "adi/media/blake3.hpp"
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
        document->parameterOps_.attachSession(document->session_);
        document->feed_.publish(document->session_);
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
bool ProjectDocument::addOneShot(const std::filesystem::path &source, std::int64_t &track) {
    try {
        audio::DecodeCache cache(media::projectFolder(*store_) / ".adi-decode");
        device::OneShot sampler;
        if (!sampler.loadSample(cache.playable(source), error_))
            return false;
        const auto bytes = sampler.saveState("component");
        const auto hash = media::blake3Bytes(std::as_bytes(std::span(bytes.data(), bytes.size())));
        if (!hash) {
            error_ = "Cannot hash sample state";
            return false;
        }
        auto &db = store_->db();
        // Content-addressed bytes and plugin identity are facts, not editable project rows.
        SQLite::Statement blob(
            db, "INSERT OR IGNORE INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
        blob.bind(1, hash.hex);
        blob.bind(2, bytes.data(), static_cast<int>(bytes.size()));
        blob.bind(3, static_cast<std::int64_t>(bytes.size()));
        blob.exec();
        SQLite::Statement find(
            db, "SELECT id FROM plugin_refs WHERE format='internal' AND uid='adi.oneshot'");
        std::int64_t ref = 0;
        if (find.executeStep())
            ref = find.getColumn(0).getInt64();
        else {
            ref = db.execAndGet("SELECT COALESCE(MAX(id),0)+1 FROM plugin_refs").getInt64();
            SQLite::Statement q(
                db, "INSERT INTO plugin_refs(id,format,uid,name,vendor,version,subtype) "
                    "VALUES(?,'internal','adi.oneshot','OneShot','ADI','1','instrument')");
            q.bind(1, ref);
            q.exec();
        }
        track = db.execAndGet("SELECT COALESCE(MAX(id),0)+1 FROM tracks").getInt64();
        const auto chain =
            db.execAndGet("SELECT COALESCE(MAX(id),0)+1 FROM device_chains").getInt64();
        const auto dev = db.execAndGet("SELECT COALESCE(MAX(id),0)+1 FROM devices").getInt64();
        std::vector<OpRequest> batch;
        auto append = [&](const char *op, Payload payload) {
            OpRequest r;
            r.opType = op;
            r.payload = std::move(payload);
            r.label = "Add OneShot";
            batch.push_back(std::move(r));
        };
        append("track.create",
               {{"id", track}, {"kind", "midi"}, {"name", "OneShot"}, {"index", track}});
        append("chain.create", {{"id", chain}, {"track", track}});
        Payload params = Payload::array();
        for (int i = 0; i < sampler.paramCount(); ++i) {
            const auto &p = *sampler.paramAt(i);
            const auto v = sampler.getParam(p.id);
            params.push_back({{"param", p.id},
                              {"name", p.name},
                              {"norm", v.normalized},
                              {"real", v.real},
                              {"unit", p.unit}});
        }
        append("device.insert",
               {{"id", dev},
                {"chain", chain},
                {"ord", 0},
                {"ref", ref},
                {"name", "OneShot"},
                {"params", params},
                {"state", Payload::array({{{"role", "component"}, {"hash", hash.hex}}})}});
        append("device.setPanel",
               {{"dev", dev},
                {"params", Payload::array({"mode", "start", "end", "length", "loop_on",
                                           "loop_length", "fade", "gate", "slice_by", "regions",
                                           "filter_on", "cutoff", "lfo_on", "volume"})}});
        const auto result = ops_.submit(batch);
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
bool ProjectDocument::learnTrack(std::int64_t track, const std::string &param,
                                 const ControllerCc &cc, std::span<const int> values,
                                 const ControllerPolicy &policy) {
    if (!view_.current()->findTrack(track)) {
        error_ = "Track no longer exists";
        return false;
    }
    const auto id =
        store_->db().execAndGet("SELECT COALESCE(MAX(id),0)+1 FROM controller_maps").getInt64();
    auto op = learnControllerTarget(id, "track", track, param, cc, values, policy, error_);
    if (!op)
        return false;
    const auto result = ops_.submit(*op);
    error_ = result.error;
    return result.ok;
}
bool ProjectDocument::unbindTrack(std::int64_t track, const std::string &param) {
    std::vector<OpRequest> ops;
    for (const auto &b : bindings())
        if (b.binding.at("target_kind") == "track" && b.binding.at("target_id") == track &&
            b.binding.at("target_param") == param) {
            OpRequest r;
            r.opType = "controller.unbind";
            r.payload = {{"id", b.id}};
            ops.push_back(std::move(r));
        }
    if (ops.empty())
        return true;
    const auto result = ops_.submit(ops);
    error_ = result.error;
    return result.ok;
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
bool ProjectDocument::parameterGesture(std::int64_t id, const std::string &param,
                                       engine::ParamEventKind kind, double value) {
    const auto *entry = session_.entryFor(id);
    if (!entry || entry->retired)
        return false; // re-read on landing
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    std::vector<OpRequest> requests;
    if (!parameterOps_.uiEvent(id, param, kind, value, now, requests))
        return false;
    feed_.touched(id, param);
    if (!requests.empty()) {
        const auto result = ops_.submit(requests);
        error_ = result.error;
        if (!result.ok || !synchronise())
            return false;
    }
    feed_.publish(session_);
    return true;
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
