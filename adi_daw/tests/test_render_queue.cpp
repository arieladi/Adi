// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/render/queue.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>

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
float source(std::int64_t frame, int channel) {
    return static_cast<float>((frame % 191) - 95) / 128.0f + static_cast<float>(channel) / 16.0f;
}
struct Fixture {
    test::TempDirectory dir{"render_queue","project"};
    std::unique_ptr<Store> store;
    Fixture() {
        StoreError error{};
        store = Store::create(dir.path()/"project.adi", error);
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
            if (p.path().filename().string().starts_with(".adi-render-")) return false;
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
std::vector<float> read(const Result& result, std::uint64_t frames, std::uint32_t rate = 48000) {
    check(result.state==JobState::Completed, result.error.empty() ? "job completed" : result.error.c_str());
    if (result.state!=JobState::Completed) return {};
    audio::WavReader wave(result.output);
    check(wave.frames()==frames && wave.channels()==2 && wave.sampleRate()==rate && wave.format()==audio::WavFormat::Float32,
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
    check(collision.state==JobState::Failed && kept=="won race" && f.noStage(),"publication refuses a racing destination and cleans only its own stage");
    j.nameTemplate="callback-failure";
    Control broken;
    broken.progress=[](JobId,std::uint64_t done,std::uint64_t) {if(done>0) throw std::runtime_error("fixture failure");};
    const auto failed=renderMasterWav(j,{},broken);
    check(failed.state==JobState::Failed && failed.error=="fixture failure" && !fs::exists(j.directory/"callback-failure.wav") && f.noStage(),
          "callback failure produces no partial published output");
    f.effect();
    j.nameTemplate="missing";
    check(renderMasterWav(j,{}).state==JobState::Failed,"missing plugin fails rather than exporting a placeholder");
    for (const double gain : {std::numeric_limits<double>::infinity(),1e100}) {
        j.nameTemplate="nonfinite"; j.session.precision=Precision::Float64;
        const auto bad=renderMasterWav(j,[gain](const DeviceRequest&,std::string&) {auto d=std::make_unique<Delay>(); d->gain=gain; return d;});
        check(bad.state==JobState::Failed && !fs::exists(j.directory/"nonfinite.wav") && f.noStage(),"nonfinite and finite-double overflow cannot reach Float32 files");
    }
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
    auto job=f.job("{project}-{bpm}");
    const auto result=renderMasterWav(job,[&](const DeviceRequest&,std::string&) {
        // Another open connection commits while the renderer loads devices,
        // between its row projection and its filename/tempo projection.
        f.store->db().exec("UPDATE project SET name='Later'; UPDATE tempo_map SET bpm=75; UPDATE tracks SET muted=1 WHERE id=1;");
        return std::make_unique<Delay>();
    });
    check(result.output.filename()=="Before-120.0.wav" && matches(read(result,866),137),
          "concurrent committed edits cannot mix project revisions within one job");
    const auto next=renderMasterWav(job,[](const DeviceRequest&,std::string&) {return std::make_unique<Delay>();});
    const auto samples=read(next,866);
    check(next.output.filename()=="Later-75.0.wav" && !samples.empty() &&
          std::all_of(samples.begin(),samples.end(),[](float x) {return x==0;}),
          "the next job sees committed metadata and mute changes together");
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
void namesAndValidation() {
    NameContext c{"../A{track}","B/C","G","90",3};
    check(resolveName("{project}-{track}-{group}-{bpm}-{counter}",c)==".._A{track}-B_C-G-90-3","metadata cannot inject paths or more placeholders");
    check(resolveName("{{literal}}",c)=="{literal}" && resolveName("CON",c)=="_CON","literal braces and Windows reserved names are portable");
    check(resolveName("COM\xC2\xB9.wav",c)=="_COM\xC2\xB9.wav" && resolveName("lpt\xC2\xB2",c)=="_lpt\xC2\xB2",
          "Windows superscript device names are escaped, including names with extensions");
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
} // namespace
int main() {
    try {ranges(); precisionAndIsolation(); sampleRateConversion(); cancellationAndFailures(); queueEdits(); latencyChanges(); coherentRevision(); namesAndValidation();}
    catch(const std::exception& e) {check(false,e.what());}
    std::printf("%s -- %d checks, %d failure(s)\n",failures ? "FAIL" : "PASS",checks,failures);
    return failures ? 1 : 0;
}
