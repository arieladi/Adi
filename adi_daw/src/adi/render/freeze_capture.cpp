// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/render/freeze_capture.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/freeze_fingerprint.hpp"
#include "adi/freeze_groups.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/render/private_copy.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/Backup.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <sqlite3.h>
#include <stdexcept>
namespace adi::render {
namespace fs = std::filesystem;
struct FreezeAudioCapture::Data {
    fs::path directory;
    std::string sourceFile;
    std::optional<std::int64_t> head;
    std::uint64_t generation = 0;
    std::stop_token stop;
    std::vector<FrozenStreamCapture> streams;
    ~Data() {
        if (!directory.empty()) {
            std::error_code error;
            fs::remove_all(directory, error);
        }
    }
};
namespace {
struct Cancelled {};
void cancellation(const Control &control) {
    if (control.stop.stop_requested())
        throw Cancelled{};
}
fs::path reserve(const fs::path &parent) {
    static std::atomic<std::uint64_t> sequence{0};
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        auto path = parent / (".adi-freeze-" +
                              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                              "-" + std::to_string(sequence.fetch_add(1)));
        std::error_code error;
        if (fs::create_directory(path, error))
            return path;
        if (error && error != std::errc::file_exists)
            throw fs::filesystem_error("freeze capture stage", path, error);
    }
    throw std::runtime_error("cannot reserve freeze capture stage");
}
std::string identity(Store &store) {
    SQLite::Statement q(store.db(), "PRAGMA database_list");
    while (q.executeStep())
        if (q.getColumn(1).getString() == "main")
            return q.getColumn(2).getString();
    throw std::runtime_error("freeze capture needs a file-backed project");
}
void backup(Store &source, const fs::path &path, std::stop_token stop) {
    SQLite::Database copy(media::pathUtf8(path), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    {
        SQLite::Backup capture(copy, source.db());
        if (capture.executeStep() != SQLITE_DONE)
            throw std::runtime_error("freeze project backup is busy or incomplete");
    }
    const auto folder = media::projectFolder(source);
    SQLite::Statement files(copy,
                            "SELECT id,rel_path,abs_path_hint FROM media_files WHERE rel_path IS NOT NULL");
    while (files.executeStep()) {
        const auto original = folder / media::pathFromUtf8(files.getColumn(1).getString());
        // Keep the source project's fallback, rather than rebasing relative
        // media against the private backup's staging directory.
        const auto chosen = fs::exists(original) || files.getColumn(2).isNull()
                                ? original
                                : media::pathFromUtf8(files.getColumn(2).getString());
        SQLite::Statement update(copy, "UPDATE media_files SET rel_path=NULL,abs_path_hint=? WHERE id=?");
        update.bind(1, media::pathUtf8(fs::absolute(chosen)));
        update.bind(2, files.getColumn(0).getInt64());
        update.exec();
    }
    // Pin the active project's audio assets into the private generation before
    // loading any reader. A later external edit cannot change half a render.
    std::set<std::int64_t> inactive;
    for (const auto &[root, members] : freeze::groups(copy)) {
        (void)root;
        for (const auto &member : members)
            inactive.insert(member.id);
    }
    std::set<std::int64_t> assets;
    SQLite::Statement clips(copy, "SELECT a.media_id,c.track_id FROM audio_clips a JOIN clips c ON "
                                  "c.id=a.clip_id JOIN tracks t ON t.id=c.track_id WHERE t.frozen=0");
    while (clips.executeStep())
        if (!inactive.contains(clips.getColumn(1).getInt64()))
            assets.insert(clips.getColumn(0).getInt64());
    if (copy.tableExists("frozen_renders")) {
        SQLite::Statement cached(
            copy, "SELECT pre_media_id FROM frozen_renders UNION SELECT post_media_id FROM frozen_renders");
        while (cached.executeStep())
            assets.insert(cached.getColumn(0).getInt64());
    }
    for (const auto id : assets) {
        const auto row = media::mediaRow(copy, id);
        std::string error;
        const auto sourcePath = media::resolveMedia(path.parent_path(), row, error);
        if (sourcePath.empty())
            throw std::runtime_error("freeze media: " + error);
        auto destination = path.parent_path() / ("asset-" + std::to_string(id));
        destination += sourcePath.extension();
        detail::copyPrivate(sourcePath, destination, stop);
        const auto hash = media::blake3File(destination, stop);
        if (!hash || hash.hex != row.at("hash_blake3").get<std::string>())
            throw std::runtime_error("source media changed while capturing freeze inputs");
        SQLite::Statement update(copy, "UPDATE media_files SET rel_path=NULL,abs_path_hint=? WHERE id=?");
        update.bind(1, media::pathUtf8(destination));
        update.bind(2, id);
        update.exec();
    }
}
std::int64_t endWithLatency(std::int64_t end, std::int64_t latency) {
    if (latency > 0 && end > std::numeric_limits<std::int64_t>::max() - latency)
        throw std::runtime_error("freeze end plus latency overflows");
    if (latency == std::numeric_limits<std::int64_t>::min())
        throw std::runtime_error("freeze latency exceeds supported range");
    return end + latency;
}
struct Sink {
    engine::OfflineOutput endpoint;
    std::int64_t rawEnd = 0;
    std::uint64_t written = 0;
    std::unique_ptr<audio::WavWriter> writer;
};
} // namespace
bool FreezeAudioCapture::ready() const noexcept {
    return data_ && error_.empty() && !cancelled();
}
bool FreezeAudioCapture::cancelled() const noexcept {
    return cancelled_ || (data_ && data_->stop.stop_requested());
}
std::uint64_t FreezeAudioCapture::sourceGeneration() const noexcept {
    return data_ ? data_->generation : 0;
}
std::stop_token FreezeAudioCapture::stopToken() const noexcept {
    return data_ ? data_->stop : std::stop_token{};
}
const std::vector<FrozenStreamCapture> &FreezeAudioCapture::streams() const noexcept {
    static const std::vector<FrozenStreamCapture> empty;
    return data_ ? data_->streams : empty;
}
const fs::path &FreezeAudioCapture::directory() const noexcept {
    static const fs::path empty;
    return data_ ? data_->directory : empty;
}
const std::string &FreezeAudioCapture::sourceFile() const noexcept {
    static const std::string empty;
    return data_ ? data_->sourceFile : empty;
}
std::optional<std::int64_t> FreezeAudioCapture::sourceHead() const noexcept {
    return data_ ? data_->head : std::nullopt;
}
FreezeAudioCapture captureFreezeAudio(Store &store, FreezeCaptureSpec spec, engine::DeviceLoader loader,
                                      Control control) {
    FreezeAudioCapture result;
    try {
        if (audio::onAudioThread)
            throw std::runtime_error("freeze capture requires the document worker");
        cancellation(control);
        const auto rate = spec.session.sampleRate;
        if (spec.tracks.empty() || spec.tracks.size() > 128 || spec.endSample <= 0 ||
            spec.endSample > (std::int64_t{1} << 50) || !std::isfinite(rate) || rate < 8000 ||
            rate > 768000 || std::floor(rate) != rate || spec.session.channels < 1 ||
            spec.session.channels > 64 || spec.session.maxFrames < 1 || spec.session.maxFrames > 4096 ||
            (spec.session.precision != engine::Precision::Float32 &&
             spec.session.precision != engine::Precision::Float64))
            throw std::invalid_argument(
                "freeze capture requires 1..128 distinct tracks and a bounded format/range");
        if (spec.format != audio::WavFormat::Float32 && spec.format != audio::WavFormat::Pcm16 &&
            spec.format != audio::WavFormat::Pcm24)
            throw std::invalid_argument("unsupported freeze cache format");
        std::set<std::int64_t> selected;
        for (const auto track : spec.tracks)
            if (track <= 0 || !selected.insert(track).second)
                throw std::invalid_argument("freeze capture track identities must be distinct and positive");
        auto data = std::make_shared<FreezeAudioCapture::Data>();
        data->sourceFile = identity(store);
        data->generation = spec.documentGeneration;
        data->stop = control.stop;
        data->directory = reserve(media::projectFolder(store));
        const auto captured = data->directory / "snapshot.adi";
        {
            SQLite::Transaction read(store.db(), SQLite::TransactionBehavior::DEFERRED);
            data->head = OpJournal(store).headSeq();
            backup(store, captured, control.stop);
            read.commit();
        }
        StoreError error{};
        auto snapshot = Store::open(captured, error, true);
        if (!snapshot)
            throw std::runtime_error("cannot open freeze project snapshot");
        const auto model = rows::readModel(*snapshot);
        if (spec.wholeGroups)
            for (const auto root : spec.tracks) {
                const auto members = freeze::groupMembers(model, root);
                freeze::checkGroup(model, root, members, true);
                if (freeze::childAudioRoutesOutside(model, root, members) &&
                    !spec.acknowledgeChildAudioRoutes)
                    throw std::runtime_error("Freeze Group disables child audio sends/routes until unfreeze; "
                                             "acknowledge before capture");
                for (const auto other : spec.tracks)
                    if (other != root && members.contains(other))
                        throw std::runtime_error("whole-group capture roots cannot overlap");
            }
        engine::Session session;
        session.graph().setFadeFrames(0);
        if (!session.load(*snapshot, std::move(loader), spec.session))
            throw std::runtime_error(session.error());
        if (session.stats().placeholders || session.stats().skipped || !session.problems().empty()) {
            std::string why = "freeze requires a fully resolved project";
            for (const auto &problem : session.problems())
                why += "; " + problem;
            throw std::runtime_error(why);
        }
        auto *graph = session.graph().currentGraph();
        if (!graph)
            throw std::runtime_error("freeze has no prepared graph");
        const auto channels = static_cast<std::size_t>(spec.session.channels),
                   block = static_cast<std::size_t>(spec.session.maxFrames);
        std::vector<float> narrow(channels * block);
        std::vector<double> wide(spec.session.precision == engine::Precision::Float64 ? channels * block : 0),
            interleaved(channels * block);
        std::vector<float *> fp(channels);
        std::vector<double *> dp(wide.empty() ? 0 : channels);
        for (std::size_t c = 0; c < channels; ++c) {
            fp[c] = narrow.data() + c * block;
            if (!wide.empty())
                dp[c] = wide.data() + c * block;
        }
        std::vector<Sink> sinks;
        std::int64_t lastFrame = 0;
        for (std::size_t i = 0; i < spec.tracks.size(); ++i) {
            const auto id = spec.tracks[i];
            const auto pre = session.offlineTapOutput(id, engine::Tap::PreFx),
                       post = session.offlineTapOutput(id, engine::Tap::PostFx);
            const auto info = session.offlineSourceInfo(id);
            if (!pre || !post || !info)
                throw std::runtime_error("freeze source has no complete pre/post-effect output");
            FrozenStreamCapture stream;
            stream.track = id;
            stream.source = *info;
            freeze::FingerprintSpec fingerprintSpec;
            fingerprintSpec.root = id;
            fingerprintSpec.subtree = spec.wholeGroups;
            stream.wholeGroup = spec.wholeGroups;
            if (spec.wholeGroups) {
                const auto members = freeze::groupMembers(model, id);
                std::map<std::int64_t, bool> forwarded;
                for (const auto member : members) {
                    if (member == id)
                        continue;
                    const auto track = std::find_if(model.tracks.begin(), model.tracks.end(),
                                                    [member](const auto &row) { return row.id == member; });
                    if (track != model.tracks.end() && track->kind == "vca")
                        continue;
                    const auto contributor = session.offlineSourceInfo(member);
                    if (!contributor)
                        throw std::runtime_error("group contributor has no loaded source identity");
                    stream.contributors.push_back(*contributor);
                }
                for (const auto member : members) {
                    const auto *source = member == id
                                             ? &stream.source
                                             : static_cast<const engine::OfflineSourceInfo *>(nullptr);
                    for (const auto &contributor : stream.contributors)
                        if (contributor.track == member)
                            source = &contributor;
                    const bool consumed = source && !source->prefixDevices.empty();
                    forwarded[member] =
                        !consumed &&
                        std::any_of(model.clips.begin(), model.clips.end(), [member](const auto &clip) {
                            return clip.trackId == member && clip.kind == "midi" && !clip.muted;
                        });
                }
                const auto plan = engine::planGraph(model);
                for (std::size_t pass = 0; pass < members.size(); ++pass)
                    for (const auto &edge : plan.edges) {
                        const auto from = plan.nodes.at(edge.from).trackId,
                                   to = plan.nodes.at(edge.to).trackId;
                        if (edge.bus != engine::Bus::Main || edge.kind == "send" || !members.contains(from) ||
                            !members.contains(to) || !forwarded[from])
                            continue;
                        const auto *source = to == id
                                                 ? &stream.source
                                                 : static_cast<const engine::OfflineSourceInfo *>(nullptr);
                        for (const auto &contributor : stream.contributors)
                            if (contributor.track == to)
                                source = &contributor;
                        if (!source || source->prefixDevices.empty())
                            forwarded[to] = true;
                    }
                if (forwarded[id])
                    throw std::runtime_error(
                        "Freeze Group cannot discard a forwarded MIDI stream; include its instrument");
            }
            stream.fingerprint = freeze::fingerprint(*snapshot, fingerprintSpec).hash;
            stream.preFx = data->directory / ("pre-" + std::to_string(i) + ".wav");
            stream.postFx = data->directory / ("post-" + std::to_string(i) + ".wav");
            stream.preFxLatency = pre->latency;
            stream.postFxLatency = post->latency;
            data->streams.push_back(stream);
            for (unsigned point = 0; point < 2; ++point) {
                Sink sink;
                sink.endpoint = point ? *post : *pre;
                sink.rawEnd = endWithLatency(spec.endSample, sink.endpoint.latency);
                lastFrame = (std::max)(lastFrame, sink.rawEnd);
                sink.writer = std::make_unique<audio::WavWriter>(
                    point ? stream.postFx : stream.preFx, static_cast<std::uint32_t>(rate),
                    static_cast<std::uint16_t>(channels), spec.format);
                if (sink.endpoint.latency < 0) {
                    auto zeros = (std::min)(static_cast<std::uint64_t>(spec.endSample),
                                            static_cast<std::uint64_t>(-sink.endpoint.latency));
                    while (zeros) {
                        cancellation(control);
                        const auto n =
                            static_cast<std::uint32_t>((std::min)(zeros, static_cast<std::uint64_t>(block)));
                        sink.writer->write(interleaved.data(), n);
                        sink.written += n;
                        zeros -= n;
                    }
                }
                sinks.push_back(std::move(sink));
            }
        }
        auto &coalescer = session.devices().coalescer();
        coalescer.setQuietPeriodMs(0);
        const auto reports = coalescer.stats().reports;
        session.transport().loop(0, 0, false);
        session.transport().locate(0);
        session.transport().play();
        const auto started = std::chrono::steady_clock::now();
        auto service = [&] {
            cancellation(control);
            if (control.serviceHost)
                control.serviceHost();
            cancellation(control);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            session.tick(elapsed);
            if (coalescer.stats().reports != reports || session.graph().currentGraph() != graph)
                throw std::runtime_error(
                    "device latency or topology changed during freeze; retry after it settles");
            for (const auto &sink : sinks)
                if (graph->outputLatencyOf(sink.endpoint.node) != sink.endpoint.latency)
                    throw std::runtime_error("freeze source latency changed during capture");
        };
        for (std::int64_t at = 0; at < lastFrame;) {
            service();
            if (!session.clips()->prime())
                throw std::runtime_error("freeze clip read timed out");
            const auto frames =
                static_cast<std::int32_t>((std::min)(static_cast<std::int64_t>(block), lastFrame - at));
            engine::AudioIo io;
            io.frames = frames;
            session.process(io);
            if (session.clips()->readErrors() || session.clips()->underrunSamples())
                throw std::runtime_error("freeze clip read failed or underrun");
            for (auto &sink : sinks) {
                engine::AudioIo copy;
                copy.frames = frames;
                copy.numOut = spec.session.channels;
                copy.out = dp.empty() ? fp.data() : nullptr;
                copy.out64 = dp.empty() ? nullptr : dp.data();
                if (!graph->copyNodeOutput(sink.endpoint.node, copy))
                    throw std::runtime_error("freeze source output block is incomplete");
                for (std::int32_t f = 0; f < frames; ++f)
                    for (std::size_t c = 0; c < channels; ++c) {
                        const auto index = static_cast<std::size_t>(f);
                        const double sample = dp.empty() ? static_cast<double>(fp[c][index]) : dp[c][index];
                        if (!std::isfinite(sample) ||
                            (spec.format == audio::WavFormat::Float32 &&
                             std::abs(sample) > static_cast<double>(std::numeric_limits<float>::max())))
                            throw std::runtime_error("nonfinite or out-of-range freeze source");
                    }
                const auto begin = (std::max)(at, sink.endpoint.latency),
                           end = (std::min)(at + frames, sink.rawEnd);
                if (begin >= end)
                    continue;
                for (auto f = begin; f < end; ++f)
                    for (std::size_t c = 0; c < channels; ++c) {
                        const auto index = static_cast<std::size_t>(f - at);
                        interleaved[static_cast<std::size_t>(f - begin) * channels + c] =
                            dp.empty() ? static_cast<double>(fp[c][index]) : dp[c][index];
                    }
                const auto count = static_cast<std::uint32_t>(end - begin);
                sink.writer->write(interleaved.data(), count);
                sink.written += count;
            }
            at += frames;
            if (control.progress)
                control.progress(0, static_cast<std::uint64_t>(at), static_cast<std::uint64_t>(lastFrame));
            cancellation(control);
        }
        service();
        for (auto &sink : sinks) {
            if (sink.written != static_cast<std::uint64_t>(spec.endSample))
                throw std::runtime_error("freeze source produced an incomplete range");
            sink.writer->close();
        }
        cancellation(control);
        result.data_ = std::move(data);
    } catch (const Cancelled &) {
        result.cancelled_ = true;
        result.error_ = "freeze capture cancelled";
    } catch (const std::exception &error) {
        result.cancelled_ = control.stop.stop_requested();
        result.error_ = error.what();
    }
    return result;
}
} // namespace adi::render
