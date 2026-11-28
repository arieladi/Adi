// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/clip_fade.hpp"
#include "adi/clip_commands.hpp"
#include "adi/history.hpp"
#include "adi/edit_plan.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/progressive_decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/ui/tempo_edits.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <new>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#if defined(__linux__)
#include <unistd.h>
#endif
using namespace adi;
using namespace adi::engine;
namespace fs = std::filesystem;
namespace { std::atomic<std::uint32_t> callbackAllocations{0}; }
#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif
ADI_TEST_NOINLINE void* operator new(std::size_t n) {
    if(audio::onAudioThread) callbackAllocations.fetch_add(1,std::memory_order_relaxed);
    if(void* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
ADI_TEST_NOINLINE void* operator new[](std::size_t n) {return operator new(n);}
ADI_TEST_NOINLINE void operator delete(void* p) noexcept {std::free(p);}
ADI_TEST_NOINLINE void operator delete[](void* p) noexcept {std::free(p);}
ADI_TEST_NOINLINE void operator delete(void* p,std::size_t) noexcept {std::free(p);}
ADI_TEST_NOINLINE void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
// Nothrow allocations use the same counted malloc/free pair under sanitizers.
ADI_TEST_NOINLINE void* operator new(std::size_t n,const std::nothrow_t&) noexcept {
    try {return ::operator new(n);} catch (...) {return nullptr;}
}
ADI_TEST_NOINLINE void* operator new[](std::size_t n,const std::nothrow_t&) noexcept {
    try {return ::operator new[](n);} catch (...) {return nullptr;}
}
ADI_TEST_NOINLINE void operator delete(void* p,const std::nothrow_t&) noexcept {::operator delete(p);}
ADI_TEST_NOINLINE void operator delete[](void* p,const std::nothrow_t&) noexcept {::operator delete[](p);}
#undef ADI_TEST_NOINLINE
namespace {
int checks=0, failures=0;
void check(bool ok,const char* name) { ++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);} }
std::int64_t ns(std::int64_t frames) { return static_cast<std::int64_t>(std::llround(static_cast<double>(frames)*1e9/48000)); }
struct Fixture {
    test::TempDirectory dir{"clip_playback","project"};
    std::unique_ptr<Store> store;
    Session session;
    std::vector<float> media;
    explicit Fixture(std::uint32_t rate=48000,std::uint16_t channels=2,std::uint32_t frames=100000,double hz=0) {
        StoreError error{};store=Store::createEmpty(dir.path()/"session.adi",error);
        if(!store)throw std::runtime_error("create project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Master',1)");
        media.resize(static_cast<std::size_t>(frames)*channels);
        for(std::uint32_t i=0;i<frames;++i)for(std::uint16_t c=0;c<channels;++c)
            media[static_cast<std::size_t>(i)*channels+c]=hz>0?static_cast<float>(0.5*std::sin(2*std::numbers::pi*hz*i/rate)):(hz<0?0.5f:static_cast<float>((i%997)+1)/1024.0f*(c==0?1.0f:0.5f));
        audio::WavWriter w(dir.path()/"audio.wav",rate,channels,audio::WavFormat::Float32);w.write(media.data(),frames);w.close();
        store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','audio.wav','wav')");
    }
    void add(int id,std::int64_t start,std::int64_t length,std::int64_t source=0,std::int64_t sourceLength=80000) {
        SQLite::Statement q(store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(?,1,'audio',1,?,?)");
        q.bind(1,id);q.bind(2,ns(start));q.bind(3,ns(length));q.exec();
        SQLite::Statement a(store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(?,1,?,?)");
        a.bind(1,id);a.bind(2,source);a.bind(3,sourceLength);a.exec();
    }
    void load(int block, double rate = 48000.0) { session.graph().setFadeFrames(0);if(!session.load(*store,{}, {2,rate,block}))throw std::runtime_error(session.error());session.transport().play(); }
    std::vector<float> render(int frames,bool prime=true,std::chrono::milliseconds timeout=std::chrono::seconds(300)) {
        if(prime && !session.clips()->prime(timeout))throw std::runtime_error("prime timeout");
        std::vector<float> result(static_cast<std::size_t>(frames)*2);
        float* out[2]={result.data(),result.data()+frames};
        session.process({out,nullptr,2,0,frames,0});return result;
    }
};
void liveInputMonitoring() {
    for (const auto precision : {Precision::Float32, Precision::Float64}) {
        Fixture f;
        f.add(1, 17, 32);
        f.store->db().exec("UPDATE tracks SET monitor_mode=2 WHERE id=1");
        f.load(64);
        if (!f.session.setPrecision(precision)) throw std::runtime_error(f.session.error());
        const std::array stereo{RecordingTarget{1, {2, 0}}};
        check(f.session.setAudioMonitorInputs(stereo), "bind a resolved stereo input pair to the audio track");
        auto change = [&](const char* opType, const Payload& payload) {
            OpRequest r;
            r.opType=opType;
            r.payload=payload;
            if(!OpJournal(*f.store).commit(r).ok || !f.session.refresh(*f.store))
                throw std::runtime_error("refreshing the track monitor controls failed");
        };
        std::int64_t stream = 0;
        auto renderBlock = [&](bool haveRight = true) {
            std::array<float,64> a{}, b{}, c{};
            a.fill(0.125f); b.fill(0.25f); c.fill(0.5f);
            const float* in[]{haveRight ? a.data() : nullptr, b.data(), c.data()};
            std::array<float,128> pcm{};
            std::array<double,128> wide{};
            float* out[]{pcm.data(),pcm.data()+64};
            double* out64[]{wide.data(),wide.data()+64};
            AudioIo io{precision == Precision::Float32 ? out : nullptr, in, 2, 3, 64, stream};
            if (precision == Precision::Float64) io.out64 = out64;
            { const audio::CallbackScope callback; f.session.process(io); }
            stream += io.frames;
            if (precision == Precision::Float32) std::copy(pcm.begin(),pcm.end(),wide.begin());
            return wide;
        };
        auto render = [&](bool playing, bool haveRight = true, std::int64_t start = 0) {
            f.session.transport().locate(start);
            f.session.transport().play(playing);
            if (playing && !f.session.clips()->prime())
                throw std::runtime_error("could not prime the Arrangement clip");
            return renderBlock(haveRight);
        };
        auto& recorder = f.session.recording();
        auto startCapture = [&] {
            AudioRecordingSpec spec;
            spec.workFolder = f.dir.path();
            spec.sampleRate = 48000;
            spec.maxFrames = 64;
            spec.ringFrames = 4096;
            spec.targets = {{1, {2, 0}}};
            std::string error;
            if (!f.session.startRecording(spec, error))
                throw std::runtime_error(error);
        };
        auto checkCapture = [&](std::uint64_t frames, std::int64_t start) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                if (auto result = recorder.poll()) {
                    check(result->failure == RecordingFailure::None && result->hasAttachableAudio() &&
                              result->frames == frames && result->startSample == start,
                          "the take has the expected recorded interval");
                    check(!result->discard(), "remove the test take after the writer closed it");
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            throw std::runtime_error("recording writer did not finish");
        };
        const auto unarmed = render(false);
        check(std::all_of(unarmed.begin(),unarmed.end(),[](double sample){return sample==0;}),
              "Auto monitoring is silent when the track is unarmed");
        change("track.setArm", {{"id",1},{"armed",true}});
        auto stopped = render(false);
        check(stopped[0]==0.5 && stopped[63]==0.5 && stopped[64]==0.125 && stopped[127]==0.125,
              "Auto monitors the selected stereo channels when stopped");
        const auto playing = render(true);
        bool exact = true;
        for (std::size_t channel = 0; channel < 2; ++channel)
            for (std::size_t frame = 0; frame < 64; ++frame) {
                const double input = channel == 0 ? 0.5 : 0.125;
                const double wanted = frame >= 17 && frame < 49 ? f.media[(frame - 17) * 2 + channel] : input;
                exact = exact && playing[channel*64+frame] == wanted;
            }
        check(exact, "Auto returns to the live input at the exact Arrangement clip end");

        // Start in an existing clip. The callback crosses the end of the count-in,
        // so only its suffix is recorded and must play the armed track's input.
        startCapture();
        auto& transport = f.session.transport();
        transport.locate(17);
        transport.play();
        if (!f.session.clips()->prime())
            throw std::runtime_error("could not prime the clip under the recording start");
        transport.countIn(7);
        const auto recorded = renderBlock();
        check(std::all_of(recorded.begin() + 7, recorded.begin() + 64,
                          [](double sample) { return sample == 0.5; }) &&
                  std::all_of(recorded.begin() + 71, recorded.end(),
                              [](double sample) { return sample == 0.125; }),
              "SC-245: armed Auto plays only the input over the existing clip once capture starts");
        recorder.stop();
        check(render(true) == playing, "stopping capture while playing restores Auto clip playback");
        checkCapture(57, 17);

        const auto punchTicks = [&] {
            auto clock = f.session.beginRender();
            return std::pair{clock.tickAtSample(23), clock.tickAtSample(39)};
        }();
        change("transport.setLoop", {{"on",false},{"start",punchTicks.first},{"end",punchTicks.second}});
        change("transport.setPunch", {{"in",true},{"out",true}});
        startCapture();
        const auto punched = render(true);
        bool punchExact = true;
        for (std::size_t channel = 0; channel < 2; ++channel)
            for (std::size_t frame = 0; frame < 64; ++frame) {
                const double input = channel == 0 ? 0.5 : 0.125;
                const auto expected = frame >= 23 && frame < 39 ? input : playing[channel * 64 + frame];
                punchExact = punchExact && punched[channel * 64 + frame] == expected;
            }
        check(punchExact && transport.playing() && transport.position() == 64,
              "Auto hears the old clip before Punch-In and after Punch-Out while playback keeps advancing");
        (void)recorder.poll(); // publish the automatic punch-out stop
        (void)renderBlock(); // the next callback acknowledges it
        checkCapture(16, 23);
        change("transport.setPunch", {{"in",false},{"out",false}});

        f.session.transport().loop(5,41,true);
        const auto looped = render(true,true,35);
        bool loopExact = true;
        for (std::size_t frame = 0; frame < 64; ++frame) {
            const auto sample = frame < 6 ? 35 + frame : 5 + (frame - 6) % 36;
            const double wanted = sample >= 17 ? f.media[(sample - 17) * 2] : 0.5;
            loopExact = loopExact && looped[frame] == wanted;
        }
        check(loopExact, "Auto reopens hardware monitoring at each loop wrap inside the callback");
        f.session.transport().loop(0,64,false);
        change("track.setMonitorMode", {{"id",1},{"mode",1}});
        change("track.setArm", {{"id",1},{"armed",false}});
        const auto inputOnly = render(true, false);
        check(inputOnly[0]==0.5 && inputOnly[17]==0.5 && inputOnly[48]==0.5 &&
              inputOnly[64]==0 && inputOnly[64+17]==0 && inputOnly[127]==0,
              "In monitors without Arm, suppresses clips and silences a missing channel");
        const std::array bad{RecordingTarget{1, {0, 1, 2}}};
        check(!f.session.setAudioMonitorInputs(bad) && render(false)[0]==0.5,
              "refusing an invalid input preserves the current hardware binding");
        const std::array mono{RecordingTarget{1, {1}}};
        check(f.session.setAudioMonitorInputs(mono), "replace the pair with one driver channel");
        const auto doubled = render(true);
        check(doubled[0]==0.25 && doubled[64]==0.25 && doubled[64+17]==0.25,
              "mono input reaches both outputs");
        change("track.setMonitorMode", {{"id",1},{"mode",0}});
        change("track.setArm", {{"id",1},{"armed",true}});
        const auto off = render(true);
        check(off[0]==0 && off[16]==0 && off[17]==f.media[0] && off[48]==f.media[62] &&
              off[49]==0 && off[63]==0, "Off keeps Arrangement PCM and discards hardware even when armed");
        startCapture();
        check(render(true) == off, "Monitor Off keeps the old clip audible during capture pending R45");
        recorder.stop();
        (void)render(false);
        checkCapture(64, 0);
        check(f.session.setAudioMonitorInputs({}), "disconnect all runtime inputs");
    }
}
void placement() {
    for(const int block:{64,128,256,512,1024,2048,4096,257}) {
        Fixture f;f.add(1,73,21001,11,25000);f.load(block);
        bool exact=true;int at=0;
        while(at<24000) {
            const int n=std::min(block,24000-at);const auto out=f.render(n);
            for(int c=0;c<2;++c)for(int i=0;i<n;++i) {
                const int t=at+i;const float want=t>=73 && t<21074?f.media[static_cast<std::size_t>(t-73+11)*2+static_cast<std::size_t>(c)]:0;
                exact=exact && out[static_cast<std::size_t>(c*n+i)]==want;
            }
            at+=n;
        }
        check(exact,"sample-exact placement and source window across block sizes/pages");
        check(f.session.transport().position()==24000,"transport advances once per callback");
        check(f.session.clips()->underrunSamples()==0,"primed rendering has no underruns");
    }
    Fixture f;f.add(1,0,2000,0,3000);
    f.store->db().exec("UPDATE clips SET time_base=0,pos_ns=NULL,length_ns=NULL,pos_ticks=576576,length_ticks=1153152; DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(576576,60,0)");
    f.load(4096);f.session.transport().locate(2300);const auto out=f.render(4096);
    check(out[99]==0 && out[100]==f.media[0] && out[101]==f.media[2],"tick placement integrates tempo map");
}
void unwarpedTempoChanges() {
    constexpr auto start = textproj::kPPQ;
    constexpr int sourceFrames = 800;
    for (const bool trimmed : {false, true}) {
        Fixture f(48000, 2, sourceFrames);
        f.store->db().exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120)");
        const auto drop = ui::dropAudio(*SnapshotBuilder::fromStore(*f.store), 1, start,
            "Source", ui::AudioImport{1, 48000, sourceFrames, {}, 0});
        if (!OpJournal(*f.store).commit(drop.ops).ok)
            throw std::runtime_error("could not place the unwarped test clip");
        const int audible = trimmed ? 600 : sourceFrames;
        if (trimmed) {
            OpRequest resize;
            resize.opType = "clip.resize";
            resize.payload = {{"id", drop.clip}, {"pos", start}, {"length", 144144}};
            if (!OpJournal(*f.store).commit(resize).ok)
                throw std::runtime_error("could not trim the unwarped test clip");
        }
        f.load(1024);
        const auto atTempo = [&](int bpm, const char* checkName) {
            const auto snapshot = SnapshotBuilder::fromStore(*f.store);
            const auto* track = snapshot->findTrack(1);
            const auto& clip = *track->clips.front();
            const auto first = 48000 * 60 / bpm;
            f.session.transport().locate(first - 1);
            const auto out = f.render(1024);
            const auto end = std::llround(snapshot->tempo->ticksToSeconds(clip.posTicks + clip.lengthTicks) * 48000);
            bool exact = clip.posTicks == start && end == first + audible;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                for (std::size_t frame = 0; frame < 1024; ++frame) {
                    const auto expected = frame > 0 && frame <= static_cast<std::size_t>(audible)
                        ? f.media[(frame - 1) * 2 + channel] : 0.0f;
                    exact = exact && out[channel * 1024 + frame] == expected;
                }
            }
            check(exact, checkName);
        };
        atTempo(120, "unwarped clip starts on its beat and plays its authored span");
        for (const int bpm : {240, 60}) {
            const auto edit = ui::setTempoAt(SnapshotBuilder::fromStore(*f.store)->tempo->events, 0, std::to_string(bpm), true);
            if (!OpJournal(*f.store).commit(edit.ops).ok || !f.session.refresh(*f.store))
                throw std::runtime_error("could not change the tempo for unwarped playback");
            atTempo(bpm, bpm == 240
                ? "raising the tempo preserves unwarped PCM and its full projected duration"
                : "lowering the tempo preserves unwarped PCM and leaves no silent arrangement tail");
        }
        check(History(*f.store).undo().ok && f.session.refresh(*f.store), "undo the slower tempo");
        atTempo(240, "tempo undo restores the exact unwarped endpoint");
        check(History(*f.store).redo().ok && f.session.refresh(*f.store), "redo the slower tempo");
        atTempo(60, "tempo redo restores the exact unwarped endpoint");
    }
}
void reversedSource() {
    constexpr int start=73, source=37, length=21001, block=317;
    const auto addReversed = [](Fixture& fixture) {
        fixture.add(1,start,length,source,length);
        fixture.store->db().exec("UPDATE audio_clips SET reverse=1");
    };
    Fixture f(48000,2,30000);
    addReversed(f);
    f.load(block);
    bool exact=true;
    for(int at=0;at<start+length+block;at+=block) {
        const auto out=f.render(block);
        for(int channel=0;channel<2;++channel)for(int frame=0;frame<block;++frame) {
            const auto local=at+frame-start;
            const auto expected=local<0 || local>=length ? 0.0f :
                f.media[static_cast<std::size_t>(2*(source+length-1-local)+channel)];
            exact=exact && out[static_cast<std::size_t>(channel*block+frame)]==expected;
        }
    }
    check(exact && f.session.problems().empty() && f.session.clips()->underrunSamples()==0,
          "native reverse plays the source window frame-exactly with intact channels across disk pages");

    Fixture forward(48000,2,30000);
    forward.add(1,start,length,source,length);
    for(int i=0;i<length/2;++i)for(int c=0;c<2;++c)
        std::swap(forward.media[static_cast<std::size_t>(2*(source+i)+c)],
                  forward.media[static_cast<std::size_t>(2*(source+length-1-i)+c)]);
    {
        audio::WavWriter writer(forward.dir.path()/"audio.wav",48000,2,audio::WavFormat::Float32);
        writer.write(forward.media.data(),30000);
        writer.close();
    }
    Fixture reversed(48000,2,30000);
    addReversed(reversed);
    reversed.load(block,44100);
    forward.load(block,44100);
    bool resampled=true;
    for(const auto at:{13000,200,7500}) {
        reversed.session.transport().locate(at);
        forward.session.transport().locate(at);
        resampled=resampled && reversed.render(block)==forward.render(block);
    }
    check(resampled,"reverse seeks and resampling match a physically reversed source window");
}
void reverseSelectedRange() {
    constexpr int source=37, block=512;
    constexpr auto quarter=textproj::kPPQ;
    Fixture f(48000,2,40000);
    f.add(1,0,24000,source,30000);
    const auto hash = media::blake3File(f.dir.path() / "audio.wav");
    if (!hash) {
        check(false,"hash the Reverse selection's source file");
        return;
    }
    {
        SQLite::Statement media(f.store->db(),"UPDATE media_files SET hash_blake3=? WHERE id=1");
        media.bind(1,hash.hex);
        media.exec();
    }
    const std::array clips{std::int64_t{1}};
    const auto edit=clip_edit::prepareReverse(*f.store,clips,std::pair{quarter/4,quarter/2},48000);
    if (!edit.ok) {
        check(false,edit.error.c_str());
        return;
    }
    const auto committed = clip_edit::commitPrepared(*f.store,edit);
    if (!committed.ok) {
        check(false,committed.error.c_str());
        return;
    }
    check(edit.requests.size()==1,"Reverse captures both cuts and the selected audio in one edit");
    f.load(block);
    const auto matches=[&](bool reversed) {
        bool exact=true;
        f.session.transport().locate(0);
        for(int at=0;at<24000;at+=block) {
            const auto out=f.render(block);
            for(int i=0;i<block && at+i<24000;++i) {
                const auto t=at+i;
                const auto material=reversed && t>=6000 && t<12000?17999-t:t;
                exact=exact && out[static_cast<std::size_t>(i)]==f.media[static_cast<std::size_t>(2*(source+material))];
            }
        }
        return exact;
    };
    check(matches(true),"the reversed middle plays only the selected audio with forward material on both sides");
    check(History(*f.store).undo().ok && f.session.refresh(*f.store) && matches(false),
          "one undo restores the original uninterrupted playback");
}
void reverseSelectedRepeats() {
    constexpr int block=256, loopSource=7, nextSource=500;
    const auto at=[](std::int64_t frame) {return frame*textproj::kPPQ/24000;};
    Fixture f(48000,2,2000);
    f.add(1,0,1200,loopSource,300);
    f.add(2,1200,120,nextSource,120);
    const auto hash=media::blake3File(f.dir.path()/"audio.wav");
    if(!hash){check(false,"hash the looping Reverse selection source");return;}
    {
        SQLite::Statement media(f.store->db(),"UPDATE media_files SET hash_blake3=? WHERE id=1");
        media.bind(1,hash.hex);
        media.exec();
    }
    f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=24024 WHERE id=1;"
                       "INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(1,48048);"
                       "INSERT INTO clip_playback_details(clip_id,source_end_ns,loop_begin_ns,loop_end_ns,in_begin_ns,in_end_ns) "
                       "VALUES(1,4166667,2083333,4166667,0,520833);");
    const std::array both{std::int64_t{1},std::int64_t{2}};
    const std::array loop{std::int64_t{1}};
    const auto head=OpJournal(*f.store).headSeq();
    const auto mixed=clip_edit::prepareReverse(*f.store,loop,std::pair{at(50),at(250)},48000);
    check(!mixed.ok && mixed.error.find("intro")!=std::string::npos && OpJournal(*f.store).headSeq()==head,
          "an intro followed by loop wraps refuses without changing any clip");

    const auto read=[&] {
        std::array<float,1320> pcm{};
        f.session.transport().locate(0);
        for(int start=0;start<1320;start+=block) {
            const auto out=f.render(block);
            std::copy_n(out.begin(),std::min(block,1320-start),pcm.data()+start);
        }
        return pcm;
    };
    f.load(block);
    const auto original=read();
    const auto matches=[&](auto&& originalIndex) {
        const auto pcm=read();
        for(int i=0;i<1320;++i)
            if(pcm[static_cast<std::size_t>(i)]!=original[static_cast<std::size_t>(originalIndex(i))])return false;
        return true;
    };
    const auto reversed=clip_edit::prepareReverse(*f.store,both,std::pair{at(900),at(1250)},48000);
    check(reversed.ok && reversed.requests.size()==1,"a late repeat and its neighbor can be reversed in one prepared edit");
    if(reversed.ok && clip_edit::commitPrepared(*f.store,reversed).ok && f.session.refresh(*f.store)) {
        check(matches([](int frame) {
            if(frame>=900 && frame<1200)return 2099-frame;
            if(frame>=1200 && frame<1250)return 2449-frame;
            return frame;
        }),"Reverse changes only the selected later repeats and the covered portion of the neighboring clip");
        if(!History(*f.store).undo().ok || !f.session.refresh(*f.store)) {
            check(false,"one Undo restores both selected clips");
            return;
        }
        check(read()==original,"one Undo restores the initial loop phase and neighboring PCM");
    } else if(reversed.ok) {
        check(false,"commit the single captured Reverse edit for both clips");
        return;
    }

    const auto firstPass=clip_edit::prepareReverse(*f.store,loop,std::pair{at(50),at(175)},48000);
    const bool committed=firstPass.ok && clip_edit::commitPrepared(*f.store,firstPass).ok && f.session.refresh(*f.store);
    check(committed && matches([](int frame) {
        return frame>=50 && frame<175?224-frame:frame;
    }),"the intro and uninterrupted first pass reverse as a one-shot, keeping the surrounding loop intact");
}
void takeLanePlayback() {
    Fixture f;
    f.add(1, 0, 500, 11, 1000);
    f.add(2, 0, 500, 51, 1000);
    f.store->db().exec("INSERT INTO lanes(id,track_id,name) VALUES(7,1,'Lane');"
                       "UPDATE clips SET lane_id=7 WHERE id=2");
    f.load(512);
    const auto out = f.render(512);
    bool exact = f.session.model().clips.size() == 2;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (std::size_t frame = 0; frame < 512; ++frame) {
            const auto expected = frame < 500 ? f.media[(frame + 11) * 2 + channel] : 0.0f;
            exact = exact && out[channel * 512 + frame] == expected;
        }
    }
    check(exact, "main comp plays sample-exactly while a stored take overlaps it");
    const auto history = OpJournal(*f.store).headSeq();
    check(f.session.auditionTakeLane(*f.store, 1, 7), "SC-253: auditioning a lane rebuilds the live timeline");
    f.session.transport().locate(0);
    const auto audition = f.render(512);
    bool takeExact = true;
    for (std::size_t channel = 0; channel < 2; ++channel)
        for (std::size_t frame = 0; frame < 512; ++frame) {
            const auto expected = frame < 500 ? f.media[(frame + 51) * 2 + channel] : 0.0f;
            takeExact = takeExact && audition[channel * 512 + frame] == expected;
        }
    check(takeExact && OpJournal(*f.store).headSeq() == history,
          "SC-253: the take replaces the main playback during audition without an Undo entry");
    check(f.session.auditionTakeLane(*f.store, 1, 0), "SC-253: turning off audition restores the main lane");
    f.session.transport().locate(0);
    check(f.render(512) == out, "SC-253: turning audition off restores the sample-exact comp");
    f.store->db().exec("DELETE FROM clips WHERE id=1");
    if (!f.session.refresh(*f.store)) throw std::runtime_error(f.session.error());
    f.session.transport().locate(0);
    const auto takeOnly = f.render(512);
    check(f.session.model().clips.size() == 1 &&
          std::ranges::all_of(takeOnly, [](float sample) { return sample == 0; }),
          "refresh with only a stored take delivers silence");
}
void properties() {
    Fixture f;f.add(1,0,500,10,1000);f.add(2,0,500,10,1000);f.add(3,0,500,10,1000);
    f.store->db().exec("UPDATE clips SET gain_db=-6.020599913279624 WHERE id=1; UPDATE clips SET muted=1 WHERE id=3; UPDATE audio_clips SET channel_mode=2 WHERE clip_id=2");
    f.load(512);auto o=f.render(512);
    check(std::abs(o[30]-(f.media[80]*0.5f+f.media[81]))<1e-6f,"overlapping clips gain mute right mode");
    check(std::abs(o[512+30]-(f.media[81]*0.5f+f.media[81]))<1e-6f,"stereo channel routing");
    check(o[500]==0 && o[499]!=0,"clip length ends exactly without default fade");
    Fixture fades;fades.add(1,0,800,0,1000);
    // At 120 BPM 240.24 ticks per sample: 100 samples = 24024 ticks.
    fades.store->db().exec("UPDATE clips SET fade_in_ticks=24024,fade_out_ticks=24024");fades.load(1024);o=fades.render(1024);
    check(o[0]==0 && std::abs(o[50]-fades.media[100]*0.5f)<1e-6f,"linear fade in is per sample");
    check(o[799]==0 && std::abs(o[749]-fades.media[1498]*0.5f)<1e-6f,"linear fade out reaches zero on final sample");
    Fixture loop;loop.add(1,0,2000,7,1000);
    loop.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048,content_offset_ticks=12012");loop.load(2048);o=loop.render(2048);
    bool exact=true;
    for(int i=0;i<2000;++i){auto s=i+50;if(s>=100)s=100+(s-100)%200;exact=exact && o[static_cast<std::size_t>(i)]==loop.media[static_cast<std::size_t>(s+7)*2];}
    check(exact,"clip loop and content offset use source window");
    for(int mode:{1,3}) {
        Fixture m;m.add(1,0,100,0,100);m.store->db().exec("UPDATE audio_clips SET channel_mode="+std::to_string(mode));m.load(128);o=m.render(128);
        const float want=mode==1?m.media[20]:(m.media[20]+m.media[21])*0.5f;
        check(o[10]==want && o[138]==want,"left and mono-sum channel modes");
    }
}
void legacyUnwarpedLoopPastSource() {
    for (const auto offset : {0, 96096}) {
        Fixture f;
        f.add(1, 0, 2000, 7, 300);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,60);"
                           "UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048,"
                           "content_offset_ticks=" + std::to_string(offset));
        f.load(512);
        const auto audio = f.render(512);
        check(audio.front() == f.media[14] && audio[299] == f.media[612] && audio[300] == 0,
              "an old unwarped loop that outgrows the source plays the file once instead of silencing it");
    }
}
void legacyUnwarpedLoopRetainsEnd() {
    for (const bool physicalEnd : {false, true}) {
        for (const auto &[offsetTicks, firstFrame] : {std::pair{0, 0}, std::pair{6006, 50},
                                                        std::pair{24024, 0}, std::pair{96096, 0}}) {
            Fixture f;
            f.add(1, 0, 2000, 7, 300);
            f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,60);"
                               "UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048,"
                               "content_offset_ticks=" + std::to_string(offsetTicks));
            if (physicalEnd)
                f.store->db().exec("INSERT INTO clip_playback_details(clip_id,source_end_ns) VALUES(1,2500000)");
            else
                f.store->db().exec("INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(1,15015)");
            f.load(512);
            const auto pcm = f.render(512);
            const auto endFrame = physicalEnd ? 120 : 125;
            const auto played = static_cast<std::size_t>(endFrame - firstFrame);
            check(pcm.front() == f.media[static_cast<std::size_t>(7 + firstFrame) * 2] &&
                      pcm[played - 1] == f.media[static_cast<std::size_t>(7 + endFrame - 1) * 2] &&
                      std::all_of(pcm.begin() + static_cast<std::ptrdiff_t>(played), pcm.begin() + 512,
                                  [](float sample) { return sample == 0.f; }),
                  "the nonloop fallback respects End and resets only offsets outside that interval");
        }
    }
}
void editedFades() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f(48000,2,4000,-1);f.add(1,0,800,0,1000);
        f.store->db().exec("UPDATE clips SET fade_in_ticks=24024,fade_in_curve=2");
        OpRequest edit;edit.opType="clip.setFades";
        edit.payload={{"id",1},{"in",{{"tension",0.0}}},{"out",{{"ticks",24024},{"curve",5},{"tension",0.3}}}};
        check(OpJournal(*f.store).commit(edit).ok,"fade gesture commits both sides as one step");
        SessionSpec spec;spec.channels=2;spec.sampleRate=48000;spec.maxFrames=1024;spec.precision=width;
        f.session.graph().setFadeFrames(0);
        check(f.session.load(*f.store,{},spec),"load edited fade at native width");
        f.session.transport().play();
        std::array<std::array<float,1024>,2> narrow{};std::array<std::array<double,1024>,2> wide{};
        float* output[]={narrow[0].data(),narrow[1].data()};double* output64[]={wide[0].data(),wide[1].data()};
        auto render=[&] {
            f.session.transport().locate(0);
            if(!f.session.clips()->prime(std::chrono::seconds(2)))throw std::runtime_error("edited fade prime");
            AudioIo io;io.numOut=2;io.frames=1024;
            if(width==Precision::Float64)io.out64=output64;else io.out=output;
            audio::CallbackScope callback;f.session.process(io);
        };
        auto sample=[&](std::size_t i){return width==Precision::Float64?wide[0][i]:static_cast<double>(narrow[0][i]);};
        render();
        check(std::abs(sample(50)-0.25)<1e-7,"explicit zero tension changes old square to continuous linear exponent");
        const double shaped=0.5*clipFadeShape(0.5,5,0.3);
        check(std::abs(sample(749)-shaped)<(width==Precision::Float64?1e-12:1e-7),"rendered fade-out matches the window's shared continuous evaluator");
        check(sample(0)==0 && sample(799)==0 && sample(800)==0,"edited fade endpoints and placement are exact");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store),"undo rebuilds legacy envelope and absent shape row");
        render();
        check(std::abs(sample(50)-0.125)<1e-7 && sample(799)==0.5,"undo restores old square arithmetic and no automatic end fade");

        const Payload offset={{"ticks",24024},{"startTicks",12012},{"curve",1}};
        edit.payload={{"id",1},{"in",offset},{"out",offset}};
        const auto setOffsets=OpJournal(*f.store).commit(edit);
        check(setOffsets.ok && f.session.refresh(*f.store),"offset fade handles rebuild in the playing session");
        render();
        check(sample(49)==0 && sample(50)==0 && sample(75)==0.25 && sample(100)==0.5,
              "fade-in stays silent until its lower handle and reaches the same peak");
        check(sample(699)==0.5 && sample(724)==0.25 && sample(749)==0 && sample(750)==0 && sample(799)==0,
              "fade-out reaches its independent lower handle and leaves the rest silent");

        edit.payload={{"id",1},{"in",{{"ticks",12013},{"startTicks",12012}}}};
        const auto shortFade=OpJournal(*f.store).commit(edit);
        check(shortFade.ok && f.session.refresh(*f.store),"handle span below one output sample keeps the clip playable");
        render();
        check(sample(49)==0 && sample(50)==0.5 && sample(724)==0.25 && sample(750)==0,
              "sub-sample handle span becomes a step and keeps the opposite fade");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store),"undo restores the authored fade domain");
        render();
        check(sample(50)==0 && sample(75)==0.25 && sample(724)==0.25 && sample(750)==0,
              "undo restores both offset fade envelopes sample exactly");
    }
}
void shortenedFade() {
    for (const int start : {0, 400}) {
        Fixture f(48000, 2, 4000, -1);
        f.add(1, start, 800, 0, 1000);
        const auto ticks = static_cast<std::int64_t>(start) * 24024 / 100;
        SQLite::Statement placement(f.store->db(), "UPDATE clips SET time_base=0,pos_ticks=?,"
            "length_ticks=192192,pos_ns=NULL,length_ns=NULL WHERE id=1");
        placement.bind(1, ticks);
        placement.exec();
        OpJournal journal(*f.store);
        OpRequest fade;
        fade.opType = "clip.setFades";
        fade.payload = {{"id",1},{"out",{{"ticks",192192}}}};
        check(journal.commit(fade).ok, "set a fade over the entire clip");
        f.load(1024);
        const auto render = [&] {
            f.session.transport().locate(start);
            return f.render(1024);
        };
        const auto before = render();
        OpRequest resize;
        resize.opType = "clip.resize";
        resize.payload = {{"id",1},{"pos",ticks},{"length",96096}};
        check(journal.commit(resize).ok && f.session.refresh(*f.store), "right edge resizes under its fade");
        const auto shorter = render();
        check(std::abs(shorter[0] - 0.5f * 399.0f / 400.0f) < 1e-7f &&
              std::abs(shorter[200] - 0.5f * 199.0f / 400.0f) < 1e-7f &&
              shorter[399] == 0.0f && shorter[400] == 0.0f,
              "shortening clamps the fallback fade to the audible clip span");
        const auto split = clip_edit::prepareSplit(*f.store, 1, 2, {0, ticks + 48048}, 48000);
        const bool splitCommitted = split.ok && clip_edit::commitPrepared(*f.store, split).ok;
        check(splitCommitted && f.session.refresh(*f.store) && render() == shorter,
              "splitting a shortened clip retains the clamped fade exactly");
        if (splitCommitted)
            check(History(*f.store).undo().ok && f.session.refresh(*f.store) && render() == shorter,
                  "Undo of Split restores the shortened clip and its audible fade");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store) && render() == before,
              "Undo of a shorter placement restores the entire original fade exactly");
    }

    Fixture warped(48000, 2, 4000, -1);
    warped.add(1, 0, 400, 0, 1000);
    auto& db = warped.store->db();
    db.exec("UPDATE clips SET time_base=0,pos_ticks=0,length_ticks=96096,pos_ns=NULL,length_ns=NULL,"
        "fade_out_ticks=192192; UPDATE audio_clips SET warp_enabled=1,warp_mode='re-pitch'");
    const std::array<WarpMarker, 2> markers{{{0, 0}, {1000, 240240}}};
    const auto map = audio::writeWarpMap(markers, 1000);
    if (!map) throw std::runtime_error("prepare shortened fade Warp map");
    SQLite::Statement setMap(db, "UPDATE audio_clips SET warp_markers=? WHERE clip_id=1");
    setMap.bind(1, map.bytes.data(), static_cast<int>(map.bytes.size()));
    setMap.exec();
    warped.load(1024);
    const auto before = warped.render(1024);
    const auto split = clip_edit::prepareSplit(*warped.store, 1, 2, {0, 48048}, 48000);
    check(before[100] > 0.1f && split.ok && clip_edit::commitPrepared(*warped.store, split).ok &&
              warped.session.refresh(*warped.store),
          "a shortened Warp clip can split inside its clamped fade");
    warped.session.transport().locate(0);
    check(warped.render(1024) == before, "the split Warp fade keeps the exact audible envelope");
}
// clip.resize stores fades that fit the new span; Undo restores the authored
// ones and entries journalled before the clamp replay as they were recorded.
void resizeClampsFades() {
    const auto fades = [](Store& store) {
        SQLite::Statement row(store.db(), "SELECT fade_in_ticks,fade_out_ticks,length_ticks FROM clips WHERE id=1");
        if (!row.executeStep()) throw std::runtime_error("resized clip is missing");
        return std::array<std::int64_t, 3>{row.getColumn(0).getInt64(), row.getColumn(1).getInt64(),
                                           row.getColumn(2).getInt64()};
    };
    const auto commit = [](Store& store, const char* type, const Payload& payload) {
        OpRequest request;
        request.opType = type;
        request.payload = payload;
        return OpJournal(store).commit(request);
    };
    Fixture warped(48000, 2, 4000, -1);
    warped.add(1, 0, 800, 0, 1000);
    auto& db = warped.store->db();
    db.exec("UPDATE clips SET time_base=0,pos_ticks=0,length_ticks=192192,pos_ns=NULL,length_ns=NULL;"
            "UPDATE audio_clips SET warp_enabled=1,warp_mode='re-pitch'");
    const std::array<WarpMarker, 2> markers{{{0, 0}, {1000, 240240}}};
    const auto map = audio::writeWarpMap(markers, 1000);
    if (!map) throw std::runtime_error("prepare resized fade Warp map");
    SQLite::Statement setMap(db, "UPDATE audio_clips SET warp_markers=? WHERE clip_id=1");
    setMap.bind(1, map.bytes.data(), static_cast<int>(map.bytes.size()));
    setMap.exec();
    History history(*warped.store);
    check(commit(*warped.store, "clip.setFades", {{"id",1},{"in",{{"ticks",96096}}},{"out",{{"ticks",144144}}}}).ok,
          "author a Warp fade-in over half the clip and a fade-out over three quarters");
    const auto resized = commit(*warped.store, "clip.resize", {{"id",1},{"pos",0},{"length",48048}});
    check(resized.ok && fades(*warped.store) == std::array<std::int64_t, 3>{48048, 48048, 48048},
          "a raw resize to a quarter clamps both stored fades to the new span");
    const auto copy = clip_edit::copyClip(*warped.store, 1, 48000);
    check(copy.ok, "a Warp clip resized under its fade-in copies");
    if (!copy.ok) std::printf("copy: %s\n", copy.error.c_str());
    check(history.undo().ok && fades(*warped.store) == std::array<std::int64_t, 3>{96096, 144144, 192192},
          "Undo of the resize restores the authored fades and length exactly");
    check(history.redo().ok && fades(*warped.store) == std::array<std::int64_t, 3>{48048, 48048, 48048},
          "Redo of the resize clamps the fades again");
    const auto split = clip_edit::prepareSplit(*warped.store, 1, 2, {0, 24024}, 48000);
    if (!split.ok) std::printf("split: %s\n", split.error.c_str());
    check(split.ok && clip_edit::commitPrepared(*warped.store, split).ok,
          "a Warp clip resized under its fade-in splits");
    check(history.undo().ok && history.undo().ok &&
              fades(*warped.store) == std::array<std::int64_t, 3>{96096, 144144, 192192},
          "Undo of Split and resize returns to the authored fades");

    // An entry journalled before the clamp has a placement image without fade
    // columns. Its Undo and Redo keep the row effects the old build recorded.
    const auto legacy = commit(*warped.store, "clip.resize", {{"id",1},{"pos",0},{"length",48048}});
    if (!legacy.ok) throw std::runtime_error(legacy.error);
    const auto inverse = encodePayload(Payload{{"id",1},{"_placement",{{"id",1},{"track_id",1},{"lane_id",nullptr},
        {"time_base",0},{"pos_ticks",0},{"pos_ns",nullptr},{"length_ticks",192192},{"length_ns",nullptr}}}});
    SQLite::Statement replace(db, "UPDATE ops SET inverse=? WHERE seq=?");
    replace.bind(1, inverse.data(), static_cast<int>(inverse.size()));
    replace.bind(2, legacy.seqs.back());
    replace.exec();
    db.exec("UPDATE clips SET fade_in_ticks=96096,fade_out_ticks=144144 WHERE id=1");
    check(history.undo().ok && fades(*warped.store) == std::array<std::int64_t, 3>{96096, 144144, 192192} &&
              history.redo().ok && fades(*warped.store) == std::array<std::int64_t, 3>{96096, 144144, 48048},
          "a resize journalled before the clamp replays its recorded row effects");

    // An unwarped resize under a long fade-in now ramps fully inside the span.
    Fixture plain(48000, 2, 4000, -1);
    plain.add(1, 0, 800, 0, 1000);
    plain.store->db().exec("UPDATE clips SET time_base=0,pos_ticks=0,length_ticks=192192,pos_ns=NULL,length_ns=NULL");
    check(commit(*plain.store, "clip.setFades", {{"id",1},{"in",{{"ticks",192192}}}}).ok,
          "author a fade-in over the entire unwarped clip");
    plain.load(1024);
    const auto render = [&] {
        plain.session.transport().locate(0);
        return plain.render(1024);
    };
    const auto before = render();
    check(commit(*plain.store, "clip.resize", {{"id",1},{"pos",0},{"length",96096}}).ok &&
              plain.session.refresh(*plain.store), "resize the unwarped clip to half under its fade-in");
    const auto shorter = render();
    check(std::abs(shorter[399] - 0.5f * 399.0f / 400.0f) < 1e-6f && shorter[400] == 0.0f &&
              fades(*plain.store) == std::array<std::int64_t, 3>{96096, 0, 96096},
          "the clamped fade-in ramps over the whole remaining span");
    check(History(*plain.store).undo().ok && plain.session.refresh(*plain.store) && render() == before,
          "Undo of the unwarped resize restores the entire authored fade-in exactly");

    // A fade-out clamped to the span renders exactly as the wider one it
    // replaced, also when the clip edges fall between samples (here at 0.4
    // and 400.6 frames, so the rounded span and the rounded edges differ).
    Fixture tail(48000, 2, 4000, -1);
    tail.add(1, 0, 800, 0, 1000);
    tail.store->db().exec("UPDATE clips SET time_base=0,pos_ticks=96,length_ticks=192192,pos_ns=NULL,length_ns=NULL");
    check(commit(*tail.store, "clip.setFades", {{"id",1},{"out",{{"ticks",192192}}}}).ok,
          "author a fade-out over the entire unaligned clip");
    tail.load(1024);
    check(commit(*tail.store, "clip.resize", {{"id",1},{"pos",96},{"length",96144}}).ok &&
              tail.session.refresh(*tail.store) && fades(*tail.store) == std::array<std::int64_t, 3>{0, 96144, 96144},
          "resize the unaligned clip under its fade-out clamps the fade-out to the span");
    const auto renderTail = [&] {
        tail.session.transport().locate(0);
        return tail.render(1024);
    };
    const auto clamped = renderTail();
    tail.store->db().exec("UPDATE clips SET fade_out_ticks=192192 WHERE id=1");
    check(tail.session.refresh(*tail.store), "reload the unclamped fade-out");
    const auto wider = renderTail();
    check(clamped == wider && clamped[0] > 0.0f && clamped[0] < 0.5f && clamped[399] > 0.0f && clamped[401] == 0.0f,
          "the clamped fade-out renders bit-exact as the wider one over the whole span");
}
void splitSourceClocks() {
    for(const auto sourceRate:{44100u,48000u})for(const bool loop:{false,true}) {
        Fixture f(sourceRate,2,4000);f.add(1,0,800,0,1000);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(24024,60,0)");
        f.store->db().exec("UPDATE clips SET content_offset_ticks=24024");
        if(loop)f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048");
        f.load(1024);const auto original=f.render(1024);
        const auto prepared=clip_edit::prepareSplit(*f.store,1,2,{1,ns(300)},48000);
        check(prepared.ok,"split captures unwarped source clocks across a tempo change");
        if(!prepared.ok){std::printf("split prepare: %s\\n",prepared.error.c_str());continue;}
        const auto committed=clip_edit::commitPrepared(*f.store,prepared);
        check(committed.ok,"prepared split commits as one journal transaction");
        if(!committed.ok){std::printf("split commit: %s\\n",committed.error.c_str());continue;}
        check(f.session.refresh(*f.store),"split refreshes playing session");
        f.session.transport().locate(0);const auto split=f.render(1024);
        check(split==original,"split preserves exact resampled PCM and loop phase on both sides");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store),"split undo rejoins original clip and removes source-clock rows");
        f.session.transport().locate(0);const auto restored=f.render(1024);
        check(restored==original,"split undo restores original rendered bytes");
        check(History(*f.store).redo().ok && f.session.refresh(*f.store),"split redo restores captured rows and clocks");
        f.session.transport().locate(0);const auto redone=f.render(1024);
        check(redone==original,"split redo remains sample exact");
    }
}
void splitInsideFades() {
    for(const auto width:{Precision::Float32,Precision::Float64})for(const std::int64_t cut:{50,750}) {
        Fixture f(48000,2,4000,-1);f.add(1,0,800,0,1000);
        f.store->db().exec("UPDATE clips SET fade_in_ticks=24024,fade_out_ticks=24024,fade_in_curve=2,fade_out_curve=5");
        f.store->db().exec("INSERT INTO clip_playback_details(clip_id,in_tension,out_tension) VALUES(1,0.4,-0.3)");
        SessionSpec spec{2,48000,1024};spec.precision=width;f.session.graph().setFadeFrames(0);
        check(f.session.load(*f.store,{},spec),"load original continuous fades at native width");
        std::vector<double> before(2048),after(2048);
        auto render=[&](std::vector<double>& result) {
            f.session.transport().locate(0);f.session.transport().play();
            if(!f.session.clips()->prime(std::chrono::seconds(2)))throw std::runtime_error("split fade prime");
            std::array<std::array<float,1024>,2> narrow{};std::array<std::array<double,1024>,2> wide{};
            float* out[]={narrow[0].data(),narrow[1].data()};double* out64[]={wide[0].data(),wide[1].data()};
            AudioIo io;io.numOut=2;io.frames=1024;
            if(width==Precision::Float64)io.out64=out64;else io.out=out;
            {audio::CallbackScope callback;f.session.process(io);}
            for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1024;++i)
                result[c*1024+i]=width==Precision::Float64?wide[c][i]:static_cast<double>(narrow[c][i]);
        };
        render(before);
        const auto prepared=clip_edit::prepareSplit(*f.store,1,2,{1,ns(cut)},48000);
        check(prepared.ok && clip_edit::commitPrepared(*f.store,prepared).ok && f.session.refresh(*f.store),"split inside an active fade commits and rebuilds");
        render(after);check(before==after,"split preserves exact original fade gain on both sides of its cut");
        OpRequest reset;reset.opType="clip.setFades";reset.payload={{"id",2},{"in",{{"ticks",0}}},{"out",{{"ticks",0}}}};
        check(OpJournal(*f.store).commit(reset).ok && History(*f.store).undo().ok && f.session.refresh(*f.store),"fade handle edit and undo restore retained envelope clocks");
        render(after);check(before==after,"undo of fade handle restores the split envelope exactly");
    }
}

