// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/stem_import.hpp"
#include "adi/audio/aac_codec.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
namespace adi::media {
namespace fs = std::filesystem;
struct StemImport::Data {
    struct Asset {
        fs::path privateFile, target;
        std::string hash;
        bool published = false;
    };
    fs::path folder;
    std::string identity;
    std::uint64_t generation = 0;
    std::optional<std::int64_t> head;
    std::vector<Asset> assets;
    std::vector<OpRequest> requests;
    StemImportOptions options;
    bool committed = false;
    ~Data() {
        if (!folder.empty()) {
            std::error_code error;
            fs::remove_all(folder, error);
        }
    }
};
namespace {
std::string identity(Store &store) {
    SQLite::Statement query(store.db(), "PRAGMA database_list");
    while (query.executeStep())
        if (query.getColumn(1).getString() == "main")
            return query.getColumn(2).getString();
    throw std::runtime_error("NI import needs a file-backed project");
}
void cancel(const StemImportOptions &options) {
    if (options.stop.stop_requested())
        throw std::runtime_error("NI Stems import cancelled");
}
void tick(const StemImportOptions &options) {
    cancel(options);
    if (options.service)
        options.service();
    cancel(options);
}
void copyAsset(const fs::path &source, const fs::path &target, const std::function<void()> &checkpoint) {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!input || !output)
        throw std::runtime_error("cannot open NI asset copy");
    std::array<char, 65536> bytes{};
    for (;;) {
        checkpoint();
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto got = input.gcount();
        if (got)
            output.write(bytes.data(), got);
        if (!output)
            throw std::runtime_error("NI asset copy write failed");
        if (input.eof())
            break;
        if (!input)
            throw std::runtime_error("NI asset copy read failed");
    }
    output.close();
    if (!output)
        throw std::runtime_error("NI asset copy close failed");
}
fs::path reserve(const fs::path &parent) {
    static std::atomic<std::uint64_t> serial{0};
    for (int i = 0; i < 128; ++i) {
        auto p = parent / (".adi-stem-import-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                           std::to_string(serial.fetch_add(1)));
        std::error_code error;
        if (fs::create_directory(p, error))
            return p;
        if (error && error != std::errc::file_exists)
            throw fs::filesystem_error("NI import stage", p, error);
    }
    throw std::runtime_error("cannot reserve NI import stage");
}
void request(std::vector<OpRequest> &out, const std::string &type, Payload payload) {
    OpRequest op;
    op.opType = type;
    op.payload = std::move(payload);
    op.actor = Actor::Import;
    op.label = "Import NI Stems";
    out.push_back(std::move(op));
}
} // namespace
bool StemImport::ready() const noexcept {
    return data_ && !data_->committed && error_.empty();
}
StemImport prepareStemImport(Store &store, const fs::path &input, const StemImportOptions &options) {
    StemImport prepared;
    try {
        if (store.readOnly())
            throw std::runtime_error("NI import project is read-only");
        if (options.group <= 0 || options.containerMedia <= 0 || options.positionTicks < 0 ||
            options.index < 0 || options.stems.empty() || options.stems.size() > 4)
            throw std::invalid_argument(
                "NI import requires positive allocated identities and a nonnegative placement");
        auto data = std::make_shared<StemImport::Data>();
        data->identity = identity(store);
        data->head = OpJournal(store).headSeq();
        data->generation = options.documentGeneration;
        data->options = options;
        const auto source = input.is_absolute() ? input : projectFolder(store) / input;
        if (!fs::is_regular_file(fs::symlink_status(source)))
            throw std::runtime_error("NI container must be a regular file");
        tick(options);
        const auto hash = blake3File(source, options.stop);
        if (!hash)
            throw std::runtime_error("cannot hash NI container");
        data->folder = reserve(projectFolder(store));
        const auto captured = data->folder / "original.stem.mp4";
        copyAsset(source, captured, [&] { tick(options); });
        const auto copiedHash = blake3File(captured, options.stop);
        if (!copiedHash || copiedHash.hex != hash.hex)
            throw std::runtime_error("NI container changed while capturing");
        const auto movie = audio::readAacMovie(captured);
        prepared.manifest_ = audio::parseStemManifest(movie.stemManifest);
        if (movie.tracks.size() != prepared.manifest_.stems.size() + 1 ||
            options.stems.size() != prepared.manifest_.stems.size())
            throw std::runtime_error("NI manifest, audio tracks and allocated identities do not match");
        if (prepared.manifest_.masteringEnabled && !options.allowRawMastering)
            throw std::runtime_error("NI mastering compressor/limiter is enabled; choose raw stems "
                                     "explicitly to import without that DSP");
        std::set<std::int64_t> tracks{options.group}, clips, medias{options.containerMedia};
        for (const auto &row : options.stems)
            if (row.track <= 0 || row.clip <= 0 || row.media <= 0 || !tracks.insert(row.track).second ||
                !clips.insert(row.clip).second || !medias.insert(row.media).second ||
                (row.paletteIndex && (*row.paletteIndex < 0 || *row.paletteIndex > 69)))
                throw std::runtime_error(
                    "NI import needs distinct positive identities and palette indices 0..69");
        std::vector<fs::path> sources{captured};
        for (std::size_t i = 0; i < options.stems.size(); ++i) {
            tick(options);
            auto wave = data->folder / ("stem-" + std::to_string(i) + ".wav");
            audio::decodeAac(movie.tracks[i + 1], wave, [&] { tick(options); });
            sources.push_back(std::move(wave));
        }
        tick(options);
        if (identity(store) != data->identity || OpJournal(store).headSeq() != data->head)
            throw std::runtime_error("project changed during NI import; prepare again");
        SQLite::Transaction capture(store.db(), SQLite::TransactionBehavior::DEFERRED);
        auto snapshot = engine::SnapshotBuilder::fromStore(store);
        const auto &tempo = *snapshot->tempo;
        const auto begin = tempo.ticksToSeconds(options.positionTicks);
        const auto duration = static_cast<double>(movie.tracks[0].frames) / movie.tracks[0].sampleRate;
        const auto end = tempo.secondsToTicks(begin + duration);
        if (!std::isfinite(begin) || begin < 0 || begin + duration > 0x1p40 || end <= options.positionTicks ||
            std::llround((tempo.ticksToSeconds(end) - begin) * movie.tracks[0].sampleRate) !=
                static_cast<std::int64_t>(movie.tracks[0].frames))
            throw std::runtime_error(
                "NI import extent cannot preserve the decoded samples on this musical clock");
        if (options.parent) {
            SQLite::Statement parent(store.db(), "SELECT kind,locked FROM tracks WHERE id=?");
            parent.bind(1, *options.parent);
            if (!parent.executeStep() || parent.getColumn(0).getString() != "group" ||
                parent.getColumn(1).getInt() != 0)
                throw std::runtime_error("NI import parent is missing, locked or not a group");
        }
        const auto audioFolder = projectFolder(store) / "audio";
        if (fs::is_symlink(fs::symlink_status(audioFolder)))
            throw std::runtime_error("NI import audio folder must not be a symlink");
        fs::create_directories(audioFolder);
        std::map<std::string, std::int64_t> byHash;
        std::vector<std::int64_t> mediaIds;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto &path = sources[i];
            const auto id = i ? options.stems[i - 1].media : options.containerMedia;
            const auto content = blake3File(path);
            if (!content)
                throw std::runtime_error("cannot hash decoded NI asset");
            if (const auto found = byHash.find(content.hex); found != byHash.end()) {
                mediaIds.push_back(found->second);
                continue;
            }
            OpRequest op;
            const auto imported = prepareImport(store, path, id, 0, op);
            if (!imported.ok)
                throw std::runtime_error(imported.error);
            if (i &&
                (!imported.audio ||
                 imported.audio->frames != static_cast<std::int64_t>(movie.tracks[0].frames) ||
                 imported.audio->sampleRate != movie.tracks[0].sampleRate || imported.audio->channels != 2))
                throw std::runtime_error("decoded NI stem dimensions changed");
            const auto target = audioFolder / ("stems-" + content.hex + (i ? ".wav" : ".stem.mp4"));
            Payload paths = {{"rel_path", pathUtf8(fs::relative(target, projectFolder(store)))},
                             {"abs_path_hint", nullptr},
                             {"missing", 0}};
            if (op.opType == "media.import") {
                op.payload["row"].update(paths);
                if (!i) {
                    op.payload["row"]["format"] = "stem.mp4";
                    op.payload["row"]["orig_name"] = pathUtf8(source.filename());
                }
            } else {
                op.opType = "media.relink";
                op.payload = {{"id", imported.mediaId}, {"hash", content.hex}, {"paths", paths}};
            }
            op.label = "Import NI Stems";
            op.actor = Actor::Import;
            data->requests.push_back(std::move(op));
            data->assets.push_back({path, target, content.hex, false});
            byHash.emplace(content.hex, imported.mediaId);
            mediaIds.push_back(imported.mediaId);
        }
        prepared.containerMedia_ = mediaIds.front();
        request(data->requests, "track.create",
                {{"id", options.group},
                 {"kind", "group"},
                 {"name", options.groupName.empty() ? pathUtf8(source.stem()) : options.groupName},
                 {"parent", options.parent ? Payload(*options.parent) : Payload(nullptr)},
                 {"index", options.index}});
        for (std::size_t i = 0; i < options.stems.size(); ++i) {
            const auto &wanted = options.stems[i];
            const auto &label = prepared.manifest_.stems[i];
            request(data->requests, "track.create",
                    {{"id", wanted.track},
                     {"kind", "audio"},
                     {"name", label.name},
                     {"parent", options.group},
                     {"index", i}});
            if (wanted.paletteIndex)
                request(data->requests, "track.setColor",
                        {{"id", wanted.track}, {"color", *wanted.paletteIndex}});
            request(data->requests, "clip.create",
                    {{"id", wanted.clip},
                     {"track", wanted.track},
                     {"kind", "audio"},
                     {"name", label.name},
                     {"pos", options.positionTicks},
                     {"length", end - options.positionTicks}});
            request(data->requests, "clip.attachAudio",
                    {{"id", wanted.clip}, {"media", mediaIds[i + 1]}, {"frames", movie.tracks[0].frames}});
        }
        capture.commit();
        prepared.data_ = std::move(data);
    } catch (const std::exception &error) {
        prepared.error_ = error.what();
    }
    return prepared;
}
CommitResult commitStemImport(Store &store, StemImport &prepared, std::uint64_t generation, Actor actor,
                              const CommitOptions &extra) {
    CommitResult refused;
    auto data = prepared.data_;
    if (!prepared.ready()) {
        refused.error =
            prepared.error_.empty() ? "NI import is absent or already committed" : prepared.error_;
        return refused;
    }
    try {
        if (generation != data->generation || identity(store) != data->identity) {
            refused.error = "NI import belongs to a different document generation/project";
            return refused;
        }
        tick(data->options);
        CommitOptions options;
        options.expectHead.emplace(data->head);
        options.beforeCommit = [data, extra](SQLite::Database &db, std::int64_t txn, std::string &error) {
            for (auto &asset : data->assets) {
                cancel(data->options);
                if (!fs::is_directory(fs::symlink_status(asset.target.parent_path()))) {
                    error = "NI asset directory changed after capture";
                    return false;
                }
                const auto hash = blake3File(asset.privateFile);
                if (!hash || hash.hex != asset.hash) {
                    error = "NI private asset changed after capture";
                    return false;
                }
                // publishFile moves its input. Keep the only decoded/source copy
                // intact until SQL commit, even after a late failure and retry.
                auto publication = asset.privateFile;
                publication += ".publication";
                std::error_code ignored;
                fs::remove(publication, ignored);
                copyAsset(asset.privateFile, publication, [&] { cancel(data->options); });
                const auto published = publishFile(publication, asset.target);
                if (published.status == PublishStatus::exists &&
                    fs::is_regular_file(fs::symlink_status(asset.target))) {
                    const auto existing = blake3File(asset.target);
                    if (existing && existing.hex == asset.hash)
                        continue;
                }
                if (published.status != PublishStatus::published) {
                    error = published.status == PublishStatus::exists
                                ? "NI owned destination contains different content"
                                : published.error;
                    return false;
                }
                asset.published = true;
            }
            cancel(data->options);
            return !extra.beforeCommit || extra.beforeCommit(db, txn, error);
        };
        auto requests = data->requests;
        for (auto &op : requests)
            op.actor = actor;
        auto result = OpJournal(store).commit(requests, options);
        if (result.ok)
            data->committed = true;
        else
            for (auto &asset : data->assets)
                if (asset.published) {
                    std::error_code ignored;
                    fs::remove(asset.target, ignored);
                    asset.published = false;
                }
        return result;
    } catch (const std::exception &error) {
        for (auto &asset : data->assets)
            if (asset.published) {
                std::error_code ignored;
                fs::remove(asset.target, ignored);
                asset.published = false;
            }
        refused.error = error.what();
        return refused;
    }
}
} // namespace adi::media
