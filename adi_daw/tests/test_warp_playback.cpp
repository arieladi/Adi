// SPDX-License-Identifier: AGPL-3.0-or-later
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"
#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/warp_projection.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "adi/clip_commands.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/edit_plan.hpp"
#include "adi/textproj_store.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <numbers>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>
namespace {std::atomic<unsigned> callbackAllocations{0};}
void adi::test::countReplacedNew() noexcept{if(adi::audio::onAudioThread)++callbackAllocations;}
namespace {
using namespace adi;using namespace adi::engine;constexpr std::int64_t q=textproj::kPPQ;
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
struct Fixture {
    test::TempDirectory temporary{"warp_playback","project"};std::unique_ptr<Store> store;Session session;
    explicit Fixture(std::uint32_t sourceRate=48000) {
        StoreError error{};store=Store::createEmpty(temporary.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        const auto frames=sourceRate/4;std::vector<float> input(static_cast<std::size_t>(frames)*2);
        for(std::uint32_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<2;++channel)
            input[static_cast<std::size_t>(frame)*2+channel]=static_cast<float>(0.25*std::sin(2*std::numbers::pi*(channel==0?440:660)*frame/sourceRate));
        {audio::WavWriter writer(temporary.path()/"source.wav",sourceRate,2,audio::WavFormat::Float32);writer.write(input.data(),frames);writer.close();}
        auto& db=store->db();
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Warp'),(9,'master','Main');"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'warp-source','source.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(7,1,'audio',0,5765760)");
        {SQLite::Statement row(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode) VALUES(7,1,?,1,'complex')");row.bind(1,frames);row.exec();}
        const auto markers=audio::writeWarpMap(std::vector<WarpMarker>{{0,0},{frames,q}},frames);
        {SQLite::Statement row(db,"UPDATE audio_clips SET warp_markers=? WHERE clip_id=7");row.bind(1,markers.bytes.data(),static_cast<int>(markers.bytes.size()));row.exec();}
        db.exec("INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns,loop_begin_ns,loop_end_ns) VALUES(7,0,250000000,0,250000000)");
    }
    void load(Precision width) {
        SessionSpec spec{2,48000,1024};spec.precision=width;session.graph().setFadeFrames(0);
        if(!session.load(*store,{},spec))throw std::runtime_error(session.error());
        session.transport().locate(0);session.transport().play();
    }
    std::vector<double> render(std::size_t frames,Precision width) {
        std::vector<double> result(frames*2);std::array<float,2048> narrow{};std::array<double,2048> wide{};
        float* out[]={narrow.data(),narrow.data()+1024};double* out64[]={wide.data(),wide.data()+1024};
        // Compare Arrangement frames at their compensated output positions,
        // including when a seek or play starts the source delay pipeline.
        const auto* graph=session.graph().currentGraph();
        const auto delay=graph?graph->latencySamples():0;
        const auto preroll=static_cast<std::size_t>(std::max<std::int64_t>(0,delay-session.transport().streamedFrames()));
        const auto rawFrames=frames+preroll;
        for(std::size_t first=0;first<rawFrames;) {
            const auto count=static_cast<std::int32_t>(std::min<std::size_t>(1024,rawFrames-first));
            if(!session.clips()->prime(std::chrono::seconds(5)))throw std::runtime_error("prime warped project");
            AudioIo io;io.frames=count;io.numOut=2;if(width==Precision::Float64)io.out64=out64;else io.out=out;
            {audio::CallbackScope scope;session.process(io);}
            for(std::size_t channel=0;channel<2;++channel)for(std::int32_t frame=0;frame<count;++frame) {
                const auto position=first+static_cast<std::size_t>(frame);
                if(position>=preroll)
                    result[(position-preroll)*2+channel]=width==Precision::Float64?wide[channel*1024+static_cast<std::size_t>(frame)]:narrow[channel*1024+static_cast<std::size_t>(frame)];
            }
            first+=static_cast<std::size_t>(count);
        }
        return result;
    }
};
void modesAndLoops() {
    for(const auto width:{Precision::Float32,Precision::Float64})for(const auto* mode:{"beats","tones","texture","re-pitch","complex","complexPro"}) {
        Fixture f;
        {SQLite::Statement row(f.store->db(),"UPDATE audio_clips SET warp_mode=?");row.bind(1,mode);row.exec();}
        if(std::string_view(mode)=="re-pitch")
            f.store->db().exec("UPDATE audio_clips SET transpose_semis=7 WHERE clip_id=7");
        f.load(width);const auto output=f.render(26000,width);
        if(!f.session.problems().empty())std::printf("mode %s: %s\n",mode,f.session.problems().front().c_str());
        check(f.session.problems().empty(),"actual Session loads every stored warp mode at either native precision");
        if(std::string_view(mode)=="re-pitch")
            check(f.store->db().execAndGet("SELECT transpose_semis FROM audio_clips WHERE clip_id=7").getDouble()==7,
                  "Re-Pitch plays without discarding transposition stored for the other modes");
        double power=0;bool finite=true;
        for(std::size_t frame=18000;frame<22000;++frame){power+=output[frame*2]*output[frame*2];finite=finite && std::isfinite(output[frame*2]);}
        check(finite && power>1,"heard warped PCM continues beyond the old unwarped source-end pin");
        check(std::all_of(output.begin()+48000,output.end(),[](double value){return value==0;}) &&
              f.session.clips()->underrunSamples()==0,"warped clip ends at its projected timeline boundary without underruns");
        f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=0,loop_len_ticks=5765760,length_ticks=11531520");
        check(f.session.refresh(*f.store),"refresh a source musical loop on a warped clip");
        f.session.transport().locate(0);const auto looped=f.render(48000,width);
        check(std::equal(looped.begin(),looped.begin()+48000,looped.begin()+48000),"warped loop uses musical bounds rather than old physical loop pins");
    }
}
std::array<float,2048> hearWarpHead(Session& session) {
    session.transport().locate(0);session.transport().play();
    if(!session.clips()->prime(std::chrono::seconds(60)))throw std::runtime_error("prime warped audio");
    std::array<float,2048> samples{};
    float* channels[]={samples.data(),samples.data()+1024};
    AudioIo io;io.frames=1024;io.numOut=2;io.out=channels;
    {audio::CallbackScope scope;session.process(io);}
    return samples;
}
// Only the prepared-cache case redirects staging. All other test directories
// are claimed before or after this process-local override.
class ScopedWarpTemp {
    using Char = std::filesystem::path::value_type;
public:
    ScopedWarpTemp(const Char* name,const std::filesystem::path& root) : name_(name) {
#if defined(_WIN32)
        wchar_t* value = nullptr;
        std::size_t size = 0;
        if (_wdupenv_s(&value,&size,name_) != 0) throw std::runtime_error("read test Warp temp root");
        const std::unique_ptr<wchar_t,decltype(&std::free)> owned(value,std::free);
        if (owned) saved_ = owned.get();
        if (_wputenv_s(name_,root.c_str()) != 0) throw std::runtime_error("set test Warp temp root");
#else
        if (const auto* value = std::getenv(name_)) saved_ = value;
        if (setenv(name_,root.c_str(),1) != 0) throw std::runtime_error("set test Warp temp root");
#endif
    }
    ~ScopedWarpTemp() {
#if defined(_WIN32)
        (void)_wputenv_s(name_,saved_ ? saved_->c_str() : L"");
#else
        if (saved_) (void)setenv(name_,saved_->c_str(),1);
        else (void)unsetenv(name_);
#endif
    }
    ScopedWarpTemp(const ScopedWarpTemp&) = delete;
    ScopedWarpTemp& operator=(const ScopedWarpTemp&) = delete;

private:
    const Char* const name_;
    std::optional<std::filesystem::path::string_type> saved_;
};

void longWarpResponse(bool prepared = false) {
    constexpr std::uint32_t rate = 48000;
    const auto seconds = prepared ? 2u : 180u;
    const auto frames = rate * seconds;
    // Stretch 2 s to 6 s to require prepared Complex beyond the live R3 range.
    const auto length = q * (prepared ? 12 : 400);
    using Clock=std::chrono::steady_clock;
    test::TempDirectory temporary{"warp_playback",prepared ? "prepared_cache" : "long_beats"};
    const auto workerRoot = temporary.path() / "worker";
    const auto workFolder = workerRoot / "adi-warp";
    std::optional<ScopedWarpTemp> scopedTemp;
#if defined(_WIN32)
    std::optional<ScopedWarpTemp> scopedSystemTemp;
#endif
    if (prepared) {
        std::filesystem::create_directory(workerRoot);
#if defined(_WIN32)
        scopedTemp.emplace(L"TMP",workerRoot);
        // GetTempPath2 uses SystemTemp when the checker runs as SYSTEM.
        if (!std::filesystem::equivalent(workerRoot,std::filesystem::temp_directory_path()))
            scopedSystemTemp.emplace(L"SystemTemp",workerRoot);
#else
        scopedTemp.emplace("TMPDIR",workerRoot);
#endif
        if (!std::filesystem::equivalent(workerRoot,std::filesystem::temp_directory_path()))
            throw std::runtime_error("Warp test staging is outside its private temp folder");
    }
    const auto project=temporary.path()/"long.adi";
    const auto source=temporary.path()/"source.wav";
    std::vector<float> burst(rate*2);
    for(std::uint32_t frame=0;frame<rate;++frame) {
        // Distinct onsets over the whole sample give Beats material to align.
        const double envelope=(frame%(rate/2))<rate/8?0.25:0.0;
        burst[2*frame]=static_cast<float>(envelope*std::sin(2*std::numbers::pi*440*frame/rate));
        burst[2*frame+1]=static_cast<float>(envelope*std::sin(2*std::numbers::pi*660*frame/rate));
    }
    {
        audio::WavWriter writer(source,rate,2,audio::WavFormat::Float32);
        // The cold request must miss even if a previous test left a cache.
        const auto stamp=static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
        for(std::size_t sample=0;sample<8;++sample)
            burst[burst.size()-1-sample]=static_cast<float>((stamp>>(8*sample))&255u)/65536.0f;
        for(std::uint32_t second=0;second<seconds;++second)writer.write(burst.data(),rate);
        writer.close();
    }
    const auto digest=media::blake3File(source);
    if(!digest)throw std::runtime_error("hash long warp fixture");
    StoreError error{};
    auto store=Store::createEmpty(project,error);
    if(!store)throw std::runtime_error(toString(error));
    {
        auto& db=store->db();
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Warp'),(2,'audio','Other'),(9,'master','Main')");
        SQLite::Statement media(db,"INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) "
                                   "VALUES(1,?,'source.wav','wav',48000,2,?)");
        media.bind(1,digest.hex);media.bind(2,frames);media.exec();
        SQLite::Statement clip(db,"INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(7,1,'audio',0,?)");
        clip.bind(1,length);clip.exec();
        const auto markers=audio::writeWarpMap(std::vector<WarpMarker>{{0,0},{frames,length}},frames);
        SQLite::Statement audio(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) "
                                   "VALUES(7,1,?,1,?,?)");
        audio.bind(1,frames);
        audio.bind(2,prepared ? "complex" : "beats");
        audio.bind(3,markers.bytes.data(),static_cast<int>(markers.bytes.size()));
        audio.exec();
    }
    const auto elapsed=[](Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();};
    const auto load=[&](Session& session) {
        session.graph().setFadeFrames(0);
        if(!session.load(*store,{},SessionSpec{2,rate,1024}))throw std::runtime_error(session.error());
    };
    double coldOwner=0,coldReady=0,renameOwner=0,renameReady=0;
    std::array<float,2048> original{};
    {
        Session session;
        const auto start=Clock::now();
        load(session);coldOwner=elapsed(start);
        std::stop_source cancelled;
        cancelled.request_stop();
        check(!session.clips()->waitForWarp(cancelled.get_token()),"an offline caller can cancel its Warp wait");
        original=hearWarpHead(session);coldReady=elapsed(start);
        check(std::any_of(original.begin(),original.end(),[](float sample){return std::abs(sample)>0.01f;}),
              "long warped PCM is audible after cold preparation");
        if (prepared) {
            // The complete cached WAV has left its private staging directory.
            // A cache miss on refresh or reopen would have to create a new
            // render here, which the regular file makes impossible.
            if (!std::filesystem::is_directory(workFolder) || !std::filesystem::remove(workFolder))
                throw std::runtime_error("cold Warp did not publish its prepared WAV");
            std::ofstream blocked(workFolder);
            if (!blocked) throw std::runtime_error("block private Warp staging");
        }
        OpRequest rename;rename.opType="track.rename";rename.payload={{"id",2},{"name","Renamed"}};
        const auto edit=Clock::now();
        if(!OpJournal(*store).commit(rename).ok || !session.refresh(*store))throw std::runtime_error("rename in warped song");
        renameOwner=elapsed(edit);
        const auto renamed=hearWarpHead(session);renameReady=elapsed(edit);
        check(renamed==original,prepared ? "an unrelated edit reuses prepared Warp with staging blocked"
                                         : "an unrelated edit preserves streamed Beats PCM");
    }
    store.reset();
    if (prepared) {
        // The worker may still own its last completed task after ready is
        // published. A distinct request drains it before testing disk reuse.
        audio::WarpRenderPlan miss;
        miss.source = source;
        miss.workFolder = workFolder;
        miss.sourceFrames = frames;
        miss.outputRate = rate;
        miss.anchors = {{0,0},{frames,frames}};
        miss.mode = audio::WarpMode::RePitch;
        const auto task = audio::requestWarp(std::move(miss));
        if (!task) throw std::runtime_error("queue uncached Warp control");
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        auto outcome = task->result();
        while (!outcome && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            outcome = task->result();
        }
        if (!outcome || outcome->error != audio::WarpError::Io)
            throw std::runtime_error("uncached Warp control must fail with staging blocked");
    }
    const auto reopen=Clock::now();
    store=Store::open(project,error);
    if(!store)throw std::runtime_error(toString(error));
    Session reopened;
    load(reopened);const auto reopenOwner=elapsed(reopen);
    const auto reopenedPcm=hearWarpHead(reopened);const auto reopenReady=elapsed(reopen);
    check(reopenedPcm==original,prepared ? "a reopened project reuses its prepared Warp WAV with staging blocked"
                                         : "a reopened project preserves streamed Beats PCM");
    std::printf("SC-194 %s, owner/first PCM ms: cold %.2f/%.2f, rename %.2f/%.2f, reopen %.2f/%.2f\n",
                prepared ? "prepared Complex" : "three-minute streamed Beats",
                coldOwner,coldReady,renameOwner,renameReady,reopenOwner,reopenReady);
}
void sourceRatesAndTempoBoundary() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f(44100);f.load(width);const auto output=f.render(24000,width);
        double power=0;for(const auto value:output)power+=value*value;
        check(f.session.problems().empty() && power>1 && f.session.clips()->underrunSamples()==0,"actual project warp converts source-rate audio before native output playback");
        // Tempo changes halfway through a bar must split a warp interval.
        f.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(2882880,60)");
        check(f.session.refresh(*f.store),"tempo edit rebuilds a prepared warp generation");
        f.session.transport().locate(0);const auto changed=f.render(38000,width);power=0;
        for(std::size_t frame=29000;frame<33000;++frame)power+=changed[frame*2]*changed[frame*2];
        check(power>1 && std::all_of(changed.begin()+72000,changed.end(),[](double value){return value==0;}),"project tempo inside a warp interval changes heard duration from 24000 to 36000 frames");
    }
    TempoMap tempo;tempo.events={{0,120,0},{q/2,60,0}};
    const std::vector<WarpMarker> map={{0,0},{12000,q}};
    const auto anchors=projectWarpAnchors(map,tempo,0,48000);
    check(anchors==std::vector<audio::WarpAnchor>{{0,0},{6000,12000},{12000,36000}},"tempo projection inserts its exact interior anchor instead of flattening a bar");
    check(warpSourceSamples(tempo,-q,q,48000)==24000 && warpSourceSamples(tempo,-q,2*q,48000)==60000,"trimmed source clock extends the initial tempo before the project origin");
}
void rampProjection() {
    const TempoMap tempo{{{0,90,1}, {q,210,0}}};
    const std::vector<WarpMarker> markers{{0,0},{12000,q}};
    const auto projected = projectWarpAnchors(markers,tempo,0,48000);
    check(projected.size() > 3 && projected.front() == audio::WarpAnchor{0,0} &&
              projected.back() == audio::WarpAnchor{12000,20335} &&
              std::find(projected.begin(), projected.end(), audio::WarpAnchor{6000,12260}) != projected.end(),
          "warp projection retains its endpoints and anchors the ramp midpoint to the integrated sample");
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Fixture f;
        f.store->db().exec("UPDATE audio_clips SET warp_mode='re-pitch'; DELETE FROM tempo_map;"
                           "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,90,1),(5765760,210,0)");
        f.load(width);
        const auto output = f.render(21000,width);
        double power = 0;
        for (std::size_t sample=19000; sample<20000; ++sample) power += output[sample*2]*output[sample*2];
        check(f.session.problems().empty() && f.session.clips()->underrunSamples()==0 && power>1 &&
              std::all_of(output.begin()+40670,output.end(),[](double value){return value==0;}),
              "real warped audio plays to the integrated end of the ramp and stops on that sample");
    }
}
void unwarpedPitch() {
    for(const double semitones:{12.0,-0.5}) {
        Fixture fixture;
        auto& db=fixture.store->db();
        db.exec("UPDATE audio_clips SET warp_enabled=0");
        if(semitones<0)
            db.exec("UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=0,length_ns=340000000 WHERE id=7");
        OpRequest request;
        request.opType="audioClip.setTranspose";
        request.payload={{"id",7},{"semitones",semitones}};
        const auto before=digestProject(*fixture.store).text;
        const auto edit=OpJournal(*fixture.store).commit(request);
        check(edit.ok,"an audio clip can store a pitch edit independently of Warp");
        if(edit.ok) {
            const auto after=digestProject(*fixture.store).text;
            History history(*fixture.store);
            check(history.undo().ok && digestProject(*fixture.store).text==before &&
                  history.redo().ok && digestProject(*fixture.store).text==after,
                  "pitch edit undo and redo preserve other clip rows exactly");
        } else {
            SQLite::Statement row(db,"UPDATE audio_clips SET transpose_semis=? WHERE clip_id=7");
            row.bind(1,semitones);row.exec();
        }
        fixture.load(Precision::Float32);
        const auto output=fixture.render(16000,Precision::Float32);
        const auto speed=std::exp2(semitones/12);
        double error=0;
        for(std::size_t frame=1024;frame<5120;++frame) {
            const auto expected=0.25*std::sin(2*std::numbers::pi*440*static_cast<double>(frame)*speed/48000);
            const auto difference=output[frame*2]-expected;
            error+=difference*difference;
        }
        check(fixture.session.problems().empty() && error/4096<0.00005,
              "an unwarped sample plays audibly at the requested octave or fractional pitch");
        const auto tail=static_cast<std::size_t>(std::ceil(12000/speed))+2;
        check(std::all_of(output.begin()+static_cast<std::ptrdiff_t>(tail*2),output.end(),[](double value){return value==0;}) &&
                  fixture.session.clips()->underrunSamples()==0,
              "unwarped pitch plays to the resampled source end without callback underruns");
    }
}
void compressedPitchAndRecordingWarp() {
    Fixture fixture;
    auto& db=fixture.store->db();
    const auto assets=std::filesystem::path(__FILE__).parent_path()/"fixtures"/"decode";
    std::filesystem::copy_file(assets/"tone16.flac",fixture.temporary.path()/"source.flac");
    db.exec("UPDATE media_files SET rel_path='source.flac',format='flac' WHERE id=1;"
            "UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=0,length_ns=300000000 WHERE id=7;"
            "UPDATE audio_clips SET warp_enabled=0,src_len_frames=9600,transpose_semis=12 WHERE clip_id=7");
    fixture.load(Precision::Float32);
    const auto compressed=fixture.render(8000,Precision::Float32);
    audio::WavReader reference(assets/"src16.wav");
    std::vector<float> source(19200);
    check(reference.read(source.data(),9600)==9600,"load the lossless FLAC fixture's original PCM");
    double error=0.;
    for(std::size_t i=1000;i<4000;++i) {
        const auto difference=compressed[i*2]-source[i*4];
        error+=difference*difference;
    }
    check(fixture.session.problems().empty() && error/3000<0.00005 &&
          std::all_of(compressed.begin()+9600,compressed.end(),[](double value){return value==0;}),
          "octave transpose works with a progressive FLAC on an absolute clip clock");

    Fixture take;
    auto& takeDb=take.store->db();
    takeDb.exec("UPDATE audio_clips SET warp_mode='beats'");
    const auto identity=audio::writeWarpMap(std::vector<WarpMarker>{{0,0},{12000,q/2}},12000);
    SQLite::Statement row(takeDb,"UPDATE audio_clips SET warp_markers=? WHERE clip_id=7");
    row.bind(1,identity.bytes.data(),static_cast<int>(identity.bytes.size()));row.exec();
    take.load(Precision::Float32);
    const auto original=take.render(14000,Precision::Float32);
    takeDb.exec("UPDATE audio_clips SET transpose_semis=12 WHERE clip_id=7");
    check(take.session.refresh(*take.store),"refresh a take whose direct Warp path now needs pitching");
    take.session.transport().locate(0);
    const auto pitched=take.render(14000,Precision::Float32);
    const auto crossings=[](const auto& signal) {
        int count=0;
        for(std::size_t i=1024;i<5120;++i)if(signal[(i-1)*2]<=0. && signal[i*2]>0.)++count;
        return count;
    };
    check(take.session.problems().empty() && crossings(pitched)>crossings(original)*3/2 &&
          std::all_of(pitched.begin()+24000,pitched.end(),[](double value){return value==0;}),
          "pitch takes Beats Warp out of its identity path while preserving the take's duration");
}
void modelAndRefusals() {
    Fixture f;const auto model=rows::readModel(*f.store);
    check(model.audioClips.size()==1 && model.audioClips[0].warpMarkers &&
          audio::readWarpMap(*model.audioClips[0].warpMarkers,12000),"coherent native rows carry the stored marker bytes into playback");
    f.store->db().exec("UPDATE audio_clips SET warp_markers=X''");auto empty=rows::readModel(*f.store);
    check(empty.audioClips[0].warpMarkers && empty.audioClips[0].warpMarkers->empty(),"empty warp BLOB stays distinct from SQL NULL");
    f.load(Precision::Float32);
    const auto silent=f.render(1024,Precision::Float32);
    check(!f.session.problems().empty() && std::all_of(silent.begin(),silent.end(),[](double value){return value==0;}),"malformed stored warp data reports a named problem");
}
void workerFailure() {
    Fixture f;
    f.store->db().exec("UPDATE audio_clips SET warp_mode='complexPro',formant_shift=48");
    f.load(Precision::Float32);
    check(!f.session.clips()->waitForWarp(),"offline Warp wait refuses a failed render");
    constexpr std::string_view diagnostic = "clips#7: invalid warp source, rate, pitch or anchor count";
    const auto problems = f.session.clips()->decodeProblems();
    check(problems.size() == 1 && problems.front().find(diagnostic) != std::string::npos,
          "the Warp worker keeps the failed clip's diagnostic");
    f.session.tick(0);
    check(f.session.clipPlaybackUnavailable(7) && !f.session.clips()->mediaUnavailable(7) &&
              std::ranges::any_of(f.session.problems(), [diagnostic](const auto& problem) {
                  return problem.find(diagnostic) != std::string::npos;
              }), "SC-151: the owner reports a terminal Warp failure on the clip and in the problem list");
}
void replacementSourceInvalidatesWarp() {
    Fixture f;
    f.store->db().exec("UPDATE audio_clips SET warp_mode='re-pitch'");
    f.load(Precision::Float32);
    const auto original=hearWarpHead(f.session);
    check(std::any_of(original.begin(),original.end(),[](float sample){return std::abs(sample)>0.01f;}),
          "the first Warp recipe reads audible source bytes");
    const auto source=f.temporary.path()/"source.wav";
    const auto bytes=std::filesystem::file_size(source);
    const auto modified=std::filesystem::last_write_time(source);
    {
        const std::vector<float> silence(12000*2);
        audio::WavWriter writer(source,48000,2,audio::WavFormat::Float32);
        writer.write(silence.data(),12000);writer.close();
    }
    std::filesystem::last_write_time(source,modified+std::chrono::seconds(10));
    check(std::filesystem::file_size(source)==bytes,"source replacement keeps the same WAV dimensions and size");
    check(f.session.refresh(*f.store),"refresh the same Warp recipe on a changed source");
    const auto replaced=hearWarpHead(f.session);
    check(f.session.problems().empty() && f.session.clips()->readErrors()==0 &&
              std::all_of(replaced.begin(),replaced.end(),[](float sample){return sample==0.0f;}),
          "changed bytes at the same media path invalidate the old Warp result");
}
void trimmedMusicalOffset() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;f.load(width);const auto complete=f.render(24000,width);
        // A prior unwarped physical pin must not override the new source
        // musical offset. Both clocks intentionally disagree in this row.
        f.store->db().exec("UPDATE clips SET content_offset_ticks=1441440,length_ticks=4324320");
        check(f.session.refresh(*f.store),"refresh a trimmed warped source with an old physical offset pin");
        f.session.transport().locate(0);const auto trimmed=f.render(20000,width);
        check(f.session.problems().empty() && std::equal(trimmed.begin(),trimmed.begin()+36000,complete.begin()+12000) &&
              std::all_of(trimmed.begin()+36000,trimmed.end(),[](double value){return value==0;}),
              "heard trimmed Warp audio uses the musical source offset and ignores its old nanosecond pin");
    }
}
bool applyEdit(Fixture& fixture,const clip_edit::PreparedEdit& edit,const char* label) {
    if(!edit.ok){std::printf("%s: %s\n",label,edit.error.c_str());check(false,label);return false;}
    const auto committed=clip_edit::commitPrepared(*fixture.store,edit);
    if(!committed.ok)std::printf("%s: %s\n",label,committed.error.c_str());
    check(committed.ok,label);return committed.ok;
}
void trimAtWarpMarker() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;const auto markers=audio::writeWarpMap(std::vector<WarpMarker>{{0,0},{6000,q/2},{12000,q}},12000);
        SQLite::Statement marker(f.store->db(),"UPDATE audio_clips SET warp_markers=? WHERE clip_id=7");
        marker.bind(1,markers.bytes.data(),static_cast<int>(markers.bytes.size()));marker.exec();
        const auto before=digestProject(*f.store).text;f.load(width);const auto original=f.render(26000,width);
        const std::array<std::int64_t,1> tracks{1};edit_plan::Options options;options.policy.automationFollowsClips=false;
        const auto trim=edit_plan::prepareTrim(*f.store,
            edit_plan::materialScope(tracks,edit_plan::PlacementClock::Ticks,{0,2*q}),{7,{0,q/2}},options);
        if(!applyEdit(f,trim,"left trim exactly at a stored warp marker"))return;
        check(f.session.refresh(*f.store),"refresh a marker-aligned anchored Warp trim");
        f.session.transport().locate(0);const auto actual=f.render(26000,width);
        bool exact=actual.size()==52000 && original.size()==52000;
        if(exact)for(std::size_t i=0;i<actual.size();++i)exact=exact && actual[i]==(i<24000?0:original[i]);
        check(f.session.problems().empty() && exact &&
                  f.store->db().execAndGet("SELECT content_offset_ticks FROM clips WHERE id=7").getInt64()==q/2,
              "Warp trim retains original anchored marker timing and native-width samples");
        check(History(*f.store).undo().ok && digestProject(*f.store).text==before,
              "Warp trim undo restores exact original rows and optional-domain absence");
    }
}
void extendWarpWithStalePhysicalPin() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;f.load(width);const auto original=f.render(26000,width);
        // Stored old ns pins are legal but ignored by Warp playback. This is
        // the same source after an earlier musical trim/move.
        f.store->db().exec("UPDATE clips SET pos_ticks=2882880,length_ticks=2882880,content_offset_ticks=2882880");
        const auto before=digestProject(*f.store).text;const std::array<std::int64_t,1> tracks{1};
        edit_plan::Options options;options.policy.automationFollowsClips=false;
        const auto trim=edit_plan::prepareTrim(*f.store,
            edit_plan::materialScope(tracks,edit_plan::PlacementClock::Ticks,{0,2*q}),{7,{0,0}},options);
        if(!applyEdit(f,trim,"extend Warp to source zero despite a stale physical pin"))return;
        check(f.store->db().execAndGet("SELECT count(*) FROM clips WHERE pos_ticks=0 AND length_ticks=5765760 AND content_offset_ticks=0").getInt()==1 &&
                  f.store->db().execAndGet("SELECT content_offset_ns FROM clip_playback_details WHERE clip_id=7").getInt64()==0,
              "Warp extension restores full musical bounds without rewriting ignored physical metadata");
        check(f.session.refresh(*f.store),"refresh an extended Warp clip with a stale physical pin");
        f.session.transport().locate(0);const auto actual=f.render(26000,width);
        check(f.session.problems().empty() && actual==original,"extended Warp renders the complete original native-width PCM");
        check(History(*f.store).undo().ok && digestProject(*f.store).text==before,
              "stale-pin Warp extension undoes exact source clocks and geometry");
    }
}
void warpedEditsKeepHeardFades() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;auto& db=f.store->db();
        db.exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=0,loop_len_ticks=5765760,length_ticks=11531520,fade_in_ticks=8648640,fade_out_ticks=8648640,fade_in_curve=2,fade_out_curve=4");
        db.exec("UPDATE clip_playback_details SET in_tension=0.35,out_tension=-0.4");
        db.exec("ALTER TABLE clip_warp_fade_domains ADD COLUMN future_blob BLOB NOT NULL DEFAULT X'00FF01'");
        f.load(width);const auto original=f.render(50000,width);const auto before=digestProject(*f.store).text;
        if(!applyEdit(f,clip_edit::prepareSplit(*f.store,7,17,{0,q/2},48000),"split a warped loop inside overlapping fades"))return;
        check(db.execAndGet("SELECT count(*) FROM clip_warp_fade_domains WHERE in_begin_ticks=0 AND in_end_ticks=8648640 AND out_begin_ticks=2882880 AND out_end_ticks=11531520 AND hex(future_blob)='00FF01'").getInt()==2,
              "both split pieces retain the original source musical fade domains and future bytes");
        check(f.session.refresh(*f.store),"refresh after Warp split");f.session.transport().locate(0);
        check(f.session.problems().empty() && f.render(50000,width)==original,"Warp split preserves every heard sample through overlapping fades and source-loop wrapping");
        if(!applyEdit(f,clip_edit::prepareSplit(*f.store,17,18,{0,3*q/4},48000),"split an already retained Warp fade again"))return;
        check(f.session.refresh(*f.store),"refresh after repeated Warp split");f.session.transport().locate(0);
        check(f.session.problems().empty() && f.render(50000,width)==original,"repeated split retains the first fade's shape and unrolled clock exactly");
        const auto after=digestProject(*f.store).text;History history(*f.store);
        check(history.undo().ok && history.undo().ok && digestProject(*f.store).text==before,"undo both Warp splits restores exact rows and absent domain metadata");
        check(history.redo().ok && history.redo().ok && digestProject(*f.store).text==after,"redo restores literal Warp maps, retained fades and source offsets");
        const auto copy=clip_edit::copyClip(*f.store,18,48000);
        check(copy.ok,"copy a trimmed warped loop with retained fades");
        if(!applyEdit(f,clip_edit::preparePaste(*f.store,copy,27,1,{0,4*q},48000),"paste warped material at a new musical position"))return;
        check(db.execAndGet("SELECT hex(warp_markers) FROM audio_clips WHERE clip_id=27").getString()==db.execAndGet("SELECT hex(warp_markers) FROM audio_clips WHERE clip_id=18").getString() &&
              db.execAndGet("SELECT out_end_ticks FROM clip_warp_fade_domains WHERE clip_id=27").getInt64()==2*q,
              "paste preserves the map byte-for-byte and remaps the full typed fade child to the new clip");
        const auto beforeHandle=digestProject(*f.store).text;
        OpRequest handle;handle.opType="clip.setFades";handle.payload={{"id",18},{"out",{{"ticks",q/8},{"tension",0.2}}}};
        check(OpJournal(*f.store).commit(handle).ok && db.execAndGet("SELECT out_begin_ticks IS NULL AND in_begin_ticks=0 FROM clip_warp_fade_domains WHERE clip_id=18").getInt()!=0,
              "a Warp fade handle replaces only its chosen side's retained domain");
        check(history.undo().ok && digestProject(*f.store).text==beforeHandle,"handle undo restores all retained domains and future bytes exactly");
    }
}
void warpFadeProjectionAndSharing() {
    Fixture f;auto& db=f.store->db();
    db.exec("INSERT INTO clip_warp_fade_domains VALUES(7,0,100,200,300)");
    auto old=SnapshotBuilder::fromStore(*f.store);
    constexpr std::array<const char*,4> columns={"in_begin_ticks","in_end_ticks","out_begin_ticks","out_end_ticks"};
    for(std::size_t field=0;field<4;++field) {
        db.exec(std::string("UPDATE clip_warp_fade_domains SET ")+columns[field]+"="+columns[field]+"+1");
        auto next=SnapshotBuilder::fromStore(*f.store,*old);const auto model=rows::readModel(*f.store);
        check(next->tracks[0]->clips[0]!=old->tracks[0]->clips[0] && next->tracks[0]->clips[0]->fadeClockTicks[field]==model.clips[0].fadeClockTicks[field] &&
              next->tracks[0]->clips[0]->fadeClockTicks[field]!=old->tracks[0]->clips[0]->fadeClockTicks[field],"each musical Warp fade coordinate participates in coherent projection and structural sharing");
        old=std::move(next);
    }
    check(SnapshotBuilder::fromStore(*f.store,*old)->tracks[0]==old->tracks[0],"unchanged Warp fade metadata shares its complete track");
    const auto text=textproj::projectStore(*f.store).text;
    check(text.find("fadeInBeginTicks 1")!=std::string::npos && text.find("fadeOutEndTicks 301")!=std::string::npos,"text projection retains source musical domains including explicit zero/absence distinctions");
    bool incomplete=false,backwards=false;
    try{db.exec("UPDATE clip_warp_fade_domains SET in_end_ticks=NULL");}catch(const std::exception&){incomplete=true;}
    try{db.exec("UPDATE clip_warp_fade_domains SET out_end_ticks=out_begin_ticks");}catch(const std::exception&){backwards=true;}
    check(incomplete && backwards,"schema refuses partial or nonincreasing musical fade pairs");
}
void warpFadesAcrossTempoAndLinearSplit() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;auto& db=f.store->db();
        db.exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(2882880,60)");
        db.exec("UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=0,length_ns=750000000,fade_in_ticks=4324320,fade_out_ticks=4324320");
        f.load(width);const auto original=f.render(38000,width);const auto before=digestProject(*f.store).text;
        if(!applyEdit(f,clip_edit::prepareSplit(*f.store,7,17,{1,500000000},48000),"split linear placement inside a warped fade after a tempo junction"))return;
        check(f.session.refresh(*f.store),"refresh a split linear Warp placement");f.session.transport().locate(0);
        check(f.session.problems().empty() && f.render(38000,width)==original,"linear Warp split preserves heard fades across the project's tempo junction");
        check(History(*f.store).undo().ok && digestProject(*f.store).text==before,"undo linear Warp split restores exact source clocks and placement domain");
        const auto range=clip_edit::copyRange(*f.store,{{1},{0,q/4},{0,3*q/4}},48000);
        check(range.ok && range.images.size()==1,"copy a partial Warp range through both fades");
        const auto deletion=clip_edit::prepareDeleteRange(*f.store,{{1},{0,q/4},{0,3*q/4}},48000);
        if(!applyEdit(f,deletion,"delete a partial Warp range with retained edges"))return;
        check(db.execAndGet("SELECT count(*) FROM clip_warp_fade_domains").getInt()==2 && History(*f.store).undo().ok && digestProject(*f.store).text==before,
              "range deletion captures both edge domains and undoes without losing the original fade");
    }
}
}
int main(int argc,char** argv) {
    try {
        test::TempDirectory home{"warp_playback","cache"};
#if defined(_WIN32)
        if(_wputenv_s(L"ADI_HOME",home.path().c_str())!=0)throw std::runtime_error("set Warp playback app data");
#else
        if(setenv("ADI_HOME",home.path().c_str(),1)!=0)throw std::runtime_error("set Warp playback app data");
#endif
        if(argc==2 && std::string_view(argv[1])=="--long-warp-speed-only")longWarpResponse();
        else {
            modesAndLoops();longWarpResponse();longWarpResponse(true);sourceRatesAndTempoBoundary();rampProjection();modelAndRefusals();workerFailure();
            replacementSourceInvalidatesWarp();
            unwarpedPitch();compressedPitchAndRecordingWarp();
            trimmedMusicalOffset();trimAtWarpMarker();extendWarpWithStalePhysicalPin();warpedEditsKeepHeardFades();
            warpFadeProjectionAndSharing();warpFadesAcrossTempoAndLinearSplit();
        }
    } catch(const std::exception& error){check(false,error.what());}
    check(callbackAllocations.load()==0 && audio::callbackFileIo.load()==0,
          "actual warped audio callback allocates nothing and performs no file I/O");
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
