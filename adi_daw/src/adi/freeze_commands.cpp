// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/freeze_commands.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/freeze_fingerprint.hpp"
#include "adi/freeze_groups.hpp"
#include "adi/freeze_state.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/render/private_copy.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
namespace adi::freeze {
namespace fs = std::filesystem;
struct FreezeFiles {
    struct File {
        fs::path source, target;
        std::string hash;
    };
    render::FreezeAudioCapture capture;
    std::vector<OpRequest> requests;
    std::vector<File> files;
    bool committed = false;
};
bool FreezeEdit::ready() const noexcept {
    return files_ && !files_->committed && files_->capture.ready() && error_.empty();
}
namespace {
std::string identity(Store &store) {
    SQLite::Statement q(store.db(), "PRAGMA database_list");
    while (q.executeStep())
        if (q.getColumn(1).getString() == "main")
            return q.getColumn(2).getString();
    throw std::runtime_error("project filename is unavailable");
}
Payload identities(const engine::OfflineSourceInfo &source) {
    Payload values = Payload::array();
    for (const auto &device : source.devices)
        values.push_back({{"id", device.id},
                          {"format", device.identity.format},
                          {"uid", device.identity.uid},
                          {"version", device.identity.version},
                          {"vendor", device.identity.vendor}});
    return values;
}
OpRequest request(std::int64_t id, const Payload &before, const Payload &after, const char *label) {
    OpRequest q;
    q.opType = "track.setFreeze";
    q.payload = {{"id", id}, {"before", before}, {"after", after}};
    q.label = label;
    q.targetKind = "track";
    q.targetId = id;
    return q;
}
void destination(OpRequest &request, const fs::path &file, Store &store) {
    auto &paths = request.payload.at(request.opType == "media.import" ? "row" : "paths");
    paths["rel_path"] = media::pathUtf8(file.lexically_relative(media::projectFolder(store)));
    paths["abs_path_hint"] = media::pathUtf8(file);
    paths["missing"] = 0;
}
} // namespace
FreezeEdit prepareFreeze(Store &store, render::FreezeAudioCapture capture, std::uint64_t generation,
                         std::int64_t nextId, std::int64_t utc) {
    FreezeEdit result;
    try {
        if (audio::onAudioThread || store.readOnly())
            throw std::runtime_error("freeze preparation requires the writable document owner");
        if (!capture.ready() || capture.streams().empty())
            throw std::runtime_error("freeze capture is incomplete");
        if (capture.sourceGeneration() != generation || capture.sourceFile() != identity(store) ||
            capture.sourceHead() != OpJournal(store).headSeq())
            throw std::runtime_error("project changed since freeze capture");
        if (nextId <= 0 || nextId > std::numeric_limits<std::int64_t>::max() - 256)
            throw std::runtime_error("invalid freeze media ID range");
        auto files = std::make_shared<FreezeFiles>();
        files->capture = std::move(capture);
        SQLite::Transaction read(store.db(), SQLite::TransactionBehavior::DEFERRED);
        const auto model = rows::readModel(store);
        engine::TempoMap tempo;
        for (const auto &event : model.tempo)
            tempo.events.push_back({event.posTicks, event.bpm, static_cast<int>(event.curve)});
        if (tempo.events.empty())
            tempo.events.push_back({0, 120, 0});
        const auto plan = engine::planGraph(model);
        if (!plan.problems.empty() || !model.problems.empty())
            throw std::runtime_error("freeze requires a fully resolved project");
        std::map<std::string, std::int64_t> mediaByHash;
        for (const auto &stream : files->capture.streams()) {
            const auto id = stream.track;
            const auto before = state(store.db(), id);
            const auto track = std::find_if(model.tracks.begin(), model.tracks.end(),
                                            [id](const auto &row) { return row.id == id; });
            if (track == model.tracks.end() ||
                (stream.wholeGroup
                     ? track->kind != "group"
                     : (track->kind != "audio" && track->kind != "midi" && track->kind != "instrument")) ||
                track->locked || track->frozen || !before.at("render").is_null())
                throw std::runtime_error("freeze requires an unlocked, unfrozen track or group");
            for (const auto &edge : plan.edges)
                if (!stream.wholeGroup && plan.nodes.at(edge.to).trackId == id)
                    throw std::runtime_error("freeze is unavailable while the track receives audio or "
                                             "sidechain input from another track");
            if (!stream.wholeGroup && stream.source.prefixDevices.empty() &&
                std::any_of(model.clips.begin(), model.clips.end(),
                            [id](const auto &clip) { return clip.trackId == id && clip.kind == "midi"; }))
                throw std::runtime_error(
                    "freeze cannot discard a forwarded MIDI stream; add its instrument to the track");
            FingerprintSpec spec;
            spec.root = id;
            spec.subtree = stream.wholeGroup;
            auto scoped = stream.wholeGroup ? groupMembers(model, id) : std::set<std::int64_t>{id};
            if (stream.wholeGroup)
                checkGroup(model, id, scoped, false);
            if (fingerprint(store, spec).hash != stream.fingerprint)
                throw std::runtime_error("source changed after freeze render");
            std::int64_t mediaIds[2]{};
            std::int64_t frames = 0, rate = 0, channels = 0;
            for (unsigned point = 0; point < 2; ++point) {
                const auto source = point ? stream.postFx : stream.preFx;
                if (source.parent_path() != files->capture.directory())
                    throw std::runtime_error("freeze stream is not privately owned");
                audio::WavReader wave(source);
                if (point && (frames != static_cast<std::int64_t>(wave.frames()) ||
                              rate != wave.sampleRate() || channels != wave.channels()))
                    throw std::runtime_error("freeze tap formats disagree");
                if (wave.frames() > (std::uint64_t{1} << 50))
                    throw std::runtime_error("freeze file exceeds supported duration");
                frames = static_cast<std::int64_t>(wave.frames());
                rate = wave.sampleRate();
                channels = wave.channels();
                if (frames <= 0 || static_cast<long double>(frames) * 1000000000.0L / static_cast<long double>(rate) >
                                       static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
                    throw std::runtime_error("unsupported freeze duration");
                const auto hash = media::blake3File(source);
                if (!hash)
                    throw std::runtime_error("cannot hash freeze cache");
                if (const auto local = mediaByHash.find(hash.hex); local != mediaByHash.end()) {
                    mediaIds[point] = local->second;
                    continue;
                }
                OpRequest mediaRequest;
                const auto imported = media::prepareImport(store, source, nextId++, utc, mediaRequest);
                if (!imported.ok)
                    throw std::runtime_error(imported.error);
                mediaIds[point] = imported.mediaId;
                mediaByHash.emplace(hash.hex, imported.mediaId);
                const auto target = media::projectFolder(store) / ("freeze-" + hash.hex + ".wav");
                if (imported.existing) {
                    mediaRequest.opType = "media.relink";
                    mediaRequest.payload = {
                        {"id", imported.mediaId}, {"hash", hash.hex}, {"paths", Payload::object()}};
                }
                destination(mediaRequest, target, store);
                files->requests.push_back(std::move(mediaRequest));
                files->files.push_back({source, target, hash.hex});
            }
            for (const auto &clip : model.clips)
                if (scoped.contains(clip.trackId)) {
                    if (clip.timeBase == 0 &&
                        clip.posTicks.value_or(0) >
                            std::numeric_limits<std::int64_t>::max() - clip.lengthTicks.value_or(0))
                        throw std::runtime_error("clip end overflows project time");
                    const auto end =
                        clip.timeBase == 0
                            ? tempo.ticksToSeconds(clip.posTicks.value_or(0) + clip.lengthTicks.value_or(0))
                            : (static_cast<long double>(clip.posNs.value_or(0)) + static_cast<long double>(clip.lengthNs.value_or(0))) *
                                  1e-9L;
                    if (std::llround(end * static_cast<long double>(rate)) > frames)
                        throw std::runtime_error(
                            "freeze range must include every clip and its requested tail");
                }
            auto capturedIdentities = identities(stream.source);
            for (const auto &contributor : stream.contributors)
                for (const auto &identity : identities(contributor))
                    capturedIdentities.push_back(identity);
            const auto bytes = encodePayload(capturedIdentities);
            std::vector<std::uint8_t> blob(bytes.size());
            for (std::size_t i = 0; i < bytes.size(); ++i)
                blob[i] = static_cast<std::uint8_t>(bytes[i]);
            Payload row = {{"track_id", id},
                           {"pre_media_id", mediaIds[0]},
                           {"post_media_id", mediaIds[1]},
                           {"frames", frames},
                           {"sample_rate", rate},
                           {"channels", channels},
                           {"fingerprint", stream.fingerprint},
                           {"identities", Payload::binary(std::move(blob))},
                           {"original_armed", before.at("armed")}};
            Payload after = {{"armed", 0}, {"frozen", 1}, {"media", mediaIds[1]}, {"render", row}};
            files->requests.push_back(request(id, before, after, "Freeze Tracks"));
            if (stream.wholeGroup) {
                Payload members = Payload::array();
                for (const auto member : scoped)
                    if (member != id) {
                        const auto found =
                            std::find_if(model.tracks.begin(), model.tracks.end(),
                                         [member](const auto &candidate) { return candidate.id == member; });
                        if (found == model.tracks.end())
                            throw std::runtime_error("group member disappeared during preparation");
                        members.push_back({{"id", member}, {"armed", found->recordArmed ? 1 : 0}});
                    }
                files->requests.push_back(
                    groupChange(id, groupState(store.db(), id), Payload{{"members", members}}));
            }
        }
        for (auto &operation : files->requests)
            operation.label = files->capture.streams().front().wholeGroup ? "Freeze Group" : "Freeze Tracks";
        read.commit();
        result.files_ = std::move(files);
    } catch (const std::exception &e) {
        result.error_ = e.what();
    }
    return result;
}
CommitResult commitFreeze(Store &store, FreezeEdit &prepared, std::uint64_t generation,
                          CommitOptions options) {
    CommitResult result;
    const auto files = prepared.files_;
    if (!prepared.ready() || files->committed) {
        result.error = "freeze edit is unavailable or already committed";
        return result;
    }
    if (audio::onAudioThread || generation != files->capture.sourceGeneration() || !files->capture.ready() ||
        identity(store) != files->capture.sourceFile()) {
        result.error = "freeze document changed";
        return result;
    }
    options.expectHead.emplace(files->capture.sourceHead());
    const auto previous = options.beforeCommit;
    std::vector<fs::path> created;
    options.beforeCommit = [&](SQLite::Database &db, std::int64_t txn, std::string &error) {
        if (previous && !previous(db, txn, error))
            return false;
        for (std::size_t i = 0; i < files->files.size(); ++i) {
            if (files->capture.cancelled()) {
                error = "freeze publication cancelled";
                return false;
            }
            const auto &file = files->files[i];
            const auto hash = media::blake3File(file.source, files->capture.stopToken());
            if (!hash || hash.hex != file.hash) {
                error = "freeze source bytes changed";
                return false;
            }
            const auto publication =
                files->capture.directory() / ("publication-" + std::to_string(i) + ".tmp");
            std::error_code ec;
            fs::remove(publication, ec);
            render::detail::copyPrivate(file.source, publication, files->capture.stopToken());
            const auto copied = media::blake3File(publication, files->capture.stopToken());
            if (!copied || copied.hex != file.hash) {
                error = "freeze publication copy changed";
                return false;
            }
            const auto published = media::publishFile(publication, file.target);
            if (published.status == media::PublishStatus::exists) {
                const auto status = fs::symlink_status(file.target, ec);
                const auto existing = !ec && fs::is_regular_file(status)
                                          ? media::blake3File(file.target)
                                          : media::HashResult{{}, media::HashError::open};
                if (existing && existing.hex == file.hash)
                    continue;
            }
            if (published.status != media::PublishStatus::published) {
                error = "cannot publish freeze cache without replacing a file";
                return false;
            }
            created.push_back(file.target);
        }
        if (files->capture.cancelled()) {
            error = "freeze publication cancelled";
            return false;
        }
        return true;
    };
    result = OpJournal(store).commit(files->requests, options);
    if (result.ok)
        files->committed = true;
    else
        for (const auto &file : created) {
            std::error_code ec;
            fs::remove(file, ec);
        }
    return result;
}
CommitResult unfreeze(Store &store, std::span<const std::int64_t> tracks, bool acknowledgeChanged,
                      std::function<bool(std::int64_t, const Payload &, std::string &)> installed) {
    CommitResult result;
    try {
        if (audio::onAudioThread || tracks.empty() || tracks.size() > 128)
            throw std::runtime_error("unfreeze requires the document owner and 1..128 tracks");
        const auto head = OpJournal(store).headSeq();
        const auto scopes = groups(store.db());
        std::vector<OpRequest> requests;
        std::set<std::int64_t> unique;
        for (const auto id : tracks) {
            if (!unique.insert(id).second)
                throw std::runtime_error("duplicate unfreeze track");
            const auto before = state(store.db(), id);
            const auto &row = before.at("render");
            if (row.is_null() && before.at("frozen") == 0)
                throw std::runtime_error("track is not frozen");
            if (!acknowledgeChanged && !row.is_null()) {
                FingerprintSpec spec;
                spec.root = id;
                spec.subtree = scopes.contains(id);
                if (fingerprint(store, spec).hash != row.at("fingerprint").get<std::string>())
                    throw std::runtime_error(
                        "frozen source changed outside native edits; acknowledge before unfreezing");
                const auto &binary = row.at("identities").get_binary();
                std::vector<std::byte> bytes(binary.size());
                for (std::size_t i = 0; i < bytes.size(); ++i)
                    bytes[i] = static_cast<std::byte>(binary[i]);
                std::string error;
                const auto decoded = decodeFromLog(bytes, error);
                if (!decoded || (installed && !installed(id, *decoded, error)))
                    throw std::runtime_error(error.empty() ? "installed device identity changed" : error);
            }
            if (scopes.contains(id))
                requests.push_back(
                    groupChange(id, groupState(store.db(), id), Payload{{"members", nullptr}}));
            auto after = before;
            after["render"] = nullptr;
            after["frozen"] = 0;
            after["media"] = nullptr;
            if (!row.is_null())
                after["armed"] = row.at("original_armed");
            requests.push_back(request(id, before, after, "Unfreeze Tracks"));
        }
        for (auto &operation : requests)
            operation.label = "Unfreeze Tracks";
        CommitOptions options;
        options.expectHead.emplace(head);
        return OpJournal(store).commit(requests, options);
    } catch (const std::exception &e) {
        result.error = e.what();
        return result;
    }
}
} // namespace adi::freeze