std::vector<double> renderSplitPcm(Fixture& f,Precision width,std::int32_t frames) {
    if(frames<=0)return {};
    f.session.transport().locate(0);f.session.transport().play();
    // The edit ranges below are in timeline frames. Render through the
    // graph's physical latency and compare the same audible timeline range.
    const auto* graph=f.session.graph().currentGraph();
    const auto latency=graph ? graph->latencySamples() : 0;
    const auto renderFrames=static_cast<std::int64_t>(frames)+latency;
    const auto total=static_cast<std::size_t>(frames);
    std::vector<double> result(total*2);
    std::array<float,2048> narrow{};std::array<double,2048> wide{};
    float* out[]={narrow.data(),narrow.data()+1024};double* out64[]={wide.data(),wide.data()+1024};
    for(std::int64_t first=0;first<renderFrames;) {
        const auto count=static_cast<std::int32_t>(std::min<std::int64_t>(1024,renderFrames-first));
        if(!f.session.clips() || !f.session.clips()->prime(std::chrono::seconds(5)))return {};
        AudioIo io;io.numOut=2;io.frames=count;
        if(width==Precision::Float64)io.out64=out64;else io.out=out;
        {audio::CallbackScope callback;f.session.process(io);}
        for(std::size_t c=0;c<2;++c)for(std::int32_t i=0;i<count;++i) {
            const auto timeline=first+i-latency;
            if(timeline<0)continue;
            const auto offset=static_cast<std::size_t>(i);
            result[c*total+static_cast<std::size_t>(timeline)]=width==Precision::Float64?
                wide[c*1024+offset]:static_cast<double>(narrow[c*1024+offset]);
        }
        first+=count;
    }
    return result;
}
bool twoSplitAudioRoundTrip(Fixture& f,Precision width,std::int32_t frames,
                           std::int32_t clipBegin,std::int32_t clipEnd,
                           std::int32_t firstCut,std::int32_t lastCut,
                           clip_edit::TimePoint begin,clip_edit::TimePoint end) {
    SessionSpec spec{2,48000,1024};spec.precision=width;f.session.graph().setFadeFrames(0);
    const bool loaded=f.session.load(*f.store,{},spec);
    check(loaded,"load native audio for one two-point split");if(!loaded)return false;
    const auto original=renderSplitPcm(f,width,frames);
    const auto total=static_cast<std::size_t>(frames);
    bool baseline=original.size()==total*2;
    if(baseline)for(std::size_t c=0;c<2;++c) {
        for(const auto cut:{firstCut,lastCut})for(const auto offset:{-16,0}) {
            const auto at=static_cast<std::size_t>(cut+offset);
            baseline=baseline && std::any_of(original.begin()+static_cast<std::ptrdiff_t>(c*total+at),
                original.begin()+static_cast<std::ptrdiff_t>(c*total+at+16),[](double value){return std::abs(value)>1e-9;});
        }
        for(std::int32_t i=0;i<frames;++i)
            if(i<clipBegin || i>=clipEnd)baseline=baseline && original[c*total+static_cast<std::size_t>(i)]==0;
    }
    check(baseline && f.session.clips()->underrunSamples()==0,
          "two-point baseline is audible on both sides of both cuts and silent outside its placement");
    if(!baseline)return false;
    const auto prepared=clip_edit::prepareSplit(*f.store,1,2,3,begin,end,48000);
    check(prepared.ok,"prepare both audio cuts from one original image");if(!prepared.ok)return false;
    const bool committed=clip_edit::commitPrepared(*f.store,prepared).ok && f.session.refresh(*f.store);
    check(committed,"commit and refresh both audio cuts as one edit");if(!committed)return false;
    check(f.store->db().execAndGet("SELECT count(*) FROM clips WHERE id IN (1,2,3)").getInt()==3,
          "two-point audio split creates both caller-identified pieces");
    const auto split=renderSplitPcm(f,width,frames);
    check(split.size()==original.size() && split==original,
          "two cuts preserve every native PCM sample, source phase and original fade envelope");
    const bool undone=History(*f.store).undo().ok && f.session.refresh(*f.store);
    check(undone && f.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==1,
          "one audio undo removes both created pieces");if(!undone)return false;
    const auto restored=renderSplitPcm(f,width,frames);
    check(restored.size()==original.size() && restored==original,"one audio undo restores all original PCM exactly");
    const bool redone=History(*f.store).redo().ok && f.session.refresh(*f.store);
    check(redone && f.store->db().execAndGet("SELECT count(*) FROM clips WHERE id IN (1,2,3)").getInt()==3,
          "one audio redo restores both captured pieces");if(!redone)return false;
    const auto replayed=renderSplitPcm(f,width,frames);
    check(replayed.size()==original.size() && replayed==original,"one audio redo restores all split PCM exactly");
    return true;
}
void twoPointSplitSourceClocks() {
    for(const auto sourceRate:{44100u,48000u})for(const auto width:{Precision::Float32,Precision::Float64})for(const bool loop:{false,true}) {
        Fixture f(sourceRate,2,4000);f.add(1,73,800,11,2000);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(72072,60,0);"
            "UPDATE clips SET content_offset_ticks=12012,fade_in_ticks=96096,fade_out_ticks=24024,fade_in_curve=2,fade_out_curve=5;"
            "INSERT INTO clip_playback_details(clip_id,in_tension,out_tension) VALUES(1,0.4,-0.3)");
        if(loop)f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048");
        (void)twoSplitAudioRoundTrip(f,width,1024,73,873,130,816,{1,ns(130)},{1,ns(816)});
    }
}
void twoPointSplitWarpFades() {
    constexpr auto quarter=textproj::kPPQ;
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f(48000,2,12000,440);f.add(1,0,48000,0,12000);
        auto& db=f.store->db();
        db.exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0);"
            "UPDATE clips SET time_base=0,pos_ticks=0,length_ticks=11531520,pos_ns=NULL,length_ns=NULL,"
            "loop_enabled=1,loop_start_ticks=0,loop_len_ticks=5765760,fade_in_ticks=8648640,fade_out_ticks=8648640,fade_in_curve=2,fade_out_curve=4;"
            "UPDATE audio_clips SET warp_enabled=1,warp_mode='complex';"
            "INSERT INTO clip_playback_details(clip_id,in_tension,out_tension) VALUES(1,0.35,-0.4)");
        const std::array<WarpMarker,2> markers{{{0,0},{12000,quarter}}};
        const auto map=audio::writeWarpMap(markers,12000);
        check(static_cast<bool>(map),"prepare the original two-point Warp marker map");if(!map)continue;
        SQLite::Statement setMap(db,"UPDATE audio_clips SET warp_markers=? WHERE clip_id=1");
        setMap.bind(1,map.bytes.data(),static_cast<int>(map.bytes.size()));setMap.exec();
        const auto originalMap=db.execAndGet("SELECT hex(warp_markers) FROM audio_clips WHERE clip_id=1").getString();
        if(!twoSplitAudioRoundTrip(f,width,50000,0,48000,12000,18000,{0,quarter/2},{0,3*quarter/4}))continue;
        check(db.execAndGet("SELECT count(*) FROM clip_warp_fade_domains WHERE clip_id IN (1,2,3) AND "
            "in_begin_ticks=0 AND in_end_ticks=8648640 AND out_begin_ticks=2882880 AND out_end_ticks=11531520").getInt()==3,
            "both Warp cuts retain the first image's complete original fade domains");
        bool sameMap=true;
        SQLite::Statement maps(db,"SELECT hex(warp_markers) FROM audio_clips ORDER BY clip_id");
        while(maps.executeStep())sameMap=sameMap && maps.getColumn(0).getString()==originalMap;
        check(sameMap,"two-point Warp split and one redo retain the original marker bytes");
    }
}

