// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/stem_manifest.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/stem_import.hpp"
#include "adi/render/queue.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <stop_token>
namespace {
using namespace adi;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}
void refuses(const std::function<void()> &call, const char *label) {
    bool threw = false;
    try {
        call();
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, label);
}
std::int64_t count(Store &store, const std::string &table) {
    return store.db().execAndGet("SELECT COUNT(*) FROM " + table).getInt64();
}
void metadata() {
    auto text = audio::makeStemManifest(
        {{"Drums", "#fd4a4a"}, {"Bass", ""}, {"Synths", "#00e8e8"}, {"Vox", "#ad65ff"}});
    text.append(7, char{0});
    const auto manifest = audio::parseStemManifest(text);
    check(manifest.stems.size() == 4 && manifest.stems[0].name == "Drums" && !manifest.masteringEnabled,
          "NI labels and disabled mastering survive padded JSON");
    refuses([] { (void)audio::parseStemManifest("{\"version\":2,\"stems\":[]}"); },
            "future manifest versions explicitly refused");
    refuses([] { (void)audio::makeStemManifest({{"Drums", "#12345x"}}); },
            "invalid RGB refused without using palette indices");
    std::string nested = "{\"version\":1,\"stems\":[{\"name\":\"Drums\"}],\"future\":";
    nested.append(40, '[');
    nested += '0';
    nested.append(40, ']');
    nested += '}';
    refuses([&] { (void)audio::parseStemManifest(nested); }, "manifest depth budget enforced");
    check(audio::parseStemManifest(
              "{\"version\":1,\"stems\":[{}],\"mastering_dsp\":{\"compressor\":{\"enabled\":true}}}")
              .masteringEnabled,
          "enabled mastering is exposed for explicit raw-import policy");
}
std::unique_ptr<Store> target(const fs::path &path) {
    StoreError error{};
    auto store = Store::create(path, error);
    if (!store)
        throw std::runtime_error("target fixture");
    store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(9,'master','Main',0);INSERT "
                     "INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);");
    return store;
}
media::StemImportOptions allocation() {
    media::StemImportOptions o;
    o.group = 100;
    o.containerMedia = 1000;
    o.documentGeneration = 42;
    o.groupName = "Imported";
    for (std::int64_t i = 0; i < 4; ++i)
        o.stems.push_back({101 + i, 201 + i, 1001 + i, i == 0 ? std::optional<int>(3) : std::nullopt});
    return o;
}
bool clean(const fs::path &parent) {
    for (const auto &item : fs::directory_iterator(parent))
        if (item.path().filename().string().starts_with(".adi-ni-"))
            return false;
    return true;
}
void normalizationRefusal() {
    test::TempDirectory dir{"ni_stems", "normalization-refusal"};
    const auto namedRefusal = [](const std::function<void()> &call) {
        try {
            call();
        } catch (const std::invalid_argument &error) {
            return std::string(error.what()).find("NI Stems normalization") != std::string::npos;
        }
        return false;
    };
    for (const auto mode : {render::NormalizeMode::Peak, render::NormalizeMode::Loudness}) {
        for (const double rate : {44100.0, 48000.0}) {
            for (const auto precision : {engine::Precision::Float32, engine::Precision::Float64}) {
                render::Job job;
                job.project = dir.path() / "source-not-opened.adi";
                job.directory = dir.path();
                job.nameTemplate = "Normalized NI";
                job.endSample = 128;
                job.session.sampleRate = rate;
                job.session.precision = precision;
                job.niStems = render::NiStems{
                    {{{1, engine::OfflineTap::PostFader}, "Drums", "#fd4a4a"}}, 256000};
                job.normalization.mode = mode;
                check(namedRefusal([&] { render::validateNiStems(job); }),
                      "NI normalization has a named refusal independent of codec availability");
                render::Queue queue;
                check(namedRefusal([&] { (void)queue.add(job); }) && queue.snapshot().empty(),
                      "unsupported NI normalization cannot enter the queue");
                auto off = job;
                off.normalization.mode = render::NormalizeMode::Off;
                off.nameTemplate = "Original NI";
                const auto id = queue.add(off);
                check(id == 1 && queue.snapshot().size() == 1,
                      "refused add consumes no identity and NI normalization Off remains valid");
                const bool refusedUpdate = namedRefusal([&] { (void)queue.update(id, job); });
                const auto pending = queue.snapshot();
                const auto original = std::find_if(pending.begin(), pending.end(),
                    [&](const auto &entry) { return entry.id == id; });
                check(refusedUpdate && original != pending.end() &&
                          original->result.state == render::JobState::Pending &&
                          original->job.nameTemplate == off.nameTemplate &&
                          original->job.normalization.mode == render::NormalizeMode::Off,
                      "refused queue update preserves the entire pending NI choice");
                unsigned services = 0, loads = 0, progress = 0;
                render::Control control;
                control.serviceHost = [&] { ++services; };
                control.progress = [&](render::JobId, std::uint64_t, std::uint64_t) { ++progress; };
                const auto result = render::renderNiStems(job,
                    [&](const engine::DeviceRequest &, std::string &) -> std::unique_ptr<device::DeviceInstance> {
                        ++loads;
                        return {};
                    }, control);
                check(result.state == render::JobState::Failed && result.output.empty() &&
                          result.framesWritten == 0 &&
                          result.error.find("NI Stems normalization") != std::string::npos,
                      "direct NI render returns the normalization refusal before opening its source");
                check(services == 0 && loads == 0 && progress == 0 && fs::is_empty(dir.path()),
                      "refused NI normalization creates no files and invokes no host or progress callback");
                job.niStems.reset();
                render::Queue wav;
                const auto wavId = wav.add(job);
                check(wavId == 1 && wav.snapshot().size() == 1 &&
                          wav.snapshot().front().job.normalization.mode == mode,
                      "ordinary WAV peak and loudness normalization remain accepted");
            }
        }
    }
}
void integration() {
    if (!audio::aacCodecAvailable()) {
        std::puts("NI Stems native codec unavailable; metadata tests still run");
        return;
    }
    test::TempDirectory dir{"ni_stems", "integration"};
    const auto source = dir.path() / "source.adi", wave = dir.path() / "source.wav";
    StoreError error{};
    auto store = Store::createEmpty(source, error);
    std::vector<float> samples(24000);
    for (unsigned i = 0; i < 12000; ++i) {
        const double t = static_cast<double>(i) / 48000;
        const double envelope = i < 1700 ? 0 : 0.7;
        samples[i * 2] = static_cast<float>(envelope * 0.2 * std::sin(t * 317 * 6.28318530718));
        samples[i * 2 + 1] = static_cast<float>(envelope * 0.2 * std::sin(t * 823 * 6.28318530718));
    }
    {
        audio::WavWriter writer(wave, 48000, 2, audio::WavFormat::Float32);
        writer.write(samples.data(), 12000);
        writer.close();
    }
    store->db().exec(
        "INSERT INTO project(id,name,sample_rate) VALUES(1,'NI Project',48000);INSERT INTO "
        "tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);INSERT INTO tracks(id,kind,name,index_in_parent) "
        "VALUES(1,'audio','Drums',0),(9,'master','Main',1);");
    OpRequest mediaOp;
    auto imported = media::prepareImport(*store, wave, 1, 0, mediaOp);
    check(imported.ok && OpJournal(*store).commit(mediaOp).ok,
          "source media captured through native journal");
    store->db().exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) "
                     "VALUES(1,1,'audio',1,0,250000000);INSERT INTO "
                     "audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,12000);");
    render::Job job;
    job.project = source;
    job.directory = dir.path();
    job.nameTemplate = "NI Export";
    job.beginSample = 127;
    job.endSample = 11003;
    job.session = {2, 48000, 64};
    job.niStems = render::NiStems{{{{1, engine::OfflineTap::PostFader}, "Drums", "#fd4a4a"}}, 256000};
    render::Queue queue;
    const auto id = queue.add(job);
    unsigned services = 0;
    render::Control control;
    control.serviceHost = [&] {
        if (++services == 2) {
            store->db().exec(
                "UPDATE tracks SET name='Edited After Snapshot' WHERE id=1; DELETE FROM audio_clips;");
        }
    };
    queue.run({}, control);
    auto result = queue.snapshot().front().result;
    check(result.state == render::JobState::Completed,
          result.error.empty() ? "NI queue job completes" : result.error.c_str());
    if (result.state != render::JobState::Completed)
        return;
    check(result.framesWritten == 10876 && result.output.filename() == "NI Export.stem.mp4" &&
              clean(dir.path()),
          "queue publishes exact cropped range and removes only its stage");
    const auto movie = audio::readAacMovie(result.output);
    check(movie.tracks.size() == 5 && movie.tracks[0].frames == 10876,
          "canonical master plus four stereo AAC tracks");
    const auto manifest = audio::parseStemManifest(movie.stemManifest);
    check(manifest.stems[0].name == "Drums" && manifest.stems[1].name == "Unused 2" &&
              !manifest.masteringEnabled,
          "requested label, silent slot labels and disabled mastering metadata");
    const auto masterWave = dir.path() / "master.wav", stemWave = dir.path() / "stem.wav";
    audio::decodeAac(movie.tracks[0], masterWave);
    audio::decodeAac(movie.tracks[1], stemWave);
    audio::WavReader master(masterWave), stem(stemWave);
    std::vector<float> a(21752), b(21752);
    master.read(a.data(), 10876);
    stem.read(b.data(), 10876);
    double energy = 0, difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        energy += a[i] * a[i];
        difference += (a[i] - b[i]) * (a[i] - b[i]);
    }
    check(energy > 1 && difference / energy < 0.001,
          "master and direct stem contain aligned actual rendered audio");
    for (std::size_t i = 2; i < 5; ++i) {
        const auto path = dir.path() / ("silent-" + std::to_string(i) + ".wav");
        audio::decodeAac(movie.tracks[i], path);
        audio::WavReader reader(path);
        std::vector<float> silence(21752);
        reader.read(silence.data(), 10876);
        check(std::all_of(silence.begin(), silence.end(), [](float v) { return std::abs(v) < 1e-7f; }),
              "unused NI slot actually decodes to silence");
    }
    const auto masteringFile = dir.path() / "mastering.stem.mp4";
    auto enabled = nlohmann::json::parse(movie.stemManifest);
    enabled["mastering_dsp"]["compressor"]["enabled"] = true;
    audio::writeAacMovie(masteringFile, movie.tracks, enabled.dump());
    auto rawTarget = target(dir.path() / "raw.adi");
    auto rawOptions = allocation();
    auto refusedMastering = media::prepareStemImport(*rawTarget, masteringFile, rawOptions);
    check(!refusedMastering.ready() && refusedMastering.error().find("enabled") != std::string::npos,
          "enabled NI mastering requires an explicit raw-stem choice");
    rawOptions.allowRawMastering = true;
    auto allowedMastering = media::prepareStemImport(*rawTarget, masteringFile, rawOptions);
    check(allowedMastering.ready() && allowedMastering.manifest().masteringEnabled,
          "explicit raw import exposes unapplied mastering while retaining original metadata");
    auto dest = target(dir.path() / "target.adi");
    const auto options = allocation();
    auto prepared = media::prepareStemImport(*dest, result.output, options);
    check(prepared.ready(),
          prepared.error().empty() ? "import captures aligned decoded assets" : prepared.error().c_str());
    if (!prepared.ready())
        return;
    check(count(*dest, "tracks") == 1 && count(*dest, "media_files") == 0,
          "preparation changes no project rows");
    check(!media::commitStemImport(*dest, prepared, 43).ok, "stale document generation refuses import");
    CommitOptions fail;
    fail.beforeCommit = [](SQLite::Database &, std::int64_t, std::string &message) {
        message = "injected late SQL failure";
        return false;
    };
    const auto failed = media::commitStemImport(*dest, prepared, 42, Actor::Import, fail);
    check(!failed.ok && count(*dest, "tracks") == 1 && count(*dest, "media_files") == 0,
          "late SQL failure rolls back whole import");
    check(fs::is_empty(dir.path() / "audio"), "late SQL failure removes newly published assets");
    const auto committed = media::commitStemImport(*dest, prepared, 42);
    check(committed.ok, committed.error.empty()
                            ? "same prepared sources survive late publication failure for retry"
                            : committed.error.c_str());
    if (!committed.ok)
        return;
    check(count(*dest, "tracks") == 6 && count(*dest, "clips") == 4 && count(*dest, "audio_clips") == 4,
          "import creates group and four stems without audible premixed master");
    check(count(*dest, "media_files") == 3,
          "identical silent slots deduplicate within captured native batch");
    check(dest->db().execAndGet("SELECT color FROM tracks WHERE id=101").getInt() == 3 &&
              dest->db().execAndGet("SELECT COUNT(*) FROM tracks WHERE id>101 AND color IS NULL").getInt() ==
                  3,
          "explicit palette mapping is separate from retained NI RGB");
    check(dest->db().execAndGet("SELECT COUNT(*) FROM audio_clips WHERE warp_enabled=0").getInt() == 4,
          "all imported stems play Warp off");
    const auto original = media::mediaRow(dest->db(), prepared.containerMedia());
    std::string why;
    const auto retained = media::resolveMedia(media::projectFolder(*dest), original, why);
    check(!retained.empty() && media::blake3File(retained).hex == media::blake3File(result.output).hex,
          "original container and opaque manifest retained byte for byte");
    for (const auto precision :
         std::array<engine::Precision, 2>{engine::Precision::Float32, engine::Precision::Float64}) {
        render::Job heard;
        heard.project = dest->path();
        heard.directory = dir.path();
        heard.nameTemplate = precision == engine::Precision::Float64 ? "heard64" : "heard32";
        heard.beginSample = 0;
        heard.endSample = 10876;
        heard.session = {2, 48000, 64};
        heard.session.precision = precision;
        heard.encoding.format = audio::WavFormat::Float32;
        const auto played = render::renderWav(heard, {});
        check(played.state == render::JobState::Completed,
              played.error.empty() ? "imported stems render through native group graph"
                                   : played.error.c_str());
        if (played.state == render::JobState::Completed) {
            audio::WavReader reader(played.output);
            std::vector<float> actual(21752);
            reader.read(actual.data(), 10876);
            double dot = 0, referenceEnergy = 0, errorEnergy = 0;
            for (std::size_t i = 0; i < actual.size(); ++i) {
                dot += actual[i] * b[i];
                referenceEnergy += b[i] * b[i];
            }
            const auto gain = dot / referenceEnergy;
            for (std::size_t i = 0; i < actual.size(); ++i) {
                const double delta = actual[i] - gain * b[i];
                errorEnergy += delta * delta;
            }
            check(gain > 0.1 && gain < 1.1 && errorEnergy / referenceEnergy < 1e-8,
                  "actual imported group plays aligned original stem in both widths");
        }
    }
    History history(*dest);
    check(history.undo().ok && count(*dest, "tracks") == 1 && count(*dest, "media_files") == 0,
          "one undo removes every imported row");
    check(history.redo().ok && count(*dest, "tracks") == 6 && count(*dest, "clips") == 4,
          "one redo restores same tracks/clips/media identities");
    const auto destPath = dest->path();
    dest.reset();
    dest = Store::open(destPath, error);
    check(dest && count(*dest, "clips") == 4 && History(*dest).undo().ok,
          "undo group survives closing and reopening project");
    auto stale = media::prepareStemImport(*dest, result.output, options);
    check(stale.ready(), "prepare after undo reuses retained owned content safely");
    OpRequest rename;
    rename.opType = "project.setName";
    rename.payload = {{"name", "Changed after prepare"}};
    check(OpJournal(*dest).commit(rename).ok, "move target journal head after NI prepare");
    const auto rejected = media::commitStemImport(*dest, stale, 42);
    check(!rejected.ok && rejected.stale && count(*dest, "tracks") == 1,
          "stale journal head refuses every native import request");
    const auto blockedFolder = dir.path() / "blocked";
    fs::create_directory(blockedFolder);
    auto blocked = target(blockedFolder / "target.adi");
    auto blockedImport = media::prepareStemImport(*blocked, result.output, options);
    check(blockedImport.ready(), "prepare independent import for collision recovery");
    const auto blocker = blockedFolder / "audio" / ("stems-" + media::blake3File(stemWave).hex + ".wav");
    {
        std::ofstream file(blocker);
        file << "external content";
    }
    const auto refusedCollision = media::commitStemImport(*blocked, blockedImport, 42);
    check(!refusedCollision.ok && count(*blocked, "tracks") == 1 && fs::exists(blocker) &&
              fs::file_size(blocker) == 16,
          "late asset collision preserves external file and rolls back all SQL");
    fs::remove(blocker);
    check(media::commitStemImport(*blocked, blockedImport, 42).ok,
          "same captured import can retry after external collision is resolved");
    store->db().exec(
        "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,12000);");
    render::Queue collision;
    collision.add(job);
    collision.run();
    const auto collided = collision.snapshot().front().result;
    check(collided.state == render::JobState::Failed && fs::exists(result.output) && clean(dir.path()),
          "collision never replaces existing NI export");
    job.nameTemplate = "Cancelled";
    std::stop_source stop;
    render::Control cancel;
    cancel.stop = stop.get_token();
    cancel.progress = [&](render::JobId, std::uint64_t, std::uint64_t) { stop.request_stop(); };
    render::Queue cancelled;
    cancelled.add(job);
    cancelled.run({}, cancel);
    check(cancelled.snapshot().front().result.state == render::JobState::Cancelled &&
              !fs::exists(dir.path() / "Cancelled.stem.mp4") && clean(dir.path()),
          "cancellation during AAC pipeline publishes nothing");
    check(id > 0, "NI uses ordinary queue identities");
}
} // namespace
int main() {
    try {
        metadata();
        normalizationRefusal();
        integration();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("NI stems: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
