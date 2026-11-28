// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/render/queue.hpp"
#include "adi/render/host_dispatch.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include "adi/history.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string_view>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace adi;
using namespace adi::engine;
using namespace adi::render;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) {++failures; std::printf("FAIL: %s\n", name);}
}
OpRequest request(const char* type, Payload payload) {
    OpRequest result;
    result.opType = type; result.payload = std::move(payload);
    return result;
}
std::int64_t commit(Store& store, std::span<const OpRequest> requests) {
    const auto result = OpJournal(store).commit(requests);
    if (!result.ok || result.seqs.empty()) throw std::runtime_error("fixture journal commit: " + result.error);
    return result.seqs.back();
}
float source(std::int64_t frame, int channel) {
    return static_cast<float>((frame % 191) - 95) / 128.0f + static_cast<float>(channel) / 16.0f;
}
struct Fixture {
    test::TempDirectory dir{"render_queue","project"};
    std::unique_ptr<Store> store;
    Fixture() {
        StoreError error{};
        store = Store::createEmpty(dir.path()/"project.adi", error);
        if (!store) throw std::runtime_error("fixture store");
        store->db().exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Before',48000);"
            "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Master',1);"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','audio.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,1000000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,4096);");
        std::vector<float> samples(8192);
        for (int i = 0; i < 4096; ++i) for (int c = 0; c < 2; ++c)
            samples[static_cast<std::size_t>(2*i+c)] = source(i,c);
        audio::WavWriter wave(dir.path()/"audio.wav",48000,2,audio::WavFormat::Float32);
        wave.write(samples.data(),4096); wave.close();
        fs::create_directory(dir.path()/"out");
    }
    Job job(const std::string& pattern = "{project}-{track}-{counter}") const {
        Job j;
        j.project = store->path(); j.directory = dir.path()/"out"; j.nameTemplate = pattern;
        j.beginSample = 137; j.endSample = 1003; j.session = {2,48000,64};
        j.encoding.format = audio::WavFormat::Float32;
        return j;
    }
    void effect() {
        store->db().exec("INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.delay','Delay','effect');"
            "INSERT INTO device_chains(id,track_id) VALUES(1,1);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Delay');");
    }
    void parallel() {
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(2,'audio','Parallel',2);"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(2,2,'audio',1,0,1000000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(2,1,0,4096);");
    }
    bool noStage() const {
        for (const auto& p : fs::directory_iterator(dir.path()/"out"))
            if (media::pathUtf8(p.path().filename()).starts_with(".adi-render-")) return false;
        return true;
    }
};
struct Delay : device::DeviceInstance {
    device::DeviceIdentity identity_;
    int delay = 77;
    std::array<std::array<double,256>,2> ring{};
    int at = 0;
    double gain = 1;
    bool distinguishWidth = false;
    int changeAfter = 0, processed = 0;
    std::uint64_t epoch = 0;
    const device::DeviceIdentity& identity() const noexcept override {return identity_;}
    bool loaded() const noexcept override {return true;}
    bool processesDouble() const noexcept override {return true;}
    std::int32_t latencySamples() const noexcept override {return delay;}
    std::uint64_t latencyEpoch() const noexcept override {return epoch;}
    std::int64_t tailSamples() const noexcept override {return delay;}
    void process(const NodeIo& io) noexcept override {
        for (int i = 0; i < io.frames; ++i) {
            for (int c = 0; c < io.channels; ++c) {
                const auto channel = static_cast<std::size_t>(c);
                const auto index = static_cast<std::size_t>(io.blockOffset+i);
                const double x = io.wide() ? io.in64[c][index] : static_cast<double>(io.in[c][index]);
                const double y = ring[channel][static_cast<std::size_t>((at+256-delay)%256)];
                ring[channel][static_cast<std::size_t>(at)] = x;
                const double out = y * gain * (distinguishWidth && io.wide() ? 2.0 : 1.0);
                if (io.wide()) io.out64[c][index] = out;
                else io.out[c][index] = static_cast<float>(out);
            }
            at = (at+1)%256;
        }
        processed += io.frames;
        if (changeAfter > 0 && processed >= changeAfter && epoch == 0) {++delay; ++epoch;}
    }
};
std::vector<float> read(const Result& result, std::uint64_t frames, std::uint32_t rate = 48000,
                        audio::WavFormat format = audio::WavFormat::Float32) {
    check(result.state==JobState::Completed, result.error.empty() ? "job completed" : result.error.c_str());
    if (result.state!=JobState::Completed) return {};
    audio::WavReader wave(result.output);
    check(wave.frames()==frames && wave.channels()==2 && wave.sampleRate()==rate && wave.format()==format,
          "published WAV has exact frames, channels, rate and format");
    std::vector<float> samples(static_cast<std::size_t>(2*frames));
    check(wave.read(samples.data(),static_cast<std::uint32_t>(frames))==frames,"published samples decode");
    return samples;
}
bool matches(const std::vector<float>& samples, std::int64_t begin, double gain = 1, std::int64_t delay = 0) {
    if (samples.empty()) return false;
    for (std::size_t i = 0; i < samples.size()/2; ++i) for (int c = 0; c < 2; ++c) {
        const auto at = begin + static_cast<std::int64_t>(i) - delay;
        const float expected = at < 0 ? 0 : static_cast<float>(gain * source(at,c));
        if (samples[2*i+static_cast<std::size_t>(c)] != expected) return false;
    }
    return true;
}
void ranges() {
    Fixture f; f.effect(); f.parallel();
    OpRequest click;click.opType="transport.setMetronome";click.payload={{"on",true}};
    OpRequest loop;loop.opType="transport.setLoop";loop.payload={{"on",true},{"start",0},{"end",1}};
    check(OpJournal(*f.store).commit(std::vector<OpRequest>{click,loop}).ok,
          "saved monitor loop/click fixture commits before export");
    int loads = 0;
    auto loader = [&](const DeviceRequest&, std::string&) {++loads; return std::make_unique<Delay>();};
    Queue queue;
    const auto first = queue.add(f.job("{project}-{bpm}-{counter}"));
    auto j = f.job("{project}-{bpm}-{counter}"); j.beginSample=513; j.endSample=789; j.session.precision=Precision::Float64;
    const auto second = queue.add(j);
    check(queue.snapshot().size()==2 && fs::is_empty(j.directory),"enqueue writes no files");
    j.endSample=809;
    check(queue.update(second,j),"pending job can be edited");
    f.store->db().exec("UPDATE project SET name='At render'; UPDATE tempo_map SET bpm=90;");
    queue.run(loader);
    auto results=queue.snapshot();
    check(loads==2,"each job creates fresh plugin state");
    check(results[0].id==first && results[1].id==second,"ordered job IDs survive execution");
    check(results[0].result.output.filename()=="At render-90.0-1.wav" && results[1].result.output.filename()=="At render-90.0-2.wav",
          "names and tempo are resolved at render start, in queue order");
    check(matches(read(results[0].result,866),137,2),"latency and parallel compensation preserve the requested heard range");
    check(matches(read(results[1].result,296),513,2),"edited second range and short final block render at double precision");
    queue.run(loader);
    check(loads==2 && !queue.retry(first) && !queue.update(first,j),"completed jobs are retained and never rerendered implicitly");
    check(f.noStage(),"successful jobs leave no private stages");

    Fixture late;
    late.store->db().exec("INSERT INTO mixer_strip(track_id,delay_samples) VALUES(1,100);");
    auto delayed=late.job("delayed"); delayed.beginSample=31; delayed.endSample=301;
    const auto result=renderMasterWav(delayed,{});
    check(matches(read(result,270),31,1,100),"positive track delay retains initial silence and heard placement");
    delayed.nameTemplate="all-silent"; delayed.beginSample=0; delayed.endSample=37;
    check(matches(read(renderMasterWav(delayed,{}),37),0,1,100),"a range entirely before delayed onset is exactly silence");
}
void precisionAndIsolation() {
    Fixture f; f.effect();
    auto loader=[](const DeviceRequest&,std::string&) {auto d=std::make_unique<Delay>(); d->distinguishWidth=true; return d;};
    auto job=f.job("wide"); job.session.precision=Precision::Float64;
    check(matches(read(renderMasterWav(job,loader),866),137,2),"double device processing is used before Float32 encoding");
    job.nameTemplate="narrow"; job.session.precision=Precision::Float32;
    check(matches(read(renderMasterWav(job,loader),866),137),"float mode takes the distinct float device path");
    job.nameTemplate="frozen-job";
    Control control;
    int progressCalls=0;
    control.progress=[&](JobId,std::uint64_t,std::uint64_t) {
        ++progressCalls;
        job.session.channels=64; job.endSample=999999;
        control.progress={};
    };
    const auto frozen=renderMasterWav(job,loader,control);
    check(matches(read(frozen,866),137) && progressCalls>1,"direct render freezes caller-owned job and callbacks");
    check(f.noStage(),"isolated renders clean stages");
}
void sampleRateConversion() {
    Fixture f;
    std::vector<float> dc(8192);
    for (std::size_t i=0;i<4096;++i) {dc[2*i]=0.25f; dc[2*i+1]=-0.5f;}
    audio::WavWriter sourceFile(f.dir.path()/"audio.wav",48000,2,audio::WavFormat::Float32);
    sourceFile.write(dc.data(),4096); sourceFile.close();
    auto job=f.job("converted"); job.session.sampleRate=44100;
    const auto samples=read(renderMasterWav(job,{}),866,44100);
    bool correct=!samples.empty();
    for (std::size_t i=0;i<samples.size()/2;++i)
        correct=correct && std::abs(samples[2*i]-0.25f)<1e-5f && std::abs(samples[2*i+1]+0.5f)<1e-5f;
    check(correct,"48 kHz media renders at 44.1 kHz with ranges measured in output samples");
}
void largeSongExportSpeed() {
    using Clock = std::chrono::steady_clock;
    constexpr std::int64_t rate = 48000, seconds = 60, frames = rate * seconds;
    constexpr std::int64_t trackCount = 8, slices = 50;
    constexpr std::uint32_t chunkFrames = 8192;
    Fixture f;
    const auto sourcePath = f.dir.path() / "audio.wav";
    const auto sample = [](std::int64_t frame, int channel) { return source(frame, channel) / 16.0f; };
    {
        audio::WavWriter wave(sourcePath, rate, 2, audio::WavFormat::Float32);
        std::array<float, chunkFrames * 2> pcm{};
        for (std::int64_t position = 0; position < frames; position += chunkFrames) {
            const auto count = static_cast<std::uint32_t>(std::min<std::int64_t>(chunkFrames, frames - position));
            for (std::uint32_t frame = 0; frame < count; ++frame) {
                pcm[2 * frame] = sample(position + frame, 0);
                pcm[2 * frame + 1] = sample(position + frame, 1);
            }
            wave.write(pcm.data(), count);
        }
        wave.close();
    }
    const auto hashed = media::blake3File(sourcePath);
    if (!hashed) throw std::runtime_error("hash export speed source");
    auto& db = f.store->db();
    {
        SQLite::Statement media(db, "UPDATE media_files SET hash_blake3=? WHERE id=1");
        media.bind(1, hashed.hex);
        media.exec();
        SQLite::Statement track(db, "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(?,'audio','Audio',?)");
        for (std::int64_t id = 2; id <= trackCount; ++id) {
            track.bind(1, id);
            track.bind(2, id);
            track.exec();
            track.reset();
        }
    }
    const auto place = [&](std::int64_t segments) {
        SQLite::Transaction seed(db);
        db.exec("DELETE FROM clips");
        SQLite::Statement clip(db, "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(?,?,'audio',1,?,?)");
        SQLite::Statement audio(db, "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(?,1,?,?)");
        const auto boundary = [segments](std::int64_t step) {
            // Move the interior seams off the render block grid while keeping the same source and timeline.
            return step * frames / segments + (step != 0 && step != segments ? step % 7 + 1 : 0);
        };
        const auto nanos = [](std::int64_t frame) { return frame * 1000000000 / rate; };
        for (std::int64_t track = 1; track <= trackCount; ++track) {
            for (std::int64_t piece = 0; piece < segments; ++piece) {
                const auto start = boundary(piece), end = boundary(piece + 1);
                const auto id = (track - 1) * segments + piece + 1;
                clip.bind(1, id);
                clip.bind(2, track);
                clip.bind(3, nanos(start));
                clip.bind(4, nanos(end) - nanos(start));
                clip.exec();
                clip.reset();
                audio.bind(1, id);
                audio.bind(2, start);
                audio.bind(3, end - start);
                audio.exec();
                audio.reset();
            }
        }
        seed.commit();
    };
    const auto renderSong = [&](std::int64_t segments, const char* tag) {
        place(segments);
        auto job = f.job(tag);
        job.beginSample = 0;
        job.endSample = frames;
        job.session.maxFrames = 512;
        const auto start = Clock::now();
        auto result = renderWav(job, {});
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        check(result.state == JobState::Completed && result.framesWritten == frames,
              result.error.empty() ? "60-second WAV export completes with every frame" : result.error.c_str());
        return std::pair{std::move(result), elapsed};
    };
    const auto [whole, wholeSeconds] = renderSong(1, "speed-whole");
    const auto [split, splitSeconds] = renderSong(slices, "speed-split");
    std::printf("SC-414 60 s stereo export: 8 clips %.3f s (%.2fx real time), 400 clips %.3f s (%.2fx), %.2fx split/whole\n",
                wholeSeconds, static_cast<double>(seconds) / wholeSeconds,
                splitSeconds, static_cast<double>(seconds) / splitSeconds, splitSeconds / wholeSeconds);
    if (whole.state != JobState::Completed || split.state != JobState::Completed) return;
    audio::WavReader reference(whole.output), cut(split.output);
    check(reference.frames() == frames && cut.frames() == frames && reference.sampleRate() == rate && cut.sampleRate() == rate &&
              reference.channels() == 2 && cut.channels() == 2 && reference.format() == audio::WavFormat::Float32 &&
              cut.format() == audio::WavFormat::Float32,
          "both export shapes publish the specified stereo PCM");
    std::array<float, chunkFrames * 2> expected{}, actual{};
    bool exact = true;
    for (std::int64_t position = 0; position < frames && exact; position += chunkFrames) {
        const auto count = static_cast<std::uint32_t>(std::min<std::int64_t>(chunkFrames, frames - position));
        exact = reference.read(expected.data(), count) == count && cut.read(actual.data(), count) == count;
        for (std::uint32_t frame = 0; frame < count && exact; ++frame) {
            exact = expected[2 * frame] == sample(position + frame, 0) * trackCount &&
                    expected[2 * frame + 1] == sample(position + frame, 1) * trackCount &&
                    expected[2 * frame] == actual[2 * frame] && expected[2 * frame + 1] == actual[2 * frame + 1];
        }
    }
    check(exact, "400 contiguous clips export the same complete PCM as 8 whole clips, across off-block seams");
    check(splitSeconds <= wholeSeconds * 3, "400 contiguous clips export within 3x the time of the same 8 whole clips");
}
void cancellationAndFailures() {
    Fixture f;
    for (int when : {0,1,2}) {
        std::stop_source stop;
        auto j=f.job("cancel-"+std::to_string(when));
        Control c; c.stop=stop.get_token();
        c.progress=[&](JobId,std::uint64_t done,std::uint64_t total) {
            if ((when==1 && done>0) || (when==2 && done==total)) stop.request_stop();
        };
        if (when==0) stop.request_stop();
        const auto result=renderMasterWav(j,{},c);
        check(result.state==JobState::Cancelled && !fs::exists(j.directory/ ("cancel-"+std::to_string(when)+".wav")) && f.noStage(),
              "cancellation before, during and before publication leaves no file or stage");
        check(result.snapshot.has_value()==(when!=0) && (!result.snapshot || result.snapshot->journalHead==0),
              "cancellation distinguishes unavailable snapshot from captured revision zero");
        check(result.mediaVerification.has_value()==(when!=0) &&
              (!result.mediaVerification || (result.mediaVerification->startComplete && !result.mediaVerification->publicationVerified)),
              "cancelled WAV keeps completed start hashes without claiming publication verification");
    }
    auto j=f.job("existing");
    {std::ofstream file(j.directory/"existing.wav"); file<<"keep me";}
    auto collision=renderMasterWav(j,{});
    std::ifstream old(j.directory/"existing.wav"); std::string kept; std::getline(old,kept);
    check(collision.state==JobState::Failed && kept=="keep me" && f.noStage(),"existing destination is never replaced");
    // The destination may appear after the early existence check.
    j.nameTemplate="race";
    Control race;
    race.progress=[&](JobId,std::uint64_t done,std::uint64_t total) {
        if(done==total) {std::ofstream file(j.directory/"race.wav"); file<<"won race";}
    };
    collision=renderMasterWav(j,{},race);
    std::ifstream winner(j.directory/"race.wav"); std::getline(winner,kept);
    check(collision.state==JobState::Failed && !collision.publication && kept=="won race" && f.noStage(),"publication refuses a racing destination without a success report and cleans only its own stage");
    j.nameTemplate="callback-failure";
    Control broken;
    broken.progress=[](JobId,std::uint64_t done,std::uint64_t) {if(done>0) throw std::runtime_error("fixture failure");};
    const auto failed=renderMasterWav(j,{},broken);
    check(failed.state==JobState::Failed && failed.error=="fixture failure" && !fs::exists(j.directory/"callback-failure.wav") && f.noStage(),
          "callback failure produces no partial published output");
    f.effect();
    for (const double gain : {std::numeric_limits<double>::infinity(),1e100}) {
        j.nameTemplate="nonfinite"; j.session.precision=Precision::Float64;
        const auto bad=renderMasterWav(j,[gain](const DeviceRequest&,std::string&) {auto d=std::make_unique<Delay>(); d->gain=gain; return d;});
        check(bad.state==JobState::Failed && !fs::exists(j.directory/"nonfinite.wav") && f.noStage(),"nonfinite and finite-double overflow cannot reach Float32 files");
    }
}
void missingSources() {
    Fixture f;
    f.parallel();
    f.store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(2,'lost','lost.wav','wav');"
        "UPDATE audio_clips SET media_id=2 WHERE clip_id=2;");
    const auto missingFile=renderWav(f.job("missing-file"),{});
    check(matches(read(missingFile,866),137) && missingFile.mediaVerification &&
          missingFile.mediaVerification->identities.size()==1 &&
          missingFile.mediaVerification->publicationVerified,
          "missing clip media renders silence while the remaining source is verified");

    f.effect();
    f.store->db().exec("INSERT INTO plugin_params(device_id,param_id,name,normalized_value) VALUES(1,'7','Mix',0.5);"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain) "
        "VALUES(1,'device',1,'7',1,'normalized');");
    const auto missingPlugin=renderWav(f.job("missing-file-and-plugin"),{});
    check(matches(read(missingPlugin,866),137) && f.noStage(),
          "an unavailable audio effect with automation is bypassed as it is during playback");

    f.store->db().exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain) "
                       "VALUES(2,'device',1,'enabled',1,'enum');");
    const auto pendingActivator = renderWav(f.job("missing-plugin-activator"), {});
    check(matches(read(pendingActivator,866),137) && f.noStage(),
          "a saved Activator lane waits for an unavailable local effect during export");

    auto stem=f.job("missing-file-stem"); stem.source.trackId=2;
    const auto samples=read(renderWav(stem,{}),866);
    check(!samples.empty() && std::all_of(samples.begin(),samples.end(),[](float value){return value==0;}),
          "a stem whose only clip lost its file contains silence");

    const auto oldMedia = f.dir.path() / "original-location" / "lost.wav";
    {
        const auto absent = fs::path("..") / (media::pathUtf8(f.dir.path().filename()) + "-moved") / "lost.wav";
        SQLite::Statement update(f.store->db(), "UPDATE media_files SET rel_path=?,abs_path_hint=? WHERE id=2");
        update.bind(1, media::pathUtf8(absent));
        update.bind(2, media::pathUtf8(oldMedia));
        update.exec();
    }
    check(matches(read(renderWav(f.job("moved-and-missing"),{}),866),137),
          "a missing moved sample is silent after both saved locations are checked");
    fs::create_directory(oldMedia.parent_path());
    std::ofstream(oldMedia) << "Different sample";
    const auto changed = renderWav(f.job("moved-but-changed"),{});
    check(changed.state == JobState::Failed && changed.error.find("hash mismatch") != std::string::npos,
          "an existing moved sample with different content remains an export error");
    fs::remove(oldMedia);

    f.store->db().exec("UPDATE audio_clips SET formant_shift=1 WHERE clip_id=1;");
    const auto unsupported=renderWav(f.job("unsupported"),{});
    check(unsupported.state==JobState::Failed && unsupported.error.find("unwarped formant shift unsupported")!=std::string::npos &&
          !fs::exists(unsupported.output) && f.noStage(),
          "an unsupported setting on a clip with available media still stops export");
}
void queueEdits() {
    Fixture f;
    Queue q;
    auto j=f.job("job-{counter}");
    const auto first=q.add(j), second=q.add(j), removed=q.add(j);
    auto immutable=q.snapshot();
    check(q.remove(removed) && immutable.size()==3 && q.snapshot().size()==2,"snapshots remain values after queue removal");
    std::stop_source stop;
    Control c; c.stop=stop.get_token();
    bool once=false;
    c.progress=[&](JobId id,std::uint64_t done,std::uint64_t total) {
        if (!once) {
            once=true;
            check(!q.remove(id) && !q.update(id,j),"running jobs cannot be edited or removed");
            auto edit=j; edit.beginSample=200; edit.endSample=333;
            check(q.update(second,edit),"callbacks may edit pending jobs");
            q.add(j); // must wait for the next run, even if vector reallocates
        }
        if(done==total) stop.request_stop();
    };
    q.run({},c);
    auto entries=q.snapshot();
    check(entries[0].id==first && entries[0].result.state==JobState::Cancelled && entries[1].result.state==JobState::Pending && entries.size()==3,
          "cancelled job is recorded and pending/added jobs remain queued");
    check(q.retry(first),"cancelled job can be explicitly retried");
    q.run(); entries=q.snapshot();
    check(matches(read(entries[1].result,133),200),"pending callback edit changes actual output");
    check(entries[2].result.state==JobState::Completed && entries[2].result.output.filename()=="job-4.wav","added job runs later with stable enqueue ordinal");
    q.add(j); q.run(); entries=q.snapshot();
    check(entries.back().result.output.filename()=="job-5.wav" && entries.back().result.state==JobState::Completed,
          "later runs count completed jobs so counters do not collide");
    q.remove(first); q.add(j); q.run(); entries=q.snapshot();
    check(entries.back().result.output.filename()=="job-6.wav" && entries.back().result.state==JobState::Completed &&
          fs::exists(j.directory/"job-1.wav"),"removing a completed entry preserves its file and never reuses its counter");
}
void latencyChanges() {
    for (const int when : {64,1080}) {
        Fixture f; f.effect();
        const auto result=renderMasterWav(f.job("moving-latency"),[when](const DeviceRequest&,std::string&) {
            auto device=std::make_unique<Delay>(); device->changeAfter=when; return device;
        });
        check(result.state==JobState::Failed && result.error.find("latency")!=std::string::npos &&
              !fs::exists(f.job().directory/"moving-latency.wav") && f.noStage(),
              "first/final block latency reports fail before publication, including renders faster than debounce");
    }
    Fixture f; f.effect(); f.parallel();
    f.store->db().exec("INSERT INTO device_chains(id,track_id) VALUES(2,2);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(2,2,0,1,'Long delay');");
    const auto result=renderMasterWav(f.job("shorter-branch"),[](const DeviceRequest& request,std::string&) {
        auto device=std::make_unique<Delay>();
        if (request.device->id==1) {device->delay=10; device->changeAfter=64;}
        return device;
    });
    check(result.state==JobState::Failed && result.error.find("latency")!=std::string::npos && f.noStage(),
          "a changing shorter branch fails even when the master latency remains unchanged");
}
void coherentRevision() {
    Fixture f; f.effect();
    const std::array beforeEdits{request("project.setName", {{"name", "Before snapshot"}})};
    const auto beforeHead=commit(*f.store,beforeEdits);
    std::int64_t afterHead=0;
    auto job=f.job("{project}-{bpm}");
    const auto result=renderMasterWav(job,[&](const DeviceRequest&,std::string&) {
        // Another open connection commits while the renderer loads devices,
        // between its row projection and its filename/tempo projection.
        const std::array edits{request("project.setName", {{"name", "Later"}}),
            request("track.setMute", {{"id", 1}, {"muted", true}})};
        afterHead=commit(*f.store,edits);
        f.store->db().exec("UPDATE tempo_map SET bpm=75;");
        return std::make_unique<Delay>();
    });
    check(result.output.filename()=="Before snapshot-120.0.wav" && matches(read(result,866),137),
          "concurrent committed edits cannot mix project revisions within one job");
    check(result.snapshot && result.snapshot->journalHead==beforeHead && beforeHead<afterHead &&
          result.snapshot->source.trackId==9 && result.snapshot->names.project=="Before snapshot" &&
          result.snapshot->beginSample==137 && result.snapshot->endSample==1003,
          "journal head, resolved Main, range and names come from the graph's same read snapshot");
    const auto next=renderMasterWav(job,[](const DeviceRequest&,std::string&) {return std::make_unique<Delay>();});
    const auto samples=read(next,866);
    check(next.output.filename()=="Later-75.0.wav" && !samples.empty() &&
          std::all_of(samples.begin(),samples.end(),[](float x) {return x==0;}),
          "the next job sees committed metadata and mute changes together");
    check(next.snapshot && next.snapshot->journalHead==afterHead && next.snapshot->names.project=="Later",
          "next render records the newly committed head, not the earlier job's revision");
    Queue queue;
    auto missing=job; missing.project=f.dir.path()/"missing.adi";
    const auto failed=queue.add(missing);
    job.nameTemplate="after-failure"; queue.add(job);
    queue.run([](const DeviceRequest&,std::string&) {return std::make_unique<Delay>();});
    const auto entries=queue.snapshot();
    check(entries[0].result.state==JobState::Failed && entries[1].result.state==JobState::Completed,
          "a failed job records its error and does not prevent later jobs");
    check(queue.retry(failed) && queue.update(failed,f.job("retry")),"failed jobs can be explicitly retried and corrected");
}
void addDevice(Fixture& f,int track,int id) {
    f.store->db().exec("INSERT INTO device_chains(id,track_id) VALUES("+std::to_string(id)+","+std::to_string(track)+");"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES("+std::to_string(id)+","+std::to_string(id)+",0,1,'Effect');");
}
void queuedRevisionRegions() {
    Fixture f;
    f.store->db().exec("INSERT INTO markers(id,time_base,pos_ns,length_ns,name,kind) VALUES(1,1,0,1000000,'Queued region','cycle');");
    auto job=f.job("{project}-{region}-{track}-{counter}");
    job.regionId=1; job.source={1,OfflineTap::PreFader};
    Queue queue;
    const auto first=queue.add(job);
    queue.add(job);
    const std::array before{request("project.setName", {{"name", "First project"}}),
        request("track.rename", {{"id", 1}, {"name", "First source"}}),
        request("marker.rename", {{"id", 1}, {"name", "First region"}}),
        request("marker.move", {{"id", 1}, {"pos", 1000000}}),
        request("marker.resize", {{"id", 1}, {"length", 3000000}})};
    const auto firstHead=commit(*f.store,before); // after enqueue, before run
    std::int64_t secondHead=0;
    Control control;
    control.progress=[&](JobId id,std::uint64_t done,std::uint64_t total) {
        if (id!=first || done!=total || secondHead!=0) return;
        // After job one's last audio block, before its publication and job two's
        // snapshot. A revision at publication would mislabel the first file.
        const std::array between{request("project.setName", {{"name", "Second project"}}),
            request("track.rename", {{"id", 1}, {"name", "Second source"}}),
            request("marker.rename", {{"id", 1}, {"name", "Second region"}}),
            request("marker.move", {{"id", 1}, {"pos", 5000000}}),
            request("marker.resize", {{"id", 1}, {"length", 2000000}})};
        secondHead=commit(*f.store,between);
    };
    queue.run({},control);
    const auto entries=queue.snapshot();
    const auto& a=entries[0].result;
    const auto& b=entries[1].result;
    check(matches(read(a,144),48) && matches(read(b,96),240),
          "queued cycle jobs hear the independently resolved start/end from their own revision");
    check(a.output.filename()=="First project-First region-First source-1.wav" &&
          b.output.filename()=="Second project-Second region-Second source-2.wav",
          "names resolve after enqueue and again for the next job, not at run entry or publication");
    check(a.snapshot && b.snapshot && a.snapshot->journalHead==firstHead && b.snapshot->journalHead==secondHead &&
          firstHead<secondHead && a.snapshot->source.trackId==1 && a.snapshot->source.tap==OfflineTap::PreFader &&
          a.snapshot->beginSample==48 && a.snapshot->endSample==192 &&
          b.snapshot->beginSample==240 && b.snapshot->endSample==336 &&
          a.snapshot->sampleRate==48000 && a.snapshot->regionId==1 &&
          a.snapshot->names.region=="First region" && b.snapshot->names.region=="Second region",
          "actual source, tap, range, region names and journal heads are captured per job");
    const auto manifest=nlohmann::json::parse(serializeRunManifest(queue.lastRun()));
    const auto& firstMember=manifest["members"][0];
    const auto& secondMember=manifest["members"][1];
    check(firstMember["journal_head"]==firstHead && secondMember["journal_head"]==secondHead &&
          firstMember["result"]["resolved_snapshot"]["journal_head"]==firstHead &&
          firstMember["result"]["resolved_snapshot"]["source"]["tap"]=="pre_fader" &&
          firstMember["result"]["resolved_snapshot"]["begin_sample"]==48 &&
          secondMember["result"]["resolved_snapshot"]["end_sample"]==336 &&
          secondMember["result"]["resolved_snapshot"]["names"]["track"]=="Second source",
          "JSON records the exact resolved provenance, including the older first head at publication");
    check(firstMember["job"]["requested_range"]["begin_sample"]==137 &&
          firstMember["job"]["requested_range"]["end_sample"]==1003 &&
          firstMember["job"]["requested_range"]["region_id"]==1 &&
          firstMember["media_identities"].size()==1 &&
          firstMember["result"]["media_verification"]["publication_verified"]==true,
          "resolved coordinates stay separate from the requested range and measured media verification");
}
void revisionAfterUndo() {
    Fixture f;
    const std::array edits{request("project.setName", {{"name", "Undone name"}})};
    const auto recorded=commit(*f.store,edits);
    check(History(*f.store).undo().ok,"revision fixture undoes its journalled name edit");
    const auto result=renderWav(f.job("{project}"),{});
    check(result.state==JobState::Completed && result.output.filename()=="Before.wav" &&
          result.snapshot && result.snapshot->journalHead==0 && recorded>0 &&
          f.store->db().execAndGet("SELECT MAX(seq) FROM ops").getInt64()==recorded,
          "snapshot captures the current empty head after Undo, not the largest recorded sequence");
}
void directStems() {
    Fixture f;f.effect();f.parallel();addDevice(f,2,2);addDevice(f,9,9);
    auto loader=[](const DeviceRequest& r,std::string&) {
        auto d=std::make_unique<Delay>();
        d->delay=r.device->id==1 ? 13 : (r.device->id==2 ? 77 : 29);
        d->gain=r.device->id==1 ? 2 : (r.device->id==9 ? 4 : 1);
        return d;
    };
    auto job=f.job("{track}-post");job.source.trackId=1;
    check(matches(read(renderWav(job,loader),866),137,2),
          "selected track uses its 13-sample latency, excludes the 77-sample sibling and downstream master FX");
    f.store->db().exec("UPDATE tracks SET muted=1 WHERE id=1;");
    job.nameTemplate="muted-post";
    const auto post=read(renderWav(job,loader),866);
    check(!post.empty() && std::all_of(post.begin(),post.end(),[](float x){return x==0;}),"post-fader stem follows the track mute");
    job.source.tap=OfflineTap::PreFader;job.nameTemplate="prefader";job.session.precision=Precision::Float64;
    check(matches(read(renderWav(job,loader),866),137,2),"pre-fader stem is wet but before strip mute/gain, at double graph precision");
    check(renderMasterWav(job,loader).state==JobState::Failed,"legacy master entry point refuses a selected stem");
    for(const int delay : {100,-100}) {
        Fixture shifted;
        shifted.store->db().exec("INSERT INTO mixer_strip(track_id,delay_samples) VALUES(1,"+std::to_string(delay)+");");
        auto j=shifted.job("shifted");j.beginSample=31;j.endSample=301;j.source.trackId=1;
        check(matches(read(renderWav(j,{}),270),31,1,delay),"direct stem preserves positive/negative track delay on the heard timeline");
        j.nameTemplate="before-fader";j.source.tap=OfflineTap::PreFader;
        check(matches(read(renderWav(j,{}),270),31),"pre-fader endpoint excludes the strip's track delay");
    }
}
void groupStems() {
    Fixture f;f.effect();f.parallel();
    f.store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(5,'group','Drums',3);"
        "UPDATE tracks SET parent_id=5 WHERE id IN(1,2);");
    addDevice(f,5,5);addDevice(f,9,9);
    auto loader=[](const DeviceRequest& r,std::string&) {
        auto d=std::make_unique<Delay>();d->delay=13;
        d->gain=r.device->id==5 ? 2 : (r.device->id==9 ? 4 : 1);return d;
    };
    auto j=f.job("{group}-{track}");j.source.trackId=5;
    const auto group=renderWav(j,loader);
    check(group.output.filename()=="Drums-Drums.wav" && matches(read(group,866),137,4),
          "group stem contains both children and group FX but excludes downstream master FX");
    j.source.trackId=1;
    const auto child=renderWav(j,loader);
    check(child.output.filename()=="Drums-Audio.wav" && matches(read(child,866),137),
          "child stem names its containing group without including that group's processing");

    Fixture analog;
    analog.store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(5,'group','Analog',3);"
        "UPDATE tracks SET parent_id=5 WHERE id=1;"
        "INSERT INTO group_summing(track_id,enabled,flavor,drive_db) VALUES(5,1,'console9',18),(9,1,'console.la',12);");
    auto selected=analog.job("plain-child");selected.source.trackId=1;
    check(matches(read(renderWav(selected,{}),866),137),
          "a child of analog-summing groups exports before the parent console encoder");
    selected.source.trackId=0;selected.nameTemplate="colored-master";
    const auto master=read(renderWav(selected,{}),866);
    check(!master.empty() && !matches(master,137),"console coloration really runs elsewhere in the graph");
}
void multichannelStem() {
    Fixture f;
    std::vector<float> samples(6*4096);
    for(int i=0;i<4096;++i) for(int c=0;c<6;++c) samples[static_cast<std::size_t>(6*i+c)]=source(i,c);
    audio::WavWriter wav(f.dir.path()/"audio.wav",48000,6,audio::WavFormat::Float32);
    wav.write(samples.data(),4096);wav.close();
    auto j=f.job("six-channels");j.source.trackId=1;j.session.channels=6;j.session.precision=Precision::Float64;
    const auto result=renderWav(j,{});
    check(result.state==JobState::Completed,result.error.empty() ? "multichannel stem completes" : result.error.c_str());
    if(result.state!=JobState::Completed)return;
    audio::WavReader reader(result.output);
    check(reader.channels()==6 && reader.frames()==866,"stem WAV retains six channels and exact length");
    std::vector<float> decoded(6*866);reader.read(decoded.data(),866);
    bool exact=true;
    for(int i=0;i<866;++i)for(int c=0;c<6;++c) exact=exact && decoded[static_cast<std::size_t>(6*i+c)]==source(137+i,c);
    check(exact,"every multichannel stem sample survives planar capture and interleaving");
}
void regionStems() {
    Fixture f;
    f.store->db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(2,480480,60);"
        "INSERT INTO markers(id,time_base,pos_ticks,length_ticks,name,kind) VALUES(1,0,120120,480480,'Old','cycle');");
    auto j=f.job("{region}-{track}-{bpm}");j.source.trackId=1;j.regionId=1;j.beginSample=0;j.endSample=0;
    Queue q;q.add(j);
    f.store->db().exec("UPDATE markers SET pos_ticks=240240,name='Changed' WHERE id=1;");
    q.run();const auto result=q.snapshot()[0].result;
    check(result.output.filename()=="Changed-Audio-120.0.wav" && matches(read(result,3000),1000),
          "region bounds/names resolve at render time; absolute end integrates the tempo change");
    check(result.snapshot && result.snapshot->beginSample==1000 && result.snapshot->endSample==4000 &&
          result.snapshot->regionId==1 && result.snapshot->names.region=="Changed",
          "musical region report uses the same integrated tempo bounds and name as its output");
    f.store->db().exec("INSERT INTO markers(id,time_base,pos_ns,length_ns,name,kind) VALUES(2,1,2000000,7000000,'Clock','cycle');");
    j.regionId=2;
    check(matches(read(renderWav(j,{}),336),96),"nanosecond region bounds resolve to exact output samples");
    f.store->db().exec("UPDATE markers SET pos_ns=10001,length_ns=10001,name='Fraction' WHERE id=2;");
    check(matches(read(renderWav(j,{}),1),0),"absolute end is rounded independently, not duration rounded then added");
    j.regionId=900;
    check(renderWav(j,{}).state==JobState::Failed,"missing region is refused");
    f.store->db().exec("UPDATE markers SET kind='position' WHERE id=2;");j.regionId=2;
    check(renderWav(j,{}).state==JobState::Failed,"a position marker is not silently treated as a region");
    f.store->db().exec("UPDATE markers SET kind='cycle',length_ns=NULL WHERE id=2;");
    check(renderWav(j,{}).state==JobState::Failed,"a region without length is refused");
    f.store->db().exec("UPDATE markers SET pos_ns=9223372036854775800,length_ns=20 WHERE id=2;");
    check(renderWav(j,{}).state==JobState::Failed,"overflowing region end is refused before any render");
    f.store->db().exec("UPDATE markers SET pos_ns=0,length_ns=1 WHERE id=2;");
    check(renderWav(j,{}).state==JobState::Failed,"sub-sample region rounded empty is refused");
}
void returnStem() {
    Fixture f;
    std::vector<float> click(8192,0.0f);click[200]=click[201]=1.0f;
    audio::WavWriter wave(f.dir.path()/"audio.wav",48000,2,audio::WavFormat::Float32);
    wave.write(click.data(),4096);wave.close();
    f.store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(4,'return','Echoes',4);"
        "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db,enabled) VALUES"
        "(11,'track',1,'track',4,'send',0,1),(12,'track',4,'track',4,'send',-6.0206,1);");
    auto j=f.job("return");j.source.trackId=4;j.beginSample=0;j.endSample=300;
    const auto samples=read(renderWav(j,{}),300);
    bool correct=!samples.empty();
    for(std::size_t i=0;i<samples.size()/2;++i) {
        const double expected=i>=100 && (i-100)%64==0 ? std::pow(0.5,static_cast<double>((i-100)/64)) : 0.0;
        correct=correct && std::abs(samples[2*i]-expected)<1e-7 && std::abs(samples[2*i+1]-expected)<1e-7;
    }
    check(correct,"return stem keeps its unexported sender and feedback echoes without including the dry master mix");
}
void invalidStemSources() {
    Fixture f;auto j=f.job();
    f.store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(5,'vca','VCA',5),(6,'marker','Markers',6);");
    for(const int track : {5,6,999}) {
        j.source.trackId=track;
        check(renderWav(j,{}).state==JobState::Failed && f.noStage(),"missing and non-audio sources fail without files");
    }
    j.source.trackId=1;j.source.tap=static_cast<OfflineTap>(77);
    check(renderWav(j,{}).state==JobState::Failed,"unsupported capture point is rejected");
}
void pcmExports() {
    Fixture f;
    check(Job{}.encoding.format==audio::WavFormat::Pcm24 && Job{}.encoding.dither==audio::WavDither::Triangular,
          "new jobs use the Export registry's PCM24 and triangular defaults");
    auto j=f.job("default");j.encoding=Encoding{};
    const auto original=read(renderWav(j,{}),866,48000,audio::WavFormat::Pcm24);
    check(!original.empty() && !matches(original,137),"default PCM24 adds dither at the final quantizer");
    j.nameTemplate="partition";j.session.maxFrames=127;
    check(read(renderWav(j,{}),866,48000,audio::WavFormat::Pcm24)==original,"dithered export is independent of callback partition");
    j.nameTemplate="seed";j.encoding.ditherSeed=123;
    check(read(renderWav(j,{}),866,48000,audio::WavFormat::Pcm24)!=original,"job dither seed changes the noise");
    Queue q;j.nameTemplate="queued-{counter}";q.add(j);q.add(j);q.run({});const auto entries=q.snapshot();
    check(read(entries[0].result,866,48000,audio::WavFormat::Pcm24)!=read(entries[1].result,866,48000,audio::WavFormat::Pcm24),
          "different queued jobs have different dither streams");
    f.effect();
    struct Constant final : Delay {
        double value=0;
        void process(const NodeIo& io) noexcept override {
            for(int c=0;c<io.channels;++c)for(int i=0;i<io.frames;++i) {
                const int offset=io.blockOffset+i;
                if(io.wide())io.out64[c][offset]=value;else io.out[c][offset]=static_cast<float>(value);
            }
        }
    };
    for(const auto format:{audio::WavFormat::Pcm16,audio::WavFormat::Pcm24}) {
        const double scale=format==audio::WavFormat::Pcm16?32768.0:8388608.0;
        j.encoding={format,audio::WavDither::None,0};j.session.precision=Precision::Float64;
        j.nameTemplate=format==audio::WavFormat::Pcm16 ? "double16" : "double24";
        auto loader=[&](const DeviceRequest&,std::string&) {
            auto d=std::make_unique<Constant>();d->delay=0;d->value=0.5+0.5/scale-0x1p-52;return d;
        };
        const auto samples=read(renderWav(j,loader),866,48000,format);
        check(!samples.empty() && std::all_of(samples.begin(),samples.end(),[](float v){return v==0.5f;}),
              "double graph reaches PCM quantization without a float interleaving boundary");
        j.nameTemplate+="-float";j.session.precision=Precision::Float32;
        const auto narrow=read(renderWav(j,loader),866,48000,format);
        check(!narrow.empty() && std::all_of(narrow.begin(),narrow.end(),[&](float v){return v==0.5+1/scale;}),
              "float graph positive control rounds the now-exact half-LSB upward");
    }
    j.encoding.format=static_cast<audio::WavFormat>(99);
    check(renderWav(j,{}).state==JobState::Failed && f.noStage(),"unknown encoding fails before rendering or publication");
    j.encoding.format=audio::WavFormat::Pcm24;j.encoding.dither=static_cast<audio::WavDither>(99);
    check(renderWav(j,{}).state==JobState::Failed && f.noStage(),"unsupported dither has no silent fallback");
}