void rangeSurvivorPcm(std::uint32_t sourceRate,Precision width,bool loop,bool wholeTime) {
    Fixture f(sourceRate,2,4000);f.add(1,100,800,11,2000);
    auto& db=f.store->db();
    db.exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(24024,60,0);"
        "UPDATE clips SET content_offset_ticks=24024,fade_in_ticks=96096,fade_out_ticks=24024,fade_in_curve=2,fade_out_curve=5;"
        "INSERT INTO clip_playback_details(clip_id,in_tension,out_tension) VALUES(1,0.4,-0.3)");
    if(loop)db.exec("UPDATE clips SET time_base=0,pos_ticks=24024,length_ticks=96096,pos_ns=NULL,length_ns=NULL,"
        "loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048");
    SessionSpec spec{2,48000,2048};spec.precision=width;f.session.graph().setFadeFrames(0);
    const bool loaded=f.session.load(*f.store,{},spec);
    check(loaded,"load a faded source whose only range survivor will be its tail");if(!loaded)return;
    auto render=[&](std::int32_t frames) {
        f.session.transport().locate(0);f.session.transport().play();
        if(!f.session.clips()->prime(std::chrono::seconds(2)))return std::vector<double>{};
        const auto count=static_cast<std::size_t>(frames);
        std::vector<double> result(count*2);std::vector<float> narrow(count*2);
        float* out[]={narrow.data(),narrow.data()+frames};double* out64[]={result.data(),result.data()+frames};
        AudioIo io;io.numOut=2;io.frames=frames;if(width==Precision::Float64)io.out64=out64;else io.out=out;
        {audio::CallbackScope callback;f.session.process(io);}
        if(width==Precision::Float32)std::copy(narrow.begin(),narrow.end(),result.begin());
        return result;
    };
    const auto original=render(1200);
    bool audible=original.size()==2400;
    if(audible)for(std::size_t channel=0;channel<2;++channel) {
        const auto base=channel*1200;
        audible=audible && original[base+299]!=0 && original[base+300]!=0 && original[base+750]!=0;
        for(std::size_t frame=0;frame<1200;++frame)
            if(frame<100 || frame>=900)audible=audible && original[base+frame]==0;
    }
    check(audible && f.session.clips()->underrunSamples()==0,
          "sole-tail baseline has real audio at the retained edge and original fade-out, with exact placement silence");
    if(!audible)return;
    const clip_edit::RangeSelection selection{{1},{1,ns(50)},{1,ns(300)}};
    clip_edit::PreparedEdit edit;
    if(wholeTime)edit=clip_edit::prepareDeleteTime(*f.store,12012,48048,48000);
    else if(loop) {
        auto cut=clip_edit::prepareCutRange(*f.store,selection,48000);
        check(cut.copy.ok && cut.copy.images.size()==1,"Cut captures only the removed material before preserving its original-ID tail");
        if(!cut.copy.ok || cut.copy.images.size()!=1)return;
        edit=std::move(cut.edit);
    } else edit=clip_edit::prepareDeleteRange(*f.store,selection,48000);
    check(edit.ok,"prepare a sole-tail material or whole-Time removal");if(!edit.ok)return;
    const bool committed=clip_edit::commitPrepared(*f.store,edit).ok && f.session.refresh(*f.store);
    check(committed,"commit and refresh one sole-tail removal");if(!committed)return;
    auto sameIdentity=[&] {
        return db.execAndGet("SELECT count(*) FROM clips").getInt()==1 &&
            db.execAndGet("SELECT count(*) FROM clips WHERE id=1").getInt()==1 &&
            db.execAndGet("SELECT count(*) FROM audio_clips WHERE clip_id=1 AND media_id=1 AND src_start_frames=11 AND src_len_frames=2000").getInt()==1;
    };
    check(sameIdentity(),"the sole audible tail keeps its original clip and audio-source row identity");
    const std::int32_t frames=wholeTime?950:1200;
    std::vector<double> expected(static_cast<std::size_t>(frames)*2);
    for(std::size_t channel=0;channel<2;++channel)for(std::int32_t frame=0;frame<frames;++frame) {
        const auto source=wholeTime && frame>=50?frame+250:frame;
        expected[channel*static_cast<std::size_t>(frames)+static_cast<std::size_t>(frame)]=
            !wholeTime && frame>=50 && frame<300?0:original[channel*1200+static_cast<std::size_t>(source)];
    }
    const auto after=render(frames);
    check(after.size()==expected.size() && after==expected,wholeTime?
        "Delete Time shifts the original suffix by exactly the removed duration, preserving source phase, loop and fades":
        "material Delete/Cut silences only the selected interval and keeps the original suffix at its exact timeline samples");
    const bool undone=History(*f.store).undo().ok && f.session.refresh(*f.store);
    check(undone && sameIdentity(),"one sole-tail undo restores the original clip and source identities");if(!undone)return;
    const auto restored=render(1200);
    check(restored.size()==original.size() && restored==original,"one sole-tail undo restores every original native PCM sample");
    const bool redone=History(*f.store).redo().ok && f.session.refresh(*f.store);
    check(redone && sameIdentity(),"one sole-tail redo retains the captured original identities");if(!redone)return;
    const auto replayed=render(frames);
    check(replayed.size()==expected.size() && replayed==expected,"one sole-tail redo restores the exact retained PCM and original envelope");
}
void rangeSurvivorSourceClocks() {
    for(const auto sourceRate:{44100u,48000u})for(const auto width:{Precision::Float32,Precision::Float64}) {
        // Delete a linear prefix; Cut a musical, looped prefix. Their shared
        // trim receives both clocks without duplicating the full audio matrix.
        for(const bool loop:{false,true})rangeSurvivorPcm(sourceRate,width,loop,false);
        rangeSurvivorPcm(sourceRate,width,true,true); // whole-Time removal also relocates the surviving tail
    }
}
void silenceSourceClocks() {
    for(const auto sourceRate:{44100u,48000u}) {
        Fixture f(sourceRate,2,4000);f.add(1,0,800,0,1000);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(24024,60)");
        f.store->db().exec("UPDATE clips SET content_offset_ticks=24024,fade_in_ticks=96096,fade_out_ticks=24024");
        f.load(2048);const auto original=f.render(1200);
        const auto prepared=clip_edit::prepareInsertSilence(*f.store,48048,12012,48000);
        check(prepared.ok && clip_edit::commitPrepared(*f.store,prepared).ok && f.session.refresh(*f.store),"Insert Silence splits and moves a faded resampled source");
        f.session.transport().locate(0);const auto shifted=f.render(1300);
        bool exact=true;
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1300;++i) {
            const float expected=i>=300 && i<400?0:original[c*1200+(i<300?i:i-100)];
            exact=exact && shifted[c*1300+i]==expected;
        }
        check(exact,"Insert Silence preserves source samples and fade envelopes exactly around the silent gap");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store),"undo Insert Silence refreshes original source");
        f.session.transport().locate(0);check(f.render(1200)==original,"Insert Silence undo restores original PCM bytes");
    }
}

