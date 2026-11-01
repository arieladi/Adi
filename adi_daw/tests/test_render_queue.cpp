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
void addDevice(Fixture& f,int track,int id) {
    f.store->db().exec("INSERT INTO device_chains(id,track_id) VALUES("+std::to_string(id)+","+std::to_string(track)+");"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES("+std::to_string(id)+","+std::to_string(id)+",0,1,'Effect');");
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
    try {ranges(); precisionAndIsolation(); sampleRateConversion(); cancellationAndFailures(); queueEdits(); latencyChanges(); coherentRevision(); directStems(); groupStems(); multichannelStem(); regionStems(); returnStem(); invalidStemSources(); namesAndValidation();}
    catch(const std::exception& e) {check(false,e.what());}
    std::printf("%s -- %d checks, %d failure(s)\n",failures ? "FAIL" : "PASS",checks,failures);
    return failures ? 1 : 0;
}
