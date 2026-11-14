// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/main_loudness.hpp"
#include "adi/engine/session.hpp"
#include "adi/dsp/auto_gain.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <cstdlib>
#include <new>
namespace {
std::atomic<long> allocations{0};
std::atomic<bool> countAllocations{false};
}
void* operator new(std::size_t n) {
    if(countAllocations.load(std::memory_order_relaxed))allocations.fetch_add(1,std::memory_order_relaxed);
    void* p=std::malloc(n==0?1:n);if(!p)throw std::bad_alloc();return p;
}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool good,const char* name){++checks;if(!good){++failures;std::printf("FAIL: %s\n",name);}}
bool near(double a,double b,double tolerance=0.06){return std::abs(a-b)<tolerance;}
double expected(double rate,double hz,double amplitude,double weights=2) {
    const auto z=std::polar(1.0,-2*std::numbers::pi*hz/rate);
    auto response=[&](const dsp::Biquad& c){return (c.b0+c.b1*z+c.b2*z*z)/(1.0+c.a1*z+c.a2*z*z);};
    const auto h=response(dsp::AutoGain::kShelf(rate))*response(dsp::AutoGain::kHighPass(rate));
    return -0.691+10*std::log10(weights*amplitude*amplitude*0.5*std::norm(h));
}
template<class T> struct Render {
    MainLoudness meter;double rate=48000;std::int64_t at=0;
    explicit Render(double sampleRate=48000,int channels=2):rate(sampleRate){if(!meter.prepare(rate,channels))throw std::runtime_error("meter format");}
    void tone(double seconds,double amplitude=0.1,double hz=1000,double phase=0,int channels=2,bool rightSilent=false) {
        std::array<T,256> l{},r{};T* output[]{l.data(),r.data()};AudioIo io;io.numOut=channels;
        if constexpr(std::is_same_v<T,float>)io.out=output;else io.out64=output;
        auto remaining=static_cast<std::int64_t>(std::llround(seconds*rate));
        while(remaining>0) {
            const auto frames=static_cast<int>(std::min<std::int64_t>(remaining,256));io.frames=frames;
            for(int i=0;i<frames;++i){const auto index=static_cast<std::size_t>(i);l[index]=static_cast<T>(amplitude*std::sin(2*std::numbers::pi*hz*static_cast<double>(at+i)/rate+phase));r[index]=rightSilent?T{0}:l[index];}
            meter.process(io);at+=frames;remaining-=frames;
        }
    }
};
template<class T> void windows() {
    Render<T> render;const auto& view=render.meter.readout();const auto level=expected(48000,1000,0.1);
    check(std::isinf(view.integratedLufs.load()) && view.integratedLufs.load()<0,"new meter reads silence");
    render.tone(0.3);
    check(std::isinf(view.momentaryLufs.load()) && std::isinf(view.shortTermLufs.load()),"windows remain unwarmed before their full duration");
    render.tone(0.2);
    check(near(view.momentaryLufs.load(),level) && near(view.integratedLufs.load(),level),"400 ms and integrated tone match independent filter response");
    check(std::isinf(view.shortTermLufs.load()),"short-term window waits for three seconds");
    render.tone(2.7);
    check(near(view.shortTermLufs.load(),level) && near(view.integratedLufs.load(),level),"three-second and integrated windows match known stereo loudness");
    const auto held=view.integratedLufs.load();
    check(render.meter.prepare(48000,2),"same-format reprepare succeeds");render.tone(0.1);
    check(near(view.integratedLufs.load(),held),"ordinary rebuild retains measurement history");
    render.tone(4,0);
    check(std::isinf(view.momentaryLufs.load()) || view.momentaryLufs.load()<-100,"momentary falls to silence");
    check(near(view.integratedLufs.load(),level,0.4),"absolute and relative gates exclude trailing silence");
    check(view.truePeakDb.load()>-21 && view.truePeakDb.load()<-19,"true peak holds the signal after silence");
    render.meter.requestReset();AudioIo empty;render.meter.process(empty);
    check(std::isinf(view.integratedLufs.load()) && std::isinf(view.truePeakDb.load()),"reset consumes at a callback boundary and clears gates and peaks");
    render.tone(0.5,0.00001);
    check(view.momentaryLufs.load()<-70 && std::isinf(view.integratedLufs.load()),"absolute -70 LUFS gate excludes a quiet tone from integrated loudness");
    render.meter.requestReset();render.tone(3.2,0.1);
    render.tone(3.2,0.001);
    check(near(view.integratedLufs.load(),level,0.4),"relative -10 LU gate excludes quieter programme material");
    check(render.meter.prepare(96000,2),"new rate can be published while meter is live");empty.frames=1;render.meter.process(empty);render.meter.collect();
    check(std::isinf(view.integratedLufs.load()),"format change starts fresh without replaying stale windows");
}
void peaksAndWeights() {
    for(double rate:{48000.0,96000.0,192000.0}) {
        Render<double> render(rate);render.tone(0.01,0.8,rate==96000 ? rate/6 : rate/4,rate==96000 ? std::numbers::pi/12 : std::numbers::pi/4);
        const auto peak=render.meter.readout().truePeakDb.load();
        const auto expectedPeak=20*std::log10(rate<176400 ? 0.8 : 0.8/std::sqrt(2.0));
        check(near(peak,expectedPeak,0.25),"true peak detects intersample crest at 4x/2x and sample peak at high rate");
    }
    Render<float> mono(48000,1);mono.tone(0.5,0.1,1000,0,1);
    check(near(mono.meter.readout().momentaryLufs.load(),expected(48000,1000,0.1,1)),"mono has one channel's energy");
    Render<double> weighted;const std::array<double,2> weights{1,0};
    check(weighted.meter.prepare(48000,2,weights),"explicit LFE exclusion layout is accepted");weighted.tone(0.5);
    check(near(weighted.meter.readout().momentaryLufs.load(),expected(48000,1000,0.1,1)),"zero-weight channel is excluded from loudness");
    const auto previous=weighted.meter.readout().integratedLufs.load();
    check(!weighted.meter.prepare(0,2) && !weighted.meter.prepare(48000,0) && !weighted.meter.prepare(48000,2,std::array<double,1>{1}),"invalid format/layout is refused without publishing");
    weighted.tone(0.1);check(near(weighted.meter.readout().integratedLufs.load(),previous),"invalid prepare preserves the active measurement");
    double bad=std::numeric_limits<double>::quiet_NaN();double* outputs[]{&bad,&bad};AudioIo io;io.out64=outputs;io.numOut=2;io.frames=1;weighted.meter.process(io);
    weighted.tone(0.5);
    check(std::isfinite(weighted.meter.readout().momentaryLufs.load()),"nonfinite device output does not poison future windows");
}
void formatIdentity() {
    MainLoudness meter;
    const auto& view=meter.readout();
    check(meter.preparedFormatSequence()==0 && view.formatSequence.load(std::memory_order_acquire)==0,
          "unprepared readouts have no accepted format identity");
    check(meter.prepare(48000,2),"format identity fixture prepares");
    const auto first=meter.preparedFormatSequence();
    check(first!=0 && view.formatSequence.load(std::memory_order_acquire)==0,
          "prepared format cannot label old or unconsumed readouts as current");
    AudioIo empty;
    meter.process(empty);
    check(view.formatSequence.load(std::memory_order_acquire)==first &&
              std::isinf(view.momentaryLufs.load()) && std::isinf(view.truePeakDb.load()),
          "audio adoption publishes the format only after clearing its readouts");
    check(meter.prepare(48000,2) && meter.preparedFormatSequence()==first &&
              view.formatSequence.load(std::memory_order_acquire)==first,
          "same-format preparation preserves measurement identity");
    check(!meter.prepare(0,2) && meter.preparedFormatSequence()==first,
          "rejected preparation cannot replace the public format identity");
    std::array<float,64> audio{}; audio.fill(.25f);
    float* outputs[]{audio.data(),audio.data()};
    AudioIo io;io.out=outputs;io.numOut=2;io.frames=64;
    meter.process(io);
    const auto peak=view.truePeakDb.load();
    check(std::isfinite(peak),"the old format has an actual held peak before replacement");
    check(meter.prepare(96000,2),"a new format stages without touching audio-owned readouts");
    const auto next=meter.preparedFormatSequence();
    check(next!=first && view.formatSequence.load(std::memory_order_acquire)==first &&
              view.truePeakDb.load()==peak,
          "UI can reject a still-held old-format value by comparing prepared and published identities");
    check(meter.prepare(44100,1),"a second replacement may supersede an unconsumed format");
    const auto latest=meter.preparedFormatSequence();
    check(latest!=next && latest!=first,"each actual replacement has a distinct prepared identity");
    meter.process(empty);meter.collect();
    check(view.formatSequence.load(std::memory_order_acquire)==latest &&
              std::isinf(view.momentaryLufs.load()) && std::isinf(view.shortTermLufs.load()) &&
              std::isinf(view.integratedLufs.load()) && std::isinf(view.truePeakDb.load()),
          "latest audio adoption clears all values before accepting its format identity");
    meter.requestReset();meter.process(empty);
    check(view.formatSequence.load(std::memory_order_acquire)==latest,
          "measurement reset retains its actual format identity");
}
void noAllocations() {
    MainLoudness meter;check(meter.prepare(8000,2),"allocation fixture prepares off-thread");
    std::array<float,256> audio{};audio.fill(0.1f);float* output[]{audio.data(),audio.data()};
    AudioIo io;io.out=output;io.numOut=2;io.frames=256;
    allocations.store(0);countAllocations.store(true);
    for(int i=0;i<110;++i)meter.process(io); // crosses all windows and many gates
    meter.requestReset();meter.process(io);
    countAllocations.store(false);
    check(allocations.load()==0,"processing, gate publication and reset allocate nothing");
}
class Sine final:public Node {
public:
    bool processesDouble() const noexcept override{return true;}
    void process(const NodeIo& io) noexcept override {
        for(int i=0;i<io.frames;++i){const double value=0.1*std::sin(2*std::numbers::pi*1000*static_cast<double>(at++)/48000);
            for(int c=0;c<io.channels;++c)if(io.wide())io.out64[c][io.blockOffset+i]=value;else io.out[c][io.blockOffset+i]=static_cast<float>(value);}
    }
    std::int64_t at=0;
};
void session() {
    test::TempDirectory temp{"main_loudness","session"};StoreError error{};auto store=Store::create(temp.path()/"song.adi",error);
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Tone'),(9,'master','Main');INSERT INTO mixer_strip(track_id,volume_db) VALUES(1,0),(9,-6)");
    Session engine;Sine sine;engine.setSourcesFor([&](std::int64_t id){return id==1?std::vector<Node*>{&sine}:std::vector<Node*>{};});
    check(engine.load(*store,{},{}),"session loads with its Main meter");
    std::array<float,256> l{},r{};float* outputs[]{l.data(),r.data()};AudioIo io;io.out=outputs;io.numOut=2;io.frames=256;
    for(int i=0;i<100;++i)engine.process(io);
    check(near(engine.mainLoudness().readout().momentaryLufs.load(),expected(48000,1000,0.1)-6),"session meter measures actual post-Main-fader output");
    engine.release();io.frames=0;engine.process(io);
    check(std::isinf(engine.mainLoudness().readout().integratedLufs.load()),"device stop requests a meter reset");
}
}
int main(){try{windows<float>();windows<double>();peaksAndWeights();formatIdentity();noAllocations();session();}catch(const std::exception& e){check(false,e.what());}
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