void deleteTimeSourceClocks() {
    for(const auto sourceRate:{44100u,48000u})for(const auto width:{Precision::Float32,Precision::Float64})for(const bool loop:{false,true}) {
        Fixture f(sourceRate,2,4000);f.add(1,0,800,0,2000);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(24024,60)");
        f.store->db().exec("UPDATE clips SET content_offset_ticks=24024,fade_in_ticks=96096,fade_out_ticks=24024");
        if(loop)f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048");
        SessionSpec spec{2,48000,2048};spec.precision=width;f.session.graph().setFadeFrames(0);
        check(f.session.load(*f.store,{},spec),"load Delete Time source at native precision");
        auto render=[&](int frames) {
            f.session.transport().locate(0);f.session.transport().play();
            if(!f.session.clips()->prime(std::chrono::seconds(2)))throw std::runtime_error("Delete Time prime");
            std::vector<double> result(static_cast<std::size_t>(frames)*2);
            std::vector<float> narrow(static_cast<std::size_t>(frames)*2);
            float* out[]={narrow.data(),narrow.data()+frames};double* out64[]={result.data(),result.data()+frames};
            AudioIo io;io.numOut=2;io.frames=frames;
            if(width==Precision::Float64)io.out64=out64;else io.out=out;
            {audio::CallbackScope callback;f.session.process(io);}
            if(width==Precision::Float32)std::copy(narrow.begin(),narrow.end(),result.begin());
            return result;
        };
        const auto original=render(1200);
        const auto prepared=clip_edit::prepareDeleteTime(*f.store,48048,72072,48000);
        check(prepared.ok && clip_edit::commitPrepared(*f.store,prepared).ok && f.session.refresh(*f.store),"Delete Time trims a resampled source and retained fade domains");
        const auto after=render(1000);bool exact=true;
        for(std::size_t channel=0;channel<2;++channel)for(std::size_t position=0;position<1000;++position)
            exact=exact && after[channel*1000+position]==original[channel*1200+(position<300?position:position+200)];
        check(exact,"heard samples equal the original with only the selected time removed, both precisions and looping sources");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store) && render(1200)==original,"Delete Time undo restores the exact rendered PCM");
        check(History(*f.store).redo().ok && f.session.refresh(*f.store) && render(1000)==after,"Delete Time redo restores the exact captured source clocks");
    }
}

