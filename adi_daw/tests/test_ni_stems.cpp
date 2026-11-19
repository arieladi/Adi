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
void collisionPublication() {
    // NI's real shared naming/publication path with opaque staged bytes. This
    // runs without AAC; it is not a claim of codec integration.
    test::TempDirectory dir{"ni_stems","collision-publication"};
    const auto stage=dir.path()/"stage.bin", original=dir.path()/"delivery.stem.mp4";
    const auto write=[](const fs::path& path,const char* text) {
        std::ofstream file(path,std::ios::binary);file<<text;file.close();
        if(!file)throw std::runtime_error("NI collision fixture write");
    };
    write(original,"original delivery");write(stage,"replacement candidate");
    const auto before=media::blake3File(original).hex;
    render::Result fail;fail.output=original;
    refuses([&] {render::detail::prepareOutput(fail,render::Collision::Fail,".stem.mp4","NI destination exists");},
            "NI Fail refuses an existing complete delivery path");
    check(media::blake3File(original).hex==before && fs::exists(stage),
          "NI Fail leaves original and private staged bytes intact");
    render::Result increment;increment.output=original;
    render::detail::prepareOutput(increment,render::Collision::Increment,".stem.mp4","NI destination exists");
    check(increment.output.filename()=="delivery (2).stem.mp4",
          "NI Increment inserts numbering before its entire compound suffix");
    const auto stageHash=media::blake3File(stage).hex;
    render::detail::publishOutput(stage,increment,{},"NI destination exists");
    check(increment.state==render::JobState::Completed && increment.publication &&
          !increment.publication->usedFallback && media::blake3File(increment.output).hex==stageHash &&
          media::blake3File(original).hex==before && !fs::exists(stage),
          "NI shared publisher atomically publishes selected bytes and preserves the original");
    render::Result late;late.output=original;
    render::detail::prepareOutput(late,render::Collision::Increment,".stem.mp4","NI destination exists");
    check(late.output.filename()=="delivery (3).stem.mp4","NI repeated preflight selects the next available delivery name");
    write(stage,"private staged again");write(late.output,"external late arrival");
    const auto lateHash=media::blake3File(late.output).hex;
    refuses([&] {render::detail::publishOutput(stage,late,{},"NI destination exists");},
            "NI publisher refuses a collision introduced after numbered preflight");
    check(!late.publication && media::blake3File(late.output).hex==lateHash && fs::exists(stage) &&
          !fs::exists(dir.path()/"delivery (4).stem.mp4"),
          "NI late-arrival refusal neither replaces external bytes nor renumbers");
    std::string stem;
    for(unsigned i=0;i<93;++i)stem+="\xC3\xA9";
    stem+='x'; // 187+4 increment+9 suffix = 200 bytes
    render::Result boundary;boundary.output=dir.path()/media::pathFromUtf8(stem+".stem.mp4");
    write(boundary.output,"occupied");
    render::detail::prepareOutput(boundary,render::Collision::Increment,".stem.mp4","NI destination exists");
    render::detail::publishOutput(stage,boundary,{},"NI destination exists");
    check(boundary.state==render::JobState::Completed && media::pathUtf8(boundary.output.filename()).size()==200,
          "NI actual publication accepts 200 UTF-8 bytes including increment and compound suffix");
    stem+='y';
    render::Result overflow;overflow.output=dir.path()/media::pathFromUtf8(stem+".stem.mp4");
    const auto validOriginal=overflow.output;write(validOriginal,"retained");
    refuses([&] {render::detail::prepareOutput(overflow,render::Collision::Increment,".stem.mp4","NI destination exists");},
            "NI increment refuses a 201-byte final compound filename");
    check(fs::file_size(validOriginal)==8 && !fs::exists(overflow.output),
          "NI increment overflow leaves the shorter existing delivery untouched");
    const auto foldedOriginal=dir.path()/media::pathFromUtf8("Stra\xC3\x9F" "e.STEM.MP4");
    write(foldedOriginal,"folded original");write(stage,"folded NI candidate");
    render::Result folded;folded.output=dir.path()/"STRASSE.stem.mp4";
    refuses([&] {render::detail::prepareOutput(folded,render::Collision::Fail,".stem.mp4","NI destination exists");},
            "NI Fail compares Unicode-expanded names and the entire compound suffix");
    render::detail::prepareOutput(folded,render::Collision::Increment,".stem.mp4","NI destination exists");
    check(folded.output.filename()=="STRASSE (2).stem.mp4","NI folded Increment preserves selected spelling and compound suffix");
    const auto arrival=dir.path()/media::pathFromUtf8("Stra\xC3\x9F" "e (2).STEM.MP4");write(arrival,"late folded NI");
    const auto retained=media::blake3File(arrival).hex;
    refuses([&] {render::detail::publishOutput(stage,folded,{},"NI destination exists");},
            "NI shared commit rechecks fold-only late arrivals");
    check(!folded.publication && fs::exists(stage) && media::blake3File(arrival).hex==retained &&
          !fs::exists(dir.path()/"STRASSE (3).stem.mp4"),"NI late folded collision leaves private bytes, arrival and numbering unchanged");
    render::Result replacement;replacement.output=original;write(stage,"replacement NI bytes");
    render::detail::prepareOutput(replacement,render::Collision::Replace,".stem.mp4","NI destination exists");
    const auto replacingHash=media::blake3File(stage).hex;
    render::detail::publishOutput(stage,replacement,{},"NI destination exists");
    check(replacement.state==render::JobState::Completed && replacement.replacement && replacement.replacement->existing &&
          replacement.output==original && media::blake3File(original).hex==replacingHash,
          "NI shared publisher uses identity-bound native Replace for the complete compound filename");
    write(stage,"refused NI bytes");
    render::Result changed;changed.output=original;
    render::detail::prepareOutput(changed,render::Collision::Replace,".stem.mp4","NI destination exists");
    write(original,"external NI edit");const auto edited=media::blake3File(original).hex;
    refuses([&]{render::detail::publishOutput(stage,changed,{},"NI destination exists");},
            "NI shared Replace refuses changed bytes after preflight");
    check(!changed.publication && media::blake3File(original).hex==edited && fs::exists(stage),
          "NI refusal preserves another writer and the caller-owned stage");
}
void deliveryMediaVerification() {
    test::TempDirectory dir{"ni_stems", "delivery-media"};
    const auto wave = dir.path() / "source.wav";
    const auto writeWave = [&](float sample) {
        std::array<float, 256> pcm;
        pcm.fill(sample);
        audio::WavWriter writer(wave, 48000, 2, audio::WavFormat::Float32);
        writer.write(pcm.data(), 128);
        writer.close();
    };
    writeWave(0.25f);
    const auto original = media::blake3File(wave).hex;
    StoreError error{};
    auto store = Store::createEmpty(dir.path() / "source.adi", error);
    if (!store) throw std::runtime_error("cannot create NI media fixture");
    store->db().exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'NI Verify',48000);"
        "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Source',0),(9,'master','Main',1);"
        "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,2666667);");
    SQLite::Statement mediaRow(store->db(),
        "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,?,'source.wav','wav')");
    mediaRow.bind(1, original);
    mediaRow.exec();
    store->db().exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,128)");

    render::Job job;
    job.project = store->path();
    job.directory = dir.path();
    job.nameTemplate = "private-main";
    job.beginSample = 0;
    job.endSample = 64;
    job.session = {2, 48000, 32};
    auto main = render::renderWav(job, {});
    job.nameTemplate = "private-stem";
    job.source.trackId = 1;
    auto stem = render::renderWav(job, {});
    check(main.state == render::JobState::Completed && stem.state == render::JobState::Completed,
          "Main and stem capture actual offline WAV input identity");
    render::Result delivery;
    delivery.output = dir.path() / "delivery.stem.mp4";
    render::detail::retainIntermediateMedia(delivery, main);
    render::detail::retainIntermediateMedia(delivery, stem);
    const auto& captured = delivery.mediaVerification.value();
    check(captured.identities.size() == 1 && captured.startComplete && !captured.publicationVerified &&
              !captured.identities.front().blake3BeforePublication,
          "the private WAV's check does not mark the final delivery verified");

    // The shared final commit gate is independent of platform AAC support.
    const auto stage = dir.path() / "encoded.bin";
    fs::copy_file(stem.output, stage);
    const auto outputHash = media::blake3File(stage).hex;
    writeWave(0.125f);
    const auto altered = media::blake3File(wave).hex;
    refuses([&] {render::detail::publishVerifiedOutput(stage, delivery, {}, "already exists");},
            "a same-size input rewrite after all WAV renders refuses the final delivery");
    check(!fs::exists(delivery.output) && fs::exists(stage) && !delivery.publication &&
              !captured.publicationVerified && captured.identities.front().blake3BeforePublication == altered,
          "failed final verification leaves staged and source bytes intact and reports both digests");
    SQLite::Statement replacement(store->db(), "UPDATE media_files SET hash_blake3=? WHERE id=1");
    replacement.bind(1, altered);
    replacement.exec();
    job.nameTemplate = "changed-stem";
    auto changed = render::renderWav(job, {});
    check(changed.state == render::JobState::Completed, "independent WAV can validly read the changed source");
    refuses([&] {render::detail::retainIntermediateMedia(delivery, changed);},
            "individually verified stems cannot mix different bytes in one delivery");
    writeWave(0.25f);
    render::detail::publishVerifiedOutput(stage, delivery, {}, "already exists");
    check(delivery.state == render::JobState::Completed && delivery.publication && captured.publicationVerified &&
              media::blake3File(delivery.output).hex == outputHash && !fs::exists(stage),
          "unchanged inputs complete the same checked exclusive delivery path");
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
    auto boundary=job;
    boundary.nameTemplate.clear();
    for (unsigned i=0;i<95;++i) boundary.nameTemplate+="\xC3\xA9";
    boundary.nameTemplate+='x'; // 191-byte stem plus the nine-byte .stem.mp4 suffix.
    const auto longest=render::renderNiStems(boundary,{});
    check(longest.state==render::JobState::Completed && media::pathUtf8(longest.output.filename()).size()==200 &&
              fs::exists(longest.output) && clean(dir.path()),"NI Stems publishes a 200-byte final UTF-8 filename");
    boundary.nameTemplate+='y';
    const auto tooLong=render::renderNiStems(boundary,{});
    check(tooLong.state==render::JobState::Failed && media::pathUtf8(tooLong.output.filename()).size()==201 &&
              tooLong.error=="final export name exceeds 200 UTF-8 bytes" && !fs::exists(tooLong.output) && clean(dir.path()),
          "NI Stems rejects its final suffix overflow although the intermediate WAV name fits");
    render::Queue queue;
    const auto id = queue.add(job);
    const auto capturedHead = OpJournal(*store).headSeq();
    check(capturedHead.has_value(), "NI source has a real journalled media-import head");
    std::int64_t laterHead = 0;
    unsigned services = 0;
    static unsigned publicationSyncs = 0;
    publicationSyncs = 0;
    render::Control control;
    control.publicationHooks.syncDirectory = [](const fs::path&) {
        ++publicationSyncs;
        return std::make_error_code(std::errc::io_error);
    };
    control.serviceHost = [&] {
        if (++services == 2) {
            OpRequest rename;
            rename.opType = "track.rename";
            rename.payload = {{"id", 1}, {"name", "Edited After Snapshot"}};
            const auto committed = OpJournal(*store).commit(rename);
            check(committed.ok && !committed.seqs.empty(), "NI original project changes after its private capture");
            if (!committed.seqs.empty()) laterHead = committed.seqs.back();
            store->db().exec("DELETE FROM audio_clips;");
        }
    };
    queue.run({}, control);
    auto result = queue.snapshot().front().result;
    check(result.state == render::JobState::Completed,
          result.error.empty() ? "NI queue job completes" : result.error.c_str());
    if (result.state != render::JobState::Completed)
        return;
    check(publicationSyncs == 1 && result.publication && !result.publication->durabilityConfirmed &&
              !result.publication->warning.empty(),
          "NI hook runs only for final publication and keeps a sync warning without failing the job");
    const auto runManifest = nlohmann::json::parse(render::serializeRunManifest(queue.lastRun()));
    check(runManifest["members"][0]["state"] == "completed" &&
              runManifest["members"][0]["result"]["publication"]["durability_confirmed"] == false &&
              runManifest["members"][0]["job"]["requested_profile"]["container"] == "ni_stems_mp4",
          "NI publication warning and format are retained in the run manifest");
    check(result.snapshot && capturedHead && result.snapshot->journalHead == *capturedHead &&
              laterHead > *capturedHead && result.snapshot->source.trackId == 9 &&
              result.snapshot->beginSample == 127 && result.snapshot->endSample == 11003 &&
              result.stemSnapshots.size() == 1 && result.stemSnapshots[0].journalHead == *capturedHead &&
              result.stemSnapshots[0].source.trackId == 1 && result.stemSnapshots[0].names.track == "Drums",
          "NI Main and actual stem preserve one original revision, range and names after source edits");
    check(runManifest["members"][0]["journal_head"] == capturedHead.value_or(-1) &&
              runManifest["members"][0]["result"]["ni_stem_snapshots"].size() == 1 &&
              runManifest["members"][0]["result"]["ni_stem_snapshots"][0]["source"]["track_id"] == 1 &&
              runManifest["members"][0]["result"]["ni_stem_snapshots"][0]["end_sample"] == 11003,
          "NI run JSON records actual supplied streams without inventing sources for silent slots");
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
    const auto retainedContainerHash=media::blake3File(result.output).hex;
    auto numbered=job;numbered.collision=render::Collision::Increment;
    const auto incremented=render::renderNiStems(numbered,{});
    check(incremented.state==render::JobState::Completed && incremented.output.filename()=="NI Export (2).stem.mp4" &&
          media::blake3File(result.output).hex==retainedContainerHash && clean(dir.path()),
          "explicit NI Increment publishes a separate real encoded delivery with private intermediates unchanged");
    if(incremented.state==render::JobState::Completed)
        check(audio::readAacMovie(incremented.output).tracks.at(0).frames==10876,
              "numbered NI delivery contains the requested encoded range");
    auto replacedJob=job;replacedJob.nameTemplate="NI Replaced";replacedJob.collision=render::Collision::Replace;
    const auto replacedPath=dir.path()/"NI Replaced.stem.mp4";
    {std::ofstream existing(replacedPath,std::ios::binary);existing<<"old NI delivery";}
    const auto replaced=render::renderNiStems(replacedJob,{});
    check(replaced.state==render::JobState::Completed && replaced.replacement && replaced.replacement->existing &&
          replaced.output==replacedPath && audio::readAacMovie(replaced.output).tracks.at(0).frames==10876 && clean(dir.path()),
          "actual NI encoder replaces the bound old delivery with the requested five-stream container");
    bool changedDuringMain=false;render::Control duringMain;
    duringMain.progress=[&](render::JobId,std::uint64_t done,std::uint64_t total) {
        if(done && done<total/5 && !changedDuringMain) {
            changedDuringMain=true;std::ofstream file(replacedPath,std::ios::binary);file<<"changed during Main";
        }
    };
    const auto rejectedReplacement=render::renderNiStems(replacedJob,{},duringMain);
    {std::ifstream file(replacedPath,std::ios::binary);const std::string bytes{std::istreambuf_iterator<char>(file),{}};
        check(changedDuringMain && rejectedReplacement.state==render::JobState::Failed && !rejectedReplacement.publication &&
              bytes=="changed during Main" && clean(dir.path()),
              "NI binds before its first Main audio block, refusing an edit during that first render");}
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
        collisionPublication();
        deliveryMediaVerification();
        integration();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("NI stems: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