void aiffExports() {
    Fixture f;
    audio::DecodeCache cache(f.dir.path() / "aiff-readback", {1ull << 24, 0});
    auto job = f.job("aiff-comparison");
    for (const auto format : {audio::WavFormat::Pcm16, audio::WavFormat::Pcm24, audio::WavFormat::Float32}) {
        job.encoding = {format, audio::WavDither::Triangular, 42};
        job.normalization.mode = format == audio::WavFormat::Pcm24 ? NormalizeMode::Peak : NormalizeMode::Off;
        job.normalization.truePeakCeilingDb = -1;
        job.nameTemplate = "pcm-" + std::to_string(static_cast<int>(format));
        const auto expected = read(renderWav(job, {}), 866, 48000, format);
        auto aiff = job;
        aiff.container = PcmContainer::Aiff;
        aiff.exactPcmFilename = job.nameTemplate + ".AIFF";
        const auto result = renderWav(aiff, {});
        check(result.state == JobState::Completed && result.output.filename() == *aiff.exactPcmFilename &&
                  result.framesWritten == 866 && audio::sniff(result.output) == audio::SourceFormat::Aiff && f.noStage(),
              result.error.empty() ? "AIFF publishes the exact chosen filename and range" : result.error.c_str());
        if (result.state != JobState::Completed)
            continue;
        const auto decoded = cache.playable(result.output);
        audio::WavReader reader(decoded);
        std::vector<float> actual(expected.size());
        check(!expected.empty() && reader.frames() == 866 && reader.channels() == 2 && reader.sampleRate() == 48000 &&
                  reader.read(actual.data(), 866) == 866 && actual == expected,
              "AIFF/AIFF-C decodes to the same stereo samples as WAV, including dither and normalization");
        cache.release(decoded);
    }

    job.container = PcmContainer::Aiff;
    job.endSample = std::numeric_limits<std::uint32_t>::max();
    const auto tooLarge = renderWav(job, {});
    check(tooLarge.state == JobState::Failed && tooLarge.error.find("AIFF file would exceed") != std::string::npos &&
              tooLarge.output.empty() && f.noStage(),
          "AIFF ranges beyond its container limit fail before writing anything");
}