void timeClipboardSourceClocks() {
    for(const auto sourceRate:{44100u,48000u})for(const auto width:{Precision::Float32,Precision::Float64})for(const bool loop:{false,true}) {
        Fixture f(sourceRate,2,4000);f.add(1,0,800,0,2000);f.add(2,1100,200,2000,500);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(24024,60),(96096,240)");
        f.store->db().exec("UPDATE clips SET fade_in_ticks=48048,fade_out_ticks=12012 WHERE id=1");
        if(loop)f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048 WHERE id=1");
        SessionSpec spec{2,48000,2048};spec.precision=width;f.session.graph().setFadeFrames(0);
        check(f.session.load(*f.store,{},spec),"load whole-Time clipboard sources at native precision");
        auto render=[&](int frames) {
            f.session.transport().locate(0);f.session.transport().play();
            if(!f.session.clips()->prime(std::chrono::seconds(2)))throw std::runtime_error("Time clipboard prime");
            std::vector<double> result(static_cast<std::size_t>(frames)*2);std::vector<float> narrow(static_cast<std::size_t>(frames)*2);
            float* out[]={narrow.data(),narrow.data()+frames};double* out64[]={result.data(),result.data()+frames};
            AudioIo io;io.numOut=2;io.frames=frames;if(width==Precision::Float64)io.out64=out64;else io.out=out;
            {audio::CallbackScope callback;f.session.process(io);}
            if(width==Precision::Float32)std::copy(narrow.begin(),narrow.end(),result.begin());
            return result;
        };
        const auto original=render(1600);const auto copy=clip_edit::copyTime(*f.store,24024,72072,48000);
        check(copy.ok && copy.nanos==8333334,"copy physical duration at source tempo");
        const auto paste=clip_edit::preparePasteTime(*f.store,copy,144144,48000);
        const auto committed=clip_edit::commitPrepared(*f.store,paste);
        const bool refreshed=committed.ok && f.session.refresh(*f.store);
        if(!paste.ok)std::printf("Time clipboard prepare: %s\n",paste.error.c_str());
        if(!committed.ok)std::printf("Time clipboard commit: %s\n",committed.error.c_str());
        if(committed.ok && !refreshed)std::printf("Time clipboard refresh: %s\n",f.session.error().c_str());
        check(paste.ok && committed.ok && refreshed,"paste source tempo, faded material and global context as one step");
        const auto after=render(2000);bool exact=true;
        for(std::size_t channel=0;channel<2;++channel)for(std::size_t position=0;position<2000;++position) {
            const auto source=position<800?position:position<1200?position-800+100:position-400;
            exact=exact && after[channel*2000+position]==original[channel*1600+source];
        }
        check(exact,"heard Paste Time uses 400 copied frames rather than 100 target-tempo frames, with exact faded/resampled/looped PCM");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store) && render(1600)==original,"Time paste undo restores original heard bytes");
        check(History(*f.store).redo().ok && f.session.refresh(*f.store) && render(2000)==after,"Time paste redo restores captured source clocks and map context");
    }
}

void sourceMarkers() {
    auto set = [](Store& store, const char* name, Payload payload) {
        OpRequest op; op.opType=name;op.payload=std::move(payload);
        if (!OpJournal(store).commit(op).ok) throw std::runtime_error("clip property op failed");
    };
    Fixture plain;plain.add(1,0,500,7,1000);
    set(*plain.store,"clip.setStartMarker",{{"id",1},{"ticks",12012}}); // source sample 50
    set(*plain.store,"clip.setEndMarker",{{"id",1},{"ticks",48048}}); // source sample 200
    plain.load(512);const auto out=plain.render(512);
    check(out[0]==plain.media[114] && out[149]==plain.media[412] && out[150]==0 && out[499]==0,
          "clip Start/End ops gate audio at exact source samples without moving placement");
    check(plain.session.setPrecision(Precision::Float64),"source markers switch to native double graph");
    plain.session.transport().locate(0);const auto wide=plain.render(512);
    check(wide[0]==plain.media[114] && wide[149]==plain.media[412] && wide[150]==0,
          "source End also gates a native double graph at the exact sample");
    Fixture loop;loop.add(1,0,500,7,1000);
    set(*loop.store,"clip.setStartMarker",{{"id",1},{"ticks",12012}});
    set(*loop.store,"clip.setEndMarker",{{"id",1},{"ticks",48048}});
    set(*loop.store,"clip.setLoop",{{"id",1},{"on",true},{"start",24024},{"length",48048}});
    loop.load(512);const auto repeated=loop.render(512);
    check(repeated[150]==loop.media[414] && repeated[250]==loop.media[214] && repeated[500]==0,
          "Loop uses its own source window while the End marker is inactive");
}
void editPinnedSourceMarkers() {
    Fixture f(48000, 2, 4000);
    f.add(1, 600, 1000, 0, 2500);
    auto& db = f.store->db();
    db.exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,240);"
            "UPDATE clips SET content_offset_ticks=24024 WHERE id=1;"
            "INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(1,120120);"
            "INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns)"
            " VALUES(1,2083333,10416667)");
    const auto markers = [&] {
        SQLite::Statement q(db, "SELECT content_offset_ticks,end_marker_ticks,"
            " COALESCE(content_offset_ns,-1),COALESCE(source_end_ns,-1) FROM clips"
            " JOIN clip_properties ON clip_properties.clip_id=clips.id"
            " JOIN clip_playback_details ON clip_playback_details.clip_id=clips.id WHERE clips.id=1");
        if (!q.executeStep()) throw std::runtime_error("no clip marker state");
        return std::array{q.getColumn(0).getInt64(), q.getColumn(1).getInt64(),
                          q.getColumn(2).getInt64(), q.getColumn(3).getInt64()};
    };
    OpJournal journal(*f.store);
    History history(*f.store);
    const auto move = [&](const char* type, std::int64_t tick) {
        OpRequest op;
        op.opType = type;
        op.payload = {{"id", 1}, {"ticks", tick}};
        return journal.commit(op).ok;
    };
    const auto original = markers();
    const bool movedStart = move("clip.setStartMarker", 96096);
    const auto start = markers();
    check(movedStart && start == std::array<std::int64_t, 4>{96096,240240,4166667,10416667},
          "a Start edit projects both displayed markers and preserves the audible End pin");
    const bool movedEnd = move("clip.setEndMarker", 192192);
    const auto end = markers();
    check(movedEnd && end == std::array<std::int64_t, 4>{96096,192192,4166667,8333333},
          "an End edit pins its own source time and keeps the physical Start");
    check(history.undo().ok && markers() == start && history.undo().ok && markers() == original,
          "Undo restores both captured source-marker clocks and the earlier display values");
    check(history.redo().ok && markers() == start && history.redo().ok && markers() == end,
          "Redo rebuilds the synchronized markers through the journal");

    db.exec("UPDATE tempo_map SET bpm=120 WHERE pos_ticks=0");
    f.load(1024);
    f.session.transport().locate(600);
    const auto audio = f.render(1024);
    check(audio[0] == f.media[400] && audio[199] == f.media[798] && audio[200] == 0,
          "an edited Start and End stay sample-exact when the placement tempo changes again");
}
void endSourceClocks() {
    Fixture plain(48000, 2, 4000, -1);
    plain.add(1, 600, 800, 0, 1000);
    plain.store->db().exec("INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0);"
        "INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(1,120120);"
        "INSERT INTO clip_playback_details(clip_id,source_end_ns) VALUES(1,10416667);"
        "UPDATE tempo_map SET bpm=240 WHERE pos_ticks=0");
    plain.load(1024);
    plain.session.transport().locate(600);
    const auto pinned = plain.render(1024);
    check(pinned[249] == 0.5f && pinned[499] == 0.5f && pinned[500] == 0.0f,
          "a physical End pin keeps 500 samples when the placement tempo doubles");

    Fixture warped(48000, 2, 4000, -1);
    warped.add(1, 600, 800, 0, 2000);
    auto& db = warped.store->db();
    db.exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(144144,240,0);"
        "UPDATE clips SET time_base=0,pos_ticks=144144,length_ticks=384384,pos_ns=NULL,length_ns=NULL,"
        "content_offset_ticks=72072;"
        "INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(1,144144);"
        "UPDATE audio_clips SET warp_enabled=1,warp_mode='re-pitch'");
    const std::array<WarpMarker, 2> markers{{{0, 0}, {2000, 480480}}};
    const auto map = audio::writeWarpMap(markers, 2000);
    if (!map) throw std::runtime_error("prepare End clock test Warp map");
    SQLite::Statement setMap(db, "UPDATE audio_clips SET warp_markers=? WHERE clip_id=1");
    setMap.bind(1, map.bytes.data(), static_cast<int>(map.bytes.size()));
    setMap.exec();
    warped.load(1024);
    warped.session.transport().locate(600);
    const auto retimed = warped.render(1024);
    check(std::abs(retimed[0]) > 0.1f && std::abs(retimed[149]) > 0.1f && retimed[150] == 0.0f,
          "Warp End retains the 150-frame tail projected across its source tempo boundary");
}

void absoluteLinearEndSamples() {
    const auto checkEnd = [](std::int64_t lengthNs, std::int64_t startTicks, std::int64_t endTicks,
                            std::optional<std::int64_t> physicalEnd, int offset, int frames,
                            const char* message) {
        Fixture fixture(48000, 2, 4000);
        fixture.add(1, 0, 1000, 0, 2000);
        auto& db = fixture.store->db();
        SQLite::Statement placement(db,
            "UPDATE clips SET pos_ns=108333,length_ns=?,content_offset_ticks=? WHERE id=1");
        placement.bind(1, lengthNs);
        placement.bind(2, startTicks);
        placement.exec();
        SQLite::Statement marker(db, "INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(1,?)");
        marker.bind(1, endTicks);
        marker.exec();
        if (physicalEnd) {
            SQLite::Statement pin(db, "INSERT INTO clip_playback_details(clip_id,source_end_ns) VALUES(1,?)");
            pin.bind(1, *physicalEnd);
            pin.exec();
        }
        fixture.load(1024);
        if (!fixture.session.setPrecision(Precision::Float64)) throw std::runtime_error("wide End fixture");
        const auto heard = fixture.render(1024);
        const auto begin = std::size_t{5};
        const auto count = static_cast<std::size_t>(frames);
        const auto source = static_cast<std::size_t>(offset);
        check(heard[begin - 1] == 0 && heard[begin] == fixture.media[source * 2] &&
                  heard[begin + count - 1] == fixture.media[(source + count - 1) * 2] &&
                  heard[begin + count] == 0,
              message);
    };
    checkEnd(6250000, 0, 48144, std::nullopt, 0, 201,
             "an early linear End marker counts from the two absolute sample positions");
    checkEnd(10360417, 24024, 143495, std::nullopt, 100, 498,
             "a marker mirroring a linear clip's stored End keeps its exact half-sample endpoint");
    checkEnd(6250000, 0, 48144, 4175000, 0, 200,
             "a physical source End keeps its source sample count at a fractional placement");
}

