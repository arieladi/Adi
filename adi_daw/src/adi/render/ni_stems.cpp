// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/stem_manifest.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/render/queue.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/Backup.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <atomic>
#include <chrono>
#include <sqlite3.h>
#include <stdexcept>
namespace adi::render {
namespace fs = std::filesystem;
namespace {
struct Cancelled {};
void tick(const Control &control) {
    if (control.stop.stop_requested())
        throw Cancelled{};
    if (control.serviceHost)
        control.serviceHost();
    if (control.stop.stop_requested())
        throw Cancelled{};
}
struct Stage {
    fs::path path;
    explicit Stage(const fs::path &parent) {
        static std::atomic<std::uint64_t> serial{0};
        for (int i = 0; i < 128; ++i) {
            auto candidate =
                parent /
                (".adi-ni-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(serial.fetch_add(1)));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                path = std::move(candidate);
                return;
            }
            if (error && error != std::errc::file_exists)
                throw fs::filesystem_error("NI staging", candidate, error);
        }
        throw std::runtime_error("cannot reserve NI Stems stage");
    }
    ~Stage() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};
fs::path captureProject(const Job &job, const Stage &stage) {
    StoreError error{};
    auto source = Store::open(job.project, error, true);
    if (!source || source->readOnly() == false)
        throw std::runtime_error("cannot read NI Stems project");
    SQLite::Transaction read(source->db(), SQLite::TransactionBehavior::DEFERRED);
    // Force a read snapshot before Backup; WAL edits after this revision cannot
    // change the database state between separately rendered stem sources.
    (void)source->db().execAndGet("SELECT COUNT(*) FROM project").getInt64();
    const auto path = stage.path / "snapshot.adi";
    {
        SQLite::Database copy(media::pathUtf8(path), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        SQLite::Backup backup(copy, source->db());
        if (backup.executeStep() != SQLITE_DONE)
            throw std::runtime_error("NI project snapshot is busy or incomplete");
    }
    auto copy = Store::open(path, error);
    if (!copy)
        throw std::runtime_error("cannot open NI project snapshot");
    SQLite::Transaction rewrite(copy->db());
    SQLite::Statement rows(copy->db(), "SELECT id,embedded FROM media_files ORDER BY id");
    while (rows.executeStep()) {
        if (rows.getColumn(1).getInt() != 0)
            continue; // embedded bytes live in the same backup
        const auto id = rows.getColumn(0).getInt64();
        std::string why;
        const auto resolved =
            media::resolveMedia(media::projectFolder(*source), media::mediaRow(source->db(), id), why);
        if (resolved.empty())
            throw std::runtime_error("NI snapshot media: " + why);
        SQLite::Statement set(copy->db(), "UPDATE media_files SET rel_path=NULL,abs_path_hint=? WHERE id=?");
        set.bind(1, media::pathUtf8(fs::absolute(resolved)));
        set.bind(2, id);
        set.exec();
    }
    rewrite.commit();
    read.commit();
    return path;
}
} // namespace
void validateNiStems(const Job &job) {
    if (!job.niStems)
        return;
    const auto &ni = *job.niStems;
    if (ni.stems.empty() || ni.stems.size() > 4 || job.session.channels != 2 ||
        (job.session.sampleRate != 44100 && job.session.sampleRate != 48000) || ni.bitRate < 64000 ||
        ni.bitRate > 320000)
        throw std::invalid_argument(
            "NI Stems requires one to four stereo sources, 44.1/48 kHz and 64000..320000 bit/s per track");
    if (job.source.trackId != 0 || job.source.tap != engine::OfflineTap::PostFader)
        throw std::invalid_argument("NI Stems master must select the Main post-fader output");
    for (const auto &stem : ni.stems)
        if (stem.source.trackId <= 0 ||
            (stem.source.tap != engine::OfflineTap::PreFader &&
             stem.source.tap != engine::OfflineTap::PostFader) ||
            stem.name.size() > 1024 || !audio::stemColorValid(stem.color))
            throw std::invalid_argument("invalid NI Stems source/name/color");
}
Result renderNiStems(Job job, engine::DeviceLoader loader, Control control, JobId id, std::uint64_t counter) {
    Result result;
    try {
        if (!job.niStems)
            throw std::invalid_argument("NI Stems options are required");
        // Apply the ordinary queue's complete job validation as well.
        Queue validation;
        (void)validation.add(job);
        validateNiStems(job);
        tick(control);
        if (!audio::aacCodecAvailable())
            throw std::runtime_error("NI Stems AAC codec is unavailable or cannot verify sample alignment on this system");
        Stage stage(job.directory);
        const auto project = captureProject(job, stage);
        const auto ni = *job.niStems;
        job.project = project;
        job.directory = stage.path;
        job.niStems.reset();
        job.encoding.format = audio::WavFormat::Float32;
        job.encoding.dither = audio::WavDither::None;
        Control renderControl = control;
        renderControl.progress = [&](JobId current, std::uint64_t done, std::uint64_t total) {
            if (total > 999990ULL * 1024)
                throw std::runtime_error("NI export exceeds AAC packet limit");
            if (control.progress)
                control.progress(current, done, total * 5);
        };
        auto rendered = renderWav(job, loader, renderControl, id, counter);
        if (rendered.state == JobState::Cancelled)
            throw Cancelled{};
        if (rendered.state != JobState::Completed)
            throw std::runtime_error(rendered.error);
        result.framesWritten = rendered.framesWritten;
        result.output = stage.path.parent_path() / rendered.output.filename();
        result.output.replace_extension(".stem.mp4");
        if (fs::exists(result.output))
            throw std::runtime_error("NI Stems destination already exists");
        std::vector<audio::AacTrack> tracks;
        std::vector<audio::StemLabel> labels;
        auto encode = [&](const fs::path &wave, unsigned ordinal) {
            tick(control);
            tracks.push_back(audio::encodeAac(wave, stage.path / ("aac-" + std::to_string(ordinal)),
                                              ni.bitRate, [&] { tick(control); }));
            if (control.progress)
                control.progress(id, (ordinal + 1) * result.framesWritten, 5 * result.framesWritten);
        };
        encode(rendered.output, 0);
        for (unsigned index = 0; index < 4; ++index) {
            fs::path wave;
            if (index < ni.stems.size()) {
                job.source = ni.stems[index].source;
                job.nameTemplate = "stem-" + std::to_string(index);
                renderControl.progress = [&, index](JobId current, std::uint64_t done, std::uint64_t total) {
                    if (control.progress)
                        control.progress(current, (index + 1) * result.framesWritten + done, total * 5);
                };
                rendered = renderWav(job, loader, renderControl, id, counter);
                if (rendered.state == JobState::Cancelled)
                    throw Cancelled{};
                if (rendered.state != JobState::Completed)
                    throw std::runtime_error(rendered.error);
                if (rendered.framesWritten != result.framesWritten)
                    throw std::runtime_error("NI Stems render ranges diverged");
                wave = rendered.output;
                labels.push_back({ni.stems[index].name.empty() ? "Stem " + std::to_string(index + 1)
                                                               : ni.stems[index].name,
                                  ni.stems[index].color});
            } else {
                wave = stage.path / ("unused-" + std::to_string(index) + ".wav");
                audio::WavWriter silence(wave, static_cast<std::uint32_t>(job.session.sampleRate), 2,
                                         audio::WavFormat::Float32);
                std::array<float, 8192> zeros{};
                for (std::uint64_t done = 0; done < result.framesWritten;) {
                    tick(control);
                    const auto n = static_cast<std::uint32_t>(
                        (std::min)(result.framesWritten - done, std::uint64_t{4096}));
                    silence.write(zeros.data(), n);
                    done += n;
                }
                silence.close();
                labels.push_back({"Unused " + std::to_string(index + 1), "#808080"});
            }
            encode(wave, index + 1);
        }
        const auto output = stage.path / "final.stem.mp4";
        tick(control);
        audio::writeAacMovie(output, tracks, audio::makeStemManifest(labels), [&] { tick(control); });
        tick(control);
        const auto published = media::publishFile(output, result.output);
        if (published.status != media::PublishStatus::published)
            throw std::runtime_error(published.status == media::PublishStatus::exists
                                         ? "NI Stems destination already exists"
                                         : published.error);
        result.state = JobState::Completed;
    } catch (const Cancelled &) {
        result.state = JobState::Cancelled;
    } catch (const std::exception &error) {
        result.state = JobState::Failed;
        result.error = error.what();
    }
    return result;
}
} // namespace adi::render