void hostOwnerLifecycle() {
    Fixture f;
    f.effect();
    std::array<std::byte,1> bytes{std::byte{1}};
    const auto hash=media::blake3Bytes(bytes).hex;
    SQLite::Statement blob(f.store->db(),"INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,1)");
    blob.bind(1,hash);
    blob.bind(2,bytes.data(),1);
    blob.exec();
    SQLite::Statement state(f.store->db(),
        "INSERT INTO plugin_state(device_id,stream_role,state_hash,format_hint) VALUES(1,'state',?,'opaque')");
    state.bind(1,hash);
    state.exec();
    const auto owner=std::this_thread::get_id();
    for(const bool cancel:{false,true}) {
        struct Trace {
            std::thread::id owner;
            bool correct=true,restored=false;
            unsigned created=0,offline=0,prepared=0,rendered=0,serviced=0,destroyed=0;
            void onOwner() { correct=correct && std::this_thread::get_id()==owner; }
        } trace{owner};
        struct Device final:Delay {
            Trace& trace;
            explicit Device(Trace& events):trace(events) {
                trace.onOwner();
                ++trace.created;
            }
            ~Device() override { trace.onOwner(); ++trace.destroyed; }
            void setOffline(bool offline) noexcept override {
                trace.onOwner();
                if(offline)++trace.offline;
            }
            bool loadState(const std::string&,const std::vector<std::uint8_t>& data) override {
                trace.onOwner();
                trace.restored=data==std::vector<std::uint8_t>{1};
                gain=0.5;
                return trace.restored;
            }
            void prepare(double,std::int32_t) override { trace.onOwner(); ++trace.prepared; }
            void pumpMainThread() override { trace.onOwner(); ++trace.serviced; }
            void process(const NodeIo& io) noexcept override {
                trace.correct=trace.correct && std::this_thread::get_id()!=trace.owner && trace.restored;
                ++trace.rendered;
                Delay::process(io);
            }
        };
        HostDispatch host;
        std::stop_source stop;
        Control control;
        control.stop=stop.get_token();
        control.onHost=[&](std::function<void()> action) { host.invoke(std::move(action)); };
        control.serviceHost=[&] {
            trace.onOwner();
            if(cancel && trace.rendered!=0)stop.request_stop();
        };
        std::packaged_task<Result()> task([&] {
            return renderWav(f.job(cancel?"host-cancel":"host-complete"),
                [&](const DeviceRequest&,std::string&) {return std::make_unique<Device>(trace);},control);
        });
        auto done=task.get_future();
        std::jthread worker(std::move(task));
        host.wait(done);
        const auto result=done.get();
        check(trace.correct && trace.created==1 && trace.offline==1 && trace.prepared!=0 &&
              trace.restored && trace.rendered!=0 && trace.serviced!=0 && trace.destroyed==1,
              "load, offline mode, state, prepare, service and destruction run on the host owner");
        if(cancel) {
            check(result.state==JobState::Cancelled && !fs::exists(result.output) && f.noStage(),
                  "owner remains available to dismantle a cancelled render without publishing it");
        } else {
            check(matches(read(result,866),137,0.5),"the worker renders the saved plug-in state into the WAV");
        }
    }
}
void exportWorkProgress() {
    Fixture f;
    auto job=f.job("normalized-progress");
    job.normalization.mode=NormalizeMode::Peak;
    std::uint64_t written=0;
    std::vector<double> fractions, afterCapture;
    bool prerollAdvanced=false, finishedAfterPublish=false, frameCountsUnchanged=true;
    Control control;
    control.progress=[&](JobId,std::uint64_t done,std::uint64_t total) {
        frameCountsUnchanged=frameCountsUnchanged && total==866;
        written=done;
    };
    control.wavProgress=[&](JobId,double fraction) {
        fractions.push_back(fraction);
        prerollAdvanced=prerollAdvanced || (written==0 && fraction>0);
        if(written==866 && fraction<1) afterCapture.push_back(fraction);
        if(fraction==1) finishedAfterPublish=fs::exists(job.directory/"normalized-progress.wav");
    };
    const auto result=renderWav(job,{},control);
    check(result.state==JobState::Completed && !fractions.empty() && fractions.front()==0 &&
          fractions.back()==1 && std::is_sorted(fractions.begin(),fractions.end()) &&
          std::count(fractions.begin(),fractions.end(),1.0)==1 && finishedAfterPublish,
          "WAV work progress advances monotonically from zero to publication");
    check(prerollAdvanced,"WAV work advances before the selected audio");
    check(afterCapture.size()>1 && afterCapture.front()<afterCapture.back(),
          "WAV work continues advancing after capture during normalization");
    check(written==866 && frameCountsUnchanged,"existing render progress still measures output frames");

    std::stop_source stop;
    job.nameTemplate="cancel-normalizing";
    double beforeNormalization=0, last=0;
    control.stop=stop.get_token();
    control.wavProgress=[&](JobId,double fraction) {
        last=fraction;
        if(written==866 && beforeNormalization==0) beforeNormalization=fraction;
        else if(beforeNormalization>0 && fraction>beforeNormalization) stop.request_stop();
    };
    const auto cancelled=renderWav(job,{},control);
    check(cancelled.state==JobState::Cancelled && beforeNormalization>0 && last<1 &&
          !fs::exists(job.directory/"cancel-normalizing.wav") && f.noStage(),
          "cancellation during normalization stays below 100 percent and removes the stage");
}
void normalizedRenders() {
    exportWorkProgress();
    for(const auto precision:{Precision::Float32,Precision::Float64}) {
        Fixture f;f.effect();auto job=f.job(precision==Precision::Float64?"normalized-wide":"normalized-float");
        job.beginSample=200;job.endSample=220;job.session.precision=precision;job.normalization.mode=NormalizeMode::Peak;
        std::uint64_t processed=0;int loads=0;
        struct CountedDelay final:Delay {
            std::uint64_t& counter;
            explicit CountedDelay(std::uint64_t& value):counter(value){}
            void process(const NodeIo& io)noexcept override{counter+=static_cast<std::uint64_t>(io.frames);Delay::process(io);}
        };
        auto loader=[&](const DeviceRequest&,std::string&){++loads;auto device=std::make_unique<CountedDelay>(processed);device->distinguishWidth=true;return device;};
        const auto result=renderWav(job,loader);const auto samples=read(result,20);
        double peak=0;for(std::int64_t frame=200;frame<220;++frame)for(int channel=0;channel<2;++channel)peak=std::max(peak,std::abs(static_cast<double>(source(frame,channel))));
        bool exact=samples.size()==40;
        for(std::size_t frame=0;frame<samples.size()/2;++frame)for(int channel=0;channel<2;++channel)exact=exact && samples[frame*2+static_cast<std::size_t>(channel)]==static_cast<float>(source(200+static_cast<std::int64_t>(frame),channel)/peak);
        check(exact && result.normalization && result.normalization->sourceSamplePeakDb && std::abs(*result.normalization->sourceSamplePeakDb-20*std::log10(peak*(precision==Precision::Float64?2:1)))<1e-10,"normalization measures only the selected compensated range, excluding preroll, and preserves either source width [P:export#16]");
        check(loads==1 && processed==297 && f.noStage(),"normalization captures one continuous plugin run and cleans private double PCM after publication");
    }
    Fixture f;auto job=f.job("normalized-cancel");job.normalization.mode=NormalizeMode::Peak;std::stop_source stop;Control control;control.stop=stop.get_token();bool duringEncoding=false;
    auto encoding=[&]{
        for(const auto& path:fs::directory_iterator(job.directory))if(path.path().filename().string().starts_with(".adi-render-") && fs::exists(path.path()/"render.wav"))return true;
        return false;
    };
    control.serviceHost=[&]{if(encoding()){duringEncoding=true;stop.request_stop();}};
    const auto cancelled=renderWav(job,{},control);
    check(duringEncoding && cancelled.state==JobState::Cancelled && !fs::exists(cancelled.output) && f.noStage(),"cancellation during encoding removes the capture/output without publishing a partial normalized job");
    job.nameTemplate="normalized-race";bool raced=false;control={};
    control.serviceHost=[&]{if(!raced && encoding()){std::ofstream external(job.directory/"normalized-race.wav",std::ios::binary);external<<"external";raced=true;}};
    const auto collision=renderWav(job,{},control);std::ifstream existing(job.directory/"normalized-race.wav",std::ios::binary);const std::string retained{std::istreambuf_iterator<char>(existing),std::istreambuf_iterator<char>()};
    check(raced && collision.state==JobState::Failed && retained=="external" && f.noStage(),"normalized publication preserves a destination created after rendering started");
    Queue queue;job=f.job("normalized-queued");const auto id=queue.add(job);job.normalization.mode=NormalizeMode::Peak;
    check(queue.update(id,job) && queue.snapshot()[0].job.normalization.mode==NormalizeMode::Peak,"pending queue edits retain the captured normalization choice");
    queue.run();check(queue.snapshot()[0].result.state==JobState::Completed && queue.snapshot()[0].result.normalization.has_value(),"a queued normalization choice produces a report on its completed job");
}