void seeksAndStalls() {
    Fixture f;f.add(1,0,90000,0,90000);f.load(4096);(void)f.render(4096);
    f.session.transport().locate(70003);
    const bool refilled=f.session.clips()->prime(std::chrono::seconds(2));
    check(refilled,"locate requests a refill before rendering");
    if(!refilled)return;
    auto o=f.render(4096);
    check(o[0]==f.media[140006] && o[4095]==f.media[148196],"locate refills distant uncached page");
    f.session.transport().loop(17003,72003);f.session.transport().locate(71900);o=f.render(4096);
    check(o[102]==f.media[144004] && o[103]==f.media[34006] && f.session.transport().position()==20996,"transport loop wraps sample-exactly mid block");
    f.session.transport().play(false);const auto pos=f.session.transport().position();o=f.render(64);
    check(std::all_of(o.begin(),o.end(),[](float x){return x==0;}) && f.session.transport().position()==pos,"stopped transport is silent and stationary");
    f.session.clips()->stallForTest(true);std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.session.transport().loop(0,0,false);f.session.transport().locate(55000);f.session.transport().play();
    const auto before=f.session.clips()->underrunSamples();const auto start=std::chrono::steady_clock::now();o=f.render(64,false);
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    check(std::all_of(o.begin(),o.end(),[](float x){return x==0;}) && f.session.clips()->underrunSamples()==before+64,"stalled stream emits silence and counts missing samples");
    check(ms<50,"underrun callback never waits for worker");
    f.session.clips()->stallForTest(false);o=f.render(64);
    check(o[0]==f.media[110128],"stream resumes after stall at current playhead");
}
void conversion() {
    for(const auto rate:{44100u,96000u}) {
        Fixture f(rate,1,rate*2,997);f.add(1,0,96000,0,rate*2);f.load(4096);
        double squared=0,peak=0;std::size_t count=0;
        for(int at=0;at<90000;at+=4096) {
            auto o=f.render(4096);
            for(int i=0;i<4096;++i) {
                if(at+i<512)continue;
                const double want=0.5*std::sin(2*std::numbers::pi*997*(at+i)/48000);
                const double error=static_cast<double>(o[static_cast<std::size_t>(i)])-want;
                squared+=error*error;peak=std::max(peak,std::abs(error));++count;
            }
        }
        const double rms=std::sqrt(squared/static_cast<double>(count));
        std::printf("SRC %u->48000 997Hz RMS %.9g peak %.9g SNR %.3f dB\n",rate,rms,peak,20*std::log10((0.5/std::sqrt(2.0))/rms));
        check(rms<1e-5 && peak<1e-4,"sine conversion accurate including page seams");
        f.session.transport().locate(37001);const auto o=f.render(64);
        check(std::abs(o[0]-0.5*std::sin(2*std::numbers::pi*997*37001/48000))<1e-4,"resampler seek preserves absolute phase");
    }
    Fixture alias(96000,1,192000,30000);alias.add(1,0,96000,0,192000);alias.load(4096);alias.session.transport().locate(10000);auto o=alias.render(4096);
    double energy=0;for(int i=0;i<4096;++i)energy+=static_cast<double>(o[static_cast<std::size_t>(i)])*o[static_cast<std::size_t>(i)];
    const double rms=std::sqrt(energy/4096);std::printf("SRC 96000->48000 30kHz rejection %.3f dB\n",20*std::log10((0.5/std::sqrt(2.0))/rms));
    check(rms<1e-5,"downsampling rejects above-Nyquist sine");
}

void stereoConversionMatchesSinc() {
    constexpr std::uint32_t frames = 62000;
    Fixture stereo(44100, 2, frames);
    Fixture left(44100, 1, frames);
    Fixture right(44100, 1, frames, 19999);
    std::uint32_t random = 1234;
    for (std::size_t i = 0; i < frames; ++i) {
        random = random * 1664525u + 1013904223u;
        left.media[i] = static_cast<float>(random >> 16) / 65536.0f - 0.5f;
        stereo.media[2 * i] = left.media[i];
        stereo.media[2 * i + 1] = right.media[i];
    }
    for (auto* f : {&stereo, &left, &right}) {
        const auto channels = static_cast<std::uint16_t>(f == &stereo ? 2 : 1);
        audio::WavWriter wav(f->dir.path() / "audio.wav", 44100, channels, audio::WavFormat::Float32);
        wav.write(f->media.data(), frames);
        wav.close();
        f->add(1, 0, 65000, 17, 60000);
        f->load(1024);
    }
    float peak = 0;
    for (const int start : {0, 8191, 37123, 64900}) {
        for (auto* f : {&stereo, &left, &right}) f->session.transport().locate(start);
        const auto combined = stereo.render(1024);
        const auto monoLeft = left.render(1024);
        const auto monoRight = right.render(1024);
        for (std::size_t i = 0; i < 1024; ++i)
            peak = std::max({peak, std::abs(combined[i] - monoLeft[i]), std::abs(combined[1024 + i] - monoRight[i])});
    }
    std::printf("SRC 44100->48000 stereo vs mono sinc-best peak %.9g\n", static_cast<double>(peak));
    check(peak < 5e-7f, "stereo conversion matches sinc-best at the start, seams, seeks, and tail");
}