void namesAndValidation() {
    NameContext c{"../A{track}","B/C","G","90",3};
    check(resolveName("{project}-{track}-{group}-{bpm}-{counter}",c)==".._A{track}-B_C-G-90-3","metadata cannot inject paths or more placeholders");
    check(resolveName("{{literal}}",c)=="{literal}" && resolveName("CON",c)=="_CON","literal braces and Windows reserved names are portable");
    check(resolveName("COM\xC2\xB9.wav",c)=="_COM\xC2\xB9.wav" && resolveName("lpt\xC2\xB2",c)=="_lpt\xC2\xB2",
          "Windows superscript device names are escaped, including names with extensions");
    for (const auto* reserved : {"CON", "con.txt", "PRN", "AUX", "nul.tar.gz", "COM9", "LPT9", "CONIN$", "CONOUT$"})
        check(resolveName(reserved,c)==std::string("_")+reserved,"reserved filename stem is escaped without changing case or extension");
    for (const auto* reserved : {"AUX .2", "CON .x", "NUL .wav", "COM1 .2", "LPT9  .tmp",
            "COM\xC2\xB9 .a", "lpt\xC2\xB2 .a", "CONIN$ .a", "CONOUT$ .a"}) {
        check(resolveName(reserved,c)==std::string("_")+reserved,
              "spaces before the first extension do not hide a reserved device stem");
    }
    for (const auto* ordinary : {"AUX x", " AUX .2", "COM0 .2", "COM10 .2", "LPT10 .2"}) {
        check(resolveName(ordinary,c)==ordinary,"ordinary stems keep their original ASCII spaces");
    }
    auto projectWithDeviceStem=c;
    projectWithDeviceStem.project="Aux .v2";projectWithDeviceStem.track="Main";
    check(resolveName("{project} - {track}",projectWithDeviceStem)=="_Aux .v2 - Main",
          "the default template escapes the project stem before a spaced dot");
    check(resolveName("COM0",c)=="COM0" && resolveName("COM10",c)=="COM10" && resolveName("LPT10",c)=="LPT10",
          "ordinary names sharing a device prefix remain unchanged");
    check(resolveName("Mix. . ",c)=="Mix" && resolveName("con. ",c)=="_con",
          "trailing ASCII dots and spaces are trimmed before reserved-name escaping");
    const auto finalNameAccepted=[](const std::string& name) {
        try { validateOutputName(media::pathFromUtf8(name)); return true; }
        catch (const std::invalid_argument&) { return false; }
    };
    for (const std::string suffix : {".wav", ".stem.mp4"}) {
        for (const std::size_t bytes : {199u,200u,201u}) {
            const auto plain=resolveName(std::string(bytes-suffix.size(),'n'),c)+suffix;
            check(plain.size()==bytes && finalNameAccepted(plain)==(bytes<=200),
                  "final byte cap includes the complete format suffix");
            const auto reserved=resolveName("CON."+std::string(bytes-suffix.size()-5,'n'),c)+suffix;
            check(reserved.size()==bytes && finalNameAccepted(reserved)==(bytes<=200),
                  "final byte cap includes the reserved-name underscore and complete suffix");
        }
    }
    check(resolveName(std::string(200,'n'),c).size()==200,
          "standalone resolveName retains its existing extension-free contract");
    check(finalNameAccepted(resolveName("Mix"+std::string(25,' ')+"...",c)+".wav"),
          "trimmed trailing bytes do not count against the final name");
    for (const char* bad : {"{unknown}","{project","project}","{}","..."}) {
        bool threw=false; try {(void)resolveName(bad,c);} catch(const std::invalid_argument&) {threw=true;}
        check(threw,"invalid name syntax fails explicitly");
    }
    Fixture f;
    auto job=f.job(); job.endSample=job.beginSample;
    check(renderMasterWav(job,{}).state==JobState::Failed,"empty range fails");
    Queue q;
    bool threw=false; try {q.add(job);} catch(const std::invalid_argument&) {threw=true;}
    check(threw && q.snapshot().empty(),"invalid queue job is refused without mutation");
    job=f.job(); job.session.maxFrames=0;
    check(renderMasterWav(job,{}).state==JobState::Failed,"zero block size fails");
}
void finalNamesAndCaseCollisions() {
    Fixture f;
    // UTF-8 byte boundaries with shorter native UTF-16 paths: this tests the
    // export policy independently of the operating system's full-path limit.
    for (const std::size_t bytes : {199u,200u,201u}) {
        std::string stem;
        for (std::size_t i=0;i<(bytes-4)/2;++i) stem+="\xC3\xA9";
        if ((bytes-4)%2) stem+='x';
        const auto result=renderWav(f.job(stem),{});
        check(media::pathUtf8(result.output.filename()).size()==bytes,"WAV result records the actual final UTF-8 filename");
        if (bytes<=200)
            check(result.state==JobState::Completed && fs::exists(result.output) && f.noStage(),
                  "WAV publishes names of 199 and 200 final UTF-8 bytes");
        else
            check(result.state==JobState::Failed && result.error=="final export name exceeds 200 UTF-8 bytes" &&
                      !fs::exists(result.output) && f.noStage(),"201-byte WAV name fails before staging or publication");
    }
    const auto first=renderWav(f.job("CaseOnly"),{});
    check(first.state==JobState::Completed,"case collision fixture is published");
    const auto folded=f.job().directory/"caseonly.wav";
    const bool aliases=fs::exists(folded);
    std::printf("Export names: case-only filesystem alias %s\n",aliases?"yes":"no");
    auto caseJob=f.job("caseonly");
    caseJob.beginSample=200; // A mistaken replacement would change the first file's length.
    const auto second=renderWav(caseJob,{});
    check(second.state==JobState::Failed && second.error=="export destination already exists",
          "portable preflight refuses a case-only collision independently of native lookup");
    audio::WavReader retained(first.output);
    check(retained.frames()==866 && f.noStage(),"case-only publication leaves the first file intact and no stage");
}
void singleWavDestination() {
    Fixture f;
    auto job = f.job();
    job.nameTemplate = "{unused template}";
    job.exactWavFilename = "Take {final} .WAV";
    Queue queue;
    queue.add(job);
    queue.run();
    const auto first = queue.snapshot().front().result;
    check(first.state == JobState::Completed && first.output == job.directory / *job.exactWavFilename &&
          first.publication && matches(read(first, 866), 137) && f.noStage(),
          "single WAV keeps the exact save-dialog filename and emits the selected PCM");
    const auto manifest = nlohmann::json::parse(serializeRunManifest(queue.lastRun()));
    check(manifest["members"][0]["job"]["exact_wav_filename"] == *job.exactWavFilename &&
          manifest["members"][0]["result"]["output"] == media::pathUtf8(first.output),
          "run manifest records the chosen literal and committed path");

    const auto original = media::blake3File(first.output);
    job.beginSample = 200;
    const auto refused = renderWav(job, {});
    check(refused.state == JobState::Failed && refused.framesWritten == 0 &&
          media::blake3File(first.output).hex == original.hex, "unconfirmed overwrite preserves the existing waveform");
    job.collision = Collision::Replace;
    const auto replaced = renderWav(job, {});
    check(replaced.state == JobState::Completed && replaced.output.filename() == first.output.filename() &&
          fs::equivalent(replaced.output, first.output) && replaced.replacement && replaced.replacement->existing &&
          matches(read(replaced, 803), 200) && f.noStage(),
          "explicit overwrite replaces only the selected ordinary WAV and keeps its literal spelling");
    job.collision = Collision::Fail;
    for (const char* invalid : {"../outside.wav", "directory\\mix.wav", "AUX .wav", "Mix", "Mix.wav."}) {
        job.exactWavFilename = invalid;
        const auto failure = renderWav(job, {});
        check(failure.state == JobState::Failed && failure.framesWritten == 0 && !failure.publication && f.noStage(),
              "a nonportable literal or non-WAV suffix is refused before publication");
    }
    job.exactWavFilename = std::string(197, 'x') + ".wav";
    check(renderWav(job, {}).state == JobState::Failed && f.noStage(),
          "the literal WAV filename limit counts the complete extension");
    job.exactWavFilename = "literal.wav";
    job.collision = Collision::Increment;
    check(renderWav(job, {}).state == JobState::Failed && !fs::exists(job.directory / "literal.wav"),
          "a literal save path cannot be silently renumbered");
}
void collisionModes() {
    Fixture f;
    check(f.job().collision==Collision::Fail,"collision policy defaults to Fail");
    const auto original=renderWav(f.job("delivery"),{});
    const auto originalHash=media::blake3File(original.output).hex;
    auto changed=f.job("delivery");changed.beginSample=200;
    const auto refused=renderWav(changed,{});
    check(refused.state==JobState::Failed && refused.framesWritten==0 && !refused.publication &&
          media::blake3File(original.output).hex==originalHash,
          "default collision refuses before audio and preserves the existing output bytes");
    changed.collision=Collision::Increment;
    Queue queue;queue.add(changed);queue.run();
    const auto numbered=queue.snapshot().front().result;
    check(numbered.output.filename()=="delivery (2).wav" && numbered.state==JobState::Completed &&
          media::blake3File(original.output).hex==originalHash && matches(read(numbered,803),200),
          "explicit Increment publishes actual selected audio under the first free numbered filename");
    const auto doc=nlohmann::json::parse(serializeRunManifest(queue.lastRun()));
    check(doc["members"][0]["job"]["collision"]=="increment" &&
          doc["members"][0]["result"]["output"]==media::pathUtf8(numbered.output),
          "run manifest records requested collision mode separately from the chosen delivery path");
    fs::create_directory(changed.directory/"delivery (3).wav");
    const auto fourth=renderWav(changed,{});
    check(fourth.state==JobState::Completed && fourth.output.filename()=="delivery (4).wav" &&
          fs::is_directory(changed.directory/"delivery (3).wav"),
          "Increment treats a directory as occupied and never removes it");
    auto race=f.job("late-increment");race.collision=Collision::Increment;
    {std::ofstream sentinel(race.directory/"late-increment.wav");sentinel<<"original";}
    bool arrived=false;
    Control control;
    control.progress=[&](JobId,std::uint64_t done,std::uint64_t) {
        if(arrived || done==0)return;
        std::ofstream sentinel(race.directory/"late-increment (2).wav");sentinel<<"late arrival";
        sentinel.close();arrived=true;
    };
    const auto collided=renderWav(race,{},control);
    check(arrived && collided.state==JobState::Failed && !collided.publication &&
          collided.output.filename()=="late-increment (2).wav" && collided.framesWritten==866 &&
          fs::file_size(collided.output)==12 && !fs::exists(race.directory/"late-increment (3).wav") && f.noStage(),
          "a late collision fails the frozen numbered delivery instead of renumbering or replacing it");
    {
        std::ifstream file(collided.output);std::string value;std::getline(file,value);
        check(value=="late arrival","late arriving output retains its exact external bytes");
    }
    auto boundary=f.job();boundary.nameTemplate.clear();boundary.collision=Collision::Increment;
    for(unsigned i=0;i<96;++i)boundary.nameTemplate+="\xC3\xA9"; // 192+4 increment+4 WAV suffix
    check(renderWav(boundary,{}).state==JobState::Completed,"increment boundary has a real occupied original");
    const auto exact=renderWav(boundary,{});
    check(exact.state==JobState::Completed && media::pathUtf8(exact.output.filename()).size()==200 &&
          exact.output.filename()==media::pathFromUtf8(boundary.nameTemplate+" (2).wav"),
          "final 200-byte budget includes both increment and complete WAV suffix");
    for(unsigned i=3;i<=9;++i) {
        std::ofstream occupied(boundary.directory/media::pathFromUtf8(boundary.nameTemplate+" ("+std::to_string(i)+").wav"));
        occupied<<"preserved";
    }
    const auto twoDigits=renderWav(boundary,{});
    check(twoDigits.state==JobState::Failed && media::pathUtf8(twoDigits.output.filename()).size()==201 &&
          twoDigits.output.filename()==media::pathFromUtf8(boundary.nameTemplate+" (10).wav") &&
          !fs::exists(twoDigits.output) && fs::file_size(boundary.directory/media::pathFromUtf8(boundary.nameTemplate+" (9).wav"))==9,
          "increment digit growth is revalidated without truncation or clobbering lower numbers");
    boundary.nameTemplate+='x';
    const auto longerOriginal=renderWav(boundary,{});
    const auto longerHash=media::blake3File(longerOriginal.output).hex;
    const auto tooLong=renderWav(boundary,{});
    check(longerOriginal.state==JobState::Completed && tooLong.state==JobState::Failed &&
          tooLong.framesWritten==0 && media::pathUtf8(tooLong.output.filename()).size()==201 &&
          tooLong.error=="final export name exceeds 200 UTF-8 bytes" &&
          !fs::exists(tooLong.output) && media::blake3File(longerOriginal.output).hex==longerHash,
          "increment overflow refuses without truncating or replacing a valid shorter original");
    const auto cased=renderWav(f.job("NativeCase"),{});
    auto lower=f.job("nativecase");lower.collision=Collision::Increment;
    const auto next=renderWav(lower,{});
    check(next.state==JobState::Completed && next.output.filename()=="nativecase (2).wav" &&
          !fs::equivalent(cased.output,next.output),
          "Increment reserves portable case-folded spellings independently of native lookup");
    const auto unicodeUpper=renderWav(f.job("\xC3\x89tude"),{});
    auto unicodeLower=f.job("\xC3\xA9tude");unicodeLower.collision=Collision::Increment;
    const auto unicodeNext=renderWav(unicodeLower,{});
    check(unicodeNext.state==JobState::Completed &&
          unicodeNext.output.filename()==media::pathFromUtf8("\xC3\xA9tude (2).wav") &&
          !fs::equivalent(unicodeUpper.output,unicodeNext.output),
          "Increment folds non-ASCII case with the pinned Unicode policy");
    auto reserved=f.job("CON.song");reserved.collision=Collision::Increment;
    check(renderWav(reserved,{}).state==JobState::Completed,"reserved stem is escaped before collision resolution");
    check(renderWav(reserved,{}).output.filename()=="_CON.song (2).wav",
          "reserved prefix escape and dotted stem survive numbered final naming");
    auto trailing=f.job("trailing. ");trailing.collision=Collision::Increment;
    check(renderWav(trailing,{}).state==JobState::Completed &&
          renderWav(trailing,{}).output.filename()=="trailing (2).wav",
          "trailing dot and space removal happens before suffix-aware Increment");
    auto invalid=f.job("invalid-policy");invalid.collision=static_cast<Collision>(99);
    check(renderWav(invalid,{}).state==JobState::Failed &&
          !fs::exists(invalid.directory/"invalid-policy.wav") && f.noStage(),
          "unknown collision policies are refused before publication");
}
std::error_code nativeExclusiveRename(const fs::path& source,const fs::path& target) {
#if defined(_WIN32)
    if(MoveFileExW(source.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))return {};
    const auto error=GetLastError();
    if(error==ERROR_ALREADY_EXISTS || error==ERROR_FILE_EXISTS)return std::make_error_code(std::errc::file_exists);
    return {static_cast<int>(error),std::system_category()};
#else
    std::error_code error;fs::create_hard_link(source,target,error);
    if(!error)fs::remove(source,error);
    return error;
#endif
}
bool nativeAliasAfterFoldCheck=false;
void foldedCollisionNames() {
    Fixture f;
    const auto write=[](const fs::path& path,const char* bytes){std::ofstream out(path,std::ios::binary);out<<bytes;out.close();if(!out)throw std::runtime_error("folded collision fixture write");};
    const auto text=[](const fs::path& path){std::ifstream in(path,std::ios::binary);return std::string{std::istreambuf_iterator<char>(in),{}};};
    const auto original=renderWav(f.job("Stra\xC3\x9F" "e"),{});
    check(original.state==JobState::Completed,"Unicode expansion source is a real rendered file");
    const auto digest=media::blake3File(original.output).hex;
    const auto upper=f.job("STRASSE");
    const bool nativeAliases=fs::exists(upper.directory/"STRASSE.wav");
    std::printf("Export names: native Strasse expansion alias %s\n",nativeAliases?"yes":"no");
#if defined(_WIN32)
    check(!nativeAliases,"Windows fixture distinguishes Strasse fold expansion from native case lookup");
#endif
    const auto fail=renderWav(upper,{});
    check(fail.state==JobState::Failed && fail.framesWritten==0 && !fail.publication &&
          media::blake3File(original.output).hex==digest,"Fail rejects full Unicode fold expansion before rendering and preserves the original");
    write(upper.directory/media::pathFromUtf8("Stra\xC3\x9F" "e (2).WAV"),"number two");
    write(upper.directory/media::pathFromUtf8("unrelated-\xF0\x9F\x8E\xB5.txt"),"unrelated Unicode");
    auto increment=upper;increment.collision=Collision::Increment;
    const auto third=renderWav(increment,{});
    check(third.state==JobState::Completed && third.output.filename()=="STRASSE (3).wav" &&
          media::blake3File(original.output).hex==digest && matches(read(third,866),137),
          "Increment uses folded complete filenames for every candidate and renders the chosen audio");
    const auto composed=renderWav(f.job("Caf\xC3\xA9"),{});
    const auto decomposed=renderWav(f.job("CAFE\xCC\x81"),{});
    check(composed.state==JobState::Completed && decomposed.state==JobState::Failed && !decomposed.publication,
          "canonical composition and Unicode folding reject a decomposed equivalent name");
    for(const auto mode:{Collision::Fail,Collision::Increment}) {
        const std::string ending=mode==Collision::Fail?"-fail":"-increment";
        auto job=f.job("Stra\xC3\x9F" "e-late"+ending);job.collision=mode;
        if(mode==Collision::Increment)write(job.directory/media::pathFromUtf8(job.nameTemplate+".wav"),"existing original");
        const auto number=mode==Collision::Increment?" (2)":"";
        const auto arrival=job.directory/media::pathFromUtf8("STRASSE-LATE"+ending+number+".WAV");
        bool introduced=false;Control control;
        control.progress=[&](JobId,std::uint64_t done,std::uint64_t){if(done && !introduced){write(arrival,"later folded arrival");introduced=true;}};
        const auto result=renderWav(job,{},control);
        check(introduced && result.state==JobState::Failed && result.framesWritten>0 && !result.publication &&
              text(arrival)=="later folded arrival" && result.output.filename()==media::pathFromUtf8(job.nameTemplate+number+".wav") &&
              !fs::exists(job.directory/media::pathFromUtf8(job.nameTemplate+" (3).wav")) && f.noStage(),
              "publication recheck rejects a late folded collision without renumbering or removing the arriving file");
    }
    Control exactRace;
    exactRace.publicationHooks.rename=[](const fs::path& source,const fs::path& target) {
        {std::ofstream out(target,std::ios::binary);out<<"native last-instant arrival";}
        return nativeExclusiveRename(source,target);
    };
    const auto exact=renderWav(f.job("native-last-instant"),{},exactRace);
    check(exact.state==JobState::Failed && !exact.publication && text(exact.output)=="native last-instant arrival" && f.noStage(),
          "native exclusive publication still refuses an exact collision introduced after the folded recheck");
    Control foldRace;
    foldRace.publicationHooks.rename=[](const fs::path& source,const fs::path& target) {
        {std::ofstream out(target.parent_path()/"AFTER-CHECK-SS.wav",std::ios::binary);out<<"fold-only late arrival";}
        nativeAliasAfterFoldCheck=fs::exists(target);
        return nativeExclusiveRename(source,target);
    };
    const auto after=renderWav(f.job("after-check-\xC3\x9F"),{},foldRace);
    check(text(after.output.parent_path()/"AFTER-CHECK-SS.wav")=="fold-only late arrival" &&
          (nativeAliasAfterFoldCheck?after.state==JobState::Failed:after.state==JobState::Completed) && f.noStage(),
          "after the folded recheck native no-clobber protects native aliases only, with no cross-process folded reservation");
}
std::error_code failDirectorySync(const fs::path&) { return std::make_error_code(std::errc::io_error); }
std::error_code confirmDirectorySync(const fs::path&) { return {}; }
std::error_code refuseRename(const fs::path&, const fs::path&) { return std::make_error_code(std::errc::operation_not_supported); }
void identityBoundReplace() {
    Fixture f;
    bool preparedBeforeProgress=false,progressBeforePrepared=false;
    Control preparedControl;
    preparedControl.progress=[&](JobId,std::uint64_t,std::uint64_t) {
        if(!preparedBeforeProgress)progressBeforePrepared=true;
    };
    const auto preparedRender=render::detail::renderWavPrepared(f.job("prepared-bridge"),{},preparedControl,0,1,
        [&](const Result& prepared) {
            preparedBeforeProgress=prepared.framesWritten==0 && prepared.snapshot.has_value() &&
                !fs::exists(prepared.output);
        });
    check(preparedRender.state==JobState::Completed && preparedBeforeProgress && !progressBeforePrepared &&
          matches(read(preparedRender,866),137),
          "NI's reusable prepared-render bridge receives names and revision before Main progress or audio");
    const auto write=[](const fs::path& path,const char* bytes) {
        std::ofstream out(path,std::ios::binary);out<<bytes;out.close();
        if(!out)throw std::runtime_error("Replace fixture write");
    };
    const auto text=[](const fs::path& path) {
        std::ifstream in(path,std::ios::binary);return std::string{std::istreambuf_iterator<char>(in),{}};
    };
    auto job=f.job("replace");job.collision=Collision::Replace;
    const auto target=job.directory/"replace.wav";
    write(target,"old complete delivery");
    std::error_code error;const auto oldIdentity=media::readFileIdentity(target,error);
    const auto oldHash=media::blake3File(target).hex;
    // Replace freezes the delivery under its canonical parent (prepareOutput).
    // The fixture's temp parent need not be canonical: macOS TMPDIR lives under
    // /var, a symlink to /private/var. The filename stays exactly as requested.
    const auto delivered=fs::canonical(job.directory)/target.filename();
    Queue queue;queue.add(job);queue.run();const auto complete=queue.snapshot().front().result;
    check(complete.state==JobState::Completed && complete.publication && !complete.publication->usedFallback &&
          complete.output==delivered && complete.replacement && complete.replacement->existing==oldIdentity &&
          complete.replacement->blake3==oldHash && matches(read(complete,866),137) && f.noStage(),
          "explicit Replace binds old identity and bytes, then publishes real rendered PCM at the same name");
    check(media::readFileIdentity(target,error)!=oldIdentity,
          "successful native Replace installs the staged file identity rather than editing old bytes in place");
    const auto manifest=nlohmann::json::parse(serializeRunManifest(queue.lastRun()));
    check(manifest["members"][0]["job"]["collision"]=="replace" &&
          manifest["members"][0]["result"]["replacement_preflight"]["blake3"]==oldHash,
          "run manifest distinguishes requested Replace and its bound preflight target");
    job.nameTemplate="absent-replace";
    const auto absent=renderWav(job,{});
    check(absent.state==JobState::Completed && absent.replacement && !absent.replacement->existing &&
          !absent.publication->usedFallback && matches(read(absent,866),137),
          "Replace with absent preflight target uses native no-clobber publication");
    for(const auto* mutation:{"rebound","removed","in-place"}) {
        job.nameTemplate=mutation;
        const auto selected=job.directory/media::pathFromUtf8(job.nameTemplate+".wav");
        const auto kept=job.directory/media::pathFromUtf8(job.nameTemplate+".kept");
        write(selected,"old bytes");bool changed=false;Control control;
        control.progress=[&](JobId,std::uint64_t done,std::uint64_t) {
            if(!done || changed)return;
            changed=true;
            if(job.nameTemplate=="rebound") {fs::rename(selected,kept);write(selected,"new writer");}
            else if(job.nameTemplate=="removed")fs::rename(selected,kept);
            else write(selected,"new bytes"); // Same native identity, same byte count.
        };
        const auto failed=renderWav(job,{},control);
        check(changed && failed.state==JobState::Failed && !failed.publication && failed.framesWritten==866 && f.noStage(),
              "identity change, disappearance, and in-place edit during render each refuse Replace");
        check(job.nameTemplate=="removed" ? !fs::exists(selected) && text(kept)=="old bytes" :
              text(selected)==(job.nameTemplate=="rebound" ? "new writer" : "new bytes"),
              "Replace refusal leaves another writer's bytes or missing path unchanged");
        if(job.nameTemplate=="in-place")
            check(failed.replacement && media::readFileIdentity(selected,error)==failed.replacement->existing && fs::file_size(selected)==9,
                  "in-place refusal is verified with the same native identity and byte count");
    }
    job.nameTemplate="Stra\xC3\x9F" "e-replace";
    const auto selected=job.directory/media::pathFromUtf8(job.nameTemplate+".wav");
    const auto folded=job.directory/"STRASSE-REPLACE.WAV";
    write(selected,"old folded bytes");
    // A volume that folds names natively (macOS APFS/HFS+ case-insensitive, the
    // Mac default, folds even the Strasse expansion) has ONE file under both
    // spellings: a second fold-only file cannot exist there. Each check below
    // asserts the outcome for the volume the test actually runs on.
    const bool foldAliases=fs::exists(folded);
    std::printf("Export names: Replace fold spellings alias natively %s\n",foldAliases?"yes":"no");
    bool introduced=false;Control foldedRace;
    foldedRace.progress=[&](JobId,std::uint64_t done,std::uint64_t) {
        if(done && !introduced){introduced=true;write(folded,"new folded occupant");}
    };
    const auto collision=renderWav(job,{},foldedRace);
    if(!foldAliases)
        check(introduced && collision.state==JobState::Failed && !collision.publication &&
              text(selected)=="old folded bytes" && text(folded)=="new folded occupant" && f.noStage(),
              "a new portable-fold collision during render preserves both files");
    else // The late write through the folded spelling edits the bound file in place.
        check(introduced && collision.state==JobState::Failed && !collision.publication &&
              text(selected)=="new folded occupant" && f.noStage(),
              "a native-alias write during render is an in-place edit that Replace refuses, keeping its bytes");
    const auto ambiguous=renderWav(job,{});
    if(!foldAliases)
        check(ambiguous.state==JobState::Failed && ambiguous.framesWritten==0 && !ambiguous.publication,
              "Replace refuses an ambiguous preflight collision set before audio processing");
    else // One directory entry, the bound file itself: nothing is ambiguous.
        check(ambiguous.state==JobState::Completed && matches(read(ambiguous,866),137) && f.noStage(),
              "on a natively folding volume the folded spelling is the selected file, so Replace proceeds");
    if(!foldAliases) {
        fs::remove(folded);job.nameTemplate="STRASSE-REPLACE";
        const auto foldOnly=renderWav(job,{});
        check(foldOnly.state==JobState::Failed && foldOnly.framesWritten==0 && text(selected)=="old folded bytes",
              "Replace does not silently select a different fold-only spelling");
    } else {
        // Removing `folded` would remove the selected file. The other spelling
        // names that same file natively, so (as with replace-native-case below)
        // native identity proves it is the requested file and Replace proceeds.
        job.nameTemplate="STRASSE-REPLACE";
        const auto foldOnly=renderWav(job,{});
        check(foldOnly.state==JobState::Completed && matches(read(foldOnly,866),137) && f.noStage(),
              "a fold spelling the volume resolves natively to the same file is replaceable, as a case spelling is");
        write(selected,"old folded bytes"); // The hardlink check below starts from these bytes.
    }
    job.nameTemplate="directory-replace";fs::create_directory(job.directory/"directory-replace.wav");
    const auto directory=renderWav(job,{});
    check(directory.state==JobState::Failed && directory.framesWritten==0 &&
          fs::is_directory(job.directory/"directory-replace.wav"),"Replace refuses a directory without touching it");
    job.nameTemplate="hardlink-replace";
    const auto alias=job.directory/"hardlink-replace.wav";
    fs::create_hard_link(selected,alias);
    const auto linked=renderWav(job,{});
    check(linked.state==JobState::Completed && matches(read(linked,866),137) && text(selected)=="old folded bytes",
          "replacing one ordinary hardlink leaves the unselected alias's old bytes intact");
    job.nameTemplate="replace-native-case";
    const auto cased=job.directory/"REPLACE-NATIVE-CASE.wav";write(cased,"case bytes");
    const bool nativeAlias=fs::exists(job.directory/"replace-native-case.wav");
    const auto caseResult=renderWav(job,{});
    check(nativeAlias ? caseResult.state==JobState::Completed && matches(read(caseResult,866),137) :
          caseResult.state==JobState::Failed && text(cased)=="case bytes",
          "case spelling is replaceable only when native identity proves it is the requested file");
    job.nameTemplate="locked-replace";const auto locked=job.directory/"locked-replace.wav";write(locked,"leased bytes");
    auto lease=media::ProjectFileLease::openShared(locked,error);
    const auto inUse=renderWav(job,{});
    check(lease && inUse.state==JobState::Failed && !inUse.publication && text(locked)=="leased bytes" && f.noStage(),
          "active shared lease refuses Replace without deleting the old delivery");lease.reset();
    Control refusal;refusal.publicationHooks.rename=refuseRename;
    const auto unsupported=renderWav(job,{},refusal);
    check(unsupported.state==JobState::Failed && !unsupported.publication && text(locked)=="leased bytes" && f.noStage(),
          "unsupported native replacement fails without copy fallback or renaming the delivery");
    Control warning;warning.publicationHooks.syncDirectory=failDirectorySync;
    const auto warned=renderWav(job,{},warning);
    check(warned.state==JobState::Completed && warned.publication && !warned.publication->durabilityConfirmed &&
          !warned.publication->warning.empty() && matches(read(warned,866),137),
          "Replace keeps published-with-durability-warning distinct from failure");
    job.nameTemplate="absent-replace-race";Control arrival;
    arrival.publicationHooks.rename=[](const fs::path& source,const fs::path& path) {
        {std::ofstream file(path,std::ios::binary);file<<"late arrival";}
        return nativeExclusiveRename(source,path);
    };
    const auto raced=renderWav(job,{},arrival);
    check(raced.state==JobState::Failed && !raced.publication && text(raced.output)=="late arrival" && f.noStage(),
          "absent Replace never adopts a late native arrival as an overwrite target");
    job.nameTemplate="cancel-replace";const auto cancelledPath=job.directory/"cancel-replace.wav";
    write(cancelledPath,"kept on cancel");std::stop_source stop;Control cancel;cancel.stop=stop.get_token();
    cancel.progress=[&](JobId,std::uint64_t done,std::uint64_t){if(done)stop.request_stop();};
    const auto cancelled=renderWav(job,{},cancel);
    check(cancelled.state==JobState::Cancelled && !cancelled.publication && text(cancelledPath)=="kept on cancel" && f.noStage(),
          "cancelled Replace preserves the preflight target and removes only its private stage");
}
void stageCleanupResults() {
#if defined(_WIN32)
    for (const bool cancel : {false, true}) {
        Fixture f;
        const auto job = f.job("cleanup");
        struct GuardFile {
            HANDLE handle = INVALID_HANDLE_VALUE;
            ~GuardFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } guard;
        fs::path guardedDirectory;
        Control control;
        std::stop_source stop;
        control.stop = stop.get_token();
        control.progress = [&](JobId, std::uint64_t, std::uint64_t) {
            if (guard.handle != INVALID_HANDLE_VALUE) return;
            for (const auto& entry : fs::directory_iterator(job.directory)) {
                if (!entry.path().filename().string().starts_with(".adi-render-")) continue;
                guardedDirectory = entry.path();
                guard.handle = CreateFileW((guardedDirectory / L"guard").c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (guard.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot reserve the stage guard");
                break;
            }
            if (cancel) stop.request_stop();
        };
        Queue queue;
        queue.add(job);
        queue.run({}, control);
        const auto result = queue.snapshot().front().result;
        check(guard.handle != INVALID_HANDLE_VALUE && result.stageCleanup &&
              result.stageCleanup->directory == guardedDirectory && result.stageCleanup->error &&
              fs::is_directory(guardedDirectory), "a real Windows sharing refusal reports the private residue");
        check(result.error.empty() && (cancel
                ? result.state == JobState::Cancelled && !fs::exists(result.output)
                : result.state == JobState::Completed && result.publication && matches(read(result,866),137)),
              "stage cleanup failure preserves the cancellation or completed WAV result");
        const auto manifest = nlohmann::json::parse(serializeRunManifest(queue.lastRun()));
        const auto& cleanup = manifest.at("members").at(0).at("result").at("stage_cleanup");
        check(cleanup.is_object() && cleanup.at("directory") == media::pathUtf8(guardedDirectory) &&
              cleanup.at("error_code").get<int>() != 0 && !cleanup.at("error").get<std::string>().empty(),
              "run JSON keeps the cleanup location and native failure separately");
    }
#endif
}

void publicationResults() {
    Fixture f;
    Control control;
    control.publicationHooks.syncDirectory = failDirectorySync;
    Queue queue;
    queue.add(f.job("sync-warning"));
    queue.run({}, control);
    const auto result = queue.snapshot().front().result;
    check(result.state==JobState::Completed && result.error.empty() && result.publication &&
          !result.publication->durabilityConfirmed && !result.publication->usedFallback &&
          result.publication->warning.find("directory sync")!=std::string::npos,
          "post-publication sync error is a completed WAV with warning, not a failed job");
    check(matches(read(result,866),137) && f.noStage() && !result.stageCleanup,
          "sync warning preserves fully published audio and cleans stage");
    const auto manifest=nlohmann::json::parse(serializeRunManifest(queue.lastRun()));
    const auto& member=manifest["members"][0];
    check(member["state"]=="completed" && member["result"]["publication"]["warning"]==result.publication->warning &&
          member["result"]["publication"]["durability_confirmed"]==false &&
          member["result"]["publication"]["used_fallback"]==false &&
          member["result"]["output"]==media::pathUtf8(result.output),
          "run JSON retains exact warning, durability, fallback and output path");
    check(!queue.retry(queue.snapshot().front().id),"warning does not make a committed job retryable");
    control.publicationHooks.syncDirectory=confirmDirectorySync;
    const auto confirmed=renderWav(f.job("sync-confirmed"),{},control);
    check(confirmed.state==JobState::Completed && confirmed.publication && confirmed.publication->durabilityConfirmed &&
          confirmed.publication->warning.empty(),"successful injected directory sync confirms publication durability");
    control.publicationHooks.rename=refuseRename;
    const auto refused=renderWav(f.job("atomic-refused"),{},control);
    check(refused.state==JobState::Failed && !refused.publication &&
          refused.error.find("atomic publication:")==0,
          "authoritative render refuses rename failure without a fallback success report");
    check(!fs::exists(refused.output) && f.noStage(),
          "atomic-only refusal publishes no copy and cleans the private stage");
    const auto collision=renderWav(f.job("sync-warning"),{},control);
    check(collision.state==JobState::Failed && !collision.publication && matches(read(result,866),137),
          "no successful publication is distinct from published-but-unconfirmed");
    auto invalid=queue.lastRun();
    invalid.members[0].job->nameTemplate=std::string(1,static_cast<char>(0xff));
    bool threw=false;
    try { (void)serializeRunManifest(invalid); } catch(const nlohmann::json::type_error&) { threw=true; }
    check(threw && queue.snapshot().front().result.state==JobState::Completed && fs::exists(result.output),
          "caller serialization failure cannot change committed queue state or delete output");
    std::ofstream failedReport(f.dir.path()/"missing-parent"/"report.json");
    check(!failedReport && queue.snapshot().front().result.state==JobState::Completed,
          "caller-owned manifest persistence failure is separate from render success");
}
void runManifests() {
    Fixture f;
    Queue queue;
    const auto first=queue.add(f.job("run-{counter}"));
    const auto second=queue.add(f.job("run-{counter}"));
    const auto removed=queue.add(f.job("run-{counter}"));
    bool edited=false;
    Control control;
    control.progress=[&](JobId id,std::uint64_t,std::uint64_t) {
        if (edited || id!=first) return;
        edited=true;
        auto changed=f.job("edited-{counter}"); changed.beginSample=200; changed.endSample=333;
        check(queue.update(second,changed) && queue.remove(removed),"manifest fixture edits/removes pending members");
        queue.add(f.job("later-{counter}"));
    };
    queue.run({},control);
    const auto retained=queue.lastRun();
    const auto doc=nlohmann::json::parse(serializeRunManifest(retained));
    check(doc["capture_complete"]==true && doc["planned_count"]==3 && doc["members"].size()==3,
          "manifest freezes original membership; callback-added job is excluded");
    check(doc["members"][1]["job_capture"]=="job_start" &&
          doc["members"][1]["job"]["requested_range"]["begin_sample"]==200 &&
          doc["members"][1]["result"]["frames_written"]==133,
          "manifest takes edited job values when that member actually starts");
    check(doc["members"][2]["state"]=="unstarted" && doc["members"][2]["removed_before_start"]==true &&
          doc["members"][2]["result"].is_null(),"removed pending member remains unstarted, without a fabricated result");
    check(doc["members"][0]["journal_head"]==0 && doc["members"][0]["media_identities"].size()==1 &&
          doc["members"][0]["result"]["resolved_snapshot"]["journal_head"]==0 &&
          doc["members"][0]["job"]["requested_profile"]["container"]=="wav" &&
          doc["members"][0]["job"]["requested_profile"]["sample_rate"]==48000,
          "manifest records captured revision zero and measured external clip identities");
    queue.run();
    check(queue.lastRun().plannedCount==1 && queue.lastRun().members.front().id==4 &&
          retained.members.size()==3 && retained.members.front().state==JobState::Completed,
          "next run includes only deferred pending job; caller's old report stays independent");
    Queue cancelled;
    const auto cancelledId=cancelled.add(f.job("cancel-{counter}"));
    cancelled.add(f.job("cancel-{counter}"));
    std::stop_source stop;
    control={}; control.stop=stop.get_token();
    control.progress=[&](JobId,std::uint64_t,std::uint64_t) { stop.request_stop(); };
    cancelled.run({},control);
    const auto cancelledDoc=nlohmann::json::parse(serializeRunManifest(cancelled.lastRun()));
    check(cancelledDoc["members"][0]["state"]=="cancelled" &&
          cancelledDoc["members"][0]["result"]["publication"].is_null() &&
          cancelledDoc["members"][1]["state"]=="unstarted" &&
          cancelled.snapshot()[1].result.state==JobState::Pending,
          "cancelled attempted work and unstarted pending work are distinct in JSON and queue");
    check(cancelled.retry(cancelledId),"run reporting preserves explicit cancelled retry");
    const auto old=serializeRunManifest(cancelled.lastRun());
    cancelled.run();
    check(cancelled.lastRun().members.size()==2 && cancelled.lastRun().members[0].state==JobState::Completed &&
          nlohmann::json::parse(old)["members"][0]["state"]=="cancelled", "retry starts a new report without rewriting old outcome");
    Queue failed;
    auto missing=f.job("missing"); missing.project=f.dir.path()/"absent.adi";
    failed.add(missing); failed.add(f.job("after-missing")); failed.run();
    const auto failedDoc=nlohmann::json::parse(serializeRunManifest(failed.lastRun()));
    check(failedDoc["members"][0]["state"]=="failed" && !failedDoc["members"][0]["result"]["error"].get<std::string>().empty() &&
          failedDoc["members"][0]["journal_head"].is_null() &&
          failedDoc["members"][0]["result"]["resolved_snapshot"].is_null() &&
          failedDoc["members"][1]["state"]=="completed", "failed open has unavailable revision and does not hide later completed work");
    RunReport incomplete; incomplete.plannedCount=2; incomplete.captureComplete=false;
    const auto incompleteDoc=nlohmann::json::parse(serializeRunManifest(incomplete));
    check(incompleteDoc["planned_count"]==2 && incompleteDoc["capture_complete"]==false && incompleteDoc["members"].empty(),
          "incomplete capture is explicit, never an apparently complete empty run");
}
void mediaIdentities() {
    {
        // Synthetic wide IDs isolate truncation from whichever filesystem the
        // runner uses. Native aliases are exercised separately below.
        const media::FileIdentity low{0x100000002ULL,41,0,media::FileIdentityScheme::Windows128};
        auto high=low;high.fileHigh=0x8000000000000001ULL;
        auto volume=high;volume.volume+=0x100000000ULL;
        auto legacy=low;legacy.scheme=media::FileIdentityScheme::WindowsLegacy64;
        auto posix=low;posix.scheme=media::FileIdentityScheme::Posix;
        const std::map<media::FileIdentity,unsigned> keys{{low,1},{high,2},{volume,3},{legacy,4},{posix,5}};
        check(keys.size()==5 && high!=low && keys.at(high)==2 && keys.at(volume)==3,
              "native identity ordering includes both file halves, full volume and query namespace");
        RunReport run;run.plannedCount=1;RunMember member;member.result.emplace();
        member.result->mediaVerification.emplace();MediaIdentity receipt;receipt.nativeIdentity=high;
        member.result->mediaVerification->identities.push_back(receipt);run.members.push_back(std::move(member));
        const auto doc=nlohmann::json::parse(serializeRunManifest(run));
        const auto& saved=doc["members"][0]["media_identities"][0]["native_identity"];
        check(saved["volume"]==high.volume && saved["file"]==high.file && saved["file_high"]==high.fileHigh &&
              saved["scheme"]=="windows_file_id_128",
              "run manifest preserves synthetic nonzero high file bits and upper volume bits");
    }
    {
        Fixture f;
        const auto path=f.dir.path()/"audio.wav";
        std::error_code error;
        const auto identity=media::readFileIdentity(path,error);
        check(identity.has_value() && !error,"native source identity reader returns a regular file key");
        {
            const auto lease=media::ProjectFileLease::openShared(path,error);
            check(identity && lease && lease->identity()==*identity,"project lease and export reader use the same complete native identity");
        }
#if defined(_WIN32)
        const HANDLE handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("open direct identity fixture");
        struct CloseIdentityHandle{HANDLE handle;~CloseIdentityHandle(){CloseHandle(handle);}} close{handle};
        FILE_ID_INFO native{};const bool wide=GetFileInformationByHandleEx(handle,FileIdInfo,&native,sizeof(native))!=0;
        wchar_t filesystem[32]{};
        if(GetVolumeInformationByHandleW(handle,nullptr,0,nullptr,nullptr,nullptr,filesystem,32) && std::wstring_view(filesystem)==L"NTFS")
            check(wide,"actual NTFS fixture exposes FileIdInfo for alias identity checks");
        if(wide){
            bool equal=identity && identity->scheme==media::FileIdentityScheme::Windows128 && identity->volume==native.VolumeSerialNumber;
            for(unsigned i=0;i<8 && equal;++i)
                equal=static_cast<unsigned char>(identity->file>>(8*i))==native.FileId.Identifier[i] &&
                    static_cast<unsigned char>(identity->fileHigh>>(8*i))==native.FileId.Identifier[i+8];
            check(equal,"native reader preserves all sixteen FileIdInfo bytes and its 64-bit volume");
        }else std::printf("SKIP: direct 128-bit file identity unavailable on fixture volume\n");
#endif
        const auto missing=media::readFileIdentity(f.dir.path()/"missing.wav",error);
        check(!missing && error,"native source identity reader preserves a missing-path error");
        const auto directory=media::readFileIdentity(f.dir.path(),error);
        check(!directory && error,"native source identity reader refuses directories");
        std::optional<media::FileIdentity> refused;
        {audio::CallbackScope callback;refused=media::readFileIdentity(path,error);}
        check(!refused && error==std::errc::operation_not_permitted,
              "native source identity reader refuses callback filesystem access");
    }
    {
        Fixture f;
        const auto path=f.dir.path()/"audio.wav";
        const auto hash=media::blake3File(path);
        if(!hash)throw std::runtime_error("fixture hash");
        // A canonical stored hash is compared with actual bytes, including a
        // legacy uppercase spelling. Other fixtures retain their 'test' label.
        auto upper=hash.hex;
        for(auto& c:upper)if(c>='a' && c<='f')c=static_cast<char>(c-'a'+'A');
        SQLite::Statement update(f.store->db(),"UPDATE media_files SET hash_blake3=? WHERE id=1");
        update.bind(1,upper); update.exec();
        Queue queue; queue.add(f.job("verified-media")); queue.run();
        const auto result=queue.snapshot().front().result;
        check(result.state==JobState::Completed && result.mediaVerification &&
              result.mediaVerification->startComplete && result.mediaVerification->publicationVerified,
              "unchanged imported media verifies before playback and publication");
        const auto& identity=result.mediaVerification.value().identities.at(0);
        check(fs::equivalent(identity.path,path) && identity.blake3AtStart==hash.hex &&
              identity.blake3BeforePublication==hash.hex && identity.references.size()==1 &&
              identity.references[0].matchesStored==true,
              "measured bytes confirm a comparable stored BLAKE3 without trusting it blindly");
        const auto doc=nlohmann::json::parse(serializeRunManifest(queue.lastRun()));
        check(doc["members"][0]["media_identities"][0]["blake3_at_start"]==hash.hex &&
              doc["members"][0]["media_identities"][0]["native_identity"]["volume"]==identity.nativeIdentity.volume &&
              doc["members"][0]["media_identities"][0]["native_identity"]["file"]==identity.nativeIdentity.file &&
              doc["members"][0]["media_identities"][0]["native_identity"]["file_high"]==identity.nativeIdentity.fileHigh &&
              doc["members"][0]["media_identities"][0]["native_identity"].contains("scheme") &&
              doc["members"][0]["media_identities"][0]["blake3_before_publication"]==hash.hex &&
              doc["members"][0]["media_identities"][0]["references"][0]["matches_stored"]==true &&
              doc["members"][0]["result"]["media_verification"]["scope"]=="external_audio_clips",
              "run JSON exposes verified hashes and the bounded source scope");
    }
    {
        Fixture f;
        f.store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES"
            "(2,'legacy-alias','./audio.wav','wav'),(3,'unused','missing.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,muted) VALUES"
            "(2,1,'audio',1,0,1000000000,0),(3,1,'audio',1,0,1000000000,0),(4,1,'audio',1,0,1000000000,1);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES"
            "(2,1,0,4096),(3,2,0,4096),(4,3,0,4096);");
        const auto result=renderWav(f.job("deduplicated-media"),{});
        check(result.state==JobState::Completed && result.mediaVerification &&
              result.mediaVerification->identities.size()==1 && result.mediaVerification->publicationVerified,
              "multiple clips and canonical path aliases use one file identity per verification pass");
        const auto& refs=result.mediaVerification.value().identities.at(0).references;
        check(refs.size()==3 && std::all_of(refs.begin(),refs.end(),[](const auto& ref){return !ref.matchesStored.has_value();}),
              "legacy labels use measured hashes and unopened muted clips are excluded");
    }
    {
        Fixture f;
        const auto original=f.dir.path()/"audio.wav", alias=f.dir.path()/"hardlink.wav", copy=f.dir.path()/"copy.wav";
        std::error_code error;
        fs::create_hard_link(original,alias,error);
        if(error)std::printf("SKIP: source hardlink aliases unavailable (%s)\n",error.message().c_str());
        else {
            fs::copy_file(original,copy);
            const auto originalId=media::readFileIdentity(original,error);
            const auto aliasId=media::readFileIdentity(alias,error);
            const auto copyId=media::readFileIdentity(copy,error);
            check(originalId && aliasId==originalId && copyId && copyId!=originalId,
                  "native identity joins hardlinks but distinguishes an identical-byte copy");
            f.store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES"
                "(2,'legacy-hardlink','hardlink.wav','wav'),(3,'legacy-copy','copy.wav','wav');"
                "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES"
                "(2,1,'audio',1,0,1000000000),(3,1,'audio',1,0,1000000000);"
                "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES"
                "(2,2,0,4096),(3,3,0,4096);");
            const auto result=renderWav(f.job("native-aliases"),{});
            check(result.state==JobState::Completed && result.mediaVerification &&
                  result.mediaVerification->publicationVerified && result.mediaVerification->identities.size()==2,
                  "keyed native dedup hashes a hardlink pair once and keeps a separate identical-byte file");
            const auto& identities=result.mediaVerification.value().identities;
            const auto links=std::find_if(identities.begin(),identities.end(),[&](const auto& entry) {
                return originalId && entry.nativeIdentity==*originalId;
            });
            check(links!=identities.end() && links->references.size()==2 &&
                  identities[0].blake3AtStart==identities[1].blake3AtStart,
                  "all hardlink references survive dedup without using content hash as the file key");
        }
    }
    {
        Fixture f;
        const auto original=f.dir.path()/"audio.wav", alias=f.dir.path()/"symbolic.wav", copy=f.dir.path()/"copy.wav";
        std::error_code error;
        fs::create_symlink(original,alias,error);
        if(error)std::printf("SKIP: source symlink rebinding unavailable (%s)\n",error.message().c_str());
        else {
            fs::copy_file(original,copy);
            f.store->db().exec("UPDATE media_files SET rel_path='symbolic.wav' WHERE id=1");
            bool rebound=false;
            Control control;
            control.progress=[&](JobId,std::uint64_t done,std::uint64_t) {
                if(rebound || done==0)return;
                fs::remove(alias);fs::create_symlink(copy,alias);rebound=true;
            };
            const auto result=renderWav(f.job("rebound-source"),{},control);
            check(rebound && result.state==JobState::Failed && !result.publication &&
                  result.error.find("export media path changed during render:")==0 &&
                  !fs::exists(result.output) && f.noStage(),
                  "same-byte symlink rebind fails against captured native identity before publication");
            const auto& identity=result.mediaVerification.value().identities.at(0);
            check(identity.blake3BeforePublication==identity.blake3AtStart &&
                  !result.mediaVerification->publicationVerified && fs::exists(original) && fs::exists(copy),
                  "unchanged representative digest cannot hide a rebound selected path or remove sources");
        }
    }
    {
        Fixture f;
        const auto path=f.dir.path()/"audio.wav";
        const auto before=media::blake3File(path).hex;
        const auto bytes=fs::file_size(path);
        bool rewritten=false;
        Control control;
        control.progress=[&](JobId,std::uint64_t done,std::uint64_t) {
            if(rewritten || done==0)return;
            std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
            if(!file)throw std::runtime_error("fixture rewrite open");
            const auto offset=static_cast<std::streamoff>(bytes-1);
            char value=0; file.seekg(offset); file.read(&value,1);
            value=static_cast<char>(static_cast<unsigned char>(value)^1u);
            file.seekp(offset); file.write(&value,1); file.flush();
            if(!file)throw std::runtime_error("fixture rewrite failed");
            rewritten=true;
        };
        Queue queue;queue.add(f.job("changed-media"));queue.run({},control);
        const auto result=queue.snapshot().front().result;
        const auto after=media::blake3File(path).hex;
        check(rewritten && before!=after && fs::file_size(path)==bytes &&
              result.state==JobState::Failed && result.error.find("export media changed during render:")==0 &&
              !result.publication && !fs::exists(result.output) && f.noStage(),
              "real same-size source rewrite fails before publication and preserves the changed source");
        const auto& verification=result.mediaVerification.value();
        check(verification.startComplete && !verification.publicationVerified &&
              verification.identities.at(0).blake3AtStart==before &&
              verification.identities.at(0).blake3BeforePublication==after && result.framesWritten==866,
              "content recheck catches an out-of-range byte edit after complete staged audio, independent of size");
        const auto doc=nlohmann::json::parse(serializeRunManifest(queue.lastRun()));
        check(doc["members"][0]["state"]=="failed" &&
              doc["members"][0]["result"]["media_verification"]["publication_verified"]==false &&
              doc["members"][0]["media_identities"][0]["blake3_before_publication"]==after,
              "failed run JSON retains the changed measured hash without claiming verified publication");
    }
    {
        Fixture f;
        const auto original=f.dir.path()/"audio.wav", alternate=f.dir.path()/"alternate.wav";
        audio::WavWriter writer(alternate,48000,2,audio::WavFormat::Float32);
        std::vector<float> silence(8192,0);writer.write(silence.data(),4096);writer.close();
        const auto alternateHash=media::blake3File(alternate).hex;
        SQLite::Statement update(f.store->db(),"UPDATE media_files SET hash_blake3=?,abs_path_hint=? WHERE id=1");
        update.bind(1,alternateHash);update.bind(2,media::pathUtf8(alternate));update.exec();
        const auto result=renderWav(f.job("stored-mismatch"),{});
        check(result.state==JobState::Failed && result.framesWritten==0 && !result.publication &&
              result.error.find("export media differs from stored BLAKE3:")==0 && !fs::exists(result.output),
              "valid stored digest mismatch refuses before audio instead of silently trusting the row");
        const auto& verification=result.mediaVerification.value();
        check(!verification.startComplete && fs::equivalent(verification.identities.at(0).path,original) &&
              verification.identities[0].references.at(0).matchesStored==false,
              "identity follows ClipPlayback's actual relative source, not a second resolver's alternate hint");
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--export-speed-only") largeSongExportSpeed();
        else {ranges(); precisionAndIsolation(); sampleRateConversion(); cancellationAndFailures(); missingSources(); queueEdits(); latencyChanges(); coherentRevision(); queuedRevisionRegions(); revisionAfterUndo(); directStems(); groupStems(); multichannelStem(); regionStems(); returnStem(); invalidStemSources(); pcmExports(); aiffExports(); hostOwnerLifecycle(); normalizedRenders(); namesAndValidation(); finalNamesAndCaseCollisions(); singleWavDestination(); collisionModes(); foldedCollisionNames(); identityBoundReplace(); publicationResults(); stageCleanupResults(); runManifests(); mediaIdentities();}
    }
    catch(const std::exception& e) {check(false,e.what());}
    std::printf("%s -- %d checks, %d failure(s)\n",failures ? "FAIL" : "PASS",checks,failures);
    return failures ? 1 : 0;
}