// Run explicitly in a Release build. A shared CI runner cannot guarantee the
// callback cadence of a device, so keep wall-clock deadlines out of the suite.
void realtimeConversion() {
    constexpr int tracks = 32, block = 256, callbacks = 480;
    for (const auto sourceRate : {48000u, 44100u}) {
        Fixture f(sourceRate, 2, 4 * sourceRate, 997);
        auto& db = f.store->db();
        SQLite::Statement track(db, "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(?,'audio','Audio',?)");
        SQLite::Statement media(db, "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(?,?,?,'wav')");
        SQLite::Statement assign(db, "UPDATE clips SET track_id=? WHERE id=?");
        SQLite::Statement source(db, "UPDATE audio_clips SET media_id=? WHERE clip_id=?");
        for (int i = 0; i < tracks; ++i) {
            const int clipId = i + 1;
            const int trackId = i + 100;
            const auto path = "track-" + std::to_string(clipId) + ".wav";
            fs::copy_file(f.dir.path() / "audio.wav", f.dir.path() / path);
            track.bind(1, trackId); track.bind(2, trackId); track.exec(); track.reset();
            media.bind(1, trackId); media.bind(2, "test-" + std::to_string(clipId));
            media.bind(3, path); media.exec(); media.reset();
            f.add(clipId, 0, 4 * 48000, 0, 4 * sourceRate);
            assign.bind(1, trackId); assign.bind(2, clipId); assign.exec(); assign.reset();
            source.bind(1, trackId); source.bind(2, clipId); source.exec(); source.reset();
        }
        f.load(block);
        const auto primeStart = std::chrono::steady_clock::now();
        if (!f.session.clips()->prime(std::chrono::seconds(5))) throw std::runtime_error("could not prime 32-track project");
        const double primeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - primeStart).count();
        std::array<float, block * 2> pcm{};
        float* out[] = {pcm.data(), pcm.data() + block};
        const auto process = [&] {
            const audio::CallbackScope callback;
            f.session.process({out, nullptr, 2, 0, block, 0});
        };
        // A running device reads ahead at the stopped playhead. Start at a
        // different, cold position and give it 250 ms before pressing Play.
        f.session.transport().play(false);
        f.session.transport().locate(48000);
        const auto stopped = std::chrono::steady_clock::now();
        for (int i = 0; i < 47; ++i) {
            std::this_thread::sleep_until(stopped + std::chrono::duration<double>(static_cast<double>(i * block) / 48000));
            process();
        }
        std::this_thread::sleep_until(stopped + std::chrono::milliseconds(250));
        f.session.transport().play();
        int gaps = 0, firstSecondGaps = 0;
        bool heardAudio = false;
        auto underruns = f.session.clips()->underrunSamples();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < callbacks; ++i) {
            std::this_thread::sleep_until(start + std::chrono::duration<double>(static_cast<double>(i * block) / 48000));
            process();
            heardAudio = heardAudio || std::abs(pcm[block - 1]) > 0.01f;
            const auto next = f.session.clips()->underrunSamples();
            if (next != underruns) {
                ++gaps;
                if (i * block < 48000) ++firstSecondGaps;
            }
            underruns = next;
        }
        std::printf("SC-407 %u->48000, %d tracks: initial prime %.1f ms, %d/%d callbacks with gaps (%d in first second), %u missing clip frames\n",
                    sourceRate, tracks, primeMs, gaps, callbacks, firstSecondGaps, underruns);
        check(heardAudio && f.session.transport().position() == 48000 + callbacks * block,
              "the benchmark drives the playing graph at the requested device cadence");
        check(gaps == 0 && f.session.clips()->readErrors() == 0, "32 tracks sustain the device callback rate without gaps");
    }
}
void refreshAndVariableBlocks() {
    Fixture f;f.add(1,31,70000,0,70000);f.load(4096);
    bool exact=true;int at=0;
    for(int round=0;round<5;++round)for(const int n:{64,137,1024,4096}) {
        const auto out=f.render(n);
        for(int i=0;i<n;++i) {
            const float want=at+i>=31?f.media[static_cast<std::size_t>(at+i-31)*2]:0;
            exact=exact && out[static_cast<std::size_t>(i)]==want;
        }
        at+=n;
    }
    check(exact,"variable callback sizes preserve exact positions");
    f.store->db().exec("UPDATE clips SET gain_db=-6.020599913279624");
    check(f.session.refresh(*f.store),"refresh publishes new clip sources");
    auto out=f.render(64);
    check(std::abs(out[0]-f.media[static_cast<std::size_t>(at-31)*2]*0.5f)<1e-6f,"refreshed clip gain reaches published graph");
    f.session.tick(1000); // reclaim old graph and its worker on this thread
    f.session.prepare(48000,128);out=f.render(128);
    check(std::abs(out[0]-f.media[static_cast<std::size_t>(at+64-31)*2]*0.5f)<1e-6f,"block reprepare preserves playhead and clip source");
    // Producer (message thread) publishes while driver consumes. Do not touch
    // Transport or prime from this thread while that driver is running.
    std::atomic<bool> stop{false};
    std::thread driver([&]{
        std::array<float,256> samples{};float* ptrs[]={samples.data(),samples.data()+128};
        while(!stop.load()) f.session.process({ptrs,nullptr,2,0,128,0});
    });
    bool refreshed=true;
    for(int i=0;i<4;++i){refreshed=f.session.refresh(*f.store)&&refreshed;f.session.tick(2000+i);}
    stop.store(true);driver.join();f.session.tick(3000);
    check(refreshed,"concurrent publication and graph reclamation keep sources alive");
}
void anchoredTrimPlayback() {
    for(const auto width:{Precision::Float32,Precision::Float64})for(const bool loop:{false,true}) {
        Fixture f(44100,2,44100);f.add(1,100,1000,11,20000);
        f.store->db().exec("UPDATE clips SET content_offset_ticks=12012");
        if(loop)f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048");
        SessionSpec spec{2,48000,2048};spec.precision=width;f.session.graph().setFadeFrames(0);
        check(f.session.load(*f.store,{},spec),"load native-width audio before an anchored trim");
        auto render=[&] {
            f.session.transport().locate(0);f.session.transport().play();
            if(!f.session.clips()->prime())throw std::runtime_error("trim prime timeout");
            std::vector<double> result(4096);std::array<float,4096> narrow{};
            float* out[]={narrow.data(),narrow.data()+2048};double* wide[]={result.data(),result.data()+2048};
            AudioIo io;io.frames=2048;io.numOut=2;
            if(width==Precision::Float64)io.out64=wide;else io.out=out;
            f.session.process(io);
            if(width==Precision::Float32)std::copy(narrow.begin(),narrow.end(),result.begin());
            return result;
        };
        const auto original=render();
        const std::array<std::int64_t,1> tracks{1};edit_plan::Options options;options.policy.automationFollowsClips=false;
        const auto trim=edit_plan::prepareTrim(*f.store,
            edit_plan::materialScope(tracks,edit_plan::PlacementClock::Nanoseconds,{0,ns(3000)}),{1,{1,ns(300)}},options);
        check(trim.ok && clip_edit::commitPrepared(*f.store,trim).ok && f.session.refresh(*f.store),
              "trim resampled audio through the captured planner and refresh at either precision");
        const auto actual=render();bool exact=actual.size()==4096 && original.size()==4096;
        if(exact)for(std::size_t channel=0;channel<2;++channel)for(std::size_t frame=0;frame<2048;++frame)
            exact=exact && actual[channel*2048+frame]==(frame<300?0:original[channel*2048+frame]);
        check(f.session.problems().empty() && exact,"anchored trim preserves resampled PCM and loop phase sample for sample");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store) && render()==original,
              "audio trim undo restores the complete original native-width render");
    }
}
void trimAcrossTempoWithSourceEnd() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;f.add(1,0,2000,0,10000);
        f.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(96096,240);"
            "UPDATE clips SET time_base=0,pos_ns=NULL,length_ns=NULL,pos_ticks=48048,length_ticks=192192;"
            "INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(1,96096)");
        SessionSpec spec{2,48000,1024};spec.precision=width;f.session.graph().setFadeFrames(0);
        check(f.session.load(*f.store,{},spec),"load audio with an explicit End across a tempo junction");
        auto render=[&] {
            f.session.transport().locate(0);f.session.transport().play();
            if(!f.session.clips()->prime())throw std::runtime_error("tempo trim prime timeout");
            std::vector<double> result(2048);std::array<float,2048> narrow{};
            float* out[]={narrow.data(),narrow.data()+1024};double* wide[]={result.data(),result.data()+1024};
            AudioIo io;io.frames=1024;io.numOut=2;if(width==Precision::Float64)io.out64=wide;else io.out=out;
            f.session.process(io);if(width==Precision::Float32)std::copy(narrow.begin(),narrow.end(),result.begin());return result;
        };
        const auto original=render();const std::array<std::int64_t,1> tracks{1};edit_plan::Options options;options.policy.automationFollowsClips=false;
        const auto trim=edit_plan::prepareTrim(*f.store,
            edit_plan::materialScope(tracks,edit_plan::PlacementClock::Ticks,{0,240240}),{1,{0,96096}},options);
        check(trim.ok && clip_edit::commitPrepared(*f.store,trim).ok && f.session.refresh(*f.store),
              "trim into a faster tempo freezes the original source End");
        const auto actual=render();bool exact=actual.size()==2048 && original.size()==2048;
        if(exact)for(std::size_t channel=0;channel<2;++channel)for(std::size_t frame=0;frame<1024;++frame)
            exact=exact && actual[channel*1024+frame]==(frame<400?0:original[channel*1024+frame]);
        check(f.session.problems().empty() && exact && original.size()==2048 && original[499]!=0 && original[500]==0,
              "anchored trim retains source End audio across tempo instead of applying a second tick clamp");
    }
}
void signedPhysicalAudioOffset() {
    Fixture f(48000,2,100000,-1);f.add(1,0,2000,0,10000);
    f.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(96096,240);"
        "UPDATE clips SET time_base=0,pos_ns=NULL,length_ns=NULL,pos_ticks=144144,length_ticks=96096,content_offset_ticks=24024;"
        "INSERT INTO clip_playback_details(clip_id,content_offset_ns) VALUES(1,2083333)");
    const std::array<std::int64_t,1> tracks{1};edit_plan::Options options;options.policy.automationFollowsClips=false;
    const auto trim=edit_plan::prepareTrim(*f.store,
        edit_plan::materialScope(tracks,edit_plan::PlacementClock::Ticks,{0,288288}),{1,{0,96096}},options);
    check(trim.ok && clip_edit::commitPrepared(*f.store,trim).ok &&
              f.store->db().execAndGet("SELECT content_offset_ticks FROM clips WHERE id=1").getInt64()==-24024 &&
              f.store->db().execAndGet("SELECT content_offset_ns FROM clip_playback_details WHERE clip_id=1").getInt64()==0,
          "source-zero extension after a tempo relocation preserves physical zero with a signed tick projection");
    f.load(1024);const auto rendered=f.render(1024);
    check(f.session.problems().empty() && rendered.size()==2048 && rendered[399]==0 &&
              rendered[400]==0.5f && rendered[699]==0.5f && rendered[700]==0,
          "unwarped reader accepts the authoritative nonnegative source pin despite a negative derived tick offset");
}
void pinnedMarkerOrderWitness() {
    Fixture f(48000,2,100000,-1);f.add(1,0,2000,0,10000);
    f.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(48048,240);"
        "UPDATE clips SET time_base=0,pos_ns=NULL,length_ns=NULL,pos_ticks=96096,length_ticks=96096,content_offset_ticks=24024;"
        "INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(1,2083333,4166667);"
        "INSERT INTO clip_properties(clip_id,end_marker_ticks) VALUES(1,48048)");
    const std::array<std::int64_t,1> tracks{1};edit_plan::Options options;options.policy.automationFollowsClips=false;
    const auto trim=edit_plan::prepareTrim(*f.store,
        edit_plan::materialScope(tracks,edit_plan::PlacementClock::Ticks,{0,288288}),{1,{0,132132}},options);
    check(trim.ok && clip_edit::commitPrepared(*f.store,trim).ok &&
              f.store->db().execAndGet("SELECT content_offset_ticks FROM clips WHERE id=1").getInt64()==60060,
          "pinned End witness trims into a valid physical span with display Start beyond display End");
    f.load(1024);const auto rendered=f.render(1024);
    check(f.session.problems().empty() && rendered.size()==2048 && rendered[374]==0 &&
              rendered[375]==0.5f && rendered[399]==0.5f && rendered[400]==0,
          "pinned source clocks, rather than stale tick ordering, admit the retained PCM tail");
}
void fractionalEndpoint() {
    Fixture f;f.add(1,0,1000,0,1000);
    f.store->db().exec("UPDATE clips SET pos_ns=10000,length_ns=10000");
    f.load(64);const auto out=f.render(64);
    check(out[0]==f.media[0] && out[1]==0,"absolute clip endpoints round once");
    // Both rows have the same exact 35ms endpoint. Converting the two
    // operands separately puts the second just below the half-sample tie.
    for (const auto begin : {0, 5}) {
        Fixture anchored(44100, 2, 44100, -1);
        anchored.add(1, 0, 1000, 0, 44100);
        SQLite::Statement placement(anchored.store->db(),
            "UPDATE clips SET pos_ns=?,length_ns=?");
        placement.bind(1, begin);
        placement.bind(2, 35000000 - begin);
        placement.exec();
        anchored.load(2048, 44100);
        const auto rendered = anchored.render(2048);
        check(rendered.size() == 4096 && rendered[1542] == 0.5f &&
                  rendered[1543] == 0.5f && rendered[1544] == 0,
              "linear audio retains its exact native end when its left edge changes");
    }
}
void relativeProject() {
    Fixture f;f.add(1,0,1000,0,1000);f.store.reset();
    const auto previous=std::filesystem::current_path();
    std::filesystem::current_path(f.dir.path());
    StoreError error{};f.store=Store::open("session.adi",error);
    std::filesystem::current_path(previous);
    if(!f.store)throw std::runtime_error("reopen relative project");
    f.load(64);const auto out=f.render(64);
    check(out[0]==f.media[0],"media stays relative to opened project after working directory changes");
}
void readAhead() {
    Fixture f;f.add(1,12000,4000,0,4000);f.load(64);
    if(!f.session.clips()->prime())throw std::runtime_error("ahead prime");
    f.session.clips()->stallForTest(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.session.transport().locate(12000);
    const auto out=f.render(64,false);
    check(out[0]==f.media[0],"fixed horizon prefetches future clip at 64 samples");
}
void segmentsAndCrop() {
    Fixture f;f.add(1,63,12000,9,20000);f.load(4096);
    if(!f.session.clips()->prime())throw std::runtime_error("segment prime");
    auto sources=f.session.clips()->sourcesFor(1);
    std::array<float,8192> output{};float* ptrs[]={output.data(),output.data()+4096};
    for(int offset=0;offset<4096;offset+=128) {
        NodeIo io;io.out=ptrs;io.channels=2;io.frames=128;io.blockOffset=offset;io.sampleRate=48000;
        audio::CallbackScope callback;sources.at(0)->process(io);
    }
    bool exact=true;
    for(int i=0;i<4096;++i)exact=exact && output[static_cast<std::size_t>(i)]==(i>=63?f.media[static_cast<std::size_t>(i-63+9)*2]:0);
    check(exact,"sub-block offsets address the original block buffers and clock");

    Fixture looped;
    looped.add(1,12,5,100,5);
    looped.add(2,24,4,200,4);
    looped.add(3,38,4,300,4);
    looped.add(4,60,10,400,10);
    looped.load(96);
    looped.session.transport().loop(10,40);
    looped.session.transport().locate(30);
    if(!looped.session.clips()->prime())throw std::runtime_error("looped segment prime");
    auto loopSources=looped.session.clips()->sourcesFor(1);
    std::array<float,192> loopPcm{};
    float* loopOut[]{loopPcm.data(),loopPcm.data()+96};
    constexpr std::array slices{std::pair{0,8},std::pair{8,64},std::pair{72,24}};
    for(const auto& [offset,frames]:slices) {
        NodeIo io;io.out=loopOut;io.channels=2;io.frames=frames;io.blockOffset=offset;io.sampleRate=48000;
        audio::CallbackScope callback;loopSources.at(0)->process(io);
    }
    bool exactLoop=true;
    for(int frame=0;frame<96;++frame) {
        const auto time=frame<10?30+frame:10+(frame-10)%30;
        int sourceFrame=-1;
        if(time>=12 && time<17)sourceFrame=100+time-12;
        if(time>=24 && time<28)sourceFrame=200+time-24;
        if(time>=38)sourceFrame=300+time-38;
        for(std::size_t channel=0;channel<2;++channel) {
            const auto wanted=sourceFrame<0?0.0f:looped.media[static_cast<std::size_t>(sourceFrame)*2+channel];
            exactLoop=exactLoop && loopPcm[channel*96+static_cast<std::size_t>(frame)]==wanted;
        }
    }
    check(exactLoop,"nonzero-offset slices include clips before, after and across repeated loop wraps");

    Fixture crop(44100,1,88200,997);crop.add(1,0,48000,123,44100);crop.load(4096);crop.session.transport().locate(8100);
    const auto out=crop.render(4096);double peak=0;
    for(int i=0;i<4096;++i)peak=std::max(peak,std::abs(static_cast<double>(out[static_cast<std::size_t>(i)])-0.5*std::sin(2*std::numbers::pi*997*(123.0/44100+(8100.0+i)/48000))));
    check(peak<1e-4,"converted cropped window keeps phase across page boundary");
    Transport t;t.loop(10,110);t.locate(100);t.play();
    check(t.sampleAt(250)==50,"multiple loop wraps inside one callback");
    check(t.sampleAt(std::numeric_limits<std::int64_t>::max())>=10 && t.sampleAt(std::numeric_limits<std::int64_t>::max())<110,"large transport offsets avoid signed overflow");
    // At or past the brace's End playback runs on and never loops, as Live's does: the largest
    // offset then stops at the largest sample instead of folding back into the brace.
    t.locate(std::numeric_limits<std::int64_t>::max());
    check(!t.looping() && t.sampleAt(std::numeric_limits<std::int64_t>::max())==std::numeric_limits<std::int64_t>::max(),
          "a transport past the loop runs on, and its large offsets avoid signed overflow");
}
// Tests set ADI_HOME, so a decoded clip's cache is a temporary folder and not
// the user's (ADR-0149). The one place this suite writes an environment variable.
void setHome(const fs::path& p) {
#if defined(_WIN32)
    _wputenv_s(L"ADI_HOME", p.wstring().c_str());
#else
    setenv("ADI_HOME", p.string().c_str(), 1);
#endif
}
void externalSourceSnapshots() {
    Fixture f;f.add(1,0,12000);f.add(2,0,12000);
    f.store->db().exec("UPDATE clips SET muted=1 WHERE id=2");
    f.load(64);
    const auto sources=f.session.clips()->externalSources();
    check(sources.size()==1 && sources[0].clipId==1 && sources[0].mediaId==1 &&
          sources[0].storedBlake3=="test" && fs::equivalent(sources[0].path,f.dir.path()/"audio.wav"),
          "off-callback source snapshot names only opened production clip inputs");
    f.store->db().exec("UPDATE media_files SET rel_path='not-the-open-file.wav' WHERE id=1");
    const auto unchanged=f.session.clips()->externalSources();
    check(unchanged.size()==1 && unchanged[0].path==sources[0].path,
          "source snapshot belongs to the loaded generation, not mutable Store rows");
    bool refused=false;
    {audio::CallbackScope callback;refused=f.session.clips()->externalSources().empty();}
    check(refused,"source descriptor copy refuses callback use without allocation or I/O");
}

#if defined(__linux__)
struct ResidentMemory {
    double currentMiB = 0, peakMiB = 0;
};
ResidentMemory residentMemory() {
    constexpr double bytesPerMiB = 1024.0 * 1024.0;
    std::ifstream stat("/proc/self/statm");
    std::uint64_t totalPages = 0, residentPages = 0;
    const auto pageSize = sysconf(_SC_PAGESIZE);
    struct rusage usage{};
    if (!(stat >> totalPages >> residentPages) || pageSize <= 0 || getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("read clip benchmark resident memory");
    return {static_cast<double>(residentPages) * static_cast<double>(pageSize) / bytesPerMiB,
            static_cast<double>(usage.ru_maxrss) / 1024.0};
}
#endif

void largeClipMemory() {
    constexpr int clipCount = 1200, sampleRate = 48000, block = 64;
    constexpr int clipFrames = 2 * 8192 + 128, pageBoundaryOffset = 8192 - block / 2;
    const auto sourceStart = [](int index) { return (index % 23) * 37; };
    const auto placement = [](int index) { return std::int64_t{index} * sampleRate; };
    Fixture f(sampleRate, 2, clipFrames + 1024);
    {
        SQLite::Transaction addClips(f.store->db());
        for (int index = 0; index < clipCount; ++index)
            f.add(index + 1, placement(index), clipFrames, sourceStart(index), clipFrames);
        addClips.commit();
    }
#if defined(__linux__)
    const auto beforeOpen = residentMemory();
#endif
    const auto beforeAllocations = callbackAllocations.load();
    const auto beforeFileIo = audio::callbackFileIo.load();
    f.load(block, sampleRate);
    auto *clips = f.session.clips();
    check(clips && clips->externalSources().size() == clipCount, "the open generation admits every unmuted WAV clip");
    if (!clips) return;
#if defined(__linux__)
    const auto opened = residentMemory();
#endif
    const auto visit = [&](int index) {
        f.session.transport().locate(placement(index) + pageBoundaryOffset);
        const auto output = f.render(block, true, std::chrono::seconds(3));
        const auto first = static_cast<std::size_t>(sourceStart(index) + pageBoundaryOffset);
        for (std::size_t frame = 0; frame < block; ++frame) {
            if (output[frame] != f.media[(first + frame) * 2] || output[block + frame] != f.media[(first + frame) * 2 + 1])
                return false;
        }
        return true;
    };
    check(visit(0), "the first primed callback matches stereo PCM across a page boundary");
#if defined(__linux__)
    const auto started = residentMemory();
#endif
    // Consecutive placements are a second apart, beyond the worker's lookahead.
    bool allExact = true;
    for (int index = 1; index < clipCount; ++index) allExact = visit(index) && allExact;
    check(allExact && clips->underrunSamples() == 0 && clips->readErrors() == 0,
          "all 1,200 cold seeks reproduce exact PCM without underruns or source failures");
#if defined(__linux__)
    const auto visited = residentMemory();
#endif
    f.session.transport().locate(placement(clipCount + 1));
    const auto silence = f.render(block, true, std::chrono::seconds(3));
    check(std::all_of(silence.begin(), silence.end(), [](float sample) { return sample == 0; }),
          "moving outside the placements plays silence");
#if defined(__linux__)
    const auto movedAway = residentMemory();
#endif
    check(visit(0), "revisiting the first clip after the sweep reproduces exact stereo PCM");
    check(callbackAllocations.load() == beforeAllocations && audio::callbackFileIo.load() == beforeFileIo,
          "the large song's callbacks do not allocate or perform file I/O");
#if defined(__linux__)
    const auto finish = residentMemory();
    std::printf("SC-418 %d stereo clips, RSS / peak MiB: before %.1f / %.1f, open %.1f / %.1f, playback %.1f / %.1f, "
                "after all cold seeks %.1f / %.1f, outside clips %.1f / %.1f, revisit %.1f / %.1f\n",
                clipCount, beforeOpen.currentMiB, beforeOpen.peakMiB, opened.currentMiB, opened.peakMiB,
                started.currentMiB, started.peakMiB, visited.currentMiB, visited.peakMiB,
                movedAway.currentMiB, movedAway.peakMiB, finish.currentMiB, finish.peakMiB);
    check(opened.currentMiB - beforeOpen.currentMiB < 120.0, "opening the large song keeps resident growth below 120 MiB");
    check(finish.peakMiB - beforeOpen.currentMiB < 160.0, "visiting all clips keeps peak resident growth below 160 MiB");
#endif
}

// Reproduce the descriptor budget of the Windows CRT on hosts that normally
// run the suite with a higher limit. Leave any lower host limit in effect.
struct ClipFileLimit {
#if defined(_WIN32)
    int previous = _getmaxstdio();
    ClipFileLimit() {
        if (_setmaxstdio(std::min(previous, 512)) < 0)
            throw std::runtime_error("could not set the clip test file limit");
    }
    ~ClipFileLimit() { (void)_setmaxstdio(previous); }
#else
    struct rlimit previous{};
    ClipFileLimit() {
        if (getrlimit(RLIMIT_NOFILE, &previous) != 0)
            throw std::runtime_error("could not get the clip test file limit");
        auto limit = previous;
        limit.rlim_cur = std::min(limit.rlim_cur, rlim_t{512});
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0)
            throw std::runtime_error("could not set the clip test file limit");
    }
    ~ClipFileLimit() { (void)setrlimit(RLIMIT_NOFILE, &previous); }
#endif
};
void sharedWavReaders() {
    constexpr int clipCount = 600;
    constexpr int frames = 64;
    constexpr int sampleRate = 48000;
    Fixture f(sampleRate, 1, clipCount * frames);
    const auto sourceStart = [](int i, int edit) {
        return ((i * 37 + edit) % clipCount) * frames;
    };
    for (int i = 0; i < clipCount; ++i)
        f.add(i + 1, i * frames, frames, sourceStart(i, 0), frames);

    const ClipFileLimit limit;
    f.load(frames);
    const auto keepFirstGeneration = f.session.clips()->readiness();
    for (int edit = 0; edit < 2; ++edit) {
        if (edit != 0) {
            f.store->db().exec("UPDATE audio_clips SET src_start_frames=(src_start_frames + 64) % 38400");
            if (!f.session.refresh(*f.store)) throw std::runtime_error(f.session.error());
            f.session.graph().collect();
        }

        const auto* playback = f.session.clips();
        const auto admitted = playback->externalSources().size();
        bool independentOpen = false;
        try {
            audio::WavReader waveformReader(f.dir.path() / "audio.wav");
            independentOpen = waveformReader.frames() == f.media.size();
        } catch (const std::exception&) {}

        f.session.transport().locate(0);
        int heard = 0;
        for (int i = 0; i < clipCount; ++i) {
            const auto block = f.render(frames);
            const auto expected = f.media.begin() + sourceStart(i, edit);
            if (std::equal(block.begin(), block.begin() + frames, expected) &&
                std::equal(block.begin() + frames, block.end(), expected)) ++heard;
        }
        std::printf("SC-408 generation %d: %zu/%d admitted, %d/%d exact, %zu errors, independent WAV open %s\n",
                    edit + 1, admitted, clipCount, heard, clipCount, playback->problems().size(),
                    independentOpen ? "yes" : "no");
        check(keepFirstGeneration && admitted == clipCount && heard == clipCount &&
              playback->problems().empty() && independentOpen,
              "600 clips share a source under the file limit across an edit with the old generation alive");
    }
}
void decodedPins() {
    // ADR-0156: a decoded clip pins its cache copy while a generation reads
    // it, and the pin goes back with the generation. Every edit rebuilds, and
    // each rebuild used to pin the copy again, for good.
    auto pins=[]{int n=0;for(const auto& e:audio::defaultDecodeCache().entries())n+=e.pins;return n;};
    {
        Fixture f;
        fs::copy_file(fs::path(ADI_DECODE_FIXTURES)/"tone16.flac",f.dir.path()/"tone.flac");
        f.store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(2,'flac','tone.flac','flac');"
                           "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,100000000);"
                           "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,2,0,4800)");
        f.load(512);
        const auto external=f.session.clips()->externalSources();
        check(external.size()==1 && external[0].mediaId==2 &&
              fs::equivalent(external[0].path,f.dir.path()/"tone.flac"),
              "compressed source descriptor retains actual external input, not a growing decoded prefix");
        // Loading no longer waits for complete compressed conversion. The
        // private prefix has no LRU entry; assert this pin after publication.
        auto decoded=audio::defaultProgressiveDecoder().open(f.dir.path()/"tone.flac");
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(decoded->snapshot().status==audio::ProgressiveStatus::Pending && std::chrono::steady_clock::now()<until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(decoded->snapshot().status==audio::ProgressiveStatus::Complete && pins()==1,"a completed decoded clip pins its cache copy while it plays");
        for(int i=0;i<5;++i) {
            f.store->db().exec("UPDATE clips SET gain_db=gain_db-1");
            if(!f.session.refresh(*f.store))throw std::runtime_error(f.session.error());
            f.render(512);f.session.graph().collect();
        }
        check(pins()==1,"five rebuilds later it is still one pin, not six");
    }
    check(pins()==0,"closing the session gives every pin back");
}
void unsupported() {
    for(int kind=0;kind<4;++kind) {
        Fixture f;f.add(1,0,500,0,1000);
        f.add(2,0,500,0,1000);
        f.store->db().exec("UPDATE clips SET muted=1 WHERE id=2");
        if(kind==0)f.store->db().exec("UPDATE audio_clips SET warp_enabled=1");
        if(kind==1)f.store->db().exec("UPDATE audio_clips SET formant_shift=1 WHERE clip_id=1");
        if(kind==2){std::ofstream file(f.dir.path()/"audio.wav",std::ios::binary|std::ios::trunc);file<<"not WAV";}
        if(kind==3)f.store->db().exec("UPDATE audio_clips SET transpose_semis=49 WHERE clip_id=1");
        f.load(512);const auto o=f.render(512);
        check(!f.session.problems().empty() && std::all_of(o.begin(),o.end(),[](float x){return x==0;}),"unsupported clip is silent with named session problem");
        check(f.session.clipPlaybackUnavailable(1) && !f.session.clipPlaybackUnavailable(2),
              "a dropped audio clip is tagged by ID, and an intentionally muted clip is not");
    }
}
void missingMediaOnUnplayedClips() {
    for (const bool missing : {false, true}) {
        Fixture f;
        f.add(1, 0, 500, 0, 1000);
        f.add(2, 0, 500, 0, 1000);
        f.store->db().exec("UPDATE clips SET muted=1 WHERE id=1;"
                           "UPDATE audio_clips SET formant_shift=1 WHERE clip_id=2");
        if (missing) fs::rename(f.dir.path() / "audio.wav", f.dir.path() / "moved.wav");
        f.load(64);
        const auto* playback = f.session.clips();
        check(playback->mediaUnavailable(1) == missing, "a muted clip reports whether its source file is missing");
        check(playback->mediaUnavailable(2) == missing, "an unsupported clip reports whether its source file is missing");
        const auto& problems = playback->problems();
        check(problems.size() == 1 && problems.front().find("unwarped formant shift unsupported") != std::string::npos,
              "the unsupported clip reports its playback problem even when the source file is missing");
        const auto pcm = f.render(64);
        check(playback->externalSources().empty() && std::all_of(pcm.begin(), pcm.end(), [](float value) { return value == 0.f; }),
              "checking skipped clip media leaves both clips out of playback");
    }
}

void highRates() {
    auto nsHigh = [](std::int64_t frames, double rate) {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(frames)*1e9/rate));
    };

    try { Fixture f; f.load(1024, 44099.0); check(false, "session below 44.1kHz must reject"); } catch(const std::exception& e) { check(std::string(e.what()).find("44.1-768")!=std::string::npos, "session floor rejection names the valid range"); }
    try { Fixture f; f.load(1024, 768000.0); check(true, "768k boundary acceptance"); } catch(const std::exception&) { check(false, "768k boundary acceptance failed"); }

        // Cascaded huge upsample: 1Hz source constant 4096 frames, away from the zero-extended edges
        {
            Fixture f(1, 1, 4096, -1.0); // constant 0.5
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,?)");
            q.bind(1, nsHigh(768000LL * 4096, 768000.0)); q.exec(); // length = 4096 seconds
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,4096)");
            a.exec();

            f.load(4096, 768000.0);
            f.session.transport().locate(2000LL * 768000); // interior, beyond the sinc support
            const auto out = f.render(4096,true,std::chrono::seconds(300));
            double peak = 0;
            for(int i = 0; i < 4096; ++i) {
                peak = std::max(peak, std::abs(static_cast<double>(out[static_cast<std::size_t>(i)]) - 0.5));
            }
            std::printf("SRC cascaded 1->768000 DC peak %.9g\n",peak);
            check(peak < 1e-4, "cascaded huge upsample 1Hz->768kHz constant");
        }

        // Cascaded large upsample: 5Hz sine 100Hz converted to 48k quality interior
        {
            Fixture f(100, 1, 100 * 60, 5.0); // 100Hz rate, 60s length, 5Hz sine
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,?)");
            q.bind(1, nsHigh(48000LL * 60, 48000.0)); q.exec();
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,6000)");
            a.exec();

            f.load(4096, 48000.0);

            // interior seek, e.g. 10 seconds
            f.session.transport().locate(10 * 48000);
            const auto out = f.render(4096,true,std::chrono::seconds(300));
            double peak = 0, squared = 0;
            for(int i = 0; i < 4096; ++i) {
                const double t = (10 * 48000.0 + i) / 48000.0;
                const double want = 0.5 * std::sin(2 * std::numbers::pi * 5.0 * t);
                const double error = static_cast<double>(out[static_cast<std::size_t>(i)]) - want;
                squared += error * error;
                peak = std::max(peak, std::abs(error));
            }
            const double rms = std::sqrt(squared / 4096);
            std::printf("SRC cascaded 100->48000 5Hz RMS %.9g peak %.9g\n", rms, peak);
            check(rms < 1e-3 && peak < 1e-2, "cascaded 100Hz->48kHz sine quality interior");
        }

    for(const auto rate : {192000u, 384000u}) {
        // exact placement
        {
            Fixture f(rate, 2, rate);
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,?,?)");
            q.bind(1, nsHigh(73, rate)); q.bind(2, nsHigh(21001, rate)); q.exec();
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,11,25000)");
            a.exec();

            f.load(256, rate);

            bool exact = true; int at = 0;
            while(at < 24000) {
                const int n = std::min(256, 24000 - at);
                const auto out = f.render(n);
                for(int c=0; c<2; ++c) for(int i=0; i<n; ++i) {
                    const int t = at + i;
                    const float want = t >= 73 && t < 21074 ? f.media[static_cast<std::size_t>(t-73+11)*2+static_cast<std::size_t>(c)] : 0;
                    exact = exact && out[static_cast<std::size_t>(c*n+i)] == want;
                }
                at += n;
            }
            check(exact, "high rate exact placement");
        }

        // conversion 22.05/44.1/96k sine accuracy
        for (const auto sourceRate : {22050u, 44100u, 96000u}) {
            Fixture f(sourceRate, 1, sourceRate * 2, 997);
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,?)");
            q.bind(1, nsHigh(rate * 2, rate)); q.exec();
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,?)");
            a.bind(1, sourceRate * 2); a.exec();

            f.load(4096, rate);

            double squared = 0, peak = 0; std::size_t count = 0;
            for(int at = 0; at < 90000; at += 4096) {
                auto out = f.render(4096);
                for(int i = 0; i < 4096; ++i) {
                    if(at + i < 512) continue;
                    const double want = 0.5 * std::sin(2 * std::numbers::pi * 997 * (at + i) / static_cast<double>(rate));
                    const double error = static_cast<double>(out[static_cast<std::size_t>(i)]) - want;
                    squared += error * error; peak = std::max(peak, std::abs(error)); ++count;
                }
            }
            const double rms = std::sqrt(squared / static_cast<double>(count));
            std::printf("SRC %u->%u 997Hz RMS %.9g peak %.9g SNR %.3f dB\n", sourceRate, rate, rms, peak, 20 * std::log10((0.5 / std::sqrt(2.0)) / rms));
            check(rms < 1e-4 && peak < 1e-3, "high rate sine conversion accurate");
        }

        // duration-based readahead stalled worker (locate)
        {
            Fixture f(rate, 2, rate * 2);
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,?)");
            q.bind(1, nsHigh(rate * 2, rate)); q.exec();
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,?)");
            a.bind(1, rate * 2); a.exec();

            f.load(64, rate);

            std::int64_t horizonSamples = 16384LL * rate / 48000LL;
            f.session.transport().locate(0);
            f.session.transport().play();
            if(!f.session.clips()->prime()) throw std::runtime_error("prime timeout");
            f.session.clips()->stallForTest(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            // Primed at zero, then no worker progress. The last promised
            // sample must already be cached, not newly requested by prime().
            f.session.transport().locate(horizonSamples - 64);
            const auto out = f.render(64, false);
            bool exact=true;for(std::size_t i=0;i<64;++i)
                exact=exact && out[i]==f.media[(static_cast<std::size_t>(horizonSamples-64)+i)*2];
            check(exact && f.session.clips()->underrunSamples()==0, "high rate duration-based readahead prefetch");
            f.session.clips()->stallForTest(false);
            f.session.transport().locate(static_cast<std::int64_t>(rate));
            const auto located=f.render(64);
            check(located[0]==f.media[static_cast<std::size_t>(rate)*2],"high rate cold locate refills");
            f.session.transport().loop(static_cast<std::int64_t>(rate),static_cast<std::int64_t>(rate)+100);
            f.session.transport().locate(static_cast<std::int64_t>(rate)+80);
            const auto wrapped=f.render(64);
            bool wrap=true;for(std::size_t i=0;i<64;++i)
                wrap=wrap && wrapped[i]==f.media[(static_cast<std::size_t>(rate)+(80+i)%100)*2];
            check(wrap,"high rate transport loop wraps mid-block");
        }

        // loop
        {
            Fixture f(rate, 2, rate * 2);
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,loop_enabled,loop_start_ticks,loop_len_ticks,content_offset_ticks) VALUES(1,1,'audio',1,0,?,1,24024,48048,12012)");
            q.bind(1, nsHigh(rate * 2, rate)); q.exec();
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,?)");
            a.bind(1, rate * 2); a.exec();

            f.load(2048, rate);

            auto out = f.render(2048);
            bool exact = true;
            std::int64_t loopBegin = 100LL * rate / 48000LL;
            std::int64_t loopLen = 200LL * rate / 48000LL;
            std::int64_t offset = 50LL * rate / 48000LL;
            for(int i = 0; i < 2000; ++i) {
                auto s = i + offset;
                if(s >= loopBegin) s = loopBegin + (s - loopBegin) % loopLen;
                exact = exact && out[static_cast<std::size_t>(i)] == f.media[static_cast<std::size_t>(s) * 2];
            }
            check(exact, "high rate clip loop and content offset");
        }
    }
}


}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::string only=argc>1?argv[1]:"all";
    test::TempDirectory home{"clip_playback","home"};setHome(home.path());
    try {
        if(only=="all" || only=="placement")placement();
        if(only=="all" || only=="unwarped-tempo")unwarpedTempoChanges();
        if(only=="all" || only=="reverse"){reversedSource();reverseSelectedRange();reverseSelectedRepeats();}
        if(only=="all" || only=="monitor-input")liveInputMonitoring();
        if(only=="all" || only=="take-lanes")takeLanePlayback();
        if(only=="all" || only=="properties"){properties();legacyUnwarpedLoopPastSource();legacyUnwarpedLoopRetainsEnd();}
        if(only=="all" || only=="fades"){editedFades();shortenedFade();resizeClampsFades();}
        if(only=="all" || only=="split"){splitSourceClocks();splitInsideFades();twoPointSplitSourceClocks();twoPointSplitWarpFades();rangeSurvivorSourceClocks();silenceSourceClocks();deleteTimeSourceClocks();timeClipboardSourceClocks();}
        if(only=="all" || only=="seeks")seeksAndStalls();
        if(only=="all" || only=="conversion"){conversion();stereoConversionMatchesSinc();}
        if(only=="realtime-conversion")realtimeConversion();
        if(only=="all" || only=="markers"){sourceMarkers();editPinnedSourceMarkers();endSourceClocks();absoluteLinearEndSamples();}
        if(only=="all" || only=="refresh")refreshAndVariableBlocks();
        if(only=="all" || only=="unsupported")unsupported();
        if(only=="all" || only=="unplayed-media")missingMediaOnUnplayedClips();
        if(only=="all" || only=="segments")segmentsAndCrop();
        if(only=="all" || only=="ahead")readAhead();
        if(only=="all" || only=="relative")relativeProject();
        if(only=="all" || only=="endpoint"){fractionalEndpoint();anchoredTrimPlayback();trimAcrossTempoWithSourceEnd();signedPhysicalAudioOffset();pinnedMarkerOrderWitness();}
        if(only=="all" || only=="highRates")highRates();
        if(only=="all" || only=="shared-wav")sharedWavReaders();
        if(only=="--large-clip-memory-only")largeClipMemory();
        if(only=="all" || only=="pins")decodedPins();
        if(only=="all" || only=="inputs")externalSourceSnapshots();
    }
    catch(const std::exception& e){check(false,e.what());}
    check(callbackAllocations.load()==0,"callback allocates nothing");
    check(audio::callbackFileIo.load()==0,"no file I/O on the audio thread");
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
